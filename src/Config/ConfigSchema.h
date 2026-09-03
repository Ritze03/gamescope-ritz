#pragma once

#include <map>
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

        // GAMESCOPE panel additions (issue #25). All live via the same
        // mechanisms the fields above already use -- see PanelDisplay.cpp.
        int fps_limit = 0;                     // 0 = unlimited, matches g_nSteamCompMgrTargetFPS's own semantics; live via the GAMESCOPE_FPS_LIMIT X11 property
        bool force_grab_cursor = false;        // mirrors --force-grab-cursor's runtime effect on g_bForceRelativeMouse; genuinely live, not startup-only

        // HDR tab -- gamescope_color_mgmt_t fields (rendervulkan.hpp) via
        // their existing set_*() functions. Meaningless while hdr_enabled is
        // false; the panel greys these out accordingly.
        float sdr_gamut_wideness = -1.0f;      // 0..1, -1 = unset/display-native; gamescope_color_mgmt_t::sdrGamutWideness
        float sdr_on_hdr_brightness_nits = 203.0f; // gamescope_color_mgmt_t::flSDROnHDRBrightness
        float hdr_input_gain = 1.0f;           // gamescope_color_mgmt_t::flHDRInputGain
        float sdr_input_gain = 1.0f;           // gamescope_color_mgmt_t::flSDRInputGain
    };

    struct FpsDisplaySettings
    {
        bool enabled = false;
        float font_size = 18.0f;

        // Backdrop (Phase 2, 2026-09-03 -- see CHANGELOG.md and
        // superdoc/features/fps-display.md): a plain rectangle behind the
        // number, sized to the text plus backdrop_padding. Collapsed to a
        // single opacity rather than an "enabled" bool plus an opacity --
        // 0 IS "off" (the user's own spec: "opacity configurable"), so
        // there is exactly one control instead of two that can disagree.
        // backdrop_rounding (a Phase 1 field, 4.0f corners) is REMOVED
        // outright rather than deprecated-and-kept: the user was explicit
        // that this backdrop never rounds its corners, so a leftover
        // nonzero value on an old config would silently contradict that
        // the moment anything looked at it again -- better gone than
        // ignored. FpsDisplay.cpp's DrawModuleBackdrop() always draws with
        // 0.0f rounding now, not a config-read value.
        float backdrop_opacity = 0.5f;
        float backdrop_padding = 6.0f; // px -- not user-facing, just hugs the text
        float text_opacity = 1.0f;

        // Phase 2: the three-way update-mode choice (FpsDisplay.cpp's
        // UpdateAndGetDisplayFps()). "smoothing" is the pre-existing EMA
        // and stays the default; "per_second" recomputes once a second so
        // the digits sit still; "immediate" shows the latest frame's own
        // instantaneous rate with no smoothing at all.
        std::string update_mode = "smoothing"; // smoothing | per_second | immediate

        // Phase 2: "Hide if FPS above X" -- a Switch (hide_above_enabled)
        // with a threshold Param (hide_above_fps), the `.Param()` idiom
        // this file's own hud.anchor row already uses for margin_x/y. The
        // hysteresis band that stops this flickering at the threshold is
        // NOT a setting -- it lives as a fixed constant in
        // FpsDisplay.cpp's DrawReadout(), same "no user setting for a
        // deliberately lean feature" call as the lag-spike heuristic below.
        bool hide_above_enabled = false;
        float hide_above_fps = 120.0f;

        // Phase 2: the two-way text-colour choice. "fixed" keeps today's
        // look (the UI's own accent colour, color_fps below can override
        // it) and inverts briefly on a detected lag spike; "inverted" is a
        // true per-pixel invert of the game's own colour under each glyph
        // -- rendervulkan's ALPHA_BLENDING_MODE_INVERT, wired up in
        // FpsDisplay.cpp's FpsDisplay_AddLayer() and implemented in
        // src/shaders/alphamode.h's BlendLayer(). See
        // superdoc/features/fps-display.md for the mid-grey guard and the
        // blend-space/HDR caveats.
        std::string color_mode = "fixed"; // fixed | inverted

        // Phase 2: drop-shadow strength, 0 = no shadow drawn at all.
        float shadow_strength = 0.0f;

        // Issue #29: optional colour override for the FPS number's text.
        // std::optional, same nullable-field shape as OverlaySettings::
        // fade_ms -- unset (the common/default case) means "derive from
        // Palette.h's accent family" (FpsDisplay.cpp's ModuleColorVec4()),
        // so the default colour moves if #37's hue-selectable accent work
        // changes what that Palette.h token resolves to at runtime. A *set*
        // value is a deliberate, explicit user override and intentionally
        // does NOT track the accent hue. Packed 0xRRGGBB (24-bit, no stored
        // alpha -- text_opacity already governs alpha).
        //
        // color_cpu/color_gpu/color_media (the CPU/GPU/Media modules' own
        // colour overrides) were removed 2026-09-03 along with those
        // modules themselves -- see superdoc/meta/TERMINOLOGY.md's
        // "profiler" entry.
        std::optional<int> color_fps;

        // Placement (scope reduction 2026-09-03): a 9-point anchor plus
        // pixel margins -- FpsDisplay.cpp's kPlacements/ResolveAnchoredOrigin().
        // Replaces the named-layout system (HudLayout/HudLayoutModule/
        // HudLayoutFpsModule, and this field's own former `layout_name`),
        // removed the same day as the profiler modules it existed to place
        // independently -- with a single module left, per-module manual x/y
        // placement had no reason left to be more complex than the anchor+
        // margin model this whole rework had originally replaced. See
        // superdoc/meta/TERMINOLOGY.md's "profiler" entry and CHANGELOG.md.
        std::string anchor = "top-right"; // one of kPlacements' 9 strings (FpsDisplay.cpp)
        int margin_x = 24; // px, distance from the left/right edge
        int margin_y = 24; // px, distance from the top/bottom edge
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

    // Issue #35: one panel window's saved screen position/size, restored on
    // the next launch (Overlay/Chrome.cpp's BeginPanelWindow()). Keyed by a
    // stable string on OverlaySettings::panel_geometry below, deliberately
    // NOT Chrome.h's PanelId enum ordinal/name -- that enum has already been
    // renamed once (issue #27: Fps -> SystemMonitor) and may be again; a key
    // that survives a rename means an old config's entry for a since-renamed
    // panel is simply never looked up again (harmless -- see
    // panel_geometry's own comment) instead of a stale ordinal silently
    // landing on the wrong panel, or a schema-version bump being needed for
    // what is otherwise a purely additive, non-breaking change.
    struct PanelGeometry
    {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;

        // Issue #47: the overlay.display_scale in effect when w/h were saved.
        // BeginPanelWindow() rescales a restored size by
        // (current DisplayScale() / this) rather than reusing it verbatim --
        // without this, a size saved at 1.0x would reopen unchanged at 2.0x
        // and reintroduce #47's overflow through the persistence path. Default
        // 1.0f (and JGetFloat's own "key absent" fallback on parse, below)
        // deliberately matches every geometry saved before this field
        // existed: pre-#47, the window's own outer size never scaled with
        // display_scale in the first place, so an old entry's w/h really do
        // reflect a 1.0x-shaped window regardless of what display_scale was
        // live when it was saved.
        float scale = 1.0f;
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
        // each field's own comment for who *consumes* it:
        // notification_scale/opacity_notifications are read live by
        // Notifications.cpp (gamescope::Notifications::g_LiveTheme, pushed
        // by PanelConfig.cpp's PushLiveTheme()); background_blur/
        // background_darkening are read live by SettingsOverlay.cpp
        // (gamescope::g_BackgroundLiveTheme, pushed the same way) -- see
        // SettingsOverlay.h's comment.
        //
        // dock_scale was removed 2026-08-24. It scaled Chrome.cpp's
        // DrawDock() geometry, and P5 deleted the dock, the floating
        // windows and all their chrome -- so the field controlled nothing
        // at all, and a slider for it was a control the user could move
        // with no effect anywhere on screen. Removed rather than left
        // dormant for exactly that reason. An old config carrying the key
        // parses fine: this file's read side (ConfigManager.cpp) only ever
        // looks keys up by name, never iterates-and-validates, so a
        // leftover key is simply never read -- the same graceful path
        // opacity_background's own removal note below describes. It is,
        // however, DROPPED the next time the file is written: the
        // serializer emits the struct's fields, so it cannot round-trip a
        // key the struct no longer has. That is accepted for a removed
        // feature, and stated here so it does not surprise anyone.
        float display_scale = 1.0f;              // 0.5..2.0 - overall UI scale (#24). Drives ImGuiIO::FontGlobalScale AND, on slider release, gamescope::fonts::RebuildAll() re-bakes the font atlas at the new effective size across all three ImGui contexts (#38), so text stays crisp across the whole range rather than resampling a fixed-size bake. Widget geometry in Widgets.cpp multiplies by this field too (#23), so controls and hit-tests scale together with the text across the whole 0.5..2.0 range instead of the text alone growing against fixed-pixel geometry.
        float notification_scale = 1.0f;         // 0.5..2.0 - Notifications.cpp's DrawToasts() GetUiScale(): scales toast card size/font/padding/slide distance.
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
        // Issue #37: hue-only accent picker. Degrees, OKLCH hue (0..360,
        // wraps). Saturation/lightness (OKLCH C/L) are NOT user-tunable -
        // every accent token in Overlay/Palette.h keeps its own spec'd C/L
        // and only rotates hue, so no combination can produce a muddy or
        // blown-out accent. Default 218 reproduces the spec's own #36BDDD
        // family exactly (Palette.cpp's per-token OKLCH table). Read live by
        // Overlay/Chrome.cpp's EnsureLiveThemeLoaded() into
        // gamescope::palette::g_LiveTheme.flAccentHue, then
        // gamescope::palette::UpdateAccentFamily() regenerates every
        // kAccent* token from it - same live, no-restart pattern as every
        // other field in this block.
        float accent_hue = 218.0f;
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

        // Issue #35: per-panel window position/size, restored on next
        // launch - replaces the "remembered only for the life of the ImGui
        // context" behavior ImGuiCond_FirstUseEver alone gives (Chrome.h's
        // IsPanelOpen() comment). Process-level UI preference, same
        // "global.json only, never profile/per-game" rule as every other
        // field in this struct - a window's screen position is about the
        // player's physical display setup, not the game running. Keyed by
        // a stable string (see PanelGeometry's own comment above); an entry
        // for a panel key this build no longer recognizes parses into the
        // map same as any other and is simply never looked up by
        // Chrome.cpp, not treated as a schema error - so one stale/renamed
        // key never costs the other panels their saved geometry. A panel
        // with no entry here (fresh install, or never moved/resized) falls
        // back to Chrome.cpp's TiledDefaultPos()/measured opening size,
        // unchanged from #34's own default-placement behavior.
        std::map<std::string, PanelGeometry> panel_geometry;

        // system_monitor_tab (which System Monitor sub-tab -- "modules" or
        // "statistics" -- was selected) was removed 2026-09-03 along with
        // the Statistics tab itself and the perf-stats modules it gated
        // collection for (superdoc/meta/TERMINOLOGY.md's "profiler" entry).
        // Nothing reads it any more; an old config's leftover key is simply
        // never looked up, same precedent as dock_scale/opacity_background's
        // own removal.

        // ---- Cursor tab -- Overlay/PanelCursor.{h,cpp} --------------------
        // Controls for the pointer the overlay draws for itself while it is
        // open (Overlay/CursorArt.cpp -- a triangle silhouette, outline over
        // a solid inlay). Process-level UI preference, same "global.json
        // only" rule as every other field in this struct: what the cursor
        // looks like is about the player's own overlay, not any one game.
        //
        // Every default below reproduces CursorArt.cpp's own compiled-in
        // look exactly, so nothing changes for anyone until they open this
        // tab and touch a control -- except cursor_scale, whose default was
        // moved to 0.8 at the user's request (2026-08-29); anyone who had
        // already set their own value keeps it, only the out-of-the-box size
        // changed. Read every draw by CursorArt_Draw() via PanelCursor.h's
        // GetCursorAppearance() accessor.
        float cursor_scale = 0.8f;          // 0.5..3.0 -- multiplies CursorArt.cpp's whole silhouette (its kTipX/kFootY/kWingX geometry), same meaning as CursorArt_Draw()'s flScale parameter today. Default is 0.8, not 1.0 -- see comment above.
        float cursor_outline_width = 2.0f;  // 1.0..6.0 px -- stroke width before the scale above is applied; matches CursorArt.cpp's kOutlineWidth constant.
        // Unset (default) = the outline follows the live accent hue, exactly
        // like today (CursorArt_AccentRgb()); set = a fixed 0xRRGGBB colour
        // instead. Same nullable "follow app theme vs. explicit override"
        // shape as FpsDisplaySettings::color_fps/cpu/gpu/media above.
        std::optional<int> cursor_outline_color;
        int cursor_inlay_color = 0x000000;  // 0xRRGGBB -- the solid fill inside the outline. Default matches CursorArt.cpp's hardcoded black inlay; always explicit, there is no "follow accent" mode for this one.

        // Off (default): unchanged behaviour -- gamescope never touches the
        // game-side cursor, exactly as upstream, and this pointer is only
        // ever drawn by the settings overlay while it's open. On: this
        // pointer (same geometry, same scale/outline/colour fields above)
        // also becomes the game's fallback cursor (steamcompmgr.cpp's
        // SetDefaultCursorImage()) -- what's shown on the root window
        // whenever no client window has defined its own, in both nested and
        // embedded mode, grabbed or not. Nobody's setup changes until they
        // opt in. See superdoc/features/cursor-pipeline.md.
        bool cursor_everywhere = false;
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

        // Issue #43 (config-UI intuitiveness pass) recommendation #10: a
        // plain provenance breadcrumb, not a live link -- DECISIONS.md #20's
        // one-time-copy semantics for profile apply stay exactly as they
        // are (see ConfigManager.cpp's ApplyProfile(), the sole writer of
        // this field). Empty means "no profile has ever been applied to
        // this file" (or it predates this field -- an old config on disk
        // with no key here just parses to ""). Deliberately top-level, not
        // nested under any of the structs above: it describes the *file*
        // (which profile last touched it), not any one settings group, and
        // unlike `overlay` it IS meaningful on every file this schema is
        // used for (global/profile/per-game alike), so it is never gated
        // behind SettingsToJson()'s bIncludeOverlay. Overwritten on every
        // ApplyProfile() call; left untouched by every other write path (a
        // slider edit does not un-set "where this game's values last came
        // from"). Displayed as "last applied profile: X", never "current" --
        // editing settings afterward does not clear it, same honesty rule
        // PanelConfig.cpp's status line already follows.
        std::string last_applied_profile;
    };
}
