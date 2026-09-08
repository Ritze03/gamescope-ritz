// Unit tests for src/Keybinds.cpp -- the editable-hotkey system's PURE half:
// the chord grammar (parse / format / round trip), the matcher, the
// modifier-only rule, the defaults-when-unset rule, conflict detection, and
// the gesture engine.
//
// No compositor, no ImGui, no config file. The engine is exercised by feeding
// ProcessKey() exactly what wlserver_process_hotkeys() feeds it -- a
// normalised keysym, a press/release flag, and the held-sym set AFTER the
// event was applied to the ledger -- so what these tests hold is the same
// function the compositor runs, not a model of it.
//
// The one thing NOT covered here is persistence: SetChord() writes global.json
// and there is no config home in this binary's environment. That half is
// covered by scripts' live verification (build-release/verify-shots/
// keybinds-2026-09-08) and by tests/test_config.cpp's own overlay round trip.

#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

#include "Keybinds.h"

using namespace gamescope::keybinds;

namespace
{
	Chord Parse( const char *psz )
	{
		Chord c;
		std::string sErr;
		REQUIRE( ParseChord( psz, &c, &sErr ) );
		return c;
	}

	std::unordered_set<xkb_keysym_t> Held( std::initializer_list<xkb_keysym_t> syms )
	{
		return std::unordered_set<xkb_keysym_t>( syms );
	}

	// Drives a whole gesture through the engine the way wlserver does: the
	// held set is maintained here and handed over post-event, exactly as the
	// ledger does.
	struct Keyboard
	{
		std::unordered_set<xkb_keysym_t> held;

		KeyResult Press( xkb_keysym_t u )
		{
			held.insert( u );
			return ProcessKey( u, true, held );
		}
		KeyResult Release( xkb_keysym_t u )
		{
			held.erase( u );
			return ProcessKey( u, false, held );
		}
	};
}

// ---------------------------------------------------------------------------
//  Grammar
// ---------------------------------------------------------------------------
TEST_CASE( "chords round-trip through parse and format", "[keybinds]" )
{
	// Exactly the strings this fork ships as defaults, plus the reserved one.
	const char *kCanonical[] = {
		"RShift", "Ctrl+Shift+O", "LCtrl+RShift", "Ctrl+Alt+Shift+O",
		"Super+K", "F5", "LAlt+Tab", "Ctrl+Alt+Shift+Super+P",
	};

	for ( const char *psz : kCanonical )
	{
		const Chord c = Parse( psz );
		INFO( psz );
		CHECK( FormatChord( c ) == psz );          // already canonical
		CHECK( Parse( FormatChord( c ).c_str() ) == c );   // and stable under a second pass
	}
}

TEST_CASE( "chord spelling is normalised to one canonical form", "[keybinds]" )
{
	// Case, spacing and term order are all free; the stored string is not.
	CHECK( FormatChord( Parse( "ctrl+shift+o" ) ) == "Ctrl+Shift+O" );
	CHECK( FormatChord( Parse( "  O + Shift + Ctrl " ) ) == "Ctrl+Shift+O" );
	CHECK( FormatChord( Parse( "rshift" ) ) == "RShift" );
	// A raw xkb keysym name for a modifier resolves to the sided token, so
	// the two spellings are the same chord and store identically.
	CHECK( FormatChord( Parse( "Control_L+Shift_R" ) ) == "LCtrl+RShift" );
	CHECK( Parse( "Control_L+Shift_R" ) == Parse( "LCtrl+RShift" ) );
	// Letters normalise to upper case, the way the hotkey layer does.
	CHECK( FormatChord( Parse( "ctrl+p" ) ) == "Ctrl+P" );
}

TEST_CASE( "bad chords are refused with a reason", "[keybinds]" )
{
	Chord c;
	std::string sErr;

	CHECK_FALSE( ParseChord( "", &c, &sErr ) );
	CHECK_FALSE( sErr.empty() );

	CHECK_FALSE( ParseChord( "Ctrl+", &c, &sErr ) );
	CHECK_FALSE( ParseChord( "Ctrl++O", &c, &sErr ) );
	CHECK_FALSE( ParseChord( "Ctrl+Wumpus", &c, &sErr ) );

	// Overlapping terms cannot both be satisfied by one keyboard, so a chord
	// containing them could never fire -- refused rather than deduplicated.
	CHECK_FALSE( ParseChord( "Ctrl+LCtrl", &c, &sErr ) );
	CHECK_FALSE( ParseChord( "O+o", &c, &sErr ) );

	CHECK_FALSE( ParseChord( "Ctrl+Shift+Alt+Super+P+Q", &c, &sErr ) );
}

