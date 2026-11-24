// Unit tests for FrameGen/FrameGenDebug.h: the Debug options' pure parts (superdoc/features/frame-generation-debug.md): the
// "Only generated frames" hold decision, the DebugConfig word, and the UI-box word / library box of Whole screen V2.
// No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>
#include <string>

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

TEST_CASE( "framegen debug: the UI mode name", "[framegen_debug]" )
{
    REQUIRE( std::string( UiModeName( UiProt::CrosshairV2, false ) ) == "Crosshair V2" );
    REQUIRE( std::string( UiModeName( UiProt::CrosshairV2, true ) ) == "Whole screen V2" );
    REQUIRE( std::string( UiModeName( UiProt::WholeScreen, true ) ) == "Whole screen" );   // the flag counts only with Crosshair V2
    REQUIRE( std::string( UiModeName( UiProt::Off, false ) ) == "Off" );
    REQUIRE( std::string( UiModeName( UiProt::Crosshair, false ) ) == "Crosshair" );
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

// Debug view (2026-11-23): the row's order (off, UI highlighted, UI removed, Motion map) differs from the library's
// framegen::DebugView (Off 0, Motion 1, UiMask 2, UiClean 3); the host static_asserts the enum, this pins the mapping.
TEST_CASE( "framegen debug: a stored debug_view string maps to the library's DebugView value", "[framegen_debug]" )
{
    REQUIRE( DebugViewFromKey( "off" ) == 0 );
    REQUIRE( DebugViewFromKey( "ui_highlight" ) == 1 );
    REQUIRE( DebugViewFromKey( "ui_removed" ) == 2 );
    REQUIRE( DebugViewFromKey( "motion" ) == 3 );
    REQUIRE( DebugViewFromKey( "nonsense" ) == 0 );
    REQUIRE( DebugViewFromKey( "" ) == 0 );
    REQUIRE( LibraryDebugView( DebugViewFromKey( "off" ) ) == 0 );
    REQUIRE( LibraryDebugView( DebugViewFromKey( "motion" ) ) == 1 );         // DebugView::Motion
    REQUIRE( LibraryDebugView( DebugViewFromKey( "ui_highlight" ) ) == 2 );   // DebugView::UiMask
    REQUIRE( LibraryDebugView( DebugViewFromKey( "ui_removed" ) ) == 3 );     // DebugView::UiClean
    REQUIRE( LibraryDebugView( 4 ) == 0 );
    REQUIRE( LibraryDebugView( -1 ) == 0 );
    REQUIRE( std::string( DebugViewName( 3 ) ) == "Motion map" );
    REQUIRE( std::string( DebugViewName( 1 ) ) == "UI highlighted" );
}

TEST_CASE( "framegen debug: the view survives the DebugConfig word for all four values and clamps", "[framegen_debug]" )
{
    for ( int n = 0; n < kDebugViewCount; ++n )
    {
        DebugConfig d;
        d.view = n;
        d.stats = true;
        const DebugConfig r = UnpackDebug( PackDebug( d ) );
        REQUIRE( r.view == n );
        REQUIRE( r.stats );
        REQUIRE( !r.onlyGenerated );
    }
    DebugConfig big;
    big.view = 9;
    REQUIRE( UnpackDebug( PackDebug( big ) ).view == kDebugViewCount - 1 );
}

// Low FPS compensation (2026-11-24): its bit in the Learned word and its status token.
TEST_CASE( "framegen: low fps compensation packs into bit 7 of the Learned word and leaves the other bits alone", "[framegen_debug]" )
{
    REQUIRE( !UnpackLowFps( 0 ) );
    REQUIRE( PackLowFps( false ) == 0u );
    REQUIRE( PackLowFps( true ) == 0x80u );
    REQUIRE( UnpackLowFps( PackLowFps( true ) ) );
    REQUIRE( !UnpackLowFps( 0x7Fu ) );                 // Learned motion's own seven bits do not set it
    REQUIRE( UnpackLowFps( 0x7Fu | PackLowFps( true ) ) );
    REQUIRE( ( kLowFpsCompBit & 0x7Fu ) == 0 );        // outside the library-config key's mask
}

TEST_CASE( "framegen: the status line names low fps compensation only while it acts", "[framegen_debug]" )
{
    REQUIRE( LowFpsStatusToken( false, 0.0f ).empty() );
    REQUIRE( LowFpsStatusToken( false, 16.67f ).empty() );   // never from a stale length
    REQUIRE( LowFpsStatusToken( true, 16.67f ) == ", low fps compensation 16.7 ms" );
}
