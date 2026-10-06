// The lag spike buffer's "off" is 0 ms, through the library's pacer (pacing.h, no
// GPU, no clock: every time is passed in). Host-side guard for the Lag spike buffer
// area's Status line ("Off - buffer 0 ms") and for removing Force minimum: with the
// switch off the buffer the pacer reports is 0 once the ramp is done, and nothing
// else about the buffer (Max buffer, a Force maximum test mode) has any effect on it.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "pacing.h"

namespace fgpacing = framegen::pacing;

namespace
{
	constexpr fgpacing::Ns kMs = fgpacing::kNsPerMs;

	struct Run
	{
		fgpacing::Pacer pacer;
		fgpacing::LagBufferSettings lag;
		fgpacing::Ns now = 0;
		uint64_t id = 0;
		const double flIntervalMs = 1000.0 / 60.0;
		const double flRefreshHz = 240.0;
		double flNextArrival = 3.0;   // ms
		double flNextVblank = 0.0;    // ms
		bool bWantPaint = false;
		bool bForced = false;

		fgpacing::Pacer::Inputs Inputs( fgpacing::Ns t ) const
		{
			fgpacing::Pacer::Inputs in;
			in.now = t;
			in.presentNs = t;
			in.mode = fgpacing::Mode::Fixed;
			in.multiplier = 2;
			in.refreshHz = flRefreshHz;
			in.frameGen = false;   // real-rate planning: the buffer alone, as with Frame generation off
			in.lagBuffer = lag;
			return in;
		}

		// Real frames at 60 fps, one two-frame gap at `flSpikeAtMs` (-1: none), a present
		// tick every 1/240 s, for `flDurMs`. Returns the last report.
		fgpacing::Pacer::Report Go( double flDurMs, double flSpikeAtMs = -1.0 )
		{
			const double flEnd = double( now ) / 1e6 + flDurMs;
			for ( ;; )
			{
				const bool bSpike = flSpikeAtMs >= 0.0 && flNextArrival >= flSpikeAtMs && flNextArrival < flSpikeAtMs + 2.0 * flIntervalMs;
				if ( flNextArrival <= flNextVblank && flNextArrival < flEnd )
				{
					if ( !bSpike )
					{
						id++;
						pacer.OnArrival( id, fgpacing::Ns( flNextArrival * 1e6 ), 1, fgpacing::FrameFormat{ 1920, 1080, 87 } );
						bWantPaint = true;
					}
					flNextArrival += flIntervalMs;
				}
				else if ( flNextVblank < flEnd )
				{
					const fgpacing::Ns t = fgpacing::Ns( flNextVblank * 1e6 );
					now = t;
					if ( bWantPaint || bForced )
					{
						bWantPaint = false;
						const fgpacing::Pacer::Decision d = pacer.OnPaint( Inputs( t ), id, true, false );
						bForced = d.repaintNext;
					}
					flNextVblank += 1000.0 / flRefreshHz;
				}
				else
					break;
			}
			now = fgpacing::Ns( flEnd * 1e6 );
			return pacer.TakeStatus( Inputs( now ), now );
		}
	};
}

TEST_CASE( "framegen pacing: a lag buffer switched off ramps to 0 ms and Max buffer has no effect", "[framegen_pacing]" )
{
	// On first: a spike builds a buffer.
	Run r;
	r.lag.enabled = true;
	r.lag.maxBufferMs = 100.0;
	r.Go( 10000.0, 4000.0 );
	REQUIRE( r.pacer.BufferMs() > 10.0 );

	// Switched off (the Max buffer and a Force maximum test mode left set): the buffer
	// ramps down and the report says 0, target included.
	r.lag.enabled = false;
	r.lag.maxBufferMs = 250.0;
	r.lag.testMode = fgpacing::LagTestMode::ForceMax;
	const fgpacing::Pacer::Report rep = r.Go( 10000.0 );
	CHECK( r.pacer.BufferMs() == 0.0 );
	CHECK( rep.bufferDelayMs == 0.0f );
	CHECK( rep.bufferTargetMs == 0.0f );
}

