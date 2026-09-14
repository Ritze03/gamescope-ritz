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
    // THE ACTIVE ADAPTIVE EFFECT'S EMA time constants, not necessarily
    // Adaptive Brightness's -- renamed from u_abUp/u_abDown 2026-09-09,
    // when Adaptive Gamma got its own pair. The two effects are mutually
    // exclusive, so the host (EffectsPushData_t) picks whichever one is
    // running and the measure pass keeps ONE EMA. They stay in this block
    // because dt and the statistics they smooth do too.
    float u_adaptUp;      // brighten time constant, seconds
    float u_adaptDown;    // darken time constant, seconds
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

    // ---- Dark floor (NEW 2026-09-14) ----
    // SHARED between Adaptive Brightness and Adaptive Gamma (one number, one
    // panel row -- see ConfigSchema.h's ReshadeSettings::dark_floor for why
    // it is not two fields). 0.0..1.0, 0 = off (today's pre-2026-09-14
    // behaviour, byte-identical). effects_curve.h's dark_weight() turns this
    // and the smoothed median into a 0..1 blend weight that fades whichever
    // effect is running toward the identity on a scene far darker than the
    // weight's own half-point -- see that header's DARK FLOOR block for the
    // formula and every "why".
    float u_darkFloor;

    // ---- Bloom (NEW 2026-09-08) ----
    // The only SPATIAL effect in this pass: three extra dispatches build an
    // eighth-resolution glow buffer (cs_effects_bloom_down.comp, then
    // effects_bloom_blur.h twice), and cs_effects_layer0.comp screens it
    // back onto the picture. The scalar half of the maths -- the bright
    // pass's gate, the Radius -> sigma mapping and the screen composite --
    // is in effects_curve.h's BLOOM block, so the unit tests assert it on
    // the same text. Every field is masked to its neutral value by the host
    // when the effect is off, for the same "the uniform says what the frame
    // did" reason as u_abLocal.
    float u_bloomThreshold;   // 0.0..1.0, where a pixel starts to glow
    float u_bloomIntensity;   // 0.0..2.0, how bright the glow is; 0 = off
    float u_bloomRadius;      // 0.0..1.0 -> effects_curve.h's bloom_sigma()

    // ---- Adaptive Brightness V2 (NEW 2026-09-14) ----
    // A NEW, ADDITIVE effect -- the user, verbatim: "Call it 'Adaptive
    // brightness V2' in the GUI. Implement it fully ... DO NOT REMOVE THE
    // ORIGINAL!" -- so this sits alongside u_ab*/u_ag* above, untouched, as
    // a third, mutually-exclusive choice. See effects_curve.h's own
    // "ADAPTIVE BRIGHTNESS V2" block for the whole operator and
    // superdoc/planning/adaptive-brightness-v2-plan.md for the design.
    float u_v2Lift;        // 0.0..1.0 -> effects_curve.h's abv2_g_static()
    float u_v2Target;      // 0.1..0.9, the content anchor's target
    float u_v2MaxLift;     // 1.0..8.0, the toe/knee's slope cap S
    float u_v2Detail;      // 0.0..2.0, scales the Weber-preserving secant
    float u_v2AdaptUp;     // seconds, this row's OWN adapt speed pair
    float u_v2AdaptDown;   // == u_v2AdaptUp * 2 (plan 4.8's fixed 1:2 ratio)
    float u_v2Radius;      // guided-filter box radius, in v2-buffer TEXELS
    uint  u_v2Mode;        // 0 = Off (static only), 1 = Scene (adapts)
    uint  u_v2Knee;        // 0 = toe shape, 1 = knee shape
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

// ---- Adaptive Brightness V2's own row-0 texels (NEW 2026-09-14) ----------
//
// Texels 8..11, still row 0, still within HISTORY_TEX_W (16) -- see this
// effect's own block in effects_curve.h and cs_effects_measure.comp's
// header for what each one is measured from.
const int HISTORY_V2_ANCHOR     = 8;    // smoothed content-median anchor (EMA'd, scene-cut snapped)
const int HISTORY_V2_ANCHOR_RAW = 9;    // this frame's raw anchor, before the EMA/snap -- debug only
const int HISTORY_V2_VOID       = 10;   // 1.0 if under 1% of taps were above the black floor
const int HISTORY_V2_CUT        = 11;   // 1.0 if a scene cut snapped the anchor THIS frame

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

// Adaptive Brightness V2's scene-cut histogram, ONE new row under the local
// map (NEW 2026-09-14). 16 bins -- coarser than the measure pass's own
// 64-bin internal histogram, deliberately: this row only has to answer
// "did at least 35% of the frame's mass move to a different bin since last
// frame", and grouping the existing 64 bins four-at-a-time costs nothing
// extra and needs no width change to the shared history texture (the
// plan's own sketch widens it to 64 columns for a bin-for-bin copy; this
// reuses the existing 16-wide layout instead -- a deliberate, documented
// simplification, not a shortfall the plan's 35% threshold itself cares
// about the difference between 16 and 64 buckets).
const int V2_HIST_PREV_ROW = AB_LOCAL_ROW + AB_LOCAL_GRID;   // == 17
const int V2_HIST_BINS     = 16;

// The whole history texture. Mirrored by kEffectsHistoryWidth/Height in
// rendervulkan.cpp -- keep the two in step.
const int HISTORY_TEX_W = 16;   // max(HISTORY_COUNT, AB_LOCAL_GRID, V2_HIST_BINS)
// +1 (NEW 2026-09-14): V2_HIST_PREV_ROW, the scene-cut histogram's own row.
const int HISTORY_TEX_H = V2_HIST_PREV_ROW + 1;

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

