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
// `Why separable at all:` a two-dimensional kernel of the same reach is the
// square of the taps a separable pair costs -- 41x41 = 1681 against 2 x 41.
// At the map's small fraction of the frame's pixels that is what makes a
// blur this WIDE affordable, and width is the whole halo control here.
//
// `The kernel, since 2026-09-10 -- +-ceil(3 sigma) taps, not a fixed 33:`
// the Radius now covers sigma 4..96 SOURCE pixels, a 24:1 range, and no
// fixed tap count covers that. It does not have to: effects_curve.h's
// bmap_down() picks the map's reduction so that the blur is always between
// 1 and 7 TEXELS wide whatever the Radius, and +-3 sigma of that is at most
// +-21 taps. So the kernel here is never truncated at more than 0.3 % of
// its mass -- where a fixed 33 taps would have been +-0.7 sigma at the top
// of the new slider, which is a box with a Gaussian's name on it, and
// visibly so. At the fine end the loop is +-3 taps and this pass costs less
// than it used to. effects_curve.h's "THE PYRAMID" note has the argument
// for solving it this way rather than by widening the kernel or by striding
// it.
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

void bmap_blur_main(ivec2 dir)
{
    ivec2 p  = ivec2(gl_GlobalInvocationID.xy);
    // The LIVE map size, not the allocated one -- the pair is allocated at
    // the finest reduction and a coarser frame uses only its corner.
    ivec2 sz = bmap_map_size();
    if (p.x >= sz.x || p.y >= sz.y)
        return;

    // Sigma is authored in SOURCE pixels; the pass runs in map texels.
    float sigma = bmap_sigma(u_bmapRadius) / max(u_bmapDown, 1.0);
    float inv2s = 1.0 / (2.0 * sigma * sigma);
    int   taps  = int(bmap_taps(sigma));

    float sum  = 0.0;
    float wsum = 0.0;
    for (int i = -taps; i <= taps; i++)
    {
        float w = exp(-float(i * i) * inv2s);
        sum  += w * bmap_fetch(p + dir * i, sz);
        wsum += w;
    }

    imageStore(dst, p, bmap_pack(sum / wsum));
}

#endif // EFFECTS_BMAP_BLUR_H_
