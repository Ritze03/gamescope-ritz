// Unit tests for the pure half of the Profiles area
// (src/Overlay/PanelConfig.h's `panelconfig` namespace) under the v2 model
// (superdoc/features/profiles.md, "The Profiles area"): what the list shows
// under the filter, what each line's secondary text is, what the Inherits
// dropdown offers, what the Create/Copy/Edit form refuses and why, when
// Delete is blocked and what its prompt says, what the status row and the
// badge read -- and the two ordering guarantees the area rests on: a
// profile saved synchronously is listed by the very next directory read
// (the 2026-09-05 "restart to see a new profile" bug), and selecting a line
// is SelectProfile(), i.e. the assignment this game remembers.
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

	config::ProfileMeta Game( const std::string &sName, const std::string &sAppId,
	                          const std::string &sGameName = "", const std::string &sInherits = "" )
	{
		config::ProfileMeta m;
		m.name = sName;
		m.kind = config::ProfileKind::Game;
		m.app_id = sAppId;
		m.game_name = sGameName;
		m.inherits = sInherits;
		return m;
	}

	// The sketch's own four lines plus another game's profile.
	std::vector<config::ProfileMeta> Sketch()
	{
		return { General( "Casual" ), General( "Comp" ), Game( "Dota", "570", "Dota 2", "Casual" ),
		         General( "Profile1" ), Game( "Rust", "252490", "Rust", "Comp" ) };
	}
}

TEST_CASE( "the list labels game profiles as [Game] <profile name>, never the game's name", "[overlay_profiles]" )
{
	// requests-2026-09-06 item 2: "It shows the process/game name. Not the
	// actual profile name." A profile named CS2 reads as CS2 whatever the
	// game's window title is.
	REQUIRE( ListLabel( General( "Comp" ) ) == "Comp" );
	REQUIRE( ListLabel( Game( "CS2", "730", "Counter-Strike 2" ) ) == "[Game] CS2" );
	REQUIRE( ListLabel( Game( "Rust Ranked", "252490", "Rust" ) ) == "[Game] Rust Ranked" );
	// A migrated profile that has never run: no title seen yet, still its name.
	REQUIRE( ListLabel( Game( "252490", "252490" ) ) == "[Game] 252490" );

	// The tag/name split the list box draws (tag muted, name in the label
	// role) is the same string cut in two -- never a second spelling.
	REQUIRE( ListTag( Game( "CS2", "730", "Counter-Strike 2" ) ) == "[Game]" );
	REQUIRE( ListName( Game( "CS2", "730", "Counter-Strike 2" ) ) == "CS2" );
	REQUIRE( ListTag( General( "Comp" ) ).empty() );
	REQUIRE( ListName( General( "Comp" ) ) == "Comp" );

	// The game's name moved to the line's muted text: the title seen, else
	// the app id; nothing for a general profile.
	REQUIRE( GameName( Game( "CS2", "730", "Counter-Strike 2" ) ) == "Counter-Strike 2" );
	REQUIRE( GameName( Game( "252490", "252490" ) ) == "252490" );
	REQUIRE( GameName( General( "Comp" ) ).empty() );
}

