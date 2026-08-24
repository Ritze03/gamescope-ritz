#pragma once

// Which cursor is visible while the settings overlay owns the pointer.
//
// Issue #69 and D29 (which supersedes #69's first pass). Three separate things
// in this codebase can put a pointer on screen:
//
//   1. gamescope's own composited cursor plane (steamcompmgr.cpp,
//      CursorTexture::paint) -- the game's cursor, drawn into our output.
//   2. ImGui's software cursor, drawn into the overlay texture
//      (ImGuiIO::MouseDrawCursor, SettingsOverlay.cpp).
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
// so that the uncertain answer is the one that keeps ImGui's cursor.
//
// Deliberately a standalone, dependency-free header: the policy is shared by
// two backends and the overlay, and being free of backend.h's include weight
// is what lets the tests link it directly (tests/test_cursor_policy.cpp).

namespace gamescope
{
	// Can a nested backend put a real host cursor on screen right now -- one
	// that actually tracks the pointer the user is moving?
	//
	// bHavePointer:     the backend has a host pointer device at all.
	// bPointerLocked:   a pointer constraint is active (a grabbed game). The
	//                   host cursor is then pinned and hidden by the host, so
	//                   it would not track anything even if we asked for it.
	//                   This is the grabbed-nested case #69 identified.
	// bHaveCursorImage: we have a system cursor image to show. Under the
	//                   Wayland backend this is a snapshot taken from the host
	//                   at startup (GetX11HostCursor()), and it is genuinely
	//                   absent when there was no X11/XWayland display to
	//                   snapshot from.
	//
	// All three must hold. If any is false there is no usable host cursor and
	// ImGui's must stay on.
	inline constexpr bool NestedHostCursorUsable( bool bHavePointer, bool bPointerLocked, bool bHaveCursorImage )
	{
		return bHavePointer && !bPointerLocked && bHaveCursorImage;
	}

	// Should ImGui draw its own software cursor? The complement of "a real
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
