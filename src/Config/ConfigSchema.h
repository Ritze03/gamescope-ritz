#pragma once

#include <optional>
#include <string>

// Plain-data settings schema for gamescope-ritz's config system.
//
// Deliberately has no dependency on nlohmann::json (or any other JSON library) so
// that consumers elsewhere in the codebase (later milestones' overlay UI, live
// gamescope-option wiring, ReShade parameter feed, etc.) can include this header
// without pulling JSON parsing into their translation unit. (De)serialization
// lives in ConfigManager.cpp, the only place that needs to know about JSON.
//
// Field shapes/defaults/ranges mirror superdoc/planning/SPEC.md's "Config schema"
// section. Enum-like fields are plain strings here (matching the on-disk JSON)
// rather than gamescope's own enums (GamescopeUpscaleFilter, etc.) so this header
// stays independent of main.hpp too; ConfigManager's apply-to-startup-globals code
// is what bridges the two.

namespace gamescope::config
{
    // Bumped on any breaking rename/removal/type-change. See ConfigManager's
    // migration scaffolding.
    inline constexpr int kCurrentSchemaVersion = 1;

    struct GamescopeSettings
    {
        std::string filter = "LINEAR";   // LINEAR | NEAREST | FSR | NIS | PIXEL
        std::string scaler = "AUTO";     // AUTO | INTEGER | FIT | FILL | STRETCH
        int sharpness = 2;               // raw 0..20, g_upscaleFilterSharpness value
        bool vrr_enabled = false;
        bool hdr_enabled = false;
        bool tearing_enabled = false;
    };

    struct FpsDisplaySettings
    {
        bool enabled = false;
        float font_size = 18.0f;
        bool backdrop_enabled = true;
        float backdrop_opacity = 0.5f;
        float backdrop_rounding = 4.0f;
        float backdrop_padding = 6.0f;
        std::string blend_mode = "alpha"; // alpha | additive
        float text_opacity = 1.0f;
    };

    struct ReshadeVibrancySettings
    {
        bool enabled = false;
        float strength = 0.0f;           // -1.0..+1.0
        bool protect_skin_tones = true;
    };

    struct ReshadePreSharpenSettings
    {
        bool enabled = false;
        // Unsharp-mask amount, range 0.0..2.0. Picked during M6 implementation
        // (SPEC.md Feature 2 flagged this TBD) - 0.5 matches the order of
        // magnitude of common reference ReShade sharpen defaults. Stays
        // std::optional so a config predating M6 (still null on disk) resolves
        // to this compiled-in default rather than a hard schema-version bump.
        std::optional<float> strength = 0.5f;
    };

    struct ReshadeAdaptiveBrightnessSettings
    {
        // Deferred to M9 (SPEC.md decision) - reserved here now so schema_version
        // doesn't need a breaking bump when it lands.
        bool enabled = false;
        float target_luminance = 0.5f;   // 0.1..0.9
        float adapt_up_speed = 1.0f;     // 0.1..5.0 seconds to ~63% of target
        float adapt_down_speed = 1.0f;   // 0.1..5.0
        float min_gain = 0.5f;           // 0.5..1.0
        float max_gain = 2.0f;           // 1.0..2.0
        float strength = 1.0f;           // 0.0..1.0 dry/wet mix
    };

    struct ReshadeSettings
    {
        ReshadeVibrancySettings vibrancy;
        ReshadePreSharpenSettings pre_sharpen;
        ReshadeAdaptiveBrightnessSettings adaptive_brightness;
    };

    struct OverlaySettings
    {
        // Motion timing TBD per SPEC.md Feature 1 - null until picked.
        // Process-level UI preference: present only in global.json, never in a
        // profile or per-game snapshot (see SPEC.md's config schema section).
        std::optional<int> fade_ms;
    };

    // The full settings shape shared by global.json, profiles/<name>.json, and
    // games/<AppId>.json. `overlay` is only meaningful on the global instance -
    // see OverlaySettings above.
    struct Settings
    {
        GamescopeSettings gamescope;
        FpsDisplaySettings fps_display;
        ReshadeSettings reshade;
        OverlaySettings overlay;
    };
}
