// Milestone M6 -- the "Shaders" panel of the settings overlay: the bundled
// effects (Vibrancy, Shadow Control, Pre-Sharpen, Adaptive Brightness) and
// their live controls. Since 2026-09-05 (DECISIONS.md #27) the effects are a
// native compute pre-pass compiled into the binary
// (src/shaders/cs_effects_layer0.comp, dispatched from vulkan_composite()),
// not a runtime-compiled ReShade .fx -- this panel writes g_nativeEffects
// (rendervulkan.hpp). See superdoc/planning/SPEC.md's Feature 2 and
// superdoc/planning/DECISIONS.md #12, #15, #27.
//
// Distinct from PanelDisplay.h's "Upscale Sharpness" (gamescope's own
// post-upscale RCAS/NIS sharpen, only live when Filter is FSR/NIS) -- this
// panel's "Pre-Sharpen" is a separate pre-upscale pass that works
// regardless of filter (DECISIONS.md #12). Both exist on purpose; see
// PanelShaders.cpp's file-level comment for why the UI must not collapse
// them into one control.
#pragma once

#include "UI/Registry.h"
#include "Config/ConfigSchema.h"

namespace gamescope
{
	// E2 (P3). This panel's four effects, DECLARED rather than drawn: one
	// `image.shaders` area whose switch rows each own their effect's
	// parameters. Same config keys, same ranges; every write lands in
	// g_nativeEffects -- see PanelShaders.cpp's "E2 (P3)" section. Called
	// once at startup from Overlay/UI/Shell.cpp.
	void PanelShaders_RegisterArea( ui::Registry &reg );

	// Startup apply: copies a resolved config's effect settings straight into
	// g_nativeEffects, so effects a user saved last session are on from the
	// first composited frame rather than from the first time the Shaders
	// area happens to be drawn (under E2 nothing in this file runs per frame
	// -- see PanelShaders.cpp's Cfg() comment). Called from main.cpp's
	// apply_ritz_config_to_startup_state(), beside the filter/sharpness
	// globals it already sets from the same config.
	void PanelShaders_ApplyStartupConfig( const config::Settings &config );
}
