// "Join a friend" -- the rules, held down by tests that need no Steam.
//
// TWO HALVES, MATCHING THE TWO FILES:
//
//   * SteamFriendsCmd.h is pure, so the version lists, the CGameID unpacking,
//     the two filter predicates and -- the one that matters most -- the URL
//     builder's guards are ordinary in-process assertions.
//
//   * SteamFriends.cpp dlopens a library and calls through a vtable, so the
//     runtime cases run in a FORKED CHILD with GS_RITZ_STEAMCLIENT pointed at
//     tests/steamclient_stub.cpp's fake. The fork is not ceremony: the loader
//     deliberately tries exactly once per process (a compositor must not retry
//     a 46 MB dlopen every poll), so each failure mode needs its own process to
//     be the first thing that process ever sees.
//
// WHAT THE 2026-09-09 NARROWING CHANGED HERE. The list is now scoped to the
// game this session is running AND to the friends who are joinable, so the
// cases below check a FILTER with two halves rather than a list with reasons
// attached to every row -- and the game-name half (the appmanifest reader, the
// disk cache, the one HTTP request this fork ever made, and every test for
// them) is gone rather than adjusted.
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

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace gamescope::steamfriends;

// The app id every stub-driven case pretends this session is running under.
// tests/steamclient_stub.cpp's fake friends are built around it.
static constexpr uint32_t kSessionAppId = 730;

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
//  The two predicates -- the whole filter
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

// THE 2026-09-09 NARROWING'S OWN PREDICATE. "Only showing friends which are
// playing the same game as the running one."
TEST_CASE( "in this game means the same app id, as a real Steam app", "[steam_friends]" )
{
	REQUIRE( InThisGame( 730, 730 ) );

	// A different game -- the case that used to be a row (with a confirmation
	// dialog behind it) and must now not appear at all.
	REQUIRE_FALSE( InThisGame( 440, 730 ) );

	// A NON-STEAM SHORTCUT WEARING OUR OWN APP ID IN ITS LOW BITS. This is the
	// one way the filter could be fooled, and the CGameID type check is what
	// stops it: joining it would build a steam:// URL for app 730 aimed at
	// somebody who is not in app 730.
	REQUIRE_FALSE( InThisGame( 730ull | ( 1ull << 24 ), 730 ) );

	// A mod id in the top 32 bits does not change which app it is.
	REQUIRE( InThisGame( 730ull | ( 0xDEADBEEFull << 32 ), 730 ) );

	// NO APP ID MATCHES NOBODY, which is what makes "this isn't a Steam game"
	// an empty list rather than a special case anywhere else in the code --
	// and is the rule the panel's AvailableWhen() gate is the UI half of.
	REQUIRE_FALSE( InThisGame( 730, 0 ) );
	REQUIRE_FALSE( InThisGame( 0, 0 ) );
	REQUIRE_FALSE( InThisGame( 440, 0 ) );
}

