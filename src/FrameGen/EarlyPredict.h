#pragma once

// The one piece of arithmetic in the "early pair" prediction (steamcompmgr.cpp's
// FrameGen_EarlyPredict()): when the next paint will present. The rest of the prediction
// is a COPY of the library's pacer asked the question it will be asked (OnPaint at that V).

#include <algorithm>
#include <cstdint>

namespace fgearly
{
	// lastV: the last paint's V (0 = none yet). tickNs: one refresh. extraNs: the extra timer's
	// interval when that paces the output (0 = the vblank grid does).
	// Vblank grid: the first grid point after lastV that is not already past; extra timer:
	// lastV + interval, never in the past (fgpacing::NextExtraTargetNs, the arming rule).
	inline uint64_t NextPaintV( uint64_t lastV, uint64_t now, uint64_t tickNs, uint64_t extraNs )
	{
		if ( lastV == 0 )
			return now;
		if ( extraNs )
			return std::max( lastV + extraNs, now );
		uint64_t V = lastV + tickNs;
		if ( tickNs && V < now )
			V += ( ( now - V + tickNs - 1 ) / tickNs ) * tickNs;
		return V;
	}
}
