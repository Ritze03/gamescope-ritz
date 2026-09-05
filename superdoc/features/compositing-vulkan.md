# Compositing (Vulkan) — the async-compute layer compositor

Gamescope has no fixed-function compositing path: every frame, every window layer
(base plane, overlays, cursor, blur, upscale) is drawn by hand-written Vulkan compute
shaders dispatched on a compute queue, not the graphics pipeline. This is the core that
turns a `FrameInfo_t` description of "what to draw this frame" into a finished output
image ready for presentation.

## How it works

- The single entry point is `vulkan_composite()` at `src/rendervulkan.cpp:4030`
  (declared `src/rendervulkan.hpp:453`). Callers (see below) fill in a `FrameInfo_t`
  describing the frame, and get back an `std::optional<uint64_t>` submission sequence
  number to wait on.
- **`FrameInfo_t`** (`src/rendervulkan.hpp:281`) is the per-frame layer stack: up to
  `k_nMaxLayers` entries in its nested `FrameInfo_t::LayerStack_t layers` member, plus
  frame-wide flags (`useFSRLayer0`,
  `useNISLayer0`, `blurLayer0`, `outputEncodingEOTF`, per-EOTF `shaperLut`/`lut3D`
  color-management LUTs). Each `FrameInfo_t::Layer_t` (`src/rendervulkan.hpp:296`)
  carries a texture, offset/scale, opacity, `zpos`, upscale `filter`, alpha-blend mode,
  colorspace, and optional `ctm`/`hdr_metadata_blob` — see
  [hdr-color-management.md](hdr-color-management.md) for how those two get populated.
  *Why a fixed-size array instead of a `vector`:* the layer count is bounded by real
  DRM/hardware overlay-plane limits (`k_nMaxLayers` = 6, `src/rendervulkan.hpp:40`),
  so a static array avoids per-frame heap churn on the hot path.
- `vulkan_composite()` picks one of four compute paths per frame based on
  `frameInfo->useFSRLayer0` / `useNISLayer0` / `blurLayer0`, else falls through to a
  plain blit — see [scaling-filters.md](scaling-filters.md) for the FSR/NIS branches in
  detail. Every path ends by binding all layers with `bind_all_layers()`
  (`src/rendervulkan.cpp:3949`), which also decides per-layer nearest-vs-linear
  sampling from `Layer_t::filter` and `isScreenSize()`.
- Before any of that, two layer-0 pre-passes may run, in this order, each substituting
  its output for `layers[0].tex` in a *private copy* of the `FrameInfo_t` so the path
  chosen below consumes it unchanged — `vulkan_composite()` takes a `const FrameInfo_t *`
  and never writes the caller's struct: (1) if a user's ReShade effect is active
  (`g_reshade_effect` non-empty), the base layer is run through `g_reshadeManager`'s
  pipeline on the general queue with a CPU wait; (2) if any bundled Shaders-area effect
  is on (`g_nativeEffects.AnyEnabled()`), the native `SHADER_TYPE_EFFECTS_MEASURE` (one
  workgroup, Adaptive Brightness's luminance EMA into the persistent 1×1
  `g_output.effectsHistory`) and `SHADER_TYPE_EFFECTS_LAYER0` (`cs_effects_layer0.comp`,
  per pixel) compute passes are recorded on the same command buffer into the pooled
  `g_output.effectsOutput`, SDR/non-YCbCr only. Both run on the ReShade output when a
  user `.fx` is set. `FrameInfo_t::bBaseLayerEffectsApplied` — set only by steamcompmgr,
  on a struct whose layer 0 is the pre-emptively upscaled texture — skips both, since
  that texture was built by a `vulkan_composite()` that already ran them; every other
  composite of a frame (the present one, DRM's copy, a `gamescopectl screenshot`
  re-composite of the same struct) runs them fresh from the raw layer 0, exactly once
  each. Ordering fact only — see [reshade-effects.md](reshade-effects.md) and
  [shader-effects.md](shader-effects.md).
- The composited result also gets an inline conversion/blit into an optional
  `pPipewireTexture` output texture in the same command buffer
  (`src/rendervulkan.cpp:4209`-`4255`) when PipeWire streaming is active, reusing the
  BLIT/RGB_TO_NV12 pipelines. The PipeWire consumer side is
  [screen-capture-pipewire.md](screen-capture-pipewire.md); this page only notes that
  the tap is a second target bound in the same dispatch, not a separate composite pass.
- Work is recorded on a `CVulkanCmdBuffer` obtained from `CVulkanDevice::commandBuffer()`
  and handed to `CVulkanDevice::submit()` (`src/rendervulkan.hpp:818`-`819`); the device
  (`class CVulkanDevice`, `src/rendervulkan.hpp:810`, global instance
  `g_device` at `src/rendervulkan.hpp:1054`) prefers a compute-only queue family
  (`VK_QUEUE_COMPUTE_BIT` search in `src/rendervulkan.cpp:365`) over a combined
  graphics+compute one when the driver offers one. *Why async compute instead of the
  graphics pipeline:* gamescope's job is small, latency-critical dispatches every
  frame (blit/upscale/blur/colorspace-convert), not a rasterization pipeline with
  fixed-function blending — compute shaders let it skip render-pass/pipeline-barrier
  overhead it doesn't need, and a dedicated compute queue can run alongside a game's
  own graphics queue instead of contending with it.
