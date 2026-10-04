# Frame generation (`image.framegen`)

Shows 2, 3 or 4 frames on screen for every frame the game renders, by synthesising the
ones in between with optical-flow interpolation. Added 2026-10-04. Off by default, per
profile, DISPLAY rail group directly below Shaders.

Code map:

| What | Where |
| --- | --- |
| The library (optical flow + synth shaders) | `subprojects/FrameGen/` (git submodule, MIT, `github.com/Ritze03/FrameGen`) |
| Library build glue | `src/FrameGen/FrameGenLib.cpp` + the `custom_target()`s in `src/meson.build` |
| Renderer host (`fghost`) | `src/FrameGen/FrameGenHost.{h,cpp}` |
| Pacing (`fgpacing::Pacer`) | `src/FrameGen/Pacing.h` (header-only) |
| Pacing glue | `src/steamcompmgr.cpp` (`FrameGen_OnArrival` / `FrameGen_PrePaint` / `FrameGen_PostPaint`) |
| Composite hook | `src/rendervulkan.cpp` (`vulkan_composite()`, `fghost::RecordBaseLayer`) |
| Frame-ring copy shader | `src/shaders/cs_fg_copy.comp` |
| Settings area | `src/Overlay/PanelFrameGen.{h,cpp}` |
| Tests | `tests/test_framegen_pacing.cpp` (28 cases) |

Not to be confused with `lsfg-vk` (an external Vulkan layer): nothing in this tree uses
it. FG here runs inside the compositor.

## What it does, and where it runs in the frame

Between two real game frames P (previous) and C (newest) the library makes N-1 generated
frames at t = k/N; the compositor shows those and then C itself.

Pipeline order inside `vulkan_composite()`:

1. FG substitutes the raw **layer 0** (the game) with the slot to show
   (`SubstituteLayer0`) -- before the ReShade block and the native shader effects.
2. Native shader effects and ReShade run on it like on any frame.
3. The upscaler (FSR / NIS) runs afterwards, in the present composite.
4. HUD, Crosshair, cursor and overlays are drawn on top.

`Why:` before the effects, because the effects then treat generated frames like real ones
with no special case (adaptive effects measure generated frames too, which is harmless).
After the upscaler is impossible: the library works at the game's own resolution. Drawing
the fork's own Crosshair/HUD after FG keeps them sharp, while the game's own HUD may
artifact.

The pre-emptive upscale is bypassed while FG is enabled: it bakes layer 0 at commit time,
which FG cannot substitute per slot.

## Renderer (`fghost`)

- **Off = zero cost**: one relaxed atomic load per frame; no Interpolator, no ring, direct
  scanout stays possible. The host object is created lazily on first use; turning FG off
  frees everything after a `g_device.waitIdle()`.
- **Own command buffer**, submitted just before the composite on the same queue.
  `Why:` a GPU wait (needed before a resize/reconfigure) in the middle of the composite
  would reset the device's upload-buffer offset and corrupt it, and the hook must run
  before the ReShade block, which comes before the composite command buffer exists. A
  pass-through of an already-seen frame records nothing.
- **Private 2-slot frame ring**, always `B8G8R8A8_UNORM`, filled by the tiny compute copy
  `cs_fg_copy.comp`. `Why:` game buffers are never pinned (pinning another commit starves
  3-image swapchains and lowers the real frame rate); commit dmabufs are SAMPLED-only so
  `vkCmdCopyImage` is impossible; the copy also writes alpha 1 and normalises BGRA/RGBA.
- **Lazy**: slot 1 records the flow estimate plus synth(1/N); each later slot records only
  its own synth(k/N) into a pooled cached output; repaints of the same slot (cursor,
  overlay, screenshot) reuse it. A dropped pair never pays for synths it never showed.
- **Skipped** (the real frame is shown and the status says why) for HDR output or content,
  YCbCr, non-8-bit formats, a frame below 16x16, and init/record failure. `Why:` the library
  is 8-bit SDR only, and a hard failure would black the screen.
- **GPU time per pair** is read from the library's timestamps, only when the device
  supports timestamps, without stalling (only after the GPU timeline has passed that
  submission); otherwise the status shows "n/a".
