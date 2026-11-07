# Frame generation (`image.framegen`)

Shows extra frames on screen between the game's own, by synthesising them with
optical-flow interpolation: a fixed multiplier of 2x to 8x, or a **Target fps**. Added
2026-10-04, pacing reworked the same day (per-vblank fractional pacing, Target fps, Low
latency / Smoothness, up to 8x). Off by default (a plain on/off switch, separate from the multiplier choice), per profile. Rail group **MOTION**
(first area; Motion blur is the second), moved there from DISPLAY the same day -- see
[motion-blur](motion-blur.md) for the sibling feature the two share a pacer and a renderer with.

**Architecture rule** (the user): *"you're basically only building the GUI in this chat and
most of the stuff should go into the frame gen itself"*. The optical flow, the synth, the
motion blur, UI protection **and the pacing** are the library's (`frame-gen-ritz`, pinned at
`981c844` (the library's local-only `ml-fg-testing` branch, which carries PRs #15 `pacing-hysteresis`, #16 `pacing-fixes`, #17 `extra-timer-leave`, #18 `ui-box-margin` and #19 `target-start-band` from main; the pin on main was `b77d058`, and `0015453` before #19)); gamescope is the GUI and the platform glue (arrival times, the vblank timer, the
composite hook, the settings).

Code map:

