// M3 Gamescope panel -- see PanelDisplay.h and superdoc/planning/SPEC.md's
// Feature 4 ("Live gamescope options: filter, scaler, sharpness"). Renamed
// title bar from "DISPLAY" to "GAMESCOPE" and split into tabs (issue #25) --
// the file/function names (PanelDisplay.cpp/.h, PanelDisplay_Draw()) were
// deliberately left as-is rather than renamed to PanelGamescope: doing so
// would also touch SettingsOverlay.cpp and meson.build's source list, both
// outside this panel's own scope and both plausibly being edited by other
// concurrently-running agents -- not worth the merge risk for a pure rename.
// chrome::PanelId::Display (Chrome.h) is unchanged for the same reason; only
// the window title string and the dock's own kDockEntries label changed.
//
// Thread safety: every value this panel edits is a plain (non-atomic) global,
// a ConVar<T> whose own m_Value is a plain field (see convar.h), or a field
// inside g_ColorMgmt.pending written through its own set_*() function --
// neither has any lock. All are safe to write here with no new
// synchronization because PanelDisplay_Draw() is called from
// SettingsOverlay_AddLayer(), which paint_all() calls in-line on the
// steamcompmgr thread (see SettingsOverlay.cpp) -- the exact same thread
// that reads g_upscaleFilter/g_upscaleScaler/g_upscaleFilterSharpness every
// frame (steamcompmgr.cpp's per-frame body), that cv_adaptive_sync/
// cv_hdr_enabled/cv_tearing_enabled are read from in vulkan_composite()/
// paint_all(), that g_bForceRelativeMouse is read from in ShouldDrawCursor()
// (steamcompmgr.cpp:2531) and the cursor-nesting check (:9413), and that
// update_color_mgmt() diffs g_ColorMgmt.pending against .current every frame
// (steamcompmgr.cpp:486). Writer and reader being the same thread is what
// makes this safe (superdoc/planning/runtime-knobs-and-fps.md Part A3) -- if
// this panel is ever called from a different thread, that guarantee breaks
// and these writes would need to move behind an X11-property path instead
// (which the Frame Limiter tab already does, for the reason explained on
// SetFpsLimit() below).
//
// Sharpness direction (2026-08-24 -- CORRECTS DECISIONS.md #11 and
// AUTONOMOUS-DECISIONS D13, which both recorded FSR and NIS as remapping the
// raw 0..20 g_upscaleFilterSharpness value in OPPOSITE visual directions).
// They do not. Raw 0 is maximum sharpening and raw 20 is minimum, for BOTH
// filters -- which is also what upstream's own `--help` says ("upscaler
// sharpness from 0 (max) to 20 (min)") and what rendervulkan.cpp's two shader
// feeds compute. Re-measured on a real build with five composited screenshots
// per setting; see RawSharpnessFromUiPercent()'s comment for the numbers and
// for why the old note's "verified empirically" claim was wrong. There is one
// mapping now, not a per-filter branch.
//
// One sharpness, not two (2026-08-24): the storage was always single -- one
// global, one `gamescope.sharpness` key. The now-removed direction flip is
// what made it read as two per-filter values, because the displayed percent
// jumped whenever the filter changed. SetFilter() also resets the percent to 0
// on an actual filter change, which is the user's own wording for "combine
// them". See SetFilter().
#include "PanelDisplay.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "convar.h"
#include "Config/ConfigManager.h"
#include "Config/AppId.h"
#include "Fonts.h"
#include "Widgets.h"

// Frame Limiter tab's live-write path (SetFpsLimit() below) -- pulls in
// gamescope_xwayland_server_t/xwayland_ctx_t (root ctx access) and
// XChangeProperty/XA_CARDINAL. Not already visible from steamcompmgr.hpp,
// which only forward-declares xwayland_ctx_t.
#include "wlserver.hpp"
#include "xwayland_ctx.hpp"

// Resolution area: steamcompmgr_set_nested_mode() (steamcompmgr.hpp) and
// INestedHints::RequestOutputSize() (backend.h) are the two live paths; the
// Hz<->mHz helpers are refresh_rate.h's.
#include "backend.h"
#include "refresh_rate.h"
// The area's pure half: the aspect shapes, their size lists and the two
// classifications the rows make from a live width/height (unit-tested by
// tests/test_resolution.cpp).
#include "ResolutionPresets.h"

// HDR tab's read-only appHDRMetadata readout (DrawHdrAppMetadataReadout()
// below) -- hdr_output_metadata/hdr_metadata_infoframe (CTA-861.G structs)
// come from here, the same header steamcompmgr.cpp itself uses for the same
// blob (see its gamescopeColorAppHDRMetadataFeedback write).
#include "drm_include.h"

#include "imgui.h"

// cv_adaptive_sync/cv_hdr_enabled aren't extern'd in a header (only
// cv_tearing_enabled is, in steamcompmgr.hpp) -- main.cpp declares its own
// extern for the same two ConVars for the same reason, this follows that
// existing precedent rather than adding new header plumbing.
extern gamescope::ConVar<bool> cv_adaptive_sync;
extern gamescope::ConVar<bool> cv_hdr_enabled;

// The per-frame application commit clock (src/commit.cpp), for
// display.budget_meter. Not declared in any header -- FpsDisplay.cpp
// declares its own extern for exactly this variable for exactly this
// reason, so this follows that precedent rather than adding header
// plumbing for one read.
extern std::atomic<uint64_t> g_ulLastAppFrametimeNs;

// Set true/false only inside the Steam-focus override branch in
// steamcompmgr.cpp's per-frame body; not extern'd in a header there either
// (rendervulkan.cpp declares its own extern for it too -- same pattern).
extern bool g_bSteamIsActiveWindow;

// HDR tab: g_ColorMgmt (rendervulkan.hpp) is already extern'd there and
// already reachable here (steamcompmgr.hpp includes rendervulkan.hpp), so no
// new extern is needed for the tracker itself -- only for the five live
// setters below, none of which are declared in any header (they have
// external linkage -- not `static` -- in steamcompmgr.cpp, they're just
// never extern'd anywhere; PropertyNotify handling there is their only
// existing caller). Same "declare it here, matching the two ConVar externs
// above" precedent. hdrTonemapOperator has no such setter to extern -- see
// DrawTonemapOperatorNote() below for why that's deliberately deferred, not
// silently dropped.
extern bool set_color_sdr_gamut_wideness( float flVal );
extern bool set_sdr_on_hdr_brightness( float flVal );
extern bool set_hdr_input_gain( float flVal );
extern bool set_sdr_input_gain( float flVal );

namespace gamescope
{
	// Lazily-loaded, mutated in place as the user edits controls, and
	// persisted via EnqueueRoutedWrite() on every change -- global.json or
	// the current session's games/<AppId>.json snapshot, whichever
	// config::IsSessionOverrideActive() says is authoritative (M7, see
	// Config/ConfigManager.h's session-routing section and PanelConfig.cpp,
	// which is the only thing that ever flips that flag). Superseded M3's
	// original "always global.json" simplification.
	static bool s_bConfigLoaded = false;
	static uint64_t s_ulLoadedGeneration = 0;
	static config::Settings s_CachedSettings;

	static void PushCachedSettingsToLiveState();
	// Forward-declared (defined below, near the E2 registration code) so the
	// Upscaling/Frame-limiter/Resolution setters above it in the file can
	// route through it too -- see Cfg()'s own comment for why every setter
	// that touches s_CachedSettings must call this first.
	static config::Settings &Cfg();

	static void EnsureConfigLoaded()
	{
		const uint64_t ulGeneration = config::ConfigGeneration();
		if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
			return;

		s_CachedSettings = config::ResolvedSettings();
		s_ulLoadedGeneration = ulGeneration;
		s_bConfigLoaded = true;

		// Pushed unconditionally, including on this panel's very first load
		// (2026-09-06 -- dropped the old `if ( bIsReload )` guard). That
		// guard assumed the very first load's values were already applied to
		// the live globals/ConVars by main.cpp's
		// apply_ritz_config_to_startup_state() before this panel ever draws
		// -- true only if the session profile hasn't changed since startup.
		// A select made from the Profiles area before this panel's first
		// draw bumps the generation and changes what ResolvedSettings()
		// returns here, so "first load" is not always "the startup state"
		// and skipping the push left this panel's sliders showing a value
		// that had never actually taken effect. config::SetLiveApplyHook()
		// now also pushes on every such bump regardless of which panel has
		// drawn, so this call is idempotent with it and with the startup
		// apply on the common path -- unconditional is simpler and closes
		// the one path where the guard was wrong.
		PushCachedSettingsToLiveState();
	}

	static void QueueSave()
	{
		config::EnqueueRoutedWrite( s_CachedSettings );
	}

	// THE ORDERING RULE, and the single door every setter in this panel goes
	// through (2026-09-06). Write the cached config FIRST, apply the live
	// value SECOND, persist LAST.
	//
	// `Why:` Cfg() is not a plain accessor. On the first call after a
	// generation bump -- session start, or a profile select made before this
	// panel had ever drawn -- it synchronously reloads s_CachedSettings from
	// config::ResolvedSettings() and pushes that (still PRE-edit) snapshot
	// into the very same live globals/ConVars/setters these controls write,
	// via PushCachedSettingsToLiveState(). A setter that assigned its live
	// global first and only then called Cfg() therefore had its assignment
	// silently overwritten one line later: the value was saved and displayed
	// correctly, but had no live effect until the identical edit was made a
	// second time (same generation -> no re-push). That was the
	// pointer-regression.sh `unlocked-resync` failure bisected to a6c413d;
	// see superdoc/features/profiles.md's "How a select reaches the screen".
	//
	// Taking the two halves as callables rather than leaving each setter to
	// sequence them is the guard: a call site physically cannot get the order
	// wrong, and a setter added later inherits the rule for free.
	template <typename TWriteCfg, typename TApplyLive>
	static void ApplyEdit( TWriteCfg &&fnWriteCfg, TApplyLive &&fnApplyLive )
	{
		fnWriteCfg( Cfg() );  // 1. config first -- Cfg() may re-push stale live state
		fnApplyLive();        // 2. live second -- so this always wins
		QueueSave();          // 3. persist
	}

	static const char *FilterToString( GamescopeUpscaleFilter eFilter )
	{
		switch ( eFilter )
		{
			case GamescopeUpscaleFilter::LINEAR:  return "LINEAR";
			case GamescopeUpscaleFilter::NEAREST: return "NEAREST";
			case GamescopeUpscaleFilter::FSR:     return "FSR";
			case GamescopeUpscaleFilter::NIS:     return "NIS";
			case GamescopeUpscaleFilter::PIXEL:   return "PIXEL";
			default:                              return "LINEAR";
		}
	}

	static const char *ScalerToString( GamescopeUpscaleScaler eScaler )
	{
		switch ( eScaler )
		{
			case GamescopeUpscaleScaler::AUTO:     return "AUTO";
			case GamescopeUpscaleScaler::INTEGER:  return "INTEGER";
			case GamescopeUpscaleScaler::FIT:      return "FIT";
			case GamescopeUpscaleScaler::FILL:     return "FILL";
			case GamescopeUpscaleScaler::STRETCH:  return "STRETCH";
			default:                                return "AUTO";
		}
	}

	static GamescopeUpscaleFilter FilterFromString( const std::string &sValue )
	{
		if ( sValue == "NEAREST" ) return GamescopeUpscaleFilter::NEAREST;
		if ( sValue == "FSR" )     return GamescopeUpscaleFilter::FSR;
		if ( sValue == "NIS" )     return GamescopeUpscaleFilter::NIS;
		if ( sValue == "PIXEL" )   return GamescopeUpscaleFilter::PIXEL;
		return GamescopeUpscaleFilter::LINEAR;
	}

	static GamescopeUpscaleScaler ScalerFromString( const std::string &sValue )
	{
		if ( sValue == "INTEGER" ) return GamescopeUpscaleScaler::INTEGER;
		if ( sValue == "FIT" )     return GamescopeUpscaleScaler::FIT;
		if ( sValue == "FILL" )    return GamescopeUpscaleScaler::FILL;
		if ( sValue == "STRETCH" ) return GamescopeUpscaleScaler::STRETCH;
		return GamescopeUpscaleScaler::AUTO;
	}