TEST_CASE( "framegen pacing: with the lag buffer off, Max buffer and test mode leave the delay untouched", "[framegen_pacing]" )
{
	double adDelay[ 3 ] = {};
	for ( int i = 0; i < 3; i++ )
	{
		Run r;
		r.lag.enabled = false;
		r.lag.maxBufferMs = i == 0 ? 0.0 : 250.0;
		r.lag.testMode = i == 2 ? fgpacing::LagTestMode::ForceMax : fgpacing::LagTestMode::Off;
		const fgpacing::Pacer::Report rep = r.Go( 6000.0, 3000.0 );
		CHECK( r.pacer.BufferMs() == 0.0 );
		CHECK( rep.bufferDelayMs == 0.0f );
		adDelay[ i ] = r.pacer.DelayMs();
	}
	CHECK( adDelay[ 0 ] == adDelay[ 1 ] );
	CHECK( adDelay[ 0 ] == adDelay[ 2 ] );
}

// ---- the vblank timer's measured wake-up lead (Display > General "Low-latency wake-up") ----
// src/vblankmanager.hpp's pure CVBlankTimer::MeasuredLead(): the largest recent draw
// time + 0.75 ms, the draw floored at 0.75 ms (a direct-scanout frame measures tiny
// draws), capped at one refresh, and 3 ms assumed before anything is measured.
#include "vblankmanager.hpp"

TEST_CASE( "vblank measured lead: max draw + 0.75 ms, floored, clamped to a refresh", "[framegen_pacing]" )
{
	using gamescope::CVBlankTimer;
	constexpr uint64_t kMsNs = 1'000'000ull;
	const uint64_t k240 = 4'166'667ull;
	const uint64_t k280 = 3'571'429ull;

	// The headline case: a 1.1 ms worst draw wakes 1.85 ms ahead.
	REQUIRE( CVBlankTimer::MeasuredLead( 1'100'000ull, k240 ) == 1'850'000ull );
	// Nothing measured yet: the starting 3 ms draw, still capped.
	REQUIRE( CVBlankTimer::MeasuredLead( 0, 8 * kMsNs ) == 3'750'000ull );
	REQUIRE( CVBlankTimer::MeasuredLead( 0, k280 ) == k280 );
	// Clamped to one refresh.
	REQUIRE( CVBlankTimer::MeasuredLead( 5 * kMsNs, k240 ) == k240 );
	REQUIRE( CVBlankTimer::MeasuredLead( 3 * kMsNs, k280 ) == k280 );
	// Floor: a tiny (direct-scanout) draw never takes the lead under 1.5 ms.
	REQUIRE( CVBlankTimer::MeasuredLead( 50'000ull, k240 ) == 1'500'000ull );
	REQUIRE( CVBlankTimer::MeasuredLead( 750'000ull, k240 ) == 1'500'000ull );
	// ...but the floor itself never exceeds a refresh.
	REQUIRE( CVBlankTimer::MeasuredLead( 50'000ull, 1'000'000ull ) == 1'000'000ull );
	// Under VRR: the same lead plus the flush allowance, still capped at one refresh.
	REQUIRE( CVBlankTimer::MeasuredLeadFor( false, 1'100'000ull, k240 ) == 1'850'000ull );
	REQUIRE( CVBlankTimer::MeasuredLeadFor( true, 1'100'000ull, k240 ) == 1'850'000ull + CVBlankTimer::kVRRFlushingTime );
	REQUIRE( CVBlankTimer::MeasuredLeadFor( true, 2 * kMsNs, k280 ) == 2 * kMsNs + 750'000ull + 300'000ull );
	REQUIRE( CVBlankTimer::MeasuredLeadFor( true, 11 * kMsNs, k280 ) == k280 );
}
