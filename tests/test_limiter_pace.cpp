// The Frame limiter's pacing rules (src/LimiterPace.h) and Auto V-Sync's limit (src/AutoVsync.h).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "LimiterPace.h"
#include "FpsLimitApply.h"
#include "AutoVsync.h"

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

TEST_CASE( "limiter: the saved value is pushed on a change only", "[limiter]" )
{
	int nLast = -1;
	CHECK( fpslimit::NeedsPush( nLast, 120 ) );      // startup: pushed
	CHECK_FALSE( fpslimit::NeedsPush( nLast, 120 ) ); // unrelated reload: a live limit stays
	CHECK( fpslimit::NeedsPush( nLast, 0 ) );        // profile with no limit: back to unlimited
	CHECK( fpslimit::NeedsPush( nLast, 5 ) );        // 1-9 is raised to the floor
	CHECK( nLast == fpslimit::kMin );
	CHECK( fpslimit::NeedsPush( nLast, 9000 ) );
	CHECK( nLast == fpslimit::kMax );
	int nFresh = -1;
	CHECK( fpslimit::NeedsPush( nFresh, 0 ) );       // a saved 0 still counts as a first push
}

// EffectiveLimit( on, nestedWayland, vrr, frameGen, tearingRequested, hostVsync, userLimit, refreshHz )
TEST_CASE( "auto v-sync: the effective limit", "[limiter]" )
{
	using autovsync::EffectiveLimit;
	// Engages: tearing not requested, or requested but the host vsyncs; no user limit or one above the refresh.
	CHECK( EffectiveLimit( true, true, false, false, false, false, false, 0, 120 ) == 120 );
	CHECK( EffectiveLimit( true, true, false, false, true, false, true, 0, 280 ) == 280 );
	CHECK( EffectiveLimit( true, true, false, false, false, false, false, 300, 280 ) == 280 );
	CHECK( EffectiveLimit( true, true, false, false, true, false, true, 480, 280 ) == 280 );
	// The output can show more than one frame per refresh: the user's limit (or none).
	CHECK( EffectiveLimit( true, true, false, false, true, false, false, 0, 280 ) == 0 );
	CHECK( EffectiveLimit( true, true, false, false, true, false, false, 300, 280 ) == 300 );
	// A user limit at or below the refresh is never changed.
	CHECK( EffectiveLimit( true, true, false, false, false, false, false, 60, 120 ) == 60 );
	CHECK( EffectiveLimit( true, true, false, false, true, false, true, 120, 120 ) == 120 );
	// Switch off, another backend, VRR, frame generation, or no refresh known: the user's limit.
	CHECK( EffectiveLimit( false, true, false, false, false, false, false, 0, 120 ) == 0 );
	CHECK( EffectiveLimit( false, true, false, false, true, false, true, 200, 120 ) == 200 );
	CHECK( EffectiveLimit( true, false, false, false, false, false, false, 0, 120 ) == 0 );
	CHECK( EffectiveLimit( true, true, true, false, false, false, false, 0, 120 ) == 0 );
	CHECK( EffectiveLimit( true, true, false, true, false, false, false, 0, 120 ) == 0 );
	CHECK( EffectiveLimit( true, true, false, false, false, false, false, 0, 0 ) == 0 );
	// The auto limit is the refresh, so the existing limiter keeps it on the vblank grid (divisor 1).
	CHECK_FALSE( limiterpace::LayerPaces( false, false, 280, EffectiveLimit( true, true, false, false, true, false, true, 0, 280 ) ) );
}

// Reason(): the first blocking condition, as the `status:` line names it.
TEST_CASE( "auto v-sync: the reason it limits or not", "[limiter]" )
{
	using autovsync::Reason;
	using autovsync::Why;
	CHECK( Reason( true, true, false, false, false, false, false, 0, 280 ) == Why::Limits );
	CHECK( Reason( false, false, true, true, true, false, false, 0, 280 ) == Why::Off );        // the first one wins
	CHECK( Reason( true, false, true, false, false, false, false, 0, 280 ) == Why::NotNested );
	CHECK( Reason( true, true, true, true, false, false, false, 0, 280 ) == Why::Vrr );
	CHECK( Reason( true, true, false, true, false, false, false, 0, 280 ) == Why::FrameGen );
	CHECK( Reason( true, true, false, false, false, false, false, 0, 0 ) == Why::NoRefresh );
	CHECK( Reason( true, true, false, false, true, false, false, 0, 280 ) == Why::HostTears );
	CHECK( Reason( true, true, false, false, true, false, false, 60, 280 ) == Why::HostTears );
	CHECK( Reason( true, true, false, false, false, false, false, 60, 280 ) == Why::UserLimit );
	CHECK( std::string( autovsync::Text( Why::HostTears ) ) == "the host tears (verdict)" );
}

