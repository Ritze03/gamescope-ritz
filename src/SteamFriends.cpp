// "Join a friend" -- the runtime half. See SteamFriends.h for the rules this
// file has to keep, SteamFriendsCmd.h for the pure ones a test can hold, and
// superdoc/planning/steam-friends-join.md for the investigation that settled
// the design (and for the probe, §6c, that proved on this machine that the
// library below loads and every symbol it needs resolves).

#include "SteamFriends.h"
#include "SteamFriendsCmd.h"

#include "Utils/Process.h"
#include "convar.h"
#include "log.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
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

		// The last `steam <url>` forwarder. It exits within a moment of being
		// started, but gamescope is a subreaper, so somebody has to collect it:
		// the next call through this file does, with one non-blocking waitpid.
		// At most one is ever outstanding, and teardown collects the rest.
		pid_t      g_nLastJoinPid    = -1;

		// Log a reason exactly ONCE per distinct reason. Re-logs if the
		// situation changes (Steam started, then was signed out), which is the
		// behaviour that is actually useful in a log, and never repeats a line
		// once per poll -- phase 3 polls this every few seconds.
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
	// EVERY EXIT FROM THIS FUNCTION IS AN EMPTY VECTOR PLUS ONE LOGGED REASON.
	// There is no throw, no fatal path, and no blocking wait anywhere in it.
	std::vector<JoinableFriend> Snapshot()
	{
		std::scoped_lock lock( g_Mutex );
		ReapJoinChild();

		g_nLastJoinable = 0;

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
		// cannot happen (§6a, the whole reason for route B).
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

		std::vector<JoinableFriend> vecOut;
		Reason eReason = Reason::NoInterface;
		int nFriends = 0, nInGame = 0, nOddLobby = 0;

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
					break;
				}

				FriendGameInfo_t info{};
				if ( !pFriends->vt->GetFriendGamePlayed( pFriends, ulSteamId, &info ) )
					continue;
				nInGame++;

				if ( !IsJoinable( info.m_gameID, info.m_steamIDLobby, ulSteamId ) )
					continue;

				if ( !LooksLikeLobbyId( info.m_steamIDLobby ) )
					nOddLobby++;

				JoinableFriend f;
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
		// WHO. `Why debugf and not infof:` phase 3 polls this every few
		// seconds, and a line per poll is a log nobody can read. This is the
		// line `log_friends debug` turns on when something needs diagnosing,
		// and it is what the phase 1/2 live check read.
		friends_log.debugf( "%d friends, %d in a game, %d joinable, %d with an odd lobby id (%.*s / %.*s)",
			nFriends, nInGame, (int)vecOut.size(), nOddLobby,
			(int)svClientVersion.size(), svClientVersion.data(),
			(int)svFriendsVersion.size(), svFriendsVersion.data() );

		SetReason( eReason );
		g_nLastJoinable = vecOut.size();
		return vecOut;
	}

	// =========================================================================
	//  The join
	// =========================================================================
	bool Join( const JoinableFriend &friendToJoin, std::string *psError )
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

		// The same spawn discipline as the companion browser
		// (SteamCompanion.cpp's Spawn): its own process group so it can never
		// be signalled as part of ours by accident, and PR_SET_PDEATHSIG so a
		// gamescope that is killed outright takes it with it. This child only
		// forwards the URL over ~/.steam/steam.pipe and exits, so nothing here
		// waits for it -- the compositor is never blocked by a join.
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
		if ( g_nLastJoinable == 0 )
			return "nobody's in a joinable game.";
		if ( g_nLastJoinable == 1 )
			return "1 friend you can join.";
		return std::to_string( g_nLastJoinable ) + " friends you can join.";
	}

	// =========================================================================
	//  The console surface -- phases 1 and 2's ONLY way in
	// =========================================================================
	// No hotkey and no panel yet; that is phases 3 and 4
	// (superdoc/planning/steam-friends-join.md §7b).
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
			"List the friends you could join right now, by index. Prints counts and app ids "
			"only -- never names, Steam IDs or lobby ids. Use friends_join <n> to act on a line.",
			[]( std::span<std::string_view> )
			{
				const std::vector<JoinableFriend> vec = Snapshot();
				console_log.infof( "friends: %s", StatusText().c_str() );
				for ( size_t i = 0; i < vec.size(); i++ )
					console_log.infof( "  #%zu  appid %u", i, vec[ i ].uAppId );
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

				const std::vector<JoinableFriend> vec = Snapshot();
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
