// Unit tests for src/shaders/effects_curve.h -- Adaptive Brightness's
// Dynamic-mode tone curve, evaluated on the CPU from the SAME header text
// the GPU compiles (superdoc/features/shader-effects.md, "Dynamic"). The
// properties below are the curve's contract with the user: nothing blows
// out, black stays black, more input never gives less output, and strength
// 0 is the untouched picture. Plus the config round-trip of the new `mode`
// key, kept here rather than in test_config.cpp so the two do not collide.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "shaders/effects_curve.h"
#include "Config/ConfigManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

using namespace gamescope::effects_curve;
using Catch::Matchers::WithinAbs;

namespace
{
	// The three reference scenes (encoded p2 / p50 / p98) the shader's
	// statistics would report for scripts/effects-regression.sh's client,
	// and a set of gain bounds spanning the panel's ranges.
	struct Scene { float p2, p50, p98; };
	constexpr Scene kDark   = { 5.0f / 255.0f, 10.0f / 255.0f, 20.0f / 255.0f };
	constexpr Scene kBright = { 30.0f / 255.0f, 225.0f / 255.0f, 250.0f / 255.0f };
	constexpr Scene kMid    = { 0.1f, 0.5f, 0.9f };

	constexpr float kTarget = 0.5f;
	// The schema defaults (src/Config/ConfigSchema.h), widened 2026-09-07
	// from 0.5/2.0 to 0.3/4.0 -- see shader-effects.md for the re-measured
	// numbers these defaults produce on the three reference scenes.
	constexpr float kMinGain = 0.3f;
	constexpr float kMaxGain = 4.0f;

	// Whole curve for one channel value, with the dry/wet mix the shader
	// applies (mix(c, graded, strength)). `flLocal` / `flRatio` mirror
	// cs_effects_layer0.comp's Local adaptation: the frame's percentiles are
	// SHIFTED by ab_local_shift(local_mean, global_mean, local_strength)
	// before the same two curve parameters are derived from them. flRatio
	// stands in for local_mean / global_mean directly, so a test can sweep
	// the ratio without inventing a pair of means for every case.
	float ApplyLocal( float x, const Scene &sc, float flStrength, float flRatio, float flLocal,
	                  float flTarget = kTarget, float flMin = kMinGain, float flMax = kMaxGain )
	{
		// A ratio, expressed as a (local, global) pair the operator could
		// actually see: both means live in 0..1, so a global mean of 0.2
		// lets flRatio sweep the whole 0.05..8 range below without
		// ab_local_ratio()'s own clamp(localMean, 0, 1) truncating it first.
		const float r = ab_local_shift( flRatio * 0.2f, 0.2f, flLocal );
		const float gain  = ab_dyn_gain( sc.p98 * r, sc.p50 * r, flTarget, flMin, flMax );
		const float gamma = ab_dyn_gamma( sc.p2 * r, sc.p50 * r, gain, flTarget, flMin, flMax );
		const float y = ab_dyn_curve( x, gain, gamma );
		return x + ( y - x ) * flStrength;
	}

	float Apply( float x, const Scene &sc, float flStrength,
	             float flTarget = kTarget, float flMin = kMinGain, float flMax = kMaxGain )
	{
		return ApplyLocal( x, sc, flStrength, 1.0f, 0.0f, flTarget, flMin, flMax );
	}

	// The local ratios the operator can actually produce, plus both clamp
	// ends and 1.0 (the "this neighbourhood is average" no-op).
	const float kLocalRatios[] = { 0.05f, 0.25f, 0.36f, 0.5f, 1.0f, 1.72f, 2.5f, 4.0f, 8.0f };
	const float kLocalStrengths[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

	// Every scene / bound / target combination the properties are checked over.
	const Scene kScenes[] = { kDark, kBright, kMid,
		{ 0.0f, 0.0f, 0.0f },          // all black
		{ 1.0f, 1.0f, 1.0f },          // all white
		{ 0.02f, 0.3f, 0.99f },        // dark room, bright window
		{ 0.6f, 0.8f, 0.85f } };       // flat and bright
	const float kTargets[]  = { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f };
	// Spans the full 2026-09-07 panel ranges (0.3..1.0 / 1.0..4.0), including
	// both new extremes, not just the pre-widening set.
	const float kMinGains[] = { 0.3f, 0.5f, 0.75f, 1.0f };
	const float kMaxGains[] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
}

TEST_CASE( "dynamic curve: output never exceeds 1.0 and never goes negative", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lo : kMinGains )
				for ( float hi : kMaxGains )
					for ( int i = 0; i <= 1000; i++ )
					{
						const float x = i / 1000.0f;
						const float y = Apply( x, sc, 1.0f, t, lo, hi );
						REQUIRE( y <= 1.0f );
						REQUIRE( y >= 0.0f );
						REQUIRE( std::isfinite( y ) );
					}
}

TEST_CASE( "dynamic curve: monotonic in the input for every scene and bound", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lo : kMinGains )
				for ( float hi : kMaxGains )
				{
					float flPrev = Apply( 0.0f, sc, 1.0f, t, lo, hi );
					for ( int i = 1; i <= 1000; i++ )
					{
						const float y = Apply( i / 1000.0f, sc, 1.0f, t, lo, hi );
						REQUIRE( y >= flPrev - 1e-6f );
						flPrev = y;
					}
				}
}

TEST_CASE( "dynamic curve: black stays black, and x = 1 reaches exactly 1 when the curve overshoots", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
		{
			REQUIRE_THAT( Apply( 0.0f, sc, 1.0f, t ), WithinAbs( 0.0f, 1e-7f ) );

			const float gain  = ab_dyn_gain( sc.p98, sc.p50, t, kMinGain, kMaxGain );
			const float gamma = ab_dyn_gamma( sc.p2, sc.p50, gain, t, kMinGain, kMaxGain );
			if ( std::pow( gain, gamma ) > 1.0f )
				REQUIRE_THAT( ab_dyn_curve( 1.0f, gain, gamma ), WithinAbs( 1.0f, 1e-5f ) );
		}
}

TEST_CASE( "dynamic curve: identity at strength 0", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( int i = 0; i <= 255; i++ )
		{
			const float x = i / 255.0f;
			REQUIRE_THAT( Apply( x, sc, 0.0f ), WithinAbs( x, 1e-7f ) );
		}
}

