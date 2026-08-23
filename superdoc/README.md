# gamescope-ritz — Documentation

A fork of Valve's gamescope, a Wayland micro-compositor for gaming. Point to
[architecture/overview.md](architecture/overview.md) as the start-here page.

## architecture/ — how the code fits together

- [Overview](architecture/overview.md) — module map, data flow, "where to look for X". **Start here.**

## features/ — what the project does

### Backends

- [DRM backend](features/backend-drm.md) — embedded-mode output via DRM/KMS.
- [SDL backend](features/backend-sdl.md) — nested-mode output via SDL.
- [Wayland backend](features/backend-wayland.md) — nested-mode output as a Wayland client.
- [Headless backend](features/backend-headless.md) — no-display backend.
- [OpenVR backend](features/backend-openvr.md) — output as a VR overlay/connector.

### Window management

- [steamcompmgr focus](features/steamcompmgr-focus.md) — X11 window manager focus handling.

### Rendering

- [Vulkan compositing](features/compositing-vulkan.md) — the per-frame Vulkan compositing pass.
- [Scaling filters](features/scaling-filters.md) — FSR (EASU/RCAS) and NIS upscaling.
- [HDR color management](features/hdr-color-management.md) — PQ conversion and color management.

### External surfaces

- [Vulkan WSI layer](features/vk-wsi-layer.md) — `VkLayer_FROG_gamescope_wsi` swapchain redirection.
- [Wayland protocols](features/wayland-protocols.md) — custom protocol surface, including `gamescope_control`.
- [Control / IPC](features/control-ipc.md) — external control and IPC (e.g. `gamescopectl`).

### Input

- [Input emulation](features/input-emulation.md) — synthetic input injection.
- [Input method / IME](features/input-method-ime.md) — input method editor support.

### Effects & capture

- [Reshade effects](features/reshade-effects.md) — the ReShade-compatible post-process pipeline.
- [Screen capture (PipeWire)](features/screen-capture-pipewire.md) — screen capture via PipeWire.

### Tooling & runtime

- [Scripting / ConVars](features/scripting-convars.md) — the runtime tunable-variable and debug-command system.
- [Build and tooling](features/build-and-tooling.md) — Meson build, dev tooling.
- [Process management](features/process-management.md) — process/Xwayland lifecycle management.

## planning/ — overlay UI implementation notes

- [UI mockup — pixel-exact spec](planning/ui-mockup-precise-spec.md) — the settings
  overlay's full chrome/color/typography/control spec, measured from the original design
  mockup.
- [Slider widget spec](planning/slider-widget-spec.md) — the shared `widgets::SliderControl()`
  slider, measured from a real render (supersedes the mockup spec's own §7 for anything the
  two disagree on).
- [Overlay redesign proposals](planning/redesign/) — three preserved design directions (A
  console, B command palette, E inspector rail) with interactive mockups, specs, proposed
  helper APIs and feasibility. **Nothing implemented**; kept so a regression can be rolled
  back against stated design intent.

## claude-instructions/ — mandatory rules for agents

- [Working with the Docs](claude-instructions/documentation.md) — read before you touch, update after you change.
- [Documentation version policy](claude-instructions/documentation-version-policy.md) — date-based dated-block convention.

## meta/

- [Terminology](meta/TERMINOLOGY.md) — project vocabulary glossary.
