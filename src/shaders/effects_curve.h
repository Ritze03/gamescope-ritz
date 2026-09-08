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
//   1. GAIN     G = clamp(max(WHITE / p98, target^(1/gamma_min) / p50),
//                          min_gain, max_gain)
//               Two demands, and the LARGER wins. `WHITE / p98` is Levels'
//               white point: the smoothed 98th percentile pulled toward
//               WHITE, as before. The second is Target brightness's own
//               demand: the exposure the gain must supply so that the
//               gamma, AT ITS FLOOR, can still land the median on the
//               target. It is deliberately the SMALLEST gain that leaves
//               the target reachable, so the toe (the gamma) keeps doing
//               the lifting it always did and the gain only covers what the
//               floor cannot -- see "Why the median demand" below. The white
//               point is therefore a floor on the gain: a bright scene is
//               never dimmed past putting p98 at 0.9.
//   2. GAMMA    g = ln(target) / ln(p50 * G), clamped to [gamma_min, gamma_max]
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
// Why the median demand, and why the gamma bounds come from the user's own
// gain bounds (2026-09-08). Both were measured before they were changed --
// see shader-effects.md's "The ceiling" tables. The user's report was
// *"anything above target brightness 0.5 and max gain 2.0 [doesn't] do
// anything at all"*, and both halves of it were true, for two different
// reasons:
//
//   * `Target went inert` because it reached the picture ONLY through the
//     exponent, and that exponent was clamped to a fixed [0.5, 1.5]. Solving
//     g = 0.5 gives target = sqrt(p50 * G): every target above that produces
//     the identical clamped gamma. On a dark frame that threshold is small
//     -- 0.44 on the dark reference scene, 0.39 on a real dark game-like
//     frame -- so the DEFAULT target 0.5 already sat on the dead side of it.
//   * `Max gain went inert` because the only demand on the gain was
//     WHITE / p98, and a realistic frame has enough bright content that the
//     demand is small: 2.05 on the dark photographic frame measured, 2.83 on
//     the textured dark scene. Above that the clamp simply never binds and
//     the slider is an EXACT no-op. The flat 5..20-code band chart hid this
//     -- its p98 is 0.15, so it wants 5.9 and every setting up to 4.0 bit.
//
// So, two changes, and each is needed for one half of the report:
//
//   * `The gamma floor is 1 / max_gain` instead of a fixed 0.5. Max gain now
//     governs "how bright may it go" in BOTH channels -- the exact mirror of
//     min_gain, which already means "how dark may it go" through the shadow
//     cap. max_gain 2.0 reproduces the old 0.5 floor exactly (which is why
//     2.0 is the anchor), 4.0 gives 0.25, and max_gain 1.0 ("do not
//     brighten") now really does not brighten, where before the gamma still
//     lifted a 20-code band to 71. This is what makes Max gain move a
//     REALISTIC frame, whose white-point demand saturates around 2: on the
//     dark photographic frame, max_gain 1.5 / 2 / 3 / 4 reads 24 / 50 / 77 /
//     77 on a 5-code input where before it read 51 at every one of them.
//   * `The gain also carries target^(1/gamma_min) / p50`, so once the gamma
//     floor is reached the gain takes over and Target keeps moving the
//     picture instead of going flat. `Why that expression and not the
//     simpler target / p50:` the simpler one is a bigger gain, and moving
//     lift out of the toe and into a linear gain DARKENS the shadows at the
//     same median -- measured on the half-dark/half-bright split scene, the
//     dark half's bands fell 12/17/23/28/34 -> 6/9/13/18/22 at the defaults.
//     Taking the smallest gain that still leaves the target reachable leaves
//     that scene bit-identical and still extends Target's useful range.
//
// `What Max gain is once the target IS reached:` a contrast control, not a
// brightness one. The median is pinned on Target by construction, so a
// higher Max gain simply puts more of the same mid-tone level into the
// linear gain and less into the toe -- the deepest shadows come out slightly
// darker and everything above the mid-tones slightly brighter (dark
// reference scene at target 0.5: max_gain 3 -> 93/152 for inputs 5/20,
// max_gain 4 -> 84/157). That is why ab_dyn_binding() below exists and why
// its "none" wording names Target brightness: in that regime Target is the
// control that moves the picture, and the panel now says so.
//
// Why these bounds: the floor is clamped at 0.25 (a fourth-root lift; below
// that a single code of near-black lands above 90 and sensor noise is all
// you see) and at 1.0 (no lift at all). GAMMA_MAX 1.5 is as far as a
// darkening gamma goes before the shadow cap above becomes the only thing
// keeping detail, and it is now additionally capped at 1 / min_gain so
// min_gain 1.0 ("do not darken") really does not darken -- at every default
// that cap is slack (1 / 0.3 = 3.33) and the shadow cap binds first, so it
// changes no measured number. WHITE 0.9 leaves the top 10 % for the
// highlights above p98. KNEE 0.7 keeps the shoulder off the midtones.
// Measured numbers for the reference scenes are in
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
const float AB_DYN_GAMMA_MIN = 0.25f;  // hardest lift any max_gain may ask for
const float AB_DYN_GAMMA_MAX = 1.5f;   // strongest darkening

