#pragma once

// =============================================================================
//  Keybinds -- this fork's own compositor hotkeys, as editable data
// =============================================================================
// See superdoc/features/keybinds.md for the whole design and the reasons
// behind it. In one paragraph:
//
//   Until 2026-09-08 the three chords this fork binds (Right Shift, Left
//   Ctrl + Right Shift, Ctrl+Shift+O) were compiled into wlserver.cpp as
//   three hand-written `if`s. They are now three ACTIONS, each carrying one
//   chord, resolved from `overlay.keybinds` in global.json -- global, never
//   per profile, for exactly the reason every other `overlay.*` field is
//   (ConfigSchema.h's OverlaySettings comment, superdoc/features/profiles.md):
//   which key opens your settings is a fact about the player's keyboard, not
//   about the game.
//
// WHAT LIVES HERE AND WHAT DOES NOT. This file owns the chord GRAMMAR (parse,
// format, match), the STORE (defaults, overrides, conflicts), the GESTURE
// ENGINE (which action a key event completes) and the CAPTURE state machine.
// It deliberately does NOT know what an action DOES: ProcessKey() answers
// "which action fired, and should this key be swallowed", and wlserver.cpp --
// which already holds all the reasoning about opening the shell versus the
// launcher -- performs it. That split is what keeps this file free of the
// overlay, the compositor and ImGui, so tests/test_keybinds.cpp can hold the
// grammar and the engine with no compositor at all.
//
// THREADING. ProcessKey()/ClearGestureState() run on the WLSERVER thread, on
// every key event. Everything else (LoadFrom, SetChord, ResetAll, the capture
// begin/poll) is called from the steamcompmgr thread (the settings panel) or
// the console thread (the ConCommands). The chord table and the capture slot
// are behind one mutex; the config layer is touched ONLY from the callers'
// threads, never from ProcessKey -- config:: is documented as safe for a
// single thread, and the hotkey path is not that thread.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <xkbcommon/xkbcommon.h>

namespace gamescope::config { struct OverlaySettings; }

namespace gamescope::keybinds
{
	// -------------------------------------------------------------------------
	//  The actions
	// -------------------------------------------------------------------------
	// One action carries exactly ONE chord. "Open settings" has two chords
	// today, so it is two actions rather than one action with a list: a list
	// would need a row per element in a UI whose grammar is one row per value,
	// and "which of my two shell chords is this row" is a worse question than
	// "what is my alternate shell chord".
	enum class Action : uint8_t
	{
		Shell = 0,      // toggle the settings shell
		ShellAlt,       // ... and its alternate chord; same effect
		Launcher,       // the command palette alone over the game
		Count,
	};

	struct ActionInfo
	{
		const char *pszId;        // the on-disk key AND the registry row leaf
		const char *pszTitle;     // the row's label
		const char *pszDefault;   // the compiled-in chord
		const char *pszHelp;      // the row's required help text
	};

	const ActionInfo &Info( Action eAction );
	// The action with this id, or nullopt.
	std::optional<Action> ActionFromId( std::string_view svId );

	// -------------------------------------------------------------------------
	//  Chords
	// -------------------------------------------------------------------------
	// A chord is an unordered SET of terms, matched against the set of keysyms
	// currently held -- the same model gamescope's own external binding table
	// uses (WaylandServer/GamescopeActionBinding.h's Keybind_t).
	//
	// A term is either ONE specific keysym (`RShift`, `O`) or an EITHER-SIDE
	// modifier pair (`Ctrl` = Control_L or Control_R). Both are needed and
	// neither can replace the other: `Ctrl+Shift+O` has always worked with
	// whichever Ctrl and whichever Shift the user reaches for, while the shell's
	// own binding is specifically the RIGHT Shift and must not fire on the left
	// one (which games use as a walk/crouch modifier all match long).
	struct ChordTerm
	{
		xkb_keysym_t uA = XKB_KEY_NoSymbol;
		xkb_keysym_t uB = XKB_KEY_NoSymbol;   // NoSymbol => uA is the only sym this term accepts

		bool EitherSide() const { return uB != XKB_KEY_NoSymbol; }
		bool Accepts( xkb_keysym_t u ) const { return u == uA || ( uB != XKB_KEY_NoSymbol && u == uB ); }
		bool operator==( const ChordTerm &o ) const { return uA == o.uA && uB == o.uB; }
	};

	struct Chord
	{
		std::vector<ChordTerm> terms;

		bool Empty() const { return terms.empty(); }
		bool operator==( const Chord &o ) const;
		bool operator!=( const Chord &o ) const { return !( *this == o ); }
	};

	// Parses a chord string ("Ctrl+Shift+O", "RShift", "LCtrl+RShift").
	// Returns false and fills *psError with a one-sentence, user-facing reason
	// on anything it will not accept. Case-insensitive; `+` separates terms and
	// surrounding whitespace is ignored.
	bool ParseChord( std::string_view svText, Chord *pOut, std::string *psError );

