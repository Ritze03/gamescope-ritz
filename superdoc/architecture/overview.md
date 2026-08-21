# Architecture Overview

The first doc to read in this tree. It orients a newcomer to gamescope-ritz's runtime
shape and links out to the 19 feature pages under `../features/` rather than
duplicating their detail — read this page for "where does X live and how do the
pieces fit," then follow a link for "how exactly does X work."

## Big picture

gamescope is a Wayland micro-compositor purpose-built for gaming: it runs its own
embedded Wayland server, hosts one or more Xwayland servers for X11 clients, and
composites every visible layer through a hand-written Vulkan compute pipeline before
handing the result to one of five interchangeable output backends. There is no single
struct everything hangs off; instead there are two long-lived owners of state — the
active `IBackend` (`src/backend.h:312`, one process-wide instance selected at startup)
and `steamcompmgr`'s window/focus tables (`src/steamcompmgr.cpp`) — bridged mainly by
atomics and locks rather than a shared object graph. `src/main.cpp:723 main` is the
one place that wires all of it together at startup.

## Subsystem map

- **Entry / CLI** — `src/main.cpp:723 main` parses `getopt_long` flags, resolves
  output size/mode defaults, selects a backend (`auto_select_backend`,
  `src/main.cpp:453`, or an explicit `--backend`), constructs it via
  `gamescope::IBackend::Set<...>()` (`src/main.cpp:980`-`1002`), calls
  `wlserver_init()` (`src/main.cpp:1062`) to stand up the embedded Wayland server and
  the first Xwayland server, spawns the steamcompmgr thread, and finally blocks in
  `wlserver_run()`. See [build-and-tooling](../features/build-and-tooling.md) for how
  the Meson feature flags gate which of this is even compiled in.
- **Backend abstraction** — `IBackend` (`src/backend.h:312`) and `IBackendConnector`
  (`src/backend.h:176`) are the display-output contract every output sink implements:
  init/deinit, present, cursor/hardware-plane capability, VRR/tearing support, and
  dmabuf import. Five implementations exist: [DRM](../features/backend-drm.md) (owns
  a real KMS CRTC — "embedded mode"), [SDL](../features/backend-sdl.md) and
  [Wayland](../features/backend-wayland.md) (nested inside a desktop session, the
  latter zero-copy via `zwp_linux_dmabuf_v1`, the former through a Vulkan swapchain),
  [Headless](../features/backend-headless.md) (no output sink at all — capture/IPC
  only), and [OpenVR](../features/backend-openvr.md) (a SteamVR overlay). Only one is
  active per process.
- **Embedded Wayland server (wlserver)** — `wlserver_init` (`src/wlserver.cpp:2019`)
  builds the `wl_display`, binds every protocol global gamescope serves (see
  [wayland-protocols](../features/wayland-protocols.md)), and spawns the first
  Xwayland server. `wlserver_run` (`src/wlserver.cpp:2254`) is the **main thread's**
  own loop after startup: it `poll()`s the Wayland event-loop fd and a nudge pipe,
  and on activity takes `wlserver_lock()` and dispatches queued Wayland protocol
  requests (`wl_event_loop_dispatch`). This is protocol dispatch, not the
  compositing/present loop — see Threading model below.
- **steamcompmgr** — the X11 window-manager subsystem (see
  [Terminology](../meta/TERMINOLOGY.md)), entered via `steamcompmgr_main`
  (`src/steamcompmgr.cpp:8670`) on its own thread. Owns the window/focus tables,
  dispatches Xwayland/XDG surface events, and — per the corrected threading model
  below — also drives the vblank-paced composite/present loop. Focus arbitration in
  particular is covered in depth on
  [steamcompmgr-focus](../features/steamcompmgr-focus.md).
- **Vulkan compositor** — `vulkan_composite()` (`src/rendervulkan.cpp:4030`) turns a
  per-frame `FrameInfo_t` layer stack into a finished image via hand-written compute
  shaders (blit/FSR/NIS/blur), with ReShade and HDR color-management steps folded
  into the same dispatch. See [compositing-vulkan](../features/compositing-vulkan.md),
  [scaling-filters](../features/scaling-filters.md),
  [hdr-color-management](../features/hdr-color-management.md), and
  [reshade-effects](../features/reshade-effects.md).
