#pragma once

// Auto V-Sync (Display > General, gamescope.auto_vsync, ConVar ritz_auto_vsync): the
// game's frame limit while the output cannot show frames faster than the refresh.
// Pure, so tests/test_auto_vsync.cpp can pin it. See superdoc/features/display-general.md.

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
	//     requested, or the host was found to show our tearing-hinted frames on its
	//     vblank anyway (bHostVsync, fghostshown::VsyncVerdict).
	// ponytail: nested Wayland only; DRM without tearing and the SDL backend would gain
	// from the same rule, once someone can test it there.
	// Why it limits or not: the FIRST condition that keeps it from limiting, in the order
	// checked. Limits = it applies the refresh. Text() is the `status:` line's wording.
	enum class Why { Limits, Off, NotNested, Vrr, FrameGen, NoRefresh, HostTears, UserLimit };

	inline Why Reason( bool bOn, bool bNestedWayland, bool bVRR, bool bFrameGen,
		bool bTearingRequested, bool bHostVsync, int nUserLimit, int nRefreshHz )
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
		if ( bTearingRequested && !bHostVsync )
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
		bool bTearingRequested, bool bHostVsync, int nUserLimit, int nRefreshHz )
	{
		return Reason( bOn, bNestedWayland, bVRR, bFrameGen, bTearingRequested, bHostVsync, nUserLimit, nRefreshHz ) == Why::Limits
			? nRefreshHz : nUserLimit;
	}
}
