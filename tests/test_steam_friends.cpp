// "Join a friend" -- phases 1 and 2, held down by tests that need no Steam.
//
// TWO HALVES, MATCHING THE TWO FILES:
//
//   * SteamFriendsCmd.h is pure, so the version lists, the CGameID unpacking,
//     the joinable predicate and -- the one that matters most -- the URL
//     builder's guards are ordinary in-process assertions.
//
//   * SteamFriends.cpp dlopens a library and calls through a vtable, so the
//     runtime cases run in a FORKED CHILD with GS_RITZ_STEAMCLIENT pointed at
//     tests/steamclient_stub.cpp's fake. The fork is not ceremony: the loader
//     deliberately tries exactly once per process (a compositor must not retry
//     a 46 MB dlopen every poll), so each failure mode needs its own process to
//     be the first thing that process ever sees.
//
// NOTHING HERE TOUCHES A REAL STEAM. No test points the loader at the user's
// installed steamclient.so, no test creates a pipe to a live client, and
// NOTHING calls Join() -- firing a steam://joinlobby URL would reach whatever
// game the developer happens to be in the middle of. The real library's
// symbols are confirmed separately, read-only, by
// build-release/verify-shots/steam-child-session-2026-09-08/steamclient_probe
// --symbols.

#include <catch2/catch_test_macros.hpp>

#include "SteamAppNames.h"
#include "SteamFriends.h"
#include "SteamFriendsCmd.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <time.h>

#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace gamescope::steamfriends;

// ===========================================================================
//  The interface version lists
// ===========================================================================
TEST_CASE( "the interface version lists are newest-first and closed", "[steam_friends]" )
{
	REQUIRE( kClientVersions.front() == "SteamClient023" );
	REQUIRE( kFriendsVersions.front() == "SteamFriends018" );

	// Strictly descending. The lists are same-length, same-prefix strings, so
	// a plain string compare IS the version compare -- and this is what fails
	// if somebody appends a newer version to the end of the array instead of
	// putting it at the front, which would silently make the loader prefer an
	// old compatibility shim over the client's own current interface.
	for ( size_t i = 1; i < kClientVersions.size(); i++ )
	{
		REQUIRE( kClientVersions[ i ].size() == kClientVersions[ i - 1 ].size() );
		REQUIRE( kClientVersions[ i ] < kClientVersions[ i - 1 ] );
	}
	for ( size_t i = 1; i < kFriendsVersions.size(); i++ )
	{
		REQUIRE( kFriendsVersions[ i ].size() == kFriendsVersions[ i - 1 ].size() );
		REQUIRE( kFriendsVersions[ i ] < kFriendsVersions[ i - 1 ] );
	}

	// Closed, not open-ended: a version we have not vouched for is not in
	// here, and SteamFriends.cpp asks for nothing that is not in here.
	REQUIRE( kFriendsVersions.size() == 3 );
	REQUIRE( kFriendsVersions.back() == "SteamFriends016" );
}

// ===========================================================================
//  CGameID
// ===========================================================================
TEST_CASE( "a CGameID is unpacked as app id plus type, not as a raw number", "[steam_friends]" )
{
	// A plain Steam app: type 0, so the low 32 bits happen to equal the app id
	// -- which is why the feasibility probe's simpler `& 0xFFFFFFFF` agreed
	// with this everywhere it was run.
	REQUIRE( AppIdFromGameId( 730 ) == 730u );
	REQUIRE( GameIdType( 730 ) == kGameIdTypeApp );

	// A mod/shortcut: type 1 in bits 24..31. The naive read would call this
	// app 16'029'434 and produce a join URL for a completely unrelated game.
	const uint64_t ulMod = 252490ull | ( 1ull << 24 );
	REQUIRE( AppIdFromGameId( ulMod ) == 252490u );
	REQUIRE( GameIdType( ulMod ) != kGameIdTypeApp );

	// The mod id in the top 32 bits must not leak into either field.
	const uint64_t ulWithModId = 440ull | ( 0xDEADBEEFull << 32 );
	REQUIRE( AppIdFromGameId( ulWithModId ) == 440u );
	REQUIRE( GameIdType( ulWithModId ) == kGameIdTypeApp );
}

// ===========================================================================
//  The joinable predicate -- the whole filter
// ===========================================================================
TEST_CASE( "joinable means a lobby id, on a real Steam app, from a real friend", "[steam_friends]" )
{
	// The one true case.
	REQUIRE( IsJoinable( 730, 555, 101 ) );

	// The field the entire feature is built on.
	REQUIRE_FALSE( IsJoinable( 730, 0, 101 ) );

	// A non-Steam shortcut. It may well have a lobby id; it has no app id
	// Steam would accept in a steam:// URL.
	REQUIRE_FALSE( IsJoinable( 252490ull | ( 1ull << 24 ), 777, 102 ) );

	// A lobby with nothing behind it, and a row with nobody to join.
	REQUIRE_FALSE( IsJoinable( 0, 888, 103 ) );
	REQUIRE_FALSE( IsJoinable( 730, 555, 0 ) );
}

// PHASE 3: the predicate answers WHY, because the panel prints a reason beside
// every row it cannot join. The order of the checks is part of the contract --
// a friend in a non-Steam game with no lobby reads as "not a Steam game", the
// more specific and more useful of the two true statements.
TEST_CASE( "a row that cannot be joined carries the one reason why", "[steam_friends]" )
{
	REQUIRE( JoinabilityOf( 730, 555, 101 ) == Joinability::Yes );
	REQUIRE( JoinabilityOf( 730, 0,   101 ) == Joinability::NoLobby );
	REQUIRE( JoinabilityOf( 252490ull | ( 1ull << 24 ), 777, 102 ) == Joinability::NotASteamApp );
	REQUIRE( JoinabilityOf( 252490ull | ( 1ull << 24 ), 0,   102 ) == Joinability::NotASteamApp );
	REQUIRE( JoinabilityOf( 0,   888, 103 ) == Joinability::NoApp );
	REQUIRE( JoinabilityOf( 730, 555, 0 )   == Joinability::NoSteamId );

	// Every reason is a sentence fragment the panel can print, and the
	// joinable case is EMPTY so a caller can print it unconditionally.
	REQUIRE( JoinabilityText( Joinability::Yes ).empty() );
	for ( Joinability e : { Joinability::NoLobby, Joinability::NotASteamApp,
	                        Joinability::NoApp, Joinability::NoSteamId } )
		REQUIRE_FALSE( JoinabilityText( e ).empty() );
}

// ===========================================================================
//  The status line
// ===========================================================================
// The list carries two numbers now (in a game, joinable) and the empty states
// have to be TELLABLE APART by a user -- "nobody is playing" and "everybody is
// playing something you cannot join" are different facts and must not share a
// sentence.
TEST_CASE( "the status line tells the empty states apart", "[steam_friends]" )
{
	REQUIRE( StatusLine( 0, 0 ) == "nobody's in a game right now." );
	REQUIRE( StatusLine( 3, 0 ) == "3 friends in a game, none you can join." );
	REQUIRE( StatusLine( 1, 0 ) == "1 friend in a game, none you can join." );
	REQUIRE( StatusLine( 1, 1 ) == "1 friend in a game, all joinable." );
	REQUIRE( StatusLine( 5, 2 ) == "5 friends in a game, 2 you can join." );

	// No two of them are the same string, which is the property that matters.
	const std::vector<std::string> v = {
		StatusLine( 0, 0 ), StatusLine( 3, 0 ), StatusLine( 1, 1 ), StatusLine( 5, 2 ) };
	for ( size_t i = 0; i < v.size(); i++ )
		for ( size_t j = i + 1; j < v.size(); j++ )
			REQUIRE( v[ i ] != v[ j ] );
}

