#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

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

    // Persistent-session topology (DECISIONS.md #21's "topology split"):
    // gamescope's process predates the game, so none of GS_RITZ_APPID/
    // STEAM_COMPAT_APP_ID/SteamAppId/STEAM_COMPAT_DATA_PATH were ever set on
    // it - this is indistinguishable from "nothing set" above, and that's
    // deliberate, not a gap: it must resolve to nothing, never to a stale or
    // wrong id. Named explicitly because it's the case decision 21 flagged
    // as unverified.
    SECTION( "persistent-session topology (no launch-time env vars) resolves to nothing, not a stale id" )
    {
        auto lookup = MakeLookup( {} );
        REQUIRE( ResolveAppId( lookup ) == std::nullopt );
    }

    // The dangerous case, guarded explicitly: "AppId=<n>" is a command-line
    // argument steamcompmgr.cpp's get_appid_from_pid() scrapes from a
    // "reaper" ancestor process's /proc/<pid>/cmdline, post-startup, per
    // window - it is NOT an environment variable, and never has been (see
    // superdoc/planning/appid-detection.md §3). If a future edit ever added
    // a bare "AppId" env-var lookup here (confusing the two), it would let a
    // leftover/unrelated "AppId" var from the launching shell silently
    // resolve to the wrong game. Assert it is never read.
    SECTION( "a bare \"AppId\" env var is never read - that name is the reaper's argv token, not an env var" )
    {
        auto lookup = MakeLookup( { { "AppId", "999999" } } );
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

TEST_CASE( "overlay.display_scale round-trips through SaveGlobal/LoadGlobal at every UI-reachable value", "[config]" )
{
    for ( float flValue : { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f } )
    {
        TempConfigHome home;

        Settings s{};
        s.overlay.display_scale = flValue;

        REQUIRE( SaveGlobal( s ) );

        Settings loaded = LoadGlobal();
        REQUIRE( loaded.overlay.display_scale == flValue );
    }
}

// Issue #70: fps_display.fps_enabled (the FPS module's own toggle,
// independent of the master `enabled`) round-trips through
// SaveGlobal/LoadGlobal, same pattern as #52's display_scale test above.
TEST_CASE( "fps_display.fps_enabled round-trips", "[config]" )
{
    for ( bool bValue : { true, false } )
    {
        TempConfigHome home;

        Settings s{};
        s.fps_display.fps_enabled = bValue;

        REQUIRE( SaveGlobal( s ) );

        Settings loaded = LoadGlobal();
        REQUIRE( loaded.fps_display.fps_enabled == bValue );
    }
}

// An old config on disk, saved before #70 added this key, must still load
// cleanly -- a missing key resolves to the compiled-in default (true,
// matching cpu_enabled/gpu_enabled/media_enabled's own default-on
// precedent), not a crash or a forced false.
TEST_CASE( "a config predating #70 loads fps_enabled at its compiled-in default", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << R"({"fps_display": {"enabled": true, "cpu_enabled": true}})";

    Settings loaded = LoadGlobal();
    REQUIRE( loaded.fps_display.enabled == true );
    REQUIRE( loaded.fps_display.fps_enabled == true );
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

// End-to-end pass for the launch-option-wrapper topology, using app id
// 3746030 - the id the user offered as this feature's test subject
// (DECISIONS.md #21). Closes the "not yet tested" flag: env var in, correct
// games/<id>.json read out, and no other app id's file is touched.
TEST_CASE( "app id 3746030 (launch-option-wrapper topology): env var resolves and reads its own games/<id>.json", "[config]" )
{
    TempConfigHome home;

    auto lookup = MakeLookup( { { "STEAM_COMPAT_APP_ID", "3746030" } } );
    std::optional<std::string> oAppId = ResolveAppId( lookup );
    REQUIRE( oAppId == "3746030" );
    REQUIRE( GamePath( *oAppId ) == GamesDir() + "/3746030.json" );

    Settings global{};
    global.gamescope.filter = "LINEAR";
    REQUIRE( SaveGlobal( global ) );

    // No override yet - falls through to global, and creates nothing.
    REQUIRE( ResolveEffective( oAppId ).gamescope.filter == "LINEAR" );
    REQUIRE_FALSE( std::filesystem::exists( GamePath( *oAppId ) ) );

    Settings snapshot = ResolveEffective( oAppId );
    snapshot.gamescope.filter = "FSR";
    REQUIRE( SnapshotPerGameOverride( *oAppId, snapshot ) );
    REQUIRE( std::filesystem::exists( GamePath( "3746030" ) ) );

    // Resolves from its own file now, and a different app id is unaffected.
    REQUIRE( ResolveEffective( oAppId ).gamescope.filter == "FSR" );
    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).gamescope.filter == "LINEAR" );
    REQUIRE_FALSE( std::filesystem::exists( GamePath( "1" ) ) );
}

