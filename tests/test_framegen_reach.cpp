// Unit tests for FrameGen/FrameGenReach.h: the four motion-search reach values packed in one 32-bit word
// (superdoc/features/frame-generation-settings.md). No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "FrameGen/FrameGenReach.h"

using namespace fghost;

namespace
{
	Config RoundTrip( const Config &in )
	{
		Config out;
		out.searchRadius = out.searchRadiusTop = out.pyramidMinSide = 0;   // prove every field is written
		out.searchPenalty = -1.0f;
		UnpackReach( PackReach( in ), &out );
		return out;
	}
}

TEST_CASE( "framegen reach: the defaults round-trip exactly", "[framegen_reach]" )
{
	const Config d;
	REQUIRE( d.searchRadius == 2 );
	REQUIRE( d.searchRadiusTop == 6 );
	REQUIRE( d.pyramidMinSide == 48 );
	REQUIRE( d.searchPenalty == 0.5f );
	const Config r = RoundTrip( d );
	REQUIRE( r.searchRadius == 2 );
	REQUIRE( r.searchRadiusTop == 6 );
	REQUIRE( r.pyramidMinSide == 48 );
	REQUIRE( r.searchPenalty == 0.5f );   // exactly: 50 / 100.0
}

TEST_CASE( "framegen reach: the extremes round-trip", "[framegen_reach]" )
{
	Config lo; lo.searchRadius = 1; lo.searchRadiusTop = 1; lo.pyramidMinSide = 16; lo.searchPenalty = 0.0f;
	Config hi; hi.searchRadius = 12; hi.searchRadiusTop = 12; hi.pyramidMinSide = 256; hi.searchPenalty = 255.0f;
	Config r = RoundTrip( lo );
	REQUIRE( ( r.searchRadius == 1 && r.searchRadiusTop == 1 && r.pyramidMinSide == 16 && r.searchPenalty == 0.0f ) );
	r = RoundTrip( hi );
	REQUIRE( ( r.searchRadius == 12 && r.searchRadiusTop == 12 && r.pyramidMinSide == 256 && r.searchPenalty == 255.0f ) );
	// The overlay's 0.05 steps.
	for ( int n = 0; n <= 80; n++ )
	{
		Config c; c.searchPenalty = float( n * 5 ) / 100.0f;
		const float fl = RoundTrip( c ).searchPenalty;
		REQUIRE( ( fl > c.searchPenalty - 0.0001f && fl < c.searchPenalty + 0.0001f ) );
	}
}

TEST_CASE( "framegen reach: out-of-range values clamp, non-finite falls back to the default", "[framegen_reach]" )
{
	Config c; c.searchRadius = 0; c.searchRadiusTop = 99; c.pyramidMinSide = 1; c.searchPenalty = -5.0f;
	Config r = RoundTrip( c );
	REQUIRE( ( r.searchRadius == 1 && r.searchRadiusTop == 12 && r.pyramidMinSide == 16 && r.searchPenalty == 0.0f ) );
	c.pyramidMinSide = 5000; c.searchPenalty = 1000.0f;
	r = RoundTrip( c );
	REQUIRE( ( r.pyramidMinSide == 256 && r.searchPenalty == 255.0f ) );
	c.searchPenalty = std::numeric_limits<float>::quiet_NaN();
	REQUIRE( RoundTrip( c ).searchPenalty == 0.5f );
	c.searchPenalty = std::numeric_limits<float>::infinity();
	REQUIRE( RoundTrip( c ).searchPenalty == 0.5f );
}

TEST_CASE( "framegen reach: a word with a field out of range unpacks to the defaults", "[framegen_reach]" )
{
	Config r;
	UnpackReach( 0u, &r );   // radii 0, side 0: invalid; penalty 0 is valid
	REQUIRE( ( r.searchRadius == 2 && r.searchRadiusTop == 6 && r.pyramidMinSide == 48 && r.searchPenalty == 0.0f ) );
	UnpackReach( 0xFFFFFFFFu, &r );   // radii 15, side 511, penalty 32767 (> 25500)
	REQUIRE( ( r.searchRadius == 2 && r.searchRadiusTop == 6 && r.pyramidMinSide == 48 && r.searchPenalty == 0.5f ) );
}

TEST_CASE( "framegen reach: a change of any one value changes the word", "[framegen_reach]" )
{
	const uint32_t u = PackReach( Config{} );
	Config c; c.searchRadius = 3; REQUIRE( PackReach( c ) != u );
	c = Config{}; c.searchRadiusTop = 7; REQUIRE( PackReach( c ) != u );
	c = Config{}; c.pyramidMinSide = 49; REQUIRE( PackReach( c ) != u );
	c = Config{}; c.searchPenalty = 0.51f; REQUIRE( PackReach( c ) != u );
	REQUIRE( u != 0u );   // EnsureReady() uses 0 as "nothing applied"
}
