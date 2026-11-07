// The Diagnostics area -- see PanelDiagnostics.h.
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
		ui::Area &a = reg.Add( "system.diagnostics", "Diagnostics", ui::Section::System );

		a.Keywords( "diagnostics check hyprland direct scanout direct_scanout hyprctl frames "
		            "lost frame generation host nested wayland problem warning fix "
		            "wiki docs documentation render options browser link open" );
		a.Summary( []{ return Headline( Diagnostics_View() ); } );
		a.WarnWhen( []{ return Diagnostics_HasWarning(); } );

		a.Group( "Hyprland" );

		a.Facts( "diagnostics.hypr_scanout", "Direct scanout",
			[]{ return Headline( Diagnostics_View() ); } )
			.Help( "Whether Hyprland lets gamescope's window go straight to the screen. Without it "
			       "Hyprland draws every frame itself, and frames that Frame generation makes are "
			       "lost. Only checked when gamescope runs nested under Hyprland." )
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

		a.Action( "diagnostics.recheck", "Recheck all", "Recheck", []{ Diagnostics_Recheck(); } )
			.DisabledUnless( []{ return !Diagnostics_View().bRunning; }, "A check is running." )
			.Help( "Runs every diagnostic again and updates the rows. It never shows a pop-up." )
			.Keywords( "recheck rerun refresh check again diagnostics" );

		a.Content( []{ return BodyLines(); } );
		a.FollowsTail( []{ return false; } );
	}
}
