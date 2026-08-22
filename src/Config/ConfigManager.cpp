#include "ConfigManager.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "AppId.h"
#include "Utils/DirHelpers.h"
#include "log.hpp"

// This translation unit is the only place in the codebase that needs to know
// about nlohmann::json - see ConfigSchema.h's header comment.
//
// gamescope's project-wide -fno-exceptions build argument means every
// throwing code path below (nlohmann::json's included) compiles down to
// std::terminate()/abort() rather than something a try/catch could recover
// from - see json.hpp's own JSON_THROW/JSON_TRY macros, which auto-detect
// -fno-exceptions and switch to abort(). "Malformed JSON fails loudly and
// falls back to defaults" therefore has to mean "never reach a throwing call
// in the first place", not "catch the exception": parsing goes through
// json::parse(..., allow_exceptions=false), which returns a discarded value
// on failure instead of throwing, and every field access below is
// type-checked before extraction rather than using at()/get<T>() blind or the
// NLOHMANN_DEFINE_TYPE_* macros (which both use at()/get_to() internally).

namespace gamescope::config
{
    namespace
    {
        LogScope s_ConfigLog( "config" );

        // ---- type-checked JSON field access -------------------------------
        // Every one of these only calls nlohmann's get<T>() after confirming
        // the field's actual type matches, so none of them can hit a throwing
        // path - a wrong-typed or missing field silently falls back to
        // `def` instead of aborting the process.

        bool JGetBool( const nlohmann::json &j, const char *pszKey, bool bDefault )
        {
            if ( !j.is_object() )
                return bDefault;
            auto it = j.find( pszKey );
            if ( it == j.end() || !it->is_boolean() )
                return bDefault;
            return it->get<bool>();
        }

        int JGetInt( const nlohmann::json &j, const char *pszKey, int nDefault )
        {
            if ( !j.is_object() )
                return nDefault;
            auto it = j.find( pszKey );
            if ( it == j.end() || !it->is_number_integer() )
                return nDefault;
            return it->get<int>();
        }

        float JGetFloat( const nlohmann::json &j, const char *pszKey, float flDefault )
        {
            if ( !j.is_object() )
                return flDefault;
            auto it = j.find( pszKey );
            if ( it == j.end() || !it->is_number() )
                return flDefault;
            return it->get<float>();
        }

        std::optional<float> JGetOptFloat( const nlohmann::json &j, const char *pszKey )
        {
            if ( !j.is_object() )
                return std::nullopt;
            auto it = j.find( pszKey );
            if ( it == j.end() || !it->is_number() )
                return std::nullopt;
            return it->get<float>();
        }

        std::optional<int> JGetOptInt( const nlohmann::json &j, const char *pszKey )
        {
            if ( !j.is_object() )
                return std::nullopt;
            auto it = j.find( pszKey );
            if ( it == j.end() || !it->is_number_integer() )
                return std::nullopt;
            return it->get<int>();
        }

        std::string JGetString( const nlohmann::json &j, const char *pszKey, const std::string &sDefault )
        {
            if ( !j.is_object() )
                return sDefault;
            auto it = j.find( pszKey );
            if ( it == j.end() || !it->is_string() )
                return sDefault;
            return it->get<std::string>();
        }

        const nlohmann::json *JGetObject( const nlohmann::json &j, const char *pszKey )
        {
            if ( !j.is_object() )
                return nullptr;
            auto it = j.find( pszKey );
            if ( it == j.end() || !it->is_object() )
                return nullptr;
            return &( *it );
        }

        // ---- Settings <-> json ---------------------------------------------