TEST_CASE( "dynamic curve: a mid scene whose p98 already sits at WHITE is the identity", "[effects_curve]" )
{
	// gain = 0.9 / 0.9 = 1, gamma = ln 0.5 / ln 0.5 = 1, top = 1 -> no shoulder.
	for ( int i = 0; i <= 255; i++ )
	{
		const float x = i / 255.0f;
		REQUIRE_THAT( Apply( x, kMid, 1.0f ), WithinAbs( x, 1e-5f ) );
	}
}

TEST_CASE( "dynamic curve: the dark reference scene is lifted, its highlights compressed, not clipped", "[effects_curve]" )
{
	// The numbers scripts/effects-regression.sh measures off the GPU; the
	// tolerances there are what the capture path adds, these are exact.
	const float p2   = Apply( kDark.p2,  kDark, 1.0f ) * 255.0f;
	const float p50  = Apply( kDark.p50, kDark, 1.0f ) * 255.0f;
	const float hi   = Apply( 240.0f / 255.0f, kDark, 1.0f ) * 255.0f;
	const float wht  = Apply( 1.0f, kDark, 1.0f ) * 255.0f;
	// At max_gain 4.0 (widened from 2.0, 2026-09-07) the darkest band lifts
	// well clear of the "readable" floor, and further still since 2026-09-08:
	// the gamma floor is 1 / max_gain = 0.25 here, so the exponent the
	// median asks for (0.374) is no longer clamped up to 0.5.
	REQUIRE( p2 > 65.0f );          // the 2 % percentile becomes readable
	REQUIRE( p50 > p2 );
	REQUIRE( hi < 255.0f );         // a 240 highlight is not blown out
	REQUIRE( hi > p50 );            // and keeps its rank
	REQUIRE( wht <= 255.0f );
	REQUIRE( hi < wht );            // 240 and 255 stay distinguishable
	// The lift is bounded: the gain is max_gain (the white point alone asks
	// for 11.5 here) and the gamma sits between its floor and 1.
	REQUIRE_THAT( ab_dyn_gain( kDark.p98, kDark.p50, kTarget, kMinGain, kMaxGain ),
	              WithinAbs( kMaxGain, 1e-6f ) );
	const float gDark = ab_dyn_gamma( kDark.p2, kDark.p50, kMaxGain, kTarget, kMinGain, kMaxGain );
	REQUIRE( gDark >= ab_gamma_min( kMaxGain ) - 1e-6f );
	REQUIRE( gDark < 1.0f );
}

TEST_CASE( "dynamic curve: the bright reference scene is dimmed and its shadows are not crushed", "[effects_curve]" )
{
	const float sh   = Apply( 30.0f / 255.0f, kBright, 1.0f ) * 255.0f;
	const float p98  = Apply( kBright.p98, kBright, 1.0f ) * 255.0f;
	const float p50  = Apply( kBright.p50, kBright, 1.0f ) * 255.0f;
	REQUIRE( p98 < 250.0f );
	REQUIRE( p50 < 225.0f );
	// min_gain's Dynamic meaning: the 2nd percentile is never pushed below
	// p2 * min_gain. At min_gain 0.3 (widened from 0.5, 2026-09-07) the
	// floor drops from 15 to 9 -- the shadow cap is real but the margin
	// above "crushed" is now visibly thinner (see the dedicated shadow-cap
	// comparison test below, and shader-effects.md's "Why min_gain 0.3"
	// note, for the numbers this loosening produces).
	REQUIRE( sh >= 30.0f * kMinGain - 0.5f );
	REQUIRE( sh > 8.0f );
}

TEST_CASE( "dynamic curve: min_gain 0.3 loosens the shadow cap relative to 0.5, measurably",
           "[effects_curve]" )
{
	// Same bright reference scene, same gain (unaffected -- 0.918 is inside
	// both old and new bounds), only min_gain differs. Answers the
	// 2026-09-07 request's question directly: does the widened floor let
	// deep shadows crush? The cap still holds (never below p2 * min_gain),
	// but the permitted floor itself moved from 15 to 9 -- both are "not
	// crushed" by the >= 8 counts a human can still distinguish from black,
	// but 9 leaves much less headroom than 15 did.
	constexpr float p2 = 30.0f / 255.0f, p50 = 225.0f / 255.0f, p98 = 250.0f / 255.0f;
	const float gainOld = ab_dyn_gain( p98, p50, 0.5f, 0.5f, 2.0f );
	const float gainNew = ab_dyn_gain( p98, p50, 0.5f, 0.3f, 4.0f );
	REQUIRE_THAT( gainOld, WithinAbs( gainNew, 1e-6f ) );   // 0.918 either way, not clamped

	const float gammaOld = ab_dyn_gamma( p2, p50, gainOld, 0.5f, 0.5f, 2.0f );
	const float gammaNew = ab_dyn_gamma( p2, p50, gainNew, 0.5f, 0.3f, 4.0f );
	REQUIRE( gammaNew > gammaOld );          // the looser cap permits more darkening
	REQUIRE_THAT( gammaNew, WithinAbs( AB_DYN_GAMMA_MAX, 1e-3f ) );   // GAMMA_MAX now binds, not the cap

	const float shOld = ab_dyn_curve( p2, gainOld, gammaOld ) * 255.0f;
	const float shNew = ab_dyn_curve( p2, gainNew, gammaNew ) * 255.0f;
	REQUIRE_THAT( shOld, WithinAbs( 15.0f, 0.5f ) );
	REQUIRE_THAT( shNew, WithinAbs( 9.0f, 0.5f ) );
	// Neither is crushed to black, and the cap invariant holds for both.
	REQUIRE( shOld >= 30.0f * 0.5f - 0.5f );
	REQUIRE( shNew >= 30.0f * 0.3f - 0.5f );
}

TEST_CASE( "dynamic curve: the gamma is clamped to its bounds and the shadow cap holds", "[effects_curve]" )
{
	// Way below target: the floor, which is 1 / max_gain since 2026-09-08.
	REQUIRE_THAT( ab_dyn_gamma( 0.001f, 0.002f, 2.0f, 0.9f, 0.5f, 2.0f ),
	              WithinAbs( ab_gamma_min( 2.0f ), 1e-6f ) );
	REQUIRE_THAT( ab_dyn_gamma( 0.001f, 0.002f, 4.0f, 0.9f, 0.5f, 4.0f ),
	              WithinAbs( AB_DYN_GAMMA_MIN, 1e-6f ) );
	// Way above target with bright shadows: ceiling.
	REQUIRE_THAT( ab_dyn_gamma( 0.9f, 0.95f, 1.0f, 0.1f, 0.5f, 4.0f ), WithinAbs( AB_DYN_GAMMA_MAX, 1e-6f ) );
	// Above target with dark shadows: the cap keeps p2 * gain ^ g >= p2 * min_gain.
	const float p2 = 0.05f, gain = 0.918f, minGain = 0.5f;
	const float g = ab_dyn_gamma( p2, 0.7f, gain, 0.5f, minGain, 4.0f );
	REQUIRE( g > 1.0f );
	REQUIRE( g < AB_DYN_GAMMA_MAX );
	REQUIRE( std::pow( p2 * gain, g ) >= p2 * minGain - 1e-6f );
}