	// Mirrors main.cpp's apply_ritz_config_to_startup_state() (file-local
	// there, so not directly reusable) for the fields this panel owns --
	// pushes a freshly (re)loaded s_CachedSettings into the same live
	// globals/ConVars/setters this panel's own Set*() handlers write, so a
	// PanelConfig-triggered reload (profile applied, override toggled) takes
	// effect immediately rather than only on the next restart.
	static void PushCachedSettingsToLiveState()
	{
		g_wantedUpscaleFilter = FilterFromString( s_CachedSettings.gamescope.filter );
		g_wantedUpscaleScaler = ScalerFromString( s_CachedSettings.gamescope.scaler );
		g_upscaleFilterSharpness = s_CachedSettings.gamescope.sharpness;
		cv_adaptive_sync = s_CachedSettings.gamescope.vrr_enabled;
		cv_hdr_enabled = s_CachedSettings.gamescope.hdr_enabled;
		cv_tearing_enabled = s_CachedSettings.gamescope.tearing_enabled;
		// Issue #68: routed through steamcompmgr_set_force_relative_mouse()
		// rather than writing g_bForceRelativeMouse directly -- see that
		// function's comment for why a direct write has no live effect.
		steamcompmgr_set_force_relative_mouse( s_CachedSettings.gamescope.force_grab_cursor );
		steamcompmgr_set_force_windows_fullscreen( s_CachedSettings.gamescope.force_windows_fullscreen );

		set_color_sdr_gamut_wideness( s_CachedSettings.gamescope.sdr_gamut_wideness );
		set_sdr_on_hdr_brightness( s_CachedSettings.gamescope.sdr_on_hdr_brightness_nits );
		set_hdr_input_gain( s_CachedSettings.gamescope.hdr_input_gain );
		set_sdr_input_gain( s_CachedSettings.gamescope.sdr_input_gain );
		// fps_limit deliberately NOT re-pushed here: SetFpsLimit() below
		// round-trips through an X11 PropertyNotify (see its own comment),
		// which needs the root Xwayland server to exist -- true once this
		// panel first draws, but re-sending it on every config reload isn't
		// needed for correctness the way the plain-global writes above are,
		// since nothing else clobbers GAMESCOPE_FPS_LIMIT out from under a
		// reload the way e.g. a Steam-focus override clobbers filter/scaler.
	}

	// One slider, always "higher = sharper" -- and, since 2026-08-24, ONE
	// mapping, because the raw value does NOT change direction between the two
	// filters. This corrects DECISIONS.md #11 / AUTONOMOUS-DECISIONS D13, both
	// of which recorded FSR as running the other way.
	//
	// THE RAW SCALE IS "0 = MAXIMUM SHARPENING, 20 = MINIMUM", FOR BOTH.
	// Three independent sources agree, and they agree with each other:
	//
	//   1. `--help` in main.cpp, upstream's own wording:
	//        "--sharpness, --fsr-sharpness   upscaler sharpness from 0 (max) to 20 (min)"
	//   2. The shader feeds, in rendervulkan.cpp's composite path:
	//        RCAS: FsrRcasCon( ..., g_upscaleFilterSharpness / 10.0f ), and
	//              ffx_fsr1.h computes 2^-x from it -- the parameter is STOPS
	//              OF REDUCTION, so raw 0 is unattenuated and raw 20 is 2 stops
	//              down.
	//        NIS:  nisSharpness = (20 - g_upscaleFilterSharpness) / 20.0f -- a
	//              0..1 strength, so raw 0 is 1.0 and raw 20 is 0.0.
	//      Different arithmetic, same direction.
	//   3. Measured, on this build: five `full_composition` screenshots per
	//      setting (the animated vkcube scene makes a single frame per setting
	//      a comparison of two different scenes, not two settings), mean
	//      Sobel/FIND_EDGES energy over the whole 1920x1080 frame:
	//
	//        raw:            0        10       20
	//        FSR   median  1.647    1.136    1.083     (ranges do not overlap)
	//        NIS   median  1.387    1.415    1.094
	//
	//      Edge energy tracks the RAW value and nothing else: it is the same
	//      curve under both filters, and it FALLS as raw rises. There is no
	//      inversion between the filters to encode.
	//
	// WHY THE OLD NOTE WAS WRONG, AND WHY IT MATTERED. The previous comment
	// here claimed FSR was screenshot-verified as "higher raw = sharper" and
	// warned that the shader math "reads as if it should invert". It does not
	// read that way -- it says what it means -- and the measurement above says
	// the same thing. The consequence was not cosmetic: with the FSR branch
	// inverted, `Sharpness 0%` under FSR selected raw 0, i.e. MAXIMUM
	// sharpening, which is precisely backwards from what the number promises
	// and from what "resets to 0% when the filter changes" is supposed to mean.
	//
	// The eFilter parameter is gone rather than kept-and-ignored: an argument
	// that no longer affects the result is an invitation to re-introduce a
	// branch here.
	static int RawSharpnessFromUiPercent( int nUiPercent )
	{
		nUiPercent = std::clamp( nUiPercent, 0, 100 );
		return (int)std::lround( (100 - nUiPercent) * 20.0 / 100.0 );
	}

	static int UiPercentFromRawSharpness( int nRaw )
	{
		nRaw = std::clamp( nRaw, 0, 20 );
		return (int)std::lround( (20 - nRaw) * 100.0 / 20.0 );
	}

	static void SetSharpnessUiPercent( int nUiPercent )
	{
		const int nRaw = RawSharpnessFromUiPercent( nUiPercent );
		// Cfg() (EnsureConfigLoaded()) FIRST -- see ApplyEdit()'s comment for
		// both reasons it has to be: writing straight to s_CachedSettings
		// without loading first left every other field at its struct default,
		// which EnqueueRoutedWrite() cannot tell from a real edit and so wrote
		// into the profile (measured: crosshair defaults and filter LINEAR
		// landing in a game profile from the first Upscaling edit of a
		// session); and assigning the live global before Cfg() got that
		// assignment clobbered by the reload's re-push. Reachable from the
		// palette or overlay_e2_set before this area has ever drawn.
		ApplyEdit(
			[ nRaw ]( config::Settings &cfg ) { cfg.gamescope.sharpness = nRaw; },
			[ nRaw ] { g_upscaleFilterSharpness = nRaw; } );
	}

	// Changing the filter RESETS sharpness to 0% (the user, 2026-08-24: "The
	// FSR/NIS sharpness are individual values right now. Combine them, so it
	// is just the sharpness (when switching between filters, it resets to
	// 0%)").
	//
	// There has only ever been ONE stored sharpness -- one global
	// (g_upscaleFilterSharpness), one config key (gamescope.sharpness). What
	// made it *look* like two per-filter values is the direction flip
	// documented at the top of this file: the same raw 16 reads as 80% under
	// FSR and 20% under NIS, so the number visibly jumped every time the
	// filter changed and each filter appeared to remember its own setting.
	//
	// Rejected: making the percent survive the switch (re-encode the old
	// percent into the new filter's raw value). That keeps the two filters
	// coupled through a number whose *meaning* differs -- 80% of RCAS and 80%
	// of NIS are not the same amount of sharpening, and carrying one over
	// silently applies a value the user never chose for that pass. Resetting
	// is the one behaviour that is unambiguous at both ends, and it is what
	// was asked for.
	//
	// Only on an actual change: re-selecting the current filter (a click on
	// the already-active segment, a config push that resolves to the same
	// value) must not wipe a sharpness the user just set.
	static void SetFilter( GamescopeUpscaleFilter eFilter )
	{
		// Cfg() before the comparison, not just before the write: on a
		// generation bump it re-pushes the resolved filter into
		// g_wantedUpscaleFilter, so asking "did the filter actually change?"
		// any earlier would answer against a value about to be replaced.
		Cfg();
		const bool bFilterChanged = ( eFilter != g_wantedUpscaleFilter );
		ApplyEdit(
			[ eFilter ]( config::Settings &cfg ) { cfg.gamescope.filter = FilterToString( eFilter ); },
			[ eFilter ] { g_wantedUpscaleFilter = eFilter; } );
		if ( bFilterChanged )
			SetSharpnessUiPercent( 0 ); // QueueSave()s on its own
	}

	static void SetScaler( GamescopeUpscaleScaler eScaler )
	{
		ApplyEdit(
			[ eScaler ]( config::Settings &cfg ) { cfg.gamescope.scaler = ScalerToString( eScaler ); },
			[ eScaler ] { g_wantedUpscaleScaler = eScaler; } );
	}

	// Frame Limiter tab.
	//
	// Originally this only wrote the legacy GAMESCOPE_FPS_LIMIT X11 property
	// and relied on steamcompmgr's own PropertyNotify handler
	// (steamcompmgr.cpp's handle_property_notify(), the `gamescopeFPSLimit`
	// branch) to land it in g_nSteamCompMgrTargetFPS. That write *does*
	// land -- but it doesn't stick: paint_all() calls
	// update_app_target_refresh_cycle() every frame
	// (steamcompmgr.cpp:2918), which unconditionally recomputes
	// g_nSteamCompMgrTargetFPS from a *different* variable,
	// g_nCombinedAppRefreshCycleOverride[type] (steamcompmgr.cpp:975-1011),
	// zeroing it first and only restoring a nonzero value if that override
	// is itself set. Nothing sets that override from the X property path,
	// so the very next frame after the PropertyNotify handler wrote a
	// nonzero g_nSteamCompMgrTargetFPS, this stomps it back to 0 -- this is
	// issue #25's bug: the control visibly writes, the FPS HUD never moves.
	// (Verified live: 28fps and 60fps both left the HUD sitting at 120fps.)
	//
	// The property write above isn't wrong, exactly -- it is just no longer
	// the mechanism upstream's own frame-pacing code actually reads on a
	// steady-state basis; g_nCombinedAppRefreshCycleOverride is. The real
	// setter for that variable is steamcompmgr_set_app_refresh_cycle_override()
	// (steamcompmgr.hpp:174), an ordinary extern'd function call -- not an
	// X11 round-trip -- already used for exactly this by both the
	// gamescope_control Wayland protocol's set_app_refresh_cycle request
	// (wlserver.cpp:1370) and the "debug_set_fps_limit" ConCommand
	// (steamcompmgr.cpp, cc_debug_set_fps_limit). This call mirrors that
	// ConCommand exactly (change_refresh=true, change_fps_cap=true). Being a
	// plain global write, it's safe with no new synchronization for the same
	// reason as every other Set*() above (see the file-top threading-safety
	// comment): PanelDisplay_Draw() only ever runs on the steamcompmgr
	// thread, the same thread paint_all()/update_app_target_refresh_cycle()
	// run on.
	//
	// The XChangeProperty write is kept alongside it (not removed) so
	// GAMESCOPE_FPS_LIMIT still reflects the panel's current value for any
	// external reader of that property (e.g. the Steam client) -- it is
	// just no longer the thing this panel relies on for the limit to
	// actually take effect.
	// Issue #67: the valid range is 0 (unlimited) or [kMinFpsLimit,
	// kMaxFpsLimit] -- NOT a plain 0..kMaxFpsLimit continuum. 1-9fps is a
	// trap: at that rate this very overlay repaints only a few times a
	// second, so a user who lands there can no longer practically drive the
	// UI to undo it. Clamping any nonzero request up to the floor (rather
	// than leaving 1..9 reachable) closes that trap at the single choke
	// point every write path (slider, ConCommand, gamescope_control) already
	// goes through.
	static constexpr int kMinFpsLimit = 10;
	static constexpr int kMaxFpsLimit = 480;

