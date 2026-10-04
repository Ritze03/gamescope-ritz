// The "Lag spike buffer" settings area (`image.lagbuffer`, MOTION rail group, directly
// below Motion blur) -- the user-facing half of the library's lag-spike buffer. All the
// work (spike detection, the recency-weighted target, the ramped delay, the gap fills,
// the history that follows the buffer) is the frame-gen-ritz library's (gpu/pacing.h);
// this file owns only the four per-profile settings, their rows and the one live
// Status line. See config::LagBufferSettings for the stored shape and
// superdoc/features/lag-spike-buffer.md for the model.
#pragma once

#include "UI/Registry.h"
#include "Config/ConfigSchema.h"

namespace gamescope
{
	// Declares the area (four rows and the Status line). Called once at startup
	// from Overlay/UI/Shell.cpp's RegisterAll().
	void PanelLagBuffer_RegisterArea( ui::Registry &reg );

	// Pushes a resolved config's `lag_buffer` section into the pacer
	// (fghost::SetLagBufferConfig). Called from main.cpp's ritz_apply_config_live(),
	// which runs at startup AND on every profile switch/load, so the pacer always
	// holds the session profile's values whether or not the overlay has ever been
	// opened. (The area also re-pushes itself whenever the config generation moves,
	// the same way PanelMotionBlur.cpp does.)
	void PanelLagBuffer_ApplyStartupConfig( const config::Settings &config );
}
