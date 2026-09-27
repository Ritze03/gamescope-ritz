// Unit tests for src/Overlay/NullBinds.h's `nullbinds::Engine` -- the WASD
// null-binds / SOCD-cleaning engine's PURE half. No compositor, no ImGui, no
// config file, no thread: every test drives the engine directly with a fake
// clock (plain uint64_t milliseconds chosen by the test) and a deterministic
// jitter source, and asserts on the Emit list / deadline the engine hands
// back -- exactly what NullBinds.cpp's glue is contractually required to
// actually send to wlserver_key().

#include <catch2/catch_test_macros.hpp>

#include <linux/input-event-codes.h>

#include "Overlay/NullBinds.h"

using namespace gamescope::nullbinds;

namespace
{
	Settings MakeSettings( bool bPairAD = true, bool bPairWS = true, int nDelayMs = 5, int nJitterMs = 0 )
	{
		Settings s;
		s.enabled = true;
		s.pair_ad = bPairAD;
		s.pair_ws = bPairWS;
		s.delay_ms = nDelayMs;
		s.jitter_ms = nJitterMs;
		return s;
	}

	// A jitter source that always returns the same fixed offset, regardless
	// of the jitter range asked for -- lets a test pick an exact, predictable
	// gap instead of a range.
	JitterFn FixedJitter( int nOffset )
	{
		return [ nOffset ]( int ){ return nOffset; };
	}
}

TEST_CASE( "nullbinds: master switch off passes every key through untouched", "[nullbinds]" )
{
	Engine e; // default-constructed: enabled == false
	Result r = e.OnPhysical( KEY_A, true, 0 );
	REQUIRE( r.consumed == false );
	REQUIRE( r.emits.empty() );
	REQUIRE( r.has_deadline == false );
}

TEST_CASE( "nullbinds: keys outside the two pairs are never consumed", "[nullbinds]" )
{
	REQUIRE( Engine::IsPairKey( KEY_A ) == true );
	REQUIRE( Engine::IsPairKey( KEY_D ) == true );
	REQUIRE( Engine::IsPairKey( KEY_W ) == true );
	REQUIRE( Engine::IsPairKey( KEY_S ) == true );
	REQUIRE( Engine::IsPairKey( KEY_SPACE ) == false );

	Engine e;
	e.SetSettings( MakeSettings() );
	Result r = e.OnPhysical( KEY_SPACE, true, 0 );
	REQUIRE( r.consumed == false );
	REQUIRE( r.emits.empty() );
}

TEST_CASE( "nullbinds: a fresh press on an idle pair goes out immediately", "[nullbinds]" )
{
	Engine e;
	e.SetSettings( MakeSettings() );

	Result r = e.OnPhysical( KEY_A, true, 0 );
	REQUIRE( r.consumed == true );
	REQUIRE( r.emits.size() == 1 );
	REQUIRE( r.emits[ 0 ].key == (uint32_t)KEY_A );
	REQUIRE( r.emits[ 0 ].press == true );
	REQUIRE( r.has_deadline == false );
}

TEST_CASE( "nullbinds: pressing the other pair key releases the old winner immediately "
           "and schedules the new one after the delay", "[nullbinds]" )
{
	Engine e;
	e.SetJitterSource( FixedJitter( 0 ) );
	e.SetSettings( MakeSettings( true, true, /*delay*/5, /*jitter*/0 ) );

	e.OnPhysical( KEY_A, true, 0 );                      // idle -> immediate A

	Result r = e.OnPhysical( KEY_D, true, 100 );         // A still held -> switch
	REQUIRE( r.consumed == true );
	REQUIRE( r.emits.size() == 1 );
	REQUIRE( r.emits[ 0 ].key == (uint32_t)KEY_A );
	REQUIRE( r.emits[ 0 ].press == false );              // A released immediately
	REQUIRE( r.has_deadline == true );
	REQUIRE( r.deadline_ms == 105 );                     // 100 + delay(5)

	Result rEarly = e.OnTimer( 104 );
	REQUIRE( rEarly.emits.empty() );                     // not due yet

	Result rDue = e.OnTimer( 105 );
	REQUIRE( rDue.emits.size() == 1 );
	REQUIRE( rDue.emits[ 0 ].key == (uint32_t)KEY_D );
	REQUIRE( rDue.emits[ 0 ].press == true );             // D pressed at +delay
	REQUIRE( rDue.has_deadline == false );
}

