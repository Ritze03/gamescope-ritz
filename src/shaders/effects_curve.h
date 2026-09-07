// effects_curve.h -- Adaptive Brightness's DYNAMIC-mode tone curve, as pure
// scalar functions compiled BOTH into the GLSL passes (cs_effects_measure.comp
// derives nothing from it, cs_effects_layer0.comp applies it per pixel) AND
// into the C++ unit tests (tests/test_effects_curve.cpp), which evaluate the
// identical text on the CPU. That is the point of the file: the properties
// the curve promises -- monotonic, identity at strength 0, never above 1.0,
// black stays black -- are asserted on the very code the GPU runs, not on a
// re-typing of it. Only float scalars, GLSL/C++ common syntax, no vec types.
//
// The maths (encoded 0..1 code values in, encoded out -- the pre-pass runs in
// encoded space on purpose, see cs_effects_layer0.comp's header):
//
//   1. GAIN     G = clamp(WHITE / p98, min_gain, max_gain)
//               Levels' white point: the smoothed 98th percentile is pulled
//               toward WHITE, as far as the user's gain bounds allow. A dark
//               scene gets max_gain, a bright one gets slightly under 1.0.
//   2. GAMMA    g = ln(target) / ln(p50 * G), clamped to [GAMMA_MIN, GAMMA_MAX]
//               The exponent that lands the smoothed MEDIAN on the target
//               mid-grey after the gain. Median, not mean: a few bright
//               windows in a dark room must not read as "the room is lit".
//               A darkening gamma (g > 1) is further capped so the smoothed
//               2nd percentile is never pushed below p2 * min_gain -- gamma
//               above 1 crushes shadows by nature, and min_gain is the user's
//               "how dark may it go" for exactly that.
//   3. SHOULDER y = k + u / (1 + u / c) above the knee k, when the curve's
//               top (G^g, its value at x = 1) exceeds 1.0. Reinhard-shaped
//               on the excess with c chosen so x = 1 lands exactly on 1.0:
//               slope 1 at the knee (no visible break), monotonic for any
//               overshoot, and it degenerates to the identity as G^g -> 1, so
//               a scene that needs no compression gets none. Below the knee,
//               and whenever the top does not overshoot, the shoulder is a
//               no-op -- a mid scene stays a mid scene.
//
// Why no black-point subtraction, though "levels" usually has one: pulling
// the 2nd percentile toward black crushes the deepest shadows, which is the
// opposite of what a dark map needs, and capped small enough to be harmless
// on a mid scene it is also too small to do anything. Every step here
// passes through (0, 0), so black stays black without a floor being needed.
//
// Why these bounds: GAMMA_MIN 0.5 is a sqrt lift -- the same floor Shadow
// Control uses -- and with max_gain 4.0 (widened from 2.0, 2026-09-07 request:
// "make min gain 0.3, max gain 4.0") lets a 5..20-code scene reach the
// 71..143 range, up from 50..101 at max_gain 2.0 -- see shader-effects.md for
// the re-measured tables. GAMMA_MAX 1.5 is as far as a darkening gamma goes
// before the shadow cap above becomes the only thing keeping detail; that cap
// is measurably looser at min_gain 0.3 than it was at 0.5 -- see the "Why
// min_gain 0.3" note in shader-effects.md for the numbers, not changed here.
// WHITE 0.9 leaves the top 10 % for the highlights above p98. KNEE 0.7 keeps
// the shoulder off the midtones. GAMMA_MIN, GAMMA_MAX, KNEE and WHITE are
// unchanged by the 2026-09-07 gain-range widening on purpose (a separate
// highlight-rolloff change is being designed against those four constants;
// keeping them fixed here lets the two changes be judged independently).
// Measured numbers for the three reference scenes are in
// superdoc/features/shader-effects.md.

#ifndef EFFECTS_CURVE_H_
#define EFFECTS_CURVE_H_

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace gamescope::effects_curve
{
	inline float clamp( float x, float lo, float hi ) { return std::min( std::max( x, lo ), hi ); }
	using std::log;
	using std::max;
	using std::min;
	using std::pow;
#define EC_FUNC inline
#else
#define EC_FUNC
#endif

const float AB_DYN_WHITE     = 0.9f;   // where p98 is pulled toward (encoded)
const float AB_DYN_KNEE      = 0.7f;   // shoulder starts here (encoded)
const float AB_DYN_GAMMA_MIN = 0.5f;   // strongest lift
const float AB_DYN_GAMMA_MAX = 1.5f;   // strongest darkening

// Step 1. Levels gain from the smoothed 98th percentile.
EC_FUNC float ab_dyn_gain( float p98, float minGain, float maxGain )
{
	return clamp( AB_DYN_WHITE / max( p98, 0.001f ), minGain, maxGain );
}

// Step 2. Gamma from the smoothed median (after the gain), with the
// shadow cap from the smoothed 2nd percentile and min_gain.
EC_FUNC float ab_dyn_gamma( float p2, float p50, float gain, float target, float minGain )
{
	float m = clamp( p50 * gain, 0.001f, 0.999f );
	float t = clamp( target, 0.01f, 0.99f );
	float g = clamp( log( t ) / log( m ), AB_DYN_GAMMA_MIN, AB_DYN_GAMMA_MAX );
	if ( g > 1.0f )
	{
		float l = p2 * gain;
		if ( l > 0.0001f && l < 0.999f )
		{
			// l^g >= p2 * minGain  <=>  g <= ln(p2 * minGain) / ln(l)
			float gmax = log( max( p2 * minGain, 0.0001f ) ) / log( l );
			g = min( g, max( gmax, 1.0f ) );
		}
	}
	return g;
}

// Step 3 applied to one channel: gain, gamma, then the shoulder if the
// curve's own top overshoots 1.0. Output is always in [0, 1].
EC_FUNC float ab_dyn_curve( float x, float gain, float gamma )
{
	float y = pow( max( x, 0.0f ) * gain, gamma );
	float top = pow( gain, gamma );   // the curve at x = 1: its largest value
	if ( top > 1.0f && y > AB_DYN_KNEE )
	{
		float k = AB_DYN_KNEE;
		float U = top - k;           // input excess to absorb
		float R = 1.0f - k;          // output room left
		float c = U * R / ( U - R ); // U > R because top > 1
		float u = y - k;
		y = k + u / ( 1.0f + u / c );
	}
	return min( y, 1.0f );
}

#ifdef __cplusplus
} // namespace gamescope::effects_curve
#endif
#undef EC_FUNC

#endif // EFFECTS_CURVE_H_
