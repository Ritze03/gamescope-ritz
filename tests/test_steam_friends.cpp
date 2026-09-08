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

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// ===========================================================================
//  The URL
// ===========================================================================
TEST_CASE( "the join URL is steam://joinlobby/appid/lobby/steamid", "[steam_friends]" )
{
	JoinableFriend f;
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
	JoinableFriend big;
	big.uAppId    = 4294967295u;
	big.ulLobbyId = 18446744073709551615ull;
	big.ulSteamId = 18446744073709551614ull;
	REQUIRE( BuildJoinUrl( big, &sUrl, &sWhy ) );
	REQUIRE( sUrl == "steam://joinlobby/4294967295/18446744073709551615/18446744073709551614" );
}

TEST_CASE( "a lobby id of 0 is refused, not formatted", "[steam_friends]" )
{
	JoinableFriend f;
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
// A persona name is the ONE field in a JoinableFriend that a remote person
// chooses, and it is the only string in the row. If it could reach the command
// line, a friend could rename themselves into extra arguments for `steam` --
// so the design is that BuildJoinUrl() reads three INTEGERS and never looks at
// the name at all. That is a property of the types rather than of an escaping
// routine somebody could forget to call, and this is what pins it.
TEST_CASE( "a hostile persona name cannot influence the join command", "[steam_friends]" )
{
	JoinableFriend base;
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
		JoinableFriend f = base;
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
		int         nJoinable = -1;   // the child's exit code
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
			if ( pszMode )
				setenv( "GS_RITZ_TEST_STEAMCLIENT_MODE", pszMode, 1 );
			else
				unsetenv( "GS_RITZ_TEST_STEAMCLIENT_MODE" );

			size_t nLast = 0;
			bool bStable = true;
			for ( int i = 0; i < nTimes; i++ )
			{
				const std::vector<JoinableFriend> vec = Snapshot();
				if ( i > 0 && vec.size() != nLast )
					bStable = false;
				nLast = vec.size();

				// The success path's contents, checked here because only the
				// child ever has them: two rows, in list order, with the app
				// ids, lobby ids and SteamIDs the stub's fake friends carry.
				constexpr uint64_t kBase = 76561197960265728ull;
				if ( vec.size() == 2 &&
				     vec[ 0 ].uAppId == 730 && vec[ 0 ].ulLobbyId == 555 && vec[ 0 ].ulSteamId == kBase + 101 &&
				     vec[ 1 ].uAppId == 440 && vec[ 1 ].ulLobbyId == 999 && vec[ 1 ].ulSteamId == kBase + 104 &&
				     vec[ 1 ].sPersona.find( "rm -rf" ) != std::string::npos )
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
		p.nJoinable = WIFEXITED( nStatus ) ? WEXITSTATUS( nStatus ) : -1;
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
	REQUIRE( p.nJoinable == 0 );
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
	REQUIRE( p.nJoinable == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "isn't the one we know" ) != std::string::npos );
}

TEST_CASE( "an interface version we do not know disables the feature", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "noiface" );
	REQUIRE( p.nJoinable == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "newer than this build knows" ) != std::string::npos );
}

TEST_CASE( "an unknown ISteamFriends version disables the feature too", "[steam_friends]" )
{
	// The second half of the same guard: the client answered, its friends
	// interface did not.
	const Probe p = RunProbe( StubPath(), "nofriends" );
	REQUIRE( p.nJoinable == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "newer than this build knows" ) != std::string::npos );
}

TEST_CASE( "Steam not running is an empty list, said in those words", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "nopipe" );
	REQUIRE( p.nJoinable == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "Steam isn't running." ) != std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-STATUS: Steam isn't running." ) != std::string::npos );
}

TEST_CASE( "a signed-out client is an empty list, said in those words", "[steam_friends]" )
{
	const Probe p = RunProbe( StubPath(), "nouser" );
	REQUIRE( p.nJoinable == 0 );
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
	REQUIRE( p.nJoinable == 0 );
	REQUIRE( CountOf( p.sStderr, "the join list is unavailable" ) == 1 );
	REQUIRE( p.sStderr.find( "isn't laid out the way this build expects" ) != std::string::npos );
}

TEST_CASE( "the success path keeps only the joinable rows, with the right fields", "[steam_friends]" )
{
	// Six fake friends, of which exactly two are joinable -- the other four
	// are each rejected by a different clause, so a loader that forgot the
	// filter would come back with more than two here.
	const Probe p = RunProbe( StubPath(), "ok" );
	REQUIRE( p.nJoinable == 2 );
	REQUIRE( p.sStderr.find( "PROBE-CONTENTS-OK" ) != std::string::npos );
	REQUIRE( p.sStderr.find( "the join list is unavailable" ) == std::string::npos );
	REQUIRE( p.sStderr.find( "PROBE-STATUS: 2 friends you can join." ) != std::string::npos );
}
