// gamescope-ritz.fx -- Milestone 6 combined ReShade effect: Vibrancy + Pre-Sharpen.
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
// Extensibility for Adaptive Brightness (M9, DECISIONS.md #14, deferred):
// its passes/uniforms are meant to be appended below the Pre-Sharpen
// section, as one more sequential gated pass sampling texPreSharpenOut and
// writing to a new named render target (or straight to the implicit
// default output, if it's the new last pass) -- nothing above this comment
// needs to change to add it.
//
// SDR only for v1 (DECISIONS.md #15): both passes work directly on the
// base layer's already-encoded 0..1 RGB, with no HDR-aware colour handling.
// PanelShaders.cpp refuses to let the user enable this effect while the
// base layer's colourspace isn't SDR (LINEAR/SRGB), so this file doesn't
// need to branch on BUFFER_COLOR_SPACE today -- see reshade-shaders.md Q6
// for what correct HDR-space math here would require.
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

// ---- Adaptive Brightness (M9, not yet built) -------------------------
// Reserved section only -- see the file-level comment above. Appending its
// gated pass here should not require touching anything above this line.
// ------------------------------------------------------------------------

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
	}
}
