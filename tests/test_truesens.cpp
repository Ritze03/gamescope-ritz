// Unit tests for src/Overlay/TrueSens.h's pure half: the multiplier maths, the
// calculator, the recorder and the whole-count carry. No compositor, no config, no
// thread. The build is -ffast-math, so the invalid-input cases are the ones that
// prove the range-based validation survives it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "Overlay/TrueSens.h"

using namespace gamescope::truesens;
using Catch::Matchers::WithinAbs;

namespace
{
	// Built from bits so the compiler cannot fold or reason about them under
	// fast-math (std::numeric_limits<double>::quiet_NaN() is allowed to be
	// "impossible" there).
	double FromBits( uint64_t u )
	{
		double d;
		std::memcpy( &d, &u, sizeof( d ) );
		return d;
	}
	const double kNaN = FromBits( 0x7FF8000000000000ull );
	const double kInf = FromBits( 0x7FF0000000000000ull );
}

TEST_CASE( "truesens: IsFinite reads the bit pattern", "[truesens]" )
{
	CHECK( IsFinite( 0.0 ) );
	CHECK( IsFinite( -5.5 ) );
	CHECK( IsFinite( 1e300 ) );
	CHECK_FALSE( IsFinite( kNaN ) );
	CHECK_FALSE( IsFinite( kInf ) );
	CHECK_FALSE( IsFinite( -kInf ) );
	CHECK_FALSE( IsPositive( 0.0 ) );
	CHECK_FALSE( IsPositive( -1.0 ) );
	CHECK_FALSE( IsPositive( kNaN ) );
	CHECK_FALSE( IsPositive( kInf ) );
	CHECK( IsPositive( 0.001 ) );
}

TEST_CASE( "truesens: conversions", "[truesens]" )
{
	CHECK_THAT( InchesFromCounts( 8000, 800 ), WithinAbs( 10.0, 1e-9 ) );
	CHECK_THAT( TargetCounts( 10.0, 800 ), WithinAbs( 8000.0, 1e-9 ) );
	CHECK_THAT( CmFromInches( 10.0 ), WithinAbs( 25.4, 1e-9 ) );
	// round trip: a DPI change needs no new recording, only a new target.
	CHECK_THAT( TargetCounts( InchesFromCounts( 5000, 800 ), 1600 ), WithinAbs( 10000.0, 1e-6 ) );
	CHECK( InchesFromCounts( 0, 800 ) == 0.0 );
	CHECK( InchesFromCounts( -5, 800 ) == 0.0 );
	CHECK( InchesFromCounts( 5000, 0 ) == 0.0 );
	CHECK( InchesFromCounts( kNaN, 800 ) == 0.0 );
	CHECK( TargetCounts( 0.0, 800 ) == 0.0 );
	CHECK( TargetCounts( kInf, 800 ) == 0.0 );
	CHECK( ClampDpi( 5 ) == kMinDpi );
	CHECK( ClampDpi( 800 ) == 800 );
	CHECK( ClampDpi( 999999 ) == kMaxDpi );
}

