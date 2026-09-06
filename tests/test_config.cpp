#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "Config/AppId.h"
#include "Config/ConfigManager.h"
#include "Overlay/FpsDisplay.h"

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
            // Profiles v2: the session cache, the global.json mirror and
            // the migration flag are process-wide; each test starts clean.
            ResetSessionRoutingForTests();
        }

        ~TempConfigHome()
        {
            FlushPendingWrites();
            std::error_code ec;
            std::filesystem::remove_all( dir, ec );
            unsetenv( "XDG_CONFIG_HOME" );
            ResetSessionRoutingForTests();
        }
    };

    // Profiles v2: the per-layer sections live in profile files, so a
    // section round-trip goes through one ("T"). Overlay round-trips still
    // use SaveGlobal/LoadGlobal, the file that carries `overlay`.
    bool SaveSections( const Settings &s )
    {
        ProfileMeta m;
        m.name = "T";
        return SaveProfile( m, s );
    }

    Settings LoadSections()
    {
        return LoadProfile( "T" ).value_or( Settings{} );
    }

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

TEST_CASE( "a profile file round-trips the per-layer sections atomically", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.gamescope.filter = "FSR";
    s.gamescope.sharpness = 12;
    s.gamescope.vrr_enabled = true;
    s.fps_display.enabled = true;
    s.fps_display.font_size = 24.0f;

    REQUIRE( SaveSections( s ) );
    REQUIRE( std::filesystem::exists( ProfilePath( "T" ) ) );

    Settings loaded = LoadSections();
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

// HUD Phase 2 (2026-09-03): round-trip tests for every field the rebuilt
// system.hud tab added -- same pattern as #70/#73's tests above.

TEST_CASE( "fps_display.update_mode round-trips", "[config]" )
{
    for ( const std::string &sValue : { std::string( "smoothing" ), std::string( "immediate" ) } )
    {
        TempConfigHome home;

        Settings s{};
        s.fps_display.update_mode = sValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.fps_display.update_mode == sValue );
        REQUIRE( gamescope::fpsmath::UpdateModeFromInt( gamescope::fpsmath::UpdateModeToInt( loaded.fps_display.update_mode ) ) == sValue );
    }
}

// 2026-09-05: "per_second" was subsumed by Smoothing. A config written while
// it existed must still load, and the stored value must resolve to the
// Smoothing choice rather than an unknown one. The string itself is
// preserved on disk (ConfigManager does no rewriting); the mapping is
// fpsmath::UpdateModeToInt's fallback rule.
TEST_CASE( "fps_display.update_mode legacy per_second loads as Smoothing", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << R"({"fps_display": {"update_mode": "per_second"}})";

    Settings loaded = ResolvedSettings();
    REQUIRE( loaded.fps_display.update_mode == "per_second" );
    REQUIRE( gamescope::fpsmath::UpdateModeToInt( loaded.fps_display.update_mode ) == 0 );
    REQUIRE( std::string( gamescope::fpsmath::UpdateModeFromInt( gamescope::fpsmath::UpdateModeToInt( loaded.fps_display.update_mode ) ) ) == "smoothing" );
}

TEST_CASE( "fps_display.hide_above_enabled and hide_above_fps round-trip", "[config]" )
{
    for ( bool bValue : { true, false } )
    {
        TempConfigHome home;

        Settings s{};
        s.fps_display.hide_above_enabled = bValue;
        s.fps_display.hide_above_fps = 90.0f;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.fps_display.hide_above_enabled == bValue );
        REQUIRE( loaded.fps_display.hide_above_fps == 90.0f );
    }
}

TEST_CASE( "fps_display.color_mode round-trips", "[config]" )
{
    for ( const std::string &sValue : { std::string( "fixed" ), std::string( "inverted" ) } )
    {
        TempConfigHome home;

        Settings s{};
        s.fps_display.color_mode = sValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.fps_display.color_mode == sValue );
    }
}

// Outline thickness in px, 0..4 (2026-09-03: it was briefly a 0..1 opacity
// the same day -- an old 0..1 value is still a valid thickness, so both
// halves of the range must survive a round trip).
TEST_CASE( "fps_display.outline_strength round-trips across the whole 0-4 px range", "[config]" )
{
    for ( float flValue : { 0.0f, 0.25f, 0.5f, 1.0f, 2.5f, 4.0f } )
    {
        TempConfigHome home;

        Settings s{};
        s.fps_display.outline_strength = flValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.fps_display.outline_strength == flValue );
    }
}

TEST_CASE( "fps_display.lag_detection_enabled round-trips", "[config]" )
{
    for ( bool bValue : { true, false } )
    {
        TempConfigHome home;

        Settings s{};
        s.fps_display.lag_detection_enabled = bValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.fps_display.lag_detection_enabled == bValue );
    }
}

// cursor_override_game (request #6, 2026-09-04): a separate opt-in from
// cursor_everywhere that substitutes the overlay's own pointer on the live
// compositing path (MouseCursor::getTexture(), steamcompmgr.cpp) instead of
// just the root-window fallback. Off by default -- see
// superdoc/features/cursor-pipeline.md.
TEST_CASE( "overlay.cursor_override_game round-trips", "[config]" )
{
    for ( bool bValue : { true, false } )
    {
        TempConfigHome home;

        Settings s{};
        s.overlay.cursor_override_game = bValue;

        REQUIRE( SaveGlobal( s ) );

        Settings loaded = LoadGlobal();
        REQUIRE( loaded.overlay.cursor_override_game == bValue );
    }
}

// The drop shadow the outline replaced (2026-09-03): an old config still
// carrying shadow_strength must load cleanly and simply take
// outline_strength's default, not error and not inherit the old value.
TEST_CASE( "a config carrying the removed shadow_strength falls back to the outline default", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << R"({"fps_display": {
        "enabled": true,
        "shadow_strength": 0.75
    }})";

    Settings loaded = ResolvedSettings();
    REQUIRE( loaded.fps_display.enabled == true );
    REQUIRE( loaded.fps_display.outline_strength == Settings{}.fps_display.outline_strength );
}

TEST_CASE( "fps_display.backdrop_opacity round-trips at every UI-reachable value", "[config]" )
{
    for ( float flValue : { 0.0f, 0.05f, 0.5f, 1.0f } )
    {
        TempConfigHome home;

        Settings s{};
        s.fps_display.backdrop_opacity = flValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.fps_display.backdrop_opacity == flValue );
    }
}

// Phase 2 removed backdrop_enabled/backdrop_rounding/blend_mode outright
// (ConfigSchema.h's own comment) rather than deprecating them -- an old
// config that still has those keys on disk must load cleanly, simply
// ignoring them, exactly like any other removed field this project has
// dropped (dock_scale, opacity_background, ...).
TEST_CASE( "a config predating Phase 2 ignores the removed backdrop/blend_mode keys", "[config]" )
{
    TempConfigHome home;

    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << R"({"fps_display": {
        "enabled": true,
        "backdrop_enabled": false,
        "backdrop_rounding": 4.0,
        "blend_mode": "additive"
    }})";

    Settings loaded = ResolvedSettings();
    REQUIRE( loaded.fps_display.enabled == true );
    // Compiled-in defaults for every Phase 2 field the old file never wrote.
    REQUIRE( loaded.fps_display.update_mode == "smoothing" );
    REQUIRE( loaded.fps_display.color_mode == "fixed" );
    REQUIRE( loaded.fps_display.hide_above_enabled == false );
    REQUIRE( loaded.fps_display.outline_strength == 0.0f );
    REQUIRE( loaded.fps_display.lag_detection_enabled == true );
}

TEST_CASE( "queued writes flush to disk without blocking the caller inline", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.overlay.display_scale = 1.35f;
    EnqueueGlobalWrite( s );
    FlushPendingWrites();

    REQUIRE( LoadGlobal().overlay.display_scale == 1.35f );
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

TEST_CASE( "an Appearance edit is never clobbered by a later routed write from a stale panel cache", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr );

    // Simulates PanelDisplay.cpp's EnsureConfigLoaded(): loads the full
    // resolved Settings once, at panel-open time, before the user ever
    // touches the Appearance area.
    Settings displayPanelCache = ResolvedSettings();

    // User edits an Appearance slider: PanelConfig.cpp's QueueGeneralSave()
    // mutates just the overlay field on its own cache and writes it.
    Settings generalTabCache = LoadGlobal();
    generalTabCache.overlay.display_scale = 1.3f;
    EnqueueGlobalWrite( generalTabCache );
    FlushPendingWrites();
    REQUIRE( LoadGlobal().overlay.display_scale == 1.3f );

    // User now edits a Display slider. PanelDisplay never reloaded its cache
    // (an Appearance edit deliberately never bumps ConfigGeneration), so its
    // own `overlay` sub-object is still the default. A routed write goes
    // into the session profile and never carries `overlay` at all.
    displayPanelCache.gamescope.filter = "FSR";
    EnqueueRoutedWrite( displayPanelCache );
    FlushPendingWrites();

    REQUIRE( LoadGlobal().overlay.display_scale == 1.3f );
    REQUIRE( ResolvedSettings().gamescope.filter == "FSR" );
    REQUIRE( ResolvedSettings().overlay.display_scale == 1.3f ); // the resolved struct is whole
}