        Settings SettingsFromJson( const nlohmann::json &j )
        {
            Settings s{};

            if ( const nlohmann::json *pGamescope = JGetObject( j, "gamescope" ) )
            {
                s.gamescope.filter = JGetString( *pGamescope, "filter", s.gamescope.filter );
                s.gamescope.scaler = JGetString( *pGamescope, "scaler", s.gamescope.scaler );
                s.gamescope.sharpness = JGetInt( *pGamescope, "sharpness", s.gamescope.sharpness );
                s.gamescope.vrr_enabled = JGetBool( *pGamescope, "vrr_enabled", s.gamescope.vrr_enabled );
                s.gamescope.hdr_enabled = JGetBool( *pGamescope, "hdr_enabled", s.gamescope.hdr_enabled );
                s.gamescope.tearing_enabled = JGetBool( *pGamescope, "tearing_enabled", s.gamescope.tearing_enabled );
            }

            if ( const nlohmann::json *pFps = JGetObject( j, "fps_display" ) )
            {
                s.fps_display.enabled = JGetBool( *pFps, "enabled", s.fps_display.enabled );
                s.fps_display.font_size = JGetFloat( *pFps, "font_size", s.fps_display.font_size );
                s.fps_display.backdrop_enabled = JGetBool( *pFps, "backdrop_enabled", s.fps_display.backdrop_enabled );
                s.fps_display.backdrop_opacity = JGetFloat( *pFps, "backdrop_opacity", s.fps_display.backdrop_opacity );
                s.fps_display.backdrop_rounding = JGetFloat( *pFps, "backdrop_rounding", s.fps_display.backdrop_rounding );
                s.fps_display.backdrop_padding = JGetFloat( *pFps, "backdrop_padding", s.fps_display.backdrop_padding );
                s.fps_display.blend_mode = JGetString( *pFps, "blend_mode", s.fps_display.blend_mode );
                s.fps_display.text_opacity = JGetFloat( *pFps, "text_opacity", s.fps_display.text_opacity );
                s.fps_display.graph_enabled = JGetBool( *pFps, "graph_enabled", s.fps_display.graph_enabled );
                s.fps_display.percentiles_enabled = JGetBool( *pFps, "percentiles_enabled", s.fps_display.percentiles_enabled );
                // Issue #27: placement/margins, same field-shape as OverlaySettings::notification_placement.
                s.fps_display.placement = JGetString( *pFps, "placement", s.fps_display.placement );
                s.fps_display.margin_vertical = JGetFloat( *pFps, "margin_vertical", s.fps_display.margin_vertical );
                s.fps_display.margin_horizontal = JGetFloat( *pFps, "margin_horizontal", s.fps_display.margin_horizontal );
            }

            if ( const nlohmann::json *pReshade = JGetObject( j, "reshade" ) )
            {
                if ( const nlohmann::json *pVibrancy = JGetObject( *pReshade, "vibrancy" ) )
                {
                    auto &v = s.reshade.vibrancy;
                    v.enabled = JGetBool( *pVibrancy, "enabled", v.enabled );
                    v.strength = JGetFloat( *pVibrancy, "strength", v.strength );
                    v.protect_skin_tones = JGetBool( *pVibrancy, "protect_skin_tones", v.protect_skin_tones );
                }

                if ( const nlohmann::json *pPreSharpen = JGetObject( *pReshade, "pre_sharpen" ) )
                {
                    s.reshade.pre_sharpen.enabled = JGetBool( *pPreSharpen, "enabled", s.reshade.pre_sharpen.enabled );
                    // Only overwrite the struct's compiled-in default (M6 resolved
                    // this field's TBD to 0.5f, see ConfigSchema.h) when the file
                    // actually has a number - a pre-M6 config on disk explicitly
                    // wrote "strength": null (the TBD placeholder), which must
                    // still resolve to today's real default, not stay null forever.
                    if ( std::optional<float> flStrength = JGetOptFloat( *pPreSharpen, "strength" ) )
                        s.reshade.pre_sharpen.strength = flStrength;
                }

                if ( const nlohmann::json *pAdaptive = JGetObject( *pReshade, "adaptive_brightness" ) )
                {
                    auto &ab = s.reshade.adaptive_brightness;
                    ab.enabled = JGetBool( *pAdaptive, "enabled", ab.enabled );
                    ab.target_luminance = JGetFloat( *pAdaptive, "target_luminance", ab.target_luminance );
                    ab.adapt_up_speed = JGetFloat( *pAdaptive, "adapt_up_speed", ab.adapt_up_speed );
                    ab.adapt_down_speed = JGetFloat( *pAdaptive, "adapt_down_speed", ab.adapt_down_speed );
                    ab.min_gain = JGetFloat( *pAdaptive, "min_gain", ab.min_gain );
                    ab.max_gain = JGetFloat( *pAdaptive, "max_gain", ab.max_gain );
                    ab.strength = JGetFloat( *pAdaptive, "strength", ab.strength );
                }
            }

            if ( const nlohmann::json *pOverlay = JGetObject( j, "overlay" ) )
            {
                s.overlay.fade_ms = JGetOptInt( *pOverlay, "fade_ms" );
                s.overlay.notification_placement = JGetString( *pOverlay, "notification_placement", s.overlay.notification_placement );

                // ---- window-chrome overhaul fields (see ConfigSchema.h) ----
                s.overlay.dock_scale = JGetFloat( *pOverlay, "dock_scale", s.overlay.dock_scale );
                s.overlay.display_scale = JGetFloat( *pOverlay, "display_scale", s.overlay.display_scale );
                s.overlay.notification_scale = JGetFloat( *pOverlay, "notification_scale", s.overlay.notification_scale );
                // opacity_background removed (see ConfigSchema.h) - deliberately
                // not read here any more. An old config's leftover key is simply
                // never looked up, which is exactly what "ignore an unknown field
                // gracefully" means for this named-lookup (not iterate-and-
                // validate) parse style.
                s.overlay.opacity_windows_focused = JGetFloat( *pOverlay, "opacity_windows_focused", s.overlay.opacity_windows_focused );
                s.overlay.opacity_windows_unfocused = JGetFloat( *pOverlay, "opacity_windows_unfocused", s.overlay.opacity_windows_unfocused );
                s.overlay.opacity_dock = JGetFloat( *pOverlay, "opacity_dock", s.overlay.opacity_dock );
                s.overlay.opacity_notifications = JGetFloat( *pOverlay, "opacity_notifications", s.overlay.opacity_notifications );
                s.overlay.background_blur = JGetFloat( *pOverlay, "background_blur", s.overlay.background_blur );
                s.overlay.background_darkening = JGetFloat( *pOverlay, "background_darkening", s.overlay.background_darkening );

                s.overlay.startup_announce_enabled = JGetBool( *pOverlay, "startup_announce_enabled", s.overlay.startup_announce_enabled );
                s.overlay.capture_all_keyboard_input = JGetBool( *pOverlay, "capture_all_keyboard_input", s.overlay.capture_all_keyboard_input );
                s.overlay.keyboard_navigation_enabled = JGetBool( *pOverlay, "keyboard_navigation_enabled", s.overlay.keyboard_navigation_enabled );
            }

            if ( const nlohmann::json *pNotifications = JGetObject( j, "notifications" ) )
                s.notifications.muted = JGetBool( *pNotifications, "muted", s.notifications.muted );

            if ( const nlohmann::json *pAudio = JGetObject( j, "audio" ) )
                s.audio.manual_node_binary = JGetString( *pAudio, "manual_node_binary", s.audio.manual_node_binary );

            return s;
        }

