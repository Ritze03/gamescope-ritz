// The Steam Overlay area (id setup.steam_overlay) -- see PanelSteamOverlay.h and
// superdoc/features/steam-overlay.md.
#include "PanelSteamOverlay.h"

#include <string>

#include "SteamOverlay.h"
#include "UI/Registry.h"

namespace gamescope
{
	void PanelSteamOverlay_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "setup.steam_overlay", "Steam Overlay", ui::Section::Setup );

		a.Keywords( "steam overlay dummy window shift tab lag notifications control enable" );
		a.Summary( []{ return std::string( SteamOverlay_Control() ? "Own window" : "Inside the game" ); } );

		// Global, never per profile: the config keys name the global.json fields
		// (overlay.*), so the per-profile dot and "Reset to inherited" leave the rows Plain.
		a.Switch( "overlay.steam_overlay_control", "Steam overlay control",
			ui::AnyBind::Of<bool>(
				[]{ return SteamOverlay_Control(); },
				[]( bool b ) { SteamOverlay_SetControl( b ); } ) )
			.Key( "overlay.steam_overlay_control" )
			.StatusWhen( []{ return SteamOverlay_Control() != SteamOverlay_ControlAtLaunch()
			                         ? std::string( "Restart to apply" ) : std::string(); } )
			.Help( "On: gamescope-ritz shows Steam's overlay in its own window above the game, "
			       "instead of Steam loading it into the game. Off keeps the usual behaviour, with "
			       "the overlay inside the game. Applies on the next launch." )
			.Default( false )
			.Keywords( "steam overlay control dummy window inside game lag restart" );

		a.Switch( "overlay.steam_overlay_enabled", "Enable Steam Overlay",
			ui::AnyBind::Of<bool>(
				[]{ return SteamOverlay_Enabled(); },
				[]( bool b ) { SteamOverlay_SetEnabled( b ); } ) )
			.Key( "overlay.steam_overlay_enabled" )
			.ShownWhen( []{ return SteamOverlay_Control(); } )
			.Help( "Turns the Steam overlay off and on while the game keeps running." )
			.Default( true )
			.Keywords( "steam overlay enable disable on off shift tab" );
	}
}
