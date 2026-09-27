// The "General" area under INPUT -- see PanelInput.h for what this is and
// why it exists.
//
// This file owns `input.general`: two switches about gamescope taking
// exclusive hold of an input device away from the host while nested --
// "Force grab cursor" (moved here 2026-09-27 from display.general,
// PanelDisplay.cpp -- same config field, gamescope.force_grab_cursor, same
// live path, steamcompmgr_set_force_relative_mouse(); only the row's id and
// home changed, display.force_grab_cursor -> input.force_grab_cursor) and
// "Force grab keyboard" (new, gamescope.force_grab_keyboard, mirrors
// -g/--grab's runtime effect on main.hpp's g_bGrabbed).
//
// THE TWO ROWS ARE NOT SYMMETRIC IN WHAT "LIVE" MEANS, and the help text on
// each says so plainly rather than implying they behave alike because they
// sit in the same area:
//   - Force grab cursor is live on every backend that has a host to grab
//     from (steamcompmgr_set_force_relative_mouse() -> INestedHints, which
//     both nested backends implement).
//   - Force grab keyboard is live ONLY on the SDL backend
//     (SDLBackend_SetKeyboardGrabbed(), SDLBackend.cpp, via
//     SDL_SetWindowKeyboardGrab()). On every other backend the switch still
//     writes the config field and g_bGrabbed (so it survives a restart and
//     the "(grabbed)" window-title suffix on Wayland follows it), but there
//     is no host-shortcut inhibition to turn on or off at runtime -- see
//     WaylandBackend.cpp's own comment on CWaylandConnector::SetTitle() and
//     superdoc/features/input-general.md's "Wayland backend" section for
//     why (no zwp_keyboard_shortcuts_inhibit_manager_v1 binding exists in
//     this build; wiring it needs a client-protocol XML added to
//     protocol/meson.build, outside this change's scope).
//
// SAFETY: neither switch can lock the user out of gamescope's own hotkeys.
// A host-level keyboard grab (SDL_WINDOW_KEYBOARD_GRABBED) stops the HOST
// desktop's own global shortcuts from firing while gamescope has focus --
// it does not touch which events gamescope's OWN SDL event loop receives,
// so RShift (Open settings) and the reserved Ctrl+Alt+Shift+O still reach
// wlserver's keybind matching exactly as before. The SDL backend's existing
// release valve if a grab ever feels stuck is Super+G (LGUI+G, this file's
// nearest neighbour in SDLBackend.cpp's SDL_KEYUP handler) -- NOT Ctrl+G;
// this row's help text says so. See superdoc/features/input-general.md.
//
// Modelled directly on PanelSystem.cpp's shape (own cached Settings, own
// EnsureConfigLoaded()/generation check, config::ResolvedSettings() since
// gamescope.* is per-profile/per-game, not overlay.*'s global-only) rather
// than on PanelDisplay.cpp's larger Cfg()/PushCachedSettingsToLiveState()
// machinery -- that machinery exists to serve SEVERAL rows' worth of
// display.* fields at once; two rows in their own file don't need it, and
// main.cpp's ritz_apply_config_live() already re-pushes force_grab_cursor
// centrally on every profile switch regardless of which panel is open (see
// that function's own comment). force_grab_keyboard has no such central
// hook -- main.cpp is outside this change's scope -- so a profile switch
// that changes it only takes effect once this area itself has drawn and
// reloaded (the same staleness class issue #25/#68 were, before
// PushCachedSettingsToLiveState() existed for Display's own fields); noted
// in superdoc/features/input-general.md as a known follow-up.
#include "PanelInput.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "backend.h"
#include "Config/ConfigManager.h"

using namespace std::string_view_literals;

namespace gamescope
{
	// SDLBackend.cpp's own free function -- declared here rather than
	// through a shared header, the same ad hoc extern convention
	// PanelDisplay.cpp already uses for e.g. set_color_sdr_gamut_wideness()
	// (steamcompmgr.cpp) and set_sdr_input_gain(). See that function's own
	// comment in SDLBackend.cpp for why a free wrapper is the seam at all
	// (CSDLBackend is a type local to that translation unit).
	extern void SDLBackend_SetKeyboardGrabbed( bool bGrabbed );

	namespace
	{
		// This file's own cached Settings -- PanelSystem.cpp's shape (see
		// that file's own comment on why ResolvedSettings(), not
		// LoadGlobal(): gamescope.* is per-profile/per-game, unlike
		// overlay.*).
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;

