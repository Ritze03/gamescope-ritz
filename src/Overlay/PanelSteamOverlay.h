// The Steam Overlay area (SETTINGS rail group, id "setup.steam_overlay"): the two global
// switches of the dummy-window route. See SteamOverlay.h and
// superdoc/features/steam-overlay.md.
#pragma once

namespace gamescope
{
	namespace ui { class Registry; }

	void PanelSteamOverlay_RegisterArea( ui::Registry &reg );
}
