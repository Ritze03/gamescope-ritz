# gamescope-ritz — Documentation

A fork of Valve's gamescope, a Wayland micro-compositor for gaming. Start with
[architecture/overview.md](architecture/overview.md); it maps the code and links into the
pages below.

- [`CHANGELOG.md`](../CHANGELOG.md) (repo root) — the user-facing list of this fork's own
  changes, newest version first. Written to the strict shape in
  [claude-instructions/changelog.md](claude-instructions/changelog.md), because the
  settings overlay parses it; the build reads its newest block's version as the project's
  own, so this file **is** the version marker.

Which docs describe current behaviour: `architecture/`, `features/`, `ui/` and `meta/`.
`planning/` is historical (see its section). The code is the source of truth.

## architecture/ — how the code fits together

- [Overview](architecture/overview.md) — module map, threading, data flow, "where to look
  for X". **Start here.**

## features/ — what the project does

Split topics have a **hub** page that stays short and lists its sub-pages; sub-pages are
indented under it.

### Backends

- [DRM backend](features/backend-drm.md) — embedded-mode output via DRM/KMS.
- [SDL backend](features/backend-sdl.md) — nested-mode output via SDL.
- [Wayland backend](features/backend-wayland.md) — nested-mode output as a Wayland client.
- [Headless backend](features/backend-headless.md) — no-display backend.
- [OpenVR backend](features/backend-openvr.md) — output as a VR overlay/connector.

### Window management

- [steamcompmgr focus](features/steamcompmgr-focus.md) — X11 window manager focus handling.
- [Cursor pipeline](features/cursor-pipeline.md) — hub: the three cursor sources, the
  pointer-lock invariant, and the thread that owns `MouseCursor`.
  - [Sources and threading](features/cursor-pipeline-sources-and-threading.md)
  - [The overlay pointer and "Use everywhere"](features/cursor-pipeline-overlay-pointer.md)
  - [The overlay freeze](features/cursor-pipeline-freeze.md)
  - [Locked pointer and the SDL3 device](features/cursor-pipeline-locked-pointer.md)
  - [Hotspot, stretched resolution and oversized windows](features/cursor-pipeline-hotspot-and-geometry.md)
  - [Doubled pointer speed and the absolute notify](features/cursor-pipeline-pointer-speed.md)

### Rendering

- [Vulkan compositing](features/compositing-vulkan.md) — the per-frame Vulkan compositing pass.
- [Scaling filters](features/scaling-filters.md) — FSR (EASU/RCAS) and NIS upscaling.
- [HDR color management](features/hdr-color-management.md) — PQ conversion and color management.
- [Resolution and refresh](features/resolution-and-refresh.md) — runtime nested game
  resolution, paced refresh, the frame limiter and low-latency wake-up (the Display areas).
- [Reshade effects](features/reshade-effects.md) — the ReShade-compatible post-process
  pipeline, for users' own `.fx` files.
- [Shader effects (Shaders area)](features/shader-effects.md) — hub: the native build-time
  compute pre-pass and the panel that drives it.
  - [Pre-pass, uniforms and panel budget](features/shader-effects-pipeline.md)
  - [Colour, shadow and sharpen effects](features/shader-effects-color.md)
  - [Bloom and the Brightness Map history](features/shader-effects-bloom.md)
  - [Adaptive Brightness: modes, ceiling](features/shader-effects-adaptive-brightness.md)
  - [Adaptive Brightness: local adaptation, preview, pulse](features/shader-effects-adaptive-brightness-local.md)
  - [Adaptive Gamma and Leave dark scenes alone](features/shader-effects-adaptive-gamma.md)
  - [Adaptive Brightness V2](features/shader-effects-adaptive-brightness-v2.md)
  - [Adaptive Brightness V2 Darkening](features/shader-effects-darkening.md)

### Motion (frame pacing)

- [Frame generation](features/frame-generation.md) — hub: optical-flow frame
  interpolation (fixed 2x-8x or Target fps), paced per vblank; the `fghost` renderer glue
  and the library's pacer.
  - [Settings and status](features/frame-generation-settings.md)
  - [Pacing](features/frame-generation-pacing.md)
  - [Renderer and HDR / 10-bit](features/frame-generation-renderer.md)
  - [Backends, host frames and direct scanout](features/frame-generation-host-scanout.md)
  - [UI protection (Crosshair box, Crosshair V2)](features/frame-generation-ui-protection.md)
  - [Motion version 2](features/frame-generation-motion-v2.md)
  - [Learned motion](features/frame-generation-learned.md)
  - [Pass-through, costs, limitations, build and testing](features/frame-generation-limits.md)
- [Motion blur](features/motion-blur.md) — interpolation-based blur, with or without frame
  generation; the shared-timeline composition model.
