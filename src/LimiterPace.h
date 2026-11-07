#pragma once

// The Frame limiter's pacing rules, shared by the compositor and the WSI layer. Pure, so
// tests/test_limiter_pace.cpp can pin them. See superdoc/features/frame-limiter.md.

#include <cstdint>

namespace limiterpace
{
	// Does the WSI layer pace the game (true), or does the compositor hold its commits to
	// the vblank grid? A limit that divides the refresh is exact on the grid and locked to
	// the display, so it stays there while nothing else needs the game's frames at their
	// own times. Everything else goes to the layer's timer: on the grid, 120 on 280 Hz is
	// really 140, and a game freed at a paint lands its GPU burst on the next one.
	inline bool LayerPaces( bool bVRR, bool bFrameGen, int nRefreshHz, int nTargetFps )
	{
		return bVRR || bFrameGen || nRefreshHz % nTargetFps != 0;
	}

	// The end of the next frame's slot. Absolute, so the rate is exact and a late frame
	// does not lower it (the next one is simply not held as long); a game more than a
	// slot behind starts a new schedule rather than rushing to catch up.
	inline uint64_t NextDeadline( uint64_t ulPrevDeadline, uint64_t ulNow, uint32_t uFps )
	{
		const uint64_t ulInterval = 1'000'000'000ull / uFps;
		const uint64_t ulDeadline = ulPrevDeadline + ulInterval;
		return ulDeadline + ulInterval < ulNow ? ulNow : ulDeadline;
	}
}
