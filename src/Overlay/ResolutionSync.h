#pragma once

// "Sync with window size" -- the pure decision (superdoc/features/
// resolution-and-refresh.md, "Sync with window size"). Given the output size
// (what the host granted gamescope's window), the nested size (the game's
// resolution), the clock and what the previous call saw, say whether to apply
// the output size as the game's resolution now, to wait, or to do nothing.
// No compositor, no config, no clock of its own: the caller passes the time,
// so tests/test_resolution.cpp can drive it with made-up instants.
//
// Why a debounce: dragging a window edge produces a stream of sizes, and every
// applied size is a RandR mode change the game has to digest. The size is
// applied only once it has stayed the same for kSyncSettleNs. Turning the
// switch on (and the first look after a session start with it on) applies at
// once -- nobody is mid-drag then.

#include <cstdint>

namespace gamescope::resolution
{
	// 250 ms: long enough to ride out a drag (events come every 10-20 ms),
	// short enough that a released window feels immediate.
	constexpr int64_t kSyncSettleNs = 250ll * 1000 * 1000;

	enum class SyncAction { Nothing, Wait, Apply };

	struct SyncState
	{
		bool    bWasEnabled = false;   // the previous call had the sync on
		bool    bPending    = false;   // a differing size is being waited on
		int     nSeenWidth  = 0;       // the output size the wait is for
		int     nSeenHeight = 0;
		int64_t nSeenSinceNs = 0;      // when that size was first seen
	};

	// bEnabled: the sync is on AND not suppressed (see PanelDisplay.cpp).
	// Apply means: set the game's resolution to the output size (the caller
	// does it; the state already assumes it happened).
	inline SyncAction DecideSync( SyncState &st, bool bEnabled,
	                              int nOutputW, int nOutputH, int nNestedW, int nNestedH,
	                              int64_t nNowNs )
	{
		if ( !bEnabled )
		{
			st.bWasEnabled = false;
			st.bPending = false;
			return SyncAction::Nothing;
		}
		// A window that does not exist yet (0 size) leaves "just enabled"
		// armed, so its first real size is applied at once.
		if ( nOutputW <= 0 || nOutputH <= 0 )
		{
			st.bPending = false;
			return SyncAction::Nothing;
		}
		const bool bJustEnabled = !st.bWasEnabled;
		st.bWasEnabled = true;
		if ( nOutputW == nNestedW && nOutputH == nNestedH )
		{
			st.bPending = false;
			return SyncAction::Nothing;
		}
		if ( bJustEnabled )
		{
			st.bPending = false;
			return SyncAction::Apply;
		}
		if ( !st.bPending || st.nSeenWidth != nOutputW || st.nSeenHeight != nOutputH )
		{
			st.bPending = true;
			st.nSeenWidth = nOutputW;
			st.nSeenHeight = nOutputH;
			st.nSeenSinceNs = nNowNs;
			return SyncAction::Wait;
		}
		if ( nNowNs - st.nSeenSinceNs >= kSyncSettleNs )
		{
			st.bPending = false;
			return SyncAction::Apply;
		}
		return SyncAction::Wait;
	}
}
