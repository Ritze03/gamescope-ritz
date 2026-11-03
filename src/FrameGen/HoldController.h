// "Hold game while generating": the adaptive game-rate controller.
//
// Pure (no clock, no GPU, no wlserver: every time is passed in), so it is unit-tested
// (tests/test_framegen_hold.cpp). steamcompmgr feeds it each paint and each game-frame
// arrival and turns its answer into time-based frame-gate releases.
//
// Why a controller and not "release when the pacer's pair ends": that release made the
// game's interval (last interval + its render time) a positive feedback loop and
// collapsed a saturated game (221 fps) to the layer's timeout floor (20-25 fps) -- see
// superdoc/features/frame-generation.md. The goal is the opposite: the game as fast as
// the GPU leaves room for, with frame generation still reaching its target. The signal
// that says "the GPU is over-subscribed" is the compositor's own paint missing its
// deadline (draw time past the vblank wake-up lead), so:
//
//  * start UNLIMITED (no throttle; the compositor sends enabled = 0, the layer never waits);
//  * misses in the recent window above kMissThreshold -> multiplicative decrease of the
//    allowed game rate (x kDecreaseFactor), starting from the game's measured rate;
//    at most one decrease per kCooldownNs (the miss window is stale until it refills);
//  * clean (<= kCleanMisses) for kCleanNs -> additive-ish increase (+kIncreasePct, at
//    least kIncreaseMinFps) every kRaiseStepNs;
//  * once the allowed rate exceeds the measured rate by kUnlimitedMargin the cap is not
//    binding any more: back to UNLIMITED;
//  * never below max(kFloorFps, output/8) (frame generation's multiplier range), never
//    above the measured rate (a cap above what the game does is not a cap).
#pragma once

#include <algorithm>
#include <cstdint>

namespace fghold
{
	constexpr uint64_t kNsPerMs = 1000ull * 1000ull;

	// Misses (of the recent paints) that count as "over-subscribed". 3/60 = 5%: one or two
	// stray misses (a UI paint, a shader compile) are noise, saturation measured 35-50/60.
	constexpr unsigned kMissThreshold = 2;
	// At most this many misses still count as clean.
	constexpr unsigned kCleanMisses = 1;
	// Fewer paints than this in the window: no verdict either way.
	constexpr unsigned kMinWindow = 30;
	// Decrease factor: ~7% per step converges within a handful of steps from a 2x overshoot
	// yet is small enough that the frame time stays smooth.
	constexpr double kDecreaseFactor = 0.93;
	// The miss ring (60 paints, ~200 ms at 300 Hz) must refill with post-decrease paints
	// before the next verdict, or one episode would be punished several times.
	constexpr uint64_t kCooldownNs = 300 * kNsPerMs;
	// Clean for this long before probing upward: with the decrease above this bounds the
	// sawtooth around the knee to about one decrease step (+/-7%) with a ~1 s dwell, so it
	// does not oscillate faster than a player can notice.
	constexpr uint64_t kCleanNs = 1000 * kNsPerMs;
	// Once probing, one step per this interval.
	constexpr uint64_t kRaiseStepNs = 250 * kNsPerMs;
	constexpr double kIncreasePct = 0.03;
	constexpr double kIncreaseMinFps = 3.0;
	// Allowed rate this far above the measured rate = the cap does nothing: unlimited.
	constexpr double kUnlimitedMargin = 1.25;
	// Absolute floor (and output/8, whichever is larger).
	constexpr double kFloorFps = 30.0;
	// Measured game rate: EMA over arrival intervals.
	constexpr double kRateEmaAlpha = 0.1;

	class Controller
	{
	public:
		void Reset()
		{
			m_bLimited = false;
			m_flCapFps = 0.0;
			m_ulLastChange = 0;
			m_ulLastMiss = 0;
			m_ulLastRaise = 0;
			m_flGameFps = 0.0;
			m_ulLastArrival = 0;
		}

		// A real game frame reached the compositor.
		void OnArrival( uint64_t ulNow )
		{
			if ( m_ulLastArrival && ulNow > m_ulLastArrival )
			{
				const double flFps = 1e9 / double( ulNow - m_ulLastArrival );
				m_flGameFps = m_flGameFps > 0.0 ? m_flGameFps + kRateEmaAlpha * ( flFps - m_flGameFps ) : flFps;
			}
			m_ulLastArrival = ulNow;
		}

		// One paint: nMissed of nWindow recent paints ran past their deadline.
		void OnPaint( uint64_t ulNow, unsigned nMissed, unsigned nWindow, double flOutputHz )
		{
			// A game that stopped arriving (menu, loading) has no meaningful rate.
			if ( m_ulLastArrival && ulNow - m_ulLastArrival > 500 * kNsPerMs )
				m_flGameFps = 0.0;
			if ( nWindow < kMinWindow || m_flGameFps <= 0.0 )
				return;
			const double flFloor = std::max( kFloorFps, flOutputHz / 8.0 );
			if ( nMissed > kMissThreshold )
			{
				m_ulLastMiss = ulNow;
				if ( m_ulLastChange && ulNow - m_ulLastChange < kCooldownNs )
					return;
				if ( !m_bLimited )
				{
					m_bLimited = true;
					m_flCapFps = m_flGameFps;
				}
				m_flCapFps = std::max( flFloor, std::min( m_flCapFps, m_flGameFps * 1.05 ) * kDecreaseFactor );
				m_ulLastChange = ulNow;
				m_ulLastRaise = ulNow;
				return;
			}
			if ( !m_bLimited )
				return;
			m_flCapFps = std::max( flFloor, m_flCapFps );
			if ( nMissed > kCleanMisses )
				return;
			const uint64_t ulSince = std::max( m_ulLastMiss, m_ulLastChange );
			if ( ulNow - ulSince < kCleanNs || ulNow - m_ulLastRaise < kRaiseStepNs )
				return;
			m_ulLastRaise = ulNow;
			m_flCapFps += std::max( kIncreaseMinFps, m_flCapFps * kIncreasePct );
			if ( m_flCapFps > m_flGameFps * kUnlimitedMargin )
			{
				m_bLimited = false;
				m_flCapFps = 0.0;
				m_ulLastChange = ulNow;
			}
		}

		bool Unlimited() const { return !m_bLimited; }
		double CapFps() const { return m_flCapFps; }
		double GameFps() const { return m_flGameFps; }
		// The release interval; 0 while unlimited.
		uint64_t IntervalNs() const { return m_bLimited ? uint64_t( 1e9 / m_flCapFps ) : 0; }

	private:
		bool m_bLimited = false;
		double m_flCapFps = 0.0;
		uint64_t m_ulLastChange = 0, m_ulLastMiss = 0, m_ulLastRaise = 0, m_ulLastArrival = 0;
		double m_flGameFps = 0.0;
	};
}