// ===========================================================================
//  The status line
// ===========================================================================
// TWO NUMBERS, AND THE SECOND ONE IS WHY THIS FUNCTION EXISTS. The list holds
// only the joinable friends in this game, so an empty list has two very
// different causes -- and one of them is a bug we know we might have
// (m_steamIDLobby's offset, superdoc/planning/steam-friends-join.md §6e, reads
// as zero when it is wrong, exactly as it does when nobody is in a lobby).
TEST_CASE( "the status line tells the empty states apart", "[steam_friends]" )
{
	REQUIRE( StatusLine( 0, 0 ) == "nobody else is in this game right now." );
	REQUIRE( StatusLine( 3, 0 ) == "3 friends in this game, 0 you can join." );
	REQUIRE( StatusLine( 1, 0 ) == "1 friend in this game, 0 you can join." );
	REQUIRE( StatusLine( 1, 1 ) == "1 friend in this game, 1 you can join." );
	REQUIRE( StatusLine( 5, 2 ) == "5 friends in this game, 2 you can join." );

	// THE PROPERTY THE WHOLE ROW IS FOR: "nobody is here" and "people are here
	// but none of them is joinable" must not share a sentence, because the
	// second one is the state that could mean a broken lobby read.
	REQUIRE( StatusLine( 0, 0 ) != StatusLine( 3, 0 ) );

	// No two of them are the same string.
	const std::vector<std::string> v = {
		StatusLine( 0, 0 ), StatusLine( 3, 0 ), StatusLine( 1, 1 ), StatusLine( 5, 2 ) };
	for ( size_t i = 0; i < v.size(); i++ )
		for ( size_t j = i + 1; j < v.size(); j++ )
			REQUIRE( v[ i ] != v[ j ] );

	// The joinable figure is always a NUMBER, never "none": the two counts
	// have to be readable as a pair for the diagnosis above to work.
	REQUIRE( StatusLine( 3, 0 ).find( " 0 you can join" ) != std::string::npos );
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
	// with the given library path, stub mode and SESSION APP ID, and hands
	// back what the child found and everything it logged.
	//
	// nTimes > 1 is how "exactly ONE log line" is tested: a compositor polls
	// this, so a failure that logs once per poll would drown the log.
	Probe RunProbe( const std::string &sLibPath, const char *pszMode, int nTimes = 3,
	                uint32_t uAppId = kSessionAppId )
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
			if ( pszMode )
				setenv( "GS_RITZ_TEST_STEAMCLIENT_MODE", pszMode, 1 );
			else
				unsetenv( "GS_RITZ_TEST_STEAMCLIENT_MODE" );

			// The one fact the whole feature is scoped by. main.cpp seeds this
			// from config::SessionAppId(); the test seeds it directly, which is
			// also how "no app id" is exercised (uAppId == 0).
			SetSessionAppId( uAppId );

			size_t nLast = 0;
			bool bStable = true;
			for ( int i = 0; i < nTimes; i++ )
			{
				const std::vector<Friend> vec = Snapshot();
				if ( i > 0 && vec.size() != nLast )
					bStable = false;
				nLast = vec.size();

				// Every row is joinable by construction now, so the row count
				// IS the joinable count.
				fprintf( stderr, "PROBE-JOINABLE: %zu\n", vec.size() );

				// The success path's contents, checked here because only the
				// child ever has them. TWO rows -- the only two of the stub's
				// eight fake friends that are in app 730 AND in a lobby -- in
				// list order, with their app ids, lobby ids and SteamIDs.
				// THE ORDER IS Snapshot()'s OWN SORT and is not the order the
				// stub declares them in, which is what makes this a check on
				// the sort as well as on the filter.
				constexpr uint64_t kBase = 76561197960265728ull;
				if ( vec.size() == 2 &&
				     vec[ 0 ].uAppId == 730 && vec[ 0 ].ulLobbyId == 111 &&
				     vec[ 0 ].ulSteamId == kBase + 106 &&
				     vec[ 0 ].sPersona.find( "rm -rf" ) != std::string::npos &&
				     vec[ 1 ].uAppId == 730 && vec[ 1 ].ulLobbyId == 555 &&
				     vec[ 1 ].ulSteamId == kBase + 101 &&
				     vec[ 1 ].sPersona == "joinable one" )
				{
					fprintf( stderr, "PROBE-CONTENTS-OK\n" );
				}

				for ( const Friend &f : vec )
					fprintf( stderr, "PROBE-ROW: %s | %s\n", f.sPersona.c_str(),
						f.bInvited ? "INVITE" : "no-invite" );
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

	// The persona of every PROBE-ROW line, in the order they were printed.
	std::vector<std::string> RowsIn( const std::string &sStderr )
	{
		std::vector<std::string> vecRows;
		for ( size_t nAt = sStderr.find( "PROBE-ROW: " ); nAt != std::string::npos;
		      nAt = sStderr.find( "PROBE-ROW: ", nAt + 1 ) )
		{
			const size_t nStart = nAt + strlen( "PROBE-ROW: " );
			const size_t nBar = sStderr.find( " | ", nStart );
			if ( nBar == std::string::npos )
				break;
			vecRows.push_back( sStderr.substr( nStart, nBar - nStart ) );
		}
		return vecRows;
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

// THE FIRST THING THE 2026-09-09 NARROWING ADDED: with no Steam app id there is
// nothing to join, so there is no list -- and, crucially, NO dlopen either. A
// gamescope that was not launched by Steam must not load Steam's 46 MB client
// library just to be told nobody is there.
TEST_CASE( "no app id is an empty list, said in those words, with no library loaded", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "ok", /* nTimes */ 3, /* uAppId */ 0 );
	REQUIRE( p.nRows == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "this isn't a Steam game" ) != std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-STABLE" ) != std::string::npos );

	// The library was never touched: the "loaded the Steam client library"
	// line is what EnsureLibrary() prints on success, and the app-id check
	// returns before it.
	REQUIRE( p.sStderr.find( "loaded the Steam client library" ) == std::string::npos );
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

// ===========================================================================
//  The filter, end to end (2026-09-09)
// ===========================================================================
// The stub declares EIGHT fake friends. Exactly two may become rows, and each
// of the other six is excluded by a different clause -- so a filter that lost
// any one half of itself changes this count.
TEST_CASE( "only the joinable friends in THIS game become rows", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "ok" );
	REQUIRE( p.nRows == 2 );
	REQUIRE( p.sStderr.find( "PROBE-JOINABLE: 2" ) != std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-CONTENTS-OK" ) != std::string::npos );
	REQUIRE( p.sStderr.find( "the join list is unavailable" ) == std::string::npos );

	// THREE friends are in app 730 (two joinable, one not), which is the first
	// number, and TWO of them can be joined, which is the second. The status
	// line carrying both is what makes an empty list diagnosable.
	REQUIRE( p.sStderr.find( "PROBE-STATUS: 3 friends in this game, 2 you can join." ) != std::string::npos );

	// A FRIEND IN A DIFFERENT GAME IS EXCLUDED. The stub's "different game"
	// friend is in app 440 and IS in a lobby, so only the app-id half of the
	// filter can be rejecting them.
	const std::vector<std::string> vecRows = RowsIn( p.sStderr );
	REQUIRE_FALSE( vecRows.empty() );
	for ( const std::string &s : vecRows )
	{
		REQUIRE( s != "different game" );
		// ... and so are the shortcut wearing our app id, the type-1 mod, the
		// app-id-0 lobby, the offline friend and the one who is here but not
		// in a lobby.
		REQUIRE( s != "shortcut wearing our app id" );
		REQUIRE( s != "non-steam game" );
		REQUIRE( s != "no app" );
		REQUIRE( s != "offline" );
		REQUIRE( s != "in this game, not joinable" );
	}
}

// THE SECOND HALF OF THE STATUS ROW'S JOB, and the state this narrowing had to
// be careful about: everybody is here and NOBODY is joinable, which is also
// exactly what a wrong m_steamIDLobby offset would look like. The list is
// empty, and the sentence beside it must still name both numbers.
TEST_CASE( "friends here but none joinable is an empty list with a telling status", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "busy" );
	REQUIRE( p.nRows == 0 );
	REQUIRE( p.sStderr.find( "PROBE-STATUS: 3 friends in this game, 0 you can join." ) != std::string::npos );

	// And it is NOT the same sentence as "nobody is here", which is the whole
	// point -- see the idle case below.
	REQUIRE( p.sStderr.find( "nobody else is in this game" ) == std::string::npos );
	REQUIRE( p.sStderr.find( "the join list is unavailable" ) == std::string::npos );
}

TEST_CASE( "nobody else in this game says so, and says something different", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "idle" );
	REQUIRE( p.nRows == 0 );
	REQUIRE( p.sStderr.find( "PROBE-STATUS: nobody else is in this game right now." ) != std::string::npos );
	REQUIRE( p.sStderr.find( "friends in this game" ) == std::string::npos );
}

TEST_CASE( "the read path never produces an invite row", "[steam_friends]" )
{
	// The honest half of the ordering rule. FriendOrderLess() ranks invites
	// first because that is what was asked for, but Steam offers no way to
	// SEE a pending received invite (superdoc/features/steam-friends.md,
	// "Received invites"), so no row may claim to be one. If that ever
	// changes, this is the case that has to be deliberately updated -- rather
	// than an invite quietly appearing because a field was left set.
	const Probe p = RunProbe( StubPath(), "ok" );
	REQUIRE( p.sStderr.find( "PROBE-ROW:" ) != std::string::npos );
	REQUIRE( p.sStderr.find( "INVITE" ) == std::string::npos );
	REQUIRE( p.sStderr.find( "no-invite" ) != std::string::npos );
}

TEST_CASE( "the rows come out of Snapshot() already in the drawn order", "[steam_friends]" )
{
	// The sort lives in Snapshot(), not in the panel, so the panel,
	// friends_dump and friends_join <n> all index the same sequence. The stub
	// deliberately declares its two joinable friends in the OTHER order.
	const Probe p = RunProbe( StubPath(), "ok", /* nTimes */ 1 );
	const std::vector<std::string> vecRows = RowsIn( p.sStderr );
	REQUIRE( vecRows.size() == 2 );
	REQUIRE( vecRows[ 0 ].rfind( "evil", 0 ) == 0 );   // "evil…" before "joinable one"
	REQUIRE( vecRows[ 1 ] == "joinable one" );
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
		setenv( "GS_RITZ_TEST_STEAMCLIENT_MODE", pszMode, 1 );
		SetSessionAppId( kSessionAppId );

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
			{
				bPolled = true;
				// The count the status row needs is published with the rows,
				// under the same lock, so it can never be from another poll.
				fprintf( stderr, "PROBE-VIEW: %zu rows, %zu in this game\n",
					v.vecFriends.size(), v.nInThisGame );
			}

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

	// The published View carries the two rows AND the count of everyone in
	// this game, which is what the panel's status row reads.
	REQUIRE( p.sStderr.find( "PROBE-VIEW: 2 rows, 3 in this game" ) != std::string::npos );

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
// invites." Since the narrowing every row IS joinable, so two bands remain --
// and one of them (invites) is never produced. Alphabetical inside a band, and
// a total order so an unchanged list never reshuffles itself.
namespace
{
	Friend MakeFriend( const char *pszPersona, uint64_t ulSteamId, bool bInvited = false )
	{
		Friend f;
		f.sPersona  = pszPersona;
		f.uAppId    = 730;
		f.ulLobbyId = 555;
		f.ulSteamId = ulSteamId;
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

TEST_CASE( "the list is ordered invites first, then everyone alphabetically", "[steam_friends]" )
{
	const std::vector<Friend> vec = {
		MakeFriend( "zoe",    201 ),
		MakeFriend( "alice",  202 ),
		MakeFriend( "bob",    203 ),
		MakeFriend( "yannis", 204 ),
		MakeFriend( "carol",  205, /* bInvited */ true ),
	};

	REQUIRE( PersonasInOrder( vec ) ==
		std::vector<std::string>{ "carol", "alice", "bob", "yannis", "zoe" } );

	// The bands themselves, named rather than inferred from the sequence.
	REQUIRE( GroupOf( vec[ 4 ] ) == FriendGroup::Invite );
	REQUIRE( GroupOf( vec[ 1 ] ) == FriendGroup::Joinable );

	// An invite outranks a joinable row even when the joinable row would sort
	// first alphabetically -- the band is checked before the name, always.
	REQUIRE( FriendOrderLess( MakeFriend( "zzz", 1, true ), MakeFriend( "aaa", 2 ) ) );
}

TEST_CASE( "the ordering copes with empty and single-band lists", "[steam_friends]" )
{
	// The two cases a comparator is most often wrong about.
	std::vector<Friend> vecEmpty;
	std::sort( vecEmpty.begin(), vecEmpty.end(), FriendOrderLess );
	REQUIRE( vecEmpty.empty() );

	const std::vector<Friend> vecOne = { MakeFriend( "solo", 301 ) };
	REQUIRE( PersonasInOrder( vecOne ) == std::vector<std::string>{ "solo" } );

	// The shipping case: everything joinable, so purely alphabetical.
	const std::vector<Friend> vecAllJoinable = {
		MakeFriend( "delta", 302 ),
		MakeFriend( "alpha", 303 ),
		MakeFriend( "charlie", 304 ),
	};
	REQUIRE( PersonasInOrder( vecAllJoinable ) ==
		std::vector<std::string>{ "alpha", "charlie", "delta" } );

	// All invites, which is the band nothing produces today -- it still has to
	// order sanely the day something does.
	const std::vector<Friend> vecAllInvites = {
		MakeFriend( "delta", 308, true ),
		MakeFriend( "alpha", 309, true ),
	};
	REQUIRE( PersonasInOrder( vecAllInvites ) == std::vector<std::string>{ "alpha", "delta" } );
}

TEST_CASE( "the order is case-insensitive, total, and stable across polls", "[steam_friends]" )
{
	// Case: "Zoe" must not sort before "alice" because 'Z' < 'a' in ASCII.
	const std::vector<Friend> vecCase = {
		MakeFriend( "Zoe",   401 ),
		MakeFriend( "alice", 402 ),
		MakeFriend( "BOB",   403 ),
	};
	REQUIRE( PersonasInOrder( vecCase ) == std::vector<std::string>{ "alice", "BOB", "Zoe" } );

	// TOTAL: two friends can share a display name, and without the SteamID
	// tiebreak the sort would be free to swap them between polls -- a list
	// that flickers while nothing has changed. Neither may compare "less"
	// than the other in both directions, and one of them must.
	const Friend a = MakeFriend( "same name", 500 );
	const Friend b = MakeFriend( "same name", 501 );
	REQUIRE( FriendOrderLess( a, b ) );
	REQUIRE_FALSE( FriendOrderLess( b, a ) );

	// Irreflexive, which is what std::sort actually requires of it.
	REQUIRE_FALSE( FriendOrderLess( a, a ) );

	// STABILITY, measured rather than argued: sorting the same rows from three
	// different starting permutations must give three identical sequences.
	std::vector<Friend> vec = {
		MakeFriend( "same name", 501 ),
		MakeFriend( "same name", 500 ),
		MakeFriend( "other",     502 ),
		MakeFriend( "another",   503 ),
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
		MakeFriend( "",            601 ),
		MakeFriend( "\xf0\x9f\x92\x80", 602 ),   // an emoji
		MakeFriend( "!!!",         603 ),
		MakeFriend( "aaa",         604 ),
	};
	const std::vector<std::string> vecOrder = PersonasInOrder( vec );
	REQUIRE( vecOrder.size() == 4 );
	REQUIRE( vecOrder[ 0 ].empty() );          // "" is less than everything
	REQUIRE( vecOrder.back() == "\xf0\x9f\x92\x80" );   // 0xf0 is above ASCII
}
