// effects_bmap.h -- Brightness Map's GLSL-only half: the vec4 wrappers
// around effects_curve.h's scalar 16-bit pack/unpack, and the bilinear
// sample of the map. It is a separate header from effects_common.h (which
// holds bloom_sample and the history helpers) for one mechanical reason:
// these call effects_curve.h's scalar functions, and effects_common.h is
// included BEFORE effects_curve.h by every shader in this pre-pass -- the
// uniform block has to be declared before anything can read it. So this is
// the "after both" header.
//
// Include descriptor_set.h, effects_common.h and effects_curve.h first.

#ifndef EFFECTS_BMAP_H_
#define EFFECTS_BMAP_H_

// ---- The brightness map, packed and sampled -------------------------------
//
// The map's texels carry the local mean luma as a 16-BIT fixed-point number
// spread over their R and G lanes -- effects_curve.h's bmap_pack_hi/lo and
// bmap_unpack2 hold the arithmetic (and the argument for why one byte is not
// enough: a 1/255 step of the map is worth about a code of output, i.e. a
// visible contour on a smooth gradient). These two wrap the scalar functions
// in the vec4 the storage image wants.
vec4 bmap_pack( float v )
{
    return vec4( bmap_pack_hi( v ), bmap_pack_lo( v ), 0.0, 1.0 );
}

float bmap_unpack( vec4 t )
{
    return bmap_unpack2( t.r, t.g );
}

// The map's LIVE size this frame -- ceil(base layer / u_bmapDown), which is
// what the down pass wrote. NOT textureSize() of the map slot: the pair is
// allocated once at the finest reduction (effects_common.h's note) and a
// coarser frame fills only its top-left corner, so textureSize() would
// report a rectangle three quarters of which is last frame's leftovers.
// Every pass that touches the map asks this, so they cannot disagree.
ivec2 bmap_map_size()
{
    ivec2 src = textureSize( s_samplers[0], 0 );
    int   d   = max( int( u_bmapDown ), 1 );
    return max( ivec2( 1 ), ( src + ivec2( d - 1 ) ) / d );
}

float bmap_fetch( ivec2 t, ivec2 sz )
{
    return bmap_unpack( texelFetch( s_samplers[VKR_EFFECTS_BMAP_SLOT],
                                    clamp( t, ivec2( 0 ), sz - ivec2( 1 ) ), 0 ) );
}

// Bilinear BY HAND over texelFetch, on the UNPACKED floats -- the same
// reason ab_local_sample() and bloom_sample() are: the slot is bound with
// the unnormalised nearest sampler slot 0 needs, and here hardware filtering
// would interpolate the packed BYTES and yield noise rather than a map.
// Four fetches of a small, cache-resident texture plus three mixes, and only
// on the frames this effect is on.
//
// Clamp-to-edge at the border: the half-texel outside the outermost texel
// centres reads that texel, so a neighbourhood at the frame's edge is
// treated as continuing off it -- which is what "how bright is it around
// here" should say in the corner a HUD or a letterbox occupies.
//
// `pos` is in SOURCE pixels (the base layer's own grid).
float bmap_sample( vec2 pos )
{
    ivec2 sz = bmap_map_size();
    vec2  g  = pos / max( u_bmapDown, 1.0 ) - 0.5;
    ivec2 i0 = ivec2( floor( g ) );
    vec2  f  = g - vec2( i0 );
    float a = bmap_fetch( i0,                  sz );
    float b = bmap_fetch( i0 + ivec2( 1, 0 ),  sz );
    float c = bmap_fetch( i0 + ivec2( 0, 1 ),  sz );
    float d = bmap_fetch( i0 + ivec2( 1, 1 ),  sz );
    return mix( mix( a, b, f.x ), mix( c, d, f.x ), f.y );
}

#endif // EFFECTS_BMAP_H_
