// The Keybinds area -- see PanelKeybinds.h, and superdoc/features/keybinds.md
// for the design.
//
// One row per action, each a Kind::Text row holding that action's chord string
// and marked .Chord( <action id> ) so the shell draws it as a capture chip
// rather than an input field (Registry.h's Chord()). The binding is an ordinary string
// getter/setter pair, which is what keeps `overlay_e2_set setup.keybinds
// keybinds.shell "Ctrl+Shift+P"` and the palette working on these rows exactly
// as on every other -- the capture chip is a nicer way to produce the same
// string, never a second code path to the value.
//
// Thread safety: drawn from SettingsOverlay_AddLayer() on the steamcompmgr
// thread, same as every other panel. keybinds::SetChord() writes global.json
// from here (a click), which is the same thread PanelConfig.cpp's own writes
// come from.
#include "PanelKeybinds.h"

#include <cstdint>
#include <string>

#include "Config/ConfigManager.h"
#include "Keybinds.h"
#include "Notifications.h"

namespace gamescope
{
	namespace
	{
		using keybinds::Action;

		// ---- the capture pump -------------------------------------------
		// A capture is armed on the steamcompmgr thread (the chip's press),
		// resolved on the wlserver thread (the keys), and must be COMMITTED
		// back on this thread, because committing means writing global.json
		// and config:: is documented as single-threaded.
		//
		// So it is pumped from the row getters, which run every frame their
		// area is on screen -- and that area is necessarily on screen,
		// because arming the capture requires clicking a chord row, and
		// every chord row (Keybinds' own, and the Zoom/Autoclicker copies
		// PanelKeybinds_ChordRow() declares) is bound through ChordBind().
		// A dedicated per-frame hook would be a second mechanism for a
		// situation that cannot arise without this one already running.
		void PumpCapture()
		{
			const keybinds::CaptureResult r = keybinds::TakeCaptureResult();
			if ( r.eStatus == keybinds::CaptureStatus::Idle )
				return;

			if ( r.eStatus == keybinds::CaptureStatus::Cancelled )
			{
				Notifications::Show( "Rebind cancelled.", Notifications::Kind::Info, 2.0f );
				return;
			}

			// The conflict rule and the reserved-chord rule both live in
			// SetChord(), so the refusal a click gets and the refusal
			// `ritz_keybind` gets are the same sentence from the same check.
			if ( const std::string sWhy = keybinds::SetChord( r.eAction, r.sChord ); !sWhy.empty() )
			{
				Notifications::Show( r.sChord + " was not taken. " + sWhy,
					Notifications::Kind::Warning, 5.0f );
				return;
			}

			Notifications::Show( std::string( keybinds::Info( r.eAction ).pszTitle ) +
				" is now " + r.sChord, Notifications::Kind::Ok, 3.0f );
		}

		ui::AnyBind ChordBind( Action eAction )
		{
			return ui::AnyBind::Of<std::string>(
				[ eAction ]
				{
					PumpCapture();
					return keybinds::ChordTextFor( eAction );
				},
				[ eAction ]( std::string s )
				{
					// The typed/scripted path. A refusal is REPORTED, never
					// silently swallowed: the row reads back unchanged either
					// way, and a setter that quietly did nothing is the one
					// failure overlay_e2_set exists to expose.
					if ( const std::string sWhy = keybinds::SetChord( eAction, s ); !sWhy.empty() )
						Notifications::Show( sWhy, Notifications::Kind::Warning, 5.0f );
				} );
		}
	}

	ui::Entry &PanelKeybinds_ChordRow( ui::Area &a, const char *pszId, const char *pszLabel,
	                                   Action eAction )
	{
		const keybinds::ActionInfo &info = keybinds::Info( eAction );
		return a.Text( pszId, pszLabel, ChordBind( eAction ) )
			.Chord( info.pszId )
			.Help( info.pszHelp )
			.Default( ui::Value{ std::string( info.pszDefault ) } )
			.Keywords( "keybind hotkey shortcut chord rebind" );
	}

	void PanelKeybinds_SeedFromConfig( const config::Settings &settings )
	{
		keybinds::ApplyFromConfig( settings.overlay );
	}

	void PanelKeybinds_RegisterArea( ui::Registry &reg )
	{
		// Registration is the earliest moment this file runs, and the shell
		// can be opened without main.cpp's startup apply ever having run in a
		// test harness -- seed here too, for the same reason PanelSystem does.
		keybinds::ApplyFromConfig( config::LoadGlobal().overlay );

		ui::Area &a = reg.Add( "setup.keybinds", "Keybinds", ui::Section::Setup );
		a.Keywords( "keybind keybinds hotkey hotkeys shortcut shortcuts key chord bind rebind "
		            "shell launcher overlay open" );
		// "global only", exactly as Appearance and Cursor say it: these rows
		// write global.json whatever profile the session is editing, and an
		// area badge is the one place a settings UI can answer "where does
		// what I change here get written" without the user guessing.
		a.Badge( []{ return std::string( "global only" ); } );
		a.Summary( []
			{
				return keybinds::ChordTextFor( Action::Shell ) + " opens the settings";
			} );

		// TWO GROUPS (2026-09-22, the user: "one for global hotkeys and one
		// for game-specific hotkeys"). Global: the chords that open this
		// fork's own surfaces -- settings, launcher, friends. In-game: the
		// chords that do something TO the game while you play (zoom,
		// autoclicker), which also have a copy of their row in their own
		// area. Both groups are the same global.json section; the split is
		// what a hotkey is for, not where it is stored.
		// ponytail: split on ActionInfo::bHeld, which is exactly the in-game
		// set today (a held action is by definition pressed mid-game). A
		// future in-game action that is NOT held would land under Global --
		// give ActionInfo its own flag then.
		for ( const bool bInGame : { false, true } )
		{
			a.Group( bInGame ? "In-game hotkeys" : "Global hotkeys" );

			for ( size_t i = 0; i < (size_t)Action::Count; i++ )
			{
				const Action eAction = (Action)i;
				const keybinds::ActionInfo &info = keybinds::Info( eAction );
				if ( info.bHeld != bInGame )
					continue;

				const std::string sId = std::string( "keybinds." ) + info.pszId;
				PanelKeybinds_ChordRow( a, sId.c_str(), info.pszTitle, eAction );
			}
		}

		a.Group( "If you get stuck" );

		// A READ-ONLY row, deliberately: the whole value of a reserved chord
		// is that it is not a setting. A row that could be edited would be a
		// row that could be edited into uselessness, which is the failure it
		// exists to prevent.
		a.Facts( "keybinds.reserved", "Always works",
			[]{ return std::string( keybinds::ReservedChordText() ); } )
			.Help( "This chord opens the settings no matter what the rows above say, and cannot "
			       "be rebound or taken by another action. It is the way back if you bind the "
			       "settings to something you cannot press. From a terminal, "
			       "`gamescopectl ritz_keybinds_reset` does the same thing." )
			.Keywords( "reserved fallback recover stuck unreachable" );

		a.Action( "keybinds.reset", "Reset every keybind", "Reset",
			[]{ keybinds::ResetAll(); } )
			.Confirm( "Reset all?" )
			.Help( "Puts every hotkey back on the chord this build ships with, and removes "
			       "them from your saved settings." )
			.Keywords( "reset default defaults restore" );
	}
}
