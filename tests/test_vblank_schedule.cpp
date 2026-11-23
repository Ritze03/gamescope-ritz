// Unit tests for the refresh timer's scheduling decisions (src/vblankmanager.hpp,
// gamescope::vblanksched): the next wake-up from anchor / period / lead / now, one tick
// per vblank, and which host `presented` times may anchor the grid. See
// superdoc/features/frame-generation-host-scanout.md ("The refresh timer and the host's
// feedback"). No compositor, no timer: the main loop's use of them is simulated.
#include <catch2/catch_test_macros.hpp>

#include "vblankmanager.hpp"

#include <cstdint>
#include <vector>

using namespace gamescope::vblanksched;

namespace
{
	constexpr uint64_t kMs = 1'000'000ull;
	constexpr uint64_t kP = 3'571'390ull;           // 280.003 Hz
	constexpr uint64_t kT0 = 1'000'000'000'000ull;  // an anchor far from 0

	// One timer schedule as CVBlankTimer::CalcNextWakeupTime() makes it: the wake-up on the
	// grid, then the one-per-vblank step. Returns the target vblank; *pWake its wake-up.
	uint64_t Schedule( uint64_t ulAnchor, uint64_t ulLead, uint64_t ulNow, uint64_t ulLastFired, uint64_t *pWake )
	{
		uint64_t ulWake = NextWakeup( ulAnchor, kP, ulLead, ulNow );
		uint64_t ulTarget = ulWake + ulLead;
		const uint64_t ulSteps = OnePerVBlankSteps( ulTarget, ulLastFired, kP );
		ulWake += ulSteps * kP;
		ulTarget += ulSteps * kP;
		*pWake = ulWake;
		return ulTarget;
	}
}

TEST_CASE( "vblank schedule: the next wake-up is a whole number of periods from the anchor", "[vblank_schedule]" )
{
	const uint64_t ulLead = 2 * kMs;
	// Right after the anchor: the next vblank's wake-up.
	CHECK( NextWakeup( kT0, kP, ulLead, kT0 + 1 ) == kT0 + kP - ulLead );
	// Exactly on a wake-up point: that point (not before now, "now" itself is fine).
	CHECK( NextWakeup( kT0, kP, ulLead, kT0 + kP - ulLead ) == kT0 + kP - ulLead );
	// Long after the anchor (a free-running timer): still on the grid.
	const uint64_t ulNow = kT0 + 1000 * kP + 123;
	const uint64_t ulWake = NextWakeup( kT0, kP, ulLead, ulNow );
	CHECK( ulWake >= ulNow );
	CHECK( ulWake - ulNow < kP );
	CHECK( ( ulWake + ulLead - kT0 ) % kP == 0 );
}

TEST_CASE( "vblank schedule: free-running, one tick per refresh whatever the lead does", "[vblank_schedule]" )
{
	// The main loop: wake at the scheduled point, paint (draw d), the paint reports a draw
	// time that changes the lead, then the tick's own re-arm schedules the next one.
	const uint64_t aLeads[] = { 2'000'000, 1'500'000, 3'571'390, 1'500'000, 2'900'000, 1'600'000, 3'200'000, 1'500'000 };
	uint64_t ulWake = 0;
	uint64_t ulLastFired = Schedule( kT0, aLeads[ 0 ], kT0 + 1, 0, &ulWake );
	std::vector<uint64_t> targets{ ulLastFired };
	for ( int i = 1; i < 2000; i++ )
	{
		const uint64_t ulDraw = 300'000 + ( i % 7 ) * 50'000;   // 0.3-0.6 ms
		const uint64_t ulNow = ulWake + ulDraw;                // the re-arm after the paint
		ulLastFired = Schedule( kT0, aLeads[ i % 8 ], ulNow, ulLastFired, &ulWake );
		targets.push_back( ulLastFired );
	}
	for ( size_t i = 1; i < targets.size(); i++ )
		REQUIRE( targets[ i ] - targets[ i - 1 ] == kP );
}