- Client buffer commits are turned into sampleable Vulkan textures by `src/commit.cpp`;
  textures created from a given `wlr_buffer` are cached so a buffer committed once but
  referenced by multiple frames (e.g. while paused) doesn't get re-imported — see
  `CBufferMemo` (per-buffer cache entry, `src/BufferMemo.h:21`) and
  `CBufferMemoizer` (owning lookup table, `src/BufferMemo.h:45`). *Why memoize:*
  DMA-BUF/wl_shm import has real driver-side cost; a still frame shouldn't pay it
  every vblank. How a committed buffer reaches the window stack that
  becomes `FrameInfo_t` layers is [steamcompmgr-focus.md](steamcompmgr-focus.md)'s
  territory.
- Explicit GPU/DRM sync (rather than implicit fencing) is modeled by `CTimeline`
  (`src/Timeline.h:25`), a thin wrapper over a DRM syncobj that can be turned into a
  Vulkan timeline semaphore via `ToVkSemaphore()`. This is what lets gamescope hand a
  DRM KMS commit a wait/signal point that a Vulkan submission also participates in,
  without a blocking CPU round-trip.
- The composite result is not presented directly by this file: the caller passes the
  finished `FrameInfo_t` to the active backend's `IBackendConnector::Present()`
  (`src/backend.h:205`), which itself calls back into `vulkan_composite()`/
  `vulkan_present_to_window()` (`src/rendervulkan.cpp:3000`) depending on whether the
  backend uses a Vulkan swapchain (`GetBackend()->UsesVulkanSwapchain()`) or a raw DRM
  KMS submit. See the relevant `features/backend-*.md` page for the per-backend half of
  that call.

- Composite dispatch isn't free-running: `paint_all()` is paced by the vblank timer,
  `class CVBlankTimer` (`src/vblankmanager.hpp:27`), which schedules the next repaint
  relative to the display's actual vblank interval rather than running flat-out. This
  page only notes the coupling; the full frame-pacing loop belongs to
  [../architecture/overview.md](../architecture/overview.md).

## Layer order (zpos)

Compositing order is **push order**, not `zpos` order: `bind_all_layers()`'s shader
loop (`cs_composite_blit.comp` and friends) folds layer `i` onto the accumulated
result via `BlendLayer()` (`src/shaders/alphamode.h`) strictly for `i` in
`0..layers.count()-1`, in the order each `Layer_t` was `push()`ed into
`FrameInfo_t::layers` — nothing sorts by `zpos`. The `g_zpos*` constants
(`src/steamcompmgr.hpp`) are therefore not an independent sort key; they're a
same-order tag used elsewhere (screenshot truncation, DRM plane assignment,
cursor/base-plane identification) that **must** be kept numerically consistent
with the push order `paint_all()` (`src/steamcompmgr.cpp`) actually uses, or the
two silently disagree — see that constants block's own comment.

Bottom-to-top as of 2026-09-03: base plane / override / external overlay /
override-window overlay (`g_zposBase`..`g_zposOverlay`) → cursor
(`g_zposCursor`) → mura correction (`g_zposMuraCorrection`) → FPS HUD
(`g_zposFpsDisplay`; also carries the [crosshair](crosshair.md), and pushes a
second `g_zposFpsDisplay` layer sampling the other half of its texture only when
the HUD's Inverted text colour and the crosshair are both on) → toast
notifications (`g_zposNotifications`) → settings overlay/Shell
(`g_zposSettingsOverlay`), topmost of all. *Why:* the user asked
for the HUD and toasts to sit **beneath** the Shell rather than drawing over
it — an earlier version pushed the settings overlay first (so it composited
*under* the HUD and toasts) while its own comment claimed the opposite; both
the push order and the `g_zpos*` values were corrected together so they can't
drift apart again.

## Using it

There's no direct end-user control surface here — this is the render core other
subsystems drive. The frame this page describes is assembled once per repaint by
`paint_all()` in `src/steamcompmgr.cpp:2564`, which walks the focused window stack,
fills in a `FrameInfo_t`, and calls into a backend's `Present()`. See
[steamcompmgr-focus.md](steamcompmgr-focus.md) for how the window/focus state feeding
that assembly is decided.

## Related links

- [scaling-filters.md](scaling-filters.md) — the FSR/NIS/nearest/integer branches
  `vulkan_composite()` dispatches into.
- [hdr-color-management.md](hdr-color-management.md) — the shaper/3D LUTs, CTM, and
  HDR metadata blob each layer carries into this pipeline.
- [reshade-effects.md](reshade-effects.md) — the post-process step that runs on the
  base layer before this pipeline's main dispatch.
- [screen-capture-pipewire.md](screen-capture-pipewire.md) — the parallel PipeWire
  output tap bound in the same command buffer.
- [steamcompmgr-focus.md](steamcompmgr-focus.md) — how the window stack that becomes
  `FrameInfo_t` layers is decided.
- [../architecture/overview.md](../architecture/overview.md) — the render loop in
  context of the whole process.
