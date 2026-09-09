# gamescope-ritz — Documentation

A fork of Valve's gamescope, a Wayland micro-compositor for gaming. Point to
[architecture/overview.md](architecture/overview.md) as the start-here page.

- [`CHANGELOG.md`](../CHANGELOG.md) (repo root) — the user-facing list of this fork's own
  changes, newest version first. Written to the strict shape in
  [claude-instructions/changelog.md](claude-instructions/changelog.md), because the
  settings overlay parses it — and because the build reads its newest block's version as
  the project's own, this file **is** the version marker; there is no other.

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
- [Cursor pipeline](features/cursor-pipeline.md) — the three cursor sources, the
  pointer-lock invariant, and the thread that owns `MouseCursor`.

### Rendering

- [Vulkan compositing](features/compositing-vulkan.md) — the per-frame Vulkan compositing pass.
- [Scaling filters](features/scaling-filters.md) — FSR (EASU/RCAS) and NIS upscaling.
- [HDR color management](features/hdr-color-management.md) — PQ conversion and color management.
- [Resolution and refresh](features/resolution-and-refresh.md) — runtime nested game
  resolution, paced refresh and window-size request (the Display > Resolution area); what
  is live, what is only a request, and the SDL focus-refresh gotcha.

### HUD

- [Notifications](features/notifications.md) — toasts: the independent-context shape, the lazy glyph-bake/QueueWaitIdle hazard, the launch warm-up and why it never pushes a layer, the HUD seam.
- [FPS display](features/fps-display.md) — the single-integer FPS HUD: update modes,
  hide-above-X, backdrop, text-colour modes (with the Inverted-mode limitation stated
  plainly), shadow, and the lag-spike heuristic.
- [Crosshair](features/crosshair.md) — the compositor-drawn crosshair (line / dot /
  outline / right-click auto-hide / Apply Scaling): why it lives in the HUD's layer, the
  centre and 1px snapping rules, the hide modes' maths, and the `BTN_RIGHT` hook's gating.

### Settings & config

- [Profiles](features/profiles.md) -- Profiles v2: a profile is the file you edit,
  general vs game profiles, live diff-based inheritance and its ceiling, the file
  formats, the resolution order, the Profiles area (the list, the four modals, the
  inherited/overridden markers, the session badge), the API it calls, `--profile` /
  `ritz_profile`, and the schema 2 -> 3 migration table (the v1 model as history).
- [Keybinds](features/keybinds.md) -- the editable compositor hotkeys: the inventory of
  what this fork binds (and what it deliberately leaves alone), the chord grammar, the
  tap-vs-press firing rules and the peak set behind them, `overlay.keybinds` in
  `global.json`, the conflict rule, the two ways back from a binding that made the
  settings unreachable, and the capture chip.

### Steam surfaces

- [Steam friends you can join](features/steam-friends.md) -- `Ctrl+Shift+Tab` lists the
  friends who are in **this** game and can be joined, read from the running Steam
  client with no sign-in, and joins one with a `steam://joinlobby` URL: what is proven
  and what is not (the lobby offset), why the Status row carries two counts, why the
  area hides itself when this is not a Steam game, the background poller that keeps
  Steam off the frame path, and the privacy rule -- including that this compositor now
  makes no outbound network request at all.

### External surfaces

- [Vulkan WSI layer](features/vk-wsi-layer.md) — `VkLayer_FROG_gamescope_wsi` swapchain redirection.
- [Wayland protocols](features/wayland-protocols.md) — custom protocol surface, including `gamescope_control`.
- [Ritz extension](features/ritz-extension.md) — the `extensions/gamescope-ritz.json` Ritz
  launcher module: fields, the `--profile` shell-split quoting, and the installer's offer
  to copy it into `~/.config/ritz/extensions/`.
- [Control / IPC](features/control-ipc.md) — external control and IPC (e.g. `gamescopectl`).

### Clipboard