TEST_CASE( "nullbinds: releasing the winner while the other is still held "
           "re-presses the other after the delay", "[nullbinds]" )
{
	Engine e;
	e.SetJitterSource( FixedJitter( 0 ) );
	e.SetSettings( MakeSettings( true, true, 5, 0 ) );

	e.OnPhysical( KEY_A, true, 0 );          // idle -> immediate A
	e.OnPhysical( KEY_D, true, 100 );        // switch -> release A, schedule D @105
	e.OnTimer( 105 );                        // D now sent; A still physically held

	Result r = e.OnPhysical( KEY_D, false, 200 ); // release the winner
	REQUIRE( r.consumed == true );
	REQUIRE( r.emits.size() == 1 );
	REQUIRE( r.emits[ 0 ].key == (uint32_t)KEY_D );
	REQUIRE( r.emits[ 0 ].press == false );  // D released immediately
	REQUIRE( r.has_deadline == true );
	REQUIRE( r.deadline_ms == 205 );          // 200 + delay(5)

	Result rDue = e.OnTimer( 205 );
	REQUIRE( rDue.emits.size() == 1 );
	REQUIRE( rDue.emits[ 0 ].key == (uint32_t)KEY_A );
	REQUIRE( rDue.emits[ 0 ].press == true ); // A re-pressed after the delay
}

TEST_CASE( "nullbinds: releasing the key that already lost the pair sends nothing", "[nullbinds]" )
{
	Engine e;
	e.SetJitterSource( FixedJitter( 0 ) );
	e.SetSettings( MakeSettings( true, true, 5, 0 ) );

	e.OnPhysical( KEY_A, true, 0 );
	e.OnPhysical( KEY_D, true, 100 );
	e.OnTimer( 105 );                       // D is the sent winner; A is the loser, still held

	Result r = e.OnPhysical( KEY_A, false, 300 ); // release the loser
	REQUIRE( r.consumed == true );
	REQUIRE( r.emits.empty() );
	REQUIRE( r.has_deadline == false );
}

TEST_CASE( "nullbinds: a pending press is cancelled if the desired winner "
           "changes again before it fires", "[nullbinds]" )
{
	Engine e;
	e.SetJitterSource( FixedJitter( 0 ) );
	e.SetSettings( MakeSettings( true, true, 5, 0 ) );

	e.OnPhysical( KEY_A, true, 0 );                 // idle -> immediate A
	Result r1 = e.OnPhysical( KEY_D, true, 1 );     // switch -> release A, schedule D @6
	REQUIRE( r1.has_deadline == true );
	REQUIRE( r1.deadline_ms == 6 );

	// D lets go again before its own scheduled press ever fires; A is still
	// physically held the whole time, so the desired winner reverts to A.
	Result r2 = e.OnPhysical( KEY_D, false, 3 );
	REQUIRE( r2.emits.empty() );          // D was never actually sent -- nothing to release
	REQUIRE( r2.has_deadline == true );
	REQUIRE( r2.deadline_ms == 8 );        // a FRESH delay for A: 3 + 5

	// D's original deadline is now stale and must not fire.
	Result rStale = e.OnTimer( 6 );
	REQUIRE( rStale.emits.empty() );

	Result rDue = e.OnTimer( 8 );
	REQUIRE( rDue.emits.size() == 1 );
	REQUIRE( rDue.emits[ 0 ].key == (uint32_t)KEY_A );
	REQUIRE( rDue.emits[ 0 ].press == true );
}

TEST_CASE( "nullbinds: jitter is added to the delay and the gap is clamped to >= 0", "[nullbinds]" )
{
	// +jitter
	{
		Engine e;
		e.SetJitterSource( FixedJitter( 3 ) );
		e.SetSettings( MakeSettings( true, true, 5, 3 ) );
		e.OnPhysical( KEY_A, true, 0 );
		Result r = e.OnPhysical( KEY_D, true, 0 );
		REQUIRE( r.has_deadline == true );
		REQUIRE( r.deadline_ms == 8 ); // 5 + 3
	}
	// -jitter, still within [delay-jitter, delay+jitter]
	{
		Engine e;
		e.SetJitterSource( FixedJitter( -3 ) );
		e.SetSettings( MakeSettings( true, true, 5, 3 ) );
		e.OnPhysical( KEY_A, true, 0 );
		Result r = e.OnPhysical( KEY_D, true, 0 );
		REQUIRE( r.has_deadline == true );
		REQUIRE( r.deadline_ms == 2 ); // 5 - 3
	}
	// A jitter draw that would push the gap negative clamps to 0, and a
	// clamped-to-zero gap is sent immediately rather than scheduled (same
	// as a genuine 0/0 setting) -- see the very next test for that in full.
	{
		Engine e;
		e.SetJitterSource( FixedJitter( -20 ) );
		e.SetSettings( MakeSettings( true, true, 0, 20 ) );
		e.OnPhysical( KEY_A, true, 0 );
		Result r = e.OnPhysical( KEY_D, true, 0 );
		REQUIRE( r.has_deadline == false );
		REQUIRE( r.emits.size() == 2 );
		REQUIRE( r.emits[ 0 ].press == false ); // A released
		REQUIRE( r.emits[ 1 ].press == true );  // D pressed, back-to-back
	}
}

