// M3 Display panel -- see PanelDisplay.h and superdoc/planning/SPEC.md's
// Feature 4 ("Live gamescope options: filter, scaler, sharpness").
//
// Thread safety: every value this panel edits is a plain (non-atomic) global
// or a ConVar<T> whose own m_Value is a plain field (see convar.h) -- neither
// has any lock. Both are safe to write here with no new synchronization
// because PanelDisplay_Draw() is called from SettingsOverlay_AddLayer(),
// which paint_all() calls in-line on the steamcompmgr thread (see
// SettingsOverlay.cpp) -- the exact same thread that reads
// g_upscaleFilter/g_upscaleScaler/g_upscaleFilterSharpness every frame
// (steamcompmgr.cpp's per-frame body) and that cv_adaptive_sync/
// cv_hdr_enabled/cv_tearing_enabled are read from in vulkan_composite()/
// paint_all(). Writer and reader being the same thread is what makes this
// safe (superdoc/planning/runtime-knobs-and-fps.md Part A3) -- if this panel
// is ever called from a different thread, that guarantee breaks and these
// writes would need to move behind the existing X11-property path instead.
//
// Sharpness direction (DECISIONS.md #11, SPEC.md Feature 4): FSR and NIS
// remap the same raw 0..20 g_upscaleFilterSharpness value in opposite visual
// directions. This was verified empirically here, not just by reading the
// shader math (the RCAS constant-setup comment reads ambiguously in
// isolation) -- see RawSharpnessFromUiPercent()'s comment for the actual
// screenshot-verified directions.
#include "PanelDisplay.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "convar.h"
#include "Config/ConfigManager.h"
#include "Config/AppId.h"
#include "Fonts.h"
#include "Chrome.h"

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
	// globals/ConVars this panel's own Set*() handlers write, so a
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
		for ( size_t i = 0; i < std::size( kFilters ); i++ )
		{
			if ( i > 0 )
				ImGui::SameLine();
			const bool bSelected = ( g_wantedUpscaleFilter == kFilters[i].eValue );
			if ( ImGui::RadioButton( kFilters[i].pszLabel, bSelected ) )
				SetFilter( kFilters[i].eValue );
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
			{ GamescopeUpscaleScaler::FIT,     "Fit" },
			{ GamescopeUpscaleScaler::FILL,    "Fill" },
			{ GamescopeUpscaleScaler::STRETCH, "Stretch" },
		};

		ImGui::TextUnformatted( "Scaler" );
		for ( size_t i = 0; i < std::size( kScalers ); i++ )
		{
			if ( i > 0 )
				ImGui::SameLine();
			const bool bSelected = ( g_wantedUpscaleScaler == kScalers[i].eValue );
			if ( ImGui::RadioButton( kScalers[i].pszLabel, bSelected ) )
				SetScaler( kScalers[i].eValue );
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

		if ( ImGui::SliderInt( "Sharpness", &nUiPercent, 0, 100, "%d%% sharper" ) )
			SetSharpnessUiPercent( eWanted, nUiPercent );

		if ( !bApplies )
		{
			ImGui::EndDisabled();
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			ImGui::TextDisabled( "No effect on Linear/Nearest/Pixel -- only FSR and NIS sharpen." );
			ImGui::PopFont();
		}
	}

	static void DrawLiveToggles()
	{
		bool bVrr = cv_adaptive_sync.Get();
		if ( ImGui::Checkbox( "VRR / Adaptive Sync", &bVrr ) )
		{
			cv_adaptive_sync = bVrr;
			s_CachedSettings.gamescope.vrr_enabled = bVrr;
			QueueSave();
		}

		bool bHdr = cv_hdr_enabled.Get();
		if ( ImGui::Checkbox( "HDR", &bHdr ) )
		{
			cv_hdr_enabled = bHdr;
			s_CachedSettings.gamescope.hdr_enabled = bHdr;
			QueueSave();
		}

		bool bTearing = cv_tearing_enabled.Get();
		if ( ImGui::Checkbox( "Allow Tearing", &bTearing ) )
		{
			cv_tearing_enabled = bTearing;
			s_CachedSettings.gamescope.tearing_enabled = bTearing;
			QueueSave();
		}
	}

	void PanelDisplay_Draw()
	{
		EnsureConfigLoaded();

		// M8 part 3 (issue #15): hosted through chrome::BeginPanelWindow(),
		// which is what makes this panel show/hide from the dock instead of
		// always being on screen -- see Overlay/Chrome.h. Position/size below
		// are the same values this panel has used since M3, just no longer
		// applied by hand.
		if ( !chrome::BeginPanelWindow( "DISPLAY", chrome::PanelId::Display,
			ImVec2( 64.0f, 340.0f ), ImVec2( 440.0f, 300.0f ) ) )
			return;

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

		DrawFilterRow();
		ImGui::Spacing();
		DrawScalerRow();
		ImGui::Spacing();
		DrawSharpnessSlider();
		ImGui::Separator();
		DrawLiveToggles();

		chrome::EndPanelWindow();
	}
}
