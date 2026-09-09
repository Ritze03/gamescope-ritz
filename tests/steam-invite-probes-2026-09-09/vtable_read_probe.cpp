// vtable_read_probe.cpp -- pin ISteamFriends' layout on the LIVE client
// WITHOUT CALLING A SINGLE VTABLE SLOT.
//
// superdoc/planning/steam-friends-join.md §6e forbids calling a slot whose
// identity is inferred.  This probe therefore never calls one.  It only READS
// the vtable POINTER ARRAYS out of memory, and reasons about them:
//
//   1. how long each vtable is (walk until a pointer stops being a function
//      inside steamclient.so, bounded, and only through addresses /proc/self/maps
//      says are readable);
//   2. whether SteamFriends018's slot i is the SAME function pointer as
//      SteamFriends017's slot i+1 -- which, if it holds for the whole table,
//      proves end-to-end that 018 is 017 with exactly one method removed at the
//      top, i.e. proves EVERY slot's identity, not only the five already
//      measured by calling.
//
// The only Steam calls it makes are the four the shipping code already makes
// and that are proven: CreateInterface, CreateSteamPipe, ConnectToGlobalUser,
// GetISteamFriends -- plus ReleaseUser / BReleaseSteamPipe on the way out.
// NOTHING is invited, messaged, joined or written.
//
// PRIVACY: prints library-relative offsets and counts.  No SteamID, no name,
// no absolute address (ASLR-dependent and useless anyway).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <dlfcn.h>
#include <link.h>

namespace {

struct Range { uintptr_t lo, hi; bool r, x; std::string path; };
std::vector<Range> g_maps;

void LoadMaps()
{
    g_maps.clear();
    std::ifstream f( "/proc/self/maps" );
    std::string line;
    while ( std::getline( f, line ) )
    {
        uintptr_t lo = 0, hi = 0;
        char perms[8] = {0};
        char path[4096] = {0};
        if ( sscanf( line.c_str(), "%lx-%lx %7s %*s %*s %*s %4095[^\n]", &lo, &hi, perms, path ) < 3 )
            continue;
        std::string p( path );
        while ( !p.empty() && p.front() == ' ' ) p.erase( p.begin() );
        g_maps.push_back( { lo, hi, perms[0] == 'r', perms[2] == 'x', p } );
    }
}

const Range *Find( uintptr_t a )
{
    for ( const Range &r : g_maps )
        if ( a >= r.lo && a < r.hi )
            return &r;
    return nullptr;
}

// Readable?  Answered from /proc/self/maps, so no SIGSEGV is ever risked.
bool Readable( uintptr_t a, size_t n )
{
    const Range *r = Find( a );
    return r && r->r && ( a + n ) <= r->hi;
}

bool IsCodeIn( uintptr_t a, const std::string &sSo, uintptr_t *pOff )
{
    const Range *r = Find( a );
    if ( !r || !r->x ) return false;
    if ( r->path.find( sSo ) == std::string::npos ) return false;
    // base = lowest mapping of the same file
    uintptr_t base = ~(uintptr_t)0;
    for ( const Range &q : g_maps )
        if ( q.path == r->path && q.lo < base ) base = q.lo;
    *pOff = a - base;
    return true;
}

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
};
struct IClient { const ClientVT *vt; };

// A vtable read out of memory: the pointers, and how many of them look real.
struct VT { void *const *p = nullptr; int n = 0; };

VT ReadVT( void *pObject, const std::string &sSo, int nCap )
{
    VT vt;
    if ( !pObject || !Readable( (uintptr_t)pObject, 8 ) ) return vt;
    void *const *tbl = *(void *const **)pObject;
    if ( !tbl || !Readable( (uintptr_t)tbl, 8 ) ) return vt;
    vt.p = tbl;
    for ( int i = 0; i < nCap; i++ )
    {
        if ( !Readable( (uintptr_t)( tbl + i ), 8 ) ) break;
        uintptr_t off = 0;
        if ( !IsCodeIn( (uintptr_t)tbl[ i ], sSo, &off ) ) break;
        vt.n = i + 1;
    }
    return vt;
}

} // namespace

