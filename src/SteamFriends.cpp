// "Join a friend" -- the runtime half. See SteamFriends.h for the rules this
// file has to keep, SteamFriendsCmd.h for the pure ones a test can hold, and
// superdoc/planning/steam-friends-join.md for the investigation that settled
// the design (and for the probe, §6c, that proved on this machine that the
// library below loads and every symbol it needs resolves).
//
// THIS FILE OPENS NO SOCKET. It reads a local library, talks to a local Steam
// over ~/.steam/steam.pipe, and spawns `steam <url>` for a join. Until
// 2026-09-09 it also fetched game names over HTTP; the list is now scoped to
// the game this session is already running, so there was nothing left to look
// up, and that request -- the compositor's only one -- was deleted along with
// src/SteamAppNames.h.

#include "SteamFriends.h"
#include "SteamFriendsCmd.h"

#include "Utils/Process.h"
#include "convar.h"
#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>

static LogScope friends_log( "friends" );

namespace gamescope::steamfriends
{
	namespace
	{
		// =====================================================================
		//  The ABI we declare -- and nothing else
		// =====================================================================
		// NO VALVE SDK HEADERS ARE VENDORED. Everything below is declared here
		// from the published interface shape, which is also what keeps a
		// licensing question out of this repo (§6a).
		//
		// The version STRING is what makes hand-declaring a vtable a pinned
		// contract rather than a guess: steamclient.so keeps every interface it
		// has ever shipped alive side by side, so a caller asking for
		// "SteamFriends017" gets the layout that string has always named.
		// SteamFriendsCmd.h's kFriendsVersions is the closed list of the ones
		// whose leading slots are the ones declared here.
		//
		// AND THE LAYOUT BELOW IS MEASURED, NOT QUOTED. Read the long comment on
		// SteamFriendsVTable before touching any of it: the published order that
		// every write-up repeats is one slot out against the real client, and
		// the way that failed was silent.

		using HSteamPipe = int32_t;
		using HSteamUser = int32_t;

		// ISteamFriends::GetFriendGamePlayed fills this. m_steamIDLobby is the
		// entire feature: non-zero exactly when the friend is in a lobby you
		// can join.
		struct FriendGameInfo_t
		{
			uint64_t m_gameID;
			uint32_t m_unGameIP;
			uint16_t m_usGamePort;
			uint16_t m_usQueryPort;
			uint64_t m_steamIDLobby;
		};

		struct ISteamClient;
		struct ISteamFriends;

		// The published ISteamClient order. Only the entries we call are named;
		// the rest are present so the indices line up.
		struct SteamClientVTable
		{
			HSteamPipe ( *CreateSteamPipe )( ISteamClient * );
			bool       ( *BReleaseSteamPipe )( ISteamClient *, HSteamPipe );
			HSteamUser ( *ConnectToGlobalUser )( ISteamClient *, HSteamPipe );
			HSteamUser ( *CreateLocalUser )( ISteamClient *, HSteamPipe *, int );
			void       ( *ReleaseUser )( ISteamClient *, HSteamPipe, HSteamUser );
			void      *( *GetISteamUser )( ISteamClient *, HSteamUser, HSteamPipe, const char * );
			void      *( *GetISteamGameServer )( ISteamClient *, HSteamUser, HSteamPipe, const char * );
			void       ( *SetLocalIPBinding )( ISteamClient *, const void *, uint16_t );
			void      *( *GetISteamFriends )( ISteamClient *, HSteamUser, HSteamPipe, const char * );
		};

		struct ISteamClient { const SteamClientVTable *vt; };

