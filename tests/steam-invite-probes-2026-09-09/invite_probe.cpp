// invite_probe.cpp -- can gamescope-ritz send a Steam game invite?
//
// Two modes.  The default one is STRICTLY READ-ONLY.  The second one performs
// exactly ONE write and refuses to do it unless the layout interlock below
// passes AND the target persona resolves to exactly one friend.
//
// THE INTERLOCK, which is the point of this file.
// superdoc/planning/steam-friends-join.md §6e's rule is "never call a slot you
// have only inferred".  ISteamFriends::InviteUserToGame's slot is no longer
// inferred -- it was MEASURED from four independent sources (see the 2026-09-09
// section of that doc) -- but a measurement made yesterday is not the same as
// the client running right now.  So before it calls anything, this probe
// re-derives the layout FROM THE LIVE PROCESS'S OWN BYTES:
//
//   * SteamFriends018's vtable must have exactly 78 function pointers;
//   * every vtable entry is a forwarding thunk onto ONE shared implementation
//     object, and the inner slot it jumps to is a version-independent identity
//     for the method.  Slots 2/3/7 must forward to inner 7/10/23 -- the three
//     the shipping code already calls and that §6e confirmed live -- and slot
//     47 must forward to inner 215, which is what every source says
//     InviteUserToGame is.
//
// If any of that fails the probe refuses, loudly, and calls nothing.
//
// PRIVACY: counts, app ids, range verdicts and yes/no.  No SteamID, no persona
// name, no lobby id is ever printed.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <dlfcn.h>

