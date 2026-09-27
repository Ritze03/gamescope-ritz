// The "General" area under the INPUT rail group (`input.general`, added
// 2026-09-27) -- the user's own table, verbatim (TERMINOLOGY.md's Rail
// group entry): "Input: General [new], Autoclicker, Null binds." This is
// that new General: two switches about what gamescope takes exclusive hold
// of from the host while nested, force grab CURSOR and force grab KEYBOARD.
//
// Force grab cursor MOVED here from display.general (PanelDisplay.cpp) --
// same config field (gamescope.force_grab_cursor), same live path
// (steamcompmgr_set_force_relative_mouse()), only the row's home changed;
// see PanelDisplay.cpp's RegisterGeneral() for what it used to look like
// there. Its row id changed from display.force_grab_cursor to
// input.force_grab_cursor -- nothing outside this Shell hardcodes the old
// id (scripts/settings-audit.sh enumerates rows live, never a fixed list;
// see its own header comment), so the rename is safe. Force grab keyboard
// is genuinely new.
//
// Modelled on PanelSystem.h/.cpp: declared rows, no ImGui in this header,
// called once at startup from Overlay/UI/Shell.cpp's RegisterAll(). See
// superdoc/features/input-general.md for what each switch does, and per
// backend, and PanelInput.cpp for the fields themselves.
#pragma once

#include "UI/Registry.h"

namespace gamescope
{
	void PanelInput_RegisterArea( ui::Registry &reg );
}