// LOCAL ADAPTATION (2026-09-07). How far a cell's own level may be treated
// as differing from the frame's. See ab_local_ratio() below for the whole
// argument; these are the only two new constants the operator introduces.
const float AB_LOCAL_RATIO_MIN = 0.25f;
const float AB_LOCAL_RATIO_MAX = 4.0f;

// The gamma bounds, derived from the user's own gain bounds (2026-09-08 --
// see the header). Both are exactly 1.0 at the "do nothing in this
// direction" end of their slider, so a bound the user set is never
// contradicted by the exponent; ab_gamma_min <= 1 <= ab_gamma_max always,
// so the pair can never invert.
EC_FUNC float ab_gamma_min( float maxGain )
{
	return clamp( 1.0f / max( maxGain, 0.001f ), AB_DYN_GAMMA_MIN, 1.0f );
}

EC_FUNC float ab_gamma_max( float minGain )
{
	return max( min( AB_DYN_GAMMA_MAX, 1.0f / max( minGain, 0.001f ) ), 1.0f );
}

// Step 1. The gain: the larger of Levels' white point and Target brightness
// read as a direct exposure, inside the user's bounds. See the header for
// why the median demand is there and why `max` and not `min`.
// The gain the two demands ASK for, before the user's bounds. Split out so
// ab_dyn_binding() below can ask "did a bound bind?" with the very
// expression the gain uses -- the two cannot drift.
EC_FUNC float ab_gain_demand( float p98, float p50, float target, float maxGain )
{
	float t = clamp( target, 0.01f, 0.99f );
	float white = AB_DYN_WHITE / max( p98, 0.001f );
	// The exposure the gain must supply so that the gamma, AT ITS FLOOR,
	// still lands the median on the target: (p50 * G)^gmin = t.
	float mid   = pow( t, 1.0f / ab_gamma_min( maxGain ) ) / max( p50, 0.001f );
	return max( white, mid );
}

EC_FUNC float ab_dyn_gain( float p98, float p50, float target, float minGain, float maxGain )
{
	return clamp( ab_gain_demand( p98, p50, target, maxGain ), minGain, maxGain );
}

