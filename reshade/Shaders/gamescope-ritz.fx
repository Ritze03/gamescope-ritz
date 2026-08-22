// gamescope-ritz.fx -- combined ReShade effect: Vibrancy + Pre-Sharpen +
// Adaptive Brightness (M9, added after the #17 persistence spike -- see
// superdoc/planning/DECISIONS.md #14 and reshade-shaders.md for the spike's
// method/evidence).
//
// One always-loaded technique, two sequential passes, each independently
// gated by its own on/off uniform, rather than shipped as separate
// techniques/effects a user "switches between". See
// superdoc/planning/DECISIONS.md #13: gamescope's ReShade manager
// (src/reshade_effect_manager.cpp) recompiles the *entire* pipeline --
// full FX parse + SPIR-V codegen + Vulkan pipeline build -- inline on the
// steamcompmgr render thread whenever the active technique/effect changes.
// Gating each effect behind a uniform inside one always-loaded technique
// means the Shaders panel toggling an effect costs a single memcpy into
// gamescope's runtime-uniform map, not a recompile. Do not split this back
// into separate techniques/files -- that reintroduces the recompile hitch
// this file exists to avoid.
//
// Live parameter updates: every user-adjustable value below carries
// gamescope's own `source` annotation. This is NOT standard ReShade
// authoring -- it's what makes a uniform live-writable from the overlay at
// all (RuntimeUniform, src/reshade_effect_manager.cpp:181-597). A uniform
// with no `source` annotation is a DataUniform and silently just re-copies
// its FX initializer every frame forever, forever ignoring any external
// write. Every knob PanelShaders.cpp exposes MUST have one of these.
//
// Adaptive Brightness (M9, DECISIONS.md #14) landed after a throwaway
// persistence spike (#17) proved out the two load-bearing constraints this
// section relies on, on real hardware (RADV/AMD), neither of which was
// exercised by Vibrancy/Pre-Sharpen:
//   1. A named render-target texture's contents DO survive from one
//      execute() (real frame) to the next -- confirmed empirically, not
//      just read off the barrier code. This is what makes
//      texAdaptedLuminance below a real persistent history buffer.
//   2. Self-sampling (a pass reading, via a sampler, the very texture that
//      is that pass's own RenderTarget -- exactly what the EMA blend below
//      needs: read last frame's adapted value, blend, write the new one,
//      all in one pass) does NOT crash on this driver, provided the shader
//      declares at least one `uniform`. A uniform-free throwaway spike
//      shader crashed in pipeline creation (a zero-size uniform buffer,
//      `total_uniform_size == 0` -> `vkCreateBuffer(size=0)`) regardless of
//      whether it self-sampled -- a real but narrow driver-adjacent edge
//      case that every effect in THIS file, having several uniforms, never
//      comes close to.
// One constraint carries forward unchanged from the spike: the pass with
// no explicit RenderTarget (the implicit default output) must be the LAST
// pass in the technique, or ReshadeEffectKey's cache goes unstable and the
// pipeline recompiles every single frame instead of caching (confirmed by
// counting "Compiling pass" log lines). That's why Pre-Sharpen below now
// has an explicit RenderTarget (texPreSharpenOut) instead of the implicit
// one it used to have -- Adaptive Brightness's own Apply pass is the new
// last/implicit-output pass.
//
// SDR only for v1 (DECISIONS.md #15): all three effects work directly on
// the base layer's already-encoded 0..1 RGB, with no HDR-aware colour
// handling. PanelShaders.cpp refuses to let the user enable any of them
// while the base layer's colourspace isn't SDR (LINEAR/SRGB), so this file
// doesn't need to branch on BUFFER_COLOR_SPACE today -- see
// reshade-shaders.md Q6 for what correct HDR-space math here would require.
//
// ponytail: works on encoded (gamma-ish) RGB directly rather than
// linearizing via the sRGB EOTF before the math and re-encoding after --
// the common ReShade community-shader shortcut, acceptable here because the
// whole effect is already scoped SDR-only for v1. Upgrade path: add an
// explicit linearize/re-encode round trip around ApplyVibrancy/ApplySharpen
// if a future milestone needs colour-accurate results.

uniform bool VibrancyEnabled <
	source = "vibrancy_enabled";
	defaultValue = false;
> = false;

uniform float VibrancyStrength <
	source = "vibrancy_strength";
	defaultValue = 0.0;
> = 0.0;

uniform bool VibrancyProtectSkinTones <
	source = "vibrancy_protect_skin_tones";
	defaultValue = true;
> = true;

uniform bool PreSharpenEnabled <
	source = "pre_sharpen_enabled";
	defaultValue = false;
> = false;

// Unsharp-mask amount. 0 = no-op, ~0.5 = moderate, 2.0 = aggressive/ringy.
// Range and default picked here (SPEC.md Feature 2 flagged this TBD) --
// 0.5 matches the order of magnitude of common reference ReShade sharpen
// defaults (e.g. LumaSharpen's own default strength).
uniform float PreSharpenStrength <
	source = "pre_sharpen_strength";
	defaultValue = 0.5;
