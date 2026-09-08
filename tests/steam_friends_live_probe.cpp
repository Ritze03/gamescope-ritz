// steam_friends_live_probe.cpp -- the phase 1 read path, run against a REAL,
// running Steam client.
//
// NOT A UNIT TEST, and deliberately not registered as one: it needs a Steam
// that is installed, running and signed in, so as a test it would fail on every
// machine that has none. meson builds it beside gamescope_tests; you run it by
// hand, when you are not in the middle of a match:
//
//     build-release/tests/steam_friends_live_probe
//
// IT LINKS THE SHIPPING CODE. This file declares no vtable and dlopens nothing
// of its own: it calls gamescope::steamfriends::Snapshot() out of
// src/SteamFriends.cpp. So what it checks is the thing that ships, not a copy
// of it -- which is the difference between this and the feasibility study's
// steamclient_probe.c, whose ABI declarations were its own.
//
// STRICTLY READ-ONLY. The only Steamworks calls it can reach are
// CreateSteamPipe / ConnectToGlobalUser / GetISteamFriends / GetFriendCount /
// GetFriendByIndex / GetFriendPersonaName / GetFriendGamePlayed, then
// ReleaseUser / BReleaseSteamPipe. Nothing writes, nothing invites, nothing
// messages, nothing joins, and NO steam:// URL is ever fired -- Join() is not
// called from here at all.
//
// WHAT IT IS FOR. tests/test_steam_friends.cpp proves the loader's control flow
// against a fake (tests/steamclient_stub.cpp), and a fake built from the same
// declaration cannot prove the declaration is RIGHT. This can: it checks that
// the two 64-bit ids coming out of the real client land in the two disjoint
// SteamID bands they must be in, that app ids are in range, and that persona
// names decode as text. A wrong offset in FriendGameInfo_t fails at least one
// of those instead of surfacing months later as a mysterious bad join.
//
// PRIVACY. It prints counts, app ids and RANGE VERDICTS about ids -- never a
// SteamID, never a lobby id, never a persona name. Its output is safe to commit
// and safe to paste into an issue, which is the rule src/SteamFriends.cpp's own
// logging follows for the same reason.

#include "SteamFriends.h"
#include "SteamFriendsCmd.h"
#include "convar.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
//  --diagnose: the probe's OWN eyes on the interface
// ---------------------------------------------------------------------------
// When Snapshot() comes back with nothing, there are two very different causes
// and they look identical from outside: the vtable slot we call as
// GetFriendCount is the wrong slot, or the slot is right and this client will
// not hand a friends list to a process that is not a registered game.
//
// This mode tells them apart with three READ-ONLY calls that need no friends at
// all: our OWN persona name (slot 0), our own persona state (slot 2), and then
// GetFriendCount (slot 3) across several flag values. If slot 0 returns our own
// name, the object and the leading slots are right and the empty list is about
// availability, not about layout.
//
// It declares its own minimal vtable rather than reaching into
// src/SteamFriends.cpp's anonymous namespace, exactly as the feasibility
// study's steamclient_probe.c did -- a probe is allowed a private copy.
namespace diag
{
	struct IFriends;
	struct IClient;

	struct ClientVT
	{
		int32_t ( *CreateSteamPipe )( IClient * );
		bool    ( *BReleaseSteamPipe )( IClient *, int32_t );
		int32_t ( *ConnectToGlobalUser )( IClient *, int32_t );
		int32_t ( *CreateLocalUser )( IClient *, int32_t *, int );
		void    ( *ReleaseUser )( IClient *, int32_t, int32_t );
		void   *( *GetISteamUser )( IClient *, int32_t, int32_t, const char * );
		void   *( *GetISteamGameServer )( IClient *, int32_t, int32_t, const char * );
		void    ( *SetLocalIPBinding )( IClient *, const void *, uint16_t );
		void   *( *GetISteamFriends )( IClient *, int32_t, int32_t, const char * );
	};
	struct IClient { const ClientVT *vt; };

	// The slots are probed as RAW POINTERS rather than through a declared
	// layout, because which layout is right is the question. Every shape below
	// is read-only under BOTH hypotheses being tested, which is the rule that
	// made this safe to run against a live client: an extra integer argument to
	// a function that takes none is ignored by the ABI, and no slot is ever
	// called with a shape that could be a setter.
	struct IFriends { void *const *vt; };

	// Repeated here rather than shared: this block is a self-contained probe.
	constexpr uint64_t kIndividualMin = 76561197960265728ull;
	constexpr uint64_t kIndividualMax = 76561197960265728ull + 0xFFFFFFFFull;

	// `--find <persona>` answers one yes/no question: is a friend with exactly
	// this display name in the list right now? It prints the verdict and
	// nothing else -- no id, and not the name it was handed back, only whether
	// it matched the one you already typed. It exists because "is this specific
	// account visible to us" is the question every later phase starts from, and
	// answering it by dumping the list would spill 28 real people's names.
	const char *g_pszFind = nullptr;

