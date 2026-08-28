// The pointer the settings overlay draws for itself, using the desktop's
// Xcursor theme instead of ImGui's built-in arrow.
//
// WHY THIS EXISTS. Three things in this codebase can put a pointer on screen
// (see CursorPolicy.h for the full rule). While the overlay owns input, two of
// them are unavailable by construction: gamescope's composited cursor plane is
// suppressed -- it is positioned from wlserver.mouse_surface_cursorx/y, which
// stops updating the moment the overlay takes input, so it would sit frozen as
// a stale ghost (the issue #69 fix in steamcompmgr.cpp) -- and a real host
// cursor needs the pointer *unlocked*, which --force-grab-cursor rules out.
// What was left was ImGui's own software cursor, a fixed vector arrow with no
// concept of an Xcursor theme, so the pointer visibly changed identity the
// instant the overlay opened. This module makes the overlay draw the same
// themed image the composited plane shows when the overlay is closed, so the
// two states agree.
//
// This is not optional and has no setting. It replaces the short-lived
// "Use system cursor theme" toggle, which only ever existed to work around
// this bug -- see superdoc/features/cursor-pipeline.md.
#pragma once

namespace gamescope::overlay
{
	// Loads the theme image (once per process) and makes sure the current
	// ImGui context's font atlas holds a copy of it. Call once per frame
	// BEFORE ImGui::NewFrame(), after any pending font-atlas rebuild has been
	// applied -- it touches the atlas, so it must not run mid-frame.
	//
	// Returns true when Draw() below will produce a themed cursor this frame.
	// A false return is the caller's signal to fall back to ImGui's built-in
	// arrow (ImGuiIO::MouseDrawCursor), which is why the decision is made out
	// here rather than inside Draw(): the "exactly one cursor, never zero"
	// invariant in CursorPolicy.h has to be settled before the frame starts,
	// not discovered halfway through it.
	bool ThemeCursor_Prepare();

	// Draws the themed cursor into the current context's foreground draw list
	// at ImGuiIO::MousePos, hotspot-corrected. Call inside the frame, once,
	// and only when ThemeCursor_Prepare() returned true for this same frame.
	void ThemeCursor_Draw();
}