			// Re-push on every reload (a profile Use, a per-game toggle),
			// the same shape PanelSystem.cpp's own EnsureConfigLoaded()
			// uses for g_bClipboardSyncEnabled -- see this file's header
			// comment for force_grab_keyboard's own gap (no central
			// main.cpp hook covers it the way ritz_apply_config_live()
			// covers force_grab_cursor).
			steamcompmgr_set_force_relative_mouse( s_Settings.gamescope.force_grab_cursor );
			g_bGrabbed = s_Settings.gamescope.force_grab_keyboard;
			SDLBackend_SetKeyboardGrabbed( s_Settings.gamescope.force_grab_keyboard );
		}

		void QueueSave()
		{
			config::EnqueueRoutedWrite( s_Settings );
		}

		// "SDLWindow" (SDLBackend.cpp's CSDLConnector::GetName()) vs.
		// "Wayland" (WaylandBackend.cpp's CWaylandConnector::GetName()) --
		// the two nested backends' own connector names, read back here
		// rather than duplicated as a second enum, so this file can never
		// disagree with what actually answers GetCurrentConnector().
		// nullptr/embedded (DRM, OpenVR, Headless) reads as neither.
		bool CurrentBackendIsSDL()
		{
			IBackend *pBackend = GetBackend();
			IBackendConnector *pConnector = pBackend ? pBackend->GetCurrentConnector() : nullptr;
			return pConnector && std::string_view( pConnector->GetName() ) == "SDLWindow"sv;
		}
	}

	void PanelInput_RegisterArea( ui::Registry &reg )
	{
		EnsureConfigLoaded();

		ui::Area &a = reg.Add( "input.general", "General", ui::Section::System );
		a.Keywords( "input general grab cursor keyboard mouse pointer capture confine "
		            "relative host focus" );
		a.Summary( []{
			std::string s = g_bForceRelativeMouse ? "cursor grabbed" : "cursor free";
			s += g_bGrabbed ? " · keyboard grabbed" : " · keyboard free";
			return s;
		} );

		a.Group( "Quick toggles" );

		// MOVED from display.general (PanelDisplay.cpp), 2026-09-27 --
		// same config field and live path, see this file's header comment.
		// Routed through steamcompmgr_set_force_relative_mouse() and NOT by
		// writing g_bForceRelativeMouse directly, which has no live effect
		// on its own (issue #68 -- see steamcompmgr.hpp's own comment on
		// that function).
		a.Switch( "input.force_grab_cursor", "Force grab cursor",
			ui::AnyBind::Of<bool>(
				[]{ return g_bForceRelativeMouse; },
				[]( bool b ) {
					EnsureConfigLoaded();
					s_Settings.gamescope.force_grab_cursor = b;
					steamcompmgr_set_force_relative_mouse( b );
					QueueSave();
				} ) )
			.Key( "gamescope.force_grab_cursor" )
			.Help( "Keeps your mouse locked to the game at all times, not just when the cursor is "
			       "hidden. Turn this on if the mouse ever seems to escape the game window." )
			.Default( false )
			.Keywords( "mouse pointer capture confine grab relative" );

		a.Switch( "input.force_grab_keyboard", "Force grab keyboard",
			ui::AnyBind::Of<bool>(
				[]{ return g_bGrabbed; },
				[]( bool b ) {
					EnsureConfigLoaded();
					s_Settings.gamescope.force_grab_keyboard = b;
					g_bGrabbed = b;
					// No-op on every backend but SDL -- see this file's
					// header comment for what "live" means per backend.
					SDLBackend_SetKeyboardGrabbed( b );
					QueueSave();
				} ) )
			.Key( "gamescope.force_grab_keyboard" )
			.Help( "Stops your desktop's own shortcuts (Alt+Tab, a Meta-key launcher, ...) from "
			       "stealing keys while gamescope is focused. Gamescope's own hotkeys (RShift to "
			       "open Settings) always still work -- this never locks you out. Live on the "
			       "SDL nested backend; on other backends it applies the next time gamescope "
			       "starts. On SDL you can also flip it in-window with Super+G (not Ctrl+G)." )
			.Default( false )
			.Keywords( "keyboard grab capture host shortcuts alt-tab focus sdl wayland" );

		a.Group( "Diagnostics" );

		a.Facts( "input.backend_grab_support", "Keyboard grab support",
			[]{
				return CurrentBackendIsSDL()
					? std::string( "live on this backend (SDL)" )
					: std::string( "applies at next launch on this backend" );
			} )
			.Help( "Whether Force grab keyboard above takes effect immediately or only the next "
			       "time gamescope starts, on the nested backend actually running right now." )
			.Keywords( "keyboard grab backend sdl wayland support live startup" );
	}
}