TEST_CASE( "dynamic curve: the shoulder is continuous at the knee and at the top", "[effects_curve]" )
{
	const float gain = 2.0f, gamma = 0.6f;   // top = 2^0.6 = 1.516 > 1
	const float k = AB_DYN_KNEE;
	// Just below the knee is untouched; just above is within a hair of it.
	const float xk = std::pow( k, 1.0f / gamma ) / gain;   // the input that lands on the knee: (xk*gain)^gamma = k
	REQUIRE_THAT( ab_dyn_curve( xk - 1e-4f, gain, gamma ), WithinAbs( std::pow( ( xk - 1e-4f ) * gain, gamma ), 1e-6f ) );
	REQUIRE_THAT( ab_dyn_curve( xk + 1e-4f, gain, gamma ), WithinAbs( k, 2e-4f ) );
	// Slope 1 at the knee: the shoulder adds no visible break.
	const float d = 1e-3f;
	const float slopeBelow = ( std::pow( xk * gain, gamma ) - std::pow( ( xk - d ) * gain, gamma ) ) / d;
	const float slopeAbove = ( ab_dyn_curve( xk + d, gain, gamma ) - ab_dyn_curve( xk, gain, gamma ) ) / d;
	REQUIRE_THAT( slopeAbove / slopeBelow, WithinAbs( 1.0f, 0.05f ) );
}

// ---- config: the `mode` key ----------------------------------------------

namespace
{
	using namespace gamescope::config;

	struct TempConfigHome
	{
		std::filesystem::path dir;
		TempConfigHome()
		{
			std::string sTemplate = ( std::filesystem::temp_directory_path() / "gamescope-ritz-effects-XXXXXX" ).string();
			char *pszResult = mkdtemp( sTemplate.data() );
			REQUIRE( pszResult != nullptr );
			dir = pszResult;
			setenv( "XDG_CONFIG_HOME", dir.c_str(), 1 );
			ResetSessionRoutingForTests();
		}
		~TempConfigHome()
		{
			FlushPendingWrites();
			std::error_code ec;
			std::filesystem::remove_all( dir, ec );
			unsetenv( "XDG_CONFIG_HOME" );
		}
	};
}

TEST_CASE( "reshade.adaptive_brightness.mode defaults to whole_image and round-trips dynamic", "[effects_curve][config]" )
{
	TempConfigHome home;

	Settings s{};
	REQUIRE( s.reshade.adaptive_brightness.mode == "whole_image" );

	s.reshade.adaptive_brightness.enabled = true;
	s.reshade.adaptive_brightness.mode = "dynamic";
	ProfileMeta meta;
	meta.name = "Effects";
	REQUIRE( SaveProfile( meta, s ) );

	std::optional<Settings> loaded = LoadProfile( "Effects" );
	REQUIRE( loaded.has_value() );
	REQUIRE( loaded->reshade.adaptive_brightness.enabled == true );
	REQUIRE( loaded->reshade.adaptive_brightness.mode == "dynamic" );
}

TEST_CASE( "reshade.adaptive_brightness.local_strength defaults to 0.5 and round-trips", "[effects_curve][config]" )
{
	TempConfigHome home;

	Settings s{};
	REQUIRE_THAT( s.reshade.adaptive_brightness.local_strength, WithinAbs( 0.5f, 1e-6f ) );

	s.reshade.adaptive_brightness.local_strength = 0.25f;
	ProfileMeta meta;
	meta.name = "Effects";
	REQUIRE( SaveProfile( meta, s ) );

	std::optional<Settings> loaded = LoadProfile( "Effects" );
	REQUIRE( loaded.has_value() );
	REQUIRE_THAT( loaded->reshade.adaptive_brightness.local_strength, WithinAbs( 0.25f, 1e-6f ) );
}

TEST_CASE( "reshade.adaptive_brightness.mode: an unknown value on disk resolves to whole_image", "[effects_curve][config]" )
{
	TempConfigHome home;

	Settings s{};
	s.reshade.adaptive_brightness.mode = "banana";
	ProfileMeta meta;
	meta.name = "Effects";
	REQUIRE( SaveProfile( meta, s ) );

	std::optional<Settings> loaded = LoadProfile( "Effects" );
	REQUIRE( loaded.has_value() );
	REQUIRE( loaded->reshade.adaptive_brightness.mode == "whole_image" );
}


// ---------------------------------------------------------------------------
// Local adaptation (2026-09-07). The per-pixel shift, and the properties the
// whole curve must keep once every pixel may get its own gain and gamma.
// ---------------------------------------------------------------------------

TEST_CASE( "local shift: strength 0 is exactly 1.0, for every neighbourhood", "[effects_curve]" )
{
	// The identity that makes "Local adaptation 0 %" bit-for-bit the old
	// global path: the apply pass feeds p98 * shift, p50 * shift, p2 * shift
	// into the same two functions, so shift == 1.0 exactly means the same
	// floats reach them.
	for ( float r : kLocalRatios )
	{
		REQUIRE( ab_local_shift( r, 1.0f, 0.0f ) == 1.0f );
		REQUIRE( ab_local_shift( r, 0.2f, 0.0f ) == 1.0f );
		REQUIRE( ab_local_shift( r, 0.9f, -1.0f ) == 1.0f );   // clamped strength
	}
	// And an average neighbourhood is the identity at ANY strength.
	for ( float s : kLocalStrengths )
		REQUIRE_THAT( ab_local_shift( 0.4f, 0.4f, s ), WithinAbs( 1.0f, 1e-6f ) );
}