TEST_CASE( "truesens: multiplier", "[truesens]" )
{
	// game turns in 6000 counts, the player wants 7500 (9.375 in at 800 DPI).
	CHECK_THAT( Multiplier( 6000, 9.375, 800 ), WithinAbs( 0.8, 1e-9 ) );
	CHECK_THAT( Multiplier( 5000, 5.0, 800 ), WithinAbs( 1.25, 1e-9 ) );
	CHECK_THAT( Multiplier( 4000, 5.0, 800 ), WithinAbs( 1.0, 1e-9 ) );

	SECTION( "every invalid input leaves the game alone" )
	{
		CHECK( Multiplier( 0, 5.0, 800 ) == 1.0 );
		CHECK( Multiplier( -100, 5.0, 800 ) == 1.0 );
		CHECK( Multiplier( 5000, 0, 800 ) == 1.0 );
		CHECK( Multiplier( 5000, -1.0, 800 ) == 1.0 );
		CHECK( Multiplier( 5000, 5.0, 0 ) == 1.0 );
		CHECK( Multiplier( 5000, 5.0, -800 ) == 1.0 );
		CHECK( Multiplier( kNaN, 5.0, 800 ) == 1.0 );
		CHECK( Multiplier( 5000, kNaN, 800 ) == 1.0 );
		CHECK( Multiplier( kInf, 5.0, 800 ) == 1.0 );
		CHECK( Multiplier( 5000, kInf, 800 ) == 1.0 );
	}
	SECTION( "within the unity band the multiplier is exactly 1 (no scaling)" )
	{
		// 4005 / 4000 = 1.00125: just outside the band, applied.
		CHECK_THAT( Multiplier( 4005, 5.0, 800 ), WithinAbs( 1.00125, 1e-9 ) );
		// 4003 / 4000 = 1.00075 and 3997 / 4000 = 0.99925: inside, passed through untouched.
		CHECK( Multiplier( 4003, 5.0, 800 ) == 1.0 );
		CHECK( Multiplier( 3997, 5.0, 800 ) == 1.0 );
		CHECK( Multiplier( 4000, 5.0, 800 ) == 1.0 );
		// 3995 / 4000 = 0.99875: just outside, applied.
		CHECK_THAT( Multiplier( 3995, 5.0, 800 ), WithinAbs( 0.99875, 1e-9 ) );
		CHECK( kUnityBand == 0.001 );
	}
	SECTION( "limits" )
	{
		// would be 0.01x and 100x: applied as 0.05x and 20x (D18).
		CHECK_THAT( Multiplier( 100, 12.5, 800 ), WithinAbs( kMinMultiplier, 1e-12 ) );
		CHECK_THAT( Multiplier( 1000000, 12.5, 800 ), WithinAbs( kMaxMultiplier, 1e-12 ) );
		CHECK_THAT( Multiplier( 500, 12.5, 800 ), WithinAbs( 0.05, 1e-12 ) ); // exactly the floor
		CHECK_THAT( Multiplier( 200000, 12.5, 800 ), WithinAbs( 20.0, 1e-12 ) );
	}
}

TEST_CASE( "truesens: calculator", "[truesens]" )
{
	// typed 0.8, game needs 5000 counts, the player's target is 10 in * 800 = 8000.
	const auto v = IngameSensForTarget( 0.8, 5000, 10.0, 800 );
	REQUIRE( v.has_value() );
	CHECK_THAT( *v, WithinAbs( 0.5, 1e-9 ) );
	// not clamped: a number to type elsewhere.
	const auto big = IngameSensForTarget( 1.0, 1000000, 1.0, 800 );
	REQUIRE( big.has_value() );
	CHECK( *big > 100.0 );

	CHECK_FALSE( IngameSensForTarget( 0, 5000, 10.0, 800 ).has_value() );
	CHECK_FALSE( IngameSensForTarget( -1, 5000, 10.0, 800 ).has_value() );
	CHECK_FALSE( IngameSensForTarget( 0.8, 0, 10.0, 800 ).has_value() );
	CHECK_FALSE( IngameSensForTarget( 0.8, 5000, 0, 800 ).has_value() );
	CHECK_FALSE( IngameSensForTarget( 0.8, 5000, 10.0, 0 ).has_value() );
	CHECK_FALSE( IngameSensForTarget( kNaN, 5000, 10.0, 800 ).has_value() );
	CHECK_FALSE( IngameSensForTarget( 0.8, kInf, 10.0, 800 ).has_value() );
}

TEST_CASE( "truesens: recorder net movement", "[truesens]" )
{
	Recorder r;
	CHECK_FALSE( r.Active() );
	r.OnDelta( 100, 100 ); // idle: ignored
	CHECK( r.Samples() == 0 );

	r.Start( Target::Game );
	CHECK( r.Active() );
	CHECK( r.CurrentTarget() == Target::Game );
	// +6000, -1500, +500 in X with some Y: net 5000.
	for ( int i = 0; i < 60; i++ ) r.OnDelta( 100, 3 );
	for ( int i = 0; i < 15; i++ ) r.OnDelta( -100, -1 );
	for ( int i = 0; i < 5; i++ ) r.OnDelta( 100, 7 );
	CHECK( r.Samples() == 80 );
	const RecordResult res = r.Stop();
	CHECK( res.ok );
	CHECK( res.target == Target::Game );
	CHECK_THAT( res.counts, WithinAbs( 5000.0, 1e-9 ) );
	CHECK_THAT( res.sum_y, WithinAbs( 60 * 3 - 15 + 35, 1e-9 ) ); // diagnostics only
	CHECK( res.samples == 80 );
	CHECK_FALSE( r.Active() );
}