int main( int argc, char **argv )
{
    const char *pszA = argc > 1 ? argv[ 1 ] : "SteamFriends018";
    const char *pszB = argc > 2 ? argv[ 2 ] : "SteamFriends017";

    const std::string sHome = getenv( "HOME" ) ? getenv( "HOME" ) : "";
    const std::string sSo = sHome + "/.steam/steam/linux64/steamclient.so";
    void *pLib = dlopen( sSo.c_str(), RTLD_LAZY | RTLD_LOCAL );
    if ( !pLib ) { printf( "RESULT probe: FAIL (no steamclient.so)\n" ); return 1; }

    auto pfn = (void *(*)( const char *, int * ))dlsym( pLib, "CreateInterface" );
    if ( !pfn ) { printf( "RESULT probe: FAIL (no CreateInterface)\n" ); return 1; }
    int nErr = 0;
    IClient *pClient = (IClient *)pfn( "SteamClient023", &nErr );
    if ( !pClient ) { printf( "RESULT probe: FAIL (no ISteamClient)\n" ); return 1; }

    const int32_t hPipe = pClient->vt->CreateSteamPipe( pClient );
    const int32_t hUser = hPipe ? pClient->vt->ConnectToGlobalUser( pClient, hPipe ) : 0;
    printf( "RESULT pipe/user: %s / %s\n", hPipe ? "ok" : "NONE", hUser ? "ok" : "NONE" );
    if ( !hUser ) { printf( "RESULT probe: FAIL (Steam not running, or signed out)\n" ); return 1; }

    void *pA = pClient->vt->GetISteamFriends( pClient, hUser, hPipe, pszA );
    void *pB = pClient->vt->GetISteamFriends( pClient, hUser, hPipe, pszB );
    printf( "RESULT %s object: %s\n", pszA, pA ? "returned" : "NULL" );
    printf( "RESULT %s object: %s\n", pszB, pB ? "returned" : "NULL" );
    printf( "RESULT the two objects are: %s\n",
        ( pA && pB ) ? ( pA == pB ? "THE SAME pointer (no cross-check possible)" : "DIFFERENT pointers" ) : "n/a" );

    LoadMaps();

    VT a = ReadVT( pA, "steamclient.so", 512 );
    VT b = ReadVT( pB, "steamclient.so", 512 );
    printf( "RESULT %s vtable length (functions inside steamclient.so): %d\n", pszA, a.n );
    printf( "RESULT %s vtable length (functions inside steamclient.so): %d\n", pszB, b.n );

    // Slot-for-slot: is A[i] == B[i+1] for the whole table?
    if ( a.n && b.n )
    {
        int nSame = 0, nDiff = 0, nCmp = 0;
        int nFirstDiff = -1;
        const int nMax = ( a.n < b.n - 1 ) ? a.n : b.n - 1;
        for ( int i = 0; i < nMax; i++ )
        {
            nCmp++;
            if ( a.p[ i ] == b.p[ i + 1 ] ) nSame++;
            else { nDiff++; if ( nFirstDiff < 0 ) nFirstDiff = i; }
        }
        printf( "RESULT shift-by-one identity  %s[i] == %s[i+1]: %d of %d slots identical, %d differ%s\n",
            pszA, pszB, nSame, nCmp, nDiff,
            nFirstDiff >= 0 ? "" : " -> UNIFORM ACROSS THE WHOLE TABLE" );
        if ( nFirstDiff >= 0 )
            printf( "   first slot that differs: %d\n", nFirstDiff );

        // And the naive alignment, for contrast: A[i] == B[i]?
        int nNaive = 0;
        const int nMax2 = ( a.n < b.n ) ? a.n : b.n;
        for ( int i = 0; i < nMax2; i++ ) if ( a.p[ i ] == b.p[ i ] ) nNaive++;
        printf( "RESULT no-shift identity      %s[i] == %s[i]:   %d of %d slots identical\n",
            pszA, pszB, nNaive, nMax2 );
    }

    // Library-relative offsets, so a second run (or another machine) can be
    // compared.  No absolute addresses: ASLR makes them meaningless anyway.
    const char *pszDump = getenv( "DUMP_SLOTS" );
    if ( pszDump && *pszDump )
    {
        for ( int i = 0; i < a.n; i++ )
        {
            uintptr_t offA = 0, offB = 0;
            IsCodeIn( (uintptr_t)a.p[ i ], "steamclient.so", &offA );
            const bool bB = ( i + 1 < b.n ) && IsCodeIn( (uintptr_t)b.p[ i + 1 ], "steamclient.so", &offB );
            printf( "  slot %-3d  %s +0x%08lx   %s +0x%08lx  %s\n", i,
                pszA, (unsigned long)offA,
                pszB, bB ? (unsigned long)offB : 0UL,
                ( bB && offA == offB ) ? "same" : "DIFFER" );
        }
    }

    pClient->vt->ReleaseUser( pClient, hPipe, hUser );
    pClient->vt->BReleaseSteamPipe( pClient, hPipe );
    printf( "RESULT probe: done -- no vtable slot was called.\n" );
    return 0;
}
