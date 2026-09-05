// Unit tests for the pure half of the Profiles / Per-game areas
// (src/Overlay/PanelConfig.h's `panelconfig` namespace) and for the one
// ordering guarantee the 2026-09-05 rework rests on: a profile saved
// synchronously is listed by the very next directory read.
//
// WHY THESE ARE TESTABLE. PanelConfig.cpp itself needs Notifications, Fonts
// and the live overlay, so it is not linked here. The parts of it that can
// be WRONG in a way a screenshot would not show -- the rebuild hash ignoring
// an input (a row that never appears), a backup restored into the wrong
// file, a Status fact that says the opposite of the routing -- were moved
// into the header as plain functions of plain values, and are held to
// account here.
//
// The user's report ("I need to restart the whole game to actually see a
// freshly created profile") was the hash never moving because the write was
// still queued when the list was re-read. The last test is that sequence,
// with the synchronous call the panel now makes.
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
		}

		~TempConfigHome()
		{
			std::error_code ec;
			std::filesystem::remove_all( dir, ec );
			unsetenv( "XDG_CONFIG_HOME" );
		}
	};

	StatusInputs Baseline()
	{
		StatusInputs in;
		in.oAppId = std::string( "440" );
		in.bOverrideActive = false;
		in.bHasSavedPerGameConfig = false;
		in.vecProfileNames = { "FPS", "Quality" };
		in.vecOtherGameIds = { "570" };
		in.sLastAppliedProfile = "";
		in.bHasBackup = false;
		return in;
	}
}

// =========================================================================
//  The rebuild hash -- every input that drives a row must move it
// =========================================================================
TEST_CASE( "the status hash is stable for equal inputs", "[overlay_profiles]" )
{
	REQUIRE( StatusHash( Baseline() ) == StatusHash( Baseline() ) );
}

TEST_CASE( "the status hash moves when a profile is saved", "[overlay_profiles]" )
{
	// The bug: the list did not move, so this did not either, so the sheet
	// was never rebuilt. With the list updated the hash must differ.
	StatusInputs a = Baseline();
	StatusInputs b = Baseline();
	b.vecProfileNames.push_back( "New" );
	REQUIRE( StatusHash( a ) != StatusHash( b ) );

	// The very first profile too -- the case where the old area had no
	// picker row at all.
	StatusInputs none = Baseline();
	none.vecProfileNames.clear();
	StatusInputs first = none;
	first.vecProfileNames = { "First" };
	REQUIRE( StatusHash( none ) != StatusHash( first ) );
}

TEST_CASE( "the status hash moves on the inputs the rework added", "[overlay_profiles]" )
{
	StatusInputs base = Baseline();

	SECTION( "last applied profile -- the Status rows" )
	{
		StatusInputs b = base;
		b.sLastAppliedProfile = "FPS";
		REQUIRE( StatusHash( base ) != StatusHash( b ) );
	}
	SECTION( "a backup appearing -- the Restore previous settings row" )
	{
		StatusInputs b = base;
		b.bHasBackup = true;
		REQUIRE( StatusHash( base ) != StatusHash( b ) );
	}
	SECTION( "which file the backup was taken for -- which area's Restore row" )
	{
		StatusInputs g = base;
		g.bHasBackup = true;
		g.bBackupWasOverride = false;
		StatusInputs p = g;
		p.bBackupWasOverride = true;
		REQUIRE( StatusHash( g ) != StatusHash( p ) );
	}
	SECTION( "the pre-existing inputs still count" )
	{
		StatusInputs b = base;
		b.bOverrideActive = true;
		REQUIRE( StatusHash( base ) != StatusHash( b ) );
		StatusInputs c = base;
		c.bHasSavedPerGameConfig = true;
		REQUIRE( StatusHash( base ) != StatusHash( c ) );
		StatusInputs d = base;
		d.vecOtherGameIds.push_back( "730" );
		REQUIRE( StatusHash( base ) != StatusHash( d ) );
		StatusInputs e = base;
		e.oAppId.reset();
		REQUIRE( StatusHash( base ) != StatusHash( e ) );
	}
}

