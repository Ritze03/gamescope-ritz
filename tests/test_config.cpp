#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "Config/AppId.h"
#include "Config/ConfigManager.h"

using namespace gamescope::config;

namespace
{
    // Points XDG_CONFIG_HOME at a fresh, unique temp directory for the
    // lifetime of this object, so every test runs against its own throwaway
    // config root and never touches a real ~/.config/gamescope-ritz - per the
    // milestone's requirement to never leave files in the user's real config
    // directory. Removes the directory tree on destruction.
    struct TempConfigHome
    {
        std::filesystem::path dir;

        TempConfigHome()
        {
            std::filesystem::path base = std::filesystem::temp_directory_path() /
                ( "gamescope-ritz-test-XXXXXX" );
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

    EnvLookupFn MakeLookup( std::map<std::string, std::string> env )
    {
        return [env = std::move( env )]( const char *pszName ) -> const char * {
            auto it = env.find( pszName );
            return it == env.end() ? nullptr : it->second.c_str();
        };
    }
}

TEST_CASE( "ResolveAppId precedence", "[config]" )
{
    SECTION( "GS_RITZ_APPID always wins" )
    {
        auto lookup = MakeLookup( {
            { "GS_RITZ_APPID", "42" },
            { "STEAM_COMPAT_APP_ID", "100" },
            { "SteamAppId", "200" },
        } );
        REQUIRE( ResolveAppId( lookup ) == "42" );
    }

    SECTION( "STEAM_COMPAT_APP_ID wins over SteamAppId" )
    {
        auto lookup = MakeLookup( {
            { "STEAM_COMPAT_APP_ID", "100" },
            { "SteamAppId", "200" },
        } );
        REQUIRE( ResolveAppId( lookup ) == "100" );
    }

    SECTION( "SteamAppId used when nonzero" )
    {
        auto lookup = MakeLookup( { { "SteamAppId", "3746030" } } );
        REQUIRE( ResolveAppId( lookup ) == "3746030" );
    }

    SECTION( "literal SteamAppId=0 is treated as absent, falls through to STEAM_COMPAT_DATA_PATH" )
    {
        auto lookup = MakeLookup( {
            { "SteamAppId", "0" },
            { "STEAM_COMPAT_DATA_PATH", "/home/user/.steam/steam/steamapps/compatdata/731" },
        } );
        REQUIRE( ResolveAppId( lookup ) == "731" );
    }

    SECTION( "SteamAppId=0 with no compat data path resolves to nothing" )
    {
        auto lookup = MakeLookup( { { "SteamAppId", "0" } } );
        REQUIRE( ResolveAppId( lookup ) == std::nullopt );
    }

    SECTION( "nothing set resolves to nothing" )
    {
        auto lookup = MakeLookup( {} );
        REQUIRE( ResolveAppId( lookup ) == std::nullopt );
    }
}

TEST_CASE( "SanitizeProfileName rejects path escapes", "[config]" )
{
    REQUIRE( SanitizeProfileName( "FPS" ) == "FPS" );
    REQUIRE( SanitizeProfileName( "My Profile-1" ) == "My Profile-1" );

    // '/' and '.' are entirely outside the allowlist, so both traversal
    // attempts collapse to something that can never leave profiles/.
    REQUIRE( SanitizeProfileName( "../../etc/passwd" ) == "etcpasswd" );
    REQUIRE( SanitizeProfileName( "/etc/passwd" ) == "etcpasswd" );
    REQUIRE( SanitizeProfileName( ".." ) == std::nullopt );
    REQUIRE( SanitizeProfileName( "." ) == std::nullopt );
    REQUIRE( SanitizeProfileName( "" ) == std::nullopt );
    REQUIRE( SanitizeProfileName( "   " ) == std::nullopt );
    REQUIRE( SanitizeProfileName( "  Trimmed  " ) == "Trimmed" );
}

TEST_CASE( "malformed JSON falls back to defaults instead of crashing", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << "{ not valid json !!";

    Settings s = LoadGlobal();
    REQUIRE( s.gamescope.filter == "LINEAR" ); // compiled-in default, not a crash
}

TEST_CASE( "a schema_version newer than this build refuses to guess", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << R"({"schema_version": 999, "gamescope": {"filter": "FSR"}})";

    Settings s = LoadGlobal();
    REQUIRE( s.gamescope.filter == "LINEAR" ); // rejected wholesale, not partially trusted
}

TEST_CASE( "SaveGlobal / LoadGlobal round-trip atomically", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.gamescope.filter = "FSR";
    s.gamescope.sharpness = 12;
    s.gamescope.vrr_enabled = true;
    s.fps_display.enabled = true;
    s.fps_display.font_size = 24.0f;

    REQUIRE( SaveGlobal( s ) );
    REQUIRE( std::filesystem::exists( GlobalConfigPath() ) );

    Settings loaded = LoadGlobal();
    REQUIRE( loaded.gamescope.filter == "FSR" );
    REQUIRE( loaded.gamescope.sharpness == 12 );
    REQUIRE( loaded.gamescope.vrr_enabled == true );
    REQUIRE( loaded.fps_display.enabled == true );
    REQUIRE( loaded.fps_display.font_size == 24.0f );
}

TEST_CASE( "no per-game file is ever created until override is enabled", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.gamescope.filter = "FSR";
    REQUIRE( SaveGlobal( s ) );

    // Global-only resolution never looks at, and never creates, games/<id>.json.
    Settings effective = ResolveEffective( std::optional<std::string>{ "1" } );
    REQUIRE( effective.gamescope.filter == "FSR" );
    REQUIRE_FALSE( std::filesystem::exists( GamePath( "1" ) ) );
}

TEST_CASE( "override_global snapshot wins over global, and is a frozen snapshot", "[config]" )
{
    TempConfigHome home;

    Settings global{};
    global.gamescope.filter = "LINEAR";
    REQUIRE( SaveGlobal( global ) );

    // Turning "Override Global Config" on for app id 1 captures the
    // *current* effective settings as a full snapshot.
    Settings snapshot = ResolveEffective( std::nullopt );
    snapshot.gamescope.filter = "FSR";
    REQUIRE( SnapshotPerGameOverride( "1", snapshot ) );
    REQUIRE( std::filesystem::exists( GamePath( "1" ) ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).gamescope.filter == "FSR" );

    // A later global-only change must not reach the already-snapshotted game -
    // it's a snapshot, not a live reference (DECISIONS.md #19).
    Settings changedGlobal{};
    changedGlobal.gamescope.filter = "NIS";
    REQUIRE( SaveGlobal( changedGlobal ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).gamescope.filter == "FSR" );
    REQUIRE( ResolveEffective( std::nullopt ).gamescope.filter == "NIS" );

    // Clearing the override falls back to (the now-changed) global again.
    REQUIRE( ClearPerGameOverride( "1" ) );
    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).gamescope.filter == "NIS" );
}

