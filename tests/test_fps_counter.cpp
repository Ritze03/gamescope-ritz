// The HUD frame-rate readout's pure arithmetic (2026-09-05 rewrite: count
// commits, glide the shown integer, pin the box to the digit count). Only
// gamescope::fpsmath (Overlay/FpsDisplay.h) is under test -- no ImGui, no
// Vulkan, no compositor. The live half (the counter in commit.cpp, the
// windows in UpdateAndGetDisplayFps(), the repaint-timer thread) is verified
// on a real display; the live check is written up in
// superdoc/features/fps-display.md.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

#include "Overlay/FpsDisplay.h"

using namespace gamescope::fpsmath;

namespace
{
    constexpr uint64_t kMs = 1000000ull;
}

// ---- rate from counts -----------------------------------------------------

TEST_CASE( "RateFromCounts is commits per wall second", "[fps_counter]" )
{
    REQUIRE( RateFromCounts( 60, 1000 * kMs ) == 60.0f );
    REQUIRE( RateFromCounts( 6, 100 * kMs ) == 60.0f );
    REQUIRE( RateFromCounts( 0, 1000 * kMs ) == 0.0f );
    // The 999 bug's shape: two commits in one batch are still two commits.
    REQUIRE( RateFromCounts( 120, 1000 * kMs ) == 120.0f );
    // Thousands of fps is a real, representable reading, not a clamp.
    REQUIRE( RateFromCounts( 3456, 1000 * kMs ) == 3456.0f );
}

TEST_CASE( "RateFromCounts never divides by zero", "[fps_counter]" )
{
    REQUIRE( RateFromCounts( 5, 0 ) == 0.0f );
    REQUIRE( RateFromCounts( 0, 0 ) == 0.0f );
}

// ---- glide -----------------------------------------------------------------

TEST_CASE( "glide starts at from and ends at to", "[fps_counter]" )
{
    REQUIRE( GlideValue( 60.0f, 120.0f, 0 ) == 60.0f );
    REQUIRE( GlideValue( 60.0f, 120.0f, kGlideNs ) == 120.0f );
    REQUIRE( GlideValue( 60.0f, 120.0f, kGlideNs + 1 ) == 120.0f );
    // Downward glides work the same way.
    REQUIRE( GlideValue( 120.0f, 60.0f, 0 ) == 120.0f );
    REQUIRE( GlideValue( 120.0f, 60.0f, kGlideNs ) == 60.0f );
}

TEST_CASE( "glide is smoothstep: halfway in time is halfway in value, eased at the ends", "[fps_counter]" )
{
    // smoothstep(0.5) = 0.5 exactly.
    REQUIRE( GlideValue( 0.0f, 100.0f, kGlideNs / 2 ) == 50.0f );
    // smoothstep(0.25) = 0.15625, smoothstep(0.75) = 0.84375 -- slower at
    // the ends than a linear ramp would be.
    REQUIRE( std::abs( GlideValue( 0.0f, 100.0f, kGlideNs / 4 ) - 15.625f ) < 0.01f );
    REQUIRE( std::abs( GlideValue( 0.0f, 100.0f, kGlideNs * 3 / 4 ) - 84.375f ) < 0.01f );
    // Monotonic through the move.
    float flPrev = GlideValue( 0.0f, 100.0f, 0 );
    for ( uint64_t ul = 10 * kMs; ul <= kGlideNs; ul += 10 * kMs )
    {
        const float flNow = GlideValue( 0.0f, 100.0f, ul );
        REQUIRE( flNow >= flPrev );
        flPrev = flNow;
    }
}

