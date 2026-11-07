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

	// The app id: only asked for a game profile, and it's an opaque string
	// now (2026-09-15), not digits-only -- a manually-set or non-Steam id
	// can be anything SanitizeAppId() (Config/AppId.h) accepts.
	REQUIRE( CheckProfileForm( false, "abc", "New", all ).ok() );
	REQUIRE( CheckProfileForm( true, "", "New", all ).sAppIdError == "Enter the game's app id" );
	REQUIRE( CheckProfileForm( true, "   ", "New", all ).sAppIdError == "Enter the game's app id" );
	// A non-numeric string id is legal and comes back trimmed.
	{
		const FormCheck stringId = CheckProfileForm( true, "25x", "New", all );
		REQUIRE( stringId.ok() );
		REQUIRE( stringId.sAppId == "25x" );
	}
	// A rejected character (path separator / control char) is still refused.
	REQUIRE( CheckProfileForm( true, "a/b", "New", all ).sAppIdError ==
	         "The app id can't contain '/', '\\', or control characters" );
	REQUIRE( CheckProfileForm( true, "a\\b", "New", all ).sAppIdError ==
	         "The app id can't contain '/', '\\', or control characters" );
	// Both fields wrong: both errors, so the user fixes them in one round.
	const FormCheck both = CheckProfileForm( true, "a/b", "Comp", all );
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

// ===========================================================================
//  Export / Import dialogs (superdoc/features/profile-export.md)
// ===========================================================================
namespace
{
	namespace pe = gamescope::config::profile_export;
	using gamescope::ui::controls::CheckState;
	using gamescope::ui::controls::ListBoxNav;

	std::string FirstChildOf( const std::string &sParent )
	{
		for ( const auto &g : pe::Groups() )
			if ( g.parent == sParent )
				return g.id;
		return {};
	}
}

TEST_CASE( "the tree is a parent row followed by its children, in group order", "[overlay_profiles]" )
{
	const std::vector<TreeRow> rows = TreeRows( pe::Groups() );
	REQUIRE( rows.size() == pe::Groups().size() + 6 );   // six rail groups
	REQUIRE( rows.front().bParent );
	REQUIRE( rows.front().sParent == "Display" );
	REQUIRE( !rows[ 1 ].bParent );
	REQUIRE( pe::Groups()[ (size_t)rows[ 1 ].nGroup ].parent == "Display" );
	size_t nParents = 0;
	for ( const TreeRow &r : rows )
	{
		nParents += r.bParent ? 1 : 0;
		if ( !r.bParent )
			REQUIRE( pe::Groups()[ (size_t)r.nGroup ].parent == r.sParent );
	}
	REQUIRE( nParents == 6 );
}

TEST_CASE( "a parent shows Off / On / Mixed from its children and a click sets them all", "[overlay_profiles]" )
{
	const auto &groups = pe::Groups();
	GroupSet ticked;
	REQUIRE( TreeParentState( groups, "Display", ticked, nullptr ) == CheckState::Off );

	ticked.insert( FirstChildOf( "Display" ) );
	REQUIRE( TreeParentState( groups, "Display", ticked, nullptr ) == CheckState::Mixed );

	// Mixed -> every child On.
	TreeClickParent( groups, "Display", ticked, nullptr );
	REQUIRE( TreeParentState( groups, "Display", ticked, nullptr ) == CheckState::On );
	for ( const auto &g : groups )
		if ( g.parent == "Display" )
			REQUIRE( ticked.count( g.id ) );

	// On -> every child Off.
	TreeClickParent( groups, "Display", ticked, nullptr );
	REQUIRE( ticked.empty() );

	// A child toggles on its own and leaves the others.
	TreeClickChild( ticked, "motion.blur", nullptr );
	REQUIRE( ticked.count( "motion.blur" ) );
	REQUIRE( TreeParentState( groups, "Motion", ticked, nullptr ) == CheckState::Mixed );
	TreeClickChild( ticked, "motion.blur", nullptr );
	REQUIRE( ticked.empty() );
}