- [Lag spike buffer](features/lag-spike-buffer.md) — bridges frame-time spikes by running
  slightly behind real time; independent of frame generation.

### Settings overlay (UI framework)

- [Overlay shell](features/overlay-shell.md) — the Shell: slab, rail groups, sheet,
  Inspector, rows and controls under `src/Overlay/UI/`, and how to register an area.
- [Command palette and Launcher](features/command-palette.md) — the ranked search over
  every setting, drawn over the Shell or alone as the Launcher.
- [Launch-option lock](features/launch-option-lock.md) — rows pinned by a `gamescope`
  command-line flag go read-only with a "LAUNCH OPTION" tag.
- [Notifications](features/notifications.md) — toasts: the independent-context shape, the
  lazy glyph-bake hazard, the launch warm-up and why it never pushes a layer.
- [Log and About](features/log-and-about.md) — the two read-only areas at the end of the
  rail: captured log, version identity, compiled-in changelog and licences.
- [System and Appearance](features/system-and-appearance.md) — the two small areas of the
  SETTINGS rail group.
- [Diagnostics](features/diagnostics.md) — the OTHER > Diagnostics area: Hyprland direct
  scanout check, why its warnings are switched off, the Recheck button.

### Settings & config

- [Config system](features/config-system.md) — JSON settings files, schema versioning and
  migrations.
- [Profiles](features/profiles.md) — general vs game profiles, diff-based inheritance, the
  Profiles area, `--profile` / `ritz_profile`, the schema 2 -> 3 migration.
- [Keybinds](features/keybinds.md) — the editable hotkeys: chord grammar, tap-vs-press
  rules, `overlay.keybinds`, the conflict rule and the ways back from a lock-out.

### Drawn over the game

- [FPS display (HUD)](features/fps-display.md) — hub: the single-integer FPS HUD.
  - [Readout behaviour](features/fps-display-readout.md)
  - [Text colour (Fixed vs. Inverted)](features/fps-display-text-colour.md)
  - [Margin](features/fps-display-margin.md)
- [Crosshair](features/crosshair.md) — the compositor-drawn crosshair; why it lives in the
  HUD's layer.
- [Zoom](features/zoom.md) — the hold-or-toggle magnifier, built inside
  `vulkan_composite()` as layer 1; held-action and mouse-button rules.

### Input

- [Autoclicker](features/autoclicker.md) — the paced synthetic click train and its
  feedback-loop guard.
- [Null binds](features/null-binds.md) — WASD SOCD cleaning with delay and jitter.
- [Input › General](features/input-general.md) — force grab cursor and keyboard.
- [Input emulation](features/input-emulation.md) — synthetic input injection (libei).
- [Input method / IME](features/input-method-ime.md) — input method editor support.

### Audio

- [Mixer](features/audio-mixer.md) — per-app PipeWire volume, detection strategies, the
  Mixer hotkey; also the Effects area.
- [Game audio capture](features/audio-capture.md) — the private per-session sink, the env
  tag, the mover, and the Dynamics (compressor/limiter) stage.

### Steam surfaces

- [Steam friends you can join](features/steam-friends.md) — the joinable-friends list read
  from the running Steam client, and the privacy rule.

### External surfaces

- [Vulkan WSI layer](features/vk-wsi-layer.md) — swapchain redirection layer.
- [Wayland protocols](features/wayland-protocols.md) — custom protocol surface, including
  `gamescope_control`.
- [Control / IPC](features/control-ipc.md) — `gamescopectl` and external control.
- [Ritz extension](features/ritz-extension.md) — the `extensions/gamescope-ritz.json`
  launcher module.
- [Screen capture (PipeWire)](features/screen-capture-pipewire.md) — streaming export of
  the composited output.
- [Clipboard sync](features/clipboard-sync.md) — one CLIPBOARD across Xwayland, gamescope's
  Wayland clients and the host.

### Tooling & runtime

- [Scripting / ConVars](features/scripting-convars.md) — the runtime tunable-variable and
  debug-command system.
- [Build and tooling](features/build-and-tooling.md) — Meson build, agent build scripts.
- [Process management](features/process-management.md) — child-process and Xwayland
  lifecycle.

## ui/ — styling rules

- [Styling guide](ui/STYLING-GUIDE.md) — the rules and snippets for settings-overlay
  controls. **Authoritative** (with the code tokens in `src/Overlay/UI/Tokens.cpp` and
  `Colors.cpp`) over `planning/ui-design-guide.md`, which is partly behind the code.

## claude-instructions/ — mandatory rules for agents

- [Working with the Docs](claude-instructions/documentation.md) — read before you touch,
  update after you change.
- [Documentation version policy](claude-instructions/documentation-version-policy.md) — one
  semver bump per dated block, sized to that day's user-facing magnitude.
- [Changelog maintenance](claude-instructions/changelog.md) — the strict `CHANGELOG.md`
  format and why that file is the version marker.

