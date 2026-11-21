// Unit tests for FrameGen/PairSpanGuard.h: when a pair of real frames is too far apart to interpolate
// (superdoc/features/frame-generation-renderer.md, "Pair span guard"). No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "FrameGen/PairSpanGuard.h"

namespace
{
	constexpr int64_t kMs = 1000000;
	constexpr int64_t kBase = 5000 * kMs;   // an arbitrary monotonic clock reading
}

TEST_CASE( "pair span guard: a normal pair is not guarded", "[framegen_pair_span]" )
{
	const fgspan::Verdict v = fgspan::Check( kBase, kBase + 5 * kMs + kMs / 2, 5.5, 3.0 );
	REQUIRE_FALSE( v.guard );
	REQUIRE( v.spanMs == 5.5 );
	REQUIRE( v.limitMs == 16.5 );
}

TEST_CASE( "pair span guard: a hitch below the pacer's reset is guarded", "[framegen_pair_span]" )
{
	// The user's captures: 43.9 ms between the two frames of a pair at a 5.5 ms game interval.
	const fgspan::Verdict v = fgspan::Check( kBase, kBase + 43900000, 5.5, 3.0 );
	REQUIRE( v.guard );
	REQUIRE( v.spanMs == 43.9 );
}

TEST_CASE( "pair span guard: exactly at the limit is not guarded, just over is", "[framegen_pair_span]" )
{
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 15 * kMs, 5.0, 3.0 ).guard );
	REQUIRE( fgspan::Check( kBase, kBase + 15 * kMs + 1, 5.0, 3.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 15 * kMs - 1, 5.0, 3.0 ).guard );
}

TEST_CASE( "pair span guard: an unknown arrival time never guards", "[framegen_pair_span]" )
{
	REQUIRE_FALSE( fgspan::Check( -1, kBase + 100 * kMs, 5.0, 3.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( kBase, -1, 5.0, 3.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( -1, -1, 5.0, 3.0 ).guard );
}

TEST_CASE( "pair span guard: an unknown or zero interval never guards", "[framegen_pair_span]" )
{
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 90 * kMs, 0.0, 3.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 90 * kMs, -5.0, 3.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 90 * kMs, std::numeric_limits<double>::quiet_NaN(), 3.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 90 * kMs, std::numeric_limits<double>::infinity(), 3.0 ).guard );
	REQUIRE( fgspan::Check( kBase, kBase + 90 * kMs, 0.0, 3.0 ).limitMs == 0.0 );
}

TEST_CASE( "pair span guard: k of 0 or below switches it off", "[framegen_pair_span]" )
{
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 90 * kMs, 5.0, 0.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 90 * kMs, 5.0, -1.0 ).guard );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase + 90 * kMs, 5.0, std::numeric_limits<double>::quiet_NaN() ).guard );
	// A small positive k still works.
	REQUIRE( fgspan::Check( kBase, kBase + 6 * kMs, 5.0, 1.1 ).guard );
}

TEST_CASE( "pair span guard: the recent pair spans' median", "[framegen_pair_span]" )
{
	// Steady 10 ms pairs with one 51 ms hitch among them: the median stays 10.
	const int64_t nSteady[] = { 0, 10 * kMs, 20 * kMs, 30 * kMs, 81 * kMs, 91 * kMs, 101 * kMs, 111 * kMs, 121 * kMs, 131 * kMs };
	REQUIRE( fgspan::RecentSpanMs( nSteady, 10 ) == 10.0 );
	// Even count: the mean of the middle two (spans 5, 10, 20, 40 -> 15).
	const int64_t nEven[] = { 0, 5 * kMs, 15 * kMs, 35 * kMs, 75 * kMs };
	REQUIRE( fgspan::RecentSpanMs( nEven, 5 ) == 15.0 );
	// Too few spans: no reference.
	REQUIRE( fgspan::RecentSpanMs( nSteady, 3 ) == 0.0 );
	REQUIRE( fgspan::RecentSpanMs( nSteady, 0 ) == 0.0 );
	// Unknown or out-of-order neighbours are skipped, not counted as spans.
	const int64_t nHoles[] = { 0, -1, 20 * kMs, 30 * kMs, 25 * kMs, 35 * kMs, 45 * kMs };
	REQUIRE( fgspan::RecentSpanMs( nHoles, 7 ) == 10.0 );
	// Only the last kRecentSpans spans count.
	const int64_t nOld[] = { 0, 100 * kMs, 200 * kMs, 300 * kMs, 302 * kMs, 304 * kMs, 306 * kMs, 308 * kMs, 310 * kMs, 312 * kMs, 314 * kMs, 316 * kMs };
	REQUIRE( fgspan::RecentSpanMs( nOld, 12 ) == 2.0 );
}

TEST_CASE( "pair span guard: arrival times out of order never guard", "[framegen_pair_span]" )
{
	const fgspan::Verdict v = fgspan::Check( kBase + 90 * kMs, kBase, 5.0, 3.0 );
	REQUIRE_FALSE( v.guard );
	REQUIRE( v.spanMs == 0.0 );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase, 5.0, 3.0 ).guard );
}