		// ISteamFriends, MEASURED -- not copied from the published order.
		//
		// THIS IS THE MOST IMPORTANT COMMENT IN THE FILE. The published
		// ISteamFriends order that everybody quotes begins
		//
		//     0 GetPersonaName, 1 SetPersonaName, 2 GetPersonaState,
		//     3 GetFriendCount, 4 GetFriendByIndex, ...
		//
		// and that is what the feasibility study's steamclient_probe.c declared.
		// AGAINST THE REAL, RUNNING CLIENT IT IS WRONG BY ONE SLOT, and the
		// symptom is not a crash -- it is a friends list that is silently and
		// permanently EMPTY, which reads exactly like "nobody is in a joinable
		// game". Measured on SteamFriends018 with
		// tests/steam_friends_live_probe --diagnose:
		//
		//     slot 2 called as GetFriendCount(flags) -> 75 for Immediate, 140
		//       for 0xFFFF, 1 for FriendshipRequested. Those vary with the
		//       flag, so slot 2 IS GetFriendCount.
		//     slot 3 called as GetFriendCount(flags) -> 0 for every flag.
		//     slot 3 called as GetFriendByIndex(0, Immediate) -> an id inside
		//       the individual-account SteamID64 band.
		//     slot 4 called the same way -> not a SteamID at all.
		//
		// So everything from GetFriendCount onward sits one slot earlier than
		// the published order says, and this struct is the measurement, not the
		// documentation.
		//
		// SLOT 1 IS DELIBERATELY OPAQUE AND MUST NEVER BE CALLED. Under the
		// published order it is SetPersonaName -- a WRITE, on the user's live
		// account. We do not need it, we cannot prove what it is without
		// calling it, and "call it and see" is not available for a setter. Same
		// for slot 0. Leaving them as void* rather than as named function
		// pointers is what makes calling them impossible by accident.
		//
		// `Why this does not make the whole approach unsafe:` the guard is no
		// longer only the version string. Snapshot() now RANGE-CHECKS the first
		// SteamID it gets back, so a client whose layout moves again produces
		// one named log line and an empty list instead of nonsense -- see
		// LooksLikeIndividualSteamId() below.
		struct SteamFriendsVTable
		{
			void       *_Slot0_GetPersonaName;      // never called
			void       *_Slot1_MAY_BE_A_SETTER;     // NEVER CALL THIS
			int         ( *GetFriendCount )( ISteamFriends *, int iFriendFlags );
			uint64_t    ( *GetFriendByIndex )( ISteamFriends *, int iFriend, int iFriendFlags );
			void       *_Slot4_GetFriendRelationship;
			void       *_Slot5_GetFriendPersonaState;
			const char *( *GetFriendPersonaName )( ISteamFriends *, uint64_t );
			bool        ( *GetFriendGamePlayed )( ISteamFriends *, uint64_t, FriendGameInfo_t * );
		};

		struct ISteamFriends { const SteamFriendsVTable *vt; };

		using CreateInterface_t = void *( * )( const char *, int * );

		// k_EFriendFlagImmediate: the actual friends list, not blocked users,
		// clan members or the "recently played with" set.
		constexpr int kFriendFlagImmediate = 0x04;

		// A stuck loop over a hostile friend count must not hang the caller.
		constexpr int kMaxFriends = 4096;

		// A SteamID64 for an INDIVIDUAL account is universe 1, type 1,
		// instance 1: 0x0110000100000000 + a 32-bit account id, i.e. a narrow
		// band 4 billion wide out of a 64-bit space.
		//
		// `Why this check is in the shipping code and not only in a test:` the
		// vtable comment above is what happens when a hand-declared layout is
		// one slot out -- an empty list, forever, with nothing to see. This
		// turns the next such drift into a named failure on the FIRST id that
		// comes back wrong. It costs two comparisons per friend.
		constexpr uint64_t kIndividualSteamIdMin = 76561197960265728ull;
		constexpr uint64_t kIndividualSteamIdMax = 76561197960265728ull + 0xFFFFFFFFull;

		constexpr bool LooksLikeIndividualSteamId( uint64_t ul )
		{
			return ul >= kIndividualSteamIdMin && ul <= kIndividualSteamIdMax;
		}

		// A lobby id is a SteamID of chat type in the public universe, a band
		// that does not overlap the individual one above.
		//
		// `Why this only COUNTS and never filters:` m_gameID's offset in
		// FriendGameInfo_t is confirmed live (every in-game friend's app id came
		// back as a real Steam app id), but m_steamIDLobby's is not -- at the
		// moment it was measured, nobody in the friends list was in a lobby, so
		// "reads as zero" is all that could be established and a wrong offset
		// into padding would read as zero too. Turning an unverified guess into
		// a FILTER would silently hide exactly the friends this feature exists
		// to show, which is worse than the failed join it would prevent. So it
		// rides along in the debug line until somebody sees a real lobby, and
		// the doc says so.
		constexpr bool LooksLikeLobbyId( uint64_t ul )
		{
			return ul >= 0x0170000000000000ull && ul <= 0x0190000000000000ull;
		}