TEST_CASE( "loading sweeps this config's own stale atomic-write temp files, but never a live writer's", "[config]" )
{
    // Covers #21: WriteFileAtomic's write-temp-then-rename can be
    // interrupted (process killed) between the temp write and the rename,
    // orphaning a "<path>.tmp-<pid>" file that nothing used to clean up.
    TempConfigHome home;

    Settings s{};
    s.overlay.display_scale = 1.4f;
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
    REQUIRE( LoadGlobal().overlay.display_scale == 1.4f );

    std::filesystem::remove( sLiveTemp );
}

// ---- issue #43: disabling an override must never delete the config file ----
// (DECISIONS.md #19's amendment.)

TEST_CASE( "panel geometry round-trips through SaveGlobal/LoadGlobal", "[config]" )
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
    REQUIRE( ResolvedSettings().gamescope.filter == "FSR" ); // unrelated section untouched (migrated into Default)
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
    REQUIRE( ResolvedSettings().gamescope.filter == "NIS" ); // unrelated section untouched (migrated into Default)
    REQUIRE( s.overlay.panel_geometry.count( "shaders" ) == 0 ); // not an object
    REQUIRE( s.overlay.panel_geometry.count( "audio" ) == 0 );   // non-positive width
    REQUIRE( s.overlay.panel_geometry.at( "config" ).w == 400.0f ); // the one valid entry survives
}

TEST_CASE( "EnqueueGeometryWrite saves one panel's geometry without clobbering an unrelated concurrent write", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr );

    // An Appearance edit lands first.
    Settings appearanceEdit = LoadGlobal();
    appearanceEdit.overlay.accent_hue = 123.0f;
    EnqueueGlobalWrite( appearanceEdit );
    FlushPendingWrites();
    REQUIRE( LoadGlobal().overlay.accent_hue == 123.0f );

    // Chrome.cpp's own geometry autosave never loads or holds the other
    // overlay fields - it must not revert the edit above just because it
    // only means to save one panel's position (EnqueueGeometryWrite's own
    // comment: patches the in-memory mirror rather than a caller-supplied
    // whole struct).
    PanelGeometry geom{ 200.0f, 150.0f, 440.0f, 300.0f };
    EnqueueGeometryWrite( "audio", geom );
    FlushPendingWrites();

    Settings after = LoadGlobal();
    REQUIRE( after.overlay.accent_hue == 123.0f ); // survived the geometry write
    REQUIRE( after.overlay.panel_geometry.at( "audio" ).x == 200.0f );
    REQUIRE( after.overlay.panel_geometry.at( "audio" ).w == 440.0f );

    // And the reverse direction: a later Appearance edit (PanelConfig.cpp's
    // QueueGeneralSave(), which starts from a fresh LoadGlobal()) must not
    // lose the just-saved geometry either.
    Settings generalEdit = LoadGlobal();
    generalEdit.overlay.display_scale = 1.2f;
    EnqueueGlobalWrite( generalEdit );
    FlushPendingWrites();

    Settings final_ = LoadGlobal();
    REQUIRE( final_.overlay.display_scale == 1.2f );
    REQUIRE( final_.overlay.panel_geometry.at( "audio" ).w == 440.0f );
}

// =========================================================================
//  An existing config must load with every value intact
// =========================================================================
// The user's two standing rules, and the ones this project has actually
// broken before: an existing config keeps loading with its values intact,
// and nothing deletes or rewrites one on its own. Profiles v2's migration is
// the one deliberate rewrite -- and it is asserted here to keep every value,
// with the old games/ files untouched (the migration tests below).
TEST_CASE( "a config written before the E2 rework loads with every value intact", "[config]" )
{
    TempConfigHome home;

    // A config as a user would already have it on disk -- every key the
    // Setup areas bind to, at a non-default value, so a field silently
    // reverting to its default is a failure rather than a coincidence.
    const std::string sGlobal = R"({
  "version": 1,
  "overlay": {
    "accent_hue": 291.0,
    "display_scale": 1.75,
    "dock_scale": 1.4,
    "notification_scale": 1.1,
    "opacity_windows_focused": 0.77,
    "opacity_windows_unfocused": 0.55,
    "opacity_dock": 0.66,
    "window_opacity": 0.6,
    "opacity_notifications": 0.88,
    "background_blur": 0.42,
    "background_darkening": 0.31,
    "notification_placement": "bottom-left"
  },
  "notifications": { "muted": true },
  "audio": { "manual_node_binary": "floorp" },
  "last_applied_profile": "Handheld 40 fps"
})";

    const std::filesystem::path pathGlobal = home.dir / "gamescope-ritz" / "global.json";
    std::filesystem::create_directories( pathGlobal.parent_path() );
    {
        std::ofstream f( pathGlobal );
        f << sGlobal;
    }

    Settings g = LoadGlobal();

    REQUIRE( g.overlay.accent_hue == 291.0f );
    REQUIRE( g.overlay.display_scale == 1.75f );
    // dock_scale is deliberately NOT asserted: it was removed 2026-08-24
    // with the dock. The key is still in the fixture above on purpose --
    // an old config carrying a removed key still loads.
    REQUIRE( g.overlay.notification_scale == 1.1f );
    // opacity_windows_focused/unfocused and opacity_dock are deliberately
    // NOT asserted: removed 2026-09-06 with the surfaces they targeted
    // (see ConfigSchema.h). The keys stay in the fixture above on purpose --
    // an old config carrying removed keys still loads.
    REQUIRE( g.overlay.window_opacity == 0.6f );
    REQUIRE( g.overlay.opacity_notifications == 0.88f );
    REQUIRE( g.overlay.background_blur == 0.42f );
    REQUIRE( g.overlay.background_darkening == 0.31f );
    REQUIRE( g.overlay.notification_placement == "bottom-left" );
    // The per-layer section moved into the Default profile the migration
    // created; a global-only audio node (no game to bind it to) and the
    // provenance breadcrumb are dropped, as documented.
    REQUIRE( ResolvedSettings().notifications.muted == true );
    REQUIRE( SessionProfile() == "Default" );
}

// dock_scale was removed 2026-08-24 with the dock itself. Existing configs
// carry the key. Reading a schema-3 config with it is completely uneventful
// (the parse looks keys up by name, so an unknown key is never consulted)
// and the file is not touched; writing drops it -- a real, one-way loss for
// a removed feature, asserted here rather than left as folklore.
TEST_CASE( "a config carrying the removed dock_scale key loads cleanly, and drops it on the next write", "[config]" )
{
    TempConfigHome home;

    const std::string sGlobal = R"({
  "schema_version": 3,
  "overlay": {
    "dock_scale": 1.4,
    "display_scale": 1.75,
    "notification_scale": 1.1
  },
  "profiles": { "last_general": "", "games": {} }
})";

    const std::filesystem::path pathGlobal = home.dir / "gamescope-ritz" / "global.json";
    std::filesystem::create_directories( pathGlobal.parent_path() );
    {
        std::ofstream f( pathGlobal );
        f << sGlobal;
    }
    const auto tWritten = std::filesystem::last_write_time( pathGlobal );

    Settings g = LoadGlobal();

    // The removed key did not disturb the keys around it.
    REQUIRE( g.overlay.display_scale == 1.75f );
    REQUIRE( g.overlay.notification_scale == 1.1f );

    // Reading did not rewrite the file - the key is still on disk, untouched.
    REQUIRE( std::filesystem::last_write_time( pathGlobal ) == tWritten );
    {
        std::ifstream in( pathGlobal );
        const std::string sStillOnDisk( ( std::istreambuf_iterator<char>( in ) ),
                                          std::istreambuf_iterator<char>() );
        REQUIRE( sStillOnDisk.find( "dock_scale" ) != std::string::npos );
    }

    // The next write drops it, and keeps everything the struct still has.
    EnqueueGlobalWrite( g );
    FlushPendingWrites();

    std::ifstream in( pathGlobal );
    const std::string sAfter( ( std::istreambuf_iterator<char>( in ) ),
                                std::istreambuf_iterator<char>() );
    REQUIRE( sAfter.find( "dock_scale" ) == std::string::npos );
    REQUIRE( sAfter.find( "display_scale" ) != std::string::npos );

    Settings reloaded = LoadGlobal();
    REQUIRE( reloaded.overlay.display_scale == 1.75f );
    REQUIRE( reloaded.overlay.notification_scale == 1.1f );
}

// ---- Requests #2/#3, 2026-09-04: vibrancy range + shadow lift -----------

TEST_CASE( "a fresh config (no file at all) resolves vibrancy strength to neutral (1.0), not greyscale", "[config]" )
{
    TempConfigHome home;

    // No file on disk yet - this exercises ConfigSchema.h's compiled-in
    // default member initializer, not JGetFloat's fallback path. Under the
    // old additive-boost meaning, 0.0 was neutral; under the new
    // multiplier meaning, 0.0 is full greyscale, so the struct default had
    // to move to 1.0 along with the semantic change, or a fresh install
    // would open with a desaturated screen.
    Settings s = ResolvedSettings();
    REQUIRE( s.reshade.vibrancy.strength == 1.0f );
}

TEST_CASE( "reshade.vibrancy.strength round-trips across the whole 0.0-3.0 multiplier range", "[config]" )
{
    for ( float flValue : { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f } )
    {
        TempConfigHome home;

        Settings s{};
        s.reshade.vibrancy.strength = flValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.reshade.vibrancy.strength == flValue );
    }
}

