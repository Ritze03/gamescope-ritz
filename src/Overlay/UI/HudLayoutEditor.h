// HUD layouts Phase 3 -- the drag-and-drop placement editor
// (superdoc/architecture/hud-layouts.md's "seam for Phase 3's editor").
//
// Lives in the Shell's own ImGui context, not the HUD's -- Overlay/
// FpsDisplay.cpp's own file-header comment explains why the HUD keeps a
// fully separate context (lifetime independence from the settings panel),
// and the consequence that matters here is the one the design doc's
// "Why:" note spells out: that context receives NO pointer input at all
// (DrainInputQueue(), SettingsOverlay.cpp, only ever feeds the Shell's
// context). So this editor draws over the live game *and* the live HUD
// from the SAME context/frame Shell.cpp already has pointer input wired
// into, converting the HUD's own module geometry (Overlay/FpsDisplay.h's
// FpsDisplay_GetModuleRects(), HUD display-pixel space) into the Shell's
// own io.DisplaySize by simple per-axis ratio -- the two are not
// guaranteed to match (see FpsDisplay_GetModuleRects()'s own comment).
//
// Public surface deliberately mirrors Overlay/UI/Shell.h's own shape (one
// enter, one per-frame draw, one Esc hook) rather than exposing anything
// an author could reach into -- there is nothing here for another file to
// drive except "is it up," "start it," "draw it," "let it own Esc."
#pragma once

namespace gamescope::ui::hudedit
{
	// True from a successful Begin() until Save()/Cancel() (via
	// HandleEscape()) ends the session. Shell::Draw() checks this to take
	// its own early-return branch -- see Shell.cpp's s_bLauncherOnly for
	// the identical precedent this follows.
	bool IsActive();

	// Enters edit mode: resolves the layout the active session's HUD
	// currently shows into a working copy (and a snapshot of the same, for
	// Cancel) and arms IsActive(). Safe to call again while already active
	// (re-resolves fresh, discarding any unsaved in-progress edit) --
	// there is exactly one entry point (FpsDisplay.cpp's "Edit placement"
	// action) and it does not itself guard against a double-invoke.
	void Begin();

	// One frame of the editor. Called from Shell::Draw() only while
	// IsActive() -- see Shell.cpp for the exact call site (the same
	// early-return shape s_bLauncherOnly uses). Runs inside the Shell's
	// live NewFrame()/Render() bracket, same as every other thing Shell.cpp
	// draws, so ordinary ImGui mouse/keyboard queries work here.
	//
	// Calls force_repaint() unconditionally on every frame it runs -- the
	// HUD's own 500ms repaint-timer thread (FpsDisplay.cpp) is far too slow
	// to carry a live drag.
	void Draw();

	// Esc's editor-specific rung in Shell.cpp's RunKeyboard() precedence
	// chain (D26) -- cancels the in-progress edit (discarding the working
	// copy, restoring nothing since nothing was ever persisted mid-drag)
	// and exits edit mode. Returns true when it consumed the key (i.e. was
	// active), false otherwise, so the caller knows whether to keep
	// falling through its own chain.
	bool HandleEscape();
}
