# Terminology

Project-specific vocabulary. When the user uses a term defined here, use the same
meaning.

**If the user uses a non-standard term that is not defined here, ask what they mean by
it before acting on it.** Once its meaning is clear, add it to this file. Keep this
file up to date automatically: whenever the user introduces or clarifies a term, add or
amend the entry here.

**Keep every entry short: what the term is, and where to read more.** One or two
sentences, about 200 characters. Details, behaviour, option names, history and rationale
belong on the linked doc page. If that page doesn't exist and there is more to say, write
the page; don't grow the entry. When you amend an entry, trim it to this form instead of
appending. This file stays in context for the whole session.

## Terms

- **steamcompmgr** — the X11 window-manager subsystem inside gamescope (historically a standalone project this compositor absorbed); not the whole binary.
- **Backend** — the display-output abstraction (`IBackend`); one of DRM, SDL, OpenVR, Headless, Wayland, selected at startup. Not a "client backend".
- **Nested mode** — running gamescope as a window inside an existing desktop session (SDL/Wayland backend).
- **Nested window** — a client window inside gamescope's own Xwayland (`xwayland_ctx_t`/`steamcompmgr_win_t`), not gamescope's outer window (that is **Nested mode**). The "Force maximize nested window" toggle names this sense; see `superdoc/features/display-general.md`.
- **Embedded mode** — gamescope owning the physical display directly via DRM/KMS (the Steam Deck / console usecase).
- **Override window** — a window painted on top of the focused app window (e.g. the Steam overlay) without taking full focus.
- **Underlay window** — a window painted beneath the current override, preserved across override changes.
- **Look** — a `gamescope_control` protocol concept (`set_look`/`unset_look`) applying a display colour/visual profile.
- **FrameInfo_t** — the per-composite-pass description of every layer (windows, cursors, overrides) to be composited in one Vulkan pass.
- **Connector** — an `IBackendConnector`: an output port/display (a DRM connector, or a VR overlay "connector" under the OpenVR backend).
- **VirtualConnectorKey_t** — the key used to look up per-output focus state in `g_VirtualConnectorFocuses`; supports multi-display and multi-VR-overlay setups.
- **ConVar / ConCommand** — gamescope's runtime tunable-variable and debug-command system (`src/convar.h`), settable live from the script console.
- **EASU / RCAS** — the two FSR1 shader passes: edge-adaptive spatial upscale, and robust contrast-adaptive sharpen.
- **NIS** — NVIDIA Image Scaling, an alternative upscaling filter to FSR.
- **PQ (Perceptual Quantizer)** — the HDR10 EOTF gamescope converts to and from nits (`pq_to_nits` / `nits_to_pq`).
- **Xwayland ctx** — `xwayland_ctx_t`, one per spawned Xwayland server instance (gamescope can run several; `--xwayland-count`).
- **Reshade effect** — a post-process shader effect applied via the ReShade-compatible pipeline; distinct from the FSR/NIS *scaling* filters.
- **gamescope_control** — the primary custom Wayland protocol interface external tools (Steam, `gamescopectl`) use to query and drive gamescope.
- **WSI layer (FROG)** — the Vulkan layer games load to redirect their swapchain into gamescope; this fork ships its OWN, `VK_LAYER_RITZ_gamescope_wsi` (`ENABLE_GAMESCOPE_RITZ_WSI`), version-matched to the compositor since 2026-09-22. See `superdoc/features/build-and-tooling.md`.
- **Low-latency wake-up** — the Display > General switch (`display.low_latency_wakeup`, default On, 2026-10-04) sizing the vblank wake-up lead from the measured draw time instead of upstream's ~4 ms; not under VRR. See `superdoc/features/display-general.md`.
- **mangoapp** — gamescope's integration point for the MangoHud-style performance overlay (`src/mangoapp.cpp`).
- **Command palette** — the searchable, ranked index over every registered Entry and Parameter (`src/Overlay/UI/CommandPalette.{h,cpp}`, drawn from `Shell.cpp`). Search with an empty query is browsing; there is no second "list everything" path.
- **Launcher** — the command palette drawn **alone over the game**, no shell behind it (`LCtrl+RShift` while the overlay is closed; a toggle). The user's *"Launcher Style UI"*. See `superdoc/features/command-palette.md`.
- **Launcher-UI** — the user's own name for the **Launcher** (see that entry above); use interchangeably with it.
- **Shell** — the settings overlay proper: slab, rail, sheet and inspector (`src/Overlay/UI/Shell.cpp`); the user's "full/clickable overlay". The launcher is not part of it. See `superdoc/features/overlay-shell.md`.
- **Click-UI** — the user's own name for the **Shell** (see that entry above); use interchangeably with it.
- **Diagnostics** — the Shell's `system.diagnostics` area (OTHER group, 2026-11-05): checks of the setup around gamescope that cost frames silently, today Hyprland direct scanout; its warning can be silenced (global switch). Not the **Log**. See `superdoc/features/diagnostics.md`.
- **Rail group** — one of the **Shell**'s eight accordion headings in its rail (DISPLAY, MOTION, OVERLAY, INPUT, AUDIO, MISC, SETTINGS, OTHER; `RailGroup`/`RailOrder()` in `src/Overlay/UI/Registry.{h,cpp}`). See `superdoc/features/overlay-shell.md`.
- **HUD** — the on-screen readout over the game, a single FPS integer (`FpsDisplay.cpp`'s `DrawReadout()`); area `system.hud`, renamed from `system.monitor` 2026-09-02. Not the **Shell**, not the **Launcher**. See `superdoc/features/fps-display.md`.
- **Color Boost** — the Shaders effect `image.shaders.color_boost` (2026-10-29): an inverted gamma curve on each colour's gamut fraction, so muted colours gain most and nothing clips; the opposite of **Vibrancy**. See `superdoc/features/shader-effects-color.md`.
- **Adaptive Gamma** — the Shaders effect `image.shaders.adaptive_gamma` (2026-09-08): one exponent `x^g` aimed at a target median, adjusting contrast without clipping; exclusive with Adaptive Brightness and V2. See `superdoc/features/shader-effects-adaptive-gamma.md`.
- **Adaptive Brightness V2** — `image.shaders.adaptive_brightness_v2` (2026-09-14): a third, exclusive choice beside Adaptive Brightness and Adaptive Gamma (toe-gamma curve, guided-filter detail, darkening); the original stays. See `superdoc/features/shader-effects-adaptive-brightness-v2.md`.
- **About** — the `system.changelog` area, labelled "About" since 2026-09-09 (the id is unchanged): version, upstream base commit, `CHANGELOG.md` and the compiled-in licences. See `superdoc/features/about.md`.
- **Brightness Map** — the experimental local-tone effect `image.shaders.brightness_map`, added 2026-09-09 and removed 2026-09-14 at the user's request; superseded by **Adaptive Brightness V2**. See `superdoc/features/shader-effects.md`.
- **Zoom** — this fork's compositor-drawn magnifier (2026-09-14; `src/Overlay/Zoom.{h,cpp}`, area `system.zoom`, keybind `zoom` = `RMB`), a staged reveal with a Fade duration. Not upstream's `zoomScaleRatio`. See `superdoc/features/zoom.md`.
- **Held action** — a **keybind action** flagged `bHeld` (`zoom`, `autoclicker`): fires when its **chord** is a subset of the held keys, reports the release, and never swallows a mouse button. See `superdoc/features/keybinds.md`.
- **Null binds** — the WASD SOCD-cleaning module (2026-09-27; `src/Overlay/NullBinds.{h,cpp}`, area `system.null_binds`): only the last-pressed of A/D (and W/S) reaches the game, with a Delay and a Randomize jitter; off by default. See `superdoc/features/null-binds.md`.
- **Motion blur** — this fork's interpolation-based motion blur (2026-10-04; area `image.motionblur`, config `motion_blur`): each shown frame averages several in-between frames; works with or without Frame generation. Not a **Reshade effect**. See `superdoc/features/motion-blur.md`.
- **Lag spike buffer** — this fork's lag-spike bridge (2026-10-04; area `image.lagbuffer`, config `lag_buffer`): runs the picture slightly behind real time so a repeated spike is bridged by generated frames. Not a frame limiter. See `superdoc/features/lag-spike-buffer.md`.
- **Crosshair** — the compositor-drawn crosshair (`src/Overlay/Crosshair.{h,cpp}`, area `system.crosshair`, 2026-09-05), drawn into the **HUD**'s layer so frame generation cannot smear it. Not the **Cursor**. See `superdoc/features/crosshair.md`.
- **Autoclicker** — this fork's paced synthetic click train (2026-09-18; `src/Overlay/Autoclicker.{h,cpp}`, area `system.autoclicker`, keybind `autoclicker` = `Mouse4`), injected through `wlserver_mousebutton()`; in the UI since 2026-09-22. See `superdoc/features/autoclicker.md`.
- **Profiler** — the perf-stats modules (CPU/GPU load, frametime graph, 1% lows, Now Playing) the **HUD** once carried; removed 2026-09-03 at the user's request, leaving the plain FPS integer. See `superdoc/features/fps-display.md`.
- **Profile** — the settings file being edited (`~/.config/gamescope-ritz/profiles/<Name>.json`), saved on every change; **general** (`Comp`) or **game** (bound to one app id, `[Game] Rust`). See `superdoc/features/profiles.md`.
- **Inherits** — a game profile's link to one general profile: it stores only differing values and follows the parent live for the rest; two levels, no chains. See `superdoc/features/profiles.md`.
- **Selected profile** — the outlined line in the Profiles list (`profiles.list`); selecting it loads it and is the assignment the current game remembers (`games.<AppId>.selected`). No load or save button. See `superdoc/features/profiles.md`.
- **Session profile** — the profile this session edits (`config::SessionProfile()`): the `--profile` / `GS_RITZ_PROFILE` override (created EMPTY if missing), else the selected profile, else `last_general`, else `Default`. See `superdoc/features/profiles.md`.
- **App id** — a game profile's identity key (`ProfileMeta::app_id`, `games.<AppId>` in `global.json`): an opaque string, not necessarily a Steam app id, resolved by `config::ResolveAppId()` (`RITZ_GS_APPID` first). See `superdoc/features/profiles.md`.
- **Chord** — a keybind in this project's grammar: an unordered `+`-separated set of keys held together (`Ctrl+Shift+O`, `LCtrl+RShift`); a modifiers-only chord is a **tap**. `src/Keybinds.{h,cpp}`; see `superdoc/features/keybinds.md`.
- **Keybind action** — one thing a **chord** can be bound to; seven (`shell`, `shell_alt`, `launcher`, `friends`, `mixer`, `zoom`, `autoclicker`), all global, stored in `global.json`'s `overlay.keybinds`. Not a `gamescope_action_binding`. See `superdoc/features/keybinds.md`.
- **Reserved chord** — `Ctrl+Alt+Shift+O`: always toggles the **Shell**, cannot be rebound and is no **keybind action**, so a lost binding never locks the user out. `gamescopectl ritz_keybinds_reset` is the other way back. See `superdoc/features/keybinds.md`.
- **Nested resolution / refresh** — the game's own resolution and paced refresh (`-w`/`-h`/`-r`; `g_nNestedWidth`/`g_nNestedHeight`/`g_nNestedRefresh`), not **Nested mode**. Defaults to the screen's resolution. See `superdoc/features/resolution-and-refresh.md`.
- **Output resolution / refresh** — the size and refresh the host granted gamescope's own window (`g_nOutputWidth`/`g_nOutputHeight`/`g_nOutputRefresh`), read-only, paired with the **Nested resolution / refresh**. See `superdoc/features/resolution-and-refresh.md`.
- **Preview split** — the Shaders area's `image.shaders.preview_split` switch (2026-09-14): applies the effects to one half only so you can judge them; display-only, every effect still runs on the whole frame. See `superdoc/features/shader-effects-pipeline.md`.
- **Friends list** — the `system.friends` area (2026-09-08): same-game friends in a joinable lobby, read from the running Steam client; click selects, double-click joins. Hidden when not a Steam game. See `superdoc/features/steam-friends.md`.
- **Frame generation** — this fork's in-compositor frame interpolation (2026-10-04; area `image.framegen`, `src/FrameGen/`), optical flow paced per vblank, off by default; not a Shaders effect, not an external layer. See `superdoc/features/frame-generation.md`.
- **Motion version 2** — the FrameGen library's second motion algorithm (V2, 2026-10-22; `framegen.motion_version`), a "morphing" interpolator with no per-pixel hold; falls back to V1 if unsupported. Not **Crosshair V2**. See `superdoc/features/frame-generation-motion-v2.md`.
- **Learned motion** — the library's third motion version (`MotionVersion::Learned`, 2026-10-26, opt-in): V1 with its motion search replaced by a small neural net (Learned net, precision, zero-motion test, static guard). See `superdoc/features/frame-generation-learned.md`.
- **Crosshair V2** — Frame generation's UI-protection detector V2 (2026-10-05, `UiProtection::CrosshairV2`) for the **Crosshair box** (`framegen.ui_box_height`); V1 is kept to compare. See `superdoc/features/frame-generation-ui-protection.md`.
- **FrameGen** — the frame-interpolation *library* (`subprojects/FrameGen`, submodule of frame-gen-ritz, MIT), compiled into the binary; **Frame generation** is the feature built on it. See `superdoc/features/frame-generation.md`.
- **Capture sink** — this fork's private per-session PipeWire sink the hosted game's audio is captured into (2026-10-07; `src/Audio/Capture.{h,cpp}`, config `audio.capture`, default on). See `superdoc/features/audio-capture.md`.
- **Dynamics** — the Mixer's compressor/limiter on the captured game audio (2026-10-21; `src/Audio/Dynamics.{h,cpp}`, **Effects** area, config `audio.dynamics.*`, default Off); needs the **Capture sink**. See `superdoc/features/audio-dynamics.md`.
- **Compress output images** — the global, Wayland-only Display > General switch (`display.compress_output_images`, 2026-11-07, default On, next launch) that lets the driver pick AMD DCC for gamescope's output images; Off restores the no-DCC filter. See `superdoc/features/display-general.md`.
- **Block direct scanout** — the global, Wayland-only Display > General switch (`display.block_direct_scanout`, 2026-11-07) that keeps the host from putting gamescope's window on a display plane. See `superdoc/features/display-general.md`.
