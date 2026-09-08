// The "Friends" area (`system.friends`) -- the friends you can join, in this
// fork's own overlay, with no browser and no second sign-in.
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
//   * NEVER JOIN FROM A GETTER. A click sets a PENDING index; Tick(), which
//     runs once per frame on the steamcompmgr thread, is what acts on it. That
//     is what keeps `overlay_e2_set friends.list 2` -- which arrives on the
//     CONSOLE thread -- from opening a modal or forking a process on a thread
//     that is not allowed to.
#pragma once

#include "UI/Registry.h"

namespace gamescope
{
	void PanelFriends_RegisterArea( ui::Registry &reg );

	// Once per frame, from Overlay/UI/Shell.cpp's Draw(), on the steamcompmgr
	// thread. Acts on a join a click or a script asked for -- either firing it
	// or opening the confirmation the different-game case needs.
	void PanelFriends_Tick();
}