// A freshly-saved config already carries the current schema_version, so it
// takes the "nothing to migrate" path through Migrate_1_to_2 - this pins
// that a same-version round trip is a true no-op, not just "close enough".
TEST_CASE( "a config saved under the current schema round-trips vibrancy strength unmigrated", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.reshade.vibrancy.strength = 0.0f; // greyscale under the CURRENT meaning
    REQUIRE( SaveSections( s ) );

    Settings loaded = LoadSections();
    REQUIRE( loaded.reshade.vibrancy.strength == 0.0f ); // not bumped to 1.0 again
}

// Request #2's actual concern: a schema-1 file (the only schema this fork
// ever shipped before today) must not have its untouched, neutral 0.0
// silently reread as full greyscale under the new 0.0-3.0 meaning.
// Migrate_1_to_2 (ConfigManager.cpp) shifts the whole old range onto the
// new one by the constant that carries old-neutral to new-neutral (+1.0),
// then clamps into 0.0..3.0 - exercised here at old min/neutral/max.
TEST_CASE( "a schema-1 config's vibrancy.strength migrates from the old additive scale to the new multiplier scale", "[config]" )
{
    struct Case { float flOld; float flExpectedNew; };
    for ( const Case &c : { Case{ 0.0f, 1.0f }, Case{ -1.0f, 0.0f }, Case{ 1.0f, 2.0f }, Case{ -0.4f, 0.6f } } )
    {
        TempConfigHome home;
        std::filesystem::create_directories( ConfigRoot() );

        std::ofstream( GlobalConfigPath() ) << R"({
            "schema_version": 1,
            "reshade": { "vibrancy": { "enabled": true, "strength": )" << c.flOld << R"( } }
        })";

        Settings s = ResolvedSettings();
        REQUIRE( s.reshade.vibrancy.enabled == true );        // unrelated field untouched
        REQUIRE( s.reshade.vibrancy.strength == c.flExpectedNew );
    }
}

// A config with no schema_version key at all (predates the field itself)
// takes the same migration path as an explicit schema_version 1 - both
// predate the vibrancy rename.
TEST_CASE( "a config with no schema_version field at all also migrates vibrancy.strength", "[config]" )
{
    TempConfigHome home;
    std::filesystem::create_directories( ConfigRoot() );

    std::ofstream( GlobalConfigPath() ) << R"({
        "reshade": { "vibrancy": { "strength": 0.0 } }
    })";

    Settings s = ResolvedSettings();
    REQUIRE( s.reshade.vibrancy.strength == 1.0f );
}

// Request #3: neutral (disabled, strength 0.0) is the default, so an
// existing config that never mentions shadow_lift at all - which is every
// config on disk today, since the field is brand new - is unaffected.
TEST_CASE( "an existing config with no shadow_lift key resolves to the neutral default", "[config]" )
{
    TempConfigHome home;
    std::filesystem::create_directories( ConfigRoot() );

    std::ofstream( GlobalConfigPath() ) << R"({
        "schema_version": 2,
        "gamescope": { "filter": "FSR" }
    })";

    Settings s = ResolvedSettings();
    REQUIRE( s.reshade.shadow_lift.enabled == false );
    REQUIRE( s.reshade.shadow_lift.strength == 0.0f );
    REQUIRE( s.gamescope.filter == "FSR" ); // unrelated section untouched
}

TEST_CASE( "reshade.shadow_lift.enabled and strength round-trip", "[config]" )
{
    for ( float flValue : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f } )
    {
        TempConfigHome home;

        Settings s{};
        s.reshade.shadow_lift.enabled = true;
        s.reshade.shadow_lift.strength = flValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.reshade.shadow_lift.enabled == true );
        REQUIRE( loaded.reshade.shadow_lift.strength == flValue );
    }
}

// ---------------------------------------------------------------------
// Crosshair (2026-09-05, superdoc/features/crosshair.md): a normal
// per-layer section, serialised under the "crosshair" key. Every field
// round-trips; an old config with no such key resolves to the defaults
// (master switch off, so nothing changes for anyone until they opt in).
// ---------------------------------------------------------------------

namespace
{
    CrosshairSettings NonDefaultCrosshair()
    {
        CrosshairSettings c;
        c.enabled = true;
        c.line_enabled = false;
        c.line_length = 12;
        c.line_width = 1;
        c.line_gap = 0;
        c.line_color = 0x12ABCD;
        c.line_opacity = 0.35f;
        c.dot_enabled = true;
        c.dot_size = 3;
        c.dot_color = 0xFF00FF;
        c.dot_opacity = 0.7f;
        c.outline_enabled = false;
        c.outline_width = 3;
        c.outline_opacity = 0.15f;
        c.outline_color = 0x101010;
        c.hide_on_right_click = true;
        c.hide_mode = "shrink";
        c.hide_time_ms = 450;
        c.hide_animate_back = false;
        c.hide_animate_back = false;
        c.apply_scaling = true;
        return c;
    }

    void RequireCrosshairEquals( const CrosshairSettings &a, const CrosshairSettings &b )
    {
        REQUIRE( a.enabled == b.enabled );
        REQUIRE( a.line_enabled == b.line_enabled );
        REQUIRE( a.line_length == b.line_length );
        REQUIRE( a.line_width == b.line_width );
        REQUIRE( a.line_gap == b.line_gap );
        REQUIRE( a.line_color == b.line_color );
        REQUIRE( a.line_opacity == b.line_opacity );
        REQUIRE( a.dot_enabled == b.dot_enabled );
        REQUIRE( a.dot_size == b.dot_size );
        REQUIRE( a.dot_color == b.dot_color );
        REQUIRE( a.dot_opacity == b.dot_opacity );
        REQUIRE( a.outline_enabled == b.outline_enabled );
        REQUIRE( a.outline_width == b.outline_width );
        REQUIRE( a.outline_opacity == b.outline_opacity );
        REQUIRE( a.outline_color == b.outline_color );
        REQUIRE( a.hide_on_right_click == b.hide_on_right_click );
        REQUIRE( a.hide_mode == b.hide_mode );
        REQUIRE( a.hide_time_ms == b.hide_time_ms );
        REQUIRE( a.hide_animate_back == b.hide_animate_back );
        REQUIRE( a.hide_animate_back == b.hide_animate_back );
        REQUIRE( a.apply_scaling == b.apply_scaling );
    }
}

TEST_CASE( "crosshair: every field round-trips through a profile file", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.crosshair = NonDefaultCrosshair();
    // Every value above differs from its default, so a field the serialiser
    // forgot would show up as the default coming back.
    REQUIRE( SaveSections( s ) );

    Settings loaded = LoadSections();
    RequireCrosshairEquals( loaded.crosshair, NonDefaultCrosshair() );
}

TEST_CASE( "crosshair: the defaults round-trip too, and the master switch defaults to off", "[config]" )
{
    TempConfigHome home;

    REQUIRE( Settings{}.crosshair.enabled == false );

    Settings s{};
    REQUIRE( SaveSections( s ) );
    RequireCrosshairEquals( LoadGlobal().crosshair, CrosshairSettings{} );
}

TEST_CASE( "crosshair.hide_mode round-trips across all three modes", "[config]" )
{
    for ( const char *pszValue : { "fade", "focus", "shrink" } )
    {
        TempConfigHome home;

        Settings s{};
        s.crosshair.hide_mode = pszValue;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.crosshair.hide_mode == pszValue );
    }
}

TEST_CASE( "crosshair: the three colours and three opacities round-trip independently", "[config]" )
{
    for ( int nColor : { 0x000000, 0xFFFFFF, 0x00FF00, 0x7F7F7F } )
    {
        TempConfigHome home;

        Settings s{};
        s.crosshair.line_color = nColor;
        s.crosshair.dot_color = nColor ^ 0x0000FF;
        s.crosshair.outline_color = nColor ^ 0xFF0000;
        s.crosshair.line_opacity = 0.25f;
        s.crosshair.dot_opacity = 0.5f;
        s.crosshair.outline_opacity = 0.75f;

        REQUIRE( SaveSections( s ) );

        Settings loaded = LoadSections();
        REQUIRE( loaded.crosshair.line_color == nColor );
        REQUIRE( loaded.crosshair.dot_color == ( nColor ^ 0x0000FF ) );
        REQUIRE( loaded.crosshair.outline_color == ( nColor ^ 0xFF0000 ) );
        REQUIRE( loaded.crosshair.line_opacity == 0.25f );
        REQUIRE( loaded.crosshair.dot_opacity == 0.5f );
        REQUIRE( loaded.crosshair.outline_opacity == 0.75f );
    }
}

TEST_CASE( "a config predating the crosshair loads with the crosshair off at its defaults", "[config]" )
{
    TempConfigHome home;
    std::filesystem::create_directories( ConfigRoot() );
    std::ofstream( GlobalConfigPath() ) << R"({"fps_display": {"enabled": true}})";

    Settings loaded = ResolvedSettings();
    REQUIRE( loaded.fps_display.enabled == true );
    RequireCrosshairEquals( loaded.crosshair, CrosshairSettings{} );
}


// =============================================================================
//  Profiles v2 (2026-09-06, superdoc/features/profiles.md): schema
// =============================================================================

namespace
{
    std::string ReadText( const std::string &sPath )
    {
        std::ifstream f( sPath );
        return std::string( ( std::istreambuf_iterator<char>( f ) ), std::istreambuf_iterator<char>() );
    }

    void WriteText( const std::filesystem::path &path, const std::string &sText )
    {
        std::filesystem::create_directories( path.parent_path() );
        std::ofstream f( path );
        f << sText;
    }