TEST_CASE( "local shift: bounded by the deviation clamp, monotone in the neighbourhood",
           "[effects_curve]" )
{
	for ( float s : kLocalStrengths )
	{
		float flPrev = -1.0f;
		for ( int i = 0; i <= 400; i++ )
		{
			const float flLocal = i / 400.0f;
			const float v = ab_local_shift( flLocal, 0.25f, s );
			REQUIRE( std::isfinite( v ) );
			// Never outside the clamp, whatever the neighbourhood does.
			REQUIRE( v >= AB_LOCAL_RATIO_MIN - 1e-6f );
			REQUIRE( v <= AB_LOCAL_RATIO_MAX + 1e-6f );
			// A brighter neighbourhood never gives a smaller shift: this is
			// what stops the map's smooth ramp across an edge from turning
			// into a non-monotone ring in the output (halo control).
			REQUIRE( v >= flPrev - 1e-6f );
			flPrev = v;
		}
	}
	// A zero global mean cannot divide by zero.
	REQUIRE( std::isfinite( ab_local_shift( 0.5f, 0.0f, 1.0f ) ) );
	REQUIRE( std::isfinite( ab_local_shift( 0.0f, 0.0f, 1.0f ) ) );
}

TEST_CASE( "local curve: still bounded, monotone and black-preserving at every local strength",
           "[effects_curve]" )
{
	// Everything the global curve promises must survive the shift, because
	// with Local adaptation on EVERY pixel may see a different gain/gamma
	// pair and the promises are per pixel.
	for ( const Scene &sc : kScenes )
		for ( float lo : kMinGains )
			for ( float hi : kMaxGains )
				for ( float r : kLocalRatios )
					for ( float ls : kLocalStrengths )
					{
						float flPrev = -1.0f;
						for ( int i = 0; i <= 200; i++ )
						{
							const float x = i / 200.0f;
							const float y = ApplyLocal( x, sc, 1.0f, r, ls, kTarget, lo, hi );
							REQUIRE( std::isfinite( y ) );
							REQUIRE( y >= 0.0f );
							REQUIRE( y <= 1.0f );
							REQUIRE( y >= flPrev - 1e-6f );
							flPrev = y;
						}
						REQUIRE_THAT( ApplyLocal( 0.0f, sc, 1.0f, r, ls, kTarget, lo, hi ),
						              WithinAbs( 0.0f, 1e-6f ) );
						// Strength 0 is still the untouched picture, local or not.
						REQUIRE_THAT( ApplyLocal( 0.37f, sc, 0.0f, r, ls, kTarget, lo, hi ),
						              WithinAbs( 0.37f, 1e-6f ) );
					}
}

TEST_CASE( "local curve: local strength 0 is the global curve, whatever the neighbourhood",
           "[effects_curve]" )
{
	// In the SHADER this is bit-for-bit identity: ab_local_shift() returns
	// the literal 1.0 and `p98 * 1.0` is exact for every finite float, so
	// ab_dyn_gain/ab_dyn_gamma receive the same bits the global path passes
	// them. Here it is asserted to within a float ulp rather than with ==,
	// because -O3 + LTO is free to contract the two inlined copies of the
	// same expression differently (one with an FMA, one without) and a
	// last-bit difference in the TEST HARNESS would say nothing about the
	// property. ab_local_shift( ., ., 0 ) == 1.0f exactly is asserted above,
	// which is the half of this that is actually about the code.
	for ( const Scene &sc : kScenes )
		for ( float r : kLocalRatios )
			for ( int i = 0; i <= 255; i++ )
			{
				const float x = i / 255.0f;
				REQUIRE_THAT( ApplyLocal( x, sc, 1.0f, r, 0.0f ),
				              WithinAbs( Apply( x, sc, 1.0f ), 1e-6f ) );
			}
}

TEST_CASE( "local curve: a dark neighbourhood is lifted further than the frame's own curve",
           "[effects_curve]" )
{
	// The whole point, as one number. The 50/50 split scene: half the frame
	// dark (bands 5..20), half bright (200..255). Globally the curve sees a
	// mid frame and does almost nothing to either half. Locally the dark
	// half's neighbourhood sits well under the frame mean, so its shift is
	// < 1, p98 * shift is small, and the gain climbs.
	const Scene split = { 8.0f / 255.0f, 128.0f / 255.0f, 250.0f / 255.0f };
	const float flDarkBand = 12.0f / 255.0f;

	const float flGlobal = ApplyLocal( flDarkBand, split, 1.0f, 1.0f, 0.0f );
	const float flLocal  = ApplyLocal( flDarkBand, split, 1.0f, 0.36f, 1.0f );
	INFO( "global " << flGlobal * 255.0f << " local " << flLocal * 255.0f );
	REQUIRE( flLocal > flGlobal );

	// ... and a bright neighbourhood is brought DOWN relative to the same
	// global curve, which is the other half of "both halves serviceable".
	const float flBrightBand = 245.0f / 255.0f;
	REQUIRE( ApplyLocal( flBrightBand, split, 1.0f, 1.72f, 1.0f )
	         < ApplyLocal( flBrightBand, split, 1.0f, 1.0f, 0.0f ) );
}

TEST_CASE( "local curve: the per-pixel gain never leaves the user's bounds", "[effects_curve]" )
{
	// The safety property the deviation clamp buys: Local adaptation
	// redistributes gain WITHIN [min_gain, max_gain], it never widens the
	// range the user set. Same for the gamma clamps.
	for ( const Scene &sc : kScenes )
		for ( float lo : kMinGains )
			for ( float hi : kMaxGains )
				for ( float r : kLocalRatios )
					for ( float ls : kLocalStrengths )
					{
						const float shift = ab_local_shift( r, 1.0f, ls );
						const float gain  = ab_dyn_gain( sc.p98 * shift, sc.p50 * shift, kTarget, lo, hi );
						const float gamma = ab_dyn_gamma( sc.p2 * shift, sc.p50 * shift, gain, kTarget, lo, hi );
						REQUIRE( gain >= lo - 1e-6f );
						REQUIRE( gain <= hi + 1e-6f );
						REQUIRE( gamma >= ab_gamma_min( hi ) - 1e-6f );
						REQUIRE( gamma <= ab_gamma_max( lo ) + 1e-6f );
					}
}


// ---------------------------------------------------------------------------
// The 2026-09-08 ceiling fix. The user's report was *"anything above target
// brightness 0.5 and max gain 2.0 [doesn't] do anything at all"*, and it was
// true: Target reached the picture only through an exponent clamped to a
// fixed [0.5, 1.5], and Max gain only through a white-point demand that a
// realistic frame satisfies at about 2. These pin both halves of the fix.
// ---------------------------------------------------------------------------

namespace
{
	// A dark frame the way a real one measures, not the way a flat band
	// chart does: enough bright content that the white point alone asks for
	// only ~2.0 of gain (0.9 / 0.44), which is exactly what made Max gain an
	// EXACT no-op above 2 before this fix. The numbers are the measured
	// statistics of a dark photographic frame -- see shader-effects.md.
	constexpr Scene kRealDark = { 0.01136f, 0.07596f, 0.43968f };