// The 2026-11-22 rule: gamescope compositing its paints limits even with tearing on and the
// verdict at "tearing" (it never samples then): composited paints are one sync flip per tick.
TEST_CASE( "auto v-sync: compositing limits like a host that does not tear", "[limiter]" )
{
	using autovsync::EffectiveLimit;
	using autovsync::Reason;
	using autovsync::Why;
	using autovsync::Cause;
	CHECK( Reason( true, true, false, false, true, true, false, 0, 280 ) == Why::Limits );
	CHECK( EffectiveLimit( true, true, false, false, true, true, false, 0, 280 ) == 280 );
	CHECK( EffectiveLimit( true, true, false, false, true, true, false, 480, 280 ) == 280 );
	// A user limit at or below the refresh still wins (60 stays 60).
	CHECK( EffectiveLimit( true, true, false, false, true, true, false, 60, 280 ) == 60 );
	CHECK( Reason( true, true, false, false, true, true, false, 60, 280 ) == Why::UserLimit );
	// Not under VRR, frame generation, another backend, or with the switch off.
	CHECK( EffectiveLimit( true, true, true, false, true, true, false, 0, 280 ) == 0 );
	CHECK( EffectiveLimit( true, true, false, true, true, true, false, 0, 280 ) == 0 );
	CHECK( EffectiveLimit( true, false, false, false, true, true, false, 0, 280 ) == 0 );
	CHECK( EffectiveLimit( false, true, false, false, true, true, false, 0, 280 ) == 0 );
	// Divisor 1 on the vblank grid, as for the other causes.
	CHECK_FALSE( limiterpace::LayerPaces( false, false, 280, EffectiveLimit( true, true, false, false, true, true, false, 0, 280 ) ) );
	// The cause: only HostVsync keeps the hint async under the limit.
	CHECK( autovsync::LimitCause( false, false ) == Cause::NoTearing );
	CHECK( autovsync::LimitCause( false, true ) == Cause::NoTearing );
	CHECK( autovsync::LimitCause( true, true ) == Cause::HostVsync );
	CHECK( autovsync::LimitCause( true, false ) == Cause::Composites );
	CHECK( std::string( autovsync::CauseText( Cause::Composites ) ) == "gamescope draws every frame itself, one per refresh" );
}

TEST_CASE( "auto v-sync: the compositing debounce", "[limiter]" )
{
	using Gate = autovsync::CompositeGate;
	constexpr uint64_t ms = 1'000'000ull;
	constexpr uint64_t t0 = 1'000'000'000ull;
	constexpr uint64_t kTick = 1'000'000'000ull / 280;

	// Nothing noted: off.
	{
		Gate g;
		CHECK_FALSE( g.Composites( t0 ) );
	}
	// Every paint composited: on after kEngageNs, not before.
	{
		Gate g;
		uint64_t t = t0;
		for ( ; t < t0 + 200 * ms; t += kTick )
		{
			g.Note( t, true );
			CHECK_FALSE( g.Composites( t ) );
		}
		for ( ; t < t0 + Gate::kEngageNs + kTick; t += kTick )
			g.Note( t, true );
		CHECK( g.Composites( t ) );
		// Stops compositing: stays on for kHoldNs, then lifts.
		const uint64_t tLast = t;
		g.Note( tLast, true );
		for ( t = tLast + kTick; t <= tLast + Gate::kHoldNs; t += kTick )
		{
			g.Note( t, false );
			CHECK( g.Composites( t ) );
		}
		CHECK_FALSE( g.Composites( tLast + Gate::kHoldNs + 1 ) );
		// Starts again: needs another kEngageNs.
		const uint64_t tAgain = tLast + 2 * Gate::kHoldNs;
		g.Note( tAgain, true );
		CHECK_FALSE( g.Composites( tAgain ) );
		g.Note( tAgain + Gate::kEngageNs, true );
		CHECK( g.Composites( tAgain + Gate::kEngageNs ) );
	}
	// Flapping per paint (every other paint composited): on, and it never toggles.
	{
		Gate g;
		int nChanges = 0;
		bool bWas = false;
		for ( int i = 0; i < 2800; i++ )
		{
			const uint64_t t = t0 + i * kTick;
			g.Note( t, i % 2 == 0 );
			const bool b = g.Composites( t );
			nChanges += b != bWas;
			bWas = b;
		}
		CHECK( bWas );
		CHECK( nChanges == 1 );
	}
	// A lone composited paint (a forced repaint, one fade frame): never engages.
	{
		Gate g;
		g.Note( t0, true );
		for ( uint64_t t = t0; t < t0 + 2 * Gate::kHoldNs; t += kTick )
		{
			g.Note( t, false );
			CHECK_FALSE( g.Composites( t ) );
		}
	}
	// Short bursts closer together than kHoldNs count as one run.
	{
		Gate g;
		for ( uint64_t t = t0; t <= t0 + 600 * ms; t += 200 * ms )
			g.Note( t, true );
		CHECK( g.Composites( t0 + 600 * ms ) );
	}
}