    ProfileMeta General( const std::string &sName )
    {
        ProfileMeta m;
        m.name = sName;
        return m;
    }

    ProfileMeta Game( const std::string &sName, const std::string &sAppId, const std::string &sInherits = "" )
    {
        ProfileMeta m;
        m.name = sName;
        m.kind = ProfileKind::Game;
        m.app_id = sAppId;
        m.inherits = sInherits;
        return m;
    }

    std::vector<std::string> Names( const std::vector<ProfileMeta> &v )
    {
        std::vector<std::string> out;
        for ( const ProfileMeta &m : v )
            out.push_back( m.name );
        return out;
    }
}

TEST_CASE( "global.json carries overlay and the profile pointers, and no per-layer section", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.gamescope.filter = "FSR";
    s.overlay.display_scale = 1.5f;
    REQUIRE( SaveGlobal( s ) );

    const std::string sText = ReadText( GlobalConfigPath() );
    REQUIRE( sText.find( "\"overlay\"" ) != std::string::npos );
    REQUIRE( sText.find( "\"profiles\"" ) != std::string::npos );
    REQUIRE( sText.find( "\"schema_version\": 3" ) != std::string::npos );
    REQUIRE( sText.find( "\"gamescope\"" ) == std::string::npos );
    REQUIRE( sText.find( "FSR" ) == std::string::npos );

    Settings loaded = LoadGlobal();
    REQUIRE( loaded.overlay.display_scale == 1.5f );
    REQUIRE( loaded.gamescope.filter == Settings{}.gamescope.filter );
}

TEST_CASE( "ProfileMeta round-trips for general, game and inheriting profiles, and ListProfiles reports them sorted", "[config]" )
{
    TempConfigHome home;
    REQUIRE( ListProfiles().empty() );

    REQUIRE( SaveProfile( General( "Comp" ), Settings{} ) );
    ProfileMeta rust = Game( "Rust", "252490", "Comp" );
    rust.game_name = "Rust";
    REQUIRE( SaveProfile( rust, Settings{} ) );
    REQUIRE( SaveProfile( Game( "Alone", "570" ), Settings{} ) );

    std::optional<ProfileMeta> oComp = LoadProfileMeta( "Comp" );
    REQUIRE( oComp.has_value() );
    REQUIRE( oComp->kind == ProfileKind::General );
    REQUIRE( oComp->inherits.empty() );

    std::optional<ProfileMeta> oRust = LoadProfileMeta( "Rust" );
    REQUIRE( oRust.has_value() );
    REQUIRE( oRust->kind == ProfileKind::Game );
    REQUIRE( oRust->app_id == "252490" );
    REQUIRE( oRust->game_name == "Rust" );
    REQUIRE( oRust->inherits == "Comp" );

    REQUIRE( Names( ListProfiles() ) == std::vector<std::string>{ "Alone", "Comp", "Rust" } );

    // A general profile's file carries no game keys at all.
    const std::string sComp = ReadText( ProfilePath( "Comp" ) );
    REQUIRE( sComp.find( "\"kind\": \"general\"" ) != std::string::npos );
    REQUIRE( sComp.find( "app_id" ) == std::string::npos );
    REQUIRE( sComp.find( "inherits" ) == std::string::npos );
}

TEST_CASE( "an inheriting game profile stores only the diff and follows its parent live", "[config]" )
{
    TempConfigHome home;

    Settings comp{};
    comp.gamescope.filter = "FSR";
    comp.gamescope.sharpness = 5;
    comp.reshade.vibrancy.strength = 1.4f;
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );

    Settings rust = comp;
    rust.gamescope.sharpness = 9;
    rust.reshade.vibrancy.enabled = true; // one nested key differs
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), rust ) );

    // The file holds the two differing keys and nothing else of those
    // sections.
    const std::string sFile = ReadText( ProfilePath( "Rust" ) );
    REQUIRE( sFile.find( "\"sharpness\": 9" ) != std::string::npos );
    REQUIRE( sFile.find( "\"filter\"" ) == std::string::npos );
    REQUIRE( sFile.find( "\"strength\"" ) == std::string::npos );
    REQUIRE( sFile.find( "\"fps_display\"" ) == std::string::npos );

    // Resolved through the parent.
    std::optional<Settings> oRust = LoadProfile( "Rust" );
    REQUIRE( oRust.has_value() );
    REQUIRE( oRust->gamescope.filter == "FSR" );
    REQUIRE( oRust->gamescope.sharpness == 9 );
    REQUIRE( oRust->reshade.vibrancy.strength == 1.4f );
    REQUIRE( oRust->reshade.vibrancy.enabled );

    // Inheritance is live: edit the parent, the child follows -- except
    // where it overrides.
    comp.gamescope.filter = "NIS";
    comp.gamescope.sharpness = 1;
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    oRust = LoadProfile( "Rust" );
    REQUIRE( oRust->gamescope.filter == "NIS" );
    REQUIRE( oRust->gamescope.sharpness == 9 );

    // A standalone game profile stores everything.
    REQUIRE( SaveProfile( Game( "Alone", "570" ), rust ) );
    REQUIRE( ReadText( ProfilePath( "Alone" ) ).find( "\"filter\"" ) != std::string::npos );
}

TEST_CASE( "a value set equal to the parent's reads as inherited -- the diff ceiling", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings comp{};
    comp.gamescope.sharpness = 5;
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    Settings rust = comp;
    rust.gamescope.sharpness = 9;
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), rust ) );
    REQUIRE( SelectProfile( "Rust" ) );
    REQUIRE( OverriddenKeys().count( "gamescope.sharpness" ) == 1 );

    // Setting it back to the parent's value stores nothing: it is
    // inherited again, and will follow the parent from now on.
    rust.gamescope.sharpness = 5;
    EnqueueRoutedWrite( rust );
    REQUIRE( OverriddenKeys().empty() );
    FlushPendingWrites();
    comp.gamescope.sharpness = 7;
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    REQUIRE( LoadProfile( "Rust" )->gamescope.sharpness == 7 );
}

TEST_CASE( "GameEntry / SetGameAudioNode live in global.json's game entry, never in a profile", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "77" );

    REQUIRE( GameEntry( "77" ).audio_node.empty() );
    SetGameAudioNode( "77", "eldenring.exe" );
    REQUIRE( GameEntry( "77" ).audio_node == "eldenring.exe" );
    FlushPendingWrites();

    const std::string sGlobal = ReadText( GlobalConfigPath() );
    REQUIRE( sGlobal.find( "eldenring.exe" ) != std::string::npos );
    REQUIRE( sGlobal.find( "\"77\"" ) != std::string::npos );

    // The session profile's file knows nothing of it.
    EnqueueRoutedWrite( Settings{} );
    FlushPendingWrites();
    REQUIRE( ReadText( ProfilePath( SessionProfile() ) ).find( "eldenring" ) == std::string::npos );

    // And it survives a fresh process.
    ResetSessionRoutingForTests();
    REQUIRE( GameEntry( "77" ).audio_node == "eldenring.exe" );
    REQUIRE( GameEntry( "78" ).audio_node.empty() );
}

TEST_CASE( "notification placement and panel geometry are global-only and never ride in a profile", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    s.overlay.notification_placement = "bottom-left";
    s.overlay.panel_geometry[ "display" ] = PanelGeometry{ 120.0f, 80.0f, 500.0f, 360.0f };
    s.notifications.muted = true;
    REQUIRE( SaveProfile( General( "P" ), s ) );

    const std::string sFile = ReadText( ProfilePath( "P" ) );
    REQUIRE( sFile.find( "\"overlay\"" ) == std::string::npos );
    REQUIRE( sFile.find( "bottom-left" ) == std::string::npos );
    REQUIRE( sFile.find( "panel_geometry" ) == std::string::npos );
    REQUIRE( LoadProfile( "P" )->notifications.muted );
    REQUIRE( LoadProfile( "P" )->overlay.notification_placement == OverlaySettings{}.notification_placement );
}

// =============================================================================
//  Migration 2 -> 3 (fixture-driven, one test per row of the table in
//  superdoc/features/profiles.md)
// =============================================================================

namespace
{
    const char *kOldGlobal = R"({
  "schema_version": 2,
  "gamescope": { "filter": "FSR", "sharpness": 4 },
  "fps_display": { "enabled": true },
  "overlay": { "display_scale": 1.75, "accent_hue": 291.0 },
  "notifications": { "muted": true },
  "audio": { "manual_node_binary": "floorp" },
  "last_applied_profile": "Old",
  "active_profile": "",
  "auto_save_profile": false
})";

    std::string OldGame( const char *pszFilter, bool bOverride, const char *pszLastApplied, const char *pszAudio )
    {
        return std::string( R"({"schema_version": 2, "override_global": )" ) + ( bOverride ? "true" : "false" ) +
            R"(, "gamescope": {"filter": ")" + pszFilter + R"(", "sharpness": 4}, "fps_display": {"enabled": true}, "audio": {"manual_node_binary": ")" + pszAudio +
            R"("}, "last_applied_profile": ")" + pszLastApplied + R"("})";
    }

    std::filesystem::path GamePathOld( const TempConfigHome &home, const char *pszAppId )
    {
        return home.dir / "gamescope-ritz" / "games" / ( std::string( pszAppId ) + ".json" );
    }
}

