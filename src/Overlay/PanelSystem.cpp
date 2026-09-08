// The "System" tab -- see PanelSystem.h for what this is and why it exists.
//
// This file owns two areas. `system.general` is the clipboard sync switch and
// its status readout; `system.companion` (2026-09-08) is the Steam chat
// overlay's three settings -- see the second registration at the bottom of
// this file for what those are and why they live here.
//
// The runtime flag the clipboard switch flips,
// gamescope::g_bClipboardSyncEnabled, lives in Clipboard/ClipboardSync.h --
// not here -- because both nested backends (WaylandBackend.cpp,
// SDLBackend.cpp) need to read it and neither should have to include an
// Overlay file to do so. See that header's own comment on the flag.
//
// Phase B (requests-2026-09-05 item 5): the switch is PERSISTED as
// config::SystemSettings::clipboard_sync, a normal per-layer field (shared
// via global.json unless the game has separate settings on), routed through
// config::EnqueueRoutedWrite() like NotificationSettings::muted. The atomic
// is seeded from that field by EnsureConfigLoaded() below -- the same
// generation-checked loader Notifications.cpp uses -- so the value survives
// a restart. The one thing still outside this file is the *outbound* gate
// (a copy made inside gamescope reaching the host): steamcompmgr.cpp's
// gamescope_broadcast_clipboard(), hints->SetSelection(), which reads the
// same atomic.
#include "PanelSystem.h"

#include <cstdint>

#include <string>
#include <utility>

#include "backend.h"
#include "Clipboard/ClipboardSync.h"
#include "Config/ConfigManager.h"
#include "SteamCompanion.h"

namespace gamescope
{
	namespace
	{
		// The protocol actually in use for host clipboard sync, or why
		// there isn't one. Same discriminator steamcompmgr.cpp's
		// gamescope_broadcast_clipboard() uses to decide whether a host
		// exists at all: GetCurrentConnector()->GetNestedHints() is
		// nullptr in embedded (DRM) mode, since there is no host
		// compositor to sync with -- CBaseBackendConnector's default
		// GetNestedHints() answers nullptr, and only the nested backends'
		// connectors (Wayland, SDL) override it.
		std::string ClipboardSyncStatus()
		{
			INestedHints *pHints = nullptr;
			if ( IBackendConnector *pConnector = GetBackend()->GetCurrentConnector() )
				pHints = pConnector->GetNestedHints();

			return pHints ? pHints->GetClipboardSyncStatus() : "inert: no host (embedded)";
		}

		// This panel's own cached Settings -- the same "cache locally, push
		// on every edit, reload when the config generation moves" shape
		// PanelDisplay/PanelShaders/Notifications use, and for the same
		// reason: ResolveEffective() is a disk read, and this getter runs
		// every frame the area is shown. A profile Use or a per-game toggle
		// (the only things that bump ConfigGeneration) reloads it, and the
		// reload re-seeds the atomic, so a profile that carries
		// system.clipboard_sync takes effect the next time this runs.
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
			g_bClipboardSyncEnabled.store( s_Settings.system.clipboard_sync, std::memory_order_relaxed );
		}

		// =====================================================================
		//  `system.companion` -- the Steam chat overlay (2026-09-08)
		// =====================================================================
		// THREE SETTINGS, ALL GLOBAL, and that is the reason they need their
		// own cache rather than sharing s_Settings above: `overlay.*` is
		// global.json-only (ConfigSchema.h's OverlaySettings comment), so it
		// has to be read with LoadGlobal() and written with
		// EnqueueGlobalWrite() -- exactly the split PanelConfig.cpp's
		// Appearance area makes, for the same reason and with the same
		// per-field merge protecting a stale copy from clobbering a sibling
		// area's write.
		//
		// WHY THEY ARE HERE AND NOT IN APPEARANCE OR A NEW SECTION. The rail
		// has three sections (Display, System, Setup). Appearance is how the
		// overlay LOOKS; Setup is how this fork is configured to run. A
		// browser gamescope launches on its own display, on a hotkey, is a
		// machine-level capability of the running system -- the same shelf the
		// clipboard bridge sits on, which is also "gamescope talking to
		// something outside itself". So: System, next to it.
		bool     s_bGlobalLoaded = false;
		uint64_t s_ulGlobalGeneration = 0;
		config::Settings s_Global;

