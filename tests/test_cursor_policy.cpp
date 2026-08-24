// Regression coverage for issue #69 / D29: which cursor is visible while the
// settings overlay owns the pointer.
//
// #69's guarantee is that exactly one cursor is visible in every mode, and
// never zero. D29 changed *which* one wins (prefer the host's real system
// cursor over ImGui's software arrow) without changing that guarantee, so
// these tests pin the guarantee rather than the preference -- zero cursors is
// the failure that is invisible on a nested desktop and fatal on a Steam Deck.
//
// See src/CursorPolicy.h for the policy itself and why it lives in its own
// dependency-free header.
#include <catch2/catch_test_macros.hpp>

#include "../src/CursorPolicy.h"

using namespace gamescope;

TEST_CASE( "NestedHostCursorUsable requires all three conditions", "[cursor_policy]" )
{
	// The only combination with a usable host cursor: we have a pointer, it
	// is not locked, and we have a system cursor image to show.
	CHECK( NestedHostCursorUsable( /* pointer */ true, /* locked */ false, /* image */ true ) );

	// No pointer device at all.
	CHECK_FALSE( NestedHostCursorUsable( false, false, true ) );
	// Pointer locked -- the grabbed-nested case from #69's table. The host
	// pins and hides the cursor, so it tracks nothing.
	CHECK_FALSE( NestedHostCursorUsable( true, true, true ) );
	// No system cursor image: the Wayland backend snapshots one from X11 at
	// startup, and legitimately has none when there was no display to
	// snapshot from.
	CHECK_FALSE( NestedHostCursorUsable( true, false, false ) );

	// And every remaining combination stays false.
	CHECK_FALSE( NestedHostCursorUsable( false, true, false ) );
	CHECK_FALSE( NestedHostCursorUsable( false, true, true ) );
	CHECK_FALSE( NestedHostCursorUsable( false, false, false ) );
	CHECK_FALSE( NestedHostCursorUsable( true, true, false ) );
}

TEST_CASE( "ImGui draws its cursor exactly when the host is not", "[cursor_policy]" )
{
	CHECK( OverlayShouldDrawSoftwareCursor( /* bHostCursorVisible = */ false ) );
	CHECK_FALSE( OverlayShouldDrawSoftwareCursor( /* bHostCursorVisible = */ true ) );
}

TEST_CASE( "Exactly one cursor is visible in every mode, never zero", "[cursor_policy]" )
{
	// The whole point of #69, restated as an exhaustive check over the
	// backend state that feeds the decision. For each combination, count the
	// cursors that end up on screen: the host's, plus ImGui's. It must always
	// be exactly 1 -- never 2 (the doubling #69 reported) and never 0 (the
	// worse bug #69 avoided by refusing the naive fix).
	for ( int i = 0; i < 8; i++ )
	{
		const bool bHavePointer     = ( i & 1 ) != 0;
		const bool bPointerLocked   = ( i & 2 ) != 0;
		const bool bHaveCursorImage = ( i & 4 ) != 0;

		const bool bHostCursorVisible =
			NestedHostCursorUsable( bHavePointer, bPointerLocked, bHaveCursorImage );
		const bool bImGuiCursorVisible =
			OverlayShouldDrawSoftwareCursor( bHostCursorVisible );

		const int nCursorsVisible = int( bHostCursorVisible ) + int( bImGuiCursorVisible );

		INFO( "pointer=" << bHavePointer << " locked=" << bPointerLocked << " image=" << bHaveCursorImage );
		CHECK( nCursorsVisible == 1 );
	}
}

TEST_CASE( "Modes with no host cursor at all keep ImGui's", "[cursor_policy]" )
{
	// Embedded (DRM/KMS) and OpenVR implement no host-cursor path at all --
	// paint_all() never gets a true out of PresentOverlayCursor() for them,
	// so the overlay is told the host cursor is absent. On a Steam Deck this
	// is the difference between a usable overlay and an invisible pointer.
	CHECK( OverlayShouldDrawSoftwareCursor( /* bHostCursorVisible = */ false ) );

	// Same answer via the nested predicate for a grabbed game, which is the
	// nested mode that behaves like embedded for cursor purposes.
	const bool bGrabbedNestedHostCursor =
		NestedHostCursorUsable( /* pointer */ true, /* locked */ true, /* image */ true );
	CHECK_FALSE( bGrabbedNestedHostCursor );
	CHECK( OverlayShouldDrawSoftwareCursor( bGrabbedNestedHostCursor ) );
}
