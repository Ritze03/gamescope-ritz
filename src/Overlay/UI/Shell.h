// The E2 shell -- SPEC.md §8.1's slab, and the three regions inside it.
//
// This is the whole of its public surface: one function. Everything else
// about the shell is private to Shell.cpp, deliberately, because SPEC §5.2
// clause 0 ("the Inspector has no authoring API") is only credible if there
// is no header a category file could include to reach into it.
//
// ---------------------------------------------------------------------------
// WHAT REPLACED WHAT
// ---------------------------------------------------------------------------
// This shell IS the settings overlay. P5 deleted the legacy dock and its five
// floating windows, and with them the whole window-management layer: no
// floating windows, no dragging, no resizing, no z-order, no tiling, no
// per-panel position memory.
//
// That is the point of the direction rather than a simplification of it --
// 39% of this project's overlay bug-fix commits were floating-window
// management, and nine of those sixteen were fixes to earlier fixes. A bug
// class is not fixed by fixing its instances; it is fixed by deleting the
// thing that produces them.
//
// The slab is centred, sized from the surface, and cannot be moved. There is
// exactly one of it.
//
// THERE IS NO `overlay_e2` ConVar ANY MORE. It was the flag both shells lived
// behind, and P5 removed it with the path it selected: a flag with one
// reachable value is not a flag, and a ConVar that accepts `0` while still
// drawing this shell would be a control that renders and does nothing --
// precisely the defect class (#25, #68, the computed-but-undrawn column
// count, the registered-nowhere Meter kind) this redesign spent itself
// removing. It was never a config key (AUTONOMOUS-DECISIONS.md D12 kept it
// runtime-only), so no config file mentions it and none can fail to load
// because it is gone. See D21.
#pragma once

namespace gamescope::ui::shell
{
	// One frame of the shell. Called from SettingsOverlay.cpp's draw loop
	// between NewFrame() and Render().
	//
	// Draws the slab, the rail, the sheet and the inspector; runs the
	// keyboard map (SPEC §8.2); and hosts whatever the selected area is.
	void Draw();

	// D22: ask for the command palette to be open on the next frame.
	//
	// The second function on this header, and the file comment above still
	// means what it says -- this is not an authoring API and reaches into
	// nothing. It exists because the palette's binding moved OUT of the
	// shell's own keyboard map and into wlserver's hotkey table (Left Ctrl +
	// Right Ctrl), and wlserver runs on a different thread from the one that
	// owns every piece of shell state.
	//
	// So it is a REQUEST, not a setter: it stores one atomic flag that
	// Draw() consumes at the top of the next frame, on the thread that owns
	// the state. Writing s_bPaletteOpen directly from wlserver would be a
	// plain data race against the frame that is drawing the palette.
	//
	// Safe to call from any thread, and idempotent -- asking twice before a
	// frame runs opens the palette once.
	void RequestPalette();

	// D25: open the palette AS A LAUNCHER -- alone, over the game, with no
	// slab, no rail, no sheet and no inspector behind it.
	//
	// WHY THIS IS A SECOND REQUEST AND NOT A PARAMETER ON THE FIRST. The two
	// callers want genuinely different things, and the difference is not a
	// flag on one behaviour: RequestPalette() is "put the palette on top of
	// the shell the user is already looking at", and this is "show only the
	// launcher, and give the game straight back on Esc". They differ in what
	// Esc means, in what is drawn, and in who owns closing the overlay after.
	//
	// Why the launcher exists at all: direction B's premise, which the user
	// kept as a FEATURE rather than as the whole UI -- mid-game you usually
	// want ONE setting, not a tour of the settings surface. Opening the
	// entire shell to reach one row defeats that, which is exactly what the
	// Left+Right Ctrl binding used to do.
	//
	// Same threading contract as RequestPalette(): a request consumed on the
	// next frame by the thread that owns the state.
	void RequestLauncher();

	// D25: whether the shell is currently showing the launcher ALONE.
	//
	// wlserver needs this to tell "the overlay is open" from "the launcher is
	// up", because settings_overlay_visible is true in both cases and the
	// Left+Right Ctrl binding has to behave differently in each -- over the
	// shell it lays the palette on top, over the game it opens the launcher.
	// Atomic, readable from any thread.
	bool LauncherOnlyActive();

	// Issue #88: whether the palette is currently open at all -- as the
	// launcher, or laid over an already-open shell. wlserver reads this to
	// turn Left+Right Ctrl into a genuine toggle: nothing open means the
	// combo should open (as the launcher, or over the shell -- see
	// LauncherOnlyActive() above), something open means it should close
	// exactly what it opened. Atomic, readable from any thread, same
	// threading contract as LauncherOnlyActive().
	bool PaletteActive();

	// Issue #88: close the palette WITHOUT touching whatever is behind it.
	// This is the combo's "close" half when the palette was laid over a
	// shell the user opened separately (LauncherOnlyActive() false) -- the
	// launcher-only case closes by hiding the whole overlay instead, since
	// there is nothing else on screen to preserve. A request consumed by
	// Draw() on its own thread, same contract as RequestPalette().
	void RequestClosePalette();

	// D25: the overlay was hidden by something OUTSIDE the shell -- the Right
	// Ctrl tap, Ctrl+Shift+O, gamescopectl. Drops any launcher state, so the
	// next open is not a launcher nobody asked for.
	//
	// Called from cv_settings_overlay_visible's own callback, so it can
	// arrive on any thread; like the two requests above it stores an atomic
	// that Draw() consumes.
	void NotifyOverlayHidden();

	// Item 2 (2026-08-29): the user's own wording -- "Closing the Launcher
	// with CtrlL and CtrlR should not empty the search field. Closing it
	// with RCtrl only or Escape should empty the search, like it does now."
	//
	// Called from wlserver's hotkey thread, ONE line before it hides the
	// overlay to close a launcher the combo itself opened
	// (wlserver_check_ctrl_shortcuts' `bLauncherOnly` branch) -- the only
	// one of the three close routes this applies to. Right Ctrl's tap
	// (SettingsOverlay_ToggleVisible) and Escape (RunPaletteKeyboard, on the
	// Draw() thread) call neither this nor anything like it, so they keep
	// today's clear-on-next-open behaviour by simply not asking.
	//
	// Stores one atomic bit, not the query text itself: the text lives in
	// s_sPaletteQuery, which only the Draw() thread ever touches, and this
	// function runs on wlserver's. OpenPalette() consumes the bit the next
	// time it actually runs, on the thread that owns the string -- so the
	// two threads never race on anything but a bool.
	void RequestLauncherClosePreservingQuery();
}
