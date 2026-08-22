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
#include "Chrome.h"

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
		g_bForceRelativeMouse = s_CachedSettings.gamescope.force_grab_cursor;

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
	static void SetFpsLimit( int nFps )
	{
		nFps = std::clamp( nFps, 0, 240 );
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

	static void DrawFilterRow()
	{
		static constexpr struct { GamescopeUpscaleFilter eValue; const char *pszLabel; } kFilters[] = {
			{ GamescopeUpscaleFilter::LINEAR,  "Linear" },
			{ GamescopeUpscaleFilter::NEAREST, "Nearest" },
			{ GamescopeUpscaleFilter::FSR,     "FSR" },
			{ GamescopeUpscaleFilter::NIS,     "NIS" },
			{ GamescopeUpscaleFilter::PIXEL,   "Pixel" },
		};

		ImGui::TextUnformatted( "Filter" );
		// Design guide's "Tabs / segmented controls" section names "filter
		// type" as its own example of this component -- widgets::SegmentedControl
		// replaces the stock (inherently circular, unthemeable-to-flat)
		// ImGui::RadioButton row this used to be.
		{
			// Spec §7 Segmented control: cell labels are lowercase.
			static const char *const kLabels[] = { "linear", "nearest", "fsr", "nis", "pixel" };
			int nSelected = 0;
			for ( size_t i = 0; i < std::size( kFilters ); i++ )
				if ( g_wantedUpscaleFilter == kFilters[i].eValue )
					nSelected = (int)i;

			if ( widgets::SegmentedControl( "##filter", &nSelected, kLabels, (int)std::size( kLabels ) ) )
				SetFilter( kFilters[nSelected].eValue );
		}
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled( "Pixel: sharp only when the scale factor is a whole number." );
		ImGui::PopFont();
	}

	static void DrawScalerRow()
	{
		static constexpr struct { GamescopeUpscaleScaler eValue; const char *pszLabel; } kScalers[] = {
			{ GamescopeUpscaleScaler::AUTO,    "Auto" },
			{ GamescopeUpscaleScaler::INTEGER, "Integer" },
			{ GamescopeUpscaleScaler::FIT,      "Fit" },
			{ GamescopeUpscaleScaler::FILL,      "Fill" },
			{ GamescopeUpscaleScaler::STRETCH,   "Stretch" },
		};

		ImGui::TextUnformatted( "Scaler" );
		{
			static const char *const kLabels[] = { "auto", "integer", "fit", "fill", "stretch" };
			int nSelected = 0;
			for ( size_t i = 0; i < std::size( kScalers ); i++ )
				if ( g_wantedUpscaleScaler == kScalers[i].eValue )
					nSelected = (int)i;

			if ( widgets::SegmentedControl( "##scaler", &nSelected, kLabels, (int)std::size( kLabels ) ) )
				SetScaler( kScalers[nSelected].eValue );
		}
	}

	static void DrawSharpnessSlider()
	{
		// Remap/gate off of what the user actually picked (g_wantedUpscaleFilter),
		// not the possibly Steam-focus-overridden live g_upscaleFilter -- see the
		// warning line below for surfacing that override instead of silently
		// disagreeing with it.
		const GamescopeUpscaleFilter eWanted = g_wantedUpscaleFilter;
		const bool bApplies = ( eWanted == GamescopeUpscaleFilter::FSR || eWanted == GamescopeUpscaleFilter::NIS );

		int nUiPercent = UiPercentFromRawSharpness( eWanted, g_upscaleFilterSharpness );

		if ( !bApplies )
			ImGui::BeginDisabled();

		// widgets::SliderInt draws the numeric readout in Mono/accent per the
		// design guide's numerals-are-always-Mono rule -- see Widgets.h.
		if ( widgets::SliderInt( "Sharpness", &nUiPercent, 0, 100, "%d%% sharper" ) )
			SetSharpnessUiPercent( eWanted, nUiPercent );

		if ( !bApplies )
		{
			ImGui::EndDisabled();
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			ImGui::TextDisabled( "No effect on Linear/Nearest/Pixel -- only FSR and NIS sharpen." );
			ImGui::PopFont();
		}

		// DECISIONS.md #12: this is gamescope's own built-in post-upscale
		// RCAS/NIS sharpen, deliberately distinct from the Shaders panel's
		// "Pre-Sharpen" (a separate ReShade pass, pre-upscale, works
		// regardless of Filter -- including Linear/Nearest/Pixel, where this
		// slider is a no-op). Not the same knob under two names -- both are
		// real, independent effects that can be combined.
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled( "Built-in post-upscale sharpen. See Shaders panel's Pre-Sharpen for a separate, filter-independent one." );
		ImGui::PopFont();
	}

	static void DrawUpscalingTab()
	{
		if ( g_bSteamIsActiveWindow )
		{
			// XXX(misyl)-flagged pre-existing override in steamcompmgr.cpp:
			// while the Steam window itself is focused, the *live* filter/
			// scaler are forced to Fit/Linear regardless of what's picked
			// here. Surface it rather than showing values that silently
			// stop applying (SPEC.md Feature 4).
			ImGui::TextColored( ImVec4( 0.95f, 0.65f, 0.25f, 1.0f ),
				"Steam window is focused: filter/scaler are temporarily forced to Fit/Linear." );
			ImGui::Separator();
		}

		// Sharpness is Filter's own parameter (it remaps per-filter and only
		// affects FSR/NIS's output, not the Scaler), so it's kept directly
		// under Filter with just Spacing() between them -- no Separator() --
		// while a full Separator() marks the boundary to Scaler, which is an
		// unrelated setting. Mirrors the group-boundary-via-Separator()
		// idiom PanelShaders.cpp's DrawVibrancyGroup()/DrawPreSharpenGroup()
		// already use, rather than inventing a new grouping treatment.
		DrawFilterRow();
		ImGui::Spacing();
		DrawSharpnessSlider();
		ImGui::Separator();
		DrawScalerRow();
	}

	static void DrawDisplayTab()
	{
		bool bVrr = cv_adaptive_sync.Get();
		if ( widgets::Toggle( "VRR / Adaptive Sync", &bVrr ) )
		{
			cv_adaptive_sync = bVrr;
			s_CachedSettings.gamescope.vrr_enabled = bVrr;
			QueueSave();
		}

		bool bTearing = cv_tearing_enabled.Get();
		if ( widgets::Toggle( "Allow Tearing", &bTearing ) )
		{
			cv_tearing_enabled = bTearing;
			s_CachedSettings.gamescope.tearing_enabled = bTearing;
			QueueSave();
		}

		ImGui::Separator();

		// force-grab-cursor (issue #25): promoted from a --force-grab-cursor
		// startup-only CLI flag to a live toggle here. g_bForceRelativeMouse
		// is read every frame on this same thread -- ShouldDrawCursor()
		// (steamcompmgr.cpp:2531) and the cursor-nesting-hint check (:9413)
		// -- so writing it directly, the same way SetFilter()/SetScaler() do
		// above, has an immediate live effect; this is genuinely NOT
		// startup-only despite the CLI flag's name suggesting otherwise, so
		// no "needs relaunch" note is needed here.
		bool bForceGrabCursor = g_bForceRelativeMouse;
		if ( widgets::Toggle( "Force Grab Cursor", &bForceGrabCursor ) )
		{
			g_bForceRelativeMouse = bForceGrabCursor;
			s_CachedSettings.gamescope.force_grab_cursor = bForceGrabCursor;
			QueueSave();
		}
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled( "Always use relative mouse mode instead of flipping on cursor visibility. Applies immediately." );
		ImGui::PopFont();
	}

	static void DrawFrameLimiterTab()
	{
		int nFps = s_CachedSettings.gamescope.fps_limit;
		if ( widgets::SliderInt( "FPS Limit", &nFps, 0, 240, "%d fps" ) )
			SetFpsLimit( nFps );

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled( "0 = unlimited. Applies immediately." );
		ImGui::PopFont();
	}

	// HDR tab's read-only readout of the focused app's own reported HDR
	// metadata (mastering display primaries/white point/luminance, MaxCLL/
	// MaxFALL) -- g_ColorMgmt.current.appHDRMetadata, the same blob
	// steamcompmgr.cpp itself decodes the identical way (as
	// hdr_output_metadata::hdmi_metadata_type1, a CTA-861.G struct) to write
	// GAMESCOPE_COLOR_APP_HDR_METADATA_FEEDBACK (steamcompmgr.cpp:9335-9352).
	// Deliberately never editable here -- this is what the game *reported*,
	// not a setting; a slider on top of it would be a bug (issue #25's own
	// warning). widgets::ReadoutStrip is the design guide's dedicated
	// never-editable-looking treatment for exactly this ("never show a value
	// the overlay didn't verify" as a look, not just a behavior).
	static void DrawHdrAppMetadataReadout()
	{
		ImGui::TextUnformatted( "Focused app's HDR metadata (read-only)" );

		if ( !g_ColorMgmt.current.appHDRMetadata )
		{
			widgets::ReadoutStrip( "no HDR metadata reported -- no HDR-capable app is currently focused", /*bLeadingDot=*/ false );
			return;
		}

		const hdr_metadata_infoframe &info = g_ColorMgmt.current.appHDRMetadata->View<hdr_output_metadata>().hdmi_metadata_type1;

		// CTA-861.G coordinates are unsigned 16-bit in units of 0.00002.
		auto Chroma = []( uint16_t uRaw ) { return uRaw * 0.00002f; };

		char szLine[192];
		std::snprintf( szLine, sizeof( szLine ), "MaxCLL %u nits  MaxFALL %u nits  mastering luminance %u / %.4f nits (max/min)",
			(unsigned)info.max_cll, (unsigned)info.max_fall,
			(unsigned)info.max_display_mastering_luminance,
			info.min_display_mastering_luminance * 0.0001f );
		widgets::ReadoutStrip( szLine, /*bLeadingDot=*/ true );

		std::snprintf( szLine, sizeof( szLine ), "primaries R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) white(%.4f,%.4f)",
			Chroma( info.display_primaries[0].x ), Chroma( info.display_primaries[0].y ),
			Chroma( info.display_primaries[1].x ), Chroma( info.display_primaries[1].y ),
			Chroma( info.display_primaries[2].x ), Chroma( info.display_primaries[2].y ),
			Chroma( info.white_point.x ), Chroma( info.white_point.y ) );
		widgets::ReadoutStrip( szLine, /*bLeadingDot=*/ false );
	}

	// hdrTonemapOperator (ETonemapOperator, color_helpers.h:275-280) has no
	// live setter today -- grep for a set_hdr_tonemap_operator()-shaped
	// function next to the five set_*() this tab already calls turns up
	// none, and there's no X11 property/PropertyNotify handler wired for it
	// either (unlike sdrGamutWideness/flSDROnHDRBrightness/the two input
	// gains, all of which are exercised by an existing GAMESCOPE_* property
	// today). Exposing it needs new plumbing symmetrical to those five, not
	// just a UI control calling something that already exists -- explicitly
	// deferred (issue #25's own acceptance criteria calls this out as the
	// two allowed outcomes: new plumbing, or a note explaining why not) --
	// not silently omitted.
	static void DrawTonemapOperatorNote()
	{
		ImGui::TextUnformatted( "Tonemap Operator" );
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled(
			"Deferred: hdrTonemapOperator has no live setter or X11 property wired yet, unlike the "
			"controls above. Exposing it needs new plumbing (a set_hdr_tonemap_operator() + property, "
			"symmetrical to what already exists for the sliders above), not just a widget." );
		ImGui::PopFont();
	}

	static void DrawHdrTab()
	{
		// Main HDR toggle (issue #66): moved here from the Display tab so
		// the switch lives with the settings it gates instead of being
		// split across tabs. Taxonomically HDR-enable is a Display-ish
		// setting, but once a dedicated HDR tab exists, separating the
		// toggle from what it governs costs more (a control the user has
		// to remember lives elsewhere) than the tidier categorisation
		// gains -- deliberate call, see superdoc/planning/ISSUES.md and
		// issue #66 itself. cv_hdr_enabled/s_CachedSettings.gamescope.
		// hdr_enabled are unchanged -- only where this is drawn moved, not
		// what it drives (issue #25's frame-limiter regression is the
		// cautionary precedent here).
		bool bHdr = cv_hdr_enabled.Get();
		if ( widgets::Toggle( "HDR", &bHdr ) )
		{
			cv_hdr_enabled = bHdr;
			s_CachedSettings.gamescope.hdr_enabled = bHdr;
			QueueSave();
		}
		ImGui::Separator();

		const bool bHdrEnabled = cv_hdr_enabled.Get();
		if ( !bHdrEnabled )
		{
			// Same "disabled/inactive" treatment SPEC.md's Feature 2 HDR
			// gate uses for the Shaders panel (PanelShaders.cpp's own
			// bSdr-gated BeginDisabled() block) -- reused here rather than
			// inventing a new one: these settings are meaningless while HDR
			// itself is off.
			ImGui::TextColored( ImVec4( 0.95f, 0.65f, 0.25f, 1.0f ),
				"HDR is off -- these controls do nothing until it's on." );
			ImGui::Separator();
		}

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled( "Every control below applies immediately -- no relaunch needed." );
		ImGui::PopFont();
		ImGui::Spacing();

		if ( !bHdrEnabled )
			ImGui::BeginDisabled();

		// sdrGamutWideness defaults to -1 (rendervulkan.hpp) meaning
		// "unset/display-native"; clamp only the *displayed* value into the
		// slider's 0..1 range (main.cpp's own --sdr-gamut-wideness help
		// text: "0 - 1") rather than silently writing 0 back over -1 before
		// the user has touched the control.
		float flGamutWideness = std::clamp( g_ColorMgmt.pending.sdrGamutWideness, 0.0f, 1.0f );
		if ( widgets::SliderFloat( "SDR Gamut Wideness", &flGamutWideness, 0.0f, 1.0f, "%.2f" ) )
		{
			set_color_sdr_gamut_wideness( flGamutWideness );
			s_CachedSettings.gamescope.sdr_gamut_wideness = flGamutWideness;
			QueueSave();
		}
		ImGui::Spacing();

		float flSdrOnHdrNits = g_ColorMgmt.pending.flSDROnHDRBrightness;
		if ( widgets::SliderFloat( "SDR-on-HDR Brightness", &flSdrOnHdrNits, 50.0f, 1000.0f, "%.0f nits" ) )
		{
			set_sdr_on_hdr_brightness( flSdrOnHdrNits );
			s_CachedSettings.gamescope.sdr_on_hdr_brightness_nits = flSdrOnHdrNits;
			QueueSave();
		}
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled( "How bright SDR content looks composited alongside HDR." );
		ImGui::PopFont();
		ImGui::Spacing();

		float flHdrInputGain = g_ColorMgmt.pending.flHDRInputGain;
		if ( widgets::SliderFloat( "HDR Input Gain", &flHdrInputGain, 0.0f, 4.0f, "%.2fx" ) )
		{
			set_hdr_input_gain( flHdrInputGain );
			s_CachedSettings.gamescope.hdr_input_gain = flHdrInputGain;
			QueueSave();
		}

		float flSdrInputGain = g_ColorMgmt.pending.flSDRInputGain;
		if ( widgets::SliderFloat( "SDR Input Gain", &flSdrInputGain, 0.0f, 4.0f, "%.2fx" ) )
		{
			set_sdr_input_gain( flSdrInputGain );
			s_CachedSettings.gamescope.sdr_input_gain = flSdrInputGain;
			QueueSave();
		}

		if ( !bHdrEnabled )
			ImGui::EndDisabled();

		ImGui::Separator();
		DrawTonemapOperatorNote();

		ImGui::Separator();
		DrawHdrAppMetadataReadout();
	}

	void PanelDisplay_Draw()
	{
		EnsureConfigLoaded();

		// M8 part 3 (issue #15): hosted through chrome::BeginPanelWindow(),
		// which is what makes this panel show/hide from the dock instead of
		// always being on screen -- see Overlay/Chrome.h. Renamed
		// "DISPLAY" -> "GAMESCOPE" and grew a bit taller for the new tabs
		// (issue #25); position unchanged since M3.
		if ( !chrome::BeginPanelWindow( "GAMESCOPE", chrome::PanelId::Display,
			ImVec2( 64.0f, 340.0f ), ImVec2( 440.0f, 360.0f ) ) )
			return;

		// Tabs (issue #25), same ImGui::BeginTabBar()/BeginTabItem() pattern
		// PanelConfig.cpp already uses for Per-Game/General -- Upscaling
		// keeps the pre-existing filter/scaler/sharpness controls unchanged;
		// Display keeps VRR/tearing and gains Force Grab Cursor; Frame
		// Limiter and HDR are new. The HDR-enable toggle itself moved from
		// Display onto the HDR tab in issue #66 -- it now owns its own
		// on/off switch alongside the settings it gates.
		if ( ImGui::BeginTabBar( "GamescopeTabs" ) )
		{
			if ( ImGui::BeginTabItem( "Upscaling" ) )
			{
				DrawUpscalingTab();
				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "Display" ) )
			{
				DrawDisplayTab();
				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "Frame Limiter" ) )
			{
				DrawFrameLimiterTab();
				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "HDR" ) )
			{
				DrawHdrTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		chrome::EndPanelWindow();
	}
}
