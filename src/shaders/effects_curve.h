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

// ---- THE ADAPTATION SPEED (the EMA both adaptive effects share) --------
//
// cs_effects_measure.comp smooths every statistic with one exponential
// moving average whose time constant is the ACTIVE effect's "adapt to
// brighter" / "adapt to darker" seconds (Adaptive Brightness's pair, or --
// since 2026-09-09 -- Adaptive Gamma's own; they are mutually exclusive, so
// one EMA serves both). This is the whole of "how fast does the picture
// follow the scene", and it lives here rather than inline in the shader so
// tests/test_effects_curve.cpp asserts it on the same text the GPU runs.
//
// tau is SECONDS TO ~63 % of a step, which is what makes the settling time
// predictable and the slider honest: a step is within 5 % of its new value
// after 3 tau, whatever dt is and however the frames are spaced. The
// per-frame form below composes to exactly that because
// (1 - alpha(dt1)) * (1 - alpha(dt2)) = exp(-(dt1 + dt2) / tau) -- so the
// adaptation over an interval depends on the elapsed TIME and not on how
// many composites the interval happened to get.
//
// EC_TAU_MIN is a floor, not a clamp of taste: the panel's own slider stops
// at 0.1 s, and this stops a hand-edited 0 (or a negative) from becoming a
// division by zero. It is deliberately small enough that hitting it means
// "as fast as the control goes", never "frozen" -- there is no setting of
// this pair that stops the picture adapting, which is why the binding
// readout has no code for it.
const float EC_TAU_MIN = 0.001f;

EC_FUNC float ema_alpha( float dt, float tau )
{
	return clamp( 1.0f - exp( -max( dt, 0.0f ) / max( tau, EC_TAU_MIN ) ), 0.0f, 1.0f );
}

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

// ===========================================================================
//  ADAPTIVE GAMMA (2026-09-08) -- the same statistics, one exponent, nothing
//  else. The user's request, verbatim: *"Make something similar, but make it
//  gamma based. Call it adaptive gamma."*
// ===========================================================================
//
// The whole operator, per channel, on the SAME smoothed statistics the
// measure pass already produces for Adaptive Brightness (there is no second
// measurement path -- see cs_effects_measure.comp):
//
//   gmin = clamp(1 / max_lift, 0.25, 1.0)          // the user's own bounds
//   gmax = clamp(max_darken, 1.0, 4.0)
//   r    = ab_local_shift(local_mean, mean, local_strength)   // 1.0 when off
//   g    = clamp( ln(target) / ln(p50 * r), gmin, gmax )
//   out  = mix(x, x^g, strength)
//
// `Why an exponent alone is a different effect and not a cheaper Adaptive
// Brightness:` for x in [0, 1] and any g > 0, x^g is again in [0, 1], with
// 0 and 1 as EXACT fixed points. So this operator
//
//   * cannot clip, ever, at any setting -- which is why it needs no
//     shoulder, no white point and no knee. Adaptive Brightness needs all
//     three because its levels gain multiplies and can push a value past
//     1.0; nothing here can. A near-white highlight comes out near-white and
//     STILL DISTINCT from white, rather than compressed by a shoulder or
//     clamped flat;
//   * changes CONTRAST rather than exposure. A gain moves every value by the
//     same factor; an exponent moves the mid-tones a lot and the ends not at
//     all, so the picture's black and white points are untouched by
//     construction and only the shape between them adapts. That is a
//     different look, deliberately, not a worse one;
//   * is cheaper: one log and one pow per channel, no gain, no shoulder
//     branch, and (with Local adaptation off) no per-pixel work at all
//     beyond the pow -- g is a frame constant.
//
// `Why the bounds are the user's, not two more constants:` this is the whole
// lesson of the 2026-09-08 ceiling (see the long note above). Target reaches
// the picture ONLY through this exponent, so whatever clamps the exponent
// also decides where Target stops working -- and a clamped slider looks
// exactly like a working one. With a fixed [0.5, 1.5] the way Adaptive
// Brightness had, Target would go inert on a dark frame at about
// sqrt(p50) and the user would have no way to see it or to reach past it.
// Making the two bounds the row's own params (`Max lift`, `Max darken`)
// means the limit that is binding is always a control the user can move,
// and ag_binding() below names which one it is. Both are 1.0 at their "do
// nothing in this direction" end, so max_lift 1.0 really does not brighten
// and max_darken 1.0 really does not darken -- exactly the property Adaptive
// Brightness's gamma bounds were given on 2026-09-08 for the same reason.
//
// `Why the hard clamps are still there:` they are the panel's own range
// ends, not a second, hidden ceiling -- AG_LIFT_MAX / AG_DARKEN_MAX are the
// numbers PanelShaders.cpp's Range() uses, so neither can bind before the
// slider does. The 0.25 floor is shared with Adaptive Brightness's
// AB_DYN_GAMMA_MIN and carries the same justification (a fourth-root lift;
// below it a single code of near-black lands above 90 and sensor noise is
// the whole picture).
const float AG_LIFT_MAX   = 4.0f;   // == PanelShaders.cpp's Max lift Range() top
const float AG_DARKEN_MAX = 4.0f;   // == PanelShaders.cpp's Max darken Range() top

EC_FUNC float ag_gamma_min( float maxLift )
{
	// Deliberately the same expression ab_gamma_min() uses, against the same
	// shared constant: "how far may it brighten" has one honest floor in this
	// pipeline, and two copies of it would drift.
	return clamp( 1.0f / max( maxLift, 0.001f ), AB_DYN_GAMMA_MIN, 1.0f );
}

EC_FUNC float ag_gamma_max( float maxDarken )
{
	return clamp( maxDarken, 1.0f, AG_DARKEN_MAX );
}

// The exponent that lands the smoothed median on the target, bounded by the
// user's own two limits. Median, not mean, for the reason ab_dyn_gamma()
// gives: a few bright windows in a dark room must not read as "the room is
// lit". `p50` is already SHIFTED by ab_local_shift() when Local adaptation
// is on -- the caller does that, exactly as the Adaptive Brightness path
// does, so the two operators share one local-adaptation definition.
EC_FUNC float ag_gamma( float p50, float target, float maxLift, float maxDarken )
{
	float m = clamp( p50, 0.001f, 0.999f );
	float t = clamp( target, 0.01f, 0.99f );
	return clamp( log( t ) / log( m ), ag_gamma_min( maxLift ), ag_gamma_max( maxDarken ) );
}

// One channel. The clamp on the way in is what makes "never above 1.0" a
// property of the arithmetic rather than of the caller: x in [0, 1] and
// g > 0 give x^g in [0, 1], with x = 0 and x = 1 exact fixed points, so
// there is nothing to clip and no shoulder to fit.
EC_FUNC float ag_curve( float x, float gamma )
{
	return pow( clamp( x, 0.0f, 1.0f ), gamma );
}

// WHICH LIMIT IS BINDING, Adaptive Gamma's own. Same contract as
// ab_dyn_binding() above -- codes here because this header is compiled as
// GLSL too, wording in ag_binding_text() on the C++ side -- so the panel's
// Diagnostics fact and the `effects_ab_log` trace cannot say different
// things about one frame. Every control this row owns can be inert, and
// each of those states has a code:
//   * Target, once the exponent is clamped              -> LIFT / DARKEN
//   * Max lift / Max darken, once the target is reached -> NONE (nothing is
//     stopping the picture; raising the limit that is not binding buys
//     nothing, which is what NONE's wording says)
//   * Strength at 0, which makes ALL of them inert      -> STRENGTH
// Local adaptation's own no-op case (a uniform frame, where every cell's
// ratio is 1 by construction) is not a limit and is not classified here;
// it is documented on the param itself and visible in `effects_ab_log`'s
// lmin/lmax spread.
const int AG_BIND_NONE     = 0;
const int AG_BIND_LIFT     = 1;   // exponent at its floor  -- Max lift binds
const int AG_BIND_DARKEN   = 2;   // exponent at its ceiling -- Max darken binds
const int AG_BIND_STRENGTH = 3;   // Strength 0: nothing is applied at all

