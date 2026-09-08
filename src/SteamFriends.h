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
// Phases 1 and 2 of §7b: no UI, no hotkey. The only way in is the `friends_dump`
// and `friends_join` ConCommands at the bottom of SteamFriends.cpp.
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
// any thread, but it is NOT safe to call from a paint: a Snapshot() is a round
// trip to the Steam client. Today's only callers are the two ConCommands, on
// the console thread. Phase 3's panel must poll it a few seconds apart, off the
// frame path (§6d's threading rule).

#include "SteamFriendsCmd.h"

#include <string>
#include <vector>

namespace gamescope::steamfriends
{
	// Friends currently in a lobby you can join, newest read every call.
	// Empty on every failure, and on "nobody is in a joinable game" -- those
	// two are deliberately indistinguishable to the caller; StatusText() is
	// what tells them apart for a user.
	std::vector<JoinableFriend> Snapshot();

	// Ask the running Steam client to join this friend's lobby. Returns false
	// (and fills *psError with one user-facing sentence) if the row cannot
	// produce a URL or `steam` is not installed. Never blocks: the client is
	// handed the URL by a child process that forwards it and exits.
	bool Join( const JoinableFriend &friendToJoin, std::string *psError );

	// One line about why the list is the length it is: "Steam isn't running",
	// "signed out", "3 friends you can join". For phase 3's empty states and
	// for `friends_dump`.
	std::string StatusText();
}
