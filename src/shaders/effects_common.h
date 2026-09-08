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
    uint  u_flags;         // EFFECT_* bits below
    float u_saturation;    // 0.0..3.0, 1.0 neutral -- renamed from u_vibrancy 2026-09-08
    float u_vibrancy;      // 0.0..2.0, 0.0 neutral -- NEW 2026-09-08, see grade() below
    float u_shadowLift;    // 0.0..1.0, 0.0 neutral
    uint  u_rcasCon;       // floatBitsToUint(con.x) for FsrRcasF; 0 = no sharpen

    // ---- Adaptive Brightness ----
    float u_abTarget;     // target luminance, 0.1..0.9
    float u_abUp;         // brighten time constant, seconds
    float u_abDown;       // darken time constant, seconds
    float u_abMin;        // min gain, 0.3..1.0
    float u_abMax;        // max gain, 1.0..4.0
    float u_abStrength;   // dry/wet mix, 0.0..1.0
    float u_abDt;         // seconds since the previous effects dispatch, host-clamped
    float u_abLocal;      // Local adaptation, 0.0..1.0 (Dynamic only; 0 = the global curve)

    // ---- Adaptive Gamma (NEW 2026-09-08) ----
    // The same statistics, one exponent, no gain and no shoulder -- see
    // effects_curve.h's ADAPTIVE GAMMA block for the whole operator and why
    // its two bounds are user-facing. The host masks EVERY field here to
    // its neutral value when Adaptive Brightness is also on (the two aim
    // the same mid-tones at the same target, so composing them would
    // correct the picture twice); see EffectsPushData_t.
    float u_agTarget;     // where the smoothed median is put, 0.1..0.9
    float u_agMaxLift;    // 1.0..4.0; the exponent floor is 1/this
    float u_agMaxDarken;  // 1.0..4.0; the exponent ceiling IS this
    float u_agStrength;   // dry/wet mix, 0.0..1.0
    float u_agLocal;      // Local adaptation, 0.0..1.0 -- the SAME operator
};

// ROW 0 of the history texture is HISTORY_COUNT texels, one smoothed
// statistic per texel (each a float packed into RGBA8 -- see history_pack()
// below). The rows under it are the local luminance map, see AB_LOCAL_* .
// Whole-image mode reads only HISTORY_MEAN; Dynamic mode reads the three
// percentiles. All four are measured and smoothed every frame the pre-pass
// runs, whatever the mode, so a mode switch needs no re-convergence.
const int HISTORY_MEAN  = 0;   // arithmetic mean of the graded encoded luma
const int HISTORY_P2    = 1;   // 2nd percentile  (shadows)
const int HISTORY_P50   = 2;   // median          (the Dynamic curve's anchor)
const int HISTORY_P98   = 3;   // 98th percentile (highlights)
// Texels 4..7 hold this frame's RAW (unsmoothed) measurement of the same
// four statistics, in the same order. No pass reads them; they exist so the
// host's `effects_ab_log` readback (rendervulkan.cpp) can print measured
// next to smoothed and tell sampling noise from adaptation dynamics.
const int HISTORY_RAW   = 4;
const int HISTORY_COUNT = 8;

