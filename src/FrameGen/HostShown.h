// How many of the commits gamescope sent to a nested host compositor the host actually
// showed. The output rate ("presented 280 fps") counts what gamescope sent; the host
// answers every commit with wp_presentation feedback, `presented` (it reached the screen)
// or `discarded` (a newer commit replaced it before the screen was updated). Only the
// nested Wayland backend feeds this; everywhere else the counters stay at zero and the
// status line leaves the numbers out.
#pragma once

#include <atomic>
#include <cstdint>

namespace fghostshown
{
	struct Counters
	{
		std::atomic<uint64_t> ulCommits{ 0 };
		std::atomic<uint64_t> ulShown{ 0 };
		std::atomic<uint64_t> ulDiscarded{ 0 };
	};

	inline Counters &Get()
	{
		static Counters s_Counters;
		return s_Counters;
	}

	// Rates since the previous call of this function (the status line's own period).
	struct Rates
	{
		bool bValid = false;
		double flCommits = 0, flShown = 0, flDiscarded = 0;
	};

	inline Rates TakeRates( uint64_t ulNowNs )
	{
		static uint64_t s_ulLastNs = 0, s_ulC = 0, s_ulS = 0, s_ulD = 0;
		Counters &C = Get();
		const uint64_t c = C.ulCommits.load( std::memory_order_relaxed );
		const uint64_t s = C.ulShown.load( std::memory_order_relaxed );
		const uint64_t d = C.ulDiscarded.load( std::memory_order_relaxed );
		Rates r;
		if ( s_ulLastNs && ulNowNs > s_ulLastNs && c > s_ulC )
		{
			const double flSec = double( ulNowNs - s_ulLastNs ) / 1e9;
			r.bValid = true;
			r.flCommits = double( c - s_ulC ) / flSec;
			r.flShown = double( s - s_ulS ) / flSec;
			r.flDiscarded = double( d - s_ulD ) / flSec;
		}
		s_ulLastNs = ulNowNs;
		s_ulC = c;
		s_ulS = s;
		s_ulD = d;
		return r;
	}
}
