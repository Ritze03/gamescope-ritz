# Wayland Backend — gamescope as a Wayland client of another compositor

The Wayland backend runs gamescope nested inside a host Wayland compositor, presenting
itself as one (or more) `xdg_toplevel` windows and importing composited client buffers
straight into the host compositor via zero-copy dmabuf, rather than round-tripping
through a Vulkan swapchain like [backend-sdl.md](backend-sdl.md). It's the preferred
"nested mode" backend on a Wayland desktop; SDL is the fallback when this one can't
initialize.

## How it works

- The largest of the five backends after DRM (~4200 lines,
  `src/Backends/WaylandBackend.cpp`). Core classes:
  `src/Backends/WaylandBackend.cpp:CWaylandBackend` (a `CBaseBackend`),
  `src/Backends/WaylandBackend.cpp:CWaylandConnector` (also implements
  `INestedHints`, same pattern as SDL's `CSDLConnector`), plus a `CWaylandPlane` per
  surface and a dedicated `CWaylandInputThread` for relative-pointer input.
- `CWaylandBackend::Init` (`src/Backends/WaylandBackend.cpp:CWaylandBackend::Init`) connects to the host
  compositor with `wl_display_connect`, walks the registry, and does two
  `wl_display_roundtrip` calls to bind every protocol object it needs. It then hard-fails
  `Init()` if any required global is missing:
  `!m_pCompositor || !m_pSubcompositor || !m_pXdgWmBase || !m_pLinuxDmabuf || !m_pViewporter || !m_pPresentation || !m_pRelativePointerManager || !m_pPointerConstraints || !m_pShm`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::Init`). *Why:* this failure path is exactly what lets
  `src/main.cpp:main` fall back to constructing `CSDLBackend` in the same switch
  case — a host claiming `$WAYLAND_DISPLAY` but missing `xdg_wm_base`/linux-dmabuf isn't
  usable, so Wayland-nesting quietly degrades to SDL rather than gamescope refusing to
  start (see [backend-sdl.md](backend-sdl.md#using-it)).
- **No Vulkan swapchain.** `UsesVulkanSwapchain()` is `false`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::UsesVulkanSwapchain`); instead `ImportDmabufToBackend`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::ImportDmabufToBackend`) wraps the client's dmabuf planes with
  `zwp_linux_dmabuf_v1_create_params` / `..._create_immed` and hands the resulting
  `wl_buffer` straight to the host compositor as a `CWaylandFb`. This is the core
  architectural split from SDL, which must round-trip composited frames through a real
  swapchain present because SDL/X11 give no zero-copy buffer-import path. `UsesModifiers()`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::UsesModifiers`) is real here (backed by
  `zwp_linux_dmabuf_v1`'s modifier events, gated by `cv_wayland_use_modifiers`), unlike
  SDL/headless where it's always `false`.
- **Fake EDID.** Since there's no real display to read an EDID off of, `UpdateEdid` synthesizes
  one with `GenerateSimpleEdid(g_nNestedWidth, g_nNestedHeight)`
  (`src/Backends/WaylandBackend.cpp:CWaylandConnector::UpdateEdid`, stored in `CWaylandConnector::m_FakeEdid`,
  served back through `src/Backends/WaylandBackend.cpp:CWaylandConnector::GetRawEDID`) and feeds it through the same
  `WritePatchedEdid` used by DRM's real EDIDs
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::HackUpdatePatchedEdid`) — see
  [backend-drm.md](backend-drm.md#how-it-works) for where the real path lives. *Why:*
  downstream consumers like DXVK's HDR probing read the patched-EDID file regardless of
  backend, so a nested backend without hardware still has to produce a plausible one.
- **Virtual connectors.** `UsesVirtualConnectors()` returns `true`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::UsesVirtualConnectors`) and `CreateVirtualConnector`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::CreateVirtualConnector`) builds a new `CWaylandConnector` (i.e. a new
  `xdg_toplevel` window) on demand. *Why:* this is what lets gamescope open more than one
  top-level window under a single Wayland-nesting session — DRM and SDL don't need this
  because DRM enumerates real physical connectors and SDL only ever has the one window.
- Relative mouse / pointer lock goes through `zwp_pointer_constraints_v1` +
  `zwp_relative_pointer_manager_v1` on the dedicated `CWaylandInputThread`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::SetRelativeMouseMode`, which calls
  `zwp_pointer_constraints_v1_lock_pointer`) —
  running pointer/relative-motion handling off the main protocol dispatch thread so input
  latency doesn't couple to compositing/frame-callback work.
