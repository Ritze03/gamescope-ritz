// M6 Shaders panel -- see PanelShaders.h and superdoc/planning/SPEC.md's
// Feature 2 ("ReShade effects -- Vibrancy, Sharpness, and (later) Adaptive
// Brightness").
//
// Two sharpness controls exist in this overlay on purpose (DECISIONS.md
// #12): PanelDisplay.cpp's "Sharpness" control is gamescope's own built-in
// post-upscale RCAS/NIS sharpen (only live when Filter is FSR or NIS); this
// panel's "Pre-Sharpen" is a separate pass that runs pre-upscale, at source
// resolution, and works regardless of which filter is active (including
// Linear/Nearest/Pixel, where the built-in sharpen is a no-op). The two can
// be combined and can visibly double up if both are pushed hard -- that's an
// accepted trade-off of shipping both, not a bug.
//
// HOW A CONTROL REACHES THE SCREEN (DECISIONS.md #27, 2026-09-05). The four
// effects are a native compute pre-pass compiled into the binary at build
// time (src/shaders/cs_effects_layer0.comp, dispatched from
// vulkan_composite() in rendervulkan.cpp). Every setter here writes the plain
// host struct g_nativeEffects (rendervulkan.hpp); vulkan_composite() reads
// it each frame and uploads it as the shader's uniform block. There is no
// runtime compile, no file on disk, and no uniform-by-name lookup that a
// stale shader could silently drop -- the failure mode the previous ReShade
// .fx design had (a stale copy under the legacy ~/.local/share/gamescope/
// reshade tree won the search and no-op'd every control it didn't declare)
// is structurally gone: the shader and this panel ship in the same binary,
// and a GLSL error fails the build.
//
// Thread safety: this panel is called from SettingsOverlay_AddLayer() on
// the steamcompmgr thread, same as PanelDisplay.cpp (see that file's own
// comment for the full argument) -- the same thread vulkan_composite() and
// the backends read g_nativeEffects on, so it is a plain struct with no
// locking, same discipline as g_upscaleFilterSharpness (main.cpp).
// g_eLastBaseLayerColorspace is a relaxed atomic for the same
// single-writer/single-reader-per-frame reason it always was.
#include "PanelShaders.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "rendervulkan.hpp"
#include "Config/ConfigManager.h"
#include "Fonts.h"

#include "imgui.h"

// Defined in rendervulkan.cpp, updated every frame vulkan_composite() sees a
// base layer -- see that file's comment on g_eLastBaseLayerColorspace for
// why it's tracked there (that's where the base layer's real colourspace is
// already being read) and exposed here as a plain extern, the same pattern
// PanelDisplay.cpp uses for cv_adaptive_sync/cv_hdr_enabled.
extern std::atomic<GamescopeAppTextureColorspace> g_eLastBaseLayerColorspace;

namespace gamescope
{
	static bool s_bConfigLoaded = false;
	static uint64_t s_ulLoadedGeneration = 0;
	static config::Settings s_CachedSettings;

	// M7: routes through config::IsSessionOverrideActive() instead of always
	// writing global.json -- PanelConfig.cpp is the only thing that ever
	// flips that flag. Superseded M6's original "always global.json"
	// simplification.
	static void QueueSave()
	{
		config::EnqueueRoutedWrite( s_CachedSettings );
	}

