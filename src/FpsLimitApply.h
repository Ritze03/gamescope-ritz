#pragma once

// The Frame limiter's saved value, as the compositor applies it (startup and profile
// switch). Pure, so tests/test_limiter_pace.cpp can pin it. See
// superdoc/features/frame-limiter.md.

#include <algorithm>

namespace fpslimit
{
	// 0 (unlimited) or [kMin, kMax]: 1-9 fps is a trap (the overlay itself repaints at that
	// rate), so any nonzero request is raised to the floor.
	inline constexpr int kMin = 10;
	inline constexpr int kMax = 480;

	inline int Clamp( int nFps )
	{
		return nFps <= 0 ? 0 : std::clamp( nFps, kMin, kMax );
	}

	// Should a config with this fps_limit be pushed into the running compositor? Only when
	// it differs from the last value the config pushed: a reload for any other reason must
	// not overwrite a limit set live meanwhile (the gamescope_control protocol, the
	// overlay's stepper, debug_set_fps_limit). nLast starts at -1, so the first call pushes.
	inline bool NeedsPush( int &nLast, int nConfigFps )
	{
		const int n = Clamp( nConfigFps );
		if ( n == nLast )
			return false;
		nLast = n;
		return true;
	}
}
