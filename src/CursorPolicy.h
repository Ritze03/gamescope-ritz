#pragma once

// Which cursor is visible while the settings overlay owns the pointer.
//
// Issue #69 and D29 (which supersedes #69's first pass). Three separate things
// in this codebase can put a pointer on screen:
//
//   1. gamescope's own composited cursor plane (steamcompmgr.cpp,
//      CursorTexture::paint) -- the game's cursor, drawn into our output.
//   2. The overlay's own cursor, drawn into the overlay texture
//      (SettingsOverlay.cpp) as vector geometry by Overlay/CursorArt.cpp --
//      the same triangle the game side gets from SetDefaultCursorImage(), so
//      opening the overlay never changes what the pointer looks like. ImGui's
//      own software cursor (ImGuiIO::MouseDrawCursor) is never used.
//   3. A nested backend's real host-level cursor, drawn by the host
//      compositor (SDL_SetCursor(), wl_pointer_set_cursor()).
//
// While the overlay is capturing input, (1) is always suppressed -- it is
// positioned from wlserver.mouse_surface_cursorx/y, which stops being updated
// the moment the overlay takes input, so it would sit frozen as a stale ghost.
// That leaves (2) and (3), and the rule below picks between them.
//
// The invariant, which matters more than which one wins: **exactly one cursor
// is visible, and never zero**. Zero is the failure that is invisible on a
// nested desktop and fatal on a Steam Deck, so every predicate here is written
// so that the uncertain answer is the one that keeps the overlay's own cursor.
//
// Deliberately a standalone, dependency-free header: the policy is shared by
// two backends and the overlay, and being free of backend.h's include weight
// is what lets the tests link it directly (tests/test_cursor_policy.cpp).

namespace gamescope
{
	// Can a nested backend put a real host cursor on screen right now -- one
	// that actually tracks the pointer the user is moving?
	//
	// bHavePointer:      the backend has a host pointer device at all.
	// bPointerLocked:    a pointer constraint is active (a grabbed game). The
	//                    host cursor is then pinned and hidden by the host, so
	//                    it would not track anything even if we asked for it.
	//                    This is the grabbed-nested case #69 identified.
	// bHaveCursorImage:  we have a system cursor image to show. Under the
	//                    Wayland backend this is a snapshot taken from the host
	//                    at startup (GetX11HostCursor()), and it is genuinely
	//                    absent when there was no X11/XWayland display to
	//                    snapshot from.
	// bCursorEverywhere: the Cursor tab's "Use everywhere" toggle
	//                    (PanelCursor.h's CursorAppearance::bEverywhere). When
	//                    on, the user has explicitly asked for gamescope's own
	//                    art in place of any system cursor, so the host
	//                    cursor is never "usable" regardless of the first
	//                    three terms -- otherwise the overlay would draw its
	//                    own pointer *and* the host would keep presenting its
	//                    real one on top of it (two cursors, the #69 failure
	//                    mode this header exists to prevent).
	//
	// All four must hold. If any is false there is no usable host cursor and
	// the overlay's own must stay on.
	inline constexpr bool NestedHostCursorUsable( bool bHavePointer, bool bPointerLocked, bool bHaveCursorImage, bool bCursorEverywhere )
	{
		return !bCursorEverywhere && bHavePointer && !bPointerLocked && bHaveCursorImage;
	}

	// Should the overlay draw a cursor of its own? The complement of "a real
	// system cursor is already doing the job", so that exactly one of the two
	// is ever on screen.
	//
	// Trivial by construction, and that is the point: it is the single place
	// the complement is taken, so the two cursor sources cannot drift into
	// both-on (the #69 bug) or both-off (the worse one) by being decided
	// independently at two call sites.
	inline constexpr bool OverlayShouldDrawSoftwareCursor( bool bHostCursorVisible )
	{
		return !bHostCursorVisible;
	}
}
