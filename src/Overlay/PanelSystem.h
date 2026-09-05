// The "System" tab (`system.general`) -- the first area in the SYSTEM rail
// section. For now it hosts exactly one thing, clipboard sync's on/off
// switch and status row; later phases add more system-level settings here.
// See superdoc/planning/requests-2026-09-05.md item 5 and
// superdoc/features/clipboard-sync.md's "Settings" section.
//
// Modelled on PanelCursor.h/.cpp: declared rows, no ImGui in this header,
// called once at startup from Overlay/UI/Shell.cpp's RegisterAll().
#pragma once

#include "UI/Registry.h"

namespace gamescope
{
	void PanelSystem_RegisterArea( ui::Registry &reg );

	// Seeds gamescope::g_bClipboardSyncEnabled (Clipboard/ClipboardSync.h)
	// from config::SystemSettings::clipboard_sync, so the switch's saved
	// value is in force from the first clipboard event of a process, not
	// from the first time the settings overlay is opened.
	//
	// PanelSystem_RegisterArea() calls this too, but the registry is built
	// lazily -- Shell.cpp's Reg() runs RegisterAll() the first time the
	// shell is drawn -- so a process in which the user never opens the
	// overlay would otherwise run on the compiled-in default. Call it once
	// from startup (SettingsOverlay.cpp's launch warm-up block, or main.cpp
	// right after apply_ritz_config_to_startup_state()); it is a single
	// config read, idempotent, and cheap to call again.
	void PanelSystem_SeedFromConfig();
}
