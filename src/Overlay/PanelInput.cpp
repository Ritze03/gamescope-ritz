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
//   - Force grab keyboard is live on BOTH nested backends as of 2026-09-27:
//     SDL (SDLBackend_SetKeyboardGrabbed(), SDL_SetWindowKeyboardGrab()) and
//     Wayland (WaylandBackend_SetKeyboardGrabbed(), WaylandBackend.cpp's
//     CWaylandBackend::SetKeyboardGrabbed() via a real
//     zwp_keyboard_shortcuts_inhibit_v1 -- protocol/meson.build wires the
//     XML, see that file's own comment). On DRM/OpenVR/Headless the switch
//     still writes the config field and g_bGrabbed (so it survives a
//     restart), but there is no host desktop to grab from at all, so
//     nothing live happens there -- and on Wayland specifically, a host
//     compositor that never advertises the inhibit-manager global degrades
//     to the same "writes the field, nothing live" behaviour (the switch
//     never errors or refuses). See superdoc/features/input-general.md's
//     "Wayland backend" section for the full per-backend table and the
//     inhibitor lifecycle (active/inactive events -> input.backend_grab_
//     support's Diagnostics fact below).
//
// SAFETY: neither switch can lock the user out of gamescope's own hotkeys.
// A host-level keyboard grab (SDL_WINDOW_KEYBOARD_GRABBED, or Wayland's
// zwp_keyboard_shortcuts_inhibit_v1) stops the HOST compositor's own global
// shortcuts from firing while gamescope has focus -- per the Wayland
// protocol's own doc comment, it changes nothing about which wl_keyboard
// events THIS process receives, so RShift (Open settings) and the reserved
// Ctrl+Alt+Shift+O still reach wlserver's keybind matching exactly as
// before, on both backends. Two independent escapes exist if a grab ever
// feels stuck: SDL's is Super+G (LGUI+G, this file's nearest neighbour in
// SDLBackend.cpp's SDL_KEYUP handler) -- NOT Ctrl+G; Wayland's is the HOST
// compositor's own mechanism, since only it can lift its own inhibitor --
// on Hyprland specifically, a bind flagged bypass ('p') still fires under an
// inhibitor (see input-general.md's Hyprland section for the exact wording
// and a link to the wiki). This row's help text says so.
//
// Modelled directly on PanelSystem.cpp's shape (own cached Settings, own
// EnsureConfigLoaded()/generation check, config::ResolvedSettings() since
// gamescope.* is per-profile/per-game, not overlay.*'s global-only) rather
// than on PanelDisplay.cpp's larger Cfg()/PushCachedSettingsToLiveState()
// machinery -- that machinery exists to serve SEVERAL rows' worth of
// display.* fields at once; two rows in their own file don't need it.
// force_grab_keyboard is now re-applied centrally on every profile switch
// too, the same as force_grab_cursor: see main.cpp's
// ritz_apply_config_live() (both SDLBackend_SetKeyboardGrabbed() and
// WaylandBackend_SetKeyboardGrabbed() are called from there, independent of
// which Shell area is open).
#include "PanelInput.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "backend.h"
#include "Config/ConfigManager.h"
#include "LaunchOptions.h"

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

	// WaylandBackend.cpp's own pair, same ad hoc convention, added
	// 2026-09-27 alongside the real zwp_keyboard_shortcuts_inhibit_v1
	// implementation (see superdoc/features/input-general.md). Both are
	// no-ops when the Wayland backend isn't the one running.
	extern void WaylandBackend_SetKeyboardGrabbed( bool bGrabbed );
	extern const char *WaylandBackend_GetKeyboardGrabStatus();

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

			// Re-push force_grab_cursor on every reload (a profile Use, a
			// per-game toggle), the same shape PanelSystem.cpp's own
			// EnsureConfigLoaded() uses for g_bClipboardSyncEnabled.
			//
			// force_grab_keyboard does NOT get the same treatment here any
			// more (removed 2026-09-27, live evidence:
			// build-release/verify-shots/kbgrab-startup-*/evidence.txt) --
			// it used to, as this exact "no central hook" workaround, but
			// that push ran unconditionally on THIS FUNCTION's very FIRST
			// call too (s_bConfigLoaded false, not just on a real reload),
			// which fires the moment anything first touches the registry
			// (the Shell's first draw, or a single gamescopectl
			// overlay_e2_get/overlay_e2_dump_keys against ANY row) --
			// stomping g_bGrabbed back to whatever the DISK config says
			// regardless of an explicit `-g`/`--grab` CLI override or a
			// live in-window toggle (SDL's Super+G) that had already run.
			// Measured live: `-g` on the Wayland backend created and
			// activated a real zwp_keyboard_shortcuts_inhibit_v1 at
			// startup, then the FIRST overlay_e2_get of this session (for
			// ANY row) destroyed it again a moment later. Now that
			// main.cpp's ritz_apply_config_live() re-applies
			// force_grab_keyboard centrally on every REAL profile switch
			// (see that function), this push was pure duplication of a
			// job something else now does correctly -- removing it fixes
			// the stomp rather than trying to special-case "first load".
			steamcompmgr_set_force_relative_mouse( s_Settings.gamescope.force_grab_cursor );
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
		bool CurrentBackendIsWayland()
		{
			IBackend *pBackend = GetBackend();
			IBackendConnector *pConnector = pBackend ? pBackend->GetCurrentConnector() : nullptr;
			return pConnector && std::string_view( pConnector->GetName() ) == "Wayland"sv;
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
			.Keywords( "mouse pointer capture confine grab relative" )
			// superdoc/features/launch-option-lock.md. --force-grab-cursor
			// seeds g_bForceRelativeMouse before this switch's binding can
			// read it (main.cpp's apply_ritz_config_to_startup_state()); a
			// profile switch that flips this back off would be silently
			// fighting the flag for the rest of the session.
			.LockedByLaunchOption( []{ return LaunchOptions::Given( LaunchOptions::Opt::GrabCursor ); },
				( std::string( "Set by the launch option " ) + LaunchOptions::Spelling( LaunchOptions::Opt::GrabCursor ) +
				  " -- remove it from the launch options to change this here." ).c_str() );

		a.Switch( "input.force_grab_keyboard", "Force grab keyboard",
			ui::AnyBind::Of<bool>(
				[]{ return g_bGrabbed; },
				[]( bool b ) {
					EnsureConfigLoaded();
					s_Settings.gamescope.force_grab_keyboard = b;
					g_bGrabbed = b;
					// Live on SDL and, since 2026-09-27, on Wayland -- see
					// this file's header comment for what "live" means per
					// backend. Both are no-ops on every OTHER backend
					// (DRM/OpenVR/Headless), and WaylandBackend_
					// SetKeyboardGrabbed() is itself a no-op if the host
					// compositor never advertised
					// zwp_keyboard_shortcuts_inhibit_manager_v1.
					SDLBackend_SetKeyboardGrabbed( b );
					WaylandBackend_SetKeyboardGrabbed( b );
					QueueSave();
				} ) )
			.Key( "gamescope.force_grab_keyboard" )
			.Help( "Stops your desktop's own shortcuts (Alt+Tab, a Meta-key launcher, ...) from "
			       "stealing keys while gamescope is focused. Gamescope's own hotkeys (RShift to "
			       "open Settings) always still work -- this never locks you out. Live on the "
			       "SDL and Wayland nested backends; on DRM/OpenVR/Headless it applies the next "
			       "time gamescope starts (there's no host desktop to grab from there). On SDL "
			       "you can also flip it in-window with Super+G (not Ctrl+G). On Wayland, if the "
			       "host compositor is Hyprland, its own escape is a bind flagged bypass ('p') -- "
			       "see the Diagnostics fact below and input-general.md for details." )
			.Default( false )
			.Keywords( "keyboard grab capture host shortcuts alt-tab focus sdl wayland" )
			// -g/--grab sets g_bGrabbed directly in the getopt loop, which
			// this switch's binding reads straight off -- same reasoning as
			// Force grab cursor just above.
			.LockedByLaunchOption( []{ return LaunchOptions::Given( LaunchOptions::Opt::GrabKeyboard ); },
				( std::string( "Set by the launch option " ) + LaunchOptions::Spelling( LaunchOptions::Opt::GrabKeyboard ) +
				  " -- remove it from the launch options to change this here." ).c_str() );

		a.Group( "Diagnostics" );

		a.Facts( "input.backend_grab_support", "Keyboard grab support",
			[]{
				if ( CurrentBackendIsSDL() )
					return std::string( "live on this backend (SDL)" );
				if ( CurrentBackendIsWayland() )
					return std::string( "on this backend (Wayland): " ) + WaylandBackend_GetKeyboardGrabStatus();
				// DRM / OpenVR / Headless: no host desktop to grab from --
				// g_bGrabbed/the config field still apply at the next
				// launch (mirrors -g/--grab there already), but there is
				// nothing to toggle live.
				return std::string( "not applicable on this backend (embedded, no host to grab from)" );
			} )
			.Help( "Whether Force grab keyboard above is actually inhibiting your desktop's "
			       "shortcuts right now, on the nested backend actually running this session. "
			       "Never says 'applies at next launch' on a backend where it's really live." )
			.Keywords( "keyboard grab backend sdl wayland support live startup inhibit shortcuts" );
	}
}
