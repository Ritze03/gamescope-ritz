// Milestone M3 -- the "Display" panel of the settings overlay: gamescope's
// own live upscale filter/scaler, the auto-corrected sharpness slider, and
// the VRR/HDR/tearing toggles. See superdoc/planning/SPEC.md's Feature 4
// ("Live gamescope options") and Build order M3.
//
// This panel needs almost no new plumbing -- every value it edits is already
// a fully live, runtime-mutable global or ConVar that paint_all() reads every
// frame (see superdoc/planning/runtime-knobs-and-fps.md Part A). The panel's
// entire job is drawing ImGui widgets that write those same variables.
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
}