TEST_CASE( "truesens: recorder takes the absolute value at the end", "[truesens]" )
{
	Recorder r;
	r.Start( Target::Low );
	for ( int i = 0; i < 50; i++ ) r.OnDelta( -100, 0 ); // a turn to the left
	const RecordResult res = r.Stop();
	CHECK( res.ok );
	CHECK_THAT( res.counts, WithinAbs( 5000.0, 1e-9 ) );
}

TEST_CASE( "truesens: Y never decides the result", "[truesens]" )
{
	Recorder r;
	r.Start( Target::High );
	for ( int i = 0; i < 100; i++ ) r.OnDelta( 0, 1000 ); // all Y, no X
	const RecordResult res = r.Stop();
	CHECK_FALSE( res.ok );
	CHECK( res.reason == kReasonNoMovement );
	CHECK( res.sum_y > 0 );
}

TEST_CASE( "truesens: recorder refuses too little movement with a reason", "[truesens]" )
{
	Recorder r;

	r.Start( Target::Low ); // nothing at all
	RecordResult res = r.Stop();
	CHECK_FALSE( res.ok );
	CHECK( res.reason == kReasonNoMovement );
	CHECK( res.reason.find( "No mouse movement was seen" ) == 0 );
	CHECK( res.samples == 0 );

	r.Start( Target::Low ); // events but they cancel out exactly
	r.OnDelta( 500, 0 );
	r.OnDelta( -500, 0 );
	res = r.Stop();
	CHECK_FALSE( res.ok );
	CHECK( res.reason == kReasonNoMovement );
	CHECK( res.samples == 2 );

	r.Start( Target::Low ); // a nudge below the floor
	r.OnDelta( kMinRecordingCounts - 1, 0 );
	res = r.Stop();
	CHECK_FALSE( res.ok );
	CHECK( res.reason == kReasonTooLittle );

	r.Start( Target::Low ); // exactly the floor is enough
	r.OnDelta( kMinRecordingCounts, 0 );
	res = r.Stop();
	CHECK( res.ok );
}

TEST_CASE( "truesens: recorder invalidated by Zoom", "[truesens]" )
{
	Recorder r;
	r.Start( Target::Game );
	for ( int i = 0; i < 60; i++ ) r.OnDelta( 100, 0 );
	r.Invalidate( kReasonZoom );
	r.Invalidate( "a later reason does not replace the first" );
	for ( int i = 0; i < 5; i++ ) r.OnDelta( 100, 0 );
	const RecordResult res = r.Stop();
	CHECK_FALSE( res.ok );
	CHECK( res.reason == kReasonZoom );
	CHECK( res.counts > 0 ); // the numbers are still reported

	Recorder idle;
	idle.Invalidate( "x" ); // idle: nothing to invalidate
	idle.Start( Target::Game );
	idle.OnDelta( 1000, 0 );
	CHECK( idle.Stop().ok );
}

TEST_CASE( "truesens: recorder restarts cleanly after stop", "[truesens]" )
{
	Recorder r;
	r.Start( Target::Low );
	r.OnDelta( 1000, 50 );
	r.Invalidate( kReasonZoom );
	CHECK_FALSE( r.Stop().ok );

	r.Start( Target::High );
	CHECK( r.SumX() == 0.0 );
	CHECK( r.SumY() == 0.0 );
	CHECK( r.Samples() == 0 );
	r.OnDelta( 2000, 0 );
	const RecordResult res = r.Stop();
	CHECK( res.ok ); // the old invalid reason is gone
	CHECK( res.target == Target::High );
	CHECK_THAT( res.counts, WithinAbs( 2000.0, 1e-9 ) );

	// stopping while idle is a not-ok result with a reason, never a crash.
	const RecordResult again = r.Stop();
	CHECK_FALSE( again.ok );
	CHECK_FALSE( again.reason.empty() );
}