TEST_CASE( "notification muting resolves per-game override vs. global exactly like every other setting", "[config]" )
{
    TempConfigHome home;

    Settings global{};
    global.notifications.muted = false;
    REQUIRE( SaveGlobal( global ) );

    // No override yet -- every app id (and no app id at all) reads the
    // global value.
    REQUIRE_FALSE( ResolveEffective( std::optional<std::string>{ "1" } ).notifications.muted );
    REQUIRE_FALSE( ResolveEffective( std::nullopt ).notifications.muted );

    // Enabling the override for app 1 with muted:true must not affect any
    // other app id or the global default itself (DECISIONS.md #19's full
    // snapshot, not a diff -- same mechanism NotificationSettings::muted
    // rides on as fps_display.enabled etc.).
    Settings snapshot = ResolveEffective( std::optional<std::string>{ "1" } );
    snapshot.notifications.muted = true;
    REQUIRE( SnapshotPerGameOverride( "1", snapshot ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).notifications.muted );
    REQUIRE_FALSE( ResolveEffective( std::optional<std::string>{ "2" } ).notifications.muted );
    REQUIRE_FALSE( ResolveEffective( std::nullopt ).notifications.muted );

    // Clearing the override falls back to global (still unmuted) again.
    REQUIRE( ClearPerGameOverride( "1" ) );
    REQUIRE_FALSE( ResolveEffective( std::optional<std::string>{ "1" } ).notifications.muted );
}

TEST_CASE( "notification placement is global-only and never rides along in a per-game snapshot", "[config]" )
{
    TempConfigHome home;

    Settings global{};
    global.overlay.notification_placement = "bottom-left";
    REQUIRE( SaveGlobal( global ) );
    REQUIRE( LoadGlobal().overlay.notification_placement == "bottom-left" );

    // Enabling "Override Global Config" for a game snapshots the full
    // effective settings (mirrors PanelConfig.cpp's EnableOverride) -- but
    // SettingsToJson's bIncludeOverlay=false for per-game files means the
    // `overlay` object (and therefore notification_placement) is never
    // written there at all, by design (ConfigSchema.h's OverlaySettings
    // comment, DECISIONS.md #25).
    Settings snapshot = ResolveEffective( std::nullopt );
    REQUIRE( SnapshotPerGameOverride( "9", snapshot ) );

    std::optional<Settings> oPerGame = LoadPerGameOverride( "9" );
    REQUIRE( oPerGame.has_value() );
    // Never present in the per-game file -> resolves back to the
    // compiled-in default, NOT the real global value -- proving placement
    // genuinely isn't per-game-eligible the way notifications.muted is.
    REQUIRE( oPerGame->overlay.notification_placement == "top-right" );

    // The real global placement is still reachable via LoadGlobal()
    // directly, regardless of any game's override state -- this is the
    // read path Notifications.cpp's own EnsureConfigLoaded() relies on.
    REQUIRE( LoadGlobal().overlay.notification_placement == "bottom-left" );

    // Changing global placement afterward must not require touching the
    // per-game file at all -- it was never in there to begin with.
    Settings changedGlobal = global;
    changedGlobal.overlay.notification_placement = "top-center";
    REQUIRE( SaveGlobal( changedGlobal ) );
    REQUIRE( LoadGlobal().overlay.notification_placement == "top-center" );
}

