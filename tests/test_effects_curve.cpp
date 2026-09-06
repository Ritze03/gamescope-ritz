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
	constexpr float kMinGain = 0.5f;
	constexpr float kMaxGain = 2.0f;

	// Whole curve for one channel value, with the dry/wet mix the shader
	// applies (mix(c, graded, strength)).
	float Apply( float x, const Scene &sc, float flStrength,
	             float flTarget = kTarget, float flMin = kMinGain, float flMax = kMaxGain )
	{
		const float gain  = ab_dyn_gain( sc.p98, flMin, flMax );
		const float gamma = ab_dyn_gamma( sc.p2, sc.p50, gain, flTarget, flMin );
		const float y = ab_dyn_curve( x, gain, gamma );
		return x + ( y - x ) * flStrength;
	}

	// Every scene / bound / target combination the properties are checked over.
	const Scene kScenes[] = { kDark, kBright, kMid,
		{ 0.0f, 0.0f, 0.0f },          // all black
		{ 1.0f, 1.0f, 1.0f },          // all white
		{ 0.02f, 0.3f, 0.99f },        // dark room, bright window
		{ 0.6f, 0.8f, 0.85f } };       // flat and bright
	const float kTargets[]  = { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f };
	const float kMinGains[] = { 0.5f, 0.75f, 1.0f };
	const float kMaxGains[] = { 1.0f, 1.5f, 2.0f };
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
	REQUIRE( p2 > 30.0f );          // the 2 % percentile becomes readable
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
	// p2 * min_gain.
	REQUIRE( sh >= 30.0f * kMinGain - 0.5f );
	REQUIRE( sh > 4.0f );
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
