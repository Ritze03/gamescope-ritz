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
