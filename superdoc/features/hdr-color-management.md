# HDR & Color Management — mapping app colorspaces to the display

Every window layer can carry a different colorspace and dynamic range (SDR gamma-2.2,
scRGB, HDR10/PQ), and the output display has its own native colorimetry and HDR
capability. This page covers how gamescope reconciles the two: per-frame LUTs, CTMs,
and tonemapping parameters that ride along in the same `FrameInfo_t` the
[compositing-vulkan.md](compositing-vulkan.md) pipeline consumes.

## How it works

- Color state lives in `gamescope_color_mgmt_t` (`src/rendervulkan.hpp:484`):
  gamut-widening (`sdrGamutWideness`), display/internal brightness
  (`flInternalDisplayBrightness`, `flSDROnHDRBrightness` — default `203.f`,
  `src/rendervulkan.hpp:491`), per-source input gain (`flHDRInputGain`,
  `flSDRInputGain`), a tonemap operator + source/display HDR metadata
  (`hdrTonemapOperator`, `hdrTonemapSourceMetadata`, `hdrTonemapDisplayMetadata`), the
  display's native colorimetry/EOTF and the chosen *output encoding* colorimetry/EOTF
  (`displayColorimetry`/`displayEOTF` vs. `outputEncodingColorimetry`/
  `outputEncodingEOTF`, `src/rendervulkan.hpp:504`-`510`), and the focused app's raw
  HDR metadata blob (`appHDRMetadata`, `src/rendervulkan.hpp:514`). *Why:*
  `flSDROnHDRBrightness` defaults to 203 nits because that's the ITU-R BT.2408
  reference white level for SDR content graded for display alongside HDR — it's not an
  arbitrary gamescope constant.
- It's double-buffered like the rest of gamescope's pending/current state:
  `gamescope_color_mgmt_tracker_t` (`src/rendervulkan.hpp:554`) holds a `pending` and
  a `current` copy plus a `serial`, with the global instance `g_ColorMgmt`
  (`src/rendervulkan.hpp:561`, defined `src/steamcompmgr.cpp:146`).
- Recomputation is driven by `update_color_mgmt()` (`src/steamcompmgr.cpp:466`, called
  once per repaint from `paint_all()` at `src/steamcompmgr.cpp:2578`). It pulls the
  display's native colorimetry/EOTF and max content light level from the active
  backend connector (`GetNativeColorimetry()`/`GetHDRInfo()` — backend-specific, see
  the relevant `features/backend-*.md` page), early-outs if nothing changed
  (`pending == current`, `src/steamcompmgr.cpp:483`), then rebuilds the per-`EOTF`
  shaper (1D) and 3D LUTs into `g_ColorMgmtLuts[EOTF_Count]`
  (`src/rendervulkan.hpp:562`) for each of the two input transfer functions gamescope
  distinguishes: `EOTF_Gamma22` (SDR) and `EOTF_PQ` (HDR10), `enum EOTF`
  (`src/color_helpers.h:233`).
  - `EOTF_Gamma22 → EOTF_Gamma22` needs no remapping (`g22_luminance = 1.f`,
    `src/steamcompmgr.cpp:364`).
  - `EOTF_Gamma22 → EOTF_PQ` (SDR content on an HDR output) maps SDR white to
    `flSDROnHDRBrightness` nits (`src/steamcompmgr.cpp:370`).
  - `EOTF_PQ → EOTF_Gamma22` (HDR content on an SDR output) tonemaps down using
    `flInternalDisplayBrightness` and the tonemap operator (EETF ITU-R BT.2390
    variants, `enum ETonemapOperator`, `src/color_helpers.h:275`) applied to the
    source/display `tonemap_info_t` HDR metadata (`src/steamcompmgr.cpp:383`-`398`).
  These LUTs are what `FrameInfo_t::shaperLut[EOTF_Count]` /
  `lut3D[EOTF_Count]` (`src/rendervulkan.hpp:289`-`290`) get bound to per-frame via
  `bindColorMgmtLuts()` inside `vulkan_composite()`
  (`src/rendervulkan.cpp:4077`-`4078`).