> = 0.5;

// ---- Adaptive Brightness (M9) ------------------------------------------
uniform bool AdaptiveBrightnessEnabled <
	source = "adaptive_brightness_enabled";
	defaultValue = false;
> = false;

// Normalized target scene brightness the effect adapts toward. Range/
// default per ConfigSchema.h's ReshadeAdaptiveBrightnessSettings.
uniform float AdaptiveBrightnessTargetLuminance <
	source = "adaptive_brightness_target_luminance";
	defaultValue = 0.5;
> = 0.5;

// Time constants (seconds to reach ~63% of the target), not frame counts --
// scaled by the real Frametime uniform below so adaptation speed doesn't
// depend on framerate. Separate up/down per DECISIONS.md's Adaptive
// Brightness design note: brightening (dark->bright, "dazzled") and
// darkening (bright->dark) shouldn't feel the same.
uniform float AdaptiveBrightnessAdaptUpSpeed <
	source = "adaptive_brightness_adapt_up_speed";
	defaultValue = 1.0;
> = 1.0;

uniform float AdaptiveBrightnessAdaptDownSpeed <
	source = "adaptive_brightness_adapt_down_speed";
	defaultValue = 1.0;
> = 1.0;

uniform float AdaptiveBrightnessMinGain <
	source = "adaptive_brightness_min_gain";
	defaultValue = 0.5;
> = 0.5;

uniform float AdaptiveBrightnessMaxGain <
	source = "adaptive_brightness_max_gain";
	defaultValue = 2.0;
> = 2.0;

// Dry/wet mix against the unmodified image -- lets the panel fade the
// effect in/out without unloading it (see the EnsureEffectLoaded() comment
// in PanelShaders.cpp for why that matters).
uniform float AdaptiveBrightnessStrength <
	source = "adaptive_brightness_strength";
	defaultValue = 1.0;
> = 1.0;

// Real elapsed frame time in milliseconds (FrameTimeUniform, a proven-live
// gamescope builtin -- see reshade-shaders.md Q1). Drives the EMA blend
// factor below so adaptation speed is frame-rate independent.
uniform float AdaptiveBrightnessFrametime < source = "frametime"; > = 16.6;

texture texColor : COLOR;
sampler samplerColor { Texture = texColor; };