TEST_CASE( "glide walks the shown integer through intermediate values", "[fps_counter]" )
{
    // The user's spec: digits tween, not jump. Sampled at the ~16 ms the
    // repaint-timer thread ticks at, a 60 -> 120 glide must show values
    // strictly between the two, and more than a couple of distinct ones.
    int nDistinct = 0, nLast = -1, nBetween = 0;
    for ( uint64_t ul = 0; ul <= kGlideNs; ul += 16 * kMs )
    {
        const int n = (int)std::lround( GlideValue( 60.0f, 120.0f, ul ) );
        if ( n != nLast ) { nDistinct++; nLast = n; }
        if ( n > 60 && n < 120 ) nBetween++;
    }
    REQUIRE( nDistinct >= 10 );
    REQUIRE( nBetween >= 10 );
}

TEST_CASE( "glide phases: moving for 300 ms, holding for the rest of the second", "[fps_counter]" )
{
    REQUIRE( GlideMoving( 0 ) );
    REQUIRE( GlideMoving( 150 * kMs ) );
    REQUIRE( GlideMoving( kGlideNs - 1 ) );
    REQUIRE_FALSE( GlideMoving( kGlideNs ) );
    REQUIRE_FALSE( GlideMoving( 700 * kMs ) );
    REQUIRE_FALSE( GlideMoving( kSmoothingWindowNs ) );

    // The spec's numbers, as constants: a 1 s sample period, a 300 ms move,
    // so the hold is the remaining 700 ms. Immediate looks at the last 100 ms.
    REQUIRE( kSmoothingWindowNs == 1000 * kMs );
    REQUIRE( kGlideNs == 300 * kMs );
    REQUIRE( kSmoothingWindowNs - kGlideNs == 700 * kMs );
    REQUIRE( kImmediateWindowNs == 100 * kMs );
}

TEST_CASE( "a glide to the same value holds flat", "[fps_counter]" )
{
    for ( uint64_t ul = 0; ul <= kGlideNs; ul += 16 * kMs )
        REQUIRE( GlideValue( 60.0f, 60.0f, ul ) == 60.0f );
}

// ---- digit-count pin ---------------------------------------------------------

TEST_CASE( "pinned digit count is never below 3", "[fps_counter]" )
{
    REQUIRE( PinnedDigitCount( 0 ) == 3 );
    REQUIRE( PinnedDigitCount( 7 ) == 3 );
    REQUIRE( PinnedDigitCount( 42 ) == 3 );
    REQUIRE( PinnedDigitCount( 99 ) == 3 );
    REQUIRE( PinnedDigitCount( 100 ) == 3 );
    REQUIRE( PinnedDigitCount( 999 ) == 3 );
}

TEST_CASE( "pinned digit count follows the number above 999", "[fps_counter]" )
{
    REQUIRE( PinnedDigitCount( 1000 ) == 4 );
    REQUIRE( PinnedDigitCount( 9999 ) == 4 );
    REQUIRE( PinnedDigitCount( 10000 ) == 5 );
    REQUIRE( PinnedDigitCount( 12345 ) == 5 );
    REQUIRE( PinnedDigitCount( 999999 ) == 6 );
}

TEST_CASE( "pinned digit count is safe against garbage", "[fps_counter]" )
{
    REQUIRE( PinnedDigitCount( -5 ) == 3 );       // negative reads as 0
    REQUIRE( PinnedDigitCount( 2147483647 ) == 7 ); // capped: the buffers hold 7 digits
}

// ---- update-mode mapping -------------------------------------------------------

TEST_CASE( "update mode: two choices, and legacy per_second maps to Smoothing", "[fps_counter]" )
{
    REQUIRE( UpdateModeToInt( "smoothing" ) == 0 );
    REQUIRE( UpdateModeToInt( "immediate" ) == 1 );
    REQUIRE( UpdateModeToInt( "per_second" ) == 0 );   // the subsumed mode
    REQUIRE( UpdateModeToInt( "" ) == 0 );             // and anything unrecognised
    REQUIRE( UpdateModeToInt( "garbage" ) == 0 );

    REQUIRE( std::string( UpdateModeFromInt( 0 ) ) == "smoothing" );
    REQUIRE( std::string( UpdateModeFromInt( 1 ) ) == "immediate" );
    REQUIRE( std::string( UpdateModeFromInt( 2 ) ) == "smoothing" ); // the old per_second slot no longer exists
    REQUIRE( std::string( UpdateModeFromInt( -1 ) ) == "smoothing" );

    // Round trip through the Choice row is stable for both live values.
    for ( const char *psz : { "smoothing", "immediate" } )
        REQUIRE( std::string( UpdateModeFromInt( UpdateModeToInt( psz ) ) ) == psz );
}

