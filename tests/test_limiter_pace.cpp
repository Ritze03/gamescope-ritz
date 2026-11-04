// The Frame limiter's pacing rules (src/LimiterPace.h).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "LimiterPace.h"

using namespace limiterpace;

TEST_CASE( "limiter: who paces", "[limiter]" )
{
	CHECK_FALSE( LayerPaces( false, false, 280, 140 ) );   // divides the refresh: vblank grid
	CHECK_FALSE( LayerPaces( false, false, 60, 60 ) );
	CHECK( LayerPaces( false, false, 280, 120 ) );         // the grid would make it 140
	CHECK( LayerPaces( true, false, 280, 140 ) );
	CHECK( LayerPaces( false, true, 280, 140 ) );
}

TEST_CASE( "limiter: the deadline schedule", "[limiter]" )
{
	constexpr uint64_t kInterval = 1'000'000'000ull / 120;

	// A game that keeps up gets the exact rate, however its frame times wobble.
	uint64_t ulDeadline = 1'000'000'000ull;
	const uint64_t ulStart = ulDeadline;
	for ( int i = 1; i <= 1200; i++ )
		ulDeadline = NextDeadline( ulDeadline, ulDeadline + ( i % 3 ) * 1000, 120 );
	CHECK( ulDeadline - ulStart == 1200 * kInterval );

	// One late frame keeps the schedule: the next is held for less.
	CHECK( NextDeadline( ulStart, ulStart + kInterval + 1000, 120 ) == ulStart + kInterval );

	// More than a slot behind: a new schedule from now, no burst of frames to catch up.
	const uint64_t ulLate = ulStart + 3 * kInterval;
	CHECK( NextDeadline( ulStart, ulLate, 120 ) == ulLate );
}
