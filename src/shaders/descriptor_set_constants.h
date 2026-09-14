#ifndef DESCRIPTOR_SET_CONSTANTS_H_
#define DESCRIPTOR_SET_CONSTANTS_H_

#define VKR_TARGET_SLOTS 2u
#define VKR_SAMPLER_SLOTS 16u
#define VKR_MAX_LAYERS 6u

#define VKR_BLUR_EXTRA_SLOT       VKR_MAX_LAYERS
#define VKR_NIS_COEF_SCALER_SLOT (VKR_BLUR_EXTRA_SLOT + 1u)
#define VKR_NIS_COEF_USM_SLOT    (VKR_NIS_COEF_SCALER_SLOT + 1u)

// cs_effects_layer0.comp / cs_effects_measure.comp: slot 0 is the base layer;
// slot 1 is Adaptive Brightness's 1x1 adapted-luminance history texel
// (g_output.effectsHistory), which the measure pass reads and rewrites and
// the per-pixel pass reads. Both sides (the shaders via effects_common.h and
// vulkan_composite()'s pre-pass block) name it via this constant so they
// cannot disagree.
#define VKR_EFFECTS_HISTORY_SLOT 1u

// cs_effects_bloom_*.comp / cs_effects_layer0.comp (Bloom, 2026-09-08): the
// eighth-resolution glow buffer. The two blur passes read the buffer they
// are blurring here and write the other one (a ping-pong pair), and the
// per-pixel pass reads the finished one here to add the glow. Named via this
// constant on both sides, exactly like the history slot above, so the
// shaders and vulkan_composite()'s pre-pass block cannot disagree.
#define VKR_EFFECTS_BLOOM_SLOT 2u

// cs_effects_v2_*.comp / cs_effects_layer0.comp (Adaptive Brightness V2,
// 2026-09-14): the quarter-resolution guided-filter coefficient buffer
// (a, b in R, G). Slot 3 was VKR_EFFECTS_BMAP_SLOT (the removed Brightness
// Map effect); reused the same day, same reasoning as effects_common.h's
// EFFECT_ADAPTIVE_V2 bit reusing that effect's flag bit.
#define VKR_EFFECTS_V2_SLOT 3u

// cs_effects_v2_box1_fine.comp / cs_effects_v2_box2_fine.comp /
// cs_effects_layer0.comp (Adaptive Brightness V2's Stage 3 Clarity,
// 2026-09-14): the SECOND, finer guided-filter coefficient pair
// (v2_coef_sample_fine() in effects_common.h). A separate slot rather than
// widening effectsV2A/B to hold both pairs side by side in one texture --
// the plan's own "or a 2x-wide v2A holding both" alternative (section 5.1)
// -- because a second slot needs no per-kernel half-boundary clamping in
// EVERY box-filter tap (down/box1/box2 all loop a radius of taps around
// each texel; a packed-width buffer would have to stop that loop from
// reading across the seam into the other half, in three shader files,
// which is exactly the kind of off-by-one this plan's own guarantees exist
// to catch). One more slot, allocated and bound only while Clarity > 0, is
// the cheaper of the two to get right.
#define VKR_EFFECTS_V2_FINE_SLOT 4u

#define VKR_LUT3D_COUNT 2 // Must match EOTF_Count

#endif
