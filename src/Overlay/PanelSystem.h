// The "System" tab (`system.general`) -- the first area in the SYSTEM rail
// section. Phase A of the System settings tab: for now it hosts exactly one
// thing, clipboard sync's on/off switch and status row. Later phases add
// more system-level settings here; see superdoc/planning/requests-2026-09-05.md
// item 5.
//
// Modelled on PanelCursor.h/.cpp: declared rows, no ImGui in this header,
// called once at startup from Overlay/UI/Shell.cpp's RegisterAll().
#pragma once

#include "UI/Registry.h"

namespace gamescope
{
	void PanelSystem_RegisterArea( ui::Registry &reg );
}