| What | Where |
| --- | --- |
| The library (optical flow + synth shaders) | `subprojects/FrameGen/` (git submodule, MIT, `github.com/Ritze03/frame-gen-ritz`) |
| Library build glue | `src/FrameGen/FrameGenLib.cpp` + the `custom_target()`s in `src/meson.build` |
| Renderer host (`fghost`) | `src/FrameGen/FrameGenHost.{h,cpp}` |
| Pacing (`framegen::pacing::Pacer`) | **the library**: `subprojects/FrameGen/gpu/pacing.h` (header-only; tests: its own `pacingtest`). gamescope's `src/FrameGen/Pacing.h` was deleted 2026-10-04 |
| Wake-up lead | `src/vblankmanager.{hpp,cpp}` (`MeasuredLead`, `vblank_measured_lead`, `GetLastLead`); setting: [Low-latency wake-up](display-general.md#low-latency-wake-up-displaylow_latency_wakeup) |
| Pacing glue + the main-loop gate | `src/steamcompmgr.cpp` (`FrameGen_OnArrival` / `FrameGen_PrePaint` / `FrameGen_PostPaint` / `FrameGen_TimerPaced`, `PaintTick` in the paint decision) |
| Composite hook | `src/rendervulkan.cpp` (`vulkan_composite()`, `fghost::RecordBaseLayer`) |
| Frame-ring copy shader | `src/shaders/cs_fg_copy.comp` |
| Settings area | `src/Overlay/PanelFrameGen.{h,cpp}` |
| Tests | `tests/test_config.cpp` (the `framegen` and `motion_blur` keys); `tests/test_framegen_pacing.cpp` (lag buffer off = 0 ms through the library's `Pacer`); pacing itself is tested in the library |

Not to be confused with an external frame-generation Vulkan layer: nothing in this tree uses
it. FG here runs inside the compositor.

## What it does, and where it runs in the frame

Between two real game frames P (previous) and C (newest) the library can make a generated
frame at **any** t in (0,1) -- one motion estimate per pair, any number of synths after it.
Pacing decides, for every output frame, which t to show (see [Pacing](frame-generation-pacing.md#pacing-frame-gen-ritz-gpupacingh-framegenpacingpacer)).

Pipeline order inside `vulkan_composite()`:

1. FG substitutes the raw **layer 0** (the game) with the output frame to show
   (`SubstituteLayer0`) -- before the ReShade block and the native shader effects.
2. Native shader effects and ReShade run on it like on any frame.
3. The upscaler (FSR / NIS) runs afterwards, in the present composite.
4. HUD, Crosshair, cursor and overlays are drawn on top.
   (The game's own still HUD is kept pixel-exact by [UI protection](frame-generation-ui-protection.md#ui-protection).)

`Why:` before the effects, because the effects then treat generated frames like real ones
with no special case (adaptive effects measure generated frames too, which is harmless).
After the upscaler is impossible: the library works at the game's own resolution. Drawing
the fork's own Crosshair/HUD after FG keeps them sharp, while the game's own HUD may
artifact unless UI protection holds it.

The pre-emptive upscale is bypassed while FG or motion blur is on (`fghost::Active()`): it bakes layer 0 at commit time,
which FG cannot substitute per output frame.

## Sub-pages

| Page | Covers |
| --- | --- |
| [Frame generation: renderer and HDR / 10-bit](frame-generation-renderer.md) | Renderer host (`fghost`), HDR and 10-bit games |
| [Frame generation: Motion version 2](frame-generation-motion-v2.md) | Motion version 2 ("morphing" interpolator) |
| [Frame generation: Learned motion](frame-generation-learned.md) | Learned motion, nets, rounds 1-4b |
| [Frame generation: UI protection](frame-generation-ui-protection.md) | UI protection, Crosshair V2, the box and its preview |
| [Frame generation: pacing](frame-generation-pacing.md) | Pacing, VRR/tearing, latency, hysteresis, pause rules, history |
| [Frame generation: settings and status](frame-generation-settings.md) | Settings and status, logging |
| [Frame generation: backends, host frames and direct scanout](frame-generation-host-scanout.md) | Backends, frames the host shows, direct scanout |
| [Frame generation: pass-through, costs, limitations, build and testing](frame-generation-limits.md) | When it passes real frames through, Frame limiter, costs, limitations, build, testing |

## Related

[lag-spike-buffer](lag-spike-buffer.md) · [motion-blur](motion-blur.md) · [shader-effects](shader-effects.md) · [compositing-vulkan](compositing-vulkan.md) ·
[resolution-and-refresh](resolution-and-refresh.md) · [frame-limiter](frame-limiter.md) · [display-general](display-general.md) · [backend-drm](backend-drm.md)

## History

- **Frame generation** (glossary detail): this fork's in-compositor frame interpolation (2026-10-04; settings area `image.framegen`, "Frame generation", MOTION rail group, per profile, off by default).
  - What it does: it synthesises frames between two real game frames with optical flow, either a **fixed 2x-8x** multiplier or a **Target fps**, with a **Priority** of *Low latency* (least delay, content time snaps to each new real frame) or *Smoothness* (even spacing, about one game frame of delay).
  - Pacing: it is paced **per vblank**. Every output frame is generated for its own content time `tau = V - D` between the two latest real frames, so "how many generated frames per real frame" is never held, it falls out of the arrival times.
  - **Pause at refresh rate** (`framegen.pause_at_refresh`, default on) decides whether it stops generating once the game alone reaches the refresh rate (on) or keeps generating above it, e.g. 800 fps, painting on its own timer between vblanks where the backend can show that (off).
  - Code: `src/FrameGen/FrameGenHost.{h,cpp}` (`fghost`, the renderer glue), `subprojects/FrameGen/gpu/pacing.h` (`framegen::pacing::Pacer`, the pure pacing logic; `fgpacing` is only a local namespace alias in `src/steamcompmgr.cpp`), `FrameGen_*` in `src/steamcompmgr.cpp`, the hook in `vulkan_composite()`, the panel `src/Overlay/PanelFrameGen.cpp`.
  - Order: it runs on the raw game layer *before* the native shader effects, ReShade and the upscalers, so generated frames get the same effects as real ones; the Crosshair and HUD are drawn after it.
  - Distinct from a **Shaders** effect (it changes *how many* frames are shown), from the FSR/NIS **upscalers** and from an external frame-generation Vulkan layer (**not** used in-tree).
  - It never caps the game or forces VRR off.
  - It passes real frames through only when the game already reaches the output rate, the GPU cost guard (the "Limit to GPU speed" switch, off by default) trips, or the content is unsupported (YCbCr, or a pixel format outside 8-bit / 10-bit / fp16 RGB; HDR and 10-bit games work since 2026-10-04).
- **FrameGen** (glossary detail): the frame-interpolation *library* itself (`subprojects/FrameGen`, a git submodule of github.com/Ritze03/frame-gen-ritz, MIT).
  - Contents: optical flow + synthesis compute shaders; 8-bit, 10-bit and fp16 RGB, SDR or HDR, built as per-format shader variants.
  - Integration: compiled straight into the gamescope binary (`src/FrameGen/FrameGenLib.cpp` wraps its `framegen.cpp`; its shaders are `custom_target()`s in `src/meson.build`), not a separate process or Vulkan layer.
  - **Frame generation** (the previous bullet) is the feature built on it.