TEST_CASE( "either-side and sided modifiers mean different things", "[keybinds]" )
{
	const Chord either = Parse( "Ctrl+Shift+O" );
	const Chord sided  = Parse( "LCtrl+RShift" );

	CHECK( either != sided );

	// Either-side accepts whichever hand.
	CHECK( ChordMatches( either, Held( { XKB_KEY_Control_L, XKB_KEY_Shift_L, XKB_KEY_O } ) ) );
	CHECK( ChordMatches( either, Held( { XKB_KEY_Control_R, XKB_KEY_Shift_R, XKB_KEY_O } ) ) );
	CHECK( ChordMatches( either, Held( { XKB_KEY_Control_L, XKB_KEY_Shift_R, XKB_KEY_O } ) ) );

	// Sided does not.
	CHECK( ChordMatches( sided, Held( { XKB_KEY_Control_L, XKB_KEY_Shift_R } ) ) );
	CHECK_FALSE( ChordMatches( sided, Held( { XKB_KEY_Control_R, XKB_KEY_Shift_R } ) ) );
	CHECK_FALSE( ChordMatches( sided, Held( { XKB_KEY_Control_L, XKB_KEY_Shift_L } ) ) );

	// A chord is the WHOLE held set, never a subset: an extra key held means
	// a different gesture. This is what stops Ctrl+Shift+O being eaten by the
	// Ctrl+Shift prefix its own combo starts with.
	CHECK_FALSE( ChordMatches( sided, Held( { XKB_KEY_Control_L, XKB_KEY_Shift_R, XKB_KEY_O } ) ) );
	CHECK_FALSE( ChordMatches( either, Held( { XKB_KEY_Control_L, XKB_KEY_Shift_L } ) ) );
}

TEST_CASE( "a chord of only modifiers is a tap", "[keybinds]" )
{
	CHECK( IsModifierOnly( Parse( "RShift" ) ) );
	CHECK( IsModifierOnly( Parse( "LCtrl+RShift" ) ) );
	CHECK_FALSE( IsModifierOnly( Parse( "Ctrl+Shift+O" ) ) );
	CHECK_FALSE( IsModifierOnly( Parse( "F5" ) ) );
}

// ---------------------------------------------------------------------------
//  Defaults and the store
// ---------------------------------------------------------------------------
TEST_CASE( "an unset binding is the compiled-in default", "[keybinds]" )
{
	// Nothing in this binary has ever written a config, so every action is
	// on its default -- which is the fresh-install case the whole "absent
	// means default" rule exists to make true.
	CHECK( ChordTextFor( Action::Shell )     == "RShift" );
	CHECK( ChordTextFor( Action::ShellAlt )  == "Ctrl+Shift+O" );
	CHECK( ChordTextFor( Action::Launcher )  == "LCtrl+RShift" );

	CHECK( IsDefault( Action::Shell ) );
	CHECK( IsDefault( Action::ShellAlt ) );
	CHECK( IsDefault( Action::Launcher ) );

	// ...and every default is exactly what the action table declares, so the
	// row's Default() (its reset target) cannot drift from what a fresh
	// process actually binds.
	for ( size_t i = 0; i < (size_t)Action::Count; i++ )
	{
		const Action e = (Action)i;
		INFO( Info( e ).pszId );
		CHECK( ChordTextFor( e ) == FormatChord( Parse( Info( e ).pszDefault ) ) );
	}

	// Ids round-trip, since they are both the on-disk key and half the row id.
	CHECK( ActionFromId( "shell" ).value() == Action::Shell );
	CHECK( ActionFromId( "launcher" ).value() == Action::Launcher );
	CHECK_FALSE( ActionFromId( "nope" ).has_value() );
}

TEST_CASE( "the reserved chord is a chord no action can hold", "[keybinds]" )
{
	CHECK( FormatChord( ReservedChord() ) == std::string( ReservedChordText() ) );
	for ( size_t i = 0; i < (size_t)Action::Count; i++ )
		CHECK( ChordFor( (Action)i ) != ReservedChord() );
}

