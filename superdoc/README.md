# gamescope-ritz — Documentation

A fork of Valve's gamescope, a Wayland micro-compositor for gaming. Point to
[architecture/overview.md](architecture/overview.md) as the start-here page.

- [`CHANGELOG.md`](../CHANGELOG.md) (repo root) — the user-facing list of this fork's own
  changes, newest date first. Written to the strict shape in
  [claude-instructions/changelog.md](claude-instructions/changelog.md), because the
  settings overlay parses it.

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
  helper APIs and feasibility. Kept so a regression can be rolled back against stated
  design intent.
- [E2 "Inspector Rail, deepened"](planning/redesign/round-2/e2-inspector-plus/) — the
  direction being implemented. `SPEC.md` and `API.md` are the contract, `index.html` the
  tiebreaker mockup, and [`IMPLEMENTATION.md`](planning/redesign/round-2/e2-inspector-plus/IMPLEMENTATION.md)
  the phase-by-phase log of what exists in C++ (`src/Overlay/UI/`) and where it departed
  from the spec.
  [`SHELL-TEST-REPORT.md`](planning/redesign/round-2/e2-inspector-plus/SHELL-TEST-REPORT.md)
  is the exhaustive pre-P5 test pass — what was exercised, with counts; what failed and
  was fixed; what was left and why; and what could not be tested at all. Read it before
  deleting the legacy UI.
  [`CONFORMANCE-AUDIT.md`](planning/redesign/round-2/e2-inspector-plus/CONFORMANCE-AUDIT.md)
  is the first side-by-side comparison of the built shell against the approved mockup —
  24 divergences with paired screenshots in `audit-shots/`, each marked as explained by a
  recorded decision or as unexplained drift. Read it before trusting a phase report's
  claim of conformance.
- [Decisions taken without the user](planning/redesign/AUTONOMOUS-DECISIONS.md) — every
  call made while the user was away, with its alternative and its reasoning, so
  disagreeing is cheap.

## claude-instructions/ — mandatory rules for agents

- [Working with the Docs](claude-instructions/documentation.md) — read before you touch, update after you change.
- [Documentation version policy](claude-instructions/documentation-version-policy.md) — date-based dated-block convention.
- [Changelog maintenance](claude-instructions/changelog.md) — the strict `CHANGELOG.md` format, its four categories, and why the date is the version.

## meta/

- [Terminology](meta/TERMINOLOGY.md) — project vocabulary glossary.