- **Vblank timing** — `gamescope::CVBlankTimer` (`src/vblankmanager.hpp:27`, global
  singleton via `GetVBlankTimer()`, `src/vblankmanager.cpp:405`) is the pacing clock
  the steamcompmgr loop waits on (`ProcessVBlank()`,
  `src/steamcompmgr.cpp:8882`) before deciding whether to repaint.
- **Protocol / IPC surface** — gamescope's private Wayland protocols
  (`protocol/gamescope-*.xml`) are the primary way external tools drive a running
  session: `gamescope_control`/`gamescope_private` for `gamescopectl` and the Steam
  client (see [control-ipc](../features/control-ipc.md)), plus dedicated protocols
  for swapchain bypass, PipeWire capture, IME, hotkeys, and ReShade — indexed in full
  on [wayland-protocols](../features/wayland-protocols.md).
- **Out-of-process WSI layer** — `VK_LAYER_FROG_gamescope_wsi` (`layer/`) is a
  separate shared object loaded *inside a game's own process*, not linked into the
  `gamescope` binary; it redirects the game's Vulkan swapchain into gamescope's
  buffers over `gamescope-swapchain.xml`, bypassing an XWayland composite hop. See
  [vk-wsi-layer](../features/vk-wsi-layer.md).
- **Scripting / ConVars** — `ConVar<T>`/`ConCommand` (`src/convar.h`) is the
  runtime-tunable-variable and debug-command system used throughout the codebase,
  optionally bound into an embedded Lua console (`CScriptManager`,
  `src/Script/Script.h:37`). See [scripting-convars](../features/scripting-convars.md).