	// The one place config state becomes render state. Called after every
	// edit, after every (re)load, and once at startup (see
	// PanelShaders_ApplyStartupConfig) so g_nativeEffects can never lag the
	// settings -- a full copy of a dozen scalars is cheaper than keeping
	// per-field setters honest.
	static void PushToRenderer( const config::Settings &settings )
	{
		const auto &r = settings.reshade;
		NativeEffectsState_t &e = g_nativeEffects;

		e.bShadowLift  = r.shadow_lift.enabled;
		e.flShadowLift = r.shadow_lift.strength;

		e.bVibrancy            = r.vibrancy.enabled;
		e.flVibrancy           = r.vibrancy.strength;
		e.bVibrancyProtectSkin = r.vibrancy.protect_skin_tones;

		e.bPreSharpen  = r.pre_sharpen.enabled;
		e.flPreSharpen = r.pre_sharpen.strength.value_or( 0.5f );

		// Adaptive Brightness: plumbed through to the shader's reserved
		// fields so Agent B only has to consume them. Until B lands the
		// history/measure half, the pass multiplies by a constant 1.0 and
		// this effect has no visible result -- the switch still saves.
		e.bAdaptiveBrightness = r.adaptive_brightness.enabled;
		e.flAbTarget    = r.adaptive_brightness.target_luminance;
		e.flAbUpSpeed   = r.adaptive_brightness.adapt_up_speed;
		e.flAbDownSpeed = r.adaptive_brightness.adapt_down_speed;
		e.flAbMinGain   = r.adaptive_brightness.min_gain;
		e.flAbMaxGain   = r.adaptive_brightness.max_gain;
		e.flAbStrength  = r.adaptive_brightness.strength;
	}

	static void PushAllToRenderer()
	{
		PushToRenderer( s_CachedSettings );
	}

	void PanelShaders_ApplyStartupConfig( const config::Settings &config )
	{
		PushToRenderer( config );
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

		// A (re)load -- a previous session's saved config on the very first
		// draw, or a PanelConfig-triggered profile/override change
		// mid-session -- must apply immediately, not wait for the user to
		// touch a widget.
		PushAllToRenderer();
	}

	static bool IsBaseLayerSdr()
	{
		const GamescopeAppTextureColorspace eColorspace = g_eLastBaseLayerColorspace.load( std::memory_order_relaxed );
		return eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR
			|| eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
	}

	// =====================================================================
	//  E2 (P3) -- the effects, declared instead of drawn
	// =====================================================================
	// P5 deleted the legacy group drawers this replaced, along with the
	// floating window that hosted them.
	//
	// SHAPE: one switch row per effect, each owning its own parameters. This
	// is the taxonomy's intended shape for exactly this data -- an effect is
	// one decision ("is this on") with tuning behind it, so the sheet stays
	// one row per effect deep no matter how many knobs an effect grows.
	// (index.html declared three at E2's original writing; Shadow Control
	// (request #3, 2026-09-04) is the fourth, added the same shape.)
	//
	// THE SIX BUDGET, AND WHY ADAPTIVE BRIGHTNESS SITS EXACTLY ON IT.
	// Vibrancy has 2 params, Pre-Sharpen 1, Adaptive Brightness 6, Shadow
	// Control 1 -- the maximum a row may own before Registry.cpp aborts
	// registration and tells the author to promote it to a category.
	// Adaptive Brightness fits, but with zero headroom, and that is worth
	// saying out loud: the NEXT parameter added
	// to this effect does not "just" overflow a limit, it is the signal
	// that Adaptive Brightness has become a category rather than a setting.
	// Nothing here routes around the budget, and nothing should.
	//
	// EVERY WRITE GOES THROUGH SetEffectEnabled()/SetEffectFloat() below --
	// edit the cached config field, push the whole struct to the renderer,
	// queue the save. Writing a config field alone would compile and look
	// right in the UI while doing nothing on screen until the next config
	// reload; the push is what makes the frame after the click different.
	static config::Settings &Cfg()
	{
		EnsureConfigLoaded();
		return s_CachedSettings;
	}

	// One reason string for all effects, so the SDR gate cannot drift
	// between them. It replaces the legacy orange banner: SPEC §3.13 makes a
	// reason mandatory on every disabled control, which puts the explanation
	// on the control that is actually greyed instead of at the top of a panel.
	static bool EffectsUsable() { return IsBaseLayerSdr(); }
	static constexpr const char *kSdrOnly =
		"effects are SDR-only for now -- the focused app is presenting HDR or scRGB content, "
		"whose values these passes would clip. A deliberate v1 limitation, not a bug";

	static void SetEffectEnabled( bool *pbField, bool bOn )
	{
		*pbField = bOn;
		PushAllToRenderer();
		QueueSave();
	}