	float OutCode( const Scene &sc, float t, float lo, float hi, float x )
	{
		const float gain  = ab_dyn_gain( sc.p98, sc.p50, t, lo, hi );
		const float gamma = ab_dyn_gamma( sc.p2, sc.p50, gain, t, lo, hi );
		return ab_dyn_curve( x, gain, gamma ) * 255.0f;
	}
}

TEST_CASE( "gamma bounds come from the user's own gain bounds", "[effects_curve]" )
{
	// max_gain 2.0 reproduces the historical fixed floor exactly -- the
	// anchor that keeps every pre-2026-09-08 measured number reproducible.
	REQUIRE_THAT( ab_gamma_min( 2.0f ), WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE_THAT( ab_gamma_min( 4.0f ), WithinAbs( AB_DYN_GAMMA_MIN, 1e-6f ) );
	// "Do not brighten" really means it, in both channels.
	REQUIRE_THAT( ab_gamma_min( 1.0f ), WithinAbs( 1.0f, 1e-6f ) );
	// The mirror on the darkening side: "do not darken" leaves no exponent
	// above 1 either, and at every shipped default the cap is slack.
	REQUIRE_THAT( ab_gamma_max( 1.0f ), WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( ab_gamma_max( 0.3f ), WithinAbs( AB_DYN_GAMMA_MAX, 1e-6f ) );
	// The pair can never invert, over the whole panel range.
	for ( float lo : kMinGains )
		for ( float hi : kMaxGains )
		{
			REQUIRE( ab_gamma_min( hi ) <= 1.0f + 1e-6f );
			REQUIRE( ab_gamma_max( lo ) >= 1.0f - 1e-6f );
			REQUIRE( ab_gamma_min( hi ) <= ab_gamma_max( lo ) );
		}
}

TEST_CASE( "max_gain 1.0 does not brighten anything, at any target", "[effects_curve]" )
{
	// The lie this fix removed: before, "Max gain 1.0" still let the gamma
	// lift a 20-code band to 71, because the floor was a fixed 0.5.
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( int i = 0; i <= 255; i++ )
			{
				const float x = i / 255.0f;
				REQUIRE( OutCode( sc, t, kMinGain, 1.0f, x ) <= (float)i + 0.5f );
			}
}

TEST_CASE( "Target brightness moves the picture, and says so when it stops", "[effects_curve]" )
{
	// The complaint, as an assertion: on a realistic dark frame at the
	// shipped defaults, raising Target from 0.5 to 0.7 must visibly change
	// the picture. Before the fix the two were bit-identical.
	const float at50 = OutCode( kRealDark, 0.50f, kMinGain, kMaxGain, 20.0f / 255.0f );
	const float at70 = OutCode( kRealDark, 0.70f, kMinGain, kMaxGain, 20.0f / 255.0f );
	REQUIRE( at70 - at50 > 20.0f );

	// And monotone in Target the whole way up to the point where a bound
	// takes over -- which the classifier then names.
	float flPrev = -1.0f;
	for ( float t = 0.10f; t <= 0.751f; t += 0.05f )
	{
		const float v = OutCode( kRealDark, t, kMinGain, kMaxGain, 20.0f / 255.0f );
		REQUIRE( v >= flPrev - 0.5f );
		flPrev = v;
	}
	// Past the reachable range the readout must say the lift limit binds,
	// rather than the picture silently going flat with no explanation.
	REQUIRE( ab_dyn_binding( kRealDark.p2, kRealDark.p50, kRealDark.p98, 0.9f, kMinGain, kMaxGain )
	         == AB_BIND_LIFT );
}

TEST_CASE( "Max gain moves the picture on a realistic dark frame", "[effects_curve]" )
{
	// The other half of the report. This frame's white-point demand is 2.05,
	// so before the fix max_gain 2 / 3 / 4 produced the identical gain and
	// the identical picture: an exact no-op over half the slider.
	const float mg15 = OutCode( kRealDark, kTarget, kMinGain, 1.5f, 5.0f / 255.0f );
	const float mg20 = OutCode( kRealDark, kTarget, kMinGain, 2.0f, 5.0f / 255.0f );
	const float mg40 = OutCode( kRealDark, kTarget, kMinGain, 4.0f, 5.0f / 255.0f );
	REQUIRE( mg20 - mg15 > 15.0f );
	REQUIRE( mg40 - mg20 > 15.0f );   // this is the step that used to be zero
	// Monotone in max_gain on this frame, over the whole panel range.
	float flPrev = -1.0f;
	for ( float hi : kMaxGains )
	{
		const float v = OutCode( kRealDark, kTarget, kMinGain, hi, 5.0f / 255.0f );
		REQUIRE( v >= flPrev - 0.5f );
		flPrev = v;
	}
}

TEST_CASE( "the binding classifier names one limit, and always names something", "[effects_curve]" )
{
	// Every combination classifies, the code is in range, and the wording
	// exists -- the panel row and the effects_ab_log trace both print this
	// one function, so an unnamed code would show as an empty fact.
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lo : kMinGains )
				for ( float hi : kMaxGains )
				{
					const int n = ab_dyn_binding( sc.p2, sc.p50, sc.p98, t, lo, hi );
					REQUIRE( n >= AB_BIND_NONE );
					REQUIRE( n <= AB_BIND_SHADOW );
					REQUIRE( ab_binding_text( n )[0] != '\0' );
				}

	// A frame whose median already sits on the target, inside every bound:
	// nothing binds, and the wording points at Target rather than at a knob
	// that cannot help.
	REQUIRE( ab_dyn_binding( 0.1f, 0.5f, 0.9f, 0.5f, 0.3f, 4.0f ) == AB_BIND_NONE );
	// A flat dark chart wants 11.5x of gain: max_gain binds.
	REQUIRE( ab_dyn_binding( kDark.p2, kDark.p50, kDark.p98, kTarget, kMinGain, kMaxGain )
	         == AB_BIND_GAIN_MAX );
	// The bright reference scene, asked to go far darker than it can: the
	// darkening side, held by the shadow cap / the ceiling.
	const int nBright = ab_dyn_binding( kBright.p2, kBright.p50, kBright.p98, 0.1f, kMinGain, kMaxGain );
	REQUIRE( ( nBright == AB_BIND_DARKEN || nBright == AB_BIND_SHADOW ) );
}

TEST_CASE( "max_gain 2.0 reproduces the pre-2026-09-08 curve exactly", "[effects_curve]" )
{
	// The regression guard for every measured table taken before the fix:
	// at max_gain 2.0 the gamma floor is the historical 0.5, and on a scene
	// whose white point alone saturates the gain, the whole curve is
	// unchanged. (The old code: gain = clamp(0.9/p98, lo, hi), gamma
	// clamped to [0.5, 1.5].)
	for ( const Scene &sc : { kDark, kRealDark } )
	{
		const float gain  = ab_dyn_gain( sc.p98, sc.p50, kTarget, kMinGain, 2.0f );
		const float gamma = ab_dyn_gamma( sc.p2, sc.p50, gain, kTarget, kMinGain, 2.0f );
		const float gainOld = std::min( std::max( 0.9f / sc.p98, kMinGain ), 2.0f );
		REQUIRE_THAT( gain, WithinAbs( gainOld, 1e-6f ) );
		REQUIRE_THAT( gamma, WithinAbs( 0.5f, 1e-6f ) );
	}
}


// ===========================================================================
//  ADAPTIVE GAMMA (2026-09-08) -- the same statistics, one exponent.
//
//  The properties asserted below are exactly the ones the operator's shape
//  promises and Adaptive Brightness's cannot: an exponent on 0..1 has 0 and
//  1 as EXACT fixed points, so black stays black, white stays white, and
//  nothing can clip -- at any setting, without a shoulder. Plus the 2026-09-08
//  ceiling lesson applied to a row where the exponent is the ONLY path from
//  Target to the picture: every limit that can stop it is a user-facing
//  control, and ag_binding() names which one.
// ===========================================================================

namespace
{
	// The panel's own ranges (PanelShaders.cpp), swept in full.
	const float kAgLifts[]   = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
	const float kAgDarkens[] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
	const float kAgLift    = 4.0f;   // == ConfigSchema.h's max_lift default
	const float kAgDarken  = 1.5f;   // == ConfigSchema.h's max_darken default

	// A dim indoor frame -- dark enough to want lifting, light enough that
	// the exponent floor does not bind over Target's whole range. Between
	// kRealDark (a night photograph, where the floor DOES bind above ~0.52)
	// and kMid.
	constexpr Scene kDimRoom = { 0.05f, 0.30f, 0.75f };

	// One channel through the whole operator, with the dry/wet mix the
	// shader applies. flRatio stands in for local_mean / global_mean, the
	// same convention ApplyLocal() above uses.
	float AgApplyLocal( float x, const Scene &sc, float flStrength, float flRatio, float flLocal,
	                    float flTarget = kTarget, float flLift = kAgLift, float flDarken = kAgDarken )
	{
		const float r = ab_local_shift( flRatio * 0.2f, 0.2f, flLocal );
		const float g = ag_gamma( sc.p50 * r, flTarget, flLift, flDarken );
		const float y = ag_curve( x, g );
		return x + ( y - x ) * flStrength;
	}

	float AgApply( float x, const Scene &sc, float flStrength,
	               float flTarget = kTarget, float flLift = kAgLift, float flDarken = kAgDarken )
	{
		return AgApplyLocal( x, sc, flStrength, 1.0f, 0.0f, flTarget, flLift, flDarken );
	}

	float AgOutCode( const Scene &sc, float t, float lift, float darken, float x )
	{
		return AgApply( x, sc, 1.0f, t, lift, darken ) * 255.0f;
	}
}

TEST_CASE( "adaptive gamma: output stays in [0, 1] and finite, at every setting", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lift : kAgLifts )
				for ( float darken : kAgDarkens )
					for ( int i = 0; i <= 1000; i++ )
					{
						const float y = AgApply( i / 1000.0f, sc, 1.0f, t, lift, darken );
						REQUIRE( y <= 1.0f );
						REQUIRE( y >= 0.0f );
						REQUIRE( std::isfinite( y ) );
					}
}

