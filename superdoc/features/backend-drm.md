# DRM Backend — embedded mode, gamescope owns the display

The DRM/KMS atomic backend is "embedded mode": gamescope becomes the display server
itself, driving a real GPU connector/CRTC/plane pipeline directly via the kernel's
atomic modesetting API, with no host compositor or window manager involved. This is how
gamescope runs on the Steam Deck and other console-like/kiosk setups. It's by far the
largest and most hardware-facing of the four backends (~4200 lines).

## How it works

- Lives in `src/Backends/DRMBackend.cpp`, built only when `meson_options.txt:3
  drm_backend` and its `libdrm` dependency are available
  (`src/meson.build:131-143`, gated `#if HAVE_DRM` in `src/main.cpp`). A global
  `struct drm_t g_DRM` (`src/Backends/DRMBackend.cpp:102`, instance at `:170`) holds the
  DRM fd, the current connector/CRTC/plane set, and the `libliftoff` device/output/layer
  handles — most of the free functions in this file operate on it directly rather than
  through the class, reflecting that this backend predates (and still partly bypasses)
  the `IBackend` abstraction added later.
- Core classes, each an atomic-property-typed KMS object
  (`src/Backends/DRMBackend.cpp:182 CDRMBackend`,
  `src/Backends/DRMBackend.cpp:286 CDRMPlane`,
  `src/Backends/DRMBackend.cpp:338 CDRMCRTC`,
  `src/Backends/DRMBackend.cpp:370 CDRMConnector` — the latter is the one that
  implements `IBackendConnector`/`CBaseBackendConnector`), plus
  `src/Backends/DRMBackend.cpp:540 CDRMFb` for framebuffer objects, and the actual
  `IBackend` implementation at `src/Backends/DRMBackend.cpp:3561 CDRMBackend` (the
  namespaced class, distinct from the forward-declared `class CDRMBackend;` at line 182
  — same name, defined once, forward-declared for use by the free functions above it).
- **Connector selection.** `setup_best_connector`
  (`src/Backends/DRMBackend.cpp:1066`) walks every connected connector and picks the
  lowest-priority one via `get_connector_priority`
  (`src/Backends/DRMBackend.cpp:1022`), which consults a name→priority map built by
  `parse_connector_priorities` (`src/Backends/DRMBackend.cpp:1004`) from the
  `-O, --prefer-output` CLI flag (`src/main.cpp:92`, `:232`; e.g.
  `DP-1,DP-2,HDMI-A-1`). `g_bForceInternal` skips any `GAMESCOPE_SCREEN_TYPE_EXTERNAL`
  connector outright (`src/Backends/DRMBackend.cpp:1082`). *Why:* a plain "first
  connected connector wins" policy breaks docking-station setups where an internal panel
  and an external monitor can both be connected simultaneously — internal/external is a
  first-class distinction here (`CDRMConnector::GetScreenType`,
  `src/Backends/DRMBackend.cpp:426-433`) that the other three backends don't need, since
  they never have more than one output candidate.