TEST_CASE( "audio manual node selection resolves per-game override vs. global exactly like every other setting", "[config]" )
{
    TempConfigHome home;

    Settings global{};
    global.audio.manual_node_binary = "";
    REQUIRE( SaveGlobal( global ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).audio.manual_node_binary.empty() );

    // Picking a manual stream for app 1 (PanelAudio.cpp's "Use this
    // stream" button) must not affect any other app id or the global
    // default - same full-snapshot mechanism as every other per-game
    // field (DECISIONS.md #19).
    Settings snapshot = ResolveEffective( std::optional<std::string>{ "1" } );
    snapshot.audio.manual_node_binary = "game.exe";
    REQUIRE( SnapshotPerGameOverride( "1", snapshot ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).audio.manual_node_binary == "game.exe" );
    REQUIRE( ResolveEffective( std::optional<std::string>{ "2" } ).audio.manual_node_binary.empty() );
    REQUIRE( ResolveEffective( std::nullopt ).audio.manual_node_binary.empty() );

    // Clearing the override (PanelAudio.cpp's "Clear manual override")
    // falls back to global (still empty, i.e. automatic detection) again.
    REQUIRE( ClearPerGameOverride( "1" ) );
    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).audio.manual_node_binary.empty() );
}

TEST_CASE( "ApplyProfile never carries a manual audio node selection into another game", "[config]" )
{
    TempConfigHome home;

    // A profile saved while some game's manual override happened to be set
    // (Settings is a full snapshot - PanelConfig.cpp's "Save as new
    // profile" has no reason to strip it out).
    Settings profile{};
    profile.gamescope.filter = "FSR";
    profile.audio.manual_node_binary = "specific-game.exe";
    REQUIRE( SaveProfile( "FPS", profile ) );

    // Applying that profile to a different game's settings must not point
    // its volume control at "specific-game.exe" - naming one game's
    // process has no meaning for another game the profile gets applied to.
    Settings target{};
    target.audio.manual_node_binary = "other-game.exe";
    REQUIRE( ApplyProfile( target, "FPS" ) );
    REQUIRE( target.gamescope.filter == "FSR" ); // the rest of the profile did apply
    REQUIRE( target.audio.manual_node_binary == "other-game.exe" ); // untouched
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
    profile.notifications.muted = true;
    REQUIRE( SaveProfile( "FPS", profile ) );

    Settings target{};
    target.gamescope.filter = "LINEAR";
    REQUIRE( ApplyProfile( target, "FPS" ) );
    REQUIRE( target.gamescope.filter == "FSR" );
    REQUIRE( target.gamescope.sharpness == 15 );
    REQUIRE( target.notifications.muted == true ); // NotificationSettings::muted is a normal per-layer field, copied like fps_display/reshade above

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

TEST_CASE( "a General-tab overlay edit is not clobbered by a later routed write from a stale panel cache", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr );

    // Simulates PanelDisplay.cpp's EnsureConfigLoaded(): loads the full
    // effective Settings once, at panel-open time, before the user ever
    // touches the General tab.
    Settings displayPanelCache = ResolveEffective( SessionAppId() );

    // User edits a General-tab slider: PanelConfig.cpp's QueueGeneralSave()
    // mutates just the overlay field on its own cache and writes the whole
    // struct.
    Settings generalTabCache = LoadGlobal();
    generalTabCache.overlay.dock_scale = 1.3f;
    EnqueueGlobalWrite( generalTabCache );
    FlushPendingWrites();
    REQUIRE( LoadGlobal().overlay.dock_scale == 1.3f );

    // User now edits a Display-tab slider. PanelDisplay never reloaded its
    // cache (only PanelConfig-driven profile-apply/override-toggle bump
    // ConfigGeneration - a General-tab edit deliberately never does), so its
    // own `overlay` sub-object is still whatever was loaded at panel-open
    // time (dock_scale == 1.0, the default) - before the General-tab edit.
    displayPanelCache.gamescope.filter = "FSR";
    EnqueueRoutedWrite( displayPanelCache );
    FlushPendingWrites();

    // The General tab's dock_scale must survive an unrelated Display-tab
    // edit.
    REQUIRE( LoadGlobal().overlay.dock_scale == 1.3f );
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

TEST_CASE( "loading sweeps this config's own stale atomic-write temp files, but never a live writer's", "[config]" )
{
    // Covers #21: WriteFileAtomic's write-temp-then-rename can be
    // interrupted (process killed) between the temp write and the rename,
    // orphaning a "<path>.tmp-<pid>" file that nothing used to clean up.
    TempConfigHome home;

    Settings s{};
    s.gamescope.filter = "FSR";
    REQUIRE( SaveGlobal( s ) );

    std::string sGlobalPath = GlobalConfigPath();

    // A dead pid: fork a child that exits immediately and reap it, so its
    // pid is guaranteed to no longer be running by the time we use it -
    // this is what the sweep must remove.
    pid_t deadPid = fork();
    REQUIRE( deadPid >= 0 );
    if ( deadPid == 0 )
        _exit( 0 );
    int status = 0;
    REQUIRE( waitpid( deadPid, &status, 0 ) == deadPid );

    std::string sStaleTemp = sGlobalPath + ".tmp-" + std::to_string( (long)deadPid );
    std::string sLiveTemp = sGlobalPath + ".tmp-" + std::to_string( (long)getpid() );

    {
        std::ofstream stale( sStaleTemp );
        stale << "{}";
    }
    {
        std::ofstream live( sLiveTemp );
        live << "{}";
    }
    REQUIRE( std::filesystem::exists( sStaleTemp ) );
    REQUIRE( std::filesystem::exists( sLiveTemp ) );

    // LoadGlobal() sweeps global.json's own temp files as a side effect.
    LoadGlobal();

    // The dead pid's temp file is orphaned litter - gone.
    REQUIRE_FALSE( std::filesystem::exists( sStaleTemp ) );
    // This process's own pid is alive (it's us), so a temp file "owned" by
    // it must never be touched - it could belong to a write in flight.
    REQUIRE( std::filesystem::exists( sLiveTemp ) );

    // The real config file itself must be untouched by the sweep.
    REQUIRE( std::filesystem::exists( sGlobalPath ) );
    REQUIRE( LoadGlobal().gamescope.filter == "FSR" );

    std::filesystem::remove( sLiveTemp );
}

// ---- issue #43: disabling an override must never delete the config file ----
// (DECISIONS.md #19's amendment.)

TEST_CASE( "disabling override deactivates the file in place - it is never deleted", "[config]" )
{
    TempConfigHome home;

    Settings snapshot{};
    snapshot.gamescope.filter = "FSR";
    REQUIRE( SnapshotPerGameOverride( "1", snapshot ) );
    REQUIRE( std::filesystem::exists( GamePath( "1" ) ) );

    // This is the exact call PanelConfig.cpp's DisableOverride() makes.
    REQUIRE( ClearPerGameOverride( "1" ) );

    // The file must still be on disk - the whole point of this fix.
    REQUIRE( std::filesystem::exists( GamePath( "1" ) ) );

    // But it must no longer be authoritative: resolution falls back to
    // global, exactly as if the file had been deleted.
    Settings global{};
    global.gamescope.filter = "LINEAR";
    REQUIRE( SaveGlobal( global ) );
    REQUIRE( ResolveEffective( std::optional<std::string>{ "1" } ).gamescope.filter == "LINEAR" );
    REQUIRE_FALSE( LoadPerGameOverride( "1" ).has_value() );

    // And the saved values are still readable via the "is there something
    // to restore" path.
    REQUIRE( HasSavedPerGameConfig( "1" ) );
}

TEST_CASE( "re-enabling override after a disable restores the saved file - it is never re-snapshotted", "[config]" )
{
    TempConfigHome home;

    // Global starts as "LINEAR" ...
    Settings global{};
    global.gamescope.filter = "LINEAR";
    REQUIRE( SaveGlobal( global ) );

    // ... but this game's saved override is "FSR" (what the user actually
    // wants for this game).
    Settings snapshot{};
    snapshot.gamescope.filter = "FSR";
    snapshot.gamescope.sharpness = 7;
    REQUIRE( SnapshotPerGameOverride( "1", snapshot ) );
    REQUIRE( ClearPerGameOverride( "1" ) ); // user turns override off

    // Global changes again while the override is off, the way it would in
    // real use between the disable and the re-enable.
    Settings changedGlobal{};
    changedGlobal.gamescope.filter = "NIS";
    REQUIRE( SaveGlobal( changedGlobal ) );

    // Re-enabling: PanelConfig.cpp's EnableOverride() calls
    // HasSavedPerGameConfig()/RestorePerGameOverride() first, and only
    // falls back to a fresh snapshot when that fails.
    REQUIRE( HasSavedPerGameConfig( "1" ) );
    REQUIRE( RestorePerGameOverride( "1" ) );

    // The restored file must have the ORIGINAL per-game values ("FSR"/7),
    // never the current global ("NIS") - re-snapshotting here would be the
    // exact same data loss issue #43 was about, one step later.
    Settings restored = ResolveEffective( std::optional<std::string>{ "1" } );
    REQUIRE( restored.gamescope.filter == "FSR" );
    REQUIRE( restored.gamescope.sharpness == 7 );
    REQUIRE( LoadPerGameOverride( "1" ).has_value() );
}

TEST_CASE( "enabling override with no existing file still snapshots from the currently effective settings", "[config]" )
{
    TempConfigHome home;

    Settings global{};
    global.gamescope.filter = "NIS";
    REQUIRE( SaveGlobal( global ) );

    // No games/2.json exists yet at all.
    REQUIRE_FALSE( HasSavedPerGameConfig( "2" ) );
    REQUIRE_FALSE( RestorePerGameOverride( "2" ) ); // nothing to restore

    // EnableOverride()'s fallback path: snapshot whatever is currently
    // effective.
    Settings snapshot = ResolveEffective( std::optional<std::string>{ "2" } );
    REQUIRE( SnapshotPerGameOverride( "2", snapshot ) );

    REQUIRE( ResolveEffective( std::optional<std::string>{ "2" } ).gamescope.filter == "NIS" );
    REQUIRE( LoadPerGameOverride( "2" ).has_value() );
}

TEST_CASE( "DeletePerGameOverride removes only the intended file", "[config]" )
{
    TempConfigHome home;

    REQUIRE( SnapshotPerGameOverride( "1", Settings{} ) );
    REQUIRE( SnapshotPerGameOverride( "2", Settings{} ) );
    Settings global{};
    global.gamescope.filter = "FSR";
    REQUIRE( SaveGlobal( global ) );

    REQUIRE( std::filesystem::exists( GamePath( "1" ) ) );
    REQUIRE( std::filesystem::exists( GamePath( "2" ) ) );
    REQUIRE( std::filesystem::exists( GlobalConfigPath() ) );

    REQUIRE( DeletePerGameOverride( "1" ) );

    // Only app id 1's file is gone.
    REQUIRE_FALSE( std::filesystem::exists( GamePath( "1" ) ) );
    REQUIRE_FALSE( HasSavedPerGameConfig( "1" ) );
    // App id 2's file, and global.json, are untouched.
    REQUIRE( std::filesystem::exists( GamePath( "2" ) ) );
    REQUIRE( std::filesystem::exists( GlobalConfigPath() ) );
    REQUIRE( LoadGlobal().gamescope.filter == "FSR" );

    // Deleting an already-absent file is still success (matches
    // ClearPerGameOverride's old missing-file-is-success contract).
    REQUIRE( DeletePerGameOverride( "1" ) );
}

TEST_CASE( "DeletePerGameOverride refuses a path-escaping app id", "[config]" )
{
    TempConfigHome home;

    REQUIRE( SnapshotPerGameOverride( "1", Settings{} ) );
    Settings global{};
    global.gamescope.filter = "FSR";
    REQUIRE( SaveGlobal( global ) );
    REQUIRE( SaveProfile( "MyProfile", Settings{} ) );

    // None of these should ever be able to reach outside GamesDir().
    REQUIRE_FALSE( DeletePerGameOverride( "../global" ) );
    REQUIRE_FALSE( DeletePerGameOverride( "../profiles/MyProfile" ) );
    REQUIRE_FALSE( DeletePerGameOverride( "." ) );
    REQUIRE_FALSE( DeletePerGameOverride( ".." ) );
    REQUIRE_FALSE( DeletePerGameOverride( "" ) );

    // Untouched.
    REQUIRE( std::filesystem::exists( GlobalConfigPath() ) );
    REQUIRE( std::filesystem::exists( ProfilePath( "MyProfile" ) ) );
    REQUIRE( std::filesystem::exists( GamePath( "1" ) ) );
    REQUIRE( LoadGlobal().gamescope.filter == "FSR" );
}

// ---- Issue #35: per-panel window geometry persistence ----------------

TEST_CASE( "panel geometry round-trips through SaveGlobal/LoadGlobal, and is excluded from a per-game snapshot", "[config]" )
{
    TempConfigHome home;

    Settings global{};
    global.overlay.panel_geometry[ "display" ] = PanelGeometry{ 120.0f, 80.0f, 500.0f, 360.0f };
    global.overlay.panel_geometry[ "audio" ] = PanelGeometry{ 900.0f, 40.0f, 420.0f, 220.0f };
    REQUIRE( SaveGlobal( global ) );

    Settings loaded = LoadGlobal();
    REQUIRE( loaded.overlay.panel_geometry.size() == 2 );
    REQUIRE( loaded.overlay.panel_geometry.at( "display" ).x == 120.0f );
    REQUIRE( loaded.overlay.panel_geometry.at( "display" ).y == 80.0f );
    REQUIRE( loaded.overlay.panel_geometry.at( "display" ).w == 500.0f );
    REQUIRE( loaded.overlay.panel_geometry.at( "display" ).h == 360.0f );
    REQUIRE( loaded.overlay.panel_geometry.at( "audio" ).w == 420.0f );

    // Process-level UI preference, same "global.json only" rule as
    // notification_placement/fade_ms (ConfigSchema.h's OverlaySettings
    // comment) - a window's screen position is about the player's physical
    // display, not the game running, so it must never ride along in a
    // per-game snapshot.
    Settings snapshot = ResolveEffective( std::nullopt );
    REQUIRE( SnapshotPerGameOverride( "41", snapshot ) );
    std::optional<Settings> oPerGame = LoadPerGameOverride( "41" );
    REQUIRE( oPerGame.has_value() );
    REQUIRE( oPerGame->overlay.panel_geometry.empty() );
}

TEST_CASE( "an unrecognized panel_geometry key in an old config is ignored, not fatal", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    // "fps" is the pre-issue-#27 key for the panel Chrome.h's PanelId enum
    // now calls SystemMonitor (Fps -> SystemMonitor rename, Chrome.h's own
    // comment) - an old config on disk can still carry it under a build
    // that no longer has a case for it in Chrome.cpp's PanelKey(). It must
    // parse harmlessly alongside a normal, currently-recognized entry, and
    // must not disturb an unrelated section of the same file.
    std::ofstream( GlobalConfigPath() ) << R"({
        "schema_version": 1,
        "gamescope": { "filter": "FSR" },
        "overlay": {
            "panel_geometry": {
                "fps": { "x": 10.0, "y": 10.0, "w": 300.0, "h": 200.0 },
                "system_monitor": { "x": 50.0, "y": 60.0, "w": 480.0, "h": 300.0 }
            }
        }
    })";

    Settings s = LoadGlobal();
    REQUIRE( s.gamescope.filter == "FSR" ); // unrelated section untouched
    REQUIRE( s.overlay.panel_geometry.count( "fps" ) == 1 ); // parsed, not dropped
    REQUIRE( s.overlay.panel_geometry.at( "system_monitor" ).w == 480.0f );
}