	// The canonical spelling of a chord: modifiers first in Ctrl, Alt, Shift,
	// Super order, then the remaining keys by name. FormatChord( ParseChord( s ) )
	// is the string this file stores and displays, and ParseChord( FormatChord( c ) )
	// is c -- the round trip tests/test_keybinds.cpp pins.
	std::string FormatChord( const Chord &chord );

	// Every term is a modifier, so this chord is a TAP gesture (see the engine
	// note in Keybinds.cpp): it fires on release, never on press, and is never
	// swallowed.
	bool IsModifierOnly( const Chord &chord );

	// Does this exact set of held keysyms complete the chord?
	bool ChordMatches( const Chord &chord, const std::unordered_set<xkb_keysym_t> &setHeld );

	// -------------------------------------------------------------------------
	//  The reserved chord -- the way back from a bad binding
	// -------------------------------------------------------------------------
	// Ctrl+Alt+Shift+O always toggles the shell, is not an action, cannot be
	// rebound, and is refused as a value for any action. It exists because the
	// one genuinely dangerous edit in this whole feature is a shell chord the
	// user cannot type any more -- at which point the settings UI, and with it
	// the way to fix the binding, is gone.
	const Chord &ReservedChord();
	const char *ReservedChordText();

	// -------------------------------------------------------------------------
	//  The store
	// -------------------------------------------------------------------------
	// The chord in effect for an action, canonically formatted.
	std::string ChordTextFor( Action eAction );
	Chord       ChordFor( Action eAction );
	// Is this action still on its compiled-in default?
	bool IsDefault( Action eAction );

	// Sets one action's chord. Returns "" on success, or the one-sentence
	// reason it was refused (bad syntax, already used by another action, or the
	// reserved chord). An empty/blank string resets the action to its default,
	// which is what makes "clear it to get the default back" true.
	//
	// Writes global.json. NOT for the wlserver thread.
	std::string SetChord( Action eAction, std::string_view svChord );

	// Every action back to its compiled-in default. Writes global.json.
	void ResetAll();

	// Seeds the live table from a loaded config (main.cpp's startup apply and
	// the live-apply hook). An unparseable or conflicting stored chord falls
	// back to that action's default with a warning -- a broken config file must
	// never be able to leave the shell unreachable.
	void ApplyFromConfig( const config::OverlaySettings &overlay );

	// The `overlay.keybinds` map to persist: only the actions that differ from
	// their default appear, so a fresh config carries no keybind keys at all
	// and a reset removes them again.
	std::vector<std::pair<std::string, std::string>> OverridesForConfig();

	// -------------------------------------------------------------------------
	//  Capture -- "press the chord you want"
	// -------------------------------------------------------------------------
	// While a capture is armed EVERY key event is swallowed by ProcessKey():
	// nothing reaches the game, and no binding -- including the one being
	// rebound and the reserved chord -- can fire from the keys being pressed.
	// That is the requirement "the chord being captured cannot itself trigger
	// an action", satisfied structurally rather than by a list of exceptions.
	void BeginCapture( Action eAction );
	void CancelCapture();
	bool CaptureActive( Action *peAction = nullptr );

	enum class CaptureStatus : uint8_t { Idle, Captured, Cancelled };
	struct CaptureResult
	{
		CaptureStatus eStatus = CaptureStatus::Idle;
		Action        eAction = Action::Shell;
		std::string   sChord;      // canonical, only for Captured
	};
	// Takes and clears the pending result. Polled by the panel on its own
	// thread, which is where the config write then happens -- ProcessKey must
	// not touch config:: (see this file's threading note).
	CaptureResult TakeCaptureResult();

	// -------------------------------------------------------------------------
	//  The hotkey path -- wlserver.cpp only
	// -------------------------------------------------------------------------
	struct KeyResult
	{
		bool   bFired   = false;   // an action completed on this event
		Action eAction  = Action::Shell;
		bool   bConsume = false;   // swallow the key: it must not reach the game
	};

	// `normalizedKeysym` is NormalizeKeysymForHotkey()'s output for this event,
	// and `setHeld` is wlserver's pressed-sym ledger AFTER this event was
	// applied to it -- i.e. it contains the key on a press and does not on a
	// release. Both are exactly what wlserver_process_hotkeys() already has.
	KeyResult ProcessKey( xkb_keysym_t normalizedKeysym, bool bPress,
	                      const std::unordered_set<xkb_keysym_t> &setHeld );

	// Drops every half-finished gesture and any armed capture. Called from
	// wlserver_clear_pressed_hotkeys() -- at a keyboard-focus boundary nothing
	// mid-chord can be completed, because the release that would complete it
	// went somewhere else (Issue #102).
	void ClearGestureState();
}
