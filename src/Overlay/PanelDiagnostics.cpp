// The Compatibility area (id system.diagnostics, "Diagnostics" until 2026-11-08) --
// see PanelDiagnostics.h and superdoc/features/diagnostics.md.
#include "PanelDiagnostics.h"

#include <string>
#include <vector>

#include "Diagnostics.h"
#include "UI/Registry.h"

namespace gamescope
{
	namespace
	{
		using diagnostics::ScanoutState;

		std::string Headline( const DiagnosticsView &v )
		{
			if ( v.bRunning )
				return "Checking...";
			if ( !v.bEverChecked )
				return "Not checked yet";
			return v.scanout.sHeadline;
		}

		ui::ContentLine Line( ui::ContentKind eKind, std::string sText, int nSeverity = 0 )
		{
			ui::ContentLine l;
			l.eKind = eKind;
			l.sText = std::move( sText );
			l.nSeverity = nSeverity;
			return l;
		}

		// The body under the rows: the fix when there is one to apply, else the
		// one sentence of why nothing is shown.
		std::vector<ui::ContentLine> BodyLines()
		{
			const DiagnosticsView v = Diagnostics_View();
			std::vector<ui::ContentLine> out;
			if ( !v.bEverChecked )
				return out;

			using K = ui::ContentKind;
			const diagnostics::ScanoutResult &r = v.scanout;
			if ( r.eState == ScanoutState::Off )
			{
				out.push_back( Line( K::Heading, "Hyprland: direct scanout is off", 2 ) );
				out.push_back( Line( K::Plain, "Hyprland draws every frame itself, and that draw waits behind the game's GPU work," ) );
				out.push_back( Line( K::Plain, "so frames made by Frame generation are lost before they reach the screen." ) );
				out.push_back( Line( K::Plain, "" ) );
				out.push_back( Line( K::Plain, "Set render:direct_scanout to 1 or 2 and reload. 1 is on for a fullscreen window," ) );
				out.push_back( Line( K::Plain, "2 is on only for windows with the game content type. Your choice." ) );
				out.push_back( Line( K::Plain, "" ) );
				out.push_back( Line( K::Plain, "    Lua config:       render = { direct_scanout = 1 }  (or 2)" ) );
				out.push_back( Line( K::Plain, "    hyprlang config:  render { direct_scanout = 1 }    (or 2)" ) );
				out.push_back( Line( K::Plain, "" ) );
				out.push_back( Line( K::Plain, "Screen capture turns scanout off again while it runs." ) );
			}
			else if ( !r.sReason.empty() )
			{
				out.push_back( Line( K::Plain, r.sReason ) );
			}
			return out;
		}
	}

