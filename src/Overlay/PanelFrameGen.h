// The "Frame generation" settings area (`image.framegen`, DISPLAY rail group,
// directly below Shaders) -- the user-facing half of the FrameGen integration.
// The renderer and the pacing live in FrameGen/FrameGenHost.h (`fghost`); this
// file owns only the four per-profile settings, their rows, and the one live
// Status line. See config::FrameGenSettings for the stored shape.
#pragma once

#include "UI/Registry.h"
#include "Config/ConfigSchema.h"

namespace gamescope
{
	// Declares the area (four rows and the Status line). Called once at
	// startup from Overlay/UI/Shell.cpp's RegisterAll().
	void PanelFrameGen_RegisterArea( ui::Registry &reg );

	// Pushes a resolved config's `framegen` section into the renderer
	// (fghost::SetConfig). Called from main.cpp's ritz_apply_config_live(),
	// which runs at startup AND on every profile switch/load, so the renderer
	// always holds the session profile's values whether or not the overlay has
	// ever been opened. (The area also re-pushes itself whenever the config
	// generation moves, the same way PanelShaders.cpp does.)
	void PanelFrameGen_ApplyStartupConfig( const config::Settings &config );
}
