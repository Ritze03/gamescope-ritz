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
    //
    // 1 -> 2 (2026-09-04, request #2): reshade.vibrancy.strength's meaning
    // changed from an additive boost (-1.0..+1.0, 0.0 neutral) to a true
    // saturation multiplier (0.0..3.0, 1.0 neutral). ConfigManager.cpp's
    // Migrate_1_to_2() shifts an old on-disk value onto the new scale
    // (+1.0, clamped) so a schema-1 file's neutral 0.0 does not get
    // silently reread as full greyscale. See superdoc/features/
    // shader-effects.md's "Vibrancy range" section for the full why.
    //
    // 2 -> 3 (2026-09-06, Profiles v2): a profile is the file being edited.
    // global.json keeps only `overlay` and the `profiles` pointers; every
    // per-layer section lives in profiles/<Name>.json; games/<AppId>.json is
    // no longer read. ConfigManager.cpp's MigrateV2ToV3() moves an old
    // global.json's sections into a general profile and each games/ file
    // into a game profile (see superdoc/features/profiles.md, "Migration").
    //
    // 3 -> 4 (2026-09-08): reshade.vibrancy renamed to reshade.saturation --
    // the effect's maths is UNCHANGED, only the name (the user's own
    // observation: it behaved like an iPhone "Saturation" slider, not a
    // "Vibrancy" one). A brand new "vibrancy" effect was added in the same
    // change (ReshadeVibrancySettings below), so the old key could not just
    // be left alone: it would be silently reread as the new effect's
    // settings once that key exists. ConfigManager.cpp's Migrate_3_to_4()
    // renames the JSON object in place, carrying an old file's
    // enabled/strength/protect_skin_tones forward exactly; the new Vibrancy
    // effect has no old data to migrate and takes its compiled-in defaults
    // (off, strength 0.0). See superdoc/features/shader-effects.md's
    // "Saturation / Vibrancy split" section.
    inline constexpr int kCurrentSchemaVersion = 4;

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
        bool force_windows_fullscreen = false; // mirrors --force-windows-fullscreen; per-Xwayland-ctx, genuinely live via steamcompmgr_set_force_windows_fullscreen()

        // HDR tab -- gamescope_color_mgmt_t fields (rendervulkan.hpp) via
        // their existing set_*() functions. Meaningless while hdr_enabled is
        // false; the panel greys these out accordingly.
        float sdr_gamut_wideness = -1.0f;      // 0..1, -1 = unset/display-native; gamescope_color_mgmt_t::sdrGamutWideness
        float sdr_on_hdr_brightness_nits = 203.0f; // gamescope_color_mgmt_t::flSDROnHDRBrightness
        float hdr_input_gain = 1.0f;           // gamescope_color_mgmt_t::flHDRInputGain
        float sdr_input_gain = 1.0f;           // gamescope_color_mgmt_t::flSDRInputGain

        // Nested resolution and refresh (requests-2026-09-05 item 7): the
        // game's internal size (-w/-h, what Xwayland reports via XRandR)
        // and the paced refresh (-r). 0 = "as launched" -- the CLI value,
        // or gamescope's own default, stays in force and nothing here is
        // applied. The OUTPUT window size (-W/-H) is deliberately NOT
        // persisted: a tiling host decides that, and window rules are the
        // right tool.
        //
        // SCHEMA ONLY here. The consumers are item 7's own work, in files
        // this schema change does not touch: main.cpp's
        // apply_ritz_config_to_startup_state() seeds g_nNestedWidth/Height/
        // Refresh from these before getopt runs (so an explicit CLI flag
        // still wins), and Overlay/PanelDisplay.cpp's `display.resolution`
        // setters write them back and call
        // wlserver_set_xwayland_server_mode() live.
        int nested_width = 0;
        int nested_height = 0;
        int nested_refresh_hz = 0;
        // The Custom size steppers' "Lock aspect ratio" switch (2026-09-07).
        // Persisted beside the size it constrains: it reads as one of that
        // row's settings, and the bool is its whole state -- the ratio it
        // holds is re-derived from nested_width/height when first needed
        // (PanelDisplay.cpp's EnsureLockedAspectReference()), never stored.
        // See superdoc/features/resolution-and-refresh.md, "Phase B".
        bool nested_lock_aspect = true;
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

        // The update-mode choice (FpsDisplay.cpp's UpdateAndGetDisplayFps(),
        // arithmetic in FpsDisplay.h's fpsmath). Two values since 2026-09-05:
        // "smoothing" (default) samples the commit count once a second,
        // glides the shown integer to it over 300 ms and holds 700 ms;
        // "immediate" shows the count over the last 100 ms. The former
        // "per_second" was subsumed by Smoothing -- a stored "per_second"
        // (or any unrecognised value) is read as "smoothing" by
        // fpsmath::UpdateModeToInt(), so old configs load unchanged.
        std::string update_mode = "smoothing"; // smoothing | immediate  (legacy: per_second -> smoothing)

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

        // Outline thickness in PIXELS, 0 to 4, 0 = no outline drawn at
        // all. Replaced the former drop shadow (`shadow_strength`)
        // 2026-09-03: a black outline reads over any background, which a
        // single offset shadow does not, and -- unlike the shadow -- it
        // stays useful in Inverted mode, where it is the one part of the
        // readout that must stay black rather than invert. An old config
        // still carrying `shadow_strength` simply falls back to this
        // default (ConfigManager.cpp never reads that key any more).
        //
        // The name kept the `_strength` suffix on purpose. It was briefly
        // an opacity in 0..1 the same day, so a config written in that
        // window already holds this key; a 0..1 value is still a perfectly
        // valid thickness (a sub-pixel to 1px outline), so re-reading it
        // under the new meaning degrades gracefully instead of needing a
        // second dead key. The setting is labelled "Outline size" in the
        // UI, which is what the user actually sees.
        //
        // 4px is the top of the range because backdrop_padding is 6px:
        // the outline stays inside the backdrop box at any setting, so
        // growing it never changes the readout's footprint.
        float outline_strength = 0.0f;

        // Whether lag-spike detection does anything at all. Default on,
        // which is what this HUD has always done, so an existing config
        // keeps the behaviour it had. When off there is no spike reaction
        // of any kind: Fixed mode never flips the number's colour and
        // Inverted mode never tints the backdrop. The frametime history
        // that feeds the detector keeps being collected either way (it is
        // a handful of floats per frame and is wanted for later work), so
        // switching this back on works immediately instead of needing to
        // refill a window of samples first -- see FpsDisplay.cpp's
        // PushFrametimeSample()/IsSpikeActive().
        bool lag_detection_enabled = true;

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

    // Compositor-drawn crosshair (2026-09-05, this fork's own addition --
    // see superdoc/features/crosshair.md). Drawn by Overlay/Crosshair.cpp
    // into the FPS HUD's own layer (Overlay/FpsDisplay.cpp), centred on
    // the game's on-screen rect, so it composites AFTER anything running
    // inside the game -- an external frame-generation layer (lsfg-vk)
    // interpolates the game's own frame and smears whatever is drawn in
    // it, above all the in-game crosshair; this one cannot smear.
    //
    // A normal per-layer section, carried by every profile, exactly like
    // FpsDisplaySettings.
    // Colours are packed 0xRRGGBB ints (the same on-disk shape as
    // color_fps / cursor_inlay_color); every element's alpha is its own
    // separate 0..1 opacity so the RGB colour picker stays a colour picker.
    // Pixel sizes are ints: the whole point of the 1px mode is that a
    // "1" is exactly one pixel, so fractional sizes have no meaning here.
    struct CrosshairSettings
    {
        bool enabled = false; // master switch, default off -- nothing changes for anyone until they turn it on

        // Line: the four arms. length is each arm's own length, gap the
        // distance from the centre to where an arm starts, width the arm's
        // thickness. All in pixels (output pixels, or game pixels when
        // apply_scaling below is on).
        bool line_enabled = true;
        int line_length = 6;
        int line_width = 2;
        int line_gap = 3;
        int line_color = 0x00FF00;
        float line_opacity = 1.0f;

        // Dot: a centred square (always a square, at every size -- see
        // superdoc/features/crosshair.md's geometry section).
        bool dot_enabled = false;
        int dot_size = 2;
        int dot_color = 0x00FF00;
        float dot_opacity = 1.0f;

        // Outline: a stroke of outline_width px drawn AROUND every arm and
        // the dot, strictly outside their fill (never underneath it), so a
        // translucent line shows the game through it, not the outline.
        bool outline_enabled = true;
        int outline_width = 1;
        float outline_opacity = 0.6f;
        int outline_color = 0x000000;

        // Auto-hide while the right mouse button is held (aiming down
        // sights). hide_mode: "fade" (opacity only), "focus" (gap closes
        // over the first half of hide_time_ms, then fades over the second
        // half), "shrink" (gap closes, then the arms and the dot shrink to
        // nothing, the two phases sharing hide_time_ms in proportion to
        // gap : length so the visible edge moves at one speed).
        // hide_animate_back: on release the animation runs backwards from
        // wherever it was (default, 2026-09-06 request #13); off restores
        // the crosshair instantly -- Overlay/CrosshairMath.h.
        bool hide_on_right_click = false;
        std::string hide_mode = "fade"; // fade | focus | shrink
        int hide_time_ms = 200;
        bool hide_animate_back = true;

        // Off: every size above is in output pixels and the crosshair is
        // drawn square whatever the game's aspect. On: sizes are GAME
        // pixels, multiplied per axis by how gamescope stretches the game
        // onto the output -- a 4:3 game stretched to 16:9 gets a
        // horizontally stretched crosshair, the way a stretched in-game
        // one looks.
        bool apply_scaling = false;
    };

    // Renamed from ReshadeVibrancySettings 2026-09-08 (kCurrentSchemaVersion's
    // 3->4 comment above): the user pointed out this effect behaves like an
    // iPhone "Saturation" slider (a flat multiplier, same relative boost for
    // every pixel regardless of how saturated it already is), not that
    // app's "Vibrancy" -- so the NAME moved to match what the effect
    // actually does, and "Vibrancy" was freed up for a new effect built to
    // the user's own definition (ReshadeVibrancySettings below). The maths
    // here is byte-for-byte unchanged by the rename.
    struct ReshadeSaturationSettings
    {
        bool enabled = false;
        // True saturation multiplier, 0.0..3.0, neutral (image unchanged) at
        // 1.0 -- 0.0 is greyscale, 3.0 is maximum boost. Changed from an
        // additive -1.0..+1.0 boost (0.0 neutral) 2026-09-04 (request #2);
        // see kCurrentSchemaVersion's 1->2 comment above and
        // superdoc/features/shader-effects.md for the migration this default
        // alone does NOT cover (a schema-1 file needs Migrate_1_to_2() in
        // ConfigManager.cpp; this default only governs a fresh install that
        // has no file to migrate at all).
        float strength = 1.0f;
        bool protect_skin_tones = true;
    };

    // NEW 2026-09-08 (superdoc/features/shader-effects.md's "Saturation /
    // Vibrancy split"): boosts a pixel's saturation in proportion to how
    // saturated it already is -- punchy colours get punchier, near-neutral
    // colours are left alone -- exactly the user's own stated definition of
    // "Vibrancy". FLAGGED, not silently resolved: this is the INVERSE of
    // Apple Photos' usual "Vibrance", which protects already-saturated
    // colours and boosts muted ones more (the shape ReshadeSaturationSettings
    // above already had, before this rename). Built exactly as asked; see
    // the doc for the full discrepancy note.
    struct ReshadeVibrancySettings
    {
        bool enabled = false;
        // 0.0..2.0, 0.0 neutral (identity, for every pixel, regardless of
        // its own saturation -- unlike Saturation's multiplier, 0.0 here is
        // NOT greyscale). Applied in src/shaders/cs_effects_layer0.comp
        // (effects_common.h's grade()) as:
        //   gain = 1.0 + strength * saturation      // saturation: 0..1, max(c)-min(c)
        //   out  = luma + (c - luma) * gain
        // A grey pixel (saturation 0) has c == luma already, so it is
        // untouched at any strength; a fully saturated pixel (1.0) gets the
        // full 1.0 + strength gain. See shader-effects.md for the range
        // choice and the measured numbers.
        float strength = 0.0f;
    };

    struct ReshadeShadowLiftSettings
    {
        // Request #3 (2026-09-04): "a darkness booster for dark games" --
        // decided as a shadow lift (brightens dark areas, leaves highlights
        // alone), not a global brightness control. Purely additive field
        // (new key, new struct), so an old config simply has neither and
        // resolves to these compiled-in defaults -- no migration needed, see
        // superdoc/features/shader-effects.md's "Shadow lift" section.
        bool enabled = false;
        // 0.0..1.0, 0.0 neutral (identity). Applied in
        // src/shaders/cs_effects_layer0.comp (the native effects pre-pass) as a
        // gamma curve on the low end: out = pow(color, 1.0 - 0.5*strength).
        // 0 and 1 are fixed points of any power curve, so black and white
        // never move; only the shape in between does, with the curve's
        // effect concentrated at the low end. See that file's own comment
        // for the reasoning and superdoc/features/shader-effects.md.
        float strength = 0.0f;
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
        // Consumed by src/shaders/cs_effects_measure.comp (the adapt maths) and
        // cs_effects_layer0.comp (the gain). These are THE defaults: the panel's
        // .Default()s in Overlay/PanelShaders.cpp read this struct rather than
        // repeating the numbers, so change them here only.
        bool enabled = false;
        // "whole_image": one gain from the smoothed mean luma (the original
        // behaviour). "dynamic": a per-frame tone curve from smoothed
        // percentiles -- levels gain, median gamma, soft highlight shoulder
        // (src/shaders/effects_curve.h; request #16, 2026-09-06). Kept
        // separate from `enabled` so switching the effect off and on
        // remembers the mode. Additive key: an old config has none and
        // resolves to the original behaviour.
        std::string mode = "whole_image";   // whole_image | dynamic
        float target_luminance = 0.5f;   // 0.1..0.9 -- Whole image: the mean's target; Dynamic: the median's
        float adapt_up_speed = 1.0f;     // 0.1..5.0 seconds to ~63% of target
        float adapt_down_speed = 1.0f;   // 0.1..5.0
        float min_gain = 0.3f;           // 0.3..1.0 (was 0.5..1.0; widened 2026-09-07, request:
                                          // "make min gain 0.3, max gain 4.0" -- see
                                          // shader-effects.md for the re-measured curve)
        float max_gain = 4.0f;           // 1.0..4.0 (was 1.0..2.0, same request)
        float strength = 1.0f;           // 0.0..1.0 dry/wet mix
        // Local adaptation (2026-09-07): 0.0 = one curve for the whole
        // frame (exactly the pre-2026-09-07 behaviour), 1.0 = every pixel's
        // curve fitted to its own neighbourhood from the measure pass's
        // 16x16 luminance map. DYNAMIC MODE ONLY -- Whole image ignores it.
        // Additive key: an old config has none and gets this default.
        // The default is 0.5, chosen from captures, not taste: see
        // shader-effects.md's split-scene and halo tables.
        float local_strength = 0.5f;     // 0.0..1.0
    };

    // NEW 2026-09-08 (the user: "Make something similar, but make it gamma
    // based. Call it adaptive gamma."). The same measured statistics
    // Adaptive Brightness uses, but the whole operator is ONE exponent:
    // g = ln(target)/ln(p50), clamped to the user's own two limits, applied
    // as x^g per channel and blended by strength. No levels gain, no white
    // point and no shoulder -- for an encoded 0..1 value a pure exponent
    // lands back in 0..1 with 0 and 1 as exact fixed points, so it cannot
    // clip and has nothing to protect the highlights FROM. See
    // src/shaders/effects_curve.h's ADAPTIVE GAMMA block and
    // superdoc/features/shader-effects.md.
    //
    // MUTUALLY EXCLUSIVE WITH adaptive_brightness: both aim the frame's
    // mid-tones at a target from the same pre-effect statistics, so both at
    // once would correct the picture twice. The panel's setters turn the
    // other one off, and the host drops this effect if a hand-edited config
    // asks for both (Adaptive Brightness wins).
    //
    // Purely additive keys: an old config has none of them, gets these
    // compiled-in defaults, and needs no schema bump or migration -- the
    // same shape ReshadeShadowLiftSettings was added in.
    struct ReshadeAdaptiveGammaSettings
    {
        bool enabled = false;
        // 0.1..0.9 -- where the smoothed MEDIAN is put, exactly as Adaptive
        // Brightness's Dynamic mode means it.
        float target_luminance = 0.5f;
        // The two bounds on the exponent, both 1.0 at their "do nothing in
        // this direction" end. They are USER-FACING on purpose: Target
        // reaches the picture only through the exponent, so whatever clamps
        // the exponent decides where Target stops working -- and a clamp the
        // user can neither see nor reach is exactly what cost a session on
        // 2026-09-08. max_lift 4.0 gives an exponent floor of 0.25 (the same
        // floor Adaptive Brightness's AB_DYN_GAMMA_MIN allows at max_gain
        // 4.0), which keeps Target live on a realistic dark frame.
        float max_lift = 4.0f;     // 1.0..4.0; exponent floor = 1 / max_lift
        // 1.5, not 4.0, by default: a darkening exponent crushes shadows by
        // nature and this operator has no shadow cap to hold them up (that
        // is Adaptive Brightness's min_gain, which does not exist here), so
        // the shipped default stops where Adaptive Brightness's GAMMA_MAX
        // stops. A user who wants a harder darken can have it.
        float max_darken = 1.5f;   // 1.0..4.0; exponent ceiling = max_darken
        float strength = 1.0f;     // 0.0..1.0 dry/wet mix
        // The SAME local operator Adaptive Brightness uses (the measure
        // pass's 16x16 map, ab_local_shift), shifting the median each
        // pixel's exponent is fitted to. Defaults to 0, unlike Adaptive
        // Brightness's 0.5: this effect's whole identity is the cheap,
        // purely global exponent, so that is what it ships as; the local
        // path costs four texture fetches and a pow per pixel and is opted
        // into. See shader-effects.md.
        float local_strength = 0.0f;   // 0.0..1.0
    };

    struct ReshadeSettings
    {
        ReshadeSaturationSettings saturation;
        ReshadeVibrancySettings vibrancy;
        ReshadePreSharpenSettings pre_sharpen;
        ReshadeAdaptiveBrightnessSettings adaptive_brightness;
        ReshadeAdaptiveGammaSettings adaptive_gamma;
        ReshadeShadowLiftSettings shadow_lift;
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
        // opacity_windows_focused, opacity_windows_unfocused and opacity_dock
        // were removed 2026-09-06 (requests-2026-09-06.md item 2), on the
        // same "removed feature, key just never read again" precedent as
        // dock_scale/opacity_background above. All three targeted surfaces
        // Chrome.cpp drew (per-window focus alpha, the dock container) that
        // P5 already deleted along with Chrome.cpp itself -- so, exactly
        // like dock_scale, the sliders moved a value nothing on screen ever
        // read: Palette.h's g_LiveTheme.flWindowAlphaFocused/Unfocused/
        // flDockAlpha were written on every edit and never read back by
        // anything. window_opacity below is their replacement: ONE slider,
        // wired to something that actually draws now.
        float window_opacity = 1.0f;             // 0.3..1 - the E2 shell's own backdrop alpha: Shell.cpp's slab background (ImGuiCol_WindowBg) and the Inspector's own fill (Role::SurfaceInspector), via palette::WindowOpacity()/WithAlpha() -- 1.0 is fully opaque (sets the final alpha, does not scale the surface's own baked-in alpha). Text is never dimmed by this -- only the surfaces behind it.
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
        // install still gets the hint at least once per launch; the
        // Appearance tab's "Startup" group surfaces this as a checkbox
        // (PanelConfig.cpp's BuildAppearanceArea(), overlay.startup_announce
        // -- added 2026-09-07, this comment previously said "the General
        // tab", which is this same area's own former/legacy name, not a
        // second tab). The checkbox only affects the NEXT launch: the flag
        // is latched by EnsureStartupAnnounceConfigLoaded() before the Shell
        // (and so this row) can ever be opened.
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

        // Profiles v2 (2026-09-06): the Profiles area's "Filter game
        // profiles" switch -- on hides other games' game profiles from the
        // list (general ones always show). A VIEW preference about this
        // screen, not a setting a game could want differently, which is why
        // it lives here in global.json with the other overlay preferences
        // rather than in a profile (where it could hide the very profile it
        // rode in on).
        bool profiles_filter_other_games = true;

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

        // Off (default): unchanged behaviour -- MouseCursor::getTexture()
        // (steamcompmgr.cpp), the LIVE compositing path, keeps reading
        // XFixesGetCursorImage() every repaint and composites whatever the
        // current X11 cursor actually is, exactly as upstream. On:
        // substitutes this pointer for that live image on every repaint
        // instead, but only once getTexture() has already proven the
        // client didn't hide its cursor -- a game that hid its cursor
        // (any first-person game during locked-pointer gameplay) still
        // gets no cursor; this can never resurrect one. A SEPARATE opt-in
        // from cursor_everywhere above, never folded into it: unlike that
        // root-only fallback, this also hides a cursor the game itself
        // meaningfully sets (an RTS's unit-select arrow), so nobody's
        // setup changes until they opt in to this specifically. Does
        // nothing while the pointer is locked -- there is no cursor layer
        // at all then, for any compositor. See
        // superdoc/features/cursor-pipeline.md.
        bool cursor_override_game = false;

        // ---- Keybinds -- src/Keybinds.{h,cpp}, area `setup.keybinds` -------
        // (2026-09-08.) This fork's own compositor hotkeys, as data: action id
        // ("shell", "shell_alt", "launcher") -> the chord string that action
        // is bound to ("RShift", "Ctrl+Shift+O", "LCtrl+RShift").
        //
        // ONLY THE ACTIONS THAT DIFFER from their compiled-in default appear
        // here, so a fresh config carries no keybind keys at all and behaves
        // exactly as every build before this one did -- and clearing a row
        // (which removes its key) is what "reset to the default" means. That
        // is deliberately the same nullable shape fade_ms and
        // cursor_outline_color use for "unset means follow the built-in",
        // expressed as absence-from-a-map rather than std::optional because
        // the set of actions is open: an action added later must not need a
        // schema change, and an entry for an action this build no longer has
        // is simply never looked up -- the same graceful path
        // panel_geometry's own comment describes.
        //
        // GLOBAL, LIKE EVERY OTHER FIELD IN THIS STRUCT, and for that same
        // reason (superdoc/features/profiles.md): which key opens your
        // settings is a fact about the player's keyboard, not about the game.
        // A per-profile keybind would also mean the chord that opens the
        // settings changed when you launched a different game -- i.e. the one
        // control you need in order to fix it would move on its own.
        std::map<std::string, std::string> keybinds;

        // ---- Steam chat companion -- src/SteamCompanion.{h,cpp}, area
        // ---- `system.companion` (2026-09-08) ------------------------------
        // A browser window on gamescope's OWN Xwayland, pointed at Steam's web
        // chat and promoted to a fullscreen interactive overlay by the
        // STEAM_OVERLAY/STEAM_INPUT_FOCUS properties steamcompmgr already
        // reads. See superdoc/features/steam-companion.md, and
        // superdoc/planning/steam-friends-window.md for why it is a browser
        // and not Steam's real Friends window (that window lives on the HOST's
        // X server; nothing on this host can route a click back into it).
        //
        // GLOBAL, like every other field in this struct, and for a reason
        // specific to these three: WHICH BROWSER EXISTS IS A FACT ABOUT THE
        // MACHINE, not about the game -- a per-profile browser command would
        // mean "chat works in CS2 and does nothing in Rust" with no visible
        // cause. The enable switch goes with them rather than being split off
        // per profile for the same reason the keybinds map above is global:
        // the chord that opens it is one setting for the whole install, so
        // whether it opens anything must be too.
        //
        // Default ON. `Why:` the chord is bound by default and is swallowed
        // whatever this says (the hotkey layer fires before any of this is
        // consulted), so defaulting to off would make Ctrl+Shift+Tab a key
        // that is taken from the game AND does nothing -- the worst of both.
        // Off still answers the press, with a toast naming this switch and the
        // Keybinds area, rather than silence.
        bool companion_enabled = true;

        // The command that opens it. Split like a command line (quotes and
        // backslashes honoured, no shell, no globbing -- SteamCompanionCmd.h's
        // SplitCommand), then `{url}` and `{profile}` are substituted INTO the
        // already-split arguments, so neither can ever become an extra
        // argument however they are punctuated. A command with no `{url}` gets
        // the URL appended as a final argument.
        //
        // `{profile}` is not decoration: without a private user-data-dir a
        // second chromium hands its URL to the user's existing one on the host
        // and exits, so nothing ever appears inside gamescope. It expands to
        // <config dir>/companion-browser, which is also where the one-time
        // Steam login is remembered.
        std::string companion_command =
            "chromium --ozone-platform=x11 --user-data-dir={profile} --no-first-run "
            "--no-default-browser-check --app={url}";

        // Where it points. Steam's own web chat by default; any page works
        // (a wiki, a guide, a second-screen tool), which is the reason this is
        // a setting rather than a constant.
        std::string companion_url = "https://steamcommunity.com/chat";
    };

    // Toast notification system (this fork's own addition, see
    // Overlay/Notifications.h and DECISIONS.md #25). Unlike OverlaySettings
    // above, this is a normal per-layer field carried by every profile,
    // exactly like FpsDisplaySettings::enabled, so a game with its own
    // profile can mute toasts for itself without affecting any other.
    struct NotificationSettings
    {
        bool muted = false;

    };

    // The System tab (Overlay/PanelSystem.cpp, area `system.general`;
    // requests-2026-09-05 item 5). A normal per-layer section, like
    // NotificationSettings above.
    struct SystemSettings
    {
        // Mirrors the runtime flag gamescope::g_bClipboardSyncEnabled
        // (Clipboard/ClipboardSync.h), which is what the nested backends
        // actually read. PanelSystem.cpp seeds the flag from this on load
        // and writes both on every change, so the switch survives a
        // restart. Default true: sync is opt-out, matching the feature's
        // behaviour before the switch existed.
        bool clipboard_sync = true;
    };

    // The per-layer settings shape: what a profile file carries
    // (profiles/<Name>.json). `overlay` is the one exception -- process-level,
    // stored in global.json only (see OverlaySettings above); it is carried
    // here so the resolved struct a panel holds is complete, but no profile
    // write ever serialises it and no profile read ever fills it.
    //
    // Profiles v2 (2026-09-06, superdoc/planning/profiles-concept.md): the
    // former session fields (last_applied_profile / active_profile /
    // auto_save_profile) and the per-game `audio` section are gone. A
    // profile IS the settings being edited, so there is nothing to "apply",
    // nothing to fan out to, and no provenance to remember; the one per-game
    // fact that is not a setting (which PipeWire stream is this game) moved
    // to GameAssignment::audio_node below.
    struct Settings
    {
        GamescopeSettings gamescope;
        FpsDisplaySettings fps_display;
        CrosshairSettings crosshair;
        ReshadeSettings reshade;
        OverlaySettings overlay;
        NotificationSettings notifications;
        SystemSettings system;
    };

    // ---- Profiles v2: the file-level metadata -------------------------------

    // Two kinds, no chains: a GENERAL profile stands alone; a GAME profile is
    // bound to one app id and may inherit from exactly one general profile.
    // `Why two levels:` the user's model is "a base setup, tweaked per game";
    // a game inheriting from another game's profile was never asked for and
    // would make "where does this value come from" a walk instead of a
    // glance.
    enum class ProfileKind
    {
        General,
        Game,
    };

    // The top-level keys of a profile file beside the settings sections.
    struct ProfileMeta
    {
        // The file name without ".json"; always exactly what
        // SanitizeProfileName() returns for itself.
        std::string name;
        ProfileKind kind = ProfileKind::General;
        // Game profiles only: the app id this profile belongs to, and a
        // display name for it ("[Game] Rust" in the list) captured from the
        // running game the first time one is seen, so the list can show it
        // while the game is not running.
        std::string app_id;
        std::string game_name;
        // Game profiles only: the general profile this one inherits from, or
        // empty for a standalone game profile. An inheriting profile's file
        // stores only the keys whose value differs from the parent's
        // (ConfigManager.cpp's SparseDiff); everything else follows the
        // parent live.
        std::string inherits;
    };

    // One game's entry in global.json's `profiles.games` map.
    struct GameAssignment
    {
        // The profile this game last selected in the list; "" means "never
        // chose" and the game uses `last_general` (ProfileAssignments below).
        std::string selected;
        // Was AudioSettings::manual_node_binary (M5, DECISIONS.md #22/#23):
        // the PipeWire stream's application.process.binary this game's
        // volume control targets when automatic detection picks wrong.
        // Empty means automatic. Lives here, not in a profile, because it
        // names one game's process -- in a profile shared by two games it
        // would point one game's volume control at the other's process.
        std::string audio_node;
    };

    // global.json's `profiles` object: the pointers. Assignments are
    // pointers, never copies -- switching one changes nothing inside any
    // profile.
    struct ProfileAssignments
    {
        // The general profile most recently selected anywhere. A game seen
        // for the first time starts on it.
        std::string last_general;
        std::map<std::string, GameAssignment> games;
    };
}