TEST_CASE( "truesens: recorder ignores non-finite deltas", "[truesens]" )
{
	Recorder r;
	r.Start( Target::Game );
	r.OnDelta( kNaN, kInf );
	r.OnDelta( 1000, 0 );
	const RecordResult res = r.Stop();
	CHECK( res.ok );
	CHECK_THAT( res.counts, WithinAbs( 1000.0, 1e-9 ) );
}

namespace
{
	// A small deterministic generator, so a failure repeats.
	struct Lcg
	{
		uint64_t s = 0x9E3779B97F4A7C15ull;
		uint32_t Next()
		{
			s = s * 6364136223846793005ull + 1442695040888963407ull;
			return (uint32_t)( s >> 33 );
		}
		// uniform in [-1, 1]
		double Unit() { return (double)Next() / (double)0x7FFFFFFFu * 2.0 - 1.0; }
	};

	bool IsWhole( double d )
	{
		return d == std::trunc( d );
	}

	// Running sums of one test sequence through a carry at a fixed factor.
	struct Sums
	{
		double inX = 0, inY = 0, outX = 0, outY = 0;
		bool bWhole = true;
		double worst = 0.0; // largest |sum(out) - m * sum(in)| seen at any step, either axis
	};

	void Feed( WholeCountCarry &c, Sums &t, double dx, double dy, double mult )
	{
		t.inX += dx;
		t.inY += dy;
		double x = dx * mult, y = dy * mult;
		c.Apply( x, y );
		t.outX += x;
		t.outY += y;
		if ( !IsWhole( x ) || !IsWhole( y ) )
			t.bWhole = false;
		t.worst = std::max( t.worst, std::max( std::fabs( t.outX - t.inX * mult ), std::fabs( t.outY - t.inY * mult ) ) );
	}
}

TEST_CASE( "truesens: whole-count carry invariant for long sequences", "[truesens]" )
{
	for ( double mult : { 0.05, 0.3, 0.37, 0.5, 1.25, 2.5, 20.0 } )
	{
		DYNAMIC_SECTION( "multiplier " << mult )
		{
			Lcg rng;
			WholeCountCarry c;
			Sums t;
			// slow 1-count events
			for ( int i = 0; i < 5000; i++ )
				Feed( c, t, 1.0, 0.0, mult );
			// bursts of random size, both signs
			for ( int i = 0; i < 5000; i++ )
				Feed( c, t, std::floor( rng.Unit() * 200.0 ), std::floor( rng.Unit() * 200.0 ), mult );
			// back and forth, small and equal
			for ( int i = 0; i < 5000; i++ )
				Feed( c, t, i % 2 ? -1.0 : 1.0, i % 2 ? 1.0 : -1.0, mult );
			for ( int i = 0; i < 5000; i++ )
				Feed( c, t, ( i / 2 ) % 2 ? -2.0 : 2.0, 0.0, mult );
			// diagonal
			for ( int i = 0; i < 5000; i++ )
				Feed( c, t, 1.0, 1.0, mult );
			// fractional input, as the nested backend's unaccelerated motion delivers
			for ( int i = 0; i < 20000; i++ )
				Feed( c, t, rng.Unit() * 3.0, rng.Unit() * 0.7, mult );
			// long mixed random walk
			for ( int i = 0; i < 20000; i++ )
				Feed( c, t, std::floor( rng.Unit() * 4.0 ), std::floor( rng.Unit() * 4.0 ), mult );
			CHECK( t.bWhole );
			CHECK( t.worst < 1.0 );
			// what is not delivered is exactly the carry
			CHECK_THAT( t.outX + c.CarryX(), WithinAbs( t.inX * mult, 1e-6 ) );
			CHECK_THAT( t.outY + c.CarryY(), WithinAbs( t.inY * mult, 1e-6 ) );
			CHECK( std::fabs( c.CarryX() ) < 1.0 );
			CHECK( std::fabs( c.CarryY() ) < 1.0 );
		}
	}
}