	static void ApplyFpsLimitLive( int nFps )
	{
		steamcompmgr_set_app_refresh_cycle_override( GetBackend()->GetScreenType(), nFps, true, true );

		gamescope_xwayland_server_t *pRootServer = wlserver_get_xwayland_server( 0 );
		if ( pRootServer && pRootServer->ctx )
		{
			xwayland_ctx_t *pRootCtx = pRootServer->ctx.get();
			uint32_t uValue = (uint32_t)nFps;
			XChangeProperty( pRootCtx->dpy, pRootCtx->root, pRootCtx->atoms.gamescopeFPSLimit,
				XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&uValue, 1 );
			// Explicit flush: steamcompmgr's own event loop only auto-flushes
			// this same Xlib connection ahead of a *blocking* read, and only
			// XFlush()es outright when ITS OWN writes this frame set a local
			// flush_root flag (steamcompmgr.cpp) -- neither is guaranteed to
			// happen promptly after a write this panel makes from outside
			// that loop, so without this the property can sit client-side
			// buffered for an arbitrary number of frames before the
			// PropertyNotify this control depends on is even sent.
			XFlush( pRootCtx->dpy );
		}
	}

	static void SetFpsLimit( int nFps )
	{
		nFps = ( nFps <= 0 ) ? 0 : std::clamp( nFps, kMinFpsLimit, kMaxFpsLimit );
		// Already had the right order before ApplyEdit() existed; routed
		// through it anyway so "every setter here goes through one door" is
		// true with no exception a reader has to check.
		ApplyEdit(
			[ nFps ]( config::Settings &cfg ) { cfg.gamescope.fps_limit = nFps; },
			[ nFps ] { ApplyFpsLimitLive( nFps ); } );
	}

	// =====================================================================
	//  E2 (P3) -- the same settings, declared instead of drawn
	// =====================================================================
	// P5 deleted the four legacy tabs this replaced, the DrawXxxTab()
	// functions behind them and the floating window that hosted them.
	//
	// WHAT IS AND IS NOT DIFFERENT HERE. Not one config key, not one value
	// range, and not one setter changed. Every binding below routes to the
	// SAME Set*() function the legacy tab calls, which is the whole reason
	// they are functions: issues #25 (frame limiter) and #68 (force grab
	// cursor) were both controls that rendered correctly while doing
	// nothing, and both were fixed by moving the write to the entry point
	// the compositor actually reads. Re-deriving a write here would be
	// re-introducing exactly those two bugs, so nothing here writes a global
	// directly that a Set*() already owns.
	//
	// THE TABS BECAME AREAS, NOT GROUPS. SPEC §8.1's rail is the product's
	// only navigation, and it lists Upscaling, Frame limiter and HDR as
	// separate rail items -- so does index.html, the tested reference. The
	// old "Display" tab had no equivalent: its three settings were originally
	// split as presentation (tearing, cursor grab) and refresh (VRR), joining
	// the areas that already owned those concerns rather than becoming a
	// fourth area with no theme. See AUTONOMOUS-DECISIONS.md D13.1.
	//
	// D13.1 CORRECTED, DIRECTLY BY THE USER (2026-08-24), NOT BY AGENT
	// JUDGEMENT. That placement is what put VRR next to the frame limiter
	// and tearing/cursor-grab inside Upscaling -- three settings a user
	// flips mid-game, scattered across two areas neither named for that
	// purpose. The user: "VRR shouldnt be placed in 'Frame Limiter'. It
	// should be in something like 'General', for quick toggling, together
	// with 'Allow tearing' and 'Force grab cursor'. Both of these make no
	// sense being in 'Upscaling'." All three now live in a new
	// display.general area, registered first so it sits above Upscaling in
	// the rail. Neither SPEC §8.1 nor index.html names a General area --
	// both predate this feedback, so the user's direct instruction overrides
	// them here; see AUTONOMOUS-DECISIONS.md's correction note for the full
	// reasoning. Not one config key or binding changed: RegisterGeneral()
	// below calls the exact same Set*()/QueueSave() code these three rows
	// called in their previous areas.
	//
	// Every getter goes through Cfg() rather than touching s_CachedSettings.
	// Under the legacy path EnsureConfigLoaded() ran once per Draw(); under
	// E2 there is no per-frame call into this file at all, so the refresh
	// has to hang off the first thing that runs each frame -- which is a
	// getter. The check is two integer compares when nothing changed.
	static config::Settings &Cfg()
	{
		EnsureConfigLoaded();
		return s_CachedSettings;
	}

	static const ui::Option kFilterOptions[] = {
		{ (int)GamescopeUpscaleFilter::LINEAR,  "linear"  },
		{ (int)GamescopeUpscaleFilter::NEAREST, "nearest" },
		{ (int)GamescopeUpscaleFilter::FSR,     "fsr"     },
		{ (int)GamescopeUpscaleFilter::NIS,     "nis"     },
		{ (int)GamescopeUpscaleFilter::PIXEL,   "pixel"   },
	};

	static const ui::Option kScalerOptions[] = {
		{ (int)GamescopeUpscaleScaler::AUTO,    "auto"    },
		{ (int)GamescopeUpscaleScaler::INTEGER, "integer" },
		{ (int)GamescopeUpscaleScaler::FIT,     "fit"     },
		{ (int)GamescopeUpscaleScaler::FILL,    "fill"    },
		{ (int)GamescopeUpscaleScaler::STRETCH, "stretch" },
	};

	static bool SharpnessApplies()
	{
		return g_wantedUpscaleFilter == GamescopeUpscaleFilter::FSR
		    || g_wantedUpscaleFilter == GamescopeUpscaleFilter::NIS;
	}

	// D13.1 CORRECTED (2026-08-24): the user, directly -- "VRR shouldnt be
	// placed in 'Frame Limiter'. It should be in something like 'General',
	// for quick toggling, together with 'Allow tearing' and 'Force grab
	// cursor'." Registered FIRST in PanelDisplay_RegisterAreas() so it is
	// first in DISPLAY's rail order, above Upscaling -- the "front door" the
	// user asked for these three to have. See AUTONOMOUS-DECISIONS.md's
	// D13.1 correction note for why this disagrees with SPEC §8.1 and
	// index.html (neither names a General area; both predate this feedback).
	//
	// Every binding below is moved, not re-derived: same Set*()/QueueSave()
	// calls this file already used for these three settings, so the bug
	// classes issues #25 and #68 came from (a control that renders and does
	// nothing) cannot reappear here.
	static void RegisterGeneral( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "display.general", "General", ui::Section::Display );
		a.Keywords( "general quick toggle vrr adaptive sync freesync gsync tearing cursor grab "
		            "maximize fullscreen nested window" );
		a.Summary( []{
			std::string s = cv_adaptive_sync.Get() ? "VRR on" : "VRR off";
			s += cv_tearing_enabled.Get() ? " · tearing on" : " · tearing off";
			s += g_bForceRelativeMouse ? " · cursor grabbed" : " · cursor free";
			s += steamcompmgr_get_force_windows_fullscreen() ? " · nested windows maximized" : "";
			return s;
		} );

		a.Group( "Quick toggles" );

		a.Switch( "display.adaptive_sync", "Adaptive sync (VRR)",
			ui::AnyBind::Of<bool>(
				[]{ return cv_adaptive_sync.Get(); },
				[]( bool b ) {
					ApplyEdit(
						[ b ]( config::Settings &cfg ) { cfg.gamescope.vrr_enabled = b; },
						[ b ] { cv_adaptive_sync = b; } );
				} ) )
			.Key( "gamescope.vrr_enabled" )
			.Help( "Matches your screen's refresh rate to the game so motion looks smoother with "
			       "less stutter. Needs a screen and cable that support VRR (FreeSync or G-Sync)." )
			.Default( false )
			.Keywords( "vrr freesync gsync adaptive sync refresh" );

		a.Switch( "display.allow_tearing", "Allow tearing",
			ui::AnyBind::Of<bool>(
				[]{ return cv_tearing_enabled.Get(); },
				[]( bool b ) {
					ApplyEdit(
						[ b ]( config::Settings &cfg ) { cfg.gamescope.tearing_enabled = b; },
						[ b ] { cv_tearing_enabled = b; } );
				} ) )
			.Key( "gamescope.tearing_enabled" )
			.Help( "Shows new frames the instant they're ready instead of waiting for the screen. "
			       "Feels more responsive, but fast camera movement can show a faint horizontal line." )
			.Default( false )
			.Keywords( "immediate flip vsync latency tear seam" );

		// Issue #68. Routed through steamcompmgr_set_force_relative_mouse()
		// and NOT by writing g_bForceRelativeMouse, which has no live effect
		// -- the flag's two real consumers only read it once at backend
		// startup. This is the fix that made the legacy toggle actually do
		// something; binding the global here would silently undo it. Moving
		// areas does not touch this call, so the fix survives the move.
		a.Switch( "display.force_grab_cursor", "Force grab cursor",
			ui::AnyBind::Of<bool>(
				[]{ return g_bForceRelativeMouse; },
				[]( bool b ) {
					ApplyEdit(
						[ b ]( config::Settings &cfg ) { cfg.gamescope.force_grab_cursor = b; },
						[ b ] { steamcompmgr_set_force_relative_mouse( b ); } );
				} ) )
			.Key( "gamescope.force_grab_cursor" )
			.Help( "Keeps your mouse locked to the game at all times, not just when the cursor is "
			       "hidden. Turn this on if the mouse ever seems to escape the game window." )
			.Default( false )
			.Keywords( "mouse pointer capture confine grab relative" );