		// =====================================================================
		//  Why the feature is (or is not) answering
		// =====================================================================
		// One reason, one sentence, and the only thing that ever reaches a log
		// line or the status row. Deliberately carries no id, no name and no
		// count of anything a friend controls.
		enum class Reason
		{
			Ok,
			NoAppId,        // this session is not a Steam game -- nothing to join
			NoLibrary,      // no Steam installed here, or it is somewhere we do not look
			NotSteam,       // a library loaded, but it is not Steam's client
			NoInterface,    // a Steam client whose interface versions we do not know
			NotRunning,     // Steam is installed but not running
			SignedOut,      // running, but nobody is logged in
			WrongShape,     // it answered, but not with the values this build expects
		};

		const char *ReasonText( Reason e )
		{
			switch ( e )
			{
			case Reason::Ok:          return "";
			case Reason::NoAppId:     return "this isn't a Steam game, so there's nobody here to join.";
			case Reason::NoLibrary:   return "Steam isn't installed here.";
			case Reason::NotSteam:    return "the Steam client library isn't the one we know.";
			case Reason::NoInterface: return "this Steam client's interfaces are newer than this build knows.";
			case Reason::NotRunning:  return "Steam isn't running.";
			case Reason::SignedOut:   return "Steam is signed out.";
			case Reason::WrongShape:  return "this Steam client's friends interface isn't laid out the way this build expects.";
			}
			return "";
		}

		// =====================================================================
		//  State
		// =====================================================================
		std::mutex g_Mutex;

		void      *g_pLibrary        = nullptr;
		CreateInterface_t g_pfnCreateInterface = nullptr;
		bool       g_bLoadAttempted  = false;

		Reason     g_eLastReason     = Reason::Ok;
		bool       g_bReasonLogged   = false;
		size_t     g_nLastJoinable   = 0;
		size_t     g_nLastInThisGame = 0;

		// THE ONE FACT THE WHOLE FEATURE IS SCOPED BY, and an atomic rather
		// than a config::SessionAppId() call for the reason SteamFriends.h
		// gives: that getter resolves lazily into an unguarded static, and this
		// is read on the POLLER thread. Seeded once from
		// PanelFriends_SeedFromConfig(), on the main thread, before any Steam
		// call this process ever makes.
		std::atomic<uint32_t> g_uSessionAppId{ 0 };

		// The last `steam <url>` forwarder. It exits within a moment of being
		// started, but gamescope is a subreaper, so somebody has to collect it:
		// the next call through this file does, with one non-blocking waitpid.
		// At most one is ever outstanding, and teardown collects the rest.
		pid_t      g_nLastJoinPid    = -1;

		// Log a reason exactly ONCE per distinct reason. Re-logs if the
		// situation changes (Steam started, then was signed out), which is the
		// behaviour that is actually useful in a log, and never repeats a line
		// once per poll -- the panel polls this every few seconds.
		void SetReason( Reason e )
		{
			if ( e != g_eLastReason )
			{
				g_eLastReason = e;
				g_bReasonLogged = false;
			}
			if ( e != Reason::Ok && !g_bReasonLogged )
			{
				g_bReasonLogged = true;
				friends_log.infof( "the join list is unavailable: %s", ReasonText( e ) );
			}
		}

		// =====================================================================
		//  Loading the library
		// =====================================================================
		// WHERE. The client's own steamclient.so, which -- unlike the
		// per-game libsteam_api.so redistributable -- is guaranteed to be part
		// of any Steam installation (§6a). GS_RITZ_STEAMCLIENT overrides the
		// search, which is how tests/test_steam_friends.cpp exercises every
		// failure path below with no Steam present at all.
		//
		// WE NEVER dlclose() IT. steamclient.so starts threads and registers
		// atexit handlers when it loads; unloading it out from under those is
		// how a compositor dies minutes later in a stack that has nothing to do
		// with this file. The handle is kept for the life of the process, and
		// "not loaded yet" is the only state this ever leaves.
		std::vector<std::string> CandidatePaths()
		{
			if ( const char *pszOverride = getenv( "GS_RITZ_STEAMCLIENT" ); pszOverride && *pszOverride )
				return { pszOverride };

			const char *pszHome = getenv( "HOME" );
			if ( !pszHome || !*pszHome )
				return {};

			const std::string sHome( pszHome );
			return {
				sHome + "/.steam/steam/linux64/steamclient.so",
				sHome + "/.steam/root/linux64/steamclient.so",
				sHome + "/.local/share/Steam/linux64/steamclient.so",
			};
		}