	void PanelDiagnostics_RegisterArea( ui::Registry &reg )
	{
		// Labelled "Compatibility" since 2026-11-08; the id stays, as About kept
		// system.changelog.
		ui::Area &a = reg.Add( "system.diagnostics", "Compatibility", ui::Section::System );

		a.Keywords( "compatibility environment desktop gpu recommended detect machine "
		            "diagnostics check hyprland gnome kde direct scanout direct_scanout hyprctl frames "
		            "lost frame generation host nested wayland problem warning fix "
		            "wiki docs documentation render options browser link open" );
		a.Summary( []{ return Headline( Diagnostics_View() ); } );
		a.WarnWhen( []{ return Diagnostics_HasWarning(); } );

		a.Columns( 2 );

		// ---- left: machine-wide settings ----------------------------------
		// Global, never per profile: they belong to the machine, not to a game. They
		// carry the config key of their global.json field (overlay.*), so the
		// per-profile dot and "Reset to inherited" leave them Plain.
		a.Group( "This machine" );
		a.GroupColumn( 0 );

		a.Switch( "display.block_direct_scanout", "Block direct scanout",
			ui::AnyBind::Of<bool>(
				[]{ return Diagnostics_BlockScanout(); },
				[]( bool b ) { Diagnostics_SetBlockScanout( b ); } ) )
			.Key( "overlay.diagnostics_block_scanout" )
			.ShownWhen( []{ return Diagnostics_ScanoutApplicable(); } )
			.Help( "Stops the host from putting gamescope's window straight on the screen. This may "
			       "help with picture corruption or artifacts. It costs frames with Frame generation, "
			       "because the host then draws every frame again. Not verified to work on every "
			       "host." )
			.Default( false )
			.Keywords( "scanout direct wayland host gnome mutter hyprland artifacts corruption "
			           "block noise glitch plane" );

		// Latched at the first output image (rendervulkan.cpp), so a change applies on
		// the next launch: the orange status says so on the row.
		a.Switch( "display.compress_output_images", "Compress output images",
			ui::AnyBind::Of<bool>(
				[]{ return Diagnostics_CompressOutputImages(); },
				[]( bool b ) { Diagnostics_SetCompressOutputImages( b ); } ) )
			.Key( "overlay.compress_output_images" )
			.ShownWhen( []{ return Diagnostics_ScanoutApplicable(); } )
			.StatusWhen( []{ return Diagnostics_CompressOutputImages() != Diagnostics_CompressOutputImagesAtLaunch()
			                         ? std::string( "Restart to apply" ) : std::string(); } )
			.Help( "On is gamescope's normal behaviour: the graphics driver may store the output "
			       "images compressed (AMD DCC). Turn it Off if the host's direct scanout "
			       "misbehaves: on RDNA 3 with Hyprland's direct scanout on, the driver picks a "
			       "compression the screen refuses and Hyprland reconfigures the display on every "
			       "frame. Applies on the next launch." )
			.Default( true )
			.Keywords( "dcc compress compression modifier amd rdna output images scanout wayland "
			           "host gnome mutter scrambled noise corruption restart" );

		a.Group( "Recommended settings" );
		a.GroupColumn( 0 );

		a.Action( "diagnostics.pick_recommended", "Pick recommended settings", "Apply",
			[]{ Diagnostics_ApplyRecommended(); } )
			.Help( "Sets the machine-wide switches above to the recommended values for the detected "
			       "desktop. Your own choices are only overwritten when you press this." )
			.Keywords( "pick recommended settings defaults apply reset desktop" );

		a.Action( "diagnostics.redetect", "Re-detect", "Detect", []{ Diagnostics_Redetect(); } )
			.Help( "Reads the desktop session, GPU and backend again and updates the Environment "
			       "rows. It changes no setting." )
			.Keywords( "redetect detect desktop gpu backend environment refresh" );

		// ---- right: environment and checks ---------------------------------
		a.Group( "Environment" );
		a.GroupColumn( 1 );

		a.Facts( "diagnostics.desktop", "Desktop", []{ return Diagnostics_Facts().sDesktop; } )
			.Help( "The desktop session gamescope runs under, read at startup (for example Hyprland, "
			       "GNOME, KDE Plasma, or none). It decides which settings are recommended for this "
			       "machine." )
			.Keywords( "desktop session hyprland gnome kde plasma" );

		a.Facts( "diagnostics.gpu", "GPU", []{ return Diagnostics_Facts().sGpu; } )
			.Help( "The graphics card and driver in use." )
			.Keywords( "gpu graphics card driver amd nvidia intel" );

		a.Facts( "diagnostics.backend", "Backend", []{ return Diagnostics_Facts().sBackend; } )
			.Help( "How gamescope presents: nested under a Wayland host, in an SDL window, or "
			       "directly on the display. The two host settings only apply to the Wayland "
			       "backend." )
			.Keywords( "backend wayland sdl drm nested" );

		a.Facts( "diagnostics.recommended", "Recommended", []{ return Diagnostics_RecommendationText(); } )
			.Help( "What this machine's desktop recommends. A switch you have never set uses this "
			       "value; once you set it yourself, only Pick recommended settings changes it." )
			.Keywords( "recommended defaults desktop" );

		a.Facts( "diagnostics.hypr_scanout", "Direct scanout",
			[]{ return Headline( Diagnostics_View() ); } )
			.Help( "Whether the host lets gamescope's window go straight to the screen. Without it "
			       "the host draws every frame itself, and frames that Frame generation makes are "
			       "lost. Read from Hyprland's settings when gamescope runs nested under Hyprland." )
			.Keywords( "direct scanout hyprland render direct_scanout frames lost" )
			// The docs button lives in the Inspector only (Entry::InspectorVerb): on
			// this row's page and on the area's Overview. Shown whenever the check
			// applies, i.e. under Hyprland on a backend that can scan out.
			.InspectorVerb( "Hyprland render options", "Open", []{ Diagnostics_OpenDocs(); },
			                []
			{
				const ScanoutState e = Diagnostics_View().scanout.eState;
				return e == ScanoutState::On || e == ScanoutState::Off || e == ScanoutState::Unknown;
			} )
			// Global, never per profile: the host setup belongs to the machine, not to a game.
			.InspectorSwitch( "Silence warning", []{ return Diagnostics_Silenced(); },
			                  []( bool b ){ Diagnostics_SetSilenced( b ); } )
			.Live( "render:direct_scanout", []
			{
				const diagnostics::ScanoutResult r = Diagnostics_View().scanout;
				return ui::Fact{ "render:direct_scanout", r.nValue >= 0 ? std::to_string( r.nValue ) : "unknown" };
			} )
			.Live( "detail", []
			{
				const DiagnosticsView v = Diagnostics_View();
				return ui::Fact{ "detail", v.scanout.sReason.empty() ? "none" : v.scanout.sReason };
			} );

		a.Group( "Actions" );
		a.GroupColumn( 1 );

		a.Action( "diagnostics.recheck", "Recheck all", "Recheck", []{ Diagnostics_Recheck(); } )
			.DisabledUnless( []{ return !Diagnostics_View().bRunning; }, "A check is running." )
			.Help( "Runs every diagnostic again and updates the rows. It never shows a pop-up." )
			.Keywords( "recheck rerun refresh check again diagnostics" );

		a.Content( []{ return BodyLines(); } );
		a.FollowsTail( []{ return false; } );
	}
}