	static void SetEffectFloat( float *pflField, float flValue )
	{
		*pflField = flValue;
		PushAllToRenderer();
		QueueSave();
	}

	void PanelShaders_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "image.shaders", "Shaders", ui::Section::Display );
		a.Keywords( "shader effect vibrancy saturation sharpen adaptive brightness exposure shadow control lift darkness" );
		a.Summary( []{
			const auto &r = Cfg().reshade;
			const int n = ( r.vibrancy.enabled ? 1 : 0 )
			            + ( r.pre_sharpen.enabled ? 1 : 0 )
			            + ( r.adaptive_brightness.enabled ? 1 : 0 )
			            + ( r.shadow_lift.enabled ? 1 : 0 );
			return std::to_string( n ) + " of 4 effects on";
		} );

		// GroupCount, not Group: SPEC §2.5 lets a band carry a `n / m` count
		// for a switch set, and the shell computes it from the band's own
		// switch rows. Independent binaries are separate rows -- they are
		// NOT a Bank, because they can be turned on for unrelated reasons
		// (SPEC §3.12's governing rule).
		a.GroupCount( "Effects" );

		a.Switch( "image.shaders.vibrancy", "Vibrancy",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.vibrancy.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.vibrancy.enabled, b ); } ) )
			.Help( "Makes dull colours more vivid, while leaving already-vivid colours alone." )
			.Default( false )
			.Keywords( "vibrancy saturation colour vividness" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "strength", "Saturation",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.vibrancy.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.vibrancy.strength, f ); } ) )
				.Help( "Colour intensity. 1x is unchanged, 0x is black and white, 3x is maximum boost." )
				.Range( 0.0f, 3.0f )
				.Step( 0.05f )   // 61 positions; 1.00, the default, is the neutral notch
				.Unit( "x" )
				.Default( 1.0f )
			.Param( "protect_skin", "Protect skin tones",
				ui::AnyBind::Of<bool>(
					[]{ return Cfg().reshade.vibrancy.protect_skin_tones; },
					[]( bool b ) { SetEffectEnabled( &Cfg().reshade.vibrancy.protect_skin_tones, b ); } ) )
				.Help( "Keeps the saturation boost off skin tones, so faces don't turn orange." )
				.Default( true );

		a.Switch( "image.shaders.presharpen", "Pre-Sharpen",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.pre_sharpen.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.pre_sharpen.enabled, b ); } ) )
			.Help( "Sharpens the picture before it's resized, so it works with any Filter -- unlike "
			       "the Upscaling area's Sharpness, which only works with FSR or NIS." )
			.Default( false )
			.Keywords( "presharpen sharpen pre upscale clarity" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "strength", "Strength",
				ui::AnyBind::Of<float>(
					// ConfigSchema.h makes this an optional<float> with a 0.5
					// default; EnsureConfigLoaded() already guarantees it is
					// engaged, and this guards anyway rather than dereferencing
					// an empty optional if that ever drifts.
					[]{
						auto &s = Cfg().reshade.pre_sharpen;
						if ( !s.strength.has_value() ) s.strength = 0.5f;
						return *s.strength;
					},
					[]( float f ) {
						auto &s = Cfg().reshade.pre_sharpen;
						s.strength = f;
						PushAllToRenderer();
						QueueSave();
					} ) )
				.Help( "How strong the sharpening is." )
				.Range( 0.0f, 2.0f )
				.Step( 0.05f )   // 41 positions
				.Default( 0.5f );

		// SIX PARAMS -- the budget exactly. See this section's header.
		//
		// NOTE (2026-09-05): until Agent B lands the native Adaptive
		// Brightness pass, this switch and its parameters save and are
		// plumbed through to the shader's reserved fields, but have no
		// visible result on screen. The Help text says so.
		a.Switch( "image.shaders.adaptive_brightness", "Adaptive Brightness",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.adaptive_brightness.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.adaptive_brightness.enabled, b ); } ) )
			.Help( "Experimental. Automatically brightens dark scenes and dims bright ones as you "
			       "play, like your eyes adjusting. Currently being rebuilt -- has no visible "
			       "effect in this build." )
			.Default( false )
			.Keywords( "adaptive brightness eye adaptation exposure auto experimental" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "strength", "Strength",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.strength, f ); } ) )
				.Help( "How strong the effect is." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions
				.Default( 1.0f )
			.Param( "target", "Target brightness",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.target_luminance; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.target_luminance, f ); } ) )
				.Help( "How bright the picture tries to settle at once it's adjusted." )
				.Range( 0.1f, 0.9f )
				.Step( 0.05f )   // 17 positions; both ends sit on the grid
				.Default( 0.5f )
			.Param( "up_speed", "Brighten speed",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.adapt_up_speed; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.adapt_up_speed, f ); } ) )
				.Help( "How quickly the picture brightens when a scene gets darker." )
				.Range( 0.1f, 5.0f )
				.Step( 0.1f )    // 50 positions, one per tenth of a second
				.Unit( "s" )
				.Default( 1.5f )
			.Param( "down_speed", "Darken speed",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.adapt_down_speed; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.adapt_down_speed, f ); } ) )
				.Help( "How quickly the picture dims when a scene gets brighter." )
				.Range( 0.1f, 5.0f )
				.Step( 0.1f )    // 50 positions, as Brighten speed above
				.Unit( "s" )
				.Default( 2.5f )
			.Param( "min_gain", "Min gain",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.min_gain; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.min_gain, f ); } ) )
				.Help( "How dark the adjustment is allowed to make the picture." )
				.Range( 0.5f, 1.0f )
				.Step( 0.05f )   // 11 positions; Shift+arrow still subdivides it
				.Default( 0.8f )
			.Param( "max_gain", "Max gain",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.max_gain; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.max_gain, f ); } ) )
				.Help( "How bright the adjustment is allowed to make the picture." )
				.Range( 1.0f, 2.0f )
				.Step( 0.05f )   // 21 positions
				.Default( 1.6f );

		// Request #3 (2026-09-04): "a darkness booster for dark games" --
		// titled "Shadow Control" (renamed from "Shadow lift" 2026-09-05);
		// the entry id and every config key deliberately keep the
		// shadow_lift spelling so existing configs and saved palette
		// entries keep working. It lifts shadows (brightens dark areas so
		// detail becomes visible) while leaving highlights alone. One param,
		// well under the six budget -- see this section's header comment.
		// Neutral (0.0, identity) is the default, so an existing config is
		// unaffected.
		a.Switch( "image.shaders.shadow_lift", "Shadow Control",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.shadow_lift.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.shadow_lift.enabled, b ); } ) )
			.Help( "Brightens dark areas so detail in dark games is easier to see, while leaving "
			       "bright areas alone." )
			.Default( false )
			.Keywords( "shadow control lift dark brightness gamma boost darkness" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "strength", "Strength",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.shadow_lift.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.shadow_lift.strength, f ); } ) )
				.Help( "How much darker areas are brightened." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions
				.Default( 0.0f );

		a.Group( "Diagnostics" );

		// The "effect file" / "compiled" / "loaded from" / "uniforms" rows
		// that used to live here diagnosed the runtime-compiled .fx drifting
		// from the binary. The effects are compiled into the binary now
		// (DECISIONS.md #27), so that failure cannot happen and the rows
		// lost their consumer; only the SDR gate is left to explain.
		a.Facts( "image.shaders_facts", "Pipeline", []{
			return std::string( IsBaseLayerSdr() ? "SDR base layer -- effects available"
			                                     : "HDR base layer -- effects unavailable" );
		} )
			.Help( "Shows whether these effects are currently active. Read-only." )
			.Keywords( "effect colourspace sdr hdr pipeline" )
			.Live( "base layer", []{
				return ui::Fact{ "base layer", IsBaseLayerSdr()
					? "SDR (linear or sRGB)"
					: "HDR (scRGB or PQ) -- the SDR-only gate is active" };
			} )
			.Live( "effects", []{
				return ui::Fact{ "effects", "built into this binary -- nothing is loaded from disk" };
			} );
	}

}
