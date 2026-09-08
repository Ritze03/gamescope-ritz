// The "Keybinds" area (`setup.keybinds`) -- this fork's own compositor
// hotkeys, editable from the settings shell. See superdoc/features/keybinds.md
// for the actions, the chord grammar, the conflict rule and the two ways back
// from a binding that made the settings unreachable.
//
// The chords themselves live in src/Keybinds.{h,cpp}; this file only declares
// the rows that read and write them, exactly as PanelSystem.cpp declares the
// clipboard switch over a flag it does not own. No ImGui in this header;
// called once at startup from Overlay/UI/Shell.cpp's RegisterAll().
#pragma once

#include "UI/Registry.h"
#include "Config/ConfigSchema.h"

namespace gamescope
{
	void PanelKeybinds_RegisterArea( ui::Registry &reg );

	// Pushes the saved chords into the live binding table at startup, so a
	// rebind is in force from the first key event of a process rather than
	// from the first time the settings shell is opened. Called from main.cpp's
	// startup apply and from its live-apply hook -- the same shape (and the
	// same reason) as PanelSystem_SeedFromConfig().
	void PanelKeybinds_SeedFromConfig( const config::Settings &settings );
}
