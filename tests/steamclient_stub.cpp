// A stand-in for Steam's steamclient.so, so tests/test_steam_friends.cpp can
// drive src/SteamFriends.cpp's REAL loader down every one of its failure paths
// -- and down its success path -- with no Steam installed, nothing running and
// nothing sent to anybody's live client.
//
// WHY A STUB AND NOT THE REAL LIBRARY. The failure modes this file stands in
// for (superdoc/planning/steam-friends-join.md §6d) are, by definition, states
// a working Steam is never in: an interface version we do not know, a client
// with no pipe, a signed-out user. There is no way to ask a healthy Steam to be
// broken, and the one machine this fork is developed on is usually mid-match.
//
// WHAT IT DOES AND DOES NOT PROVE. It proves the loader's CONTROL FLOW: that
// each failure ends in an empty vector and one named line, and that the
// success path unpacks CGameID and applies the joinable filter correctly. It
// CANNOT prove the vtable layout is right, because it is built from the same
// declaration -- and that is not a hypothetical caveat: the layout this stub
// originally mirrored WAS wrong by one slot, every case here passed anyway, and
// only tests/steam_friends_live_probe against the real client caught it. Layout
// is that probe's job; control flow is this one's.
//
// GS_RITZ_TEST_STEAMCLIENT_MODE selects the behaviour; see kMode below.
// Built twice: once as the Steam-shaped library, and once (-DSTUB_NOT_STEAM) as
// a library that exports CreateInterface but none of the Steam_* fingerprint
// symbols, which is the "this is not Steam's client" path.

#include <cstdint>
#include <cstdlib>
#include <cstring>

using HSteamPipe = int32_t;
using HSteamUser = int32_t;

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

// Mirrors src/SteamFriends.cpp's MEASURED layout, one slot earlier than the
// published order -- see the long comment there. The two must move together or
// this stub stops standing in for anything.
struct SteamFriendsVTable
{
	void       *_Slot0;
	void       *_Slot1;
	int         ( *GetFriendCount )( ISteamFriends *, int );
	uint64_t    ( *GetFriendByIndex )( ISteamFriends *, int, int );
	void       *_Slot4;
	void       *_Slot5;
	const char *( *GetFriendPersonaName )( ISteamFriends *, uint64_t );
	bool        ( *GetFriendGamePlayed )( ISteamFriends *, uint64_t, FriendGameInfo_t * );
};

struct ISteamFriends { const SteamFriendsVTable *vt; };

namespace
{
	const char *Mode()
	{
		const char *p = getenv( "GS_RITZ_TEST_STEAMCLIENT_MODE" );
		return ( p && *p ) ? p : "ok";
	}

	bool ModeIs( const char *psz ) { return strcmp( Mode(), psz ) == 0; }

	// ---------------------------------------------------------------------
	//  The fake friends list
	// ---------------------------------------------------------------------
	// Deliberately a mix, so a test that only checked "we got rows back" would
	// still fail: three of these five must be filtered out, each by a
	// different clause of IsJoinable().
	//
	// CGameID packs the app id into bits 0..23 and the TYPE into bits 24..31.
	struct FakeFriend
	{
		uint64_t    ulSteamId;
		uint64_t    ulGameId;
		uint64_t    ulLobbyId;
		const char *pszPersona;
		bool        bInGame;
	};

	constexpr uint64_t GameId( uint32_t uAppId, uint8_t uType = 0 )
	{
		return (uint64_t)uAppId | ( (uint64_t)uType << 24 );
	}

	// The ids have to be REAL-SHAPED, not 1/2/3: src/SteamFriends.cpp range-
	// checks every SteamID it gets back as a layout tripwire, so a stub handing
	// out small integers would trip it and every case here would report
	// "wrong shape" instead of the case it is meant to be testing.
	constexpr uint64_t kSteamIdBase = 76561197960265728ull;

	const FakeFriend kFriends[] = {
		// in CS2, but not in a joinable lobby        -> filtered on lobby id
		{ kSteamIdBase + 100, GameId( 730 ), 0, "in a game, not joinable", true },
		// in CS2 in a joinable lobby                 -> KEPT
		{ kSteamIdBase + 101, GameId( 730 ), 555, "joinable one", true },
		// a non-Steam shortcut / mod (type 1)        -> filtered on CGameID type
		{ kSteamIdBase + 102, GameId( 252490, 1 ), 777, "non-steam game", true },
		// a lobby with no app behind it              -> filtered on app id 0
		{ kSteamIdBase + 103, GameId( 0 ), 888, "no app", true },
		// in TF2 in a joinable lobby, HOSTILE NAME   -> KEPT
		{ kSteamIdBase + 104, GameId( 440 ), 999, "evil\";rm -rf $HOME;\" --kiosk", true },
		// not in a game at all
		{ kSteamIdBase + 105, 0, 0, "offline", false },
	};

	constexpr int kFriendCount = (int)( sizeof( kFriends ) / sizeof( kFriends[ 0 ] ) );

	const FakeFriend *Find( uint64_t ulSteamId )
	{
		for ( const FakeFriend &f : kFriends )
			if ( f.ulSteamId == ulSteamId )
				return &f;
		return nullptr;
	}

