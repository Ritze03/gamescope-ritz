# Wayland Backend — gamescope as a Wayland client of another compositor

The Wayland backend runs gamescope nested inside a host Wayland compositor, presenting
itself as one (or more) `xdg_toplevel` windows and importing composited client buffers
straight into the host compositor via zero-copy dmabuf, rather than round-tripping
through a Vulkan swapchain like [backend-sdl.md](backend-sdl.md). It's the preferred
"nested mode" backend on a Wayland desktop; SDL is the fallback when this one can't
initialize.

## How it works

- The largest of the four backends after DRM (~3300 lines,
  `src/Backends/WaylandBackend.cpp`). Core classes:
  `src/Backends/WaylandBackend.cpp:643 CWaylandBackend` (a `CBaseBackend`),
  `src/Backends/WaylandBackend.cpp:399 CWaylandConnector` (also implements
  `INestedHints`, same pattern as SDL's `CSDLConnector`), plus a `CWaylandPlane` per
  surface and a dedicated `CWaylandInputThread` for relative-pointer input.
- `CWaylandBackend::Init` (`src/Backends/WaylandBackend.cpp:1967`) connects to the host
  compositor with `wl_display_connect`, walks the registry, and does two
  `wl_display_roundtrip` calls to bind every protocol object it needs. It then hard-fails
  `Init()` if any required global is missing:
  `!m_pCompositor || !m_pSubcompositor || !m_pXdgWmBase || !m_pLinuxDmabuf || !m_pViewporter || !m_pPresentation || !m_pRelativePointerManager || !m_pPointerConstraints || !m_pShm`
  (`src/Backends/WaylandBackend.cpp:2005`). *Why:* this failure path is exactly what lets
  `src/main.cpp:997-1002` fall back to constructing `CSDLBackend` in the same switch
  case — a host claiming `$WAYLAND_DISPLAY` but missing `xdg_wm_base`/linux-dmabuf isn't
  usable, so Wayland-nesting quietly degrades to SDL rather than gamescope refusing to
  start (see [backend-sdl.md](backend-sdl.md#using-it)).
- **No Vulkan swapchain.** `UsesVulkanSwapchain()` is `false`
  (`src/Backends/WaylandBackend.cpp:2296`); instead `ImportDmabufToBackend`
  (`src/Backends/WaylandBackend.cpp:2218`) wraps the client's dmabuf planes with
  `zwp_linux_dmabuf_v1_create_params` / `..._create_immed` and hands the resulting
  `wl_buffer` straight to the host compositor as a `CWaylandFb`. This is the core
  architectural split from SDL, which must round-trip composited frames through a real
  swapchain present because SDL/X11 give no zero-copy buffer-import path. `UsesModifiers()`
  (`src/Backends/WaylandBackend.cpp:2257`) is real here (backed by
  `zwp_linux_dmabuf_v1`'s modifier events, gated by `cv_wayland_use_modifiers`), unlike
  SDL/headless where it's always `false`.
- **Fake EDID.** Since there's no real display to read an EDID off of, `Init` synthesizes
  one with `GenerateSimpleEdid(g_nNestedWidth, g_nNestedHeight)`
  (`src/Backends/WaylandBackend.cpp:1002`, stored in `CWaylandConnector::m_FakeEdid` at
  `:476`, served back through `GetRawEDID()` at `:1204`) and feeds it through the same
  `WritePatchedEdid` used by DRM's real EDIDs
  (`src/Backends/WaylandBackend.cpp:2342`, `HackUpdatePatchedEdid`) — see
  [backend-drm.md](backend-drm.md#how-it-works) for where the real path lives. *Why:*
  downstream consumers like DXVK's HDR probing read the patched-EDID file regardless of
  backend, so a nested backend without hardware still has to produce a plausible one.
- **Virtual connectors.** `UsesVirtualConnectors()` returns `true`
  (`src/Backends/WaylandBackend.cpp:2345`) and `CreateVirtualConnector`
  (`src/Backends/WaylandBackend.cpp:2349`) builds a new `CWaylandConnector` (i.e. a new
  `xdg_toplevel` window) on demand. *Why:* this is what lets gamescope open more than one
  top-level window under a single Wayland-nesting session — DRM and SDL don't need this
  because DRM enumerates real physical connectors and SDL only ever has the one window.
- Relative mouse / pointer lock goes through `zwp_pointer_constraints_v1` +
  `zwp_relative_pointer_manager_v1` on the dedicated `CWaylandInputThread`
  (`src/Backends/WaylandBackend.cpp:2417 SetRelativeMouseMode`,
  `src/Backends/WaylandBackend.cpp:2440` `zwp_pointer_constraints_v1_lock_pointer`) —
  running pointer/relative-motion handling off the main protocol dispatch thread so input
  latency doesn't couple to compositing/frame-callback work.
- Also wires up several optional protocols the DRM/SDL backends have no equivalent for:
  `wp_color_manager_v1` / `frog_color_management_factory_v1` (HDR/colorimetry
  negotiation with the host compositor — see
  [hdr-color-management.md](hdr-color-management.md)), `wp_fractional_scale_v1`,
  `xdg_toplevel_icon_manager_v1`, primary-selection via
  `zwp_primary_selection_source_v1`, and the clipboard via
  `ext_data_control_manager_v1` / `zwlr_data_control_manager_v1` with a
  `wl_data_device` fallback — see
  [clipboard-sync.md](clipboard-sync.md) for the fallback chain, the loop guard
  and why every transfer runs on a worker thread.
- `SupportsExplicitSync()` is unconditionally `true`
  (`src/Backends/WaylandBackend.cpp:2306`) and `SupportsPlaneHardwareCursor()` is `false`
  (`src/Backends/WaylandBackend.cpp:2285`, same reasoning as SDL: cursor goes through
  `INestedHints`, not a real cursor plane) and `SupportsTearing()` is `false`
  (`:2292`) — there's no CRTC to schedule a tearing flip against.
- **Settings-overlay cursor** — `INestedHints::PresentOverlayCursor( bool ) -> bool`, driven
  every frame from `paint_all()`, folded into `UpdateCursor()`. While the overlay owns the
  pointer this shows `m_pDefaultCursorSurface` — the host's *system* cursor, snapshotted from
  X11 at startup by `GetX11HostCursor()` — ahead of every other rule, including the
  `m_bKeyboardEntered` test that normally selects the game's cursor image. It returns whether
  that cursor is really on screen, and the overlay turns ImGui's own software cursor off for
  exactly as long as the answer is true, so **exactly one cursor is visible, never zero**
  (`#69`, revised by D29). Two cases legitimately answer false and keep ImGui's cursor: a
  grabbed pointer, and no snapshot to show (gamescope started with no X11 display).
  *Why "grabbed" means requested-**or**-confirmed* (`m_bRelativeMouseRequested ||
  m_bPointerLocked`) rather than the `zwp_locked_pointer_v1::locked` confirmation alone: the
  confirmation can lag the request indefinitely and, observed live under
  `--force-grab-cursor`, may never arrive at all — keying on it alone produced *zero* cursors.
  The shared rule lives in `src/CursorPolicy.h`.
- **Runtime window-size request** — `INestedHints::RequestOutputSize(w, h)` unsets
  fullscreen, writes `g_nOutputWidth/Height` and marks plane 0 `RequestDecorCommit()` so the
  next `Commit()` sends `libdecor_state_new(w, h)` — the same thing a host-initiated
  `LibDecor_Frame_Configure()` does, with our number first. A tiled host answers with its own
  configure, which overwrites the globals again; the overlay reads those back rather than the
  request. See [resolution-and-refresh.md](resolution-and-refresh.md).

## Using it

- Select explicitly with `--backend wayland`
  (`src/main.cpp:445-446 parse_backend_name`); dispatched at `src/main.cpp:997-1003`,
  where — uniquely among the four backends — failure of `IBackend::Set<CWaylandBackend>()`
  falls straight through to `IBackend::Set<CSDLBackend>()` in the same case, gated
  `#if HAVE_SDL2`.
- `src/main.cpp:453 auto_select_backend` picks Wayland whenever `$WAYLAND_DISPLAY` is set
  (checked first, before `$DISPLAY`) — see [backend-sdl.md](backend-sdl.md#using-it) for
  the X11 fallback branch and [backend-drm.md](backend-drm.md#using-it) for the
  no-desktop-session case.
- Sizing follows the same nested-mode convention as every other nested backend: `-W`/`-H`/`-r`
  set the window size and refresh, defaulting to 720p @ 60 Hz 16:9 if unset
  (`src/Backends/WaylandBackend.cpp:1975-1987`, identical logic to
  [backend-sdl.md](backend-sdl.md#how-it-works) and
  [backend-headless.md](backend-headless.md#how-it-works)).
- Unlike DRM/SDL, there's no dedicated `meson_options.txt` feature flag gating this
  backend — it's built unconditionally (`src/meson.build:99`), since libwayland-client is
  already a hard dependency of gamescope's own Wayland server.

## Related links

- [backend-sdl.md](backend-sdl.md) — the fallback nested backend, both at auto-select
  time and when this backend's `Init()` fails; contrast the swapchain-vs-dmabuf-import
  presentation model.
- [backend-drm.md](backend-drm.md) — the embedded backend; source of the real EDID this
  backend's fake one mimics, and the `WritePatchedEdid`/`edid.h` infrastructure both share.
- [hdr-color-management.md](hdr-color-management.md) — the `wp_color_manager_v1` /
  `frog_color_management_factory_v1` negotiation this backend performs against the host
  compositor.
- [wayland-protocols.md](wayland-protocols.md) — the broader catalogue of Wayland
  protocol extensions gamescope speaks, both as a server (wlserver) and, here, as a
  client.
- [../planning/wayland-vrr-buffer-lifetime.md](../planning/wayland-vrr-buffer-lifetime.md)
  — host buffer lifetime on this backend: why `CWaylandFb` acquires a reference only on a
  real attach transition and never re-attaches a buffer the surface already holds, and the
  `--force-grab-cursor` reference leak that rule was written to stop. Read it before
  touching `CWaylandPlane::Present()`.
- [../meta/TERMINOLOGY.md](../meta/TERMINOLOGY.md) — "nested mode" vs "embedded mode".
