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
// Sharpness direction (DECISIONS.md #11, SPEC.md Feature 4): FSR and NIS
// remap the same raw 0..20 g_upscaleFilterSharpness value in opposite visual
// directions. This was verified empirically here, not just by reading the
// shader math (the RCAS constant-setup comment reads ambiguously in
// isolation) -- see RawSharpnessFromUiPercent()'s comment for the actual
// screenshot-verified directions. Do not "fix" the inversion by making the
// two filters share a single raw-value meaning -- upstream
// (ValveSoftware/gamescope#515) looked at unifying it and closed it as not
// planned; the UI-side auto-correction below is the permanent fix.
#include "PanelDisplay.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

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
// display.budget_meter. Not declared in any header -- src/Metrics/
// SystemStats.cpp declares its own extern for exactly this variable for
// exactly this reason, so this follows that precedent rather than adding
// header plumbing for one read.
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

	static void EnsureConfigLoaded()
	{
		const uint64_t ulGeneration = config::ConfigGeneration();
		if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
			return;

		const bool bIsReload = s_bConfigLoaded; // false only on this panel's very first draw
		s_CachedSettings = config::ResolveEffective( config::SessionAppId() );
		s_ulLoadedGeneration = ulGeneration;
		s_bConfigLoaded = true;

		// The very first load's values are already applied to the live
		// globals/ConVars by main.cpp's apply_ritz_config_to_startup_state()
		// before this panel ever draws -- only a later reload (PanelConfig
		// applied a profile, enabled/cleared override, or copied another
		// game's config in, bumping the generation) needs pushing here too,
		// or this panel's sliders would show new values that never actually
		// took effect.
		if ( bIsReload )
			PushCachedSettingsToLiveState();
	}

	static void QueueSave()
	{
		config::EnqueueRoutedWrite( s_CachedSettings );
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

	// One slider, always "higher = sharper" regardless of which filter is
	// active (DECISIONS.md #11) -- the raw<->UI-percent mapping direction
	// itself flips between filters underneath.
	//
	// Empirically verified (screenshot comparison of vkcube upscaled at
	// -w 640 -h 480 -W 1920 -H 1080, raw 0 vs raw 20, both filters):
	//   - FSR:  raw 0 visibly softer, raw 20 visibly sharper (more grain/
	//     edge contrast in the logo texture)   => higher raw = sharper.
	//   - NIS:  raw 0 shows a visible bright edge halo around the logo (a
	//     classic over-sharpen ringing artifact), raw 20 is smooth/clean
	//     => higher raw = LESS sharp.
	// This matches SPEC.md/DECISIONS.md's claimed direction, but was checked
	// against the actual rendered output rather than trusted from the shader
	// math alone -- ffx_fsr1.h's own FsrRcasCon() doc comment ("0.0 :=
	// maximum, N>0 = stops of reduction") reads as if it should invert, and
	// static reading here would have gotten this backwards.
	static int RawSharpnessFromUiPercent( GamescopeUpscaleFilter eFilter, int nUiPercent )
	{
		nUiPercent = std::clamp( nUiPercent, 0, 100 );
		if ( eFilter == GamescopeUpscaleFilter::NIS )
			return (int)std::lround( (100 - nUiPercent) * 20.0 / 100.0 );
		return (int)std::lround( nUiPercent * 20.0 / 100.0 ); // FSR (and the no-op filters)
	}

	static int UiPercentFromRawSharpness( GamescopeUpscaleFilter eFilter, int nRaw )
	{
		nRaw = std::clamp( nRaw, 0, 20 );
		if ( eFilter == GamescopeUpscaleFilter::NIS )
			return (int)std::lround( (20 - nRaw) * 100.0 / 20.0 );
		return (int)std::lround( nRaw * 100.0 / 20.0 );
	}

	static void SetFilter( GamescopeUpscaleFilter eFilter )
	{
		g_wantedUpscaleFilter = eFilter;
		s_CachedSettings.gamescope.filter = FilterToString( eFilter );
		QueueSave();
	}

	static void SetScaler( GamescopeUpscaleScaler eScaler )
	{
		g_wantedUpscaleScaler = eScaler;
		s_CachedSettings.gamescope.scaler = ScalerToString( eScaler );
		QueueSave();
	}

	static void SetSharpnessUiPercent( GamescopeUpscaleFilter eFilter, int nUiPercent )
	{
		const int nRaw = RawSharpnessFromUiPercent( eFilter, nUiPercent );
		g_upscaleFilterSharpness = nRaw;
		s_CachedSettings.gamescope.sharpness = nRaw;
		QueueSave();
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

	static void SetFpsLimit( int nFps )
	{
		nFps = ( nFps <= 0 ) ? 0 : std::clamp( nFps, kMinFpsLimit, kMaxFpsLimit );
		s_CachedSettings.gamescope.fps_limit = nFps;
		QueueSave();

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
		a.Keywords( "general quick toggle vrr adaptive sync freesync gsync tearing cursor grab" );
		a.Summary( []{
			std::string s = cv_adaptive_sync.Get() ? "VRR on" : "VRR off";
			s += cv_tearing_enabled.Get() ? " · tearing on" : " · tearing off";
			s += g_bForceRelativeMouse ? " · cursor grabbed" : " · cursor free";
			return s;
		} );

		a.Group( "Quick toggles" );

		a.Switch( "display.adaptive_sync", "Adaptive sync (VRR)",
			ui::AnyBind::Of<bool>(
				[]{ return cv_adaptive_sync.Get(); },
				[]( bool b ) {
					cv_adaptive_sync = b;
					Cfg().gamescope.vrr_enabled = b;
					QueueSave();
				} ) )
			.Help( "Lets the display's refresh follow the game's frame rate instead of the other way "
			       "around. Requires a VRR-capable display and connector." )
			.Default( false )
			.Keywords( "vrr freesync gsync adaptive sync refresh" );

		a.Switch( "display.allow_tearing", "Allow tearing",
			ui::AnyBind::Of<bool>(
				[]{ return cv_tearing_enabled.Get(); },
				[]( bool b ) {
					cv_tearing_enabled = b;
					Cfg().gamescope.tearing_enabled = b;
					QueueSave();
				} ) )
			.Help( "Lets the game present without waiting for the display's refresh. Lowest latency; "
			       "can show a horizontal seam on fast camera pans." )
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
					steamcompmgr_set_force_relative_mouse( b );
					Cfg().gamescope.force_grab_cursor = b;
					QueueSave();
				} ) )
			.Help( "Always use relative mouse mode instead of flipping on cursor visibility. "
			       "Applies immediately." )
			.Default( false )
			.Keywords( "mouse pointer capture confine grab relative" );
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
			.Help( "Resamples the game's image up to the output resolution. FSR and NIS add a "
			       "sharpening pass afterwards; Pixel is nearest-neighbour and is only sharp when "
			       "the scale factor is a whole number." )
			.Default( (int)GamescopeUpscaleFilter::LINEAR )
			.Keywords( "upscale scaling resample fsr nis pixel linear nearest" );

		// The UI percent, not the raw 0..20 -- DECISIONS.md #11. The percent
		// is always "higher = sharper"; the raw value it maps to flips
		// direction between FSR and NIS underneath, which is why this binds
		// through the same two remap functions the legacy slider uses rather
		// than exposing g_upscaleFilterSharpness directly.
		a.Slider( "display.sharpness", "Sharpness",
			ui::AnyBind::Of<int>(
				[]{ return UiPercentFromRawSharpness( g_wantedUpscaleFilter, g_upscaleFilterSharpness ); },
				[]( int n ) { SetSharpnessUiPercent( g_wantedUpscaleFilter, n ); } ) )
			.Help( "Strength of gamescope's own post-upscale sharpening pass (FSR RCAS or NIS). "
			       "Higher is crisper; too high adds ringing around high-contrast edges. This is "
			       "not the Shaders area's Pre-sharpen -- that one is a separate ReShade pass, runs "
			       "before upscaling, and works with any filter. Both are real and can be combined." )
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
			.Default( UiPercentFromRawSharpness( GamescopeUpscaleFilter::FSR, 2 ) )
			.Keywords( "sharpen sharpness rcas cas crisp clarity ringing" )
			.DisabledUnless( SharpnessApplies,
				"only FSR and NIS sharpen -- the Linear, Nearest and Pixel filters have no "
				"sharpening pass, so this has no effect while one of them is selected" );

		a.Choice( "display.scaler", "Scaler",
			ui::AnyBind::Of<int>(
				[]{ return (int)g_wantedUpscaleScaler; },
				[]( int n ) { SetScaler( (GamescopeUpscaleScaler)n ); } ),
			kScalerOptions, std::size( kScalerOptions ) )
			.Help( "What to do when the game's aspect ratio does not match the output. Integer only "
			       "scales by whole numbers, which avoids resampling blur at the cost of black bars." )
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
			.Help( "What the compositor is actually doing right now, resolved after every override. "
			       "Read-only: this is the live state, not the setting." )
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
			.Help( "Caps presentation rate. Stepping down from 10 reaches 0, which removes the cap "
			       "entirely; there is deliberately nothing between the two, because below 10 fps "
			       "this overlay becomes too slow to drive -- including too slow to undo it. "
			       "Applies immediately." )
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
			.Help( "How much of the time available for one frame the game actually spent. The "
			       "budget is the FPS cap when one is set, and the display's refresh interval "
			       "otherwise. At 100 % the frame took its whole budget and the next one is "
			       "already late -- so sustained 100 % is what a stutter looks like before you "
			       "can see it. Read-only." )
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
			.Help( "What the limiter has been asked for, and how that request reaches the "
			       "compositor. Read-only." )
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
					cv_hdr_enabled = b;
					Cfg().gamescope.hdr_enabled = b;
					QueueSave();
				} ) )
			.Help( "Requests an HDR10 signal on the connector and switches the composite pipeline "
			       "to PQ. Everything else in this area is meaningless while this is off." )
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
					set_color_sdr_gamut_wideness( f );
					Cfg().gamescope.sdr_gamut_wideness = f;
					QueueSave();
				} ) )
			.Help( "How far SDR content is stretched toward the display's wide gamut. 0 keeps "
			       "BT.709 exactly. Applies immediately." )
			.Range( 0.0f, 1.0f )
			.Default( 0.0f )
			.Keywords( "gamut wideness sdr saturation bt709 bt2020" )
			.DisabledUnless( HdrOn, kHdrOff );

		a.Slider( "display.sdr_on_hdr_brightness", "SDR-on-HDR brightness",
			ui::AnyBind::Of<float>(
				[]{ return g_ColorMgmt.pending.flSDROnHDRBrightness; },
				[]( float f ) {
					set_sdr_on_hdr_brightness( f );
					Cfg().gamescope.sdr_on_hdr_brightness_nits = f;
					QueueSave();
				} ) )
			.Help( "How bright SDR content looks composited alongside HDR. Applies immediately." )
			.Range( 50.0f, 1000.0f )
			.Unit( "nits" )
			.Default( 203.0f )
			.Keywords( "sdr brightness nits paper white luminance" )
			.DisabledUnless( HdrOn, kHdrOff );

		a.Group( "Input gain" );

		a.Slider( "display.hdr_input_gain", "HDR input gain",
			ui::AnyBind::Of<float>(
				[]{ return g_ColorMgmt.pending.flHDRInputGain; },
				[]( float f ) {
					set_hdr_input_gain( f );
					Cfg().gamescope.hdr_input_gain = f;
					QueueSave();
				} ) )
			.Help( "Multiplier applied to HDR content before tone mapping. Applies immediately." )
			.Range( 0.0f, 4.0f )
			.Unit( "x" )
			.Default( 1.0f )
			.Keywords( "hdr input gain multiplier exposure" )
			.DisabledUnless( HdrOn, kHdrOff );

		a.Slider( "display.sdr_input_gain", "SDR input gain",
			ui::AnyBind::Of<float>(
				[]{ return g_ColorMgmt.pending.flSDRInputGain; },
				[]( float f ) {
					set_sdr_input_gain( f );
					Cfg().gamescope.sdr_input_gain = f;
					QueueSave();
				} ) )
			.Help( "Multiplier applied to SDR content before it is composited into the HDR "
			       "container. Applies immediately." )
			.Range( 0.0f, 4.0f )
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
			.Help( "HDR metadata the focused app is actually sending, read from its surface. Never "
			       "inferred, and never editable -- this is what the app reported." )
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
		RegisterFrameLimiter( reg );
		RegisterHdr( reg );
	}

}