		void EnsureGlobalLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bGlobalLoaded && ulGeneration == s_ulGlobalGeneration )
				return;
			s_Global = config::LoadGlobal();
			s_ulGlobalGeneration = ulGeneration;
			s_bGlobalLoaded = true;
		}

		void SaveGlobal()
		{
			config::EnqueueGlobalWrite( s_Global );
		}

		void RegisterCompanionArea( ui::Registry &reg )
		{
			ui::Area &a = reg.Add( "system.companion", "Steam chat", ui::Section::System );
			a.Keywords( "steam chat friends message browser companion overlay web chromium "
			            "firefox url page talk friend list" );
			// Same badge Appearance, Cursor and Keybinds carry, and for the
			// same reason: these rows write global.json whatever profile the
			// session is editing.
			a.Badge( []{ return std::string( "global only" ); } );
			a.Summary( []{ return companion::StatusText(); } );

			a.Group( "Steam chat" );

			a.Switch( "overlay.companion_enabled", "Steam chat overlay",
				ui::AnyBind::Of<bool>(
					[]
					{
						EnsureGlobalLoaded();
						return s_Global.overlay.companion_enabled;
					},
					[]( bool b )
					{
						EnsureGlobalLoaded();
						s_Global.overlay.companion_enabled = b;
						SaveGlobal();
					} ) )
				.Help( "Opens Steam's web chat over the game on the Steam chat hotkey, in a "
				       "browser gamescope runs on its own display. It is a separate sign-in from "
				       "the Steam client, and it has no voice, no invites and no new-message "
				       "alerts - see the Steam chat page in the docs. Off, the hotkey says so "
				       "instead of opening anything; the key is still taken from the game, so "
				       "rebind it under Setup if you want it back." )
				.Key( "overlay.companion_enabled" )
				.Default( config::OverlaySettings{}.companion_enabled )
				.Keywords( "steam chat friends enable browser overlay" );

			a.Facts( "system.companion_status", "Status",
				[]{ return companion::StatusText(); } )
				.Help( "The hotkey that opens it, the browser it would run, and whether that "
				       "browser is actually installed. Read-only." )
				.Keywords( "status hotkey browser installed running" );

			a.Group( "Browser" );

			a.Text( "overlay.companion_command", "Browser command",
				ui::AnyBind::Of<std::string>(
					[]
					{
						EnsureGlobalLoaded();
						return s_Global.overlay.companion_command;
					},
					[]( std::string s )
					{
						EnsureGlobalLoaded();
						s_Global.overlay.companion_command = std::move( s );
						SaveGlobal();
					} ) )
				.Help( "The command that opens the chat window. {url} is replaced by the page "
				       "below and {profile} by a private browser profile folder - keep {profile}, "
				       "or a second copy of your browser will just open a tab on your desktop "
				       "instead. A command with no {url} gets the page added at the end." )
				.Key( "overlay.companion_command" )
				.Default( std::string( config::OverlaySettings{}.companion_command ) )
				.Keywords( "browser command chromium firefox executable arguments flags" );

			a.Text( "overlay.companion_url", "Page",
				ui::AnyBind::Of<std::string>(
					[]
					{
						EnsureGlobalLoaded();
						return s_Global.overlay.companion_url;
					},
					[]( std::string s )
					{
						EnsureGlobalLoaded();
						s_Global.overlay.companion_url = std::move( s );
						SaveGlobal();
					} ) )
				.Help( "The page it opens. Steam's own web chat by default; any page works, so "
				       "this can be a wiki or a guide instead." )
				.Key( "overlay.companion_url" )
				.Default( std::string( config::OverlaySettings{}.companion_url ) )
				.Keywords( "url page address link steamcommunity chat" );
		}
	}

	void PanelSystem_SeedFromConfig()
	{
		EnsureConfigLoaded();
	}

	void PanelSystem_RegisterArea( ui::Registry &reg )
	{
		// Registration is the earliest moment this file runs in a process
		// (Shell.cpp's RegisterAll(), the first time the registry is
		// built), so seed the runtime flag here too -- see
		// PanelSystem_SeedFromConfig()'s comment in the header for the
		// startup call that covers the time before the shell is first
		// opened.
		EnsureConfigLoaded();

		ui::Area &a = reg.Add( "system.general", "System", ui::Section::System );
		a.Keywords( "system clipboard copy paste sync host" );
		a.Summary( []
			{
				if ( !g_bClipboardSyncEnabled.load( std::memory_order_relaxed ) )
					return std::string( "clipboard sync off" );
				return std::string( "clipboard sync on · " ) + ClipboardSyncStatus();
			} );

		a.Group( "Clipboard" );

		a.Switch( "system.clipboard_sync", "Clipboard sync",
			ui::AnyBind::Of<bool>(
				[]
				{
					EnsureConfigLoaded();
					return s_Settings.system.clipboard_sync;
				},
				[]( bool b )
				{
					EnsureConfigLoaded();
					s_Settings.system.clipboard_sync = b;
					// Runtime flag first (the backends read it on their next
					// event), then the routed write so it survives a restart.
					g_bClipboardSyncEnabled.store( b, std::memory_order_relaxed );
					config::EnqueueRoutedWrite( s_Settings );
				} ) )
			.Help( "Shares the clipboard between games running here and the rest of your desktop, "
			       "both ways. Off stops text crossing in either direction; copying and pasting "
			       "between games still works." )
			.Default( config::SystemSettings{}.clipboard_sync )
			.Keywords( "clipboard copy paste sync host" );

		a.Group( "Diagnostics" );

		a.Facts( "system.clipboard_status", "Clipboard status",
			[]{ return ClipboardSyncStatus(); } )
			.Help( "Which protocol clipboard sync with the host is actually using, or why it "
			       "isn't syncing at all. Read-only, nothing here can be changed." )
			.Keywords( "clipboard protocol status ext_data_control zwlr wl_data_device sdl" );

		RegisterCompanionArea( reg );
	}
}