- **`g_device.waitIdle()`** before every Interpolator resize / reconfigure / teardown (the
  library has no deferred free). So a **Quality** change (it alters the flow scale) causes
  one brief pause; the other presets apply live.

## Crosshair protection

Active whenever **Static HUD protection** is Normal or Strong; Off records none of it. No
setting of its own. `Why:` the user, after testing in a real game ("looks REALLY good"):
*"One thing that many frame gens get wrong is the crosshair [...] We won't optimize for
like weird big crosshairs, but like the middle one to maybe three percent of the screen
[...] It should analyze multiple frames and can easily tell if the crosshair isn't moving.
And then it should detect it, that it is the crosshair. Then the image that is being used
to generate frames should have this crosshair removed and at a later point patched on
again. This of course doesn't work as well if there's transparency, but it should work for
most games."* The library's `hudBonus` does not solve it: a static crosshair over a moving
background drags or smears, or makes the background around it stick.

Algorithm (all in `FrameGenHost.cpp`, three shaders, every pass over the ROI only):

1. **ROI**: a centred square of 3% of the frame area, `side = round(sqrt(0.03 * w * h))`
   (332 px at 2560x1440, 249 px at 1920x1080), clamped to the frame. `Why:` the user's
   "middle one to maybe three percent of the screen", and they asked to cover a crosshair
   two to three times the usual size. Off-centre reticles are not covered.
2. **Detect** (`cs_fg_crosshair_detect.comp`, once per NEW real frame, in the ring-copy
   command buffer): a per-ROI-pixel counter of consecutive real frames on which the pixel's
   max channel changed by at most 3.5/255; any larger change resets it to 0 at once. A
   pixel is **masked** at a count of 8, dilated by 2 px (full strength within 1 px, half
   within 2 px, a one-pixel feather). `Why` no "is this a crosshair shape" test: "static
   for 8 frames inside the central ROI" is the whole detector, which is the user's point
   that the crosshair "isn't moving". A still camera also masks static background, which is
   harmless (it is restored with identical pixels), and as soon as anything moves its
   pixels leave the mask on that same pair. `Why` 8 frames: long enough that a stall of
   the picture does not mask a whole scene, short enough to protect within ~0.1 s.
   `Why` every real frame and not only generated pairs: the counters need consecutive real
   frames.
3. **Remove** (`cs_fg_inpaint.comp`): the new frame's original ROI pixels plus the mask are
   saved to a ROI-sized **patch** texture (`.rgb` original, `.a` mask), then the masked
   pixels of the ring slot are overwritten in place with a 1/distance^2-weighted average of
   the nearest unmasked pixel along each of 8 directions (reach 12 px). Motion estimation
   and the warp therefore see plain background where the crosshair was. The previous
   frame's slot was inpainted in its own turn and is only ever `prev`, so nothing is redone
   for it; the next detect compares against the patch texture's ORIGINAL pixels, never the
   inpainted ring. `Why` a directional search and not a push-pull pyramid: a crosshair is
   thin, and this is one dispatch with no scratch levels. A pixel with no unmasked
   neighbour in reach (the whole ROI is still) is left as it is.
4. **Patch back** (`cs_fg_crosshair_patch.comp`): after each `recordSynth` into its output
   texture, `out = mix(out, patch.rgb, mask)` over the ROI. Outputs are cached per
   (pair, k), so repaints keep the crosshair at no extra cost. The real frame shown at slot
   N is the game's own texture and is never touched.

Textures: two ping-pong counter textures and the patch texture, all ARGB8888 ROI-sized
(about 0.44 MB each at 1440p), created lazily and released with the ring (after
`g_device.waitIdle()`). The counters restart from 0 on any gap: `fghost::Reset()`, a
size/format change, FG toggled, HUD protection switched Off and on.

Limits: a semi-transparent crosshair (its pixels change with the background, so it never
reaches the mask); a crosshair that changes shape (spread, hit markers) stays unmasked
while it changes; off-centre reticles; a still camera masks the background too, so a big
moving object crossing the ROI of an otherwise still scene is interpolated from slightly
smeared neighbours. The fork's own Crosshair is drawn after FG and is unaffected.

