# Frame generation (`image.framegen`)

Shows extra frames on screen between the game's own, by synthesising them with
optical-flow interpolation: a fixed multiplier of 2x to 8x, or a **Target fps**. Added
2026-10-04, pacing reworked the same day (per-vblank fractional pacing, Target fps, Low
latency / Smoothness, up to 8x). Off by default (a plain on/off switch, separate from the multiplier choice), per profile. Rail group **MOTION**
(first area; Motion blur is the second; the Lag spike buffer was the third, disabled 2026-11-07), moved there from DISPLAY the same day -- see
[motion-blur](motion-blur.md) for the sibling feature the two share a pacer and a renderer with.

**Architecture rule** (the user): *"you're basically only building the GUI in this chat and
most of the stuff should go into the frame gen itself"*. The optical flow, the synth, the
motion blur, UI protection **and the pacing** are the library's (`frame-gen-ritz`, pinned at
`a1d18f9` (library `dev`, since 2026-11-26: V3.5 cheaper, one more shader variant
`v3_search_35` in the build list, no API or setting change; before that `1ccc602`, since
2026-11-25: `Settings::v3Motion` and V3.5, see [frame-generation-v3-5.md](frame-generation-v3-5.md); `Learned` unchanged; before that `678f9cc`, since 2026-11-25: Learned keeps only the net e2v5 (`LearnedNet` value 4, the one the host always passes; every other value is refused under any motion version), nine nets and the shaders only they used left the library, frames byte-identical, see [frame-generation-learned.md](frame-generation-learned.md); before that `d9ee7c7`, since 2026-11-24: the learned blend keeps two nets, Fluidity (`v3Blend` 1, was 6) and Consistency (`v3Blend` 2, was 15), `kV3BlendMax` = 2, frames byte-identical to the old 6 and 15, see [frame-generation-v3-hybrid.md](frame-generation-v3-hybrid.md); before that `d6d31df`, since 2026-11-24: only offline scripts differ from the pin before; before that `5df970e`, since 2026-11-24: the learned blend's weight sets are now base, A-, AR, AR3 and AR4 (AR2 left; `v3Blend` 1..9 unchanged, 10..15 = AR3, AR4), `kV3BlendMax` = 15; before that `aefad17`: nine more nets, the sets AR, AR2 and AR3, `v3Blend` 1..6 unchanged, see [frame-generation-v3-hybrid.md](frame-generation-v3-hybrid.md); before that `88e071e`, since 2026-11-24: any number of learned-blend weight sets, public `framegen::kV3BlendMax` = 6, a 4-bit variant field in the synthesis; outputs for `v3Blend` 0..6 unchanged, see [frame-generation-v3-hybrid.md](frame-generation-v3-hybrid.md); before that `bfaade7`, since 2026-11-23: the UI removed debug view draws no colour or outline under Crosshair V2 either, its masked pixels are filled from nearby ones, see [frame-generation-debug.md](frame-generation-debug.md); before that `ce8b383`, since 2026-11-23: `Settings::debugView`, the Motion / UiMask / UiClean debug views drawn by the library, see [frame-generation-debug.md](frame-generation-debug.md); before that `1e68c8d`, since 2026-11-23: V3 Hybrid's second set of trained weights, `v3Blend` 4..6; before that `19b15ef`, since 2026-11-23: the trained weights of V3 Hybrid's three nets, first round; before that `34d8bc1`, since 2026-11-23: V3 Hybrid's learned blend also in the 10-bit format, `Interpolator::v3BlendActive()`; before that `c62376c`, since 2026-11-23: `Settings::v3Blend`, the learned blend with zero weights, 8-bit only; before that `6b1db03`, since 2026-11-21: Motion version 3 shows the nearer real frame for a pair whose two frames do not correspond, and two UI-protection bookkeeping fixes; before that `2dc54bb`, Motion version 3 uses the plain blend of the two real frames where a motion vector points out of the picture in both frames or a value is undefined; before that `275fef0`, its synthesis stores the plain blend where its colour is not finite; before that `f301d7d`, Motion version 3's motion guard and its two shaders; before that `c9c6718`, V3 honours Static HUD protection; before that `d61951c`, V3 frame-edge band and projection fix and scene cut default Never; before that `acab885`, Motion version 3's two switches and final shader builds; before that `c2644af`, Motion version 3; before that `185fdf5`, the reach settings and the wide search shaders; before that `981c844`, the library's local-only `ml-fg-testing` branch, which carries PRs #15 `pacing-hysteresis`, #16 `pacing-fixes`, #17 `extra-timer-leave`, #18 `ui-box-margin` and #19 `target-start-band` from main; the pin on main was `b77d058`, and `0015453` before #19)); gamescope is the GUI and the platform glue (arrival times, the vblank timer, the
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
| [Frame generation: the capture](frame-generation-capture.md) | `ritz_fg_capture`: record generated frames, their real pairs, settings and pacer decisions to files for offline replay |

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