        nlohmann::json SettingsToJson( const Settings &s, bool bIncludeOverlay )
        {
            nlohmann::json jGamescope = nlohmann::json::object();
            jGamescope[ "filter" ] = s.gamescope.filter;
            jGamescope[ "scaler" ] = s.gamescope.scaler;
            jGamescope[ "sharpness" ] = s.gamescope.sharpness;
            jGamescope[ "vrr_enabled" ] = s.gamescope.vrr_enabled;
            jGamescope[ "hdr_enabled" ] = s.gamescope.hdr_enabled;
            jGamescope[ "tearing_enabled" ] = s.gamescope.tearing_enabled;

            nlohmann::json jFps = nlohmann::json::object();
            jFps[ "enabled" ] = s.fps_display.enabled;
            jFps[ "font_size" ] = s.fps_display.font_size;
            jFps[ "backdrop_enabled" ] = s.fps_display.backdrop_enabled;
            jFps[ "backdrop_opacity" ] = s.fps_display.backdrop_opacity;
            jFps[ "backdrop_rounding" ] = s.fps_display.backdrop_rounding;
            jFps[ "backdrop_padding" ] = s.fps_display.backdrop_padding;
            jFps[ "blend_mode" ] = s.fps_display.blend_mode;
            jFps[ "text_opacity" ] = s.fps_display.text_opacity;
            jFps[ "graph_enabled" ] = s.fps_display.graph_enabled;
            jFps[ "percentiles_enabled" ] = s.fps_display.percentiles_enabled;
            jFps[ "placement" ] = s.fps_display.placement;
            jFps[ "margin_vertical" ] = s.fps_display.margin_vertical;
            jFps[ "margin_horizontal" ] = s.fps_display.margin_horizontal;

            nlohmann::json jVibrancy = nlohmann::json::object();
            jVibrancy[ "enabled" ] = s.reshade.vibrancy.enabled;
            jVibrancy[ "strength" ] = s.reshade.vibrancy.strength;
            jVibrancy[ "protect_skin_tones" ] = s.reshade.vibrancy.protect_skin_tones;

            nlohmann::json jPreSharpen = nlohmann::json::object();
            jPreSharpen[ "enabled" ] = s.reshade.pre_sharpen.enabled;
            jPreSharpen[ "strength" ] = s.reshade.pre_sharpen.strength.has_value()
                ? nlohmann::json( *s.reshade.pre_sharpen.strength )
                : nlohmann::json( nullptr );

            const auto &ab = s.reshade.adaptive_brightness;
            nlohmann::json jAdaptive = nlohmann::json::object();
            jAdaptive[ "enabled" ] = ab.enabled;
            jAdaptive[ "target_luminance" ] = ab.target_luminance;
            jAdaptive[ "adapt_up_speed" ] = ab.adapt_up_speed;
            jAdaptive[ "adapt_down_speed" ] = ab.adapt_down_speed;
            jAdaptive[ "min_gain" ] = ab.min_gain;
            jAdaptive[ "max_gain" ] = ab.max_gain;
            jAdaptive[ "strength" ] = ab.strength;

            nlohmann::json jReshade = nlohmann::json::object();
            jReshade[ "vibrancy" ] = std::move( jVibrancy );
            jReshade[ "pre_sharpen" ] = std::move( jPreSharpen );
            jReshade[ "adaptive_brightness" ] = std::move( jAdaptive );

            nlohmann::json jNotifications = nlohmann::json::object();
            jNotifications[ "muted" ] = s.notifications.muted;

            nlohmann::json jAudio = nlohmann::json::object();
            jAudio[ "manual_node_binary" ] = s.audio.manual_node_binary;

            nlohmann::json j = nlohmann::json::object();
            j[ "schema_version" ] = kCurrentSchemaVersion;
            j[ "gamescope" ] = std::move( jGamescope );
            j[ "fps_display" ] = std::move( jFps );
            j[ "reshade" ] = std::move( jReshade );
            j[ "notifications" ] = std::move( jNotifications );
            j[ "audio" ] = std::move( jAudio );

            // Process-level UI preference, only ever present on global.json -
            // see ConfigSchema.h's OverlaySettings comment.
            if ( bIncludeOverlay )
            {
                nlohmann::json jOverlay = nlohmann::json::object();
                jOverlay[ "fade_ms" ] = s.overlay.fade_ms.has_value()
                    ? nlohmann::json( *s.overlay.fade_ms )
                    : nlohmann::json( nullptr );
                jOverlay[ "notification_placement" ] = s.overlay.notification_placement;

                // ---- window-chrome overhaul fields (see ConfigSchema.h) ----
                jOverlay[ "dock_scale" ] = s.overlay.dock_scale;
                jOverlay[ "display_scale" ] = s.overlay.display_scale;
                jOverlay[ "notification_scale" ] = s.overlay.notification_scale;
                jOverlay[ "opacity_windows_focused" ] = s.overlay.opacity_windows_focused;
                jOverlay[ "opacity_windows_unfocused" ] = s.overlay.opacity_windows_unfocused;
                jOverlay[ "opacity_dock" ] = s.overlay.opacity_dock;
                jOverlay[ "opacity_notifications" ] = s.overlay.opacity_notifications;
                jOverlay[ "background_blur" ] = s.overlay.background_blur;
                jOverlay[ "background_darkening" ] = s.overlay.background_darkening;

                jOverlay[ "startup_announce_enabled" ] = s.overlay.startup_announce_enabled;
                jOverlay[ "capture_all_keyboard_input" ] = s.overlay.capture_all_keyboard_input;
                jOverlay[ "keyboard_navigation_enabled" ] = s.overlay.keyboard_navigation_enabled;
                j[ "overlay" ] = std::move( jOverlay );
            }

            return j;
        }