TEST_CASE( "migration: an old global.json's sections become the Default profile, overlay stays, and the old file is schema 3", "[config]" )
{
    TempConfigHome home;
    WriteText( GlobalConfigPath(), kOldGlobal );

    // Any read triggers it.
    Settings g = LoadGlobal();
    REQUIRE( g.overlay.display_scale == 1.75f );
    REQUIRE( g.overlay.accent_hue == 291.0f );

    REQUIRE( ProfileExists( "Default" ) );
    std::optional<ProfileMeta> oMeta = LoadProfileMeta( "Default" );
    REQUIRE( oMeta->kind == ProfileKind::General );
    std::optional<Settings> oDefault = LoadProfile( "Default" );
    REQUIRE( oDefault->gamescope.filter == "FSR" );
    REQUIRE( oDefault->gamescope.sharpness == 4 );
    REQUIRE( oDefault->fps_display.enabled );
    REQUIRE( oDefault->notifications.muted );

    const std::string sGlobal = ReadText( GlobalConfigPath() );
    REQUIRE( sGlobal.find( "\"schema_version\": 3" ) != std::string::npos );
    REQUIRE( sGlobal.find( "\"last_general\": \"Default\"" ) != std::string::npos );
    REQUIRE( sGlobal.find( "active_profile" ) == std::string::npos );
    REQUIRE( sGlobal.find( "last_applied_profile" ) == std::string::npos );
    REQUIRE( sGlobal.find( "\"gamescope\"" ) == std::string::npos );

    // The session lands on it.
    REQUIRE( SessionProfile() == "Default" );
    REQUIRE( ResolvedSettings().gamescope.filter == "FSR" );
}

TEST_CASE( "migration: an existing profile with identical content is used instead of creating Default", "[config]" )
{
    TempConfigHome home;
    WriteText( GlobalConfigPath(), kOldGlobal );
    // The same sections, saved as a schema-2 profile the way v1 wrote them.
    Settings same{};
    same.gamescope.filter = "FSR";
    same.gamescope.sharpness = 4;
    same.fps_display.enabled = true;
    same.notifications.muted = true;
    WriteText( ProfilePath( "Mine" ), R"({"schema_version": 2, "name": "Mine", "gamescope": {"filter": "FSR", "sharpness": 4}, "fps_display": {"enabled": true}, "notifications": {"muted": true}, "last_applied_profile": ""})" );

    LoadGlobal();
    REQUIRE_FALSE( ProfileExists( "Default" ) );
    REQUIRE( SessionProfile() == "Mine" );
}

TEST_CASE( "migration: a profile named Default with different content is kept, and the old global becomes Default 2", "[config]" )
{
    TempConfigHome home;
    WriteText( GlobalConfigPath(), kOldGlobal );
    WriteText( ProfilePath( "Default" ), R"({"schema_version": 2, "name": "Default", "gamescope": {"filter": "NIS"}})" );

    LoadGlobal();
    REQUIRE( LoadProfile( "Default" )->gamescope.filter == "NIS" );
    REQUIRE( ProfileExists( "Default 2" ) );
    REQUIRE( LoadProfile( "Default 2" )->gamescope.filter == "FSR" );
    REQUIRE( SessionProfile() == "Default 2" );
}

TEST_CASE( "migration: active_profile becomes last_general when that profile exists", "[config]" )
{
    TempConfigHome home;
    std::string sGlobal = kOldGlobal;
    sGlobal.replace( sGlobal.find( "\"active_profile\": \"\"" ), std::string( "\"active_profile\": \"\"" ).size(), "\"active_profile\": \"Comp\"" );
    WriteText( GlobalConfigPath(), sGlobal );
    WriteText( ProfilePath( "Comp" ), R"({"schema_version": 2, "name": "Comp", "gamescope": {"filter": "NIS"}})" );

    LoadGlobal();
    REQUIRE( SessionProfile() == "Comp" );
    REQUIRE( ProfileExists( "Default" ) ); // the old values are still kept
}

TEST_CASE( "migration: a games/<AppId>.json becomes a game profile inheriting Default, stored as the diff, selected only if it was on", "[config]" )
{
    TempConfigHome home;
    WriteText( GlobalConfigPath(), kOldGlobal );
    WriteText( GamePathOld( home, "252490" ), OldGame( "NIS", true, "", "rust.exe" ) );
    WriteText( GamePathOld( home, "570" ), OldGame( "FSR", false, "", "" ) );
    const std::string sOldRust = ReadText( GamePathOld( home, "252490" ).string() );

    LoadGlobal();

    std::optional<ProfileMeta> oRust = LoadProfileMeta( "252490" );
    REQUIRE( oRust.has_value() );
    REQUIRE( oRust->kind == ProfileKind::Game );
    REQUIRE( oRust->app_id == "252490" );
    REQUIRE( oRust->inherits == "Default" );
    // Only the filter differs from Default (FSR/4/enabled) -> only it is stored.
    const std::string sRust = ReadText( ProfilePath( "252490" ) );
    REQUIRE( sRust.find( "\"filter\": \"NIS\"" ) != std::string::npos );
    REQUIRE( sRust.find( "\"sharpness\"" ) == std::string::npos );
    REQUIRE( sRust.find( "manual_node_binary" ) == std::string::npos );
    REQUIRE( LoadProfile( "252490" )->gamescope.sharpness == 4 );

    // Assignment and audio node.
    REQUIRE( GameEntry( "252490" ).selected == "252490" );
    REQUIRE( GameEntry( "252490" ).audio_node == "rust.exe" );
    // override_global: false -> the profile exists but is not selected.
    REQUIRE( ProfileExists( "570" ) );
    REQUIRE( GameEntry( "570" ).selected.empty() );

    // The old files are left exactly as they were.
    REQUIRE( ReadText( GamePathOld( home, "252490" ).string() ) == sOldRust );
    REQUIRE( std::filesystem::exists( GamePathOld( home, "570" ) ) );

    // A session for that game lands on its profile.
    ResetSessionRoutingForTests();
    ScopedSessionAppId scopedAppId( "252490" );
    REQUIRE( SessionProfile() == "252490" );
    REQUIRE( ResolvedSettings().gamescope.filter == "NIS" );
}

TEST_CASE( "migration: a game file's last_applied_profile becomes its parent when that profile exists", "[config]" )
{
    TempConfigHome home;
    WriteText( GlobalConfigPath(), kOldGlobal );
    WriteText( ProfilePath( "Comp" ), R"({"schema_version": 2, "name": "Comp", "gamescope": {"filter": "NIS", "sharpness": 4}})" );
    WriteText( GamePathOld( home, "1" ), OldGame( "NIS", true, "Comp", "" ) );
    WriteText( GamePathOld( home, "2" ), OldGame( "NIS", true, "Gone", "" ) );

    LoadGlobal();
    REQUIRE( LoadProfileMeta( "1" )->inherits == "Comp" );
    REQUIRE( LoadProfileMeta( "2" )->inherits == "Default" );
}

TEST_CASE( "migration: a game whose app id collides with an existing profile name gets 'Game <id>'", "[config]" )
{
    TempConfigHome home;
    WriteText( GlobalConfigPath(), kOldGlobal );
    WriteText( ProfilePath( "440" ), R"({"schema_version": 2, "name": "440", "gamescope": {"filter": "NIS"}})" );
    WriteText( GamePathOld( home, "440" ), OldGame( "PIXEL", true, "", "" ) );

    LoadGlobal();
    REQUIRE( LoadProfileMeta( "440" )->kind == ProfileKind::General );
    REQUIRE( LoadProfileMeta( "Game 440" )->kind == ProfileKind::Game );
    REQUIRE( GameEntry( "440" ).selected == "Game 440" );
}

TEST_CASE( "migration: an interrupted run (profiles written, global not) re-runs without duplicating anything", "[config]" )
{
    TempConfigHome home;
    WriteText( GlobalConfigPath(), kOldGlobal );
    WriteText( GamePathOld( home, "252490" ), OldGame( "NIS", true, "", "rust.exe" ) );
    LoadGlobal();
    const std::vector<std::string> vecFirst = Names( ListProfiles() );
    const std::string sGlobalFirst = ReadText( GlobalConfigPath() );

    // Put the schema-2 global back, as if the process died before step 4.
    WriteText( GlobalConfigPath(), kOldGlobal );
    ResetSessionRoutingForTests();
    LoadGlobal();

    REQUIRE( Names( ListProfiles() ) == vecFirst );
    REQUIRE( ReadText( GlobalConfigPath() ) == sGlobalFirst );
    REQUIRE( GameEntry( "252490" ).selected == "252490" );
}

TEST_CASE( "migration: a fresh install and a schema-3 config are left alone", "[config]" )
{
    TempConfigHome home;
    REQUIRE_FALSE( std::filesystem::exists( GlobalConfigPath() ) );
    LoadGlobal();
    REQUIRE_FALSE( std::filesystem::exists( GlobalConfigPath() ) ); // reading did not write

    WriteText( GlobalConfigPath(), R"({"schema_version": 3, "overlay": {"display_scale": 1.25}, "profiles": {"last_general": "X", "games": {}}})" );
    const auto tWritten = std::filesystem::last_write_time( GlobalConfigPath() );
    ResetSessionRoutingForTests();
    REQUIRE( LoadGlobal().overlay.display_scale == 1.25f );
    REQUIRE( std::filesystem::last_write_time( GlobalConfigPath() ) == tWritten );
    REQUIRE_FALSE( ProfileExists( "Default" ) );
}

