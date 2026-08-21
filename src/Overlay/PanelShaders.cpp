// M6 Shaders panel -- see PanelShaders.h and superdoc/planning/SPEC.md's
// Feature 2 ("ReShade effects -- Vibrancy, Sharpness, and (later) Adaptive
// Brightness").
//
// Two sharpness controls exist in this overlay on purpose (DECISIONS.md
// #12): PanelDisplay.cpp's "Sharpness" control is gamescope's own built-in
// post-upscale RCAS/NIS sharpen (only live when Filter is FSR or NIS); this
// panel's "Pre-Sharpen" is a separate ReShade pass that runs pre-upscale,
// at source resolution, and works regardless of which filter is active
// (including Linear/Nearest/Pixel, where the built-in sharpen is a no-op).
// The two can be combined and can visibly double up if both are pushed
// hard -- that's an accepted trade-off of shipping both, not a bug.
//
// Live parameter updates (superdoc/planning/reshade-shaders.md Q1): every
// value this panel edits must be written via
// reshade_effect_manager_set_uniform_variable(), which only reaches the
// shader for a uniform gamescope-ritz.fx declared with its own `source`
// annotation (RuntimeUniform, src/reshade_effect_manager.cpp:181-597).
// Writing a plain global here instead would compile and look correct in
// the UI while silently doing nothing on screen -- the exact failure mode
// that mechanism warns about. See SetRuntimeUniformFloat/Bool below; every
// control in this file goes through one of the two.
//
// Thread safety: this panel is called from SettingsOverlay_AddLayer() on
// the steamcompmgr thread, same as PanelDisplay.cpp (see that file's own
// comment for the full argument). reshade_effect_manager_set_uniform_variable()
// takes its own internal lock (g_runtimeUniformsMutex,
// src/reshade_effect_manager.cpp) so it's actually safe to call from any
// thread -- same-thread-as-the-render-loop is what this file relies on for
// g_eLastBaseLayerColorspace (a relaxed atomic, but still logically a
// single-writer/single-reader-per-frame value) and for
// EnsureEffectLoaded()'s plain (non-atomic) s_bEffectLoaded latch.
#include "PanelShaders.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "reshade_effect_manager.hpp"
#include "rendervulkan.hpp"
#include "Config/ConfigManager.h"
#include "Fonts.h"
#include "Chrome.h"

#include "imgui.h"

// Defined in rendervulkan.cpp, updated every frame vulkan_composite() sees a
// base layer -- see that file's comment on g_eLastBaseLayerColorspace for
// why it's tracked there (that's where the base layer's real colourspace is
// already being read) and exposed here as a plain extern, the same pattern
// PanelDisplay.cpp uses for cv_adaptive_sync/cv_hdr_enabled.
extern std::atomic<GamescopeAppTextureColorspace> g_eLastBaseLayerColorspace;

namespace gamescope
{
	// Install path for the combined effect: default_extras_install.sh
	// installs this repo's reshade/ tree to $prefix/share/gamescope/reshade,
	// matching exactly what ReshadeEffectPipeline::init()
	// (src/reshade_effect_manager.cpp) looks under (it prefixes this path
	// with ".../share/gamescope/reshade/Shaders/", local-usr then
	// global-usr). See reshade/Shaders/gamescope-ritz.fx for the effect
	// itself.
	static constexpr const char *k_pszEffectPath = "gamescope-ritz.fx";

	static bool s_bConfigLoaded = false;
	static uint64_t s_ulLoadedGeneration = 0;
	static config::Settings s_CachedSettings;

	// Set once the combined effect has actually been handed to
	// ReshadeEffectManager. See EnsureEffectLoaded()'s comment for why this
	// deliberately never goes back to false for the rest of the process's
	// life once it's true.
	static bool s_bEffectLoaded = false;

	// M7: routes through config::IsSessionOverrideActive() instead of always
	// writing global.json -- PanelConfig.cpp is the only thing that ever
	// flips that flag. Superseded M6's original "always global.json"
	// simplification.
	static void QueueSave()
	{
		config::EnqueueRoutedWrite( s_CachedSettings );
	}