TEST_CASE( "truesens: whole-count carry lets slow movement through", "[truesens]" )
{
	// The owner's report: at 0.3x a hand moving one count at a time moved nothing.
	struct Case { double mult; int events; double expect; };
	for ( const Case &k : { Case{ 0.3, 100, 30 }, Case{ 0.05, 100, 5 }, Case{ 0.37, 100, 37 },
							Case{ 0.5, 100, 50 }, Case{ 2.5, 100, 250 }, Case{ 20.0, 100, 2000 },
							Case{ 0.3, 1000, 300 } } )
	{
		WholeCountCarry c;
		Sums t;
		int nMoving = 0;
		for ( int i = 0; i < k.events; i++ )
		{
			const double before = t.outX;
			Feed( c, t, 1.0, 1.0, k.mult );
			if ( t.outX != before )
				nMoving++;
		}
		INFO( "multiplier " << k.mult );
		CHECK( t.bWhole );
		CHECK( t.outX == k.expect );
		CHECK( t.outY == k.expect );
		if ( k.mult < 1.0 )
			CHECK( nMoving == (int)k.expect ); // one count per emission, never two
	}
}

TEST_CASE( "truesens: whole-count carry above 1 alternates whole counts", "[truesens]" )
{
	WholeCountCarry c;
	std::string seen;
	for ( int i = 0; i < 6; i++ )
	{
		double x = 1.0 * 2.5, y = 0.0;
		c.Apply( x, y );
		seen += std::to_string( (int)x ) + ",";
	}
	CHECK( seen == "2,3,2,3,2,3," );
}

TEST_CASE( "truesens: whole-count carry truncates, rounding would jitter", "[truesens]" )
{
	// A wobble of +2 -2 at 0.3x nets zero. Truncation emits nothing; rounding to
	// nearest emits +1 -1 every cycle (the aim jitters from movement that
	// cancels). This is why the carry truncates.
	WholeCountCarry c;
	double accRound = 0.0;
	int nEmitTrunc = 0, nEmitRound = 0;
	for ( int cycle = 0; cycle < 100; cycle++ )
		for ( double d : { 2.0, -2.0 } )
		{
			double x = d * 0.3, y = 0.0;
			c.Apply( x, y );
			if ( x != 0.0 )
				nEmitTrunc++;
			accRound += d * 0.3;
			const double out = std::floor( accRound + 0.5 );
			accRound -= out;
			if ( out != 0.0 )
				nEmitRound++;
		}
	CHECK( nEmitTrunc == 0 );
	CHECK( nEmitRound == 200 );

	// A direction change does not emit a count early either: 0.3x of +3 (0.9
	// accrued) then -1 leaves nothing, where rounding would already have sent +1.
	WholeCountCarry d;
	double x = 3.0 * 0.3, y = 0.0;
	d.Apply( x, y );
	CHECK( x == 0.0 );
	x = -1.0 * 0.3;
	d.Apply( x, y );
	CHECK( x == 0.0 );
}

TEST_CASE( "truesens: whole-count carry survives a changing factor", "[truesens]" )
{
	// Zoom's fade changes its factor every frame (1.0 -> 0.25). The carry is in
	// output counts, so nothing needs resetting and nothing is thrown away.
	WholeCountCarry c;
	double out = 0.0, expect = 0.0;
	for ( int i = 0; i < 30; i++ )
	{
		const double f = 1.0 - 0.75 * (double)i / 29.0;
		double x = 1.0 * f, y = 0.0;
		c.Apply( x, y );
		expect += 1.0 * f;
		out += x;
		CHECK( IsWhole( x ) );
		CHECK( std::fabs( out - expect ) < 1.0 );
	}
	CHECK_THAT( out + c.CarryX(), WithinAbs( expect, 1e-9 ) );

	// then settled at 0.25: the remainder of the ramp is not lost.
	for ( int i = 0; i < 400; i++ )
	{
		double x = 1.0 * 0.25, y = 0.0;
		c.Apply( x, y );
		expect += 0.25;
		out += x;
	}
	CHECK( std::fabs( out - expect ) < 1.0 );
}