// Named intermediate target: pass 1's output, pass 2's input. Using a real
// named texture (rather than relying on any implicit backbuffer ping-pong --
// this ReShade implementation has none, each sampler's source texture is
// fixed at pipeline-build time, see reshade_effect_manager.cpp's
// ReshadeEffectPipeline::init()/execute()) is what makes Pre-Sharpen see
// Vibrancy's output instead of the original input.
texture texVibrancyOut { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler samplerVibrancyOut { Texture = texVibrancyOut; };

// Pre-Sharpen's output, now a named target (it used to be the technique's
// implicit last pass) -- Adaptive Brightness's Apply pass is the new last/
// implicit-output pass, per this file's header comment on why that pass
// must be the one with no explicit RenderTarget.
texture texPreSharpenOut { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler samplerPreSharpenOut { Texture = texPreSharpenOut; };

// 1x1 persistent history texture: the exponentially-smoothed "adapted"
// scene luminance, carried frame to frame. Self-sampled by
// PS_AdaptiveBrightnessAdapt below -- see this file's header comment for
// why that's safe here (spike #17).
texture texAdaptedLuminance { Width = 1; Height = 1; Format = R32F; };
sampler samplerAdaptedLuminance { Texture = texAdaptedLuminance; };

// Standard full-screen-triangle vertex shader (three vertices, no vertex
// buffer) -- the common ReShade-authoring pattern, since this ReShade
// implementation has no built-in default vertex shader to reuse.
void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Adaptive vibrance: boosts saturation more on already-desaturated pixels
// (so already-vivid colours don't blow out/clip as fast), with an optional
// hue-based mask that dampens the effect on warm, skin-tone-like hues.
float3 ApplyVibrancy(float3 color)
{
	float luma = dot(color, float3(0.299, 0.587, 0.114));
	float maxc = max(color.r, max(color.g, color.b));
	float minc = min(color.r, min(color.g, color.b));
	float sat = maxc - minc;

	float skinMask = 0.0;
	if (VibrancyProtectSkinTones)
	{
		// Warm hues where red clearly leads blue, and green sits roughly
		// between them -- a cheap, deliberately approximate skin-tone mask,
		// not a colour-science-accurate one.
		skinMask = saturate((color.r - color.b) * 3.0) * saturate(1.0 - abs(color.g - (color.r + color.b) * 0.5) * 4.0);
	}
	float protect = lerp(1.0, 0.3, skinMask);

	float amount = VibrancyStrength * (1.0 - sat) * protect;
	return lerp(luma.xxx, color, 1.0 + amount);
}

void PS_Vibrancy(float4 pos : SV_Position, float2 texcoord : TEXCOORD, out float4 outColor : SV_Target)
{
	float3 color = tex2D(samplerColor, texcoord).rgb;
	if (VibrancyEnabled)
		color = ApplyVibrancy(color);
	outColor = float4(saturate(color), 1.0);
}

// Classic 4-tap cross unsharp mask: subtract a cheap blur estimate from the
// centre sample and add the difference back in, scaled by strength.
float3 ApplySharpen(float2 texcoord)
{
	float2 px = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
	float3 center = tex2D(samplerVibrancyOut, texcoord).rgb;
	float3 n = tex2D(samplerVibrancyOut, texcoord + float2(0.0, -px.y)).rgb;
	float3 s = tex2D(samplerVibrancyOut, texcoord + float2(0.0,  px.y)).rgb;
	float3 e = tex2D(samplerVibrancyOut, texcoord + float2( px.x, 0.0)).rgb;
	float3 w = tex2D(samplerVibrancyOut, texcoord + float2(-px.x, 0.0)).rgb;
	float3 blur = (n + s + e + w) * 0.25;
	return center + (center - blur) * PreSharpenStrength;
}

void PS_PreSharpen(float4 pos : SV_Position, float2 texcoord : TEXCOORD, out float4 outColor : SV_Target)
{
	float3 color = tex2D(samplerVibrancyOut, texcoord).rgb;
	if (PreSharpenEnabled)
		color = ApplySharpen(texcoord);
	outColor = float4(saturate(color), 1.0);
}

// Cheap fixed 5x5 grid downsample of the in-progress image, picked over a
// luminance histogram (the scout's other option,
// superdoc/planning/reshade-shaders.md's Adaptive Brightness design
// section) because this effect only needs an average-brightness exposure
// estimate, not the histogram's outlier resistance -- and a histogram needs
// extra passes/atomics this experiment doesn't need. 25 taps is enough to
// avoid a single stray pixel dominating the read, without a second pass.
//
// ponytail: fixed grid, not a real mip-chain reduction -- texPreSharpenOut
// has no mips to reduce anyway (see the file's other ponytail note on
// working with encoded RGB directly). Upgrade path: a proper log-luminance
// mip chain if the grid ever proves too noisy in practice.
float MeasureLuminance()
{
	float total = 0.0;
	for (int y = 0; y < 5; y++)
	{
		for (int x = 0; x < 5; x++)
		{
			float2 uv = (float2(x, y) + 0.5) / 5.0;
			float3 c = tex2D(samplerPreSharpenOut, uv).rgb;
			total += dot(c, float3(0.299, 0.587, 0.114));
		}
	}
	return total / 25.0;
}

// Reads AND writes texAdaptedLuminance in this one pass (self-sampling --
// see this file's header comment on why that's safe here). Always runs,
// regardless of AdaptiveBrightnessEnabled, so the adapted-luminance history
// keeps tracking scene brightness in the background even while the visible
// gain is off (PS_AdaptiveBrightnessApply below gates the visible effect) --
// re-enabling doesn't have to re-adapt from a stale/frozen value.
void PS_AdaptiveBrightnessAdapt(float4 pos : SV_Position, float2 texcoord : TEXCOORD, out float4 outColor : SV_Target)
{
	float measured = MeasureLuminance();
	float adapted = tex2D(samplerAdaptedLuminance, float2(0.5, 0.5)).r;

	float tau = (measured > adapted) ? AdaptiveBrightnessAdaptUpSpeed : AdaptiveBrightnessAdaptDownSpeed;
	float dt = AdaptiveBrightnessFrametime / 1000.0;
	float alpha = saturate(1.0 - exp(-dt / max(tau, 0.001)));

	outColor = float4(lerp(adapted, measured, alpha), 0.0, 0.0, 1.0);
}

void PS_AdaptiveBrightnessApply(float4 pos : SV_Position, float2 texcoord : TEXCOORD, out float4 outColor : SV_Target)
{
	float3 color = tex2D(samplerPreSharpenOut, texcoord).rgb;
	if (AdaptiveBrightnessEnabled)
	{
		float adapted = tex2D(samplerAdaptedLuminance, float2(0.5, 0.5)).r;
		float gain = clamp(AdaptiveBrightnessTargetLuminance / max(adapted, 0.001), AdaptiveBrightnessMinGain, AdaptiveBrightnessMaxGain);
		color = lerp(color, color * gain, AdaptiveBrightnessStrength);
	}
	outColor = float4(saturate(color), 1.0);
}

technique GamescopeRitz
{
	pass Vibrancy
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Vibrancy;
		RenderTarget = texVibrancyOut;
	}
	pass PreSharpen
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_PreSharpen;
		RenderTarget = texPreSharpenOut;
	}
	pass AdaptiveBrightnessAdapt
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_AdaptiveBrightnessAdapt;
		RenderTarget = texAdaptedLuminance;
	}
	pass AdaptiveBrightnessApply
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_AdaptiveBrightnessApply;
	}
}