		// The fingerprint. These six are the exact set the feasibility probe
		// resolved on this machine (§6c's results-steamclient.txt), and this
		// file deliberately calls NONE of them -- every call goes through the
		// version-pinned vtables instead, whose layout is published.
		//
		// `Why check for symbols we do not call:` it is the cheap proof that
		// the thing we just dlopened really is Steam's client library and not
		// merely something else that happens to export a `CreateInterface`
		// (plenty of engines do). Without it, a wrong GS_RITZ_STEAMCLIENT or a
		// flatpak layout change would end with us casting a stranger's pointer
		// to a vtable and calling through it. With it, that is one log line.
		constexpr const char *kFingerprintSymbols[] = {
			"Steam_CreateSteamPipe",
			"Steam_ConnectToGlobalUser",
			"Steam_BConnected",
			"Steam_BLoggedOn",
			"Steam_BReleaseSteamPipe",
			"Steam_ReleaseUser",
		};

		// Returns false and sets the reason; caller must hold g_Mutex.
		bool EnsureLibrary()
		{
			if ( g_pfnCreateInterface )
				return true;
			if ( g_bLoadAttempted )
				return false;    // the reason is already set, and already logged
			g_bLoadAttempted = true;

			for ( const std::string &sPath : CandidatePaths() )
			{
				void *pLib = dlopen( sPath.c_str(), RTLD_LAZY | RTLD_LOCAL );
				if ( !pLib )
					continue;

				auto pfn = (CreateInterface_t)dlsym( pLib, "CreateInterface" );
				bool bFingerprint = pfn != nullptr;
				for ( const char *pszSym : kFingerprintSymbols )
				{
					if ( !dlsym( pLib, pszSym ) )
						bFingerprint = false;
				}

				if ( !bFingerprint )
				{
					// Not dlclose()d -- see this section's comment. Leaving one
					// wrong library mapped costs address space and nothing else.
					SetReason( Reason::NotSteam );
					return false;
				}

				g_pLibrary = pLib;
				g_pfnCreateInterface = pfn;
				friends_log.infof( "loaded the Steam client library." );
				return true;
			}

			SetReason( Reason::NoLibrary );
			return false;
		}

		void ReapJoinChild()
		{
			if ( g_nLastJoinPid <= 0 )
				return;
			if ( waitpid( g_nLastJoinPid, nullptr, WNOHANG ) == g_nLastJoinPid )
				g_nLastJoinPid = -1;
		}
	}

	void SetSessionAppId( uint32_t uAppId )
	{
		// NO LOCK, DELIBERATELY: this is called from the main thread at
		// startup, and g_Mutex is held for a whole Steam round trip. One
		// relaxed store into an atomic cannot be behind a wedged Steam.
		g_uSessionAppId.store( uAppId, std::memory_order_relaxed );
	}

	uint32_t SessionAppId()
	{
		return g_uSessionAppId.load( std::memory_order_relaxed );
	}

