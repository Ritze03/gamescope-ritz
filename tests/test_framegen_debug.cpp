// Unit tests for FrameGen/FrameGenDebug.h: the Debug options' pure parts (superdoc/features/frame-generation-debug.md): the
// "Only generated frames" hold decision, the DebugConfig word, and the UI-box word / library box of Whole screen V2.
// No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>

#include "FrameGen/FrameGenDebug.h"

using namespace fghost;

TEST_CASE( "framegen debug: a real tick is held only with the option on, generation running and a fresh output", "[framegen_debug]" )
{
    REQUIRE( ShouldHoldGenerated( true, true, true, true, 0 ) );
    REQUIRE( ShouldHoldGenerated( true, true, true, true, kHoldMaxNs ) );        // the cap itself still holds
    REQUIRE( !ShouldHoldGenerated( true, true, true, true, kHoldMaxNs + 1 ) );   // older than the cap: the game's frame shows
    REQUIRE( !ShouldHoldGenerated( false, true, true, true, 0 ) );               // option off
    REQUIRE( !ShouldHoldGenerated( true, false, true, true, 0 ) );               // a generated tick shows its own image
    REQUIRE( !ShouldHoldGenerated( true, true, false, true, 0 ) );               // nothing generated: no freeze
    REQUIRE( !ShouldHoldGenerated( true, true, true, false, 0 ) );               // no valid output slot
}

TEST_CASE( "framegen debug: what counts as a real tick", "[framegen_debug]" )
{
    REQUIRE( IsRealTick( 0, 1.0f ) );    // no pair: the real frame showId
    REQUIRE( IsRealTick( 0, 0.5f ) );
    REQUIRE( IsRealTick( 7, 1.0f ) );    // Motion blur's real frame carries a pair and t = 1
    REQUIRE( !IsRealTick( 7, 0.5f ) );   // generated
    REQUIRE( !IsRealTick( 7, 0.0f ) );
}

TEST_CASE( "framegen debug: the multiplier sequences", "[framegen_debug]" )
{
    // Ticks as (prevId, t): 2x = gen, real; 3x = gen, gen, real. Exactly one tick per cycle is a held (repeated) image.
    const struct { uint64_t prev; float t; } k2[] = { { 5, 0.5f }, { 0, 1.0f } };
    const struct { uint64_t prev; float t; } k3[] = { { 5, 0.33f }, { 5, 0.66f }, { 0, 1.0f } };
    int nHeld2 = 0, nHeld3 = 0;
    for ( const auto &q : k2 )
        nHeld2 += ShouldHoldGenerated( true, IsRealTick( q.prev, q.t ), true, true, 1000 ) ? 1 : 0;
    for ( const auto &q : k3 )
        nHeld3 += ShouldHoldGenerated( true, IsRealTick( q.prev, q.t ), true, true, 1000 ) ? 1 : 0;
    REQUIRE( nHeld2 == 1 );
    REQUIRE( nHeld3 == 1 );
}

TEST_CASE( "framegen debug: the DebugConfig word round-trips", "[framegen_debug]" )
{
    REQUIRE( PackDebug( DebugConfig{} ) == 0u );
    for ( int n = 0; n < 4; ++n )
    {
        DebugConfig d;
        d.onlyGenerated = ( n & 1 ) != 0;
        d.stats = ( n & 2 ) != 0;
        const DebugConfig r = UnpackDebug( PackDebug( d ) );
        REQUIRE( r.onlyGenerated == d.onlyGenerated );
        REQUIRE( r.stats == d.stats );
        REQUIRE( r.view == 0 );
    }
    DebugConfig v;
    v.view = 3;
    REQUIRE( UnpackDebug( PackDebug( v ) ).view == 3 );
    REQUIRE( !UnpackDebug( 0xFFFFFFC0u ).onlyGenerated );   // the unused bits are ignored
}

TEST_CASE( "framegen debug: the UI-box word carries Whole screen V2 beside the tenths", "[framegen_debug]" )
{
    for ( const bool bWhole : { false, true } )
        for ( const int nTenths : { kUiBoxMinTenths, kUiBoxDefaultTenths, 37, kUiBoxMaxTenths } )
        {
            int nOut = -1;
            bool bOut = !bWhole;
            UnpackUiBox( PackUiBox( nTenths, bWhole ), &nOut, &bOut );
            REQUIRE( nOut == nTenths );
            REQUIRE( bOut == bWhole );
        }
    // The old word (tenths only) reads as not whole-frame; an out-of-range value clamps.
    int n = 0; bool b = true;
    UnpackUiBox( uint32_t( kUiBoxDefaultTenths ), &n, &b );
    REQUIRE( ( n == kUiBoxDefaultTenths && !b ) );
    REQUIRE( PackUiBox( 9999, false ) == uint32_t( kUiBoxMaxTenths ) );
    static_assert( kUiBoxMaxTenths < 256, "the tenths stay below the whole-frame bit" );
}

TEST_CASE( "framegen debug: Whole screen V2 maps to Crosshair V2 with a full-frame box, nothing else does", "[framegen_debug]" )
{
    float flFrac = 0.0f; uint32_t w = 1, h = 1;
    UiBoxSettings( true, true, 25, &flFrac, &w, &h );
    REQUIRE( ( w == 16384u && h == 16384u ) );   // the library clamps it to the frame
    REQUIRE( flFrac == 0.025f );                 // the slider's value is still handed over
    UiBoxSettings( true, false, 25, &flFrac, &w, &h );
    REQUIRE( ( w == 0u && h == 0u ) );           // Crosshair V2: the box from the fraction
    UiBoxSettings( false, true, 25, &flFrac, &w, &h );
    REQUIRE( ( w == 0u && h == 0u ) );           // the flag counts only with Crosshair V2
}
