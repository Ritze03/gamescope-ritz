# Terminology

Project-specific vocabulary. When the user uses a term defined here, use the same
meaning.

**If the user uses a non-standard term that is not defined here, ask what they mean by
it before acting on it.** Once its meaning is clear, add it to this file. Keep this
file up to date automatically: whenever the user introduces or clarifies a term, add or
amend the entry here.

## Terms

- **steamcompmgr** — the X11 window-manager subsystem inside gamescope (historically a standalone project this compositor absorbed); not the whole binary.
- **Backend** — the display-output abstraction (`IBackend`); one of DRM, SDL, OpenVR, Headless, Wayland, selected at startup. Not a "client backend".
- **Nested mode** — running gamescope as a window inside an existing desktop session (SDL/Wayland backend).
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
- **WSI layer (FROG)** — `VkLayer_FROG_gamescope_wsi`, the Vulkan layer games load so their swapchain is redirected into gamescope's compositing pipeline.
- **mangoapp** — gamescope's integration point for the MangoHud-style performance overlay (`src/mangoapp.cpp`).
- **Command palette** — the searchable, ranked index over every registered Entry and Parameter (`src/Overlay/UI/CommandPalette.{h,cpp}`, drawn from `Shell.cpp`). Search with an empty query is browsing; there is no second "list everything" path.
- **Launcher** — the command palette drawn **alone over the game**, with no shell behind it (`Left Ctrl + Right Shift` while the overlay is closed — rebound from `Left Ctrl + Right Ctrl` 2026-09-01, see CHANGELOG.md). The user's *"Launcher Style UI"*. Distinct from the palette drawn *over an open shell*, which is the same widget with a scrim and a different Esc. Direction B's contribution, kept as a feature rather than as the whole GUI — see AUTONOMOUS-DECISIONS D25. The binding is a **toggle**: pressed again while the palette is already up, it closes what it opened rather than opening something else — `SetVisible(false)` if the launcher is alone on screen, `RequestClosePalette()` (leaving the shell untouched) if the palette is layered over a shell that was opened separately. See IMPLEMENTATION.md's Issue #88 entry.
- **Launcher-UI** — the user's own name for the **Launcher** (see that entry above); use interchangeably with it.
- **Shell** — the E2 settings overlay proper: slab, rail, sheet and inspector (`src/Overlay/UI/Shell.cpp`). "The full/clickable overlay" in the user's wording. The launcher is deliberately *not* part of it. Closed with the X in the title bar; there is no separate settings tab for this any more (the former `setup.shell` tab was removed 2026-08-27).
- **Click-UI** — the user's own name for the **Shell** (see that entry above); use interchangeably with it.
- **HUD** — the on-screen readout drawn over the game: a single FPS integer (`src/Overlay/FpsDisplay.cpp`'s `DrawReadout()`), positioned by a 9-point anchor plus pixel margins. Its settings-panel area is `system.hud` ("HUD" in the rail), renamed from `system.monitor`/"Monitor" (HUD layouts Phase 1, 2026-09-02) because "Monitor" read as ambiguous against this project's own "Overlay" naming, which **Shell**/**Click-UI** above already claims. Distinct from both the **Shell** (the settings surface that hosts the HUD's own settings tab) and the **Launcher** (an unrelated command-palette feature). Used to carry a small **profiler** (see that entry below) and a named per-module layout system; both were removed 2026-09-03 as a deliberate scope reduction, and the current minimal `system.hud` area (Show HUD / anchor / margins / font size) is a placeholder a later phase rebuilds properly.
- **Profiler** — the user's own term for the perf-stats modules the **HUD** used to carry on top of the plain FPS number: CPU load, GPU load, a frametime readout, a frametime graph, a 1%/0.1%-low percentile row, and a Now Playing (MPRIS) module. Removed 2026-09-03 at the user's explicit request as feature bloat — the HUD is a single FPS integer now. The underlying frame-rate counting/timing and lag-spike detection (the raw per-frame frametime history, `FpsDisplay.cpp`'s `s_flFrametimeHistoryMs`) were kept, since a later phase builds on them; the CPU/GPU/media sampling (`src/Metrics/SystemStats.{h,cpp}`, the only consumer of which was the profiler) was deleted along with it.