TEST_CASE( "a per-game file with override_global: false behaves as absent", "[config]" )
{
    TempConfigHome home;

    Settings global{};
    global.gamescope.filter = "LINEAR";
    REQUIRE( SaveGlobal( global ) );

    std::filesystem::create_directories( GamesDir() );
    std::ofstream( GamePath( "5" ) ) << R"({"schema_version": 1, "override_global": false, "gamescope": {"filter": "FSR"}})";

    REQUIRE( ResolveEffective( std::optional<std::string>{ "5" } ).gamescope.filter == "LINEAR" );
}

TEST_CASE( "ApplyProfile copies values in once, not a live reference", "[config]" )
{
    TempConfigHome home;

    Settings profile{};
    profile.gamescope.filter = "FSR";
    profile.gamescope.sharpness = 15;
    REQUIRE( SaveProfile( "FPS", profile ) );

    Settings target{};
    target.gamescope.filter = "LINEAR";
    REQUIRE( ApplyProfile( target, "FPS" ) );
    REQUIRE( target.gamescope.filter == "FSR" );
    REQUIRE( target.gamescope.sharpness == 15 );

    // Editing the profile afterwards must not retroactively change `target`
    // (DECISIONS.md #20 - a one-time copy, not a live reference).
    Settings editedProfile{};
    editedProfile.gamescope.filter = "NIS";
    REQUIRE( SaveProfile( "FPS", editedProfile ) );

    REQUIRE( target.gamescope.filter == "FSR" );
}

TEST_CASE( "queued writes flush to disk without blocking the caller inline", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.gamescope.filter = "PIXEL";
    EnqueueGlobalWrite( s );
    FlushPendingWrites();

    REQUIRE( LoadGlobal().gamescope.filter == "PIXEL" );
}

namespace
{
    // Points GS_RITZ_APPID at `sAppId` (highest-precedence env var,
    // ResolveAppId's decision 21 order) for the lifetime of the object, and
    // resets ConfigManager's session-routing cache (SessionAppId/
    // IsSessionOverrideActive/ConfigGeneration) on both construction and
    // destruction - production never re-resolves an app id mid-process, but
    // catch2 runs every [config]-tagged TEST_CASE in one shared process, so
    // each M7 routing test needs its own clean session identity.
    struct ScopedSessionAppId
    {
        explicit ScopedSessionAppId( const char *pszAppId )
        {
            ResetSessionRoutingForTests();
            if ( pszAppId )
                setenv( "GS_RITZ_APPID", pszAppId, 1 );
            else
                unsetenv( "GS_RITZ_APPID" );
        }

        ~ScopedSessionAppId()
        {
            unsetenv( "GS_RITZ_APPID" );
            ResetSessionRoutingForTests();
        }
    };
}

