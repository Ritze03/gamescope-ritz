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
// WHAT LIVES HERE:
//   * Friend              -- the one row the feature is about.
//   * the interface version lists, newest-first, and the order's reasoning.
//   * AppIdFromGameId()/GameIdType() -- unpacking Valve's CGameID.
//   * InThisGame()        -- is this friend in the game WE are running?
//   * IsJoinable()        -- can a URL be built for them at all?
//   * BuildJoinUrl()      -- steam://joinlobby/..., and its guards.
//   * BuildJoinArgv()     -- the argv handed to Process::SpawnProcess().
//   * GroupOf()/FriendOrderLess() -- the order the list is drawn in.
//   * StatusLine()        -- the two counts, and why there are two.
//
// WHAT USED TO LIVE HERE AND DOES NOT ANY MORE (2026-09-09): everything about
// a game's NAME -- the appmanifest reader, the libraryfolders.vdf reader and
// GameLabel(). The list is now scoped to the game this session is already
// running, so every row would have printed the same words; the name lookup
// (and src/SteamAppNames.h, and this compositor's only outbound network
// request) went with them. See superdoc/features/steam-friends.md.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gamescope::steamfriends
{
	// =========================================================================
	//  One friend you can join
	// =========================================================================
	// Exactly the three fields a join needs, plus the one the UI shows. Note
	// which of them the URL is allowed to touch: see BuildJoinUrl().
	//
	// THERE IS NO `sGame` AND NO "why not" FIELD ANY MORE. Every row that
	// reaches this struct is in the game this session is running AND joinable
	// (SteamFriends.cpp's Snapshot() applies both predicates below), so a game
	// name would repeat the same words down the whole list and a reason would
	// always be "none".
	struct Friend
	{
		std::string sPersona;    // display only -- NEVER reaches a command line
		uint32_t    uAppId    = 0;
		uint64_t    ulLobbyId = 0;
		uint64_t    ulSteamId = 0;

		// "This person has invited you, and the invite is still waiting."
		//
		// NOTHING IN THE SHIPPING READ PATH EVER SETS THIS, and that is a
		// measured conclusion rather than an unfinished job: Steamworks has no
		// call that enumerates a pending received invite, and the two
		// invite-shaped callbacks it does have (GameLobbyJoinRequested_t,
		// GameRichPresenceJoinRequested_t) fire when the user has ALREADY
		// accepted one in Steam's own UI, addressed to the game registered for
		// that app id. See superdoc/features/steam-friends.md, "Received
		// invites", for the whole measurement and for what would have to
		// change. The field is here because FriendOrderLess() below encodes
		// the user's stated order ("topmost should be invites") in full, and
		// tests/test_steam_friends.cpp pins BOTH halves: that the band sorts
		// first, and that Snapshot() never puts a row in it.
		bool        bInvited = false;
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
	// some unrelated app -- and, since 2026-09-09, can never be mistaken for
	// somebody in OUR app either, because a shortcut whose low 24 bits happen
	// to equal our app id would otherwise match InThisGame() below.
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
	//  The two predicates the whole feature is
	// =========================================================================
	// A row is drawn iff BOTH answer true. They are separate functions rather
	// than one because the STATUS LINE needs the first count on its own: "3
	// friends in this game, 0 you can join" is the sentence that tells a
	// working lobby read with nobody available apart from a lobby field being
	// read from the wrong offset (superdoc/planning/steam-friends-join.md §6e,
	// which is still open). One predicate would collapse those two numbers
	// into one and lose that.

	// Is this friend playing the game THIS session is running? A session with
	// no Steam app id (uSessionAppId == 0) matches nobody, which is what makes
	// "not a Steam game" an empty list rather than a special case.
	inline constexpr bool InThisGame( uint64_t ulGameId, uint32_t uSessionAppId )
	{
		return uSessionAppId != 0 &&
		       GameIdType( ulGameId ) == kGameIdTypeApp &&
		       AppIdFromGameId( ulGameId ) == uSessionAppId;
	}

	// Can a join URL actually be built for them? m_steamIDLobby is non-zero
	// exactly when the friend is in a lobby you can join; that single field is
	// what the whole feature is built on (§5). The other conditions are what
	// keeps the row USABLE once it is shown -- a row we cannot build a URL
	// from is worse than no row.
	inline constexpr bool IsJoinable( uint64_t ulGameId, uint64_t ulLobbyId, uint64_t ulSteamId )
	{
		return ulSteamId != 0 &&
		       GameIdType( ulGameId ) == kGameIdTypeApp &&
		       AppIdFromGameId( ulGameId ) != 0 &&
		       ulLobbyId != 0;
	}

	// =========================================================================
	//  The order the list is drawn in
	// =========================================================================
	// Requested 2026-09-09: "Joinable players should be sorted towards the top.
	// And topmost should be invites." Since the 2026-09-09 narrowing every row
	// IS joinable, so only two of the three original bands can hold anything --
	// and one of those two never does.
	//
	// `Why the Invite band is kept even though NOTHING PRODUCES ONE:` the rule
	// above is the user's own, it costs ten lines, and deleting it would lose
	// the only written record of the order they asked for. But be clear about
	// the state of that day -- superdoc/features/steam-friends.md's "Received
	// invites" section is the measurement, and the short version is that
	// Steamworks has NO call that enumerates a pending received invite, so
	// GroupOf() below can only ever return Joinable. The panel draws no invite
	// row and offers no Accept/Deny, because there is nothing true to draw.
	// This enum is the rule, not a promise that it fires.
	enum class FriendGroup : uint8_t
	{
		Invite   = 0,   // an invite waiting for an answer -- see above: never produced today
		Joinable = 1,   // [Join] -- in a lobby you can walk into
	};

	inline constexpr FriendGroup GroupOf( const Friend &f )
	{
		return f.bInvited ? FriendGroup::Invite : FriendGroup::Joinable;
	}

	// ASCII-only lowering, deliberately: this runs over persona names, which
	// are arbitrary UTF-8, and a locale-aware fold would make the order depend
	// on the user's locale AND drag std::locale into a header a test includes.
	// Bytes >= 0x80 are left alone, so two names that differ only in accents
	// sort by their bytes -- stable, predictable, and never wrong twice.
	inline std::string LowerAscii( std::string_view sv )
	{
		std::string s( sv );
		for ( char &c : s )
			if ( c >= 'A' && c <= 'Z' )
				c = (char)( c - 'A' + 'a' );
		return s;
	}

	// A STRICT WEAK ORDERING, and a TOTAL one: band, then persona folded to
	// lower case, then the SteamID.
	//
	// `Why the SteamID is in there at all:` two friends can share a display
	// name -- personas are not unique, and a friend can rename themselves into
	// somebody else's name on purpose. Without a final tiebreak std::sort
	// would be free to swap them on every poll, and the list would flicker
	// between two orders while nothing about it had changed. The SteamID makes
	// the order a function of the DATA rather than of the sort's internals, so
	// an unchanged friends list draws identically forever.
	inline bool FriendOrderLess( const Friend &a, const Friend &b )
	{
		const FriendGroup eA = GroupOf( a );
		const FriendGroup eB = GroupOf( b );
		if ( eA != eB )
			return (uint8_t)eA < (uint8_t)eB;

		const std::string sA = LowerAscii( a.sPersona );
		const std::string sB = LowerAscii( b.sPersona );
		if ( sA != sB )
			return sA < sB;

		return a.ulSteamId < b.ulSteamId;
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
	inline bool BuildJoinUrl( const Friend &f, std::string *pOut, std::string *psError )
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
	// split, nothing is substituted and there is no shell -- this command line
	// is not a setting the user can type into, so it needs none of that
	// machinery.
	inline constexpr std::string_view kSteamProgram = "steam";

	inline std::vector<std::string> BuildJoinArgv( std::string_view svUrl )
	{
		return { std::string( kSteamProgram ), std::string( svUrl ) };
	}

	// =========================================================================
	//  The one line that says why the list is the length it is
	// =========================================================================
	// TWO COUNTS, AND THE SECOND ONE IS THE WHOLE REASON THIS FUNCTION IS NOT
	// JUST vec.size(). The list holds only the friends who are in this game AND
	// joinable, so an empty list has two very different causes -- and one of
	// them is a bug we know we might have.
	//
	// m_steamIDLobby's offset inside FriendGameInfo_t is UNPROVEN
	// (superdoc/planning/steam-friends-join.md §6e): nobody has ever been
	// observed in a joinable lobby, so all we know is that the field reads as
	// zero -- which is exactly what a WRONG OFFSET would also do. So:
	//
	//   "0 friends in this game."               -> nobody is here. Says nothing
	//                                              about the lobby read.
	//   "3 friends in this game, 0 you can join" -> the friends read works, the
	//                                              app-id match works, and only
	//                                              the lobby field is empty.
	//                                              THAT is the sentence that
	//                                              tells a broken offset from a
	//                                              quiet evening.
	//
	// The joinable count is always printed as a NUMBER rather than as "none",
	// so the two figures always sit side by side and can be read as a pair.
	// Pure, so tests/test_steam_friends.cpp holds every wording.
	inline std::string StatusLine( size_t nInThisGame, size_t nJoinable )
	{
		if ( nInThisGame == 0 )
			return "nobody else is in this game right now.";

		return std::to_string( (unsigned long long)nInThisGame ) +
			( nInThisGame == 1 ? " friend in this game, " : " friends in this game, " ) +
			std::to_string( (unsigned long long)nJoinable ) + " you can join.";
	}
}
