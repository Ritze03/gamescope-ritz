// The nested-Wayland tearing hint as a state (src/FrameGen/TearHint.h): a run
// that alternates output-timer paints and vblank paints, or a pass-through that
// flickers, must not flip it. Journal evidence for the rule: 22,995 flips in 44
// minutes of one Forza Horizon 6 session (2026-10-05).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "FrameGen/TearHint.h"

namespace
{
	constexpr uint64_t kMs = 1000ull * 1000ull;

	// Drives a Hint with a per-paint callback and counts the flips of its output.
	struct Run
	{
		fgtear::Hint hint;
		uint64_t now = 1000 * kMs;
		bool last = false;
		int flips = 0;

		bool Step( uint64_t dtNs, bool bActive, bool bExcluded = false )
		{
			now += dtNs;
			const bool b = hint.Update( now, bActive, bExcluded );
			if ( b != last )
				flips++;
			last = b;
			return b;
		}
	};
}

TEST_CASE( "Tear hint: timer and vblank paints alternating never flip it", "[framegen_tearhint]" )
{
	Run r;
	// ~280 Hz output timer with vblank paints between timer paints: bActive is the
	// same state on both kinds of paint, so it never changes.
	for ( int i = 0; i < 5000; i++ )
		r.Step( 3571 * 1000ull, true );
	CHECK( r.flips == 1 );   // off -> async, once
	CHECK( r.last );
}

TEST_CASE( "Tear hint: the plan flickering for a few frames does not flip it", "[framegen_tearhint]" )
{
	Run r;
	// Generating / passing through toggling every ~90 ms (the journal's rate).
	for ( int i = 0; i < 400; i++ )
		r.Step( 8 * kMs, ( i / 11 ) % 2 == 0 );
	CHECK( r.flips == 1 );
	CHECK( r.last );
}

TEST_CASE( "Tear hint: goes back to vsync only after the timer has been off for the hold", "[framegen_tearhint]" )
{
	Run r;
	r.Step( kMs, true );
	CHECK( r.last );
	r.Step( fgtear::kHoldNs - 2 * kMs, false );
	CHECK( r.last );    // inactive for just under the hold
	r.Step( 3 * kMs, false );
	CHECK_FALSE( r.last );
	CHECK( r.flips == 2 );
}

TEST_CASE( "Tear hint: never active means never async", "[framegen_tearhint]" )
{
	Run r;
	for ( int i = 0; i < 100; i++ )
		r.Step( 10 * kMs, false );
	CHECK( r.flips == 0 );
	CHECK_FALSE( r.last );
}

TEST_CASE( "Tear hint: an overlay or fade is an immediate exception, resumed after the dwell", "[framegen_tearhint]" )
{
	Run r;
	r.Step( kMs, true );
	CHECK( r.last );
	CHECK_FALSE( r.Step( kMs, true, true ) );   // immediate
	CHECK_FALSE( r.Step( fgtear::kReenterNs - 2 * kMs, true ) );
	CHECK( r.Step( 3 * kMs, true ) );
	CHECK( r.flips == 3 );
}

TEST_CASE( "Tear hint: an exclusion that pulses faster than the dwell stays on vsync", "[framegen_tearhint]" )
{
	Run r;
	r.Step( kMs, true );
	for ( int i = 0; i < 200; i++ )
		r.Step( 10 * kMs, true, i % 5 == 0 );   // excluded every 50 ms
	CHECK_FALSE( r.last );
	CHECK( r.flips == 2 );   // on, off, and no flapping after that
}