- **Latent/unwired code** — `gamescope::CLibInputHandler` (`src/LibInputHandler.h:11`)
  is a fully-implemented `IWaitable` for driving raw `libinput` events without a seat
  (intended for a VR global-input path) but has **no call site anywhere in the
  codebase** — it is dead code today, not an active feature. See
  [input-emulation](../features/input-emulation.md#how-it-works) for the verified
  detail.

## Threading model

*Corrected against source — the code's actual division of labor is the reverse of the
naive "main thread renders" assumption; verify this against `src/steamcompmgr.cpp`
and `src/main.cpp` yourself if it matters to your task, don't take it on faith from
another doc.*

Two long-lived threads:

- **The main thread** finishes startup (backend construction, `wlserver_init()`,
  spawning the steamcompmgr thread at `src/main.cpp:1101`), then calls
  `wlserver_run()` (`src/wlserver.cpp:2254`, named `"gamescope-wl"`) and stays there
  until shutdown. That loop is a plain `poll()` over the Wayland event-loop fd and an
  internal nudge pipe — it dispatches **Wayland protocol requests** from connected
  clients (`wl_display_flush_clients` / `wl_event_loop_dispatch`), not frame
  presentation.
- **The steamcompmgr thread**, spawned at `src/main.cpp:1101` running
  `steamCompMgrThreadRun` → `steamcompmgr_main` (`src/steamcompmgr.cpp:8670`), owns
  X11 window management *and* the vblank-paced render/present loop in the same
  `for (;;)` body (from `src/steamcompmgr.cpp:8865`): each iteration dispatches
  queued Xwayland/XDG events, polls `g_SteamCompMgrWaiter` (which the vblank timer is
  registered into, `src/steamcompmgr.cpp:8813`), checks `GetVBlankTimer().ProcessVBlank()`
  (`:8882`), and — when a repaint is due — calls `paint_all()`
  (`src/steamcompmgr.cpp:2564`, called at `:9488`), which builds `FrameInfo_t` and
  presents through the active backend. *Why this reads unusual:* the vblank-paced
  render loop and the X11 window-manager loop are the same loop on the same thread,
  because focus/window state and the frame it's about to composite need to stay
  consistent within one pass — splitting them across threads would need to
  synchronize per-frame focus decisions against in-flight composites anyway.

They share state via **locks and atomics, not message passing**: `wlserver_lock()`/
`wlserver_unlock()` guard the Wayland-server-owned state the steamcompmgr thread also
touches; cross-thread focus changes go through a dirty-serial bump
(`MakeFocusDirty()`, `src/steamcompmgr.cpp:831`) plus `nudge_steamcompmgr()`
(`:7537`, writes to a pipe the waiter polls) rather than a queued message — see
[steamcompmgr-focus](../features/steamcompmgr-focus.md#using-it) for the concrete
example. *Why:* the two threads are cooperating on one shared frame/focus model with
tight latency requirements (every vblank), not passing discrete work items between
independently-paced stages — a lock/atomic-per-field model keeps the hot path free of
queue/allocation overhead, at the cost of needing careful dirty-flag discipline
instead of a simpler "owns its own mailbox" thread boundary.

Two additional subsystems each get their **own** dedicated OS thread, independent of
the two above:

- **PipeWire streaming** — `init_pipewire()` (`src/pipewire.cpp:671`) spawns a thread
  named `"gamescope-pw"` (`src/pipewire.cpp:746`) running a private, blocking `poll()`
  loop over the PipeWire loop's fd and an internal nudge pipe — not folded into any
  shared epoll waiter. *Why:* PipeWire's `pw_loop` expects to own its own iterate
  cycle, so coupling it to gamescope's compositor-thread waiter would tie PipeWire's
  dispatch cadence to compositing. See
  [screen-capture-pipewire](../features/screen-capture-pipewire.md#threading-model-verified).
- **libei input emulation** — by contrast, `GamescopeInputServer` is *not* a
  dedicated blocking-read thread; it's registered as an `IWaitable` on
  `gamescope::CAsyncWaiter g_LibEisWaiter("gamescope-eis")`
  (`src/wlserver.cpp:2014`, `:2190`). `CAsyncWaiter` (`src/waitable.h:376`) does own a
  small dedicated thread internally (`m_Thread`, `src/waitable.h:473`, started at
  `:380`), but that thread only runs a generic epoll loop calling registered
  waitables' `OnPollIn()` — the input-emulation code itself is poll-driven, not
  thread-driven. See
  [input-emulation](../features/input-emulation.md#threading-model-verified).

## Frame data flow, end to end

A client commits a buffer → `src/commit.cpp` turns it into a sampleable Vulkan
texture (memoized per-`wlr_buffer` via `CBufferMemo`/`CBufferMemoizer`,
`src/BufferMemo.h:21`/`:45`, so a still frame isn't re-imported every vblank) →
steamcompmgr marks the corresponding window's commit ready and, on its next vblank
pass through `steamcompmgr_main`'s loop, re-derives focus if dirty
([steamcompmgr-focus](../features/steamcompmgr-focus.md)) → `paint_all()`
(`src/steamcompmgr.cpp:2564`) walks the focused window stack, fills a `FrameInfo_t`
(`src/rendervulkan.hpp:281`) with one `Layer_t` per visible layer (base plane,
override, underlay, decorations, cursor) → `vulkan_composite()`
(`src/rendervulkan.cpp:4030`) runs the ReShade pass if active
([reshade-effects](../features/reshade-effects.md)), the FSR/NIS/blit scaling pass
([scaling-filters](../features/scaling-filters.md)), and binds the per-EOTF shaper/3D
LUTs and CTM ([hdr-color-management](../features/hdr-color-management.md)) → the
active `IBackendConnector::Present()` (`src/backend.h:205`) either submits a real DRM
KMS atomic commit (DRM backend) or presents through a Vulkan swapchain / dmabuf
hand-off (nested backends) — see the relevant `backend-*.md` page for which.

Two parallel taps ride the same composited frame rather than triggering a second
composite pass: a **screenshot** request
([control-ipc](../features/control-ipc.md#key-requests)) is served out of this same
pipeline by `CScreenshotManager`, and when PipeWire streaming is active the composite
result is also blitted into a `pPipewireTexture` in the *same command buffer*
(`src/rendervulkan.cpp:4209`-`4255`) for
[screen-capture-pipewire](../features/screen-capture-pipewire.md) to hand off to its
own thread.

```
client commit
      │  wlr_buffer → Vulkan texture (memoized)
      ▼
commit.cpp                              [import/cache]
      │  window marked ready; focus rerolled if dirty
      ▼
steamcompmgr.cpp:paint_all (:2564)      [owns window/focus state, vblank-paced]
      │  build FrameInfo_t (layer stack)
      ▼
rendervulkan.cpp:vulkan_composite (:4030)   [reshade → scale/upscale → color mgmt]
      │  ├─ screenshot tap (control-ipc)
      │  └─ pipewire tap (same cmd buffer)
      ▼
IBackendConnector::Present                  [DRM atomic commit | swapchain | dmabuf]
```

## Recent upstream focus-arbitration work (NOT this fork's divergence)

**Correction (2026-08-21): this repo has not diverged from upstream in the OpenVR
backend or steamcompmgr's focus logic.** This section previously described the commits
below as "this fork's divergence from upstream gamescope". That was wrong: this repo's
base commit `fcc1341` is confirmed (via `git merge-base --is-ancestor`) to be *exactly*
upstream `ValveSoftware/gamescope` HEAD, so every commit at or before it — including
all five listed here — is Valve's own upstream work, not something this fork produced.
This fork's actual own work is additive on top of that base (the overlay/HUD system;
see [overlay-presentation-architecture](../planning/overlay-presentation-architecture.md)),
not the OpenVR/focus commits. Both are still worth knowing about — recent upstream
history happened to be dense with focus-arbitration fixes right at this repo's base —
covered in depth on their own pages rather than repeated here:

- [backend-openvr](../features/backend-openvr.md) — `COpenVRConnector::UpdateVisibility`
  (`src/Backends/OpenVRBackend.cpp:1842`) **takes** keyboard/mouse focus on the
  visible-edge transition (since upstream commit `fcc1341`, this repo's own base
  commit) instead of only reacting to a SteamVR-granted focus event, closing a window
  where a launching app's overlay could leave gamescope's focus bookkeeping frozen on
  the previous app.
- [steamcompmgr-focus](../features/steamcompmgr-focus.md) — five of the last ten
  commits in this codebase's history are upstream focus-arbitration fixes here:
  preserving X11 keyboard focus on a CEF subwindow across a no-op reroll (`0f8dc34`),
  reclaiming focus when it lands on `None` (`396794a`), and two crash/UB fixes in the
  property read/write path (`1efc919`, `1f0321c`) that make an empty/absent
  input-focus window safe to publish and read back.

## Where to look for X

- **"How does gamescope pick its output backend?"** — `auto_select_backend`
  (`src/main.cpp:453`) and the `IBackend::Set<...>()` dispatch at
  `src/main.cpp:980`-`1002`; then the relevant `backend-*.md` page.
- **"Why did focus/keyboard input go to the wrong window?"** —
  [steamcompmgr-focus](../features/steamcompmgr-focus.md), starting with `focus_info`
  and `g_VirtualConnectorFocuses`.
- **"A frame isn't compositing/presenting correctly"** — walk the data-flow sketch
  above; `paint_all()` (`src/steamcompmgr.cpp:2564`) and `vulkan_composite()`
  (`src/rendervulkan.cpp:4030`) are the two functions to break on.
- **"I want to drive gamescope from outside"** —
  [control-ipc](../features/control-ipc.md) (`gamescopectl`, convars, screenshots) and
  [wayland-protocols](../features/wayland-protocols.md) for the full protocol index.
- **"A game's swapchain isn't taking the fast path"** —
  [vk-wsi-layer](../features/vk-wsi-layer.md)'s `canBypassXWayland()` eligibility
  gate.
- **"Where does a new build/feature flag get wired up?"** —
  [build-and-tooling](../features/build-and-tooling.md)'s Meson options table.
- New to the codebase entirely: read this page top to bottom, then
  [steamcompmgr-focus](../features/steamcompmgr-focus.md) (the most-changed
  subsystem in this fork) and [compositing-vulkan](../features/compositing-vulkan.md)
  (the render core almost everything else feeds into).

---
*Last updated: 2026-08-21*