- [Clipboard sync](features/clipboard-sync.md) — one CLIPBOARD across Xwayland, gamescope's
  own Wayland clients and the host session: the data-control event hook and its fallback
  chain, the loop guard, and why every pipe transfer runs on a worker thread.

### Input

- [Input emulation](features/input-emulation.md) — synthetic input injection.
- [Input method / IME](features/input-method-ime.md) — input method editor support.

### Effects & capture

- [Reshade effects](features/reshade-effects.md) — the ReShade-compatible post-process
  pipeline, for users' own `.fx` files.
- [Shader effects (Shaders settings area)](features/shader-effects.md) — Saturation,
  Vibrancy, Shadow Control, Pre-Sharpen and Adaptive Brightness: the settings panel and
  the native build-time compute pre-pass that implements them.
- [Screen capture (PipeWire)](features/screen-capture-pipewire.md) — screen capture via PipeWire.

### Tooling & runtime

- [Scripting / ConVars](features/scripting-convars.md) — the runtime tunable-variable and debug-command system.
- [Build and tooling](features/build-and-tooling.md) — Meson build, dev tooling.
- [Process management](features/process-management.md) — process/Xwayland lifecycle management.

## planning/ — overlay UI implementation notes

- Frame-generation studies — [FidelityFX OpticalFlow / Frame Interpolation](planning/fidelityfx-opticalflow-framegen.md) (2026-09-05: feasible but not sensible — FI needs game depth and motion vectors a compositor never has) and the earlier [lsfg-vk in-tree port](planning/lsfg-in-tree-port.md) (GPLv3 blocker). Both conclude: run `lsfg-vk` as a layer under gamescope instead.
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
- [Profiles concept](planning/profiles-concept.md) — v2 (2026-09-06, file layer implemented): a profile is the file you edit, general and game profiles, live inheritance, `--profile <name>` for the session; why each decision, what it deleted, the UI still to build.
- [Feature ideas 2026-09-05](planning/feature-ideas-2026-09-05.md) — prioritised filters (native pre-pass and colour management) and features, with a Top 5.
- [Steam's Friends window inside gamescope](planning/steam-friends-window.md) — 2026-09-08:
  the real Friends window belongs to the host's Xwayland and cannot be moved, captured
  usefully, or clicked from a nested compositor; what *does* work (an interactive overlay
  on gamescope's own Xwayland, proven end to end) and why Shift+Tab may cover it already.
- [Joining friends from inside the game](planning/steam-friends-join.md) — 2026-09-08:
  the real goal was never the Friends window but *joining* a friend; why moving a window
  between X servers, a child session, a second Steam client and Steam's CEF debugging
  endpoint were each rejected (with the measurements), and the approach that wins — a
  native join list reading the joinable lobby id out of the running client's own
  `steamclient.so`, with no API key, no browser and no app id.
- [Inviting a friend, and the vtable layout that decides it](planning/steam-invite-and-vtable-layout.md)
  — 2026-09-09: `InviteUserToGame` pinned to a **named** vtable slot four independent
  ways (Proton's own bridge, CS2's shipped SDK, the live vtable lengths, and the live
  client's own thunks), which corrects the join page's "uniform one-slot shift" and its
  "no app id anywhere"; why no `steam://` invite URL exists; the one read the user must
  take; and why *receiving* invites is still a no.
- [Decisions taken without the user](planning/redesign/AUTONOMOUS-DECISIONS.md) — every
  call made while the user was away, with its alternative and its reasoning, so
  disagreeing is cheap.

## claude-instructions/ — mandatory rules for agents

- [Working with the Docs](claude-instructions/documentation.md) — read before you touch, update after you change.
- [Documentation version policy](claude-instructions/documentation-version-policy.md) — one semver bump per dated block, sized to that day's user-facing magnitude.
- [Changelog maintenance](claude-instructions/changelog.md) — the strict `CHANGELOG.md` format, its four categories, and why that file is the project's version marker.

## meta/

- [Terminology](meta/TERMINOLOGY.md) — project vocabulary glossary.
