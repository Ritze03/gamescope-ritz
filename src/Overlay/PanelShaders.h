// Milestone M6 -- the "Shaders" panel of the settings overlay: the combined
// ReShade effect (Vibrancy + Pre-Sharpen, gamescope-ritz.fx) and its live
// controls. See superdoc/planning/SPEC.md's Feature 2 ("ReShade effects")
// and Build order M6, and superdoc/planning/DECISIONS.md #12-#15.
//
// Distinct from PanelDisplay.h's "Upscale Sharpness" (gamescope's own
// post-upscale RCAS/NIS sharpen, only live when Filter is FSR/NIS) -- this
// panel's "Pre-Sharpen" is a separate pre-upscale ReShade pass that works
// regardless of filter (DECISIONS.md #12). Both exist on purpose; see
// PanelShaders.cpp's file-level comment for why the UI must not collapse
// them into one control.
#pragma once

namespace gamescope
{
	// Draws the Shaders panel's ImGui window. Must be called from the same
	// place/thread SettingsOverlay draws its own window (steamcompmgr
	// thread, between ImGui::NewFrame() and ImGui::Render()) -- see
	// PanelShaders.cpp's file-level comment for why that's a correctness
	// requirement here, not just a style preference (this panel writes the
	// same plain-global ReShade entry points the render thread reads/calls
	// every frame, with no lock of its own beyond the ones those entry
	// points already take internally).
	void PanelShaders_Draw();
}
