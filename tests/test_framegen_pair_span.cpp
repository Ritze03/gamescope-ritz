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

TEST_CASE( "pair span guard: arrival times out of order never guard", "[framegen_pair_span]" )
{
	const fgspan::Verdict v = fgspan::Check( kBase + 90 * kMs, kBase, 5.0, 3.0 );
	REQUIRE_FALSE( v.guard );
	REQUIRE( v.spanMs == 0.0 );
	REQUIRE_FALSE( fgspan::Check( kBase, kBase, 5.0, 3.0 ).guard );
}
