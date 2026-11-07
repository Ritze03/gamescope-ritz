// The Diagnostics pure half (src/Overlay/DiagnosticsLogic.h): parsing what
// `hyprctl -j getoption render:direct_scanout` printed, and deciding what that
// means for each place gamescope can run. No process is started here.

#include <catch2/catch_test_macros.hpp>

#include "Overlay/DiagnosticsLogic.h"

using namespace gamescope::diagnostics;

namespace
{
	Facts Nested() { Facts f; f.bBackendApplicable = true; f.sBackend = "Wayland"; f.bHyprland = true; return f; }

	RunResult Ok( const char *psz )
	{
		RunResult r;
		r.eOutcome = RunOutcome::Ok;
		r.sOut = psz;
		return r;
	}
	RunResult Out( RunOutcome e, int nExit = 0 ) { RunResult r; r.eOutcome = e; r.nExit = nExit; return r; }
}

TEST_CASE( "diagnostics: direct_scanout JSON parses, garbage does not", "[diagnostics]" )
{
	REQUIRE( ParseDirectScanout( R"({"option": "render:direct_scanout", "int": 1, "set": true })" ) == 1 );
	REQUIRE( ParseDirectScanout( R"({"option": "render:direct_scanout", "int": 0, "set": false })" ) == 0 );
	REQUIRE( ParseDirectScanout( R"({"int": 2})" ) == 2 );
	REQUIRE( !ParseDirectScanout( "no such option" ) );
	REQUIRE( !ParseDirectScanout( "" ) );
	REQUIRE( !ParseDirectScanout( "int: 1\nset: true\n" ) );
	REQUIRE( !ParseDirectScanout( R"({"option": "x", "str": "a"})" ) );
	REQUIRE( !ParseDirectScanout( R"({"int": "1"})" ) );
	REQUIRE( !ParseDirectScanout( R"({"int": 1.5})" ) );
	REQUIRE( !ParseDirectScanout( R"([1])" ) );
	REQUIRE( !ParseDirectScanout( R"({"int": 1)" ) );   // truncated
}

TEST_CASE( "diagnostics: 0 is off and warns; 1 and 2 are on", "[diagnostics]" )
{
	ScanoutResult off = DecideDirectScanout( Nested(), Ok( R"({"int": 0, "set": false})" ) );
	REQUIRE( off.eState == ScanoutState::Off );
	REQUIRE( off.nValue == 0 );
	REQUIRE( WarnsAbout( off.eState ) );
	REQUIRE( off.sHeadline == "Off (0): generated frames are lost" );

	for ( const char *psz : { R"({"int": 1})", R"({"int": 2})" } )
	{
		ScanoutResult on = DecideDirectScanout( Nested(), Ok( psz ) );
		REQUIRE( on.eState == ScanoutState::On );
		REQUIRE( !WarnsAbout( on.eState ) );
	}
}

TEST_CASE( "diagnostics: a check that could not run never warns as if off", "[diagnostics]" )
{
	const RunResult runs[] = {
		Out( RunOutcome::NotInstalled ), Out( RunOutcome::TimedOut ), Out( RunOutcome::Failed, 3 ),
		Ok( "no such option" ), Ok( "garbage" ), Ok( "" ), Ok( R"({"int": 7})" ), Ok( R"({"int": -1})" ),
	};
	for ( const RunResult &run : runs )
	{
		ScanoutResult r = DecideDirectScanout( Nested(), run );
		REQUIRE( r.eState == ScanoutState::Unknown );
		REQUIRE( !WarnsAbout( r.eState ) );
		REQUIRE( !r.sReason.empty() );
	}
	REQUIRE( DecideDirectScanout( Nested(), Out( RunOutcome::Failed, 3 ) ).sReason.find( "exit 3" ) != std::string::npos );
	REQUIRE( DecideDirectScanout( Nested(), Ok( "no such option" ) ).sReason.find( "no render:direct_scanout" ) != std::string::npos );
}

TEST_CASE( "diagnostics: not applicable and not Hyprland say so and never warn", "[diagnostics]" )
{
	// Even with an Off answer sitting in the run result, the backend decides first.
	const RunResult off = Ok( R"({"int": 0})" );

	for ( const char *pszBackend : { "Headless", "DRM", "SDLWindow", "OpenVR" } )
	{
		Facts f = Nested();
		f.bBackendApplicable = false;
		f.sBackend = pszBackend;
		ScanoutResult r = DecideDirectScanout( f, off );
		REQUIRE( r.eState == ScanoutState::NotApplicable );
		REQUIRE( !WarnsAbout( r.eState ) );
		REQUIRE( r.nValue == -1 );
	}

	Facts sdl = Nested();
	sdl.bBackendApplicable = false;
	sdl.sBackend = "SDLWindow";
	REQUIRE( DecideDirectScanout( sdl, off ).sReason.find( "SDL" ) != std::string::npos );

	Facts other = Nested();
	other.bHyprland = false;
	ScanoutResult r = DecideDirectScanout( other, off );
	REQUIRE( r.eState == ScanoutState::NoHyprland );
	REQUIRE( !WarnsAbout( r.eState ) );
}