TEST_CASE( "a line's secondary text: inherits <parent>, or launch option on the override's line", "[overlay_profiles]" )
{
	// A game profile's line names its game (muted, right-aligned), then
	// its parent: `Counter-Strike 2 · inherits Comp`, or the game alone.
	REQUIRE( ListSecondary( Game( "CS2", "730", "Counter-Strike 2", "Comp" ), "Comp", false )
	         == "Counter-Strike 2 · inherits Comp" );
	REQUIRE( ListSecondary( Game( "CS2", "730", "Counter-Strike 2" ), "CS2", false ) == "Counter-Strike 2" );
	REQUIRE( ListSecondary( Game( "252490", "252490" ), "252490", false ) == "252490" );
	REQUIRE( ListSecondary( General( "Comp" ), "Comp", false ).empty() );
	// `--profile Casual`: the line the session edits says so, first -- the
	// override is the more surprising fact.
	REQUIRE( ListSecondary( General( "Casual" ), "Casual", true ) == "launch option" );
	REQUIRE( ListSecondary( Game( "CS2", "730", "Counter-Strike 2", "Comp" ), "CS2", true )
	         == "launch option · Counter-Strike 2 · inherits Comp" );
	REQUIRE( ListSecondary( General( "Comp" ), "Casual", true ).empty() );
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

TEST_CASE( "the visible list under the filter, and the session profile is always on it", "[overlay_profiles]" )
{
	const std::vector<config::ProfileMeta> all = Sketch();
	const std::optional<std::string> oRust = std::string( "252490" );

	// Filter on, running Rust: Dota's profile is hidden, everything else shows.
	std::vector<size_t> on = VisibleProfiles( all, true, oRust, "Rust" );
	REQUIRE( on == std::vector<size_t>{ 0, 1, 3, 4 } );
	REQUIRE( VisibleIndexOf( all, on, "Rust" ) == 3 );
	REQUIRE( VisibleIndexOf( all, on, "Dota" ) == -1 );

	// Filter off: all five, in list order.
	std::vector<size_t> off = VisibleProfiles( all, false, oRust, "Rust" );
	REQUIRE( off == std::vector<size_t>{ 0, 1, 2, 3, 4 } );
	REQUIRE( VisibleIndexOf( all, off, "Dota" ) == 2 );

	// `--profile Dota` while running Rust, filter on: the session profile
	// is another game's, and it still has a line -- the selected one.
	std::vector<size_t> launched = VisibleProfiles( all, true, oRust, "Dota" );
	REQUIRE( launched == std::vector<size_t>{ 0, 1, 2, 3, 4 } );
	REQUIRE( VisibleIndexOf( all, launched, "Dota" ) == 2 );
}

TEST_CASE( "the Inherits dropdown offers None and every general profile", "[overlay_profiles]" )
{
	const std::vector<std::string> names = InheritOptionNames( Sketch() );
	REQUIRE( names == std::vector<std::string>{ "None", "Casual", "Comp", "Profile1" } );
	REQUIRE( InheritIndex( names, "Comp" ) == 2 );
	REQUIRE( InheritIndex( names, "" ) == 0 );
	// A parent that vanished from disk reads as None rather than as a
	// wrong general profile.
	REQUIRE( InheritIndex( names, "Gone" ) == 0 );

	REQUIRE( CountChildren( Sketch(), "Comp" ) == 1 );
	REQUIRE( CountChildren( Sketch(), "Profile1" ) == 0 );

	// A new game profile is a child of the general profile in play.
	REQUIRE( NewGameInherits( General( "Comp" ) ) == "Comp" );
	REQUIRE( NewGameInherits( Game( "Rust", "252490", "Rust", "Comp" ) ) == "Comp" );
	REQUIRE( NewGameInherits( Game( "Rust", "252490", "Rust" ) ).empty() );
}

TEST_CASE( "the form refuses a bad name or app id, inline and per field", "[overlay_profiles]" )
{
	const std::vector<config::ProfileMeta> all = Sketch();

	// The happy path.
	FormCheck ok = CheckProfileForm( true, "252490", "Rust Ranked", all );
	REQUIRE( ok.ok() );
	REQUIRE( ok.sName == "Rust Ranked" );

	// SanitizeProfileName()'s rules, surfaced before the config layer is
	// asked: an empty name, a name that would be changed, a taken name.
	REQUIRE_FALSE( CheckProfileForm( false, "", "", all ).sNameError.empty() );
	REQUIRE_FALSE( CheckProfileForm( false, "", "../etc", all ).sNameError.empty() );
	REQUIRE_FALSE( CheckProfileForm( false, "", " Comp", all ).sNameError.empty() );
	REQUIRE( CheckProfileForm( false, "", "Comp", all ).sNameError == "A profile named 'Comp' already exists" );
	// Editing Comp may keep the name Comp.
	REQUIRE( CheckProfileForm( false, "", "Comp", all, "Comp" ).ok() );

	// The app id: only asked for a game profile, digits only.
	REQUIRE( CheckProfileForm( false, "abc", "New", all ).ok() );
	REQUIRE( CheckProfileForm( true, "", "New", all ).sAppIdError == "Enter the game's app id" );
	REQUIRE( CheckProfileForm( true, "25x", "New", all ).sAppIdError == "The app id is digits only" );
	// Both fields wrong: both errors, so the user fixes them in one round.
	const FormCheck both = CheckProfileForm( true, "x", "Comp", all );
	REQUIRE_FALSE( both.sNameError.empty() );
	REQUIRE_FALSE( both.sAppIdError.empty() );
}

TEST_CASE( "delete: blocked on the last profile, and the prompt counts the children", "[overlay_profiles]" )
{
	REQUIRE( DeleteBlocker( 1 ) == "this is the last profile; create another before deleting it" );
	REQUIRE( DeleteBlocker( 0 ) == "this is the last profile; create another before deleting it" );
	REQUIRE( DeleteBlocker( 2 ).empty() );

	REQUIRE( DeleteChildrenLine( 0 ).empty() );
	REQUIRE( DeleteChildrenLine( 1 ) == "1 profile inherits from it and will keep its values." );
	REQUIRE( DeleteChildrenLine( 3 ) == "3 profiles inherit from it and will keep its values." );
}

TEST_CASE( "the status row and the badge name the session", "[overlay_profiles]" )
{
	const std::optional<std::string> oRust = std::string( "252490" );
	const std::optional<std::string> oNone;

	REQUIRE( GameStatusFact( "Rust", oRust ) == "Rust (252490)" );
	REQUIRE( GameStatusFact( "252490", oRust ) == "252490" );
	REQUIRE( GameStatusFact( "", oRust ) == "252490" );
	REQUIRE( GameStatusFact( "", oNone ) == "none identified" );

	// The sheet's compact line and the Inspector's long one say the same
	// thing; only the long one carries the app id and "this session".
	REQUIRE( StatusSummary( Game( "Rust", "252490", "Rust", "Comp" ), "Rust", oRust, std::nullopt )
	         == "[Game] Rust · inherits Comp" );
	// Another game's profile under Rust (a --profile pick made permanent
	// by selecting it): the game is worth saying then.
	REQUIRE( StatusSummary( Game( "Dota", "570", "Dota 2", "Casual" ), "Rust", oRust, std::nullopt )
	         == "[Game] Dota · inherits Casual · game Rust" );
	REQUIRE( StatusSummary( General( "Comp" ), "", oNone, std::nullopt )
	         == "Comp · no game identified" );
	REQUIRE( StatusSummary( General( "Casual" ), "Rust", oRust, std::string( "Casual" ) )
	         == "Casual (launch) · game Rust" );
	REQUIRE( StatusLong( Game( "Rust", "252490", "Rust", "Comp" ), "Rust", oRust, std::nullopt )
	         == "editing: [Game] Rust · inherits Comp · game: Rust (252490)" );
	REQUIRE( StatusLong( General( "Comp" ), "", oNone, std::nullopt )
	         == "editing: Comp · game: none identified" );
	REQUIRE( StatusLong( General( "Casual" ), "Rust", oRust, std::string( "Casual" ) )
	         == "editing: Casual · game: Rust (252490) · launch option: Casual (this session)" );

	REQUIRE( SessionBadge( Game( "Rust", "252490", "Rust", "Comp" ), false ) == "[Game] Rust" );
	REQUIRE( SessionBadge( General( "Casual" ), true ) == "Casual (launch)" );
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

TEST_CASE( "selecting a line is SelectProfile(): it loads and is remembered", "[overlay_profiles]" )
{
	TempConfigHome home;

	config::Settings a;
	a.gamescope.sharpness = 2;
	config::Settings b;
	b.gamescope.sharpness = 7;
	REQUIRE( config::CreateProfile( General( "Comp" ), &a ) );
	REQUIRE( config::CreateProfile( General( "Casual" ), &b ) );

	// The list's setter, minus the toast: index -> name -> SelectProfile.
	const std::vector<config::ProfileMeta> all = config::ListProfiles();
	const std::vector<size_t> visible = VisibleProfiles( all, true, std::nullopt, config::SessionProfile() );
	const int nCasual = VisibleIndexOf( all, visible, "Casual" );
	REQUIRE( nCasual >= 0 );

	const uint64_t ulBefore = config::ConfigGeneration();
	REQUIRE( config::SelectProfile( all[ visible[ (size_t)nCasual ] ].name ) );

	// Loaded: the session now edits Casual and resolves to its values.
	REQUIRE( config::SessionProfile() == "Casual" );
	REQUIRE( config::ResolvedSettings().gamescope.sharpness == 7 );
	// Remembered: a fresh session (no app id here) lands on it again.
	config::ResetSessionRoutingForTests();
	REQUIRE( config::SessionProfile() == "Casual" );
	// And every panel is told to reload.
	REQUIRE( config::ConfigGeneration() != ulBefore );
}
