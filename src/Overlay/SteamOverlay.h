// The Steam overlay's two global switches (superdoc/features/steam-overlay.md).
//
//   "Steam overlay control"  global.json overlay.steam_overlay_control, default Off.
//        Off: the game carries the overlay as it always did. On: the dummy-window route
//        carries it. Read ONCE at startup (the overlay library is loaded when the game
//        starts), so a change applies on the next launch.
//   "Enable Steam Overlay"   global.json overlay.steam_overlay_enabled, default On.
//        Only meaningful while control was on at launch; starts / stops the dummy window
//        live.
//
// Both are global (machine-wide), never in a profile. UI-free: the Shell's tab binds to
// these functions.
#pragma once

#include <optional>

namespace gamescope
{
	namespace steamoverlay
	{
		// The route on for this process: the env override GS_RITZ_STEAM_OVERLAY_DUMMY
		// (1 on, 0 off; nullopt = unset) wins over the stored key.
		inline bool ResolveControl( std::optional<bool> oEnvOverride, bool bKey )
		{
			return oEnvOverride.value_or( bKey );
		}

		// Whether the dummy window should be running right now.
		inline bool HelperShouldRun( bool bControlAtLaunch, bool bEnabled )
		{
			return bControlAtLaunch && bEnabled;
		}
	}

	// The stored "Steam overlay control" value. The setter persists to global.json
	// (and repaints); it changes nothing in this process, see ...AtLaunch(). Any thread.
	bool SteamOverlay_Control();
	void SteamOverlay_SetControl( bool bControl );

	// The value this process STARTED with: the env override if set, else the key as it
	// was read at the first call (main() asks before Vulkan starts). The route runs on
	// this. The UI shows "Restart to apply" while Control() != ControlAtLaunch(). Any
	// thread.
	bool SteamOverlay_ControlAtLaunch();

	// The stored "Enable Steam Overlay" value. The setter persists to global.json; with
	// control-at-launch On it also starts or stops the dummy window (handed to the
	// steamcompmgr thread, which does the fork/kill), otherwise it only stores. Any thread.
	bool SteamOverlay_Enabled();
	void SteamOverlay_SetEnabled( bool bEnabled );
}
