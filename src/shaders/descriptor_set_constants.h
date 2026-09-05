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

#define VKR_LUT3D_COUNT 2 // Must match EOTF_Count

#endif
