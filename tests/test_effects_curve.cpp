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
		const float gain  = ab_dyn_gain( sc.p98 * r, flMin, flMax );
		const float gamma = ab_dyn_gamma( sc.p2 * r, sc.p50 * r, gain, flTarget, flMin );
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

			const float gain  = ab_dyn_gain( sc.p98, kMinGain, kMaxGain );
			const float gamma = ab_dyn_gamma( sc.p2, sc.p50, gain, t, kMinGain );
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
	// to ~71 rather than ~50 -- still well clear of the "readable" floor.
	REQUIRE( p2 > 65.0f );          // the 2 % percentile becomes readable
	REQUIRE( p50 > p2 );
	REQUIRE( hi < 255.0f );         // a 240 highlight is not blown out
	REQUIRE( hi > p50 );            // and keeps its rank
	REQUIRE( wht <= 255.0f );
	REQUIRE( hi < wht );            // 240 and 255 stay distinguishable
	// The lift is bounded: the gain is max_gain and the gamma is the floor.
	REQUIRE_THAT( ab_dyn_gain( kDark.p98, kMinGain, kMaxGain ), WithinAbs( kMaxGain, 1e-6f ) );
	REQUIRE_THAT( ab_dyn_gamma( kDark.p2, kDark.p50, kMaxGain, kTarget, kMinGain ),
	              WithinAbs( AB_DYN_GAMMA_MIN, 1e-6f ) );
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
	const float gainOld = ab_dyn_gain( p98, 0.5f, 2.0f );
	const float gainNew = ab_dyn_gain( p98, 0.3f, 4.0f );
	REQUIRE_THAT( gainOld, WithinAbs( gainNew, 1e-6f ) );   // 0.918 either way, not clamped

	const float gammaOld = ab_dyn_gamma( p2, p50, gainOld, 0.5f, 0.5f );
	const float gammaNew = ab_dyn_gamma( p2, p50, gainNew, 0.5f, 0.3f );
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
	// Way below target: floor.
	REQUIRE_THAT( ab_dyn_gamma( 0.001f, 0.002f, 2.0f, 0.9f, 0.5f ), WithinAbs( AB_DYN_GAMMA_MIN, 1e-6f ) );
	// Way above target with bright shadows: ceiling.
	REQUIRE_THAT( ab_dyn_gamma( 0.9f, 0.95f, 1.0f, 0.1f, 0.5f ), WithinAbs( AB_DYN_GAMMA_MAX, 1e-6f ) );
	// Above target with dark shadows: the cap keeps p2 * gain ^ g >= p2 * min_gain.
	const float p2 = 0.05f, gain = 0.918f, minGain = 0.5f;
	const float g = ab_dyn_gamma( p2, 0.7f, gain, 0.5f, minGain );
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
						const float gain  = ab_dyn_gain( sc.p98 * shift, lo, hi );
						const float gamma = ab_dyn_gamma( sc.p2 * shift, sc.p50 * shift, gain, kTarget, lo );
						REQUIRE( gain >= lo - 1e-6f );
						REQUIRE( gain <= hi + 1e-6f );
						REQUIRE( gamma >= AB_DYN_GAMMA_MIN - 1e-6f );
						REQUIRE( gamma <= AB_DYN_GAMMA_MAX + 1e-6f );
					}
}