// ---- margin fix (2026-09-07): EdgeShift ------------------------------------
//
// FpsDisplay.cpp's MeasureFpsModule() comment and superdoc/features/
// fps-display.md's "Margin" section carry the full reasoning: the
// configured margin is the distance from the screen edge to the outermost
// DRAWN pixel. Nothing pins the digits to the readout's own box -- it is
// invisible (the backdrop that used to fill it, and the backdrop_padding
// that inflated it, were both removed 2026-09-09) -- so they sit inset from
// it by the glyph's own side bearing / cap-height gap (`flBearing`).
// EdgeShift is the pure correction that cancels exactly that, pulling the
// ink (or the outline's own outer ring, `flOutlineGeomRadius` px further
// out) flush to the margin instead.

TEST_CASE( "EdgeShift is zero on a centred axis", "[fps_counter][margin]" )
{
    // nSide == 1 (centre) has no edge to hug -- and so no margin claim to
    // satisfy -- whatever the bearing or the outline.
    REQUIRE( EdgeShift( 1, 1.0f, 0.0f ) == 0.0f );
    REQUIRE( EdgeShift( 1, 8.0f, 4.0f ) == 0.0f );
}

TEST_CASE( "EdgeShift with no outline cancels the glyph's own bearing", "[fps_counter][margin]" )
{
    // Near edge (left/top, nSide 0): moving the ink OUTWARD (toward the
    // edge) by exactly the bearing is a NEGATIVE shift (smaller x/y).
    REQUIRE( EdgeShift( 0, 1.0f, 0.0f ) == -1.0f );
    // Far edge (right/bottom, nSide 2): moving the ink outward is a
    // POSITIVE shift (larger x/y, i.e. toward the far screen edge).
    REQUIRE( EdgeShift( 2, 1.0f, 0.0f ) == 1.0f );
    // Measured 2026-09-07 (build-release/verify-shots/hud-margin-2026-09-07/):
    // ~8px of vertical cap-height headroom on the top edge, over and above
    // the 6px of backdrop_padding that also had to be cancelled then.
    REQUIRE( EdgeShift( 0, 8.0f, 0.0f ) == -8.0f );
    // A zero bearing needs no correction at all.
    REQUIRE( EdgeShift( 0, 0.0f, 0.0f ) == 0.0f );
}

TEST_CASE( "EdgeShift with an outline reduces the correction by the outline's own reach", "[fps_counter][margin]" )
{
    // The outline pokes `flOutlineGeomRadius` px further out than the fill
    // ink on every side, so the ink itself needs to sit that much LESS far
    // toward the edge than the no-outline case for the outline's own outer
    // ring to land on the margin.
    REQUIRE( EdgeShift( 0, 8.0f, 2.0f ) == -6.0f );
    REQUIRE( EdgeShift( 2, 8.0f, 2.0f ) == 6.0f );
    // An outline that reaches further out than the bearing flips the
    // shift's sign: the ink moves INWARD so the ring itself lands on the
    // margin. Reachable in practice now that backdrop_padding is gone --
    // a 1px horizontal bearing under a 4px outline is exactly this.
    REQUIRE( EdgeShift( 0, 1.0f, 4.0f ) == 3.0f );
    REQUIRE( EdgeShift( 2, 1.0f, 4.0f ) == -3.0f );
}