// ---- The local luminance map (Local adaptation, 2026-09-07) ----
//
// Row 0 of the history texture is the eight statistics above. Rows
// AB_LOCAL_ROW .. AB_LOCAL_ROW + AB_LOCAL_GRID - 1 are an
// AB_LOCAL_GRID x AB_LOCAL_GRID grid of SMOOTHED local mean luminances --
// one texel per cell, packed the same way, so the whole thing is still one
// texture, one storage target and one sampler slot. Cell (cx, cy) lives at
// texel (cx, AB_LOCAL_ROW + cy) and covers image rect
// [cx/GRID, (cx+1)/GRID) x [cy/GRID, (cy+1)/GRID).
//
// `Why 16x16, and why exactly that:` the measure pass is 16x16 threads and
// thread (tx, ty) already owns the contiguous 8x8 block of the 128x128 tap
// grid covering exactly that image cell, so its per-thread tap sum -- which
// it computes anyway, for the mean's tree reduction -- IS the cell's mean.
// The map therefore costs ZERO extra taps and zero extra fetches in the
// measure pass. A finer grid (the 32x18 first sketched) would need its own
// tap layout and would make the operator's radius smaller, which is the
// wrong direction: coarse is what keeps halos away (shader-effects.md).
//
// `Why arithmetic mean of the ENCODED luma, not a log mean:` the whole
// pre-pass works on gamma-encoded code values on purpose (see
// cs_effects_layer0.comp's header), and an encoded value is already
// perceptually spaced -- taking a log of it would apply a second
// perceptual curve. The global HISTORY_MEAN is the arithmetic mean of the
// same quantity, so local/global is a pure ratio of like for like.
const int AB_LOCAL_GRID = 16;
const int AB_LOCAL_ROW  = 1;
// The whole history texture. Mirrored by kEffectsHistoryWidth/Height in
// rendervulkan.cpp -- keep the two in step.
const int HISTORY_TEX_W = 16;   // max(HISTORY_COUNT, AB_LOCAL_GRID)
const int HISTORY_TEX_H = AB_LOCAL_ROW + AB_LOCAL_GRID;

// ---- The Inspector's before/after preview capture (2026-09-07) ----
//
// cs_effects_preview.comp writes one downscaled, graded copy of the base
// layer at this size; the host copies it to host-mappable staging and the
// settings Inspector re-runs effects_curve.h over it on the CPU (see
// src/Overlay/EffectPreview.cpp). Mirrored by kAbPreviewWidth/Height in
// rendervulkan.hpp -- keep the three in step. `Why 256x144:` 16:9, the shape
// the strip is drawn at, and 36864 pixels -- small enough that a full CPU
// re-apply of the curve is well under a millisecond, large enough that the
// strip is not visibly blocky at the widths the Inspector uses (it is drawn
// at 200..320 logical px wide, i.e. always downscaled again).
const int AB_PREVIEW_W = 256;
const int AB_PREVIEW_H = 144;

// Bit assignments are the contract with EffectsPushData_t's constructor.
const uint EFFECT_SHADOW_LIFT         = 1u << 0;
// Renamed from EFFECT_VIBRANCY/EFFECT_VIBRANCY_SKIN 2026-09-08 -- see
// grade() below. Same bit values, maths unchanged.
const uint EFFECT_SATURATION          = 1u << 1;
const uint EFFECT_SATURATION_SKIN     = 1u << 2;
const uint EFFECT_PRE_SHARPEN         = 1u << 3;
const uint EFFECT_ADAPTIVE_BRIGHTNESS = 1u << 4;
// Adaptive Brightness's Dynamic mode (effects_curve.h's percentile tone
// curve) instead of the Whole-image gain. Only the apply pass reads it; the
// measure pass tracks every statistic regardless of mode, so switching
// modes is instant.
const uint EFFECT_AB_DYNAMIC          = 1u << 5;
// NEW 2026-09-08: the "punchy colours punchier" effect -- see grade() below.
const uint EFFECT_VIBRANCY            = 1u << 6;
// NEW 2026-09-08: Adaptive Gamma -- one adaptive exponent, no levels gain
// and no shoulder (effects_curve.h's ag_* block). Applied by
// cs_effects_layer0.comp after Adaptive Brightness; the host never sets
// both bits at once, so the order between them is a formality.
const uint EFFECT_ADAPTIVE_GAMMA      = 1u << 7;
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

