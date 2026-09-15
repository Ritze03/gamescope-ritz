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
#include <bit>
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

// ===========================================================================
//  DARK FLOOR (2026-09-14) -- effects_curve.h's dark_weight(), shared by
//  both adaptive effects. See that header's own DARK FLOOR block for the
//  formula and every "why"; asserted here is its CONTRACT (exactly 1 at
//  dark_floor 0 -- today's pre-2026-09-14 behaviour, byte-identical -- and a
//  smoothstep ramp with no kink at either end), that it is wired into BOTH
//  of Adaptive Brightness Dynamic's parameters (not just the gain) and into
//  Adaptive Gamma's one exponent exactly as cs_effects_layer0.comp wires
//  them, that the pre-existing exponent/gain bounds already hold as the
//  median goes to zero (so the floor is a THIRD control, not a bound fix),
//  and -- at the shipped default -- an exact identity on the report's own
//  near-black capture while the existing `dark` reference scene, whose own
//  pre-existing regression contract this feature must not break, keeps
//  full strength.
// ===========================================================================

namespace
{
	// The report's own capture (superdoc/features/shader-effects.md has the
	// full PIL measurement): a near-black CS2 corridor with a HUD, a lamp
	// and chat text. p50 is a fraction of a code above zero -- most of the
	// frame is literal black -- while the scattered bright UI pulls the
	// MEAN to 0.055, which is why the floor is keyed on the median and not
	// the mean (effects_curve.h's "why p50 and not the mean").
	constexpr Scene kRealBlackout = { 0.0f, 0.0003f, 0.4336f };

	// tests/effects_scene_client.c's `blackout` scene, added for this
	// feature: ~90% of pixels at code 0..6, ~9% a textured 10..40 band, ~1%
	// lights 200+. Measured off the same rank-window-mean the GPU computes
	// (a Python simulation of the client's own paint, not eyeballed).
	constexpr Scene kBlackout = { 0.0001f, 0.0078f, 0.88f };

	// The EXISTING `dark` reference scene (tests/effects_scene_client.c's
	// five bands 5/8/12/16/20 plus the six 240 rectangles and the black
	// corner), measured the same careful way -- NOT the simplified kDark
	// stand-in above (p50 10/255) this file already uses for generic
	// sweeps, which is close but not what the real capture pipeline
	// produces. scripts/effects-regression.sh's `dark-dynamic` check
	// already requires band 5 to lift to >= 30 on this exact scene, so the
	// floor's default must leave it at full weight.
	constexpr Scene kDarkSceneReal = { 4.0f / 255.0f, 12.1f / 255.0f, 24.6f / 255.0f };

	// `texdark`, the textured dark-game stand-in (tests/effects_scene_client.c,
	// ~1.5% light cells), measured the same way -- the other existing scene
	// the default must not touch.
	constexpr Scene kTexdarkReal = { 6.0f / 255.0f, 20.0f / 255.0f, 85.0f / 255.0f };

	// == ConfigSchema.h's ReshadeSettings::dark_floor default.
	const float kDarkFloorDefault = 0.03f;
	const float kDarkFloors[] = { 0.01f, 0.02f, 0.03f, 0.05f, 0.10f, 0.15f };

	float Mix( float a, float b, float t ) { return a + ( b - a ) * t; }
}

TEST_CASE( "dark floor: 0 is an EXACT identity, at every scene", "[effects_curve]" )
{
	for ( const Scene &sc : kScenes )
		REQUIRE_THAT( dark_weight( sc.p50, 0.0f ), WithinAbs( 1.0f, 1e-7f ) );
	REQUIRE_THAT( dark_weight( 0.0f, 0.0f ), WithinAbs( 1.0f, 1e-7f ) );
	REQUIRE_THAT( dark_weight( 1.0f, 0.0f ), WithinAbs( 1.0f, 1e-7f ) );
	// A negative, hand-edited value reads the same as "off" -- never a
	// divide that could turn a bad config into a NaN.
	REQUIRE_THAT( dark_weight( 0.02f, -1.0f ), WithinAbs( 1.0f, 1e-7f ) );
}

TEST_CASE( "dark floor: a smoothstep ramp -- 0 at/below half, 1 at/above the full value, monotone "
           "and flat-sloped at both ends", "[effects_curve]" )
{
	for ( float floor : kDarkFloors )
	{
		REQUIRE_THAT( dark_weight( 0.0f, floor ), WithinAbs( 0.0f, 1e-6f ) );
		REQUIRE_THAT( dark_weight( floor * 0.5f, floor ), WithinAbs( 0.0f, 1e-6f ) );
		REQUIRE_THAT( dark_weight( floor, floor ), WithinAbs( 1.0f, 1e-6f ) );
		REQUIRE_THAT( dark_weight( 1.0f, floor ), WithinAbs( 1.0f, 1e-6f ) );

		float flPrev = -1.0f;
		for ( int i = 0; i <= 200; i++ )
		{
			const float m = floor * 2.0f * i / 200.0f;
			const float w = dark_weight( m, floor );
			REQUIRE( w >= 0.0f );
			REQUIRE( w <= 1.0f );
			REQUIRE( w >= flPrev - 1e-6f );   // monotone in m
			flPrev = w;
		}

		// The slope at each end of the ramp is 0: a step of h off lo/hi
		// moves w by O(h^2), not O(h) -- "no kink" as a number, not an
		// eyeball on a graph.
		const float lo = floor * 0.5f, hi = floor, h = ( hi - lo ) * 0.001f;
		REQUIRE( dark_weight( lo + h, floor ) < h );
		REQUIRE( ( 1.0f - dark_weight( hi - h, floor ) ) < h );
	}
}

TEST_CASE( "dark floor: blends BOTH of Adaptive Brightness Dynamic's parameters, so w = 0 is a "
           "bit-exact identity and not merely a small number", "[effects_curve]" )
{
	// ab_dyn_curve(x, 1, 1) is y = x exactly -- its own top (gain^gamma) is
	// exactly 1, so the shoulder never engages -- which is the property
	// that makes blending BOTH gain and gamma toward 1 (not just the gain)
	// what cs_effects_layer0.comp relies on.
	const float gain  = ab_dyn_gain( kRealBlackout.p98, kRealBlackout.p50, kTarget, kMinGain, kMaxGain );
	const float gamma = ab_dyn_gamma( kRealBlackout.p2, kRealBlackout.p50, gain, kTarget, kMinGain, kMaxGain );
	// A sanity check that this scene really does hit the reported failure
	// shape absent the floor: the gamma is driven to its floor (maximum
	// lift) and the gain to its ceiling.
	REQUIRE_THAT( gain, WithinAbs( kMaxGain, 1e-5f ) );
	REQUIRE_THAT( gamma, WithinAbs( ab_gamma_min( kMaxGain ), 1e-5f ) );

	for ( int i = 0; i <= 255; i++ )
	{
		const float x = i / 255.0f;
		const float y = ab_dyn_curve( x, Mix( 1.0f, gain, 0.0f ), Mix( 1.0f, gamma, 0.0f ) );
		REQUIRE_THAT( y, WithinAbs( x, 1e-6f ) );
	}
}

TEST_CASE( "dark floor: blends Adaptive Gamma's one exponent, so w = 0 is a bit-exact identity",
           "[effects_curve]" )
{
	const float gamma = ag_gamma( kRealBlackout.p50, kTarget, kAgLift, kAgDarken );
	REQUIRE_THAT( gamma, WithinAbs( ag_gamma_min( kAgLift ), 1e-5f ) );   // the same failure shape

	for ( int i = 0; i <= 255; i++ )
	{
		const float x = i / 255.0f;
		REQUIRE_THAT( ag_curve( x, Mix( 1.0f, gamma, 0.0f ) ), WithinAbs( x, 1e-6f ) );
	}
}

TEST_CASE( "dark floor: the pre-existing exponent/gain bounds already hold as the median -> 0 -- "
           "the floor is a THIRD control, not a bound fix", "[effects_curve]" )
{
	for ( float lift : kAgLifts )
		for ( float darken : kAgDarkens )
		{
			const float g = ag_gamma( 0.0f, kTarget, lift, darken );
			REQUIRE( g >= ag_gamma_min( lift ) - 1e-6f );
			REQUIRE( g <= ag_gamma_max( darken ) + 1e-6f );
			REQUIRE( std::isfinite( g ) );
		}
	for ( float lo : kMinGains )
		for ( float hi : kMaxGains )
		{
			const float gain = ab_dyn_gain( 0.0f, 0.0f, kTarget, lo, hi );
			REQUIRE( gain >= lo - 1e-6f );
			REQUIRE( gain <= hi + 1e-6f );
			const float gamma = ab_dyn_gamma( 0.0f, 0.0f, gain, kTarget, lo, hi );
			REQUIRE( gamma >= ab_gamma_min( hi ) - 1e-6f );
			REQUIRE( gamma <= ab_gamma_max( lo ) + 1e-6f );
			REQUIRE( std::isfinite( gamma ) );
		}
}

TEST_CASE( "dark floor: at the shipped default, the report's near-black capture and the `blackout` "
           "scene are fully neutralised while the existing `dark` and `texdark` reference scenes "
           "keep full strength", "[effects_curve]" )
{
	// The two scenes this feature exists for: essentially zero weight.
	REQUIRE( dark_weight( kRealBlackout.p50, kDarkFloorDefault ) < 1e-3f );
	REQUIRE( dark_weight( kBlackout.p50, kDarkFloorDefault ) < 1e-3f );

	// The two EXISTING scenes scripts/effects-regression.sh already has a
	// contract on (dark-dynamic's ">= 30" among them): full weight, so this
	// feature changes nothing about them at its default.
	REQUIRE( dark_weight( kDarkSceneReal.p50, kDarkFloorDefault ) > 0.999f );
	REQUIRE( dark_weight( kTexdarkReal.p50, kDarkFloorDefault ) > 0.999f );

	// And the untouched-picture scene, trivially.
	REQUIRE( dark_weight( kMid.p50, kDarkFloorDefault ) > 0.999f );
}