	// =========================================================================
	//  The snapshot
	// =========================================================================
	// Pipe and user are created and released PER CALL rather than cached.
	// `Why:` a cached pipe is a handle into a process we do not control, and
	// Steam restarting (or being killed) leaves it pointing at nothing -- which
	// is exactly the shape of a crash that would land in the compositor. A
	// create/release pair costs a round trip to a local process, this runs at
	// human speed, and re-doing it every call is also what makes "Steam went
	// away" a fresh, correct answer instead of a stale one.
	//
	// WHAT COMES BACK (2026-09-09): the friends who are in THIS session's game
	// AND joinable, and nothing else. The count of friends in this game who are
	// NOT joinable is kept separately, for the status line -- see
	// SteamFriendsCmd.h's StatusLine() for why that second number is not
	// optional.
	//
	// EVERY EXIT FROM THIS FUNCTION IS AN EMPTY VECTOR PLUS ONE LOGGED REASON.
	// There is no throw, no fatal path, and no blocking wait anywhere in it.
	std::vector<Friend> Snapshot()
	{
		std::scoped_lock lock( g_Mutex );
		ReapJoinChild();

		g_nLastJoinable   = 0;
		g_nLastInThisGame = 0;

		// NO APP ID, NO LIST -- and, importantly, no dlopen either. A gamescope
		// that was not launched by Steam never loads Steam's 46 MB client
		// library just to be told there is nobody to join.
		const uint32_t uSessionAppId = g_uSessionAppId.load( std::memory_order_relaxed );
		if ( uSessionAppId == 0 )
		{
			SetReason( Reason::NoAppId );
			return {};
		}

		if ( !EnsureLibrary() )
			return {};

		int nIgnored = 0;
		ISteamClient *pClient = nullptr;
		std::string_view svClientVersion;
		for ( std::string_view svVersion : kClientVersions )
		{
			pClient = (ISteamClient *)g_pfnCreateInterface( std::string( svVersion ).c_str(), &nIgnored );
			if ( pClient )
			{
				svClientVersion = svVersion;
				break;
			}
		}
		if ( !pClient || !pClient->vt )
		{
			SetReason( Reason::NoInterface );
			return {};
		}

		// No app id is supplied anywhere below, so nothing registers a game and
		// the "your friends see you playing something you are not" problem
		// cannot happen (§6a, the whole reason for route B). The session app id
		// above is used only to FILTER what comes back; it is never handed to
		// Steam.
		const HSteamPipe hPipe = pClient->vt->CreateSteamPipe( pClient );
		if ( !hPipe )
		{
			SetReason( Reason::NotRunning );
			return {};
		}

		const HSteamUser hUser = pClient->vt->ConnectToGlobalUser( pClient, hPipe );
		if ( !hUser )
		{
			pClient->vt->BReleaseSteamPipe( pClient, hPipe );
			SetReason( Reason::SignedOut );
			return {};
		}

		ISteamFriends *pFriends = nullptr;
		std::string_view svFriendsVersion;
		for ( std::string_view svVersion : kFriendsVersions )
		{
			pFriends = (ISteamFriends *)pClient->vt->GetISteamFriends(
				pClient, hUser, hPipe, std::string( svVersion ).c_str() );
			if ( pFriends )
			{
				svFriendsVersion = svVersion;
				break;
			}
		}

		std::vector<Friend> vecOut;
		Reason eReason = Reason::NoInterface;
		int nFriends = 0, nInAnyGame = 0, nInThisGame = 0, nOddLobby = 0;

		if ( pFriends && pFriends->vt )
		{
			eReason = Reason::Ok;

			nFriends = pFriends->vt->GetFriendCount( pFriends, kFriendFlagImmediate );
			for ( int i = 0; i < nFriends && i < kMaxFriends; i++ )
			{
				const uint64_t ulSteamId = pFriends->vt->GetFriendByIndex( pFriends, i, kFriendFlagImmediate );
				if ( !ulSteamId )
					continue;

				// THE LAYOUT TRIPWIRE. The vtable comment above is what happens
				// when a hand-declared layout is one slot out; this is what
				// makes the next such drift loud. A number here that is not a
				// SteamID means we are not calling GetFriendByIndex any more,
				// so the whole read is abandoned rather than half-believed --
				// keeping going would hand GetFriendGamePlayed a garbage id.
				if ( !LooksLikeIndividualSteamId( ulSteamId ) )
				{
					eReason = Reason::WrongShape;
					vecOut.clear();
					nInThisGame = 0;
					break;
				}

				FriendGameInfo_t info{};
				if ( !pFriends->vt->GetFriendGamePlayed( pFriends, ulSteamId, &info ) )
					continue;
				nInAnyGame++;

				// THE TWO PREDICATES, AND THE ORDER THEY ARE APPLIED IN.
				// Everyone in THIS game is counted -- that count is what makes
				// an empty list diagnosable (StatusLine()) -- and only the
				// joinable ones become rows.
				if ( !InThisGame( info.m_gameID, uSessionAppId ) )
					continue;
				nInThisGame++;

				if ( !IsJoinable( info.m_gameID, info.m_steamIDLobby, ulSteamId ) )
					continue;

				if ( !LooksLikeLobbyId( info.m_steamIDLobby ) )
					nOddLobby++;

				Friend f;
				f.uAppId    = AppIdFromGameId( info.m_gameID );
				f.ulLobbyId = info.m_steamIDLobby;
				f.ulSteamId = ulSteamId;
				if ( const char *pszName = pFriends->vt->GetFriendPersonaName( pFriends, ulSteamId ) )
					f.sPersona = pszName;

				vecOut.push_back( std::move( f ) );
			}
		}

		pClient->vt->ReleaseUser( pClient, hPipe, hUser );
		pClient->vt->BReleaseSteamPipe( pClient, hPipe );

		// The shape of what came back, and which pinned interface answered.
		// Counts and version strings only -- no name, no id, nothing that says
		// WHO. `Why debugf and not infof:` this is polled every few seconds,
		// and a line per poll is a log nobody can read. This is the line
		// `log_friends debug` turns on when something needs diagnosing.
		friends_log.debugf( "%d friends, %d in a game, %d in app %u, %zu joinable, %d with an odd lobby id (%.*s / %.*s)",
			nFriends, nInAnyGame, nInThisGame, uSessionAppId, vecOut.size(), nOddLobby,
			(int)svClientVersion.size(), svClientVersion.data(),
			(int)svFriendsVersion.size(), svFriendsVersion.data() );

		// THE ORDER, and it is applied HERE rather than in the panel so every
		// reader agrees: the panel, `friends_dump`, `friends_join <n>`'s
		// indices and the live probe all see one list in one order. A panel
		// that sorted its own copy would make friends_dump's index numbers
		// mean something different from the numbers on screen.
		//
		// FriendOrderLess() is a total order over the data (SteamFriendsCmd.h),
		// so an unchanged friends list sorts to the identical sequence every
		// poll -- which is what stops the rows shuffling under a user who is
		// reading them.
		std::sort( vecOut.begin(), vecOut.end(), FriendOrderLess );

		SetReason( eReason );
		g_nLastInThisGame = (size_t)nInThisGame;
		g_nLastJoinable   = vecOut.size();
		return vecOut;
	}

