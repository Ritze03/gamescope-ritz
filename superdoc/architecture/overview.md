# Architecture Overview

The first doc to read in this tree. It orients a newcomer to gamescope-ritz's runtime
shape and links out to the pages under `../features/` rather than duplicating their
detail. Read this page for "where does X live and how do the pieces fit", then follow a
link for "how exactly does X work". The full page list is [../README.md](../README.md).

Symbols are written `file:symbol`, with no line numbers; `grep` the symbol.

## Big picture

gamescope is a Wayland micro-compositor purpose-built for gaming: it runs its own
embedded Wayland server, hosts one or more Xwayland servers for X11 clients, and
composites every visible layer through a hand-written Vulkan compute pipeline before
handing the result to one of five interchangeable output backends. There is no single
struct everything hangs off; instead there are two long-lived owners of state: the
active `IBackend` (`src/backend.h:IBackend`, one process-wide instance selected at
startup) and `steamcompmgr`'s window/focus tables (`src/steamcompmgr.cpp`), bridged mainly
by atomics and locks rather than a shared object graph. `src/main.cpp:main` is the one
place that wires all of it together at startup.

This fork's additions (settings overlay, HUD, zoom, crosshair, profiles, frame generation,
shader effects, audio capture) hang off the compositing pass and the per-frame
steamcompmgr loop described below.

## Subsystem map

### Upstream core

- **Entry / CLI** — `src/main.cpp:main` parses `getopt_long` flags, resolves output
  size/mode defaults, selects a backend (`auto_select_backend`, or an explicit
  `--backend`), constructs it via `gamescope::IBackend::Set<...>()`, calls
  `wlserver_init()` to stand up the embedded Wayland server and the first Xwayland
  server, spawns the steamcompmgr thread (`steamCompMgrThreadRun`), and finally blocks in
  `wlserver_run()`. Meson feature flags gate what is compiled in:
  [build-and-tooling](../features/build-and-tooling.md).
- **Backend abstraction** — `IBackend` and `IBackendConnector` (`src/backend.h`) are the
  display-output contract every output sink implements: init/deinit, present,
  cursor/hardware-plane capability, VRR/tearing support, and dmabuf import. Five
  implementations (`src/Backends/`): [DRM](../features/backend-drm.md) (owns a real KMS
  CRTC, "embedded mode"), [SDL](../features/backend-sdl.md) and
  [Wayland](../features/backend-wayland.md) (nested inside a desktop session, the latter
  zero-copy via `zwp_linux_dmabuf_v1`, the former through a Vulkan swapchain),
  [Headless](../features/backend-headless.md) (no output sink, capture/IPC only) and
  [OpenVR](../features/backend-openvr.md) (a SteamVR overlay). Only one is active per
  process.
- **Embedded Wayland server (wlserver)** — `wlserver_init` (`src/wlserver.cpp`) builds the
  `wl_display`, binds every protocol global gamescope serves (see
  [wayland-protocols](../features/wayland-protocols.md)), and spawns the first Xwayland
  server. `wlserver_run` is the **main thread's** own loop after startup: it `poll()`s the
  Wayland event-loop fd and a nudge pipe, and on activity takes `wlserver_lock()` and
  dispatches queued protocol requests (not the compositing loop; see Threading model).
- **steamcompmgr** — the X11 window-manager subsystem (see
  [Terminology](../meta/TERMINOLOGY.md)), entered via `steamcompmgr_main`
  (`src/steamcompmgr.cpp`) on its own thread. Owns the window/focus tables, dispatches
  Xwayland/XDG surface events and drives the vblank-paced composite/present loop. Focus
  arbitration: [steamcompmgr-focus](../features/steamcompmgr-focus.md).
- **Vulkan compositor** — `vulkan_composite()` (`src/rendervulkan.cpp`) turns a per-frame
  `FrameInfo_t` layer stack (`src/rendervulkan.hpp`) into a finished image via
  hand-written compute shaders (blit/FSR/NIS/blur), with the frame-generation hook, the
  bundled effects pre-pass, ReShade, zoom and HDR colour management folded into the same
  function. See [compositing-vulkan](../features/compositing-vulkan.md),
  [scaling-filters](../features/scaling-filters.md),
  [hdr-color-management](../features/hdr-color-management.md),
  [shader-effects](../features/shader-effects.md) and
  [reshade-effects](../features/reshade-effects.md).
