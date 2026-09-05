// effects_common.h -- everything cs_effects_layer0.comp (the per-pixel pass)
// and cs_effects_measure.comp (Adaptive Brightness's one-workgroup
// measure/adapt pass) must agree on: the uniform block, the flag bits, the
// per-tap colour grade, and the adapted-luminance history encoding. One
// definition so the two dispatches can share one uploadConstants() and so
// the measure pass grades its taps with exactly the code the apply pass
// grades its pixels with (the retired .fx measured its PreSharpenOut
// texture, i.e. the graded image -- see MeasureLuminance() there).
//
// Include descriptor_set.h first (dst, s_samplers).

#ifndef EFFECTS_COMMON_H_
#define EFFECTS_COMMON_H_

// Mirrors EffectsPushData_t in src/rendervulkan.cpp field-for-field -- keep
// the two in step.
layout(binding = 0, scalar)
uniform effects_t {
    uint  u_flags;        // EFFECT_* bits below
    float u_vibrancy;     // 0.0..3.0, 1.0 neutral
    float u_shadowLift;   // 0.0..1.0, 0.0 neutral
    uint  u_rcasCon;      // floatBitsToUint(con.x) for FsrRcasF; 0 = no sharpen

    // ---- Adaptive Brightness ----
    float u_abTarget;     // target luminance, 0.1..0.9
    float u_abUp;         // brighten time constant, seconds
    float u_abDown;       // darken time constant, seconds
    float u_abMin;        // min gain, 0.5..1.0
    float u_abMax;        // max gain, 1.0..2.0
    float u_abStrength;   // dry/wet mix, 0.0..1.0
    float u_abDt;         // seconds since the previous effects dispatch, host-clamped
};

// Bit assignments are the contract with EffectsPushData_t's constructor.
const uint EFFECT_SHADOW_LIFT         = 1u << 0;
const uint EFFECT_VIBRANCY            = 1u << 1;
const uint EFFECT_VIBRANCY_SKIN       = 1u << 2;
const uint EFFECT_PRE_SHARPEN         = 1u << 3;
const uint EFFECT_ADAPTIVE_BRIGHTNESS = 1u << 4;
// The history texture was (re)created this frame and holds nothing: the
// measure pass writes `measured` straight in instead of blending with it.
const uint EFFECT_RESET_HISTORY       = 1u << 31;

// Rec.601 luma on ENCODED values -- the .fx's MeasureLuminance() weights,
// applied in the same space it applied them (the file header of
// cs_effects_layer0.comp explains why this pass works on encoded values).
float effects_luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// Per-tap colour grade: steps 1 and 2 of the effect order. Both are pure
// per-pixel operations on one sample, so the apply pass runs them inside
// RCAS's load (every one of its 5 taps is graded) and the measure pass runs
// them on each of its taps -- a separate graded intermediate would cost a
// texture round trip and change nothing.
vec3 grade(vec3 c)
{
    // 1. Shadow Control: a gamma curve on the low end, exponent 1.0
    //    (identity) down to 0.5 (sqrt) at full strength. 0 and 1 are fixed
    //    points of any power curve, so black and white never move.
    if ((u_flags & EFFECT_SHADOW_LIFT) != 0u)
    {
        float e = 1.0 - 0.5 * clamp(u_shadowLift, 0.0, 1.0);
        c = pow(clamp(c, 0.0, 1.0), vec3(e));
    }

    // 2. Vibrancy: `m` alone walks 0.0 (grey) .. 1.0 (unchanged) uniformly;
    //    `boost` carries the adaptive/skin-tone shape only above neutral.
    if ((u_flags & EFFECT_VIBRANCY) != 0u)
    {
        float luma = effects_luma(c);
        float sat  = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float skin = 0.0;
        if ((u_flags & EFFECT_VIBRANCY_SKIN) != 0u)
        {
            // Warm hues where red clearly leads blue and green sits
            // roughly between them -- a cheap, deliberately approximate
            // skin-tone mask.
            skin = clamp((c.r - c.b) * 3.0, 0.0, 1.0)
                 * clamp(1.0 - abs(c.g - (c.r + c.b) * 0.5) * 4.0, 0.0, 1.0);
        }
        float prot  = mix(1.0, 0.3, skin);
        float m     = min(u_vibrancy, 1.0);
        float boost = max(u_vibrancy - 1.0, 0.0) * (1.0 - sat) * prot;
        c = clamp(mix(vec3(luma), c, m + boost), 0.0, 1.0);
    }

    return c;
}

// ---- Adaptive Brightness history: one float in a 1x1 RGBA8 texel ----
//
// The history is stored in the same `dst` binding every pass writes
// (descriptor_set.h declares it rgba8, and CVulkanCmdBuffer::dispatch()
// only knows how to bind one RGB target there), so the adapted luminance --
// a float that needs far more than 8 bits, or a slow EMA step would
// quantise to zero and the value would freeze short of its target -- is
// spread bit-for-bit over the four 8-bit channels. Both directions are
// exact: unpackUnorm4x8 yields b/255, UNORM8 storage rounds b/255*255 back
// to b (the conversion is round-to-nearest by spec), the fetch returns
// b/255 again and packUnorm4x8 rounds it back to b. Zero plumbing in the
// shared descriptor set, versus adding an r32f second-target path that only
// this one pass would use. Read and written through the same raw UNORM
// view, so the format's channel order cancels out.
vec4 history_pack(float v)
{
    return unpackUnorm4x8(floatBitsToUint(v));
}

float history_unpack(vec4 t)
{
    return uintBitsToFloat(packUnorm4x8(t));
}

float history_read()
{
    return history_unpack(texelFetch(s_samplers[VKR_EFFECTS_HISTORY_SLOT], ivec2(0), 0));
}

#endif // EFFECTS_COMMON_H_
