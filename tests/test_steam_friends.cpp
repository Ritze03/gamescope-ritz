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

#include "SteamFriends.h"
#include "SteamFriendsCmd.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <time.h>

#include <limits.h>
#include <signal.h>
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
	// Unlike the companion's browser command, this is not a user-editable
	// string, so there is no splitting step for anything to hide inside.
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
				constexpr uint64_t kBase = 76561197960265728ull;
				if ( vec.size() == 5 &&
				     vec[ 0 ].eJoinable == Joinability::NoLobby &&
				     vec[ 0 ].uAppId == 730 && vec[ 0 ].ulSteamId == kBase + 100 &&
				     vec[ 1 ].CanJoin() &&
				     vec[ 1 ].uAppId == 730 && vec[ 1 ].ulLobbyId == 555 && vec[ 1 ].ulSteamId == kBase + 101 &&
				     vec[ 2 ].eJoinable == Joinability::NotASteamApp &&
				     vec[ 3 ].eJoinable == Joinability::NoApp &&
				     vec[ 4 ].CanJoin() &&
				     vec[ 4 ].uAppId == 440 && vec[ 4 ].ulLobbyId == 999 && vec[ 4 ].ulSteamId == kBase + 104 &&
				     vec[ 4 ].sPersona.find( "rm -rf" ) != std::string::npos &&
				     vec[ 4 ].sGame == "App 440" )
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
