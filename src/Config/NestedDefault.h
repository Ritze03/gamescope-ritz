// The game's default (nested) resolution when neither -w/-h nor a saved
// nested_width/height gave one. Pure, so tests/test_resolution.cpp can pin the
// precedence without a backend. superdoc/features/resolution-and-refresh.md,
// "Default game resolution".
#pragma once

#include <cstdint>

namespace gamescope::config
{
	struct NestedSize
	{
		uint32_t uWidth;
		uint32_t uHeight;
	};

	// The size "Native" means: the screen's own resolution (g_nScreenWidth/
	// Height, filled by the nested backends, 0 = unknown), else the output
	// window's size -- today's behaviour, and the DRM/headless/OpenVR answer.
	inline NestedSize NativeNestedSize( uint32_t uScreenW, uint32_t uScreenH, uint32_t uOutputW, uint32_t uOutputH )
	{
		if ( uScreenW && uScreenH )
			return { uScreenW, uScreenH };
		return { uOutputW, uOutputH };
	}

	// bOutputSizeGiven: the user passed -W/-H. That is an explicit request for
	// a window size, so the game follows it as upstream always did; only an
	// unspecified window defers to the screen (whose first-pop-up size is an
	// accident of the host, not a choice).
	inline NestedSize DefaultNestedSize( bool bOutputSizeGiven, uint32_t uScreenW, uint32_t uScreenH, uint32_t uOutputW, uint32_t uOutputH )
	{
		if ( bOutputSizeGiven )
			return { uOutputW, uOutputH };
		return NativeNestedSize( uScreenW, uScreenH, uOutputW, uOutputH );
	}
}
