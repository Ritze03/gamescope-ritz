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
//   * Friend              -- the one row the feature is about.
//   * the interface version lists, newest-first, and the order's reasoning.
//   * AppIdFromGameId()/GameIdType() -- unpacking Valve's CGameID.
//   * JoinabilityOf()     -- joinable, or the one reason it is not.
//   * BuildJoinUrl()      -- steam://joinlobby/..., and its guards.
//   * BuildJoinArgv()     -- the argv handed to Process::SpawnProcess().
//   * AppNameFromManifest()/LibraryPathsFromVdf()/GameLabel() -- turning an
//     app id into the words the panel prints.
//   * GroupOf()/FriendOrderLess() -- the order the list is drawn in.
//   * the app-name CACHE: its file format, its bound, and the one URL this
//     compositor is ever allowed to fetch.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace gamescope::steamfriends
{
	// =========================================================================
	//  Why a friend cannot be joined
	// =========================================================================
	// PHASE 3 CHANGED WHAT THE LIST CONTAINS, AND THIS ENUM IS THE CHANGE.
	// Phase 1 returned only friends with a non-zero m_steamIDLobby and threw
	// the rest away. The panel lists EVERY friend who is in a game and marks
	// which of them can be joined, because m_steamIDLobby's offset in
	// FriendGameInfo_t is still unproven (superdoc/planning/
	// steam-friends-join.md §6e): nobody was in a joinable lobby while the
	// read path was measured, and a WRONG OFFSET READS AS ZERO EXACTLY LIKE
	// "not joinable". A joinable-only list would therefore be
	// indistinguishable from a broken one -- an empty panel, and no way for
	// the user to tell which it was. Listing everyone in a game degrades
	// honestly: the user sees their friends and no Join, rather than nothing
	// at all.
	enum class Joinability : uint8_t
	{
		Yes = 0,        // the lobby id is non-zero and the row can build a URL
		NoLobby,        // in a game, but not in a lobby you can join
		NotASteamApp,   // a mod, a shortcut or a non-Steam game they added
		NoApp,          // a lobby with no app behind it
		NoSteamId,      // nobody to join
	};

	// The quiet reason a row is not joinable, in the panel's own words. Empty
	// for Yes, so a caller can print it unconditionally.
	inline constexpr std::string_view JoinabilityText( Joinability e )
	{
		switch ( e )
		{
		case Joinability::Yes:          return "";
		case Joinability::NoLobby:      return "not in a lobby you can join";
		case Joinability::NotASteamApp: return "not a Steam game";
		case Joinability::NoApp:        return "no game to join";
		case Joinability::NoSteamId:    return "no Steam ID";
		}
		return "";
	}

	// =========================================================================
	//  One friend who is in a game
	// =========================================================================
	// Exactly the four fields a join needs, plus the two the UI shows. Note
	// which of them the URL is allowed to touch: see BuildJoinUrl().
	struct Friend
	{
		std::string sPersona;    // display only -- NEVER reaches a command line
		std::string sGame;       // display only -- "Counter-Strike 2", else "App 730"
		uint32_t    uAppId   = 0;
		uint64_t    ulLobbyId = 0;
		uint64_t    ulSteamId = 0;
		Joinability eJoinable = Joinability::NoLobby;

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

		bool CanJoin() const { return eJoinable == Joinability::Yes; }
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
	//
	// It answers WHY rather than yes/no because phase 3's list shows the
	// friends it rejects (see Joinability above) and has to say something
	// true beside each of them.
	inline constexpr Joinability JoinabilityOf( uint64_t ulGameId, uint64_t ulLobbyId, uint64_t ulSteamId )
	{
		if ( ulSteamId == 0 )
			return Joinability::NoSteamId;                   // nobody to join
		if ( GameIdType( ulGameId ) != kGameIdTypeApp )
			return Joinability::NotASteamApp;                // a mod/shortcut, not a Steam app
		if ( AppIdFromGameId( ulGameId ) == 0 )
			return Joinability::NoApp;                       // no app to join
		if ( ulLobbyId == 0 )
			return Joinability::NoLobby;                     // not in a joinable lobby
		return Joinability::Yes;
	}

	inline constexpr bool IsJoinable( uint64_t ulGameId, uint64_t ulLobbyId, uint64_t ulSteamId )
	{
		return JoinabilityOf( ulGameId, ulLobbyId, ulSteamId ) == Joinability::Yes;
	}

	// =========================================================================
	//  The order the list is drawn in
	// =========================================================================
	// Requested 2026-09-09: "Joinable players should be sorted towards the top.
	// And topmost should be invites." Three bands, in that order, and inside a
	// band the rows are alphabetical.
	//
	// `Why the Invite band exists even though NOTHING PRODUCES ONE TODAY:` the
	// rule above is the user's, and it is written here in full so the day
	// invites become readable the ordering needs no second thought. But be
	// clear about the state of that day -- superdoc/features/steam-friends.md's
	// "Received invites" section is the measurement, and the short version is
	// that Steamworks has NO call that enumerates a pending received invite,
	// so GroupOf() below can only ever return Joinable or InGame. The panel
	// draws no invite row and offers no Accept/Deny, because there is nothing
	// true to draw. This enum is the rule, not a promise that it fires.
	enum class FriendGroup : uint8_t
	{
		Invite   = 0,   // an invite waiting for an answer -- see above: never produced today
		Joinable = 1,   // [Join] -- in a lobby you can walk into
		InGame   = 2,   // playing something, but not joinable
	};

	inline constexpr FriendGroup GroupOf( const Friend &f )
	{
		if ( f.bInvited )
			return FriendGroup::Invite;
		return f.CanJoin() ? FriendGroup::Joinable : FriendGroup::InGame;
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
	// split, nothing is substituted and there is no shell -- unlike the
	// companion's browser command (SteamCompanionCmd.h's BuildArgv), this
	// command line is not a setting the user can type into, so it needs none of
	// that machinery.
	inline constexpr std::string_view kSteamProgram = "steam";

	inline std::vector<std::string> BuildJoinArgv( std::string_view svUrl )
	{
		return { std::string( kSteamProgram ), std::string( svUrl ) };
	}

	// =========================================================================
	//  What to call the game
	// =========================================================================
	// ISteamFriends hands back an APP ID and nothing else -- there is no name
	// anywhere in the read path. "App 730" on every row would be a list of
	// numbers, so the name is looked up in the one place it is already on this
	// machine for free: Steam's own appmanifest_<appid>.acf, the file the
	// client writes for every INSTALLED game.
	//
	// `Why that is enough rather than a half-measure:` the case this feature
	// exists for is joining a friend in the game you are already in, and a
	// game you can join is a game you have installed. A friend in something
	// you do not own falls back to "App <id>", which is true, short and never
	// wrong -- unlike a web lookup, which would need a network call, a key or
	// both to say the same thing.
	//
	// Both parsers below are deliberate MINIMAL readers, not VDF parsers. They
	// take the first `"key" "value"` pair whose key matches, which is all
	// these two files need and is why they can live in a header a test runs
	// with no Steam installed.

	// The value of the first `"<key>" "<value>"` pair in `svText`. Empty when
	// there is none. Escapes are not interpreted: Steam writes app names
	// verbatim, and a half-done unescaper would be worse than none.
	inline std::string VdfFirstValue( std::string_view svText, std::string_view svKey )
	{
		std::string sNeedle = "\"";
		sNeedle += std::string( svKey );
		sNeedle += "\"";

		size_t nAt = 0;
		while ( ( nAt = svText.find( sNeedle, nAt ) ) != std::string_view::npos )
		{
			size_t i = nAt + sNeedle.size();
			// Only whitespace may sit between the key and its value.
			while ( i < svText.size() && ( svText[ i ] == ' ' || svText[ i ] == '\t' ) )
				i++;
			if ( i >= svText.size() || svText[ i ] != '"' )
			{
				nAt += sNeedle.size();
				continue;
			}
			const size_t nStart = i + 1;
			const size_t nEnd = svText.find( '"', nStart );
			if ( nEnd == std::string_view::npos )
				return {};
			return std::string( svText.substr( nStart, nEnd - nStart ) );
		}
		return {};
	}

	// The game's name out of an appmanifest_<appid>.acf.
	inline std::string AppNameFromManifest( std::string_view svAcf )
	{
		return VdfFirstValue( svAcf, "name" );
	}

	// Every library root listed in a libraryfolders.vdf, in file order. Steam
	// writes one `"path"` per numbered block; the games on a second drive live
	// under those, and a friend playing one of them would otherwise be a bare
	// app id.
	inline std::vector<std::string> LibraryPathsFromVdf( std::string_view svVdf )
	{
		std::vector<std::string> vecOut;
		const std::string_view svKey = "\"path\"";
		size_t nAt = 0;
		while ( ( nAt = svVdf.find( svKey, nAt ) ) != std::string_view::npos )
		{
			size_t i = nAt + svKey.size();
			while ( i < svVdf.size() && ( svVdf[ i ] == ' ' || svVdf[ i ] == '\t' ) )
				i++;
			nAt += svKey.size();
			if ( i >= svVdf.size() || svVdf[ i ] != '"' )
				continue;
			const size_t nStart = i + 1;
			const size_t nEnd = svVdf.find( '"', nStart );
			if ( nEnd == std::string_view::npos )
				break;
			if ( nEnd > nStart )
				vecOut.push_back( std::string( svVdf.substr( nStart, nEnd - nStart ) ) );
			nAt = nEnd;
		}
		return vecOut;
	}

	// =========================================================================
	//  The one line that says why the list is the length it is
	// =========================================================================
	// The list shows every friend in a game, so "how many are there" and "how
	// many can I actually join" are two different numbers and the status line
	// has to carry both without becoming a sentence nobody reads. Pure, so
	// tests/test_steam_friends.cpp holds every wording.
	inline std::string StatusLine( size_t nInGame, size_t nJoinable )
	{
		if ( nInGame == 0 )
			return "nobody's in a game right now.";

		const std::string sInGame = std::to_string( (unsigned long long)nInGame ) +
			( nInGame == 1 ? " friend in a game" : " friends in a game" );

		if ( nJoinable == 0 )
			return sInGame + ", none you can join.";
		if ( nJoinable == nInGame )
			return sInGame + ", all joinable.";
		return sInGame + ", " + std::to_string( (unsigned long long)nJoinable ) + " you can join.";
	}

	// What the row prints for the game: the name when one was found, else the
	// app id in words. Never empty for a real app, so the panel never has to
	// draw a blank column.
	inline std::string GameLabel( uint32_t uAppId, std::string_view svName )
	{
		if ( !svName.empty() )
			return std::string( svName );
		if ( uAppId == 0 )
			return "a game Steam has no id for";
		return "App " + std::to_string( (unsigned long long)uAppId );
	}
}
