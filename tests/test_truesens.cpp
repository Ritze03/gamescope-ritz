// Unit tests for src/Overlay/TrueSens.h's pure half: the multiplier maths, the
// calculator, the recorder and the fraction carry. No compositor, no config, no
// thread. The build is -ffast-math, so the invalid-input cases are the ones that
// prove the range-based validation survives it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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

TEST_CASE( "truesens: fraction carry keeps every fraction", "[truesens]" )
{
	for ( double mult : { 0.05, 0.37, 1.0, 2.5, 20.0 } )
	{
		DYNAMIC_SECTION( "multiplier " << mult )
		{
			FractionCarry c;
			double inX = 0, inY = 0, outX = 0, outY = 0;
			bool bOffGrid = false;
			// tiny and irregular deltas, both signs.
			for ( int i = 0; i < 20000; i++ )
			{
				double dx = ( ( i * 7 ) % 5 ) * 0.1 - 0.15; // -0.15 .. 0.25
				double dy = ( ( i * 3 ) % 4 ) * 0.3 - 0.2;
				inX += dx;
				inY += dy;
				c.Apply( dx, dy, mult );
				outX += dx;
				outY += dy;
				// every output is on the 1/256 grid
				if ( std::fmod( std::fabs( dx ) * 256.0, 1.0 ) != 0.0 ) bOffGrid = true;
			}
			CHECK_FALSE( bOffGrid );
			// the sum property: within one quantum per axis.
			CHECK( std::fabs( outX - inX * mult ) <= kWireQuantum );
			CHECK( std::fabs( outY - inY * mult ) <= kWireQuantum );
		}
	}
}

TEST_CASE( "truesens: fraction carry does not lose small moves", "[truesens]" )
{
	// 0.05x of a one-count move is 0.05: a plain rounding would give 0 forever.
	FractionCarry c;
	double sum = 0;
	for ( int i = 0; i < 1000; i++ )
	{
		double dx = 1.0, dy = 0.0;
		c.Apply( dx, dy, 0.05 );
		sum += dx;
	}
	CHECK_THAT( sum, WithinAbs( 50.0, kWireQuantum ) );
}

TEST_CASE( "truesens: fraction carry resets", "[truesens]" )
{
	FractionCarry c;
	double dx = 0.001, dy = 0.001; // below one quantum: all carried
	c.Apply( dx, dy, 1.0 );
	CHECK( dx == 0.0 );
	CHECK( c.CarryX() > 0.0 );

	c.Reset();
	CHECK( c.CarryX() == 0.0 );
	CHECK( c.CarryY() == 0.0 );

	// a changed multiplier drops the remainder too.
	dx = 0.001; dy = 0.001;
	c.Apply( dx, dy, 1.0 );
	CHECK( c.CarryX() > 0.0 );
	dx = 0.0; dy = 0.0;
	c.Apply( dx, dy, 2.0 );
	CHECK( dx == 0.0 );
	CHECK( c.CarryX() == 0.0 );

	// a non-finite input yields nothing and carries nothing.
	dx = kNaN; dy = 1.0;
	c.Apply( dx, dy, 1.0 );
	CHECK( dx == 0.0 );
	CHECK( dy == 1.0 );
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
	const std::string kReserved = "Ctrl+Alt+Shift+O";
	const std::string kAction = "Turn once around (360\xC2\xB0) to the same spot, then press ";

	HintText h = HintLines( Target::Low, "LCtrl+RShift", kReserved );
	CHECK( h.title == "Recording Low sensitivity" );
	CHECK( h.action == kAction + "LCtrl+RShift and Stop recording" );

	CHECK( HintLines( Target::High, "F9", kReserved ).title == "Recording High sensitivity" );
	CHECK( HintLines( Target::Game, "F9", kReserved ).title == "Recording game sensitivity" );

	// the shell action is unbound: the reserved chord is named instead
	h = HintLines( Target::Game, "", kReserved );
	CHECK( h.action == kAction + kReserved + " and Stop recording" );

	// no recording: nothing to show
	h = HintLines( Target::Idle, "F9", kReserved );
	CHECK( h.title.empty() );
	CHECK( h.action.empty() );
}
