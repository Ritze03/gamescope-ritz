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
	inline int EffectiveLimit( bool bOn, bool bNestedWayland, bool bVRR, bool bFrameGen,
		bool bTearingRequested, bool bHostVsync, int nUserLimit, int nRefreshHz )
	{
		if ( !bOn || !bNestedWayland || bVRR || bFrameGen || nRefreshHz <= 0 )
			return nUserLimit;
		if ( bTearingRequested && !bHostVsync )
			return nUserLimit;
		if ( nUserLimit > 0 && nUserLimit <= nRefreshHz )
			return nUserLimit;
		return nRefreshHz;
	}
}
