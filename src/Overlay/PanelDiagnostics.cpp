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
				out.push_back( Line( K::Plain, "Leave it off for now. Hyprland then draws every frame itself, which costs" ) );
				out.push_back( Line( K::Plain, "some frames made by Frame generation." ) );
				out.push_back( Line( K::Plain, "" ) );
				out.push_back( Line( K::Plain, "With direct scanout on, entering or leaving fullscreen while a game runs can" ) );
				out.push_back( Line( K::Plain, "currently crash the GPU, so it is not recommended." ) );
			}
			else if ( r.eState == ScanoutState::On )
			{
				out.push_back( Line( K::Heading, "Hyprland: direct scanout is on", 2 ) );
				out.push_back( Line( K::Plain, "Caution: with direct scanout on, entering or leaving fullscreen while a game" ) );
				out.push_back( Line( K::Plain, "runs can currently crash the GPU. Turn it off if that happens." ) );
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
			.Help( "Whether Hyprland lets gamescope's window go straight to the screen. Off costs some "
			       "frames made by Frame generation, but it is the safe setting: on can currently "
			       "crash the GPU when entering fullscreen. Only checked when gamescope runs nested "
			       "under Hyprland." )
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
