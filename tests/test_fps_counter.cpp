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
