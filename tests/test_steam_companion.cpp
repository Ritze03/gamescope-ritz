// The Steam companion overlay's pure half: the toggle state machine, the
// browser command line, and -- the reason this file exists at all -- the pair
// of X properties that hiding has to clear.
//
// Nothing here forks, opens an X connection or needs a compositor. The runtime
// (src/SteamCompanion.cpp) is exercised for real by the captures under
// build-release/verify-shots/steam-companion-2026-09-08/.
#include <catch2/catch_test_macros.hpp>

#include "SteamCompanionCmd.h"

#include <string>
#include <vector>

using namespace gamescope::companion;

// ===========================================================================
//  THE TRAP
// ===========================================================================
// The measured bug this whole feature had to be built around: an overlay left
// at opacity 0 that still carries STEAM_INPUT_FOCUS=1 is invisible and keeps
// swallowing every keystroke, because steamcompmgr's DetermineAndApplyFocus()
// routes input on `w->isOverlay && w->inputFocusMode` with no reference to
// opacity at all.
//
// SteamCompanionCmd.h pins this at COMPILE time with static_asserts -- a build
// that reintroduces it does not link, let alone run. These cases are the
// second lock: they fail loudly, by name, in the ordinary test run, so the
// failure says what is wrong rather than only where.
TEST_CASE( "hiding the companion clears BOTH the opacity and the input focus", "[steam_companion]" )
{
	const OverlayProps hidden = PropsFor( false );

	// The half everyone remembers.
	REQUIRE( hidden.uOpacity == 0u );

	// The half that was measured to swallow the keyboard. If this ever fails,
	// the overlay is invisible AND the game answers no key.
	REQUIRE( hidden.uInputFocus == 0u );
}

TEST_CASE( "showing the companion takes input focus and is fully opaque", "[steam_companion]" )
{
	const OverlayProps shown = PropsFor( true );
	REQUIRE( shown.uOpacity == kOpaque );
	REQUIRE( shown.uInputFocus == 1u );
}

TEST_CASE( "show and hide are opposites in both properties", "[steam_companion]" )
{
	// Not a restatement of the two above: this is what fails if someone adds
	// a third state and quietly makes hide() a partial show.
	REQUIRE_FALSE( PropsFor( true ) == PropsFor( false ) );
	REQUIRE( PropsFor( true ).uInputFocus != PropsFor( false ).uInputFocus );
	REQUIRE( PropsFor( true ).uOpacity != PropsFor( false ).uOpacity );
}

// ===========================================================================
//  The toggle state machine
// ===========================================================================
TEST_CASE( "the first press launches, later presses toggle", "[steam_companion]" )
{
	// Nothing running: press == launch, whatever the last visibility was.
	REQUIRE( PlanToggle( false, false ) == ToggleAct::Launch );
	REQUIRE( PlanToggle( false, true  ) == ToggleAct::Launch );

	// Running: a plain toggle.
	REQUIRE( PlanToggle( true, false ) == ToggleAct::Show );
	REQUIRE( PlanToggle( true, true  ) == ToggleAct::Hide );
}

TEST_CASE( "a browser that died while hidden relaunches on the next press", "[steam_companion]" )
{
	// The state after the reaper notices the exit is bRunning=false, and the
	// remembered visibility is irrelevant -- both rows above answer Launch.
	// This is the case a "have we ever started one" flag would get wrong.
	REQUIRE( PlanToggle( false, false ) == ToggleAct::Launch );
}

TEST_CASE( "a press during a launch cancels it rather than starting a second browser",
           "[steam_companion]" )
{
	// While the browser is starting, nPid > 0 and bWantShown is true, so the
	// plan is Hide -- never a second Launch. That is what makes "spawned at
	// most once" a property of the machine and not of a lock.
	REQUIRE( PlanToggle( true, true ) == ToggleAct::Hide );
}

// ===========================================================================
//  The command line
// ===========================================================================
static std::vector<std::string> Split( const std::string &s )
{
	std::vector<std::string> out;
	std::string sError;
	REQUIRE( SplitCommand( s, &out, &sError ) );
	REQUIRE( sError.empty() );
	return out;
}

TEST_CASE( "the browser command splits like a command line", "[steam_companion]" )
{
	REQUIRE( Split( "chromium --app=x" ) == std::vector<std::string>{ "chromium", "--app=x" } );

	// Runs of whitespace, and leading/trailing whitespace, are separators.
	REQUIRE( Split( "  a\t\t b \n" ) == std::vector<std::string>{ "a", "b" } );

	// Quotes hold an argument together.
	REQUIRE( Split( "browser \"--flag=a b\"" ) ==
		std::vector<std::string>{ "browser", "--flag=a b" } );
	REQUIRE( Split( "browser '--flag=a b'" ) ==
		std::vector<std::string>{ "browser", "--flag=a b" } );

	// A quoted section is part of the word it touches, not a word of its own.
	REQUIRE( Split( "--flag=\"a b\"c" ) == std::vector<std::string>{ "--flag=a bc" } );

	// Escapes.
	REQUIRE( Split( "a\\ b" ) == std::vector<std::string>{ "a b" } );
	REQUIRE( Split( "\"a\\\"b\"" ) == std::vector<std::string>{ "a\"b" } );
	REQUIRE( Split( "\"a\\\\b\"" ) == std::vector<std::string>{ "a\\b" } );

	// Single quotes are literal, backslashes included -- a Windows-ish path
	// pasted in by hand survives.
	REQUIRE( Split( "'C:\\x\\y'" ) == std::vector<std::string>{ "C:\\x\\y" } );

	// An empty quoted argument is still an argument.
	REQUIRE( Split( "a \"\" b" ) == std::vector<std::string>{ "a", "", "b" } );
}