// ---- the visibility floor (2026-09-14) -------------------------------------
// fps-display.md's "Margin" section, 2026-09-14: the bearing is measured to
// the first glyph row/column that can actually show on screen, not to the
// metric box, because a round glyph's overshoot can clip a row at a few
// percent coverage that the composite's blend rounds back into the game.
// ScanInk() is the measurement; InkCoverageFloor() is what "can show" means
// per blend path, derived here from the blends themselves.

namespace
{
    // sRGB transfer, as src/shaders' srgbToLinear / linearToSrgb.
    double Decode( double v ) { return v <= 0.04045 ? v / 12.92 : std::pow( ( v + 0.055 ) / 1.055, 2.4 ); }
    double Encode( double l ) { return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow( l, 1.0 / 2.4 ) - 0.055; }

    // What one glyph pixel of coverage c (0..255) does to the screen, in
    // 8-bit counts, on the background where it shows best.
    //
    // Fixed digits, white, no outline: ImGui's straight-alpha blend stores
    // the texel as (c, c, c, c) over the cleared texture; the composite's
    // ALPHA_BLENDING_MODE_COVERAGE (alphamode.h) decodes the colour from
    // sRGB and multiplies it by the raw alpha, over black.
    double FixedOverBlack( int c )
    {
        const double a = c / 255.0;
        return Encode( Decode( a ) * a ) * 255.0;
    }
    // The outline: black stamps in straight alpha over white. Measured on
    // the bright scene (measurements.txt, 2026-09-14) the ring's outer row
    // darkens white by about its edge coverage in counts (15 -> 13, 27 ->
    // 42, 54 -> 65, 89 -> 119), so the model is the darkening 255 * c.
    double OutlineOverWhite( int c )
    {
        return 255.0 * ( c / 255.0 );
    }
    // Inverted digits over black: alphamode.h's invert path recovers the
    // coverage d = c and applies `inverted * d` in linear light, where
    // inverted = white over black.
    double InvertedOverBlack( int c )
    {
        return Encode( c / 255.0 ) * 255.0;
    }

    // The smallest coverage whose on-screen change reaches 1/16 of full
    // scale (16 counts), for a monotonic response.
    template <typename F>
    int SmallestVisible( F response )
    {
        for ( int c = 1; c <= 255; c++ )
            if ( response( c ) >= 16.0 )
                return c;
        return 256;
    }
}

TEST_CASE( "ink floors are the 1/16-of-full-scale rule under each blend path", "[fps_counter][margin]" )
{
    // The three constants are not tuned by eye: each is the first coverage
    // at which that path can change a pixel by 16/255. If a shader's blend
    // changes, this is what moves.
    REQUIRE( SmallestVisible( FixedOverBlack ) == kInkFloorFixed );
    REQUIRE( SmallestVisible( OutlineOverWhite ) == kInkFloorOutline );
    REQUIRE( SmallestVisible( InvertedOverBlack ) == kInkFloorInverted );

    // The rows this exists for: the pinned '0' at 36 px tops out at 8/255
    // on its first row, at 12 px bottoms out at 15/255 (measured 2026-09-14,
    // build-release/verify-shots/fps-hud-bottom-2026-09-14/). Under the
    // coverage blend those are under a quarter of a count -- invisible on
    // any background -- while the 36 px bottom row (89/255) is a plain
    // grey that must keep counting.
    REQUIRE( FixedOverBlack( 8 ) < 0.5 );
    REQUIRE( FixedOverBlack( 15 ) < 1.0 );
    REQUIRE( FixedOverBlack( 89 ) > 40.0 );
    REQUIRE( 8 < kInkFloorFixed );
    REQUIRE( 15 < kInkFloorFixed );
    REQUIRE( 89 >= kInkFloorFixed );
    // In Inverted mode that same 8/255 row lands near 50/255 on black --
    // very much visible -- so it has to keep counting there.
    REQUIRE( InvertedOverBlack( 8 ) > 40.0 );
    REQUIRE( 8 >= kInkFloorInverted );
}

