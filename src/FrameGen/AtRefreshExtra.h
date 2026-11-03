// Frame generation at the display refresh on a backend that can exceed it: which timer
// paces the output, and at what interval. Pure, so tests/test_framegen_pacing.cpp can
// pin it; the host (steamcompmgr.cpp FrameGen_*) supplies the numbers.
//
// The library's ExtraTimer() band stops at o < 0.98 tick, so Target "display refresh"
// (o == tick exactly) is driven by the vblank timer, where a paint that overruns its
// vblank (the game's GPU burst delays our queued composite) loses that output for good.
// A target above the refresh is driven by the extra timer and does not. Why this lives
// here and not in the library: the fix is a host choice of timer, the library's plan
// (o, delay, content time) is unchanged.
#pragma once

#include <algorithm>

namespace fgatrefresh
{
	// o within this fraction of one tick counts as "at the refresh".
	constexpr double kBand = 0.02;
	// Paint every kOversupply * tick instead of every tick: the host shows only the newest
	// buffer per vblank, so a timer at exactly the refresh loses outputs where a late paint
	// and its successor land in one host vblank, and has no slack to make a late one up.
	// A little surplus is what the user's 320-on-280 workaround amounted to (0.875 tick).
	constexpr double kOversupply = 0.9;

	inline bool AtRefresh( double flOutMs, double flTickMs )
	{
		return flOutMs > 0.0 && flOutMs >= flTickMs * ( 1.0 - kBand ) && flOutMs <= flTickMs * ( 1.0 + kBand );
	}

	// Should the extra timer drive this output? bCanExceed: the backend can show a present
	// between vblanks; bPause: the cap at the refresh is on (Pause at refresh rate).
	inline bool UseExtraTimer( bool bCanExceed, bool bPause, double flOutMs, double flTickMs )
	{
		return bCanExceed && !bPause && AtRefresh( flOutMs, flTickMs );
	}

	// The interval the extra timer ticks at for plan interval flOutMs.
	inline double TimerIntervalMs( double flOutMs, double flTickMs )
	{
		return flOutMs >= flTickMs * ( 1.0 - kBand ) ? std::min( flOutMs, flTickMs * kOversupply ) : flOutMs;
	}
}
