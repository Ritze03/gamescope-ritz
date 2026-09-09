#include "ConfigManager.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
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
                s.gamescope.fps_limit = JGetInt( *pGamescope, "fps_limit", s.gamescope.fps_limit );
                s.gamescope.force_grab_cursor = JGetBool( *pGamescope, "force_grab_cursor", s.gamescope.force_grab_cursor );
                s.gamescope.force_windows_fullscreen = JGetBool( *pGamescope, "force_windows_fullscreen", s.gamescope.force_windows_fullscreen );
                s.gamescope.sdr_gamut_wideness = JGetFloat( *pGamescope, "sdr_gamut_wideness", s.gamescope.sdr_gamut_wideness );
                s.gamescope.sdr_on_hdr_brightness_nits = JGetFloat( *pGamescope, "sdr_on_hdr_brightness_nits", s.gamescope.sdr_on_hdr_brightness_nits );
                s.gamescope.hdr_input_gain = JGetFloat( *pGamescope, "hdr_input_gain", s.gamescope.hdr_input_gain );
                s.gamescope.sdr_input_gain = JGetFloat( *pGamescope, "sdr_input_gain", s.gamescope.sdr_input_gain );
                // Item 7 (2026-09-05): absent on every file written before
                // these existed -- the struct's 0 ("as launched") is exactly
                // the right fallback.
                s.gamescope.nested_width = JGetInt( *pGamescope, "nested_width", s.gamescope.nested_width );
                s.gamescope.nested_height = JGetInt( *pGamescope, "nested_height", s.gamescope.nested_height );
                s.gamescope.nested_refresh_hz = JGetInt( *pGamescope, "nested_refresh_hz", s.gamescope.nested_refresh_hz );
                s.gamescope.nested_lock_aspect = JGetBool( *pGamescope, "nested_lock_aspect", s.gamescope.nested_lock_aspect );
            }

            if ( const nlohmann::json *pFps = JGetObject( j, "fps_display" ) )
            {
                s.fps_display.enabled = JGetBool( *pFps, "enabled", s.fps_display.enabled );
                s.fps_display.font_size = JGetFloat( *pFps, "font_size", s.fps_display.font_size );
                s.fps_display.backdrop_opacity = JGetFloat( *pFps, "backdrop_opacity", s.fps_display.backdrop_opacity );
                s.fps_display.backdrop_padding = JGetFloat( *pFps, "backdrop_padding", s.fps_display.backdrop_padding );
                s.fps_display.text_opacity = JGetFloat( *pFps, "text_opacity", s.fps_display.text_opacity );
                // fps_enabled/fps_label_enabled removed 2026-09-03 -- `enabled`
                // above already covers the module, and the unit-label suffix
                // is gone entirely (the readout is a bare integer), see
                // ConfigSchema.h's own comment and this repo's CHANGELOG.md.
                // graph_enabled/percentiles_enabled/cpu_enabled/gpu_enabled/
                // media_enabled/frametime_enabled/color_cpu/color_gpu/
                // color_media/layout_name removed 2026-09-03 with the
                // perf-stats modules and the named-layout system
                // (superdoc/meta/TERMINOLOGY.md's "profiler" entry) --
                // deliberately not read here any more, same precedent as
                // dock_scale/opacity_background's own removal below.
                // backdrop_enabled/backdrop_rounding/blend_mode removed
                // Phase 2 (2026-09-03, see ConfigSchema.h's own comment) --
                // same "just stop reading an old key" precedent, not a
                // migration: an old config with backdrop_enabled=false and
                // a nonzero backdrop_opacity now shows that backdrop, which
                // is a deliberate behaviour change (documented in
                // CHANGELOG.md), not an oversight.
                s.fps_display.color_fps = JGetOptInt( *pFps, "color_fps" );
                // Placement (scope reduction 2026-09-03): 9-point anchor +
                // pixel margins, see ConfigSchema.h's own comment.
                s.fps_display.anchor = JGetString( *pFps, "anchor", s.fps_display.anchor );
                s.fps_display.margin_x = JGetInt( *pFps, "margin_x", s.fps_display.margin_x );
                s.fps_display.margin_y = JGetInt( *pFps, "margin_y", s.fps_display.margin_y );
                // Phase 2 (2026-09-03): update mode, hide-above-X, text
                // colour mode, outline strength, lag detection -- see
                // ConfigSchema.h.
                s.fps_display.update_mode = JGetString( *pFps, "update_mode", s.fps_display.update_mode );
                s.fps_display.hide_above_enabled = JGetBool( *pFps, "hide_above_enabled", s.fps_display.hide_above_enabled );
                s.fps_display.hide_above_fps = JGetFloat( *pFps, "hide_above_fps", s.fps_display.hide_above_fps );
                s.fps_display.color_mode = JGetString( *pFps, "color_mode", s.fps_display.color_mode );
                // "shadow_strength" (the drop shadow the outline replaced
                // 2026-09-03) is deliberately not read: an old config
                // carrying it falls back to outline_strength's default,
                // the same "just stop reading an old key" precedent the
                // removed backdrop keys above follow.
                s.fps_display.outline_strength = JGetFloat( *pFps, "outline_strength", s.fps_display.outline_strength );
                s.fps_display.lag_detection_enabled = JGetBool( *pFps, "lag_detection_enabled", s.fps_display.lag_detection_enabled );
            }

            if ( const nlohmann::json *pCross = JGetObject( j, "crosshair" ) )
            {
                auto &c = s.crosshair;
                c.enabled = JGetBool( *pCross, "enabled", c.enabled );
                c.line_enabled = JGetBool( *pCross, "line_enabled", c.line_enabled );
                c.line_length = JGetInt( *pCross, "line_length", c.line_length );
                c.line_width = JGetInt( *pCross, "line_width", c.line_width );
                c.line_gap = JGetInt( *pCross, "line_gap", c.line_gap );
                c.line_color = JGetInt( *pCross, "line_color", c.line_color );
                c.line_opacity = JGetFloat( *pCross, "line_opacity", c.line_opacity );
                c.dot_enabled = JGetBool( *pCross, "dot_enabled", c.dot_enabled );
                c.dot_size = JGetInt( *pCross, "dot_size", c.dot_size );
                c.dot_color = JGetInt( *pCross, "dot_color", c.dot_color );
                c.dot_opacity = JGetFloat( *pCross, "dot_opacity", c.dot_opacity );
                c.outline_enabled = JGetBool( *pCross, "outline_enabled", c.outline_enabled );
                c.outline_width = JGetInt( *pCross, "outline_width", c.outline_width );
                c.outline_opacity = JGetFloat( *pCross, "outline_opacity", c.outline_opacity );
                c.outline_color = JGetInt( *pCross, "outline_color", c.outline_color );
                c.hide_on_right_click = JGetBool( *pCross, "hide_on_right_click", c.hide_on_right_click );
                c.hide_mode = JGetString( *pCross, "hide_mode", c.hide_mode );
                c.hide_time_ms = JGetInt( *pCross, "hide_time_ms", c.hide_time_ms );
                c.hide_animate_back = JGetBool( *pCross, "hide_animate_back", c.hide_animate_back );
                c.apply_scaling = JGetBool( *pCross, "apply_scaling", c.apply_scaling );
            }

            if ( const nlohmann::json *pReshade = JGetObject( j, "reshade" ) )
            {
                // Renamed from "vibrancy" 2026-09-08 (ConfigSchema.h's
                // kCurrentSchemaVersion 3->4 comment). Any raw JSON reaching
                // this function has already been through ParseConfigFile's
                // migration chain (Migrate_3_to_4 renamed an old file's
                // "vibrancy" object to "saturation" before this runs), so
                // only the new key needs reading here -- same pattern as
                // Migrate_1_to_2's value transform not needing a fallback
                // read either.
                if ( const nlohmann::json *pSaturation = JGetObject( *pReshade, "saturation" ) )
                {
                    auto &v = s.reshade.saturation;
                    v.enabled = JGetBool( *pSaturation, "enabled", v.enabled );
                    v.strength = JGetFloat( *pSaturation, "strength", v.strength );
                    v.protect_skin_tones = JGetBool( *pSaturation, "protect_skin_tones", v.protect_skin_tones );
                }

                // NEW 2026-09-08: the "punchy colours punchier" effect --
                // see ConfigSchema.h's ReshadeVibrancySettings. Additive key;
                // an old config has none and resolves to these defaults
                // (off, strength 0.0).
                if ( const nlohmann::json *pVibrancy = JGetObject( *pReshade, "vibrancy" ) )
                {
                    auto &vb = s.reshade.vibrancy;
                    vb.enabled = JGetBool( *pVibrancy, "enabled", vb.enabled );
                    vb.strength = JGetFloat( *pVibrancy, "strength", vb.strength );
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

                // NEW 2026-09-08: Bloom -- see ConfigSchema.h's
                // ReshadeBloomSettings. Additive keys; an old config has
                // none and resolves to the compiled-in defaults (off).
                if ( const nlohmann::json *pBloom = JGetObject( *pReshade, "bloom" ) )
                {
                    auto &bl = s.reshade.bloom;
                    bl.enabled = JGetBool( *pBloom, "enabled", bl.enabled );
                    bl.threshold = JGetFloat( *pBloom, "threshold", bl.threshold );
                    bl.intensity = JGetFloat( *pBloom, "intensity", bl.intensity );
                    bl.radius = JGetFloat( *pBloom, "radius", bl.radius );
                }

                if ( const nlohmann::json *pAdaptive = JGetObject( *pReshade, "adaptive_brightness" ) )
                {
                    auto &ab = s.reshade.adaptive_brightness;
                    ab.enabled = JGetBool( *pAdaptive, "enabled", ab.enabled );
                    ab.mode = JGetString( *pAdaptive, "mode", ab.mode );
                    if ( ab.mode != "dynamic" )
                        ab.mode = "whole_image";
                    ab.target_luminance = JGetFloat( *pAdaptive, "target_luminance", ab.target_luminance );
                    ab.adapt_up_speed = JGetFloat( *pAdaptive, "adapt_up_speed", ab.adapt_up_speed );
                    ab.adapt_down_speed = JGetFloat( *pAdaptive, "adapt_down_speed", ab.adapt_down_speed );
                    ab.min_gain = JGetFloat( *pAdaptive, "min_gain", ab.min_gain );
                    ab.max_gain = JGetFloat( *pAdaptive, "max_gain", ab.max_gain );
                    ab.strength = JGetFloat( *pAdaptive, "strength", ab.strength );
                    ab.local_strength = JGetFloat( *pAdaptive, "local_strength", ab.local_strength );
                }

                // NEW 2026-09-08: Adaptive Gamma -- see ConfigSchema.h's
                // ReshadeAdaptiveGammaSettings. Additive keys; an old config
                // has none and resolves to the compiled-in defaults.
                if ( const nlohmann::json *pAdaptiveGamma = JGetObject( *pReshade, "adaptive_gamma" ) )
                {
                    auto &ag = s.reshade.adaptive_gamma;
                    ag.enabled = JGetBool( *pAdaptiveGamma, "enabled", ag.enabled );
                    ag.target_luminance = JGetFloat( *pAdaptiveGamma, "target_luminance", ag.target_luminance );
                    ag.max_lift = JGetFloat( *pAdaptiveGamma, "max_lift", ag.max_lift );
                    ag.max_darken = JGetFloat( *pAdaptiveGamma, "max_darken", ag.max_darken );
                    ag.strength = JGetFloat( *pAdaptiveGamma, "strength", ag.strength );
                    ag.local_strength = JGetFloat( *pAdaptiveGamma, "local_strength", ag.local_strength );
                }

                if ( const nlohmann::json *pShadowLift = JGetObject( *pReshade, "shadow_lift" ) )
                {
                    auto &sl = s.reshade.shadow_lift;
                    sl.enabled = JGetBool( *pShadowLift, "enabled", sl.enabled );
                    sl.strength = JGetFloat( *pShadowLift, "strength", sl.strength );
                }
            }

            if ( const nlohmann::json *pOverlay = JGetObject( j, "overlay" ) )
            {
                s.overlay.fade_ms = JGetOptInt( *pOverlay, "fade_ms" );
                s.overlay.notification_placement = JGetString( *pOverlay, "notification_placement", s.overlay.notification_placement );

                // ---- window-chrome overhaul fields (see ConfigSchema.h) ----
                // dock_scale removed 2026-08-24 with the dock itself (see
                // ConfigSchema.h) - deliberately not read here any more, the
                // same way opacity_background's removal is handled below.
                s.overlay.display_scale = JGetFloat( *pOverlay, "display_scale", s.overlay.display_scale );
                s.overlay.notification_scale = JGetFloat( *pOverlay, "notification_scale", s.overlay.notification_scale );
                // opacity_background removed (see ConfigSchema.h) - deliberately
                // not read here any more. An old config's leftover key is simply
                // never looked up, which is exactly what "ignore an unknown field
                // gracefully" means for this named-lookup (not iterate-and-
                // validate) parse style.
                // opacity_windows_focused/unfocused and opacity_dock removed
                // 2026-09-06 (see ConfigSchema.h) - deliberately not read
                // here any more, same as opacity_background above. An old
                // config's leftover keys are simply never looked up.
                s.overlay.window_opacity = JGetFloat( *pOverlay, "window_opacity", s.overlay.window_opacity );
                s.overlay.opacity_notifications = JGetFloat( *pOverlay, "opacity_notifications", s.overlay.opacity_notifications );
                s.overlay.accent_hue = JGetFloat( *pOverlay, "accent_hue", s.overlay.accent_hue );
                s.overlay.background_blur = JGetFloat( *pOverlay, "background_blur", s.overlay.background_blur );
                s.overlay.background_darkening = JGetFloat( *pOverlay, "background_darkening", s.overlay.background_darkening );

                s.overlay.startup_announce_enabled = JGetBool( *pOverlay, "startup_announce_enabled", s.overlay.startup_announce_enabled );
                s.overlay.capture_all_keyboard_input = JGetBool( *pOverlay, "capture_all_keyboard_input", s.overlay.capture_all_keyboard_input );
                s.overlay.keyboard_navigation_enabled = JGetBool( *pOverlay, "keyboard_navigation_enabled", s.overlay.keyboard_navigation_enabled );
                s.overlay.profiles_filter_other_games = JGetBool( *pOverlay, "profiles_filter_other_games", s.overlay.profiles_filter_other_games );

                // Issue #35: per-panel saved window geometry (ConfigSchema.h's
                // PanelGeometry/OverlaySettings::panel_geometry comments).
                // Iterates every key present rather than a fixed named-lookup
                // list (every other field above) precisely because the key
                // set here is open-ended and forward/backward compatible by
                // design - an entry whose key this build's PanelId no longer
                // has a string for (a since-renamed or since-removed panel)
                // is parsed into the map exactly like any other and simply
                // never looked up again, not an error, and does not affect
                // any other entry in the map.
                if ( const nlohmann::json *pGeometry = JGetObject( *pOverlay, "panel_geometry" ) )
                {
                    for ( auto it = pGeometry->begin(); it != pGeometry->end(); ++it )
                    {
                        if ( !it->is_object() )
                            continue; // malformed entry for this one key - skip it, not the whole map

                        PanelGeometry g;
                        g.x = JGetFloat( *it, "x", g.x );
                        g.y = JGetFloat( *it, "y", g.y );
                        g.w = JGetFloat( *it, "w", g.w );
                        g.h = JGetFloat( *it, "h", g.h );
                        // Issue #47: absent on any file written before this
                        // field existed -- g's own default (1.0f) is exactly
                        // the right fallback there (PanelGeometry::scale's
                        // comment explains why).
                        g.scale = JGetFloat( *it, "scale", g.scale );

                        // A non-positive size is never valid saved geometry
                        // (PanelGeometry{}'s own 0,0 default, or a corrupt/
                        // hand-edited entry) - skip it so Chrome.cpp's
                        // TiledDefaultPos()/measured-size fallback applies to
                        // that one panel instead of opening a zero/negative-
                        // size window. Position is deliberately NOT bounds-
                        // checked here - Chrome.cpp's existing on-screen
                        // clamp (#31) already pulls an out-of-bounds saved
                        // position back on screen every frame, including the
                        // first, so re-validating it here would just be a
                        // second, redundant copy of that same policy.
                        if ( g.w > 0.0f && g.h > 0.0f )
                            s.overlay.panel_geometry[ it.key() ] = g;
                    }
                }

                // system_monitor_tab removed 2026-09-03 (see ConfigSchema.h)
                // -- deliberately not read here any more.

                // Cursor tab (ConfigSchema.h's OverlaySettings comment block).
                s.overlay.cursor_scale = JGetFloat( *pOverlay, "cursor_scale", s.overlay.cursor_scale );
                s.overlay.cursor_outline_width = JGetFloat( *pOverlay, "cursor_outline_width", s.overlay.cursor_outline_width );
                s.overlay.cursor_outline_color = JGetOptInt( *pOverlay, "cursor_outline_color" );
                s.overlay.cursor_inlay_color = JGetInt( *pOverlay, "cursor_inlay_color", s.overlay.cursor_inlay_color );
                s.overlay.cursor_everywhere = JGetBool( *pOverlay, "cursor_everywhere", s.overlay.cursor_everywhere );
                s.overlay.cursor_override_game = JGetBool( *pOverlay, "cursor_override_game", s.overlay.cursor_override_game );

                // Keybinds (2026-09-08) -- action id -> chord string. Only the
                // actions the user changed are on disk (ConfigSchema.h), so an
                // absent object is the normal, fresh-install case rather than
                // an error. Nothing is validated here: this layer does not own
                // the chord grammar, and src/Keybinds.cpp's ApplyFromConfig()
                // is what falls a bad or conflicting entry back to its default
                // -- so a hand-edited file loads instead of failing, and the
                // string round-trips untouched if this build does not know the
                // action it names.
                if ( const nlohmann::json *pKeybinds = JGetObject( *pOverlay, "keybinds" ) )
                {
                    for ( auto it = pKeybinds->begin(); it != pKeybinds->end(); ++it )
                    {
                        if ( it->is_string() )
                            s.overlay.keybinds[ it.key() ] = it->get<std::string>();
                    }
                }

                s.overlay.friends_lookup_names = JGetBool( *pOverlay, "friends_lookup_names", s.overlay.friends_lookup_names );
            }

            if ( const nlohmann::json *pNotifications = JGetObject( j, "notifications" ) )
                s.notifications.muted = JGetBool( *pNotifications, "muted", s.notifications.muted );

            // System tab (item 5, Phase B) -- see ConfigSchema.h's
            // SystemSettings. Absent on older files: default (sync on).
            if ( const nlohmann::json *pSystem = JGetObject( j, "system" ) )
                s.system.clipboard_sync = JGetBool( *pSystem, "clipboard_sync", s.system.clipboard_sync );

            // No audio / last_applied_profile / active_profile /
            // auto_save_profile: schema 3 dropped them (ConfigSchema.h's
            // Settings comment). An old file's keys are simply never looked
            // up -- the migration reads the ones it needs itself.

            return s;
        }

        // The per-layer sections, exactly as a profile file stores them
        // (no schema_version, no metadata -- ProfileFileJson() adds those).
        // This serializer emits the struct's fields, so an old file's
        // leftover keys for a removed feature are dropped the first time
        // anything rewrites it -- the accepted precedent since dock_scale.
        nlohmann::json SectionsToJson( const Settings &s )
        {
            nlohmann::json jGamescope = nlohmann::json::object();
            jGamescope[ "filter" ] = s.gamescope.filter;
            jGamescope[ "scaler" ] = s.gamescope.scaler;
            jGamescope[ "sharpness" ] = s.gamescope.sharpness;
            jGamescope[ "vrr_enabled" ] = s.gamescope.vrr_enabled;
            jGamescope[ "hdr_enabled" ] = s.gamescope.hdr_enabled;
            jGamescope[ "tearing_enabled" ] = s.gamescope.tearing_enabled;
            jGamescope[ "fps_limit" ] = s.gamescope.fps_limit;
            jGamescope[ "force_grab_cursor" ] = s.gamescope.force_grab_cursor;
            jGamescope[ "force_windows_fullscreen" ] = s.gamescope.force_windows_fullscreen;
            jGamescope[ "sdr_gamut_wideness" ] = s.gamescope.sdr_gamut_wideness;
            jGamescope[ "sdr_on_hdr_brightness_nits" ] = s.gamescope.sdr_on_hdr_brightness_nits;
            jGamescope[ "hdr_input_gain" ] = s.gamescope.hdr_input_gain;
            jGamescope[ "sdr_input_gain" ] = s.gamescope.sdr_input_gain;
            jGamescope[ "nested_width" ] = s.gamescope.nested_width;
            jGamescope[ "nested_height" ] = s.gamescope.nested_height;
            jGamescope[ "nested_refresh_hz" ] = s.gamescope.nested_refresh_hz;
            jGamescope[ "nested_lock_aspect" ] = s.gamescope.nested_lock_aspect;

            nlohmann::json jFps = nlohmann::json::object();
            jFps[ "enabled" ] = s.fps_display.enabled;
            jFps[ "font_size" ] = s.fps_display.font_size;
            jFps[ "backdrop_opacity" ] = s.fps_display.backdrop_opacity;
            jFps[ "backdrop_padding" ] = s.fps_display.backdrop_padding;
            jFps[ "text_opacity" ] = s.fps_display.text_opacity;
            // No fps_enabled/fps_label_enabled/graph_enabled/percentiles_enabled/
            // cpu_enabled/gpu_enabled/
            // media_enabled/frametime_enabled/color_cpu/color_gpu/
            // color_media/layout_name/backdrop_enabled/backdrop_rounding/
            // blend_mode: removed 2026-09-03 (Phase 1's perf-stats/layout
            // removal, then Phase 2's backdrop/colour rework -- see the
            // parse side above and ConfigSchema.h). This serializer emits
            // the struct's fields, so an old file's leftover keys are
            // dropped the first time anything writes this file -- same
            // accepted-for-a-removed-feature precedent as dock_scale below.
            jFps[ "color_fps" ] = s.fps_display.color_fps.has_value()
                ? nlohmann::json( *s.fps_display.color_fps ) : nlohmann::json( nullptr );
            jFps[ "anchor" ] = s.fps_display.anchor;
            jFps[ "margin_x" ] = s.fps_display.margin_x;
            jFps[ "margin_y" ] = s.fps_display.margin_y;
            jFps[ "update_mode" ] = s.fps_display.update_mode;
            jFps[ "hide_above_enabled" ] = s.fps_display.hide_above_enabled;
            jFps[ "hide_above_fps" ] = s.fps_display.hide_above_fps;
            jFps[ "color_mode" ] = s.fps_display.color_mode;
            jFps[ "outline_strength" ] = s.fps_display.outline_strength;
            jFps[ "lag_detection_enabled" ] = s.fps_display.lag_detection_enabled;

            const auto &c = s.crosshair;
            nlohmann::json jCross = nlohmann::json::object();
            jCross[ "enabled" ] = c.enabled;
            jCross[ "line_enabled" ] = c.line_enabled;
            jCross[ "line_length" ] = c.line_length;
            jCross[ "line_width" ] = c.line_width;
            jCross[ "line_gap" ] = c.line_gap;
            jCross[ "line_color" ] = c.line_color;
            jCross[ "line_opacity" ] = c.line_opacity;
            jCross[ "dot_enabled" ] = c.dot_enabled;
            jCross[ "dot_size" ] = c.dot_size;
            jCross[ "dot_color" ] = c.dot_color;
            jCross[ "dot_opacity" ] = c.dot_opacity;
            jCross[ "outline_enabled" ] = c.outline_enabled;
            jCross[ "outline_width" ] = c.outline_width;
            jCross[ "outline_opacity" ] = c.outline_opacity;
            jCross[ "outline_color" ] = c.outline_color;
            jCross[ "hide_on_right_click" ] = c.hide_on_right_click;
            jCross[ "hide_mode" ] = c.hide_mode;
            jCross[ "hide_time_ms" ] = c.hide_time_ms;
            jCross[ "hide_animate_back" ] = c.hide_animate_back;
            jCross[ "apply_scaling" ] = c.apply_scaling;

            nlohmann::json jSaturation = nlohmann::json::object();
            jSaturation[ "enabled" ] = s.reshade.saturation.enabled;
            jSaturation[ "strength" ] = s.reshade.saturation.strength;
            jSaturation[ "protect_skin_tones" ] = s.reshade.saturation.protect_skin_tones;

            nlohmann::json jVibrancy = nlohmann::json::object();
            jVibrancy[ "enabled" ] = s.reshade.vibrancy.enabled;
            jVibrancy[ "strength" ] = s.reshade.vibrancy.strength;

            nlohmann::json jPreSharpen = nlohmann::json::object();
            jPreSharpen[ "enabled" ] = s.reshade.pre_sharpen.enabled;
            jPreSharpen[ "strength" ] = s.reshade.pre_sharpen.strength.has_value()
                ? nlohmann::json( *s.reshade.pre_sharpen.strength )
                : nlohmann::json( nullptr );

            const auto &bl = s.reshade.bloom;
            nlohmann::json jBloom = nlohmann::json::object();
            jBloom[ "enabled" ] = bl.enabled;
            jBloom[ "threshold" ] = bl.threshold;
            jBloom[ "intensity" ] = bl.intensity;
            jBloom[ "radius" ] = bl.radius;

            const auto &ab = s.reshade.adaptive_brightness;
            nlohmann::json jAdaptive = nlohmann::json::object();
            jAdaptive[ "enabled" ] = ab.enabled;
            jAdaptive[ "mode" ] = ab.mode;
            jAdaptive[ "target_luminance" ] = ab.target_luminance;
            jAdaptive[ "adapt_up_speed" ] = ab.adapt_up_speed;
            jAdaptive[ "adapt_down_speed" ] = ab.adapt_down_speed;
            jAdaptive[ "min_gain" ] = ab.min_gain;
            jAdaptive[ "max_gain" ] = ab.max_gain;
            jAdaptive[ "strength" ] = ab.strength;
            jAdaptive[ "local_strength" ] = ab.local_strength;

            const auto &ag = s.reshade.adaptive_gamma;
            nlohmann::json jAdaptiveGamma = nlohmann::json::object();
            jAdaptiveGamma[ "enabled" ] = ag.enabled;
            jAdaptiveGamma[ "target_luminance" ] = ag.target_luminance;
            jAdaptiveGamma[ "max_lift" ] = ag.max_lift;
            jAdaptiveGamma[ "max_darken" ] = ag.max_darken;
            jAdaptiveGamma[ "strength" ] = ag.strength;
            jAdaptiveGamma[ "local_strength" ] = ag.local_strength;

            const auto &sl = s.reshade.shadow_lift;
            nlohmann::json jShadowLift = nlohmann::json::object();
            jShadowLift[ "enabled" ] = sl.enabled;
            jShadowLift[ "strength" ] = sl.strength;

            nlohmann::json jReshade = nlohmann::json::object();
            jReshade[ "saturation" ] = std::move( jSaturation );
            jReshade[ "vibrancy" ] = std::move( jVibrancy );
            jReshade[ "pre_sharpen" ] = std::move( jPreSharpen );
            jReshade[ "bloom" ] = std::move( jBloom );
            jReshade[ "adaptive_brightness" ] = std::move( jAdaptive );
            jReshade[ "adaptive_gamma" ] = std::move( jAdaptiveGamma );
            jReshade[ "shadow_lift" ] = std::move( jShadowLift );

            nlohmann::json jNotifications = nlohmann::json::object();
            jNotifications[ "muted" ] = s.notifications.muted;

            nlohmann::json jSystem = nlohmann::json::object();
            jSystem[ "clipboard_sync" ] = s.system.clipboard_sync;

            nlohmann::json j = nlohmann::json::object();
            j[ "gamescope" ] = std::move( jGamescope );
            j[ "fps_display" ] = std::move( jFps );
            j[ "crosshair" ] = std::move( jCross );
            j[ "reshade" ] = std::move( jReshade );
            j[ "notifications" ] = std::move( jNotifications );
            j[ "system" ] = std::move( jSystem );

            return j;
        }

        // global.json's `overlay` object -- process-level UI preference,
        // never in a profile (ConfigSchema.h's OverlaySettings comment).
        nlohmann::json OverlayToJson( const OverlaySettings &o )
        {
            nlohmann::json jOverlay = nlohmann::json::object();
            jOverlay[ "fade_ms" ] = o.fade_ms.has_value()
                ? nlohmann::json( *o.fade_ms )
                : nlohmann::json( nullptr );
            jOverlay[ "notification_placement" ] = o.notification_placement;

            // ---- window-chrome overhaul fields (see ConfigSchema.h) ----
            // No dock_scale: removed with the dock. This serializer emits
            // the struct's fields, so an old file's leftover dock_scale is
            // dropped the first time anything writes global.json - stated
            // in ConfigSchema.h, and accepted for a removed feature.
            jOverlay[ "display_scale" ] = o.display_scale;
            jOverlay[ "notification_scale" ] = o.notification_scale;
            // No opacity_windows_focused/unfocused or opacity_dock: removed
            // with the surfaces they targeted (see ConfigSchema.h). This
            // serializer emits the struct's fields, so an old file's
            // leftover keys are dropped the first time anything writes
            // global.json - accepted for a removed feature.
            jOverlay[ "window_opacity" ] = o.window_opacity;
            jOverlay[ "opacity_notifications" ] = o.opacity_notifications;
            jOverlay[ "accent_hue" ] = o.accent_hue;
            jOverlay[ "background_blur" ] = o.background_blur;
            jOverlay[ "background_darkening" ] = o.background_darkening;

            jOverlay[ "startup_announce_enabled" ] = o.startup_announce_enabled;
            jOverlay[ "capture_all_keyboard_input" ] = o.capture_all_keyboard_input;
            jOverlay[ "keyboard_navigation_enabled" ] = o.keyboard_navigation_enabled;
            jOverlay[ "profiles_filter_other_games" ] = o.profiles_filter_other_games;

            // Issue #35: per-panel saved window geometry - see the parse
            // side above and ConfigSchema.h's PanelGeometry/
            // OverlaySettings::panel_geometry comments.
            nlohmann::json jGeometry = nlohmann::json::object();
            for ( const auto &[ sKey, g ] : o.panel_geometry )
            {
                nlohmann::json jg = nlohmann::json::object();
                jg[ "x" ] = g.x;
                jg[ "y" ] = g.y;
                jg[ "w" ] = g.w;
                jg[ "h" ] = g.h;
                jg[ "scale" ] = g.scale; // issue #47 -- see PanelGeometry::scale's comment
                jGeometry[ sKey ] = std::move( jg );
            }
            jOverlay[ "panel_geometry" ] = std::move( jGeometry );

            // No system_monitor_tab: removed 2026-09-03 (see the parse
            // side above and ConfigSchema.h's own comment).

            // Cursor tab (see the parse side above and ConfigSchema.h's
            // OverlaySettings comment block).
            jOverlay[ "cursor_scale" ] = o.cursor_scale;
            jOverlay[ "cursor_outline_width" ] = o.cursor_outline_width;
            jOverlay[ "cursor_outline_color" ] = o.cursor_outline_color.has_value()
                ? nlohmann::json( *o.cursor_outline_color ) : nlohmann::json( nullptr );
            jOverlay[ "cursor_inlay_color" ] = o.cursor_inlay_color;
            jOverlay[ "cursor_everywhere" ] = o.cursor_everywhere;
            jOverlay[ "cursor_override_game" ] = o.cursor_override_game;

            // Keybinds -- see the parse side above. Always emitted, empty on a
            // fresh config: the per-field merge in EnqueueGlobalWrite/
            // EnqueueOverlayWrite compares this one key as a whole object, so
            // an absent key would read as "this caller did not change it" and
            // a reset-to-defaults (which empties the map) could never be
            // written back.
            nlohmann::json jKeybinds = nlohmann::json::object();
            for ( const auto &[ sAction, sChord ] : o.keybinds )
                jKeybinds[ sAction ] = sChord;
            jOverlay[ "keybinds" ] = std::move( jKeybinds );

            jOverlay[ "friends_lookup_names" ] = o.friends_lookup_names;

            return jOverlay;
        }

        // ---- parsing / migration --------------------------------------------

        // This is the scaffold future schema changes hang off: add a
        // migrate_N_to_N+1(nlohmann::json &) step here and run it in
        // ParseConfigFile below as versions accumulate. Deliberately built
        // (SPEC.md's "Schema migrations" section) rather than retrofitted
        // once real files are in the wild without one - Migrate_1_to_2 below
        // is its first real use.

        // Schema 1 -> 2 (2026-09-04, request #2): reshade.vibrancy.strength
        // changed meaning from an additive boost (-1.0..+1.0, 0.0 neutral) to
        // a true saturation multiplier (0.0..3.0, 1.0 neutral) - see
        // ConfigSchema.h's kCurrentSchemaVersion comment and
        // superdoc/features/shader-effects.md's "Vibrancy range" section.
        //
        // Reread blind under the new meaning, a schema-1 file's neutral 0.0
        // would become full greyscale - exactly the surprise this step
        // exists to prevent. The transform shifts the whole old range onto
        // the new one by the constant that carries old-neutral to
        // new-neutral (+1.0), then clamps into 0.0..3.0:
        //   - untouched (old 0.0)  -> 1.0  (still neutral, exact)
        //   - old min   (-1.0)     -> 0.0  (full greyscale, exact)
        //   - old max   (+1.0)     -> 2.0  (a real boost, short of the new
        //                                   3.0 ceiling - not exact, but
        //                                   monotonic and sane)
        // A config that never touched vibrancy is therefore unaffected by
        // this rename in the way that matters (no greyscale surprise); one
        // that did keeps the same displacement from neutral rather than
        // being silently reset.
        void Migrate_1_to_2( nlohmann::json &j )
        {
            auto itReshade = j.find( "reshade" );
            if ( itReshade == j.end() || !itReshade->is_object() )
                return;

            auto itVibrancy = itReshade->find( "vibrancy" );
            if ( itVibrancy == itReshade->end() || !itVibrancy->is_object() )
                return;

            auto itStrength = itVibrancy->find( "strength" );
            if ( itStrength == itVibrancy->end() || !itStrength->is_number() )
                return;

            const float flOld = itStrength->get<float>();
            const float flNew = std::clamp( flOld + 1.0f, 0.0f, 3.0f );
            ( *itVibrancy )[ "strength" ] = flNew;
        }

        // Schema 3 -> 4 (2026-09-08): reshade.vibrancy renamed to
        // reshade.saturation -- the effect kept its exact maths, only the
        // name changed (see ConfigSchema.h's kCurrentSchemaVersion comment
        // and superdoc/features/shader-effects.md). A NEW, unrelated
        // "vibrancy" effect was added in the same change, so this step must
        // not simply leave the old key alone: once that key exists, an old
        // file's "reshade.vibrancy" object would otherwise be silently
        // reread as the brand new effect's settings -- wrong shape (no
        // protect_skin_tones there), wrong meaning (a different strength
        // scale entirely). Renaming the JSON object in place carries an old
        // file's enabled/strength/protect_skin_tones forward exactly; the
        // new Vibrancy effect has no old data to migrate and simply takes
        // its compiled-in defaults (off, strength 0.0). Runs after
        // Migrate_1_to_2 in ParseConfigFile below, since that step still
        // expects to find its value under the OLD "vibrancy" key.
        void Migrate_3_to_4( nlohmann::json &j )
        {
            auto itReshade = j.find( "reshade" );
            if ( itReshade == j.end() || !itReshade->is_object() )
                return;

            auto itOld = itReshade->find( "vibrancy" );
            if ( itOld == itReshade->end() || !itOld->is_object() )
                return;

            ( *itReshade )[ "saturation" ] = *itOld;
            itReshade->erase( "vibrancy" );
        }

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
            // entirely" -> 0 case) runs the migration chain here. 0 and 1 both
            // predate the vibrancy rename, so both take this step.
            if ( nVersion < 2 )
                Migrate_1_to_2( j );
            // Every version below 4 (0..3 -- schema 3 is the common real
            // case, every profile file on disk before this change) predates
            // the vibrancy -> saturation key rename; must run AFTER
            // Migrate_1_to_2, which still expects the old key.
            if ( nVersion < 4 )
                Migrate_3_to_4( j );

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


        // ---- profile metadata <-> json --------------------------------------

        const char *KindToString( ProfileKind eKind )
        {
            return eKind == ProfileKind::Game ? "game" : "general";
        }

        ProfileKind KindFromString( const std::string &s )
        {
            return s == "game" ? ProfileKind::Game : ProfileKind::General;
        }

        // The metadata keys sit beside the sections at the top level of a
        // profile file. `name` is always the filename stem (RenameProfile
        // keeps the two equal; a hand-edited mismatch loses to the stem).
        ProfileMeta MetaFromJson( const nlohmann::json &j, std::string_view svStem )
        {
            ProfileMeta m;
            m.name = std::string( svStem );
            m.kind = KindFromString( JGetString( j, "kind", "general" ) );
            if ( m.kind == ProfileKind::Game )
            {
                m.app_id = JGetString( j, "app_id", "" );
                m.game_name = JGetString( j, "game_name", "" );
                m.inherits = JGetString( j, "inherits", "" );
            }
            return m;
        }

        void MetaToJson( nlohmann::json &j, const ProfileMeta &m )
        {
            j[ "name" ] = m.name;
            j[ "kind" ] = KindToString( m.kind );
            if ( m.kind == ProfileKind::Game )
            {
                j[ "app_id" ] = m.app_id;
                j[ "game_name" ] = m.game_name;
                j[ "inherits" ] = m.inherits;
            }
        }

        constexpr const char *kMetaKeys[] = { "schema_version", "name", "kind", "app_id", "game_name", "inherits" };

        bool IsMetaKey( const std::string &sKey )
        {
            for ( const char *psz : kMetaKeys )
                if ( sKey == psz )
                    return true;
            return false;
        }

        // The sections of a profile file, with the metadata stripped.
        nlohmann::json SectionsOf( const nlohmann::json &jFile )
        {
            nlohmann::json j = nlohmann::json::object();
            if ( !jFile.is_object() )
                return j;
            for ( auto it = jFile.begin(); it != jFile.end(); ++it )
                if ( !IsMetaKey( it.key() ) )
                    j[ it.key() ] = *it;
            return j;
        }

        // ---- the pointers <-> json ----------------------------------------------

        ProfileAssignments AssignmentsFromJson( const nlohmann::json &jGlobal )
        {
            ProfileAssignments a;
            const nlohmann::json *pProfiles = JGetObject( jGlobal, "profiles" );
            if ( !pProfiles )
                return a;
            a.last_general = JGetString( *pProfiles, "last_general", "" );
            if ( const nlohmann::json *pGames = JGetObject( *pProfiles, "games" ) )
            {
                for ( auto it = pGames->begin(); it != pGames->end(); ++it )
                {
                    if ( !it->is_object() )
                        continue;
                    GameAssignment g;
                    g.selected = JGetString( *it, "selected", "" );
                    g.audio_node = JGetString( *it, "audio_node", "" );
                    a.games[ it.key() ] = std::move( g );
                }
            }
            return a;
        }

        nlohmann::json AssignmentsToJson( const ProfileAssignments &a )
        {
            nlohmann::json jGames = nlohmann::json::object();
            for ( const auto &[ sAppId, g ] : a.games )
            {
                // An entry that says nothing is not written: the map would
                // otherwise grow one empty row per game ever launched.
                if ( g.selected.empty() && g.audio_node.empty() )
                    continue;
                nlohmann::json jg = nlohmann::json::object();
                jg[ "selected" ] = g.selected;
                jg[ "audio_node" ] = g.audio_node;
                jGames[ sAppId ] = std::move( jg );
            }
            nlohmann::json j = nlohmann::json::object();
            j[ "last_general" ] = a.last_general;
            j[ "games" ] = std::move( jGames );
            return j;
        }

        nlohmann::json GlobalToJson( const OverlaySettings &overlay, const ProfileAssignments &assignments )
        {
            nlohmann::json j = nlohmann::json::object();
            j[ "schema_version" ] = kCurrentSchemaVersion;
            j[ "overlay" ] = OverlayToJson( overlay );
            j[ "profiles" ] = AssignmentsToJson( assignments );
            return j;
        }

        // ---- inheritance: diff and merge -------------------------------------
        // A game profile that inherits stores only what differs from its
        // parent. Both directions are plain recursive JSON operations on the
        // section objects the same serializer built, so "equal" is exact.

        // Keys of `child` whose value differs from `parent`'s; nested
        // objects recurse and vanish when nothing inside differs.
        nlohmann::json SparseDiff( const nlohmann::json &child, const nlohmann::json &parent )
        {
            nlohmann::json out = nlohmann::json::object();
            if ( !child.is_object() )
                return out;
            for ( auto it = child.begin(); it != child.end(); ++it )
            {
                const nlohmann::json *pParent = parent.is_object() ? JGetObject( parent, it.key().c_str() ) : nullptr;
                if ( it->is_object() && pParent )
                {
                    nlohmann::json sub = SparseDiff( *it, *pParent );
                    if ( !sub.empty() )
                        out[ it.key() ] = std::move( sub );
                    continue;
                }
                auto itParent = parent.is_object() ? parent.find( it.key() ) : parent.end();
                if ( itParent == parent.end() || *itParent != *it )
                    out[ it.key() ] = *it;
            }
            return out;
        }

        // `over`'s values onto `base`: objects merge, anything else replaces.
        void DeepMerge( nlohmann::json &base, const nlohmann::json &over )
        {
            if ( !over.is_object() )
                return;
            if ( !base.is_object() )
                base = nlohmann::json::object();
            for ( auto it = over.begin(); it != over.end(); ++it )
            {
                if ( it->is_object() && base.contains( it.key() ) && base[ it.key() ].is_object() )
                    DeepMerge( base[ it.key() ], *it );
                else
                    base[ it.key() ] = *it;
            }
        }

        // "reshade.vibrancy.strength" for every leaf of `j`.
        void FlattenKeys( const nlohmann::json &j, const std::string &sPrefix, std::set<std::string> &out )
        {
            if ( !j.is_object() )
                return;
            for ( auto it = j.begin(); it != j.end(); ++it )
            {
                const std::string sPath = sPrefix.empty() ? it.key() : sPrefix + "." + it.key();
                if ( it->is_object() )
                    FlattenKeys( *it, sPath, out );
                else
                    out.insert( sPath );
            }
        }

        // Removes the leaf at a dotted path; empty parents go with it.
        // False if the path was not there.
        bool RemoveDottedKey( nlohmann::json &j, std::string_view svPath )
        {
            if ( !j.is_object() )
                return false;
            const size_t nDot = svPath.find( '.' );
            const std::string sHead( svPath.substr( 0, nDot ) );
            auto it = j.find( sHead );
            if ( it == j.end() )
                return false;
            if ( nDot == std::string_view::npos )
            {
                j.erase( it );
                return true;
            }
            if ( !RemoveDottedKey( *it, svPath.substr( nDot + 1 ) ) )
                return false;
            if ( it->is_object() && it->empty() )
                j.erase( it );
            return true;
        }

        // ---- in-process state ---------------------------------------------------
        // Everything below is process-wide, single-thread data (the panels
        // draw on the steamcompmgr thread; the console thread's ritz_profile
        // goes through the same functions -- see main.cpp). Reset for tests
        // by ResetSessionRoutingForTests().

        // global.json's mirror: what this process last read or wrote.
        bool s_bGlobalLoaded = false;
        OverlaySettings s_Overlay;
        ProfileAssignments s_Assignments;

        // Every profile this process has written and what it holds -- the
        // resolved settings and the JSON that actually went to disk (sparse
        // for an inheriting game profile). Every read in this file prefers
        // this to the disk: the coalescing writer may still have the write
        // queued, and a synchronous write that read the file instead would
        // then be overtaken by the older queued one (seen in the tests).
        struct WrittenProfile
        {
            Settings settings;
            nlohmann::json json;
        };
        std::map<std::string, WrittenProfile> s_Written;

        const WrittenProfile *Written( std::string_view svName )
        {
            auto it = s_Written.find( std::string( svName ) );
            return it == s_Written.end() ? nullptr : &it->second;
        }

        // Session identity.
        std::optional<std::string> s_oSessionAppId;
        bool s_bSessionAppIdResolved = false;
        std::optional<std::string> s_oSessionOverride;
        // The resolution cache: the session profile, its metadata and (for
        // an inheriting game profile) the parent's resolved sections, so a
        // routed write's diff never reads the parent's file per slider tick.
        std::optional<std::string> s_oSessionProfile;
        ProfileMeta s_SessionMeta;
        std::optional<nlohmann::json> s_oSessionParentSections;
        const char *s_pszSessionSource = "";
        uint64_t s_ulConfigGeneration = 0;

        // ---- the caller-edit merge (requests-2026-09-06 item 1; the global
        // path 2026-09-07) --------------------------------------------------
        // Every panel keeps a whole Settings and writes the whole thing, but
        // its copy of the OTHER parts is only as fresh as its last reload --
        // a generation bump, never another panel's edit. So a write from
        // panel Y used to carry X's values as Y last saw them, undoing X's
        // edit on disk AND in the mirror. Measured twice, in two files:
        //   - profile: a crosshair edit put sharpness back to 5 after the
        //     Display area had set it to 10 (2026-09-06);
        //   - global.json: one Cursor-area write put nine Appearance/
        //     Profiles fields back (accent hue 255 -> 218, UI scale 1.05 ->
        //     1.0, blur, darkening, window and notification opacity,
        //     notification scale and placement, the profiles filter) --
        //     settings-audit 2026-09-07, symmetric in the other direction.
        // The funnels merge instead: a key is taken from the caller only
        // when the CALLER changed it, else the mirror's current value stays.
        // "Changed by the caller" is judged against:
        //   - the caller's own last write since the last generation bump,
        //     keyed by the address of the struct it passes (every panel
        //     passes its file-static copy, so the address is the panel);
        //   - failing that, the states handed out since that bump
        //     (ResolvedSettings(), LoadGlobal()): a value equal to one of
        //     those is a copy the caller loaded, not an edit.
        // Both are cleared on every generation bump, when every panel
        // reloads anyway.
        //
        // One mechanism, two instances: the routed write (EnqueueRoutedWrite)
        // merges per top-level SECTION of the session profile; the global
        // write (EnqueueGlobalWrite) merges per FIELD of global.json's
        // `overlay`, because that one section has several writers
        // (Appearance, Cursor, the notification placement) which each own a
        // few of its fields -- a per-section merge would still have let the
        // last writer's whole copy win.
        constexpr size_t kMaxHandedOutPerKey = 256;

        struct CallerEditMerge
        {
            std::map<const void *, nlohmann::json> bases;                    // caller -> what it last wrote
            std::map<std::string, std::vector<std::string>> handedOut;       // key -> compact dumps handed out

            void Forget()
            {
                bases.clear();
                handedOut.clear();
            }

            // Records one object's keys as "handed out" (deduplicated and
            // bounded per key).
            void RememberHandedOut( const nlohmann::json &jObject )
            {
                for ( auto it = jObject.begin(); it != jObject.end(); ++it )
                {
                    std::vector<std::string> &v = handedOut[ it.key() ];
                    const std::string sDump = it->dump();
                    if ( std::find( v.begin(), v.end(), sDump ) != v.end() )
                        continue;
                    if ( v.size() >= kMaxHandedOutPerKey )
                        v.erase( v.begin() );
                    v.push_back( sDump );
                }
            }

            // Starts from `mirror` and takes a key from `caller` only when
            // the caller changed it (see above). Records `caller` as this
            // caller's base for its next write.
            nlohmann::json Merge( const void *pCaller, const nlohmann::json &mirror, const nlohmann::json &caller )
            {
                nlohmann::json out = mirror;
                const auto itBase = bases.find( pCaller );
                for ( auto it = caller.begin(); it != caller.end(); ++it )
                {
                    const std::string &sKey = it.key();
                    if ( out.contains( sKey ) && out[ sKey ] == *it )
                        continue; // same as the mirror: nothing to decide
                    bool bCallerChanged;
                    if ( itBase != bases.end() )
                    {
                        const nlohmann::json &base = itBase->second;
                        bCallerChanged = !( base.contains( sKey ) && base[ sKey ] == *it );
                    }
                    else
                    {
                        const auto itHanded = handedOut.find( sKey );
                        const std::string sDump = it->dump();
                        bCallerChanged = itHanded == handedOut.end() ||
                            std::find( itHanded->second.begin(), itHanded->second.end(), sDump ) == itHanded->second.end();
                    }
                    if ( bCallerChanged )
                        out[ sKey ] = *it;
                }
                bases[ pCaller ] = caller;
                return out;
            }
        };
        CallerEditMerge s_RoutedMerge; // per section of the session profile
        CallerEditMerge s_GlobalMerge; // per field of global.json's `overlay`

        void ForgetRoutedWriteBases()
        {
            s_RoutedMerge.Forget();
            s_GlobalMerge.Forget();
        }

        // See SetLiveApplyHook().
        std::function<void( const Settings & )> s_fnLiveApply;

        // Focused-window title, for the game display name.
        std::string s_sFocusedTitle;
        bool s_bGameNameNoted = false;

        // Bumped on every write or state change; OverriddenKeys() caches on it.
        uint64_t s_ulMutationSeq = 1;
        void BumpMutation() { s_ulMutationSeq++; }

        bool s_bMigrationChecked = false;

        void InvalidateSession()
        {
            s_oSessionProfile.reset();
            s_oSessionParentSections.reset();
            BumpMutation();
        }

        // Forward declarations for the migration, which uses the loaders.
        std::optional<nlohmann::json> ReadProfileFileJson( std::string_view svName );
        void EnsureMigrated();
        void EnsureGlobalLoaded();
        bool WriteGlobalNow();
        void DiscardQueuedWrite( const std::string &sPath );
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

    // ---- file primitives -----------------------------------------------------

    namespace
    {
        // A profile name is only ever a direct child of ProfilesDir():
        // the name must survive SanitizeProfileName() unchanged, and the
        // path it produces must resolve there. Two layers, the same
        // defense every deleting path in this file has always had.
        std::optional<std::filesystem::path> ContainedProfilePath( std::string_view svName, const char *pszWho )
        {
            std::optional<std::string> oSanitized = SanitizeProfileName( svName );
            if ( !oSanitized || *oSanitized != svName )
            {
                s_ConfigLog.errorf( "%s: refusing suspicious profile name '%.*s'",
                    pszWho, (int)svName.size(), svName.data() );
                return std::nullopt;
            }
            std::filesystem::path path( ProfilePath( *oSanitized ) );
            if ( path.parent_path() != std::filesystem::path( ProfilesDir() ) )
                return std::nullopt;
            return path;
        }

        std::optional<nlohmann::json> ReadProfileFileJson( std::string_view svName )
        {
            std::string sPath = ProfilePath( svName );
            SweepStaleTempFiles( sPath );
            std::optional<std::string> oText = ReadWholeFile( sPath );
            if ( !oText )
                return std::nullopt;
            return ParseConfigFile( *oText, sPath );
        }

        // The freshest JSON for a profile: what this process last queued or
        // wrote for it, else the file.
        std::optional<nlohmann::json> CurrentProfileJson( std::string_view svName )
        {
            if ( const WrittenProfile *pWritten = Written( svName ) )
                return pWritten->json;
            return ReadProfileFileJson( svName );
        }

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

        // The resolved sections of a profile as CANONICAL JSON (every key
        // the struct has, in the serializer's shape): the file's own for a
        // general/standalone profile, the parent's with the child's sparse
        // keys merged over for an inheriting one. A missing parent makes
        // the child behave as standalone (logged once per read).
        //
        // Canonical matters for the diff: a parent written by an older
        // build lacks the sections added since, and a raw comparison would
        // then store a child's whole `crosshair` object as "different"
        // (seen in the headless end-to-end, 2026-09-06).
        std::optional<nlohmann::json> ResolvedSectionsJson( std::string_view svName, ProfileMeta *pMetaOut = nullptr )
        {
            std::optional<nlohmann::json> oFile = CurrentProfileJson( svName );
            if ( !oFile )
                return std::nullopt;
            ProfileMeta meta = MetaFromJson( *oFile, svName );
            if ( pMetaOut )
                *pMetaOut = meta;
            nlohmann::json own = SectionsOf( *oFile );
            if ( meta.kind != ProfileKind::Game || meta.inherits.empty() )
                return SectionsToJson( SettingsFromJson( own ) );

            std::optional<nlohmann::json> oParentFile = CurrentProfileJson( meta.inherits );
            if ( !oParentFile )
            {
                s_ConfigLog.errorf( "profile '%s' inherits from '%s', which does not exist -- treating it as standalone",
                    meta.name.c_str(), meta.inherits.c_str() );
                return SectionsToJson( SettingsFromJson( own ) );
            }
            nlohmann::json merged = SectionsOf( *oParentFile );
            DeepMerge( merged, own );
            return SectionsToJson( SettingsFromJson( merged ) );
        }

        // What actually goes to disk for `meta`: the sections (sparse when
        // inheriting, against `pParentSections` if the caller has them
        // cached, else the parent's file), the metadata, the version.
        nlohmann::json ProfileFileJson( const ProfileMeta &meta, const Settings &settings,
                                        const nlohmann::json *pParentSections )
        {
            nlohmann::json sections = SectionsToJson( settings );
            if ( meta.kind == ProfileKind::Game && !meta.inherits.empty() )
            {
                std::optional<nlohmann::json> oParent;
                if ( !pParentSections )
                    oParent = ResolvedSectionsJson( meta.inherits );
                const nlohmann::json *pParent = pParentSections ? pParentSections : ( oParent ? &*oParent : nullptr );
                if ( pParent )
                    sections = SparseDiff( sections, *pParent );
            }
            nlohmann::json j = std::move( sections );
            j[ "schema_version" ] = kCurrentSchemaVersion;
            MetaToJson( j, meta );
            return j;
        }

        void RememberProfileWrite( const ProfileMeta &meta, const Settings &settings, nlohmann::json jFile )
        {
            s_Written[ meta.name ] = WrittenProfile{ settings, std::move( jFile ) };
            BumpMutation();
        }

        // The freshest resolved settings for a profile: the mirror, else
        // the file.
        std::optional<Settings> ProfileSettingsNow( std::string_view svName )
        {
            if ( const WrittenProfile *pWritten = Written( svName ) )
                return pWritten->settings;
            std::optional<nlohmann::json> oSections = ResolvedSectionsJson( svName );
            if ( !oSections )
                return std::nullopt;
            return SettingsFromJson( *oSections );
        }

        // Synchronous write. Anything still queued for the same file is
        // dropped first: this write was computed from the mirror, so it is
        // the newer of the two, and the queued one landing later would undo
        // it.
        bool WriteProfileNow( const ProfileMeta &meta, const Settings &settings, const nlohmann::json *pParentSections )
        {
            nlohmann::json j = ProfileFileJson( meta, settings, pParentSections );
            const std::string sText = DumpJson( j );
            RememberProfileWrite( meta, settings, std::move( j ) );
            const std::string sPath = ProfilePath( meta.name );
            DiscardQueuedWrite( sPath );
            return WriteFileAtomic( sPath, sText );
        }

        // Rewrites a profile file in place with `edit` applied to its JSON
        // (metadata or single-key edits that must not re-diff anything).
        template <typename Fn>
        bool PatchProfileFile( std::string_view svName, Fn &&edit )
        {
            std::optional<nlohmann::json> oFile = CurrentProfileJson( svName );
            if ( !oFile )
                return false;
            edit( *oFile );
            ( *oFile )[ "schema_version" ] = kCurrentSchemaVersion;
            auto it = s_Written.find( std::string( svName ) );
            if ( it != s_Written.end() )
                it->second.json = *oFile;
            BumpMutation();
            const std::string sPath = ProfilePath( svName );
            DiscardQueuedWrite( sPath );
            return WriteFileAtomic( sPath, DumpJson( *oFile ) );
        }

        void ForgetWritten( std::string_view svName )
        {
            s_Written.erase( std::string( svName ) );
            DiscardQueuedWrite( ProfilePath( svName ) );
        }
    }

    // ---- global.json ---------------------------------------------------------

    namespace
    {
        void EnsureGlobalLoaded()
        {
            if ( s_bGlobalLoaded )
                return;
            EnsureMigrated();
            s_Overlay = OverlaySettings{};
            s_Assignments = ProfileAssignments{};
            std::string sPath = GlobalConfigPath();
            SweepStaleTempFiles( sPath );
            if ( std::optional<std::string> oText = ReadWholeFile( sPath ) )
            {
                if ( std::optional<nlohmann::json> oJson = ParseConfigFile( *oText, sPath ) )
                {
                    s_Overlay = SettingsFromJson( *oJson ).overlay;
                    s_Assignments = AssignmentsFromJson( *oJson );
                }
            }
            s_bGlobalLoaded = true;
        }

        bool WriteGlobalNow()
        {
            EnsureGlobalLoaded();
            BumpMutation();
            return WriteFileAtomic( GlobalConfigPath(), DumpJson( GlobalToJson( s_Overlay, s_Assignments ) ) );
        }
    }

    Settings LoadGlobal()
    {
        // The mirror, not the file (2026-09-07): what this process last
        // queued or wrote for global.json, else the file as first read.
        // Reading the file here handed a caller a copy that was already
        // behind a queued write (the writer coalesces for up to 500 ms), and
        // the copy then went back through EnqueueGlobalWrite() -- the
        // notification placement's "fresh LoadGlobal() per click" was such
        // a caller. Serving the mirror makes a fresh copy equal to the
        // mirror in every field the caller did not touch, so the per-field
        // merge below has nothing to misjudge. Every read is recorded as
        // handed out for that merge.
        //
        // The stale-temp sweep (#21) stays on THIS call rather than moving
        // into EnsureGlobalLoaded(): that one latches after the first load,
        // so a temp file orphaned by a later kill would never be collected
        // again in this process. A readdir of the config directory is the
        // whole cost, and this runs on a generation bump, not per frame.
        SweepStaleTempFiles( GlobalConfigPath() );
        EnsureGlobalLoaded();
        Settings s{};
        s.overlay = s_Overlay;
        s_GlobalMerge.RememberHandedOut( OverlayToJson( s.overlay ) );
        return s;
    }

    bool SaveGlobal( const Settings &settings )
    {
        EnsureGlobalLoaded();
        s_Overlay = settings.overlay;
        return WriteGlobalNow();
    }

    // ---- profile files --------------------------------------------------------

    std::optional<Settings> LoadProfile( std::string_view svSanitizedName )
    {
        EnsureMigrated();
        std::optional<nlohmann::json> oSections = ResolvedSectionsJson( svSanitizedName );
        if ( !oSections )
            return std::nullopt;
        return SettingsFromJson( *oSections );
    }

    std::optional<ProfileMeta> LoadProfileMeta( std::string_view svSanitizedName )
    {
        EnsureMigrated();
        std::optional<nlohmann::json> oFile = ReadProfileFileJson( svSanitizedName );
        if ( !oFile )
            return std::nullopt;
        return MetaFromJson( *oFile, svSanitizedName );
    }

    bool IsSettingsKey( std::string_view svDottedKey )
    {
        // The struct's own serializer is the one list of keys a profile
        // can hold, so it is the one list this answers from -- a key added
        // to SectionsToJson() is known here without a second table.
        static const std::set<std::string> s_Keys = []{
            std::set<std::string> keys;
            FlattenKeys( SectionsToJson( Settings{} ), "", keys );
            return keys;
        }();
        return s_Keys.count( std::string( svDottedKey ) ) > 0;
    }

    bool ProfileExists( std::string_view svSanitizedName )
    {
        if ( svSanitizedName.empty() )
            return false;
        std::error_code ec;
        return std::filesystem::exists( ProfilePath( svSanitizedName ), ec ) && !ec;
    }

    bool SaveProfile( const ProfileMeta &meta, const Settings &settings )
    {
        EnsureMigrated();
        if ( !ContainedProfilePath( meta.name, "SaveProfile" ) )
            return false;
        const bool bOk = WriteProfileNow( meta, settings, nullptr );
        // Writing the session profile's parent changes what the session
        // resolves to: drop the cached parent so the next routed write
        // diffs against the new values.
        if ( s_oSessionProfile && s_SessionMeta.inherits == meta.name )
            InvalidateSession();
        return bOk;
    }

    std::vector<ProfileMeta> ListProfiles()
    {
        EnsureMigrated();
        std::vector<ProfileMeta> out;
        for ( const std::string &sStem : ListJsonStems( ProfilesDir() ) )
        {
            std::optional<nlohmann::json> oFile = ReadProfileFileJson( sStem );
            // A file that no longer parses is still listed (as general):
            // the user's to fix or delete, not ours to hide.
            out.push_back( oFile ? MetaFromJson( *oFile, sStem ) : ProfileMeta{ sStem } );
        }
        return out;
    }

    // ---- schema 2 -> 3 migration ----------------------------------------------

    namespace
    {
        // A name that does not collide with an existing profile: `sBase`,
        // then "<sBase> 2", "<sBase> 3", ...
        std::string FreeProfileName( const std::string &sBase )
        {
            if ( !ProfileExists( sBase ) )
                return sBase;
            for ( int n = 2; n < 1000; n++ )
            {
                std::string s = sBase + " " + std::to_string( n );
                if ( !ProfileExists( s ) )
                    return s;
            }
            return sBase;
        }

        bool IsGeneralProfile( const std::string &sName )
        {
            std::optional<ProfileMeta> oMeta = LoadProfileMeta( sName );
            return oMeta && oMeta->kind == ProfileKind::General;
        }

        // Schema 2 -> 3 (2026-09-06, Profiles v2). File-level, because it
        // creates files. Order matters for the user's un-backed-up config:
        // every profile file is written FIRST and global.json LAST, so an
        // interruption leaves a schema-2 global.json that re-runs this on
        // the next launch -- and every step is idempotent (an identical
        // Default is found, not duplicated; a game profile already bound to
        // its app id is kept). The old games/ files are never touched
        // (the user's rule: never delete a config automatically).
        //
        //   old global.json sections          -> profiles/Default.json (general),
        //                                        unless an existing profile has
        //                                        identical sections
        //   active_profile (if it exists)     -> last_general, else Default
        //   profiles/*.json                   -> unchanged; they are general
        //   games/<AppId>.json                -> game profile "<AppId>", inheriting
        //                                        its last_applied_profile if that
        //                                        exists, else Default; stored as the
        //                                        diff; games[<AppId>].selected set
        //                                        only if override_global was true
        //   audio.manual_node_binary (game)   -> games[<AppId>].audio_node
        //   last_applied_profile, auto_save   -> dropped
        void MigrateV2ToV3( const nlohmann::json &jOld )
        {
            const Settings oldGlobal = SettingsFromJson( jOld );
            const nlohmann::json oldSections = SectionsToJson( oldGlobal );

            // 1. Where the old global.json's values go.
            std::string sDefault;
            for ( const std::string &sStem : ListJsonStems( ProfilesDir() ) )
            {
                std::optional<nlohmann::json> oFile = ReadProfileFileJson( sStem );
                if ( !oFile || MetaFromJson( *oFile, sStem ).kind != ProfileKind::General )
                    continue;
                // Canonicalise through the struct so an old file's dropped
                // keys and key order do not count as a difference.
                if ( SectionsToJson( SettingsFromJson( SectionsOf( *oFile ) ) ) == oldSections )
                {
                    sDefault = sStem;
                    break;
                }
            }
            if ( sDefault.empty() )
            {
                sDefault = FreeProfileName( "Default" );
                ProfileMeta meta{ sDefault };
                WriteProfileNow( meta, oldGlobal, nullptr );
                s_ConfigLog.infof( "migration 2->3: global.json's settings are now profile '%s'", sDefault.c_str() );
            }
            else
            {
                s_ConfigLog.infof( "migration 2->3: profile '%s' already holds global.json's settings", sDefault.c_str() );
            }

            // 2. The general pointer.
            ProfileAssignments assignments;
            const std::string sActive = JGetString( jOld, "active_profile", "" );
            assignments.last_general = ( !sActive.empty() && IsGeneralProfile( sActive ) ) ? sActive : sDefault;

            // 3. Every games/<AppId>.json becomes a game profile.
            for ( const std::string &sAppId : ListJsonStems( GamesDir() ) )
            {
                std::string sPath = GamesDir() + "/" + sAppId + ".json";
                std::optional<std::string> oText = ReadWholeFile( sPath );
                std::optional<nlohmann::json> oGame = oText ? ParseConfigFile( *oText, sPath ) : std::nullopt;
                if ( !oGame )
                    continue;

                std::string sName = sAppId;
                std::optional<ProfileMeta> oExisting = LoadProfileMeta( sName );
                const bool bAlreadyMigrated = oExisting && oExisting->kind == ProfileKind::Game && oExisting->app_id == sAppId;
                if ( oExisting && !bAlreadyMigrated )
                    sName = FreeProfileName( "Game " + sAppId );

                if ( !bAlreadyMigrated )
                {
                    const std::string sLastApplied = JGetString( *oGame, "last_applied_profile", "" );
                    ProfileMeta meta;
                    meta.name = sName;
                    meta.kind = ProfileKind::Game;
                    meta.app_id = sAppId;
                    meta.inherits = ( !sLastApplied.empty() && IsGeneralProfile( sLastApplied ) ) ? sLastApplied : sDefault;
                    WriteProfileNow( meta, SettingsFromJson( *oGame ), nullptr );
                    s_ConfigLog.infof( "migration 2->3: games/%s.json is now game profile '%s' inheriting '%s'",
                        sAppId.c_str(), sName.c_str(), meta.inherits.c_str() );
                }

                GameAssignment &entry = assignments.games[ sAppId ];
                entry.selected = JGetBool( *oGame, "override_global", false ) ? sName : "";
                if ( const nlohmann::json *pAudio = JGetObject( *oGame, "audio" ) )
                    entry.audio_node = JGetString( *pAudio, "manual_node_binary", "" );
            }

            // 4. global.json last.
            s_Overlay = oldGlobal.overlay;
            s_Assignments = std::move( assignments );
            s_bGlobalLoaded = true;
            if ( WriteGlobalNow() )
                s_ConfigLog.infof( "migration 2->3: global.json rewritten (schema %d)", kCurrentSchemaVersion );
        }

        void EnsureMigrated()
        {
            if ( s_bMigrationChecked )
                return;
            s_bMigrationChecked = true;

            std::string sPath = GlobalConfigPath();
            std::optional<std::string> oText = ReadWholeFile( sPath );
            if ( !oText )
                return; // fresh install: nothing to migrate
            nlohmann::json j = nlohmann::json::parse( *oText, nullptr, false );
            if ( j.is_discarded() || !j.is_object() )
                return; // LoadGlobal() logs the malformed file
            int nVersion = 0;
            if ( auto it = j.find( "schema_version" ); it != j.end() && it->is_number_integer() )
                nVersion = it->get<int>();
            if ( nVersion >= 3 )
                return;
            std::optional<nlohmann::json> oParsed = ParseConfigFile( *oText, sPath ); // runs 1->2 first
            if ( oParsed )
                MigrateV2ToV3( *oParsed );
        }
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

            // Coalescing (see ConfigManager.h's EnqueueGlobalWrite comment).
            // Two halves: a pending write to the same path is REPLACED
            // (last wins -- which is also what writing both in order would
            // have produced, minus the first fsync), and ThreadMain waits
            // for the queue to go quiet before taking a batch.
            static constexpr auto kWriteCoalesceMs = std::chrono::milliseconds( 50 );
            // Cap on how long an unbroken stream of edits (a long slider
            // drag) can hold the disk copy back. Bounded so a crash mid-drag
            // loses at most this much, not the whole drag.
            static constexpr auto kWriteCoalesceMaxMs = std::chrono::milliseconds( 500 );

            void Enqueue( std::string sPath, std::string sContents )
            {
                {
                    std::lock_guard<std::mutex> lock( m_Mutex );
                    const auto tNow = std::chrono::steady_clock::now();
                    m_tLastEnqueue = tNow;
                    if ( m_Pending.empty() )
                        m_tOldestPending = tNow;

                    auto it = std::find_if( m_Pending.begin(), m_Pending.end(),
                        [&]( const PendingWrite &w ) { return w.sPath == sPath; } );
                    if ( it != m_Pending.end() )
                        it->sContents = std::move( sContents );
                    else
                        m_Pending.push_back( PendingWrite{ std::move( sPath ), std::move( sContents ) } );
                }
                m_Cv.notify_all();
            }

            // Drops a pending write for `sPath` (a synchronous, newer write
            // is about to land there -- see WriteProfileNow).
            void Discard( const std::string &sPath )
            {
                std::lock_guard<std::mutex> lock( m_Mutex );
                m_Pending.erase( std::remove_if( m_Pending.begin(), m_Pending.end(),
                    [&]( const PendingWrite &w ) { return w.sPath == sPath; } ), m_Pending.end() );
            }

            void Flush()
            {
                std::unique_lock<std::mutex> lock( m_Mutex );
                m_nFlushWaiters++;
                m_Cv.notify_all(); // wake the worker out of its quiet-period wait
                m_Cv.wait( lock, [this]() { return m_Pending.empty() && !m_bWriting; } );
                m_nFlushWaiters--;
            }

        private:
            // ponytail: never joined - detached immediately. This lives for the
            // process's lifetime, same as gamescope's other small
            // dedicated-thread subsystems - fine for a compositor process whose
            // threads the OS reclaims on exit. A joinable std::thread destroyed
            // without join()/detach() calls std::terminate(), which a
            // function-local static's exit-time destructor would otherwise hit
            // here; detaching avoids that outright.
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

                    // Quiet period: keep waiting while edits keep arriving,
                    // until kWriteCoalesceMs pass with none, the batch has
                    // been held kWriteCoalesceMaxMs, or someone is blocked
                    // in Flush() and wants it on disk now.
                    for ( ;; )
                    {
                        if ( m_nFlushWaiters > 0 )
                            break;
                        const auto tNow = std::chrono::steady_clock::now();
                        const auto tDeadline = std::min( m_tLastEnqueue + kWriteCoalesceMs,
                                                         m_tOldestPending + kWriteCoalesceMaxMs );
                        if ( tNow >= tDeadline )
                            break;
                        m_Cv.wait_until( lock, tDeadline );
                    }

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
            int m_nFlushWaiters = 0;
            std::chrono::steady_clock::time_point m_tLastEnqueue{};
            std::chrono::steady_clock::time_point m_tOldestPending{};
        };

        void DiscardQueuedWrite( const std::string &sPath )
        {
            ConfigWriter::Instance().Discard( sPath );
        }

        void EnqueueGlobalFromMirror()
        {
            EnsureGlobalLoaded();
            BumpMutation();
            ConfigWriter::Instance().Enqueue( GlobalConfigPath(), DumpJson( GlobalToJson( s_Overlay, s_Assignments ) ) );
        }
    }

    // Every global write goes through the mirror: `overlay` from the
    // caller, the pointers from what this process knows. No caller of these
    // owns the pointers (SelectProfile & co. do), so a panel's stale copy
    // can never overwrite them -- the same "don't clobber a field you don't
    // own" rule the old routed write applied to `overlay` itself.
    //
    // And since 2026-09-07 the same rule INSIDE `overlay`: the caller's
    // copy is merged onto the mirror one field at a time (s_GlobalMerge --
    // see the CallerEditMerge comment for the judgement and the measured
    // clobber this replaces), so a panel only contributes the fields it
    // actually changed.
    namespace
    {
        void MergeOverlayFromCaller( const void *pCaller, const OverlaySettings &overlay )
        {
            EnsureGlobalLoaded();
            const nlohmann::json mirror = OverlayToJson( s_Overlay );
            const nlohmann::json merged = s_GlobalMerge.Merge( pCaller, mirror, OverlayToJson( overlay ) );
            if ( merged != mirror )
            {
                nlohmann::json jDoc = nlohmann::json::object();
                jDoc[ "overlay" ] = merged;
                s_Overlay = SettingsFromJson( jDoc ).overlay;
            }
            EnqueueGlobalFromMirror();
        }
    }

    void EnqueueGlobalWrite( const Settings &settings )
    {
        MergeOverlayFromCaller( &settings, settings.overlay );
    }

    void EnqueueOverlayWrite( const OverlaySettings &overlay )
    {
        MergeOverlayFromCaller( &overlay, overlay );
    }

    void EnqueueGeometryWrite( const std::string &sPanelKey, const PanelGeometry &geometry )
    {
        EnsureGlobalLoaded();
        s_Overlay.panel_geometry[ sPanelKey ] = geometry;
        EnqueueGlobalFromMirror();
    }

    void EnqueueProfileWrite( const ProfileMeta &meta, const Settings &settings )
    {
        EnsureMigrated();
        const nlohmann::json *pParent = nullptr;
        if ( s_oSessionProfile && *s_oSessionProfile == meta.name && s_oSessionParentSections )
            pParent = &*s_oSessionParentSections;
        nlohmann::json j = ProfileFileJson( meta, settings, pParent );
        const std::string sText = DumpJson( j );
        RememberProfileWrite( meta, settings, std::move( j ) );
        ConfigWriter::Instance().Enqueue( ProfilePath( meta.name ), sText );
    }

    void FlushPendingWrites()
    {
        ConfigWriter::Instance().Flush();
    }

    // ---- session ----------------------------------------------------------------

    const std::optional<std::string> &SessionAppId()
    {
        if ( !s_bSessionAppIdResolved )
        {
            s_oSessionAppId = ResolveAppId();
            s_bSessionAppIdResolved = true;
        }
        return s_oSessionAppId;
    }

    namespace
    {
        // The flagless rule: assignment, then last_general, then Default.
        // `pszSource` names the rule that won, for --ritz-dump-config.
        std::string ResolveAssignedProfile( const char **ppszSource )
        {
            EnsureGlobalLoaded();
            const std::optional<std::string> &oAppId = SessionAppId();
            if ( oAppId )
            {
                auto it = s_Assignments.games.find( *oAppId );
                if ( it != s_Assignments.games.end() && ProfileExists( it->second.selected ) )
                {
                    *ppszSource = "selected by this game";
                    return it->second.selected;
                }
            }
            if ( ProfileExists( s_Assignments.last_general ) )
            {
                *ppszSource = "last general profile";
                return s_Assignments.last_general;
            }
            *ppszSource = "default";
            if ( !ProfileExists( "Default" ) )
            {
                WriteProfileNow( ProfileMeta{ "Default" }, Settings{}, nullptr );
                s_ConfigLog.infof( "created profiles/Default.json from the built-in defaults" );
            }
            return "Default";
        }

        void ResolveSession()
        {
            if ( s_oSessionProfile )
                return;
            EnsureGlobalLoaded();
            std::string sName;
            if ( s_oSessionOverride && ProfileExists( *s_oSessionOverride ) )
            {
                sName = *s_oSessionOverride;
                s_pszSessionSource = "session override";
            }
            else
            {
                s_oSessionOverride.reset();
                sName = ResolveAssignedProfile( &s_pszSessionSource );
            }
            s_SessionMeta = LoadProfileMeta( sName ).value_or( ProfileMeta{ sName } );
            s_oSessionParentSections.reset();
            if ( s_SessionMeta.kind == ProfileKind::Game && !s_SessionMeta.inherits.empty() )
                s_oSessionParentSections = ResolvedSectionsJson( s_SessionMeta.inherits );
            s_oSessionProfile = sName;
        }
    }

    const std::string &SessionProfile()
    {
        ResolveSession();
        return *s_oSessionProfile;
    }

    const std::optional<std::string> &SessionProfileOverride()
    {
        ResolveSession();
        return s_oSessionOverride;
    }

    std::optional<std::string> SessionProfileParent()
    {
        ResolveSession();
        if ( s_SessionMeta.kind == ProfileKind::Game && !s_SessionMeta.inherits.empty() )
            return s_SessionMeta.inherits;
        return std::nullopt;
    }

    Settings ResolvedSettings()
    {
        const std::string &sName = SessionProfile();
        Settings s = ProfileSettingsNow( sName ).value_or( Settings{} );
        EnsureGlobalLoaded();
        s.overlay = s_Overlay;

        // Remember what went out -- per section for the routed write, per
        // overlay field for the global one -- so a later write can tell a
        // loaded copy from an edit (see CallerEditMerge). A bump clears it.
        s_RoutedMerge.RememberHandedOut( SectionsToJson( s ) );
        s_GlobalMerge.RememberHandedOut( OverlayToJson( s.overlay ) );
        return s;
    }

    bool SelectProfile( std::string_view svSanitizedName )
    {
        EnsureGlobalLoaded();
        std::optional<ProfileMeta> oMeta = LoadProfileMeta( svSanitizedName );
        if ( !oMeta )
            return false;
        const std::string sName( svSanitizedName );
        s_oSessionOverride.reset();

        const std::optional<std::string> &oAppId = SessionAppId();
        if ( oAppId )
            s_Assignments.games[ *oAppId ].selected = sName;
        if ( oMeta->kind == ProfileKind::General )
            s_Assignments.last_general = sName;
        else if ( !oAppId )
            s_oSessionOverride = sName; // no game to remember it for: this session only

        WriteGlobalNow();
        InvalidateSession();
        BumpConfigGeneration();
        return true;
    }

    SessionProfileResult UseSessionProfile( std::string_view svRawName )
    {
        SessionProfileResult r;
        std::optional<std::string> oName = SanitizeProfileName( svRawName );
        if ( !oName )
            return r;
        r.ok = true;
        r.name = *oName;
        EnsureGlobalLoaded();
        if ( !ProfileExists( r.name ) )
        {
            const char *pszUnused = "";
            r.copied_from = ResolveAssignedProfile( &pszUnused );
            r.created = true;
            WriteProfileNow( ProfileMeta{ r.name }, ProfileSettingsNow( r.copied_from ).value_or( Settings{} ), nullptr );
            s_ConfigLog.infof( "created profile '%s' from '%s'", r.name.c_str(), r.copied_from.c_str() );
        }
        s_oSessionOverride = r.name;
        InvalidateSession();
        BumpConfigGeneration();
        return r;
    }

    void EnqueueRoutedWrite( const Settings &settings )
    {
        ResolveSession();

        // The merge (see CallerEditMerge): start from the mirror's current
        // sections and take from the caller only what the caller changed.
        const nlohmann::json caller = SectionsToJson( settings );
        std::optional<Settings> oCur = ProfileSettingsNow( *s_oSessionProfile );
        if ( !oCur )
        {
            // Nothing to merge against (no mirror, no readable file): the
            // caller's struct is the whole truth, as before.
            s_RoutedMerge.bases[ &settings ] = caller;
            EnqueueProfileWrite( s_SessionMeta, settings );
            return;
        }

        const nlohmann::json out = s_RoutedMerge.Merge( &settings, SectionsToJson( *oCur ), caller );
        Settings merged = SettingsFromJson( out );
        merged.overlay = settings.overlay; // ignored by the profile write either way
        EnqueueProfileWrite( s_SessionMeta, merged );
    }

    uint64_t ConfigGeneration()
    {
        return s_ulConfigGeneration;
    }

    void BumpConfigGeneration()
    {
        s_ulConfigGeneration++;
        ForgetRoutedWriteBases(); // every panel reloads now; their old copies are no longer a base
        if ( s_fnLiveApply )
            s_fnLiveApply( ResolvedSettings() );
    }

    void SetLiveApplyHook( std::function<void( const Settings & )> fn )
    {
        s_fnLiveApply = std::move( fn );
    }

    // ---- CRUD ------------------------------------------------------------------------

    namespace
    {
        ProfileOp Fail( std::string s )
        {
            return ProfileOp{ false, std::move( s ) };
        }

        // The rules every create/copy/edit checks. `svSelf` is the name the
        // profile will have, so it cannot inherit from itself.
        ProfileOp ValidateMeta( const ProfileMeta &meta )
        {
            std::optional<std::string> oName = SanitizeProfileName( meta.name );
            if ( !oName || *oName != meta.name )
                return Fail( "name must be letters, digits, space, hyphen or underscore" );
            if ( meta.kind == ProfileKind::General )
            {
                if ( !meta.inherits.empty() )
                    return Fail( "only a game profile can inherit" );
                return {};
            }
            if ( meta.app_id.empty() )
                return Fail( "a game profile needs an app id" );
            if ( !meta.inherits.empty() )
            {
                if ( meta.inherits == meta.name )
                    return Fail( "a profile cannot inherit from itself" );
                std::optional<ProfileMeta> oParent = LoadProfileMeta( meta.inherits );
                if ( !oParent )
                    return Fail( "no profile named '" + meta.inherits + "' to inherit from" );
                if ( oParent->kind != ProfileKind::General )
                    return Fail( "'" + meta.inherits + "' is a game profile -- only a general profile can be inherited" );
            }
            return {};
        }

        std::vector<ProfileMeta> ChildrenOf( std::string_view svName )
        {
            std::vector<ProfileMeta> out;
            for ( ProfileMeta &m : ListProfiles() )
                if ( m.kind == ProfileKind::Game && m.inherits == svName )
                    out.push_back( std::move( m ) );
            return out;
        }

        // A pointer change (rename, delete) touches the session too.
        void AfterPointerChange()
        {
            WriteGlobalNow();
            InvalidateSession();
            BumpConfigGeneration();
        }
    }

    ProfileOp CreateProfile( const ProfileMeta &meta, const Settings *pFrom )
    {
        EnsureGlobalLoaded();
        if ( ProfileOp op = ValidateMeta( meta ); !op )
            return op;
        if ( ProfileExists( meta.name ) )
            return Fail( "a profile named '" + meta.name + "' already exists" );
        const Settings settings = pFrom ? *pFrom : ResolvedSettings();
        if ( !WriteProfileNow( meta, settings, nullptr ) )
            return Fail( "could not write '" + meta.name + "'" );
        BumpConfigGeneration();
        return {};
    }

    ProfileOp CopyProfile( std::string_view svSource, const ProfileMeta &meta )
    {
        EnsureGlobalLoaded();
        EnsureMigrated();
        std::optional<Settings> oSource = ProfileSettingsNow( svSource );
        if ( !oSource )
            return Fail( "could not read '" + std::string( svSource ) + "'" );
        return CreateProfile( meta, &*oSource );
    }

    ProfileOp EditProfileMeta( std::string_view svOldName, const ProfileMeta &meta )
    {
        EnsureGlobalLoaded();
        std::optional<ProfileMeta> oOld = LoadProfileMeta( svOldName );
        if ( !oOld || !ContainedProfilePath( svOldName, "EditProfileMeta" ) )
            return Fail( "no profile named '" + std::string( svOldName ) + "'" );
        if ( ProfileOp op = ValidateMeta( meta ); !op )
            return op;
        const bool bRename = meta.name != svOldName;
        if ( bRename && ProfileExists( meta.name ) )
            return Fail( "a profile named '" + meta.name + "' already exists" );

        std::vector<ProfileMeta> children;
        if ( oOld->kind == ProfileKind::General )
        {
            children = ChildrenOf( svOldName );
            if ( meta.kind == ProfileKind::Game && !children.empty() )
                return Fail( "'" + std::string( svOldName ) + "' is inherited by " +
                    std::to_string( children.size() ) + ( children.size() == 1 ? " game profile" : " game profiles" ) +
                    " -- a game profile cannot be a parent" );
        }

        // The resolved values survive whatever the metadata does: a new
        // parent re-diffs them, becoming general bakes them in.
        std::optional<Settings> oResolved = ProfileSettingsNow( svOldName );
        if ( !oResolved )
            return Fail( "could not read '" + std::string( svOldName ) + "'" );
        if ( bRename )
            ForgetWritten( svOldName );
        if ( !WriteProfileNow( meta, *oResolved, nullptr ) )
            return Fail( "could not write '" + meta.name + "'" );

        if ( bRename )
        {
            std::error_code ec;
            std::filesystem::remove( ProfilePath( svOldName ), ec );

            // Every pointer follows.
            for ( auto &[ sAppId, g ] : s_Assignments.games )
                if ( g.selected == svOldName )
                    g.selected = meta.name;
            if ( s_Assignments.last_general == svOldName )
                s_Assignments.last_general = meta.name;
            if ( s_oSessionOverride && *s_oSessionOverride == svOldName )
                s_oSessionOverride = meta.name;
            for ( const ProfileMeta &child : children )
                PatchProfileFile( child.name, [&]( nlohmann::json &j ) { j[ "inherits" ] = meta.name; } );
        }
        AfterPointerChange();
        return {};
    }

    ProfileOp DeleteProfile( std::string_view svSanitizedName )
    {
        EnsureGlobalLoaded();
        std::optional<std::filesystem::path> oPath = ContainedProfilePath( svSanitizedName, "DeleteProfile" );
        if ( !oPath )
            return Fail( "refusing suspicious profile name" );
        if ( !ProfileExists( svSanitizedName ) )
            return {}; // already gone

        // Children get the resolved values baked in and stand alone.
        for ( const ProfileMeta &child : ChildrenOf( svSanitizedName ) )
        {
            std::optional<Settings> oResolved = ProfileSettingsNow( child.name );
            ProfileMeta baked = child;
            baked.inherits.clear();
            if ( oResolved )
                WriteProfileNow( baked, *oResolved, nullptr );
        }

        ForgetWritten( svSanitizedName );
        std::error_code ec;
        std::filesystem::remove( *oPath, ec );
        if ( ec && ec != std::errc::no_such_file_or_directory )
            return Fail( "could not delete '" + std::string( svSanitizedName ) + "': " + ec.message() );

        for ( auto &[ sAppId, g ] : s_Assignments.games )
            if ( g.selected == svSanitizedName )
                g.selected.clear();
        if ( s_Assignments.last_general == svSanitizedName )
            s_Assignments.last_general.clear();
        if ( s_oSessionOverride && *s_oSessionOverride == svSanitizedName )
            s_oSessionOverride.reset();
        AfterPointerChange();
        return {};
    }

    // ---- inheritance markers -----------------------------------------------------

    namespace
    {
        uint64_t s_ulOverriddenCachedSeq = 0;
        std::set<std::string> s_OverriddenCached;
    }

    const std::set<std::string> &OverriddenKeys()
    {
        ResolveSession();
        if ( s_ulOverriddenCachedSeq == s_ulMutationSeq )
            return s_OverriddenCached;

        s_OverriddenCached.clear();
        if ( s_SessionMeta.kind == ProfileKind::Game && !s_SessionMeta.inherits.empty() )
        {
            if ( std::optional<nlohmann::json> oFile = CurrentProfileJson( *s_oSessionProfile ) )
                FlattenKeys( SectionsOf( *oFile ), "", s_OverriddenCached );
        }
        // Read AFTER the work: a write that raced in would otherwise be
        // cached over.
        s_ulOverriddenCachedSeq = s_ulMutationSeq;
        return s_OverriddenCached;
    }

    bool ResetKeyToInherited( std::string_view svDottedKey )
    {
        ResolveSession();
        if ( s_SessionMeta.kind != ProfileKind::Game || s_SessionMeta.inherits.empty() )
            return false;
        const std::string sName = *s_oSessionProfile;

        // Start from what is queued if this process wrote it -- the file on
        // disk may be up to the coalescing window behind.
        std::optional<nlohmann::json> oJson = CurrentProfileJson( sName );
        if ( !oJson )
            return false;
        nlohmann::json j = std::move( *oJson );
        if ( !RemoveDottedKey( j, svDottedKey ) )
            return false;
        j[ "schema_version" ] = kCurrentSchemaVersion;
        MetaToJson( j, s_SessionMeta );
        const std::string sPath = ProfilePath( sName );
        DiscardQueuedWrite( sPath );
        const bool bOk = WriteFileAtomic( sPath, DumpJson( j ) );

        // Keep the mirror honest: the resolved settings are the parent's
        // with what is left merged over.
        nlohmann::json merged = s_oSessionParentSections ? *s_oSessionParentSections : nlohmann::json::object();
        DeepMerge( merged, SectionsOf( j ) );
        RememberProfileWrite( s_SessionMeta, SettingsFromJson( merged ), std::move( j ) );
        BumpConfigGeneration();
        return bOk;
    }

    // ---- the per-game entry -----------------------------------------------------------

    GameAssignment GameEntry( std::string_view svAppId )
    {
        EnsureGlobalLoaded();
        auto it = s_Assignments.games.find( std::string( svAppId ) );
        return it == s_Assignments.games.end() ? GameAssignment{} : it->second;
    }

    void SetGameAudioNode( std::string_view svAppId, std::string_view svBinary )
    {
        EnsureGlobalLoaded();
        GameAssignment &g = s_Assignments.games[ std::string( svAppId ) ];
        if ( g.audio_node == svBinary )
            return;
        g.audio_node = std::string( svBinary );
        EnqueueGlobalFromMirror();
    }

    // ---- the game's display name ----------------------------------------------------

    void NoteFocusedWindowTitle( std::string_view svTitle )
    {
        if ( svTitle.empty() )
            return;
        s_sFocusedTitle = std::string( svTitle );
        if ( s_bGameNameNoted )
            return;
        const std::optional<std::string> &oAppId = SessionAppId();
        if ( !oAppId )
            return;
        s_bGameNameNoted = true;
        // Once per process: fill in any game profile of this app that has
        // no display name yet. A small read per profile, then a queued
        // write -- fine for a once-only step on the compositor thread.
        for ( const ProfileMeta &m : ListProfiles() )
        {
            if ( m.kind != ProfileKind::Game || m.app_id != *oAppId || !m.game_name.empty() )
                continue;
            std::optional<nlohmann::json> oFile = CurrentProfileJson( m.name );
            if ( !oFile )
                continue;
            ( *oFile )[ "game_name" ] = s_sFocusedTitle;
            ( *oFile )[ "schema_version" ] = kCurrentSchemaVersion;
            auto it = s_Written.find( m.name );
            if ( it != s_Written.end() )
                it->second.json = *oFile;
            if ( s_oSessionProfile && *s_oSessionProfile == m.name )
                s_SessionMeta.game_name = s_sFocusedTitle;
            ConfigWriter::Instance().Enqueue( ProfilePath( m.name ), DumpJson( *oFile ) );
        }
    }

    std::string SessionGameName()
    {
        if ( !s_sFocusedTitle.empty() )
            return s_sFocusedTitle;
        return SessionAppId().value_or( "" );
    }

    // ---- tests / debug --------------------------------------------------------------------

    void ResetSessionRoutingForTests()
    {
        s_oSessionAppId.reset();
        s_bSessionAppIdResolved = false;
        s_oSessionOverride.reset();
        s_oSessionProfile.reset();
        s_SessionMeta = ProfileMeta{};
        s_oSessionParentSections.reset();
        s_pszSessionSource = "";
        s_ulConfigGeneration = 0;
        ForgetRoutedWriteBases();
        s_fnLiveApply = nullptr;
        s_bGlobalLoaded = false;
        s_Written.clear();
        s_sFocusedTitle.clear();
        s_bGameNameNoted = false;
        s_bMigrationChecked = false;
        s_ulOverriddenCachedSeq = 0;
        s_OverriddenCached.clear();
        BumpMutation();
    }

    std::string DebugDumpEffective()
    {
        ResolveSession();
        nlohmann::json j = nlohmann::json::object();
        const std::optional<std::string> &oAppId = SessionAppId();
        j[ "resolved_app_id" ] = oAppId ? nlohmann::json( *oAppId ) : nlohmann::json( nullptr );
        j[ "session_profile" ] = *s_oSessionProfile;
        j[ "kind" ] = KindToString( s_SessionMeta.kind );
        j[ "inherits" ] = s_SessionMeta.inherits.empty() ? nlohmann::json( nullptr ) : nlohmann::json( s_SessionMeta.inherits );
        j[ "source" ] = s_pszSessionSource;
        j[ "launch_option" ] = s_oSessionOverride ? nlohmann::json( *s_oSessionOverride ) : nlohmann::json( nullptr );
        nlohmann::json jSettings = SectionsToJson( ResolvedSettings() );
        jSettings[ "overlay" ] = OverlayToJson( s_Overlay );
        j[ "settings" ] = std::move( jSettings );
        return DumpJson( j );
    }
}
