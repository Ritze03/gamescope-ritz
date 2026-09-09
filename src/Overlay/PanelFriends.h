// The "Friends" area (`system.friends`) -- the friends you can join in the game
// you are already in, in this fork's own overlay, with no browser and no second
// sign-in.
//
// See superdoc/features/steam-friends.md for the feature, and
// superdoc/planning/steam-friends-join.md for the investigation that settled
// the design. The Steam half lives in src/SteamFriends.{h,cpp}; this file only
// declares the rows that read it, exactly as PanelKeybinds.cpp declares rows
// over chords it does not own.
//
// WHAT THIS AREA MUST NEVER DO, and the part of PanelFriends.cpp that stops it:
//
//   * NEVER CALL Snapshot() FROM A DRAW. It reads steamfriends::CurrentView(),
//     which copies what the poller thread last published and returns -- see
//     SteamFriends.h's poller section. A Steam that has stopped answering must
//     cost this panel a stale list, never a dropped frame.
//
//   * NEVER JOIN FROM A GETTER. A click sets a PENDING id; Tick(), which runs
//     once per frame on the steamcompmgr thread, is what acts on it. That is
//     what keeps `overlay_e2_set friends.list 2` -- which arrives on the
//     CONSOLE thread -- from forking a process on a thread that is not allowed
//     to.
//
//   * NEVER BE OFFERED WHEN THERE IS NO GAME TO JOIN. The area declares
//     AvailableWhen( a Steam app id exists ), so a non-Steam game (or a
//     gamescope not launched by Steam) has no Friends entry in the rail, no
//     Friends rows in the command palette, and no reachable sheet -- rather
//     than a page that could only ever be empty.
#pragma once

#include "UI/Registry.h"

namespace gamescope
{
	void PanelFriends_RegisterArea( ui::Registry &reg );

	// Seeds src/SteamFriends.cpp's copy of this session's Steam app id -- the
	// one fact the whole feature is scoped by. Called at startup from
	// main.cpp, beside PanelSystem_SeedFromConfig(), because the poller runs
	// on its own thread and config::SessionAppId() resolves lazily with no
	// lock. See the definition in PanelFriends.cpp.
	void PanelFriends_SeedFromConfig();

	// Once per frame, from Overlay/UI/Shell.cpp's Draw(), on the steamcompmgr
	// thread. Fires a join a click or a script asked for.
	void PanelFriends_Tick();
}