	// ---------------------------------------------------------------------
	//  ISteamFriends
	// ---------------------------------------------------------------------
	const char *FriendsGetPersonaName( ISteamFriends * ) { return "me"; }
	int         FriendsGetPersonaState( ISteamFriends * ) { return 1; }

	int FriendsGetFriendCount( ISteamFriends *, int ) { return kFriendCount; }

	uint64_t FriendsGetFriendByIndex( ISteamFriends *, int i, int )
	{
		if ( i < 0 || i >= kFriendCount )
			return 0;
		// "badids" stands for the failure that actually happened: a vtable that
		// moved under us, so this slot is no longer GetFriendByIndex and the
		// number coming back is not a SteamID at all.
		if ( ModeIs( "badids" ) )
			return (uint64_t)( i + 1 );
		return kFriends[ i ].ulSteamId;
	}

	int FriendsGetFriendRelationship( ISteamFriends *, uint64_t ) { return 3; }
	int FriendsGetFriendPersonaState( ISteamFriends *, uint64_t ) { return 1; }

	const char *FriendsGetFriendPersonaName( ISteamFriends *, uint64_t ulSteamId )
	{
		const FakeFriend *p = Find( ulSteamId );
		return p ? p->pszPersona : "";
	}

	bool FriendsGetFriendGamePlayed( ISteamFriends *, uint64_t ulSteamId, FriendGameInfo_t *pInfo )
	{
		const FakeFriend *p = Find( ulSteamId );
		if ( !p || !p->bInGame || !pInfo )
			return false;
		pInfo->m_gameID       = p->ulGameId;
		pInfo->m_unGameIP     = 0;
		pInfo->m_usGamePort   = 0;
		pInfo->m_usQueryPort  = 0;
		pInfo->m_steamIDLobby = p->ulLobbyId;
		return true;
	}

	const SteamFriendsVTable kFriendsVT = {
		(void *)FriendsGetPersonaName,
		nullptr,                             // the slot the real code refuses to call
		FriendsGetFriendCount,
		FriendsGetFriendByIndex,
		(void *)FriendsGetFriendRelationship,
		(void *)FriendsGetFriendPersonaState,
		FriendsGetFriendPersonaName,
		FriendsGetFriendGamePlayed,
	};

	ISteamFriends g_Friends = { &kFriendsVT };

	// ---------------------------------------------------------------------
	//  ISteamClient
	// ---------------------------------------------------------------------
	HSteamPipe ClientCreateSteamPipe( ISteamClient * )
	{
		return ModeIs( "nopipe" ) ? 0 : 7;   // "Steam isn't running"
	}

	bool ClientBReleaseSteamPipe( ISteamClient *, HSteamPipe ) { return true; }

	HSteamUser ClientConnectToGlobalUser( ISteamClient *, HSteamPipe )
	{
		return ModeIs( "nouser" ) ? 0 : 3;   // "signed out"
	}

	HSteamUser ClientCreateLocalUser( ISteamClient *, HSteamPipe *, int ) { return 0; }
	void       ClientReleaseUser( ISteamClient *, HSteamPipe, HSteamUser ) {}
	void      *ClientGetISteamUser( ISteamClient *, HSteamUser, HSteamPipe, const char * ) { return nullptr; }
	void      *ClientGetISteamGameServer( ISteamClient *, HSteamUser, HSteamPipe, const char * ) { return nullptr; }
	void       ClientSetLocalIPBinding( ISteamClient *, const void *, uint16_t ) {}

	void *ClientGetISteamFriends( ISteamClient *, HSteamUser, HSteamPipe, const char * )
	{
		// "nofriends" stands for a client that has moved past every
		// SteamFriends version this build knows about.
		return ModeIs( "nofriends" ) ? nullptr : (void *)&g_Friends;
	}

	const SteamClientVTable kClientVT = {
		ClientCreateSteamPipe,
		ClientBReleaseSteamPipe,
		ClientConnectToGlobalUser,
		ClientCreateLocalUser,
		ClientReleaseUser,
		ClientGetISteamUser,
		ClientGetISteamGameServer,
		ClientSetLocalIPBinding,
		ClientGetISteamFriends,
	};

	ISteamClient g_Client = { &kClientVT };
}

extern "C"
{
	void *CreateInterface( const char *pszVersion, int *pnErr )
	{
		if ( pnErr )
			*pnErr = 0;
		// "noiface": a client that answers to no SteamClient version we ask
		// for, which is the "newer than this build knows" path.
		if ( ModeIs( "noiface" ) )
			return nullptr;
		if ( pszVersion && strncmp( pszVersion, "SteamClient", 11 ) == 0 )
			return (void *)&g_Client;
		return nullptr;
	}

#ifndef STUB_NOT_STEAM
	// The fingerprint SteamFriends.cpp requires before it will cast anything to
	// a vtable. Present here, absent from the -DSTUB_NOT_STEAM build; neither
	// build is ever called through, and neither is gamescope-ritz's real code.
	int  Steam_CreateSteamPipe()                { return 0; }
	int  Steam_ConnectToGlobalUser( int )       { return 0; }
	bool Steam_BConnected( int, int )           { return false; }
	bool Steam_BLoggedOn( int, int )            { return false; }
	bool Steam_BReleaseSteamPipe( int )         { return false; }
	void Steam_ReleaseUser( int, int )          {}
#endif
}