// Split 2026-09-14 (the user's follow-up: "Make the 'Leave dark scenes
// alone' part individual settings for both Adaptive Gamma and Adaptive
// Brightness") from a single shared reshade.dark_floor field into one per
// effect. The schema-4 -> 5 MIGRATION that carries an old shared value
// forward is tested in tests/test_config.cpp (it needs raw on-disk JSON,
// which that file already has the fixtures for); this test only pins the
// two new fields' own default and round-trip, independently of each other.
TEST_CASE( "reshade.adaptive_{brightness,gamma}.dark_floor: default and round-trip, independently",
           "[effects_curve][config]" )
{
	TempConfigHome home;

	Settings s{};
	REQUIRE_THAT( s.reshade.adaptive_brightness.dark_floor, WithinAbs( 0.03f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.dark_floor, WithinAbs( 0.03f, 1e-6f ) );

	// Set to two DIFFERENT values -- proves the split, not just that a
	// single number still round-trips under a new name.
	s.reshade.adaptive_brightness.dark_floor = 0.12f;
	s.reshade.adaptive_gamma.dark_floor = 0.20f;
	ProfileMeta meta;
	meta.name = "DarkFloor";
	REQUIRE( SaveProfile( meta, s ) );

	std::optional<Settings> loaded = LoadProfile( "DarkFloor" );
	REQUIRE( loaded.has_value() );
	REQUIRE_THAT( loaded->reshade.adaptive_brightness.dark_floor, WithinAbs( 0.12f, 1e-6f ) );
	REQUIRE_THAT( loaded->reshade.adaptive_gamma.dark_floor, WithinAbs( 0.20f, 1e-6f ) );

	// An old profile with no "dark_floor" key at all under either effect
	// (every OTHER field present, this one simply absent) resolves both to
	// their compiled-in default -- purely additive key, no migration
	// needed for a file that never had ANY dark_floor, the same shape
	// every other field added since ReshadeShadowLiftSettings follows.
	REQUIRE( loaded->reshade.adaptive_gamma.max_lift > 0.0f );   // the file round-tripped at all
}

// ===========================================================================
//  BLOOM (2026-09-08) -- the scalar half of the glow.
// ===========================================================================
//
// Bloom is a SPATIAL effect, so most of it lives in three compute passes and
// cannot be asserted here. What can be, and is, are the three scalar
// functions the whole effect is built out of -- the bright-pass gate, the
// Radius -> sigma mapping and the composite -- because those carry the
// properties the user is promised: raising Threshold shrinks what glows,
// raising Radius spreads it further, and adding light can never clip.

TEST_CASE( "bloom: the bright-pass weight is 0 at the threshold, 1 at white, rising between",
           "[effects_curve]" )
{
	for ( int nT = 0; nT <= 19; nT++ )
	{
		const float t = (float)nT / 20.0f;
		REQUIRE_THAT( bloom_weight( 0.0f, t ), WithinAbs( 0.0f, 1e-6f ) );
		REQUIRE_THAT( bloom_weight( t, t ), WithinAbs( 0.0f, 1e-6f ) );
		REQUIRE_THAT( bloom_weight( 1.0f, t ), WithinAbs( 1.0f, 1e-6f ) );

		// Monotone increasing in luma, inside [0, 1] everywhere, and still
		// rising right up to white (no plateau -- a 250-code pixel must not
		// emit the same as a 255-code one; see the header's note on why not
		// a smoothstep).
		float flPrev = -1.0f;
		for ( int i = 0; i <= 255; i++ )
		{
			const float w = bloom_weight( (float)i / 255.0f, t );
			REQUIRE( w >= -1e-6f );
			REQUIRE( w <= 1.0f + 1e-6f );
			REQUIRE( w >= flPrev - 1e-6f );
			flPrev = w;
		}
		REQUIRE( bloom_weight( 1.0f, t ) > bloom_weight( 250.0f / 255.0f, t ) );
	}

	// THE TOP OF THE SLIDER: at Threshold 1.0 nothing glows at any luma,
	// which is the right meaning for "the threshold is above everything the
	// picture can contain" and is what BLOOM_HEADROOM_MIN's guarded divide
	// produces rather than a NaN.
	for ( int i = 0; i <= 255; i++ )
		REQUIRE_THAT( bloom_weight( (float)i / 255.0f, 1.0f ), WithinAbs( 0.0f, 1e-6f ) );
}

TEST_CASE( "bloom: raising the threshold can only shrink what glows", "[effects_curve]" )
{
	// The user-facing statement of the Threshold slider, as an inequality
	// over every (luma, threshold) pair the panel can produce. Anything else
	// would make the control ambiguous: a value that starts glowing MORE as
	// the threshold rises is a bug no capture would obviously catch.
	for ( int i = 0; i <= 255; i++ )
	{
		const float flLuma = (float)i / 255.0f;
		float flPrev = 2.0f;
		for ( int nT = 0; nT <= 20; nT++ )
		{
			const float w = bloom_weight( flLuma, (float)nT / 20.0f );
			REQUIRE( w <= flPrev + 1e-6f );
			flPrev = w;
		}
	}

	// And it is a STRICT shrink in the middle, not a flat line: a mid-bright
	// pixel emits most of its colour at threshold 0.2 and none at 0.9.
	REQUIRE( bloom_weight( 0.6f, 0.2f ) > 0.2f );
	REQUIRE_THAT( bloom_weight( 0.6f, 0.9f ), WithinAbs( 0.0f, 1e-6f ) );
	REQUIRE( bloom_weight( 0.6f, 0.5f ) > 0.0f );
	REQUIRE( bloom_weight( 0.6f, 0.5f ) < bloom_weight( 0.6f, 0.2f ) );
}

TEST_CASE( "bloom: the weight is flat at the threshold, which is what keeps it from shimmering",
           "[effects_curve]" )
{
	// A hard threshold changes a pixel's whole contribution the instant it
	// crosses it; a linear ramp changes it at a constant rate from the very
	// first code above it. This weight is zero AND has zero slope there, so
	// a pixel wandering across the boundary under a pan -- the case the
	// shimmer question is about -- moves by a second-order amount. Asserted
	// as a Lipschitz bound plus a local flatness check, so a later "sharper
	// threshold" change fails here rather than in a capture nobody looks at
	// twice.
	for ( int nT = 0; nT <= 18; nT++ )
	{
		const float t = (float)nT / 20.0f;
		const float flMaxSlope = 2.0f / ( 1.0f - t );   // d/dx of x^2 at x = 1
		const float flStep = 1.0f / 4096.0f;
		for ( int i = 0; i < 4096; i++ )
		{
			const float a = bloom_weight( (float)i * flStep, t );
			const float b = bloom_weight( (float)( i + 1 ) * flStep, t );
			REQUIRE( std::fabs( b - a ) <= flMaxSlope * flStep + 1e-5f );
		}
		// One code above the threshold contributes essentially nothing. A
		// plain gate would make that first step 1.0 and a linear ramp would
		// make it x = (1/255)/(1-t); the quadratic makes it x squared, which
		// is at most a twentieth of the linear ramp's anywhere in this
		// range. That factor IS the anti-shimmer margin, so it is what gets
		// asserted rather than an absolute number that would drift with the
		// threshold.
		const float flLinear = ( 1.0f / 255.0f ) / ( 1.0f - t );
		REQUIRE( bloom_weight( t + 1.0f / 255.0f, t ) <= 0.05f * flLinear + 1e-9f );
	}
}

TEST_CASE( "bloom: Radius maps monotonically onto the blur's sigma", "[effects_curve]" )
{
	REQUIRE_THAT( bloom_sigma( 0.0f ), WithinAbs( BLOOM_SIGMA_MIN, 1e-6f ) );
	REQUIRE_THAT( bloom_sigma( 1.0f ), WithinAbs( BLOOM_SIGMA_MAX, 1e-6f ) );
	float flPrev = -1.0f;
	for ( int i = 0; i <= 20; i++ )
	{
		const float s = bloom_sigma( (float)i / 20.0f );
		REQUIRE( s > flPrev );
		REQUIRE( s >= BLOOM_SIGMA_MIN );
		REQUIRE( s <= BLOOM_SIGMA_MAX );
		flPrev = s;
	}
	// Out-of-range inputs clamp rather than extrapolating into a negative or
	// unbounded sigma (the blur divides by it).
	REQUIRE_THAT( bloom_sigma( -1.0f ), WithinAbs( BLOOM_SIGMA_MIN, 1e-6f ) );
	REQUIRE_THAT( bloom_sigma( 5.0f ), WithinAbs( BLOOM_SIGMA_MAX, 1e-6f ) );
}

TEST_CASE( "bloom: the composite can never clip, at any Intensity, over every 8-bit pair",
           "[effects_curve]" )
{
	// THE HEADLINE PROPERTY. Adding light is the one operation in this
	// pipeline that naturally blows highlights out, and the composite's
	// shape is how it is prevented -- not by a clamp afterwards, which would
	// flatten detail into white, but by an operator whose range IS [0, 1] at
	// every setting of every knob. Checked exhaustively over all 65,536
	// 8-bit (base, glow) pairs at each end and the middle of the Intensity
	// slider: the result is in range, never darkens the picture, is monotone
	// in the glow, and stays STRICTLY below white wherever the picture was
	// below white -- which is the statement that failed for the obvious
	// screen(base, glow * intensity) at intensity 2 and is why this shape
	// exists (see effects_curve.h's BLOOM block).
	for ( float flIntensity : { 0.0f, 0.05f, 0.5f, 1.0f, 1.5f, 2.0f } )
	{
		for ( int nBase = 0; nBase <= 255; nBase++ )
		{
			const float a = (float)nBase / 255.0f;
			float flPrev = -1.0f;
			for ( int nGlow = 0; nGlow <= 255; nGlow++ )
			{
				const float b = (float)nGlow / 255.0f;
				const float y = bloom_apply( a, b, flIntensity );
				REQUIRE( y >= a - 1e-6f );              // never darkens
				REQUIRE( y <= 1.0f );                   // never clips
				REQUIRE( y >= flPrev - 1e-6f );         // monotone in the glow
				if ( nBase < 255 )
					REQUIRE( y < 1.0f );                // strictly below white
				flPrev = y;
			}
		}
	}

	// Monotone in the base, and in the Intensity, at a fixed glow.
	for ( int nGlow = 0; nGlow <= 255; nGlow += 17 )
	{
		float flPrev = -1.0f;
		for ( int nBase = 0; nBase <= 255; nBase++ )
		{
			const float y = bloom_apply( (float)nBase / 255.0f, (float)nGlow / 255.0f, 1.0f );
			REQUIRE( y >= flPrev - 1e-6f );
			flPrev = y;
		}
		float flPrevK = -1.0f;
		for ( int nK = 0; nK <= 40; nK++ )
		{
			const float y = bloom_apply( 0.25f, (float)nGlow / 255.0f, (float)nK / 20.0f );
			REQUIRE( y >= flPrevK - 1e-6f );
			flPrevK = y;
		}
	}

	// Intensity 0 is an EXACT identity for every base and every glow, which
	// is what "0 is off" has to mean for the paired param of a switch -- the
	// pow(0, 0) case (a fully-lit neighbourhood at intensity 0) included,
	// which is why bloom_apply floors its base.
	for ( int nBase = 0; nBase <= 255; nBase++ )
		for ( int nGlow = 0; nGlow <= 255; nGlow += 51 )
			REQUIRE_THAT( bloom_apply( (float)nBase / 255.0f, (float)nGlow / 255.0f, 0.0f ),
			              WithinAbs( (float)nBase / 255.0f, 1e-6f ) );

	// Zero glow is likewise an exact identity, at every intensity: a pixel
	// with nothing glowing near it is untouched however hard the slider is
	// pushed.
	for ( float flIntensity : { 0.0f, 0.5f, 1.0f, 2.0f } )
		for ( int nBase = 0; nBase <= 255; nBase++ )
			REQUIRE_THAT( bloom_apply( (float)nBase / 255.0f, 0.0f, flIntensity ),
			              WithinAbs( (float)nBase / 255.0f, 1e-6f ) );

	// At Intensity 1 it IS the plain screen, so the shape is a
	// generalisation of the familiar operator rather than a different one.
	for ( int nBase = 0; nBase <= 255; nBase += 5 )
		for ( int nGlow = 0; nGlow <= 255; nGlow += 5 )
		{
			const float a = (float)nBase / 255.0f, b = (float)nGlow / 255.0f;
			REQUIRE_THAT( bloom_apply( a, b, 1.0f ), WithinAbs( a + b * ( 1.0f - a ), 1e-5f ) );
		}

	// And it is LINEAR in the intensity where the glow is faint -- the
	// regime a dark scene with a small light actually lives in -- so the
	// slider still feels like a gain rather than like a saturating curve.
	for ( int nGlow = 1; nGlow <= 20; nGlow++ )
	{
		const float b = (float)nGlow / 255.0f;
		const float flLinear = bloom_apply( 0.0f, b, 1.0f ) * 2.0f;
		REQUIRE_THAT( bloom_apply( 0.0f, b, 2.0f ), WithinAbs( flLinear, 0.01f ) );
	}
}

TEST_CASE( "reshade.bloom defaults and round-trip", "[effects_curve][config]" )
{
	TempConfigHome home;

	Settings s{};
	REQUIRE( s.reshade.bloom.enabled == false );
	REQUIRE_THAT( s.reshade.bloom.threshold, WithinAbs( 0.75f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.bloom.intensity, WithinAbs( 0.8f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.bloom.radius, WithinAbs( 0.5f, 1e-6f ) );

	s.reshade.bloom.enabled = true;
	s.reshade.bloom.threshold = 0.55f;
	s.reshade.bloom.intensity = 1.35f;
	s.reshade.bloom.radius = 0.85f;
	ProfileMeta meta;
	meta.name = "Bloom";
	REQUIRE( SaveProfile( meta, s ) );

	std::optional<Settings> loaded = LoadProfile( "Bloom" );
	REQUIRE( loaded.has_value() );
	const auto &bl = loaded->reshade.bloom;
	REQUIRE( bl.enabled == true );
	REQUIRE_THAT( bl.threshold, WithinAbs( 0.55f, 1e-6f ) );
	REQUIRE_THAT( bl.intensity, WithinAbs( 1.35f, 1e-6f ) );
	REQUIRE_THAT( bl.radius, WithinAbs( 0.85f, 1e-6f ) );
}

// ---- THE ADAPTATION SPEED (ema_alpha) --------------------------------
//
// The EMA cs_effects_measure.comp smooths every statistic with, asserted on
// the same header text the GPU compiles. This is the whole of what an
// "adaptation speed" slider means, for BOTH adaptive effects: Adaptive
// Gamma got its own pair on 2026-09-09, and because the two effects are
// mutually exclusive the shader still runs exactly one EMA -- so a property
// proven here is a property of both rows' sliders at once.
TEST_CASE( "ema_alpha: a step is within 5% after 3 tau, at every speed", "[effects_curve]" )
{
	// The panel's slider ends, and the default. tau is SECONDS TO ~63%, so
	// the settling time the user actually observes is 3 tau -- which is the
	// number scripts/effects-regression.sh's ag-speed check measures on a
	// real capture, and this is its closed form.
	for ( float tau : { 0.1f, 0.3f, 1.0f, 3.0f, 5.0f } )
	{
		// Integrated in 120 Hz steps: the residual after t seconds is
		// exp(-t / tau) however the frames are spaced (see below), so the
		// step size must not change the answer.
		float flResidual = 1.0f;
		const float flDt = 1.0f / 120.0f;
		for ( int i = 0; i < int( 3.0f * tau * 120.0f ); i++ )
			flResidual *= ( 1.0f - ema_alpha( flDt, tau ) );
		REQUIRE( flResidual <= 0.05f );
		// ...and NOT already there a third of the way: the slider has to
		// mean something at each end, not saturate immediately.
		float flEarly = 1.0f;
		for ( int i = 0; i < int( 1.0f * tau * 120.0f ); i++ )
			flEarly *= ( 1.0f - ema_alpha( flDt, tau ) );
		REQUIRE( flEarly > 0.05f );
	}
}

TEST_CASE( "ema_alpha: slower tau is always slower, and the range spans", "[effects_curve]" )
{
	// Monotone in tau at a fixed dt -- the property that makes the slider a
	// speed control rather than a number that happens to correlate with one.
	const float flDt = 1.0f / 60.0f;
	float flPrev = 2.0f;
	for ( float tau = 0.1f; tau <= 5.0001f; tau += 0.1f )
	{
		const float a = ema_alpha( flDt, tau );
		REQUIRE( a < flPrev );
		REQUIRE( a > 0.0f );
		REQUIRE( a <= 1.0f );
		flPrev = a;
	}
	// The two ends of the slider are a factor of ~50 apart in settling
	// time, so "fast" and "slow" are genuinely different pictures rather
	// than two names for the same one.
	REQUIRE( ema_alpha( flDt, 0.1f ) / ema_alpha( flDt, 5.0f ) > 20.0f );
}

TEST_CASE( "ema_alpha: elapsed time, not frame count", "[effects_curve]" )
{
	// The reason rendervulkan.cpp can feed this a wall-clock dt and let a
	// screenshot re-composite take its own tiny step: composing per-frame
	// alphas over an interval must depend only on the interval. Two very
	// different frame paces over the same 1.0 s must leave the same residual.
	// nSteps steps that exactly tile flTotal, so the two paces cover the
	// same interval and the comparison is about the maths, not about
	// truncating a step count.
	auto Residual = [] ( int nSteps, float flTotal, float tau )
	{
		float r = 1.0f;
		for ( int i = 0; i < nSteps; i++ )
			r *= ( 1.0f - ema_alpha( flTotal / float( nSteps ), tau ) );
		return r;
	};
	for ( float tau : { 0.2f, 1.0f, 4.0f } )
	{
		REQUIRE_THAT( Residual( 240, 1.0f, tau ),
		              WithinAbs( Residual( 30, 1.0f, tau ), 1e-5f ) );
		REQUIRE_THAT( Residual( 60, 1.0f, tau ),
		              WithinAbs( std::exp( -1.0f / tau ), 1e-5f ) );
	}
}

TEST_CASE( "ema_alpha: no setting can freeze the history", "[effects_curve]" )
{
	// The floor exists so a hand-edited 0 (or a negative) is "as fast as the
	// control goes", never a division by zero and never a stuck picture.
	// That is WHY the binding readout needs no code for an extreme speed:
	// unlike Max lift or Max darken, no value of this pair makes anything
	// inert -- see ag_binding() and shader-effects.md.
	for ( float tau : { 0.0f, -1.0f, 1e-9f } )
		REQUIRE_THAT( ema_alpha( 1.0f / 60.0f, tau ), WithinAbs( 1.0f, 1e-6f ) );
	// And at the slowest end it still moves every frame -- slow is slow,
	// not off.
	REQUIRE( ema_alpha( 1.0f / 60.0f, 5.0f ) > 0.0f );
	// dt 0 is the only "nothing happens" case, and it is time not settings.
	REQUIRE_THAT( ema_alpha( 0.0f, 1.0f ), WithinAbs( 0.0f, 1e-6f ) );
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
	// 2026-09-09: this row's own adaptation speeds, defaulting to exactly
	// Adaptive Brightness's, so switching between the two mutually exclusive
	// effects does not change how fast the picture follows the scene.
	REQUIRE_THAT( s.reshade.adaptive_gamma.adapt_up_speed, WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.adapt_down_speed, WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.adapt_up_speed,
	              WithinAbs( s.reshade.adaptive_brightness.adapt_up_speed, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.adapt_down_speed,
	              WithinAbs( s.reshade.adaptive_brightness.adapt_down_speed, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_gamma.local_strength, WithinAbs( 0.0f, 1e-6f ) );

	s.reshade.adaptive_gamma.enabled = true;
	s.reshade.adaptive_gamma.target_luminance = 0.65f;
	s.reshade.adaptive_gamma.max_lift = 2.5f;
	s.reshade.adaptive_gamma.max_darken = 2.0f;
	s.reshade.adaptive_gamma.strength = 0.8f;
	s.reshade.adaptive_gamma.adapt_up_speed = 0.3f;
	s.reshade.adaptive_gamma.adapt_down_speed = 2.7f;
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
	REQUIRE_THAT( ag.adapt_up_speed, WithinAbs( 0.3f, 1e-6f ) );
	REQUIRE_THAT( ag.adapt_down_speed, WithinAbs( 2.7f, 1e-6f ) );
	// Adaptive BRIGHTNESS's own pair is untouched by writing this row's --
	// two independent settings, not one aliased under two names.
	REQUIRE_THAT( loaded->reshade.adaptive_brightness.adapt_up_speed, WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( loaded->reshade.adaptive_brightness.adapt_down_speed, WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( ag.local_strength, WithinAbs( 0.35f, 1e-6f ) );

	// An old profile with no adaptive_gamma object at all resolves to the
	// compiled-in defaults -- purely additive keys, no migration.
	REQUIRE( loaded->reshade.adaptive_brightness.enabled == false );
}

// ===========================================================================
//  ADAPTIVE BRIGHTNESS V2 (2026-09-14) -- superdoc/planning/
//  adaptive-brightness-v2-plan.md. A NEW, ADDITIVE effect (the user's own
//  words: "DO NOT REMOVE THE ORIGINAL"), so every test above this point --
//  Adaptive Brightness, Adaptive Gamma, the dark floor -- is UNCHANGED and
//  still has to pass. These cases pin the plan's five guarantees (section
//  4.1) on the same abv2_* text the GPU compiles.
// ===========================================================================

TEST_CASE( "abv2_toe/abv2_knee: f(0) = 0 and f(1) = 1, over the whole g x S grid",
           "[effects_curve][abv2]" )
{
	for ( float g = ABV2_G_MIN; g <= 1.0f; g += 0.05f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 0.5f )
		{
			REQUIRE_THAT( abv2_toe( 0.0f, g, S ), WithinAbs( 0.0f, 1e-5f ) );
			REQUIRE_THAT( abv2_toe( 1.0f, g, S ), WithinAbs( 1.0f, 1e-4f ) );
			REQUIRE_THAT( abv2_knee( 0.0f, g, S ), WithinAbs( 0.0f, 1e-5f ) );
			REQUIRE_THAT( abv2_knee( 1.0f, g, S ), WithinAbs( 1.0f, 1e-4f ) );
		}
	}
}

TEST_CASE( "abv2_toe/abv2_knee: guarantee 1 -- no local slope anywhere exceeds S",
           "[effects_curve][abv2]" )
{
	const float h = 1e-4f;
	for ( float g = ABV2_G_MIN; g <= 1.0f; g += 0.1f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 0.5f )
		{
			float flMaxSlopeToe = 0.0f, flMaxSlopeKnee = 0.0f;
			for ( float x = 0.0f; x <= 1.0f - h; x += 0.005f )
			{
				flMaxSlopeToe  = std::max( flMaxSlopeToe,  ( abv2_toe( x + h, g, S )  - abv2_toe( x, g, S ) )  / h );
				flMaxSlopeKnee = std::max( flMaxSlopeKnee, ( abv2_knee( x + h, g, S ) - abv2_knee( x, g, S ) ) / h );
			}
			// A little slack for the finite-difference sampling itself
			// (the true maximum is exactly S at x = 0, proven algebraically
			// in effects_curve.h's own comment; this just checks the CODE
			// matches the proof).
			REQUIRE( flMaxSlopeToe <= S + 0.05f );
			REQUIRE( flMaxSlopeKnee <= S + 0.05f );
		}
	}
}

TEST_CASE( "abv2_toe/abv2_knee: guarantee 3 -- monotone, no two samples invert",
           "[effects_curve][abv2]" )
{
	for ( float g = ABV2_G_MIN; g <= 1.0f; g += 0.1f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 0.5f )
		{
			float flPrevToe = -1.0f, flPrevKnee = -1.0f;
			for ( float x = 0.0f; x <= 1.0f; x += 0.01f )
			{
				const float yToe = abv2_toe( x, g, S );
				const float yKnee = abv2_knee( x, g, S );
				REQUIRE( yToe >= flPrevToe - 1e-6f );
				REQUIRE( yKnee >= flPrevKnee - 1e-6f );
				flPrevToe = yToe;
				flPrevKnee = yKnee;
			}
		}
	}
}

TEST_CASE( "abv2_toe: identity when g == 1 (no lift requested) or S == 1 (Max lift at its floor)",
           "[effects_curve][abv2]" )
{
	for ( float x = 0.0f; x <= 1.0f; x += 0.05f )
	{
		REQUIRE_THAT( abv2_toe( x, 1.0f, 6.0f ), WithinAbs( x, 1e-5f ) );
		REQUIRE_THAT( abv2_toe( x, 0.4f, 1.0f ), WithinAbs( x, 1e-5f ) );
		REQUIRE_THAT( abv2_knee( x, 1.0f, 6.0f ), WithinAbs( x, 1e-5f ) );
		REQUIRE_THAT( abv2_knee( x, 0.4f, 1.0f ), WithinAbs( x, 1e-5f ) );
	}
}

TEST_CASE( "abv2_toe/abv2_knee: finite everywhere, and f(0) == 0, where float32 makes t underflow to 0 "
           "(g within ~0.02 of 1 at Max lift 8 -- Lift below ~0.04)", "[effects_curve][abv2]" )
{
	// V2 QC (2026-09-14): abv2_solve_t()'s S^(1/(1-g)) overflows float32 to
	// inf for g >= ~0.977 at S = 8 (>= ~0.984 at S = 4), so t == 0 exactly
	// and the unguarded formula evaluated 0 * pow(1/0, 1-g) == NaN at x == 0.
	// The grid steps the older cases use (0.05 from 0.2) never land in that
	// band; this one does, on purpose.
	for ( float g = 0.95f; g < 1.0f; g += 0.005f )
	{
		for ( float S = 1.5f; S <= 8.0f; S += 0.5f )
		{
			REQUIRE( std::isfinite( abv2_solve_t( g, S ) ) );
			REQUIRE( abv2_toe( 0.0f, g, S ) == 0.0f );
			REQUIRE( abv2_knee( 0.0f, g, S ) == 0.0f );
			float flPrev = 0.0f;
			for ( float x = 0.0f; x <= 1.0f; x += 0.01f )
			{
				const float y = abv2_toe( x, g, S );
				REQUIRE( std::isfinite( y ) );
				REQUIRE( y >= flPrev - 1e-6f );
				flPrev = y;
			}
			// The secant guard at B ~ 0 hands back S, never the NaN.
			REQUIRE( std::isfinite( abv2_secant( 0.0f, g, S, false ) ) );
			REQUIRE( std::isfinite( abv2_secant( 0.0f, g, S, true ) ) );
		}
	}
}

TEST_CASE( "abv2_toe: f'(0) equals S -- the closed-form identity abv2_solve_t() exists to guarantee",
           "[effects_curve][abv2]" )
{
	// f'(0) = ((1+t)/t)^(1-g) by direct differentiation of the toe formula
	// (effects_curve.h's own header comment carries the derivation); this
	// checks the CODE reproduces that identity, algebraically, rather than
	// sampling near x = 0 -- t itself can be far smaller than any fixed
	// finite-difference step once g is close to 1 or S is large, so a
	// numerical derivative near zero is not a stable way to test this.
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.05f )
	{
		for ( float S = 1.5f; S <= 8.0f; S += 0.25f )
		{
			const float t = abv2_solve_t( g, S );
			REQUIRE( t > 0.0f );
			const float flSlope = std::pow( ( 1.0f + t ) / t, 1.0f - g );
			REQUIRE_THAT( flSlope, WithinAbs( S, S * 1e-3f ) );
		}
	}
}

TEST_CASE( "abv2_g: min(g_static, g_adapt) in Scene mode, g_static alone in Off mode",
           "[effects_curve][abv2]" )
{
	// Lift 0.5 -> g_static = 1 - 0.6*0.5 = 0.7.
	REQUIRE_THAT( abv2_g_static( 0.5f ), WithinAbs( 0.7f, 1e-6f ) );
	REQUIRE_THAT( abv2_g_static( 0.0f ), WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( abv2_g_static( 1.0f ), WithinAbs( 0.4f, 1e-6f ) );

	// A DARK anchor: g_adapt < g_static, so Scene mode deepens the lift
	// past what Lift alone would give.
	{
		const float g = abv2_g( 0.5f, 0.35f, 0.02f, true );
		REQUIRE( g < 0.7f - 1e-4f );
		REQUIRE( g >= ABV2_G_MIN );
	}
	// A BRIGHT anchor: g_adapt > g_static (the anchor is already above
	// target), so `min` keeps the STATIC floor -- Scene mode never RELAXES
	// the lift on a bright scene (plan 3.5's "no global statistic can find
	// a 1%-of-frame object").
	{
		const float g = abv2_g( 0.5f, 0.35f, 0.8f, true );
		REQUIRE_THAT( g, WithinAbs( 0.7f, 1e-4f ) );
	}
	// Off mode ignores the anchor entirely, even a very dark one.
	{
		const float g = abv2_g( 0.5f, 0.35f, 0.001f, false );
		REQUIRE_THAT( g, WithinAbs( 0.7f, 1e-4f ) );
	}
}

TEST_CASE( "abv2_g: the content anchor ignores an all-black frame -- caller holds the previous value",
           "[effects_curve][abv2]" )
{
	// This is the contract cs_effects_measure.comp's void-frame branch
	// implements (holding the previous smoothed anchor rather than feeding
	// a near-zero raw measurement into g_adapt, which would otherwise pin
	// g at its floor exactly like the retired gamma-based operators did on
	// the report's own capture -- plan section 3.2/3.3). abv2_g_adapt()
	// itself is a pure function of whatever anchor it is given; this pins
	// that an anchor near zero WOULD drive g toward the floor, i.e. that
	// the caller-side exclusion is doing real work and not guarding
	// against a no-op.
	const float gFromVoid = abv2_g_adapt( 0.0005f, 0.35f );
	REQUIRE( gFromVoid < ABV2_G_MIN );   // unclamped -- abv2_g() clamps it
	const float gClamped = abv2_g( 0.5f, 0.35f, 0.0005f, true );
	REQUIRE_THAT( gClamped, WithinAbs( ABV2_G_MIN, 1e-4f ) );
}

TEST_CASE( "abv2_secant: bounded by S, and matches the curve's own secant off the floor",
           "[effects_curve][abv2]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.1f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 0.5f )
		{
			for ( float B = 0.01f; B <= 1.0f; B += 0.02f )
			{
				const float secToe  = abv2_secant( B, g, S, false );
				const float secKnee = abv2_secant( B, g, S, true );
				REQUIRE( secToe  <= S + 1e-4f );
				REQUIRE( secKnee <= S + 1e-4f );
				REQUIRE_THAT( secToe,  WithinAbs( abv2_toe( B, g, S ) / B, 1e-4f ) );
			}
		}
	}
}

// ===========================================================================
//  DARKENING (2026-09-14) -- the user: "Make it able to make the image
//  darker (both full and on parts of the image)". abv2_toe_dark() is the
//  convex mirror of abv2_toe() (same family, f''(x) flips sign with g),
//  abv2_g_dark()/abv2_g_static_dark() choose its exponent the mirrored way,
//  and abv2_curve2()/abv2_secant2() are the two-sided combiner. See
//  effects_curve.h's own DARKENING block for the closed-form proofs these
//  tests exercise numerically.
// ===========================================================================

TEST_CASE( "abv2_toe_dark: f(0) = 0 and f(1) = 1, over the whole g x D grid",
           "[effects_curve][abv2][darken]" )
{
	for ( float g = 1.0f; g <= ABV2_G_MAX; g += 0.2f )
	{
		for ( float D = 1.0f; D <= 4.0f; D += 0.25f )
		{
			REQUIRE_THAT( abv2_toe_dark( 0.0f, g, D ), WithinAbs( 0.0f, 1e-5f ) );
			REQUIRE_THAT( abv2_toe_dark( 1.0f, g, D ), WithinAbs( 1.0f, 1e-4f ) );
		}
	}
}

TEST_CASE( "abv2_toe_dark: identity when g == 1 (no darken requested) or D == 1 "
           "(Max darken at its floor)", "[effects_curve][abv2][darken]" )
{
	for ( float x = 0.0f; x <= 1.0f; x += 0.05f )
	{
		REQUIRE_THAT( abv2_toe_dark( x, 1.0f, 3.0f ), WithinAbs( x, 1e-5f ) );
		REQUIRE_THAT( abv2_toe_dark( x, 2.5f, 1.0f ), WithinAbs( x, 1e-5f ) );
	}
}

TEST_CASE( "abv2_toe_dark: f'(0) equals 1/D -- the closed-form abv2_solve_t_dark() "
           "exists to guarantee", "[effects_curve][abv2][darken]" )
{
	// Same reasoning as abv2_toe's own "f'(0) equals S" test just above:
	// checked ALGEBRAICALLY against the closed form
	// (t/(1+t))^(g-1) == 1/D, not by a numerical derivative near zero --
	// t itself can be far smaller than any fixed finite-difference step.
	for ( float g = 1.05f; g <= ABV2_G_MAX; g += 0.1f )
	{
		for ( float D = 1.25f; D <= 4.0f; D += 0.25f )
		{
			const float t = abv2_solve_t_dark( g, D );
			REQUIRE( t > 0.0f );
			const float flSlope = std::pow( t / ( 1.0f + t ), g - 1.0f );
			REQUIRE_THAT( flSlope, WithinAbs( 1.0f / D, ( 1.0f / D ) * 1e-3f ) );
		}
	}
}

TEST_CASE( "abv2_toe_dark: guarantee -- never lifts, secant never below 1/D, "
           "convex-through-the-origin monotone, over the whole g x D grid",
           "[effects_curve][abv2][darken]" )
{
	for ( float g = 1.0f; g <= ABV2_G_MAX; g += 0.25f )
	{
		for ( float D = 1.0f; D <= 4.0f; D += 0.25f )
		{
			float flPrevSecant = 0.0f;
			float flPrevY = 0.0f;
			for ( float x = 0.01f; x <= 1.0f; x += 0.01f )
			{
				const float y = abv2_toe_dark( x, g, D );
				REQUIRE( std::isfinite( y ) );
				REQUIRE( y >= -1e-5f );
				REQUIRE( y <= 1.0f + 1e-4f );
				REQUIRE( y <= x + 1e-4f );               // never lifts
				const float flSecant = y / x;
				REQUIRE( flSecant >= 1.0f / D - 1e-3f );  // never darkens past 1/D
				REQUIRE( flSecant >= flPrevSecant - 1e-3f );   // secant non-decreasing (convex)
				REQUIRE( y >= flPrevY - 1e-5f );          // monotone
				flPrevSecant = flSecant;
				flPrevY = y;
			}
		}
	}
}

TEST_CASE( "abv2_toe_dark: finite everywhere, no NaN/inf, near every degenerate edge "
           "(g -> 1, D -> 1, and the ABV2_G_MAX ceiling)", "[effects_curve][abv2][darken]" )
{
	const float gEdges[] = { 1.0001f, 1.001f, 1.02f, ABV2_G_MAX - 0.001f, ABV2_G_MAX, ABV2_G_MAX + 1.0f };
	const float dEdges[] = { 1.0f, 1.0001f, 1.001f, 1.02f, 4.0f, 8.0f };
	for ( float g : gEdges )
	{
		for ( float D : dEdges )
		{
			REQUIRE( std::isfinite( abv2_solve_t_dark( g, D ) ) );
			REQUIRE( abv2_toe_dark( 0.0f, g, D ) == 0.0f );
			for ( float x = 0.0f; x <= 1.0f; x += 0.05f )
			{
				const float y = abv2_toe_dark( x, g, D );
				REQUIRE( std::isfinite( y ) );
			}
		}
	}
}

TEST_CASE( "abv2_g_static_dark/abv2_g_dark: max(g_static_dark, g_adapt) in Scene mode, "
           "g_static_dark alone in Off mode", "[effects_curve][abv2][darken]" )
{
	REQUIRE_THAT( abv2_g_static_dark( 0.0f ), WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( abv2_g_static_dark( 0.5f ), WithinAbs( 1.3f, 1e-6f ) );
	REQUIRE_THAT( abv2_g_static_dark( 1.0f ), WithinAbs( 1.6f, 1e-6f ) );

	// A bright scene (anchor > target) drives g_adapt above 1; Scene mode
	// may only DEEPEN the darken (max), never relax it below the static
	// floor -- the mirror of abv2_g()'s own min().
	{
		const float g = abv2_g_dark( 0.0f, 0.35f, 0.9f, true );
		REQUIRE( g > 1.0f );   // g_adapt alone (static floor is 1, off)
	}
	// A dark scene (anchor < target) must NOT relax the static floor.
	{
		const float g = abv2_g_dark( 0.5f, 0.35f, 0.02f, true );
		REQUIRE_THAT( g, WithinAbs( abv2_g_static_dark( 0.5f ), 1e-4f ) );
	}
	// Off mode: static floor alone, whatever the anchor says.
	{
		const float g = abv2_g_dark( 0.5f, 0.35f, 0.9f, false );
		REQUIRE_THAT( g, WithinAbs( abv2_g_static_dark( 0.5f ), 1e-4f ) );
	}
	// The internal ceiling holds.
	{
		const float g = abv2_g_dark( 1.0f, 0.35f, 0.999f, true );
		REQUIRE( g <= ABV2_G_MAX + 1e-4f );
	}
}

TEST_CASE( "abv2_curve2: byte-identical to abv2_curve at Max darken 1 / Darken 0, "
           "over a wide grid", "[effects_curve][abv2][darken]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.1f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 1.0f )
		{
			for ( bool bKnee : { false, true } )
			{
				for ( float target = 0.1f; target <= 0.9f; target += 0.1f )
				{
					// gDark/D at their OWN "off" defaults (1.0 g, D 1.0), as
					// the host always passes when Darken == 0 / Max darken
					// == 1 -- see cs_effects_layer0.comp / EffectsPushData_t.
					for ( float x = 0.0f; x <= 1.0f; x += 0.02f )
					{
						const float flOld = abv2_curve( x, g, S, bKnee );
						const float flNew = abv2_curve2( x, g, S, bKnee, 1.0f, 1.0f, target );
						REQUIRE_THAT( flNew, WithinAbs( flOld, 1e-5f ) );
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_secant2: byte-identical to abv2_secant at Max darken 1 / Darken 0",
           "[effects_curve][abv2][darken]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.15f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 1.5f )
		{
			for ( float B = 0.0f; B <= 1.0f; B += 0.02f )
			{
				const float flOld = abv2_secant( B, g, S, false );
				const float flNew = abv2_secant2( B, g, S, false, 1.0f, 1.0f, 0.35f );
				REQUIRE_THAT( flNew, WithinAbs( flOld, 1e-5f ) );
			}
		}
	}
}

TEST_CASE( "abv2_curve2: continuous at the pivot (C0), bounded, monotone, no NaN/inf, "
           "over the whole g x S x gDark x D x target grid", "[effects_curve][abv2][darken]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.2f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 2.0f )
		{
			for ( float gDark = 1.0f; gDark <= ABV2_G_MAX; gDark += 1.0f )
			{
				for ( float D = 1.0f; D <= 4.0f; D += 1.0f )
				{
					for ( bool bKnee : { false, true } )
					{
						for ( float target = 0.1f; target <= 0.9f; target += 0.2f )
						{
							// C0 at the pivot: the two branches must agree at x == target.
							const float flBelow = abv2_curve2( target, g, S, bKnee, gDark, D, target );
							const float flJustAbove = abv2_curve2( target + 1e-5f, g, S, bKnee, gDark, D, target );
							REQUIRE_THAT( flJustAbove, WithinAbs( flBelow, 1e-3f ) );

							float flPrev = -1.0f;
							for ( float x = 0.0f; x <= 1.0f; x += 0.02f )
							{
								const float y = abv2_curve2( x, g, S, bKnee, gDark, D, target );
								REQUIRE( std::isfinite( y ) );
								REQUIRE( y >= -1e-4f );
								REQUIRE( y <= 1.0f + 1e-4f );
								REQUIRE( y >= flPrev - 1e-4f );   // monotone
								flPrev = y;
							}
						}
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_secant2: bounded by max(S, D)*Detail (task guarantee), over the "
           "whole grid, no NaN/inf", "[effects_curve][abv2][darken]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.2f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 2.0f )
		{
			for ( float gDark = 1.0f; gDark <= ABV2_G_MAX; gDark += 1.0f )
			{
				for ( float D = 1.0f; D <= 4.0f; D += 1.0f )
				{
					for ( float target = 0.1f; target <= 0.9f; target += 0.2f )
					{
						for ( float flDetail = 0.0f; flDetail <= 2.0f; flDetail += 0.5f )
						{
							for ( float B = 0.0f; B <= 1.0f; B += 0.05f )
							{
								const float sec = abv2_secant2( B, g, S, false, gDark, D, target );
								REQUIRE( std::isfinite( sec ) );
								const float flBound = std::max( S, D ) * flDetail;
								REQUIRE( sec * flDetail <= flBound + 1e-3f );
							}
						}
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_binding_dark: mirrors abv2_binding()'s codes for the darken side",
           "[effects_curve][abv2][darken]" )
{
	REQUIRE( abv2_binding_dark( 0.5f, 0.35f, 0.02f, true, true ) == ABV2_BIND_VOID );

	// A bright scene deepens past the static floor -> NONE (g_adapt binds).
	// This now reads on the RESCALED aim, abv2_g_adapt_dark_z() (V2 darken
	// QC, 2026-09-15 -- see effects_curve.h's own note by abv2_binding_dark()):
	// anchor 0.7 vs target 0.35 gives w = (1 - 0.7) / 0.65 ~= 0.4615, g_adapt_z
	// = ln(0.4615) / ln(0.65) ~= 1.795, comfortably inside (1, ABV2_G_MAX) --
	// the OLD un-rescaled abv2_g_adapt(0.5, 0.35) ~= 1.51 that this test used
	// to exercise at anchor 0.5 now lands at g_adapt_z ~= 0.61 (below the
	// static floor of 1.0, i.e. DARKEN_FLOOR, not NONE), which is exactly the
	// bug this fix closes -- so the NONE case needs a deeper anchor to still
	// land past the static floor under the rescaled aim.
	REQUIRE( abv2_binding_dark( 0.0f, 0.35f, 0.7f, true, false ) == ABV2_BIND_NONE );

	// A dark scene: the static floor is stronger than the (irrelevant)
	// adaptive pull toward darken, so the floor is what's binding.
	REQUIRE( abv2_binding_dark( 0.5f, 0.35f, 0.1f, true, false ) == ABV2_BIND_DARKEN_FLOOR );

	// Off mode: the static floor alone.
	REQUIRE( abv2_binding_dark( 0.5f, 0.35f, 0.9f, false, false ) == ABV2_BIND_DARKEN_FLOOR );

	for ( int n = 0; n <= ABV2_BIND_G_MAX; n++ )
		REQUIRE( std::string( abv2_binding_dark_text( n ) ).size() > 0 );
}

// ===========================================================================
//  THE S-CURVE REDESIGN (2026-09-15, V2 darken QC) -- the fixed point at
//  Target, and the two new guarantees the first cut did not have: F(x) <= x
//  for every x above Target (not merely "usually"), and F(Target) == Target
//  exactly (not L(Target)). See effects_curve.h's own "THE FIXED POINT"
//  block for the closed-form proofs these tests exercise numerically.
// ===========================================================================

TEST_CASE( "abv2_curve2 (S-curve): F(Target) == Target exactly, whenever darkening "
           "is genuinely on, over the whole g x S x gDark x D x target grid",
           "[effects_curve][abv2][darken]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.2f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 2.0f )
		{
			for ( float gDark = 1.05f; gDark <= ABV2_G_MAX; gDark += 1.0f )
			{
				for ( float D = 1.25f; D <= 4.0f; D += 1.0f )
				{
					for ( bool bKnee : { false, true } )
					{
						for ( float target = 0.1f; target <= 0.9f; target += 0.2f )
						{
							const float F = abv2_curve2( target, g, S, bKnee, gDark, D, target );
							REQUIRE_THAT( F, WithinAbs( target, 1e-4f ) );
						}
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_curve2 (S-curve): F(x) <= x above Target and F(x)/x >= 1/D -- the "
           "new guarantee the first cut (pivot at L(Target)) did not have",
           "[effects_curve][abv2][darken]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.2f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 2.0f )
		{
			for ( float gDark = 1.05f; gDark <= ABV2_G_MAX; gDark += 1.0f )
			{
				for ( float D = 1.25f; D <= 4.0f; D += 1.0f )
				{
					for ( float target = 0.1f; target <= 0.9f; target += 0.2f )
					{
						for ( float x = target + 0.01f; x <= 1.0f; x += 0.02f )
						{
							const float F = abv2_curve2( x, g, S, false, gDark, D, target );
							REQUIRE( F <= x + 1e-4f );                    // never above raw
							REQUIRE( F >= x / D - 1e-3f );                // never darkened past 1/D
						}
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_curve2 (S-curve): F(x)/x <= S below Target, inherited unchanged "
           "through the rescale", "[effects_curve][abv2][darken]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.2f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 2.0f )
		{
			for ( float D = 1.25f; D <= 4.0f; D += 1.0f )
			{
				for ( float target = 0.1f; target <= 0.9f; target += 0.2f )
				{
					for ( float x = 0.01f; x < target; x += 0.02f )
					{
						const float F = abv2_curve2( x, g, S, false, 1.3f, D, target );
						REQUIRE( F <= x * S + 1e-3f );
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_curve2 (S-curve): the pivot kink's ratio matches (1/D)/L'(1) at the "
           "shipped defaults, and is closer to 1 than the first cut's plain 1/D there",
           "[effects_curve][abv2][darken]" )
{
	// Shipped defaults: Lift 0.5 (g_static 0.7) / Max lift 4 / Darken 0.5
	// (g_static_dark 1.3) / Max darken 2 / Target 0.35, Scene off -- the
	// same numbers shader-effects.md's own worked table uses.
	const float g = 0.7f, S = 4.0f, gDark = 1.3f, D = 2.0f, target = 0.35f;
	const float flEps = 1e-5f;
	const float left  = ( abv2_curve2( target, g, S, false, gDark, D, target )
	                       - abv2_curve2( target - flEps, g, S, false, gDark, D, target ) ) / flEps;
	const float right = ( abv2_curve2( target + flEps, g, S, false, gDark, D, target )
	                       - abv2_curve2( target, g, S, false, gDark, D, target ) ) / flEps;
	REQUIRE_THAT( left, WithinAbs( 0.7030f, 0.01f ) );    // L'(1), matches f'(1) ~= g
	REQUIRE_THAT( right, WithinAbs( 0.5000f, 0.01f ) );   // K'(0) == 1/D exactly
	const float flRatio = right / left;
	REQUIRE_THAT( flRatio, WithinAbs( 0.7113f, 0.01f ) );
	// Closer to 1 (less discontinuous) than the first cut's own plain 1/D
	// ratio at the same D (0.5) -- true whenever L'(1) < 1, which the next
	// test checks holds across the grid.
	REQUIRE( std::abs( flRatio - 1.0f ) < std::abs( ( 1.0f / D ) - 1.0f ) );
}

TEST_CASE( "abv2_curve (toe/knee): the highlight slope L'(1) stays below 1, over a "
           "grid -- what makes the S-curve's own kink ratio always closer to 1 than "
           "the first cut's plain 1/D", "[effects_curve][abv2][darken]" )
{
	const float flH = 1e-3f;
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.1f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 1.0f )
		{
			for ( bool bKnee : { false, true } )
			{
				const float flSlope = ( abv2_curve( 1.0f, g, S, bKnee )
				                         - abv2_curve( 1.0f - flH, g, S, bKnee ) ) / flH;
				REQUIRE( flSlope <= 1.0f + 1e-3f );
			}
		}
	}
}

TEST_CASE( "abv2_curve2 (S-curve): quantised to 8 bits, the pivot's own kink steps "
           "at most 1 code either side -- no visible band beyond ordinary rounding",
           "[effects_curve][abv2][darken]" )
{
	for ( float g = 0.5f; g < 1.0f; g += 0.2f )
	{
		for ( float S = 2.0f; S <= 8.0f; S += 3.0f )
		{
			for ( float gDark = 1.2f; gDark <= ABV2_G_MAX; gDark += 1.5f )
			{
				for ( float D = 1.5f; D <= 4.0f; D += 1.0f )
				{
					for ( float target = 0.15f; target <= 0.85f; target += 0.2f )
					{
						const int nCode = (int)std::lround( target * 255.0f );
						const auto Code = [&]( int i )
						{
							const float x = std::clamp( i, 0, 255 ) / 255.0f;
							return (int)std::lround( abv2_curve2( x, g, S, false, gDark, D, target ) * 255.0f );
						};
						REQUIRE( std::abs( Code( nCode ) - Code( nCode - 1 ) ) <= 2 );
						REQUIRE( std::abs( Code( nCode + 1 ) - Code( nCode ) ) <= 2 );
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_g_adapt_lift_z / abv2_g_adapt_dark_z: the rescaled aim's own "
           "boundary limits -- defer to static at the pivot, saturate at the "
           "internal ceiling/floor at the far edge, monotone in between",
           "[effects_curve][abv2][darken]" )
{
	const float target = 0.35f;
	// Lift-side: z = anchor/target. anchor -> target (z -> 1) defers
	// (g_adapt -> large, min() picks gStatic); anchor -> 0 (z -> 0)
	// saturates toward the internal floor.
	REQUIRE( abv2_g_adapt_lift_z( target * 0.999f, target ) > 5.0f );
	// The z -> 0 limit is g_adapt -> 0, but the approach is LOGARITHMIC --
	// at anchor 0.001 (z = 0.00286) it is still only ~0.18, not near-zero;
	// checked as a bound loose enough to be true, not the limit itself.
	REQUIRE( abv2_g_adapt_lift_z( 0.001f, target ) < 0.3f );
	{
		float flPrev = -1.0f;
		for ( float anchor = 0.001f; anchor < target; anchor += 0.01f )
		{
			const float g = abv2_g_adapt_lift_z( anchor, target );
			REQUIRE( std::isfinite( g ) );
			REQUIRE( g >= flPrev - 1e-4f );   // monotone increasing in anchor
			flPrev = g;
		}
	}
	// Dark-side: w = (1-anchor)/(1-target). anchor -> target (w -> 1)
	// defers (g_adapt -> ~0, max() picks gStatic); anchor -> 1 (w -> 0)
	// saturates toward the internal ceiling.
	REQUIRE( abv2_g_adapt_dark_z( target * 1.001f, target ) < 0.05f );
	REQUIRE( abv2_g_adapt_dark_z( 0.999f, target ) > 5.0f );
	{
		float flPrev = -1.0f;
		for ( float anchor = target + 0.01f; anchor < 1.0f; anchor += 0.01f )
		{
			const float g = abv2_g_adapt_dark_z( anchor, target );
			REQUIRE( std::isfinite( g ) );
			REQUIRE( g >= flPrev - 1e-4f );   // monotone INcreasing as anchor climbs toward white
			flPrev = g;
		}
	}
	// Worked examples from the header comment (Target 0.35, anchor 0.7 / 0.05).
	REQUIRE_THAT( abv2_g_adapt_dark_z( 0.7f, target ), WithinAbs( 1.795f, 0.01f ) );
	// NOT the old (wrong-domain) formula's 0.350 (ln(target)/ln(anchor)
	// directly) -- the rescaled aim's own answer is 0.540, see the header
	// comment next to abv2_g_adapt_lift_z() for why the two differ.
	REQUIRE_THAT( abv2_g_adapt_lift_z( 0.05f, target ), WithinAbs( 0.5395f, 0.01f ) );
}

TEST_CASE( "abv2_g_lift_scurve: byte-identical delegate to abv2_g() at Max darken "
           "1, its own rescaled aim only once Max darken is active",
           "[effects_curve][abv2][darken]" )
{
	for ( float lift = 0.0f; lift <= 1.0f; lift += 0.25f )
	{
		for ( bool bScene : { false, true } )
		{
			for ( float anchor = 0.02f; anchor <= 0.9f; anchor += 0.08f )
			{
				const float flOld = abv2_g( lift, 0.35f, anchor, bScene );
				const float flDelegate = abv2_g_lift_scurve( lift, 0.35f, anchor, bScene, 1.0f );
				REQUIRE_THAT( flDelegate, WithinAbs( flOld, 1e-6f ) );
			}
		}
	}
	// Once Max darken is active, a very dark anchor drives the rescaled aim
	// toward the internal floor same as before, and a Scene-off call still
	// reduces to the static floor alone.
	REQUIRE_THAT( abv2_g_lift_scurve( 0.5f, 0.35f, 0.9f, false, 2.0f ),
	              WithinAbs( abv2_g_static( 0.5f ), 1e-4f ) );
	REQUIRE( abv2_g_lift_scurve( 0.5f, 0.35f, 0.001f, true, 2.0f ) < abv2_g_static( 0.5f ) + 1e-4f );
}

TEST_CASE( "abv2_shoulder: never exceeds the headroom, unit slope at u = 0, 0 when h <= 0",
           "[effects_curve][abv2]" )
{
	REQUIRE_THAT( abv2_shoulder( 0.0f, 0.3f ), WithinAbs( 0.0f, 1e-6f ) );
	REQUIRE_THAT( abv2_shoulder( 5.0f, 0.0f ), WithinAbs( 0.0f, 1e-6f ) );
	for ( float h = 0.05f; h <= 1.0f; h += 0.05f )
	{
		for ( float u = 0.0f; u <= 5.0f; u += 0.05f )
			REQUIRE( abv2_shoulder( u, h ) < h + 1e-5f );
		// Unit slope at u = 0: a small step in u produces (almost) the
		// same step in the shoulder, i.e. invisible wherever nothing
		// would clip.
		const float flEps = 1e-4f;
		REQUIRE_THAT( abv2_shoulder( flEps, h ) / flEps, WithinAbs( 1.0f, 0.01f ) );
	}
}

TEST_CASE( "abv2_detail_apply: negative detail never goes below 0, positive never reaches 1 "
           "unless the base already was", "[effects_curve][abv2]" )
{
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.15f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 1.0f )
		{
			for ( float B = 0.02f; B < 1.0f; B += 0.05f )
			{
				const float fB = abv2_toe( B, g, S );
				const float sec = abv2_secant( B, g, S, false );
				REQUIRE( abv2_detail_apply( fB, -B, sec ) >= -1e-5f );        // D = -B, the darkest possible
				REQUIRE( abv2_detail_apply( fB, 1.0f - B, sec ) <= 1.0f + 1e-5f );   // D = 1-B, the brightest
			}
		}
	}
}

TEST_CASE( "abv2_binding: VOID when the frame is void, otherwise exactly one code",
           "[effects_curve][abv2]" )
{
	REQUIRE( abv2_binding( 0.5f, 0.35f, 0.02f, true, true ) == ABV2_BIND_VOID );
	// A bright anchor with Scene on: the static floor is the stronger of
	// the two, so LIFT_FLOOR.
	REQUIRE( abv2_binding( 0.5f, 0.35f, 0.9f, true, false ) == ABV2_BIND_LIFT_FLOOR );
	// A dark anchor pushing g below G_MIN: G_MIN.
	REQUIRE( abv2_binding( 0.0f, 0.35f, 0.0005f, true, false ) == ABV2_BIND_G_MIN );
	// Off mode never reaches NONE (there is no adapt target to reach): it is
	// LIFT_FLOOR at every Lift, since g_static's own range (0.4..1.0) never
	// touches ABV2_G_MIN (0.2) -- the internal floor exists only to keep
	// `t` finite in Scene mode's g_adapt, not as a reachable Off-mode state.
	REQUIRE( abv2_binding( 1.0f, 0.35f, 0.5f, false, false ) == ABV2_BIND_LIFT_FLOOR );
	REQUIRE( abv2_binding( 0.1f, 0.35f, 0.5f, false, false ) == ABV2_BIND_LIFT_FLOOR );
	for ( const char *psz : { "x" } )   // every code has non-empty wording
	{
		(void)psz;
		for ( int n = 0; n <= ABV2_BIND_VOID; n++ )
			REQUIRE( std::string( abv2_binding_text( n ) ).size() > 0 );
	}
}

TEST_CASE( "reshade.adaptive_brightness_v2 defaults and round-trip, and the older two effects "
           "are untouched by its presence", "[effects_curve][config][abv2]" )
{
	TempConfigHome home;

	Settings s{};
	REQUIRE( s.reshade.adaptive_brightness_v2.enabled == false );
	REQUIRE( s.reshade.adaptive_brightness_v2.mode == "scene" );
	REQUIRE( s.reshade.adaptive_brightness_v2.shape == "toe" );
	REQUIRE_THAT( s.reshade.adaptive_brightness_v2.lift, WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_brightness_v2.target_luminance, WithinAbs( 0.35f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_brightness_v2.max_lift, WithinAbs( 4.0f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_brightness_v2.detail, WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_brightness_v2.scale, WithinAbs( 1.5f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_brightness_v2.adapt_speed, WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE_THAT( s.reshade.adaptive_brightness_v2.clarity, WithinAbs( 0.0f, 1e-6f ) );

	s.reshade.adaptive_brightness_v2.enabled = true;
	s.reshade.adaptive_brightness_v2.mode = "off";
	s.reshade.adaptive_brightness_v2.shape = "knee";
	s.reshade.adaptive_brightness_v2.lift = 0.8f;
	s.reshade.adaptive_brightness_v2.target_luminance = 0.4f;
	s.reshade.adaptive_brightness_v2.max_lift = 6.0f;
	s.reshade.adaptive_brightness_v2.detail = 1.5f;
	s.reshade.adaptive_brightness_v2.scale = 2.0f;
	s.reshade.adaptive_brightness_v2.adapt_speed = 1.2f;
	s.reshade.adaptive_brightness_v2.clarity = 0.6f;
	// The user's own instruction, pinned as a test: the OLDER two effects
	// stay exactly as they were, alongside V2 being configured.
	s.reshade.adaptive_brightness.enabled = true;
	s.reshade.adaptive_brightness.target_luminance = 0.6f;
	ProfileMeta meta;
	meta.name = "EffectsV2";
	REQUIRE( SaveProfile( meta, s ) );

	std::optional<Settings> loaded = LoadProfile( "EffectsV2" );
	REQUIRE( loaded.has_value() );
	const auto &v2 = loaded->reshade.adaptive_brightness_v2;
	REQUIRE( v2.enabled == true );
	REQUIRE( v2.mode == "off" );
	REQUIRE( v2.shape == "knee" );
	REQUIRE_THAT( v2.lift, WithinAbs( 0.8f, 1e-6f ) );
	REQUIRE_THAT( v2.target_luminance, WithinAbs( 0.4f, 1e-6f ) );
	REQUIRE_THAT( v2.max_lift, WithinAbs( 6.0f, 1e-6f ) );
	REQUIRE_THAT( v2.detail, WithinAbs( 1.5f, 1e-6f ) );
	REQUIRE_THAT( v2.scale, WithinAbs( 2.0f, 1e-6f ) );
	REQUIRE_THAT( v2.adapt_speed, WithinAbs( 1.2f, 1e-6f ) );
	REQUIRE_THAT( v2.clarity, WithinAbs( 0.6f, 1e-6f ) );

	// Adaptive Brightness (the OLDER effect) is UNTOUCHED -- this is the
	// user's explicit "DO NOT REMOVE THE ORIGINAL" requirement, pinned.
	REQUIRE( loaded->reshade.adaptive_brightness.enabled == true );
	REQUIRE_THAT( loaded->reshade.adaptive_brightness.target_luminance, WithinAbs( 0.6f, 1e-6f ) );
	REQUIRE( loaded->reshade.adaptive_gamma.enabled == false );

	// An old profile with no adaptive_brightness_v2 object at all (an
	// on-disk file predating this feature) resolves to the compiled-in
	// defaults -- purely additive keys, no migration.
	ProfileMeta meta2;
	meta2.name = "EffectsPreV2";
	Settings sOld{};
	sOld.reshade.adaptive_brightness.enabled = true;
	REQUIRE( SaveProfile( meta2, sOld ) );
	std::optional<Settings> loadedOld = LoadProfile( "EffectsPreV2" );
	REQUIRE( loadedOld.has_value() );
	REQUIRE( loadedOld->reshade.adaptive_brightness_v2.enabled == false );
	REQUIRE( loadedOld->reshade.adaptive_brightness_v2.mode == "scene" );
}

// ===========================================================================
//  STAGE 3 CLARITY (NEW 2026-09-14, plan section 4.9) -- the silhouette
//  band. abv2_clarity_combine() is the one pure function this stage adds;
//  every guarantee it must keep flows through the ALREADY-tested
//  abv2_detail_apply()/abv2_secant(), so these cases pin abv2_clarity_combine
//  itself and its composition with those, on synthetic edges, exactly as the
//  worker task requires.
// ===========================================================================

TEST_CASE( "abv2_clarity_combine: exact identity on D at clarity 0, D + M at clarity 1, "
           "linear in between", "[effects_curve][abv2][clarity]" )
{
	for ( float D = -0.4f; D <= 0.4f; D += 0.05f )
	{
		for ( float M = -0.4f; M <= 0.4f; M += 0.05f )
		{
			REQUIRE_THAT( abv2_clarity_combine( D, M, 0.0f ), WithinAbs( D, 1e-6f ) );
			REQUIRE_THAT( abv2_clarity_combine( D, M, 1.0f ), WithinAbs( D + M, 1e-6f ) );
			REQUIRE_THAT( abv2_clarity_combine( D, M, 0.5f ), WithinAbs( D + 0.5f * M, 1e-6f ) );
			// Out-of-range clarity is clamped, not extrapolated -- a
			// hand-edited config above the panel's own 0..1 range must not
			// widen the guarantee below past what the panel can reach.
			REQUIRE_THAT( abv2_clarity_combine( D, M, 2.0f ), WithinAbs( D + M, 1e-6f ) );
			REQUIRE_THAT( abv2_clarity_combine( D, M, -1.0f ), WithinAbs( D, 1e-6f ) );
		}
	}
}

TEST_CASE( "abv2_clarity_combine + abv2_detail_apply: guarantee 1 widens to EXACTLY "
           "S * Detail * (1 + Clarity), on synthetic step edges", "[effects_curve][abv2][clarity]" )
{
	// A synthetic silhouette step: D and M each up to one step's worth of
	// contrast (the plan's own bound for "an object's own level differs
	// from its surround by at most `step`"), independently signed -- the
	// bound must hold whether or not the two filters agree, since a
	// hand-edited or adversarial scene is not required to be the aligned
	// case Clarity is tuned for.
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.2f )
	{
		for ( float S = 1.0f; S <= 8.0f; S += 1.0f )
		{
			for ( float flDetail = 0.0f; flDetail <= 2.0f; flDetail += 0.5f )
			{
				for ( float clarity = 0.0f; clarity <= 1.0f; clarity += 0.25f )
				{
					for ( float B = 0.02f; B < 1.0f; B += 0.1f )
					{
						const float fB = abv2_toe( B, g, S );
						const float sec = abv2_secant( B, g, S, false ) * flDetail;
						for ( float step = 0.05f; step <= 0.4f; step += 0.1f )
						{
							for ( int sD = -1; sD <= 1; sD += 2 )
							{
								for ( int sM = -1; sM <= 1; sM += 2 )
								{
									const float D = sD * step;
									const float M = sM * step;
									const float Dtotal = abv2_clarity_combine( D, M, clarity );
									const float Yp = abv2_detail_apply( fB, Dtotal, sec );
									const float flBound = S * flDetail * ( 1.0f + clarity ) * step;
									REQUIRE( std::fabs( Yp - fB ) <= flBound + 1e-4f );
								}
							}
						}
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_clarity_combine + abv2_detail_apply: Weber contrast never decreases "
           "when the fine and coarse filters AGREE (the edge-aware case Clarity is built "
           "for), and Clarity 0 reproduces Stage 2 exactly", "[effects_curve][abv2][clarity]" )
{
	// Near a real silhouette edge the finer base B2 tracks the step more
	// closely than the coarser B does, so M = B2 - B and D = Y - B point
	// the SAME direction (the standard unsharp-mask property of two box
	// filters at different radii on a monotone step -- see plan 4.9's own
	// "edge-awareness ... M ~= 0 [at a hard edge], only low-contrast
	// mid-scale structure is boosted", which is the SAME-sign case for any
	// structure that is not already at a=1 in both filters). Modelled here
	// directly by construction (D, M same sign) rather than by simulating
	// the guided filter's box passes -- the guarantee is about the
	// function composition, and the box filters' own halo/edge-awareness
	// properties are covered separately by the GPU regression harness
	// (v2-halo, v2-silhouette in scripts/effects-regression.sh).
	for ( float g = ABV2_G_MIN; g < 1.0f; g += 0.15f )
	{
		for ( float S = 2.0f; S <= 8.0f; S += 2.0f )
		{
			for ( float B = 0.05f; B < 0.95f; B += 0.1f )
			{
				const float fB = abv2_toe( B, g, S );
				const float sec = abv2_secant( B, g, S, false );   // Detail == 1
				for ( float D = 0.05f; D <= 0.3f; D += 0.05f )
				{
					const float baselineYp = abv2_detail_apply( fB, D, sec );
					const float baselineWeber = ( baselineYp - fB ) / std::max( fB, 1e-4f );

					for ( float M = 0.0f; M <= D; M += 0.05f )   // same sign, |M| <= |D|
					{
						for ( float clarity = 0.0f; clarity <= 1.0f; clarity += 0.25f )
						{
							const float Dtotal = abv2_clarity_combine( D, M, clarity );
							const float Yp = abv2_detail_apply( fB, Dtotal, sec );
							const float weber = ( Yp - fB ) / std::max( fB, 1e-4f );

							if ( clarity <= 1e-6f )
							{
								// Exact identity to Stage 2 -- not just "close".
								REQUIRE_THAT( Yp, WithinAbs( baselineYp, 1e-5f ) );
							}
							else
							{
								// Same-sign M can only add magnitude to a
								// positive step, so the output Weber
								// contrast is >= the Clarity-off baseline.
								REQUIRE( weber >= baselineWeber - 1e-5f );
							}
						}
					}
				}
			}
		}
	}
}

TEST_CASE( "abv2_clarity_combine: a hard edge (a ~= 1 in both filters) contributes nothing",
           "[effects_curve][abv2][clarity]" )
{
	// plan 4.9: "a hard, already-visible edge has a ~= 1 in BOTH filters,
	// so M ~= 0 there and no rim is added". At the function level that is
	// simply M == 0 -- both guided filters agree exactly on B, so B2 == B
	// -- and the combine must then be an identity on D regardless of
	// Clarity, which is the "no rim on a real edge" guarantee restated as
	// one line.
	for ( float D = -0.5f; D <= 0.5f; D += 0.05f )
		for ( float clarity = 0.0f; clarity <= 1.0f; clarity += 0.1f )
			REQUIRE_THAT( abv2_clarity_combine( D, 0.0f, clarity ), WithinAbs( D, 1e-6f ) );
}
