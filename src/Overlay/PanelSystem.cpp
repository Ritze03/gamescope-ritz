// The "System" tab -- see PanelSystem.h for what this is and why it exists.
//
// This file owns exactly one row group: the clipboard sync switch and its
// status readout. The runtime flag it flips,
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

#include "backend.h"
#include "Clipboard/ClipboardSync.h"
#include "Config/ConfigManager.h"

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
			s_Settings = config::ResolveEffective( config::SessionAppId() );
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			g_bClipboardSyncEnabled.store( s_Settings.system.clipboard_sync, std::memory_order_relaxed );
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
	}
}
