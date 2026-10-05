// The nested-Wayland tearing hint as a STATE, not a per-paint value (pure: no
// clock, no backend -- every time is passed in, so it is unit-tested).
//
// The hint (wp_tearing_control: async = the host may show a buffer without
// waiting for a refresh) was recomputed on every paint from whatever the loop
// looked like at that instant. While frame generation generated, a single
// paint that was a genuine forced repaint, an ignored overlay repaint or a
// momentary pass-through (the pacer's plan flips between generating and passing
// through whenever the game's rate hovers around the output rate) turned it to
// vsync and the next paint turned it back: 22,995 flips in 44 minutes (a Forza
// Horizon 6 session, 2026-10-05), each one a wp_tearing_control request and a
// host-side change of how the surface is scheduled.
//
// Rule (Hint::Update, called once per main-loop iteration):
//   - bActive: the condition that wants async right now (frame generation's
//     output timer paces the output, tearing is allowed, the host offers the
//     protocol). It is LATCHED: the hint stays async until bActive has been false
//     for kHoldNs, so a vblank-driven paint between two timer paints, or a plan
//     that flickers for a few frames, does not change it.
//   - bExcluded: a genuine reason to present synchronously (an overlay is up, a
//     fade is playing). Immediate: vsync on the very call it appears. Async is
//     not resumed until it has been absent for kReenterNs, so something that
//     pulses faster than that stays on vsync instead of flapping.
// Why a dwell on both sides: a hint costs a protocol request and the host may
// reschedule the surface; a per-frame value is never what anyone wants, and the
// two directions fail differently (stay async: a few extra tearing frames; stay
// vsync: the tearing the user turned on is lost), so both are bounded by the same
// quarter second.
#pragma once

#include <cstdint>

namespace fgtear
{
	constexpr uint64_t kHoldNs = 250ull * 1000ull * 1000ull;     // inactive this long -> back to vsync
	constexpr uint64_t kReenterNs = 250ull * 1000ull * 1000ull;  // exclusion-free this long -> async again

	class Hint
	{
	public:
		// Returns the hint to present with now: true = async. Times are monotonic ns.
		bool Update( uint64_t ulNow, bool bActive, bool bExcluded )
		{
			if ( bExcluded )
			{
				m_ulLastExcluded = ulNow;
				m_bHasExcluded = true;
				return false;
			}
			if ( bActive )
			{
				m_ulLastActive = ulNow;
				m_bHasActive = true;
			}
			if ( !m_bHasActive || ulNow - m_ulLastActive >= kHoldNs )
				return false;
			if ( m_bHasExcluded && ulNow - m_ulLastExcluded < kReenterNs )
				return false;
			return true;
		}

	private:
		uint64_t m_ulLastActive = 0;
		uint64_t m_ulLastExcluded = 0;
		bool m_bHasActive = false;
		bool m_bHasExcluded = false;
	};
}