TEST_CASE( "InkCoverageFloor picks the floor of the outermost drawn element", "[fps_counter][margin]" )
{
    REQUIRE( InkCoverageFloor( false, false ) == kInkFloorFixed );
    REQUIRE( InkCoverageFloor( true,  false ) == kInkFloorInverted );
    // With an outline the ring is the outermost pixel in either mode.
    REQUIRE( InkCoverageFloor( false, true ) == kInkFloorOutline );
    REQUIRE( InkCoverageFloor( true,  true ) == kInkFloorOutline );
}

TEST_CASE( "ScanInk boxes the pixels at or above the floor", "[fps_counter][margin]" )
{
    // A 6x5 synthetic '0': a faint overshoot row on top (peak 8), a strong
    // body, a faint bottom row (peak 15), an empty left column and a
    // faint right column (peak 3) -- every edge shape the real atlas
    // showed at 12 / 36 / 28 / 33 px.
    const uint8_t glyph[5][6] = {
        {   0,   0,   8,   6,   0,   0 },
        {   0, 200, 255, 255, 180,   3 },
        {   0, 255,  40,  40, 255,   2 },
        {   0, 210, 255, 255, 190,   3 },
        {   0,   0,  15,  12,   0,   0 },
    };
    const uint8_t *p = &glyph[0][0];

    SECTION( "the Fixed floor drops the faint rows and columns" )
    {
        const InkBox b = ScanInk( p, 6, 5, 6, 1, kInkFloorFixed );
        REQUIRE( b.bAny );
        REQUIRE( b.x0 == 1 );
        REQUIRE( b.y0 == 1 );
        REQUIRE( b.x1 == 5 );
        REQUIRE( b.y1 == 4 );
    }
    SECTION( "the Inverted floor keeps the overshoot rows but not the empty column" )
    {
        const InkBox b = ScanInk( p, 6, 5, 6, 1, kInkFloorInverted );
        REQUIRE( b.bAny );
        REQUIRE( b.x0 == 1 );
        REQUIRE( b.y0 == 0 );
        REQUIRE( b.x1 == 6 );
        REQUIRE( b.y1 == 5 );
    }
    SECTION( "a floor of 0 is treated as 1: padding is never ink" )
    {
        const InkBox b = ScanInk( p, 6, 5, 6, 1, 0 );
        REQUIRE( b.x0 == 1 );
        REQUIRE( b.x1 == 6 );
    }
    SECTION( "nothing at the floor reports no box" )
    {
        const InkBox b = ScanInk( p, 6, 5, 6, 1, 256 ); // above every 8-bit value
        REQUIRE_FALSE( b.bAny );
        REQUIRE( b.x0 == 0 );
        REQUIRE( b.y1 == 0 );
    }
    SECTION( "RGBA32 layout: alpha byte every fourth, row stride in bytes" )
    {
        // The same glyph, interleaved as RGBA with the coverage in A.
        uint8_t rgba[5][6][4] = {};
        for ( int y = 0; y < 5; y++ )
            for ( int x = 0; x < 6; x++ )
                rgba[y][x][3] = glyph[y][x];
        const InkBox b = ScanInk( &rgba[0][0][3], 6, 5, 6 * 4, 4, kInkFloorFixed );
        REQUIRE( b.bAny );
        REQUIRE( b.x0 == 1 );
        REQUIRE( b.y0 == 1 );
        REQUIRE( b.x1 == 5 );
        REQUIRE( b.y1 == 4 );
    }
    SECTION( "empty input is safe" )
    {
        REQUIRE_FALSE( ScanInk( nullptr, 6, 5, 6, 1, 1 ).bAny );
        REQUIRE_FALSE( ScanInk( p, 0, 5, 6, 1, 1 ).bAny );
    }
}