namespace {

// ---- reading the live vtable, and decoding one thunk ----------------------
struct IClient;
struct ClientVT {
    int32_t ( *CreateSteamPipe )( IClient * );
    bool    ( *BReleaseSteamPipe )( IClient *, int32_t );
    int32_t ( *ConnectToGlobalUser )( IClient *, int32_t );
    int32_t ( *CreateLocalUser )( IClient *, int32_t *, int );
    void    ( *ReleaseUser )( IClient *, int32_t, int32_t );
    void   *( *GetISteamUser )( IClient *, int32_t, int32_t, const char * );
    void   *( *GetISteamGameServer )( IClient *, int32_t, int32_t, const char * );
    void    ( *SetLocalIPBinding )( IClient *, const void *, uint16_t );
    void   *( *GetISteamFriends )( IClient *, int32_t, int32_t, const char * );
    void   *( *GetISteamUtils )( IClient *, int32_t, const char * );
};
struct IClient { const ClientVT *vt; };
struct IFriends { void *const *vt; };

struct Range { uintptr_t lo, hi; bool r, x; std::string path; };
std::vector<Range> g_maps;
void LoadMaps()
{
    g_maps.clear();
    std::ifstream f( "/proc/self/maps" );
    std::string line;
    while ( std::getline( f, line ) )
    {
        uintptr_t lo = 0, hi = 0; char perms[8] = {0}; char path[4096] = {0};
        if ( sscanf( line.c_str(), "%lx-%lx %7s %*s %*s %*s %4095[^\n]", &lo, &hi, perms, path ) < 3 )
            continue;
        std::string p( path );
        while ( !p.empty() && p.front() == ' ' ) p.erase( p.begin() );
        g_maps.push_back( { lo, hi, perms[0] == 'r', perms[2] == 'x', p } );
    }
}
const Range *Find( uintptr_t a )
{
    for ( const Range &r : g_maps ) if ( a >= r.lo && a < r.hi ) return &r;
    return nullptr;
}
bool Readable( uintptr_t a, size_t n ) { const Range *r = Find( a ); return r && r->r && a + n <= r->hi; }
bool IsSteamCode( uintptr_t a )
{
    const Range *r = Find( a );
    return r && r->x && r->path.find( "steamclient.so" ) != std::string::npos;
}

// The inner-vtable slot a public thunk dispatches to, or -1.
// Patterns seen in this client:
//   48 8b 7f 08  48 8b 07  ff 60 d8 / ff a0 d32 / ff 90 d32 / ff 20
//   ...          4c 8b 88 d32 (mov d32(%rax),%r9) then jmp *%r9
//   e9 rel32     (a shared tail)
int InnerSlot( uintptr_t a, int depth = 0 )
{
    if ( depth > 3 || !Readable( a, 48 ) ) return -1;
    const uint8_t *b = (const uint8_t *)a;
    for ( int i = 0; i < 40; i++ )
    {
        if ( b[ i ] == 0xE9 )
        {
            int32_t rel; memcpy( &rel, b + i + 1, 4 );
            return InnerSlot( a + i + 5 + rel, depth + 1 );
        }
        if ( b[ i ] == 0xFF )
        {
            const uint8_t m = b[ i + 1 ];
            if ( m == 0x20 || m == 0x10 ) return 0;
            if ( m == 0x60 || m == 0x50 ) return b[ i + 2 ] / 8;
            if ( m == 0xA0 || m == 0x90 ) { uint32_t d; memcpy( &d, b + i + 2, 4 ); return (int)( d / 8 ); }
        }
    }
    for ( int i = 0; i < 40; i++ )
        if ( ( b[ i ] == 0x48 || b[ i ] == 0x4C ) && b[ i + 1 ] == 0x8B && ( b[ i + 2 ] & 0xC7 ) == 0x80 )
        { uint32_t d; memcpy( &d, b + i + 3, 4 ); return (int)( d / 8 ); }
    return -1;
}

constexpr uint64_t kIndividualMin = 76561197960265728ull;
constexpr uint64_t kIndividualMax = 76561197960265728ull + 0xFFFFFFFFull;

// The signed-in account, out of Steam's own loginusers.vdf -- the entry with
// the newest "Timestamp" (this client version writes no "MostRecent" key).
// Read from disk, so no extra interface and no extra vtable slot is needed.
uint64_t MostRecentSteamId()
{
    const std::string sHome = getenv( "HOME" ) ? getenv( "HOME" ) : "";
    std::ifstream f( sHome + "/.steam/steam/config/loginusers.vdf" );
    if ( !f ) return 0;
    std::string line;
    uint64_t ulCur = 0, ulBest = 0; long long llBest = -1;
    while ( std::getline( f, line ) )
    {
        size_t q = line.find( '"' );
        if ( q == std::string::npos ) continue;
        size_t e = line.find( '"', q + 1 );
        if ( e == std::string::npos ) continue;
        const std::string k = line.substr( q + 1, e - q - 1 );
        if ( k.size() == 17 && k.rfind( "7656", 0 ) == 0 && line.find( '"', e + 1 ) == std::string::npos )
        { ulCur = strtoull( k.c_str(), nullptr, 10 ); continue; }
        if ( k == "Timestamp" )
        {
            size_t q2 = line.find( '"', e + 1 );
            size_t e2 = q2 == std::string::npos ? q2 : line.find( '"', q2 + 1 );
            if ( e2 == std::string::npos ) continue;
            const long long ll = atoll( line.substr( q2 + 1, e2 - q2 - 1 ).c_str() );
            if ( ll > llBest ) { llBest = ll; ulBest = ulCur; }
        }
    }
    return ulBest;
}

struct GameInfo {
    uint64_t m_gameID; uint32_t m_unGameIP; uint16_t m_usGamePort;
    uint16_t m_usQueryPort; uint64_t m_steamIDLobby;
};

} // namespace