TEST_CASE( "truesens: whole-count carry resets and bad input", "[truesens]" )
{
	WholeCountCarry c;
	double dx = 0.4, dy = -0.4;
	c.Apply( dx, dy );
	CHECK( dx == 0.0 );
	CHECK( dy == 0.0 );
	CHECK_THAT( c.CarryX(), WithinAbs( 0.4, 1e-12 ) );
	CHECK_THAT( c.CarryY(), WithinAbs( -0.4, 1e-12 ) );

	c.Reset();
	CHECK( c.CarryX() == 0.0 );
	CHECK( c.CarryY() == 0.0 );

	// after a reset the old remainder is gone: 0.4 + 0.4 + 0.4 accrues 1.2 and
	// sends one count only on the third call.
	dx = 0.4; dy = 0.0;
	c.Apply( dx, dy );
	dx = 0.4; dy = 0.0;
	c.Apply( dx, dy );
	CHECK( dx == 0.0 );
	dx = 0.4; dy = 0.0;
	c.Apply( dx, dy );
	CHECK( dx == 1.0 );

	// non-finite input yields nothing and does not touch the carry of that axis.
	c.Reset();
	dx = 0.5; dy = 0.5;
	c.Apply( dx, dy );
	dx = kNaN; dy = 1.0;
	c.Apply( dx, dy );
	CHECK( dx == 0.0 );
	CHECK( dy == 1.0 ); // 1.0 + 0.5 carried
	CHECK_THAT( c.CarryX(), WithinAbs( 0.5, 1e-12 ) );
	CHECK_THAT( c.CarryY(), WithinAbs( 0.5, 1e-12 ) );
	dx = kInf; dy = -kInf;
	c.Apply( dx, dy );
	CHECK( dx == 0.0 );
	CHECK( dy == 0.0 );
	CHECK_THAT( c.CarryX(), WithinAbs( 0.5, 1e-12 ) );
}

TEST_CASE( "truesens: text helpers", "[truesens]" )
{
	CHECK( FormatCmPer360( 34.2 / kCmPerInch ) == "34.2 cm per 360\xC2\xB0" );
	CHECK( FormatCmPer360( 0.0 ) == "not recorded" );
	CHECK( FormatCmPer360( kNaN ) == "not recorded" );
	CHECK( FormatMultiplier( 0.82 ) == "0.82x" );
	CHECK( FormatMultiplier( 20.0 ) == "20.00x" );
	CHECK( FormatMultiplier( 0.0 ) == "not recorded" );
	CHECK( FormatCounts360( 5000 ) == "5000 counts per 360\xC2\xB0" );
	CHECK( FormatCounts360( 0 ) == "not recorded" );
	CHECK( std::string( TargetName( Target::Low ) ) == "low" );
	CHECK( std::string( TargetName( Target::Idle ) ) == "none" );
}

TEST_CASE( "truesens: recording hint lines", "[truesens]" )
{
	const std::string kArmed = "Aim at a spot you can find again, then left click";
	const std::string kRunning = "Turn once around (360\xC2\xB0) to the same spot, then left click again";

	HintText h = HintLines( Target::Low, Phase::Armed );
	CHECK( h.title == "Recording Low sensitivity" );
	CHECK( h.action == kArmed );
	h = HintLines( Target::Low, Phase::Recording );
	CHECK( h.title == "Recording Low sensitivity" );
	CHECK( h.action == kRunning );
	// the one frame between the second click and the tick keeps the recording text
	CHECK( HintLines( Target::Low, Phase::Finished ).action == kRunning );

	CHECK( HintLines( Target::High, Phase::Armed ).title == "Recording High sensitivity" );
	CHECK( HintLines( Target::Game, Phase::Recording ).title == "Recording game sensitivity" );

	// no recording: nothing to show, whatever the other argument says
	h = HintLines( Target::Idle, Phase::Armed );
	CHECK( h.title.empty() );
	CHECK( h.action.empty() );
	h = HintLines( Target::Low, Phase::Idle );
	CHECK( h.title.empty() );
	CHECK( h.action.empty() );
}

TEST_CASE( "truesens: the Recording row always has a value", "[truesens]" )
{
	CHECK( RecordingStatusText( Target::Idle, Phase::Idle, 0 ) == "None" );
	CHECK( RecordingStatusText( Target::Low, Phase::Idle, 5 ) == "None" );
	CHECK( RecordingStatusText( Target::Low, Phase::Armed, 0 ) == "Low, waiting for the first click" );
	CHECK( RecordingStatusText( Target::High, Phase::Armed, 99 ) == "High, waiting for the first click" );
	CHECK( RecordingStatusText( Target::Low, Phase::Recording, 3120.4 ) == "Low, 3120 counts so far" );
	CHECK( RecordingStatusText( Target::Game, Phase::Recording, 0 ) == "Game, 0 counts so far" );
}