TEST_CASE( "the status hash separates the two lists", "[overlay_profiles]" )
{
	// A name moving from one list to the other is a different sheet
	// (different rows), so it must be a different hash -- a naive
	// concatenation of the two lists would collide here.
	StatusInputs a = Baseline();
	a.vecProfileNames = { "A", "B" };
	a.vecOtherGameIds = {};
	StatusInputs b = Baseline();
	b.vecProfileNames = { "A" };
	b.vecOtherGameIds = { "B" };
	REQUIRE( StatusHash( a ) != StatusHash( b ) );
}

// =========================================================================
//  The one-step backup
// =========================================================================
TEST_CASE( "a backup round-trips the Settings value it was taken from", "[overlay_profiles]" )
{
	config::Settings before;
	before.gamescope.sharpness = 3;
	before.fps_display.enabled = !config::FpsDisplaySettings{}.enabled;
	before.last_applied_profile = "Old";

	SettingsBackup backup{ before, "New", false };

	// Simulate the Use: the live target changes underneath.
	config::Settings live = before;
	live.gamescope.sharpness = 7;
	live.last_applied_profile = "New";
	REQUIRE( live.gamescope.sharpness != before.gamescope.sharpness );

	// Restore writes the backup back verbatim -- including the provenance
	// breadcrumb, so the Status row goes back to naming the old source.
	live = backup.settings;
	REQUIRE( live.gamescope.sharpness == 3 );
	REQUIRE( live.fps_display.enabled == before.fps_display.enabled );
	REQUIRE( live.last_applied_profile == "Old" );
	REQUIRE( backup.sReplacedBy == "New" );
}

TEST_CASE( "a backup is only restorable into the file it came from", "[overlay_profiles]" )
{
	SettingsBackup fromGlobal{ config::Settings{}, "FPS", /*bWasOverride=*/false };
	SettingsBackup fromGame{ config::Settings{}, "FPS", /*bWasOverride=*/true };

	REQUIRE( BackupMatchesRouting( fromGlobal, false ) );
	REQUIRE_FALSE( BackupMatchesRouting( fromGlobal, true ) );   // switched separate settings on since
	REQUIRE( BackupMatchesRouting( fromGame, true ) );
	REQUIRE_FALSE( BackupMatchesRouting( fromGame, false ) );    // switched them off since
}

// =========================================================================
//  Status wording
// =========================================================================
TEST_CASE( "status facts say where edits go and what is in use", "[overlay_profiles]" )
{
	const std::optional<std::string> oId = std::string( "440" );
	const std::optional<std::string> oNone;

	REQUIRE( EditsGoTo( oId, false ) == "global.json" );
	REQUIRE( EditsGoTo( oId, true ) == "this game (games/440.json)" );
	// No app id: there is no per-game file, whatever the flag says.
	REQUIRE( EditsGoTo( oNone, true ) == "global.json" );

	REQUIRE( GameFact( oId ) == "app 440" );
	REQUIRE( GameFact( oNone ) == "none identified" );

	REQUIRE( ProfileFact( "" ) == "none" );
	REQUIRE( ProfileFact( "FPS" ) == "FPS" );

	REQUIRE( OwnSettingsFact( true ).rfind( "on -- ", 0 ) == 0 );
	REQUIRE( OwnSettingsFact( false ).rfind( "off -- ", 0 ) == 0 );

	REQUIRE( PerGameFilePath( oId ) == "games/440.json" );
}

// =========================================================================
//  The sequence the bug was in: save, then list
// =========================================================================
TEST_CASE( "a synchronously saved profile is listed by the next directory read", "[overlay_profiles]" )
{
	TempConfigHome home;

	REQUIRE( config::ListProfiles().empty() );

	// What SaveCurrentAsNewProfile() does now: SaveProfile() returns with
	// the file on disk, RefreshLists() lists it. No flush, no wait.
	config::Settings current;
	current.gamescope.sharpness = 4;
	REQUIRE( config::SaveProfile( "Fresh", current ) );

	std::vector<std::string> vecNames = config::ListProfiles();
	REQUIRE( std::find( vecNames.begin(), vecNames.end(), "Fresh" ) != vecNames.end() );

	// And the hash that drives the rebuild moves with it.
	StatusInputs before = Baseline();
	before.vecProfileNames.clear();
	StatusInputs after = before;
	after.vecProfileNames = vecNames;
	REQUIRE( StatusHash( before ) != StatusHash( after ) );

	// The saved file is the one Use reads back.
	std::optional<config::Settings> oLoaded = config::LoadProfile( "Fresh" );
	REQUIRE( oLoaded.has_value() );
	REQUIRE( oLoaded->gamescope.sharpness == 4 );
}