		// --force-windows-fullscreen (upstream flag; xwayland_ctx_t::
		// force_windows_fullscreen). Genuinely live, same shape as Force
		// grab cursor just above: routed through
		// steamcompmgr_set_force_windows_fullscreen(), which sets every
		// live Xwayland ctx and marks focus dirty so
		// determine_and_apply_focus() force-resizes the focused window on
		// the very next frame -- not a startup-only flag despite having
		// been CLI-only before this toggle existed. See that function's
		// definition comment in steamcompmgr.cpp for the two consumers
		// this reaches (the game-window branch in
		// determine_and_apply_focus() and handle_desktop_window()).
		a.Switch( "display.force_windows_fullscreen", "Force maximize nested window",
			ui::AnyBind::Of<bool>(
				[]{ return steamcompmgr_get_force_windows_fullscreen(); },
				[]( bool b ) {
					ApplyEdit(
						[ b ]( config::Settings &cfg ) { cfg.gamescope.force_windows_fullscreen = b; },
						[ b ] { steamcompmgr_set_force_windows_fullscreen( b ); } );
				} ) )
			.Key( "gamescope.force_windows_fullscreen" )
			.Help( "Makes windows inside gamescope open maximized/fullscreen, filling the nested "
			       "display instead of using their own requested size. Takes effect immediately, "
			       "even on windows already open." )
			.Default( false )
			.Keywords( "maximize fullscreen nested window force size windows-fullscreen" );
	}

	static void RegisterUpscaling( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "display.upscaling", "Upscaling", ui::Section::Display );
		a.Keywords( "upscale scaling resample filter fsr nis sharpen scaler aspect" );
		a.Summary( []{
			std::string s = FilterToString( g_wantedUpscaleFilter );
			s += " · ";
			s += ScalerToString( g_wantedUpscaleScaler );
			if ( g_bSteamIsActiveWindow )
				s += "  (Steam is focused -- both are temporarily forced to Fit/Linear)";
			return s;
		} );

		a.Group( "Scaling filter" );

		a.Choice( "display.filter", "Filter",
			ui::AnyBind::Of<int>(
				[]{ return (int)g_wantedUpscaleFilter; },
				[]( int n ) { SetFilter( (GamescopeUpscaleFilter)n ); } ),
			kFilterOptions, std::size( kFilterOptions ) )
			.Key( "gamescope.filter" )
			.Help( "Chooses how the game's picture is stretched to fill your screen. FSR and NIS "
			       "also sharpen it afterward; Pixel stays blocky except at exact resolution "
			       "multiples." )
			.Default( (int)GamescopeUpscaleFilter::LINEAR )
			.Keywords( "upscale scaling resample fsr nis pixel linear nearest" );

		// The UI percent, not the raw 0..20 -- DECISIONS.md #11. The percent
		// is always "higher = sharper"; the raw value it maps to flips
		// direction between FSR and NIS underneath, which is why this binds
		// through the same two remap functions the legacy slider uses rather
		// than exposing g_upscaleFilterSharpness directly.
		a.Slider( "display.filter.sharpness", "Sharpness",
			ui::AnyBind::Of<int>(
				[]{ return UiPercentFromRawSharpness( g_upscaleFilterSharpness ); },
				[]( int n ) { SetSharpnessUiPercent( n ); } ) )
			.Key( "gamescope.sharpness" )
			.Help( "How much extra sharpening FSR or NIS adds after resizing the picture. Higher "
			       "looks crisper but too high adds haloing; this is separate from the Shaders "
			       "area's Pre-sharpen, which works with any filter." )
			.Range( 0.0f, 100.0f )
			// D18: the binding has 21 real notches, not 101. The raw value is
			// 0..20 and the percentage is round(raw x 100 / 20), so the UI
			// scale moves in fives and a smaller step round-trips straight
			// back to the value it started from.
			//
			// Undeclared, that made Left/Right a DEAD KEY on this row --
			// AdjustValue's default step is (hi-lo)/100 = 1, Set(61) became
			// raw 12, and Get() returned 60 again. A drag never showed it
			// because a drag crosses several notches at once; only the
			// keyboard moves by exactly one, which is why closing the
			// keyboard gaps is what found it.
			.Step( 5.0f )
			.Unit( "%" )
			// The schema default is raw 2, which on the corrected scale is 90%:
			// gamescope's own compiled-in default really is near-maximum
			// sharpening (main.cpp: g_upscaleFilterSharpness = 2, "0 (max)").
			.Default( UiPercentFromRawSharpness( 2 ) )
			.Keywords( "sharpen sharpness rcas cas crisp clarity ringing" )
			.DisabledUnless( SharpnessApplies,
				"only FSR and NIS sharpen -- the Linear, Nearest and Pixel filters have no "
				"sharpening pass, so this has no effect while one of them is selected" );

		a.Choice( "display.filter.scaler", "Scaler",
			ui::AnyBind::Of<int>(
				[]{ return (int)g_wantedUpscaleScaler; },
				[]( int n ) { SetScaler( (GamescopeUpscaleScaler)n ); } ),
			kScalerOptions, std::size( kScalerOptions ) )
			.Key( "gamescope.scaler" )
			.Help( "Decides how the picture fits your screen when its shape doesn't match. Integer "
			       "only resizes in whole-number steps, which stays sharp but can add black bars." )
			.Default( (int)GamescopeUpscaleScaler::AUTO )
			.Keywords( "aspect fit fill stretch integer letterbox" );

		// Allow tearing and Force grab cursor lived here as a "Presentation"
		// group until the user corrected D13.1 (2026-08-24): both moved to
		// display.general, "for quick toggling" -- see RegisterGeneral()
		// below and AUTONOMOUS-DECISIONS.md's D13.1 correction note.

		a.Group( "Diagnostics" );

		a.Facts( "display.upscaling_facts", "Effective path", []{
			if ( g_bSteamIsActiveWindow )
				return std::string( "overridden while Steam is focused" );
			return std::string( FilterToString( g_upscaleFilter ) ) + " · " + ScalerToString( g_upscaleScaler );
		} )
			.Help( "Shows the filter and scaler actually in use right now, including any temporary "
			       "override. Read-only, this just reports the current state." )
			.Keywords( "effective live override steam actual" )
			.Live( "wanted filter", []{ return ui::Fact{ "wanted filter", FilterToString( g_wantedUpscaleFilter ) }; } )
			.Live( "live filter",   []{ return ui::Fact{ "live filter",   FilterToString( g_upscaleFilter ) }; } )
			.Live( "wanted scaler", []{ return ui::Fact{ "wanted scaler", ScalerToString( g_wantedUpscaleScaler ) }; } )
			.Live( "live scaler",   []{ return ui::Fact{ "live scaler",   ScalerToString( g_upscaleScaler ) }; } )
			.Live( "raw sharpness", []{
				char sz[ 48 ];
				std::snprintf( sz, sizeof( sz ), "%d of 20 (%s)", g_upscaleFilterSharpness,
					SharpnessApplies() ? "in use" : "not applied by this filter" );
				return ui::Fact{ "raw sharpness", sz };
			} )
			// The old Upscaling tab's orange banner, kept as a fact rather
			// than dropped. It is a statement about live state, and live
			// state is what Details is for.
			.Live( "steam override", []{
				return ui::Fact{ "steam override", g_bSteamIsActiveWindow
					? "active -- filter and scaler are forced to Fit/Linear while the Steam window is focused"
					: "inactive" };
			} );
	}


	// =====================================================================
	//  Resolution -- the game's resolution and the paced refresh
	// =====================================================================
	// Tracker item 7 (superdoc/planning/requests-2026-09-05.md), Phase A;
	// reshaped by requests-2026-09-06 items 7-10.
	// Feature doc: superdoc/features/resolution-and-refresh.md.
	//
	// TWO DIFFERENT THINGS, DELIBERATELY IN ONE AREA. The user asked for
	// "the nested resolution and refresh rate"; gamescope has numbers that
	// could each be meant, and a user who wants one is one wrong guess away
	// from the other:
	//   * the resolution THE GAME SEES (-w/-h): Xwayland's RandR screen,
	//     which gamescope scales to the window. Live via
	//     steamcompmgr_set_nested_mode() -- the same mechanism the Steam
	//     Deck's GAMESCOPE_XWAYLAND_MODE_CONTROL atom uses.
	//   * the paced REFRESH (-r): the fake vblank rate and the mode's
	//     advertised Hz. Live: the vblank timer re-reads g_nNestedRefresh
	//     every cycle. 0 = follow the host.
	//
	// The WINDOW size (-W/-H) used to be a third group of rows here, asking
	// the host through INestedHints::RequestOutputSize(). The user had it
	// removed on 2026-09-06 (item 10); the granted size is still reported as
	// a Live fact, because it is what the game's mode gets scaled to.
	//
	// WHAT IS NOT PROMISED, and the help text does not: changing the host
	// monitor's refresh, or guaranteeing a running game adopts the new mode. A game that read the
	// mode list once at start-up lists the new one after a restart; most
	// switch within a frame or two because determine_and_apply_focus()
	// force-resizes a fullscreen window to the new root size.
	//
	// ui::Applies::NeedsRestart exists (Registry.h) but nothing draws it yet,
	// so the "some games need a restart" caveat is carried by the Facts row
	// and the help text rather than a badge. Swap to the badge when the shell
	// grows one.
	//
	// Nested width/height/refresh (0 = as launched) are persisted into
	// GamescopeSettings, serialised by ConfigManager.cpp and applied in
	// main.cpp's apply_ritz_config_to_startup_state(), which already runs
	// before getopt so the CLI wins for free. The single write point is
	// ApplyNestedMode() below.
	//
	// Nested-only in this phase: AvailableWhen() hides the area when the
	// current connector has no INestedHints (embedded DRM, where the display
	// owns the mode). OpenVR implements INestedHints and so sees the area;
	// its RequestOutputSize() is the default no-op, which is the honest
	// answer there.
	//
	// Threading: every setter below runs on the steamcompmgr thread (see the
	// file-top comment). steamcompmgr_set_nested_mode() takes wlserver_lock()
	// itself. Nothing here needs new synchronisation.

	using gamescope::resolution::SizePreset;
	using gamescope::resolution::AspectList;
	using gamescope::resolution::kAspectLists;
	using gamescope::resolution::kSizeCustom;
	using gamescope::resolution::ListFor;
	using gamescope::resolution::MatchSizePreset;
	using gamescope::resolution::ClosestByHeight;
	using gamescope::resolution::ClassifyAspect;
	using gamescope::resolution::FormatLiveLine;
	using gamescope::resolution::kAspectNative;
	using gamescope::resolution::kAspect16x9;
	using gamescope::resolution::kAspect4x3;
	using gamescope::resolution::kAspect16x10;
	using gamescope::resolution::kAspect21x9;
	using gamescope::resolution::kAspectCustom;

	static constexpr int kMinDim = 320, kMaxDim = 7680, kDimStep = 8;
	static constexpr int kMinRefreshHz = 24, kMaxRefreshHz = 500;

	// ---- game resolution --------------------------------------------------
	// TWO rows, not six (requests-2026-09-06 item 7, replacing item 13 of
	// 2026-09-05): the SHAPE (Aspect: Native / 16:9 / 4:3 / 16:10 / 21:9 /
	// Custom), then ONE Resolution row whose option list IS the selected
	// shape's list. The user: "There shouldnt be individual resolution
	// elements for the different aspect ratios. It should only change the
	// available option."
	//
	// What made that impossible before was that the Registry copied a
	// Choice's options once at registration, so "the list follows the aspect"
	// had to be one greyed row per shape. Entry::OptionsFrom() (Registry.h,
	// added for this) makes the option set a READ instead, so there is one
	// row again and the four `display.resolution.preset_*` ids are gone. They
	// were never config keys -- nothing persisted them, so nothing has to be
	// migrated; a stale `overlay_e2_set display.resolution.preset_16_9 3`
	// simply reports an unknown id.
	//
	// UPDATE (requests-2026-09-07 item 3): the lists no longer end in Custom
	// -- see kSizeOptions16x9 & co.'s own comment below. A mode that has the
	// shape but is on no list (a launch-time -w 1600 -h 1200) now reflects as
	// the Custom ASPECT rather than "4:3 + Custom", since that is the only
	// place Custom still exists.
	static const ui::Option kAspectOptions[] = {
		{ kAspectNative, "Native" },
		{ kAspect16x9, "16:9" }, { kAspect4x3, "4:3" }, { kAspect16x10, "16:10" }, { kAspect21x9, "21:9" },
		{ kAspectCustom, "Custom" },
	};

	// The size lists as options. Static storage, because ui::Option borrows
	// its label pointer (Registry.h) and SizeOptionsForAspect() below hands
	// these out to a live option set.
	//
	// NO "Custom" ENTRY HERE (requests-2026-09-07 item 3, the user: "the
	// custom selection should only be a part of the aspect ratio ... it
	// shouldnt be visible to the user" [in the Resolution dropdown]). Custom
	// now lives ONLY on the Aspect row above -- these four lists offer real
	// sizes and nothing else, so there is no stray "Custom" entry to sit
	// among "1920 x 1080" and friends. A size that does not exactly match one
	// of these entries is not representable within a shape's list any more;
	// see CurrentAspect()'s comment for how that is kept true (it flips
	// Aspect itself to Custom instead).
	static const ui::Option kSizeOptions16x9[] = {
		{ 1, "3840 x 2160" }, { 2, "2560 x 1440" }, { 3, "1920 x 1080" }, { 4, "1600 x 900" }, { 5, "1280 x 720" },
	};
	static const ui::Option kSizeOptions4x3[] = {
		{ 1, "2880 x 2160" }, { 2, "1920 x 1440" }, { 3, "1440 x 1080" }, { 4, "1280 x 960" },
	};
	static const ui::Option kSizeOptions16x10[] = {
		{ 1, "3840 x 2400" }, { 2, "2560 x 1600" }, { 3, "1920 x 1200" }, { 4, "1680 x 1050" }, { 5, "1440 x 900" },
		{ 6, "1280 x 800" },
	};
	static const ui::Option kSizeOptions21x9[] = {
		{ 1, "5120 x 2160" }, { 2, "3440 x 1440" }, { 3, "2560 x 1080" },
	};
	// Native and Custom are sizes in their own right rather than shapes with
	// a list, so the row shows the one entry that is true and is greyed out
	// -- this single entry is NOT the same thing as the removed "Custom"
	// list entry above: it is the whole (disabled) option set for the
	// Native/Custom aspects, never one choice among several real sizes.
	static const ui::Option kSizeOptionsNative[] = { { kSizeCustom, "Window size" } };
	static const ui::Option kSizeOptionsCustom[] = { { kSizeCustom, "Custom" } };

	// The user's last picks, or -1 before any. Needed because the live
	// numbers alone are ambiguous: 1920x1080 in a 1920x1080 window is both
	// "Native" and "16:9 + 1920 x 1080", and a Custom 1280x720 is also the
	// 16:9 preset. A pick is trusted while the live mode is still the one it
	// was made against; once the mode changes for any other reason (Steam's
	// mode-control atom, a per-game switch, a config apply), the live value
	// wins.
	static int  s_nAspectChoice = -1;
	static int  s_nSizeChoice   = -1;   // within s_nAspectChoice's list; kSizeCustom or 1..N; -1 = none yet
	// The live mode at the moment of the pick. The pick is trusted exactly
	// as long as the live mode still equals this -- see CurrentAspect().
	static int  s_nPickWidth = 0, s_nPickHeight = 0;
	static int  s_nCustomWidth = 0, s_nCustomHeight = 0;   // 0 = not yet used, seed from live
	static bool s_bLockAspect = true;
	// The pair the lock derives from -- a pair rather than a single ratio
	// float so repeated rounding on one axis can't quietly drift the other.
	// Explicitly captured (CaptureLockedAspect(), below) at every point the
	// reference should change; NEVER derived from s_nCustomWidth/Height
	// inside a setter that is mid-mutation -- see LockedAspect()'s own
	// comment for the bug that shipped from doing exactly that. 0/0 = not
	// yet captured this stretch of Custom.
	static int s_nLockRefWidth = 0, s_nLockRefHeight = 0;

	static bool NestedModeAvailable()
	{
		IBackendConnector *pConnector = GetBackend() ? GetBackend()->GetCurrentConnector() : nullptr;
		return pConnector && pConnector->GetNestedHints() != nullptr;
	}

	static int EffectiveNestedRefreshmHz()
	{
		return g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh;
	}

	// The RandR screen the game actually sees, read from the game's Xwayland
	// root rather than from g_nNestedWidth/Height -- the two agree after a
	// change from here, but Steam's mode-control atom writes only the former,
	// and this row exists to show the truth.
	static SizePreset GameRootSize()
	{
		// With --xwayland-count > 1, server #0 is Steam's (kept at the output
		// size); the game lives on #1.
		gamescope_xwayland_server_t *pServer = wlserver_get_xwayland_server( g_nXWaylandCount > 1 ? 1 : 0 );
		if ( pServer && pServer->ctx )
			return { pServer->ctx->root_width, pServer->ctx->root_height };
		return { g_nNestedWidth, g_nNestedHeight };
	}

	static int ClampDim( int n )
	{
		return std::clamp( n, kMinDim, kMaxDim );
	}

	// Persists GamescopeSettings::nested_width/height/refresh_hz (0 = as
	// launched, see ConfigSchema.h's comment on that field) -- this is the
	// single write point for all three. Native resolution passes the actual
	// output pixel size here (steamcompmgr_set_nested_mode() has no other way
	// to mean "native"), so storing nWidth/nHeight verbatim would freeze
	// today's monitor size into config instead of "as launched"; checking
	// s_nAspectChoice -- already set to kAspectNative by the caller before
	// this runs -- is what tells the two apart. Follow-host refresh needs no
	// such check: SetRefreshChoice's kRefreshFollowHost branch already
	// passes nRefreshmHz == 0 down to here. Window (output) size is
	// deliberately not persisted anywhere -- host window rules are the right
	// tool for that (see the area's top-of-file comment).
	static void ApplyNestedMode( int nWidth, int nHeight, int nRefreshmHz )
	{
		// Config first, live second -- see ApplyEdit(). This setter is
		// reachable (Aspect/Resolution/refresh rows, or overlay_e2_set)
		// before any other area of this panel has drawn, the same "first
		// Upscaling edit of the session" trap. The live push does not carry
		// the nested mode today, so only half the hazard bites here; the
		// order is uniform anyway rather than depending on that staying true.
		const int nW = ClampDim( nWidth );
		const int nH = ClampDim( nHeight );
		ApplyEdit(
			[ nW, nH, nRefreshmHz ]( config::Settings &cfg ) {
				cfg.gamescope.nested_width  = ( s_nAspectChoice == kAspectNative ) ? 0 : nW;
				cfg.gamescope.nested_height = ( s_nAspectChoice == kAspectNative ) ? 0 : nH;
				cfg.gamescope.nested_refresh_hz = nRefreshmHz ? ConvertmHzToHz( nRefreshmHz ) : 0;
			},
			[ nW, nH, nRefreshmHz ] { steamcompmgr_set_nested_mode( nW, nH, nRefreshmHz ); } );
	}

	static int CustomWidth()  { return s_nCustomWidth  ? s_nCustomWidth  : g_nNestedWidth; }
	static int CustomHeight() { return s_nCustomHeight ? s_nCustomHeight : g_nNestedHeight; }

	// Records the pick against the live mode it was made on (after any
	// apply, so a size pick is recorded against the size it applied).
	static void RecordPick( int nAspect, int nSize )
	{
		s_nAspectChoice = nAspect;
		s_nSizeChoice   = nSize;
		s_nPickWidth    = g_nNestedWidth;
		s_nPickHeight   = g_nNestedHeight;
	}

	static bool PickStillLive()
	{
		return s_nAspectChoice >= 0
		    && g_nNestedWidth == s_nPickWidth && g_nNestedHeight == s_nPickHeight;
	}

	// THE REFLECTION RULE. An explicit pick is trusted for as long as the
	// live mode is still the one it was made against -- which is what lets
	// a shape stay selected while the user browses its list, and what tells
	// "Native" from "16:9 + 1920 x 1080" in a 1920x1080 window. The moment
	// the mode changes for any other reason (a preset applied elsewhere,
	// Steam's atom, a per-game switch, a config apply), the live mode is
	// classified on its own: a size on any list -> that shape; the window's
	// own size -> Native; anything else -> Custom.
	//
	// NO MORE "shape + Custom" (requests-2026-09-07 item 3). Before this, a
	// size that was merely close to a shape's nominal ratio (NearestAspect(),
	// within kAspectTolerance) classified as that shape with the size row
	// showing its "Custom" entry -- which is exactly the entry item 3 removes
	// from the four real lists above. There is now nothing for that state to
	// display, so a live mode that does not EXACTLY match an entry on any
	// list -- a launch-time -w 1600 -h 1200, a console/config nested_width
	// that isn't one of these numbers -- classifies straight as the Custom
	// ASPECT instead, which is where Custom now exclusively lives; the
	// steppers then show the real (unmatched) size, honestly, rather than a
	// shape label paired with a "Custom" option nobody could see was
	// selected. A persisted exact 1280x960 still comes back as "4:3 + 1280 x
	// 960" with no pick stored anywhere; a persisted 1300x975 (same ratio,
	// no exact entry) now comes back as "Custom" rather than "4:3 + Custom".
	static int CurrentAspect()
	{
		if ( PickStillLive() )
			return s_nAspectChoice;
		// The no-trusted-pick half is ClassifyAspect() (ResolutionPresets.h),
		// pure and unit-tested -- this function's own job is just the
		// "trust an explicit pick while it is still live" half above, which
		// needs the real globals.
		return ClassifyAspect( g_nNestedWidth, g_nNestedHeight, (int)g_nOutputWidth, (int)g_nOutputHeight );
	}

	// The one Resolution row's option set: the live aspect's list, or the
	// single true entry for Native / Custom, which are sizes rather than
	// shapes with a list. Handed to Entry::OptionsFrom(), so it is re-asked
	// whenever the row is drawn, searched or set.
	static std::vector<ui::Option> SizeOptionsForAspect( int nAspect )
	{
		const auto Vec = []( const ui::Option *p, size_t n ) {
			return std::vector<ui::Option>( p, p + n );
		};
		switch ( nAspect )
		{
			case kAspect16x9:  return Vec( kSizeOptions16x9,  std::size( kSizeOptions16x9 ) );
			case kAspect4x3:   return Vec( kSizeOptions4x3,   std::size( kSizeOptions4x3 ) );
			case kAspect16x10: return Vec( kSizeOptions16x10, std::size( kSizeOptions16x10 ) );
			case kAspect21x9:  return Vec( kSizeOptions21x9,  std::size( kSizeOptions21x9 ) );
			case kAspectNative: return Vec( kSizeOptionsNative, std::size( kSizeOptionsNative ) );
			default:            return Vec( kSizeOptionsCustom, std::size( kSizeOptionsCustom ) );
		}
	}

	// The value the Resolution row shows: the live mode's entry when it is on
	// the live shape's list, else Custom. A row shows what IS, never a
	// suggestion of what a pick would do -- so a shape whose list has no live
	// entry reads "Custom" with the steppers at the live size, which is the
	// truth.
	static int CurrentSizeChoice( const AspectList &list )
	{
		const int nMatch = MatchSizePreset( list.pSizes, list.nSizes, g_nNestedWidth, g_nNestedHeight );
		return nMatch > 0 ? nMatch : kSizeCustom;
	}

	static int CurrentSizeChoice()
	{
		const AspectList *pList = ListFor( CurrentAspect() );
		return pList ? CurrentSizeChoice( *pList ) : kSizeCustom;
	}

	// The lock's one and only write point (besides the guarded fallback
	// inside SetCustomWidth()/SetCustomHeight() -- see their comments).
	// Every other capture site below funnels through this.
	static void CaptureLockedAspect( int nWidth, int nHeight )
	{
		s_nLockRefWidth  = nWidth;
		s_nLockRefHeight = nHeight;
	}

	// Custom is seeded from the live mode every time it is picked -- which
	// is the last preset applied, so a 4:3 pick followed by Custom starts
	// the steppers at a 4:3 size and the lock (re-captured here) holds 4:3.
	// Picking Custom therefore changes nothing until a stepper moves.
	static void SeedCustomFromLive()
	{
		s_nCustomWidth  = g_nNestedWidth;
		s_nCustomHeight = g_nNestedHeight;
		CaptureLockedAspect( s_nCustomWidth, s_nCustomHeight );
	}

	// Forward-declared: SetSizeChoice() below redirects a stray kSizeCustom
	// to it, and SetAspectChoice() itself isn't defined until further down
	// this file (it calls SetSizeChoice() too, for the "picking a shape
	// applies the closest size" behaviour).
	static void SetAspectChoice( int nChoice );

	static void SetSizeChoice( const AspectList &list, int nChoice )
	{
		if ( nChoice == kSizeCustom )
		{
			// Custom is no longer one of a shape's own options (item 3) -- a
			// caller that still asks for it (an old console/config value;
			// the dropdown itself never offers kSizeCustom for a real shape
			// any more) is redirected to the Aspect row's own Custom rather
			// than landing on a "shape + Custom" state the size list can no
			// longer represent. SetAspectChoice() does the seeding.
			SetAspectChoice( kAspectCustom );
			return;
		}
		if ( nChoice < 1 || nChoice > (int)list.nSizes )
			return;
		// ApplyNestedMode() keys the persisted "as launched" zero on the
		// aspect, so the pick must be set before the apply; the live size
		// is captured after it.
		s_nAspectChoice = list.nAspect;
		s_nSizeChoice   = nChoice;
		ApplyNestedMode( list.pSizes[ nChoice - 1 ].nWidth, list.pSizes[ nChoice - 1 ].nHeight, g_nNestedRefresh );
		RecordPick( list.nAspect, nChoice );
	}

	// The single Resolution row's setter. Native and Custom have no list, so
	// the only value their one-entry option set can carry is kSizeCustom,
	// which for them means "the size the aspect row already applied" -- a
	// no-op rather than a second way to change the mode.
	static void SetSizeChoice( int nChoice )
	{
		if ( const AspectList *pList = ListFor( CurrentAspect() ) )
			SetSizeChoice( *pList, nChoice );
	}

	// Picking a SHAPE now APPLIES a size: the entry of the new shape's list
	// whose height is closest to the height on screen (ties to the wider
	// one), exactly as if it had been clicked in the Resolution row.
	//
	// Why: this REVERSES 2026-09-05's "picking a shape applies nothing", on
	// the user's own instruction of 2026-09-06 -- "When changing the aspect
	// ratio, make it automatically pick the closest resolution (measured by
	// height)." With one Resolution row rather than four greyed ones, a shape
	// pick that changed nothing left the row showing a list that did not
	// describe the picture on screen; landing on the nearest size keeps the
	// two rows and the screen telling the same story. Native and Custom keep
	// their old behaviour (Native applies the window size, Custom seeds the
	// steppers from the live mode and re-applies it, a no-op on screen).
	static void SetAspectChoice( int nChoice )
	{
		if ( nChoice < kAspectNative || nChoice > kAspectCustom )
			return;
		if ( const AspectList *pList = ListFor( nChoice ) )
		{
			SetSizeChoice( *pList, ClosestByHeight( *pList, g_nNestedHeight ) );
			return;
		}
		s_nAspectChoice = nChoice;
		s_nSizeChoice   = -1;
		if ( nChoice == kAspectNative )
		{
			// "Native" is the window size AT THE MOMENT OF THE PICK. It does
			// not track later host resizes -- nothing in gamescope does with
			// one Xwayland (only the multi-Xwayland Steam path re-modes).
			ApplyNestedMode( (int)g_nOutputWidth, (int)g_nOutputHeight, g_nNestedRefresh );
		}
		else
		{
			SeedCustomFromLive();
			ApplyNestedMode( CustomWidth(), CustomHeight(), g_nNestedRefresh );
		}
		RecordPick( nChoice, -1 );
	}

	// The steppers' gate (requests-2026-09-07 item 3): editable EXACTLY when
	// Aspect is Custom, and nothing else. Before this, a "shape + Custom"
	// state (a size on no list, but close enough in ratio to a shape) also
	// enabled the steppers -- that state is gone now that CurrentAspect()
	// itself reports Custom whenever the live size isn't an exact entry on
	// some list (see its own comment), so checking the aspect alone is the
	// whole rule.
	static bool ResolutionIsCustom()
	{
		return CurrentAspect() == kAspectCustom;
	}

	// Deliberately NOT a lazy bootstrap off CustomWidth()/CustomHeight() --
	// that used to read (new width) / (old height) when called from inside
	// SetCustomWidth() after it had already written the new width but
	// before the reference was ever captured (e.g. a fresh session that
	// starts already in Custom): 1280x960, type width 1600, and the ratio
	// silently locked to 1600/960 = 1.667 instead of the real 1280/960 =
	// 1.333, for the rest of the session. The reference is now written
	// only by CaptureLockedAspect() and its guarded fallback
	// (EnsureLockedAspectReference(), below) -- never read out of values a
	// setter is mid-mutation on.
	static float LockedAspect()
	{
		if ( s_nLockRefWidth > 0 && s_nLockRefHeight > 0 )
			return (float)s_nLockRefWidth / (float)s_nLockRefHeight;
		return 16.0f / 9.0f;   // no reference captured yet; never happens once every site below runs
	}

	// The fallback capture for SetCustomWidth()/SetCustomHeight(): a
	// session that starts already in Custom (persisted config, no pick
	// made yet -- PickStillLive() is false because s_nAspectChoice starts
	// at -1) or the live mode moving for some other reason while Custom
	// was active (Steam's mode-control atom, a per-game switch, a config
	// apply -- the same "unpick" CurrentAspect()'s own reflection rule
	// already treats as live-mode-wins) both leave the reference stale or
	// never captured. Called BEFORE either setter mutates
	// s_nCustomWidth/Height, so CustomWidth()/CustomHeight() here still
	// read the pre-edit, live-accurate pair. Once the edit runs,
	// ApplyCustomIfActive()'s RecordPick() makes PickStillLive() true
	// again, so the next edit (and the one after that) skips this and
	// reads the same captured reference instead of re-deriving off the
	// last rounded value -- which is what stopped a hundred stepper steps
	// from drifting the aspect in the first place.
	static void EnsureLockedAspectReference()
	{
		if ( !PickStillLive() )
			CaptureLockedAspect( CustomWidth(), CustomHeight() );
	}

	// Even dimensions only: an odd width or height is a size no display mode
	// uses, and some video paths choke on it.
	static int SnapEven( int n )
	{
		return ClampDim( ( n + 1 ) & ~1 );
	}

	// The steppers are enabled by ResolutionIsCustom() -- the DISPLAYED
	// choice -- which is also true for a launch-time -w/-h that matches no
	// preset, when nobody has picked anything yet. So the apply gate must be
	// the same predicate, evaluated BEFORE the mutation (afterwards the live
	// mode no longer equals the custom numbers), or a stepper the UI shows
	// as enabled would move its number and change nothing.
	//
	// SIMPLIFIED (requests-2026-09-07 item 3): ResolutionIsCustom() is now
	// exactly "Aspect is Custom" -- there is no "shape + Custom" state left
	// to distinguish it from -- so whenever the steppers were enabled the
	// aspect they belong to can only be kAspectCustom, and the pick is always
	// recorded as the Custom shape. The old nAspectBefore parameter (which
	// used to tell "4:3 + Custom" from the bare Custom shape) has nothing
	// left to select between and is gone.
	static void ApplyCustomIfActive( bool bWasActive )
	{
		if ( !bWasActive )
			return;
		s_nAspectChoice = kAspectCustom;   // before the apply: ApplyNestedMode() reads it
		s_nSizeChoice   = -1;
		ApplyNestedMode( CustomWidth(), CustomHeight(), g_nNestedRefresh );
		RecordPick( kAspectCustom, -1 );
	}

	static void SetCustomWidth( int nWidth )
	{
		const bool bActive = ResolutionIsCustom();
		const int nOldHeight = CustomHeight();
		// Read (and, if stale, refresh) the locked reference BEFORE
		// mutating s_nCustomWidth -- see EnsureLockedAspectReference()'s
		// comment for why the order matters.
		if ( s_bLockAspect )
			EnsureLockedAspectReference();
		s_nCustomWidth = ClampDim( nWidth );
		s_nCustomHeight = s_bLockAspect
			? SnapEven( (int)std::lround( s_nCustomWidth / LockedAspect() ) )
			: nOldHeight;
		ApplyCustomIfActive( bActive );
	}

	static void SetCustomHeight( int nHeight )
	{
		const bool bActive = ResolutionIsCustom();
		const int nOldWidth = CustomWidth();
		// Symmetric with SetCustomWidth(): read the reference before
		// mutating s_nCustomHeight.
		if ( s_bLockAspect )
			EnsureLockedAspectReference();
		s_nCustomHeight = ClampDim( nHeight );
		s_nCustomWidth = s_bLockAspect
			? SnapEven( (int)std::lround( s_nCustomHeight * LockedAspect() ) )
			: nOldWidth;
		ApplyCustomIfActive( bActive );
	}

	static void SetLockAspect( bool bLock )
	{
		s_bLockAspect = bLock;
		// Re-capture on every engage: the ratio the user locks is the one on
		// screen when they flip the switch, not the one from last session.
		if ( bLock )
			CaptureLockedAspect( CustomWidth(), CustomHeight() );
	}

	// ---- refresh ----------------------------------------------------------
	// Option values are Hz; 0 = follow host, -1 = Custom.
	static constexpr int kRefreshFollowHost = 0;
	static constexpr int kRefreshCustom = -1;
	static const ui::Option kRefreshOptions[] = {
		{ kRefreshFollowHost, "Follow host" },
		{ 60, "60 Hz" }, { 90, "90 Hz" }, { 120, "120 Hz" },
		{ 144, "144 Hz" }, { 165, "165 Hz" }, { 240, "240 Hz" },
		{ kRefreshCustom, "Custom" },
	};

	static int s_nRefreshChoice = kRefreshFollowHost;   // last pick; live value wins when it disagrees
	static int s_nCustomRefreshHz = 0;                  // 0 = not yet used, seed from live

	static int CurrentRefreshChoice()
	{
		if ( g_nNestedRefresh == 0 )
			return kRefreshFollowHost;
		const int nHz = ConvertmHzToHz( g_nNestedRefresh );
		if ( s_nRefreshChoice == kRefreshCustom )
			return kRefreshCustom;
		for ( const ui::Option &opt : kRefreshOptions )
			if ( opt.nValue > 0 && opt.nValue == nHz )
				return nHz;
		return kRefreshCustom;
	}

	static int CustomRefreshHz()
	{
		return s_nCustomRefreshHz ? s_nCustomRefreshHz : ConvertmHzToHz( EffectiveNestedRefreshmHz() );
	}

	static void SetRefreshChoice( int nChoice )
	{
		s_nRefreshChoice = nChoice;
		int nRefreshmHz;
		if ( nChoice == kRefreshFollowHost )
			nRefreshmHz = 0;
		else if ( nChoice == kRefreshCustom )
		{
			if ( !s_nCustomRefreshHz )
				s_nCustomRefreshHz = std::clamp( CustomRefreshHz(), kMinRefreshHz, kMaxRefreshHz );
			nRefreshmHz = ConvertHztomHz( s_nCustomRefreshHz );
		}
		else
			nRefreshmHz = ConvertHztomHz( std::clamp( nChoice, kMinRefreshHz, kMaxRefreshHz ) );
		ApplyNestedMode( g_nNestedWidth, g_nNestedHeight, nRefreshmHz );
	}

	static bool RefreshIsCustom()
	{
		return CurrentRefreshChoice() == kRefreshCustom;
	}

	// Same gate as the stepper's DisabledUnless (a launch-time -r 75 shows
	// as Custom before anyone picked it), for the reason given at
	// ApplyCustomIfActive().
	static void SetCustomRefreshHz( int nHz )
	{
		const bool bActive = RefreshIsCustom();
		s_nCustomRefreshHz = std::clamp( nHz, kMinRefreshHz, kMaxRefreshHz );
		if ( bActive )
		{
			s_nRefreshChoice = kRefreshCustom;
			ApplyNestedMode( g_nNestedWidth, g_nNestedHeight, ConvertHztomHz( s_nCustomRefreshHz ) );
		}
	}

	// The WINDOW-SIZING GROUP IS GONE (requests-2026-09-06 item 10, the user:
	// "Remove the 'WINDOW'/Window sizing part."). It was three rows asking
	// the host to resize gamescope's own window through
	// INestedHints::RequestOutputSize(); this panel was that call's only
	// caller in the tree, but the interface and its SDL/Wayland
	// implementations stay -- they are backend API, not this area's private
	// helper, and deleting them is a backend change rather than a UI one.
	// Nothing was persisted for it, so there is no config field to drop
	// either. The window's GRANTED size is still reported, as a Live fact.

	static std::string ResolutionSummary()
	{
		const SizePreset root = GameRootSize();
		char sz[ 96 ];
		std::snprintf( sz, sizeof( sz ), "%dx%d @ %d Hz · window %ux%u",
			root.nWidth, root.nHeight, ConvertmHzToHz( EffectiveNestedRefreshmHz() ),
			(unsigned)g_nOutputWidth, (unsigned)g_nOutputHeight );
		return sz;
	}

	static void RegisterResolution( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "display.resolution", "Resolution", ui::Section::Display );
		a.Keywords( "resolution render internal nested game size width height refresh hz hertz "
		            "window output xrandr mode aspect ratio 1080p 1440p 4k 720p" );
		a.Summary( ResolutionSummary );
		// Embedded (DRM) has no window and the display owns the mode; that
		// is a different feature (GetModes() + the dynamic-refresh atom) for
		// a later phase, not a disabled copy of this one.
		a.AvailableWhen( NestedModeAvailable );

		a.Group( "Game resolution" );

		a.Choice( "display.resolution.aspect", "Aspect",
			ui::AnyBind::Of<int>(
				[]{ return CurrentAspect(); },
				[]( int n ) { SetAspectChoice( n ); } ),
			kAspectOptions, std::size( kAspectOptions ) )
			.Help( "Pick the shape first; the Resolution list below then offers common sizes for "
			       "it, and switching shape jumps straight to the closest size to the one you are "
			       "on. Native uses the window's size; Custom lets you type any size." )
			.Default( kAspectNative )
			.Keywords( "resolution aspect ratio shape native custom 16:9 4:3 16:10 21:9 widescreen "
			           "ultrawide" );

		// ONE size row, whose option list is the aspect's (item 7). Forced to
		// a dropdown: the lists run to seven entries of "3840 x 2400", which
		// no segmented strip can hold.
		a.Choice( "display.resolution.size", "Resolution",
			ui::AnyBind::Of<int>(
				[]{ return CurrentSizeChoice(); },
				[]( int n ) { SetSizeChoice( n ); } ),
			kSizeOptions16x9, std::size( kSizeOptions16x9 ) )
			.OptionsFrom( []{ return SizeOptionsForAspect( CurrentAspect() ); } )
			.Dropdown()
			.Help( "Common sizes for the shape picked above; the game sees and renders at the one "
			       "you pick and gamescope scales it to the window. Most games switch instantly; a "
			       "few only list it after a restart. Custom hands the size to Width and Height "
			       "below." )
			.Keywords( "resolution render internal nested preset size 16:9 4:3 16:10 21:9 "
			           "1080p 1440p 4k 720p 1200p" )
			.DisabledUnless( []{ return ListFor( CurrentAspect() ) != nullptr; },
			                 "pick a shape (16:9, 4:3, 16:10, 21:9) in Aspect above to choose a size" );

		static constexpr const char *kNotCustom =
			"pick Custom in Aspect above to type your own size";

		a.Stepper( "display.resolution.width", "Width",
			ui::AnyBind::Of<int>(
				[]{ return CustomWidth(); },
				[]( int n ) { SetCustomWidth( n ); } ) )
			.Key( "gamescope.nested_width" )
			.Help( "Width of the custom resolution the game renders at. With the aspect lock on, "
			       "the height follows so the picture keeps its shape." )
			.Range( (float)kMinDim, (float)kMaxDim )
			.Step( (float)kDimStep )
			.Unit( "px" )
			.Default( 1280 )
			.Keywords( "width custom horizontal pixels" )
			.DisabledUnless( ResolutionIsCustom, kNotCustom )
			.Param( "lock_aspect", "Lock aspect ratio",
				ui::AnyBind::Of<bool>(
					[]{ return s_bLockAspect; },
					[]( bool b ) { SetLockAspect( b ); } ) )
				.Help( "Keeps width and height in the same proportion as when you switched this on, "
				       "so changing one adjusts the other. Off lets you set them independently." )
				.Default( true )
				.Keywords( "aspect ratio lock proportion 16:9" );

		a.Stepper( "display.resolution.height", "Height",
			ui::AnyBind::Of<int>(
				[]{ return CustomHeight(); },
				[]( int n ) { SetCustomHeight( n ); } ) )
			.Key( "gamescope.nested_height" )
			.Help( "Height of the custom resolution the game renders at. With the aspect lock on, "
			       "the width follows." )
			.Range( (float)kMinDim, (float)kMaxDim )
			.Step( (float)kDimStep )
			.Unit( "px" )
			.Default( 720 )
			.Keywords( "height custom vertical pixels" )
			.DisabledUnless( ResolutionIsCustom, kNotCustom );

		a.Group( "Refresh" );

		a.Choice( "display.refresh", "Refresh rate",
			ui::AnyBind::Of<int>(
				[]{ return CurrentRefreshChoice(); },
				[]( int n ) { SetRefreshChoice( n ); } ),
			kRefreshOptions, std::size( kRefreshOptions ) )
			.Key( "gamescope.nested_refresh_hz" )
			.Help( "The refresh rate the game is paced at and told about. Follow host uses your "
			       "screen's own rate. Above the host's rate, frames are paced faster than the "
			       "screen can show them -- this cannot change the monitor's real refresh." )
			.Default( kRefreshFollowHost )
			.Keywords( "refresh rate hz hertz vblank pacing follow host 60 120 144" );

		a.Stepper( "display.refresh.custom", "Custom refresh",
			ui::AnyBind::Of<int>(
				[]{ return CustomRefreshHz(); },
				[]( int n ) { SetCustomRefreshHz( n ); } ) )
			.Key( "gamescope.nested_refresh_hz" )
			.Help( "A refresh rate not in the list above. Takes effect while Refresh rate is set to "
			       "Custom." )
			.Range( (float)kMinRefreshHz, (float)kMaxRefreshHz )
			.Step( 1.0f )
			.Unit( "Hz" )
			.Default( 60 )
			.Keywords( "custom refresh hz hertz" )
			.DisabledUnless( RefreshIsCustom, "pick Custom in Refresh rate above to use this number" );

		a.Group( "Diagnostics" );

		// The "Game sees" line is gone from BOTH the summary and the facts
		// (requests-2026-09-06 item 9, the user: "For the Live state, remove
		// the 'Game sees' part, but keep the rest."). What it read -- the
		// game Xwayland root -- still drives the area's own rail summary
		// (ResolutionSummary()), which is where the number a player looks
		// for actually belongs.
		//
		// THE LINE ITSELF (requests-2026-09-07 item 4, the user: 'Use the
		// terminology "nested" and "output". Make the line like this: "nested
		// <nested_res>@<nested_refresh> · output <output_res>@
		// <output_refresh>"'). Replaces the old "paced at R Hz · window WxH
		// · host R Hz" -- same numbers, but named for what they actually are
		// (the code's own g_nNested*/g_nOutput* prefixes) and now including
		// the nested RESOLUTION, which the old line dropped entirely. See
		// TERMINOLOGY.md's "Nested resolution/refresh" and "Output
		// resolution/refresh" entries for why those words mean this. The
		// three sub-facts the new line replaces ("paced at", "window", "host
		// refresh") are removed below rather than kept alongside it --
		// nothing they said is missing from the one line above them.
		a.Facts( "display.resolution_facts", "Live state", []{
			return FormatLiveLine( g_nNestedWidth, g_nNestedHeight, ConvertmHzToHz( EffectiveNestedRefreshmHz() ),
				(int)g_nOutputWidth, (int)g_nOutputHeight, ConvertmHzToHz( g_nOutputRefresh ) );
		} )
			.Help( "Shows the nested resolution and refresh the game is actually being paced at, "
			       "and the output resolution and refresh your screen actually granted. Read-only." )
			.Keywords( "live state actual xrandr root nested output window host refresh facts" )
			// The NeedsRestart caveat, as a fact until the shell draws
			// ui::Applies::NeedsRestart itself.
			.Live( "takes effect", []{
				return ui::Fact{ "takes effect",
					"now for most games; some only list the new mode after a restart" };
			} )
			.Live( "applied via", []{
				return ui::Fact{ "applied via",
					"steamcompmgr_set_nested_mode() -> wlserver_set_xwayland_server_mode(), the same "
					"path as Steam's GAMESCOPE_XWAYLAND_MODE_CONTROL" };
			} );
	}

	// ---- the frame budget, and what was spent against it -----------------
	// The frametime is read straight off the commit clock rather than
	// through FpsDisplay, because that file's smoothed copy only advances
	// while the HUD is drawing -- a budget meter reading 0 whenever the HUD
	// is switched off would be exactly the "renders but does nothing"
	// defect this row exists to answer.
	//
	// Shared by display.budget_meter's scalar and its four Live facts, so
	// the bar and the numbers under it can never disagree.
	static float FrameBudgetMs()
	{
		const int nCap = Cfg().gamescope.fps_limit;
		if ( nCap > 0 )
			return 1000.0f / (float)nCap;
		// g_nOutputRefresh is mHz, so ms = 1e6 / mHz.
		if ( g_nOutputRefresh > 0 )
			return 1000000.0f / (float)g_nOutputRefresh;
		return 1000.0f / 60.0f;
	}

	static float LastFrametimeMs()
	{
		const uint64_t ul = g_ulLastAppFrametimeNs.load( std::memory_order_relaxed );
		return ul ? (float)ul / 1e6f : 0.0f;
	}

	static void RegisterFrameLimiter( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "display.frame_limiter", "Frame limiter", ui::Section::Display );
		a.Keywords( "fps frame rate cap limiter throttle" );
		a.Summary( []{
			const int n = Cfg().gamescope.fps_limit;
			return n == 0 ? std::string( "uncapped" ) : std::to_string( n ) + " fps cap";
		} );

		a.Group( "Frame limiter" );

		// ISSUE #67, AND WHY THIS IS A STEPPER AND NOT A SLIDER.
		//
		// The valid set is 0 (unlimited) OR [10, 480] -- it is NOT the
		// continuum 0..480. 1-9 fps is a trap: at that rate this overlay
		// itself repaints a few times a second, so a user who lands there can
		// no longer practically drive the UI to undo it.
		//
		// The legacy tab expressed the gap with two controls, an "Unlimited"
		// toggle plus a 10..480 slider, because widgets::SliderInt has no
		// notion of a hole in its range. A stepper has no hole either -- but
		// it does not need one: with a step of 10 anchored at 0, the reachable
		// set IS {0, 10, 20, ... 480}. The gap is a consequence of the step,
		// not a special case anyone has to maintain, and one setting is now
		// one row instead of two. SetFpsLimit() still clamps every write, so
		// the floor holds for the ConCommand and gamescope_control paths too.
		a.Stepper( "display.fps_limit", "FPS limit",
			ui::AnyBind::Of<int>(
				[]{ return Cfg().gamescope.fps_limit; },
				[]( int n ) { SetFpsLimit( n ); } ) )
			.Key( "gamescope.fps_limit" )
			.Help( "Limits how many frames per second the game can show. Stepping down from 10 "
			       "jumps straight to Unlimited -- the overlay itself gets too slow below that." )
			.Range( 0.0f, (float)kMaxFpsLimit )
			.Step( (float)kMinFpsLimit )
			.Unit( "fps" )
			.ZeroMeans( "Unlimited" )
			.Default( 0 )
			.Keywords( "fps frame rate cap limit limiter throttle unlimited" );

		// Adaptive sync (VRR) lived here until the user corrected D13.1
		// (2026-08-24): "VRR shouldnt be placed in 'Frame Limiter'." It moved
		// to display.general -- see RegisterGeneral() below and
		// AUTONOMOUS-DECISIONS.md's D13.1 correction note. The Diagnostics
		// facts below are unaffected: they read the limiter's own pacing
		// state, never cv_adaptive_sync.

		a.Group( "Diagnostics" );

		// SPEC §3.8's drawn instance. The kind was declared in the first
		// version, `controls::Meter()` was implemented, `UsesValueColumn`
		// and `IsReadOnly` both handled it -- and nothing registered one, so
		// the rendering had never run in the product. That is the same smell
		// as `nColumns` computing a number nothing drew (D20.2), with the
		// arrow reversed, and P5 resolves it the same way D20 did: by
		// building the thing the spec names, not by deleting the kind the
		// spec specifies.
		//
		// WHY A PERCENTAGE AND NOT MILLISECONDS. A Meter's range is fixed at
		// registration, and the frame budget is not: it is the cap when
		// there is one and the output's refresh interval otherwise, so a
		// millisecond range would be wrong the moment either changed. As a
		// share of budget the range is 0..100 by construction, and it means
		// the same thing at 60 Hz capped and 240 Hz uncapped -- which is
		// what makes the bar comparable to itself across sessions.
		//
		// Saturating at 100 % is deliberate. Over budget is over budget; how
		// far over is what the exact numbers below and the HUD's frametime
		// graph are for, and letting the bar overrun its own track would
		// break SPEC §2.2's right bound.
		a.Meter( "display.budget_meter", "Frame budget",
			[]() -> double {
				const float flBudget = FrameBudgetMs();
				if ( flBudget <= 0.0f )
					return 0.0;
				return std::clamp( LastFrametimeMs() / flBudget * 100.0f, 0.0f, 100.0f );
			}, 0.0, 100.0 )
			.Help( "Shows how much of its allotted time each frame is using. Read-only -- if it "
			       "stays at 100%, frames are running late and you're about to see stutter." )
			.Unit( " %" )
			.Keywords( "frame budget meter frametime headroom pacing stutter missed deadline" )
			.DisabledUnless(
				[]{ return LastFrametimeMs() > 0.0f; },
				"no application frame has been presented yet" )
			.Live( "frame time", []{
				char sz[ 32 ];
				std::snprintf( sz, sizeof( sz ), "%.2f ms", LastFrametimeMs() );
				return ui::Fact{ "frame time", sz };
			} )
			.Live( "budget", []{
				char sz[ 32 ];
				std::snprintf( sz, sizeof( sz ), "%.2f ms", FrameBudgetMs() );
				return ui::Fact{ "budget", sz };
			} )
			// Names which of the two sources the budget came from, for the
			// same reason limiter_facts names its apply path: "the number is
			// wrong" is nearly always "it is measuring the other one".
			.Live( "budget from", []{
				return ui::Fact{ "budget from",
					Cfg().gamescope.fps_limit > 0 ? "the FPS cap"
					: ( g_nOutputRefresh > 0 ? "the output refresh" : "60 Hz fallback" ) };
			} )
			.Live( "headroom", []{
				const float flBudget = FrameBudgetMs(), flFrame = LastFrametimeMs();
				char sz[ 48 ];
				std::snprintf( sz, sizeof( sz ), "%.2f ms %s", std::fabs( flBudget - flFrame ),
					flFrame > flBudget ? "over" : "spare" );
				return ui::Fact{ "headroom", sz };
			} );

		a.Facts( "display.limiter_facts", "Limiter state", []{
			const int n = Cfg().gamescope.fps_limit;
			return n == 0 ? std::string( "idle -- no cap" ) : std::to_string( n ) + " fps requested";
		} )
			.Help( "Shows the FPS limit you asked for and how it's being applied. Read-only." )
			.Keywords( "limiter state cap refresh cycle override" )
			.Live( "requested", []{
				const int n = Cfg().gamescope.fps_limit;
				return ui::Fact{ "requested", n == 0 ? "unlimited" : ( std::to_string( n ) + " fps" ) };
			} )
			.Live( "valid set", []{
				char sz[ 64 ];
				std::snprintf( sz, sizeof( sz ), "0, or %d - %d fps", kMinFpsLimit, kMaxFpsLimit );
				return ui::Fact{ "valid set", sz };
			} )
			// Names the mechanism, because "the control writes but the value
			// does not stick" was issue #25 and the answer to it is which of
			// the two paths is load-bearing.
			.Live( "applied via", []{
				return ui::Fact{ "applied via",
					"steamcompmgr_set_app_refresh_cycle_override() -- the GAMESCOPE_FPS_LIMIT "
					"X11 property is written alongside it for external readers only" };
			} )
			.Live( "screen type", []{
				return ui::Fact{ "screen type",
					GetBackend()->GetScreenType() == GAMESCOPE_SCREEN_TYPE_INTERNAL ? "internal" : "external" };
			} );
	}

	static void RegisterHdr( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "display.hdr", "HDR", ui::Section::Display );
		a.Keywords( "hdr pq bt2020 wide gamut tonemap sdr brightness nits gain" );
		a.Summary( []{
			return cv_hdr_enabled.Get() ? std::string( "on" ) : std::string( "off" );
		} );

		// The HDR-enable switch lives in the same grouping as the settings it
		// gates -- issue #66. Taxonomically it is a Display-ish setting, but
		// separating a switch from what it governs costs a user more (a
		// control they have to remember lives elsewhere) than the tidier
		// categorisation gains.
		a.Group( "Output" );

		a.Switch( "display.hdr_enabled", "HDR output",
			ui::AnyBind::Of<bool>(
				[]{ return cv_hdr_enabled.Get(); },
				[]( bool b ) {
					ApplyEdit(
						[ b ]( config::Settings &cfg ) { cfg.gamescope.hdr_enabled = b; },
						[ b ] { cv_hdr_enabled = b; } );
				} ) )
			.Key( "gamescope.hdr_enabled" )
			.Help( "Turns on HDR for richer colour and brighter highlights, on a screen that "
			       "supports it. Every other setting in this area only matters while this is on." )
			.Default( false )
			.Keywords( "hdr pq bt2020 wide gamut high dynamic range" );

		// One predicate, declared once and reused, so the four gated rows
		// cannot drift apart about what gates them.
		const auto HdrOn = []{ return cv_hdr_enabled.Get(); };
		static constexpr const char *kHdrOff = "HDR output is off -- this does nothing until it is on";

		// sdrGamutWideness defaults to -1 ("unset / display-native"); only the
		// DISPLAYED value is clamped into 0..1, so the -1 is not silently
		// written back over before the user has touched the control.
		a.Slider( "display.sdr_gamut_wideness", "SDR gamut wideness",
			ui::AnyBind::Of<float>(
				[]{ return std::clamp( g_ColorMgmt.pending.sdrGamutWideness, 0.0f, 1.0f ); },
				[]( float f ) {
					ApplyEdit(
						[ f ]( config::Settings &cfg ) { cfg.gamescope.sdr_gamut_wideness = f; },
						[ f ] { set_color_sdr_gamut_wideness( f ); } );
				} ) )
			.Key( "gamescope.sdr_gamut_wideness" )
			.Help( "Makes colours in regular (non-HDR) content richer by stretching them toward "
			       "your screen's wider colour range. 0 leaves colours exactly as the game intended." )
			.Range( 0.0f, 1.0f )
			.Step( 0.05f )       // 21 positions across a 0..1 normalised amount
			.Default( 0.0f )
			.Keywords( "gamut wideness sdr saturation bt709 bt2020" )
			.DisabledUnless( HdrOn, kHdrOff );

		a.Slider( "display.sdr_on_hdr_brightness", "SDR-on-HDR brightness",
			ui::AnyBind::Of<float>(
				[]{ return g_ColorMgmt.pending.flSDROnHDRBrightness; },
				[]( float f ) {
					ApplyEdit(
						[ f ]( config::Settings &cfg ) { cfg.gamescope.sdr_on_hdr_brightness_nits = f; },
						[ f ] { set_sdr_on_hdr_brightness( f ); } );
				} ) )
			.Key( "gamescope.sdr_on_hdr_brightness_nits" )
			.Help( "Sets how bright regular (non-HDR) content looks when it's shown next to HDR "
			       "content." )
			.Range( 50.0f, 1000.0f )
			// 96 positions. The 203-nit default is deliberately NOT on this
			// grid and does not need to be: only a DRAG is quantised, so the
			// reset chip, the arrow keys and `overlay_e2_set` all still reach
			// SDR reference white exactly (Registry.cpp's SnapDragsTo()).
			.Step( 10.0f )
			.Unit( "nits" )
			.Default( 203.0f )
			.Keywords( "sdr brightness nits paper white luminance" )
			.DisabledUnless( HdrOn, kHdrOff );

		a.Group( "Input gain" );

		a.Slider( "display.hdr_input_gain", "HDR input gain",
			ui::AnyBind::Of<float>(
				[]{ return g_ColorMgmt.pending.flHDRInputGain; },
				[]( float f ) {
					ApplyEdit(
						[ f ]( config::Settings &cfg ) { cfg.gamescope.hdr_input_gain = f; },
						[ f ] { set_hdr_input_gain( f ); } );
				} ) )
			.Key( "gamescope.hdr_input_gain" )
			.Help( "Turns HDR content brighter or dimmer before it's shown on screen." )
			.Range( 0.0f, 4.0f )
			.Step( 0.05f )       // 81 positions; 1.00x, the default, is on the grid
			.Unit( "x" )
			.Default( 1.0f )
			.Keywords( "hdr input gain multiplier exposure" )
			.DisabledUnless( HdrOn, kHdrOff );

		a.Slider( "display.sdr_input_gain", "SDR input gain",
			ui::AnyBind::Of<float>(
				[]{ return g_ColorMgmt.pending.flSDRInputGain; },
				[]( float f ) {
					ApplyEdit(
						[ f ]( config::Settings &cfg ) { cfg.gamescope.sdr_input_gain = f; },
						[ f ] { set_sdr_input_gain( f ); } );
				} ) )
			.Key( "gamescope.sdr_input_gain" )
			.Help( "Turns regular (non-HDR) content brighter or dimmer before it's blended in with "
			       "the HDR picture." )
			.Range( 0.0f, 4.0f )
			.Step( 0.05f )       // 81 positions, as HDR input gain above
			.Unit( "x" )
			.Default( 1.0f )
			.Keywords( "sdr input gain multiplier exposure" )
			.DisabledUnless( HdrOn, kHdrOff );

		a.Group( "Diagnostics" );

		// The old HDR tab's read-only appHDRMetadata strip, unchanged in
		// substance: this is what the focused app REPORTED, never a setting.
		// A Facts row cannot be given a control at all (Registry.h -- .Live()
		// has no Bind overload), which is a stronger guarantee than the
		// legacy "never editable here" comment was.
		a.Facts( "display.hdr_facts", "Signal", []{
			if ( !g_ColorMgmt.current.appHDRMetadata )
				return std::string( "no HDR metadata reported" );
			const hdr_metadata_infoframe &info =
				g_ColorMgmt.current.appHDRMetadata->View<hdr_output_metadata>().hdmi_metadata_type1;
			char sz[ 64 ];
			std::snprintf( sz, sizeof( sz ), "MaxCLL %u · MaxFALL %u nits",
				(unsigned)info.max_cll, (unsigned)info.max_fall );
			return std::string( sz );
		} )
			.Help( "Shows the HDR brightness info the game itself is sending. Read-only, this is "
			       "exactly what the game reports, not a setting you can change." )
			.Keywords( "metadata maxcll maxfall mastering primaries white point tonemap" )
			.Live( "source", []{
				return ui::Fact{ "source", g_ColorMgmt.current.appHDRMetadata
					? "app-provided (surface metadata)"
					: "none -- no HDR-capable app is currently focused" };
			} )
			.Live( "content light", []{
				if ( !g_ColorMgmt.current.appHDRMetadata )
					return ui::Fact{ "content light", "-" };
				const hdr_metadata_infoframe &info =
					g_ColorMgmt.current.appHDRMetadata->View<hdr_output_metadata>().hdmi_metadata_type1;
				char sz[ 64 ];
				std::snprintf( sz, sizeof( sz ), "MaxCLL %u nits · MaxFALL %u nits",
					(unsigned)info.max_cll, (unsigned)info.max_fall );
				return ui::Fact{ "content light", sz };
			} )
			.Live( "mastering", []{
				if ( !g_ColorMgmt.current.appHDRMetadata )
					return ui::Fact{ "mastering", "-" };
				const hdr_metadata_infoframe &info =
					g_ColorMgmt.current.appHDRMetadata->View<hdr_output_metadata>().hdmi_metadata_type1;
				char sz[ 80 ];
				std::snprintf( sz, sizeof( sz ), "%u / %.4f nits (max/min)",
					(unsigned)info.max_display_mastering_luminance,
					info.min_display_mastering_luminance * 0.0001f );
				return ui::Fact{ "mastering", sz };
			} )
			.Live( "primaries", []{
				if ( !g_ColorMgmt.current.appHDRMetadata )
					return ui::Fact{ "primaries", "-" };
				const hdr_metadata_infoframe &info =
					g_ColorMgmt.current.appHDRMetadata->View<hdr_output_metadata>().hdmi_metadata_type1;
				// CTA-861.G coordinates are unsigned 16-bit in units of 0.00002.
				const auto Chroma = []( uint16_t uRaw ) { return uRaw * 0.00002f; };
				char sz[ 160 ];
				std::snprintf( sz, sizeof( sz ),
					"R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) white(%.4f,%.4f)",
					Chroma( info.display_primaries[0].x ), Chroma( info.display_primaries[0].y ),
					Chroma( info.display_primaries[1].x ), Chroma( info.display_primaries[1].y ),
					Chroma( info.display_primaries[2].x ), Chroma( info.display_primaries[2].y ),
					Chroma( info.white_point.x ), Chroma( info.white_point.y ) );
				return ui::Fact{ "primaries", sz };
			} )
			// The legacy HDR tab carried a visible "Tonemap Operator:
			// deferred" note. It is not a setting and must not become a row,
			// but dropping it silently would lose the one thing it recorded
			// -- WHY there is no control. So it survives as a fact.
			.Live( "tonemap operator", []{
				return ui::Fact{ "tonemap operator",
					"not exposed -- hdrTonemapOperator has no live setter and no X11 property, "
					"unlike the four sliders above. Exposing it needs new plumbing, not just a control." };
			} );
	}

	void PanelDisplay_RegisterAreas( ui::Registry &reg )
	{
		// General registers first so it sits above Upscaling in the rail --
		// see RegisterGeneral()'s own comment for why.
		RegisterGeneral( reg );
		RegisterUpscaling( reg );
		// Item 7 (2026-09-05): right after Upscaling, since "what the game
		// renders at" and "how it is scaled to the window" are one decision
		// seen from two sides.
		RegisterResolution( reg );
		RegisterFrameLimiter( reg );
		RegisterHdr( reg );
	}

}