EC_FUNC int ag_binding( float p50, float target, float maxLift, float maxDarken, float strength )
{
	if ( strength <= 0.0f )
		return AG_BIND_STRENGTH;
	float m   = clamp( p50, 0.001f, 0.999f );
	float t   = clamp( target, 0.01f, 0.99f );
	float raw = log( t ) / log( m );
	if ( raw <= ag_gamma_min( maxLift ) )
		return AG_BIND_LIFT;
	if ( raw >= ag_gamma_max( maxDarken ) )
		return AG_BIND_DARKEN;
	return AG_BIND_NONE;
}

// ===========================================================================
//  DARK FLOOR (2026-09-14) -- keeps a genuinely dark scene dark under either
//  adaptive effect. The user's report, verbatim: *"the adaptive brightness
//  and the adaptive gamma both completely destroy REALLY dark images. ...
//  Is there some kind of filter, that keeps really dark stuff really dark or
//  something?"* -- shown on a real capture: a near-black CS2 corridor (the
//  smoothed median a fraction of a code above zero) came out BINARISED --
//  pure black held, and everything even slightly above it slammed toward
//  white. That is not a rounding error: with the median that close to zero,
//  BOTH operators are already doing exactly what their own bounds say. AG's
//  exponent is ln(target)/ln(p50), which for p50 -> 0 is driven to its
//  floor (1 / max_lift) regardless of how dark the actual pixel is; AB's
//  Dynamic gain is pinned to max_gain by the same p50 -> 0 limit and its
//  gamma to the same floor. Both floors are already honoured correctly (see
//  ag_gamma_min() / ab_gamma_min() and their callers' own clamp()s -- this
//  header's own tests sweep p50 -> 0 and assert neither exponent nor gain
//  ever exceeds the user's bound), so the fix is not a clamp fix: it is
//  this, a THIRD control that fades the whole operator out, rather than
//  bounding it, when the scene itself is the thing that is dark.
//
//  THE FORMULA. Let `m` be the SAME smoothed statistic each operator
//  already keys its curve on -- p50, the median both AG's gamma and AB
//  Dynamic's gamma are fitted to (AB's own gain reads p98 too, but p50 is
//  what discriminates a scene that is dark BECAUSE it is mostly literal
//  black, which is exactly the case this exists for, from one that is
//  merely dim throughout -- see "why p50 and not the mean" below). Then:
//
//    w = smoothstep(dark_floor / 2, dark_floor, m)
//    dark_floor <= 0  =>  w = 1                      -- the floor is off
//
//  and each operator blends its own parameter(s) toward the identity by w
//  rather than being clamped:
//
//    AG:            gamma_eff = mix(1, gamma, w)     -- x^gamma_eff
//    AB Whole image: gain_eff = mix(1, gain,  w)      -- c * gain_eff
//    AB Dynamic:    gain_eff = mix(1, gain,  w), gamma_eff = mix(1, gamma, w)
//                   -- BOTH parameters, so the curve itself (not just one
//                   half of it) becomes y = x at w = 0: ab_dyn_curve(x, 1, 1)
//                   is the identity by construction (its own top is exactly
//                   1, so the shoulder never engages either).
//
//  `Why smoothstep, and why centred so it spans dark_floor/2 .. dark_floor
//  rather than 0 .. dark_floor:` the ramp's SLOPE has to reach zero at both
//  ends, for the same reason the bloom weight and the AB shoulder do -- the
//  scene's smoothed statistic drifting across the floor under the EMA must
//  never produce a kink a panning camera could reveal, and smoothstep is
//  exactly the cubic that is flat at both ends. Splitting the span in half
//  around dark_floor, rather than running it 0..dark_floor, means the
//  slider's own value is the point the effect is HALFWAY restored, which is
//  the reading that matches what the number does when the user drags it.
//
//  `Why the WEIGHT is computed from the RAW, un-shifted statistic, not the
//  one Local adaptation shifts per pixel:` "leave dark SCENES alone" is a
//  question about the frame as a whole. Feeding the locally-shifted value in
//  would make the floor's own strength vary across the image under Local
//  adaptation -- a second, hidden halo control nobody asked for, layered on
//  top of the one Local adaptation already owns. Local adaptation still
//  reaches every pixel exactly as before once the weight has decided the
//  scene qualifies; the two are independent.
//
//  `Why p50 and not the mean, for AB Whole image too, even though its OWN
//  gain reads the mean:` measured directly on the report's own capture (see
//  shader-effects.md) -- a corridor that is genuinely black over most of its
//  area but carries a HUD, a lamp and chat text reads p50 = 0.0003 (a
//  literal black majority) but MEAN = 0.055, because scattered small bright
//  regions pull an average up far more than they move a median. A floor keyed
//  on the mean would barely engage on exactly the frame this feature exists
//  for. p50 is computed unconditionally every frame regardless of mode (see
//  cs_effects_measure.comp's header), so using it for Whole image's floor
//  costs nothing extra and is a strictly better discriminator of "is this
//  scene dark" than the statistic that mode's own gain happens to use.
//
//  `Why 0 must be an EXACT identity, not merely a very small number:` this is
//  the "0 turns it off" contract every switch/param pair in this pipeline
//  follows, and dividing by (dark_floor - dark_floor/2) at dark_floor = 0
//  would be 0/0, not a small number -- so it is its own branch rather than a
//  falling-out of the general formula.
//
//  THE DEFAULT, AND WHY IT IS SMALL (measured, shader-effects.md has the
//  full table). 0.03 (encoded), i.e. the ramp runs code 4..8. Two existing
//  regression scenes bracket it: tests/effects_scene_client.c's `dark`
//  scene -- five bands 5..20, no large black area -- measures p50 = 0.047
//  (code 12), and scripts/effects-regression.sh already has a PRE-EXISTING
//  contract on it (`dark-dynamic`: band 5 must lift to >= 30) that predates
//  this feature and must keep passing; `texdark`, the textured dark-game
//  stand-in, measures p50 = 0.078 (code 20). Both sit comfortably above
//  0.03's full-weight point, so neither loses any lift by default -- this
//  floor is deliberately narrow enough to catch only scenes far darker than
//  either, i.e. the majority-black case the report was actually about, not
//  "any dark scene". The report's own capture (p50 = 0.0003) and the
//  `blackout` scene added for this feature (p50 ~= 0.008, ~90% of pixels at
//  code 0..6) both sit far below 0.03's half-point and come out fully
//  neutralised.
const float DARK_FLOOR_MIN_T = 0.0001f;   // guards a hand-edited negative span