        // ---- parsing / migration --------------------------------------------

        // No migrations exist yet - schema_version 1 is the only version that
        // has ever shipped. This is the scaffold future schema changes hang
        // off: add a migrate_N_to_N+1(nlohmann::json &) step here and run it in
        // ParseConfigFile below as versions accumulate. Deliberately built now
        // (SPEC.md's "Schema migrations" section) rather than retrofitted once
        // real files are in the wild without one.

        // Parses `sText` as JSON without ever throwing/aborting on malformed
        // input, validates schema_version, and returns std::nullopt - having
        // already logged loudly - on any failure. `svContext` is only used for
        // the log message (typically the file path).
        std::optional<nlohmann::json> ParseConfigFile( const std::string &sText, std::string_view svContext )
        {
            nlohmann::json j = nlohmann::json::parse( sText, /*callback*/ nullptr, /*allow_exceptions*/ false );
            if ( j.is_discarded() || !j.is_object() )
            {
                s_ConfigLog.errorf( "%.*s: malformed JSON, falling back to defaults",
                    (int)svContext.size(), svContext.data() );
                return std::nullopt;
            }

            int nVersion = 0;
            if ( auto it = j.find( "schema_version" ); it != j.end() && it->is_number_integer() )
                nVersion = it->get<int>();

            if ( nVersion > kCurrentSchemaVersion )
            {
                s_ConfigLog.errorf( "%.*s: schema_version %d is newer than this build understands (%d) - refusing to guess, falling back to defaults",
                    (int)svContext.size(), svContext.data(), nVersion, kCurrentSchemaVersion );
                return std::nullopt;
            }

            // nVersion < kCurrentSchemaVersion (including the "field missing
            // entirely" -> 0 case) would run the migration chain here once one
            // exists. Nothing to do yet.

            return j;
        }

        std::optional<std::string> ReadWholeFile( const std::string &sPath )
        {
            std::ifstream file( sPath, std::ios::binary );
            if ( !file.is_open() )
                return std::nullopt;

            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }

        // ---- atomic writes ---------------------------------------------------

        bool EnsureDirExists( const std::string &sDir )
        {
            std::error_code ec;
            std::filesystem::create_directories( sDir, ec );
            if ( ec )
            {
                s_ConfigLog.errorf( "failed to create directory %s: %s", sDir.c_str(), ec.message().c_str() );
                return false;
            }
            return true;
        }

        // Write-temp-then-rename: `rename()` within the same filesystem is
        // atomic on Linux, so a crash mid-write leaves either the old file
        // intact or the new one fully written, never a half-written JSON file.
        bool WriteFileAtomic( const std::string &sPath, const std::string &sContents )
        {
            std::filesystem::path path( sPath );
            std::string sDir = path.parent_path().string();
            if ( !sDir.empty() && !EnsureDirExists( sDir ) )
                return false;

            std::string sTempPath = sPath + ".tmp-" + std::to_string( (long)getpid() );

            int nFd = ::open( sTempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644 );
            if ( nFd < 0 )
            {
                s_ConfigLog.errorf( "failed to open %s for writing: %s", sTempPath.c_str(), strerror( errno ) );
                return false;
            }

            const char *pData = sContents.data();
            size_t nRemaining = sContents.size();
            bool bOk = true;
            while ( nRemaining > 0 )
            {
                ssize_t nWritten = ::write( nFd, pData, nRemaining );
                if ( nWritten < 0 )
                {
                    if ( errno == EINTR )
                        continue;
                    s_ConfigLog.errorf( "failed writing %s: %s", sTempPath.c_str(), strerror( errno ) );
                    bOk = false;
                    break;
                }
                pData += nWritten;
                nRemaining -= (size_t)nWritten;
            }

            if ( bOk && ::fsync( nFd ) != 0 )
            {
                s_ConfigLog.errorf( "fsync failed for %s: %s", sTempPath.c_str(), strerror( errno ) );
                bOk = false;
            }

            ::close( nFd );

            if ( !bOk )
            {
                ::unlink( sTempPath.c_str() );
                return false;
            }

            if ( ::rename( sTempPath.c_str(), sPath.c_str() ) != 0 )
            {
                s_ConfigLog.errorf( "failed to rename %s -> %s: %s", sTempPath.c_str(), sPath.c_str(), strerror( errno ) );
                ::unlink( sTempPath.c_str() );
                return false;
            }

            return true;
        }