TEST_CASE( "adaptive gamma: monotonic in the input for every scene and bound", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lift : kAgLifts )
				for ( float darken : kAgDarkens )
				{
					float flPrev = AgApply( 0.0f, sc, 1.0f, t, lift, darken );
					for ( int i = 1; i <= 1000; i++ )
					{
						const float y = AgApply( i / 1000.0f, sc, 1.0f, t, lift, darken );
						REQUIRE( y >= flPrev - 1e-6f );
						flPrev = y;
					}
				}
}

TEST_CASE( "adaptive gamma: black stays black and white stays exactly white", "[effects_curve]" )
{
	// Both ends are fixed points of ANY exponent, which is the whole reason
	// this operator needs no shoulder: x = 1 lands on exactly 1 always, not
	// only when the curve happens not to overshoot (Adaptive Brightness has
	// to compress to get there).
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lift : kAgLifts )
				for ( float darken : kAgDarkens )
					for ( float s : { 0.0f, 0.5f, 1.0f } )
					{
						REQUIRE_THAT( AgApply( 0.0f, sc, s, t, lift, darken ), WithinAbs( 0.0f, 1e-6f ) );
						REQUIRE_THAT( AgApply( 1.0f, sc, s, t, lift, darken ), WithinAbs( 1.0f, 1e-6f ) );
					}
}

TEST_CASE( "adaptive gamma: it cannot clip -- near-white highlights stay below white and stay apart",
           "[effects_curve]" )
{
	// The no-clipping property, as the thing a user would actually see: two
	// distinct near-white codes must come out distinct and below 255. A
	// levels gain cannot promise this (Adaptive Brightness's Whole image
	// mode drives 240 to a clipped 255 on the dark reference scene); an
	// exponent can, because it is strictly increasing on [0, 1] and maps 1
	// to 1.
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lift : kAgLifts )
				for ( float darken : kAgDarkens )
					for ( float s : { 0.25f, 0.5f, 1.0f } )
					{
						const float a = AgApply( 245.0f / 255.0f, sc, s, t, lift, darken ) * 255.0f;
						const float b = AgApply( 254.0f / 255.0f, sc, s, t, lift, darken ) * 255.0f;
						REQUIRE( a < 255.0f );
						REQUIRE( b < 255.0f );
						REQUIRE( b > a );
					}
}

TEST_CASE( "adaptive gamma: identity at strength 0", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lift : kAgLifts )
				for ( float darken : kAgDarkens )
					for ( int i = 0; i <= 255; i++ )
					{
						const float x = i / 255.0f;
						REQUIRE_THAT( AgApply( x, sc, 0.0f, t, lift, darken ), WithinAbs( x, 1e-6f ) );
					}
}