// =========================================================================
//  Phase B -- the inputs and wording the active profile / auto-save added
// =========================================================================
TEST_CASE( "the status hash moves on the Phase B inputs", "[overlay_profiles]" )
{
	StatusInputs base = Baseline();

	SECTION( "the active profile -- Status, Save changes, auto-save's disabled state" )
	{
		StatusInputs b = base;
		b.sActiveProfile = "FPS";
		REQUIRE( StatusHash( base ) != StatusHash( b ) );
	}
	SECTION( "auto-save -- the saving fact and the Use confirm" )
	{
		StatusInputs b = base;
		b.bAutoSave = true;
		REQUIRE( StatusHash( base ) != StatusHash( b ) );
	}
	SECTION( "the dirty count -- every value is a different sheet" )
	{
		StatusInputs unknown = base; unknown.nDirtySections = -1;
		StatusInputs clean = base;   clean.nDirtySections = 0;
		StatusInputs one = base;     one.nDirtySections = 1;
		StatusInputs two = base;     two.nDirtySections = 2;
		REQUIRE( StatusHash( unknown ) != StatusHash( clean ) );
		REQUIRE( StatusHash( clean ) != StatusHash( one ) );
		REQUIRE( StatusHash( one ) != StatusHash( two ) );
	}
}

TEST_CASE( "the changes-since-applied fact", "[overlay_profiles]" )
{
	REQUIRE( ChangesFact( std::nullopt ) == "n/a -- no profile is active" );
	REQUIRE( ChangesFact( 0 ) == "none" );
	REQUIRE( ChangesFact( 1 ) == "1 section changed" );
	REQUIRE( ChangesFact( 3 ) == "3 sections changed" );
}

TEST_CASE( "the saving fact names where edits go", "[overlay_profiles]" )
{
	REQUIRE( SavingFact( false, false ) == "every change is saved to disk immediately" );
	REQUIRE( SavingFact( false, true ) == "every change is saved to disk immediately" ); // auto-save with no profile: nothing to say
	REQUIRE( SavingFact( true, true ).find( "automatically into the profile" ) != std::string::npos );
	REQUIRE( SavingFact( true, false ).find( "press Save changes" ) != std::string::npos );
}

TEST_CASE( "Use confirms only when there is something to lose", "[overlay_profiles]" )
{
	// Clean, or no active profile: a plain press.
	REQUIRE( UseConfirmPrompt( std::nullopt, false ).empty() );
	REQUIRE( UseConfirmPrompt( 0, false ).empty() );
	// Dirty with auto-save on: already saved, nothing to lose.
	REQUIRE( UseConfirmPrompt( 3, true ).empty() );
	// Dirty with auto-save off: the one case that asks, and says how much.
	REQUIRE( UseConfirmPrompt( 1, false ) == "discard 1 unsaved change?" );
	REQUIRE( UseConfirmPrompt( 2, false ) == "discard 2 unsaved changes?" );
}

TEST_CASE( "Save changes is disabled for exactly three reasons", "[overlay_profiles]" )
{
	REQUIRE( SaveChangesBlocker( false, false, std::nullopt ) == "no profile is active" );
	REQUIRE( SaveChangesBlocker( true, true, 2 ) == "auto-save is on -- already saved" );
	REQUIRE( SaveChangesBlocker( true, false, 0 ) == "nothing has changed" );
	// Dirty with auto-save off: enabled.
	REQUIRE( SaveChangesBlocker( true, false, 1 ).empty() );
	// Profile unreadable (nullopt) but active: let the user try -- the
	// action itself reports the failure.
	REQUIRE( SaveChangesBlocker( true, false, std::nullopt ).empty() );
}

TEST_CASE( "a backup carries the active profile it replaced", "[overlay_profiles]" )
{
	SettingsBackup backup{ config::Settings{}, "New", false, "Old" };
	REQUIRE( backup.sPreviousActiveProfile == "Old" );
	SettingsBackup fresh{ config::Settings{}, "First", false, "" };
	REQUIRE( fresh.sPreviousActiveProfile.empty() );
}