// ---------------------------------------------------------------------------
//  Conflicts
// ---------------------------------------------------------------------------
// SetChord() persists, which needs a config home this binary does not have --
// so the CHECK is on the refusal, which happens before any write. A refused
// set must leave the store untouched, which is the property that matters.
TEST_CASE( "two actions cannot share a chord", "[keybinds]" )
{
	const std::string sBefore = ChordTextFor( Action::Launcher );

	// Exactly the shell's alternate chord.
	const std::string sWhy = SetChord( Action::Launcher, "Ctrl+Shift+O" );
	CHECK_FALSE( sWhy.empty() );
	CHECK( sWhy.find( "Open settings" ) != std::string::npos );
	CHECK( ChordTextFor( Action::Launcher ) == sBefore );

	// The same chord written a different way is still the same chord.
	CHECK_FALSE( SetChord( Action::Launcher, "  shift + ctrl + o " ).empty() );
	CHECK( ChordTextFor( Action::Launcher ) == sBefore );

	// The reserved chord is refused for every action, with its own reason.
	for ( size_t i = 0; i < (size_t)Action::Count; i++ )
	{
		const std::string sReason = SetChord( (Action)i, ReservedChordText() );
		INFO( Info( (Action)i ).pszId );
		CHECK_FALSE( sReason.empty() );
		CHECK( sReason.find( "reserved" ) != std::string::npos );
	}

	// Nonsense is refused by the grammar, not by the conflict rule.
	CHECK_FALSE( SetChord( Action::Launcher, "Ctrl+Wumpus" ).empty() );
	CHECK( ChordTextFor( Action::Launcher ) == sBefore );

	// NOTE: no successful SetChord() anywhere in this file, deliberately. A
	// success persists, and a test binary has no isolated config home -- it
	// would write the developer's own ~/.config/gamescope-ritz. Every call
	// here is one that must be REFUSED, and the refusal is checked to have
	// left the store untouched, which is the property that matters.
}

// ---------------------------------------------------------------------------
//  The gesture engine
// ---------------------------------------------------------------------------
TEST_CASE( "a modifier tap fires on release and is never swallowed", "[keybinds]" )
{
	ClearGestureState();
	Keyboard kb;

	KeyResult r = kb.Press( XKB_KEY_Shift_R );
	CHECK_FALSE( r.bFired );
	CHECK_FALSE( r.bConsume );   // a modifier's press must always reach the game

	r = kb.Release( XKB_KEY_Shift_R );
	CHECK( r.bFired );
	CHECK( r.eAction == Action::Shell );
	CHECK_FALSE( r.bConsume );   // ...and so must its release, or the game sticks
}

TEST_CASE( "a tap does not fire when the key was used as a modifier", "[keybinds]" )
{
	ClearGestureState();
	Keyboard kb;

	kb.Press( XKB_KEY_Shift_R );
	kb.Press( XKB_KEY_C );        // Right Shift + C: a modifier doing its day job
	kb.Release( XKB_KEY_C );
	const KeyResult r = kb.Release( XKB_KEY_Shift_R );
	CHECK_FALSE( r.bFired );
}

TEST_CASE( "the launcher combo fires in either key order and only once", "[keybinds]" )
{
	for ( int nOrder = 0; nOrder < 2; nOrder++ )
	{
		ClearGestureState();
		Keyboard kb;

		if ( nOrder == 0 )
		{
			kb.Press( XKB_KEY_Control_L );
			kb.Press( XKB_KEY_Shift_R );
		}
		else
		{
			kb.Press( XKB_KEY_Shift_R );
			kb.Press( XKB_KEY_Control_L );
		}

		KeyResult r = kb.Release( XKB_KEY_Shift_R );
		INFO( nOrder );
		CHECK( r.bFired );
		CHECK( r.eAction == Action::Launcher );   // never Shell: the peak was two keys

		// Left Ctrl is still down; letting go of it must not fire anything.
		r = kb.Release( XKB_KEY_Control_L );
		CHECK_FALSE( r.bFired );
	}
}

TEST_CASE( "Ctrl+Shift+O fires on the press and is swallowed", "[keybinds]" )
{
	ClearGestureState();
	Keyboard kb;

	CHECK_FALSE( kb.Press( XKB_KEY_Control_L ).bConsume );
	CHECK_FALSE( kb.Press( XKB_KEY_Shift_R ).bConsume );

	KeyResult r = kb.Press( XKB_KEY_O );
	CHECK( r.bFired );
	CHECK( r.eAction == Action::ShellAlt );
	CHECK( r.bConsume );          // the letter must not reach the game

	// The completing key's release is ours too -- otherwise it arrives as a
	// bare, unmatched 'O' release.
	r = kb.Release( XKB_KEY_O );
	CHECK( r.bConsume );
	CHECK_FALSE( r.bFired );

	// And the modifiers coming up afterwards fire no tap: the peak was three.
	CHECK_FALSE( kb.Release( XKB_KEY_Shift_R ).bFired );
	CHECK_FALSE( kb.Release( XKB_KEY_Control_L ).bFired );
}