- **Host-shortcut inhibition** (Input › General's "Force grab keyboard", added
  2026-09-27) goes through `zwp_keyboard_shortcuts_inhibit_manager_v1`, bound as an
  **optional** global in `CWaylandBackend::Wayland_Registry_Global()` — unlike the
  required-globals check `Init()` hard-fails on (see below), a host that never
  advertises this one just leaves the switch unable to do anything live rather than
  refusing to start. `CWaylandBackend::SetKeyboardGrabbed( wl_surface *, bool )` creates
  or destroys a `zwp_keyboard_shortcuts_inhibitor_v1` for the connector's own toplevel
  surface against `m_pSeat`, on whatever thread calls it — the same "steamcompmgr
  thread" `SetTitle()`/`RequestOutputSize()` already call straight into libdecor from,
  not `CWaylandInputThread`'s separate dispatch thread, which owns none of this state.
  `CWaylandConnector::Init()` seeds `g_bGrabbed` from config and grabs at startup the
  same "config seeds, CLI wins" way `-g`/`--grab` already worked on the SDL backend. The
  inhibitor's `active`/`inactive` events are logged and drive
  `CWaylandBackend::GetKeyboardGrabStatus()`, read by the Input › General row's own
  Diagnostics fact. See [input-general.md](input-general.md)'s "Force grab keyboard"
  section for the full lifecycle, the live evidence, and the Hyprland escape (a bind
  flagged `p` still fires under this inhibitor).
