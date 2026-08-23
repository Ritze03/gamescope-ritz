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

#include "UI/Registry.h"

namespace gamescope
{
	// E2 (P3). This panel's three effects, DECLARED rather than drawn: one
	// `image.shaders` area whose three switch rows each own their effect's
	// parameters. Same config keys, same ranges, and every write still goes
	// through the runtime-uniform path -- see PanelShaders.cpp's "E2 (P3)"
	// section. Called once at startup from Overlay/UI/Shell.cpp.
	void PanelShaders_RegisterArea( ui::Registry &reg );
}
