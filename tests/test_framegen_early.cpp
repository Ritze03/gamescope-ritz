// The early pair's prediction (src/FrameGen/EarlyPredict.h + a COPY of the library's pacer):
// the copy must answer "which pair does the next paint play" exactly as the real pacer then
// does, and asking must not move the real one.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "FrameGen/EarlyPredict.h"
#include "pacing.h"

namespace fgpacing = framegen::pacing;

TEST_CASE( "Early pair: next paint V", "[framegen_early]" )
{
	constexpr uint64_t T = 3571428;
	CHECK( fgearly::NextPaintV( 0, 500, T, 0 ) == 500 );
	CHECK( fgearly::NextPaintV( 1000, 1500, T, 0 ) == 1000 + T );                   // the next grid point
	CHECK( fgearly::NextPaintV( 1000, 1000 + 3 * T + 5, T, 0 ) == 1000 + 4 * T );   // grid points already past are skipped
	CHECK( fgearly::NextPaintV( 1000, 1500, T, 2 * T ) == 1000 + 2 * T );           // extra timer: one interval
	CHECK( fgearly::NextPaintV( 1000, 1000 + 5 * T, T, 2 * T ) == 1000 + 5 * T );   // ... never in the past
}

TEST_CASE( "Early pair: a copy of the pacer predicts the next paint's pair", "[framegen_early]" )
{
	for ( const fgpacing::Priority prio : { fgpacing::Priority::Smoothness, fgpacing::Priority::LowLatency } )
	{
		fgpacing::Pacer pacer;
		const double flRefreshHz = 280.0;
		const double flGameMs = 1000.0 / 230.0;
		const double flTickMs = 1000.0 / flRefreshHz;
		double flNextArrival = 2.0, flNextV = 0.0;
		uint64_t id = 0;
		int nPairs = 0, nChanged = 0;
		uint64_t ulPrevPrev = 0, ulPrevCurr = 0;

		while ( flNextV < 3000.0 )
		{
			if ( flNextArrival <= flNextV )
			{
				pacer.OnArrival( ++id, fgpacing::Ns( flNextArrival * 1e6 ), 1, fgpacing::FrameFormat{ 1920, 1080, 87 } );
				flNextArrival += flGameMs;
				continue;
			}
			const fgpacing::Ns V = fgpacing::Ns( flNextV * 1e6 );
			flNextV += flTickMs;
			fgpacing::Pacer::Inputs in;
			in.now = V;
			in.presentNs = V;
			in.mode = fgpacing::Mode::Target;
			in.targetFps = 0;
			in.priority = prio;
			in.pauseAtRefresh = false;
			in.refreshHz = flRefreshHz;
			in.frameGen = true;

			// The prediction asks a copy; the original is then asked for real and must agree.
			fgpacing::Pacer copy = pacer;
			const fgpacing::Pacer::Decision p = copy.OnPaint( in, id, true, true );
			const fgpacing::Pacer::Decision d = pacer.OnPaint( in, id, true, true );
			REQUIRE( p.prevId == d.prevId );
			REQUIRE( p.currId == d.currId );
			REQUIRE( p.skip == d.skip );
			if ( d.prevId && d.currId != d.prevId && !d.skip )
			{
				nPairs++;
				if ( d.prevId != ulPrevPrev || d.currId != ulPrevCurr )
					nChanged++;
				ulPrevPrev = d.prevId;
				ulPrevCurr = d.currId;
			}
		}
		// 230 fps into 280 Hz: the pair moves on at most paints, which is what the early pair is for.
		CHECK( nPairs > 100 );
		CHECK( nChanged > 10 );
	}
}