## Backends

Generated slots must go through the composite, so full composite is forced while FG is on.
DRM also disables partial composite (and therefore direct scanout): a scanned-out buffer
never passes `vulkan_composite()`, and a partial composite has already dropped the base
layer. Wayland and OpenVR force full composite; SDL always composites. See the backend
pages.

## Pacing model (`fgpacing::Pacer`)

Pure decision logic, no clock or Vulkan of its own (times are passed in), so the tests
drive it with a fake clock; `steamcompmgr.cpp` feeds it arrivals and paints and takes back
`(pair, k, n)`.

- **Phase A (now): every backend uses the fixed-refresh vblank sequence.** A real frame
  arriving starts a pair; the N-1 generated frames go out on the next N-1 vblanks, the
  real frame on the vblank after. **Phase B (planned, not done)** adds timer pacing for VRR
  and nested windows; the vblank is the only place the display clock enters
  (`Pacer::OnPaint`), so it can be replaced without touching the rest.
- **Arrival time** is the commit's acquire fence signalling (`commit_t::present_time`, set
  in `handle_done_commit`), not the vblank latch. `Why:` latch times are vblank-quantised
  (about +/-7 ms at 60 fps on a 144 Hz display), which would wreck the interval estimate.
- **Game interval** = median of the last 8 arrival intervals, clamped to 4-50 ms, trusted
  after 4 intervals.
- **"Fits"**: game fps x N <= 1.2 x refresh (so 0.6x refresh at 2x, 0.4x at 3x, 0.3x at
  4x). Refresh is the rate the vblank timer ticks at (nested refresh if set, else output
  refresh). The effective N is the highest N <= the chosen one that fits and passes the
  cost guard; below 2x it passes real frames through with no hold and no added latency.
  `Why:` above about 0.6x refresh the doubled frames collide on vblanks and real frames
  get dropped -- worse than no FG; the user wanted it "always on where it can't make
  things worse". The game is never capped and VRR is never forced off.
- **Hysteresis** of 0.5 s on every N change and pass-through switch (the first estimate, a
  new game or a user change apply at once). Changes take effect only at a pair boundary.
- **Latency first.** A newer real frame arriving mid-pair drops the rest of the pair and
  starts a new one at once, so delay never piles up. Added delay is about (N-1)/N of a game
  frame: 1/2 at 2x, 2/3 at 3x, 3/4 at 4x. `Why:` finishing the old pair first would grow a
  queue whenever the game briefly ran above refresh/N, and latency is what the user is
  least willing to give up.
- **Reset** (no FG for the next frame, the previous real frame is forgotten) on a
  focus/window change, size/format change, a gap over 100 ms, fades, Steam UI windows and
  streaming clients. `Why:` pairs are keyed by commit id, not texture pointer, because
  gamescope reuses one texture per `wl_buffer` -- pointer equality misses re-commits, and
  without a reset two unrelated images would be blended.
- **Cost guard.** If the per-pair FG GPU time exceeds 25% of the game interval, N steps
  down (cost is assumed to scale with N; the first 3 pairs after an N change are ignored
  because the measurement lags); at 2x and still over budget it passes through; a probe
  retries 2x every 10 s so the guard is not permanent. It never changes the user's Quality
  preset (a flow-scale change needs a GPU wait, which would stutter if it flapped). No
  guard without timestamps.

## Settings and status

Area `image.framegen` ("Frame generation"), per profile (game profiles inherit), no
keybind, config section `framegen` (schema stays 5; additive).