// ===========================================================================
//  The game's name
// ===========================================================================
// ISteamFriends gives an app id and no name, so the panel reads Steam's own
// appmanifest_<appid>.acf. These are the two minimal readers that do it.
TEST_CASE( "a game's name is read out of its appmanifest", "[steam_friends]" )
{
	const std::string sAcf =
		"\"AppState\"\n{\n\t\"appid\"\t\t\"730\"\n\t\"Universe\"\t\t\"1\"\n"
		"\t\"name\"\t\t\"Counter-Strike 2\"\n\t\"StateFlags\"\t\t\"4\"\n}\n";
	REQUIRE( AppNameFromManifest( sAcf ) == "Counter-Strike 2" );

	// A file that is not a manifest, an empty file and a truncated one all
	// answer "" rather than a fragment -- GameLabel() then prints the app id,
	// which is always true.
	REQUIRE( AppNameFromManifest( "" ).empty() );
	REQUIRE( AppNameFromManifest( "\"AppState\"\n{\n\t\"appid\"\t\"730\"\n}" ).empty() );
	REQUIRE( AppNameFromManifest( "\"name\"\t\"unterminated" ).empty() );

	// A key that merely CONTAINS the name is not the name.
	REQUIRE( AppNameFromManifest( "\"nameless\"\t\"no\"\n\t\"name\"\t\"yes\"" ) == "yes" );
}

TEST_CASE( "the extra Steam libraries are read out of libraryfolders.vdf", "[steam_friends]" )
{
	const std::string sVdf =
		"\"libraryfolders\"\n{\n"
		"\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/mo/.steam/steam\"\n\t}\n"
		"\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/mnt/games/SteamLibrary\"\n\t}\n}\n";
	const std::vector<std::string> v = LibraryPathsFromVdf( sVdf );
	REQUIRE( v.size() == 2 );
	REQUIRE( v[ 0 ] == "/home/mo/.steam/steam" );
	REQUIRE( v[ 1 ] == "/mnt/games/SteamLibrary" );

	REQUIRE( LibraryPathsFromVdf( "" ).empty() );
	REQUIRE( LibraryPathsFromVdf( "\"path\"\t\"\"" ).empty() );   // an empty path is not a library
}

TEST_CASE( "a game with no manifest still gets a label", "[steam_friends]" )
{
	REQUIRE( GameLabel( 730, "Counter-Strike 2" ) == "Counter-Strike 2" );
	REQUIRE( GameLabel( 730, "" ) == "App 730" );
	REQUIRE( GameLabel( 0, "" ) == "a game Steam has no id for" );
	// Never empty: the panel draws this into a column and a blank would read
	// as a bug rather than as an unknown.
	REQUIRE_FALSE( GameLabel( 12345, "" ).empty() );
}

// ===========================================================================
//  The URL
// ===========================================================================
TEST_CASE( "the join URL is steam://joinlobby/appid/lobby/steamid", "[steam_friends]" )
{
	Friend f;
	f.sPersona  = "someone";
	f.uAppId    = 730;
	f.ulLobbyId = 109775241234567890ull;
	f.ulSteamId = 76561198000000000ull;

	std::string sUrl, sWhy;
	REQUIRE( BuildJoinUrl( f, &sUrl, &sWhy ) );
	REQUIRE( sWhy.empty() );
	REQUIRE( sUrl == "steam://joinlobby/730/109775241234567890/76561198000000000" );

	// 64-bit ids must survive intact -- a lobby id truncated to 32 bits is a
	// join request for somebody else's lobby.
	Friend big;
	big.uAppId    = 4294967295u;
	big.ulLobbyId = 18446744073709551615ull;
	big.ulSteamId = 18446744073709551614ull;
	REQUIRE( BuildJoinUrl( big, &sUrl, &sWhy ) );
	REQUIRE( sUrl == "steam://joinlobby/4294967295/18446744073709551615/18446744073709551614" );
}

TEST_CASE( "a lobby id of 0 is refused, not formatted", "[steam_friends]" )
{
	Friend f;
	f.uAppId    = 730;
	f.ulLobbyId = 0;
	f.ulSteamId = 76561198000000000ull;

	std::string sUrl = "not cleared";
	std::string sWhy;
	REQUIRE_FALSE( BuildJoinUrl( f, &sUrl, &sWhy ) );
	REQUIRE_FALSE( sWhy.empty() );
	// The out parameter must not be left holding a half-built URL a caller
	// might then fire.
	REQUIRE( sUrl.empty() );

	// The same for the other two numeric guards.
	f.ulLobbyId = 555;
	f.uAppId    = 0;
	REQUIRE_FALSE( BuildJoinUrl( f, &sUrl, &sWhy ) );
	f.uAppId    = 730;
	f.ulSteamId = 0;
	REQUIRE_FALSE( BuildJoinUrl( f, &sUrl, &sWhy ) );
}

// This is the case the whole struct-shaped signature exists for.
//
// A persona name is the ONE field in a Friend that a remote person
// chooses, and it is the only string in the row. If it could reach the command
// line, a friend could rename themselves into extra arguments for `steam` --
// so the design is that BuildJoinUrl() reads three INTEGERS and never looks at
// the name at all. That is a property of the types rather than of an escaping
// routine somebody could forget to call, and this is what pins it.
TEST_CASE( "a hostile persona name cannot influence the join command", "[steam_friends]" )
{
	Friend base;
	base.sPersona  = "";
	base.uAppId    = 730;
	base.ulLobbyId = 555;
	base.ulSteamId = 76561198000000000ull;

	std::string sExpected, sWhy;
	REQUIRE( BuildJoinUrl( base, &sExpected, &sWhy ) );

	const char *kHostileNames[] = {
		"evil\";rm -rf $HOME;\"",
		"x/../../../../etc/passwd",
		"a b c --kiosk --no-sandbox",
		"steam://joinlobby/440/1/2",
		"\n\rsteam://run/4000",
		"'; steam steam://uninstall/730 ;'",
		"%s%s%s%n",
		"../../730/999/1",
	};

	for ( const char *pszName : kHostileNames )
	{
		Friend f = base;
		f.sPersona = pszName;

		std::string sUrl;
		REQUIRE( BuildJoinUrl( f, &sUrl, &sWhy ) );

		// Byte-identical to the empty-name build: the name contributed nothing.
		REQUIRE( sUrl == sExpected );

		// And the argv it becomes is two arguments -- the program and one URL
		// -- however long or ugly the name was.
		const std::vector<std::string> vecArgv = BuildJoinArgv( sUrl );
		REQUIRE( vecArgv.size() == 2 );
		REQUIRE( vecArgv[ 0 ] == "steam" );
		REQUIRE( vecArgv[ 1 ] == sExpected );
		REQUIRE( vecArgv[ 1 ].find( std::string( pszName ) ) == std::string::npos );
	}
}

TEST_CASE( "the join argv is the program and one URL, and nothing is split", "[steam_friends]" )
{
	// Not a user-editable string, so there is no splitting step for anything
	// to hide inside.
	const std::vector<std::string> vecArgv = BuildJoinArgv( "steam://joinlobby/730/555/1" );
	REQUIRE( vecArgv.size() == 2 );
	REQUIRE( vecArgv[ 0 ] == "steam" );
	REQUIRE( vecArgv[ 1 ] == "steam://joinlobby/730/555/1" );
}

// ===========================================================================
//  The loader's failure modes, in a forked child
// ===========================================================================
namespace
{
	std::string ExeDir()
	{
		char szBuf[ PATH_MAX ] = {};
		const ssize_t n = readlink( "/proc/self/exe", szBuf, sizeof( szBuf ) - 1 );
		if ( n <= 0 )
			return {};
		std::string s( szBuf, (size_t)n );
		const size_t nSlash = s.rfind( '/' );
		return nSlash == std::string::npos ? std::string( "." ) : s.substr( 0, nSlash );
	}

	struct Probe
	{
		int         nRows = -1;      // the child's exit code: rows Snapshot() returned
		std::string sStderr;
	};