TEST_CASE( "EnqueueRoutedWrite goes to global.json until override is active", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "77" );

    REQUIRE_FALSE( IsSessionOverrideActive() ); // no games/77.json on disk yet

    Settings s{};
    s.gamescope.filter = "FSR";
    EnqueueRoutedWrite( s );
    FlushPendingWrites();

    REQUIRE( LoadGlobal().gamescope.filter == "FSR" );
    REQUIRE_FALSE( std::filesystem::exists( GamePath( "77" ) ) ); // M7's core "no speculative file" requirement
}

TEST_CASE( "EnqueueRoutedWrite switches to the per-game file once override is active, and stays a frozen snapshot", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "77" );

    Settings global{};
    global.gamescope.filter = "LINEAR";
    REQUIRE( SaveGlobal( global ) );

    // Mirrors PanelConfig.cpp's EnableOverride(): snapshot the currently
    // effective settings, then flip the routing flag.
    Settings snapshot = ResolveEffective( SessionAppId() );
    EnqueuePerGameSnapshot( "77", snapshot );
    SetSessionOverrideActive( true );
    BumpConfigGeneration();
    FlushPendingWrites();
    REQUIRE( std::filesystem::exists( GamePath( "77" ) ) );

    // Every further routed write from any panel now lands in games/77.json,
    // never global.json.
    Settings edited{};
    edited.gamescope.filter = "NIS";
    EnqueueRoutedWrite( edited );
    FlushPendingWrites();
    REQUIRE( ResolveEffective( std::optional<std::string>{ "77" } ).gamescope.filter == "NIS" );

    // A later change to global.json itself (as if a different, non-
    // overridden game edited it) must not reach app 77 - it's a snapshot,
    // not a live reference (DECISIONS.md #19), and the routing wiring must
    // not accidentally reintroduce a merge.
    Settings changedGlobal{};
    changedGlobal.gamescope.filter = "PIXEL";
    REQUIRE( SaveGlobal( changedGlobal ) );
    REQUIRE( ResolveEffective( std::optional<std::string>{ "77" } ).gamescope.filter == "NIS" );
}

TEST_CASE( "EnqueueRoutedWrite falls back to global.json when no app id was resolved", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr ); // no GS_RITZ_APPID/Steam env vars at all

    REQUIRE( SessionAppId() == std::nullopt );
    // Even a stray SetSessionOverrideActive(true) (shouldn't happen -
    // PanelConfig.cpp's checkbox is disabled with no app id - but defend
    // against it anyway) must not create a per-game file with no app id to
    // name it after.
    SetSessionOverrideActive( true );

    Settings s{};
    s.gamescope.filter = "FSR";
    EnqueueRoutedWrite( s );
    FlushPendingWrites();

    REQUIRE( LoadGlobal().gamescope.filter == "FSR" );
}

TEST_CASE( "ListProfiles and ListGameIds report what's actually on disk, sorted", "[config]" )
{
    TempConfigHome home;

    REQUIRE( ListProfiles().empty() ); // no profiles/ directory yet at all
    REQUIRE( ListGameIds().empty() );  // no games/ directory yet at all

    REQUIRE( SaveProfile( "Casual", Settings{} ) );
    REQUIRE( SaveProfile( "FPS", Settings{} ) );
    REQUIRE( SnapshotPerGameOverride( "200", Settings{} ) );
    REQUIRE( SnapshotPerGameOverride( "10", Settings{} ) );

    std::vector<std::string> profiles = ListProfiles();
    REQUIRE( profiles == std::vector<std::string>{ "Casual", "FPS" } );

    std::vector<std::string> games = ListGameIds();
    REQUIRE( games == std::vector<std::string>{ "10", "200" } );
}

TEST_CASE( "copying another game's config is a one-time snapshot, not a link between the two games", "[config]" )
{
    TempConfigHome home;

    // App 10 already has its own overridden config.
    Settings source{};
    source.gamescope.filter = "FSR";
    source.gamescope.sharpness = 18;
    REQUIRE( SnapshotPerGameOverride( "10", source ) );

    // Mirrors PanelConfig.cpp's CopySelectedGameConfig(): read app 10's
    // config and snapshot it as app 20's own.
    std::optional<Settings> oSource = LoadPerGameOverride( "10" );
    REQUIRE( oSource.has_value() );
    REQUIRE( SnapshotPerGameOverride( "20", *oSource ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "20" } ).gamescope.filter == "FSR" );
    REQUIRE( ResolveEffective( std::optional<std::string>{ "20" } ).gamescope.sharpness == 18 );

    // Editing app 10's config afterwards must not retroactively change the
    // copy app 20 already took - copying is a snapshot, exactly like
    // enabling the override itself (DECISIONS.md #19) and applying a
    // profile (DECISIONS.md #20).
    Settings editedSource{};
    editedSource.gamescope.filter = "NIS";
    REQUIRE( SnapshotPerGameOverride( "10", editedSource ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "20" } ).gamescope.filter == "FSR" );
}