// =============================================================================
//  Session resolution, routing and the CRUD the list needs
// =============================================================================

TEST_CASE( "SessionProfile: override, then this game's selection, then last_general, then a created Default", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    // Nothing on disk: Default is created from the struct defaults.
    REQUIRE( SessionProfile() == "Default" );
    REQUIRE( ProfileExists( "Default" ) );
    REQUIRE( ResolvedSettings().gamescope.filter == Settings{}.gamescope.filter );

    // last_general: a general profile selected anywhere.
    REQUIRE( SaveProfile( General( "Comp" ), Settings{} ) );
    REQUIRE( SelectProfile( "Comp" ) );
    ResetSessionRoutingForTests();
    setenv( "GS_RITZ_APPID", "570", 1 ); // a game never seen before
    REQUIRE( SessionProfile() == "Comp" );

    // This game's own selection wins over last_general.
    REQUIRE( SaveProfile( Game( "Dota", "570", "Comp" ), Settings{} ) );
    REQUIRE( SelectProfile( "Dota" ) );
    ResetSessionRoutingForTests();
    setenv( "GS_RITZ_APPID", "570", 1 );
    REQUIRE( SessionProfile() == "Dota" );
    REQUIRE( SessionProfileParent() == std::optional<std::string>( "Comp" ) );
    // ... and did not move last_general.
    ResetSessionRoutingForTests();
    setenv( "GS_RITZ_APPID", "999", 1 );
    REQUIRE( SessionProfile() == "Comp" );

    // The override beats everything and persists nothing.
    ResetSessionRoutingForTests();
    setenv( "GS_RITZ_APPID", "570", 1 );
    SessionProfileResult r = UseSessionProfile( "Comp" );
    REQUIRE( r.ok );
    REQUIRE_FALSE( r.created );
    REQUIRE( SessionProfile() == "Comp" );
    REQUIRE( SessionProfileOverride() == std::optional<std::string>( "Comp" ) );
    REQUIRE( GameEntry( "570" ).selected == "Dota" );
    ResetSessionRoutingForTests();
    setenv( "GS_RITZ_APPID", "570", 1 );
    REQUIRE( SessionProfile() == "Dota" );
}

TEST_CASE( "SelectProfile persists the assignment, clears the override and bumps the generation", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );
    REQUIRE( SaveProfile( General( "Comp" ), Settings{} ) );
    REQUIRE( SaveProfile( General( "Casual" ), Settings{} ) );

    const uint64_t ulBefore = ConfigGeneration();
    REQUIRE_FALSE( SelectProfile( "Nope" ) );
    REQUIRE( ConfigGeneration() == ulBefore );

    UseSessionProfile( "Casual" );
    REQUIRE( SessionProfileOverride().has_value() );
    REQUIRE( SelectProfile( "Comp" ) );
    REQUIRE( ConfigGeneration() > ulBefore );
    REQUIRE_FALSE( SessionProfileOverride().has_value() );
    REQUIRE( SessionProfile() == "Comp" );

    const std::string sGlobal = ReadText( GlobalConfigPath() );
    REQUIRE( sGlobal.find( "\"last_general\": \"Comp\"" ) != std::string::npos );
    REQUIRE( sGlobal.find( "\"selected\": \"Comp\"" ) != std::string::npos );
}

TEST_CASE( "with no app id, a general profile is remembered and a game profile is session-only", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr );
    REQUIRE( SaveProfile( General( "Comp" ), Settings{} ) );
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), Settings{} ) );

    REQUIRE( SelectProfile( "Comp" ) );
    ResetSessionRoutingForTests();
    REQUIRE( SessionProfile() == "Comp" );

    REQUIRE( SelectProfile( "Rust" ) );
    REQUIRE( SessionProfile() == "Rust" );
    ResetSessionRoutingForTests();
    REQUIRE( SessionProfile() == "Comp" );
}

TEST_CASE( "EnqueueRoutedWrite writes the session profile and only it, as a diff for an inheriting game profile", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings comp{};
    comp.gamescope.filter = "FSR";
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), comp ) );
    REQUIRE( SelectProfile( "Rust" ) );
    const std::string sCompBefore = ReadText( ProfilePath( "Comp" ) );

    Settings edit = ResolvedSettings();
    edit.gamescope.sharpness = 11;
    edit.overlay.display_scale = 1.9f; // a stale panel copy of overlay must not leak anywhere
    EnqueueRoutedWrite( edit );
    FlushPendingWrites();

    const std::string sRust = ReadText( ProfilePath( "Rust" ) );
    REQUIRE( sRust.find( "\"sharpness\": 11" ) != std::string::npos );
    REQUIRE( sRust.find( "\"filter\"" ) == std::string::npos );
    REQUIRE( sRust.find( "\"overlay\"" ) == std::string::npos );
    REQUIRE( ReadText( ProfilePath( "Comp" ) ) == sCompBefore );
    REQUIRE( LoadGlobal().overlay.display_scale == OverlaySettings{}.display_scale );
    REQUIRE( LoadProfile( "Rust" )->gamescope.sharpness == 11 );
    REQUIRE( LoadProfile( "Rust" )->gamescope.filter == "FSR" );
}

TEST_CASE( "ResolvedSettings reflects a queued routed write before it is flushed", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr );

    Settings s = ResolvedSettings();
    s.gamescope.sharpness = 3;
    EnqueueRoutedWrite( s );
    REQUIRE( ResolvedSettings().gamescope.sharpness == 3 );
    FlushPendingWrites();
    REQUIRE( LoadProfile( SessionProfile() )->gamescope.sharpness == 3 );
}

TEST_CASE( "UseSessionProfile creates a missing profile from what the session would have used, sanitized", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings comp{};
    comp.gamescope.filter = "FSR";
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    REQUIRE( SelectProfile( "Comp" ) );

    SessionProfileResult r = UseSessionProfile( "../Tourney!" );
    REQUIRE( r.ok );
    REQUIRE( r.name == "Tourney" );
    REQUIRE( r.created );
    REQUIRE( r.copied_from == "Comp" );
    REQUIRE( LoadProfileMeta( "Tourney" )->kind == ProfileKind::General );
    REQUIRE( LoadProfile( "Tourney" )->gamescope.filter == "FSR" );
    REQUIRE( SessionProfile() == "Tourney" );
    REQUIRE( GameEntry( "252490" ).selected == "Comp" ); // the assignment is untouched

    // Edits go into it.
    Settings edit = ResolvedSettings();
    edit.gamescope.sharpness = 2;
    EnqueueRoutedWrite( edit );
    FlushPendingWrites();
    REQUIRE( LoadProfile( "Tourney" )->gamescope.sharpness == 2 );
    REQUIRE( LoadProfile( "Comp" )->gamescope.sharpness == comp.gamescope.sharpness );

    REQUIRE_FALSE( UseSessionProfile( "!!!" ).ok );
    REQUIRE( SessionProfile() == "Tourney" );
}

TEST_CASE( "CreateProfile and CopyProfile enforce the two-level rule", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings live = ResolvedSettings();
    live.gamescope.sharpness = 8;
    EnqueueRoutedWrite( live );

    REQUIRE( CreateProfile( General( "Comp" ) ) );          // from the current resolved settings
    REQUIRE( LoadProfile( "Comp" )->gamescope.sharpness == 8 );
    REQUIRE_FALSE( CreateProfile( General( "Comp" ) ) );    // exists
    REQUIRE_FALSE( CreateProfile( General( "" ) ) );        // bad name
    ProfileMeta badGeneral = General( "X" );
    badGeneral.inherits = "Comp";
    REQUIRE_FALSE( CreateProfile( badGeneral ) );           // only a game profile inherits
    REQUIRE_FALSE( CreateProfile( Game( "NoApp", "" ) ) );  // needs an app id
    REQUIRE_FALSE( CreateProfile( Game( "R", "1", "Nope" ) ) ); // parent must exist
    REQUIRE( CreateProfile( Game( "Rust", "252490", "Comp" ) ) );
    REQUIRE_FALSE( CreateProfile( Game( "Rust2", "252490", "Rust" ) ) ); // no chains
    ProfileOp op = CreateProfile( Game( "Rust2", "252490", "Rust" ) );
    REQUIRE( op.error.find( "game profile" ) != std::string::npos );

    Settings explicitFrom{};
    explicitFrom.gamescope.filter = "NIS";
    REQUIRE( CreateProfile( General( "Casual" ), &explicitFrom ) );
    REQUIRE( LoadProfile( "Casual" )->gamescope.filter == "NIS" );

    // Copy takes the source's RESOLVED values.
    REQUIRE( CopyProfile( "Rust", General( "RustAsGeneral" ) ) );
    REQUIRE( LoadProfile( "RustAsGeneral" )->gamescope.sharpness == 8 );
    REQUIRE( ReadText( ProfilePath( "RustAsGeneral" ) ).find( "\"sharpness\": 8" ) != std::string::npos );
    REQUIRE_FALSE( CopyProfile( "Nope", General( "Y" ) ) );
}

