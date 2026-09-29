#pragma once

// =============================================================================
//  LaunchOptions -- which settings were pinned by a `gamescope` launch flag
// =============================================================================
// See superdoc/features/launch-option-lock.md for the whole design. In one
// paragraph: a handful of settings rows have a matching command-line flag
// (-F/--filter, --adaptive-sync, ...). If the game was launched WITH that
// flag, the value it set has to survive the session -- a profile switch or
// an in-overlay edit that silently overrode it would be confusing (the
// player set it on the command line for a reason, and the overlay would be
// lying about what's actually in effect). This module is the bookkeeping:
// one bit per lockable option, set ONCE at startup from `main.cpp`'s and
// `steamcompmgr.cpp`'s getopt loops, and read everywhere else (the
// registry's row declarations, `ritz_apply_config_live()`) to decide
// whether that row/push is allowed to touch the live value this session.
//
// Why: the user, verbatim -- "Make sure that when a value is overwritten,
// as a startup argument, ... that it can't be modified inside of the game
// anymore. Just to avoid confusion with the user, just add like a small red
// or yellow label behind it that warns the user about it being set through
// a launch argument to avoid confusion in general."
//
// WRITE ONCE, AT STARTUP. MarkGiven() is only ever called from the two
// getopt loops, both of which finish before steamcompmgr's paint thread (or
// any overlay code) can run. Given()/Spelling() are read-only after that,
// from any code that runs later -- no lock needed, the same "write before
// the readers exist" contract g_bForceWindowsFullscreenStartup already
// relies on (main.cpp's apply_ritz_config_to_startup_state()).
//
// SPELLING. getopt_long collapses a flag's short and long forms into the
// same case, so for an option reached through a `case 'x':` branch this
// records a canonical "-x/--long-name" string rather than which spelling
// the player actually typed (getopt doesn't tell us). An option reached
// only through the `case 0:` long-option branch records the exact name
// getopt matched (gamescope_options[opt_index].name), since that IS known
// -- --sharpness and --fsr-sharpness, e.g., report themselves distinctly.

#include <cstdint>

namespace gamescope::LaunchOptions
{
	// One entry per row this project locks against its launch flag.
	// superdoc/features/launch-option-lock.md has the full flag/row table;
	// keep the two in sync when adding an entry here.
	enum class Opt : uint8_t
	{
		NestedWidth,             // -w / --nested-width
		NestedHeight,            // -h / --nested-height
		NestedRefresh,           // -r / --nested-refresh
		Scaler,                  // -S / --scaler
		Filter,                  // -F / --filter
		Sharpness,                // --sharpness / --fsr-sharpness
		GrabKeyboard,             // -g / --grab
		GrabCursor,               // --force-grab-cursor
		ForceWindowsFullscreen,   // --force-windows-fullscreen
		AdaptiveSync,             // --adaptive-sync
		ImmediateFlips,           // --immediate-flips
		HdrEnabled,               // --hdr-enabled
		FramerateLimit,           // --framerate-limit

		Count,
	};

	// Called once, from the getopt loop's own case, the moment that flag is
	// recognized. `pszSpelling` is copied (not borrowed), so a call site can
	// hand it a temporary.
	void MarkGiven( Opt eOpt, const char *pszSpelling );

	// True for the rest of the process's life once MarkGiven() has been
	// called for that option.
	bool Given( Opt eOpt );

	// The flag text to show the player (e.g. "-F/--filter"), or "" if
	// Given() is false. Stable for the process's lifetime once set.
	const char *Spelling( Opt eOpt );
}