	// =========================================================================
	//  The join
	// =========================================================================
	bool Join( const Friend &friendToJoin, std::string *psError )
	{
		auto Fail = [ & ]( std::string sWhy ) {
			if ( psError ) *psError = std::move( sWhy );
			return false;
		};

		std::string sUrl;
		std::string sWhy;
		if ( !BuildJoinUrl( friendToJoin, &sUrl, &sWhy ) )
			return Fail( std::move( sWhy ) );

		if ( !Process::ExecutableExists( std::string( kSteamProgram ) ) )
			return Fail( "\"steam\" isn't installed (or isn't on PATH)." );

		std::scoped_lock lock( g_Mutex );
		ReapJoinChild();

		std::vector<std::string> vecArgs = BuildJoinArgv( sUrl );
		std::vector<char *> vecArgv;
		vecArgv.reserve( vecArgs.size() + 1 );
		for ( std::string &s : vecArgs )
			vecArgv.push_back( s.data() );
		vecArgv.push_back( nullptr );

		// Its own process group so it can never be signalled as part of ours
		// by accident, and PR_SET_PDEATHSIG so a gamescope that is killed
		// outright takes it with it. This child only forwards the URL over
		// ~/.steam/steam.pipe and exits, so nothing here waits for it -- the
		// compositor is never blocked by a join.
		const pid_t nPid = Process::SpawnProcess( vecArgv.data(), []()
			{
				setpgid( 0, 0 );
				Process::SetDeathSignal( SIGTERM );
			} );

		if ( nPid <= 0 )
			return Fail( "Couldn't start \"steam\" to hand it the join." );

		g_nLastJoinPid = nPid;

		// NO IDS, NO NAME. An app id is not personal; a SteamID64 plus a lobby
		// id is enough for anyone reading a pasted log to identify and join a
		// stranger's friend, and a persona name identifies a real person.
		friends_log.infof( "asked Steam to join a lobby in app %u.", friendToJoin.uAppId );
		return true;
	}

	std::string StatusText()
	{
		std::scoped_lock lock( g_Mutex );

		if ( g_eLastReason != Reason::Ok )
			return ReasonText( g_eLastReason );
		return StatusLine( g_nLastInThisGame, g_nLastJoinable );
	}

	// =========================================================================
	//  The poller
	// =========================================================================
	// See SteamFriends.h's poller section for the contract. What is here is
	// the mechanism, and it is deliberately the smallest one that keeps the
	// promise: ONE thread, ONE published View, ONE condition variable.
	//
	// THE PROPERTY THAT MATTERS: nothing on the frame path ever waits for
	// Steam. CurrentView() takes g_ViewMutex -- which is held only for the
	// microseconds it takes to copy a vector of a handful of rows -- and never
	// g_Mutex, which is the lock Snapshot() holds for the whole round trip to
	// the Steam client. The two locks cannot be the same lock, or a wedged
	// client would stall the caller of CurrentView() exactly as if the panel
	// had called Snapshot() itself; that is the bug this split exists to make
	// unrepresentable, and tests/test_steam_friends.cpp pins it with a stub
	// that sleeps.
	namespace
	{
		// Presence does not change faster than a human reads a list, and each
		// poll is a round trip to another process.
		constexpr auto kPollInterval  = std::chrono::seconds( 3 );
		// How long after the last CurrentView() the worker keeps polling. One
		// interval of slack, so a panel redrawn every frame never sees a gap
		// and a closed panel stops the polling within a few seconds.
		constexpr auto kInterestWindow = std::chrono::seconds( 8 );