TEST_CASE( "a group the paste lacks is greyed: never ticked, never counted", "[overlay_profiles]" )
{
	const auto &groups = pe::Groups();
	const GroupSet usable = ImportUsable( { "motion.blur" } );   // Motion has two children, one usable

	GroupSet ticked;
	TreeClickChild( ticked, "motion.framegen", &usable );   // greyed: a no-op
	REQUIRE( ticked.empty() );

	// The parent covers the usable child only, so it reads On when that one is.
	TreeClickParent( groups, "Motion", ticked, &usable );
	REQUIRE( ticked == GroupSet{ "motion.blur" } );
	REQUIRE( TreeParentState( groups, "Motion", ticked, &usable ) == CheckState::On );

	// A parent with no usable child has nothing to do.
	REQUIRE( TreeUsableChildren( groups, "Audio", &usable ) == 0 );
	TreeClickParent( groups, "Audio", ticked, &usable );
	REQUIRE( ticked == GroupSet{ "motion.blur" } );

	const std::vector<TreeRow> rows = TreeRows( groups );
	const std::vector<bool> enabled = TreeRowEnabled( groups, rows, &usable );
	for ( size_t i = 0; i < rows.size(); ++i )
	{
		const bool bExpected = rows[ i ].bParent ? rows[ i ].sParent == "Motion"
			: groups[ (size_t)rows[ i ].nGroup ].id == "motion.blur";
		REQUIRE( enabled[ i ] == bExpected );
	}

	// Ticks outside the paste never count.
	GroupSet stale{ "motion.blur", "audio.mixer" };
	REQUIRE( TicksWithin( stale, usable ) == GroupSet{ "motion.blur" } );
}

TEST_CASE( "the keyboard cursor steps over rows and skips greyed ones", "[overlay_profiles]" )
{
	const std::vector<bool> e{ false, true, true, false, true };
	// -1: the first arrow lands on the first usable row, whichever arrow.
	REQUIRE( TreeCursorStep( e, -1, ListBoxNav::Down ) == 1 );
	REQUIRE( TreeCursorStep( e, -1, ListBoxNav::Up ) == 1 );
	REQUIRE( TreeCursorStep( e, 1, ListBoxNav::Down ) == 2 );
	REQUIRE( TreeCursorStep( e, 2, ListBoxNav::Down ) == 4 );    // jumps the greyed 3
	REQUIRE( TreeCursorStep( e, 4, ListBoxNav::Down ) == 4 );    // clamps at the end
	REQUIRE( TreeCursorStep( e, 4, ListBoxNav::Up ) == 2 );
	REQUIRE( TreeCursorStep( e, 1, ListBoxNav::Up ) == 1 );      // greyed 0 is no stop
	REQUIRE( TreeCursorStep( e, 2, ListBoxNav::Home ) == 1 );
	REQUIRE( TreeCursorStep( e, 1, ListBoxNav::End ) == 4 );
	REQUIRE( TreeCursorStep( { false, false }, -1, ListBoxNav::Down ) == -1 );
	REQUIRE( TreeCursorStep( {}, -1, ListBoxNav::Down ) == -1 );
}

TEST_CASE( "export starts from the default ticks and is available only with something ticked", "[overlay_profiles]" )
{
	const GroupSet start = pe::DefaultTicks();
	REQUIRE( ExportPrimaryEnabled( start ) );
	REQUIRE( start.count( "display.resolution" ) == 0 );   // machine-specific: opt in
	REQUIRE( !ExportPrimaryEnabled( {} ) );

	REQUIRE( ExportToast( 1 ) == "Settings copied: 1 group" );
	REQUIRE( ExportToast( 12 ) == "Settings copied: 12 groups" );

	// Only an inheriting game profile has "Include inherited values".
	REQUIRE( !ExportOffersInherited( General( "Comp" ) ) );
	REQUIRE( !ExportOffersInherited( Game( "Rust", "252490", "", "" ) ) );
	REQUIRE( ExportOffersInherited( Game( "Rust", "252490", "", "Comp" ) ) );
}

TEST_CASE( "import ticks every present group except Resolution; the rest are not usable", "[overlay_profiles]" )
{
	const std::vector<std::string> present{ "display.resolution", "display.general", "motion.blur" };
	const GroupSet ticks = ImportInitialTicks( pe::Groups(), present );
	REQUIRE( ticks == GroupSet{ "display.general", "motion.blur" } );
	REQUIRE( ImportUsable( present ).size() == 3 );
	// A group the paste lacks is never ticked, default or not.
	REQUIRE( ImportInitialTicks( pe::Groups(), { "motion.blur" } ) == GroupSet{ "motion.blur" } );
	REQUIRE( ImportInitialTicks( pe::Groups(), {} ).empty() );
}