- **Vblank timing** — `gamescope::CVBlankTimer` (`src/vblankmanager.hpp`, global singleton
  via `GetVBlankTimer()`) is the pacing clock the steamcompmgr loop waits on
  (`ProcessVBlank()` inside `steamcompmgr_main`) before deciding whether to repaint.
- **Protocol / IPC surface** — gamescope's private Wayland protocols
  (`protocol/gamescope-*.xml`) are the primary way external tools drive a running session:
  `gamescope_control`/`gamescope_private` for `gamescopectl` (`src/Apps/gamescopectl.cpp`)
  and the Steam client ([control-ipc](../features/control-ipc.md)), plus dedicated
  protocols for swapchain bypass, PipeWire capture, IME, hotkeys and ReShade, indexed on
  [wayland-protocols](../features/wayland-protocols.md).
- **Out-of-process WSI layer** — `layer/VkLayer_FROG_gamescope_wsi.cpp` is a separate
  shared object loaded *inside a game's own process*, not linked into the `gamescope`
  binary; it redirects the swapchain into gamescope's buffers over
  `gamescope-swapchain.xml`. This fork builds its own renamed copy. See
  [vk-wsi-layer](../features/vk-wsi-layer.md).
- **Scripting / ConVars** — `ConVar<T>`/`ConCommand` (`src/convar.h`) is the
  runtime-tunable-variable and debug-command system, optionally bound into an embedded Lua
  console (`CScriptManager`, `src/Script/Script.h`). See
  [scripting-convars](../features/scripting-convars.md).
- **Input emulation / IME / clipboard** — libei virtual input
  ([input-emulation](../features/input-emulation.md), `src/InputEmulation.{h,cpp}`), the
  IME bridge ([input-method-ime](../features/input-method-ime.md), `src/ime.cpp`) and the
  clipboard sync ([clipboard-sync](../features/clipboard-sync.md), `src/Clipboard/`).