TEST_CASE( "nullbinds: zero delay and zero jitter sends the release and the press "
           "back-to-back, release first", "[nullbinds]" )
{
	Engine e;
	e.SetJitterSource( FixedJitter( 0 ) );
	e.SetSettings( MakeSettings( true, true, 0, 0 ) );

	e.OnPhysical( KEY_W, true, 0 );
	Result r = e.OnPhysical( KEY_S, true, 5 );

	REQUIRE( r.emits.size() == 2 );
	REQUIRE( r.emits[ 0 ].key == (uint32_t)KEY_W );
	REQUIRE( r.emits[ 0 ].press == false );
	REQUIRE( r.emits[ 1 ].key == (uint32_t)KEY_S );
	REQUIRE( r.emits[ 1 ].press == true );
	REQUIRE( r.has_deadline == false );
}

TEST_CASE( "nullbinds: turning the master switch off while keys are held leaves nothing stuck", "[nullbinds]" )
{
	Engine e;
	e.SetJitterSource( FixedJitter( 0 ) );
	e.SetSettings( MakeSettings( true, true, 5, 0 ) );

	e.OnPhysical( KEY_A, true, 0 );             // idle -> immediate A (sent)
	Result r1 = e.OnPhysical( KEY_D, true, 1 ); // switch -> release A, schedule D (never fires)
	REQUIRE( r1.emits.size() == 1 );
	REQUIRE( r1.emits[ 0 ].press == false );
	// State here: A held, not sent. D held, not sent (pending only).

	Settings off = MakeSettings();
	off.enabled = false;
	Result r2 = e.SetSettings( off );

	// Both physical keys are still down; disabling must forward BOTH of
	// them now (nothing left artificially released, nothing left stuck).
	REQUIRE( r2.emits.size() == 2 );
	bool bSawA = false, bSawD = false;
	for ( const Emit &em : r2.emits )
	{
		if ( em.key == (uint32_t)KEY_A ) { bSawA = true; REQUIRE( em.press == true ); }
		if ( em.key == (uint32_t)KEY_D ) { bSawD = true; REQUIRE( em.press == true ); }
	}
	REQUIRE( bSawA );
	REQUIRE( bSawD );
	REQUIRE( r2.has_deadline == false ); // D's pending press was cancelled, not fired late

	// And from here on, physical events pass straight through again.
	Result r3 = e.OnPhysical( KEY_A, false, 100 );
	REQUIRE( r3.consumed == false );
	REQUIRE( r3.emits.empty() );
}

TEST_CASE( "nullbinds: the A/D and W/S pairs are independent of each other", "[nullbinds]" )
{
	Engine e;
	e.SetJitterSource( FixedJitter( 0 ) );
	e.SetSettings( MakeSettings( true, true, 5, 0 ) );

	Result r1 = e.OnPhysical( KEY_A, true, 0 );
	Result r2 = e.OnPhysical( KEY_W, true, 0 );
	REQUIRE( r1.emits.size() == 1 );
	REQUIRE( r1.emits[ 0 ].key == (uint32_t)KEY_A );
	REQUIRE( r2.emits.size() == 1 );
	REQUIRE( r2.emits[ 0 ].key == (uint32_t)KEY_W ); // unaffected by A/D's state
}

TEST_CASE( "nullbinds: disabling one pair only affects that pair", "[nullbinds]" )
{
	Engine e;
	e.SetSettings( MakeSettings( /*pair_ad*/false, /*pair_ws*/true, 5, 0 ) );

	Result rAD = e.OnPhysical( KEY_A, true, 0 );
	REQUIRE( rAD.consumed == false ); // A/D passes straight through

	Result rWS = e.OnPhysical( KEY_W, true, 0 );
	REQUIRE( rWS.consumed == true );  // W/S is still handled
	REQUIRE( rWS.emits.size() == 1 );
}