	// Every parameter this panel exposes MUST go through one of these two
	// helpers, never a plain global write -- see this file's header comment
	// and gamescope-ritz.fx's own header comment for why. Ownership note:
	// reshade_effect_manager_set_uniform_variable() takes ownership of the
	// buffer (it `delete[]`s whatever was stored under the same key on the
	// *next* call), so always hand it a freshly `new`'d buffer, never a
	// pointer to a local/static.
	static void SetRuntimeUniformFloat( const char *pszKey, float flValue )
	{
		uint8_t *pBuffer = new uint8_t[ sizeof( float ) ];
		std::memcpy( pBuffer, &flValue, sizeof( float ) );
		reshade_effect_manager_set_uniform_variable( pszKey, pBuffer );
	}

	static void SetRuntimeUniformBool( const char *pszKey, bool bValue )
	{
		// RuntimeUniform::update's boolean path reads exactly
		// components() * sizeof(uint8_t) bytes -- one byte for a scalar
		// bool uniform (src/reshade_effect_manager.cpp:559-565).
		uint8_t *pBuffer = new uint8_t[ 1 ];
		pBuffer[ 0 ] = bValue ? 1 : 0;
		reshade_effect_manager_set_uniform_variable( pszKey, pBuffer );
	}

	// Loads the combined .fx exactly once per process -- the first time any
	// effect in it is turned on -- and never unloads it again for the rest
	// of the session, even once every effect is toggled back off. This is
	// deliberate (DECISIONS.md #13): the whole point of the combined-.fx
	// design is that toggling an effect costs a uniform write, not a
	// recompile. Unloading whenever both effects are off would mean the
	// *next* enable pays the synchronous parse+SPIR-V+pipeline-build cost
	// on the render thread again, right as the user drags a slider --
	// exactly the hitch this design exists to avoid. Paying a small
	// constant per-frame general-queue cost for a loaded-but-fully-gated
	// technique (both passes short-circuit to a plain copy when their
	// uniform is off, see gamescope-ritz.fx) for the rest of the session is
	// the accepted trade-off.
	//
	// ponytail: no reference counting or idle-timeout unload -- "load once,
	// keep for the session" is the simplest thing that satisfies the
	// "no recompile stutter on toggle" acceptance criterion (SPEC.md
	// Build order M6). Upgrade path: an idle-timeout unload via
	// reshade_effect_manager_disable_effect() if the always-loaded general-
	// queue cost ever turns out to matter with both effects off in
	// practice.
	static void EnsureEffectLoaded()
	{
		if ( s_bEffectLoaded )
			return;

		reshade_effect_manager_set_effect( k_pszEffectPath, nullptr );
		reshade_effect_manager_enable_effect();
		s_bEffectLoaded = true;
	}

	static void PushAllUniformsToShader()
	{
		auto &r = s_CachedSettings.reshade;
		SetRuntimeUniformBool( "vibrancy_enabled", r.vibrancy.enabled );
		SetRuntimeUniformFloat( "vibrancy_strength", r.vibrancy.strength );
		SetRuntimeUniformBool( "vibrancy_protect_skin_tones", r.vibrancy.protect_skin_tones );
		SetRuntimeUniformBool( "pre_sharpen_enabled", r.pre_sharpen.enabled );
		SetRuntimeUniformFloat( "pre_sharpen_strength", *r.pre_sharpen.strength );
	}

	static void EnsureConfigLoaded()
	{
		const uint64_t ulGeneration = config::ConfigGeneration();
		if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
			return;

		// M7: resolves against the current session's effective config
		// (per-game snapshot when active, global.json otherwise) instead of
		// always global.json -- see config::ResolveEffective(). Re-run on
		// every PanelConfig-triggered generation bump too (profile applied,
		// override toggled, another game's config copied in), not just the
		// first draw.
		s_CachedSettings = config::ResolveEffective( config::SessionAppId() );
		s_ulLoadedGeneration = ulGeneration;

		// Defensive: ConfigSchema.h's default member initializer already
		// gives this a value (0.5f) and ConfigManager.cpp's loader
		// preserves that default when the on-disk value is absent/null,
		// but guard here too rather than dereferencing an empty optional
		// below if either of those ever drifts.
		if ( !s_CachedSettings.reshade.pre_sharpen.strength.has_value() )
			s_CachedSettings.reshade.pre_sharpen.strength = 0.5f;

		s_bConfigLoaded = true;

		// A (re)load with effects enabled -- whether from a previous
		// session's saved config on the very first draw, or a
		// PanelConfig-triggered profile/override change mid-session -- must
		// apply immediately, not wait for the user to touch a widget: load
		// the effect and push its full state now if anything is on.
		if ( s_CachedSettings.reshade.vibrancy.enabled || s_CachedSettings.reshade.pre_sharpen.enabled )
			EnsureEffectLoaded();
		PushAllUniformsToShader();
	}

