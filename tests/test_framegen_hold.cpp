// The "Hold game while generating" rate controller (FrameGen/HoldController.h), pure:
// a toy GPU (game frames and output paints share one budget) stands in for the real one.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

#include "FrameGen/HoldController.h"

namespace
{
	struct Sim
	{
		fghold::Controller c;
		double flOutputHz = 280.0;
		double flUnheldFps = 220.0;   // what the game does uncapped
		double flGameCostMs = 3.0;    // GPU ms per game frame
		double flFgCostMs = 1.2;      // GPU ms per output paint
		uint64_t now = 1000ull * fghold::kNsPerMs;
		double flNextArrival = 0.0;
		double flMinCap = 1e9, flMaxCap = 0.0;

		bool Saturated( double flGameFps ) const
		{
			return flGameFps * flGameCostMs + flOutputHz * flFgCostMs > 1000.0;
		}

		void Run( double seconds, bool bTrackBand = false )
		{
			const uint64_t ulEnd = now + uint64_t( seconds * 1e9 );
			const uint64_t ulDt = uint64_t( 1e9 / flOutputHz );
			while ( now < ulEnd )
			{
				now += ulDt;
				const double flGame = c.Unlimited() ? flUnheldFps : std::min( flUnheldFps, c.CapFps() );
				while ( double( now ) >= flNextArrival )
				{
					c.OnArrival( uint64_t( flNextArrival > 0 ? flNextArrival : double( now ) ) );
					flNextArrival = std::max( flNextArrival, double( now ) - 1e9 / flGame ) + 1e9 / flGame;
				}
				c.OnPaint( now, Saturated( flGame ) ? 40 : 0, 60, flOutputHz );
				if ( bTrackBand && !c.Unlimited() )
				{
					flMinCap = std::min( flMinCap, c.CapFps() );
					flMaxCap = std::max( flMaxCap, c.CapFps() );
				}
			}
		}
	};
}

TEST_CASE( "hold: no misses stays unlimited", "[framegen_hold]" )
{
	Sim s;
	s.flGameCostMs = 1.0;
	s.Run( 10.0 );
	CHECK( s.c.Unlimited() );
	CHECK( s.c.IntervalNs() == 0 );
}

TEST_CASE( "hold: saturation converges to a rate where misses stop", "[framegen_hold]" )
{
	Sim t;
	t.flGameCostMs = 4.5;   // knee: (1000-336)/4.5 = 147.6 fps
	t.Run( 30.0 );
	REQUIRE( !t.c.Unlimited() );
	CHECK( t.c.CapFps() <= 147.6 * 1.10 );
	CHECK( t.c.CapFps() >= 147.6 * 0.80 );
	CHECK( !t.Saturated( std::min( t.flUnheldFps, t.c.CapFps() ) ) );
}

TEST_CASE( "hold: sawtooth around the knee stays bounded", "[framegen_hold]" )
{
	Sim s;
	s.flGameCostMs = 4.5;
	s.Run( 20.0 );   // settle
	s.Run( 60.0, true );
	REQUIRE( s.flMaxCap > 0.0 );
	CHECK( s.flMaxCap / s.flMinCap < 1.20 );
}

TEST_CASE( "hold: recovers upward when the load drops, then unlimited", "[framegen_hold]" )
{
	Sim s;
	s.flGameCostMs = 4.5;
	s.Run( 20.0 );
	REQUIRE( !s.c.Unlimited() );
	const double flLow = s.c.CapFps();
	s.flGameCostMs = 1.0;   // the scene got light
	s.Run( 3.0 );
	CHECK( ( s.c.Unlimited() || s.c.CapFps() > flLow ) );
	s.Run( 30.0 );
	CHECK( s.c.Unlimited() );
}

TEST_CASE( "hold: never below the floor", "[framegen_hold]" )
{
	Sim s;
	s.flFgCostMs = 4.0;   // FG alone saturates: no game rate fixes that
	s.Run( 60.0, true );
	REQUIRE( !s.c.Unlimited() );
	CHECK( s.flMinCap >= std::max( fghold::kFloorFps, 280.0 / 8.0 ) - 1e-6 );
}

TEST_CASE( "hold: a stray miss or two changes nothing", "[framegen_hold]" )
{
	fghold::Controller c;
	uint64_t now = 1000ull * fghold::kNsPerMs;
	for ( int i = 0; i < 600; i++ )
	{
		now += 4 * fghold::kNsPerMs;
		if ( i % 4 == 0 )
			c.OnArrival( now );
		c.OnPaint( now, 2, 60, 250.0 );
	}
	CHECK( c.Unlimited() );
}