TEST_CASE( "adaptive gamma: the exponent bounds are the user's own and can never invert",
           "[effects_curve]" )
{
	// Both are exactly 1.0 at their "do nothing in this direction" end, and
	// the lift floor is the same 0.25 Adaptive Brightness allows at max_gain
	// 4.0 -- one shared constant, not two that can drift.
	REQUIRE_THAT( ag_gamma_min( 1.0f ), WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( ag_gamma_min( 2.0f ), WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE_THAT( ag_gamma_min( 4.0f ), WithinAbs( AB_DYN_GAMMA_MIN, 1e-6f ) );
	REQUIRE_THAT( ag_gamma_max( 1.0f ), WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( ag_gamma_max( 4.0f ), WithinAbs( AG_DARKEN_MAX, 1e-6f ) );
	for ( float lift : kAgLifts )
		for ( float darken : kAgDarkens )
		{
			REQUIRE( ag_gamma_min( lift ) <= 1.0f + 1e-6f );
			REQUIRE( ag_gamma_max( darken ) >= 1.0f - 1e-6f );
			REQUIRE( ag_gamma_min( lift ) <= ag_gamma_max( darken ) );
			// And the panel's range ends are never clipped by the header's
			// own hard limits -- i.e. the slider always reaches the bound it
			// says it does, which is the 2026-09-08 lesson as an assertion.
			REQUIRE_THAT( ag_gamma_min( lift ), WithinAbs( 1.0f / lift, 1e-6f ) );
			REQUIRE_THAT( ag_gamma_max( darken ), WithinAbs( darken, 1e-6f ) );
		}
}

TEST_CASE( "adaptive gamma: Max lift 1.0 brightens nothing, Max darken 1.0 darkens nothing",
           "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( int i = 0; i <= 255; i++ )
			{
				const float x = i / 255.0f;
				REQUIRE( AgOutCode( sc, t, 1.0f, kAgDarken, x ) <= (float)i + 0.5f );
				REQUIRE( AgOutCode( sc, t, kAgLift, 1.0f, x ) >= (float)i - 0.5f );
			}
	// Both at 1.0 pins the exponent to exactly 1: the identity, at every
	// target and on every scene.
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			REQUIRE_THAT( ag_gamma( sc.p50, t, 1.0f, 1.0f ), WithinAbs( 1.0f, 1e-6f ) );
}

TEST_CASE( "adaptive gamma: Target and Strength both move the picture", "[effects_curve]" )
{
	// Target, on a dim indoor frame at the shipped defaults: monotone over
	// its whole range and worth far more than 20 codes from 0.5 to 0.7 --
	// the same bar Adaptive Brightness's own Target check uses.
	const float at50 = AgOutCode( kDimRoom, 0.50f, kAgLift, kAgDarken, 20.0f / 255.0f );
	const float at70 = AgOutCode( kDimRoom, 0.70f, kAgLift, kAgDarken, 20.0f / 255.0f );
	REQUIRE( at70 - at50 > 20.0f );
	float flPrev = -1.0f;
	for ( float t = 0.10f; t <= 0.901f; t += 0.05f )
	{
		const float v = AgOutCode( kDimRoom, t, kAgLift, kAgDarken, 20.0f / 255.0f );
		REQUIRE( v >= flPrev - 0.5f );
		flPrev = v;
	}

	// THE RESIDUAL CEILING, STATED AS AN ASSERTION rather than left for a
	// user to discover. An exponent floor of 1/max_lift means Target can
	// only reach p50^(1/max_lift): on the darker photographic frame, at the
	// top of the Max lift slider, that is about 0.52, so Target above it
	// does nothing -- and the classifier says exactly that. This is the same
	// shape of limit Adaptive Brightness has (its own is 0.75 on this
	// frame); the difference that matters is that it is NAMED.
	REQUIRE( ag_binding( kRealDark.p50, 0.70f, kAgLift, kAgDarken, 1.0f ) == AG_BIND_LIFT );
	REQUIRE( ag_binding( kRealDark.p50, 0.50f, kAgLift, kAgDarken, 1.0f ) == AG_BIND_NONE );

	// Strength, on the same frame: monotone from the untouched picture to
	// the fully graded one, and a real distance apart.
	flPrev = -1.0f;
	for ( float s = 0.0f; s <= 1.001f; s += 0.1f )
	{
		const float v = AgApply( 20.0f / 255.0f, kRealDark, s ) * 255.0f;
		REQUIRE( v >= flPrev - 0.5f );
		flPrev = v;
	}
	REQUIRE( AgApply( 20.0f / 255.0f, kRealDark, 1.0f ) * 255.0f
	         - AgApply( 20.0f / 255.0f, kRealDark, 0.0f ) * 255.0f > 20.0f );
}

TEST_CASE( "adaptive gamma: Max lift moves the picture wherever it is what binds", "[effects_curve]" )
{
	// The flat dark band chart's median is 0.047, so the exponent it wants
	// (0.23) is below every floor in the range: Max lift is the binding
	// limit at every setting, and every step of it must therefore move the
	// picture. This is the case Adaptive Brightness's Max gain was inert in
	// before 2026-09-08 -- here the classifier says so AND the slider works.
	float flPrev = -1.0f;
	for ( float lift : kAgLifts )
	{
		REQUIRE( ag_binding( kDark.p50, kTarget, lift, kAgDarken, 1.0f ) == AG_BIND_LIFT );
		const float v = AgOutCode( kDark, kTarget, lift, kAgDarken, 20.0f / 255.0f );
		REQUIRE( v > flPrev );
		flPrev = v;
	}
	// And it is worth a lot: 1.0 ("do not brighten") to 4.0 on a 20-code
	// input is the difference between leaving it alone and a real lift.
	REQUIRE( AgOutCode( kDark, kTarget, 4.0f, kAgDarken, 20.0f / 255.0f )
	         - AgOutCode( kDark, kTarget, 1.0f, kAgDarken, 20.0f / 255.0f ) > 40.0f );
}

TEST_CASE( "adaptive gamma: the binding classifier names one limit, and always names something",
           "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		for ( float t : kTargets )
			for ( float lift : kAgLifts )
				for ( float darken : kAgDarkens )
				{
					const int n = ag_binding( sc.p50, t, lift, darken, 1.0f );
					REQUIRE( n >= AG_BIND_NONE );
					REQUIRE( n <= AG_BIND_STRENGTH );
					REQUIRE( ag_binding_text( n )[0] != '\0' );
				}

	// Nothing binding: a frame whose median is already on the target.
	REQUIRE( ag_binding( 0.5f, 0.5f, kAgLift, kAgDarken, 1.0f ) == AG_BIND_NONE );
	// A very dark frame at the default lift limit: the floor holds it.
	REQUIRE( ag_binding( kDark.p50, kTarget, kAgLift, kAgDarken, 1.0f ) == AG_BIND_LIFT );
	// The bright reference scene asked to go far darker than the default
	// ceiling allows.
	REQUIRE( ag_binding( kBright.p50, 0.1f, kAgLift, kAgDarken, 1.0f ) == AG_BIND_DARKEN );
	// Strength 0 makes every other control inert, and the readout says so
	// rather than blaming a limit that is not the reason.
	REQUIRE( ag_binding( 0.5f, 0.5f, kAgLift, kAgDarken, 0.0f ) == AG_BIND_STRENGTH );

	// THE HONEST PART: whenever the classifier says a limit binds, Target
	// really has stopped moving the picture -- and whenever it says NONE,
	// Target really does still move it. That equivalence is what makes the
	// readout worth trusting.
	for ( const Scene &sc : kScenes )
		for ( float lift : kAgLifts )
			for ( float darken : kAgDarkens )
				for ( float t = 0.15f; t <= 0.851f; t += 0.05f )
				{
					const int n = ag_binding( sc.p50, t, lift, darken, 1.0f );
					const float a = AgOutCode( sc, t, lift, darken, 0.5f );
					const float b = AgOutCode( sc, t + 0.05f, lift, darken, 0.5f );
					if ( n == AG_BIND_LIFT || n == AG_BIND_DARKEN )
					{
						// Clamped on this side: a further step of Target
						// can only move the picture if the step crosses
						// back out of the clamp, which the next code's
						// own classification then reports.
						if ( ag_binding( sc.p50, t + 0.05f, lift, darken, 1.0f ) == n )
							REQUIRE_THAT( b, WithinAbs( a, 1e-4f ) );
					}
					else if ( n == AG_BIND_NONE && sc.p50 > 0.001f && sc.p50 < 0.999f )
					{
						REQUIRE( b >= a - 1e-4f );
					}
				}
}

TEST_CASE( "adaptive gamma: local adaptation is the same operator, inside the same bounds",
           "[effects_curve]" )
{
	// Strength 0 is the global exponent, bit for bit -- the shift is exactly
	// 1.0 and p50 * 1.0 is exact for every finite float.
	for ( const Scene &sc : kScenes )
		for ( float r : kLocalRatios )
			for ( int i = 0; i <= 255; i++ )
			{
				const float x = i / 255.0f;
				REQUIRE_THAT( AgApplyLocal( x, sc, 1.0f, r, 0.0f ),
				              WithinAbs( AgApply( x, sc, 1.0f ), 0.0f ) );
			}

	// Every property above survives a per-pixel exponent, and the exponent
	// itself never leaves the user's own bounds: local adaptation
	// redistributes inside them, it never widens them.
	for ( const Scene &sc : kScenes )
		for ( float lift : kAgLifts )
			for ( float darken : kAgDarkens )
				for ( float r : kLocalRatios )
					for ( float s : kLocalStrengths )
					{
						const float shift = ab_local_shift( r * 0.2f, 0.2f, s );
						const float g = ag_gamma( sc.p50 * shift, kTarget, lift, darken );
						REQUIRE( g >= ag_gamma_min( lift ) - 1e-6f );
						REQUIRE( g <= ag_gamma_max( darken ) + 1e-6f );
						REQUIRE_THAT( AgApplyLocal( 0.0f, sc, 1.0f, r, s ), WithinAbs( 0.0f, 1e-6f ) );
						REQUIRE_THAT( AgApplyLocal( 1.0f, sc, 1.0f, r, s ), WithinAbs( 1.0f, 1e-6f ) );
						float flPrev = -1.0f;
						for ( int i = 0; i <= 64; i++ )
						{
							const float y = AgApplyLocal( i / 64.0f, sc, 1.0f, r, s );
							REQUIRE( y >= flPrev - 1e-6f );
							REQUIRE( y <= 1.0f );
							flPrev = y;
						}
					}

	// And it points the right way: a darker-than-average neighbourhood gets
	// a smaller exponent, i.e. MORE lift, than the frame's own curve.
	const float gGlobal = ag_gamma( kRealDark.p50, kTarget, kAgLift, kAgDarken );
	const float gDark   = ag_gamma( kRealDark.p50 * ab_local_shift( 0.05f, 0.2f, 1.0f ),
	                                kTarget, kAgLift, kAgDarken );
	REQUIRE( gDark <= gGlobal + 1e-6f );
}

TEST_CASE( "reshade.adaptive_gamma defaults and round-trip", "[effects_curve][config]" )
{
	TempConfigHome home;

	Settings s{};
	REQUIRE( s.reshade.adaptive_gamma.enabled == false );
	REQUIRE_THAT( s.reshade.adaptive_gamma.target_luminance, WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.max_lift, WithinAbs( 4.0f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.max_darken, WithinAbs( 1.5f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.strength, WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.local_strength, WithinAbs( 0.0f, 1e-6f ) );

	s.reshade.adaptive_gamma.enabled = true;
	s.reshade.adaptive_gamma.target_luminance = 0.65f;
	s.reshade.adaptive_gamma.max_lift = 2.5f;
	s.reshade.adaptive_gamma.max_darken = 2.0f;
	s.reshade.adaptive_gamma.strength = 0.8f;
	s.reshade.adaptive_gamma.local_strength = 0.35f;
	ProfileMeta meta;
	meta.name = "Effects";
	REQUIRE( SaveProfile( meta, s ) );

	std::optional<Settings> loaded = LoadProfile( "Effects" );
	REQUIRE( loaded.has_value() );
	const auto &ag = loaded->reshade.adaptive_gamma;
	REQUIRE( ag.enabled == true );
	REQUIRE_THAT( ag.target_luminance, WithinAbs( 0.65f, 1e-6f ) );
	REQUIRE_THAT( ag.max_lift, WithinAbs( 2.5f, 1e-6f ) );
	REQUIRE_THAT( ag.max_darken, WithinAbs( 2.0f, 1e-6f ) );
	REQUIRE_THAT( ag.strength, WithinAbs( 0.8f, 1e-6f ) );
	REQUIRE_THAT( ag.local_strength, WithinAbs( 0.35f, 1e-6f ) );

	// An old profile with no adaptive_gamma object at all resolves to the
	// compiled-in defaults -- purely additive keys, no migration.
	REQUIRE( loaded->reshade.adaptive_brightness.enabled == false );
}
