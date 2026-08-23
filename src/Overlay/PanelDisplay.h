// Milestone M3, renamed/expanded in issue #25 -- the "GAMESCOPE" panel of the
// settings overlay (window title "GAMESCOPE", chrome::PanelId::Display
// unchanged): gamescope's own live upscale filter/scaler, the auto-corrected
// sharpness slider, VRR/HDR/tearing/force-grab-cursor toggles, a frame
// limiter, and an HDR tuning tab, organized into tabs. See
// superdoc/planning/SPEC.md's Feature 4 ("Live gamescope options") and Build
// order M3.
//
// This panel needs almost no new plumbing -- every value it edits is already
// a fully live, runtime-mutable global, ConVar, or gamescope_color_mgmt_t
// setter that paint_all()/update_color_mgmt() reads/diffs every frame (see
// superdoc/planning/runtime-knobs-and-fps.md Part A). The panel's entire job
// is drawing ImGui widgets that write those same variables -- except the
// Frame Limiter tab, which round-trips through an X11 property since
// GAMESCOPE_FPS_LIMIT has no plain-global equivalent (see PanelDisplay.cpp's
// SetFpsLimit() comment).
#pragma once

namespace gamescope
{
	// Draws the Display panel's ImGui window. Must be called from the same
	// place/thread SettingsOverlay draws its own window (steamcompmgr thread,
	// between ImGui::NewFrame() and ImGui::Render()) -- every control here
	// writes plain (non-atomic) globals and ConVars that paint_all() and
	// vulkan_composite() read per-frame on that same thread with no locking,
	// so same-thread-only is a correctness requirement, not just a style
	// preference. See PanelDisplay.cpp's file-level comment for the thread-
	// safety argument in full.
	void PanelDisplay_Draw();

	// E2 MIGRATION SEAM (P2). The same body, with no window around it, for
	// the E2 shell to host inside its sheet through ui::Area::Escape() --
	// see Overlay/UI/Registry.h's Escape() comment for why that hatch
	// exists and what deletes it. Draws into whatever ImGui window is
	// current; the caller owns the Begin/End and the style. Same thread
	// contract as PanelDisplay_Draw() above, unchanged.
	//
	// TEMPORARY: P3 rewrites this panel against the ui:: kit and both this
	// declaration and its call site go away with it.
	void PanelDisplay_DrawBody();
}