TEST_CASE( "import is available only for valid JSON with a ticked group and a usable name", "[overlay_profiles]" )
{
	const GroupSet one{ "motion.blur" };
	REQUIRE( ImportPrimaryEnabled( true, one, false, false ) );    // existing: the name is not asked
	REQUIRE( ImportPrimaryEnabled( true, one, true, true ) );
	REQUIRE( !ImportPrimaryEnabled( true, one, true, false ) );    // new, name refused
	REQUIRE( !ImportPrimaryEnabled( false, one, true, true ) );    // invalid JSON
	REQUIRE( !ImportPrimaryEnabled( true, {}, true, true ) );      // nothing ticked
	REQUIRE( !ImportPrimaryEnabled( false, {}, false, false ) );

	REQUIRE( ImportTextBlank( "" ) );
	REQUIRE( ImportTextBlank( " \n\t" ) );
	REQUIRE( !ImportTextBlank( " {" ) );
}

TEST_CASE( "an invalid paste never gets as far as an import", "[overlay_profiles]" )
{
	// What the dialog gates on: ParseExport().valid and ImportPrimaryEnabled().
	for ( const char *psz : { "{", "[]", "{\"settings\":{}}", "{\"gamescope_ritz_export\":1}",
	                          "{\"gamescope_ritz_export\":2,\"settings\":{}}", "plain text" } )
	{
		const pe::ParsedExport p = pe::ParseExport( psz );
		INFO( psz );
		REQUIRE( !p.valid );
		REQUIRE( !p.error.empty() );
		REQUIRE( !ImportPrimaryEnabled( p.valid, ImportInitialTicks( pe::Groups(), p.groups_present ), true, true ) );
	}
}

TEST_CASE( "the new profile's name starts from the pasted one, made free", "[overlay_profiles]" )
{
	const std::vector<config::ProfileMeta> existing{ General( "Comp" ), General( "Comp 2" ), General( "Imported" ) };
	REQUIRE( ImportSuggestedName( "Friend", existing ) == "Friend" );
	REQUIRE( ImportSuggestedName( "Comp", existing ) == "Comp 3" );
	REQUIRE( ImportSuggestedName( "", existing ) == "Imported 2" );
	REQUIRE( ImportSuggestedName( "bad/name", existing ) == "badname" );   // sanitised, as ApplyImport does
	REQUIRE( ImportSuggestedName( "///", existing ) == "Imported 2" );   // nothing usable left: the fallback
	REQUIRE( ImportSuggestedName( "", {} ) == "Imported" );

	// The field's refusal is the Create form's: free and sanitised.
	REQUIRE( ImportNameError( "Fresh", existing ).empty() );
	REQUIRE( !ImportNameError( "Comp", existing ).empty() );
	REQUIRE( !ImportNameError( "", existing ).empty() );
	REQUIRE( !ImportNameError( " Fresh", existing ).empty() );
}

TEST_CASE( "the Changes pane's lines are plain ASCII and map keys to row labels", "[overlay_profiles]" )
{
	pe::DiffRow r;
	r.group = "Motion blur";
	r.key = "motion_blur.samples";
	r.old_value = "4";
	r.new_value = "8";
	const auto Lookup = []( const std::string &s ) { return s == "motion_blur.samples" ? std::string( "Samples" ) : std::string(); };
	REQUIRE( DiffLabel( r.key, Lookup ) == "Samples" );
	REQUIRE( DiffLabel( "motion_blur.other", Lookup ) == "motion_blur.other" );   // no row: the key
	const std::string s = DiffLine( r, DiffLabel( r.key, Lookup ) );
	REQUIRE( s == "Motion blur / Samples: 4 -> 8" );
	for ( char c : s )
		REQUIRE( (unsigned char)c < 0x80 );

	// A long value is cut with "...", still ASCII.
	REQUIRE( ShortValue( "\"abcdefghijklmnopqrstuvwxyz0123456789\"", 12 ) == "\"abcdefgh..." );
	REQUIRE( ShortValue( "short" ) == "short" );

	REQUIRE( UnknownLine( 1 ) == "1 unknown value skipped" );
	REQUIRE( UnknownLine( 3 ) == "3 unknown values skipped" );
	REQUIRE( ChangesSummary( false, "Comp", 0 ) == "No settings change" );
	REQUIRE( ChangesSummary( false, "Comp", 1 ) == "1 setting changes" );
	REQUIRE( ChangesSummary( false, "Comp", 4 ) == "4 settings change" );
	REQUIRE( ChangesSummary( true, "Friend", 4 ) == "New profile 'Friend': 4 settings set" );
	REQUIRE( ImportBackupNote( "Comp" ) == "A backup of Comp is kept as Comp.json.bak" );
}