int main( int argc, char **argv )
{
    const char *pszInvite = nullptr;
    const char *pszConnect = nullptr;
    for ( int i = 1; i < argc; i++ )
    {
        if ( !strcmp( argv[ i ], "--invite" ) && i + 1 < argc ) pszInvite = argv[ ++i ];
        else if ( !strcmp( argv[ i ], "--connect" ) && i + 1 < argc ) pszConnect = argv[ ++i ];
    }

    const std::string sHome = getenv( "HOME" ) ? getenv( "HOME" ) : "";
    void *pLib = dlopen( ( sHome + "/.steam/steam/linux64/steamclient.so" ).c_str(), RTLD_LAZY | RTLD_LOCAL );
    if ( !pLib ) { printf( "RESULT probe: FAIL (no steamclient.so)\n" ); return 1; }
    auto pfn = (void *(*)( const char *, int * ))dlsym( pLib, "CreateInterface" );
    int nErr = 0;
    IClient *pClient = pfn ? (IClient *)pfn( "SteamClient023", &nErr ) : nullptr;
    if ( !pClient ) { printf( "RESULT probe: FAIL (no ISteamClient)\n" ); return 1; }

    const int32_t hPipe = pClient->vt->CreateSteamPipe( pClient );
    const int32_t hUser = hPipe ? pClient->vt->ConnectToGlobalUser( pClient, hPipe ) : 0;
    if ( !hUser ) { printf( "RESULT probe: FAIL (Steam not running or signed out)\n" ); return 1; }

    IFriends *pF = (IFriends *)pClient->vt->GetISteamFriends( pClient, hUser, hPipe, "SteamFriends018" );
    if ( !pF ) { printf( "RESULT probe: FAIL (no SteamFriends018)\n" ); return 1; }

    LoadMaps();

    // ---- the interlock ----------------------------------------------------
    int nLen = 0;
    for ( int i = 0; i < 512; i++ )
    {
        if ( !Readable( (uintptr_t)( pF->vt + i ), 8 ) ) break;
        if ( !IsSteamCode( (uintptr_t)pF->vt[ i ] ) ) break;
        nLen = i + 1;
    }
    const int nS2 = InnerSlot( (uintptr_t)pF->vt[ 2 ] );
    const int nS3 = InnerSlot( (uintptr_t)pF->vt[ 3 ] );
    const int nS7 = InnerSlot( (uintptr_t)pF->vt[ 7 ] );
    const int nS47 = nLen > 47 ? InnerSlot( (uintptr_t)pF->vt[ 47 ] ) : -1;
    printf( "RESULT vtable length: %d (expect 78)\n", nLen );
    printf( "RESULT inner slots: [2]=%d (expect 7, GetFriendCount)  [3]=%d (10, GetFriendByIndex)  "
            "[7]=%d (23, GetFriendGamePlayed)  [47]=%d (215, InviteUserToGame)\n", nS2, nS3, nS7, nS47 );
    const bool bInterlock = ( nLen == 78 && nS2 == 7 && nS3 == 10 && nS7 == 23 && nS47 == 215 );
    printf( "RESULT layout interlock: %s\n", bInterlock ? "PASS" : "FAIL -- nothing further will be called" );
    if ( !bInterlock ) return 2;

    using CountFn  = int ( * )( IFriends *, int );
    using IndexFn  = uint64_t ( * )( IFriends *, int, int );
    using NameFn   = const char *( * )( IFriends *, uint64_t );
    using PlayedFn = bool ( * )( IFriends *, uint64_t, GameInfo * );

    // ---- read: our OWN session, at slots the shipping code already uses ----
    const uint64_t ulMe = MostRecentSteamId();
    printf( "RESULT own SteamID from loginusers.vdf: %s\n",
        ( ulMe >= kIndividualMin && ulMe <= kIndividualMax ) ? "resolved, in the individual band" : "NOT resolved" );
    if ( ulMe )
    {
        GameInfo gi{};
        const bool bOk = ( (PlayedFn)pF->vt[ 7 ] )( pF, ulMe, &gi );
        const uint32_t uApp = (uint32_t)( gi.m_gameID & 0x00FFFFFFull );
        const uint8_t  uType = (uint8_t)( ( gi.m_gameID >> 24 ) & 0xFFull );
        printf( "RESULT GetFriendGamePlayed(self): %s, app %u, CGameID type %u, own lobby id %s\n",
            bOk ? "true" : "false", uApp, uType,
            gi.m_steamIDLobby ? ( ( gi.m_steamIDLobby >= 0x0170000000000000ull &&
                                    gi.m_steamIDLobby <= 0x0190000000000000ull )
                                  ? "PRESENT and in the chat band" : "PRESENT but OUT OF BAND" )
                              : "zero" );
    }

    // ---- read: does OUR pipe carry an app id at all? ----------------------
    // ISteamUtils::GetAppID is slot 9 -- measured the same four ways as slot 47,
    // and its neighbours (GetCurrentBatteryPower, SetOverlayNotificationPosition)
    // are harmless.  This is the question an invite lives or dies on: an invite
    // names the CALLER'S app, and route B deliberately registers none.
    if ( void *pUtilsRaw = pClient->vt->GetISteamUtils( pClient, hPipe, "SteamUtils010" ) )
    {
        IFriends *pU = (IFriends *)pUtilsRaw;   // same shape: object -> vtable
        int nULen = 0;
        for ( int i = 0; i < 512; i++ )
        {
            if ( !Readable( (uintptr_t)( pU->vt + i ), 8 ) ) break;
            if ( !IsSteamCode( (uintptr_t)pU->vt[ i ] ) ) break;
            nULen = i + 1;
        }
        // The walk can OVERCOUNT BY ONE: the word after a vtable is sometimes
        // another function pointer inside the same library, and nothing in the
        // bytes distinguishes "one past the end" from "one more slot".  Same
        // thing happened to SteamFriends015 (73 read vs 72 real).  It matters
        // for ISteamFriends only because 78-vs-80 is what separates 018 from
        // 017; here 39-or-40 is the same table either way, and slot 9's
        // NEIGHBOURS are what actually bounds the risk: 8 is
        // GetCurrentBatteryPower and 10 is SetOverlayNotificationPosition, both
        // harmless, and Proton's bridge and CS2's own shipped SDK agree on
        // every ISteamUtils slot.
        printf( "RESULT SteamUtils010 vtable length: %d (Proton's bridge says 39 methods)\n", nULen );
        if ( nULen == 39 || nULen == 40 )
        {
            using AppIdFn = uint32_t ( * )( IFriends * );
            const uint32_t uApp = ( (AppIdFn)pU->vt[ 9 ] )( pU );
            printf( "RESULT ISteamUtils::GetAppID() on our pipe: %u\n", uApp );
        }
        else
            printf( "RESULT GetAppID: SKIPPED (utils layout did not interlock)\n" );
    }
    else
        printf( "RESULT SteamUtils010: not returned\n" );

    // ---- the one write, and only toward a persona named on the command line
    int nRet = 0;
    if ( pszInvite )
    {
        const int nFriends = ( (CountFn)pF->vt[ 2 ] )( pF, 0x04 );
        uint64_t ulTarget = 0; int nMatches = 0;
        for ( int i = 0; i < nFriends && i < 4096; i++ )
        {
            const uint64_t ul = ( (IndexFn)pF->vt[ 3 ] )( pF, i, 0x04 );
            if ( ul < kIndividualMin || ul > kIndividualMax ) continue;
            const char *psz = ( (NameFn)pF->vt[ 6 ] )( pF, ul );
            if ( psz && !strcmp( psz, pszInvite ) ) { ulTarget = ul; nMatches++; }
        }
        printf( "RESULT target persona resolved: %d match%s\n", nMatches, nMatches == 1 ? "" : "es" );
        if ( nMatches != 1 )
        {
            printf( "RESULT invite: REFUSED (the persona must resolve to exactly one friend)\n" );
            nRet = 3;
        }
        else
        {
            using InviteFn = bool ( * )( IFriends *, uint64_t, const char * );
            const char *pszCs = pszConnect ? pszConnect : "+connect_lobby 0";
            printf( "calling SteamFriends018 slot 47 (InviteUserToGame) once, connect string %s\n", pszCs );
            const bool b = ( (InviteFn)pF->vt[ 47 ] )( pF, ulTarget, pszCs );
            printf( "RESULT InviteUserToGame returned: %s\n", b ? "TRUE" : "FALSE" );
        }
    }

    pClient->vt->ReleaseUser( pClient, hPipe, hUser );
    pClient->vt->BReleaseSteamPipe( pClient, hPipe );
    return nRet;
}