// Per-tap colour grade: steps 1-3 of the effect order (Pre-Sharpen and
// Adaptive Brightness are not part of grade() -- see cs_effects_layer0.comp).
// All are pure per-pixel operations on one sample, so the apply pass runs
// them inside RCAS's load (every one of its 5 taps is graded) and the
// measure pass runs them on each of its taps -- a separate graded
// intermediate would cost a texture round trip and change nothing.
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

    // 2. Saturation (renamed from "Vibrancy" 2026-09-08 -- see
    //    superdoc/features/shader-effects.md's "Saturation / Vibrancy
    //    split"; maths UNCHANGED by the rename). `m` alone walks 0.0
    //    (grey) .. 1.0 (unchanged) uniformly, the same relative boost for
    //    every pixel regardless of how saturated it already is -- exactly
    //    what an iPhone "Saturation" slider does; `boost` carries the
    //    adaptive/skin-tone shape only above neutral.
    if ((u_flags & EFFECT_SATURATION) != 0u)
    {
        float luma = effects_luma(c);
        float sat  = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float skin = 0.0;
        if ((u_flags & EFFECT_SATURATION_SKIN) != 0u)
        {
            // Warm hues where red clearly leads blue and green sits
            // roughly between them -- a cheap, deliberately approximate
            // skin-tone mask.
            skin = clamp((c.r - c.b) * 3.0, 0.0, 1.0)
                 * clamp(1.0 - abs(c.g - (c.r + c.b) * 0.5) * 4.0, 0.0, 1.0);
        }
        float prot  = mix(1.0, 0.3, skin);
        float m     = min(u_saturation, 1.0);
        float boost = max(u_saturation - 1.0, 0.0) * (1.0 - sat) * prot;
        c = clamp(mix(vec3(luma), c, m + boost), 0.0, 1.0);
    }

    // 3. Vibrancy (NEW 2026-09-08): boosts a pixel's saturation IN
    //    PROPORTION to how saturated it already is -- the more `sat` (the
    //    pixel's own chroma), the larger the gain applied to it, so already-
    //    punchy colours get pushed further and near-neutral colours (small
    //    sat) are left close to untouched. A pure grey pixel has c == luma
    //    exactly, so it is an exact no-op there at ANY strength -- no
    //    epsilon needed. `Why this is the INVERSE of Apple Photos'
    //    "Vibrance":` that control does the opposite (protects saturated
    //    colours, boosts muted ones -- the shape step 2 above already had).
    //    This is the user's own, deliberate definition; see the doc.
    //      gain = 1.0 + strength * sat        // sat: 0..1, >= 1.0 always
    //      out  = luma + (c - luma) * gain
    //    Monotonic in strength and in sat; never wraps hue (c - luma keeps
    //    its sign per channel, only its magnitude scales); the final clamp
    //    is the only clipping, exactly like every other step here.
    if ((u_flags & EFFECT_VIBRANCY) != 0u)
    {
        float luma = effects_luma(c);
        float sat  = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float gain = 1.0 + u_vibrancy * sat;
        c = clamp(luma + (c - luma) * gain, 0.0, 1.0);
    }

    return c;
}

// ---- Adaptive Brightness history: one float per RGBA8 texel ----
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

float history_read_at(ivec2 texel)
{
    return history_unpack(texelFetch(s_samplers[VKR_EFFECTS_HISTORY_SLOT], texel, 0));
}

float history_read(int which)
{
    return history_read_at(ivec2(which, 0));
}

// The local map, sampled BILINEARLY at normalised image position uv.
// Bilinear by hand, on the UNPACKED floats: the texels are float bits
// spread over four UNORM8 lanes (history_pack above), so hardware
// filtering would interpolate the bytes and yield noise. Four fetches of a
// 16x17 texture that is fully resident in cache, plus three mixes.
//
// Clamp-to-edge at the border: the half-cell outside the outermost cell
// centres reads that cell, which is what a "local mean around here" should
// do at the image edge (an extrapolation there would push the gain the
// wrong way in exactly the corner a HUD or a letterbox occupies).
float ab_local_sample(vec2 uv)
{
    vec2 g = clamp(clamp(uv, 0.0, 1.0) * float(AB_LOCAL_GRID) - 0.5,
                   0.0, float(AB_LOCAL_GRID - 1));
    ivec2 i0 = ivec2(floor(g));
    ivec2 i1 = min(i0 + ivec2(1), ivec2(AB_LOCAL_GRID - 1));
    vec2 f = g - vec2(i0);
    float a = history_read_at(ivec2(i0.x, AB_LOCAL_ROW + i0.y));
    float b = history_read_at(ivec2(i1.x, AB_LOCAL_ROW + i0.y));
    float c = history_read_at(ivec2(i0.x, AB_LOCAL_ROW + i1.y));
    float d = history_read_at(ivec2(i1.x, AB_LOCAL_ROW + i1.y));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

#endif // EFFECTS_COMMON_H_