        // Removes this config file's own orphaned atomic-write temp files
        // (`<sPath>.tmp-<pid>`, see WriteFileAtomic above) left behind when a
        // previous process was killed between the temp write and the rename
        // - observed for real as a `global.json.tmp-<pid>` left beside a
        // valid global.json after a test process was killed mid-write (#21).
        //
        // The temp name embeds the writing process's pid, so a file is only
        // ever removed once kill(pid, 0) reports that pid as no longer
        // running (errno == ESRCH). Anything else - the pid is alive, or we
        // simply can't tell (e.g. EPERM, a live pid owned by another user) -
        // is left alone, since the file may belong to a concurrently
        // running gamescope-ritz instance mid-write right now; deleting
        // that would corrupt a live write rather than just clean up litter.
        // Only files matching this exact "<basename>.tmp-<digits>" shape in
        // this file's own directory are ever considered.
        //
        // ponytail: a pid can in theory be reused by an unrelated process
        // between the original writer's death and this check, which would
        // make a truly stale temp file look "alive" and get skipped - the
        // file just stays as litter until a later load re-checks it once
        // that new process is also gone, not corruption (this only ever
        // skips-on-doubt, never deletes-on-doubt), so a stronger liveness
        // check (e.g. /proc/<pid>/exe identity or a pidfd) isn't needed for
        // gamescope-ritz's usage.
        void SweepStaleTempFiles( const std::string &sPath )
        {
            std::filesystem::path path( sPath );
            std::string sDir = path.parent_path().string();
            std::string sPrefix = path.filename().string() + ".tmp-";

            std::error_code ec;
            std::filesystem::directory_iterator it( sDir, ec );
            if ( ec )
                return; // directory doesn't exist yet - nothing to sweep

            for ( const std::filesystem::directory_entry &entry : it )
            {
                std::error_code ecFile;
                if ( !entry.is_regular_file( ecFile ) || ecFile )
                    continue;

                const std::string sName = entry.path().filename().string();
                if ( sName.compare( 0, sPrefix.size(), sPrefix ) != 0 )
                    continue;

                std::string sPidPart = sName.substr( sPrefix.size() );
                if ( sPidPart.empty() || sPidPart.find_first_not_of( "0123456789" ) != std::string::npos )
                    continue; // doesn't match "<prefix><digits>" exactly - not confidently ours

                errno = 0;
                char *pszEnd = nullptr;
                long lPid = std::strtol( sPidPart.c_str(), &pszEnd, 10 );
                if ( lPid <= 0 || pszEnd == sPidPart.c_str() || errno == ERANGE )
                    continue;

                errno = 0;
                if ( ::kill( (pid_t)lPid, 0 ) == 0 || errno != ESRCH )
                    continue; // pid is alive, or liveness is uncertain - may be a live writer, leave it

                std::error_code ecRemove;
                std::filesystem::remove( entry.path(), ecRemove );
                if ( ecRemove )
                {
                    s_ConfigLog.errorf( "failed to remove stale temp file %s: %s",
                        entry.path().c_str(), ecRemove.message().c_str() );
                }
                else
                {
                    s_ConfigLog.infof( "removed orphaned atomic-write temp file %s (pid %ld no longer running)",
                        entry.path().c_str(), lPid );
                }
            }
        }

        std::string DumpJson( const nlohmann::json &j )
        {
            // error_handler_t::replace: defense in depth against abort()-on-
            // invalid-UTF8 under -fno-exceptions (see this file's header
            // comment) - none of our own fields should ever contain invalid
            // UTF-8, but a sanitized profile name still originates from user
            // input.
            return j.dump( 2, ' ', false, nlohmann::json::error_handler_t::replace );
        }
    }

    // ---- paths ---------------------------------------------------------------

    std::string ConfigRoot()
    {
        const char *pszXdgConfigHome = getenv( "XDG_CONFIG_HOME" );
        std::string sBase = ( pszXdgConfigHome && *pszXdgConfigHome )
            ? std::string{ pszXdgConfigHome }
            : ( std::string{ gamescope::GetHomeDir() } + "/.config" );

        return sBase + "/gamescope-ritz";
    }

    std::string GlobalConfigPath()
    {
        return ConfigRoot() + "/global.json";
    }

    std::string ProfilesDir()
    {
        return ConfigRoot() + "/profiles";
    }

    std::string GamesDir()
    {
        return ConfigRoot() + "/games";
    }

    std::string ProfilePath( std::string_view svSanitizedName )
    {
        return ProfilesDir() + "/" + std::string{ svSanitizedName } + ".json";
    }

    std::string GamePath( std::string_view svAppId )
    {
        return GamesDir() + "/" + std::string{ svAppId } + ".json";
    }

    std::optional<std::string> SanitizeProfileName( std::string_view svName )
    {
        std::string sOut;
        sOut.reserve( svName.size() );
        for ( char c : svName )
        {
            bool bAllowed = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) ||
                ( c >= '0' && c <= '9' ) || c == ' ' || c == '_' || c == '-';
            if ( bAllowed )
                sOut.push_back( c );
        }

        size_t nStart = sOut.find_first_not_of( ' ' );
        if ( nStart == std::string::npos )
            return std::nullopt;
        size_t nEnd = sOut.find_last_not_of( ' ' );
        sOut = sOut.substr( nStart, nEnd - nStart + 1 );

        if ( sOut.empty() || sOut == "." || sOut == ".." )
            return std::nullopt;

        constexpr size_t kMaxLength = 100;
        if ( sOut.size() > kMaxLength )
            sOut.resize( kMaxLength );

