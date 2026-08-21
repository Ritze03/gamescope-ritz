# Headless Backend — no display, streaming/CI-only output

The headless backend runs gamescope with no window, no compositor connection, and no
physical display at all — it exists purely to give the compositor a valid `IBackend` so
the rest of the pipeline (Vulkan compositing, `steamcompmgr`, capture) runs the same as
any other mode, with output only reachable via the [screen-capture pipeline](screen-capture-pipewire.md)
or [control IPC](control-ipc.md). It is the smallest of the four backends by a wide
margin.

## How it works

- Implemented entirely in `src/Backends/HeadlessBackend.cpp:97 CHeadlessBackend` (a
  `CBaseBackend` subclass) plus a matching stub connector,
  `src/Backends/HeadlessBackend.cpp:11 CHeadlessConnector`. Both classes are compact
  enough that nearly every virtual from `src/backend.h:312 IBackend` and
  `src/backend.h:176 IBackendConnector` is a one-line stub.
- `CHeadlessBackend::Init` (`src/Backends/HeadlessBackend.cpp:108`) only sets the output
  size/refresh from the same `-W`/`-H`/`-r` globals every nested backend uses (defaulting
  to a 720p, 16:9, 60 Hz virtual display — `src/Backends/HeadlessBackend.cpp:114`), calls
  `vulkan_init`, and calls `wlsession_init()`. *Why:* even headless mode needs a working
  Wayland server (`wlserver`) because clients still connect to it and get composited —
  "headless" only means "no output sink", not "no compositor."
- `CHeadlessBackend::ImportDmabufToBackend` (`src/Backends/HeadlessBackend.cpp:183`) just
  wraps the client's dmabuf in a plain `CBaseBackendFb` — there is no real scanout object
  to import into, unlike DRM's KMS framebuffer or Wayland's `zwp_linux_dmabuf_v1` import.
- `CHeadlessConnector::Present` (`src/Backends/HeadlessBackend.cpp:88`) is a no-op that
  returns `0` immediately — no compositing wait, no vblank, no presentation feedback.
  *Why:* there is nothing to present to; the composited frame is only observed by
  whatever reads it back out-of-band (capture/IPC), not by this backend.
- `UsesVulkanSwapchain()` returns `false` and `IsSessionBased()` returns `false`
  (`src/Backends/HeadlessBackend.cpp:219`, `:224`) — it needs neither a windowing
  swapchain nor a logind/DRM session, which is why it's the mode CI and server-side
  streaming setups use when there's no display hardware or desktop session available at
  all.
- `GetSupportedModifiers`/`UsesModifiers` both report no modifier support
  (`src/Backends/HeadlessBackend.cpp:188`, `:192`) — buffers are always treated as
  linear/implicit since there's no real display controller with format/modifier
  constraints to negotiate against.

## Using it

Select it explicitly with `--backend headless` (parsed in
`src/main.cpp:427 parse_backend_name` and dispatched at `src/main.cpp:993`). Unlike DRM,
SDL, and Wayland, headless is never chosen by `src/main.cpp:453 auto_select_backend` — it
must be requested on the command line, since auto-detection only distinguishes
"nested under Wayland" / "nested under X11" / "own the DRM device" (see
[backend-drm.md](backend-drm.md#using-it)). There is no build-time flag gating it
(unlike `drm_backend`/`sdl2_backend` in `meson_options.txt:3-4`) — it has no external
library dependency, so it's always compiled in.

## Related links

- [backend-drm.md](backend-drm.md), [backend-sdl.md](backend-sdl.md),
  [backend-wayland.md](backend-wayland.md) — the other three `IBackend` implementations;
  contrast with each for what a "real" output sink adds.
- [backend-openvr.md](backend-openvr.md) — the other backend with no traditional 2D
  display, but for VR compositing rather than absence of output.
- [screen-capture-pipewire.md](screen-capture-pipewire.md), [control-ipc.md](control-ipc.md)
  — the actual consumers of frames composited under this backend.
- [../meta/TERMINOLOGY.md](../meta/TERMINOLOGY.md) — "embedded mode" / "nested mode"
  definitions (headless is neither, strictly — it's display-less).
