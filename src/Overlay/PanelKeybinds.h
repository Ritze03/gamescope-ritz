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
#include "Keybinds.h"

namespace gamescope
{
	void PanelKeybinds_RegisterArea( ui::Registry &reg );

	// Pushes the saved chords into the live binding table at startup, so a
	// rebind is in force from the first key event of a process rather than
	// from the first time the settings shell is opened. Called from main.cpp's
	// startup apply and from its live-apply hook -- the same shape (and the
	// same reason) as PanelSystem_SeedFromConfig().
	void PanelKeybinds_SeedFromConfig( const config::Settings &settings );

	// One action's chord row -- a capture chip over the action's chord, the
	// same binding (and the same capture pump) wherever it is declared. The
	// Keybinds area declares one per action; since 2026-09-22 the Zoom and
	// Autoclicker areas also declare a copy of their own action's row, so a
	// feature's hotkey is set where the feature is. Both copies are the one
	// chord in global.json: a rebind in either shows in the other.
	ui::Entry &PanelKeybinds_ChordRow( ui::Area &a, const char *pszId, const char *pszLabel,
	                                   keybinds::Action eAction );
}