- **Latent/unwired code** — `gamescope::CLibInputHandler` (`src/LibInputHandler.h`) is a
  fully-implemented `IWaitable` for driving raw `libinput` events without a seat (intended
  for a VR global-input path) but has **no call site anywhere in the codebase**: it is dead
  code today, not an active feature. See
  [input-emulation](../features/input-emulation.md#how-it-works) for the verified detail.

### This fork's additions

- **Settings overlay (Shell, rail, Inspector)** — `src/SettingsOverlay.cpp`
  (`SettingsOverlay_AddLayer`, the ImGui layer pushed onto the frame) hosts
  `src/Overlay/UI/` (`Shell.cpp`, `Registry.{h,cpp}`, `Row.cpp`, `Controls.cpp`, `Band.cpp`,
  `Lane.cpp`, `Tokens.cpp`, `Colors.cpp`, `Icons.cpp`, `Layout.cpp`) and one `Panel*.cpp`
  per area under `src/Overlay/`. See [overlay-shell](../features/overlay-shell.md) and
  [../ui/STYLING-GUIDE.md](../ui/STYLING-GUIDE.md).
- **Command palette and Launcher** — `src/Overlay/UI/CommandPalette.{h,cpp}` (ranked
  search over every Entry and Parameter), drawn by `Shell.cpp` either over the Shell or
  alone as the Launcher. See [command-palette](../features/command-palette.md).
- **Config and profiles** — `src/Config/` (`ConfigManager.{h,cpp}`, `ConfigSchema.h`,
  `AppId.{h,cpp}`, `NestedDefault.h`): JSON settings files, schema migrations, profile
  selection (`SelectProfile`, `SessionProfile`), game-profile inheritance and app-id
  resolution. The Profiles area is `src/Overlay/PanelConfig.cpp`. See
  [config-system](../features/config-system.md) and [profiles](../features/profiles.md).
- **Keybinds** — `src/Keybinds.{h,cpp}` (chord grammar, `ProcessKey`, held actions,
  capture), hooked from `wlserver.cpp` (`wlserver_ritz_mouse_hotkey`, the key handlers);
  editor `src/Overlay/PanelKeybinds.cpp`. See [keybinds](../features/keybinds.md).
- **HUD (FPS display)** — `src/Overlay/FpsDisplay.{h,cpp}` (`DrawReadout`,
  `FpsDisplay_AddLayer`), area `system.hud`. See
  [fps-display](../features/fps-display.md).
- **Zoom** — `src/Overlay/Zoom.{h,cpp}` (`Zoom_FillRequest`, called per frame from
  `paint_all`), recorded inside `vulkan_composite()` with `src/shaders/cs_zoom.comp`. See
  [zoom](../features/zoom.md).
- **Crosshair** — `src/Overlay/Crosshair.{h,cpp}` and `CrosshairMath.h`, drawn into the
  HUD's layer. See [crosshair](../features/crosshair.md).
- **Cursor** — `src/Overlay/CursorArt.cpp` (the overlay pointer), `PanelCursor.cpp`,
  `src/x11cursor.cpp`, `src/CursorPolicy.h`, `src/PointerMapping.h`, and the `MouseCursor`
  class (declared in `src/steamcompmgr.hpp`, defined in `src/steamcompmgr.cpp`). See [cursor-pipeline](../features/cursor-pipeline.md).
- **Input extras** — Autoclicker (`src/Overlay/Autoclicker.{h,cpp}`,
  `Autoclicker_IsInjecting`) and Null binds (`gamescope::nullbinds::Engine`,
  `src/Overlay/NullBinds.{h,cpp}`), a pure engine hooked from the **top** of both
  `wlserver_key()` and `wlserver_handle_key()` (`src/wlserver.cpp`, under
  `wlserver_lock()`, guarded by `!NullBinds_IsInjecting()` so its own synthetic key events
  do not re-enter it); the second call site exists because a real keyboard under
  DRM/embedded mode never goes through `wlserver_key()`. `NullBinds_Tick()` is called once
  a frame from `paint_all` next to `Zoom_FillRequest()`. Force grab cursor/keyboard are
  `src/Overlay/PanelInput.cpp`. See [autoclicker](../features/autoclicker.md),
  [null-binds](../features/null-binds.md) and
  [input-general](../features/input-general.md).
- **Frame generation, motion blur and the lag spike buffer** (rail group MOTION) —
  `src/FrameGen/`: `FrameGenHost.{h,cpp}` (`fghost`, the renderer glue: frame ring, lazy
  per-slot synthesis, the one `recordSynthBlur` call, own command buffer, status; `Config`,
  `BlurConfig`, `LagBufferConfig`), `TearHint.h`, `HostShown.h`, and `FrameGenLib.cpp`
  (compiles the `subprojects/FrameGen` library, including its pacer
  `framegen::pacing` in `gpu/pacing.h`, into the binary). Glue: `FrameGen_OnArrival`,
  `FrameGen_PrePaint`, `FrameGen_PostPaint` in `src/steamcompmgr.cpp`; the hook is
  `fghost::RecordBaseLayer` in `vulkan_composite()`. Panels: `PanelFrameGen.cpp`,
  `PanelMotionBlur.cpp`, `PanelLagBuffer.cpp`; Inspector box picture
  `UiBoxPreview.{h,cpp}`. See [frame-generation](../features/frame-generation.md),
  [motion-blur](../features/motion-blur.md) and
  [lag-spike-buffer](../features/lag-spike-buffer.md).
- **Shader effects** — `src/shaders/effects_*.h` and `cs_effects_*.comp`, the
  `NativeEffectsState_t g_nativeEffects` state (`src/rendervulkan.hpp`) and pre-pass in
  `vulkan_composite()`, panel `src/Overlay/PanelShaders.cpp`
  (`PanelShaders_RegisterArea`), live preview `EffectPreview.{h,cpp}`. See
  [shader-effects](../features/shader-effects.md).
- **Frame limiter and refresh** — `g_nSteamCompMgrTargetFPS` in `src/steamcompmgr.cpp`
  holds commits to the vblank grid; `src/LimiterPace.h` decides when the WSI layer paces
  the game on its own timer. Nested resolution/refresh: `steamcompmgr_set_nested_mode()`.
  See [resolution-and-refresh](../features/resolution-and-refresh.md).
- **Audio** — `src/Audio/Volume.{h,cpp}` (Mixer, wpctl-based stream guessing, fallback;
  panel `PanelAudio.cpp`), `Capture.{h,cpp}` (a private PipeWire sink per session, a
  loopback pair on its own `pw_thread_loop`, thread `"gamescope-audio"`; session tag
  injected by `ApplyGameEnvironment()` from `LaunchNestedChildren()` in
  `src/steamcompmgr.cpp`; live switch `audio.capture` via `ritz_apply_config_live()` in
  `main.cpp`) and `Dynamics.{h,cpp}` (compressor/limiter in the capture's realtime
  playback callback). See [audio-mixer](../features/audio-mixer.md) and
  [audio-capture](../features/audio-capture.md).
- **Notifications** — `src/Overlay/Notifications.{h,cpp}` (`Notifications::Show`,
  `AddLayer`, `WarmUp`). See [notifications](../features/notifications.md).
- **Launch-option lock** — `src/LaunchOptions.{h,cpp}` (`MarkGiven`, `Given`): a row whose
  setting a `gamescope` flag pinned goes read-only. See
  [launch-option-lock](../features/launch-option-lock.md).
- **Friends** — `src/SteamFriends.{h,cpp}` (background poller, `Snapshot`, `Join`),
  `SteamFriendsCmd.h`, panel `src/Overlay/PanelFriends.cpp`. See
  [steam-friends](../features/steam-friends.md).
- **Diagnostics** — `src/Overlay/Diagnostics.{h,cpp}` (launch-environment snapshot taken
  first in `main()`, a worker thread running `hyprctl`), `DiagnosticsLogic.h` (pure
  parsing and state decisions), `PanelDiagnostics.cpp`; `Diagnostics_Tick()` is called
  every frame from `SettingsOverlay_AddLayer`. See [diagnostics](../features/diagnostics.md).
- **Log, About, System, Appearance** — `src/Overlay/LogCapture.{h,cpp}`, `PanelLog.cpp`,
  `PanelChangelog.cpp` (`ChangelogParse.cpp`, `embed_changelog.py`),
  `PanelSystem.cpp`, `PanelConfig.cpp`. See [log-and-about](../features/log-and-about.md)
  and [system-and-appearance](../features/system-and-appearance.md).
- **Ritz launcher module** — `extensions/gamescope-ritz.json`:
  [ritz-extension](../features/ritz-extension.md).

## Threading model

*Verified against source: the division of labor is the reverse of the naive "main thread
renders" assumption.*

Two long-lived threads:

- **The main thread** finishes startup (backend construction, `wlserver_init()`, spawning
  the steamcompmgr thread in `main`), then calls `wlserver_run()` (thread name
  `"gamescope-wl"`) and stays there until shutdown. That loop is a plain `poll()` over the
  Wayland event-loop fd and an internal nudge pipe; it dispatches **Wayland protocol
  requests** from connected clients, not frame presentation.
- **The steamcompmgr thread**, running `steamCompMgrThreadRun` (`src/main.cpp`) →
  `steamcompmgr_main`, owns X11 window management *and* the vblank-paced render/present
  loop in the same `for (;;)` body: each iteration dispatches queued Xwayland/XDG events,
  polls `g_SteamCompMgrWaiter` (the vblank timer, the VRR limiter timer and the
  frame-generation extra timer are registered into it in `steamcompmgr_main`), checks
  `GetVBlankTimer().ProcessVBlank()` and, when a repaint is due, calls `paint_all()`,
  which builds `FrameInfo_t` and presents through the active backend. *Why one
  loop:* focus/window state and the frame about to be composited must stay consistent
  within one pass; splitting them would need per-frame synchronisation anyway.

They share state via **locks and atomics, not message passing**: `wlserver_lock()` /
`wlserver_unlock()` guard the Wayland-server-owned state the steamcompmgr thread also
touches; cross-thread focus changes go through a dirty-serial bump (`MakeFocusDirty()`,
`src/steamcompmgr.cpp`) plus `nudge_steamcompmgr()` (writes to a pipe the waiter polls)
rather than a queued message; see
[steamcompmgr-focus](../features/steamcompmgr-focus.md#using-it). *Why:* the two threads
cooperate on one shared frame/focus model with tight latency requirements (every vblank),
not passing discrete work items between independently paced stages, so a lock/atomic per
field keeps the hot path free of queue/allocation overhead, at the cost of careful
dirty-flag discipline.

Further threads, independent of the two above:

- **PipeWire screen streaming** — `init_pipewire()` (`src/pipewire.cpp`) spawns a thread
  named `"gamescope-pw"` running a private blocking `poll()` loop. *Why:* PipeWire's
  `pw_loop` expects to own its own iterate cycle, so coupling it to the compositor-thread
  waiter would tie PipeWire's dispatch cadence to compositing. See
  [screen-capture-pipewire](../features/screen-capture-pipewire.md#threading-model-verified).
- **libei input emulation** — `GamescopeInputServer` is registered as an `IWaitable` on
  `gamescope::CAsyncWaiter g_LibEisWaiter("gamescope-eis")` (`src/wlserver.cpp`).
  `CAsyncWaiter` (`src/waitable.h`) owns a small thread (`m_Thread`) that only runs a
  generic epoll loop calling waitables' `OnPollIn()`. See
  [input-emulation](../features/input-emulation.md#threading-model-verified).
- **Game audio capture** — its own `pw_thread_loop` (`gamescope-audio`), with a realtime
  playback callback that also runs Dynamics. See [audio-capture](../features/audio-capture.md).
- **Autoclicker worker, Friends poller, Diagnostics worker** — small background threads
  that keep clicks, Steam reads and `hyprctl` off the frame path
  ([autoclicker](../features/autoclicker.md), [steam-friends](../features/steam-friends.md),
  [diagnostics](../features/diagnostics.md)).

## Frame data flow, end to end

A client commits a buffer → `commit_t` (`src/commit.cpp`) makes it a sampleable Vulkan
texture (memoized per `wlr_buffer` via `CBufferMemo`/`CBufferMemoizer`, `src/BufferMemo.h`,
so a still frame is not re-imported every vblank) → steamcompmgr marks the window's commit
ready and, on its next vblank pass through `steamcompmgr_main`'s loop, re-derives focus if
dirty ([steamcompmgr-focus](../features/steamcompmgr-focus.md)) → `FrameGen_PrePaint`
decides what this vblank shows ([frame-generation](../features/frame-generation.md)) →
`paint_all()` walks the focused window stack, fills a `FrameInfo_t` with one `Layer_t` per
visible layer (base plane, override, underlay, decorations, cursor, plus this fork's
`Zoom_FillRequest`, `SettingsOverlay_AddLayer`, HUD, crosshair and notification layers) →
`vulkan_composite()` runs the frame-generation hook (`fghost::RecordBaseLayer`), the
ReShade pass if a user `.fx` is active ([reshade-effects](../features/reshade-effects.md)),
the bundled effects pre-pass if any is on ([shader-effects](../features/shader-effects.md)),
zoom as layer 1, the FSR/NIS/blit scaling pass
([scaling-filters](../features/scaling-filters.md)), and binds the per-EOTF shaper/3D LUTs
and CTM ([hdr-color-management](../features/hdr-color-management.md)) → the active
`IBackendConnector::Present()` (`src/backend.h`) either submits a real DRM KMS atomic
commit (DRM backend) or presents through a Vulkan swapchain / dmabuf hand-off (nested
backends); see the relevant `backend-*.md` page.

Two taps ride the same composited frame without a second composite pass: a **screenshot**
([control-ipc](../features/control-ipc.md#key-requests), `CScreenshotManager` in
`src/steamcompmgr_shared.hpp`), and, when PipeWire streaming is active, a blit into the
`pPipewireTexture` argument of `vulkan_composite()` in the *same command buffer*
([screen-capture-pipewire](../features/screen-capture-pipewire.md)).

```
client commit
      │  wlr_buffer → Vulkan texture (memoized)
      ▼
commit.cpp:commit_t                     [import/cache]
      │  window marked ready; focus rerolled if dirty
      ▼
steamcompmgr.cpp:paint_all              [owns window/focus state, vblank-paced]
      │  build FrameInfo_t (layer stack)
      ▼
rendervulkan.cpp:vulkan_composite       [framegen → reshade → effects pre-pass → zoom → scale/upscale → colour mgmt]
      │  ├─ screenshot tap (control-ipc)
      │  └─ pipewire tap (same cmd buffer)
      ▼
IBackendConnector::Present              [DRM atomic commit | swapchain | dmabuf]
```

## Upstream focus-arbitration work (NOT this fork's divergence)

This repo's base commit `fcc1341` is exactly upstream `ValveSoftware/gamescope` HEAD, so
the OpenVR-backend and steamcompmgr focus commits at its tip are Valve's own work; this
fork's own work is the additive set above (historical design:
[overlay-presentation-architecture](../planning/overlay-presentation-architecture.md)).
See [backend-openvr](../features/backend-openvr.md) (`COpenVRConnector::UpdateVisibility`
in `src/Backends/OpenVRBackend.cpp` now takes focus on the visible edge) and
[steamcompmgr-focus](../features/steamcompmgr-focus.md) for the upstream fixes.

## Where to look for X

**Upstream core**

- **Output backend choice** — `auto_select_backend` and the `IBackend::Set<...>()`
  dispatch in `src/main.cpp:main`; then the relevant `backend-*.md` page.
- **Focus/keyboard input went to the wrong window** —
  [steamcompmgr-focus](../features/steamcompmgr-focus.md), starting with the `focus_info`
  ConCommand and `g_VirtualConnectorFocuses` (`src/steamcompmgr.cpp`).
- **A frame is not compositing/presenting correctly** — walk the data-flow sketch;
  `paint_all()` (`src/steamcompmgr.cpp`) and `vulkan_composite()`
  (`src/rendervulkan.cpp`) are the two functions to break on.
- **A layer (HUD, notification, crosshair, cursor, override, ...) silently vanished** —
  `LayerStack_t::push()` (`src/rendervulkan.hpp`) returns `nullptr` once a frame's
  `FrameInfo_t` hits `k_nMaxLayers` (6); callers log a rate-limited warning naming the
  dropped layer, and the `layer_budget_stats` ConCommand (`cc_layer_budget_stats` in `src/rendervulkan.cpp`) prints
  the drop count and the highest layer count reached.
- **Drive gamescope from outside** — [control-ipc](../features/control-ipc.md)
  (`gamescopectl`, convars, screenshots) and
  [wayland-protocols](../features/wayland-protocols.md) for the protocol index.
- **A game's swapchain is not taking the fast path** —
  [vk-wsi-layer](../features/vk-wsi-layer.md)'s `canBypassXWayland()`
  (`layer/VkLayer_FROG_gamescope_wsi.cpp`) eligibility gate.
- **Copy/paste is not crossing in or out** — [clipboard-sync](../features/clipboard-sync.md):
  `gamescope_broadcast_clipboard()` (`src/steamcompmgr.cpp`), fed by the data-control
  protocols in `src/Backends/WaylandBackend.cpp`.
- **Resolution, refresh, frame limit** —
  [resolution-and-refresh](../features/resolution-and-refresh.md):
  `steamcompmgr_set_nested_mode()` and `g_nSteamCompMgrTargetFPS` in
  `src/steamcompmgr.cpp`.
- **A new build/feature flag** — [build-and-tooling](../features/build-and-tooling.md)'s
  Meson options table (`meson.build`, `src/meson.build`).

**Settings overlay and config**

- **Shell, rail, rows, Inspector; add a new settings area** —
  [overlay-shell](../features/overlay-shell.md): `src/Overlay/UI/Registry.{h,cpp}`
  (`RailOrder`, `RailGroupForId`, `PanelXxx_RegisterArea` pattern in `Panel*.cpp`),
  `Shell.cpp`; visual rules in [../ui/STYLING-GUIDE.md](../ui/STYLING-GUIDE.md).
- **Command palette / Launcher search or ranking** —
  [command-palette](../features/command-palette.md): `src/Overlay/UI/CommandPalette.cpp`
  (`Score`, `Build`), `Shell::RequestPalette` / `RequestLauncher` (`Shell.h`).
- **A setting's config key, schema or migration; profiles; app id** —
  [config-system](../features/config-system.md) and [profiles](../features/profiles.md):
  `src/Config/ConfigManager.cpp`, `ConfigSchema.h`, `SelectProfile`, `SessionProfile`,
  `ResolveAppId` (`src/Config/AppId.cpp`).
- **A row is read-only with a "LAUNCH OPTION" tag** —
  [launch-option-lock](../features/launch-option-lock.md): `src/LaunchOptions.cpp`
  (`MarkGiven`, `Given`).
- **A toast** — [notifications](../features/notifications.md):
  `src/Overlay/Notifications.cpp` (`Notifications::Show`).
- **Log, About/changelog, System, Appearance areas** —
  [log-and-about](../features/log-and-about.md),
  [system-and-appearance](../features/system-and-appearance.md).
- **Diagnostics tab (Hyprland direct scanout check)** —
  [diagnostics](../features/diagnostics.md): `src/Overlay/Diagnostics.cpp`,
  `DiagnosticsLogic.h`.

**Drawn over the game**

- **HUD / FPS number** — [fps-display](../features/fps-display.md):
  `src/Overlay/FpsDisplay.cpp` (`DrawReadout`, `FpsDisplay_AddLayer`).
- **Crosshair** — [crosshair](../features/crosshair.md): `src/Overlay/Crosshair.cpp`.
- **Zoom** — [zoom](../features/zoom.md): `src/Overlay/Zoom.cpp` (`Zoom_FillRequest`),
  `src/shaders/cs_zoom.comp`.
- **Cursor (overlay pointer, hotspot, pointer lock)** —
  [cursor-pipeline](../features/cursor-pipeline.md): `src/Overlay/CursorArt.cpp`,
  `src/x11cursor.cpp`, `MouseCursor` (`src/steamcompmgr.hpp`), `src/PointerMapping.h`.

**Input**

- **A hotkey or chord misbehaves** — [keybinds](../features/keybinds.md):
  `src/Keybinds.cpp` (`ProcessKey`, `ParseChord`), `wlserver_ritz_mouse_hotkey`
  (`src/wlserver.cpp`).
- **Autoclicker** — [autoclicker](../features/autoclicker.md):
  `src/Overlay/Autoclicker.cpp`.
- **A/D or W/S are not behaving like normal keys** — [null-binds](../features/null-binds.md):
  the hook sits at the top of both `wlserver_key()` and `wlserver_handle_key()` in
  `src/wlserver.cpp`; engine `src/Overlay/NullBinds.cpp`.
- **Force grab cursor/keyboard** — [input-general](../features/input-general.md):
  `src/Overlay/PanelInput.cpp`.

**Audio**

- **Per-app volume (Mixer)** — [audio-mixer](../features/audio-mixer.md):
  `src/Audio/Volume.cpp`, `src/Overlay/PanelAudio.cpp`.
- **Capture sink and Dynamics (compressor)** — [audio-capture](../features/audio-capture.md):
  `src/Audio/Capture.cpp`, `src/Audio/Dynamics.cpp`.

**Image and frame pacing**

- **Shader effects (saturation, bloom, adaptive brightness/gamma, ...)** —
  [shader-effects](../features/shader-effects.md): `src/shaders/effects_*.h`,
  `cs_effects_*.comp`, `PanelShaders_RegisterArea` (`src/Overlay/PanelShaders.cpp`).
- **Frame generation misbehaves, stutters or adds delay** —
  [frame-generation](../features/frame-generation.md): `src/FrameGen/FrameGenHost.cpp`
  (`fghost`), `FrameGenLib.cpp`, `FrameGen_PrePaint` / `FrameGen_PostPaint` in
  `src/steamcompmgr.cpp`, the hook `fghost::RecordBaseLayer` in `vulkan_composite()`.
- **Motion blur** — [motion-blur](../features/motion-blur.md):
  `fghost::BlurConfig` (`src/FrameGen/FrameGenHost.h`), `src/Overlay/PanelMotionBlur.cpp`.
- **Lag spike buffer** — [lag-spike-buffer](../features/lag-spike-buffer.md):
  `fghost::LagBufferConfig`, `src/Overlay/PanelLagBuffer.cpp`.

**Steam**

- **Friends list / joining a friend** — [steam-friends](../features/steam-friends.md):
  `src/SteamFriends.cpp` (`Snapshot`, `Join`), `src/Overlay/PanelFriends.cpp`.

New to the codebase: read this page, then
[compositing-vulkan](../features/compositing-vulkan.md) (the render core),
[steamcompmgr-focus](../features/steamcompmgr-focus.md) and
[overlay-shell](../features/overlay-shell.md) (the fork's UI framework).

---
*Last updated: 2026-11-07*