		std::mutex              g_ViewMutex;      // guards g_View ONLY
		View                    g_View;
		std::chrono::steady_clock::time_point g_tPolled{};

		std::mutex              g_PollMutex;      // guards the three below
		std::condition_variable g_PollCv;
		bool                    g_bStop      = false;
		bool                    g_bRefreshNow = false;
		std::chrono::steady_clock::time_point g_tWanted{};

		std::thread             g_Thread;
		std::once_flag          g_OnceStart;

		// The status sentence and the "friends in this game" count, read under
		// ONE lock. `Why together:` they are two views of the same poll, and
		// two separate locked reads could straddle a console-driven Snapshot()
		// and publish a sentence from one poll beside a number from another --
		// which is precisely the pairing the status row exists to be trusted
		// about.
		void LastStatusAndCount( std::string *psStatus, size_t *pnInThisGame )
		{
			std::scoped_lock lock( g_Mutex );
			*psStatus = ( g_eLastReason != Reason::Ok )
				? std::string( ReasonText( g_eLastReason ) )
				: StatusLine( g_nLastInThisGame, g_nLastJoinable );
			*pnInThisGame = g_nLastInThisGame;
		}

		void PublishView( std::vector<Friend> vec, std::string sStatus, size_t nInThisGame )
		{
			std::scoped_lock lock( g_ViewMutex );
			g_View.vecFriends  = std::move( vec );
			g_View.sStatus     = std::move( sStatus );
			g_View.nInThisGame = nInThisGame;
			g_View.bPolled     = true;
			g_tPolled          = std::chrono::steady_clock::now();
		}

		void PollOnce()
		{
			// Snapshot() takes g_Mutex; LastStatusAndCount() takes it again
			// after. Neither is g_ViewMutex, so the copy below is the only
			// thing a CurrentView() caller can ever be behind.
			std::vector<Friend> vec = Snapshot();
			std::string sStatus;
			size_t nInThisGame = 0;
			LastStatusAndCount( &sStatus, &nInThisGame );
			PublishView( std::move( vec ), std::move( sStatus ), nInThisGame );
		}

		void PollLoop()
		{
			pthread_setname_np( pthread_self(), "gs-friends" );
			for ( ;; )
			{
				bool bPoll = false;
				{
					std::unique_lock lock( g_PollMutex );
					// Sleep until: told to stop, told to refresh, or the
					// interval elapsed while somebody is still looking.
					g_PollCv.wait_for( lock, kPollInterval, []
						{
							return g_bStop || g_bRefreshNow;
						} );
					if ( g_bStop )
						return;
					const bool bWanted = std::chrono::steady_clock::now() - g_tWanted < kInterestWindow;
					bPoll = g_bRefreshNow || bWanted;
					g_bRefreshNow = false;

					if ( !bPoll )
					{
						// Nobody is looking. Sleep indefinitely rather than
						// spinning a Steam round trip every three seconds for
						// a panel that is not on screen.
						g_PollCv.wait( lock, []
							{
								return g_bStop || g_bRefreshNow ||
								       std::chrono::steady_clock::now() - g_tWanted < kInterestWindow;
							} );
						if ( g_bStop )
							return;
						g_bRefreshNow = false;
					}
				}
				PollOnce();
			}
		}

		// Records the ask and starts the thread on the first one. `Why the
		// thread is started lazily and never at boot:` dlopening Steam's 46 MB
		// client library is the first thing a poll does, and a build whose
		// friends panel is never opened must never pay for it (SteamFriends.h's
		// first rule).
		void ArmPoller()
		{
			{
				std::scoped_lock lock( g_PollMutex );
				g_tWanted = std::chrono::steady_clock::now();
			}
			std::call_once( g_OnceStart, []
				{
					// The FIRST poll happens at once rather than one interval
					// later: a panel that opened to "asking Steam..." and then
					// sat there for three seconds would read as broken.
					{
						std::scoped_lock lock( g_PollMutex );
						g_bRefreshNow = true;
					}
					g_Thread = std::thread( PollLoop );
				} );
			g_PollCv.notify_all();
		}
	}