	static bool IsBaseLayerSdr()
	{
		const GamescopeAppTextureColorspace eColorspace = g_eLastBaseLayerColorspace.load( std::memory_order_relaxed );
		return eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR
			|| eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
	}

	static void DrawVibrancyGroup()
	{
		auto &v = s_CachedSettings.reshade.vibrancy;

		if ( ImGui::Checkbox( "Vibrancy##enabled", &v.enabled ) )
		{
			if ( v.enabled )
				EnsureEffectLoaded();
			SetRuntimeUniformBool( "vibrancy_enabled", v.enabled );
			QueueSave();
		}

		if ( !v.enabled )
			ImGui::BeginDisabled();

		if ( ImGui::SliderFloat( "Strength##vibrancy", &v.strength, -1.0f, 1.0f, "%.2f" ) )
		{
			SetRuntimeUniformFloat( "vibrancy_strength", v.strength );
			QueueSave();
		}

		if ( ImGui::Checkbox( "Protect skin tones", &v.protect_skin_tones ) )
		{
			SetRuntimeUniformBool( "vibrancy_protect_skin_tones", v.protect_skin_tones );
			QueueSave();
		}

		if ( !v.enabled )
			ImGui::EndDisabled();
	}

	static void DrawPreSharpenGroup()
	{
		auto &s = s_CachedSettings.reshade.pre_sharpen;
		if ( !s.strength.has_value() )
			s.strength = 0.5f;

		if ( ImGui::Checkbox( "Pre-Sharpen##enabled", &s.enabled ) )
		{
			if ( s.enabled )
				EnsureEffectLoaded();
			SetRuntimeUniformBool( "pre_sharpen_enabled", s.enabled );
			QueueSave();
		}
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		ImGui::TextDisabled( "Pre-upscale -- works with any Filter, unlike Sharpness (Display panel)." );
		ImGui::PopFont();

		if ( !s.enabled )
			ImGui::BeginDisabled();

		if ( ImGui::SliderFloat( "Strength##presharpen", &( *s.strength ), 0.0f, 2.0f, "%.2f" ) )
		{
			SetRuntimeUniformFloat( "pre_sharpen_strength", *s.strength );
			QueueSave();
		}

		if ( !s.enabled )
			ImGui::EndDisabled();
	}

	void PanelShaders_Draw()
	{
		EnsureConfigLoaded();

		// M8 part 3 (issue #15): hosted through chrome::BeginPanelWindow(),
		// see Overlay/Chrome.h -- position/size unchanged from M6.
		if ( !chrome::BeginPanelWindow( "SHADERS", chrome::PanelId::Shaders,
			ImVec2( 520.0f, 64.0f ), ImVec2( 430.0f, 300.0f ) ) )
			return;

		// SDR-only gate (DECISIONS.md #15): a deliberate v1 limitation, not
		// an oversight -- naive vibrancy/sharpen math assumes clamped 0..1
		// SDR RGB and produces silently wrong results (over-saturating/
		// clipping) on scRGB/HDR10_PQ content. Say so in the UI rather than
		// just disabling controls with no explanation.
		const bool bSdr = IsBaseLayerSdr();
		if ( !bSdr )
		{
			ImGui::TextColored( ImVec4( 0.95f, 0.65f, 0.25f, 1.0f ),
				"Effects are SDR-only for now -- disable HDR to use them." );
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			ImGui::TextDisabled( "Deliberate v1 limitation, not a bug (DECISIONS.md #15)." );
			ImGui::PopFont();
			ImGui::Separator();
			ImGui::BeginDisabled();
		}

		DrawVibrancyGroup();
		ImGui::Separator();
		DrawPreSharpenGroup();

		if ( !bSdr )
			ImGui::EndDisabled();

		chrome::EndPanelWindow();
	}
}