// ---- Bloom's glow buffer (2026-09-08) -------------------------------------
//
// The glow is built at 1 / BLOOM_DOWN of the base layer's size in each axis
// and sampled back bilinearly. Mirrored by kEffectsBloomDown in
// rendervulkan.cpp, which sizes the two textures -- keep the two in step.
//
// `Why 8 and not 2 or 4:` the blur is what costs, and it costs per glow
// texel, so every doubling of this number takes three quarters off both blur
// passes. 8 is the largest reduction that still leaves the falloff smooth:
// the buffer's own texels are an 8-pixel box, the smallest sigma the slider
// can ask for is one texel (effects_curve.h's BLOOM_SIGMA_MIN), and the
// bilinear upsample then interpolates between values that already differ by
// less than the eye resolves. A glow has no high frequencies to lose -- that
// is what makes it the one effect here that can be computed small.
//
// `Why the bright pass still reads EVERY source pixel` (an 8x8 box, i.e.
// one fetch per source pixel, rather than a few taps inside each block):
// skipping pixels would make a small bright source appear and disappear as
// the sampling grid slid over it under camera motion -- the same shimmer the
// smoothstep gate exists to avoid, reintroduced at a different scale. The
// box mean is also exactly the right prefilter for a reduction this large.
const int BLOOM_DOWN = 8;

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
// NEW 2026-09-08: Bloom -- a glow around bright areas. The only effect here
// that reads more than one pixel; the bright pass and the two blur passes
// are separate dispatches and this bit gates only the final composite in
// cs_effects_layer0.comp (the host simply does not record the three
// dispatches when it is clear).
const uint EFFECT_BLOOM               = 1u << 8;
// Bit 1u << 9 was EFFECT_BRIGHTNESS_MAP (removed 2026-09-14, see
// superdoc/features/shader-effects.md's History note) -- REUSED the same
// day for Adaptive Brightness V2 (also 2026-09-14), a NEW, ADDITIVE effect
// (the user: "DO NOT REMOVE THE ORIGINAL" -- Adaptive Brightness and
// Adaptive Gamma above are untouched). Mutually exclusive with BOTH of
// them -- see PanelShaders.cpp's three-way exclusion.
const uint EFFECT_ADAPTIVE_V2         = 1u << 9;
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

// ---- Bloom's glow buffer, sampled ----------------------------------------
//
// Bilinear BY HAND over texelFetch, for the same reason ab_local_sample()
// above is: the slot is bound with the unnormalised nearest sampler the
// other two dispatches need for slot 0, so there is no filtering hardware to
// ask. Four fetches of a tiny, fully cache-resident texture plus three
// mixes, and only on the frames Bloom is on.
//
// Clamp-to-edge at the border: a glow at the edge of the picture continues
// off it rather than fading, which is what a light half out of frame does.
vec3 bloom_fetch(ivec2 t, ivec2 sz)
{
    return texelFetch(s_samplers[VKR_EFFECTS_BLOOM_SLOT],
                      clamp(t, ivec2(0), sz - ivec2(1)), 0).rgb;
}

// `pos` is in SOURCE pixels (the base layer's own grid).
vec3 bloom_sample(vec2 pos)
{
    ivec2 sz = textureSize(s_samplers[VKR_EFFECTS_BLOOM_SLOT], 0);
    vec2  g  = pos / float(BLOOM_DOWN) - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2  f  = g - vec2(i0);
    vec3 a = bloom_fetch(i0,                  sz);
    vec3 b = bloom_fetch(i0 + ivec2(1, 0),    sz);
    vec3 c = bloom_fetch(i0 + ivec2(0, 1),    sz);
    vec3 d = bloom_fetch(i0 + ivec2(1, 1),    sz);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// ---- Adaptive Brightness V2's guided-filter coefficients (Stage 2) ------
//
// cs_effects_v2_box2.comp writes the smoothed guided-filter pair (a, b) --
// B(p) = a * Y(p) + b, effects_curve.h's own header and
// superdoc/planning/adaptive-brightness-v2-plan.md section 4.5 -- into the
// R and G lanes of a QUARTER-resolution buffer (V2_DOWN == 4, plain 8-bit
// UNORM; unlike Bloom's/the retired Brightness Map's PACKED 16-bit floats,
// a and b are themselves already fractions in [0, 1] with no float-bits
// round-trip to protect, so a plain byte each is enough precision for an
// interpolation coefficient -- see the plan's own note on why the packing
// bmap used does NOT need reviving here).
vec2 v2_coef_fetch(ivec2 t, ivec2 sz)
{
    return texelFetch(s_samplers[VKR_EFFECTS_V2_SLOT],
                      clamp(t, ivec2(0), sz - ivec2(1)), 0).rg;
}

// `pos` is in SOURCE pixels. Bilinear by hand, same reason bloom_sample()
// is: the slot is bound with the unnormalised nearest sampler slot 0 needs.
vec2 v2_coef_sample(vec2 pos)
{
    ivec2 sz = textureSize(s_samplers[VKR_EFFECTS_V2_SLOT], 0);
    vec2  g  = pos / 4.0 - 0.5;   // == V2_DOWN
    ivec2 i0 = ivec2(floor(g));
    vec2  f  = g - vec2(i0);
    vec2 a = v2_coef_fetch(i0,                 sz);
    vec2 b = v2_coef_fetch(i0 + ivec2(1, 0),   sz);
    vec2 c = v2_coef_fetch(i0 + ivec2(0, 1),   sz);
    vec2 d = v2_coef_fetch(i0 + ivec2(1, 1),   sz);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

#endif // EFFECTS_COMMON_H_
