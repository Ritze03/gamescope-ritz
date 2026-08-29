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

TEST_CASE( "NestedHostCursorUsable requires all four conditions", "[cursor_policy]" )
{
	// The only combination with a usable host cursor: we have a pointer, it
	// is not locked, we have a system cursor image to show, and the user
	// hasn't asked for gamescope's own art everywhere.
	CHECK( NestedHostCursorUsable( /* pointer */ true, /* locked */ false, /* image */ true, /* everywhere */ false ) );

	// No pointer device at all.
	CHECK_FALSE( NestedHostCursorUsable( false, false, true, false ) );
	// Pointer locked -- the grabbed-nested case from #69's table. The host
	// pins and hides the cursor, so it tracks nothing.
	CHECK_FALSE( NestedHostCursorUsable( true, true, true, false ) );
	// No system cursor image: the Wayland backend snapshots one from X11 at
	// startup, and legitimately has none when there was no display to
	// snapshot from.
	CHECK_FALSE( NestedHostCursorUsable( true, false, false, false ) );
	// "Use everywhere" on: the user has explicitly asked for gamescope's own
	// pointer in place of the host's, even though the first three terms are
	// otherwise all favourable.
	CHECK_FALSE( NestedHostCursorUsable( true, false, true, true ) );

	// And every remaining combination stays false.
	CHECK_FALSE( NestedHostCursorUsable( false, true, false, false ) );
	CHECK_FALSE( NestedHostCursorUsable( false, true, true, false ) );
	CHECK_FALSE( NestedHostCursorUsable( false, false, false, false ) );
	CHECK_FALSE( NestedHostCursorUsable( true, true, false, false ) );
	CHECK_FALSE( NestedHostCursorUsable( true, true, true, true ) );
	CHECK_FALSE( NestedHostCursorUsable( false, false, false, true ) );
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
	for ( int i = 0; i < 16; i++ )
	{
		const bool bHavePointer     = ( i & 1 ) != 0;
		const bool bPointerLocked   = ( i & 2 ) != 0;
		const bool bHaveCursorImage = ( i & 4 ) != 0;
		const bool bCursorEverywhere = ( i & 8 ) != 0;

		const bool bHostCursorVisible =
			NestedHostCursorUsable( bHavePointer, bPointerLocked, bHaveCursorImage, bCursorEverywhere );
		const bool bImGuiCursorVisible =
			OverlayShouldDrawSoftwareCursor( bHostCursorVisible );

		const int nCursorsVisible = int( bHostCursorVisible ) + int( bImGuiCursorVisible );

		INFO( "pointer=" << bHavePointer << " locked=" << bPointerLocked << " image=" << bHaveCursorImage << " everywhere=" << bCursorEverywhere );
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
		NestedHostCursorUsable( /* pointer */ true, /* locked */ true, /* image */ true, /* everywhere */ false );
	CHECK_FALSE( bGrabbedNestedHostCursor );
	CHECK( OverlayShouldDrawSoftwareCursor( bGrabbedNestedHostCursor ) );
}

TEST_CASE( "Use everywhere keeps gamescope's own cursor even where the host's would be usable", "[cursor_policy]" )
{
	// This is issue: with force-grab OFF and the overlay open, an otherwise
	// fully-usable host cursor (pointer present, unlocked, image available)
	// must still lose to gamescope's own art once "Use everywhere" is on --
	// that was the reported bug: the user saw the host's default cursor
	// instead of the custom one in exactly this state.
	const bool bHostCursorVisible =
		NestedHostCursorUsable( /* pointer */ true, /* locked */ false, /* image */ true, /* everywhere */ true );
	CHECK_FALSE( bHostCursorVisible );
	CHECK( OverlayShouldDrawSoftwareCursor( bHostCursorVisible ) );
}