TEST_CASE( "vblank schedule: a lead that shrinks after a paint cannot give a vblank a second tick", "[vblank_schedule]" )
{
	// Wake-up for V0 with a 3 ms lead, the paint takes 0.5 ms, and the draw it reports
	// shrinks the lead to 1.5 ms: V0 - 1.5 ms is still in the future.
	const uint64_t ulV0 = kT0 + kP;
	const uint64_t ulNow = ulV0 - 3 * kMs + kMs / 2;
	const uint64_t ulRaw = NextWakeup( kT0, kP, 3 * kMs / 2, ulNow );
	REQUIRE( ulRaw + 3 * kMs / 2 == ulV0 );                     // without the rule: V0 again
	CHECK( OnePerVBlankSteps( ulV0, ulV0, kP ) == 1 );          // with it: the next refresh
	uint64_t ulWake = 0;
	CHECK( Schedule( kT0, 3 * kMs / 2, ulNow, ulV0, &ulWake ) == ulV0 + kP );
	// A later target is left alone.
	CHECK( OnePerVBlankSteps( ulV0 + kP, ulV0, kP ) == 0 );
	// No previous wake-up yet: nothing to compare with.
	CHECK( OnePerVBlankSteps( ulV0, 0, kP ) == 0 );
}

TEST_CASE( "vblank schedule: grid phase and tolerance", "[vblank_schedule]" )
{
	CHECK( GridPhase( kT0 + 5 * kP, kT0, kP ) == 0 );
	CHECK( GridPhase( kT0 + 5 * kP + 100'000, kT0, kP ) == 100'000 );
	CHECK( GridPhase( kT0 + 5 * kP - 600'000, kT0, kP ) == -600'000 );
	CHECK( GridPhase( kT0 - 2 * kP + 10, kT0, kP ) == 10 );       // before the anchor too
	CHECK( OnGrid( 178'000, kP, 5 ) );                            // 5% of 3.57 ms = 178.6 us
	CHECK_FALSE( OnGrid( 180'000, kP, 5 ) );
	CHECK_FALSE( OnGrid( -600'000, kP, 5 ) );
}

TEST_CASE( "vblank anchor filter: an early feedback does not move the grid, a real vblank corrects it", "[vblank_schedule]" )
{
	AnchorFilter F;
	// A host showing the frame at once: stamped 0.63 ms before the vblank gamescope aimed at.
	CHECK_FALSE( F.Decide( kT0 + kP - 630'000, kT0, kP, 5 ) );
	// A real vblank, a little off the grid (host clock vs the reported refresh): taken.
	CHECK( F.Decide( kT0 + 3 * kP + 40'000, kT0, kP, 5 ) );
	CHECK( F.Decide( kT0 + 3 * kP - 40'000, kT0, kP, 5 ) );
	// The anchor's own vblank reported again: nothing to learn.
	CHECK_FALSE( F.Decide( kT0, kT0, kP, 5 ) );
	CHECK_FALSE( F.Decide( kT0 + 10'000, kT0, kP, 5 ) );
	// The default 1%: a headless host stamping 0.33 ms early at 120 Hz (3.9%) would creep
	// the grid 0.33 ms per feedback at 5%; at 1% it is off the grid.
	constexpr uint64_t kP120 = 8'333'333ull;
	CHECK_FALSE( F.Decide( kT0 + 2 * kP120 - 325'000, kT0, kP120, 1 ) );
	CHECK( F.Decide( kT0 + 2 * kP120 + 3'000, kT0, kP120, 1 ) );
	// No grid yet: anything anchors.
	CHECK( F.Decide( kT0, 0, kP, 5 ) );
}

TEST_CASE( "vblank anchor filter: stamps that follow gamescope's commits never take over", "[vblank_schedule]" )
{
	// Commit-driven stamps: each at its tick's wake-up + draw + host latency, the draw
	// varying by frame (0.9-1.4 ms, so 0.23-0.73 ms early): no common grid of their own.
	AnchorFilter F;
	const uint64_t ulLead = 2 * kMs;
	int nTaken = 0;
	for ( int i = 1; i <= 3000; i++ )
	{
		const uint64_t ulDraw = 900'000 + uint64_t( ( i * 7919 ) % 501 ) * 1'000;
		const uint64_t ulStamp = kT0 + uint64_t( i ) * 4 * kP - ulLead + ulDraw + 370'000;
		nTaken += F.Decide( ulStamp, kT0, kP, 5 ) ? 1 : 0;
	}
	CHECK( nTaken == 0 );
}

TEST_CASE( "vblank anchor filter: a host that really moved its phase is followed after a run that agrees", "[vblank_schedule]" )
{
	// The host's vblank now sits 1 ms later than the grid: every stamp is off our grid but
	// on one grid of its own.
	AnchorFilter F;
	const uint64_t ulShift = kMs;
	for ( uint32_t i = 1; i < AnchorFilter::kAgree; i++ )
		REQUIRE_FALSE( F.Decide( kT0 + uint64_t( i ) * kP + ulShift, kT0, kP, 5 ) );
	CHECK( F.Decide( kT0 + uint64_t( AnchorFilter::kAgree ) * kP + ulShift, kT0, kP, 5 ) );
	// An on-grid stamp in between restarts the count.
	AnchorFilter G;
	for ( uint32_t i = 1; i < AnchorFilter::kAgree; i++ )
		REQUIRE_FALSE( G.Decide( kT0 + uint64_t( i ) * kP + ulShift, kT0, kP, 5 ) );
	REQUIRE( G.Decide( kT0 + 40 * kP, kT0, kP, 5 ) );
	CHECK_FALSE( G.Decide( kT0 + 41 * kP + ulShift, kT0 + 40 * kP, kP, 5 ) );
}

TEST_CASE( "vblank anchor filter: an early-stamping host cannot speed the timer up", "[vblank_schedule]" )
{
	// The loop of the field bug: a painted tick's feedback is stamped 0.4-0.9 ms before its
	// vblank (the draw varies). Without the filter each one re-anchors the grid that much
	// earlier; with it the timer keeps whole periods. 76 paints per 280 refreshes, as in a
	// 60 fps menu. Returns the ticks over 280 refresh periods (281 counting both ends).
	auto Run = []( bool bFilter, bool bJitter )
	{
		AnchorFilter F;
		uint64_t ulAnchor = kT0, ulWake = 0;
		const uint64_t ulLead = 2 * kMs;
		uint64_t ulLastFired = Schedule( ulAnchor, ulLead, kT0 + 1, 0, &ulWake );
		const uint64_t ulFirst = ulLastFired;
		int nTicks = 1;
		while ( ulLastFired - ulFirst < 280 * kP )
		{
			const uint64_t ulNow = ulWake + 500'000;
			if ( nTicks % 280 < 76 )
			{
				const uint64_t ulEarly = bJitter ? 400'000 + uint64_t( ( nTicks * 7919 ) % 501 ) * 1'000 : 630'000;
				const uint64_t ulStamp = ulLastFired - ulEarly;
				if ( !bFilter || F.Decide( ulStamp, ulAnchor, kP, 5 ) )
					ulAnchor = ulStamp;
			}
			ulLastFired = Schedule( ulAnchor, ulLead, ulNow, ulLastFired, &ulWake );
			nTicks++;
		}
		return nTicks;
	};
	CHECK( Run( false, true ) > 290 );
	CHECK( Run( true, true ) == 281 );
	// Worst case: stamps exactly periodic (a headless host) agree with each other, so the
	// phase-follow rule takes one per kAgree rejected ones: at most one grid move per 30
	// feedbacks, here 76 / 30 * 0.63 / 3.57 = 0.45 extra ticks.
	CHECK( Run( false, false ) > 290 );
	CHECK( Run( true, false ) <= 282 );
}

TEST_CASE( "vblank anchor filter: a nested refresh below the host's is a foreign grid", "[vblank_schedule]" )
{
	CHECK( ForeignGrid( 90'000, 120'000 ) );
	CHECK( ForeignGrid( 180'000, 280'003 ) );
	CHECK_FALSE( ForeignGrid( 280'000, 280'003 ) );
	CHECK_FALSE( ForeignGrid( 120'000, 120'000 ) );
	CHECK_FALSE( ForeignGrid( 0, 120'000 ) );
	CHECK_FALSE( ForeignGrid( 120'000, 0 ) );
}