## meta/

- [Terminology](meta/TERMINOLOGY.md) — project vocabulary glossary.

## planning/ — historical design notes, not current behaviour

110 files, about 11.8 MB. These are scouting reports, research, specs and decision logs
written while the overlay was designed; many carry a RESOLVED banner or describe a path
not taken. **Read them for the why behind a decision, never for what the code does now.**
About 8.6 MB of the total is 29 audit screenshots in
`planning/redesign/round-2/e2-inspector-plus/audit-shots/`; do not open that folder
casually. `planning/ui-design-guide.md` is partly behind the code: use
[ui/STYLING-GUIDE.md](ui/STYLING-GUIDE.md) and the code tokens instead.

- [TODO](planning/TODO.md) — open work with no feature doc yet.
- [SPEC](planning/SPEC.md), [DECISIONS](planning/DECISIONS.md),
  [ISSUES](planning/ISSUES.md) — the original overlay master spec, decision record and issue
  breakdown (2026-08-21).
- [Overlay presentation architecture](planning/overlay-presentation-architecture.md) —
  how the overlay is presented to the compositor; the fullscreen-VRR "mystery" is resolved
  in it.
- [ImGui overlay feasibility](planning/imgui-overlay-feasibility.md) — the first verdict.
- [Config system](planning/config-system.md), [Profiles concept](planning/profiles-concept.md),
  [App-ID detection](planning/appid-detection.md) — scouting for the config layer and
  profiles (current behaviour: [config-system](features/config-system.md),
  [profiles](features/profiles.md)).
- [Config-UI research](planning/config-ui-research.md),
  [UI design guide](planning/ui-design-guide.md) (partly stale),
  [UI mockup precise spec](planning/ui-mockup-precise-spec.md),
  [Slider widget spec](planning/slider-widget-spec.md) — UI design notes.
- [Runtime knobs and FPS](planning/runtime-knobs-and-fps.md),
  [PipeWire loudness](planning/pipewire-loudness.md),
  [ReShade shaders](planning/reshade-shaders.md) — feature scouting.
- [Feature ideas](planning/feature-ideas-research.md) and
  [Feature ideas 2026-09-05](planning/feature-ideas-2026-09-05.md) — prioritised ideas.
- [Requests 2026-09-04](planning/requests-2026-09-04.md),
  [2026-09-05](planning/requests-2026-09-05.md),
  [2026-09-05 round 2](planning/requests-2026-09-05-round2.md),
  [2026-09-06](planning/requests-2026-09-06.md),
  [2026-09-07](planning/requests-2026-09-07.md),
  [2026-09-08](planning/requests-2026-09-08.md) — dated user request batches.
- Steam friends: [window study](planning/steam-friends-window.md),
  [joining](planning/steam-friends-join.md),
  [invite and vtable layout](planning/steam-invite-and-vtable-layout.md).
- Frame generation: [FidelityFX optical flow](planning/fidelityfx-opticalflow-framegen.md),
  [external-layer in-tree port](planning/external-framegen-in-tree-port.md),
  [external-layer integration](planning/external-framegen-layer-integration.md) (the
  external layer is not used in-tree).
- [Adaptive Brightness v2 plan](planning/adaptive-brightness-v2-plan.md) — research and the
  operator design, implemented alongside the older effects.
- Flicker investigation (all RESOLVED, the backend was the cause):
  [client variation test](planning/client-variation-test.md),
  [A/B test plan](planning/flicker-ab-test-plan.md),
  [upstream regression bisection](planning/upstream-flicker-regression.md),
  [SDL backend report draft](planning/upstream-sdl-backend-flicker-report.md),
  [Wayland VRR buffer lifetime](planning/wayland-vrr-buffer-lifetime.md).
- [Overlay redesign](planning/redesign/README.md) — three preserved directions (A console,
  B command palette, E inspector rail) with mockups and specs, then round 2 under
  `planning/redesign/round-2/`. The chosen E2 direction's implementation log,
  [IMPLEMENTATION.md](planning/redesign/round-2/e2-inspector-plus/IMPLEMENTATION.md), plus
  its [SPEC](planning/redesign/round-2/e2-inspector-plus/SPEC.md),
  [API](planning/redesign/round-2/e2-inspector-plus/API.md),
  [SHELL-TEST-REPORT](planning/redesign/round-2/e2-inspector-plus/SHELL-TEST-REPORT.md) and
  [CONFORMANCE-AUDIT](planning/redesign/round-2/e2-inspector-plus/CONFORMANCE-AUDIT.md),
  sit next to the screenshots. For current behaviour read
  [overlay-shell](features/overlay-shell.md) instead.
- [Autonomous decisions](planning/redesign/AUTONOMOUS-DECISIONS.md) — every call made
  without the user, with its alternative and reasoning, so disagreeing is cheap.
