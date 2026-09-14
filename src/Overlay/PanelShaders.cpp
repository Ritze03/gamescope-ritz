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
#include "EffectPreview.h"
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

		// Preview (split screen) (NEW 2026-09-14): a bare bool, no
		// exclusion logic -- see rendervulkan.hpp's bPreviewSplit comment
		// for why it is deliberately outside AnyEnabled().
		e.bPreviewSplit = r.preview_split;

		e.bShadowLift  = r.shadow_lift.enabled;
		e.flShadowLift = r.shadow_lift.strength;

		e.bSaturation            = r.saturation.enabled;
		e.flSaturation           = r.saturation.strength;
		e.bSaturationProtectSkin = r.saturation.protect_skin_tones;

		e.bVibrancy  = r.vibrancy.enabled;
		e.flVibrancy = r.vibrancy.strength;

		e.bPreSharpen  = r.pre_sharpen.enabled;
		e.flPreSharpen = r.pre_sharpen.strength.value_or( 0.5f );

		// Bloom (2026-09-08). The three dispatches that build the glow are
		// recorded by vulkan_composite() only while bBloom is set, so the
		// switch is genuinely "do this work or don't", not a masked uniform.
		e.bBloom          = r.bloom.enabled;
		e.flBloomThreshold = r.bloom.threshold;
		e.flBloomIntensity = r.bloom.intensity;
		e.flBloomRadius    = r.bloom.radius;

		// Adaptive Brightness: consumed by cs_effects_measure.comp (the
		// adapt maths) and cs_effects_layer0.comp (the visible gain).
		e.bAdaptiveBrightness = r.adaptive_brightness.enabled;
		e.bAbDynamic    = r.adaptive_brightness.mode == "dynamic";
		e.flAbTarget    = r.adaptive_brightness.target_luminance;
		e.flAbUpSpeed   = r.adaptive_brightness.adapt_up_speed;
		e.flAbDownSpeed = r.adaptive_brightness.adapt_down_speed;
		e.flAbMinGain   = r.adaptive_brightness.min_gain;
		e.flAbMaxGain   = r.adaptive_brightness.max_gain;
		e.flAbStrength  = r.adaptive_brightness.strength;
		e.flAbLocal     = r.adaptive_brightness.local_strength;
		// Dark floor (2026-09-14, split into a per-effect field the SAME
		// day): this effect's OWN copy -- see this file's standalone
		// "Leave dark scenes alone (Adaptive Brightness)" row below.
		e.flAbDarkFloor = r.adaptive_brightness.dark_floor;

		// Adaptive Gamma (2026-09-08): the same statistics, one exponent.
		// The exclusion with Adaptive Brightness is NOT applied here -- the
		// struct carries what the config says, and EffectsPushData_t drops
		// the flag for the frame. Masking it here too would make the panel
		// and the uniform disagree about what the user's config holds.
		e.bAdaptiveGamma = r.adaptive_gamma.enabled;
		e.flAgTarget     = r.adaptive_gamma.target_luminance;
		e.flAgMaxLift    = r.adaptive_gamma.max_lift;
		e.flAgMaxDarken  = r.adaptive_gamma.max_darken;
		e.flAgStrength   = r.adaptive_gamma.strength;
		e.flAgUpSpeed    = r.adaptive_gamma.adapt_up_speed;
		e.flAgDownSpeed  = r.adaptive_gamma.adapt_down_speed;
		e.flAgLocal      = r.adaptive_gamma.local_strength;
		// Dark floor (2026-09-14, split into a per-effect field the SAME
		// day): this effect's OWN copy -- see this file's "dark_floor"
		// Param, registered as this row's own eighth Param below.
		e.flAgDarkFloor  = r.adaptive_gamma.dark_floor;

		// Adaptive Brightness V2 (2026-09-14): a NEW, ADDITIVE effect --
		// see ConfigSchema.h's ReshadeAdaptiveBrightnessV2Settings. The
		// three-way exclusion with the two effects above is NOT applied
		// here either, for the same reason it is not applied to Adaptive
		// Gamma's own fields just above: this struct carries what the
		// config says, and EffectsPushData_t (rendervulkan.cpp) drops
		// whichever loses for the frame.
		e.bAdaptiveV2   = r.adaptive_brightness_v2.enabled;
		e.bV2Scene      = r.adaptive_brightness_v2.mode != "off";
		e.bV2Knee       = r.adaptive_brightness_v2.shape == "knee";
		e.flV2Lift      = r.adaptive_brightness_v2.lift;
		e.flV2Target    = r.adaptive_brightness_v2.target_luminance;
		e.flV2MaxLift   = r.adaptive_brightness_v2.max_lift;
		e.flV2Detail    = r.adaptive_brightness_v2.detail;
		e.flV2Scale     = r.adaptive_brightness_v2.scale;
		e.flV2AdaptSpeed = r.adaptive_brightness_v2.adapt_speed;
		e.flV2Clarity   = r.adaptive_brightness_v2.clarity;   // Stage 3, 2026-09-14
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
		s_CachedSettings = config::ResolvedSettings();
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
	// (request #3, 2026-09-04) is the fourth, added the same shape; Vibrancy
	// (2026-09-08, alongside the Vibrancy -> Saturation rename) is the
	// fifth; Adaptive Gamma (2026-09-08) is the sixth; Bloom (2026-09-08) is
	// the seventh -- and the first one that is not a per-pixel function, so
	// it is also the first whose switch turns extra DISPATCHES on rather
	// than only a flag bit. An eighth, the experimental Brightness Map
	// effect (2026-09-09), briefly held the second spatial slot; it was
	// removed 2026-09-14 at the user's request -- see
	// superdoc/features/shader-effects.md's History note.)
	//
	// THE SIX BUDGET (now seven), AND WHY ADAPTIVE BRIGHTNESS SITS EXACTLY
	// ON IT. Saturation has 2 params, Vibrancy 1, Pre-Sharpen 1, Bloom 3
	// (2026-09-08), Adaptive Brightness 8,
	// Adaptive Gamma 7, Shadow Control
	// 1 -- the maximum a row
	// may own before Registry.cpp aborts registration and tells the author
	// to promote it to a category. The budget was NOT raised again for
	// Adaptive Gamma and did not need to be: it has fewer knobs because it
	// has fewer mechanisms (no gain to bound, no shadow cap, no mode).
	// Adaptive Brightness fits, but with zero headroom, and that is worth
	// saying out loud: the NEXT parameter added to this effect does not
	// "just" overflow a limit, it is the signal that Adaptive Brightness has
	// become a category rather than a setting.
	//
	// THE SECOND RAISE, 7 -> 8 (2026-09-07, Local adaptation). The note left
	// here after the first raise said the next param was the signal to
	// PROMOTE, not to raise again. That note was not wrong and this raise
	// does not pretend otherwise -- promoting Adaptive Brightness to its own
	// rail category is still the right end state, and it is now recorded as
	// owed work (superdoc/planning/requests-2026-09-08.md). It was not done
	// in the same change as the local operator for two reasons, both about
	// what a user would get: the promotion moves the effect out of
	// "Shaders", where every capture, doc and keyword currently points at
	// it, and it is a shell-layout change whose risk has nothing to do with
	// the tone curve this request is actually about. Landing them together
	// would make one hard-to-judge diff out of two easy ones. The measured
	// cost of the raise itself is one more Inspector row, which
	// test_overlay_shell.cpp already shows scrolls at 2.0x and fits at 1.0x
	// with room to spare.
	//
	// Request #16 (2026-09-06) asked for a MODE on Adaptive Brightness, and
	// it briefly lived as the row's own three-way Choice (Off | Whole image
	// | Dynamic) rather than a seventh param, to stay under the budget --
	// see the git history on this file for that shape. Request #17
	// (2026-09-07, requests-2026-09-07.md item 7) asked for the opposite:
	// "the mode selector should be inside of the inspector rail. In the main
	// view, it should still only be a switch." That put the mode back
	// exactly where a knob belongs (the Inspector's params column) but made
	// it a genuine seventh param on a row that already had six.
	//
	// THE BUDGET DECISION (superdoc/features/shader-effects.md has the full
	// write-up). Every one of the six existing params -- strength, target,
	// up_speed, down_speed, min_gain, max_gain -- is independently
	// documented and independently meaningful; up_speed/down_speed in
	// particular are a deliberate asymmetry (adapting to a brighter vs a
	// darker scene), not two names for one idea, so merging them would be a
	// real loss, not a tidy-up. No honest merge or relocation existed, so
	// Registry.cpp's kParamBudget was raised 6 -> 7 instead (its own comment
	// carries the same reasoning) -- a one-time, evidenced exception, not a
	// standing invitation to keep adding params here.
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

	// Dark floor, Adaptive Brightness's own copy (2026-09-14; split from a
	// shared control into two per-effect ones the SAME day) -- see the
	// standalone row below for why this one is a standalone row rather than
	// a Param under the Switch.
	static bool AdaptiveBrightnessEnabled()
	{
		return EffectsUsable() && Cfg().reshade.adaptive_brightness.enabled;
	}
	static constexpr const char *kNeedsAdaptiveBrightness =
		"turn on Adaptive Brightness above to use this -- it fades that effect specifically, "
		"and does nothing on its own";

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

	// THE TWO ADAPTIVE EFFECTS ARE MUTUALLY EXCLUSIVE (2026-09-08). Adaptive
	// Brightness and Adaptive Gamma both aim the frame's mid-tones at a
	// Target, and both read the SAME statistics -- the measure pass grades
	// its taps but knows nothing about either effect, so its p50 is always
	// the PRE-effect median. Run together, the second one would fit its
	// curve to a median the first has already moved and correct the picture
	// twice; the result is not "both, a bit" but a visible over-lift with no
	// setting that fixes it.
	//
	// `Why a radio and not a greyed-out switch:` greying Adaptive Gamma
	// while Adaptive Brightness is on would leave a config that somehow has
	// both stuck (neither row toggleable), and greying only one of them
	// makes the pair asymmetric for no reason a user could infer. Turning
	// one on turning the other off is a familiar interaction and it is
	// VISIBLE: both switches sit in the same Effects band, so the user sees
	// the other one go dark in the same frame. `Why not compose them and
	// document the result:` the composed result is not a look anybody would
	// choose -- see the doc's measured numbers. The host enforces the same
	// rule again (EffectsPushData_t drops Adaptive Gamma when Adaptive
	// Brightness is on) so a hand-edited config with both cannot produce the
	// double correction either.
	static void SetAdaptiveBrightnessEnabled( bool bOn )
	{
		auto &r = Cfg().reshade;
		r.adaptive_brightness.enabled = bOn;
		if ( bOn )
		{
			r.adaptive_gamma.enabled = false;
			// THREE-WAY, 2026-09-14: Adaptive Brightness V2 joins the same
			// exclusion (see this file's header note on the row itself for
			// why -- same mid-tones, same target, same statistics).
			r.adaptive_brightness_v2.enabled = false;
		}
		PushAllToRenderer();
		QueueSave();
	}

	static void SetAdaptiveGammaEnabled( bool bOn )
	{
		auto &r = Cfg().reshade;
		r.adaptive_gamma.enabled = bOn;
		if ( bOn )
		{
			r.adaptive_brightness.enabled = false;
			r.adaptive_brightness_v2.enabled = false;   // three-way, 2026-09-14
		}
		PushAllToRenderer();
		QueueSave();
	}

	// ADAPTIVE BRIGHTNESS V2 (NEW 2026-09-14) joins the SAME three-way
	// exclusion as the two setters above: it aims the same mid-tones at the
	// same target from the same pre-effect statistics (its own content-only
	// anchor is still a statistic of the SAME graded frame), so running it
	// alongside either of the older two would correct the picture twice.
	// The user's decision was explicit that the older two are UNCHANGED and
	// this is a THIRD, additive choice -- not a replacement -- so the
	// exclusion widens to three rather than the older two being retired.
	static void SetAdaptiveV2Enabled( bool bOn )
	{
		auto &r = Cfg().reshade;
		r.adaptive_brightness_v2.enabled = bOn;
		if ( bOn )
		{
			r.adaptive_brightness.enabled = false;
			r.adaptive_gamma.enabled = false;
		}
		PushAllToRenderer();
		QueueSave();
	}

	// Adaptive Brightness's mode: a Param, not the row's own value (request
	// #17, 2026-09-07) -- the row itself is a plain on/off Switch again, and
	// this two-way choice lives in the Inspector's params column with the
	// other six knobs. `enabled` and `mode` are independent config fields
	// (ConfigSchema.h), so this only ever writes `mode`; the Switch's own
	// SetEffectEnabled() owns `enabled`.
	enum AbModeChoice : int { kAbWholeImage = 0, kAbDynamic = 1 };
	static const ui::Option kAbModeOptions[] = {
		{ kAbWholeImage, "Whole image" },
		{ kAbDynamic,    "Dynamic" },
	};
	static int GetAbMode()
	{
		return Cfg().reshade.adaptive_brightness.mode == "dynamic" ? kAbDynamic : kAbWholeImage;
	}
	static void SetAbMode( int n )
	{
		Cfg().reshade.adaptive_brightness.mode = ( n == kAbDynamic ) ? "dynamic" : "whole_image";
		PushAllToRenderer();
		QueueSave();
	}

	void PanelShaders_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "image.shaders", "Shaders", ui::Section::Display );
		a.Keywords( "shader effect vibrancy saturation sharpen adaptive brightness gamma contrast "
		            "exposure shadow control lift darkness bloom glow local tone" );
		a.Summary( []{
			const auto &r = Cfg().reshade;
			const int n = ( r.saturation.enabled ? 1 : 0 )
			            + ( r.vibrancy.enabled ? 1 : 0 )
			            + ( r.pre_sharpen.enabled ? 1 : 0 )
			            + ( r.bloom.enabled ? 1 : 0 )
			            + ( r.adaptive_brightness.enabled ? 1 : 0 )
			            + ( r.adaptive_gamma.enabled ? 1 : 0 )
			            + ( r.adaptive_brightness_v2.enabled ? 1 : 0 )
			            + ( r.shadow_lift.enabled ? 1 : 0 );
			return std::to_string( n ) + " of 8 effects on";
		} );

		// PREVIEW (SPLIT SCREEN) -- NEW 2026-09-14, the user's own testing
		// aid: "Create a feature for the shaders itself, that's called
		// something like preview mode. It should only apply the shader to
		// the right half of the screen ... so you can judge what an effect
		// you can actually achieve." Deliberately the FIRST row in the
		// area, above the "Effects" band -- it is not itself one of the
		// eight effects the Summary/GroupCount above count, it applies to
		// whichever of them are already on. An empty Group() first: without
		// it this row's own m_nGroup (0, the "no Group() call yet" default)
		// would coincide with the "Effects" GroupCount band pushed right
		// below, which would draw the EFFECTS header above this row and
		// count it as a ninth switch in that band's on/total badge.
		a.Group( "" );
		a.Switch( "image.shaders.preview_split", "Preview (split screen)",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.preview_split; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.preview_split, b ); } ) )
			.Key( "reshade.preview_split" )
			.Help( "Shows the untouched game on the left half and every effect below on the "
			       "right half, so you can judge what an effect really does. The whole frame is "
			       "still processed, only what is displayed differs. A user ReShade .fx file is "
			       "not included in the split." )
			.Default( false )
			.Keywords( "preview split half compare before after side by side" );

		// GroupCount, not Group: SPEC §2.5 lets a band carry a `n / m` count
		// for a switch set, and the shell computes it from the band's own
		// switch rows. Independent binaries are separate rows -- they are
		// NOT a Bank, because they can be turned on for unrelated reasons
		// (SPEC §3.12's governing rule).
		a.GroupCount( "Effects" );

		// Renamed from "Vibrancy" 2026-09-08 (see this file's Vibrancy switch
		// just below, and superdoc/features/shader-effects.md's "Saturation
		// / Vibrancy split"): the user pointed out this effect behaves like
		// an iPhone "Saturation" slider -- a flat multiplier, the same
		// relative boost for every pixel no matter how saturated it already
		// is -- not that app's "Vibrancy". The id, config key and maths are
		// otherwise unchanged.
		a.Switch( "image.shaders.saturation", "Saturation",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.saturation.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.saturation.enabled, b ); } ) )
			.Key( "reshade.saturation.enabled" )
			.Help( "Makes dull colours more vivid, while leaving already-vivid colours alone." )
			.Default( false )
			.Keywords( "saturation vibrancy colour vividness" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "strength", "Saturation",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.saturation.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.saturation.strength, f ); } ) )
				.Key( "reshade.saturation.strength" )
				.Help( "Colour intensity. 1 is unchanged, 0 is black and white, 3 is maximum boost." )
				.Range( 0.0f, 3.0f )
				.Step( 0.05f )   // 61 positions; 1.00, the default, is the neutral notch
				.Default( 1.0f )
			.Param( "protect_skin", "Protect skin tones",
				ui::AnyBind::Of<bool>(
					[]{ return Cfg().reshade.saturation.protect_skin_tones; },
					[]( bool b ) { SetEffectEnabled( &Cfg().reshade.saturation.protect_skin_tones, b ); } ) )
				.Key( "reshade.saturation.protect_skin_tones" )
				.Help( "Keeps the saturation boost off skin tones, so faces don't turn orange." )
				.Default( true );

		// NEW 2026-09-08: the effect the user actually meant by "Vibrancy"
		// -- boosts a pixel's saturation IN PROPORTION to how saturated it
		// already is, so punchy colours get punchier and near-neutral
		// colours are left close to alone. Placed directly after Saturation
		// so the two read together. `Why no "protect skin tones" here:`
		// skin tones sit at a moderate, not extreme, saturation, so this
		// effect already gives them a moderate rather than maximal boost --
		// the shape Saturation's protect-skin toggle exists to force onto a
		// FLAT multiplier is closer to this effect's default behaviour.
		a.Switch( "image.shaders.vibrancy", "Vibrancy",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.vibrancy.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.vibrancy.enabled, b ); } ) )
			.Key( "reshade.vibrancy.enabled" )
			.Help( "Makes already-punchy colours even punchier, while leaving dull, near-grey "
			       "colours close to alone." )
			.Default( false )
			.Keywords( "vibrancy saturation colour punch pop" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "strength", "Strength",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.vibrancy.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.vibrancy.strength, f ); } ) )
				.Key( "reshade.vibrancy.strength" )
				.Help( "How much punchier the already-punchy colours get. 0 is unchanged." )
				.Range( 0.0f, 2.0f )
				.Step( 0.05f )   // 41 positions; 0.00, the default, is neutral
				.Default( 0.0f );

		a.Switch( "image.shaders.presharpen", "Pre-Sharpen",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.pre_sharpen.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.pre_sharpen.enabled, b ); } ) )
			.Key( "reshade.pre_sharpen.enabled" )
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
				.Key( "reshade.pre_sharpen.strength" )
				.Help( "How strong the sharpening is." )
				.Range( 0.0f, 2.0f )
				.Step( 0.05f )   // 41 positions
				.Default( 0.5f );

		// BLOOM -- NEW 2026-09-08. The user's request, verbatim: "Add a
		// bloom shader for more casual games." A glow around bright areas,
		// aimed at looking good rather than at competitive clarity -- so it
		// is off by default and its help text says what it is for.
		//
		// `Where it sits in this band, and why:` immediately after
		// Pre-Sharpen, because those two are the only SPATIAL effects here
		// (the only ones that read a pixel's neighbours), and because the
		// pipeline runs Pre-Sharpen and then Bloom -- so the pair reads in
		// pipeline order in the panel too. Everything above it is a
		// per-pixel colour operation and everything below it is tone.
		//
		// THREE PARAMS, and no more. Threshold, Intensity and Radius are
		// the three questions a bloom actually has ("what glows", "how
		// much", "how far"); the two obvious candidates for a fourth were
		// weighed and rejected. A KNEE / falloff shape was rejected because
		// the bright pass has no separate knee to expose -- the
		// contribution is already a smooth function of how far above the
		// threshold a pixel is (effects_curve.h's bloom_weight), and the
		// only thing a control there could do is make it harder, which is
		// the setting that shimmers. A SEPARATE COLOUR/TINT was rejected
		// because the glow is built from grade()'s own output, so it
		// already carries the picture's colour -- a tint would be a second,
		// contradicting answer to a question Saturation and Vibrancy above
		// already own.
		using BloomDefaults = config::ReshadeBloomSettings;
		a.Switch( "image.shaders.bloom", "Bloom",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.bloom.enabled; },
				[]( bool b ) { SetEffectEnabled( &Cfg().reshade.bloom.enabled, b ); } ) )
			.Key( "reshade.bloom.enabled" )
			.Help( "Adds a soft glow around bright things, the way a camera does -- a look for "
			       "atmospheric games rather than for competitive clarity. Off by default." )
			.Default( BloomDefaults{}.enabled )
			.Keywords( "bloom glow light halo haze soft dreamy cinematic" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "threshold", "Threshold",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.bloom.threshold; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.bloom.threshold, f ); } ) )
				.Key( "reshade.bloom.threshold" )
				.Help( "How bright something has to be before it glows, and how quickly it "
				       "picks up once it is. Higher means only the brightest lights glow; "
				       "lower makes more of the picture hazy." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions; 0.75, the default, is on the grid
				.Default( BloomDefaults{}.threshold )
			.Param( "intensity", "Intensity",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.bloom.intensity; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.bloom.intensity, f ); } ) )
				.Key( "reshade.bloom.intensity" )
				.Help( "How strong the glow is. The glow is blended so it can never blow a "
				       "bright area out to pure white, however high this goes." )
				.Range( 0.0f, 2.0f )
				.Step( 0.05f )   // 41 positions, the same grid Pre-Sharpen's Strength uses
				.Default( BloomDefaults{}.intensity )
			.Param( "radius", "Radius",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.bloom.radius; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.bloom.radius, f ); } ) )
				.Key( "reshade.bloom.radius" )
				.Help( "How far the glow spreads out from a bright area -- a tight halo at 0, a "
				       "wide soft haze at 1." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions
				.Default( BloomDefaults{}.radius );

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
			.Key( "reshade.shadow_lift.enabled" )
			.Help( "Brightens dark areas so detail in dark games is easier to see, while leaving "
			       "bright areas alone." )
			.Default( false )
			.Keywords( "shadow control lift dark brightness gamma boost darkness" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Param( "strength", "Strength",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.shadow_lift.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.shadow_lift.strength, f ); } ) )
				.Key( "reshade.shadow_lift.strength" )
				.Help( "How much darker areas are brightened." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions
				.Default( 0.0f );

		// EIGHT PARAMS -- the budget again, exactly (see this section's
		// header comment for both raises and why each one happened).
		// Back in the Effects GroupCount band as a plain Switch (request
		// #17, 2026-09-07): the mode is now the row's first Param instead of
		// the row's own value, so it shows in the Inspector's params column
		// like the other seven.
		//
		// The .Default()s below read the compiled-in ConfigSchema.h values
		// (as PanelCursor.cpp's rows do) rather than repeating literals:
		// the two drifted once (panel said 1.5s/2.5s/0.8/1.6, schema said
		// 1.0s/1.0s/0.5/2.0) and a "reset to default" then landed on a
		// value no fresh install ever had.
		using AbDefaults = config::ReshadeAdaptiveBrightnessSettings;
		a.Switch( "image.shaders.adaptive_brightness", "Adaptive Brightness",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.adaptive_brightness.enabled; },
				[]( bool b ) { SetAdaptiveBrightnessEnabled( b ); } ) )
			.Key( "reshade.adaptive_brightness.enabled" )
			.Help( "Adjusts the picture as you play, like your eyes adjusting. See the Mode param "
			       "for Whole image vs. Dynamic. Turning this on turns Adaptive Gamma off -- they "
			       "aim the same mid-tones at the same target." )
			.Default( AbDefaults{}.enabled )
			.Keywords( "adaptive brightness eye adaptation exposure auto dynamic contrast gamma "
			           "whole image tone mapping" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			// The Inspector's before/after strip (2026-09-07 request):
			// a frame captured from the game the moment the page appears,
			// split down the middle -- untouched on the left, this effect
			// applied on the right -- re-graded on the CPU as the params
			// below are dragged. See src/Overlay/EffectPreview.cpp.
			// Declared, not drawn: Registry.h's PreviewKind names it and
			// Shell.cpp decides where and how big it goes.
			.Preview( ui::Entry::PreviewKind::AdaptiveBrightness )
			.Param( "mode", "Mode",
				ui::AnyBind::Of<int>( GetAbMode, SetAbMode ),
				kAbModeOptions, std::size( kAbModeOptions ) )
				.Key( "reshade.adaptive_brightness.mode" )
				.Help( "Whole image: one brightness gain from the average. Dynamic: lifts dark "
				       "scenes, tames bright ones and rolls off the highlights so nothing blows "
				       "out -- for maps that are much darker or brighter than the rest." )
				.Default( (int)( AbDefaults{}.mode == "dynamic" ? kAbDynamic : kAbWholeImage ) )
			.Param( "strength", "Strength",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.strength, f ); } ) )
				.Key( "reshade.adaptive_brightness.strength" )
				.Help( "How strong the effect is: blends between the untouched picture and the "
				       "fully adjusted one." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions
				.Default( AbDefaults{}.strength )
			.Param( "target", "Target brightness",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.target_luminance; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.target_luminance, f ); } ) )
				.Key( "reshade.adaptive_brightness.target_luminance" )
				.Help( "Where the picture settles once adjusted -- Whole image aims its average "
				       "here, Dynamic aims its mid-tones here." )
				.Range( 0.1f, 0.9f )
				.Step( 0.05f )   // 17 positions; both ends sit on the grid
				.Default( AbDefaults{}.target_luminance )
			.Param( "up_speed", "Adapt to brighter",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.adapt_up_speed; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.adapt_up_speed, f ); } ) )
				.Key( "reshade.adaptive_brightness.adapt_up_speed" )
				.Help( "How long it takes to settle after the scene gets brighter (the picture "
				       "is dimmed). Shorter reacts faster; longer is calmer." )
				.Range( 0.1f, 5.0f )
				.Step( 0.1f )    // 50 positions, one per tenth of a second
				.Unit( "s" )
				.Default( AbDefaults{}.adapt_up_speed )
			.Param( "down_speed", "Adapt to darker",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.adapt_down_speed; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.adapt_down_speed, f ); } ) )
				.Key( "reshade.adaptive_brightness.adapt_down_speed" )
				.Help( "How long it takes to settle after the scene gets darker (the picture is "
				       "lifted). Shorter reacts faster; longer is calmer." )
				.Range( 0.1f, 5.0f )
				.Step( 0.1f )    // 50 positions, as above
				.Unit( "s" )
				.Default( AbDefaults{}.adapt_down_speed )
			.Param( "min_gain", "Min gain",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.min_gain; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.min_gain, f ); } ) )
				.Key( "reshade.adaptive_brightness.min_gain" )
				.Help( "How dark the adjustment may make the picture. In Dynamic, also how far "
				       "the deepest shadows may be pushed down." )
				.Range( 0.3f, 1.0f )   // widened from 0.5..1.0, 2026-09-07 request
				.Step( 0.05f )   // 15 positions; Shift+arrow still subdivides it
				.Default( AbDefaults{}.min_gain )
			.Param( "max_gain", "Max gain",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.max_gain; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.max_gain, f ); } ) )
				.Key( "reshade.adaptive_brightness.max_gain" )
				.Help( "How bright the adjustment may make the picture. In Dynamic it caps both "
				       "the gain and how far the mid-tones may be lifted on top of it, so 1.0 "
				       "really does mean \"do not brighten\"." )
				.Range( 1.0f, 4.0f )   // widened from 1.0..2.0, 2026-09-07 request
				.Step( 0.1f )    // 31 positions; a finer 0.05 step would be 61, too fine
				                 // over the wider span for a slider to feel graduated
				.Default( AbDefaults{}.max_gain )
			// Local adaptation (2026-09-07): the eighth param, and the one
			// that took the budget 7 -> 8 -- see this section's header.
			// Dynamic only, hence the "in Dynamic" wording; in Whole image
			// the host masks it to 0 (rendervulkan.cpp's EffectsPushData_t)
			// because that mode has no per-pixel curve to fit locally.
			.Param( "local_strength", "Local adaptation",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness.local_strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.local_strength, f ); } ) )
				.Key( "reshade.adaptive_brightness.local_strength" )
				.Help( "In Dynamic, how much each part of the picture is adjusted for its own "
				       "brightness rather than the whole frame's -- so a dark room and a bright "
				       "window can both be readable at once. 0% is one setting for the whole "
				       "picture." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions, as Strength has -- and no
				                 // Unit, exactly as Strength has none: both
				                 // are 0..1 dry/wet mixes, and a "%" suffix
				                 // on a 0..1 range would read "0.50 %"
				.Default( AbDefaults{}.local_strength );

		// DARK FLOOR (ADAPTIVE BRIGHTNESS) -- this effect's own copy, split
		// 2026-09-14 from a single shared reshade.dark_floor field into one
		// per effect (the user's follow-up request: "Make the 'Leave dark
		// scenes alone' part individual settings for both Adaptive Gamma
		// and Adaptive Brightness" -- see ConfigSchema.h's
		// kCurrentSchemaVersion 4->5 comment for the migration). A
		// standalone row placed directly under Adaptive Brightness's own
		// group, NOT a Param under the Switch above: that row is already
		// AT kParamBudget (8, "zero headroom" per its own header comment),
		// so a ninth Param is unrepresentable -- Registry.cpp's
		// AddParam() refuses it outright (SixBudget). Adaptive Gamma's
		// identical control, just below, IS a normal Param instead,
		// because that row had a spare eighth slot; the two ended up in
		// different places in the panel for that reason alone, not a
		// difference in what either control does. Labelled "(Adaptive
		// Brightness)" so the two rows are never mistaken for the shared
		// control this replaces.
		using AbDarkFloorDefaults = config::ReshadeAdaptiveBrightnessSettings;
		a.Slider( "image.shaders.adaptive_brightness_dark_floor",
			"Leave dark scenes alone (Adaptive Brightness)",
			ui::AnyBind::Of<float>(
				[]{ return Cfg().reshade.adaptive_brightness.dark_floor; },
				[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness.dark_floor, f ); } ) )
			.Key( "reshade.adaptive_brightness.dark_floor" )
			.Help( "Below this scene brightness Adaptive Brightness fades out, so a truly dark "
			       "scene stays dark instead of being lifted to grey. 0 turns this off. Does "
			       "nothing while Adaptive Brightness itself is off." )
			.Range( 0.0f, 0.5f )
			.Step( 0.01f )
			.ZeroMeans( "Off" )
			.Default( AbDarkFloorDefaults{}.dark_floor )
			.Keywords( "dark floor black crush destroy binarise binarize adaptive brightness "
			           "near black" )
			.DisabledUnless( AdaptiveBrightnessEnabled, kNeedsAdaptiveBrightness );

		// ADAPTIVE GAMMA -- NEW 2026-09-08. The user's request, verbatim:
		// "Make something similar, but make it gamma based. Call it adaptive
		// gamma." The same measured statistics, and the whole operator is
		// ONE exponent fitted to land the smoothed median on Target: no
		// levels gain, no white point, no shoulder. See
		// src/shaders/effects_curve.h's ADAPTIVE GAMMA block for the
		// arithmetic and superdoc/features/shader-effects.md for the
		// measurements.
		//
		// EIGHT PARAMS as of 2026-09-14 -- Strength, Target brightness, Max
		// lift, Max darken, Adapt to brighter/darker (2026-09-09), Local
		// adaptation, and Leave dark scenes alone (2026-09-14, this row's
		// own copy of the dark-floor split below) -- at Registry.cpp's
		// kParamBudget of 8, zero headroom, the same ceiling Adaptive
		// Brightness's own row sits at. Fewer MECHANISMS than Adaptive
		// Brightness even so: there is no gain to bound and no shadow cap,
		// so min_gain/max_gain/mode have no counterpart here -- the params
		// this row does have are Target's own bounds and its adaptation
		// controls, not a second exposure path.
		//
		// `Why Max lift and Max darken are params at all, rather than two
		// constants in the header:` Target reaches the picture ONLY through
		// the exponent, so whatever clamps the exponent decides where Target
		// stops doing anything -- and 2026-09-08 cost a whole session to the
		// discovery that a clamped slider looks exactly like a working one.
		// Every limit that can stop this effect is therefore a control the
		// user can see and move, and the Diagnostics row below names which
		// one is binding right now. Placed immediately after Adaptive
		// Brightness because the two are alternatives to each other.
		using AgDefaults = config::ReshadeAdaptiveGammaSettings;
		a.Switch( "image.shaders.adaptive_gamma", "Adaptive Gamma",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.adaptive_gamma.enabled; },
				[]( bool b ) { SetAdaptiveGammaEnabled( b ); } ) )
			.Key( "reshade.adaptive_gamma.enabled" )
			.Help( "Adjusts the picture's contrast as you play by bending the mid-tones toward a "
			       "target, leaving black and white exactly where they are -- so nothing can blow "
			       "out. Turning this on turns Adaptive Brightness off." )
			.Default( AgDefaults{}.enabled )
			.Keywords( "adaptive gamma contrast curve exposure auto tone midtones dark bright" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			// The same before/after strip Adaptive Brightness declares, and
			// deliberately the same PreviewKind: the two effects are
			// mutually exclusive, so only one of them can ever be the one
			// being previewed, and EffectPreview.cpp picks whichever is on.
			.Preview( ui::Entry::PreviewKind::AdaptiveBrightness )
			.Param( "strength", "Strength",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.strength, f ); } ) )
				.Key( "reshade.adaptive_gamma.strength" )
				.Help( "How strong the effect is: blends between the untouched picture and the "
				       "fully adjusted one." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions, as Adaptive Brightness's Strength has
				.Default( AgDefaults{}.strength )
			.Param( "target", "Target brightness",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.target_luminance; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.target_luminance, f ); } ) )
				.Key( "reshade.adaptive_gamma.target_luminance" )
				.Help( "Where the picture's mid-tones settle. Higher lifts the whole middle of the "
				       "picture; black and white stay where they are either way." )
				.Range( 0.1f, 0.9f )
				.Step( 0.05f )   // 17 positions; both ends sit on the grid
				.Default( AgDefaults{}.target_luminance )
			.Param( "max_lift", "Max lift",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.max_lift; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.max_lift, f ); } ) )
				.Key( "reshade.adaptive_gamma.max_lift" )
				.Help( "How far the mid-tones may be brightened on a dark scene. 1.0 means \"do not "
				       "brighten\". Raise it if the Diagnostics line says Max lift is what's "
				       "stopping Target brightness." )
				.Range( 1.0f, 4.0f )
				.Step( 0.1f )    // 31 positions, as Adaptive Brightness's Max gain has
				.Default( AgDefaults{}.max_lift )
			.Param( "max_darken", "Max darken",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.max_darken; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.max_darken, f ); } ) )
				.Key( "reshade.adaptive_gamma.max_darken" )
				.Help( "How far the mid-tones may be darkened on a bright scene. 1.0 means \"do not "
				       "darken\". Above about 1.5 the deepest shadows start to go black." )
				.Range( 1.0f, 4.0f )
				.Step( 0.1f )    // 31 positions, the same grid as Max lift
				.Default( AgDefaults{}.max_darken )
			// ADAPTATION SPEED, this row's own pair -- 2026-09-09, the user:
			// "For adaptive gamma, there should also be some value, to
			// adjust the speed of it". Until now the measure pass's EMA was
			// driven unconditionally by Adaptive Brightness's two speeds,
			// i.e. by sliders that are not even reachable while this effect
			// is the one running (the two are mutually exclusive). TWO, not
			// the one "some value" literally asks for, because this row
			// already pairs its directions everywhere else (Max lift / Max
			// darken) and because "react quickly when the scene brightens,
			// ease slowly into darkness" is a setting one number cannot
			// express -- exactly the argument that kept Adaptive
			// Brightness's own pair intact when the budget was tight. Same
			// range, step, unit and defaults as that row's, so switching
			// between the two effects does not change how fast the picture
			// follows the scene. That took this row to 7 params of
			// kParamBudget's 8 at the time; the budget was NOT raised. (An
			// eighth, this row's own "Leave dark scenes alone" copy of the
			// 2026-09-14 dark-floor split, fills the last slot below.)
			.Param( "up_speed", "Adapt to brighter",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.adapt_up_speed; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.adapt_up_speed, f ); } ) )
				.Key( "reshade.adaptive_gamma.adapt_up_speed" )
				.Help( "How long it takes to settle after the scene gets brighter (the mid-tones "
				       "are darkened). Shorter reacts faster; longer is calmer." )
				.Range( 0.1f, 5.0f )
				.Step( 0.1f )    // 50 positions, one per tenth of a second
				.Unit( "s" )
				.Default( AgDefaults{}.adapt_up_speed )
			.Param( "down_speed", "Adapt to darker",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.adapt_down_speed; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.adapt_down_speed, f ); } ) )
				.Key( "reshade.adaptive_gamma.adapt_down_speed" )
				.Help( "How long it takes to settle after the scene gets darker (the mid-tones are "
				       "lifted). Shorter reacts faster; longer is calmer." )
				.Range( 0.1f, 5.0f )
				.Step( 0.1f )    // 50 positions, as above
				.Unit( "s" )
				.Default( AgDefaults{}.adapt_down_speed )
			.Param( "local_strength", "Local adaptation",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.local_strength; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.local_strength, f ); } ) )
				.Key( "reshade.adaptive_gamma.local_strength" )
				.Help( "How much each part of the picture is adjusted for its own brightness rather "
				       "than the whole frame's, so a dark room and a bright window can both be "
				       "readable. Does nothing on a picture that is evenly lit." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )   // 21 positions, as Adaptive Brightness's own has
				.Default( AgDefaults{}.local_strength )
			// DARK FLOOR -- this row's own EIGHTH param, and the reason the
			// header comment above says "zero headroom" (2026-09-14). Split
			// the SAME day from a single shared reshade.dark_floor field
			// into one copy per effect (the user's follow-up request:
			// "Make the 'Leave dark scenes alone' part individual settings
			// for both Adaptive Gamma and Adaptive Brightness") -- unlike
			// Adaptive Brightness's copy just below in this file, THIS one
			// fits as a normal Param because this row had a spare slot
			// (7 of 8) rather than none.
			.Param( "dark_floor", "Leave dark scenes alone",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_gamma.dark_floor; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_gamma.dark_floor, f ); } ) )
				.Key( "reshade.adaptive_gamma.dark_floor" )
				.Help( "Below this scene brightness the effect fades out, so a truly dark scene "
				       "stays dark instead of being lifted to grey. 0 turns this off." )
				.Range( 0.0f, 0.5f )
				.Step( 0.01f )
				.ZeroMeans( "Off" )
				.Default( AgDefaults{}.dark_floor )
				.Keywords( "dark floor black crush destroy binarise binarize near black" );

		// ADAPTIVE BRIGHTNESS V2 -- NEW 2026-09-14. A NEW, ADDITIVE effect --
		// the user's decision, verbatim: "Call it 'Adaptive brightness V2'
		// in the GUI. Implement it fully, so I can test it later. DO NOT
		// REMOVE THE ORIGINAL!" -- so Adaptive Brightness and Adaptive
		// Gamma above are UNCHANGED, and this is a THIRD choice, alongside
		// them, not a replacement. See superdoc/planning/adaptive-
		// brightness-v2-plan.md for the whole design and
		// src/shaders/effects_curve.h's own block for every formula.
		//
		// WHY IT EXISTS ALONGSIDE THE OTHER TWO. Every operator shipped so
		// far lifts with x^g, and x^g for g < 1 has an INFINITE slope at
		// black -- on a near-black scene (the user's own capture, 73.5% at
		// code 0..1) the exponent is driven to its floor and code 4 lands
		// on 90: structural binarisation, not a tuning error (plan section
		// 3.2). This effect's curve bends into a straight line of slope S
		// (the user's own Max lift) at black instead, so the LARGEST
		// amplification anywhere in the frame is S, by construction --
		// black can never be pushed past white the way it could before.
		//
		// EIGHT PARAMS (Shape, Target, Max lift, Lift, Adaptation, Adapt
		// speed, Detail, Clarity), at kParamBudget's 8 -- zero headroom, the
		// same ceiling Adaptive Brightness's own row sits at. Clarity
		// (Stage 3, plan section 4.9) is the last of the seven that were
		// left spare when Stage 1+2 shipped 2026-09-14.
		using V2Defaults = config::ReshadeAdaptiveBrightnessV2Settings;
		enum V2ShapeChoice : int { kV2Toe = 0, kV2Knee = 1 };
		static const ui::Option kV2ShapeOptions[] = {
			{ kV2Toe,  "Toe" },
			{ kV2Knee, "Knee" },
		};
		enum V2ModeChoice : int { kV2Off = 0, kV2Scene = 1 };
		static const ui::Option kV2ModeOptions[] = {
			{ kV2Off,   "Off" },
			{ kV2Scene, "Scene" },
		};
		a.Switch( "image.shaders.adaptive_brightness_v2", "Adaptive Brightness V2",
			ui::AnyBind::Of<bool>(
				[]{ return Cfg().reshade.adaptive_brightness_v2.enabled; },
				[]( bool b ) { SetAdaptiveV2Enabled( b ); } ) )
			.Key( "reshade.adaptive_brightness_v2.enabled" )
			.Help( "A newer take on Adaptive Brightness: lifts dark scenes without ever turning "
			       "them into flat grey, even on a near-black map. Turning this on turns Adaptive "
			       "Brightness and Adaptive Gamma off -- all three aim the same mid-tones at the "
			       "same target." )
			.Default( V2Defaults{}.enabled )
			.Keywords( "adaptive brightness v2 shadow lift toe knee gamma dark scene contrast "
			           "binarise binarize black clarity see enemies pvp" )
			.DisabledUnless( EffectsUsable, kSdrOnly )
			.Preview( ui::Entry::PreviewKind::AdaptiveBrightness )
			.Param( "shape", "Shape",
				ui::AnyBind::Of<int>(
					[]{ return (int)( Cfg().reshade.adaptive_brightness_v2.shape == "knee" ? kV2Knee : kV2Toe ); },
					[]( int n ) {
						Cfg().reshade.adaptive_brightness_v2.shape = ( n == kV2Knee ) ? "knee" : "toe";
						PushAllToRenderer();
						QueueSave();
					} ),
				kV2ShapeOptions, std::size( kV2ShapeOptions ) )
				.Key( "reshade.adaptive_brightness_v2.shape" )
				.Help( "Toe compresses highlights a little everywhere, so nothing ever clips. Knee "
				       "leaves highlights exactly alone and puts the trade in the mid-tones just "
				       "above the lifted shadows instead -- closer to a monitor's Shadow Boost." )
				.Default( (int)( V2Defaults{}.shape == "knee" ? kV2Knee : kV2Toe ) )
			.Param( "target", "Target brightness",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness_v2.target_luminance; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness_v2.target_luminance, f ); } ) )
				.Key( "reshade.adaptive_brightness_v2.target_luminance" )
				.Help( "Where the CONTENT median is put on a dark scene (black itself is excluded, "
				       "so a mostly-void frame doesn't chase the void). Lower than the older effects' "
				       "default -- this reads the content, not the void, and 0.5 reads milky." )
				.Range( 0.1f, 0.9f )
				.Step( 0.05f )
				.Default( V2Defaults{}.target_luminance )
			.Param( "max_lift", "Max lift",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness_v2.max_lift; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness_v2.max_lift, f ); } ) )
				.Key( "reshade.adaptive_brightness_v2.max_lift" )
				.Help( "The hardest any dark step may be amplified, anywhere in the frame. This is "
				       "the anti-binarisation guarantee: 1.0 means \"do not lift at all\"; 8.0 makes "
				       "a 3-code shape readable at the cost of visible dither on flat walls." )
				.Range( 1.0f, 8.0f )
				.Step( 0.5f )
				.Default( V2Defaults{}.max_lift )
			.Param( "lift", "Lift",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness_v2.lift; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness_v2.lift, f ); } ) )
				.Key( "reshade.adaptive_brightness_v2.lift" )
				.Help( "The lift that is ALWAYS there, whatever Adaptation says: how much a dark "
				       "shape on a bright world is raised. The control that fixes a dark player "
				       "model turning almost invisible on a sunny map." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )
				.Default( V2Defaults{}.lift )
			.Param( "mode", "Adaptation",
				ui::AnyBind::Of<int>(
					[]{ return (int)( Cfg().reshade.adaptive_brightness_v2.mode == "off" ? kV2Off : kV2Scene ); },
					[]( int n ) {
						Cfg().reshade.adaptive_brightness_v2.mode = ( n == kV2Off ) ? "off" : "scene";
						PushAllToRenderer();
						QueueSave();
					} ),
				kV2ModeOptions, std::size( kV2ModeOptions ) )
				.Key( "reshade.adaptive_brightness_v2.mode" )
				.Help( "Off: a purely static shadow lift with no exposure movement at all -- the "
				       "mode to pick if you never want the picture to shift. Scene: a genuinely dark "
				       "map deepens the lift toward Target brightness as you play." )
				.Default( (int)( V2Defaults{}.mode == "off" ? kV2Off : kV2Scene ) )
			.Param( "adapt_speed", "Adapt speed",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness_v2.adapt_speed; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness_v2.adapt_speed, f ); } ) )
				.Key( "reshade.adaptive_brightness_v2.adapt_speed" )
				.Help( "In Scene mode, how fast a GRADUAL scene change is followed. A flashbang or "
				       "walking through a door is not gradual -- this effect snaps instantly for "
				       "those instead of sliding, so there is only one speed to tune here." )
				.Range( 0.1f, 5.0f )
				.Step( 0.1f )
				.Unit( "s" )
				.Default( V2Defaults{}.adapt_speed )
			.Param( "detail", "Detail",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness_v2.detail; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness_v2.detail, f ); } ) )
				.Key( "reshade.adaptive_brightness_v2.detail" )
				.Help( "Texture and outline contrast inside lifted regions. 1.0 preserves it exactly "
				       "as it was before the lift (Weber contrast, the cue a silhouette needs); "
				       "higher boosts it, 0 flattens lifted regions to a plain wash." )
				.Range( 0.0f, 2.0f )
				.Step( 0.05f )
				.Default( V2Defaults{}.detail )
			// CLARITY -- Stage 3 (NEW 2026-09-14), the row the seven params
			// above left spare in kParamBudget. A second, FINER guided-
			// filter pair (r2 = r/4) gives the 4..16px "silhouette band" --
			// the one term in this whole effect that targets an object's
			// OWN outline rather than exposure, edge-aware and bounded the
			// same way Detail is (a hard edge has a ~= 1 in both filters,
			// so nothing is added there -- no rim). See
			// superdoc/planning/adaptive-brightness-v2-plan.md section 4.9
			// and effects_common.h's v2_coef_sample_fine().
			.Param( "clarity", "Clarity",
				ui::AnyBind::Of<float>(
					[]{ return Cfg().reshade.adaptive_brightness_v2.clarity; },
					[]( float f ) { SetEffectFloat( &Cfg().reshade.adaptive_brightness_v2.clarity, f ); } ) )
				.Key( "reshade.adaptive_brightness_v2.clarity" )
				.Help( "Makes a silhouette's own outline pop without touching exposure -- a second, "
				       "finer filter than Detail's, at the scale a limb or a weapon reads at. 0 is "
				       "off; a hard, already-visible edge is left alone either way, so this cannot "
				       "turn into an unsharp-mask rim." )
				.Range( 0.0f, 1.0f )
				.Step( 0.05f )
				.Default( V2Defaults{}.clarity );

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
			} )
			// PRE-PASS TIMING (NEW 2026-09-14, Stage 0). Every cost figure
			// this page used to state ("sub-millisecond", "~0.2-0.3ms") was
			// an ESTIMATE -- vulkan_composite() had no GPU timing until this
			// pass added a vkCmdWriteTimestamp pair around the whole
			// pre-pass. -1.0 (vulkan_effects_gpu_us()'s "no measurement")
			// covers both "the device does not support timestamps" and
			// "the pre-pass has not run yet", so this reads that once
			// rather than asking two questions.
			.Live( "pre-pass", []{
				const float flUs = vulkan_effects_gpu_us();
				if ( flUs < 0.0f )
					return ui::Fact{ "pre-pass", "not measured -- GPU timestamps are unsupported here, or nothing has run yet" };
				char szLine[64];
				snprintf( szLine, sizeof( szLine ), "%.2f ms (%.0f us)", flUs / 1000.0f, flUs );
				return ui::Fact{ "pre-pass", std::string( szLine ) };
			} )
			// WHICH LIMIT IS BINDING (2026-09-08). A clamped slider looks
			// exactly like a working one: the user spent a session moving
			// Target brightness and Max gain over ranges where the maths
			// could not respond, because nothing on screen said so. This
			// names the constraint in the same words `effects_ab_log`
			// prints, classified by effects_curve.h's ab_dyn_binding() from
			// the frame the Inspector's preview already captures -- one
			// classifier, so the panel and a trace cannot disagree. It
			// describes the FRAME's curve (Local adaptation redistributes
			// inside these same bounds, it never widens them).
			// Serves BOTH adaptive effects (Adaptive Gamma, 2026-09-08):
			// they are mutually exclusive, so exactly one classifier can
			// apply at a time and the row never has to choose.
			.Live( "adaptive limit", []{
				std::string sLine;
				if ( !gamescope::overlay::AbPreview_BindingLine( sLine ) )
					sLine = "not measured -- turn Adaptive Brightness or Adaptive Gamma on over an SDR game";
				return ui::Fact{ "adaptive limit", sLine };
			} );
	}

}