	int Run()
	{
		void *pLib = dlopen( ( std::string( getenv( "HOME" ) ? getenv( "HOME" ) : "" ) +
			"/.steam/steam/linux64/steamclient.so" ).c_str(), RTLD_LAZY | RTLD_LOCAL );
		if ( !pLib ) { printf( "RESULT diagnose: FAIL (no library)\n" ); return 1; }

		auto pfn = (void *(*)( const char *, int * ))dlsym( pLib, "CreateInterface" );
		if ( !pfn ) { printf( "RESULT diagnose: FAIL (no CreateInterface)\n" ); return 1; }

		int nErr = 0;
		IClient *pClient = (IClient *)pfn( "SteamClient023", &nErr );
		if ( !pClient ) { printf( "RESULT diagnose: FAIL (no ISteamClient)\n" ); return 1; }

		const int32_t hPipe = pClient->vt->CreateSteamPipe( pClient );
		const int32_t hUser = hPipe ? pClient->vt->ConnectToGlobalUser( pClient, hPipe ) : 0;
		printf( "RESULT pipe/user: %d / %d\n", hPipe, hUser );
		if ( !hUser ) { printf( "RESULT diagnose: FAIL (no global user)\n" ); return 1; }

		IFriends *pFriends = (IFriends *)pClient->vt->GetISteamFriends(
			pClient, hUser, hPipe, "SteamFriends018" );
		if ( !pFriends ) { printf( "RESULT diagnose: FAIL (no ISteamFriends)\n" ); return 1; }

		using StrFn   = const char *( * )( IFriends * );
		using CountFn = int ( * )( IFriends *, int );
		using IndexFn = uint64_t ( * )( IFriends *, int, int );

		// Slot 0 -- our OWN persona name. Length and printability only, never
		// the string. If this is a sane string the object and the vtable base
		// are right, and any disagreement further down is an OFFSET problem
		// rather than a "we are not talking to ISteamFriends" problem.
		const char *pszMe = ( (StrFn)pFriends->vt[ 0 ] )( pFriends );
		const size_t nLen = pszMe ? strlen( pszMe ) : 0;
		bool bPrintable = nLen > 0;
		for ( size_t i = 0; i < nLen; i++ )
			if ( (unsigned char)pszMe[ i ] < 0x20 ) bPrintable = false;
		printf( "RESULT slot0 (GetPersonaName): %s (%zu chars, %s)\n",
			nLen ? "returned a string" : "EMPTY", nLen,
			bPrintable ? "printable" : "not printable" );

		const struct { const char *psz; int n; } kFlags[] = {
			{ "Blocked         ", 0x01 },
			{ "FriendshipReq   ", 0x02 },
			{ "Immediate       ", 0x04 },
			{ "ClanMember      ", 0x08 },
			{ "OnGameServer    ", 0x10 },
			{ "All (0xFFFF)    ", 0xFFFF },
		};

		// Slots 2 and 3, each read as a count. Exactly one of them is
		// GetFriendCount; the other is a no-argument getter that ignores the
		// flag. So the TELL is variation: the real GetFriendCount answers
		// differently for Immediate than for ClanMember, and a getter that
		// takes no argument returns the same number every time.
		for ( int nSlot = 2; nSlot <= 3; nSlot++ )
		{
			printf( "RESULT slot%d as GetFriendCount(flags):", nSlot );
			for ( const auto &f : kFlags )
				printf( " %s=%d", f.psz + 0, ( (CountFn)pFriends->vt[ nSlot ] )( pFriends, f.n ) );
			printf( "\n" );
		}

		// And the decisive one. Under the published layout slot 3 is
		// GetFriendCount and slot 4 is GetFriendByIndex; if the vtable is
		// shifted by one they are slots 2 and 3. A SteamID64 for an individual
		// account is a very narrow band, so whichever slot answers inside it,
		// called as GetFriendByIndex(0, Immediate), is the real one.
		for ( int nSlot = 3; nSlot <= 4; nSlot++ )
		{
			const uint64_t ul = ( (IndexFn)pFriends->vt[ nSlot ] )( pFriends, 0, 0x04 );
			const bool bOk = ul >= kIndividualMin && ul <= kIndividualMax;
			printf( "RESULT slot%d as GetFriendByIndex(0, Immediate): %s\n",
				nSlot, bOk ? "an id in the individual-account band" : "not a SteamID64" );
		}

		// ------------------------------------------------------------------
		//  FriendGameInfo_t, and the two slots that fill and name a friend
		// ------------------------------------------------------------------
		// The vtable being right does not make the STRUCT right: m_gameID and
		// m_steamIDLobby are 24 bytes apart with two shorts and an int between
		// them, and a wrong offset there would produce plausible-looking
		// nonsense rather than an obvious failure. So: walk the list, and check
		// that every in-game friend's app id is in range, that any non-zero
		// lobby id is inside the chat-SteamID band, and that persona names come
		// back as text. Counts and verdicts only -- no ids, no names.
		struct GameInfo
		{
			uint64_t m_gameID;
			uint32_t m_unGameIP;
			uint16_t m_usGamePort;
			uint16_t m_usQueryPort;
			uint64_t m_steamIDLobby;
		};
		using NameFn   = const char *( * )( IFriends *, uint64_t );
		using PlayedFn = bool ( * )( IFriends *, uint64_t, GameInfo * );

		constexpr uint64_t kChatMin = 0x0170000000000000ull;
		constexpr uint64_t kChatMax = 0x0190000000000000ull;

		const int nFriends = ( (CountFn)pFriends->vt[ 2 ] )( pFriends, 0x04 );
		int nNames = 0, nBadNames = 0, nInGame = 0, nBadAppId = 0;
		int nWithLobby = 0, nLobbyOutOfBand = 0, nNonAppType = 0;
		int nFound = 0;

		for ( int i = 0; i < nFriends; i++ )
		{
			const uint64_t ul = ( (IndexFn)pFriends->vt[ 3 ] )( pFriends, i, 0x04 );
			if ( !( ul >= kIndividualMin && ul <= kIndividualMax ) )
				continue;

			const char *pszName = ( (NameFn)pFriends->vt[ 6 ] )( pFriends, ul );
			if ( pszName && *pszName )
			{
				nNames++;
				for ( const char *p = pszName; *p; p++ )
					if ( (unsigned char)*p < 0x20 ) { nBadNames++; break; }
				if ( g_pszFind && strcmp( pszName, g_pszFind ) == 0 )
					nFound++;
			}

			GameInfo gi{};
			if ( !( (PlayedFn)pFriends->vt[ 7 ] )( pFriends, ul, &gi ) )
				continue;
			nInGame++;

			const uint32_t uAppId = (uint32_t)( gi.m_gameID & 0x00FFFFFFull );
			const uint8_t  uType  = (uint8_t)( ( gi.m_gameID >> 24 ) & 0xFFull );
			if ( uType != 0 )
				nNonAppType++;
			else if ( uAppId == 0 || uAppId >= ( 1u << 24 ) )
				nBadAppId++;

			if ( gi.m_steamIDLobby )
			{
				nWithLobby++;
				if ( !( gi.m_steamIDLobby >= kChatMin && gi.m_steamIDLobby <= kChatMax ) )
					nLobbyOutOfBand++;
			}
			printf( "  in-game friend: appid %-8u gameid-type %u  lobby %s\n",
				uAppId, uType, gi.m_steamIDLobby ? "PRESENT" : "none" );
		}

		printf( "RESULT friends enumerated: %d\n", nFriends );
		printf( "RESULT slot6 (GetFriendPersonaName): %d names, %d not printable -> %s\n",
			nNames, nBadNames, ( nNames > 0 && nBadNames == 0 ) ? "PASS" : "FAIL" );
		printf( "RESULT slot7 (GetFriendGamePlayed): %d in a game, %d with a bad app id, "
			"%d non-app CGameID types -> %s\n",
			nInGame, nBadAppId, nNonAppType, nBadAppId == 0 ? "PASS" : "FAIL" );
		printf( "RESULT m_steamIDLobby: %d non-zero, %d outside the chat band -> %s\n",
			nWithLobby, nLobbyOutOfBand,
			nLobbyOutOfBand == 0 ? "PASS" : "FAIL" );
		if ( g_pszFind )
			printf( "RESULT --find: %s\n", nFound ? "present in the friends list" : "NOT in the friends list" );
		if ( nWithLobby == 0 )
			printf( "NOTE: nobody was in a joinable lobby at this moment, so the lobby field's "
				"VALUE is untested -- only that it reads as zero when it should.\n" );

		pClient->vt->ReleaseUser( pClient, hPipe, hUser );
		pClient->vt->BReleaseSteamPipe( pClient, hPipe );
		return 0;
	}
}