| Key | Values | Meaning |
| --- | --- | --- |
| `framegen.multiplier` | 0 / 2 / 3 / 4 (Off default) | frames shown per game frame |
| `framegen.quality` | `quality` / `performance` | Performance = flow scale 4, sub-pixel off: about a third cheaper, can miss thin fast detail |
| `framegen.safety` | `low` / `default` / `high` | trust ramp (16,56) / (12,40) / (8,28): how readily doubtful pixels fall back to the real frame |
| `framegen.hud_protection` | `off` / `normal` / `strong` | zero-vector bonus 0 / 1 / 2.5: keeps a static HUD from wobbling; any value but `off` also switches on [Crosshair protection](#crosshair-protection) |

The three sub-rows are disabled while Off. `Why:` the library's expert dials
(searchPenalty, smoothBonus, sceneCutSad, globalFallback) are deliberately not exposed --
only the library's defaults were visually reviewed, and raw sliders would invite settings
nobody has looked at.

Status line: `game 60 fps -> presented ~120 fps · 4× (2× active) · FG 0.42 ms · +8.3 ms
delay`, or the step-down / pass-through / unavailable reason, `Off`, or `Waiting for
frames`. It writes `->` and 1/2, 2/3, 3/4 in help text because the overlay font atlas is
Latin-1 only (no U+2192 or the vulgar-fraction glyphs). The HUD's FPS keeps counting real
game frames; MangoHud with output timing shows the presented rate.

## When it steps down or passes through

| Situation | Result |
| --- | --- |
| game fps x N above 1.2 x refresh | steps N down; below 2x, real frames, no added latency |
| per-pair GPU time over 25% of the game interval | steps N down, then pass-through; retried every 10 s |
| HDR output/content, YCbCr, non-8-bit, under 16x16, init failure | real frame, status names the reason |
| focus/size/format change, gap over 100 ms, fade, Steam UI, streaming client | one frame without FG, then resumes |

## Interaction with the Frame limiter

The Frame limiter caps the *game*; FG multiplies the capped rate. refresh/N is the natural
pairing (e.g. 144 Hz, limit 48, 3x). FG never caps the game itself. See
[resolution-and-refresh](resolution-and-refresh.md).

## Costs

Library measurements on a 7900 XTX, ms of GPU per real pair:

| Resolution | 2x / 3x / 4x, defaults | 2x / 3x / 4x, Performance |
| --- | --- | --- |
| 1080p | 0.35 / 0.42 / 0.49 | 0.19 / 0.26 / 0.31 |
| 1440p | 0.50 / 0.62 / 0.71 | 0.25 / 0.36 / 0.46 |

About 12 MB VRAM at 1080p plus the frame ring (about 8 MB per slot at 1080p).

Crosshair protection (estimated from the texel-fetch count, not measured on a GPU): per real
frame about 0.03 ms typical and up to about 0.1 ms on a still scene at 1440p (~110k ROI
pixels; detect 4 fetches, mask 26, inpaint up to 96 on masked pixels only), plus a ~110k-pixel
load/store per generated frame; about 1.3 MB VRAM.

## Limitations

- SDR, 8-bit only.
- Compute on the same queue, no async: FG time adds to composite time.
- Adds delay; not suited to twitch shooters.
- Fast flicks can leave seams; a semi-transparent HUD over motion artifacts; scene cuts
  hold the nearer real frame. A game's own still centred crosshair is protected (see
  above) except when semi-transparent or changing shape.
- With several virtual connectors (VR) a slot repaint could be swallowed by the shared
  force-repaint flag.
- Phase B (timer pacing for VRR / nested windows) is not implemented yet.

## Build

The library has no root `meson.build`, and Meson's sandbox forbids handing files under
`subprojects/` to the parent project (`Sandbox violation: Tried to grab file ... from a
nested subproject`), so `subproject()` is impossible. Its 11 shaders are compiled with
`custom_target()` (same glslang flags and `--vn <name>_spv` embedded headers as gamescope's
own) using absolute paths, and `framegen.cpp` is compiled through the wrapper
`src/FrameGen/FrameGenLib.cpp`. `Why:` bumping the submodule pulls upstream library work
with no copy to drift. `fgtest` is not built. See [build-and-tooling](build-and-tooling.md).

## Testing

`tests/test_framegen_pacing.cpp` (28 cases) covers the pure pacer: slot sequencing,
interval estimation, fits/step-down, hysteresis, cost guard and probe, resets. The
renderer needs a GPU and is verified by eye on a real game (status line + MangoHud output
timing).

## Related

[shader-effects](shader-effects.md) · [compositing-vulkan](compositing-vulkan.md) ·
[resolution-and-refresh](resolution-and-refresh.md) · [backend-drm](backend-drm.md)
