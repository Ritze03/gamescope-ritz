// effects_bmap_blur.h -- the body of Brightness Map's two blur dispatches,
// shared so the horizontal and the vertical pass cannot drift apart. Each
// .comp file passes its own axis to bmap_blur_main().
//
// `Why two shaders and not one with a direction uniform:` the same reason
// effects_bloom_blur.h gives -- the direction would have to ride in the
// effects_t block, which is uploaded ONCE per frame and read by every
// dispatch of the pre-pass, so making it a uniform would mean extra uploads
// a frame and a field whose value differs between two dispatches documented
// to share one.
//
// `Why separable at all:` a 33x33 two-dimensional kernel is 1089 taps per
// texel; two 33-tap passes are 66. At the map's 1/64 of the frame's pixels
// that is about one tap per source pixel, which is what makes a blur this
// WIDE affordable -- and width is the whole halo control here.
//
// `Why 33 taps (+-16) where Bloom uses 17:` this effect's Radius reaches
// sigma 6 map texels against Bloom's 3 (effects_curve.h's BMAP_SIGMA_MAX),
// because a tone operator needs the option of a much softer map than a glow
// does. +-16 is +-2.67 sigma at that top end -- the same truncation Bloom
// accepts at ITS top end -- so under 1 % of the Gaussian's mass falls
// outside, and the weights are renormalised by their own sum so what is left
// is still a proper average. At the low end (sigma 1) the outer taps weigh
// essentially nothing and the pass costs the same; taps are not the
// expensive part at this resolution.
//
// `Why the map is unpacked and repacked around the blur:` the texels carry a
// 16-bit fixed-point luma over two UNORM8 lanes (effects_curve.h's
// bmap_pack_hi/lo), so blurring the RAW lanes would average the high bytes
// and the low bytes separately and produce noise wherever the high byte
// steps. One dot product per tap.
//
// Edge handling is clamp-to-edge, matching effects_common.h's bmap_sample():
// a neighbourhood at the frame's edge continues off it rather than fading
// toward black, which would put a spurious lift in every corner.

#ifndef EFFECTS_BMAP_BLUR_H_
#define EFFECTS_BMAP_BLUR_H_

const int BMAP_BLUR_TAPS = 16;   // +-16, i.e. 33 taps

void bmap_blur_main(ivec2 dir)
{
    ivec2 p  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = textureSize(s_samplers[VKR_EFFECTS_BMAP_SLOT], 0);
    if (p.x >= sz.x || p.y >= sz.y)
        return;

    float sigma = bmap_sigma(u_bmapRadius);
    float inv2s = 1.0 / (2.0 * sigma * sigma);

    float sum  = 0.0;
    float wsum = 0.0;
    for (int i = -BMAP_BLUR_TAPS; i <= BMAP_BLUR_TAPS; i++)
    {
        float w = exp(-float(i * i) * inv2s);
        sum  += w * bmap_fetch(p + dir * i, sz);
        wsum += w;
    }

    imageStore(dst, p, bmap_pack(sum / wsum));
}

#endif // EFFECTS_BMAP_BLUR_H_