- Also wires up several optional protocols the DRM/SDL backends have no equivalent for:
  `wp_color_manager_v1` / `frog_color_management_factory_v1` (HDR/colorimetry
  negotiation with the host compositor — see
  [hdr-color-management.md](hdr-color-management.md)), `wp_fractional_scale_v1`,
  `xdg_toplevel_icon_manager_v1`, primary-selection via
  `zwp_primary_selection_source_v1`, and the clipboard via
  `ext_data_control_manager_v1` / `zwlr_data_control_manager_v1` with a
  `wl_data_device` fallback — see
  [clipboard-sync.md](clipboard-sync.md) for the fallback chain, the loop guard
  and why every transfer runs on a worker thread — and
  `zwp_keyboard_shortcuts_inhibit_manager_v1` (host-shortcut inhibition for Input ›
  General's "Force grab keyboard", see the bullet below).
- `SupportsExplicitSync()` is unconditionally `true`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::SupportsExplicitSync`) and `SupportsPlaneHardwareCursor()` is `false`
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::SupportsPlaneHardwareCursor`, same reasoning as SDL: cursor goes through
  `INestedHints`, not a real cursor plane). `SupportsTearing()` is true only when the host
  offers `wp_tearing_control_manager_v1` — see [Tearing](#tearing) below.
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

## Tearing

Since 2026-10-04 the **Allow tearing** setting (`gamescope.tearing_enabled`) works when
gamescope runs as a window. The backend binds the standard `wp_tearing_control_manager_v1`
(wayland-protocols `staging/tearing-control/tearing-control-v1.xml`, taken from
`wl_protocol_dir` in `protocol/meson.build` like `fractional-scale-v1`; no vendored copy)
if the host offers it.

- **Where the hint is set.** `CWaylandPlane::Init()` creates one `wp_tearing_control_v1`
  for the **toplevel plane only** (plane 0, the libdecor surface; the sync subsurfaces
  latch with it, and a host decides tearing from the window's root surface).
  `CWaylandConnector::Present( pFrameInfo, bAsync )` calls
  `m_Planes[0].SetPresentationHint( bAsync )` just before the commit loop: `async` for an
  async/tearing present, `vsync` otherwise, sent **only when it changes** (the protocol's
  own default is vsync). The hint is double-buffered, so plane 0's commit — the last of the
  loop — applies it.
- **`SupportsTearing()`** returns `m_pTearingControlManager != nullptr`, i.e. "the host
  offers the protocol". The main loop's `bTearing` (`cv_tearing_enabled &&
  SupportsTearing() && the base commit wants async`) therefore becomes true nested exactly as
  on DRM, which also lets frame generation's timer-paced paints present async
  (`bFGTearPresent`, see [frame-generation.md](frame-generation.md)) and sets
  `STEAM_GAMESCOPE_TEARING_SUPPORTED` for Steam. Nothing in the Shell gates its tearing row
  on `SupportsTearing()` (only the startup log line `Supports Tearing:` does), so the row is
  unchanged. It is a **request**: the host decides whether the window really tears.
- **Frame generation's output timer** also requests `async` when the game itself is vsynced, as a
  latched state (`fgtear::Hint`, `src/FrameGen/TearHint.h`; 2026-10-05, made a state 2026-10-06):
  async while the timer paces the output and for 250 ms after it stops, vsync immediately over an
  overlay or a fade (async again after 250 ms without one). `Why:` a per-paint value flipped 22,995
  times in 44 minutes (about 23 per second) in a Forza Horizon 6 session. Each hint change is logged
  (`wayland: tearing hint -> async` / `vsync`), so the log is one line per real transition. See
  [frame-generation.md](frame-generation.md).
- **Tearing off, or a host without the protocol:** `async` is only ever requested when
  `bTearing` (or the output-timer state above, which needs the setting too) holds, so with the setting off the hint stays at its initial `vsync` and no
  request is sent; without the protocol no object is created. Wire behaviour is as before.
- **Hyprland** (the user's host) needs all of: `general { allow_tearing = true }`; the
  gamescope window **fullscreen** (Hyprland only tears fullscreen windows); and, to force it
  rather than rely on the client's hint, a window rule `immediate` for the gamescope window
  (its class/app-id is `gamescope`, set in `CWaylandPlane::Init()`), e.g.
  `windowrule = immediate, class:^(gamescope)$` (older config syntax) / the equivalent
  `immediate` rule in newer Hyprland versions. Without them Hyprland accepts the hint and
  keeps vsyncing. This repo does not touch the user's Hyprland config; these are for the
  user to set.
- `Why:` before this, every nested frame — real and frame-generated alike — waited for the
  host's next refresh to be shown, up to one refresh of extra delay, with no way for the
  user's tearing setting to change that. The protocol is the host-side contract for exactly
  this ("show this buffer as soon as possible, tearing allowed").

## Using it

- Select explicitly with `--backend wayland`
  (`src/main.cpp:parse_backend_name`); dispatched in the backend `switch` in `src/main.cpp:main`,
  where — uniquely among the five backends — failure of `IBackend::Set<CWaylandBackend>()`
  falls straight through to `IBackend::Set<CSDLBackend>()` in the same case, gated
  `#if HAVE_SDL2`.
- `src/main.cpp:auto_select_backend` picks Wayland whenever `$WAYLAND_DISPLAY` is set
  (checked first, before `$DISPLAY`) — see [backend-sdl.md](backend-sdl.md#using-it) for
  the X11 fallback branch and [backend-drm.md](backend-drm.md#using-it) for the
  no-desktop-session case.
- Sizing follows the same nested-mode convention as every other nested backend: `-W`/`-H`/`-r`
  set the window size and refresh, defaulting to 720p @ 60 Hz 16:9 if unset
  (`src/Backends/WaylandBackend.cpp:CWaylandBackend::Init`, identical logic to
  [backend-sdl.md](backend-sdl.md#how-it-works) and
  [backend-headless.md](backend-headless.md#how-it-works)).
- Unlike DRM/SDL, there's no dedicated `meson_options.txt` feature flag gating this
  backend — it's built unconditionally (`src/meson.build`, the unconditional `src` list), since libwayland-client is
  already a hard dependency of gamescope's own Wayland server.

## Frame generation

While [frame generation](frame-generation.md) is enabled, full composite is forced (`bNeedsFullComposite |= fghost::Enabled()`).

## Output images the host still holds

A full composite draws into one of four output images (`g_output.outputImages`; three on every
other backend, `vulkan_output_image_count()`) and attaches it to the toplevel; the host keeps that image from the attach until it sends
`wl_buffer.release`, which the backend Fb's acquisition count (and so
`CVulkanTexture::IsInUse()`) reflects. Before each composite,
`CWaylandConnector::Present()` calls `vulkan_select_free_output_image()`
(`src/rendervulkan.cpp`), which points `g_output.nOutImage` at an image the host has
released. If all of them are still held, the frame is skipped (`force_repaint()`, return
`-EBUSY`) and the next vblank tries again. `Why:` upstream simply advances the ring
(`(n + 1) % 3`), assuming the host releases an image within two frames. GNOME/Mutter
scanning a fullscreen gamescope window out directly does not. Measured 2026-10-26 on
a high-end AMD GPU at 165 Hz with CS2:

- 6.9% of composites targeted an image Mutter still held.
- 2.2% found all three held.

Drawing into a buffer the host is still reading can show on screen as corruption that
a screen recording does not catch, since the recording copies the finished frame. This
was found while chasing coloured specks over moving content in CS2; whether this change
removes those specks is **not yet confirmed** (the user's first retest ran the old
binary). Upstream rarely composites in nested mode, so the race is latent there. This
fork composites every frame whenever its HUD, crosshair or overlay is up, since those
layers have no backend Fb (see the check above `if ( !bNeedsFullComposite )`). A
headless host (private headless mutter, sway) releases promptly and never shows the
race: 0 of 1662 composites were held there. Measure on a real output.

## Direct scanout under a host

**Crash fixed (2026-11-07); picture artifacts still open.** Gamescope nested on Hyprland with
scanout on and a game running crashed the GPU about once per 70 fullscreen entries (gfxhub write
page fault from gamescope, `PERMISSION_FAULTS=0x1`, then a cs2 gfx ring timeout and a full GPU
reset that killed the desktop). The fix: `CVulkanTexture::BInit()` no longer chains
`VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA` onto an output image's memory when
`HostMayScanOutOutputImages()` is true (this backend only). The image-side `wsi_image_create_info`
(scanout) stays; DRM and the other backends are untouched. `Why:` RADV keeps WSI-flagged memory
out of its global buffer-object list, so an output image is only listed in submissions that use
it. When the host's scanout pins and moves the exported image, a gamescope submission that does
not list it can make the kernel clear its mapping (`amdgpu_vm_handle_moved()`, reservation held
by the display side), and a queued write then faults.

Evidence (Hyprland nested, scanout on, 300 scripted fullscreen entries, CS2): without the fix a
GPU reset at entry 78 and again at 85, all 10 logged faults of the second one inside a single
output image, 0.2 s after a rebuild of the output images. The descriptor ring was ruled out
(in-flight sets peaked at 6-8 of 24). `RADV_DEBUG=allbos` on gamescope alone: 300 clean. With the
fix and no `allbos`: 300 clean with Frame generation off and 300 clean with it on at a 140 fps
limit, scanout engaged on 298 of 300 each, 0 faults.

**Still open:** the picture artifacts when entering and leaving fullscreen with scanout on, which
sometimes show older frames. They persist with the fix and were reportedly absent under `allbos`;
cause unknown ([TODO](../planning/TODO.md)). The Compatibility area's warning about scanout being off
is back since the crash fix ([diagnostics.md](diagnostics.md)).

### Block direct scanout from gamescope

User switch "Block direct scanout" ([diagnostics.md](diagnostics.md#block-direct-scanout-switch-displayblock_direct_scanout), default
off). Hosts only scan out a window that is a single surface (Hyprland reports `solitaryBlockedBy`
`OPAQUE, SURFACES` when it is not; Mutter's scanout candidate check needing a single surface too is
from memory and **unverified until tested**). `m_Planes` are subsurfaces of plane 0, so when the
switch is on `CWaylandConnector::Present()` puts a 1x1 fully transparent buffer
(`wp_single_pixel_buffer_manager_v1`, rgba 0, `bOpaque = false`, viewport destination 1x1 at the
origin; `CWaylandBackend::GetTransparentFb()`) on the top plane, `m_Planes[7]`, in both the
direct-plane and the full-composite path, before the commit loop. Plane 7 is normally empty; in
the direct-plane path, if a real layer wants it (8 layers, or 7 plus the backing plane) it is left
alone, because the host is not solitary then anyway. Hidden windows are skipped. Without the
single-pixel manager the switch does nothing and logs once.

The plane subsurface already has an empty input region (`CWaylandPlane::Init()`), so it takes no
input and leaves pointer focus on the toplevel. Buffer lifetime, per
[wayland-vrr-buffer-lifetime.md](../planning/wayland-vrr-buffer-lifetime.md): `CWaylandPlane::Present()`
only attaches when the plane's buffer changes and acquires on that attach, so presenting the same
transparent buffer every frame is one attach, one acquire and one release in total; the blocker
makes exactly one `Present()` call per frame (never `Present(nullptr)` then a state), and turning
the switch off presents `nullptr` on plane 7, which detaches it on the next frame. Cost: the host
composites, so Frame generation's shown rate falls back (about 160 uncapped against 261-272).

A host can put gamescope's buffer straight on a display plane instead of drawing it. Under
Hyprland that is the only way past 160-230 shown frames with a game running (why:
[frame-generation.md](frame-generation-host-scanout.md#why-the-host-loses-frames)). Two things have to hold,
both behind `HostMayScanOutOutputImages()` (`src/backend.h`, true only here), and the status
line's `zero-copy` count says whether it worked.

**With "Compress output images" Off, output images carry no AMD DCC modifier** (modifier filter in
`CVulkanTexture::BInit()`; the switch is On by default, which skips the filter, see the last
paragraph of this section). The
host's `zwp_linux_dmabuf_v1` list is what it can sample, not what its planes take. From that
list RADV picked `0x200000028a6bf04` (RDNA 3: DCC, pipe-aligned, no retile), which is missing
from the plane's `IN_FORMATS`, so `drmModeAddFB2WithModifiers` failed. Hyprland logged it three
times, then remembered the dead buffer and only failed silently: `directScanoutBlockedBy` empty,
`directScanoutTo` 0. Worse, each frame's failed attempt switched the output to the buffer's
format (`XB30`, the screen runs `XR24`) and the rollback modeset it back. Measured: 511 modesets
in 6 s with the game, 2342 in about 8 s with vkcube and FSR, and gamescope's images held so long that
shown fell to 73-147 (limit 120 and uncapped). **That is what "Breaks FSR" meant**, and it was never about FSR's pixels:
FSR forces a full composite, so gamescope's own output image reaches the host. With frame
generation also forcing it, the same happens without FSR. Without any composite the window is
not solitary (`solitaryBlockedBy` `OPAQUE, SURFACES`) and nothing is attempted.

`Why uncompressed and not a displayable modifier:` `0x200000028a67f04` (DCC with retile, the one
Hyprland allocates for itself) also engaged scanout, but a plane then reads a second DCC surface
that the driver has to retile, and nothing here can see what a plane actually reads (a
screenshot makes the host composite again). With no compression a plane reads the same memory a
screenshot does. Cost, without scanout: about 3% shown (232 -> 224, 206 -> 199, 162 -> 158),
across launches, so roughly. Games keep DCC: `GetSupportedModifiers()` stays whole, because it
also decides what gamescope offers its own clients.

The "Compress output images" setting (Display > General, **On by default**, next launch) is why the
filter above only applies with it Off. On, the filter is skipped and the driver may pick DCC, as
upstream gamescope does; on RDNA 3 with Hyprland's direct scanout that brings back the modeset loop
measured above, so Off is the remedy there. Whether DCC cures the RDNA 4 scrambling under GNOME is a
hypothesis, untested. The chosen modifier is logged at creation. See
[diagnostics.md](diagnostics.md#compress-output-images-switch-displaycompress_output_images).

**Four output images.** One is on screen, one committed behind it and held until the next flip,
one being drawn. With three, 34-46 paints a second found none free and shown stopped at 240-273;
with four, 7-14 and 261-278. Without scanout four also mostly clears the 4-53 per second the
uncompressed images otherwise cost.

The picture was checked with `grim`, which makes the host composite for that frame, so it shows
gamescope's buffer and not the plane's read. 720p game, 1080p output, scanout on, frame generation
on: FSR sharpened (edge variance 190 against linear 126), HUD and Crosshair in place, settings
overlay drawn, no modesets after the first, no failed import, shown 277-278 with FSR (205 with
the overlay open, which is the overlay's own cost: gamescope itself sent 216). Flicker, tearing
and black frames can only be ruled out by eye; `activelyTearing` stayed false and every screenshot
was intact. Entering and leaving scanout costs one modeset each, since the 10-bit output image is
not the screen's 8-bit format (`bitdepth = 8`). An 8-bit output image would avoid that but loses
the composite's precision, and it was not tested with the game.


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