TEST_CASE( "diagnostics: the rail dot is on for Off alone, unless silenced", "[diagnostics]" )
{
	const auto Warns = []( const Facts &f, const RunResult &run )
	{
		const ScanoutResult r = DecideDirectScanout( f, run );
		return AnyWarns( &r, &r + 1 );
	};
	Facts sdl;      // not applicable: no host to scan out to
	Facts noHypr = Nested();
	noHypr.bHyprland = false;

	REQUIRE(  Warns( Nested(), Ok( R"({"int": 0})" ) ) );                 // Off
	REQUIRE( !Warns( Nested(), Ok( R"({"int": 1})" ) ) );                 // On
	REQUIRE( !Warns( Nested(), Ok( R"({"int": 2})" ) ) );
	REQUIRE( !Warns( Nested(), Out( RunOutcome::TimedOut ) ) );           // Could not check
	REQUIRE( !Warns( Nested(), Ok( "no such option" ) ) );
	REQUIRE( !Warns( sdl, Ok( R"({"int": 0})" ) ) );                      // Not applicable
	REQUIRE( !Warns( noHypr, Ok( R"({"int": 0})" ) ) );                   // Hyprland not detected

	// A recheck that finds it fixed clears the dot; with several results, any one warns.
	const ScanoutResult both[] = { DecideDirectScanout( Nested(), Ok( R"({"int": 2})" ) ),
	                               DecideDirectScanout( Nested(), Ok( R"({"int": 0})" ) ) };
	REQUIRE( AnyWarns( both, both + 2 ) );
	REQUIRE( !AnyWarns( both, both + 1 ) );
	REQUIRE( !AnyWarns( both, both ) );
}

TEST_CASE( "diagnostics: Silence warning mutes the alerts but not the state", "[diagnostics]" )
{
	const ScanoutResult off = DecideDirectScanout( Nested(), Ok( R"({"int": 0})" ) );
	REQUIRE( off.eState == ScanoutState::Off );          // the true state is unchanged
	REQUIRE( Alerts( off.eState, false ) );
	REQUIRE( !Alerts( off.eState, true ) );
	REQUIRE( !Alerts( ScanoutState::On, false ) );
	REQUIRE( AnyWarns( &off, &off + 1, false ) );
	REQUIRE( !AnyWarns( &off, &off + 1, true ) );
}

TEST_CASE( "diagnostics: Block direct scanout states the truth and never alerts", "[diagnostics]" )
{
	// Off would warn; with blocking on, the area says gamescope blocks it and nothing alerts.
	const ScanoutResult off = DecideDirectScanout( Nested(), Ok( R"({"int": 0})" ) );
	REQUIRE( Alerts( off.eState, false ) );
	const ScanoutResult blockedOff = WithBlock( off, true );
	REQUIRE( blockedOff.eState == ScanoutState::Blocked );
	REQUIRE( blockedOff.sHeadline == "Blocked by gamescope" );
	REQUIRE( blockedOff.nValue == 0 );
	REQUIRE( !WarnsAbout( blockedOff.eState ) );
	REQUIRE( !Alerts( blockedOff.eState, false ) );
	REQUIRE( !AnyWarns( &blockedOff, &blockedOff + 1, false ) );

	// Every applicable state reads as Blocked, on any host (GNOME has no hyprctl).
	Facts gnome = Nested();
	gnome.bHyprland = false;
	REQUIRE( WithBlock( DecideDirectScanout( gnome, Ok( "" ) ), true ).eState == ScanoutState::Blocked );
	REQUIRE( WithBlock( DecideDirectScanout( Nested(), Ok( R"({"int": 1})" ) ), true ).eState == ScanoutState::Blocked );
	REQUIRE( WithBlock( DecideDirectScanout( Nested(), Out( RunOutcome::Failed, 1 ) ), true ).eState == ScanoutState::Blocked );

	// Off by default: the result passes through untouched.
	const ScanoutResult same = WithBlock( off, false );
	REQUIRE( same.eState == ScanoutState::Off );
	REQUIRE( same.sHeadline == off.sHeadline );
	REQUIRE( Alerts( same.eState, false ) );

	// Nothing to block where the host cannot scan out, and nothing known before a check.
	Facts sdl;
	REQUIRE( WithBlock( DecideDirectScanout( sdl, Ok( "" ) ), true ).eState == ScanoutState::NotApplicable );
	REQUIRE( WithBlock( ScanoutResult{}, true ).eState == ScanoutState::Pending );
}
