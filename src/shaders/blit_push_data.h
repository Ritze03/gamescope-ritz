layout(binding = 0, scalar)
uniform layers_t {
    vec2 u_scale[VKR_MAX_LAYERS];
    vec2 u_offset[VKR_MAX_LAYERS];
    float u_opacity[VKR_MAX_LAYERS];
    mat3x4 u_ctm[VKR_MAX_LAYERS];
    uint u_borderMask;
    uint u_frameId;
    uint u_blur_radius;

    uint u_shaderFilter;
    uint u_alphaMode;

    // hdr
    float u_linearToNits; // sdr -> hdr
    float u_nitsToLinear; // hdr -> sdr
    float u_itmSdrNits;
    float u_itmTargetNits;

    uint u_rotation;

    // The FPS HUD's single-sample Inverted colour mode (2026-09-27,
    // superdoc/features/fps-display.md): the point, in this same output
    // pixel space, to sample layer 0 at once and invert -- see
    // alphamode.h's alpha_mode_invert branch. u_hasInvertSample is 0 on
    // every frame that doesn't have an Inverted-mode HUD layer, in which
    // case u_invertSamplePos is unused.
    vec2 u_invertSamplePos;
    uint u_hasInvertSample;
};