TEST_CASE( "the import toast names the destination, what was applied and what was skipped", "[overlay_profiles]" )
{
	REQUIRE( ImportToast( "Comp", false, 12, 0 ) == "Imported into 'Comp': 12 settings" );
	REQUIRE( ImportToast( "Comp", false, 1, 2 ) == "Imported into 'Comp': 1 setting, 2 skipped" );
	REQUIRE( ImportToast( "Friend", true, 5, 0 ) == "Imported as 'Friend': 5 settings" );
	REQUIRE( ImportToast( "Comp", false, 0, 0 ) == "Nothing changed in 'Comp'" );
}

TEST_CASE( "word wrap fits a monospace column count and never loses text", "[overlay_profiles]" )
{
	REQUIRE( WrapWords( "", 20 ) == std::vector<std::string>{ "" } );
	REQUIRE( WrapWords( "one two three", 20 ) == std::vector<std::string>{ "one two three" } );
	REQUIRE( WrapWords( "one two three four", 10 ) == std::vector<std::string>{ "one two", "three four" } );
	// Continuation lines carry the indent.
	REQUIRE( WrapWords( "alpha beta gamma", 10, "  " ) == std::vector<std::string>{ "alpha beta", "  gamma" } );
	// A word longer than a line is split, not dropped.
	const std::vector<std::string> v = WrapWords( "abcdefghijklmnopqrstuvwxyz", 10 );
	std::string sJoined;
	for ( const std::string &s : v )
	{
		REQUIRE( s.size() <= 10 );
		sJoined += s;
	}
	REQUIRE( sJoined == "abcdefghijklmnopqrstuvwxyz" );
}

TEST_CASE( "importing into the session profile reloads the panels and the values apply", "[overlay_profiles]" )
{
	TempConfigHome home;

	config::Settings base;
	base.gamescope.sharpness = 2;
	REQUIRE( config::CreateProfile( General( "Comp" ), &base ) );
	REQUIRE( config::SelectProfile( "Comp" ) );
	REQUIRE( config::SessionProfile() == "Comp" );

	// A friend's export: only sharpness, 9.
	const pe::ParsedExport p = pe::ParseExport( R"({"gamescope_ritz_export":1,"profile":"Friend","settings":{"gamescope":{"sharpness":9}}})" );
	REQUIRE( p.valid );
	const GroupSet ticks = ImportInitialTicks( pe::Groups(), p.groups_present );
	REQUIRE( ImportPrimaryEnabled( p.valid, ticks, false, false ) );

	const pe::DiffResult d = pe::DiffImport( p, "Comp", ticks );
	REQUIRE( d.count == 1 );
	REQUIRE( d.rows[ 0 ].key == "gamescope.sharpness" );

	uint64_t ulBefore = config::ConfigGeneration();
	const pe::ApplyResult r = pe::ApplyImport( p, "Comp", false, ticks );
	REQUIRE( r.success );
	REQUIRE( r.applied == 1 );
	// Every panel reloads on the bump, and what they reload is the new value.
	REQUIRE( config::ConfigGeneration() != ulBefore );
	REQUIRE( config::ResolvedSettings().gamescope.sharpness == 9 );
	// The backup of the old file is next to it.
	REQUIRE( std::filesystem::exists( config::ProfilePath( "Comp" ) + ".bak" ) );

	// Into a NEW profile: listed at once, the session untouched.
	ulBefore = config::ConfigGeneration();
	const pe::ApplyResult n = pe::ApplyImport( p, ImportSuggestedName( p.profile, config::ListProfiles() ), true, ticks );
	REQUIRE( n.success );
	REQUIRE( n.destination == "Friend" );
	REQUIRE( config::SessionProfile() == "Comp" );
	bool bListed = false;
	for ( const config::ProfileMeta &m : config::ListProfiles() )
		bListed = bListed || m.name == "Friend";
	REQUIRE( bListed );
}
