#pragma once

// =============================================================================
//  "Join a friend" -- the parts with no Steam in them
// =============================================================================
// See superdoc/planning/steam-friends-join.md for the whole investigation that
// settled this design (§5 for the shape, §6 for why it binds to the client's
// own steamclient.so rather than to a per-game libsteam_api.so, §6d for the
// failure modes). src/SteamFriends.{h,cpp} is the half that dlopens things and
// forks; this header is deliberately free of all of it, so
// tests/test_steam_friends.cpp can hold the rules with no Steam installed, no
// compositor and no child process.
//
// Same split, and the same reason, as SteamCompanionCmd.h next door.
//
// WHAT LIVES HERE:
//   * JoinableFriend      -- the one row the feature is about.
//   * the interface version lists, newest-first, and the order's reasoning.
//   * AppIdFromGameId()/GameIdType() -- unpacking Valve's CGameID.
//   * IsJoinable()        -- the whole filter, in one predicate.
//   * BuildJoinUrl()      -- steam://joinlobby/..., and its guards.
//   * BuildJoinArgv()     -- the argv handed to Process::SpawnProcess().

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gamescope::steamfriends
{
	// =========================================================================
	//  One joinable friend
	// =========================================================================
	// Exactly the four fields a join needs, plus the one the UI shows. Note
	// which of them the URL is allowed to touch: see BuildJoinUrl().
	struct JoinableFriend
	{
		std::string sPersona;    // display only -- NEVER reaches a command line
		uint32_t    uAppId   = 0;
		uint64_t    ulLobbyId = 0;
		uint64_t    ulSteamId = 0;
	};

	// =========================================================================
	//  Interface versions
	// =========================================================================
	// steamclient.so exposes every interface it has ever shipped, side by side,
	// keyed by a version string: this machine's client answers to
	// SteamClient006..023 and SteamFriends001..018 (read out of the shipped
	// library with `strings`). That versioning is precisely what makes binding
	// to a hand-declared vtable tolerable rather than reckless -- a caller
	// pinning "SteamFriends017" keeps the layout that string named.
	//
	// THE ORDER IS NEWEST-FIRST, and the list is CLOSED. Both halves matter:
	//
	//   * newest-first, because the newest interface is the one the running
	//     client actually uses internally; the older strings are compatibility
	//     shims kept alive for old games, and a shim is one more layer that can
	//     be wrong. (The honest counter-argument -- that an OLD version's
	//     leading slots are frozen forever and therefore safer -- is real, and
	//     it is why the list is closed rather than why the order is reversed:
	//     every entry below shares the leading vtable layout we declare in
	//     SteamFriends.cpp, so either order would work today, and the study
	//     that settled this design (§6a) specified newest-first.)
	//
	//   * closed, because the guard against a future client that reshuffles the
	//     vtable is that we NEVER cast into a version string we do not know.
	//     A client offering only SteamFriends019 makes this feature disable
	//     itself with one named log line -- not guess, and not crash.
	//
	// Adding a version here is therefore a deliberate act: it asserts that the
	// leading ISteamFriends slots in SteamFriends.cpp are still that version's
	// leading slots -- and that assertion is worth more than it looks, because
	// the layout there is one slot away from the order every published write-up
	// gives, and it took a live read against a real client to find that out.
	// Only SteamFriends018 has actually been measured; 017 and 016 are here on
	// the compatibility argument alone, and a build that finds itself using one
	// of them is running against a client old enough to deserve a fresh look.
	inline constexpr std::array<std::string_view, 4> kClientVersions = {
		"SteamClient023", "SteamClient022", "SteamClient021", "SteamClient020",
	};

	inline constexpr std::array<std::string_view, 3> kFriendsVersions = {
		"SteamFriends018", "SteamFriends017", "SteamFriends016",
	};

	// =========================================================================
	//  CGameID
	// =========================================================================
	// ISteamFriends::GetFriendGamePlayed hands back a CGameID, which is a
	// bitfield, not an app id:
	//
	//     uint32 m_nAppID : 24;    // bits  0..23
	//     uint32 m_nType  :  8;    // bits 24..31
	//     uint32 m_nModID : 32;    // bits 32..63
	//
	// `Why the type check is not optional:` type 0 (k_EGameIDTypeApp) is a real
	// Steam app; anything else is a mod, a shortcut or a non-Steam game the
	// user added themselves, and its low bits are NOT an app id Steam would
	// accept in a steam:// URL. Filtering on the type here means a friend
	// playing a non-Steam shortcut can never produce a join URL pointing at
	// some unrelated app.
	//
	// (The feasibility probe printed `gameID & 0xFFFFFFFF` as the app id. For a
	// type-0 app that is the same number, because the type byte is zero -- so
	// the probe's output and this function agree wherever it matters.)
	inline constexpr uint32_t AppIdFromGameId( uint64_t ulGameId )
	{
		return (uint32_t)( ulGameId & 0x00FFFFFFull );
	}

	inline constexpr uint8_t GameIdType( uint64_t ulGameId )
	{
		return (uint8_t)( ( ulGameId >> 24 ) & 0xFFull );
	}

	inline constexpr uint8_t kGameIdTypeApp = 0;

	// =========================================================================
	//  The filter -- the entire feature, in one predicate
	// =========================================================================
	// m_steamIDLobby is non-zero exactly when the friend is in a lobby you can
	// join. That single field is what the whole join list is built on (§5). The
	// other two conditions are what keeps the row USABLE once it is shown: a
	// row we cannot build a URL from is worse than no row.
	inline constexpr bool IsJoinable( uint64_t ulGameId, uint64_t ulLobbyId, uint64_t ulSteamId )
	{
		if ( ulLobbyId == 0 )
			return false;                                    // not in a joinable lobby
		if ( GameIdType( ulGameId ) != kGameIdTypeApp )
			return false;                                    // a mod/shortcut, not a Steam app
		if ( AppIdFromGameId( ulGameId ) == 0 )
			return false;                                    // no app to join
		if ( ulSteamId == 0 )
			return false;                                    // nobody to join
		return true;
	}

	// =========================================================================
	//  The URL
	// =========================================================================
	// steam://joinlobby/<appid>/<lobbyid>/<steamid64>, handed to the RUNNING
	// Steam client by exec'ing `steam <url>`, which forwards it over
	// ~/.steam/steam.pipe.
	//
	// `joinlobby` IS STEAM'S OWN MECHANISM, NOT A GUESS. It is a registered
	// entry in the URL-command table inside
	// ~/.steam/steam/ubuntu12_32/steamui.so, sitting alongside `rungame`,
	// `rungameid`, `openurl` and `connect`, dispatched by
	// CSteamURLController::ExecuteSteamURL -- and the string immediately after
	// `joinlobby` in that binary is `+connect_lobby %llu`, the launch argument
	// Steam appends when it acts on a join. (superdoc/planning/
	// steam-friends-join.md §5, "Action -- hand the running client a URL".)
	//
	// THE SECURITY STORY, AND IT IS THE WHOLE REASON THIS TAKES A STRUCT:
	// every byte of the URL is produced by std::to_string() over an INTEGER.
	// sPersona -- the one field a remote person controls -- has no path into
	// this function's output at all, so a friend who renames themselves
	// `x";rm -rf ~;"` changes the URL by exactly nothing. That is a property of
	// the types, not of an escaping routine that could be forgotten, and
	// tests/test_steam_friends.cpp pins it by building the same URL twice with
	// two hostile names and requiring the bytes to be identical.
	//
	// The zero guards are separate and are refusals, not sanitisation: a lobby
	// id of 0 means "this friend is not joinable", and firing
	// steam://joinlobby/730/0/... at the client is a nonsense request we should
	// never make. Same for an app id or a SteamID of 0.
	inline bool BuildJoinUrl( const JoinableFriend &f, std::string *pOut, std::string *psError )
	{
		auto Fail = [ & ]( const char *pszWhy ) {
			if ( psError ) *psError = pszWhy;
			if ( pOut ) pOut->clear();
			return false;
		};

		if ( f.ulLobbyId == 0 )
			return Fail( "That friend isn't in a lobby you can join." );
		if ( f.uAppId == 0 )
			return Fail( "That friend isn't in a Steam game." );
		if ( f.ulSteamId == 0 )
			return Fail( "That friend has no Steam ID." );

		std::string s = "steam://joinlobby/";
		s += std::to_string( (unsigned long long)f.uAppId );
		s += '/';
		s += std::to_string( (unsigned long long)f.ulLobbyId );
		s += '/';
		s += std::to_string( (unsigned long long)f.ulSteamId );

		if ( pOut ) *pOut = std::move( s );
		if ( psError ) psError->clear();
		return true;
	}

	// The argv Process::SpawnProcess() execvp()s. Two arguments, both of them
	// ours: the program, and one URL built by BuildJoinUrl() above. Nothing is
	// split, nothing is substituted and there is no shell -- unlike the
	// companion's browser command (SteamCompanionCmd.h's BuildArgv), this
	// command line is not a setting the user can type into, so it needs none of
	// that machinery.
	inline constexpr std::string_view kSteamProgram = "steam";

	inline std::vector<std::string> BuildJoinArgv( std::string_view svUrl )
	{
		return { std::string( kSteamProgram ), std::string( svUrl ) };
	}
}