TEST_CASE( "EditProfileMeta renames with every pointer following, and refuses to make a parent a game profile", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings comp{};
    comp.gamescope.filter = "FSR";
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    Settings rust = comp;
    rust.gamescope.sharpness = 9;
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), rust ) );
    REQUIRE( SelectProfile( "Comp" ) );
    REQUIRE( SelectProfile( "Rust" ) );

    // A parent cannot become a game profile while it has children.
    ProfileOp op = EditProfileMeta( "Comp", Game( "Comp", "1" ) );
    REQUIRE_FALSE( op );
    REQUIRE( op.error.find( "inherited by 1 game profile" ) != std::string::npos );

    // Rename the parent: the child, the pointers and the session follow.
    REQUIRE( EditProfileMeta( "Comp", General( "Competitive" ) ) );
    REQUIRE_FALSE( ProfileExists( "Comp" ) );
    REQUIRE( LoadProfileMeta( "Rust" )->inherits == "Competitive" );
    REQUIRE( LoadProfile( "Rust" )->gamescope.filter == "FSR" );
    REQUIRE( ReadText( GlobalConfigPath() ).find( "\"last_general\": \"Competitive\"" ) != std::string::npos );
    REQUIRE( SessionProfileParent() == std::optional<std::string>( "Competitive" ) );

    // Rename the session profile itself.
    ProfileMeta renamed = Game( "Rust Ranked", "252490", "Competitive" );
    renamed.game_name = "Rust";
    REQUIRE( EditProfileMeta( "Rust", renamed ) );
    REQUIRE( SessionProfile() == "Rust Ranked" );
    REQUIRE( GameEntry( "252490" ).selected == "Rust Ranked" );
    REQUIRE( LoadProfileMeta( "Rust Ranked" )->game_name == "Rust" );
    REQUIRE_FALSE( EditProfileMeta( "Rust Ranked", General( "Competitive" ) ) ); // over an existing one
}

TEST_CASE( "EditProfileMeta keeps the resolved values when the parent changes or the kind changes", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings comp{};
    comp.gamescope.filter = "FSR";
    comp.gamescope.sharpness = 5;
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    Settings casual{};
    casual.gamescope.filter = "NIS";
    casual.gamescope.sharpness = 5;
    REQUIRE( SaveProfile( General( "Casual" ), casual ) );
    Settings rust = comp;
    rust.gamescope.sharpness = 9;
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), rust ) );

    // Switch parent: what the user saw stays; the diff is re-expressed.
    REQUIRE( EditProfileMeta( "Rust", Game( "Rust", "252490", "Casual" ) ) );
    REQUIRE( LoadProfile( "Rust" )->gamescope.filter == "FSR" );
    REQUIRE( LoadProfile( "Rust" )->gamescope.sharpness == 9 );
    REQUIRE( ReadText( ProfilePath( "Rust" ) ).find( "\"filter\": \"FSR\"" ) != std::string::npos );

    // Become general: everything baked, game keys gone.
    REQUIRE( EditProfileMeta( "Rust", General( "Rust" ) ) );
    REQUIRE( LoadProfileMeta( "Rust" )->kind == ProfileKind::General );
    REQUIRE( LoadProfileMeta( "Rust" )->inherits.empty() );
    REQUIRE( LoadProfile( "Rust" )->gamescope.sharpness == 9 );
    REQUIRE( ReadText( ProfilePath( "Rust" ) ).find( "app_id" ) == std::string::npos );

    // And back to a game profile (no children, so allowed).
    REQUIRE( EditProfileMeta( "Rust", Game( "Rust", "252490", "Comp" ) ) );
    REQUIRE( LoadProfileMeta( "Rust" )->inherits == "Comp" );
    REQUIRE( LoadProfile( "Rust" )->gamescope.sharpness == 9 );
}

TEST_CASE( "DeleteProfile bakes its children, clears every pointer, and refuses to leave profiles/", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings comp{};
    comp.gamescope.filter = "FSR";
    comp.gamescope.sharpness = 5;
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    Settings rust = comp;
    rust.gamescope.sharpness = 9;
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), rust ) );
    REQUIRE( SelectProfile( "Comp" ) );
    REQUIRE( SelectProfile( "Rust" ) );

    REQUIRE_FALSE( DeleteProfile( "../global" ) );
    REQUIRE_FALSE( DeleteProfile( ".." ) );
    REQUIRE( std::filesystem::exists( GlobalConfigPath() ) );

    REQUIRE( DeleteProfile( "Comp" ) );
    REQUIRE_FALSE( ProfileExists( "Comp" ) );
    // The child stands alone with the values it had.
    REQUIRE( LoadProfileMeta( "Rust" )->inherits.empty() );
    REQUIRE( LoadProfile( "Rust" )->gamescope.filter == "FSR" );
    REQUIRE( LoadProfile( "Rust" )->gamescope.sharpness == 9 );
    REQUIRE( ReadText( ProfilePath( "Rust" ) ).find( "\"filter\": \"FSR\"" ) != std::string::npos );
    REQUIRE( ReadText( GlobalConfigPath() ).find( "\"last_general\": \"\"" ) != std::string::npos );
    REQUIRE( SessionProfile() == "Rust" );
    REQUIRE_FALSE( SessionProfileParent().has_value() );

    // Deleting the session profile: the session falls through and Default
    // is created.
    const uint64_t ulBefore = ConfigGeneration();
    REQUIRE( DeleteProfile( "Rust" ) );
    REQUIRE( ConfigGeneration() > ulBefore );
    REQUIRE( GameEntry( "252490" ).selected.empty() );
    REQUIRE( SessionProfile() == "Default" );
    REQUIRE( DeleteProfile( "Rust" ) ); // already gone is fine
}

TEST_CASE( "OverriddenKeys and ResetKeyToInherited track the session profile's own keys", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );

    Settings comp{};
    comp.reshade.vibrancy.strength = 1.0f;
    REQUIRE( SaveProfile( General( "Comp" ), comp ) );
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), comp ) );

    // A general profile has no overrides by definition.
    REQUIRE( SelectProfile( "Comp" ) );
    REQUIRE( OverriddenKeys().empty() );
    REQUIRE_FALSE( ResetKeyToInherited( "gamescope.filter" ) );

    REQUIRE( SelectProfile( "Rust" ) );
    REQUIRE( OverriddenKeys().empty() );
    Settings edit = ResolvedSettings();
    edit.reshade.vibrancy.strength = 2.0f;
    edit.fps_display.enabled = !edit.fps_display.enabled;
    EnqueueRoutedWrite( edit );
    REQUIRE( OverriddenKeys() == std::set<std::string>{ "fps_display.enabled", "reshade.vibrancy.strength" } );

    const uint64_t ulBefore = ConfigGeneration();
    REQUIRE( ResetKeyToInherited( "reshade.vibrancy.strength" ) );
    REQUIRE( ConfigGeneration() > ulBefore );
    REQUIRE( OverriddenKeys() == std::set<std::string>{ "fps_display.enabled" } );
    REQUIRE( ResolvedSettings().reshade.vibrancy.strength == 1.0f );
    REQUIRE( ResolvedSettings().fps_display.enabled == edit.fps_display.enabled );
    FlushPendingWrites();
    REQUIRE( LoadProfile( "Rust" )->reshade.vibrancy.strength == 1.0f );
    REQUIRE( ReadText( ProfilePath( "Rust" ) ).find( "vibrancy" ) == std::string::npos );
    REQUIRE_FALSE( ResetKeyToInherited( "reshade.vibrancy.strength" ) ); // not overridden any more
    REQUIRE_FALSE( ResetKeyToInherited( "nonsense.key" ) );

    // Survives a fresh process: read from the file, not the mirror.
    ResetSessionRoutingForTests();
    setenv( "GS_RITZ_APPID", "252490", 1 );
    REQUIRE( OverriddenKeys() == std::set<std::string>{ "fps_display.enabled" } );
}

TEST_CASE( "NoteFocusedWindowTitle fills a game profile's display name once, and SessionGameName falls back to the app id", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );
    REQUIRE( SaveProfile( Game( "252490", "252490" ), Settings{} ) );
    REQUIRE( SaveProfile( Game( "Other", "570" ), Settings{} ) );

    REQUIRE( SessionGameName() == "252490" );
    NoteFocusedWindowTitle( "Rust" );
    REQUIRE( SessionGameName() == "Rust" );
    FlushPendingWrites();
    REQUIRE( LoadProfileMeta( "252490" )->game_name == "Rust" );
    REQUIRE( LoadProfileMeta( "Other" )->game_name.empty() );

    // Only the first title is stored; later titles (a loading screen, a
    // level name) do not keep rewriting the file.
    NoteFocusedWindowTitle( "Rust - Loading" );
    FlushPendingWrites();
    REQUIRE( LoadProfileMeta( "252490" )->game_name == "Rust" );
}

TEST_CASE( "DebugDumpEffective names the session profile, its parent and the launch option", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "252490" );
    REQUIRE( SaveProfile( General( "Comp" ), Settings{} ) );
    REQUIRE( SaveProfile( Game( "Rust", "252490", "Comp" ), Settings{} ) );
    REQUIRE( SelectProfile( "Rust" ) );

    std::string sDump = DebugDumpEffective();
    REQUIRE( sDump.find( "\"session_profile\": \"Rust\"" ) != std::string::npos );
    REQUIRE( sDump.find( "\"inherits\": \"Comp\"" ) != std::string::npos );
    REQUIRE( sDump.find( "\"source\": \"selected by this game\"" ) != std::string::npos );
    REQUIRE( sDump.find( "\"launch_option\": null" ) != std::string::npos );

    UseSessionProfile( "Comp" );
    sDump = DebugDumpEffective();
    REQUIRE( sDump.find( "\"session_profile\": \"Comp\"" ) != std::string::npos );
    REQUIRE( sDump.find( "\"launch_option\": \"Comp\"" ) != std::string::npos );
    REQUIRE( sDump.find( "\"source\": \"session override\"" ) != std::string::npos );
}