TEST_CASE( "truesens: two left clicks advance a recording", "[truesens]" )
{
	// StepOnButton( phase, bLeft, bPressed, bCursorHidden, bSynthetic )
	ClickStep s = StepOnButton( Phase::Armed, true, true, true, false );
	CHECK( s.next == Phase::Recording );
	CHECK( s.swallow );
	CHECK( s.started );
	CHECK_FALSE( s.finished );

	s = StepOnButton( Phase::Recording, true, true, true, false );
	CHECK( s.next == Phase::Finished );
	CHECK( s.swallow );
	CHECK_FALSE( s.started );
	CHECK( s.finished );
}

TEST_CASE( "truesens: a click that does not count changes nothing and is not swallowed", "[truesens]" )
{
	for ( Phase p : { Phase::Idle, Phase::Armed, Phase::Recording, Phase::Finished } )
	{
		const bool bActive = p == Phase::Armed || p == Phase::Recording;
		// cursor visible: a click in a menu
		ClickStep s = StepOnButton( p, true, true, false, false );
		CHECK( s.next == p );
		CHECK_FALSE( s.swallow );
		// right / middle button
		s = StepOnButton( p, false, true, true, false );
		CHECK( s.next == p );
		CHECK_FALSE( s.swallow );
		// a release is never the click
		s = StepOnButton( p, true, false, true, false );
		CHECK( s.next == p );
		CHECK_FALSE( s.swallow );
		// the Autoclicker's own press
		s = StepOnButton( p, true, true, true, true );
		CHECK( s.next == p );
		CHECK_FALSE( s.swallow );
		// and the counting click only does something while armed or running
		s = StepOnButton( p, true, true, true, false );
		CHECK( s.swallow == bActive );
	}
	// a press while idle or while the result is pending: no effect
	CHECK( StepOnButton( Phase::Idle, true, true, true, false ).next == Phase::Idle );
	CHECK( StepOnButton( Phase::Finished, true, true, true, false ).next == Phase::Finished );
}

TEST_CASE( "truesens: a full click sequence and a third click", "[truesens]" )
{
	Phase p = Phase::Armed;
	int nSwallowed = 0;
	for ( int i = 0; i < 3; i++ )
	{
		const ClickStep s = StepOnButton( p, true, true, true, false );
		nSwallowed += s.swallow ? 1 : 0;
		p = s.next;
	}
	// armed -> recording -> finished; the third click finds the result pending and goes to the game
	CHECK( p == Phase::Finished );
	CHECK( nSwallowed == 2 );
}

TEST_CASE( "truesens: reference picture crop", "[truesens]" )
{
	// 1080p: the centre 25 % x 25 %, centred exactly
	CropRect c = ReferenceCrop( 1920, 1080 );
	REQUIRE( c.ok );
	CHECK( c.w == 480 );
	CHECK( c.h == 270 );
	CHECK( c.x == 720 );
	CHECK( c.y == 405 );

	// odd sizes: integer division, centred to within one pixel, never outside the picture
	c = ReferenceCrop( 1001, 777 );
	REQUIRE( c.ok );
	CHECK( c.w == 250 );
	CHECK( c.h == 194 );
	CHECK( c.x == 375 );
	CHECK( c.y == 291 );
	CHECK( c.x + c.w <= 1001 );
	CHECK( c.y + c.h <= 777 );
	CHECK( std::abs( ( c.x + c.w / 2 ) - 1001 / 2 ) <= 1 );

	// tiny pictures: the smallest that works, then nothing
	c = ReferenceCrop( 4, 4 );
	REQUIRE( c.ok );
	CHECK( c.w == 1 );
	CHECK( c.h == 1 );
	CHECK( c.x == 1 );
	CHECK( c.y == 1 );
	CHECK( !ReferenceCrop( 3, 100 ).ok );
	CHECK( !ReferenceCrop( 100, 3 ).ok );
	CHECK( !ReferenceCrop( 0, 0 ).ok );
	CHECK( !ReferenceCrop( -5, 1080 ).ok );

	// every size from 4 up stays inside the picture
	for ( int w = 4; w < 300; w += 7 )
		for ( int h = 4; h < 300; h += 11 )
		{
			const CropRect r = ReferenceCrop( w, h );
			REQUIRE( r.ok );
			REQUIRE( r.w >= 1 );
			REQUIRE( r.h >= 1 );
			REQUIRE( r.x >= 0 );
			REQUIRE( r.y >= 0 );
			REQUIRE( r.x + r.w <= w );
			REQUIRE( r.y + r.h <= h );
		}

	// the texture reduction
	CHECK( ReferenceDownscale( 480, 270, 768 ) == 1 );
	CHECK( ReferenceDownscale( 768, 10, 768 ) == 1 );
	CHECK( ReferenceDownscale( 960, 540, 768 ) == 2 );
	CHECK( ReferenceDownscale( 10, 1537, 768 ) == 3 );
	CHECK( ReferenceDownscale( 100, 100, 0 ) == 1 );
}

