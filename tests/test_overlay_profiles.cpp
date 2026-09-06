// Unit tests for the pure half of the Profiles area
// (src/Overlay/PanelConfig.h's `panelconfig` namespace) under the v2 model
// (superdoc/features/profiles.md): the list label rule, the "Filter Game
// Profiles" rule, the picker-index clamp, and the one ordering guarantee the
// area rests on -- a profile saved synchronously is listed by the very next
// directory read (the 2026-09-05 "restart to see a new profile" bug).
//
// PanelConfig.cpp itself needs Notifications, Fonts and the live overlay, so
// it is not linked here; the parts of it that can be wrong in a way a
// screenshot would not show live in the header as plain functions.
#include <catch2/catch_test_macros.hpp>

#include "Overlay/PanelConfig.h"
#include "Config/ConfigManager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

using namespace gamescope;
using namespace gamescope::panelconfig;

namespace
{
	// Same shape as tests/test_config.cpp's fixture: a throwaway
	// XDG_CONFIG_HOME so nothing here ever touches ~/.config/gamescope-ritz.
	struct TempConfigHome
	{
		std::filesystem::path dir;

		TempConfigHome()
		{
			std::filesystem::path base = std::filesystem::temp_directory_path() /
				( "gamescope-ritz-profiles-test-XXXXXX" );
			std::string sTemplate = base.string();
			char *pszResult = mkdtemp( sTemplate.data() );
			REQUIRE( pszResult != nullptr );
			dir = pszResult;
			setenv( "XDG_CONFIG_HOME", dir.c_str(), 1 );
			config::ResetSessionRoutingForTests();
		}

		~TempConfigHome()
		{
			config::FlushPendingWrites();
			std::error_code ec;
			std::filesystem::remove_all( dir, ec );
			unsetenv( "XDG_CONFIG_HOME" );
			config::ResetSessionRoutingForTests();
		}
	};

	config::ProfileMeta General( const std::string &sName )
	{
		config::ProfileMeta m;
		m.name = sName;
		return m;
	}

	config::ProfileMeta Game( const std::string &sName, const std::string &sAppId, const std::string &sGameName = "" )
	{
		config::ProfileMeta m;
		m.name = sName;
		m.kind = config::ProfileKind::Game;
		m.app_id = sAppId;
		m.game_name = sGameName;
		return m;
	}
}

TEST_CASE( "the list labels game profiles as [Game] <name>, falling back to the app id", "[overlay_profiles]" )
{
	REQUIRE( ListLabel( General( "Comp" ) ) == "Comp" );
	REQUIRE( ListLabel( Game( "Rust Ranked", "252490", "Rust" ) ) == "[Game] Rust" );
	// A migrated profile that has never run: no title seen yet.
	REQUIRE( ListLabel( Game( "252490", "252490" ) ) == "[Game] 252490" );
}

TEST_CASE( "Filter Game Profiles hides other games' profiles and never a general one", "[overlay_profiles]" )
{
	const std::optional<std::string> oRust = std::string( "252490" );
	const std::optional<std::string> oNone;

	REQUIRE( ShowsInList( General( "Comp" ), true, oRust ) );
	REQUIRE( ShowsInList( General( "Comp" ), true, oNone ) );
	REQUIRE( ShowsInList( Game( "Rust", "252490" ), true, oRust ) );
	REQUIRE_FALSE( ShowsInList( Game( "Dota", "570" ), true, oRust ) );
	REQUIRE_FALSE( ShowsInList( Game( "Dota", "570" ), true, oNone ) );
	// Filter off: everything.
	REQUIRE( ShowsInList( Game( "Dota", "570" ), false, oRust ) );
}

TEST_CASE( "the picker index agrees with what the picker shows", "[overlay_profiles]" )
{
	// The 2026-09-05 laptop bug: one profile existed at startup, the index was
	// still -1, the Choice drew item 0, and Delete's range guard returned
	// silently. -1 with a non-empty list must become 0 -- the item drawn.
	REQUIRE( ClampPickerSelection( -1, 1 ) == 0 );
	REQUIRE( ClampPickerSelection( -1, 3 ) == 0 );
	// Too large (a profile was deleted from the end): back to the first.
	REQUIRE( ClampPickerSelection( 3, 3 ) == 0 );
	REQUIRE( ClampPickerSelection( 7, 3 ) == 0 );
	// In range: untouched.
	REQUIRE( ClampPickerSelection( 0, 1 ) == 0 );
	REQUIRE( ClampPickerSelection( 2, 3 ) == 2 );
	// Empty list: -1 whatever it was, so the actions' guards still refuse.
	REQUIRE( ClampPickerSelection( -1, 0 ) == -1 );
	REQUIRE( ClampPickerSelection( 0, 0 ) == -1 );
	REQUIRE( ClampPickerSelection( 5, 0 ) == -1 );
}

TEST_CASE( "status facts name the game", "[overlay_profiles]" )
{
	REQUIRE( GameFact( std::string( "440" ) ) == "app 440" );
	REQUIRE( GameFact( std::nullopt ) == "none identified" );
}

TEST_CASE( "a synchronously created profile is listed by the next directory read", "[overlay_profiles]" )
{
	TempConfigHome home;

	REQUIRE( config::ListProfiles().empty() );

	// What the Create modal does: CreateProfile() returns with the file on
	// disk, the list is re-read. No flush, no wait.
	config::Settings current;
	current.gamescope.sharpness = 4;
	REQUIRE( config::CreateProfile( General( "Fresh" ), &current ) );

	std::vector<config::ProfileMeta> vec = config::ListProfiles();
	REQUIRE( std::find_if( vec.begin(), vec.end(),
		[]( const config::ProfileMeta &m ) { return m.name == "Fresh"; } ) != vec.end() );

	// The saved file is the one selecting it loads.
	std::optional<config::Settings> oLoaded = config::LoadProfile( "Fresh" );
	REQUIRE( oLoaded.has_value() );
	REQUIRE( oLoaded->gamescope.sharpness == 4 );
}
