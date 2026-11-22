// The judder measure (src/FrameGen/Judder.h): synthetic presented/content sequences.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>

#include "FrameGen/Judder.h"

using Catch::Matchers::WithinAbs;

namespace
{
	constexpr uint64_t kRefresh = 1'000'000'000ull / 280;   // 3.571 ms
	constexpr uint64_t kStart = 10'000'000'000ull;
}

TEST_CASE( "judder: an even stream is smooth", "[judder]" )
{
	fgjudder::Meter m;
	for ( uint64_t i = 0; i < 280; i++ )
		m.Note( kStart + i * kRefresh, kStart - 5'000'000 + i * kRefresh, kRefresh );
	const fgjudder::Stats s = m.Take();
	CHECK( s.shown == 280 );
	CHECK( s.judged == 279 );
	CHECK( s.repeat == 0 );
	CHECK( s.uneven == 0 );
	CHECK( s.gap == 0 );
	CHECK( s.unknown == 0 );
	CHECK_THAT( s.errMaxMs, WithinAbs( 0.0, 1e-9 ) );
	// The next interval starts empty, the chain continues.
	m.Note( kStart + 280 * kRefresh, kStart - 5'000'000 + 280 * kRefresh, kRefresh );
	const fgjudder::Stats s2 = m.Take();
	CHECK( s2.shown == 1 );
	CHECK( s2.judged == 1 );
}

TEST_CASE( "judder: the host drops one refresh in nine", "[judder]" )
{
	// The game delivers every refresh; every 9th refresh the host shows nothing (a gap), and
	// the next frame shown is the newest one: content and presented step both two refreshes.
	// The game frame of the skipped refresh is never shown, so the step is still even.
	fgjudder::Meter m;
	uint64_t ulSlot = 0;
	for ( int i = 0; i < 90; i++ )
	{
		ulSlot++;
		if ( ulSlot % 9 == 0 )
			ulSlot++;   // skipped refresh
		m.Note( kStart + ulSlot * kRefresh, kStart + ulSlot * kRefresh - 1'000'000, kRefresh );
	}
	const fgjudder::Stats s = m.Take();
	CHECK( s.shown == 90 );
	CHECK( s.gap == 11 );     // refreshes 9, 18, ... 99 skipped
	CHECK( s.uneven == 0 );
}

TEST_CASE( "judder: a dropped game frame per nine at the refresh is uneven", "[judder]" )
{
	// The host shows every refresh, but one game frame in nine is dropped before it: the
	// content steps two frames while the screen steps one -- a visible hitch.
	fgjudder::Meter m;
	uint64_t ulContent = kStart;
	for ( uint64_t i = 0; i < 90; i++ )
	{
		ulContent += ( i % 9 == 8 ) ? 2 * kRefresh : kRefresh;
		m.Note( kStart + i * kRefresh, ulContent, kRefresh );
	}
	const fgjudder::Stats s = m.Take();
	CHECK( s.gap == 0 );
	CHECK( s.uneven == 10 );
	CHECK( s.repeat == 0 );
	CHECK_THAT( s.errMaxMs, WithinAbs( kRefresh / 1e6, 1e-6 ) );
	CHECK_THAT( s.errP99Ms, WithinAbs( kRefresh / 1e6, 1e-6 ) );
	CHECK( s.errMeanMs > 0.3 );
	CHECK( s.errMeanMs < 0.5 );
}

TEST_CASE( "judder: a repeated frame", "[judder]" )
{
	// Frame 5 is shown again at the next refresh: one repeat (content step 0, uneven) and
	// the step after it covers two frames in one refresh (uneven again).
	fgjudder::Meter m;
	const uint64_t content[] = { 1, 2, 3, 4, 5, 5, 7, 8, 9, 10 };
	for ( uint64_t i = 0; i < 10; i++ )
		m.Note( kStart + i * kRefresh, kStart + content[ i ] * kRefresh, kRefresh );
	const fgjudder::Stats s = m.Take();
	CHECK( s.repeat == 1 );
	CHECK( s.uneven == 2 );
	CHECK( s.gap == 0 );
}

TEST_CASE( "judder: unknown content is counted, not judged", "[judder]" )
{
	fgjudder::Meter m;
	m.Note( kStart, kStart, kRefresh );
	m.Note( kStart + kRefresh, 0, kRefresh );                         // unknown
	m.Note( kStart + 2 * kRefresh, kStart + 2 * kRefresh, kRefresh );  // previous unknown: not judged
	m.Note( kStart + 3 * kRefresh, kStart + 3 * kRefresh, kRefresh );
	m.Note( kStart + 3 * kRefresh + 500'000'000ull, kStart + 4 * kRefresh, kRefresh );   // idle: not judged, no gap
	const fgjudder::Stats s = m.Take();
	CHECK( s.shown == 5 );
	CHECK( s.unknown == 1 );
	CHECK( s.judged == 1 );
	CHECK( s.gap == 0 );
	CHECK( s.uneven == 0 );
}