TEST_CASE( "the longer chord is not eaten by the shorter one it starts with", "[keybinds]" )
{
	// This is the historical break the peak model exists to make impossible:
	// Ctrl+Shift+O begins with exactly the launcher's LCtrl+RShift, and the
	// launcher used to fire on the press of whichever went down second --
	// opening the launcher before the O was ever typed.
	ClearGestureState();
	Keyboard kb;

	CHECK_FALSE( kb.Press( XKB_KEY_Control_L ).bFired );
	CHECK_FALSE( kb.Press( XKB_KEY_Shift_R ).bFired );   // NOT the launcher yet

	const KeyResult r = kb.Press( XKB_KEY_O );
	CHECK( r.bFired );
	CHECK( r.eAction == Action::ShellAlt );
}

TEST_CASE( "the reserved chord always opens the settings", "[keybinds]" )
{
	ClearGestureState();
	Keyboard kb;

	kb.Press( XKB_KEY_Control_L );
	kb.Press( XKB_KEY_Alt_L );
	kb.Press( XKB_KEY_Shift_L );
	const KeyResult r = kb.Press( XKB_KEY_O );

	CHECK( r.bFired );
	CHECK( r.eAction == Action::Shell );
	CHECK( r.bConsume );
}

TEST_CASE( "a focus boundary drops a half-finished gesture", "[keybinds]" )
{
	ClearGestureState();
	Keyboard kb;

	kb.Press( XKB_KEY_Shift_R );
	ClearGestureState();          // wlserver_clear_pressed_hotkeys()
	kb.held.clear();

	// The release the compositor never saw the press for must not complete
	// the tap -- the peak went with the ledger.
	const KeyResult r = ProcessKey( XKB_KEY_Shift_R, false, {} );
	CHECK_FALSE( r.bFired );
}

TEST_CASE( "an armed capture swallows every key and matches nothing", "[keybinds]" )
{
	ClearGestureState();
	BeginCapture( Action::Launcher );
	CHECK( CaptureActive() );

	Keyboard kb;
	// The chord being captured is the SHELL's own chord; it must not fire.
	KeyResult r = kb.Press( XKB_KEY_Shift_R );
	CHECK( r.bConsume );
	CHECK_FALSE( r.bFired );

	r = kb.Release( XKB_KEY_Shift_R );
	CHECK( r.bConsume );
	CHECK_FALSE( r.bFired );
	CHECK_FALSE( CaptureActive() );

	const CaptureResult res = TakeCaptureResult();
	CHECK( res.eStatus == CaptureStatus::Captured );
	CHECK( res.eAction == Action::Launcher );
	// A modifier-ONLY capture keeps its side: which Shift you tapped is the
	// whole content of the gesture.
	CHECK( res.sChord == "RShift" );
	// ...and taking it clears it.
	CHECK( TakeCaptureResult().eStatus == CaptureStatus::Idle );
}

TEST_CASE( "a captured modifier inside a real chord generalises to either side", "[keybinds]" )
{
	ClearGestureState();
	BeginCapture( Action::ShellAlt );

	Keyboard kb;
	kb.Press( XKB_KEY_Control_L );
	kb.Press( XKB_KEY_Shift_L );
	kb.Press( XKB_KEY_P );
	kb.Release( XKB_KEY_P );

	const CaptureResult res = TakeCaptureResult();
	CHECK( res.eStatus == CaptureStatus::Captured );
	CHECK( res.sChord == "Ctrl+Shift+P" );   // not LCtrl+LShift+P
}

TEST_CASE( "Escape cancels a capture", "[keybinds]" )
{
	ClearGestureState();
	BeginCapture( Action::Shell );

	Keyboard kb;
	kb.Press( XKB_KEY_Escape );
	kb.Release( XKB_KEY_Escape );

	const CaptureResult res = TakeCaptureResult();
	CHECK( res.eStatus == CaptureStatus::Cancelled );
	CHECK( res.sChord.empty() );
	CHECK_FALSE( CaptureActive() );

	// The binding it was rebinding is untouched.
	CHECK( ChordTextFor( Action::Shell ) == "RShift" );
}

TEST_CASE( "a capture cancelled from the UI leaves the binding alone", "[keybinds]" )
{
	ClearGestureState();
	BeginCapture( Action::Shell );
	CancelCapture();
	CHECK_FALSE( CaptureActive() );
	CHECK( TakeCaptureResult().eStatus == CaptureStatus::Cancelled );
	CHECK( ChordTextFor( Action::Shell ) == "RShift" );
}
