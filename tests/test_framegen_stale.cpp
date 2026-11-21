// Unit tests for FrameGen/StaleFrame.h: the verdict of frame generation's stale real-frame guard
// (superdoc/features/frame-generation-stale-frames.md). No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>

#include "FrameGen/StaleFrame.h"

#include <string>
#include <vector>

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

TEST_CASE( "stale frame: a buffer is handed back once its release point has signalled", "[framegen_stale]" )
{
	CHECK_FALSE( HandedBack( false, 10, 5 ) );    // no release point: unknown, never "handed back"
	CHECK_FALSE( HandedBack( true, 100, 0 ) );    // point 0 is no point (a timeline always reads >= 0)
	CHECK_FALSE( HandedBack( true, 4, 5 ) );      // still held
	CHECK( HandedBack( true, 5, 5 ) );            // exactly signalled
	CHECK( HandedBack( true, 9, 5 ) );            // a later point of the same timeline covers it
}

TEST_CASE( "stale frame: the commit facts format into one bounded log suffix", "[framegen_stale]" )
{
	char sz[ 512 ];
	CommitFacts f;
	FormatFacts( f, sz, sizeof( sz ) );
	CHECK( std::string( sz ) == "commit gone" );

	f.bFound = true;
	f.bExplicitSync = true;
	f.bReadyAtCommit = true;
	f.bRecommit = true;
	f.nReleased = 1;
	f.nWritePending = 0;
	f.flReceivedToDoneMs = 0.0;
	f.flDoneToTakenMs = 1.25;
	f.flTakenToProbeMs = 2.5;
	f.uTexRefs = 3;
	f.uFbRefs = 1;
	FormatFacts( f, sz, sizeof( sz ) );
	const std::string s( sz );
	CHECK( s.find( "explicit sync" ) != std::string::npos );
	CHECK( s.find( "ready at commit" ) != std::string::npos );
	CHECK( s.find( "re-commit without a new buffer" ) != std::string::npos );
	CHECK( s.find( "released to the game YES" ) != std::string::npos );
	CHECK( s.find( "write fence signalled" ) != std::string::npos );
	CHECK( s.find( "refs 3/1" ) != std::string::npos );

	f.nReleased = -1;
	f.nWritePending = -1;
	FormatFacts( f, sz, sizeof( sz ) );
	CHECK( std::string( sz ).find( "released to the game n/a, write fence n/a" ) != std::string::npos );

	// Truncated, never overrun (the size read at run time: a constant one is a -Wformat-truncation warning).
	volatile unsigned long ulSmall = 16;
	std::vector<char> small( ulSmall, 'x' );
	FormatFacts( f, small.data(), ulSmall );
	CHECK( std::string( small.data() ).size() == small.size() - 1 );
	CHECK( FormatFacts( f, nullptr, 0 ) == 0 );
}
