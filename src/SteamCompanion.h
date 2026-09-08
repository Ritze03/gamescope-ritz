#pragma once

// =============================================================================
//  The Steam companion overlay -- the runtime half
// =============================================================================
// A browser window, launched on GAMESCOPE'S OWN Xwayland, pointed at Steam's
// web chat, promoted to a fullscreen interactive overlay by the same
// STEAM_OVERLAY / STEAM_INPUT_FOCUS properties steamcompmgr already reads for
// the Steam Deck's own overlay. Bound to Ctrl+Shift+Tab by default
// (keybinds::Action::Companion), configured in Settings -> System -> Steam
// chat, documented in superdoc/features/steam-companion.md.
//
// It is NOT Steam's real Friends window and cannot be: that window belongs to
// the HOST's X server, and no protocol on this host can route a click back
// into it. superdoc/planning/steam-friends-window.md measured that, tried the
// alternatives and rejected them; this is what survived.
//
// THE RULES AND THE PART OF THE FILE THAT ENFORCES EACH:
//
//   * Launched on demand, never resident -- the first press spawns, later
//     presses only toggle two X properties. Spawn() is reached from
//     ToggleAct::Launch and nowhere else.
//   * Spawned once -- a launch while a child is alive is impossible by
//     construction: PlanToggle() cannot answer Launch while g_State.nPid > 0,
//     and the reaper is what makes that field true.
//   * Never outlives gamescope -- own process group + PR_SET_PDEATHSIG +
//     Shutdown() from steamcompmgr_exit(), belt, braces and a third belt
//     (main.cpp's KillAllChildren). See Shutdown()'s comment.
//   * Never blocks the compositor -- Tick() does one waitpid(WNOHANG), one
//     walk of the window list and at most three XChangeProperty calls. There
//     is no wait-for-the-browser anywhere.
//   * A browser that died while hidden relaunches cleanly on the next press,
//     because "is it running" is re-answered by that waitpid every tick.
//
// THREADING. RequestToggle() is called from the WLSERVER thread (the hotkey
// path) and does nothing but bump an atomic and nudge steamcompmgr.
// Everything else -- the config read, the fork, the toasts and every X call --
// runs on the STEAMCOMPMGR thread, from Tick(), which is where the window list
// and Notifications::Show() are legal to touch.

#include <string>

struct xwayland_ctx_t;
struct steamcompmgr_win_t;

namespace gamescope::companion
{
	// wlserver thread. Queues one press of the chord; returns immediately.
	void RequestToggle();

	// steamcompmgr thread, once per main-loop iteration, with the root ctx.
	// Drains queued presses, reaps a dead browser, finds a newly-mapped
	// companion window and applies the show/hide properties.
	void Tick( xwayland_ctx_t *ctx );

	// steamcompmgr thread. Is this the companion's own window? Used by
	// handle_desktop_window() to keep its "resize a desktop window to its
	// requested size" rule off a window this file sizes itself.
	bool OwnsWindow( const steamcompmgr_win_t *w );

	// steamcompmgr thread, from steamcompmgr_exit(). Kills the browser and
	// everything it spawned.
	void Shutdown();

	// One line for the settings area's Status row and its rail summary: the
	// chord, and whether the configured browser is actually there. `Why a
	// readout and not a validator:` the command is a free-text row, and the
	// question a user actually has after typing one is "will this work" --
	// which is answered by looking for the program, not by parsing.
	std::string StatusText();
}
