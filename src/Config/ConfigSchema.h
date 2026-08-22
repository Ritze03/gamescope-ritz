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

        // Issue #27 (System Monitor part 1/3): one of the 9 anchor strings
        // Notifications.cpp's kPlacements/OverlaySettings::notification_placement
        // already use ("top-left" .. "bottom-right", issue #26's model) --
        // this readout gets its own copy of that field/string-set rather than
        // sharing OverlaySettings::notification_placement, since that field
        // is explicitly process-level/global-only (see its own comment)
        // while this one is a normal per-layer field like every other
        // fps_display.* setting (global default, per-game overridable).
        // Replaces the old hardcoded top-right kAnchorOffset
        // (FpsDisplay.cpp) -- see that file for the 3x3 grid math.
        std::string placement = "top-right"; // Spec §10: "Default anchor: top-right"
        // Independent vertical/horizontal margins from whichever edge(s)
        // the placement selects, replacing the old single kAnchorOffset=32
        // constant with two config-driven values (issue #27).
        float margin_vertical = 32.0f;   // px, Spec §10 "offset 32/32"
        float margin_horizontal = 32.0f; // px, Spec §10 "offset 32/32"
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
        // notification_scale/opacity_notifications are read live by
        // Notifications.cpp (gamescope::Notifications::g_LiveTheme, pushed
        // by PanelConfig.cpp's PushLiveTheme() alongside the Chrome fields);
        // background_blur/background_darkening are read live by
        // SettingsOverlay.cpp (gamescope::g_BackgroundLiveTheme, pushed the
        // same way) -- see SettingsOverlay.h's comment).
        float dock_scale = 1.0f;                 // 0.85..1.5 - Chrome.cpp's DrawDock() button/gap/padding scale. Spec §1 note: keep the 54px dock button >=44px physical, hence the 0.85 floor (54*0.85 ~= 46px).
        float display_scale = 1.0f;              // 0.8..1.4 - overall UI scale. Drives ImGuiIO::FontGlobalScale only (see Chrome.cpp's EnsureLiveThemeLoaded() comment for why that's the deliberate ceiling: the font atlas in Fonts.cpp is baked at fixed pixel sizes, so text softens above ~1.4x instead of resampling crisply, and every hand-drawn widget geometry in Widgets.cpp/Chrome.cpp is spec-exact fixed pixels that does not scale with it - full geometric rescaling would mean rebuilding the atlas per scale step and re-deriving every hardcoded constant, out of scope for this pass).
        float notification_scale = 1.0f;         // 0.6..1.6 - Notifications.cpp's DrawToasts() GetUiScale(): scales toast card size/font/padding/slide distance.
        // opacity_background ("Background veil", an ImGui-drawn flat dim tint
        // behind the whole overlay) was removed 2026-08-22: with
        // background_darkening below now a real, working native-compositor
        // dim, a second control that also just dims the screen was exactly
        // the redundant-controls confusion the user flagged ("two controls
        // that dim the screen"). An old config on disk carrying this key
        // parses fine - ConfigManager.cpp's read side never looked it up by
        // iterating the JSON object, only by explicit named lookups, so a
        // leftover key is simply never read, not an error.
        float opacity_windows_focused = 1.0f;    // 0.3..1 - focused panel window/popup surface alpha (the window currently holding overlay input focus). Chrome.cpp's BeginPanelWindow() applies this vs. opacity_windows_unfocused per-window using the same one-frame-cached focus state that already drives its border-alpha/thickness focus treatment.
        float opacity_windows_unfocused = 0.9f;  // 0.3..1 - every other (unfocused) panel window/popup surface alpha.
        float opacity_dock = 0.7f;               // 0.3..1 - dock container alpha, spec §8 default.
        float opacity_notifications = 0.9f;      // 0.3..1 - Notifications.cpp's DrawToasts() GetUiOpacity(): multiplies each toast card's bg/border/accent/text alpha uniformly.
        float background_blur = 1.0f;            // 0..1 - drives FrameInfo_t::blurRadius (via blurLayer0), linearly mapped onto 0..k_nMaxOverlayBlurRadius in SettingsOverlay.cpp (0 == no blur pass requested at all, not a minimum blur).
        float background_darkening = 0.8f;       // 0..1 - a native-compositor dim multiply on the game layer (FrameInfo_t::Layer_t::ctm on layers[0]), SettingsOverlay.cpp's GetDarkeningCtmBlob(). Composes with background_blur above (blur.h's gaussian_blur() applies this ctm on its final/vertical pass too, see that file's comment) - both default on now that they compose correctly, so a fresh install shows the design's intended look immediately.

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

    // Toast notification system (this fork's own addition, see
    // Overlay/Notifications.h and DECISIONS.md #25). Unlike OverlaySettings
    // above, this is a normal per-layer field - shared via global.json
    // unless a game has "Override Global Config" on, exactly like
    // FpsDisplaySettings::enabled, so a game can mute toasts for itself
    // without affecting any other game or the global default.
    struct NotificationSettings
    {
        bool muted = false;

    };

    // M5 addition (see Audio/Volume.h, superdoc/planning/DECISIONS.md #22/#23):
    // a manual override for which PipeWire stream this game's volume control
    // targets, set from the Audio panel's picker when automatic detection
    // (PID-tree walk, then process-name match, then "newest stream since
    // launch") fails or picks the wrong one.
    struct AudioSettings
    {
        // The stream's application.process.binary (falling back to
        // application.name if the client never set .binary) - a value
        // that's stable across relaunches, unlike the wpctl node id, which
        // is a fresh integer every session. Empty means "no manual
        // override, use automatic detection." A normal per-layer field
        // (like NotificationSettings::muted above), but in practice only
        // ever meaningful per-game - "which stream is this game" has no
        // sensible global default.
        //
        // Distinct from volume/mute themselves (Audio/Volume.h's header
        // comment): this is a node *selection*, not a volume level -
        // WirePlumber already owns remembering the volume value itself
        // (node.stream.restore-props), so gamescope deliberately doesn't
        // duplicate that, but WirePlumber has no concept of "which stream
        // did the user mean," so there's nothing else that could remember
        // this choice.
        std::string manual_node_binary;
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
        AudioSettings audio;
    };
}