EC_FUNC float dark_weight( float m, float darkFloor )
{
	if ( darkFloor <= 0.0f )
		return 1.0f;
	const float lo = darkFloor * 0.5f;
	const float hi = darkFloor;
	const float t = clamp( ( m - lo ) / max( hi - lo, DARK_FLOOR_MIN_T ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );   // smoothstep
}

// ===========================================================================
//  BLOOM (2026-09-08) -- the scalar half of the glow. The user's request,
//  verbatim: *"Add a bloom shader for more casual games"*.
// ===========================================================================
//
// Bloom is the first effect in this pre-pass that is NOT a per-pixel
// function: it needs a bright pass, a blur at reduced resolution, and a
// composite (see cs_effects_bloom_down.comp and effects_bloom_blur.h). What
// lives here is the part that IS scalar, and therefore the part
// tests/test_effects_curve.cpp can assert on the same text the GPU compiles:
// how a pixel's luma turns into a bright-pass weight, how the Radius slider
// turns into a blur sigma, and how the glow is put back onto the picture.
//
// `Why the weight is proportional to how far ABOVE the threshold a pixel is,
// rather than a gate that is simply on above it:` this pipeline is SDR
// (DECISIONS.md #15). In an HDR renderer a bloom threshold has an
// unambiguous meaning -- only values above 1.0 emit, and an ordinary lit
// surface never reaches one -- but here every pixel is already inside
// [0, 1], so a plain "above this, glow fully" gate makes a bright wall emit
// exactly as hard as a lamp does, and a scene that is mostly bright turns
// into uniform haze. Arithmetic, at the shipped threshold of 0.75: a
// 200-code surface contributes 2 % of its colour under the weight below and
// would contribute 100 % under a gate whose knee ended beneath it. Measured
// consequence of the shipped weight on the reference `bright` scene: its
// 30-code shadow rectangles read 48 rather than being washed most of the way
// to the bands above them. Scaling with the excess restores the distinction
// the missing HDR range would have given.
//
// `Why x squared and not x, and not a smoothstep:` the shape has to be zero
// AND flat at the threshold, because that is where the shimmer question is
// decided -- a pixel wandering across the boundary under a pan must not
// change its contribution abruptly. x is zero there but not flat (its slope
// is a constant 1/headroom, so a pixel crossing the cut jumps). A smoothstep
// is flat at BOTH ends, which would put a plateau at the top: a 250-code
// pixel and a 255-code one would emit identically, throwing away the
// "brighter things glow more" the whole effect is about. x squared is flat
// where it must be and still rising where it must be.
//
// `Why the glow is SCREENed onto the picture and not added:`
// screen(a, b) = a + b(1 - a) is, for a and b in [0, 1], again in [0, 1]
// -- with a = 1 and b = 1 as exact fixed points, so it CANNOT clip, which a
// plain add cannot promise. It is also the physically nicer of the two here:
// the amount of light a glow adds is scaled by how much headroom the pixel
// underneath still has, so the bright source at the centre of the glow (the
// one thing a naive additive bloom always blows out) is left essentially
// where it was while the dark field around it takes the light. Every other
// effect in this pipeline is careful never to blow a highlight; this is how
// the one operation that naturally would, does not.
//
// `Why Intensity is an EXPONENT on the transmitted light rather than a
// multiplier on the glow -- the version that was built first, and dropped:`
// the obvious composite is screen(base, glow x intensity), and it clips.
// Above intensity 1 the product runs past 1.0, the clamp that has to follow
// pins it there, and every neighbourhood whose blurred glow exceeds
// 1/intensity goes to pure white. Worked back out of the shipped form's own
// captures (shader-effects.md's tables; the glow at each region is recovered
// from the output, then both composites evaluated on it): on the reference
// `bright` scene at Intensity 2.0 the 245-code band would land on a clipped
// 255 where this form gives 254, and at Threshold 0 with Intensity 2.0 four
// of its five bands would merge onto white instead of two. So the intensity
// is applied where it cannot leave the range:
//
//     out = 1 - (1 - base) * (1 - glow)^intensity
//
// which is exactly "screen the glow on `intensity` times over". It is
// linear in intensity for small glows -- (1-g)^k ~ 1 - kg, so on the dark
// field where the glow is faint this is the multiply, to within a fraction
// of a code -- and it saturates instead of clipping where the glow is
// strong. Intensity 0 is an exact identity, intensity 1 is the plain screen,
// and no setting of any of the three params can put a pixel on 1.0 unless
// it was already there. The price is one pow per channel instead of one
// multiply, on the frames Bloom is on.
const float BLOOM_HEADROOM_MIN = 0.001f;  // guards 1 - threshold at threshold 1.0
const float BLOOM_SIGMA_MIN    = 1.0f;    // blur sigma at Radius 0, in glow-buffer texels
const float BLOOM_SIGMA_MAX    = 3.0f;    // ... and at Radius 1

// How much of a pixel's colour reaches the glow buffer: 0 at and below the
// threshold, 1 at pure white, and the square of the fraction of the
// remaining headroom in between. Monotone increasing in `luma` and monotone
// DECREASING in `threshold` -- which is exactly the two properties "a
// brighter pixel glows more" and "raise the threshold and less glows", both
// asserted exhaustively on the CPU. At threshold 1.0 nothing glows at all,
// at any luma, which is the right meaning for the top of that slider.
EC_FUNC float bloom_weight( float luma, float threshold )
{
	float t = clamp( threshold, 0.0f, 1.0f );
	float x = clamp( ( clamp( luma, 0.0f, 1.0f ) - t ) / max( 1.0f - t, BLOOM_HEADROOM_MIN ),
	                 0.0f, 1.0f );
	return x * x;
}

// Radius 0..1 -> the separable blur's sigma, in glow-buffer texels. The glow
// buffer is BLOOM_DOWN (8) times smaller than the game's own image, so this
// is 8..24 SOURCE pixels of sigma, i.e. a visible glow roughly 24..72 source
// pixels across. `Why the floor is 1 and not 0:` the buffer is built by an
// 8x8 box average, so its own texels are already an 8-pixel-wide feature; a
// sigma below one texel would leave that box structure visible as blocking
// after the bilinear upsample instead of a smooth falloff.
EC_FUNC float bloom_sigma( float radius )
{
	return BLOOM_SIGMA_MIN + ( BLOOM_SIGMA_MAX - BLOOM_SIGMA_MIN ) * clamp( radius, 0.0f, 1.0f );
}

// One channel of the composite: the picture, the blurred bright pass at that
// pixel, and the Intensity slider. See the header note above for why this
// shape and not screen(base, glow * intensity).
//
// The 1e-6 floor is not cosmetic: GLSL's pow is UNDEFINED for pow(0, 0), and
// glow = 1 with intensity = 0 reaches exactly that. With the floor,
// intensity 0 returns `base` unchanged for every glow, which is what "0 is
// off" has to mean -- and it also makes the "never reaches 1.0 unless the
// base already was" claim strict rather than approximate.
EC_FUNC float bloom_apply( float base, float glow, float intensity )
{
	float a = clamp( base, 0.0f, 1.0f );
	float g = clamp( glow, 0.0f, 1.0f );
	float k = max( intensity, 0.0f );
	return 1.0f - ( 1.0f - a ) * pow( max( 1.0f - g, 1e-6f ), k );
}

// ===========================================================================
//  ADAPTIVE BRIGHTNESS V2 (2026-09-14) -- a NEW, ADDITIVE effect. The user's
//  decision, verbatim: *"Call it 'Adaptive brightness V2' in the GUI.
//  Implement it fully, so I can test it later. DO NOT REMOVE THE ORIGINAL!"*
//  So this sits ALONGSIDE ab_dyn_*/ag_* above (both untouched), as a third,
//  mutually-exclusive choice -- see superdoc/planning/adaptive-brightness-v2-
//  plan.md for the whole design and superdoc/features/shader-effects.md for
//  the measured numbers.
//
//  THE PROBLEM THIS SOLVES (plan section 3.2): x^g for g < 1 has an INFINITE
//  slope at x = 0. On a near-black scene the exponent is driven to its floor
//  and a single code of near-black is amplified by dozens of stops --
//  structural binarisation, not a tuning error. v2's base curve bends into a
//  straight line of slope S (the user's own Max lift) at black instead, so
//  the largest amplification ANYWHERE in the picture is S, by construction.
//
//  THE TOE CURVE (plan 4.3):
//
//    f(x; g, S) = x * ((1 + t) / (x + t)) ^ (1 - g),  t solved so f'(0) = S
//
//  for 0 < g < 1; f(x; 1, S) == x and f(x; g, 1) == x (both are "no lift at
//  all", one via the exponent, one via the slope cap -- either alone is
//  enough to disable the effect, and abv2_toe() below returns the identity
//  directly rather than solving for an infinite t). Closed-form: f(0) = 0,
//  f(1) = 1, f is concave so f' is DECREASING from S at x = 0 -- the slope
//  is bounded by S everywhere, and so is the secant f(x)/x, which is what
//  makes this curve safe to build a Weber-preserving detail term on (see
//  abv2_secant() below).
const float ABV2_BLACK  = 2.0f / 255.0f;   // == cs_effects_layer0.comp's hard floor, effects_curve.h's own dark floor is a SEPARATE, older feature
const float ABV2_G_MIN  = 0.2f;            // not user-facing -- see abv2_g() below
const float ABV2_S_MIN  = 1.0f;            // Max lift's own floor: "do not lift at all"
const float ABV2_T_EPS  = 1e-4f;

// Solves t so that f'(0) == S exactly (plan 4.3's closed form). Callers
// guard the "no lift" cases (g >= 1 or S <= 1) before calling this -- see
// abv2_toe() -- because t -> infinity there and (1+t)/(x+t) would evaluate
// as inf/inf (NaN) rather than the identity the caller actually wants.
EC_FUNC float abv2_solve_t( float g, float S )
{
	float gg = clamp( g, ABV2_G_MIN, 0.999f );
	float s  = max( S, ABV2_S_MIN );
	float p  = 1.0f / ( 1.0f - gg );
	float denom = pow( s, p ) - 1.0f;
	return 1.0f / max( denom, ABV2_T_EPS );
}

// The toe-gamma itself, guarding both "no lift" degenerate cases as an exact
// identity (see the header note above -- this is what makes g == 1 or
// S == 1 a byte-exact no-op rather than a near-miss).
EC_FUNC float abv2_toe( float x, float g, float S )
{
	float xx = clamp( x, 0.0f, 1.0f );
	float gg = clamp( g, ABV2_G_MIN, 1.0f );
	float s  = max( S, ABV2_S_MIN );
	if ( gg >= 0.999f || s <= 1.0f + ABV2_T_EPS )
		return xx;
	// f(0) == 0 by construction -- returned directly rather than evaluated,
	// because in float32 abv2_solve_t()'s S^(1/(1-g)) overflows to inf once
	// g is within ~0.02 of 1 at S = 8 (Lift below ~0.04), which makes t
	// exactly 0 and 0 * pow(1/0, 1-g) a NaN. (V2 QC, 2026-09-14: the GPU
	// never reaches x == 0 here -- the black floor runs first -- but the
	// shared header must not hand a NaN to any caller that does.)
	if ( xx <= 0.0f )
		return 0.0f;
	float t = abv2_solve_t( gg, s );
	return xx * pow( ( 1.0f + t ) / ( xx + t ), 1.0f - gg );
}

// THE KNEE VARIANT (the "monitor" trade, plan 8.2 Q1 -- the plan leaves the
// exact formula open; this is the lead's own resolution, verified by
// exhaustive sampling in tests/test_effects_curve.cpp against the same five
// guarantees the toe carries). Where the toe compresses the WHOLE upper
// range by about g (f'(1) ~= g, the highlight guard), the knee leaves
// highlights EXACTLY untouched (f'(1) == 1 always) and puts the whole
// compression into the mid-tones just above the lifted shadows instead --
// the monitor "Shadow Boost" trade (plan 2.4) rather than the film trade.
//
// Construction: below the knee point ABV2_KNEE_X, a toe curve RESCALED into
// the box [0, KNEE_X] x [0, KNEE_X] (so it still lands exactly on the
// identity line at the knee); above it, the plain identity. Both halves are
// independently monotone and bounded by S (the rescaled toe by the same
// proof as abv2_toe() -- rescaling a bounded-slope curve into a smaller box
// cannot raise its slope; the identity trivially is), so the whole curve is
// too, even though the two halves' slopes do not match AT the knee (a jump
// from the toe's own highlight slope, ~g, up to 1) -- guarantee 3 (monotone)
// only asks for non-negative slope, not a smooth derivative, and a jump
// from a smaller positive slope to a larger one cannot invert anything.
const float ABV2_KNEE_X = 0.5f;

EC_FUNC float abv2_knee( float x, float g, float S )
{
	float xx = clamp( x, 0.0f, 1.0f );
	if ( xx >= ABV2_KNEE_X )
		return xx;
	return ABV2_KNEE_X * abv2_toe( xx / ABV2_KNEE_X, g, S );
}

// The dispatch both the shader and the tests use, so "which shape" is one
// switch, not two copies of an if/else.
EC_FUNC float abv2_curve( float x, float g, float S, bool bKnee )
{
	return bKnee ? abv2_knee( x, g, S ) : abv2_toe( x, g, S );
}

// ---- Choosing g (plan 4.4): a static floor, deepened by adaptation -------
//
//   g_static = 1 - 0.6 * Lift                      // Lift 0..1 -> 1.0..0.4
//   g_adapt  = ln(Target) / ln(anchor_smoothed)     // < 1 when content is dark
//   g        = clamp(min(g_static, g_adapt), G_MIN, 1)   // Adaptation "Scene"
//   g        = clamp(g_static, G_MIN, 1)                 // Adaptation "Off"
//
// `min`, not `max`: adaptation can only make the lift STRONGER on a dark
// scene (a smaller g); on a bright scene the STATIC curve is what lifts the
// one dark thing in it (plan 3.5 -- no global statistic can find a 1%-of-
// frame object), so the floor must never be relaxed by "the scene looks
// bright". G_MIN (0.2) is not a user-facing bound -- it exists only so `t`
// stays finite and the highlight compression stays above a fifth; Lift and
// Max lift are the controls that actually reach the user.
EC_FUNC float abv2_g_static( float lift )
{
	return 1.0f - 0.6f * clamp( lift, 0.0f, 1.0f );
}

EC_FUNC float abv2_g_adapt( float anchorSmoothed, float target )
{
	float m = clamp( anchorSmoothed, 0.001f, 0.999f );
	float t = clamp( target, 0.01f, 0.99f );
	return log( t ) / log( m );
}

EC_FUNC float abv2_g( float lift, float target, float anchorSmoothed, bool bSceneMode )
{
	float gStatic = clamp( abv2_g_static( lift ), ABV2_G_MIN, 1.0f );
	if ( !bSceneMode )
		return gStatic;
	float gAdapt = abv2_g_adapt( anchorSmoothed, target );
	return clamp( min( gStatic, gAdapt ), ABV2_G_MIN, 1.0f );
}

// ===========================================================================
//  DARKENING (2026-09-14; REDESIGNED 2026-09-15 -- the true S-curve) -- the
//  two-sided curve, now fixed exactly AT Target rather than at the lift
//  curve's own continuation above it. The user's request, verbatim: "Make it
//  able to make the image darker (both full and on parts of the image)".
//
//  WHY THE REDESIGN. The first cut (2026-09-14) composed the darken half on
//  L(x)'s own CONTINUATION above Target -- L(Target) was the pivot's height,
//  not Target itself, and L(Target) is a REAL number above Target (0.477
//  against Target 0.35 at the shipped Lift 0.5 / Max lift 4, i.e. code 122
//  vs 89 -- V2 darken QC, 2026-09-15). Two consequences, both measured on
//  the shipped defaults: (1) F(x) >= x for EVERY x above Target -- so
//  "darkening" only ever undid the lift curve's own highlight raise, never
//  took a pixel below its own raw value (skyfore's sky band read 225 raw,
//  233.7 lift-only, 225.0 with Darken 0.5 / Max darken 2 -- back to raw, not
//  below it); (2) L(Target) was a FLOOR nothing above Target could be
//  darkened past, whatever D said. Fixing the pivot's height to Target
//  itself is what buys a real "F(x) <= x above Target" guarantee, and that
//  requires rescaling the LIFT half into [0, Target] too (composing on raw
//  x, not on L's continuation) -- see THE FIXED POINT below for why that is
//  unavoidable once the pivot must sit exactly on Target, and why it costs
//  an off switch instead of the first cut's own pivot gap.
//
//  THE CLOSED FORM (unchanged from the first cut -- only where it is
//  EVALUATED changes, not the family itself). abv2_toe(x; g, S) is
//
//    f(x; g, t) = x * ((1 + t) / (x + t)) ^ (1 - g),  0 < g < 1
//
//  and abv2_solve_t() picks t so f'(0) == S exactly. Differentiating the
//  GENERAL form (any real g, t > 0) gives, at every x in [0, 1]:
//
//    f''(x) has the SAME SIGN as (g - 1)
//
//  (worked out fully: with c = (1+t)^-(g-1), f = c * x * (x+t)^(g-1), so
//  f'' = c * (g-1) * (x+t)^(g-3) * (g*x + 2t), and g*x + 2t > 0 for x in
//  [0,1], t > 0 and g > 0 -- the whole of this pipeline's range -- so only
//  the sign of (g-1) survives. V2 darken QC, 2026-09-15: the factor used to
//  be written "x(1-g) + 2t", which is not what the algebra gives; the
//  conclusion was right, the stated factor was not). So the family is
//  CONCAVE for g < 1 (the toe -- lift, proven/tested above) and, by the
//  same formula, CONVEX for g > 1: swap the roles of the two endpoints and
//  rewrite the exponent as positive (gg - 1 instead of 1 - gg) to avoid a
//  negative-exponent pow, and the SAME algebra that solves t for a target
//  slope at x = 0 works unchanged, now targeting 1/D instead of S:
//
//    f_dark(x; g, t) = x * ((x + t) / (1 + t)) ^ (g - 1),   g > 1
//    f_dark'(0) == 1/D  <=>  t = 1 / (D ^ (1/(g-1)) - 1)
//
//  (abv2_solve_t_dark() below -- literally abv2_solve_t()'s own derivation,
//  target 1/D in place of S, p = 1/(g-1) in place of 1/(1-g)).
//
//  THE BOUND, exactly, not approximately. f_dark(0) = 0, f_dark(1) = 1 (by
//  construction, checked directly), and f_dark is CONVEX (sign(g-1) > 0),
//  so by Jensen's inequality on the two endpoints, f_dark(x) <= x for every
//  x in [0,1] -- "never lifts" is a consequence of convexity plus matching
//  endpoints, not a separate clamp. And for any convex g with g(0) = 0, the
//  tangent-line inequality g(y) >= g(x) + g'(x)(y-x) at y = 0 gives
//  g'(x)*x >= g(x), i.e. the secant g(x)/x is NON-DECREASING in x -- so its
//  minimum over (0,1] is its limit at x -> 0, which is g'(0) = 1/D by
//  construction. Hence f_dark(x)/x >= 1/D everywhere, tight in the limit at
//  black: the darkest anything is ever pushed, anywhere in the frame, is a
//  factor of D. This is the exact mirror of the toe's own S proof, not an
//  approximation of it -- same family, same closed form, one sign flip.
const float ABV2_D_MIN = 1.0f;              // Max darken's own floor: "do not darken at all"
const float ABV2_G_MAX = 1.0f / ABV2_G_MIN; // 5.0 -- reciprocal of the lift floor; see abv2_g_dark()

// Solves t so f_dark'(0) == 1/D exactly, for g > 1 -- the darkening mirror
// of abv2_solve_t(). Same EPS floor on the denominator, same reason: D very
// close to 1 (Darken near "off") makes the denominator near 0, i.e. t huge
// rather than infinite, which is the correct limit (a huge t makes
// (x+t)/(1+t) -> 1, i.e. the identity) -- not a divide that needs guarding
// against NaN, just against float overflow turning "huge" into "inf".
EC_FUNC float abv2_solve_t_dark( float g, float D )
{
	float gg = clamp( g, 1.001f, ABV2_G_MAX );
	float d  = max( D, ABV2_D_MIN );
	float p  = 1.0f / ( gg - 1.0f );   // positive: gg > 1
	float denom = pow( d, p ) - 1.0f;
	return 1.0f / max( denom, ABV2_T_EPS );
}

// The darkening curve itself. Always toe-shaped (the "monitor" knee mirror
// the design doc sketches as an option is NOT built here -- a darkening
// knee is a structurally different curve than this toe mirror, and this
// shipped the plan's own escape hatch instead: "if that is genuinely
// awkward, make Knee use the toe-mirror for darkening and say so" -- this
// comment is the "say so"; see shader-effects.md's "Darkening" section).
// Guards both degenerate "no darken" cases as an EXACT identity, same
// pattern as abv2_toe(): g <= 1 (no darkening exponent requested) or
// D <= 1 (Max darken at its floor) each alone disable it.
EC_FUNC float abv2_toe_dark( float x, float g, float D )
{
	float xx = clamp( x, 0.0f, 1.0f );
	float gg = clamp( g, 1.0f, ABV2_G_MAX );
	float d  = max( D, ABV2_D_MIN );
	if ( gg <= 1.0f + ABV2_T_EPS || d <= 1.0f + ABV2_T_EPS )
		return xx;
	if ( xx <= 0.0f )
		return 0.0f;
	float t = abv2_solve_t_dark( gg, d );
	return xx * pow( ( xx + t ) / ( 1.0f + t ), gg - 1.0f );
}

// ---- Choosing g_dark: a static floor (unchanged), adaptation re-derived on
// the RESCALED variable (V2 darken QC, 2026-09-15) -------------------------
//
//   g_static_dark = 1 + 0.6 * Darken                       // Darken 0..1 -> 1.0..1.6
//   g             = clamp(max(g_static_dark, g_adapt_z), 1, G_MAX)   // Scene
//   g             = clamp(g_static_dark, 1, G_MAX)                  // Off
//
// `max`, not `min`: adaptation can only make the darken STRONGER on a
// bright scene (a LARGER g here); on a dark scene the static floor is what
// darkens the one bright thing in it (the mirror of plan 3.5's own
// argument), so the floor must never be relaxed by "the scene looks dark".
//
// `g_adapt_z, not the old g_adapt = ln(target)/ln(anchor):` that formula
// solves anchor^g == target directly on RAW x -- correct reasoning for a
// curve whose own domain IS [0,1], wrong once the darken half's own domain
// is the RESCALED w = (1 - anchor) / (1 - target) (the combiner below runs
// f_dark on THAT w, not on anchor itself). Worked example, Target 0.35,
// anchor 0.7 (a scene reading twice Target): the OLD formula gives
// g = ln(0.35)/ln(0.7) = 2.943 -- the exponent for a curve that maps 0.7
// straight to 0.35, which is not what f_dark's own w-domain does with it
// at all. The derivation below solves the equation THIS curve actually
// evaluates:
//
//   w = (1 - anchor) / (1 - target)   // anchor's own position in the
//                                      // darken half's domain -- w == 1 at
//                                      // anchor == target (the pivot, no
//                                      // extra darken needed), w == 0 at
//                                      // anchor == 1 (white, the deepest
//                                      // darken needed)
//   g_adapt_z = ln(w) / ln(1 - target)
//
// chosen so the boundary limits are the ones adaptation actually needs:
// w -> 1 (anchor -> target) gives g_adapt_z -> 0, LESS than 1, so
// max(g_static_dark, g_adapt_z) falls back to the static floor right at
// the pivot -- "no adaptive darken needed, the scene IS at Target" -- and
// w -> 0 (anchor -> white) gives g_adapt_z -> +infinity, saturating at
// ABV2_G_MAX through the outer clamp -- "as much darken as this shape
// allows" at the brightest a scene can read. (Same anchor 0.7 / Target
// 0.35: w = 0.4615, g_adapt_z = ln(0.4615)/ln(0.65) = 1.795 -- already a
// meaningfully deeper exponent than the static-only floor at Darken 0, and
// it keeps climbing toward ABV2_G_MAX as anchor approaches 1, unlike the
// old formula's g = 2.943, which was neither derived from nor bounded by
// this curve's own domain.) Monotone in anchor by construction (w is
// monotone in anchor; ln is monotone; dividing by the FIXED negative
// ln(1 - target) flips the sign back to increasing) -- checked
// exhaustively in the unit tests below alongside the lift-side mirror.
//
// `Which direction is "attack":` unchanged from the first cut -- the
// anchor's own EMA (cs_effects_measure.comp) still uses Adapt speed
// (tau_up) while the anchor RISES and its 2x (tau_down) while it FALLS. A
// rising anchor is "brighten": it both takes the lift OFF (relaxes toward
// its own neutral) and puts the darken ON (deepens away from its own
// neutral) at the SAME tau_up, so "attack" is "the anchor is rising"
// whichever operator is the one actually engaging, and "decay" (tau_down,
// 2x slower) is the anchor falling back. No second, independent speed.
EC_FUNC float abv2_g_static_dark( float darken )
{
	return 1.0f + 0.6f * clamp( darken, 0.0f, 1.0f );
}

// The rescaled aim itself, split out so the unit tests can sweep it alone.
// See the derivation above for why w, not raw anchor, is the input, and why
// the two clamps below are not cosmetic: an anchor AT OR BELOW target (this
// half's own domain does not cover it -- the lift half does) makes w >= 1,
// clamped to just under 1, which saturates g_adapt_z toward 0 -- exactly
// the "defer to static" limit, so calling this with an anchor on the wrong
// side of Target is harmless, not undefined.
EC_FUNC float abv2_g_adapt_dark_z( float anchorSmoothed, float target )
{
	float t    = clamp( target, 0.01f, 0.99f );
	float a    = clamp( anchorSmoothed, 0.0001f, 0.9999f );
	float span = max( 1.0f - t, ABV2_T_EPS );
	float w    = clamp( ( 1.0f - a ) / span, ABV2_T_EPS, 1.0f - ABV2_T_EPS );
	return log( w ) / log( span );
}

EC_FUNC float abv2_g_dark( float darken, float target, float anchorSmoothed, bool bSceneMode )
{
	float gStatic = clamp( abv2_g_static_dark( darken ), 1.0f, ABV2_G_MAX );
	if ( !bSceneMode )
		return gStatic;
	float gAdapt = abv2_g_adapt_dark_z( anchorSmoothed, target );
	return clamp( max( gStatic, gAdapt ), 1.0f, ABV2_G_MAX );
}

// ---- The lift half's OWN scene aim, re-derived the same way (V2 darken QC,
// 2026-09-15) -- the mirror of abv2_g_adapt_dark_z() above and for the SAME
// reason: once the lift half is rescaled into [0, Target] (below), its own
// scene term must be solved on z = anchor / target, not on raw anchor, or
// it under-shoots. Worked example, Target 0.35, anchor 0.05 (a scene a
// fourteenth of Target): the OLD g = ln(0.35)/ln(0.05) = 0.350 fed straight
// into the RESCALED curve gives F(anchor) = 0.35 * (0.05/0.35)^0.35 ~=
// 0.177, not the ~0.35 the old formula's own equation (anchor^g == target)
// was solving for -- the QC's "does not land the anchor near Target" on
// this half too. z = anchor / target has the same two boundary limits as
// w above, mirrored: z -> 1 (anchor -> target) gives g_adapt_z ->
// +infinity, i.e. "defer to static" through min(); z -> 0 (anchor ->
// black) gives g_adapt_z -> 0, i.e. ABV2_G_MIN through the outer clamp,
// "as much lift as this shape allows" at the darkest a scene can read.
//
// `Why this is a NEW function and not a change to abv2_g() above:` abv2_g()
// still feeds the OLD, un-rescaled path directly whenever Max darken is at
// its floor (the byte-identical guarantee -- see abv2_curve2() below).
// abv2_g_lift_scurve() below is what the compute shader calls instead once
// Max darken is active; it falls back to abv2_g() itself byte-for-byte when
// it is not, so there is exactly one formula for the old regime and one
// additional one for the new. FIXED (V2 darken QC, 2026-09-15):
// Overlay/EffectPreviewMath.h's split-screen preview now calls
// abv2_g_lift_scurve() too, so the CPU preview and the GPU agree on this
// half's aim whenever darkening is active, instead of the preview's own
// (slightly different) plain-abv2_g() approximation this used to note as a
// documented gap.
EC_FUNC float abv2_g_adapt_lift_z( float anchorSmoothed, float target )
{
	float t = clamp( target, 0.01f, 0.99f );
	float a = clamp( anchorSmoothed, 0.0001f, 0.9999f );
	float z = clamp( a / t, ABV2_T_EPS, 1.0f - ABV2_T_EPS );
	return log( t ) / log( z );
}

EC_FUNC float abv2_g_lift_scurve( float lift, float target, float anchorSmoothed, bool bSceneMode, float D )
{
	if ( D <= ABV2_D_MIN + ABV2_T_EPS )
		return abv2_g( lift, target, anchorSmoothed, bSceneMode );   // byte-identical delegate
	float gStatic = clamp( abv2_g_static( lift ), ABV2_G_MIN, 1.0f );
	if ( !bSceneMode )
		return gStatic;
	float gAdapt = abv2_g_adapt_lift_z( anchorSmoothed, target );
	return clamp( min( gStatic, gAdapt ), ABV2_G_MIN, 1.0f );
}

// ---- THE FIXED POINT (2026-09-15 redesign): lift below Target rescaled
// into [0, Target], darken above it rescaled into [Target, 1], identity
// EXACTLY at Target ---------------------------------------------------------
//
//   x <= Target:  F(x) = Target * L(x / Target)
//   x >  Target:  F(x) = Target + (1 - Target) *
//                        K( (x - Target) / (1 - Target) ; gDark, D )
//
// where L(x) = abv2_curve(x, gLift, SLift, bKnee) (the existing toe/knee,
// untouched) and K = abv2_toe_dark (also untouched). Both halves are the
// SAME primitives as the first cut -- what changed is that L now runs on
// x / Target, a curve of its OWN, rather than on x itself continued past
// Target: L(1) == 1 by construction, so F(Target) == Target * 1 == Target
// EXACTLY (not L(Target), which was >= Target -- the first cut's own pivot
// gap, gone by construction rather than merely bounded). The x > Target
// branch is unchanged in SHAPE (K on a rescale of the excess above the
// pivot) but now rescales the excess against (1 - Target) directly rather
// than against (1 - L(Target)); K(0) == 0 for any g, D, so it too gives
// exactly Target at x == Target.
//
// `Why rescaling the LIFT half is unavoidable once the pivot must be
// Target itself, not the first cut's L(Target):` L is concave with
// L(x) >= x, so unless L is itself the identity (Lift 0), continuing L past
// Target and starting K's rescale from L(Target) was the only way to keep
// x <= Target's own values byte-identical to the single-sided curve. Moving
// the pivot down to Target instead requires the x <= Target branch to land
// ON Target at x == Target, which the un-rescaled L does not do
// (L(Target) > Target for any real lift) -- so a fixed point at Target and
// byte-identical un-rescaled shadows are mutually exclusive once darkening
// is genuinely active. This redesign picks the fixed point (the explicit
// ask) and pays for it with the OFF SWITCH below instead of with the old
// pivot gap.
//
// THE BOUND, both halves, exactly:
//   x <= Target:  F(x)/x = Target * L(z) / x = L(z)/z  (z = x/Target)
//                 <= S     -- L's own secant bound, inherited unchanged
//                 through the rescale (L(z) <= S*z for all z in [0,1], so
//                 Target*L(z) <= Target*S*z == S*x).
//   x >  Target:  F(x) <= x     -- K(z) <= z (K convex, matching endpoints
//                 -- the SAME Jensen argument as the un-rescaled f_dark
//                 above, now on z = (x-Target)/(1-Target)), so
//                 F(x) = Target + span*K(z) <= Target + span*z == x. And
//                 F(x)/x >= 1/D by the same non-decreasing-secant argument,
//                 tight in the limit at x -> Target+ -- so nothing above
//                 Target is ever pushed darker than a factor of D below its
//                 raw value, and nothing above Target can exceed its own
//                 raw value any more (the first cut's "never below raw"
//                 gap -- closed).
//
// THE OFF SWITCH, exactly (the backward-compatibility rule): D <= 1 (Max
// darken at its own floor) returns abv2_curve(x, gLift, SLift, bKnee)
// DIRECTLY -- the plain, un-rescaled single-sided curve, not a limit of the
// two-sided one. `Why gate on D alone, not also on Darken:` abv2_toe_dark()
// already returns the exact identity whenever D <= 1 REGARDLESS of gDark,
// so the x > Target branch already telescopes to F(x) == x there for any
// gDark -- but the x <= Target branch would STILL be the RESCALED
// Target*L(x/Target), which is NOT byte-identical to the un-rescaled L(x)
// whenever Lift > 0. Gating the WHOLE dispatch on D, not just K, is what
// keeps the shadows byte-identical too -- the stated requirement is "Max
// darken 1 AND Darken 0"; gating on D alone is a STRICTLY STRONGER
// guarantee (Darken's own value cannot matter once D has already turned
// the whole feature off), which subsumes the stated requirement rather
// than merely meeting it. Verified in the unit tests below as an exact
// float comparison against abv2_curve(), not an approximation.
//
// CONTINUITY: C0 by construction (both branches give exactly Target at
// x == Target, not merely close). C1 is NOT achieved when darkening is
// active: the slope arriving at the pivot from below is L'(1) (the
// toe/knee's OWN highlight slope, x/Target having reached 1), and the
// slope leaving it above is K'(0) == 1/D exactly -- a kink whose RATIO is
// (1/D) / L'(1), not the first cut's plain 1/D (L'(1) is a different point
// on L than the first cut's L'(Target), since the rescale moves WHERE on L
// the pivot's left slope is read from). Measured at the shipped Lift 0.5
// (g_lift 0.7, Scene off) / Max lift 4 / Darken 0.5 (g_dark 1.3) / Max
// darken 2 / Target 0.35: left slope 0.7030 (L'(1), matching this file's
// own f'(1) ~= g approximation), right slope 0.5000 (1/D exactly), ratio
// 0.7113 -- CLOSER to 1 (less discontinuous) than the first cut's own
// plain-1/D ratio of 0.5 at the same D, so the kink this redesign carries
// is, if anything, tighter at these defaults, not looser. (General D, g:
// the ratio is (1/D)/L'(1), and L'(1) < 1 for any real lift, so the new
// ratio is always LARGER, i.e. closer to 1, than the old plain 1/D.) At
// D == 1 the two slopes match trivially (K'(0) == 1), because the whole
// dispatch has already returned the un-rescaled L, whose own continuity is
// unaffected by any of this.
//
// BOUNDED, MONOTONE: F is a monotone rescale of L (x <= Target, L itself
// monotone, L(0) == 0, L(1) == 1, so Target*L(x/Target) covers [0, Target]
// monotonically) composed with a monotone rescale of K (x > Target,
// likewise covering [Target, 1] monotonically) -- checked exhaustively in
// the unit tests below.
EC_FUNC float abv2_curve2( float x, float gLift, float SLift, bool bKnee,
                            float gDark, float D, float target )
{
	float xx = clamp( x, 0.0f, 1.0f );
	if ( D <= ABV2_D_MIN + ABV2_T_EPS )
		return abv2_curve( xx, gLift, SLift, bKnee );   // the off switch -- see above
	float tgt = clamp( target, 0.001f, 0.999f );
	if ( xx <= tgt )
		return tgt * abv2_curve( xx / tgt, gLift, SLift, bKnee );
	float span = max( 1.0f - tgt, 1e-4f );
	float z    = clamp( ( xx - tgt ) / span, 0.0f, 1.0f );
	float K    = abv2_toe_dark( z, gDark, D );
	return clamp( tgt + span * K, 0.0f, 1.0f );
}

// ---- Base/detail (Stage 2, plan 4.5): the secant and the soft shoulder ---
//
// The secant f(B)/B, clamped to S -- what a per-pixel curve would apply to a
// region's OWN texture if it scaled it by the tangent (flattening it); using
// the secant instead preserves the region's Weber contrast exactly, which is
// the entire content of "local tone mapping" at these compression ratios
// (plan 4.5's worked numbers). Guarded at B near 0 (where the secant's own
// limit is f'(0) == S) so a divide by a near-zero base never produces a
// stray large or NaN value the shoulder would then have to absorb.
EC_FUNC float abv2_secant( float B, float g, float S, bool bKnee )
{
	if ( B <= 1e-4f )
		return S;
	return min( abv2_curve( B, g, S, bKnee ) / B, S );
}

// The two-sided curve's own secant. The clamp below still reads
// `max(SLift, D)` (unchanged code -- a SAFE, if no longer the tightest,
// ceiling), but the redesigned combiner's own bound is tighter than that
// expression suggests: F(B)/B <= S everywhere now, on BOTH sides of
// Target, not only below it -- below Target from L's own secant bound,
// inherited unchanged through the rescale (see THE BOUND above); above
// Target because F(B) <= B itself there (the fixed-point redesign's own
// guarantee), so F(B)/B <= 1 <= S always, S being at least 1 by Max lift's
// own range floor. So "S * Detail" is the bound that actually holds now,
// not "max(S, D) * Detail" -- the latter is simply a looser number the
// same clamp expression also happens to satisfy, kept as-is because
// changing the clamp buys nothing (S alone already bounds it). A darkened
// region's detail SHRINKS in step with its own base: F(B)/B <= 1 there, so
// this function's sec factor is at most Detail on the darkening side,
// proportionally smaller the darker the region gets (down to
// sec >= Detail/D at the darkest, from the same non-decreasing-secant
// argument as F(x) >= x/D above). Byte-identical to abv2_secant() at
// D <= 1: max(SLift, 1) == SLift (Max lift's own range floor is 1), and
// abv2_curve2() itself already telescopes to abv2_curve() there, so this
// returns exactly the same number the one-sided function does.
EC_FUNC float abv2_secant2( float B, float gLift, float SLift, bool bKnee,
                             float gDark, float D, float target )
{
	if ( B <= 1e-4f )
		return SLift;
	float F = abv2_curve2( B, gLift, SLift, bKnee, gDark, D, target );
	return min( F / B, max( SLift, D ) );
}

// The Reinhard-shaped soft shoulder on POSITIVE detail only (plan 4.5):
// D <= 0 needs nothing (sec <= f(B)/B keeps u >= -f(B), so f(B) + u >= 0
// already); D > 0 could push f(B) + u past 1, and this maps any u >= 0 into
// the remaining headroom h = 1 - f(B) with unit slope at u = 0 (so it is
// invisible wherever nothing would clip) and asymptotes to h (never
// reaching 1) as u grows. h <= 0 (the base is already at 1) can lift
// nothing further, so the shoulder is 0 there rather than a 0/0.
EC_FUNC float abv2_shoulder( float u, float h )
{
	if ( h <= 1e-4f )
		return 0.0f;
	float uu = max( u, 0.0f );
	return uu * h / ( h + uu );
}

// One channel's detail step, base already through the curve (plan 4.5):
// D = Y - B; positive detail gets the shoulder, negative does not (it
// cannot go below -f(B), i.e. Y' cannot go negative, because sec <= f(B)/B).
EC_FUNC float abv2_detail_apply( float fB, float D, float sec )
{
	float u = D * sec;
	if ( D > 0.0f )
		return fB + abv2_shoulder( u, 1.0f - fB );
	return fB + u;
}

// ---- Stage 3 Clarity (plan 4.9): the silhouette band ---------------------
//
// M = B2 - B, the difference between two guided-filter bases at two radii
// (r and r/4 -- cs_effects_layer0.comp computes M from the two coefficient
// samples; this is the part that is a pure function of the two numbers).
// Folded into ONE combined detail signal ALONGSIDE D, before the shoulder
// -- not through a second, independent abv2_detail_apply() call -- so
// guarantee 4 (halo-bounded) is inherited rather than re-derived: the
// shoulder does not know or care whether its input came from one region's
// texture or two guided filters' disagreement, so the same range and
// monotonicity proof abv2_detail_apply() already carries covers Dtotal
// exactly as it covers D alone.
//
// clarity in [0, 1]; clamped here (not just by the caller) so a hand-typed
// config value outside the panel's own range cannot exceed the guarantee
// this function exists to keep: clarity == 0 makes this an EXACT identity
// on D (byte-identical to Stage 2, not an approximation of one), and
// clarity == 1 is the worst case the "S * Detail * (1 + Clarity)" guarantee
// in shader-effects.md is stated against (D and M at their own extremes,
// same sign, summing rather than partially cancelling).
EC_FUNC float abv2_clarity_combine( float D, float M, float clarity )
{
	return D + M * clamp( clarity, 0.0f, 1.0f );
}

// ---- The black floor (plan 4.6) ------------------------------------------
//
// B <= ABV2_BLACK: Y' = Y exactly, never touched by the curve at all. The
// toe already keeps these near-untouched (at most S * BLACK further from
// zero), but a HARD floor is what makes literal black, letterbox bars and
// 1-code dither EXACTLY unchanged rather than "close" -- see the header
// note on ab_dyn's own reasoning for why a floor is a separate guarantee
// from a bounded slope, not a consequence of one.
EC_FUNC bool abv2_is_void( float B )
{
	return B <= ABV2_BLACK;
}

// ---- The binding readout (plan 4.11) -------------------------------------
//
// Which of the effect's own controls is the one actually holding the
// picture back right now -- the same "a clamped slider looks exactly like a
// working one" lesson ab_dyn_binding()/ag_binding() above exist for. Four
// codes here, not the plan's five: VOID and NONE/LIFT_FLOOR/G_MIN classify
// cleanly from g_static vs g_adapt alone (exactly what the plan's wording
// for each of them describes), but the plan's fifth code, MAX_LIFT, would
// require attributing a LIMIT to the slope cap S -- and S does not clamp g
// at all (it is an independent shape parameter, not a second ceiling on the
// same exponent), so there is no clean "S is what's binding" test to write
// that could not just be NONE with a different S already applied. Left out
// rather than faked; CUT is reported as its own fact (see the panel row),
// not folded into this enum, because a scene cut is a one-frame EVENT, not
// a standing limit the way the other three are.
const int ABV2_BIND_NONE       = 0;   // g_adapt is what's binding -- Target does the work
const int ABV2_BIND_LIFT_FLOOR = 1;   // g_static (Lift's own floor) is stronger than Target asks for
const int ABV2_BIND_G_MIN      = 2;   // the internal floor -- effectively "as much lift as this shape allows"
const int ABV2_BIND_VOID       = 3;   // under 1% of the frame is above the black floor -- anchor frozen

EC_FUNC int abv2_binding( float lift, float target, float anchorSmoothed, bool bSceneMode, bool bVoid )
{
	if ( bVoid )
		return ABV2_BIND_VOID;
	float gStatic = clamp( abv2_g_static( lift ), ABV2_G_MIN, 1.0f );
	if ( !bSceneMode )
		return gStatic <= ABV2_G_MIN + 1e-4f ? ABV2_BIND_G_MIN : ABV2_BIND_LIFT_FLOOR;
	float gAdapt = abv2_g_adapt( anchorSmoothed, target );
	float g = min( gStatic, gAdapt );
	if ( g <= ABV2_G_MIN + 1e-4f )
		return ABV2_BIND_G_MIN;
	return ( gStatic <= gAdapt ) ? ABV2_BIND_LIFT_FLOOR : ABV2_BIND_NONE;
}

// The darkening mirror (2026-09-14), additive: abv2_binding() above is
// UNCHANGED and still the whole answer whenever the scene is not in a
// darken-dominant state (g_adapt <= 1). This is the Darken-side twin,
// called by the host only when g_adapt > 1 (the bright-scene case) --
// PanelShaders.cpp's Diagnostics row still reports the lift-side reading
// today; this exists for the unit tests and for a later panel pass to wire
// in, per shader-effects.md's own note on the scope of this change.
//
// FIXED (V2 darken QC, 2026-09-15 S-curve redesign): now calls
// abv2_g_adapt_dark_z(), the SAME rescaled aim abv2_g_dark() itself runs on
// (see that function above) -- so this diagnostic classifies a frame
// exactly the way the curve that actually ran on it did, instead of the
// OLD, un-rescaled abv2_g_adapt() this used to call. Still unwired to the
// panel today (see the paragraph above); informational only until a later
// pass wires the dark side into the Diagnostics row.
const int ABV2_BIND_DARKEN_FLOOR = 4;   // g_static_dark (Darken's own floor) is stronger than Target asks for
const int ABV2_BIND_G_MAX        = 5;   // the internal ceiling -- "as much darken as this shape allows"

EC_FUNC int abv2_binding_dark( float darken, float target, float anchorSmoothed, bool bSceneMode, bool bVoid )
{
	if ( bVoid )
		return ABV2_BIND_VOID;
	float gStatic = clamp( abv2_g_static_dark( darken ), 1.0f, ABV2_G_MAX );
	if ( !bSceneMode )
		return gStatic >= ABV2_G_MAX - 1e-4f ? ABV2_BIND_G_MAX : ABV2_BIND_DARKEN_FLOOR;
	float gAdapt = abv2_g_adapt_dark_z( anchorSmoothed, target );
	float g = max( gStatic, gAdapt );
	if ( g >= ABV2_G_MAX - 1e-4f )
		return ABV2_BIND_G_MAX;
	return ( gStatic >= gAdapt ) ? ABV2_BIND_DARKEN_FLOOR : ABV2_BIND_NONE;
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

// The same, for Adaptive Gamma's ag_binding() codes. A separate wording
// because the controls have different names: naming "Max gain" on a row
// that has no such slider is exactly the kind of near-miss that sent a
// user hunting for a control that was not there.
inline const char *ag_binding_text( int nBinding )
{
	switch ( nBinding )
	{
		case AG_BIND_LIFT:     return "Max lift -- Target brightness does no more here";
		case AG_BIND_DARKEN:   return "Max darken -- Target brightness does no more here";
		case AG_BIND_STRENGTH: return "Strength is 0 -- nothing is applied";
		default:               return "none -- the mid-tones are on Target brightness";
	}
}

// Adaptive Brightness V2's own wording (abv2_binding() above). "Scene
// void"/"floor" are this row's own vocabulary, not AB's/AG's, because
// nothing in this effect is called a gain or a gamma from the user's side --
// Lift and Max lift are the names on the row.
inline const char *abv2_binding_text( int nBinding )
{
	switch ( nBinding )
	{
		case ABV2_BIND_LIFT_FLOOR: return "Lift -- Target brightness does no more here";
		case ABV2_BIND_G_MIN:      return "as much lift as this shape allows";
		case ABV2_BIND_VOID:       return "scene mostly void -- holding the last reading";
		default:                   return "none -- the content median is on Target brightness";
	}
}

// abv2_binding_dark()'s own wording (2026-09-14), the darkening mirror.
inline const char *abv2_binding_dark_text( int nBinding )
{
	switch ( nBinding )
	{
		case ABV2_BIND_DARKEN_FLOOR: return "Darken -- Target brightness does no more here";
		case ABV2_BIND_G_MAX:        return "as much darken as this shape allows";
		case ABV2_BIND_VOID:         return "scene mostly void -- holding the last reading";
		default:                     return "none -- the content median is on Target brightness";
	}
}
} // namespace gamescope::effects_curve
#endif
#undef EC_FUNC

#endif // EFFECTS_CURVE_H_
