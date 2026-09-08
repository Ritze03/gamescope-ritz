#pragma once

// =============================================================================
//  "Join a friend" -- the runtime half
// =============================================================================
// Reads the ALREADY-LOGGED-IN Steam client's friends list, locally, and offers
// the ones sitting in a joinable lobby; acts on a choice by handing the running
// client a steam://joinlobby/... URL. No Web API key, no browser, no second
// Steam client, no app id and no SteamAPI_Init anywhere.
//
// superdoc/planning/steam-friends-join.md is the investigation that settled all
// of this -- §5 for the shape, §6a for why route B (the client's own
// steamclient.so) rather than a per-game libsteam_api.so, §6c for the probe
// that proved the library loads and every symbol resolves on this machine,
// §6d for the failure modes this file has to survive.
//
// Phases 1 and 2 of §7b built the read and the join; phase 3 added the panel
// (src/Overlay/PanelFriends.cpp, area `system.friends`) and the POLLER at the
// bottom of this header, and phase 4 the `Ctrl+Shift+Tab` binding. The
// `friends_dump` / `friends_join` ConCommands are still there and still work.
//
// THE RULES, AND THE PART OF SteamFriends.cpp THAT ENFORCES EACH:
//
//   * NOTHING HAPPENS UNTIL ASKED. steamclient.so is 46 MB and dlopening it
//     starts threads inside our process; Library() does it lazily, on the first
//     Snapshot() or Join(), never at startup. A build of this fork that never
//     touches the feature never loads Steam's library at all.
//
//   * EVERY FAILURE IS AN EMPTY VECTOR AND ONE NAMED LOG LINE. Steam not
//     running, no library, a library that is not Steam's, an interface version
//     we do not know, signed out -- see Snapshot()'s comment for the table.
//     Nothing here throws, nothing blocks and nothing is fatal.
//
//   * WE NEVER CAST INTO A VERSION WE DO NOT KNOW. The version lists in
//     SteamFriendsCmd.h are closed; an unknown client disables the feature.
//
//   * WE NEVER dlclose(). See Library()'s comment -- unloading a library that
//     has spawned threads is how a compositor crashes minutes later, in
//     somebody else's stack.
//
//   * A PERSONA NAME NEVER REACHES A COMMAND LINE. BuildJoinUrl() is built from
//     three integers; see its comment in SteamFriendsCmd.h.
//
//   * SteamIDs, lobby ids and persona names STAY OUT OF THE LOG. Log lines from
//     this file carry counts, indices and app ids and nothing else -- the same
//     rule the committed probe output follows, for the same reason: these lines
//     end up in the overlay's log area and in pasted bug reports.
//
// THREADING. Everything here is guarded by one mutex and is safe to call from
// any thread, but Snapshot() is NOT safe to call from a paint: it is a round
// trip to the Steam client, and a wedged client would take the compositor's
// frame with it. THE PANEL THEREFORE NEVER CALLS IT. It calls CurrentView(),
// which only copies whatever the poller thread last published -- see the
// poller section below, which is §6d's threading rule made structural rather
// than remembered.

#include "SteamFriendsCmd.h"

#include <string>
#include <vector>

namespace gamescope::steamfriends
{
	// EVERY friend currently in a game, newest read every call, each carrying
	// whether it can be joined and -- when it cannot -- the one reason why
	// (Friend::eJoinable, SteamFriendsCmd.h's Joinability).
	//
	// `Why not only the joinable ones, which is what phase 1 returned:`
	// m_steamIDLobby's offset is still unproven, and a wrong offset reads as
	// zero exactly like "not in a lobby". A joinable-only list would be
	// indistinguishable from a broken one. See Joinability's own comment.
	//
	// Empty on every failure, and on "nobody is in a game" -- those two are
	// deliberately indistinguishable to the caller; StatusText() is what tells
	// them apart for a user.
	//
	// BLOCKS. Never call it from a paint; call CurrentView() instead.
	std::vector<Friend> Snapshot();

	// Ask the running Steam client to join this friend's lobby. Returns false
	// (and fills *psError with one user-facing sentence) if the row cannot
	// produce a URL or `steam` is not installed. Never blocks: the client is
	// handed the URL by a child process that forwards it and exits.
	bool Join( const Friend &friendToJoin, std::string *psError );

	// One line about why the list is the length it is: "Steam isn't running",
	// "Steam is signed out.", "3 friends in a game, 1 you can join." For the
	// panel's empty states and for `friends_dump`.
	std::string StatusText();

	// =========================================================================
	//  The poller -- the only thing the panel is allowed to touch
	// =========================================================================
	// ONE BACKGROUND THREAD, AND THE PANEL NEVER WAITS ON IT. Snapshot() is a
	// round trip to another process over ~/.steam/steam.pipe; a Steam that is
	// swapping, starting up or wedged can make that take seconds, and doing it
	// on the steamcompmgr thread -- which is the frame path -- would drop
	// frames in a running game. So a worker owns every Steam call and
	// publishes its result, and CurrentView() copies the published result and
	// returns.
	//
	// IT ONLY RUNS WHILE SOMEBODY IS LOOKING. CurrentView() records that it
	// was asked; the worker polls every few seconds while the asks keep
	// coming and then goes back to sleep on a condition variable. A build
	// whose friends panel is never opened and whose hotkey is never pressed
	// starts no thread and dlopens nothing, exactly as before phase 3.
	struct View
	{
		std::vector<Friend> vecFriends;   // everyone in a game, joinable or not
		std::string         sStatus;      // StatusText() as of the last poll
		size_t              nJoinable = 0;
		bool                bPolled   = false;  // has a poll ever finished?
		double              flAgeSec  = 0.0;    // how old the rows are
	};

	// Never blocks, never touches Steam, safe from a paint. Also arms the
	// poller: the first call starts the worker thread.
	View CurrentView();

	// Poll now rather than at the next interval -- the panel's Refresh verb.
	// Returns immediately; the answer arrives in a later CurrentView().
	void RequestRefresh();

	// steamcompmgr_exit(). Stops and joins the worker. Idempotent, and a
	// no-op when the poller was never armed.
	void Shutdown();
}
