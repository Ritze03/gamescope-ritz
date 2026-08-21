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

        // Row toggles (spec §11's "ROWS checkbox list") -- independent of
        // `enabled` above (the HUD as a whole); these just show/hide the
        // frametime graph and percentile-stats rows within it, reusing this
        // struct's existing font_size/backdrop_*/blend_mode/text_opacity
        // fields rather than defining a second set per row.
        bool graph_enabled = true;
        bool percentiles_enabled = true;
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

        // Toast notification system (this fork's own addition, see
        // Overlay/Notifications.h and DECISIONS.md #25): where toasts anchor
        // on screen. One of the 9 values Notifications.cpp's kPlacements
        // lists ("top-left" .. "bottom-right"). Deliberately placed here,
        // not on NotificationSettings below - like fade_ms above, this is a
        // process-level UI preference that is *always* global, by explicit
        // design (DECISIONS.md #25's "placement is global, muting is
        // per-game" split), so it gets fade_ms's same exemption from
        // profile/per-game snapshots (SettingsToJson's bIncludeOverlay)
        // rather than becoming a per-game-eligible field the way
        // NotificationSettings::muted is.
        std::string notification_placement = "top-right";
        // ---- window-chrome overhaul: General-tab scale/opacity/effects ----
        // (kept contiguous on purpose - a sibling worker is adding its own
        // fields to this struct concurrently and this block is the seam we
        // agreed to keep merge-clean). Same "process-level, global.json only"
        // rule as fade_ms above: ApplyProfile() never touches `overlay`, and
        // SettingsToJson()'s bIncludeOverlay is false for every profile/
        // per-game write - so a per-game override or an applied profile can
        // never change any of these. Every field here takes effect live (no
        // restart) - see Overlay/Chrome.cpp's EnsureLiveThemeLoaded() and
        // Overlay/PanelConfig.cpp's General tab for the read/write side, and
        // each field's own comment for who *consumes* it (some are drawn by
        // Chrome.cpp/Widgets.cpp directly; dock_scale is the odd one out -
        // notification_scale/opacity_notifications/background_blur/
        // background_darkening are only ever read by sibling milestones'
        // files - Notifications.* and SettingsOverlay.cpp - which this
        // worker does not own and does not touch).
        float dock_scale = 1.0f;                 // 0.85..1.5 - Chrome.cpp's DrawDock() button/gap/padding scale. Spec §1 note: keep the 54px dock button >=44px physical, hence the 0.85 floor (54*0.85 ~= 46px).
        float display_scale = 1.0f;              // 0.8..1.4 - overall UI scale. Drives ImGuiIO::FontGlobalScale only (see Chrome.cpp's EnsureLiveThemeLoaded() comment for why that's the deliberate ceiling: the font atlas in Fonts.cpp is baked at fixed pixel sizes, so text softens above ~1.4x instead of resampling crisply, and every hand-drawn widget geometry in Widgets.cpp/Chrome.cpp is spec-exact fixed pixels that does not scale with it - full geometric rescaling would mean rebuilding the atlas per scale step and re-deriving every hardcoded constant, out of scope for this pass).
        float notification_scale = 1.0f;         // 0.6..1.6 - consumed by Notifications.* (sibling milestone, not touched here). Field/default/range only.
        float opacity_background = 0.0f;         // 0..1 - alpha of a full-screen dim veil Chrome.cpp draws behind the whole overlay (GetBackgroundDrawList(), spec §14's "whether we dim the game while the overlay is open" - left off by default, matching the spec's own "not a measured requirement" note). Distinct from background_darkening below: this is an ImGui-drawn flat tint, off unless the user opts in; background_darkening is a native-compositor multiply on the game layer itself.
        float opacity_windows = 0.88f;           // 0.3..1 - panel window/popup surface alpha, spec §1 `surface` default (rgba(9,10,12,.88)).
        float opacity_dock = 0.86f;              // 0.3..1 - dock container alpha, spec §8 default.
        float opacity_notifications = 0.9f;      // 0.3..1 - consumed by Notifications.* (sibling milestone). Field/default/range only.
        float background_blur = 0.0f;            // 0..1 - drives FrameInfo_t::blurRadius (via blurLayer0, already wired - see SettingsOverlay.cpp's nOverlayBlurRadius). Consumed by SettingsOverlay.cpp (sibling milestone; that file is off-limits here, see this worker's task scope) - field/default/range only.
        float background_darkening = 0.0f;       // 0..1 - a native-compositor dim multiply on the game layer, meant to sit alongside background_blur in whatever consumes FrameInfo_t (SettingsOverlay.cpp, sibling milestone). Field/default/range only - not yet consumed anywhere.
    };

    // Toast notification system (this fork's own addition, see
    // Overlay/Notifications.h and DECISIONS.md #25). Unlike OverlaySettings
    // above, this is a normal per-layer field - shared via global.json
    // unless a game has "Override Global Config" on, exactly like
    // FpsDisplaySettings::enabled, so a game can mute toasts for itself
    // without affecting any other game or the global default.
    struct NotificationSettings
    {
        bool muted = false;
        // Whether the brief startup announcement (animated "gamescope-ritz is
        // active" toast, with the Ctrl+Shift+O hint) plays on process start.
        // Process-level UI preference, same rules as fade_ms above - read
        // once by SettingsOverlay.cpp directly via LoadGlobal(), never via
        // ResolveEffective()/a per-game override. Default true so a fresh
        // install still gets the hint at least once per launch; the General
        // tab surfaces this as a checkbox.
        bool startup_announce_enabled = true;

        // Keyboard-control toggles for the overlay's own input capture (M2)
        // while it is open - see wlserver.cpp's wlserver_dispatch_key() and
        // SettingsOverlay.cpp's SettingsOverlay_IsCapturingKeyboard(). Also
        // process-level, same rules as fade_ms above.
        //
        // capture_all_keyboard_input: true (default, matches M2's shipped
        // behavior) means every keystroke goes to the overlay while it's
        // open, none reach the game. false lets keyboard input pass straight
        // through to the game even while the overlay is open (mouse capture
        // is unaffected either way) - useful for a controller/mouse-only
        // overlay workflow where the player wants to keep typing/using
        // hotkeys in the game itself. Ctrl+Shift+O still always works to
        // close the overlay regardless of this setting, since the hotkey
        // check runs before this gate (wlserver_process_hotkeys()).
        bool capture_all_keyboard_input = true;

        // keyboard_navigation_enabled: whether Tab/arrow-key ImGui keyboard
        // navigation of the overlay's own widgets is enabled while it holds
        // keyboard capture (ImGuiConfigFlags_NavEnableKeyboard). Purely an
        // ImGui-side flag - never changes what wlserver.cpp routes where, so
        // toggling it carries none of capture_all_keyboard_input's release-
        // routing risk.
        bool keyboard_navigation_enabled = true;
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
        NotificationSettings notifications;
    };
}
