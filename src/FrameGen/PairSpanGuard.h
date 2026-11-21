// The pair span guard: is a pair of real frames too far apart in time to interpolate between?
// (superdoc/features/frame-generation-renderer.md, "Pair span guard".) Header-only and pure -- no Vulkan, no
// globals -- so tests/test_framegen_pair_span.cpp checks it without the compositor.
//
// The pacer resets only after an arrival gap of 100 ms or more; a shorter hitch of the game leaves a pair whose
// two frames arrived several frame intervals apart, and that pair is interpolated like any other. The host
// (FrameGen_PrePaint() in steamcompmgr.cpp) asks this predicate for every Decision that would use a pair and,
// when it says so, shows the pair's later real frame instead.
#pragma once

#include <cstdint>
#include <cstring>

namespace fgspan
{
	struct Verdict
	{
		bool   guard = false;    // the pair is too far apart: show a real frame instead of interpolating it
		double spanMs = 0.0;     // the pair's arrival span (0 when unknown or out of order)
		double limitMs = 0.0;    // k x interval (0 when the guard is off or the interval unknown)
	};

	// std::isfinite() is folded to true under the project's -ffast-math, so NaN / inf are told by their bit pattern.
	inline bool IsFinite( double f )
	{
		uint64_t u;
		std::memcpy( &u, &f, sizeof u );
		return ( u & 0x7FF0000000000000ull ) != 0x7FF0000000000000ull;
	}

	// prevNs / currNs: the arrival times (ns) of the pair's earlier and later real frame, -1 (any negative value)
	// when unknown. flIntervalMs: the game's current frame interval (the pacer's estimate), 0 when unknown.
	// flK: the limit in frame intervals; 0 or negative switches the guard off.
	// Guards when currNs - prevNs > flK x flIntervalMs, strictly. Never guards on an unknown time, an unknown
	// interval, k <= 0, a non-finite input, or a pair whose later frame arrived first (the pacer itself treats
	// such a span as 1 ns, i.e. no motion to cover).
	inline Verdict Check( int64_t prevNs, int64_t currNs, double flIntervalMs, double flK )
	{
		Verdict v;
		if ( !IsFinite( flK ) || !( flK > 0.0 ) )
			return v;
		if ( !IsFinite( flIntervalMs ) || !( flIntervalMs > 0.0 ) )
			return v;
		v.limitMs = flK * flIntervalMs;
		if ( prevNs < 0 || currNs < 0 || currNs <= prevNs )
			return v;
		v.spanMs = double( currNs - prevNs ) / 1e6;
		v.guard = double( currNs - prevNs ) > v.limitMs * 1e6;
		return v;
	}
}