- **EDID.** `CDRMConnector::ParseEDID` (`src/Backends/DRMBackend.cpp:2293`) reads the raw
  EDID property blob and parses it with `libdisplay-info`
  (`di_info_parse_edid`, `src/Backends/DRMBackend.cpp:2310`) to pull HDR metadata,
  panel orientation, and the product name. `HandleEdidChange()`
  (`src/Backends/DRMBackend.cpp:405`) is polled so a live EDID change (e.g. a dock
  switching monitors) forces a mode re-negotiation (`src/Backends/DRMBackend.cpp:1097`).
  The raw EDID is then re-exported through `WritePatchedEdid`
  (`src/edid.h`, called at `src/Backends/DRMBackend.cpp:1181`, `:3594`, `:4054`) — this is
  the *real* EDID that the Wayland and OpenVR backends' synthetic EDIDs
  (`GenerateSimpleEdid`) exist to imitate; see
  [backend-wayland.md](backend-wayland.md#how-it-works). Modes that don't come straight
  off the EDID (custom `-W`/`-H`/`-r`) are synthesized with `generate_cvt_mode` /
  `generate_fixed_mode` from `modegen.hpp`/`modegen.cpp`
  (`src/Backends/DRMBackend.cpp:3457`, `:3462`).
- **Plane assignment via libliftoff.** Rather than gamescope hand-picking which
  composited layer goes on which hardware overlay plane, `drm_prepare_liftoff`
  (`src/Backends/DRMBackend.cpp:2619`) hands up to `k_nMaxLayers` (6, defined
  `src/rendervulkan.hpp:40`) layers to `liftoff_layer_set_property` calls per-layer
  (FB_ID, zpos, alpha, blend mode, src/crtc rects, rotation — from
  `src/Backends/DRMBackend.cpp:2655` onward), then `liftoff_output_apply`
  (`src/Backends/DRMBackend.cpp:2814`) figures out at runtime which layers the specific
  GPU's overlay planes can actually satisfy, falling back internally to fewer/composited
  planes when it can't. A second call at `src/Backends/DRMBackend.cpp:2832` retries once
  more specifically for an NVIDIA-555-series `IN_FENCE_FD`/`EPERM` workaround
  (`src/Backends/DRMBackend.cpp:2815-2831`). *Why:* the number and capability of hardware
  planes varies per GPU/driver, so plane assignment is solved by a library that probes
  hardware limits at commit time instead of gamescope hard-coding driver-specific plane
  counts.
- **Color management is largely AMD-specific here**: `AMD_PLANE_DEGAMMA_TF`,
  `AMD_PLANE_SHAPER_LUT`, `AMD_PLANE_LUT3D`, `AMD_PLANE_CTM`, `AMD_PLANE_BLEND_TF`, and
  `AMD_PLANE_HDR_MULT` atomic properties are set per-layer when present
  (`src/Backends/DRMBackend.cpp:2741-2786`, property declared at `:323`). See
  [hdr-color-management.md](hdr-color-management.md) for the cross-backend HDR pipeline
  this feeds into.
- **VRR.** `VRR_ENABLED` is a CRTC atomic property (`src/Backends/DRMBackend.cpp:357`,
  set at `:3131`); `CDRMConnector::IsVRRActive`
  (`src/Backends/DRMBackend.cpp:462`) reads it back off `g_DRM.pCRTC`. This backend is
  the only one of the four where `SupportsVRR()`/`IsVRRActive()` can be genuinely `true`
  — the nested backends (SDL, Wayland, headless) all hard-return `false` since they don't
  own a CRTC to negotiate VRR against.
- **Tearing / async flips.** `g_bSupportsAsyncFlips`
  (`src/Backends/DRMBackend.cpp:584`) is probed once via
  `drmGetCap(DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP)` (`src/Backends/DRMBackend.cpp:1322`), and
  `CDRMBackend::SupportsTearing()` (`src/Backends/DRMBackend.cpp:3992`) just returns that
  flag — contrast SDL/Wayland's unconditional `false` (no CRTC to tear against).
- `CDRMBackend::Init()` (`src/Backends/DRMBackend.cpp:3574`) does `vulkan_init` +
  `wlsession_init()` like the other backends, then calls `init_drm(...)`
  (`src/Backends/DRMBackend.cpp:1248`), which opens the DRM device, builds the
  connector/CRTC/plane objects from `drmModeGetResources`
  (`get_resources`, `src/Backends/DRMBackend.cpp:924`), and creates the `libliftoff`
  device (`src/Backends/DRMBackend.cpp:1338`).
  `UsesVulkanSwapchain()` is `false` and `IsSessionBased()` is `true`
  (`src/Backends/DRMBackend.cpp:3997`, `:4002`) — DRM needs a logind/seat session to hold
  DRM master, which nested backends never require.
  `SupportsPlaneHardwareCursor()` is `true` (`src/Backends/DRMBackend.cpp:3987`) — unlike
  every nested backend, there's a real hardware cursor plane to use.

## Using it

- Select explicitly with `--backend drm`, or let
  `src/main.cpp:453 auto_select_backend` choose it as the last-resort default when
  neither `$WAYLAND_DISPLAY` nor `$DISPLAY` is set — i.e. running from a bare TTY/session
  with no host compositor, which is the embedded-mode case this backend exists for.
- `-O, --prefer-output DP-1,DP-2,...` (`src/main.cpp:92`) — ordered connector name
  preference, consumed by `parse_connector_priorities`
  (`src/Backends/DRMBackend.cpp:1004`).
- `-W`/`-H`/`-r` still apply (as a *requested* mode fed into `generate_cvt_mode` when the
  EDID doesn't already offer a matching mode), but unlike the nested backends there's no
  hardcoded 720p default fallback — the connector's preferred EDID mode is used instead
  when nothing is requested.
- Requires a working DRM device node the process can open/become master of — normally via
  logind/seatd, handled through `wlsession_init()` (shared with every backend, but only
  load-bearing here since `IsSessionBased()` is `true`).

## Related links

- [backend-sdl.md](backend-sdl.md), [backend-wayland.md](backend-wayland.md) — the
  nested-mode counterparts; contrast hardware cursor planes, VRR, tearing, and
  session/DRM-master handling, all of which are DRM-only.
- [backend-headless.md](backend-headless.md) — the no-display counterpart; contrast the
  real KMS EDID/mode negotiation here against headless's fixed virtual size.
- [backend-openvr.md](backend-openvr.md) — also has genuine per-frame display timing
  concerns, but drives a VR runtime instead of a KMS CRTC.
- [hdr-color-management.md](hdr-color-management.md) — the AMD-specific plane
  degamma/shaper/LUT3D/CTM properties this backend sets are the embedded-mode half of
  gamescope's HDR pipeline.
- [scaling-filters.md](scaling-filters.md) — mode generation
  (`generate_cvt_mode`/`generate_fixed_mode`) interacts with the requested output
  size/scaling here.
- [../meta/TERMINOLOGY.md](../meta/TERMINOLOGY.md) — "embedded mode" vs "nested mode".
