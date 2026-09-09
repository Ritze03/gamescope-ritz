#pragma once

// =============================================================================
//  "Join a friend" -- the runtime half
// =============================================================================
// Reads the ALREADY-LOGGED-IN Steam client's friends list, locally, and offers
// the ones sitting in a joinable lobby OF THE GAME THIS SESSION IS RUNNING;
// acts on a choice by handing the running client a steam://joinlobby/... URL.
// No Web API key, no browser, no second Steam client, no app id handed to
// Steam, no SteamAPI_Init anywhere -- AND, since 2026-09-09, NO NETWORK AT ALL:
// this compositor makes no outbound request of any kind.
//
// superdoc/planning/steam-friends-join.md is the investigation that settled all
// of this -- §5 for the shape, §6a for why route B (the client's own
// steamclient.so) rather than a per-game libsteam_api.so, §6c for the probe
// that proved the library loads and every symbol resolves on this machine,
// §6d for the failure modes this file has to survive.
// superdoc/features/steam-friends.md is the feature as shipped.
//
// THE 2026-09-09 NARROWING, because it changes what Snapshot() means: the list
// used to be every friend in any game, with a name looked up for each. It is
// now only the friends who are IN THIS GAME AND JOINABLE -- the user's request
// was "only showing friends which are playing the same game as the running one
// ... just show joinable friends in the UI". Every row is therefore the game
// you are already in, which is what let the whole game-name machinery (the
// appmanifest reader, the disk cache and the one HTTP request this fork ever
// made) be deleted rather than kept.
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

#include <cstdint>
#include <string>
#include <vector>

namespace gamescope::steamfriends
{
	// =========================================================================
	//  Which game we are in
	// =========================================================================
	// The app id this session is running under, 0 for "not a Steam game, or no
	// game identified". EVERYTHING the feature does is scoped by it: with 0,
	// Snapshot() is empty, StatusText() says why, and the panel's whole area is
	// hidden (PanelFriends.cpp's AvailableWhen).
	//
	// `Why an atomic here rather than a config::SessionAppId() call:` that
	// getter resolves lazily into a plain static on first use, with no lock, so
	// calling it from the POLLER thread would race the draw thread's first
	// call. Seeding one integer from PanelFriends_SeedFromConfig() -- which
	// runs on the main thread at startup -- keeps the worker away from the
	// config layer entirely. It is exactly the arrangement the (now deleted)
	// name-lookup switch used, and for the same reason.
	void     SetSessionAppId( uint32_t uAppId );
	uint32_t SessionAppId();

	// The friends who are in THIS session's game AND in a lobby you can join,
	// newest read every call, already in the order the panel draws them.
	//
	// Empty on every failure, on "no app id", on "nobody else is in this game"
	// and on "they are all here but none of them is joinable". Those are
	// deliberately indistinguishable to this caller; StatusText() is what tells
	// them apart for a user, and it is the reason the status line carries BOTH
	// counts -- see StatusLine() in SteamFriendsCmd.h.
	//
	// BLOCKS. Never call it from a paint; call CurrentView() instead.
	std::vector<Friend> Snapshot();

	// Ask the running Steam client to join this friend's lobby. Returns false
	// (and fills *psError with one user-facing sentence) if the row cannot
	// produce a URL or `steam` is not installed. Never blocks: the client is
	// handed the URL by a child process that forwards it and exits.
	bool Join( const Friend &friendToJoin, std::string *psError );

	// One line about why the list is the length it is: "Steam isn't running",
	// "Steam is signed out.", "3 friends in this game, 0 you can join." For the
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
		// The rows the panel draws: joinable, in this game.
		std::vector<Friend> vecFriends;
		std::string         sStatus;      // StatusText() as of the last poll
		// How many friends were in this game AT ALL, joinable or not. The
		// panel does not draw them -- it is what makes "0 you can join"
		// distinguishable from "the lobby read is broken". See StatusLine().
		size_t              nInThisGame = 0;
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