TEST_CASE( "queued writes to the same path coalesce, last wins", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( nullptr );

    // A burst, like a slider drag. Whatever the worker batches, the file
    // must end up holding the LAST value -- and Flush must not wait out the
    // quiet period.
    for ( int i = 0; i < 25; i++ )
    {
        Settings s{};
        s.gamescope.sharpness = i;
        EnqueueRoutedWrite( s );
    }
    FlushPendingWrites();
    REQUIRE( LoadProfile( SessionProfile() )->gamescope.sharpness == 24 );

    // Two different paths in one burst both land.
    Settings g{};
    g.overlay.display_scale = 1.3f;
    g.gamescope.filter = "FSR";
    EnqueueGlobalWrite( g );
    EnqueueProfileWrite( General( "P" ), g );
    FlushPendingWrites();
    REQUIRE( LoadGlobal().overlay.display_scale == 1.3f );
    REQUIRE( LoadProfile( "P" )->gamescope.filter == "FSR" );
}

TEST_CASE( "system.clipboard_sync and the crosshair ride in a profile like every other per-layer field", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    REQUIRE( s.system.clipboard_sync ); // default on
    s.system.clipboard_sync = false;
    s.crosshair = NonDefaultCrosshair();
    REQUIRE( SaveSections( s ) );
    REQUIRE_FALSE( LoadSections().system.clipboard_sync );
    RequireCrosshairEquals( LoadSections().crosshair, NonDefaultCrosshair() );

    // And through an inheriting game profile.
    REQUIRE( SaveProfile( General( "Comp" ), s ) );
    Settings child = s;
    child.crosshair.hide_mode = "focus";
    REQUIRE( SaveProfile( Game( "Aim", "1", "Comp" ), child ) );
    REQUIRE_FALSE( LoadProfile( "Aim" )->system.clipboard_sync );
    REQUIRE( LoadProfile( "Aim" )->crosshair.hide_mode == "focus" );
    REQUIRE( LoadProfile( "Aim" )->crosshair.line_length == 12 );
}

TEST_CASE( "gamescope.nested_width/height/refresh_hz round-trip and default to as-launched", "[config]" )
{
    TempConfigHome home;

    Settings s{};
    REQUIRE( s.gamescope.nested_width == 0 );
    REQUIRE( s.gamescope.nested_height == 0 );
    REQUIRE( s.gamescope.nested_refresh_hz == 0 );

    s.gamescope.nested_width = 1280;
    s.gamescope.nested_height = 720;
    s.gamescope.nested_refresh_hz = 144;
    REQUIRE( SaveSections( s ) );

    Settings loaded = LoadSections();
    REQUIRE( loaded.gamescope.nested_width == 1280 );
    REQUIRE( loaded.gamescope.nested_height == 720 );
    REQUIRE( loaded.gamescope.nested_refresh_hz == 144 );
}

// ---- requests-2026-09-06 item 1: select loads, and edits land in the selection ----

TEST_CASE( "select B then a routed write: B's file changes and A's does not; ResolvedSettings follows the selection", "[config]" )
{
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "730" );

    Settings a{};
    a.gamescope.sharpness = 2;
    a.crosshair.line_length = 12;
    Settings b{};
    b.gamescope.sharpness = 8;
    b.crosshair.line_length = 30;
    REQUIRE( SaveProfile( General( "A" ), a ) );
    REQUIRE( SaveProfile( General( "B" ), b ) );
    REQUIRE( SelectProfile( "A" ) );
    REQUIRE( ResolvedSettings().gamescope.sharpness == 2 );
    REQUIRE( ResolvedSettings().crosshair.line_length == 12 );

    // Select B: the resolved values are B's at once, before any write.
    REQUIRE( SelectProfile( "B" ) );
    REQUIRE( SessionProfile() == "B" );
    REQUIRE( ResolvedSettings().gamescope.sharpness == 8 );
    REQUIRE( ResolvedSettings().crosshair.line_length == 30 );

    // A panel reloads (as every EnsureConfigLoaded() does on the bump) and
    // edits one value: B's file changes, A's does not.
    const std::string sABefore = ReadText( ProfilePath( "A" ) );
    static Settings panel; // a panel's file-static copy, the caller identity
    panel = ResolvedSettings();
    panel.crosshair.line_length = 40;
    EnqueueRoutedWrite( panel );
    FlushPendingWrites();
    REQUIRE( ReadText( ProfilePath( "A" ) ) == sABefore );
    REQUIRE( ReadText( ProfilePath( "B" ) ).find( "\"line_length\": 40" ) != std::string::npos );
    REQUIRE( LoadProfile( "B" )->gamescope.sharpness == 8 );
    REQUIRE( LoadProfile( "A" )->crosshair.line_length == 12 );

    // Back to A: A's values, with B's edit kept in B.
    REQUIRE( SelectProfile( "A" ) );
    REQUIRE( ResolvedSettings().gamescope.sharpness == 2 );
    REQUIRE( ResolvedSettings().crosshair.line_length == 12 );
    REQUIRE( LoadProfile( "B" )->crosshair.line_length == 40 );
}

TEST_CASE( "a routed write from one panel never undoes another panel's edit (per-section merge)", "[config]" )
{
    // The 2026-09-06 root cause: every panel writes its WHOLE Settings, and
    // its copy of the other sections is only as fresh as its last generation
    // reload. Measured headless: the Display area set sharpness, the
    // Crosshair area then changed a gap, and the crosshair write put
    // sharpness back. The funnel now merges per section.
    TempConfigHome home;
    ScopedSessionAppId scopedAppId( "730" );

    Settings base{};
    base.gamescope.sharpness = 5;
    base.crosshair.line_gap = 3;
    REQUIRE( SaveProfile( General( "A" ), base ) );
    REQUIRE( SelectProfile( "A" ) );

    // Two panels load at the same generation -- two file-static copies.
    static Settings display;
    static Settings crosshair;
    display = ResolvedSettings();
    crosshair = ResolvedSettings();

    // Display edits sharpness; the crosshair panel's copy still says 5.
    display.gamescope.sharpness = 10;
    EnqueueRoutedWrite( display );
    REQUIRE( ResolvedSettings().gamescope.sharpness == 10 );

    // Crosshair edits its own section and writes its (stale) whole struct.
    crosshair.crosshair.line_gap = 4;
    EnqueueRoutedWrite( crosshair );
    FlushPendingWrites();
    REQUIRE( ResolvedSettings().gamescope.sharpness == 10 );  // kept
    REQUIRE( ResolvedSettings().crosshair.line_gap == 4 );    // taken
    REQUIRE( LoadProfile( "A" )->gamescope.sharpness == 10 );
    REQUIRE( LoadProfile( "A" )->crosshair.line_gap == 4 );

    // Display's next edit (its copy of crosshair is stale too) keeps the gap.
    display.gamescope.sharpness = 12;
    EnqueueRoutedWrite( display );
    FlushPendingWrites();
    REQUIRE( LoadProfile( "A" )->gamescope.sharpness == 12 );
    REQUIRE( LoadProfile( "A" )->crosshair.line_gap == 4 );

    // Reverting to a value a panel loaded with is still that panel's edit.
    display.gamescope.sharpness = 5;
    EnqueueRoutedWrite( display );
    FlushPendingWrites();
    REQUIRE( LoadProfile( "A" )->gamescope.sharpness == 5 );
    REQUIRE( LoadProfile( "A" )->crosshair.line_gap == 4 );

    // A panel that loaded BETWEEN two edits of another panel, writing for
    // the first time, does not resurrect the older value either.
    static Settings hud;
    display.gamescope.sharpness = 7;
    EnqueueRoutedWrite( display );
    hud = ResolvedSettings();          // sees 7
    display.gamescope.sharpness = 9;
    EnqueueRoutedWrite( display );
    hud.fps_display.font_size = 30.0f;
    EnqueueRoutedWrite( hud );
    FlushPendingWrites();
    REQUIRE( LoadProfile( "A" )->gamescope.sharpness == 9 );
    REQUIRE( LoadProfile( "A" )->fps_display.font_size == 30.0f );

    // And the same through an inheriting game profile: the diff is taken
    // from the merged result, so only the two edited keys are stored.
    REQUIRE( SaveProfile( Game( "CS2", "730", "A" ), *LoadProfile( "A" ) ) );
    REQUIRE( SelectProfile( "CS2" ) );
    display = ResolvedSettings();
    crosshair = ResolvedSettings();
    display.gamescope.sharpness = 11;
    EnqueueRoutedWrite( display );
    crosshair.crosshair.line_gap = 6;
    EnqueueRoutedWrite( crosshair );
    FlushPendingWrites();
    const std::string sCs2 = ReadText( ProfilePath( "CS2" ) );
    REQUIRE( sCs2.find( "\"sharpness\": 11" ) != std::string::npos );
    REQUIRE( sCs2.find( "\"line_gap\": 6" ) != std::string::npos );
    REQUIRE( sCs2.find( "\"line_length\"" ) == std::string::npos );
    REQUIRE( LoadProfile( "CS2" )->gamescope.sharpness == 11 );
    REQUIRE( LoadProfile( "CS2" )->crosshair.line_gap == 6 );
    REQUIRE( LoadProfile( "A" )->gamescope.sharpness == 9 );
}
