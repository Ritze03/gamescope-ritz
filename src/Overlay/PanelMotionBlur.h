// The "Motion blur" settings area (`image.motionblur`, MOTION rail group, directly
// below Frame generation) -- the user-facing half of the library's interpolation-
// based motion blur. All the work (which window, how many samples, the averaging) is
// the frame-gen-ritz library's (gpu/pacing.h plans it, recordSynthBlur renders it);
// this file owns only the five per-profile settings, their rows and the one live
// Status line. See config::MotionBlurSettings for the stored shape and
// superdoc/features/motion-blur.md for the model.
#pragma once

#include "UI/Registry.h"
#include "Config/ConfigSchema.h"

namespace gamescope
{
	// Declares the area (five rows and the Status line). Called once at startup
	// from Overlay/UI/Shell.cpp's RegisterAll().
	void PanelMotionBlur_RegisterArea( ui::Registry &reg );

	// Pushes a resolved config's `motion_blur` section into the renderer and the
	// pacer (fghost::SetBlurConfig). Called from main.cpp's ritz_apply_config_live(),
	// which runs at startup AND on every profile switch/load, so the renderer always
	// holds the session profile's values whether or not the overlay has ever been
	// opened. (The area also re-pushes itself whenever the config generation moves,
	// the same way PanelFrameGen.cpp does.)
	void PanelMotionBlur_ApplyStartupConfig( const config::Settings &config );
}