	View CurrentView()
	{
		ArmPoller();

		std::scoped_lock lock( g_ViewMutex );
		View v = g_View;
		if ( v.bPolled )
		{
			v.flAgeSec = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - g_tPolled ).count();
		}
		else
		{
			// Before the first poll finishes there is nothing true to say
			// about Steam, so say what is actually happening instead of
			// "nobody else is in this game", which would be a guess presented
			// as a fact.
			v.sStatus = "asking Steam...";
		}
		return v;
	}

	void RequestRefresh()
	{
		ArmPoller();
		{
			std::scoped_lock lock( g_PollMutex );
			g_bRefreshNow = true;
		}
		g_PollCv.notify_all();
	}

	// `Why this JOINS rather than detaches:` a detached worker would outlive
	// the statics it writes into (g_View owns a vector), which is a crash at
	// exit rather than a clean one. The cost is that a Steam that has stopped
	// answering can delay gamescope's exit by up to one round trip -- an exit,
	// never a frame, and the alternative is undefined behaviour.
	void Shutdown()
	{
		{
			std::scoped_lock lock( g_PollMutex );
			g_bStop = true;
		}
		g_PollCv.notify_all();
		if ( g_Thread.joinable() )
			g_Thread.join();

		// The stop flag is CLEARED again once the worker is gone, so this
		// function leaves no trace in the process it ran in. That matters for
		// exactly one caller -- tests/test_steam_friends.cpp forks children
		// out of a parent that has already run this -- and a Shutdown() that
		// permanently poisoned the flag would silently stop every forked
		// child's poller from ever starting, which is a test that passes for
		// the wrong reason. Nothing restarts here: g_OnceStart is still
		// consumed, so a CurrentView() after a shutdown answers from the last
		// published view and starts no thread.
		std::scoped_lock lock( g_PollMutex );
		g_bStop = false;
	}

	// =========================================================================
	//  The console surface
	// =========================================================================
	// Phases 1 and 2's only way in, and still the way a headless check drives
	// the feature without a screenshot. The panel (phase 3) and the hotkey
	// (phase 4) are on top of the same two calls, not beside them.
	namespace
	{
		// WHAT THIS PRINTS, AND WHAT IT DELIBERATELY DOES NOT.
		//
		// Counts, indices and app ids. No persona names, no SteamIDs, no lobby
		// ids -- the same rule the committed probe output follows (§6c), and
		// for a stronger reason here: these lines go through the overlay's log
		// capture into a file users paste into bug reports. An app id says
		// "somebody is in CS2", which is not a fact about a person; a SteamID64
		// names one, and a lobby id lets a reader walk into their game.
		//
		// The index is what makes that sufficient rather than merely safe:
		// `friends_join <n>` takes the number from this listing, so a user
		// never has to see -- or retype -- an id to use the feature at all.
		static ConCommand cc_friends_dump(
			"friends_dump",
			"List the friends who are in THIS game right now and can be joined, by index. "
			"Prints counts and app ids only -- never names, Steam IDs or lobby ids. Use "
			"friends_join <n> to act on a line.",
			[]( std::span<std::string_view> )
			{
				const std::vector<Friend> vec = Snapshot();
				console_log.infof( "friends: %s", StatusText().c_str() );
				console_log.infof( "  this session's app id: %u", SessionAppId() );
				for ( size_t i = 0; i < vec.size(); i++ )
					console_log.infof( "  #%zu  appid %-8u joinable", i, vec[ i ].uAppId );
			} );

		static ConCommand cc_friends_join(
			"friends_join",
			"Join a friend's lobby: friends_join <n>, where <n> is an index from friends_dump. "
			"Hands the running Steam client a steam://joinlobby URL.",
			[]( std::span<std::string_view> args )
			{
				if ( args.size() < 2 )
				{
					console_log.errorf( "usage: friends_join <n>   (see friends_dump)" );
					return;
				}
				const std::optional<uint32_t> oIndex = Parse<uint32_t>( args[ 1 ] );
				if ( !oIndex )
				{
					console_log.errorf( "friends_join takes an index from friends_dump." );
					return;
				}

				const std::vector<Friend> vec = Snapshot();
				if ( *oIndex >= vec.size() )
				{
					console_log.errorf( "there is no #%u right now: %s", *oIndex, StatusText().c_str() );
					return;
				}

				std::string sWhy;
				if ( !Join( vec[ *oIndex ], &sWhy ) )
				{
					console_log.errorf( "%s", sWhy.c_str() );
					return;
				}
				console_log.infof( "joining #%u (app %u).", *oIndex, vec[ *oIndex ].uAppId );
			} );
	}
}