        return sOut;
    }

    // ---- loading ---------------------------------------------------------------

    Settings LoadGlobal()
    {
        std::string sPath = GlobalConfigPath();
        SweepStaleTempFiles( sPath );
        std::optional<std::string> oText = ReadWholeFile( sPath );
        if ( !oText )
            return Settings{};

        std::optional<nlohmann::json> oJson = ParseConfigFile( *oText, sPath );
        if ( !oJson )
            return Settings{};

        return SettingsFromJson( *oJson );
    }

    std::optional<Settings> LoadProfile( std::string_view svSanitizedName )
    {
        std::string sPath = ProfilePath( svSanitizedName );
        SweepStaleTempFiles( sPath );
        std::optional<std::string> oText = ReadWholeFile( sPath );
        if ( !oText )
            return std::nullopt;

        std::optional<nlohmann::json> oJson = ParseConfigFile( *oText, sPath );
        if ( !oJson )
            return std::nullopt;

        return SettingsFromJson( *oJson );
    }

    std::optional<Settings> LoadPerGameOverride( std::string_view svAppId )
    {
        std::string sPath = GamePath( svAppId );
        SweepStaleTempFiles( sPath );
        std::optional<std::string> oText = ReadWholeFile( sPath );
        if ( !oText )
            return std::nullopt;

        std::optional<nlohmann::json> oJson = ParseConfigFile( *oText, sPath );
        if ( !oJson )
            return std::nullopt; // parse failure -> behave as if override_global were off

        if ( !JGetBool( *oJson, "override_global", false ) )
            return std::nullopt;

        return SettingsFromJson( *oJson );
    }

    Settings ResolveEffective( const std::optional<std::string> &oAppId )
    {
        if ( oAppId )
        {
            if ( std::optional<Settings> oGame = LoadPerGameOverride( *oAppId ) )
                return *oGame;
        }
        return LoadGlobal();
    }

    // ---- saving ---------------------------------------------------------------

    bool SaveGlobal( const Settings &settings )
    {
        return WriteFileAtomic( GlobalConfigPath(), DumpJson( SettingsToJson( settings, /*bIncludeOverlay*/ true ) ) );
    }

    bool SaveProfile( std::string_view svSanitizedName, const Settings &settings )
    {
        nlohmann::json j = SettingsToJson( settings, /*bIncludeOverlay*/ false );
        j[ "name" ] = std::string{ svSanitizedName };
        return WriteFileAtomic( ProfilePath( svSanitizedName ), DumpJson( j ) );
    }

    bool SnapshotPerGameOverride( std::string_view svAppId, const Settings &snapshot )
    {
        nlohmann::json j = SettingsToJson( snapshot, /*bIncludeOverlay*/ false );
        j[ "override_global" ] = true;
        return WriteFileAtomic( GamePath( svAppId ), DumpJson( j ) );
    }

    namespace
    {
        // Reads games/<AppId>.json regardless of its own override_global
        // flag and returns the parsed object - unlike LoadPerGameOverride
        // (ConfigSchema-typed, gated on the flag), this is "is there
        // anything on disk to restore/deactivate/delete at all", which
        // ClearPerGameOverride/RestorePerGameOverride/HasSavedPerGameConfig
        // below all need to answer without caring whether it's currently
        // active.
        std::optional<nlohmann::json> ReadGameFileJson( std::string_view svAppId )
        {
            std::string sPath = GamePath( svAppId );
            std::optional<std::string> oText = ReadWholeFile( sPath );
            if ( !oText )
                return std::nullopt;
            return ParseConfigFile( *oText, sPath );
        }
    }

    bool ClearPerGameOverride( std::string_view svAppId )
    {
        std::optional<nlohmann::json> oJson = ReadGameFileJson( svAppId );
        if ( !oJson )
            return true; // nothing on disk - nothing to deactivate

        ( *oJson )[ "override_global" ] = false;
        return WriteFileAtomic( GamePath( svAppId ), DumpJson( *oJson ) );
    }

    bool HasSavedPerGameConfig( std::string_view svAppId )
    {
        return ReadGameFileJson( svAppId ).has_value();
    }

    bool RestorePerGameOverride( std::string_view svAppId )
    {
        std::optional<nlohmann::json> oJson = ReadGameFileJson( svAppId );
        if ( !oJson )
            return false;

        ( *oJson )[ "override_global" ] = true;
        return WriteFileAtomic( GamePath( svAppId ), DumpJson( *oJson ) );
    }

    bool DeletePerGameOverride( std::string_view svAppId )
    {
        // Bare-id guard: no path separator, and not "." / ".." - mirrors
        // SanitizeProfileName's profiles/ containment in spirit (see
        // ConfigManager.h's comment on this function).
        if ( svAppId.empty() || svAppId.find( '/' ) != std::string_view::npos ||
            svAppId == "." || svAppId == ".." )
        {
            s_ConfigLog.errorf( "DeletePerGameOverride: refusing suspicious app id '%.*s'",
                (int)svAppId.size(), svAppId.data() );
            return false;
        }

        std::filesystem::path path( GamePath( svAppId ) );
        // Containment check: the path this function is about to remove must
        // resolve to a direct child of GamesDir(), never anything else -
        // GamePath() can only ever produce that shape given the guard above,
        // but this is checked again anyway so the delete path never relies
        // on a single layer of defense.
        if ( path.parent_path() != std::filesystem::path( GamesDir() ) )
            return false;

        std::error_code ec;
        std::filesystem::remove( path, ec );
        return !ec || ec == std::errc::no_such_file_or_directory;
    }

    bool ApplyProfile( Settings &target, std::string_view svSanitizedName )
    {
        std::optional<Settings> oProfile = LoadProfile( svSanitizedName );
        if ( !oProfile )
            return false;

        // One-time copy (DECISIONS.md #20) - not a live reference. `overlay` is
        // a process-level preference, not part of a profile's shape, so it's
        // deliberately left untouched on `target`. `audio.manual_node_binary`
        // is deliberately left untouched too - it names one specific game's
        // process, so copying it in from a profile (meant to be reusable
        // across different games) would silently point volume control at the
        // wrong process for every other game the profile is applied to.
        target.gamescope = oProfile->gamescope;
        target.fps_display = oProfile->fps_display;
        target.reshade = oProfile->reshade;
        target.notifications = oProfile->notifications;

        return true;
    }

    // ---- background writer ---------------------------------------------------

    namespace
    {
        struct PendingWrite
        {
            std::string sPath;
            std::string sContents;
        };

        // A small one-shot background writer, mirroring the shape of
        // gamescope's other dedicated small subsystem threads (e.g.
        // pipewire.cpp's capture thread) - exists so config writes triggered
        // from the steamcompmgr thread never block on fsync()/rename() inline
        // (SPEC.md's threading section: a stall there shows up as a
        // dropped/late frame).
        class ConfigWriter
        {
        public:
            static ConfigWriter &Instance()
            {
                // Deliberately leaked (never destroyed) - construct-on-first-use
                // with no static destructor to run. A normal function-local
                // static's destructor runs at exit-time via __cxa_atexit, racing
                // against the still-live background thread's own teardown; that
                // race hung the process in practice (observed exiting the test
                // binary). This object is meant to live for the whole process
                // anyway (see the ThreadMain comment below), so simply never
                // destroying it sidesteps the ordering hazard entirely.
                static ConfigWriter *s_pInstance = new ConfigWriter();
                return *s_pInstance;
            }

            void Enqueue( std::string sPath, std::string sContents )
            {
                {
                    std::lock_guard<std::mutex> lock( m_Mutex );
                    m_Pending.push_back( PendingWrite{ std::move( sPath ), std::move( sContents ) } );
                }
                m_Cv.notify_all();
            }

            void Flush()
            {
                std::unique_lock<std::mutex> lock( m_Mutex );
                m_Cv.wait( lock, [this]() { return m_Pending.empty() && !m_bWriting; } );
            }

        private:
            // ponytail: never joined - detached immediately. This lives for the
            // process's lifetime, same as gamescope's other small
            // dedicated-thread subsystems - fine for a compositor process whose
            // threads the OS reclaims on exit. A joinable std::thread destroyed
            // without join()/detach() calls std::terminate(), which a
            // function-local static's exit-time destructor would otherwise hit
            // here; detaching avoids that outright. Add Shutdown()/join() if
            // this ever needs orderly teardown (e.g. a future test harness
            // spinning many of these up).
            ConfigWriter()
                : m_Thread( [this]() { ThreadMain(); } )
            {
                m_Thread.detach();
            }
            void ThreadMain()
            {
                std::unique_lock<std::mutex> lock( m_Mutex );
                for ( ;; )
                {
                    m_Cv.wait( lock, [this]() { return !m_Pending.empty(); } );

                    std::vector<PendingWrite> batch;
                    batch.swap( m_Pending );
                    m_bWriting = true;

                    lock.unlock();
                    for ( const PendingWrite &write : batch )
                        WriteFileAtomic( write.sPath, write.sContents );
                    lock.lock();

                    m_bWriting = false;
                    m_Cv.notify_all();
                }
            }

            std::thread m_Thread;
            std::mutex m_Mutex;
            std::condition_variable m_Cv;
            std::vector<PendingWrite> m_Pending;
            bool m_bWriting = false;
        };
    }

    namespace
    {
        // In-process mirror of the most recently *known-good* `overlay`
        // sub-object, updated synchronously (no disk round trip) by every
        // EnqueueGlobalWrite() call below. Exists so EnqueueRoutedWrite()'s
        // global-write branch (further down) can pull a fresh `overlay`
        // without racing the background ConfigWriter thread: a disk read
        // right before enqueueing looks fresh but isn't, if an
        // just-enqueued-but-not-yet-flushed overlay write from the same
        // frame hasn't hit disk yet -- reading this in-memory value instead
        // always reflects the latest enqueued write instantly, flushed or
        // not.
        bool s_bLastKnownOverlayLoaded = false;
        OverlaySettings s_LastKnownOverlay;

        const OverlaySettings &CurrentOverlaySettings()
        {
            if ( !s_bLastKnownOverlayLoaded )
            {
                // First call this process (nothing has written global.json's
                // overlay yet this session, e.g. the very first edit the
                // user makes is on a non-General tab) -- fall back to a
                // one-time disk read, same as before this cache existed.
                s_LastKnownOverlay = LoadGlobal().overlay;
                s_bLastKnownOverlayLoaded = true;
            }
            return s_LastKnownOverlay;
        }
    }

    void EnqueueGlobalWrite( Settings settings )
    {
        s_LastKnownOverlay = settings.overlay;
        s_bLastKnownOverlayLoaded = true;
        ConfigWriter::Instance().Enqueue( GlobalConfigPath(), DumpJson( SettingsToJson( settings, /*bIncludeOverlay*/ true ) ) );
    }

    void EnqueuePerGameSnapshot( std::string sAppId, Settings snapshot )
    {
        nlohmann::json j = SettingsToJson( snapshot, /*bIncludeOverlay*/ false );
        j[ "override_global" ] = true;
        ConfigWriter::Instance().Enqueue( GamePath( sAppId ), DumpJson( j ) );
    }

    void EnqueueProfileWrite( std::string sSanitizedName, Settings settings )
    {
        nlohmann::json j = SettingsToJson( settings, /*bIncludeOverlay*/ false );
        j[ "name" ] = sSanitizedName;
        ConfigWriter::Instance().Enqueue( ProfilePath( sSanitizedName ), DumpJson( j ) );
    }

    void FlushPendingWrites()
    {
        ConfigWriter::Instance().Flush();
    }

    // ---- directory listings ---------------------------------------------------

    namespace
    {
        std::vector<std::string> ListJsonStems( const std::string &sDir )
        {
            std::vector<std::string> out;
            std::error_code ec;
            std::filesystem::directory_iterator it( sDir, ec );
            if ( ec )
                return out; // directory doesn't exist yet - empty, not an error

            for ( const std::filesystem::directory_entry &entry : it )
            {
                std::error_code ecFile;
                if ( !entry.is_regular_file( ecFile ) || ecFile )
                    continue;
                const std::filesystem::path &path = entry.path();
                if ( path.extension() != ".json" )
                    continue;
                out.push_back( path.stem().string() );
            }
            std::sort( out.begin(), out.end() );
            return out;
        }
    }

    std::vector<std::string> ListProfiles()
    {
        return ListJsonStems( ProfilesDir() );
    }

    std::vector<std::string> ListGameIds()
    {
        return ListJsonStems( GamesDir() );
    }

    // ---- session routing -------------------------------------------------------

    namespace
    {
        std::optional<std::string> s_oSessionAppId;
        bool s_bSessionAppIdResolved = false;
        bool s_bSessionOverrideActive = false;
        bool s_bSessionOverrideResolved = false;
        uint64_t s_ulConfigGeneration = 0;
    }

    const std::optional<std::string> &SessionAppId()
    {
        if ( !s_bSessionAppIdResolved )
        {
            s_oSessionAppId = ResolveAppId();
            s_bSessionAppIdResolved = true;
        }
        return s_oSessionAppId;
    }

    bool IsSessionOverrideActive()
    {
        if ( !s_bSessionOverrideResolved )
        {
            const std::optional<std::string> &oAppId = SessionAppId();
            s_bSessionOverrideActive = oAppId.has_value() && LoadPerGameOverride( *oAppId ).has_value();
            s_bSessionOverrideResolved = true;
        }
        return s_bSessionOverrideActive;
    }

    void SetSessionOverrideActive( bool bActive )
    {
        s_bSessionOverrideActive = bActive;
        s_bSessionOverrideResolved = true;
    }

    uint64_t ConfigGeneration()
    {
        return s_ulConfigGeneration;
    }

    void BumpConfigGeneration()
    {
        s_ulConfigGeneration++;
    }

    void EnqueueRoutedWrite( const Settings &settings )
    {
        const std::optional<std::string> &oAppId = SessionAppId();
        if ( oAppId.has_value() && IsSessionOverrideActive() )
        {
            EnqueuePerGameSnapshot( *oAppId, settings );
            return;
        }

        // global.json is the one file every panel can end up writing to
        // (whenever no per-game override is active), but `overlay` is
        // deliberately process-level/General-tab-owned (ConfigSchema.h's
        // OverlaySettings comment: "process-level and global.json-only").
        // No caller of EnqueueRoutedWrite() owns that field -- PanelConfig's
        // General tab persists overlay edits through EnqueueGlobalWrite()
        // directly (PanelConfig.cpp's QueueGeneralSave()), never through
        // here. Every OTHER panel's cached `settings.overlay` here is
        // whatever it happened to load at panel-open time, which goes stale
        // the instant the General tab writes a change: a General-tab edit
        // deliberately never bumps ConfigGeneration (see
        // EnsureGeneralSettingsLoaded()'s own comment), so nothing reloads
        // these callers' caches. Forwarding that stale `overlay` straight
        // through used to silently overwrite every General-tab change on
        // the very next unrelated routed write from any other panel --
        // "changed General settings, they don't stick" was this exact bug.
        // Fix: substitute in the freshest known `overlay` (see
        // CurrentOverlaySettings() above) immediately before writing,
        // instead of forwarding this caller's own stale copy.
        Settings toWrite = settings;
        toWrite.overlay = CurrentOverlaySettings();
        EnqueueGlobalWrite( std::move( toWrite ) );
    }

    void ResetSessionRoutingForTests()
    {
        s_oSessionAppId.reset();
        s_bSessionAppIdResolved = false;
        s_bSessionOverrideActive = false;
        s_bSessionOverrideResolved = false;
        s_ulConfigGeneration = 0;
        // CurrentOverlaySettings()'s cache (above) is process-wide, same
        // hazard every other piece of session-routing state here has:
        // catch2 runs every [config] TEST_CASE in one shared process, each
        // against its own fresh TempConfigHome, so a value cached against a
        // prior test's (already-deleted) temp directory must not leak into
        // the next one.
        s_bLastKnownOverlayLoaded = false;
    }

    std::string DebugDumpEffective( const std::optional<std::string> &oAppId )
    {
        bool bPerGame = oAppId.has_value() && LoadPerGameOverride( *oAppId ).has_value();
        Settings effective = ResolveEffective( oAppId );

        nlohmann::json j = nlohmann::json::object();
        j[ "resolved_app_id" ] = oAppId.has_value() ? nlohmann::json( *oAppId ) : nlohmann::json( nullptr );
        j[ "source" ] = bPerGame ? "per-game override" : "global";
        j[ "settings" ] = SettingsToJson( effective, /*bIncludeOverlay*/ !bPerGame );

        return DumpJson( j );
    }
}