TEST_CASE( "a malformed single panel_geometry entry is skipped, not the whole map or file", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << R"({
        "schema_version": 1,
        "gamescope": { "filter": "NIS" },
        "overlay": {
            "panel_geometry": {
                "shaders": "not an object",
                "audio": { "x": 5.0, "y": 5.0, "w": -10.0, "h": 200.0 },
                "config": { "x": 15.0, "y": 25.0, "w": 400.0, "h": 240.0 }
            }
        }
    })";

    Settings s = LoadGlobal();
    REQUIRE( s.gamescope.filter == "NIS" );          // unrelated section untouched
    REQUIRE( s.overlay.panel_geometry.count( "shaders" ) == 0 ); // not an object
    REQUIRE( s.overlay.panel_geometry.count( "audio" ) == 0 );   // non-positive width
    REQUIRE( s.overlay.panel_geometry.at( "config" ).w == 400.0f ); // the one valid entry survives
}

TEST_CASE( "EnqueueGeometryWrite saves one panel's geometry without clobbering an unrelated concurrent write", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr );

    // Simulates PanelDisplay.cpp writing a GAMESCOPE-tab change first
    // (EnqueueRoutedWrite -> EnqueueGlobalWrite for the no-override case).
    Settings displayEdit = LoadGlobal();
    displayEdit.gamescope.filter = "FSR";
    EnqueueGlobalWrite( displayEdit );
    FlushPendingWrites();
    REQUIRE( LoadGlobal().gamescope.filter == "FSR" );

    // Chrome.cpp's own geometry autosave never loads or holds gamescope.*
    // at all - it must not revert the Display-tab edit above just because
    // it only means to save one panel's position (EnqueueGeometryWrite's
    // own comment: merges onto CurrentOverlaySettings()/
    // CurrentFullSettings() rather than a caller-supplied whole struct).
    PanelGeometry geom{ 200.0f, 150.0f, 440.0f, 300.0f };
    EnqueueGeometryWrite( "audio", geom );
    FlushPendingWrites();

    Settings after = LoadGlobal();
    REQUIRE( after.gamescope.filter == "FSR" ); // survived the geometry write
    REQUIRE( after.overlay.panel_geometry.at( "audio" ).x == 200.0f );
    REQUIRE( after.overlay.panel_geometry.at( "audio" ).w == 440.0f );

    // And the reverse direction: a later General-tab edit (PanelConfig.cpp's
    // QueueGeneralSave(), which always starts from a fresh LoadGlobal())
    // must not lose the just-saved geometry either.
    Settings generalEdit = LoadGlobal();
    generalEdit.overlay.dock_scale = 1.2f;
    EnqueueGlobalWrite( generalEdit );
    FlushPendingWrites();

    Settings final_ = LoadGlobal();
    REQUIRE( final_.overlay.dock_scale == 1.2f );
    REQUIRE( final_.overlay.panel_geometry.at( "audio" ).w == 440.0f );
}
