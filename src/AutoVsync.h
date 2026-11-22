#pragma once

#include <cstdint>

// Auto V-Sync (Display > General, gamescope.auto_vsync, ConVar ritz_auto_vsync): the
// game's frame limit while the output cannot show frames faster than the refresh.
// Pure, so tests/test_limiter_pace.cpp can pin it. See superdoc/features/display-general.md.

namespace autovsync
{
	// The limit the compositor applies, from the user's own (0 = none; the stepper, the
	// gamescope_control protocol, --framerate-limit, debug_set_fps_limit). The auto limit
	// only ever lowers "no limit" or "above the refresh" to the refresh, and only while:
	//   - the switch is on, on the nested Wayland backend, not under VRR, and frame
	//     generation / motion blur is off (those pace on the layer's timer, which is not
	//     locked to the host's refreshes, and frame generation caps its own output on
	//     such a host);
	//   - the output cannot show more than one frame per refresh: tearing is not
	//     requested, or gamescope composites its paints (bComposites, CompositeGate
	//     below: a composited paint is always a sync flip, one per vblank-timer tick,
	//     whatever the host would do with a torn frame), or the host was found to show
	//     our tearing-hinted frames on its vblank anyway (bHostVsync,
	//     fghostshown::VsyncVerdict).
	// ponytail: nested Wayland only; DRM without tearing and the SDL backend would gain
	// from the same rule, once someone can test it there.
	// Why it limits or not: the FIRST condition that keeps it from limiting, in the order
	// checked. Limits = it applies the refresh. Text() is the `status:` line's wording.
	enum class Why { Limits, Off, NotNested, Vrr, FrameGen, NoRefresh, HostTears, UserLimit };

	inline Why Reason( bool bOn, bool bNestedWayland, bool bVRR, bool bFrameGen,
		bool bTearingRequested, bool bComposites, bool bHostVsync, int nUserLimit, int nRefreshHz )
	{
		if ( !bOn )
			return Why::Off;
		if ( !bNestedWayland )
			return Why::NotNested;
		if ( bVRR )
			return Why::Vrr;
		if ( bFrameGen )
			return Why::FrameGen;
		if ( nRefreshHz <= 0 )
			return Why::NoRefresh;
		if ( bTearingRequested && !bComposites && !bHostVsync )
			return Why::HostTears;
		if ( nUserLimit > 0 && nUserLimit <= nRefreshHz )
			return Why::UserLimit;
		return Why::Limits;
	}

	inline const char *Text( Why e )
	{
		switch ( e )
		{
			case Why::Limits: return "limiting";
			case Why::Off: return "switch off";
			case Why::NotNested: return "not nested Wayland";
			case Why::Vrr: return "VRR in use";
			case Why::FrameGen: return "frame generation or motion blur on";
			case Why::NoRefresh: return "no refresh known";
			case Why::HostTears: return "the host tears (verdict)";
			case Why::UserLimit: return "the user's own limit applies";
		}
		return "?";
	}

	inline int EffectiveLimit( bool bOn, bool bNestedWayland, bool bVRR, bool bFrameGen,
		bool bTearingRequested, bool bComposites, bool bHostVsync, int nUserLimit, int nRefreshHz )
	{
		return Reason( bOn, bNestedWayland, bVRR, bFrameGen, bTearingRequested, bComposites, bHostVsync, nUserLimit, nRefreshHz ) == Why::Limits
			? nRefreshHz : nUserLimit;
	}

	// Why the output shows at most one frame per refresh, while it limits: the first that
	// holds, in this order. Only HostVsync keeps the tearing hint async under the limit
	// (bAutoVsyncAsync in steamcompmgr.cpp, so the verdict keeps sampling); under the
	// other two, what is sent to the host does not change.
	enum class Cause { NoTearing, HostVsync, Composites };

	inline Cause LimitCause( bool bTearingRequested, bool bHostVsync )
	{
		if ( !bTearingRequested )
			return Cause::NoTearing;
		if ( bHostVsync )
			return Cause::HostVsync;
		return Cause::Composites;
	}

	inline const char *CauseText( Cause e )
	{
		switch ( e )
		{
			case Cause::NoTearing: return "tearing is not requested";
			case Cause::HostVsync: return "the host does not tear";
			case Cause::Composites: return "gamescope draws every frame itself, one per refresh";
		}
		return "?";
	}

	// "gamescope composites its paints", debounced (bComposites above). A paint counts as
	// sync-only when it was composited or an overlay window was up: the conditions under
	// which the main loop never flips a paint async, whatever the game asks
	// (WasCompositing(), bHasOverlay). Neither depends on the game's present mode, which
	// the limit itself turns to FIFO. The state can flap for a moment (an overlay opening,
	// a fade), so:
	//   - it turns on once sync-only paints kept coming, no gap longer than kHoldNs,
	//     for kEngageNs: a lone composited paint never engages the limit;
	//   - it turns off once no sync-only paint came for kHoldNs.
	// Note() on every paint, Composites() on every vblank.
	struct CompositeGate
	{
		static constexpr uint64_t kHoldNs = 500'000'000ull;
		static constexpr uint64_t kEngageNs = 250'000'000ull;

		uint64_t ulLast = 0;    // the newest sync-only paint, 0 = none yet
		uint64_t ulSince = 0;   // the first of the current run (gaps <= kHoldNs)
		bool bOn = false;

		void Note( uint64_t ulNow, bool bSyncOnly )
		{
			if ( !bSyncOnly )
				return;
			if ( ulLast == 0 || ulNow - ulLast > kHoldNs )
				ulSince = ulNow;
			ulLast = ulNow;
		}

		bool Composites( uint64_t ulNow )
		{
			const bool bRecent = ulLast != 0 && ulNow - ulLast <= kHoldNs;
			if ( !bRecent )
				bOn = false;
			else if ( !bOn && ulLast - ulSince >= kEngageNs )   // the run itself spans kEngageNs
				bOn = true;
			return bOn;
		}
	};
}
