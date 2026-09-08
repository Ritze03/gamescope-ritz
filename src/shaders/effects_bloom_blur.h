// effects_bloom_blur.h -- the body of Bloom's two blur dispatches, shared so
// the horizontal and the vertical pass cannot drift apart. Each .comp file
// #defines BLOOM_BLUR_DX / BLOOM_BLUR_DY and includes this.
//
// `Why two shaders and not one with a direction uniform:` the direction
// would have to ride in the effects_t block, which is uploaded ONCE per
// frame and read by every dispatch of the pre-pass (see rendervulkan.cpp's
// "one upload for both dispatches" comment). Making it a uniform would mean
// three uploads a frame and a uniform whose value differs between two
// dispatches that are documented to share one -- for the sake of not
// duplicating eleven lines. Two tiny files sharing this header is what
// cs_composite_blur.comp / cs_gaussian_blur_horizontal.comp already do.
//
// `Why separable at all:` a 17x17 two-dimensional kernel is 289 taps per
// texel; two 17-tap passes are 34. At the glow buffer's 1/64 of the frame's
// pixels that is about half a tap per source pixel, which is what makes a
// wide, genuinely Gaussian falloff affordable here.
//
// `Why 17 taps (+-8) and a run-time sigma rather than a fixed kernel:` the
// Radius slider has to actually change the spread, and BLOOM_SIGMA_MAX is 3
// glow texels, so +-8 is +-2.7 sigma -- under 1 % of the Gaussian's mass
// falls outside it, and the weights are renormalised by their own sum so
// what is left is still a proper average. At the low end (sigma 1) the outer
// taps weigh essentially nothing and the pass costs the same; taps are not
// the expensive part at this resolution.
//
// Edge handling is clamp-to-edge, matching effects_common.h's bloom_sample():
// a glow at the frame's edge continues off it rather than fading, which is
// what a light half out of shot does.

#ifndef EFFECTS_BLOOM_BLUR_H_
#define EFFECTS_BLOOM_BLUR_H_

const int BLOOM_BLUR_TAPS = 8;   // +-8, i.e. 17 taps

void bloom_blur_main(ivec2 dir)
{
    ivec2 p  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = textureSize(s_samplers[VKR_EFFECTS_BLOOM_SLOT], 0);
    if (p.x >= sz.x || p.y >= sz.y)
        return;

    float sigma = bloom_sigma(u_bloomRadius);
    float inv2s = 1.0 / (2.0 * sigma * sigma);

    vec3  sum  = vec3(0.0);
    float wsum = 0.0;
    for (int i = -BLOOM_BLUR_TAPS; i <= BLOOM_BLUR_TAPS; i++)
    {
        float w = exp(-float(i * i) * inv2s);
        ivec2 t = clamp(p + dir * i, ivec2(0), sz - ivec2(1));
        sum  += w * texelFetch(s_samplers[VKR_EFFECTS_BLOOM_SLOT], t, 0).rgb;
        wsum += w;
    }

    imageStore(dst, p, vec4(sum / wsum, 1.0));
}

#endif // EFFECTS_BLOOM_BLUR_H_