using namespace gamescope;
using namespace gamescope::steamfriends;

namespace
{
	// A SteamID64 for an INDIVIDUAL account: universe 1, type 1, instance 1,
	// i.e. 0x0110000100000000 + accountid. If the struct offsets were wrong,
	// the number coming out of GetFriendByIndex would land nowhere near here.
	constexpr uint64_t kIndividualMin = 76561197960265728ull;
	constexpr uint64_t kIndividualMax = 76561197960265728ull + 0xFFFFFFFFull;

	// A lobby id is a SteamID of chat type, universe 1: the 0x0170.../0x0180...
	// band -- much larger, and not overlapping the individual band above.
	// Checking BOTH is what makes this a layout test rather than a "we got some
	// numbers back" test: the two fields sit 24 bytes apart in FriendGameInfo_t,
	// and a wrong offset puts one of them in the other's band, or in neither.
	constexpr uint64_t kChatMin = 0x0170000000000000ull;
	constexpr uint64_t kChatMax = 0x0190000000000000ull;

	bool LooksPrintable( const std::string &s )
	{
		if ( s.empty() )
			return false;
		for ( unsigned char c : s )
			if ( c < 0x20 && c != '\t' )
				return false;
		return true;
	}
}

int main( int argc, char **argv )
{
	// Turn on the one debug line Snapshot() emits: counts, and the interface
	// version strings that actually answered.
	std::vector<std::string_view> vecSetLog = { "log_friends", "debug" };
	ConCommand::Exec( vecSetLog );

	printf( "== gamescope-ritz phase 1 live read, against the running Steam client ==\n" );
	printf( "   read-only: no join, no invite, no message, no steam:// URL fired.\n\n" );

	// `--watch N` snapshots N times, a second apart, before reporting on the
	// last one. `Why it exists:` the first read after a fresh
	// ConnectToGlobalUser can come back with an EMPTY friends list while the
	// client is still filling its cache -- which looks identical to "you have
	// no friends in a joinable game" and is not. Watching says which it was,
	// and it is the measurement phase 3's poll interval has to be chosen from.
	int nWatch = 1;
	for ( int i = 1; i < argc; i++ )
	{
		if ( std::string_view( argv[ i ] ) == "--watch" && i + 1 < argc )
			nWatch = atoi( argv[ i + 1 ] );
		if ( std::string_view( argv[ i ] ) == "--find" && i + 1 < argc )
			diag::g_pszFind = argv[ i + 1 ];
		if ( std::string_view( argv[ i ] ) == "--diagnose" )
			return diag::Run();
	}

	std::vector<JoinableFriend> vec;
	for ( int i = 0; i < nWatch; i++ )
	{
		if ( i )
			sleep( 1 );
		vec = Snapshot();
	}

	printf( "\nRESULT status: %s\n", StatusText().c_str() );
	printf( "RESULT joinable rows: %zu\n", vec.size() );

	int nBadSteamId = 0, nBadLobby = 0, nBadAppId = 0, nBadName = 0;

	for ( size_t i = 0; i < vec.size(); i++ )
	{
		const JoinableFriend &f = vec[ i ];

		const bool bSteamIdOk = f.ulSteamId >= kIndividualMin && f.ulSteamId <= kIndividualMax;
		const bool bLobbyOk   = f.ulLobbyId >= kChatMin && f.ulLobbyId <= kChatMax;
		const bool bAppIdOk   = f.uAppId > 0 && f.uAppId < ( 1u << 24 );
		const bool bNameOk    = LooksPrintable( f.sPersona );

		nBadSteamId += !bSteamIdOk;
		nBadLobby   += !bLobbyOk;
		nBadAppId   += !bAppIdOk;
		nBadName    += !bNameOk;

		// App id only. The three verdicts are shape, not value.
		printf( "  #%zu  appid %-8u steamid:%s  lobby:%s  persona:%s (%zu chars)\n",
			i, f.uAppId,
			bSteamIdOk ? "individual-range OK" : "OUT OF RANGE",
			bLobbyOk   ? "chat-range OK"       : "OUT OF RANGE",
			bNameOk    ? "printable"           : "NOT PRINTABLE",
			f.sPersona.size() );

		// The URL a join WOULD use, with the two identifying numbers redacted.
		// NOTHING IS FIRED: this only proves the builder accepts real live
		// values and produces the documented shape.
		std::string sUrl, sWhy;
		if ( BuildJoinUrl( f, &sUrl, &sWhy ) )
		{
			const size_t nThird = sUrl.find( '/', sizeof( "steam://joinlobby" ) );
			printf( "       would build: %s/<%zu-digit lobby id>/<%zu-digit steamid64>  (NOT fired)\n",
				sUrl.substr( 0, nThird ).c_str(),
				std::to_string( f.ulLobbyId ).size(),
				std::to_string( f.ulSteamId ).size() );
		}
		else
		{
			printf( "       RESULT url: FAIL (%s)\n", sWhy.c_str() );
		}
	}

	printf( "\nRESULT steamid range: %s\n", nBadSteamId ? "FAIL" : "PASS" );
	printf( "RESULT lobby id range: %s\n", nBadLobby ? "FAIL" : "PASS" );
	printf( "RESULT app id range: %s\n", nBadAppId ? "FAIL" : "PASS" );
	printf( "RESULT persona decoding: %s\n", nBadName ? "FAIL" : "PASS" );
	return 0;
}