	// Runs Snapshot() nTimes in a child process whose loader has never run,
	// with the given library path and stub mode, and hands back what the child
	// found and everything it logged.
	//
	// nTimes > 1 is how "exactly ONE log line" is tested: a compositor polls
	// this, so a failure that logs once per poll would drown the log.
	Probe RunProbe( const std::string &sLibPath, const char *pszMode, int nTimes = 3 )
	{
		int nPipe[ 2 ] = { -1, -1 };
		REQUIRE( pipe( nPipe ) == 0 );

		const pid_t nChild = fork();
		REQUIRE( nChild >= 0 );

		if ( nChild == 0 )
		{
			close( nPipe[ 0 ] );
			dup2( nPipe[ 1 ], STDERR_FILENO );
			close( nPipe[ 1 ] );

			setenv( "GS_RITZ_STEAMCLIENT", sLibPath.c_str(), 1 );
			// Keep the game-name lookup off the developer's own Steam
			// install: an empty override means "look in this directory",
			// which has no appmanifest files, so every row falls back to
			// "App <id>" deterministically.
			setenv( "GS_RITZ_STEAMAPPS", "/nonexistent/steamapps", 1 );
			// And nowhere near a real name cache: these cases are about the
			// loader, and a lookup would be a network call in a unit test.
			setenv( "GS_RITZ_APPNAME_CACHE", "/nonexistent/appnames.json", 1 );
			if ( pszMode )
				setenv( "GS_RITZ_TEST_STEAMCLIENT_MODE", pszMode, 1 );
			else
				unsetenv( "GS_RITZ_TEST_STEAMCLIENT_MODE" );

			size_t nLast = 0;
			bool bStable = true;
			for ( int i = 0; i < nTimes; i++ )
			{
				const std::vector<Friend> vec = Snapshot();
				if ( i > 0 && vec.size() != nLast )
					bStable = false;
				nLast = vec.size();

				size_t nJoinable = 0;
				for ( const Friend &f : vec )
					if ( f.CanJoin() )
						nJoinable++;
				fprintf( stderr, "PROBE-JOINABLE: %zu\n", nJoinable );

				// The success path's contents, checked here because only the
				// child ever has them. FIVE rows -- every fake friend who is
				// in a game, joinable or not (phase 3's change) -- in list
				// order, with the app ids, lobby ids, SteamIDs and the exact
				// non-joinable reason each of them carries.
				// IN THE ORDER Snapshot() SORTS THEM (2026-09-09), which is not the
				// order the stub declares them in: the two joinable rows first,
				// alphabetically, then the three that are merely in a game, also
				// alphabetically. That the two orders DIFFER is what makes this a
				// check on the sort as well as on the contents.
				constexpr uint64_t kBase = 76561197960265728ull;
				if ( vec.size() == 5 &&
				     vec[ 0 ].CanJoin() &&
				     vec[ 0 ].uAppId == 440 && vec[ 0 ].ulLobbyId == 999 && vec[ 0 ].ulSteamId == kBase + 104 &&
				     vec[ 0 ].sPersona.find( "rm -rf" ) != std::string::npos &&
				     vec[ 0 ].sGame == "App 440" &&
				     vec[ 1 ].CanJoin() &&
				     vec[ 1 ].uAppId == 730 && vec[ 1 ].ulLobbyId == 555 && vec[ 1 ].ulSteamId == kBase + 101 &&
				     vec[ 2 ].eJoinable == Joinability::NoLobby &&
				     vec[ 2 ].uAppId == 730 && vec[ 2 ].ulSteamId == kBase + 100 &&
				     vec[ 3 ].eJoinable == Joinability::NoApp &&
				     vec[ 4 ].eJoinable == Joinability::NotASteamApp )
				{
					fprintf( stderr, "PROBE-CONTENTS-OK\n" );
				}
			}
			if ( bStable )
				fprintf( stderr, "PROBE-STABLE\n" );
			fprintf( stderr, "PROBE-STATUS: %s\n", StatusText().c_str() );
			fflush( stderr );

			// _exit, not exit: Catch2's own atexit machinery must not run in
			// the child and report a second, bogus test result.
			_exit( (int)( nLast & 0x7F ) );
		}

		close( nPipe[ 1 ] );
		std::string sOut;
		char szBuf[ 512 ];
		for ( ssize_t n; ( n = read( nPipe[ 0 ], szBuf, sizeof( szBuf ) ) ) > 0; )
			sOut.append( szBuf, (size_t)n );
		close( nPipe[ 0 ] );

		int nStatus = 0;
		waitpid( nChild, &nStatus, 0 );

		Probe p;
		p.sStderr = std::move( sOut );
		// A child that died on a signal is the one outcome this feature must
		// never produce, so it is reported as such rather than as a count.
		p.nRows = WIFEXITED( nStatus ) ? WEXITSTATUS( nStatus ) : -1;
		return p;
	}

	size_t CountOf( const std::string &sHaystack, const std::string &sNeedle )
	{
		size_t n = 0;
		for ( size_t i = sHaystack.find( sNeedle ); i != std::string::npos;
		      i = sHaystack.find( sNeedle, i + sNeedle.size() ) )
			n++;
		return n;
	}

	std::string StubPath()      { return ExeDir() + "/libsteamclient_stub.so"; }
	std::string NotSteamPath()  { return ExeDir() + "/libnotsteam_stub.so"; }
}

TEST_CASE( "the test stubs were built next to this binary", "[steam_friends]" )
{
	// If this fails, every case below is testing nothing -- so it fails here,
	// by name, rather than as six confusing empty results.
	REQUIRE( access( StubPath().c_str(), R_OK ) == 0 );
	REQUIRE( access( NotSteamPath().c_str(), R_OK ) == 0 );
}

TEST_CASE( "a missing library is an empty list and one log line", "[steam_friends]" )
{
	const Probe p = RunProbe( "/nonexistent/definitely/not/steamclient.so", nullptr );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "Steam isn't installed here." ) != std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-STABLE" ) != std::string::npos );
}

TEST_CASE( "a library that is not Steam's client is refused before any vtable call", "[steam_friends]" )
{
	// libnotsteam_stub.so exports a CreateInterface -- plenty of libraries do
	// -- but none of the Steam_* fingerprint symbols. Without the fingerprint
	// check this run would cast a stranger's pointer to a vtable and call
	// through it; with it, the run is one log line and an empty list.
	const Probe p = RunProbe( NotSteamPath(), nullptr );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "isn't the one we know" ) != std::string::npos );
}

TEST_CASE( "an interface version we do not know disables the feature", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "noiface" );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "newer than this build knows" ) != std::string::npos );
}

TEST_CASE( "an unknown ISteamFriends version disables the feature too", "[steam_friends]" )
{
	// The second half of the same guard: the client answered, its friends
	// interface did not.
	const Probe p = RunProbe( StubPath(), "nofriends" );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "newer than this build knows" ) != std::string::npos );
}

TEST_CASE( "Steam not running is an empty list, said in those words", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "nopipe" );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "Steam isn't running." ) != std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-STATUS: Steam isn't running." ) != std::string::npos );
}

TEST_CASE( "a signed-out client is an empty list, said in those words", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "nouser" );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "Steam is signed out." ) != std::string::npos );
}

// THE REGRESSION THIS WHOLE SESSION EXISTS FOR.
//
// The vtable layout src/SteamFriends.cpp declares was originally taken from the
// published ISteamFriends order, and against the real client that order is one
// slot out. Every stub-based case above passed anyway -- a fake built from the
// same wrong declaration agrees with itself -- and the only visible symptom was
// a friends list that was permanently empty, which is indistinguishable from
// "nobody is in a joinable game".
//
// So the shipping code now range-checks the first SteamID it gets back, and
// this is the case that keeps that check honest: a slot returning numbers that
// are not SteamIDs must produce a NAMED failure, not silence.
TEST_CASE( "ids that are not SteamIDs are reported, not quietly dropped", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "badids" );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "isn't laid out the way this build expects" ) != std::string::npos );
}

