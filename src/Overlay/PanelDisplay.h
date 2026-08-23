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

#include "UI/Registry.h"

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

	// E2 (P3). This panel's settings, DECLARED rather than drawn: three
	// areas -- Upscaling, Frame limiter, HDR -- covering exactly what the
	// four legacy tabs above cover, with the same config keys, the same
	// ranges and the same Set*() functions behind every binding.
	//
	// Called once at startup from Overlay/UI/Shell.cpp's RegisterAll().
	// Registration is data and touches no ImGui, so it has no thread
	// contract of its own; the BINDINGS it installs are invoked during the
	// shell's draw and inherit PanelDisplay_Draw()'s contract above.
	//
	// P2's PanelDisplay_DrawBody() -- the ui::Area::Escape() hatch that
	// hosted the legacy body verbatim in the E2 sheet -- is gone with this.
	void PanelDisplay_RegisterAreas( ui::Registry &reg );
}
