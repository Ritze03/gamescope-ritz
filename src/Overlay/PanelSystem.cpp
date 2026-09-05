// The "System" tab -- see PanelSystem.h for what this is and why it exists.
//
// Phase A (this file) owns exactly one row group: the clipboard sync switch
// and its status readout. The runtime flag it flips,
// gamescope::g_bClipboardSyncEnabled, lives in Clipboard/ClipboardSync.h --
// not here -- because both nested backends (WaylandBackend.cpp,
// SDLBackend.cpp) need to read it and neither should have to include an
// Overlay file to do so. See that header's own comment on the flag.
//
// PHASE B (not this file): persisting the switch to config::SystemSettings,
// and the *outbound* gate (a copy made inside gamescope reaching the host --
// steamcompmgr.cpp's gamescope_broadcast_clipboard(), hints->SetSelection()).
// Every place that matters is marked "PHASE B" below and in ClipboardSync.h.
#include "PanelSystem.h"

#include "backend.h"
#include "Clipboard/ClipboardSync.h"

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
	}

	void PanelSystem_RegisterArea( ui::Registry &reg )
	{
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
				// PHASE B: persist via config::SystemSettings::clipboard_sync.
				[]{ return g_bClipboardSyncEnabled.load( std::memory_order_relaxed ); },
				[]( bool b )
				{
					// PHASE B: persist via config::SystemSettings::clipboard_sync
					// -- for now this only flips the runtime flag, so the value
					// does not survive a restart. That is deliberate for Phase A,
					// not an oversight: see this file's header comment. The
					// *outbound* gate (a copy made inside gamescope reaching the
					// host) is Phase B too -- steamcompmgr.cpp's
					// gamescope_broadcast_clipboard(), hints->SetSelection().
					g_bClipboardSyncEnabled.store( b, std::memory_order_relaxed );
				} ) )
			.Help( "Shares the clipboard between games running here and the rest of your desktop, "
			       "both ways. Off stops text crossing in either direction; copying and pasting "
			       "between games still works." )
			.Default( true )
			.Keywords( "clipboard copy paste sync host" );

		a.Group( "Diagnostics" );

		a.Facts( "system.clipboard_status", "Clipboard status",
			[]{ return ClipboardSyncStatus(); } )
			.Help( "Which protocol clipboard sync with the host is actually using, or why it "
			       "isn't syncing at all. Read-only, nothing here can be changed." )
			.Keywords( "clipboard protocol status ext_data_control zwlr wl_data_device sdl" );
	}
}