// PHASE 3'S CHANGE, PINNED. Snapshot() used to return only the joinable rows;
// it now returns everyone in a game, each carrying the one reason it cannot be
// joined. A regression to the old behaviour shows up here as three missing
// rows -- see SteamFriendsCmd.h's Joinability comment for why that would be
// the wrong answer even though it is the smaller list.
TEST_CASE( "the success path lists everyone in a game and marks the joinable ones", "[steam_friends]" )
{
	// Six fake friends: five in a game, of which exactly two are joinable --
	// the other three are each rejected by a different clause of
	// JoinabilityOf(), and the sixth is not in a game at all and must NOT
	// appear.
	const Probe p = RunProbe( StubPath(), "ok" );
	REQUIRE( p.nRows == 5 );
	REQUIRE( p.sStderr.find( "PROBE-JOINABLE: 2" ) != std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-CONTENTS-OK" ) != std::string::npos );
	REQUIRE( p.sStderr.find( "the join list is unavailable" ) == std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-STATUS: 5 friends in a game, 2 you can join." ) != std::string::npos );
}

// ===========================================================================
//  The poller -- the proof that a slow Steam cannot stall a frame
// ===========================================================================
// THIS IS THE CASE THE WHOLE BACKGROUND THREAD EXISTS FOR, so it is tested
// against a stub that is DELIBERATELY SLOW (tests/steamclient_stub.cpp's
// "slow" mode sleeps 1.5 s inside CreateSteamPipe, which is the first call of
// every snapshot). A panel that called Snapshot() directly would block for
// that long, once per poll, on the compositor's own thread.
//
// The measurement is the WORST single CurrentView() call while a poll is in
// flight, in microseconds. It is compared against a threshold two orders of
// magnitude below the stub's sleep, so this fails on a genuinely blocking
// implementation and passes on a loaded machine.
static Probe RunPollerProbe( const std::string &sLibPath, const char *pszMode )
{
	int nPipe[ 2 ] = { -1, -1 };
	REQUIRE( pipe( nPipe ) == 0 );

	const pid_t nChild = fork();
	REQUIRE( nChild >= 0 );

	if ( nChild == 0 )
	{
		close( nPipe[ 0 ] );
		dup2( nPipe[ 1 ], STDERR_FILENO );
		close( nPipe[ 1 ] );

		setenv( "GS_RITZ_STEAMCLIENT", sLibPath.c_str(), 1 );
		setenv( "GS_RITZ_STEAMAPPS", "/nonexistent/steamapps", 1 );
		setenv( "GS_RITZ_APPNAME_CACHE", "/nonexistent/appnames.json", 1 );
		setenv( "GS_RITZ_TEST_STEAMCLIENT_MODE", pszMode, 1 );

		// The first call arms the poller and starts the worker; every call
		// after it is racing a snapshot that is asleep inside the stub.
		long nWorstUs = 0;
		bool bPolled = false;
		for ( int i = 0; i < 400; i++ )   // ~2 s of 5 ms frames, past the stub's 1.5 s sleep
		{
			const auto t0 = std::chrono::steady_clock::now();
			const View v = CurrentView();
			const auto t1 = std::chrono::steady_clock::now();

			const long nUs = (long)std::chrono::duration_cast<std::chrono::microseconds>( t1 - t0 ).count();
			if ( nUs > nWorstUs )
				nWorstUs = nUs;
			if ( v.bPolled )
				bPolled = true;

			// A status is available on the very first call, before any poll
			// has finished -- an empty panel with nothing to say is the
			// failure this replaces.
			if ( v.sStatus.empty() )
				fprintf( stderr, "PROBE-EMPTY-STATUS\n" );

			struct timespec ts = { 0, 5 * 1000 * 1000 };   // 5 ms, ~a frame
			nanosleep( &ts, nullptr );
		}

		fprintf( stderr, "PROBE-WORST-VIEW-US: %ld\n", nWorstUs );
		fprintf( stderr, "PROBE-POLL-FINISHED: %d\n", bPolled ? 1 : 0 );

		// Joining the worker is what proves Shutdown() terminates at all --
		// a poller that could not be stopped would hang this child and the
		// case would fail on the read below rather than silently pass.
		Shutdown();
		fprintf( stderr, "PROBE-SHUTDOWN-OK\n" );
		fflush( stderr );
		_exit( 0 );
	}

	close( nPipe[ 1 ] );
	std::string sOut;
	char szBuf[ 512 ];
	for ( ssize_t n; ( n = read( nPipe[ 0 ], szBuf, sizeof( szBuf ) ) ) > 0; )
		sOut.append( szBuf, (size_t)n );
	close( nPipe[ 0 ] );

	int nStatus = 0;
	waitpid( nChild, &nStatus, 0 );

	Probe p;
	p.sStderr = std::move( sOut );
	p.nRows = WIFEXITED( nStatus ) ? WEXITSTATUS( nStatus ) : -1;
	return p;
}

static long WorstViewUs( const std::string &sStderr )
{
	const std::string sKey = "PROBE-WORST-VIEW-US: ";
	const size_t nAt = sStderr.find( sKey );
	if ( nAt == std::string::npos )
		return -1;
	return strtol( sStderr.c_str() + nAt + sKey.size(), nullptr, 10 );
}

TEST_CASE( "a Steam client that takes seconds to answer never delays a reader", "[steam_friends]" )
{
	const Probe p = RunPollerProbe( StubPath(), "slow" );
	REQUIRE( p.nRows == 0 );                       // the child exited cleanly
	REQUIRE( p.sStderr.find( "PROBE-SHUTDOWN-OK" ) != std::string::npos );

	const long nWorstUs = WorstViewUs( p.sStderr );
	REQUIRE( nWorstUs >= 0 );

	// 15 ms: two orders of magnitude below the stub's 1.5 s sleep, and still
	// generous enough for a scheduler hiccup on a machine running a build.
	// A CurrentView() that waited for the snapshot would measure ~1 500 000.
	REQUIRE( nWorstUs < 15000 );

	// And the poll it was racing really did happen -- otherwise the fast
	// answers above would only prove that nothing was ever asked.
	REQUIRE( p.sStderr.find( "PROBE-POLL-FINISHED: 1" ) != std::string::npos );

	// Every reader saw a sentence, including the ones before the first poll
	// landed.
	REQUIRE( p.sStderr.find( "PROBE-EMPTY-STATUS" ) == std::string::npos );
}

TEST_CASE( "the poller stops cleanly when it was never armed", "[steam_friends]" )
{
	// Shutdown() with no worker started is a no-op, and gamescope's exit path
	// calls it unconditionally -- including in a run that never opened the
	// friends panel, which is most of them.
	Shutdown();
	Shutdown();
	REQUIRE( true );
}

// ===========================================================================
//  The order the list is drawn in (2026-09-09)
// ===========================================================================
// "Joinable players should be sorted towards the top. And topmost should be
// invites." Three bands, alphabetical inside a band, and a total order so an
// unchanged list never reshuffles itself.
namespace
{
	Friend MakeFriend( const char *pszPersona, Joinability e, uint64_t ulSteamId, bool bInvited = false )
	{
		Friend f;
		f.sPersona  = pszPersona;
		f.uAppId    = 730;
		f.ulLobbyId = ( e == Joinability::Yes ) ? 555 : 0;
		f.ulSteamId = ulSteamId;
		f.eJoinable = e;
		f.bInvited  = bInvited;
		return f;
	}

	std::vector<std::string> PersonasInOrder( std::vector<Friend> vec )
	{
		std::sort( vec.begin(), vec.end(), FriendOrderLess );
		std::vector<std::string> vecOut;
		for ( const Friend &f : vec )
			vecOut.push_back( f.sPersona );
		return vecOut;
	}
}

TEST_CASE( "the list is ordered invites, then joinable, then everyone else", "[steam_friends]" )
{
	const std::vector<Friend> vec = {
		MakeFriend( "zoe",    Joinability::NoLobby,      201 ),
		MakeFriend( "alice",  Joinability::Yes,          202 ),
		MakeFriend( "bob",    Joinability::NotASteamApp, 203 ),
		MakeFriend( "yannis", Joinability::Yes,          204 ),
		MakeFriend( "carol",  Joinability::NoLobby,      205, /* bInvited */ true ),
	};

	REQUIRE( PersonasInOrder( vec ) ==
		std::vector<std::string>{ "carol", "alice", "yannis", "bob", "zoe" } );

	// The bands themselves, named rather than inferred from the sequence.
	REQUIRE( GroupOf( vec[ 4 ] ) == FriendGroup::Invite );
	REQUIRE( GroupOf( vec[ 1 ] ) == FriendGroup::Joinable );
	REQUIRE( GroupOf( vec[ 0 ] ) == FriendGroup::InGame );

	// An invite outranks a joinable row even when the joinable row would sort
	// first alphabetically -- the band is checked before the name, always.
	REQUIRE( FriendOrderLess( MakeFriend( "zzz", Joinability::NoLobby, 1, true ),
	                          MakeFriend( "aaa", Joinability::Yes, 2 ) ) );
}

TEST_CASE( "the ordering copes with empty and single-band lists", "[steam_friends]" )
{
	// The two cases a comparator is most often wrong about.
	std::vector<Friend> vecEmpty;
	std::sort( vecEmpty.begin(), vecEmpty.end(), FriendOrderLess );
	REQUIRE( vecEmpty.empty() );

	const std::vector<Friend> vecOne = { MakeFriend( "solo", Joinability::Yes, 301 ) };
	REQUIRE( PersonasInOrder( vecOne ) == std::vector<std::string>{ "solo" } );

	// All joinable: purely alphabetical.
	const std::vector<Friend> vecAllJoinable = {
		MakeFriend( "delta", Joinability::Yes, 302 ),
		MakeFriend( "alpha", Joinability::Yes, 303 ),
		MakeFriend( "charlie", Joinability::Yes, 304 ),
	};
	REQUIRE( PersonasInOrder( vecAllJoinable ) ==
		std::vector<std::string>{ "alpha", "charlie", "delta" } );

	// All in a game and none joinable: the same, with nothing promoted.
	const std::vector<Friend> vecNoneJoinable = {
		MakeFriend( "delta", Joinability::NoLobby, 305 ),
		MakeFriend( "alpha", Joinability::NoApp,   306 ),
		MakeFriend( "charlie", Joinability::NoSteamId, 307 ),
	};
	REQUIRE( PersonasInOrder( vecNoneJoinable ) ==
		std::vector<std::string>{ "alpha", "charlie", "delta" } );

	// All invites, which is the band nothing produces today -- it still has to
	// order sanely the day something does.
	const std::vector<Friend> vecAllInvites = {
		MakeFriend( "delta", Joinability::NoLobby, 308, true ),
		MakeFriend( "alpha", Joinability::Yes,     309, true ),
	};
	REQUIRE( PersonasInOrder( vecAllInvites ) == std::vector<std::string>{ "alpha", "delta" } );
}

TEST_CASE( "the order is case-insensitive, total, and stable across polls", "[steam_friends]" )
{
	// Case: "Zoe" must not sort before "alice" because 'Z' < 'a' in ASCII.
	const std::vector<Friend> vecCase = {
		MakeFriend( "Zoe",   Joinability::Yes, 401 ),
		MakeFriend( "alice", Joinability::Yes, 402 ),
		MakeFriend( "BOB",   Joinability::Yes, 403 ),
	};
	REQUIRE( PersonasInOrder( vecCase ) == std::vector<std::string>{ "alice", "BOB", "Zoe" } );

	// TOTAL: two friends can share a display name, and without the SteamID
	// tiebreak the sort would be free to swap them between polls -- a list
	// that flickers while nothing has changed. Neither may compare "less"
	// than the other in both directions, and one of them must.
	const Friend a = MakeFriend( "same name", Joinability::Yes, 500 );
	const Friend b = MakeFriend( "same name", Joinability::Yes, 501 );
	REQUIRE( FriendOrderLess( a, b ) );
	REQUIRE_FALSE( FriendOrderLess( b, a ) );

	// Irreflexive, which is what std::sort actually requires of it.
	REQUIRE_FALSE( FriendOrderLess( a, a ) );

	// STABILITY, measured rather than argued: sorting the same rows from three
	// different starting permutations must give three identical sequences.
	std::vector<Friend> vec = {
		MakeFriend( "same name", Joinability::Yes, 501 ),
		MakeFriend( "same name", Joinability::Yes, 500 ),
		MakeFriend( "other",     Joinability::NoLobby, 502 ),
		MakeFriend( "another",   Joinability::Yes, 503 ),
	};
	const std::vector<std::string> vecFirst = PersonasInOrder( vec );
	std::reverse( vec.begin(), vec.end() );
	REQUIRE( PersonasInOrder( vec ) == vecFirst );
	std::rotate( vec.begin(), vec.begin() + 2, vec.end() );
	REQUIRE( PersonasInOrder( vec ) == vecFirst );
}

TEST_CASE( "a persona name with no letters still has a defined place", "[steam_friends]" )
{
	// Personas are arbitrary UTF-8 and are frequently emoji, punctuation or
	// empty. None of that may make the comparator inconsistent.
	const std::vector<Friend> vec = {
		MakeFriend( "",            Joinability::Yes, 601 ),
		MakeFriend( "\xf0\x9f\x92\x80", Joinability::Yes, 602 ),   // an emoji
		MakeFriend( "!!!",         Joinability::Yes, 603 ),
		MakeFriend( "aaa",         Joinability::Yes, 604 ),
	};
	const std::vector<std::string> vecOrder = PersonasInOrder( vec );
	REQUIRE( vecOrder.size() == 4 );
	REQUIRE( vecOrder[ 0 ].empty() );          // "" is less than everything
	REQUIRE( vecOrder.back() == "\xf0\x9f\x92\x80" );   // 0xf0 is above ASCII
}

// ===========================================================================
//  The game-name cache (2026-09-09)
// ===========================================================================
// src/SteamAppNames.h. A cache is a thing the program must work WITHOUT, so
// every case below is really the same question asked five ways: does a bad
// file, a bad answer or a full cache ever produce anything but "start again,
// quietly"?
TEST_CASE( "the name cache round-trips through its file format", "[steam_friends]" )
{
	AppNameMap map;
	map[ 730 ]    = AppName{ "Counter-Strike 2", 1757000000 };
	map[ 252490 ] = AppName{ "Rust", 1757000001 };
	// The authoritative negative: Steam was asked and has no name for this id.
	// It has to survive the round trip, or the id would be asked about again
	// on every single poll forever.
	map[ 99999999 ] = AppName{ "", 1757000002 };

	const std::string sJson = SerialiseAppNameCache( map );
	const AppNameMap back = ParseAppNameCache( sJson );

	REQUIRE( back.size() == 3 );
	REQUIRE( back.at( 730 ).sName == "Counter-Strike 2" );
	REQUIRE( back.at( 730 ).nSeen == 1757000000 );
	REQUIRE( back.at( 252490 ).sName == "Rust" );
	REQUIRE( back.count( 99999999 ) == 1 );
	REQUIRE( back.at( 99999999 ).sName.empty() );

	// It really is JSON, and it really is small.
	REQUIRE( sJson.front() == '{' );
	REQUIRE( sJson.find( "\"Counter-Strike 2\"" ) != std::string::npos );
	REQUIRE( sJson.size() < 400 );
}

TEST_CASE( "a corrupt or truncated cache file starts empty rather than failing", "[steam_friends]" )
{
	const std::string sGood = SerialiseAppNameCache(
		AppNameMap{ { 730, AppName{ "Counter-Strike 2", 1757000000 } } } );

	// Every one of these is a real way a cache file goes wrong on a machine
	// that lost power, ran out of disk, or had the file edited by hand.
	REQUIRE( ParseAppNameCache( "" ).empty() );
	REQUIRE( ParseAppNameCache( "not json at all" ).empty() );
	REQUIRE( ParseAppNameCache( "{" ).empty() );
	REQUIRE( ParseAppNameCache( "[1,2,3]" ).empty() );
	REQUIRE( ParseAppNameCache( std::string( 4096, '\0' ) ).empty() );
	// Truncated halfway: the single most likely corruption, and the one a
	// hand-rolled reader would half-accept.
	REQUIRE( ParseAppNameCache( sGood.substr( 0, sGood.size() / 2 ) ).empty() );
	// A future format must not be half-read by this build.
	REQUIRE( ParseAppNameCache( "{\"version\":99,\"apps\":{\"730\":{\"name\":\"x\"}}}" ).empty() );
	REQUIRE( ParseAppNameCache( "{\"apps\":{\"730\":{\"name\":\"x\"}}}" ).empty() );
	// A file that is absurdly large is refused before it is allocated.
	REQUIRE( ParseAppNameCache( std::string( kAppNameCacheMaxBytes + 1, 'x' ) ).empty() );

	// And one BAD ENTRY inside an otherwise good file loses that entry only.
	const AppNameMap mixed = ParseAppNameCache(
		"{\"version\":1,\"apps\":{"
		"\"730\":{\"name\":\"Counter-Strike 2\",\"seen\":5},"
		"\"notanumber\":{\"name\":\"nope\"},"
		"\"0\":{\"name\":\"also nope\"},"
		"\"440\":\"not an object\","
		"\"252490\":{\"name\":42}}}" );
	REQUIRE( mixed.size() == 2 );
	REQUIRE( mixed.at( 730 ).sName == "Counter-Strike 2" );
	REQUIRE( mixed.at( 252490 ).sName.empty() );   // the wrong-typed name is dropped, the row survives
	REQUIRE( mixed.count( 440 ) == 0 );
}

TEST_CASE( "the name cache cannot grow without bound", "[steam_friends]" )
{
	// Fill it well past the cap, with an age that says which entries matter.
	AppNameMap map;
	for ( uint32_t i = 1; i <= kAppNameCacheMax * 2; i++ )
		map[ i ] = AppName{ "game " + std::to_string( i ), (int64_t)i };
	REQUIRE( map.size() == kAppNameCacheMax * 2 );

	TrimAppNameCache( map );
	REQUIRE( map.size() == kAppNameCacheMax );

	// It is an LRU, not a truncation: the NEWEST entries are the survivors.
	REQUIRE( map.count( kAppNameCacheMax * 2 ) == 1 );      // newest, kept
	REQUIRE( map.count( kAppNameCacheMax + 1 ) == 1 );      // the oldest survivor
	REQUIRE( map.count( kAppNameCacheMax ) == 0 );          // one past the cut
	REQUIRE( map.count( 1 ) == 0 );                         // oldest, gone

	// Idempotent, and a below-cap map is untouched.
	TrimAppNameCache( map );
	REQUIRE( map.size() == kAppNameCacheMax );
	AppNameMap small{ { 730, AppName{ "x", 1 } } };
	TrimAppNameCache( small );
	REQUIRE( small.size() == 1 );

	// A file that somehow grew past the bound is bounded on READ too, so a
	// hand-edited or older file cannot reintroduce unbounded growth.
	AppNameMap huge;
	for ( uint32_t i = 1; i <= kAppNameCacheMax * 2; i++ )
		huge[ i ] = AppName{ "g", (int64_t)i };
	REQUIRE( ParseAppNameCache( SerialiseAppNameCache( huge ) ).size() <= kAppNameCacheMax );

	// Deterministic: same input, same survivors, whatever the machine.
	AppNameMap tie1, tie2;
	for ( uint32_t i = 1; i <= kAppNameCacheMax + 10; i++ )
	{
		tie1[ i ] = AppName{ "g", 7 };   // every entry the SAME age
		tie2[ i ] = AppName{ "g", 7 };
	}
	TrimAppNameCache( tie1 );
	TrimAppNameCache( tie2 );
	REQUIRE( tie1.size() == kAppNameCacheMax );
	REQUIRE( tie1 == tie2 );
}

// ===========================================================================
//  The one URL this compositor ever fetches
// ===========================================================================
TEST_CASE( "the lookup URL carries app ids and nothing else", "[steam_friends]" )
{
	const std::string sUrl = BuildAppNamesUrl( { 730, 252490, 440 } );
	REQUIRE( sUrl ==
		"https://api.steampowered.com/ICommunityService/GetApps/v1/"
		"?appids%5B0%5D=730&appids%5B1%5D=252490&appids%5B2%5D=440" );

	// https, one host, no key, no auth parameter.
	REQUIRE( sUrl.rfind( "https://api.steampowered.com/", 0 ) == 0 );
	REQUIRE( sUrl.find( "key=" ) == std::string::npos );

	// EVERY BYTE AFTER THE '?' IS A DIGIT OR PUNCTUATION WE WROTE. This is the
	// property that makes "nothing about the user is sent" a fact about the
	// types rather than a promise: there is no string input to smuggle a
	// SteamID, a persona or a header through.
	const size_t nQ = sUrl.find( '?' );
	REQUIRE( nQ != std::string::npos );
	for ( size_t i = nQ + 1; i < sUrl.size(); i++ )
	{
		const char c = sUrl[ i ];
		const bool bAllowed = ( c >= '0' && c <= '9' ) || c == 'a' || c == 'p' || c == 'i' ||
		                      c == 'd' || c == 's' || c == '%' || c == 'B' || c == 'D' ||
		                      c == '=' || c == '&';
		REQUIRE( bAllowed );
	}

	// Brackets are percent-encoded, because curl treats a literal [] as a glob.
	REQUIRE( sUrl.find( '[' ) == std::string::npos );
	REQUIRE( sUrl.find( ']' ) == std::string::npos );

	// Nothing to ask about is no URL at all, so the caller cannot fetch by
	// accident.
	REQUIRE( BuildAppNamesUrl( {} ).empty() );
	REQUIRE( BuildAppNamesUrl( { 0 } ).empty() );

	// A batch is bounded, so one very long friends list cannot build an
	// unbounded URL.
	std::vector<uint32_t> vecMany;
	for ( uint32_t i = 1; i <= kAppNamesPerRequest * 4; i++ )
		vecMany.push_back( i );
	const std::string sMany = BuildAppNamesUrl( vecMany );
	REQUIRE( sMany.find( "appids%5B" + std::to_string( kAppNamesPerRequest - 1 ) + "%5D=" )
		!= std::string::npos );
	REQUIRE( sMany.find( "appids%5B" + std::to_string( kAppNamesPerRequest ) + "%5D=" )
		== std::string::npos );
}

TEST_CASE( "the fetch command line reads no curlrc, follows nothing and times out", "[steam_friends]" )
{
	const std::vector<std::string> vecArgv = BuildFetchArgv( "https://example.invalid/x", "/tmp/out" );

	auto Has = [ & ]( const char *psz )
	{
		return std::find( vecArgv.begin(), vecArgv.end(), std::string( psz ) ) != vecArgv.end();
	};

	REQUIRE( vecArgv.front() == "curl" );
	REQUIRE( Has( "-q" ) );              // ~/.curlrc could add a header, a proxy or a cookie jar
	REQUIRE( Has( "-f" ) );              // an error page must not be written into the cache
	REQUIRE( Has( "--proto" ) );
	REQUIRE( Has( "=https" ) );
	REQUIRE( Has( "--max-redirs" ) );
	REQUIRE( Has( "0" ) );
	REQUIRE( Has( "--max-time" ) );
	REQUIRE( Has( "--connect-timeout" ) );

	// Nothing that could attach the user's identity to the request.
	for ( const std::string &s : vecArgv )
	{
		REQUIRE( s != "-b" );
		REQUIRE( s != "--cookie" );
		REQUIRE( s != "-c" );
		REQUIRE( s != "--cookie-jar" );
		REQUIRE( s != "-n" );
		REQUIRE( s != "--netrc" );
		REQUIRE( s != "--netrc-optional" );
		REQUIRE( s != "-L" );
		REQUIRE( s != "--location" );
	}

	// The URL is last and behind a "--", so it can never be read as a flag.
	REQUIRE( vecArgv.back() == "https://example.invalid/x" );
	REQUIRE( vecArgv[ vecArgv.size() - 2 ] == "--" );
}

TEST_CASE( "Steam's answer is read, bounded, and its \"no name\" is authoritative", "[steam_friends]" )
{
	// The exact shape measured from the live endpoint on 2026-09-09, including
	// the bogus id, which comes back with an appid and NO name.
	const std::string sBody =
		"{\"response\":{\"apps\":["
		"{\"appid\":730,\"name\":\"Counter-Strike 2\",\"icon\":\"8dbc\"},"
		"{\"appid\":252490,\"name\":\"Rust\"},"
		"{\"appid\":99999999}]}}";

	const AppNameMap map = ParseAppNamesResponse( sBody, 1757000000 );
	REQUIRE( map.size() == 3 );
	REQUIRE( map.at( 730 ).sName == "Counter-Strike 2" );
	REQUIRE( map.at( 730 ).nSeen == 1757000000 );
	REQUIRE( map.at( 252490 ).sName == "Rust" );
	// Present, with an empty name: "Steam has no name for this", which is a
	// real answer worth caching -- as opposed to an id missing from the
	// response entirely, which is not an answer and must not appear at all.
	REQUIRE( map.count( 99999999 ) == 1 );
	REQUIRE( map.at( 99999999 ).sName.empty() );
	REQUIRE( map.count( 440 ) == 0 );

	// A failed or nonsense fetch resolves NOTHING, so the caller cannot mistake
	// it for "Steam says these games have no names".
	REQUIRE( ParseAppNamesResponse( "", 1 ).empty() );
	REQUIRE( ParseAppNamesResponse( "<html>404</html>", 1 ).empty() );
	REQUIRE( ParseAppNamesResponse( "{\"response\":{}}", 1 ).empty() );
	REQUIRE( ParseAppNamesResponse( "{\"response\":{\"apps\":\"nope\"}}", 1 ).empty() );

	// A NAME IS DRAWN STRAIGHT INTO A ROW, so it is bounded here rather than
	// trusted -- an endpoint that is compromised, proxied or simply confused
	// must not be able to hand the panel a kilobyte of control characters.
	const std::string sHostile =
		"{\"response\":{\"apps\":[{\"appid\":1,\"name\":\"" + std::string( 4000, 'x' ) +
		"\"},{\"appid\":2,\"name\":\"a\\nb\\tc\"}]}}";
	const AppNameMap hostile = ParseAppNamesResponse( sHostile, 1 );
	REQUIRE( hostile.at( 1 ).sName.size() == 128 );
	REQUIRE( hostile.at( 2 ).sName == "a b c" );
}

TEST_CASE( "the cache lives in a cache directory, never in the config", "[steam_friends]" )
{
	// Game names are not a setting: nothing here is the user's choice and
	// throwing the file away must cost nothing. The environment is restored
	// afterwards so no later case inherits it.
	const char *pszOldXdg   = getenv( "XDG_CACHE_HOME" );
	const char *pszOldForce = getenv( "GS_RITZ_APPNAME_CACHE" );
	const char *pszOldHome  = getenv( "HOME" );
	const std::string sOldXdg   = pszOldXdg   ? pszOldXdg   : "";
	const std::string sOldForce = pszOldForce ? pszOldForce : "";
	const std::string sOldHome  = pszOldHome  ? pszOldHome  : "";

	unsetenv( "GS_RITZ_APPNAME_CACHE" );
	setenv( "XDG_CACHE_HOME", "/tmp/gs-ritz-test-cache", 1 );
	REQUIRE( AppNameCachePath() == "/tmp/gs-ritz-test-cache/gamescope-ritz/appnames.json" );
	REQUIRE( AppNameCachePath().find( ".config" ) == std::string::npos );

	// No XDG_CACHE_HOME: the standard fallback, still under a cache directory.
	unsetenv( "XDG_CACHE_HOME" );
	setenv( "HOME", "/tmp/gs-ritz-test-home", 1 );
	REQUIRE( AppNameCachePath() == "/tmp/gs-ritz-test-home/.cache/gamescope-ritz/appnames.json" );

	// And the test lever wins over both, which is how every case in this file
	// stays away from a real home directory.
	setenv( "GS_RITZ_APPNAME_CACHE", "/tmp/whatever.json", 1 );
	REQUIRE( AppNameCachePath() == "/tmp/whatever.json" );

	if ( sOldXdg.empty() ) unsetenv( "XDG_CACHE_HOME" );
	else                   setenv( "XDG_CACHE_HOME", sOldXdg.c_str(), 1 );
	if ( sOldForce.empty() ) unsetenv( "GS_RITZ_APPNAME_CACHE" );
	else                     setenv( "GS_RITZ_APPNAME_CACHE", sOldForce.c_str(), 1 );
	if ( sOldHome.empty() ) unsetenv( "HOME" );
	else                    setenv( "HOME", sOldHome.c_str(), 1 );
}

// ===========================================================================
//  The network path, against a `curl` that records what it was asked
// ===========================================================================
// THE SWITCH BEING HONOURED IS PROVEN, NOT ASSERTED. A shim named `curl` goes
// on PATH ahead of the real one and appends its argv to a log; "no request was
// made" is then the ABSENCE OF A LINE IN A FILE, which is a measurement. An
// assertion inside our own code could only ever prove that our own code
// believes itself.
//
// The same shim is what makes the fetch's timing testable: a `curl` that
// sleeps for five seconds stands in for a blackholed endpoint, and the
// measurement is the worst CurrentView() call while one is in flight -- the
// same shape as the slow-Steam case above, because it is the same promise.
namespace
{
	struct Shim
	{
		std::string sDir;      // goes on the front of PATH
		std::string sLog;      // one line per invocation, or absent if never run
		std::string sCache;    // GS_RITZ_APPNAME_CACHE
	};

	std::string TempDir( const char *pszTag )
	{
		char szTemplate[ 256 ];
		snprintf( szTemplate, sizeof( szTemplate ), "/tmp/gs-ritz-friends-%s-XXXXXX", pszTag );
		const char *pszDir = mkdtemp( szTemplate );
		REQUIRE( pszDir != nullptr );
		return pszDir;
	}

	// pszBody: what the fake curl writes to its -o file. nullptr means "fail
	// like curl -f does on an HTTP error", which is the no-network case.
	Shim MakeCurlShim( const char *pszTag, const char *pszBody, int nSleepSec = 0 )
	{
		Shim shim;
		shim.sDir   = TempDir( pszTag );
		shim.sLog   = shim.sDir + "/calls.log";
		shim.sCache = shim.sDir + "/appnames.json";

		// The shim writes its whole argv, so the test can read back the URL
		// that would have gone out -- including proving it carries no id but
		// an app id.
		std::string sScript =
			"#!/bin/sh\n"
			"echo \"$@\" >> \"" + shim.sLog + "\"\n";
		if ( nSleepSec > 0 )
			sScript += "sleep " + std::to_string( nSleepSec ) + "\n";
		if ( pszBody )
		{
			// The -o argument is the second-to-last-but-two; find it rather
			// than counting, so the argv order stays free to change.
			sScript +=
				"out=\"\"\n"
				"prev=\"\"\n"
				"for a in \"$@\"; do\n"
				"  if [ \"$prev\" = \"-o\" ]; then out=\"$a\"; fi\n"
				"  prev=\"$a\"\n"
				"done\n"
				"[ -n \"$out\" ] && printf '%s' '" + std::string( pszBody ) + "' > \"$out\"\n"
				"exit 0\n";
		}
		else
		{
			sScript += "exit 22\n";   // curl's own \"HTTP error\" exit code
		}

		const std::string sPath = shim.sDir + "/curl";
		FILE *f = fopen( sPath.c_str(), "w" );
		REQUIRE( f != nullptr );
		fwrite( sScript.data(), 1, sScript.size(), f );
		fclose( f );
		REQUIRE( chmod( sPath.c_str(), 0755 ) == 0 );
		return shim;
	}

	std::string ReadFileOrEmpty( const std::string &sPath )
	{
		FILE *f = fopen( sPath.c_str(), "rb" );
		if ( !f )
			return {};
		std::string s;
		char szBuf[ 4096 ];
		for ( size_t n; ( n = fread( szBuf, 1, sizeof( szBuf ), f ) ) > 0; )
			s.append( szBuf, n );
		fclose( f );
		return s;
	}

	// Runs the poller for a couple of seconds in a child whose loader has
	// never run, with the shim on PATH, and reports the worst CurrentView()
	// it measured. Returns the child's stderr.
	std::string RunNetworkProbe( const Shim &shim, bool bLookupEnabled, const char *pszStubMode = "ok" )
	{
		int nPipe[ 2 ] = { -1, -1 };
		REQUIRE( pipe( nPipe ) == 0 );

		const pid_t nChild = fork();
		REQUIRE( nChild >= 0 );

		if ( nChild == 0 )
		{
			close( nPipe[ 0 ] );
			dup2( nPipe[ 1 ], STDERR_FILENO );
			close( nPipe[ 1 ] );

			setenv( "GS_RITZ_STEAMCLIENT", StubPath().c_str(), 1 );
			// No manifests anywhere, so EVERY app id the stub reports is one
			// the local files cannot answer -- which is exactly the case the
			// network path exists for.
			setenv( "GS_RITZ_STEAMAPPS", "/nonexistent/steamapps", 1 );
			setenv( "GS_RITZ_TEST_STEAMCLIENT_MODE", pszStubMode, 1 );
			setenv( "GS_RITZ_APPNAME_CACHE", shim.sCache.c_str(), 1 );

			const char *pszPath = getenv( "PATH" );
			setenv( "PATH", ( shim.sDir + ":" + ( pszPath ? pszPath : "/usr/bin" ) ).c_str(), 1 );

			SetLookupNames( bLookupEnabled );

			long nWorstUs = 0;
			for ( int i = 0; i < 400; i++ )   // ~2 s of 5 ms frames
			{
				const auto t0 = std::chrono::steady_clock::now();
				const View v = CurrentView();
				const auto t1 = std::chrono::steady_clock::now();
				const long nUs = (long)std::chrono::duration_cast<std::chrono::microseconds>( t1 - t0 ).count();
				if ( nUs > nWorstUs )
					nWorstUs = nUs;

				if ( i == 399 )
					for ( const Friend &f : v.vecFriends )
						fprintf( stderr, "PROBE-ROW: %s | %s | %s\n",
							f.sPersona.c_str(), f.sGame.c_str(),
							f.bInvited ? "INVITE" : "no-invite" );

				struct timespec ts = { 0, 5 * 1000 * 1000 };
				nanosleep( &ts, nullptr );
			}
			fprintf( stderr, "PROBE-WORST-VIEW-US: %ld\n", nWorstUs );
			Shutdown();
			fprintf( stderr, "PROBE-SHUTDOWN-OK\n" );
			fflush( stderr );
			_exit( 0 );
		}

		close( nPipe[ 1 ] );
		std::string sOut;
		char szBuf[ 512 ];
		for ( ssize_t n; ( n = read( nPipe[ 0 ], szBuf, sizeof( szBuf ) ) ) > 0; )
			sOut.append( szBuf, (size_t)n );
		close( nPipe[ 0 ] );

		int nStatus = 0;
		waitpid( nChild, &nStatus, 0 );
		return sOut;
	}
}

TEST_CASE( "a game the local files cannot name is looked up once and cached", "[steam_friends]" )
{
	// The stub's two joinable friends are in apps 730 and 440, and the probe
	// runs with no appmanifest directory at all -- so both are unknown and both
	// have to come from the lookup.
	const Shim shim = MakeCurlShim( "hit",
		"{\"response\":{\"apps\":["
		"{\"appid\":730,\"name\":\"Counter-Strike 2\"},"
		"{\"appid\":440,\"name\":\"Team Fortress 2\"}]}}" );

	const std::string sErr = RunNetworkProbe( shim, /* bLookupEnabled */ true );
	REQUIRE( sErr.find( "PROBE-SHUTDOWN-OK" ) != std::string::npos );

	// A request really went out, and the log says exactly what was in it.
	const std::string sCalls = ReadFileOrEmpty( shim.sLog );
	REQUIRE_FALSE( sCalls.empty() );
	REQUIRE( sCalls.find( "api.steampowered.com/ICommunityService/GetApps" ) != std::string::npos );
	REQUIRE( sCalls.find( "appids%5B0%5D=" ) != std::string::npos );
	// No SteamID64 and no persona name went anywhere near it. The stub's
	// hostile persona is the one that would show up if a name could leak.
	REQUIRE( sCalls.find( "7656119" ) == std::string::npos );
	REQUIRE( sCalls.find( "rm -rf" ) == std::string::npos );

	// The names reached the rows.
	REQUIRE( sErr.find( "| Counter-Strike 2 |" ) != std::string::npos );
	REQUIRE( sErr.find( "| Team Fortress 2 |" ) != std::string::npos );

	// AND THEY ARE ON DISK, which is the half that survives a restart.
	const AppNameMap cached = ParseAppNameCache( ReadFileOrEmpty( shim.sCache ) );
	REQUIRE( cached.count( 730 ) == 1 );
	REQUIRE( cached.at( 730 ).sName == "Counter-Strike 2" );
	REQUIRE( cached.at( 440 ).sName == "Team Fortress 2" );

	// ONE request, not one per poll: the second and third polls answered from
	// the cache. (The shim logs a line per invocation; two seconds of a
	// three-second poll interval is one or two polls, and only the first may
	// have had anything to ask.)
	size_t nCalls = 0;
	for ( char c : sCalls )
		nCalls += ( c == '\n' );
	REQUIRE( nCalls == 1 );
}

TEST_CASE( "with the lookup switched off, no request is made at all", "[steam_friends]" )
{
	const Shim shim = MakeCurlShim( "off",
		"{\"response\":{\"apps\":[{\"appid\":730,\"name\":\"Counter-Strike 2\"}]}}" );

	const std::string sErr = RunNetworkProbe( shim, /* bLookupEnabled */ false );
	REQUIRE( sErr.find( "PROBE-SHUTDOWN-OK" ) != std::string::npos );

	// THE PROOF: the shim was never executed, so its log does not exist. Had
	// anything fetched, this file would have a line in it.
	REQUIRE( ReadFileOrEmpty( shim.sLog ).empty() );
	REQUIRE( access( shim.sLog.c_str(), F_OK ) != 0 );

	// And no cache file was written either -- nothing was learned, so nothing
	// was stored.
	REQUIRE( ReadFileOrEmpty( shim.sCache ).empty() );

	// The rows still say something true. "App 730" is short and never wrong,
	// which is the whole degradation contract.
	REQUIRE( sErr.find( "| App 730 |" ) != std::string::npos );
	REQUIRE( sErr.find( "| Counter-Strike 2 |" ) == std::string::npos );
}

TEST_CASE( "an endpoint that never answers cannot delay a reader", "[steam_friends]" )
{
	// A `curl` that sits there for five seconds is a blackholed endpoint --
	// the case the whole worker-thread arrangement exists for. A fetch on the
	// frame path would show up here as a multi-second CurrentView().
	const Shim shim = MakeCurlShim( "slow", nullptr, /* nSleepSec */ 5 );

	const std::string sErr = RunNetworkProbe( shim, /* bLookupEnabled */ true );
	REQUIRE( sErr.find( "PROBE-SHUTDOWN-OK" ) != std::string::npos );

	const long nWorstUs = WorstViewUs( sErr );
	REQUIRE( nWorstUs >= 0 );
	// The same 15 ms threshold the slow-Steam case uses, against a 5 s stall.
	REQUIRE( nWorstUs < 15000 );

	// It really was reached, so the fast answers above are not just proving
	// that nothing was ever asked.
	REQUIRE_FALSE( ReadFileOrEmpty( shim.sLog ).empty() );

	// A failed fetch teaches nothing, so nothing is cached -- an outage must
	// never bake "App 730" into the file permanently.
	REQUIRE( ParseAppNameCache( ReadFileOrEmpty( shim.sCache ) ).empty() );
	REQUIRE( sErr.find( "| App 730 |" ) != std::string::npos );

	// And Shutdown() was NOT held up by it: the child exited, which it could
	// not have done if the poller were still waiting on a five-second curl.
}

TEST_CASE( "the read path never produces an invite row", "[steam_friends]" )
{
	// The honest half of the ordering rule. FriendOrderLess() ranks invites
	// first because that is what was asked for, but Steam offers no way to
	// SEE a pending received invite (superdoc/features/steam-friends.md,
	// "Received invites"), so no row may claim to be one. If that ever
	// changes, this is the case that has to be deliberately updated -- rather
	// than an invite quietly appearing because a field was left set.
	const Shim shim = MakeCurlShim( "invite", nullptr );
	const std::string sErr = RunNetworkProbe( shim, /* bLookupEnabled */ false );
	REQUIRE( sErr.find( "PROBE-ROW:" ) != std::string::npos );
	REQUIRE( sErr.find( "INVITE" ) == std::string::npos );
	REQUIRE( sErr.find( "no-invite" ) != std::string::npos );
}

TEST_CASE( "the rows come out of Snapshot() already in the drawn order", "[steam_friends]" )
{
	// The sort lives in Snapshot(), not in the panel, so the panel,
	// friends_dump and friends_join <n> all index the same sequence. The stub
	// deliberately declares its friends in a DIFFERENT order from this one.
	const Shim shim = MakeCurlShim( "order", nullptr );
	const std::string sErr = RunNetworkProbe( shim, /* bLookupEnabled */ false );

	std::vector<std::string> vecRows;
	for ( size_t nAt = sErr.find( "PROBE-ROW: " ); nAt != std::string::npos;
	      nAt = sErr.find( "PROBE-ROW: ", nAt + 1 ) )
	{
		const size_t nStart = nAt + strlen( "PROBE-ROW: " );
		const size_t nBar = sErr.find( " | ", nStart );
		REQUIRE( nBar != std::string::npos );
		vecRows.push_back( sErr.substr( nStart, nBar - nStart ) );
	}

	REQUIRE( vecRows.size() == 5 );
	// The two joinable ones first, alphabetically ("evil…" before "joinable
	// one"), then the three that are merely in a game, also alphabetically.
	REQUIRE( vecRows[ 0 ].rfind( "evil", 0 ) == 0 );
	REQUIRE( vecRows[ 1 ] == "joinable one" );
	REQUIRE( vecRows[ 2 ] == "in a game, not joinable" );
	REQUIRE( vecRows[ 3 ] == "no app" );
	REQUIRE( vecRows[ 4 ] == "non-steam game" );
}
