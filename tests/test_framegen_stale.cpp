// Unit tests for FrameGen/StaleFrame.h: the verdict of frame generation's stale real-frame guard
// (superdoc/features/frame-generation-stale-frames.md). No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>

#include "FrameGen/StaleFrame.h"

using namespace fghost::stale;

namespace
{
	constexpr int kAll = kGrid * kGrid;

	// eq1..eq3: texels equal to the newest, 2 back and 3 back; v1..v3: whether that ring frame exists.
	Result J( int eq1, int eq2, int eq3, bool v1 = true, bool v2 = true, bool v3 = true )
	{
		const int nEq[ kMaxRef ] = { eq1, eq2, eq3 };
		const bool bValid[ kMaxRef ] = { v1, v2, v3 };
		return Judge( kAll, nEq, bValid, kMaxRef );
	}
}

TEST_CASE( "stale frame: no ring frame at all gives no verdict", "[framegen_stale]" )
{
	CHECK( J( kAll, kAll, kAll, false, false, false ).eVerdict == Verdict::NoReference );
	CHECK( J( 0, 0, 0, false, false, false ).eVerdict == Verdict::NoReference );
	const int nEq[ kMaxRef ] = { kAll, kAll, kAll };
	const bool bValid[ kMaxRef ] = { true, true, true };
	CHECK( Judge( 0, nEq, bValid, kMaxRef ).eVerdict == Verdict::NoReference );   // nothing probed
	CHECK( Judge( kAll, nEq, bValid, 0 ).eVerdict == Verdict::NoReference );      // nothing to compare with
}

TEST_CASE( "stale frame: no older frame valid can never be stale", "[framegen_stale]" )
{
	// Only the newest exists (the first frames after a start or a reset).
	CHECK( J( 0, kAll, kAll, true, false, false ).eVerdict == Verdict::Fresh );
	CHECK( J( 17, kAll, kAll, true, false, false ).eVerdict == Verdict::Fresh );
	const Result r = J( kAll, 0, 0, true, false, false );
	CHECK( r.eVerdict == Verdict::Repeat );
	CHECK( r.nBack == 1 );
}

TEST_CASE( "stale frame: identical to all three is a static picture, not stale", "[framegen_stale]" )
{
	const Result r = J( kAll, kAll, kAll );
	CHECK( r.eVerdict == Verdict::Repeat );
	CHECK( r.nBack == 1 );
}

TEST_CASE( "stale frame: identical to the frame three back only is stale", "[framegen_stale]" )
{
	const Result r = J( 0, 0, kAll );
	CHECK( r.eVerdict == Verdict::Stale );
	CHECK( r.nBack == 3 );
	// Mostly equal to the newest (a slow scene) changes nothing: only a full match counts.
	CHECK( J( kAll - 1, 3000, kAll ).eVerdict == Verdict::Stale );
}

TEST_CASE( "stale frame: identical to the frame two back only is stale", "[framegen_stale]" )
{
	const Result r = J( 0, kAll, 0 );
	CHECK( r.eVerdict == Verdict::Stale );
	CHECK( r.nBack == 2 );
	// Two back and three back alike (the scene stood still before): still stale, the nearer one named.
	const Result r2 = J( 5, kAll, kAll );
	CHECK( r2.eVerdict == Verdict::Stale );
	CHECK( r2.nBack == 2 );
	// A match with a frame the ring does not hold (invalid) is ignored.
	CHECK( J( 0, kAll, 0, true, false, true ).eVerdict == Verdict::Fresh );
	CHECK( J( 0, 0, kAll, true, true, false ).eVerdict == Verdict::Fresh );
}

TEST_CASE( "stale frame: identical to the newest only is a repeat, used as usual", "[framegen_stale]" )
{
	const Result r = J( kAll, 0, 0 );
	CHECK( r.eVerdict == Verdict::Repeat );
	CHECK( r.nBack == 1 );
	CHECK( J( kAll, kAll, 0 ).eVerdict == Verdict::Repeat );
	CHECK( J( kAll, 0, kAll ).eVerdict == Verdict::Repeat );
}

TEST_CASE( "stale frame: identical to none is a fresh frame", "[framegen_stale]" )
{
	const Result r = J( 0, 0, 0 );
	CHECK( r.eVerdict == Verdict::Fresh );
	CHECK( r.nBack == 0 );
	// One differing texel is enough to be different.
	CHECK( J( kAll - 1, kAll - 1, kAll - 1 ).eVerdict == Verdict::Fresh );
}

TEST_CASE( "stale frame: a shallower ring compares only what it has", "[framegen_stale]" )
{
	const int nEq[ kMaxRef ] = { 0, kAll, kAll };
	const bool bValid[ kMaxRef ] = { true, true, true };
	CHECK( Judge( kAll, nEq, bValid, 1 ).eVerdict == Verdict::Fresh );
	CHECK( Judge( kAll, nEq, bValid, 2 ).nBack == 2 );
}

TEST_CASE( "stale frame: the forced test marks every n-th new frame", "[framegen_stale]" )
{
	CHECK_FALSE( ForcedStale( 0, 5 ) );
	CHECK_FALSE( ForcedStale( -3, 6 ) );
	CHECK_FALSE( ForcedStale( 5, 0 ) );
	CHECK_FALSE( ForcedStale( 5, 4 ) );
	CHECK( ForcedStale( 5, 5 ) );
	CHECK( ForcedStale( 5, 10 ) );
	CHECK( ForcedStale( 1, 1 ) );
	CHECK( ForcedStale( 1, 2 ) );
	int nHit = 0;
	for ( unsigned long long i = 1; i <= 100; i++ )
		nHit += ForcedStale( 4, i ) ? 1 : 0;
	CHECK( nHit == 25 );
}