TEST_CASE( "an unusable browser command is refused with a reason, never guessed at",
           "[steam_companion]" )
{
	std::vector<std::string> out;
	std::string sError;

	REQUIRE_FALSE( SplitCommand( "chromium \"--app=x", &out, &sError ) );
	REQUIRE_FALSE( sError.empty() );

	REQUIRE_FALSE( SplitCommand( "chromium '--app=x", &out, &sError ) );
	REQUIRE_FALSE( sError.empty() );

	REQUIRE_FALSE( SplitCommand( "   ", &out, &sError ) );
	REQUIRE_FALSE( sError.empty() );

	REQUIRE_FALSE( SplitCommand( "", &out, &sError ) );
	REQUIRE_FALSE( sError.empty() );
}

TEST_CASE( "the shipped default command builds the argv it is meant to", "[steam_companion]" )
{
	// Kept in step with ConfigSchema.h's companion_command by being the same
	// string; if that default changes, this case is the one that says so.
	const std::string sDefault =
		"chromium --ozone-platform=x11 --user-data-dir={profile} --no-first-run "
		"--no-default-browser-check --app={url}";

	std::vector<std::string> argv;
	std::string sError;
	REQUIRE( BuildArgv( sDefault, "https://steamcommunity.com/chat", "/cfg/companion-browser",
		&argv, &sError ) );

	REQUIRE( argv == std::vector<std::string>{
		"chromium",
		"--ozone-platform=x11",
		"--user-data-dir=/cfg/companion-browser",
		"--no-first-run",
		"--no-default-browser-check",
		"--app=https://steamcommunity.com/chat",
	} );
}

TEST_CASE( "a command with no {url} gets the page appended", "[steam_companion]" )
{
	std::vector<std::string> argv;
	std::string sError;
	REQUIRE( BuildArgv( "firefox --kiosk", "https://example.com", "/cfg/p", &argv, &sError ) );
	REQUIRE( argv == std::vector<std::string>{ "firefox", "--kiosk", "https://example.com" } );
}

TEST_CASE( "every {url} and {profile} in the command is substituted", "[steam_companion]" )
{
	std::vector<std::string> argv;
	std::string sError;
	REQUIRE( BuildArgv( "b --a={url} --b={url} --p={profile}/x", "U", "P", &argv, &sError ) );
	REQUIRE( argv == std::vector<std::string>{ "b", "--a=U", "--b=U", "--p=P/x" } );
}

TEST_CASE( "the URL is substituted AFTER splitting, so it can never add an argument",
           "[steam_companion]" )
{
	// This is the whole security story of the feature: the URL is a setting
	// the user types, and a URL that contained spaces, quotes or a shell
	// metacharacter must land inside ONE argument. If substitution ever moved
	// before the split, the strings below would each become several arguments
	// -- i.e. the URL row would be able to add flags to the browser.
	std::vector<std::string> argv;
	std::string sError;

	REQUIRE( BuildArgv( "b --app={url}", "https://x/ --no-sandbox", "P", &argv, &sError ) );
	REQUIRE( argv.size() == 2 );
	REQUIRE( argv[ 1 ] == "--app=https://x/ --no-sandbox" );

	REQUIRE( BuildArgv( "b --app={url}", "https://x/\" \"--kiosk", "P", &argv, &sError ) );
	REQUIRE( argv.size() == 2 );
	REQUIRE( argv[ 1 ] == "--app=https://x/\" \"--kiosk" );

	// And the same for the profile path, which is built from $XDG_CONFIG_HOME
	// and so can contain a space the moment the user's home directory does.
	// (Three arguments here, not two: this command has no {url}, so the page
	// is appended -- which is the other rule, checked above.)
	REQUIRE( BuildArgv( "b --dir={profile}", "u", "/a b/c d", &argv, &sError ) );
	REQUIRE( argv.size() == 3 );
	REQUIRE( argv[ 1 ] == "--dir=/a b/c d" );
	REQUIRE( argv[ 2 ] == "u" );
}

TEST_CASE( "a bad command is reported from BuildArgv too, not only from the splitter",
           "[steam_companion]" )
{
	std::vector<std::string> argv;
	std::string sError;
	REQUIRE_FALSE( BuildArgv( "b \"--app={url}", "u", "p", &argv, &sError ) );
	REQUIRE_FALSE( sError.empty() );
}