- The PQ transfer function conversions the tonemapping math runs on top of are
  `pq_to_nits()` / `nits_to_pq()` (`src/color_helpers.h:53`, `:69`) — the standard
  SMPTE ST 2084 PQ EOTF, evaluated per-channel via `glm` template functions so the same
  code works scalar or vectorized.
- Per-layer colorspace is a separate axis from the shaper/3D LUT machinery above:
  each `FrameInfo_t::Layer_t` carries a `GamescopeAppTextureColorspace`
  (`enum GamescopeAppTextureColorspace`, `src/gamescope_shared.h:24`:
  `..._LINEAR`, `..._SRGB`, `..._SCRGB`, `..._HDR10_PQ`, `..._PASSTHRU`) that decides
  sRGB view formatting (`setTextureSrgb()`) and whether the layer needs an explicit
  linearization step (`viewConvertsToLinearAutomatically()`,
  `src/rendervulkan.hpp:341`). `ColorspaceIsHDR()` (`src/gamescope_shared.h:35`) flags
  the SCRGB/HDR10_PQ cases.
- A layer's `ctm` (color transform matrix, a `BackendBlob`) applies a fixed matrix
  ahead of blending — e.g. `s_scRGB709To2020Matrix` (`src/steamcompmgr.cpp:169`,
  built from `k_2020_from_709` at `src/steamcompmgr.cpp:8849`) is attached to any
  layer whose colorspace is scRGB (`src/steamcompmgr.cpp:2060`, `:2246`), converting
  a Rec.709-primaries scRGB buffer into the Rec.2020 space the output pipeline
  expects for HDR.
- A layer's `hdr_metadata_blob` (static HDR10 metadata: mastering display
  primaries/luminance, MaxCLL/MaxFALL) is set from the focused window's commit
  feedback — populated by the client via the `gamescope_swapchain`
  `set_hdr_metadata` request (`src/wlserver.cpp:986`) — and mirrored into
  `g_ColorMgmt.pending.appHDRMetadata` once per repaint whenever it changes
  (`src/steamcompmgr.cpp:9260`-`9298`) so the tonemapper can use the app's real
  mastering metadata as the source `tonemap_info_t` instead of a guess.
- The Wayland-side color-management protocols
  (`protocol/color-management-v1.xml`, `protocol/frog-color-management-v1.xml`) are
  the client-facing surface that lets an app *declare* the colorspace/HDR metadata
  above in the first place (and, on the nested-Wayland backend, let gamescope itself
  declare its output colorspace to a host compositor). The protocol requests/events
  themselves, and per-backend advertisement of `wp_color_manager_v1` /
  `frog_color_management_factory_v1`, are covered in
  [wayland-protocols.md](wayland-protocols.md); this page only tracks where the
  values they carry land in `gamescope_color_mgmt_t` / `FrameInfo_t::Layer_t`.

## Using it

HDR is opt-in and depends on backend + display capability:

- `cv_hdr_enabled` (`gamescope::ConVar<bool>`, `src/steamcompmgr.cpp:461`, default
  `false`) — whether HDR output is used at all when the display supports it. See
  [scripting-convars.md](scripting-convars.md) for how convars are set at runtime.
- SDR gamut widening, input gain, and SDR-on-HDR brightness are exposed the same way
  as the rest of gamescope's live-tunable state — via X11 root-window properties the
  compositor polls (e.g. `flSDROnHDRBrightness` is settable from a CLI flag parsed at
  `src/steamcompmgr.cpp:8734` and updated live at `src/steamcompmgr.cpp:569`-`572`).

## Related links

- [compositing-vulkan.md](compositing-vulkan.md) — where the shaper/3D LUTs and CTM
  this page describes get bound and consumed per frame.
- [scaling-filters.md](scaling-filters.md) — the neighboring compute passes that share
  the same command buffer and output-encoding EOTF.
- [wayland-protocols.md](wayland-protocols.md) — the `color-management-v1` /
  `frog-color-management-v1` protocol surface that feeds the values this page tracks.
- [scripting-convars.md](scripting-convars.md) — the general convar/property
  mechanism used to tune color management at runtime.