TEST_CASE( "truesens: reference picture display size", "[truesens]" )
{
	// 1080p, plate bottom at 90 px (scale 1): 25 % of the output, centred, 8 px below the plate
	DisplayRect d = ReferenceDisplay( 1920.0f, 1080.0f, 90.0f, 1.0f, 480.0f / 270.0f );
	REQUIRE( d.ok );
	CHECK_THAT( d.w, Catch::Matchers::WithinAbs( 480.0, 0.01 ) );
	CHECK_THAT( d.h, Catch::Matchers::WithinAbs( 270.0, 0.01 ) );
	CHECK_THAT( d.x, Catch::Matchers::WithinAbs( 720.0, 0.01 ) );
	CHECK_THAT( d.y, Catch::Matchers::WithinAbs( 98.0, 0.01 ) );
	CHECK( d.y + d.h < 1080.0f * 0.5f - 1080.0f * 0.08f ); // clear of the centre

	// 4K: the scale (and so the plate and the gap) is twice as large, same relative place
	d = ReferenceDisplay( 3840.0f, 2160.0f, 180.0f, 2.0f, 16.0f / 9.0f );
	REQUIRE( d.ok );
	CHECK_THAT( d.w, Catch::Matchers::WithinAbs( 960.0, 0.01 ) );
	CHECK_THAT( d.y, Catch::Matchers::WithinAbs( 196.0, 0.01 ) );

	// a low plate (a readout above it pushed it down): the picture shrinks, keeping the aspect,
	// and its bottom stops 8 % of the height short of the centre
	d = ReferenceDisplay( 1920.0f, 1080.0f, 200.0f, 1.0f, 16.0f / 9.0f );
	REQUIRE( d.ok );
	CHECK_THAT( d.y + d.h, Catch::Matchers::WithinAbs( 1080.0 * 0.42, 0.01 ) );
	CHECK_THAT( d.w / d.h, Catch::Matchers::WithinAbs( 16.0 / 9.0, 0.001 ) );
	CHECK( d.w < 480.0f );
	CHECK_THAT( d.x, Catch::Matchers::WithinAbs( ( 1920.0 - d.w ) * 0.5, 0.01 ) );

	// no room at all: nothing is drawn
	CHECK( !ReferenceDisplay( 1920.0f, 1080.0f, 440.0f, 1.0f, 16.0f / 9.0f ).ok );
	CHECK( !ReferenceDisplay( 1920.0f, 1080.0f, 90.0f, 1.0f, 0.0f ).ok );
	CHECK( !ReferenceDisplay( 0.0f, 0.0f, 0.0f, 1.0f, 1.0f ).ok );

	// a wide picture is shorter at the same width, so it fits where a square one would shrink
	d = ReferenceDisplay( 2560.0f, 1080.0f, 90.0f, 1.0f, 640.0f / 270.0f );
	REQUIRE( d.ok );
	CHECK_THAT( d.w, Catch::Matchers::WithinAbs( 640.0, 0.01 ) );
	CHECK_THAT( d.h, Catch::Matchers::WithinAbs( 270.0, 0.01 ) );
}