// Step 2. Gamma from the smoothed median (after the gain), with the
// shadow cap from the smoothed 2nd percentile and min_gain. When the gain
// alone reached the target this is exactly 1.0 (p50 * G == target), so the
// exponent only ever takes up what the gain could not.
EC_FUNC float ab_dyn_gamma( float p2, float p50, float gain, float target, float minGain, float maxGain )
{
	float m = clamp( p50 * gain, 0.001f, 0.999f );
	float t = clamp( target, 0.01f, 0.99f );
	float g = clamp( log( t ) / log( m ), ab_gamma_min( maxGain ), ab_gamma_max( minGain ) );
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

// WHICH LIMIT IS BINDING (2026-09-08). The single most expensive thing about
// the ceiling above was not the ceiling: it was that a clamped slider looks
// exactly like a working one, so the user spent a session dragging a control
// that could not move. This classifies, from the same three statistics and
// the same bounds the two functions above use, WHICH constraint is currently
// stopping the picture -- so the settings panel and the `effects_ab_log`
// trace say the same thing and cannot drift. Codes, not strings, because
// this header is compiled as GLSL too; ab_binding_text() below names them
// for the C++ side.
const int AB_BIND_NONE      = 0;   // the mid-tones are on Target brightness
const int AB_BIND_GAIN_MAX  = 1;   // gain pinned at max_gain (gamma still free)
const int AB_BIND_GAIN_MIN  = 2;   // gain pinned at min_gain (gamma still free)
const int AB_BIND_LIFT      = 3;   // gamma at its floor -- Target does no more
const int AB_BIND_DARKEN    = 4;   // gamma at its ceiling -- Target does no more
const int AB_BIND_SHADOW    = 5;   // the shadow cap, i.e. min_gain, holds gamma

EC_FUNC int ab_dyn_binding( float p2, float p50, float p98, float target, float minGain, float maxGain )
{
	float t     = clamp( target, 0.01f, 0.99f );
	float want  = ab_gain_demand( p98, p50, target, maxGain );
	float gain  = ab_dyn_gain( p98, p50, target, minGain, maxGain );
	float m     = clamp( p50 * gain, 0.001f, 0.999f );
	float raw   = log( t ) / log( m );
	float gmin  = ab_gamma_min( maxGain );
	float gmx   = ab_gamma_max( minGain );
	float g     = ab_dyn_gamma( p2, p50, gain, target, minGain, maxGain );

	if ( raw <= gmin )
		return AB_BIND_LIFT;
	if ( raw >= gmx )
		return AB_BIND_DARKEN;
	if ( g < raw - 1e-4f )
		return AB_BIND_SHADOW;
	if ( want > maxGain )
		return AB_BIND_GAIN_MAX;
	if ( want < minGain )
		return AB_BIND_GAIN_MIN;
	return AB_BIND_NONE;
}

// LOCAL ADAPTATION, step 0 (2026-09-07). One number per pixel: how bright
// THIS part of the frame is relative to the frame as a whole, blended
// toward 1 by the user's Local adaptation strength.
//
//   r = clamp(local_mean / global_mean, RATIO_MIN, RATIO_MAX)
//   r_eff = mix(1, r, strength)
//
// The apply pass then runs the SAME ab_dyn_gain / ab_dyn_gamma above on
// p98 * r_eff, p50 * r_eff, p2 * r_eff -- i.e. on the frame's own histogram
// SHIFTED to this neighbourhood's level. That is the whole operator:
//
//   * `Why a shift of the global histogram, not a second set of local
//     statistics:` a 16x16 cell has one number, not a histogram; assuming a
//     dark corner has the frame's shape at a lower level is the cheapest
//     assumption that is exactly right when the frame is uniform.
//   * `Why this is EXACTLY the identity at strength 0:` r_eff is then 1 and
//     every input to the curve is bit-for-bit what the global path passes.
//     No "local off" branch is needed for correctness -- the apply pass
//     branches only to skip the four map fetches.
//   * `Why the deviation clamp:` r is the only thing that can push a pixel's
//     curve away from the frame's. Bounding it bounds the halo amplitude
//     directly, and because ab_dyn_gain() and ab_dyn_gamma() clamp their
//     own outputs afterwards, a locally-adapted pixel can never leave the
//     user's [min_gain, max_gain] and [GAMMA_MIN, GAMMA_MAX] -- local
//     adaptation redistributes inside the user's bounds, it never widens
//     them. `Why 0.25..4:` two stops each way in encoded terms. Measured on
//     the 50/50 split scene -- the hardest case the test client has -- the
//     RAW ratios run 0.21..1.93, so the low clamp does bind there (by a
//     little) and the high one does not. It binds harmlessly: at 0.25 the
//     gain it produces is already 3.6, so the user's own max_gain 4.0 takes
//     over almost immediately after it.
EC_FUNC float ab_local_ratio( float localMean, float globalMean )
{
	float r = clamp( localMean, 0.0f, 1.0f ) / max( globalMean, 0.001f );
	return clamp( r, AB_LOCAL_RATIO_MIN, AB_LOCAL_RATIO_MAX );
}

// The blend the apply pass actually uses: r^strength, NOT mix(1, r,
// strength). Separate from ab_local_ratio() so the "strength 0 is the global
// path exactly" property is one testable line rather than an inline blend.
//
// `Why the geometric blend:` the ratio's whole job is to divide into the
// white point, so the GAIN it produces goes as 1/r -- and a linear blend of
// r therefore gives a wildly uneven slider. Measured on the 50/50 split
// scene at ratio 0.25 (the clamp), linear blend, strengths 0/25/50/75/100 %:
// the dark half's gain read 0.90 / 1.11 / 1.44 / 2.06 / 3.60 -- three
// quarters of the effect crammed into the last quarter of the travel. r^s
// makes it 0.90 / 1.27 / 1.80 / 2.55 / 3.60: an even step in stops per step
// of the slider (every ratio 1.41), which is what a brightness control
// should be. Both forms
// are exactly 1 at s = 0 and exactly r at s = 1; only the middle differs.
// pow is safe here because ab_local_ratio() has already clamped r away
// from 0, and r^s for 0 <= s <= 1 always lies between 1 and r, so the
// deviation clamp still bounds the result.
EC_FUNC float ab_local_shift( float localMean, float globalMean, float strength )
{
	float s = clamp( strength, 0.0f, 1.0f );
	if ( s <= 0.0f )
		return 1.0f;
	return pow( ab_local_ratio( localMean, globalMean ), s );
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
// The one wording of ab_dyn_binding()'s codes: the settings panel's
// Diagnostics fact and the `effects_ab_log` trace both print this, so the
// two can never say different things about the same frame. One short line,
// Facts vocabulary, no trailing full stop.
inline const char *ab_binding_text( int nBinding )
{
	switch ( nBinding )
	{
		case AB_BIND_GAIN_MAX: return "gain is at Max gain";
		case AB_BIND_GAIN_MIN: return "gain is at Min gain";
		case AB_BIND_LIFT:     return "Max gain -- Target brightness does no more here";
		case AB_BIND_DARKEN:   return "the darkening limit -- Target brightness does no more here";
		case AB_BIND_SHADOW:   return "Min gain, holding the shadows up";
		default:               return "none -- the mid-tones are on Target brightness";
	}
}
} // namespace gamescope::effects_curve
#endif
#undef EC_FUNC

#endif // EFFECTS_CURVE_H_
