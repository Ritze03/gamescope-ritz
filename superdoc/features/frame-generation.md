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
`615a2d1`); gamescope is the GUI and the platform glue (arrival times, the vblank timer, the
composite hook, the settings).

Code map:

| What | Where |
| --- | --- |
| The library (optical flow + synth shaders) | `subprojects/FrameGen/` (git submodule, MIT, `github.com/Ritze03/frame-gen-ritz`) |
| Library build glue | `src/FrameGen/FrameGenLib.cpp` + the `custom_target()`s in `src/meson.build` |
| Renderer host (`fghost`) | `src/FrameGen/FrameGenHost.{h,cpp}` |
| Pacing (`framegen::pacing::Pacer`) | **the library**: `subprojects/FrameGen/gpu/pacing.h` (header-only; tests: its own `pacingtest`). gamescope's `src/FrameGen/Pacing.h` was deleted 2026-10-04 |
| Wake-up lead | `src/vblankmanager.{hpp,cpp}` (`MeasuredLead`, `vblank_measured_lead`, `GetLastLead`); setting: [Low-latency wake-up](resolution-and-refresh.md#low-latency-wake-up-displaylow_latency_wakeup) |
| Pacing glue + the main-loop gate | `src/steamcompmgr.cpp` (`FrameGen_OnArrival` / `FrameGen_PrePaint` / `FrameGen_PostPaint` / `FrameGen_TimerPaced`, `PaintTick` in the paint decision) |
| Composite hook | `src/rendervulkan.cpp` (`vulkan_composite()`, `fghost::RecordBaseLayer`) |
| Frame-ring copy shader | `src/shaders/cs_fg_copy.comp` |
| Settings area | `src/Overlay/PanelFrameGen.{h,cpp}` |
| Tests | `tests/test_config.cpp` (the `framegen` and `motion_blur` keys); `tests/test_framegen_pacing.cpp` (lag buffer off = 0 ms through the library's `Pacer`); pacing itself is tested in the library |

Not to be confused with `lsfg-vk` (an external Vulkan layer): nothing in this tree uses
it. FG here runs inside the compositor.

## What it does, and where it runs in the frame

Between two real game frames P (previous) and C (newest) the library can make a generated
frame at **any** t in (0,1) -- one motion estimate per pair, any number of synths after it.
Pacing decides, for every output frame, which t to show (see [Pacing](#pacing-frame-gen-ritz-gpupacingh-framegenpacingpacer)).

Pipeline order inside `vulkan_composite()`:

1. FG substitutes the raw **layer 0** (the game) with the output frame to show
   (`SubstituteLayer0`) -- before the ReShade block and the native shader effects.
2. Native shader effects and ReShade run on it like on any frame.
3. The upscaler (FSR / NIS) runs afterwards, in the present composite.
4. HUD, Crosshair, cursor and overlays are drawn on top.
   (The game's own still HUD is kept pixel-exact by [UI protection](#ui-protection).)

`Why:` before the effects, because the effects then treat generated frames like real ones
with no special case (adaptive effects measure generated frames too, which is harmless).
After the upscaler is impossible: the library works at the game's own resolution. Drawing
the fork's own Crosshair/HUD after FG keeps them sharp, while the game's own HUD may
artifact unless UI protection holds it.

The pre-emptive upscale is bypassed while FG or motion blur is on (`fghost::Active()`): it bakes layer 0 at commit time,
which FG cannot substitute per output frame.

## Renderer (`fghost`)

- **Off = zero cost**: one relaxed atomic load per frame; no Interpolator, no ring, direct
  scanout stays possible. The host object is created lazily on first use; turning **all three**
  (FG, motion blur, the lag spike buffer) off frees everything after a `g_device.waitIdle()`.
  The three share one host (one Interpolator, one ring, one output pool): the gate is
  `Config::enabled || BlurConfig::enabled || LagBufferConfig::enabled` (`fghost::Active()`;
  `Enabled()` stays "frame generation"). **How the three combine** (the library's pacer owns
  it; gamescope sets each `Inputs` field from its own config): frame generation = which
  instants get an output (the cadence), motion blur = the shutter window of each output, the
  lag spike buffer = how far behind real time we show. Any subset works; with only the buffer
  on, real frames are shown and generated frames appear only inside gaps -- see
  [lag-spike-buffer](lag-spike-buffer.md).
- **Backends force a full composite while `fghost::RenderWanted()`** -- FG, blur or the lag spike buffer enabled,
  *or* it still holds resources -- so turning the last one off releases them: the next
  composite runs `RecordBaseLayer`, which tears down, instead of direct scanout skipping it
  forever. (`SetConfig` / `SetBlurConfig` to Off also ask for that composite with
  `force_repaint()`.)
- **API**: `SetFrame(FrameRequest{ newestId, prevId, currId, outId, t, t0, t1, showId,
  historyDepth })`. `t < 0` is inert (not even the ring copy: settled pass-through). **The one
  renderer rule** (the pacer's `Decision`, applied as it says): `prevId != 0` ->
  `recordSynthBlur( prev, curr, out, t0, t1 )` into a pooled output (a plain synth when
  `t0 == t1`, which also covers blur off; with blur on a *real* frame's output carries a pair
  too, `t = 1`, and is blurred); `prevId == 0` -> no renderer work, the real frame `showId`
  is shown (layer 0 when it is the newest, which with no buffer delay it always is; an older
  one still in the ring is substituted). The ring copy of a new `newestId` always happens.
  The same `outId` is the same image, so a repaint (cursor, overlay, screenshot) reuses the
  cached output. `Why` the pair is named by id and not "the newest pair": Smoothness may
  still be playing the pair before the newest one.
- **Own command buffer**, submitted just before the composite on the same queue.
  `Why:` a GPU wait (needed before a resize/reconfigure) in the middle of the composite
  would reset the device's upload-buffer offset and corrupt it, and the hook must run
  before the ReShade block, which comes before the composite command buffer exists. A
  pass-through of an already-seen frame records nothing.
- **Private frame ring** (3 slots normally; it FOLLOWS the pacer's `HistoryDepth()` through
  `FrameRequest::historyDepth`, up to the library's `kHistoryMax` = 65 with the lag spike
  buffer: grown at once, shrunk only after a 4 s dwell at a deliberate `waitIdle`, with the
  library's `Settings::uiHistory` kept equal to it while UI protection is on -- see
  [lag-spike-buffer](lag-spike-buffer.md)'s "gamescope wiring"), in the game's bit depth (`B8G8R8A8_UNORM` /
  `A2B10G10R10` / `R16G16B16A16_SFLOAT`, see [HDR and 10-bit games](#hdr-and-10-bit-games)), filled by the tiny compute copy
  `cs_fg_copy.comp`; each slot remembers its commit id. `Why:` game buffers are never
  pinned (pinning another commit starves 3-image swapchains and lowers the real frame
  rate); commit dmabufs are SAMPLED-only so `vkCmdCopyImage` is impossible; the copy also
  writes alpha 1 and normalises BGRA/RGBA. `Why` three: Low latency only needs two (the
  newest pair); Smoothness can be inside the pair before the newest while the newest has
  already arrived.
- **Lazy**: the first synth of a pair records the estimate plus its synth; each later output
  records only its own synth into a pooled cached output. A dropped pair never pays for
  synths it never showed. **Pool: 3 outputs** (the shown one, the one in flight, one spare),
  least-recently-used; at most **24 synths per pair** (the library's descriptor ring is 32
  and counts the not-yet-retired submissions too). Beyond the cap the nearest cached output,
  or the real frame, is shown. `Why 3`: one composite shows an output while the next paints
  into another; the old per-k pool also needed up to 3 (4x).
- **Skipped** (the real frame is shown and the status says why) for YCbCr surfaces, a game pixel
  format that is none of 8-bit RGB / 10-bit RGB / fp16 RGB (565, 16-bit UNORM), a 10-bit / fp16
  game on a GPU that cannot do that format, a pass-through-tagged surface, a frame below 16x16,
  and init/record failure. HDR and 10-bit games are NOT skipped any more -- see
  [HDR and 10-bit games](#hdr-and-10-bit-games). `Why:` a hard failure would black the screen.
- **GPU time** is read from the library's timestamps, only when the device supports
  timestamps, without stalling (only after the GPU timeline has passed that submission);
  otherwise the status shows "n/a". Per pair the renderer publishes the **estimate's** time
  and the **mean time of one synth** (and their sum, `lastPairGpuMs`), plus a sequence number
  that moves with every new reading; the pair's profile is read once it is over (a pair has a
  variable number of synths now), and the idle gap before every later synth of a pair is
  not counted.
- **`g_device.waitIdle()`** before every Interpolator resize / reconfigure / teardown (the
  library has no deferred free). So a **Quality** change (it alters the flow scale) causes
  one brief pause; the other presets apply live.
- **Motion blur settings** reach the renderer as `framegen::Settings::blurSamples` /
  `blurWeights` (runtime `setSettings()`, no wait; `blurSamples = 0` while blur is off) and
  the pacer as `Inputs::blur*`. The GPU timing harvest counts the library's "blur lookup" /
  "blur mismatch" / "blur resolve" / "blur fixup" stamps as one blurred output = `blurSamples`
  synths, so the published synth time stays *per sample* and the pacer's cost guard
  (`blurSamples x synth`) is right.
- **Logs**: the renderer logs under the `framegen` scope (ready / unavailable / UI protection
  warnings); the pacing glue logs under **`framegen_pacing`** (see
  [Logging](#logging)). They are two scopes because every `LogScope` registers a
  ConCommand of its own name and the console asserts on a duplicate -- two scopes called
  `framegen` aborted the binary in static init.

## HDR and 10-bit games

(2026-10-04, library PR #6.) The renderer no longer refuses HDR or 10-bit content: the library's
`Format` is chosen per game layer, once at `init()`, from the layer 0 texture's `VkFormat` and
`GamescopeAppTextureColorspace`, by the pure helper `ClassifyLayer()` in
`src/FrameGen/FrameGenFormat.h` (pinned by `tests/test_framegen_format.cpp`).

| Layer 0 texture | Colourspace tag | Library `Format` | `Transfer` | Ring / outputs |
| --- | --- | --- | --- | --- |
| `B8G8R8A8` / `R8G8B8A8` UNORM | any but pass-through | `Rgba8` (as before, byte-identical) | - | `B8G8R8A8_UNORM` (4 B/px) |
| `A2B10G10R10` / `A2R10G10B10` UNORM | SRGB (SDR 10-bit) or HDR10_PQ | `Rgb10` | - (codes are display-encoded, PQ included) | `A2B10G10R10_UNORM_PACK32` (4 B/px) |
| `R16G16B16A16_SFLOAT` | SCRGB | `RgbaF16` | `Linear`, `linearWhiteNits` 80 | `R16G16B16A16_SFLOAT` (8 B/px) |
| `R16G16B16A16_SFLOAT` | SRGB / LINEAR / HDR10_PQ | `RgbaF16` | `Encoded` | same |
| YCbCr (NV12 ...) | - | refused: `YCbCr` | | |
| pass-through tagged | - | refused: `Hdr` | | |
| anything else (565, 16-bit UNORM) | - | refused: `Format` | | |

- **The ring carries the game's own bits.** `cs_fg_copy.comp` is compiled three times
  (`DST_FORMAT` = `rgba8` / `rgb10_a2` / `rgba16f`, a macro in `shaders/descriptor_set.h` that
  expands to the old `rgba8` for every other shader) and `SHADER_TYPE_FG_COPY{,_RGB10,_F16}` is
  picked from the plan. It stays a texel-exact copy of the logical rgba, so channel order, an
  fp16 value above 1.0 and a negative scRGB value all survive. The substituted layer 0 keeps the
  game's colourspace tag (`SubstituteLayer0` replaces only the texture), so the compositor decodes
  the generated frame exactly like a real one: PQ / scRGB reach the output colour management
  unchanged. `Why` the ring is always `A2B10G10R10` for 10-bit, whatever the game's order: storage
  on `A2R10G10B10` is optional (NVIDIA lacks it) and the copy samples logical rgba anyway.
- **Transfer.** Only the library's *measurement* depends on it (mismatch, stillness, luma), never
  the warp or blend, which run in the picture's own space. scRGB is linear, 1.0 = 80 nits
  (`colorimetry.h`'s `c_scRGBLightScale`, not gamescope's SDR-on-HDR brightness: that one is
  applied by the output colour management after our substitution). Thresholds stay "8-bit levels"
  in every format, so every preset (Artifact safety, UI protection ...) applies unchanged.
- **Device feature.** `rgb10_a2` stores need `shaderStorageImageExtendedFormats`. gamescope now
  reads it from the physical device (`CVulkanDevice::supportsStorageImageExtendedFormats()`) and
  enables it only where offered; the 10-bit copy shader module and pipeline exist only then.
  `FrameGenHost.cpp`'s `DeviceCaps()` (once, logged as `frame generation formats: ...`) also
  checks sampled + linear filter + storage on the ring format; fp16 needs no extra feature.
  Without it, 10-bit games stay pass-through with the status "HDR / 10-bit format not supported
  by this GPU" (`Unavailable::HdrFormat`; an init failure of a 10-bit / fp16 plan reports the same,
  the library's `initError()` is in the log).
- **Re-init on a format change.** `resize()` cannot change the `Format`, so a changed plan (8-bit <->
  10-bit <-> fp16, or scRGB <-> encoded fp16) does `Teardown()` (a `waitIdle`, like every other free)
  and a fresh `init()`; the failure key includes the plan, so a 10-bit game that cannot start does
  not block the 8-bit game the user switches to. A change of the colourspace tag alone (SDR <->
  HDR10 on one 10-bit surface) just drops the ring's frames. Focus flapping between games of
  different formats therefore costs one pipeline build per switch (not per frame).
- **HDR output with SDR content is not special**: `g_bOutputHDREnabled` no longer forces
  pass-through; the layer is a plain 8-bit one and runs as always.
- **Memory** scales with bytes per pixel: ring slot and clean copy are 4 B/px at 8-bit and 10-bit,
  8 B/px at fp16 (29.5 MB per frame at 1440p, 66 MB at 4K). With a lag spike buffer the ring can grow
  to 65 frames; a grow that cannot allocate is logged and keeps the old depth (`FollowDepth`).
- **Not built: NV12 / P010.** Layer 0 is a YCbCr texture only for video players; supporting it
  needs the game buffer split into two plane views and a plane-aware copy and output, for a
  use (interpolating a video) the library's video path covers better. `isYcbcr()` stays refused.
- **Status.** The Frame generation status line appends the format when it is not plain 8-bit:
  ` · HDR`, ` · 10-bit` or ` · 16-bit float` (`RenderStatus::formatTag`).
- **Untested on real hardware at the time of writing**: the headless smoke covers 8-bit (unchanged);
  HDR / 10-bit need a real HDR game.

## UI protection

The **UI protection** row (`framegen.ui_protection`: Off / **Crosshair** (default) /
**Crosshair V2** / Whole screen) keeps a still crosshair, or with Whole screen every solid
still HUD element, pixel-exact on generated frames. It is **independent of Static HUD
protection**. Two more things sit with it: [Crosshair V2](#crosshair-v2-and-the-box-size)
and the **Crosshair box size** slider, with a [live preview](#the-box-preview) in the Inspector.

**It lives in the FrameGen library, not in this repository.** The library work is
[Ritze03/frame-gen-ritz PR #1](https://github.com/Ritze03/frame-gen-ritz/pull/1) (branch
`ui-protection`, merged; pinned here at FrameGen main `b699301`, which includes PR #1, UI protection); the host only maps the setting and feeds frames
in. The first version of this feature was host code (three shaders of our own, ROI-sized
textures per ring slot). `Why` it moved: the user, on where it belongs: *"It should really
be part of the library ... especially since the repository will be public in the end ... so
if anyone actually decides to use it, he can just implement it easily."* A host-side
crosshair hack that every user of the library would have to rewrite is the opposite of that.
The old `cs_fg_crosshair_*.comp` / `cs_fg_inpaint.comp` shaders, their `SHADER_TYPE_FG_*`
registrations, `CVulkanCmdBuffer::uploadConstantsRaw` and the per-slot patch textures were
deleted; the library's six shaders (`ui_detect`, `ui_box_update`, `ui_box_mask`, `ui_mask`,
`ui_inpaint`, `ui_patch`, plus the include `ui_common.glsl`) are compiled by
`src/meson.build` like its other eleven.

`Why` a mask beats Static HUD protection's `hudBonus`: `hudBonus` only biases the block
motion search towards a zero vector, and a static crosshair shares its block with the
moving background behind it, so the block still gets one vector (the background's drags the
crosshair, or the crosshair's freezes the background) and the output is an interpolation,
never exact. The mask instead *removes* the still pixels from what the estimate and warp
see (the library's clean copy has them inpainted from their surroundings) and pastes the
**exact real pixels** back over each generated frame. That is why the old text of the
Static HUD row ("keeps a still crosshair sharp") was wrong, and the row now only describes
the soft bias, which still matters for see-through HUD the mask cannot cover.

The user's original request (after testing in a real game, "looks REALLY good"): *"One
thing that many frame gens get wrong is the crosshair [...] We won't optimize for like weird
big crosshairs, but like the middle one to maybe three percent of the screen [...] It should
analyze multiple frames and can easily tell if the crosshair isn't moving. And then it should
detect it, that it is the crosshair. Then the image that is being used to generate frames
should have this crosshair removed and at a later point patched on again. This of course
doesn't work as well if there's transparency, but it should work for most games."*

**What the library does** (full contract in `subprojects/FrameGen/gpu/framegen.h` and
`INTEGRATION.md`): a pixel is *still* if no channel moved more than `uiStillThreshold`
(3.5 levels) between two consecutive real frames, and is protected after `uiStillFrames` (8)
consecutive still frames. Crosshair mode watches a centred square of side
`round(H * uiBoxHeightFrac)` (default 0.025, i.e. 36 px at 2560x1440; until 2026-10-05 it was
`round(sqrt(0.03 * w * h))`, 332 px there -- see [the box size](#crosshair-v2-and-the-box-size));
Whole screen watches every pixel in cells and protects the still ones, giving up on a frame
where more than `uiMaxStillFraction` (0.75) of it is still (a paused game, a loading
screen). Per observed frame the library keeps its own **clean copy** with the protected
pixels inpainted (reach `uiInpaintReach` 12 px); estimate and synth run on the clean copies,
and every synth ends with `out = mix(out, real pixel, mask)`, reading the real pixels from
the `curr` view it is given. All of these are runtime `Settings` fields; the host sets only
`uiProtection` and `uiBoxHeightFrac` (the slider) and leaves the rest at the library's defaults.

**Host wiring** (`FrameGenHost.cpp`):

- `recordObserve(cb, ringSlotView)` is called **once per new real frame**, in the command
  buffer that copied it into the ring, right after the ring copy and its barrier, **also
  for frames that are only passed through** (the stillness counters compare consecutive real
  frames; a gap costs only the exact "8 in a row"). While the pacer is **inert** the renderer
  tracks instead (`recordTrack`, see below). Off: the call is skipped entirely.
- The **ring holds the real, un-inpainted frame**: `recordSynth` must get the real `curr`
  to paste from. The library keeps `uiHistory` (default `kUiFrames` = 3) clean copies, LRU by
  view; the ring re-observes exactly one slot per new frame and `uiHistory` follows the ring's
  depth, so the library holds exactly the ring's frames and a pair whose frames are both in the ring is always protected. A pair
  with an unobserved view (protection just switched on) simply runs unprotected.
- If `recordSynth` returns false with UI protection on (an observed frame of the pair was
  evicted or re-observed after the estimate, or a descriptor ring is full), the real frame
  is shown for that slot and the estimate is dropped: the same path as a failed record but
  **without** the "A generation step failed" status reason and without disabling FG. It
  logs once under `framegen`. It should not occur with the 3-slot ring: the pacer never
  needs a frame that has left the ring (`bEstimateValid` is cleared the moment the pair's
  frames are overwritten, before the next observe).
- `setSettings()` is a runtime change (no GPU wait): `ToLibrarySettings()` maps the config
  to `uiProtection`, and the packed config word carries it at bits 24-25 so a change goes
  through the same applied-preset check as Quality/Artifact safety/Static HUD protection.
  If the library refuses (`false`), the old settings stay and it logs once.
- The library has a **third descriptor ring**, `kUiSets = 64` (separate from the two of
  32). Per composite the host records one observe (1 set Crosshair, 2 Whole screen) plus
  one patch set per synth, far below it (`kMaxSynthsPerPair` is 24).

**Tracking while passing real frames through (the inert state, 2026-10-04).** When the
pacer is inert (the game reaches the refresh / target with Pause at refresh rate on, the
cost guard, ...) the renderer does no ring copy and no observe, so the library's stillness
counters used to stop at the last frame before the stretch. They survive that gap, but then
compare the frame before it with the one after, so UI that **changed meanwhile** (a
recoloured crosshair, a scope, a HUD update) restarted cold at generation's resume and could
shift for `uiStillFrames` real frames. The user: *"make sure that it still keeps track of
like static UI elements, like the crosshair, even when it's temporarily deactivated because
the FPS is high enough ... so it doesn't start shifting around once it starts generating
frames again."* `RecordBaseLayer()`'s inert branch now calls `TrackWhileInert()`: **one
`recordTrack()` (library PR #9) per NEW real frame**, in a command buffer of its own that is
built and submitted only for such a frame (never an empty one per composite). Rules, pinned
by the pure `ShouldTrackUi()` / `LayerFitsHost()` (`FrameGenFormat.h`, `tests/test_framegen_format.cpp`):
UI protection on; the Interpolator live; the layer still the format plan / size / colourspace
it was created for (the D4 gates: an unsupported layer is not tracked); and a commit id the
library was not already handed (`Host_t::ulUiFedId`, set by an observe **and** a track), so
a repaint of the same frame (cursor, overlay) or the frame observed on the way into
pass-through is never counted twice. A frame is tracked **or** observed, never both; on
resume the next real frames go through `recordObserve` as always (ring copy and clean copy
return), and the first pair is (first observed frame, the one after it). The game's own texture is
sampled directly (its raw UNORM view, GENERAL layout after `prepareSrcImage` +
`insertBarrier`): no ring copy, and the values equal what the ring would hold (the copy is
exact; the library reads rgb only). **Cost: ~0.003 ms of GPU in Crosshair mode, ~0.02-0.03
ms in Whole screen**, plus one small command buffer per real frame on the CPU, only while
inert. `Why` no lazy Interpolator init for tracking: pacing drives the renderer (and so
`EnsureReady()`) through its warm-up before it can ever say "game too fast", so a not-live
host while inert means generation never started and there is nothing to keep warm; an init
only for tracking would buy a pipeline-build stall. `Why` nothing else had to change: the
audit of what could reset the counters found only legitimate triggers: `DropFrames()`
forgets the host's ring and cached outputs, never the library's state; the inert branch
returns before `EnsureReady()`, so no `resize()` / `reconfigure()` / `setSettings()` runs
during the stretch; at resume `setSettings()` (UI-history follow, `FollowDepth`, and the
preset change at `EnsureReady`) only zeroes the box counters when UI protection itself goes
Off or comes On (`uiAdopted()`), and `Teardown()` (the format class / FG-off) is a true
reset by nature. Known edges: a UI protection **change made while inert** (Off -> On, a
different mode) is only applied at resume (`EnsureReady` is not run), so that stretch tracks
with the old setting and the counters start cold at the change, as before; a resume on the
very frame last tracked observes it again (one still-step of over-count, harmless).
The debug log (`framegen` scope) counts tracked frames ("UI protection: tracking while
passing through") and notes the resume ("observing again after N tracked real frames").

**Timing.** The library adds `ui_detect` and `ui_inpaint` (per observe) and `ui_patch`
(per synth) to its profile, and an observe opens a profile that the next estimate
continues. `HarvestProfile()` therefore classifies stamps by name: the observe's two
passes are added to the **estimate's** time (per real frame, like the estimate) and
`ui_patch` to each **synth's**, so the cost guard (and the debug log's FG time) include
them; the UI share alone is published as `RenderStatus::lastUiMs` (one observe plus one
patch) (the Status line no longer shows either figure). Idle gaps are the hazard: a delta whose previous stamp was recorded in an
earlier command buffer contains up to a frame of nothing, so it is dropped (a `ui_detect`
that does not directly follow the profile's start stamp; `luma` behind an observe unless the
estimate was recorded in the observe's own command buffer; the first stamp of a synth that
does not directly follow the estimate) -- an under-count of 0.01-0.05 ms in place of a
16 ms over-count. The host reads the previous profile **before** recording an observe (it
resets the query pool), never in the same composite after it (that would wait on a
submission that does not exist yet).

**Cost** (library figures, 7900 XTX, 1440p, moving scene, marginal over Off): Crosshair
+0.03 ms, Whole screen +0.05 ms; the worst case (Whole screen, 70% still) about 0.25 ms in
total. **Memory**: three clean copies of the frame (44 MB at 1440p) in both modes, about
15 MB more for Whole screen. The memory stays with the library until the interpolator is
destroyed or resized (switching the row Off stops the work, not the allocation).

**BGRA validation warning (known, upstream).** The ring and every output are
`B8G8R8A8_UNORM`; the library's `ui_detect.comp`, `ui_inpaint.comp` and `ui_patch.comp` declare
their storage images as `rgba8` on those B8G8R8A8 views (emitted only while UI protection
is on), which the validation layer reports as
`Undefined-Value-StorageImage-FormatMismatch-ImageView`. It works on RADV (gamescope itself
does the same on BGRA views); it is not fixed here because the library is not edited from
this repository. Expect that line in a validation-layer run.

**Limits**: UI that is see-through (its pixels change with the background, so it is never
still), UI that changes (hit markers, an animated ammo counter, a spreading crosshair),
off-centre reticles in Crosshair mode. A still camera masks the background too, which is
harmless (restored with identical pixels) and leaves the mask the moment anything moves.
With Whole screen on, **Static HUD protection can usually be lowered**: the solid HUD no
longer needs the bias. **Off** for racing games or anything without a fixed HUD (the
detector would spend its time on nothing, or on a still dashboard it should not freeze).
The fork's own Crosshair (`system.crosshair`) is drawn after FG and unaffected either way.

### Crosshair V2 and the box size

**Crosshair V2** (`ui_protection = "crosshair_v2"`, library PR #14, `UiProtection::CrosshairV2`)
is a second crosshair mode next to the original, which stays selectable as **Crosshair** so the
two can be compared in the GUI. The user, on why both: *"Then do a V2 for now (in the GUI, so i
can compare them). The V1 (current) does result in artifacting."* V1 hides the crosshair by
inpainting it and pastes it back over a clean copy, which on a moving background leaves white
specks and moving patterns inside the box. V2 has **no inpaint and no clean copy**: a pixel is
protected only if it stays still **and** stands out from what is behind it
(`uiContrastThreshold` 20), it leaves the mask only after `uiLeaveFrames` (4) consecutive changed
frames and re-enters after `uiReentryFrames` (2), and a protected pixel is pasted back only if it did
not change in the pair being synthesised -- it never pastes over moving content. Outside the pasted
pixels its output is bit-identical to Off. `uiEstimateFill` (hide the UI from the motion estimate)
stays **false**: it measured as a wash. The host does nothing different for it: `recordObserve` /
`recordTrack` are called exactly as for V1 (V2's track is a full detect pass, 0.02 ms). Switching
between V2 and another mode forgets the observed frames, so one pair goes unprotected; harmless.
`Why` the numbers (library README, footage of 12 segments / 1173 frames): artifact pixels per frame
(worse than Off by more than 24 levels) **V2 0.012**, V1 4.44 at a 27 px box and **20.9 at the old
249 px box**; white specks 0.009 against 1.36; mask flips per frame 2.0 against 16.7; recall of the
crosshair after 8 frames 0.977 against 0.917; V2's observe costs 0.022 ms at 1440p against V1's
0.066 ms. Limits: opaque, static crosshairs only -- a semi-transparent one or one that changes shape
stays unprotected while it changes, by design; a *thick* crosshair on a hard-edged moving background
interpolates better around it with V1 (which inpaints it out of the estimate). Whole screen stays
V1-style (no V2 variant exists yet). The enum value order is the library's (Off 0, Crosshair 1,
Whole screen 2, CrosshairV2 3) and `fghost::UiProt` matches it, so `PanelFrameGen.cpp`'s `kUiKeys`
is indexed by it; the GUI's *display* order (Off / Crosshair / Crosshair V2 / Whole screen) is only
the order of `kUiOptions`. Pinned: `tests/test_framegen_format.cpp` asserts the values.

**Crosshair box size** (`framegen.ui_box_height`, a float **percent of the game's height**,
default 2.5, 0.5-10, step 0.1; row text `2.5% · 24 px`, the pixel figure being the library's own
`Interpolator::boxSize()` for the game's frame). The side is `clamp(round(H * pct), 8, min(W, H))`:
24 px at 960 rows, 27 at 1080, 36 at 1440, 54 at 2160. It applies to Crosshair, Crosshair V2 and the
box part of Whole screen. `Why` a percentage of the **height**, not of the area: the old default
was 3% of the frame's *area*, which is 192 px on the user's 1280x960 CS2 (249 px at 1080p, 332 at
1440p) -- far more than a crosshair, and everything in the box beyond it is background that gets
pasted from the real frame, which is where V1's specks came from (20.9 artifact pixels per frame
against 4.44 at 27 px). A height fraction is also what a human can judge ("a bit bigger than the
crosshair") and keeps the box square and independent of the aspect ratio. The smaller default also
removes most of V1's specks on its own. `Why` a slider and not a fixed number: crosshairs differ
(a dot, a thin cross, a big ring), and the preview below lets the user size it exactly. Per profile, like the other rows; the host takes it in **tenths of a percent** in its
own atomic word (`g_uUiBox`; the main config word is full), and a change is a runtime `setSettings`,
no wait. A game whose frame size changes needs nothing: the library recomputes the box from the
fraction.

### The box preview

The two UI-protection rows (`framegen.ui_protection`, `framegen.ui_box_height`) declare
`.Preview( Entry::PreviewKind::UiBox )`, so while either is the selected row the Shell's
**Inspector** shows, above VALUES, the game's pixels under the box: a crop of the frame's centre
(the box plus its own size again around it, at least 48 px), enlarged by a whole number with
**nearest** replication so each game pixel is a crisp block, everything outside the box dimmed,
the box outlined (a dark halo under an accent line), and `Box 24 × 24 px` / `Game frame 1280 × 960`
under it. The user's request: *"a slider, so the user can adjust it for himself, and then in the
right inspector rail we can show an image of the area, basically, that's underneath the current UI,
so he can see if the whole crosshair fits in, and then the user can configure it, like, really
precise."* It follows the **selected** row like the Adaptive Brightness strip does (a click on the row
selects it); there is no hover-only preview.

**How the picture gets its pixels (no GPU wait, no new threads):**

- `fghost::BoxPreviewWanted()` is called by `UiBoxPreview_Draw()` every frame the block is on screen;
  it lapses by itself 400 ms after the last call, so closing the Inspector, selecting another row or
  closing the overlay stops all capture work with no hook. `Why` a lapse and not a visibility flag:
  the block has more ways to leave the screen than the overlay has of closing (the same argument
  `EffectPreview.cpp` makes for its strip).
- While wanted, the renderer records the crop **into the command buffer it already submits for a new
  real frame** (`BoxPreviewRecord()` in `FrameGenHost.cpp`, from the ring-copy branch and from
  `TrackWhileInert()` for the pass-through case): one `cs_fg_crop.comp` dispatch (at most 128 x 128
  threads) on **layer 0 itself**, i.e. the real game frame before any effect, the same pixels the
  library protects, then a copy into a host-mappable 128 x 128 staging image. At most 10 captures a
  second and one in flight. Cost: a dispatch of a few thousand threads plus a 64 KB copy per capture,
  only while the preview is on screen.
- The pixels are read back on a **later composite**, once `g_device.completedSeqNo()` shows the
  submission has retired (`BoxPreviewPoll()`), never by waiting. This differs from the Adaptive
  Brightness strip (`vulkan_effects_preview_*`), which arms one composite and waits once; that
  mechanism lives inside the effects pre-pass, which does not run when no native effect is on, so it
  could not serve a row of this area. `cs_fg_crop.comp` is new (`SHADER_TYPE_FG_CROP`, precompiled).
- **Format handling:** the shader fetches the game's raw UNORM view like every layer-0 pass. 8-bit and
  10-bit values are already display-encoded and are only clamped; an fp16 **scRGB** (linear) game is
  sRGB-encoded for display, and anything above 1.0 clips; HDR10 (PQ) is shown as encoded, so it looks
  flat. A crop wider than 128 px (a big box on a high-resolution game) is reduced by a whole factor with
  a box mean, so it never aliases; the caption then says *(picture reduced)*. YCbCr / unsupported
  formats are refused by the renderer as they are for generation, so there is no picture.
- The overlay side (`src/Overlay/UiBoxPreview.{h,cpp}`) owns one fixed 192 x 192 `ImTextureData`
  registered with ImGui's context (`ImGui::RegisterUserTexture`, the same user-texture path
  `EffectPreview.cpp` uses, uploaded by the existing `ImGui_ImplVulkan` backend), enlarges the capture
  by the largest whole factor that fits and uploads only that sub-rectangle.
- **Placeholders:** with Frame generation, Motion blur and the Lag spike buffer all off the renderer is
  not running (the rows are disabled then anyway): *"Turn Frame generation on to see what is under
  the box."* With the renderer on but no capture in the last two seconds (no game frame, or the game
  is paused): *"Waiting for a game frame."* With UI protection **Off** the picture still shows, and
  the caption says protection is off. The box is the centred one (`uiBoxOffsetX/Y` stay 0).

The Registry got one new generic hook for this: `Entry::ValueText( fn )`, a value-text override for a
row whose number alone says too little (used by the box-size row for `2.5% · 24 px`); it replaces
`FormatDeclValue()`'s "value + unit" in every host that draws the value (sheet row, Inspector row),
so they cannot disagree.

## Backends

Generated frames must go through the composite, so full composite is forced while FG is on
(or still holds resources, see above). DRM also disables partial composite (and therefore
direct scanout): a scanned-out buffer never passes `vulkan_composite()`, and a partial
composite has already dropped the base layer. Wayland and OpenVR force full composite; SDL
always composites. See the backend pages.

## Pacing (frame-gen-ritz `gpu/pacing.h`, `framegen::pacing::Pacer`)

**The pacer is the library's, not gamescope's** (2026-10-04, frame-gen-ritz `2eca330`,
its PR #3). It used to be `src/FrameGen/Pacing.h` here, with its tests in
`tests/test_framegen_pacing.cpp`; both are deleted, and the library's own `pacingtest`
holds the full ported suite. The authoritative description of the model -- content time,
the two priorities, pass-through, the cost guard, the beyond-refresh cadence, how the
three features compose -- is the header comment of
`subprojects/FrameGen/gpu/pacing.h` and the "Pacing" / "How the features combine"
sections of `subprojects/FrameGen/INTEGRATION.md`. **This page does not restate it**;
it keeps the *why* of the user's asks and what gamescope's side does.

`Why` it moved, the user's architecture rule: *"you're basically only building the GUI in
this chat and most of the stuff should go into the frame gen itself"* -- so gamescope
keeps GUI and platform glue. The second reason is composition: *"all of them are kind of
separate, but they can all be turned on at the same time, and they actually act nicely
together"* (frame generation, motion blur and the future lag-spike buffer). One planner in
the library owns all three dimensions of the content timeline (buffer = how far behind,
frame generation = which instants, blur = the shutter window), so gamescope has no feature
logic of its own to keep consistent. See [motion-blur](motion-blur.md).

**What gamescope does** (`src/steamcompmgr.cpp`, the "Frame generation / motion blur pacing"
block; vocabulary per the library: `presentNs`, `FrameFormat`, `OnPaint( in, newestId,
bPresentTick, bSkippable )`):

- **Arrival** (`FrameGen_OnArrival`, from `handle_done_commit()`): `pacer.OnArrival( commitID,
  commit_t::present_time, window seq, FrameFormat{ w, h, drmFormat } )`. The arrival time is
  the commit's acquire fence signalling (stamped by `commit_t::Signal()`), **not** the vblank
  that latches it. `Why:` latch times are vblank-quantised (about +/-7 ms for 60 fps on a
  144 Hz display), which would wreck the interval estimate.
- **Paint** (`FrameGen_PrePaint`, right before `paint_all()`): runs the pacer whenever **any**
  of frame generation, motion blur or the lag spike buffer is on (`fghost::Active()`), with
  `Inputs::frameGen` = the Frame generation switch, `blur*` = the Motion blur config,
  `lagBuffer` = the Lag spike buffer config (each its own switch: the three combine, see
  [lag-spike-buffer](lag-spike-buffer.md)), `extraDelayNs` = 0 (a fixed manual delay, unused). `presentNs` (V) is the vblank
  timer's predicted target for this paint (`g_SteamCompMgrVBlankTime.schedule.ulTargetVBlank`,
  never in the past; a missing or implausible value, or a non-vblank paint, falls back to
  `now`; an extra-timer tick uses its own target).
- **Apply** (`fghost::SetFrame` -> `RecordBaseLayer`): the **single renderer rule** -- the
  `Decision`'s `prevId != 0` means `recordSynthBlur( prev, curr, out, t0, t1 )` (a plain synth
  when `t0 == t1`; with blur on a *real* frame's output also carries a pair, `t = 1`, and a
  window); `prevId == 0` means no renderer work, show the real frame `showId` (the newest
  unless a buffer delays, then an older ring frame is substituted). gamescope does not
  interpret `t`.
- **Status** (`TakeStatus` -> `fghost::PacingStatus`): the library's `Report`, each feature
  separately (`fgActive`, `blurActive`, `blurWindowMs`, `blurSamples`, `bufferDelayMs`, `bufferTargetMs`, `lastSpikeMs`, `spikesInWindow`, `outliersIgnored`, `historyFrames`),
  feeds each tab's own status line.
- **Reset** (`Pacer::Discontinuity()` / `Decision::reset` -> `fghost::Reset()`): no FG for the
  next frame, the previous real frames are forgotten, on a focus/window change, size/format
  change, a gap over 100 ms, fades, Steam UI windows and streaming clients. `Why:` frames are
  keyed by commit id, not texture pointer, because gamescope reuses one texture per
  `wl_buffer` -- pointer equality misses re-commits, and without a reset two unrelated images
  would be blended.
- **Main-loop gate**: `FrameGen_TimerPaced()` = `fghost::Active() && pacer.Planning()` (not
  `Generating()`, which is false in **real-rate** mode -- frame generation off, or the game
  already at the output rate with blur on -- where VRR arrival paints would otherwise leak
  through); it is also `PaintTick()`'s first argument.
- **Cadence-skip vblanks do not paint.** When the output interval is longer than a vblank and
  no new output is due the pacer says `Decision::skip` (only if nothing but pacing asked for
  the paint): `FrameGen_PrePaint()` returns true, the loop does not call `paint_all()` and
  nothing is committed; the host keeps the last buffer. `Why`: re-presenting the cached output
  on every such vblank made a 30 fps game at 2x commit 83 times a second instead of 60 (host
  and GPU work for an identical image).

**The user's spec for the dynamic mode and the two priorities** (the origin of the
pacer's shape, kept as rationale):

> *"Also add something like a dynamic mode that lets me set a target FPS, and then it
> should either set the multiplier dynamically and let the system handle the rest. So, for
> example, I drop below 120 FPS, and I have a 2x multiplier. Right now, because my frame
> target is 240, I drop to, for example, 100 FPS, then it should just switch to 3x
> multiplier on the fly. It should do that based on the frame times, so it reacts quickly,
> not like full second accuracy, but per frame accuracy. There should be two modes, one for
> low latency and one for maximum smoothness, basically, so the user can take the path that
> he wants."*

*"Shouldn't it only turn off when my raw FPS is equal to my refresh rate and not at 60% of
my refresh rate?"* -- yes (generation stops only once the game reaches the output rate, with a
ratio band). *"Also let me change the multiplier to up to 8x."* *"It's fine if we generate way
too many frames on a fixed multiplier, this system should handle that as I told you earlier."*

**Cost guard, and its switch (2026-10-04, `framegen.gpu_limit`, row "Limit to GPU speed").**
The guard's mechanics are the library's (`Inputs::costGuard`; the budget is the estimate plus
every synth, a blurred output counting as `blurSamples` of them -- the renderer's published
"synth" time is per *sample* for that reason). The user: *"I should be able to disable the
'limit to keep up with GPU' feature."* Off sets `Inputs::costGuard = false` (packed config bit
27, `fghost::Config::gpuLimit`): a cap or pass-through lifts at once and it never probes; the
renderer still measures, so turning it back on caps immediately and the status / debug log
keep their numbers. `Why:` the guard trades the chosen output rate for the game's own frame
rate; someone who wants the rate regardless (and accepts a slower game) needs a way to say so.
**Default off:** the user, on the first cut (default on): *"turn it off by default. I personally
believe, that it is useless/defeats the whole purpose"*; so a fresh or older profile never caps
or passes through for cost unless the switch is turned on. The user's fuller reasoning: *"it's
fine if it takes up more GPU time. So it actually decreases the real FPS because it's supposed
to just make the whole experience move and not switch back and forth. ... For example, if I
have 120 FPS and it turns off, that looks substantially worse than, for example, having 90 FPS
and it being generated up to 280."* In short: a steady generated output beats protecting the
game's own frame rate, because toggling generation on and off looks worse than a lower real
rate that is always generated. (The library's `Inputs::costGuard` itself defaults to true.)

### VRR and tearing: timer-paced while generating

While FG is **generating**, gamescope paces its own output frames on the **vblank timer**,
exactly as on a fixed-refresh display, even with VRR or tearing enabled -- the main loop's
`PaintTick()` (`pacing.h`) lets only timer ticks paint, and the flip is a normal one;
otherwise the loop would paint on every commit arrival (VRR treats every iteration as a
vblank, a tearing surface paints at once) and the output frames would go out bunched at
arrival times. VRR/tearing stay **enabled on the display** (D2: FG never forces VRR off);
only OUR paint cadence follows the timer. While FG **passes real frames through** (the game
already reaches the output rate, HDR, the cost guard) VRR/tearing behave exactly as without
FG: immediate presents, no timer gating.

`Why` (the user's real CS2 test): the profile has `vrr_enabled: true` and `tearing_enabled:
true`, nested Wayland in Hyprland, the game uncapped at 125-218 fps. An earlier version
passed through for the whole session under VRR/tearing, and the user reported *"it doesn't
feel like it's doing anything"*, having said before that it "looks REALLY good". An
uncapped 200 fps game on a 240 Hz panel with target = refresh has ratio 1.2 and now
generates; it passes through only at game fps >= the refresh.

There is no `VariableRefresh` pass reason any more -- nothing passes through for VRR. If the
timer ever stopped ticking under VRR, generation would stall; it does not: the main loop
re-arms it after every tick (`ArmNextVBlank( true )` at the end of a timer iteration).

### Latency: Low latency, the wake-up lead and tearing (2026-10-04)

The user's complaint: input latency got *worse* with frame generation on, even with blur and
the lag buffer off (nested in Hyprland, ~280 Hz, CS2 at 123-208 fps, target = refresh: Low
latency added 7.9-9.2 ms, no better than Smoothness). Three causes, one fix each:

1. **The library's Low latency margin** (frame-gen-ritz PR #8, `ec18261`): `D = max(0, i - o) +
   margin`, `margin = min(p75, 1 ms, 20% of i)` (0 for a steady game), with **no paint-lead
   term and no floor of one `o`**. At 120 / 160 / 210 fps on 280 Hz `D` is about 5.5 / 3.5 /
   1.9 ms. `Why` it is that tight: `D` now sits right at the point where the next real frame is
   expected, so a frame landing a little late leaves that tick with nothing new (one repeated
   tick; the library measured 97-99% distinct ticks at +-1 ms jitter). The user prefers that to
   latency. Details: the library's `INTEGRATION.md`, "Pacing".
2. **gamescope's wake-up lead** (`src/vblankmanager.cpp`, `CVBlankTimer::CalcNextWakeupTime()`).
   The timer wakes `offset` before the vblank it paints for, `offset = rollingMaxDrawTime +
   redZone`. Upstream seeds the rolling draw time at 3 ms, floors it at 2.4 ms while compositing
   (`m_ulVBlankDrawTimeMinCompositing`, a GPU-clock feedback-loop guard for the Steam Deck) and
   adds a 1.65 ms red zone (internal screen types, which includes nested Wayland), so a
   compositing frame **never** gets under about 4.05 ms whatever drawing really costs (measured
   `vblank_debug`: `rollingMaxDrawTime 2.40 ms + redZone 1.65 ms = offset 4.05 ms`; at 280 Hz
   that is more than a whole 3.57 ms refresh). The lead is latency the library cannot see: a
   real frame landing inside it is shown one refresh late, and `now` is that much earlier than
   `V` (`Inputs::presentNs`), so with a long lead Low latency fits no generated frames between
   real ones (the PR #8 author: 240 of 560 ticks at 120 fps with a 4 ms lead). **Now** (first only while any
   of frame generation / motion blur / the lag spike buffer was on; **since 2026-10-04 for all
   play**, controlled by the Display > General **Low-latency wake-up** setting, default on, see
   [resolution-and-refresh.md](resolution-and-refresh.md#low-latency-wake-up-displaylow_latency_wakeup)),
   `offset = min(max(max of the last 60 draw times, 0.75 ms) + 0.75 ms, one refresh)` (`kDrawTimeWindow`,
   `kMeasuredLeadMargin`, `CVBlankTimer::MeasuredLead()`): the draw time
   is wake-up -> present as the backends already report it through `UpdateLastDrawTime()`, so
   the FG work is inside it; a rolling **max**, not an average, so one slow draw widens the lead
   at once and it narrows only after 60 clean ones. VRR keeps its own small lead (0.3 ms red
   zone). ConVar `vblank_measured_lead` (default on; the setting mirrors into it) turns it off, and
   **with the setting Off the upstream heuristic runs always, even with frame generation on**
   (the user gets upstream's cautious timing and the generation headroom suffers, as
   measured below). `Why` it was first FG-only: the heuristic is deliberate upstream tuning;
   the user then approved the measured lead for all play, with the trade-off stated in the
   setting's help text. The
   effective lead is in the debug status line: `wake-up lead 1.89 ms (measured, max draw 1.14
   ms)` or `(default)`.
3. **Tearing was off for generated frames.** The main loop forced `FlipType::Normal` while
   `FrameGen_TimerPaced()` even with tearing on (the user's CS2 profile has it on). Now the paint
   is still *decided* on the timer tick, but when `bTearing` (`cv_tearing_enabled &&
   SupportsTearing() && the base commit wants async`, no overlay, no fade) it is *presented* as an
   async flip (`bFGTearPresent` in the main loop, passed to `paint_all()`). FG's own
   "rest of the pair" forced repaints tear too; a genuine forced repaint or a fade stays sync.
   Under VRR without tearing it stays Normal. **Per backend:** DRM: yes, as above (works from the
   code, **not tested on hardware**). `WasCompositing()` is deliberately not consulted: a
   generating FG paint is always a full composite (`RenderWanted()`), so honouring it would turn
   tearing off for good; `CDRMBackend::Present()` waits for the composite before it commits
   (`vulkan_wait()`), which is what the historic "if we are compositing, force sync flips" guard
   protects, and the output-timer branch already presents composites async. **Nested Wayland:
   works since 2026-10-04** -- `CWaylandBackend::SupportsTearing()` is true when the host offers
   `wp_tearing_control_manager_v1`, and `Present()` sends the `async`/`vsync` presentation hint
   (see [backend-wayland.md](backend-wayland.md#tearing)), so `bTearing` and `bFGTearPresent`
   hold there as on DRM and generated frames tear too, where the host allows it (Hyprland:
   `allow_tearing`, an `immediate` rule, fullscreen). The first version of this change left it
   out because it also changes the non-frame-generation path; it was added at the user's
   request with the same hint covering real frames. SDL / OpenVR / headless: `SupportsTearing()` is
   false or the flag unused.

**Measured** (headless sway, nested Wayland, `vkcube` capped by MangoHud, FG on, Target = display
refresh, Low latency; before = `6579889`, after = this change, library `ec18261`). Wake-up lead,
`delay` (D), outputs presented per second:

| Refresh / game | Lead before -> after | `delay` before -> after | Presented before -> after |
| --- | --- | --- | --- |
| 120 Hz / 30 fps | 4.05 -> 1.8-2.3 ms | 29.1 -> 25.1 ms | 117 -> 114 /s |
| 240 Hz / 30 fps | ~4.05 -> 1.7-1.8 ms | 33.3 -> 29.6-30.0 ms | 241 -> 238 /s |
| 280 Hz / 120 fps | 3.57 -> 1.9 ms | 8.4 -> 5.4 ms | 273 -> 253 /s |
| 280 Hz / 150 fps | 3.57 -> 1.9-2.1 ms | 8.9 -> 3.9-4.1 ms | 273 -> 210-231 /s |

(At 280 Hz upstream's heuristic is clamped to one refresh, 3.57 ms.) Low latency now trades
output rate for delay, as the library documents: some ticks repeat when a real frame lands late
(vkcube behind a headless sway has coarse arrival jitter, so these figures are not a CS2
prediction). The lead is what makes generation possible at all at 280 Hz: the same build with
`vblank_measured_lead 0` (lead 3.57 ms) presented 156 /s at 150 fps (1.04x, almost nothing
generated) against 210 /s with the measured lead. FG, blur and the lag buffer all off: output equals
the game rate (30.08 commits/s at a 30 fps game, identical before and after).


**Phase B** (a per-output timer that is not the vblank timer) exists now, as the extra-frame timer
of [Pause at refresh rate](#pause-at-refresh-rate-framegenpause_at_refresh): `tau = V - D`
generalised unchanged, `V` being that timer's target time.

### Pause at refresh rate (`framegen.pause_at_refresh`)

The user, after the pacing rewrite had fixed "it should only turn off when my raw FPS is
equal to my refresh rate": *"So instead of just taking that decision for the user, whether
it should stop generating frames if the input frame rate is already above its set refresh
rate, we can add a switch that allows the user to enable that feature like auto turn off
if... FPS higher than refresh or something like that, you'll find a better name for it,
then we don't have to make the decision the user can choose whether he wants like 800 FPS
or if he wants it to automatically limit itself if basically it's not doing anything."*
(Earlier: *"It's fine if we generate way too many frames on a fixed multiplier, this system
should handle that."*) Row **Pause at refresh rate**, per profile, default **On**, after
Priority.

The library now measures game rates up to about 1000 fps (the interval clamp is 1-50 ms); before this, a game above 250 fps read as 250, so Pause at refresh rate never paused it.

- **On (default)** -- everything above, unchanged: the output is capped at the refresh
  (fixed `o = max(i / N, 1 / refresh)`, target clamped to the refresh) and generation stops
  once the game alone reaches it (the ratio band).
- **Off** -- the refresh cap is gone. Fixed N: `o = i / N` (floor 0.5 ms), so an uncapped
  200 fps game at 4x makes about 800 output frames a second; Target: the target is not
  clamped (target 500 on 144 Hz = 500/s; 0 is still the refresh). Pass-through only when
  the game itself reaches the output rate (fixed N >= 2 makes that impossible; Target:
  game fps >= target), the renderer is unavailable, or the **cost guard** trips -- the guard
  is what actually bounds the output. `presentedFps` keeps counting output frames handed
  to present, so the HUD's *Count generated frames* shows ~800.
- `Why` a switch and not a rule: whether 800 frames a second is wanted depends on the
  display path (a tearing display or a desktop window shows only the newest, a plain
  display cannot show more than its refresh) and on how much GPU the user wants to spend;
  the pacer cannot know, the user can.

**Presenting between vblanks (the extra-frame timer).** The vblank timer ticks once per
refresh, so an output interval shorter than a vblank needs another wakeup. When
`Pacer::ExtraTimer()` is true (Off, generating, `o` below 98% of a vblank; back to the
vblank timer above 102%) `steamcompmgr.cpp` runs the output on `g_FrameGenExtraTimer`, a
`gamescope::CTimerFunction` (timerfd) registered on `g_SteamCompMgrWaiter` next to the
vblank timer:

- **Arming** (`FrameGen_ArmExtraTimer()`, end of every main-loop iteration): when the pair in
  progress needs its next output (`FrameGen_PostPaint()` sets `bExtraWantNext` instead of
  `force_repaint()`), or a new real frame / UI repaint / fade is pending, arm for
  `NextExtraTargetNs(lastV, now, o)` = the previous paint's V + `o`, never in the past (a late
  wakeup does not make up lost slots with a burst).
- **Firing**: the callback only disarms and records the target; the main loop picks it up
  (`FrameGen_TakeExtraTick()`), and `PaintTick()` lets **only** these ticks paint while it is
  active -- vblank-timer ticks keep running (presented events, cursor, re-arm) but do not paint,
  or output frames would also land at vblank times that are not the output cadence.
- **V** for the paint is the timer's target time (`Inputs::presentNs`), so `tau = V - D` is
  evenly spaced; the paint goes through the same `FrameGen_PrePaint` -> `SetFrame` ->
  `paint_all` path with `bPaintTick` true. `g_SteamCompMgrVBlankTime.ulWakeupTime` is set to the
  tick's wakeup so the backends' draw-time feedback measures this paint, not the time since the
  last vblank.
- **Disarm / hand back**: when the cap goes back on, the mode changes, generation stops or the
  cost guard raises `o` past a vblank, `FrameGen_ExtraPaced()` is false and the next iteration
  disarms; the vblank path then paints exactly as before. With the switch On, or FG off, no timer
  is ever armed.

**Where it can present faster than the refresh** (`FrameGen_CanExceedRefresh()`; no backend-type
accessor exists and `Backends/` is not ours to extend, so the Wayland backend is recognised by its
connector's name, `"Wayland"`):

| Backend | Predicate | Flip |
| --- | --- | --- |
| Nested Wayland | connector name `Wayland` | normal commit at once; the host shows the newest buffer and discards the rest (`Wayland_PresentationFeedback_Discarded`); `Present()` never blocks |
| DRM | `SupportsTearing()` and a tearing flip would be chosen for this paint (`cv_tearing_enabled`, the surface wants async, no overlay up) | `FlipType::Async` for the extra ticks |
| DRM without tearing | -- | capped at the refresh: Off behaves as On (a second atomic commit while a flip is pending fails with `EBUSY`) |
| SDL, OpenVR, headless | -- | capped at the refresh: Off behaves as On |

So on a real display without tearing it cannot go past the refresh; the row's help says so. The
DRM tearing path is written from the code and **not tested on hardware**. Caveat on Wayland: the
output image ring is 3 images and, faster than the host releases buffers, gamescope may render
into one the host still holds; this is the existing backend behaviour, only exercised more often.


## Settings and status

Area `image.framegen` ("Frame generation"), per profile (game profiles inherit), no
keybind, config section `framegen` (schema stays 5; additive).

| Key | Values | Meaning |
| --- | --- | --- |
| `framegen.enabled` | `false` (default) / `true` | the master switch, the row "Frame generation" (split out of `mode` 2026-10-04) |
| `framegen.mode` | `fixed` / `target` (default) | which kind of multiplier, the row "Multiplier": 2x..8x / Target fps. Never `off` any more |
| `framegen.multiplier` | 2..8 (default 2) | fixed mode's multiplier; normalised to 2..8 on load (below 2 -> 2, above 8 -> 8) |
| `framegen.target_fps` | 0, 30..1000 | target mode: 0 = the display's refresh; below 30 clamps to 30, above 1000 to 1000 |
| `framegen.priority` | `low_latency` / `smoothness` (default) | what pacing trades under jittery frame times |
| `framegen.pause_at_refresh` | `true` (default) / `false` | stop generating once the game reaches the refresh (on), or keep generating above it (off); additive, an older config loads `true` |
| `framegen.gpu_limit` | `false` (default) / `true` | the row "Limit to GPU speed": the cost guard (see "Cost guard" in the pacing section) on (lower the output rate, then pass through) or off (always generate at the chosen rate); additive, absent loads `false`, schema stays 5 |
| `framegen.quality` | `quality` / `performance` | Performance = flow scale 4, sub-pixel off: roughly 35-50% cheaper, can miss thin fast detail. Help quotes the measured GPU ms per game frame on an RX 7900 XTX (estimate + generated frames; Quality / Performance): 1080p 0.35 / 0.42 / 0.49 vs 0.19 / 0.26 / 0.31 at 2x / 3x / 4x, 1440p 0.50 / 0.62 / 0.71 vs 0.25 / 0.36 / 0.46; PSNR at 2x 29.15 vs 28.33 dB. Asked for 2026-10-04: *"actually show how much faster / slower quality to performance mode roughly is"* |
| `framegen.safety` | `off` (default) / `low` / `default` / `high` | trust ramp (16,56) / (12,40) / (8,28) in 8-bit levels on the 7x7-averaged mismatch of the two warped frames (below `trustLow` interpolated, above `trustHigh` the nearer real frame, blended between); the help states each level's range plus the 15% whole-frame and scene-cut fallbacks. `off` disables every fallback: trust (254,255), `globalFallback` 1.0, `sceneCutSad` 255 -- never the real frame, not on fast flicks or scene cuts (smoothest, visible smearing and blended cuts). The others keep the library's 0.15 / 30 |
| `framegen.hud_protection` | `off` / `normal` / `strong` (default) | zero-vector bonus 0 / 1 / 2.5: a soft "prefer still" bias for see-through HUD. It no longer gates anything else (until 2026-10-04 it also switched the crosshair protection on) |
| `framegen.ui_protection` | `off` / `crosshair` (default) / `crosshair_v2` / `whole_screen` | the library's [UI protection](#ui-protection); additive (schema stays 5), an older config loads `crosshair`, an unknown string keeps it (`crosshair_v2` added 2026-10-05) |
| `framegen.ui_box_height` | float percent of the game's height, `2.5` (default), clamped to 0.5-10 on load | the row "Crosshair box size" ([box size](#crosshair-v2-and-the-box-size)); additive (schema stays 5), absent loads 2.5; written rounded to 0.1 |

**Migration** (on load; save writes `enabled`, `mode`, `multiplier`, `target_fps` and never
`"off"` again): `mode: "off"` -> `enabled = false`, `mode = "fixed"`, the stored
multiplier kept (normalised to 2..8); `mode: "fixed"` / `"target"` with no `enabled` ->
`enabled = true` (a `fixed` with multiplier below 2 stays disabled, as it used to load as
off); no `mode` and no `enabled` (written before Target fps) derives from the old
`multiplier`: 0 -> disabled, 2..8 -> enabled + `fixed`, so an old 3x profile stays 3x. An
explicit `enabled` always wins. Unknown strings keep the default. Since the mode default
became `target` (below): a section with **neither** `mode` nor `multiplier` keeps the
compiled-in `target`; only a legacy section that carries a `multiplier` (with no or an
unknown `mode`) loads as `fixed`, so what an existing user configured is unchanged.

**Defaults are the user's tuned values (2026-10-04).** Why: the user tuned Frame generation,
Motion blur and Lag spike buffer by hand in their `Test` profile and chose those as the
out-of-the-box values; the master switches stay off (*"All of them are off by default, but
the settings themselves are correct."*). Changed from before: `mode` `fixed` -> `target`,
`priority` `low_latency` -> `smoothness`, `safety` `default` -> `off`, `hud_protection`
`normal` -> `strong`; everything else already matched. Kept in step in `ConfigSchema.h`,
`fghost::Config` (also `multiplier` 0 -> 2) and each row's `.Default()` in
`PanelFrameGen.cpp`; Motion blur and Lag spike buffer already matched in all three places.

Rows, in order: **Frame generation** (a switch, `framegen.enabled`; help *Generates extra
frames between the game's own frames. Adds delay, so it is not suited to twitch
shooters.*), **Multiplier** (2x / 3x / 4x / 5x / 6x / 7x / 8x / Target fps; it sets `mode`,
and `multiplier` for a fixed choice, and is keyed to `framegen.mode` -- the Shell's
overridden-dot follows one key per row; disabled while the switch is off), **Target fps**
(a slider, 0 shown as "Display refresh", disabled unless Multiplier is Target fps; a drag
into 1..29 snaps to 0 or 30), **Priority** (Low latency / Smoothness), **Pause at refresh
rate** (a switch; help: stops at the refresh rate or the Target fps, whichever is lower), Quality, Artifact safety (Off / Low / Default / High), Static HUD
protection, UI protection (Off / Crosshair / Crosshair V2 / Whole screen), Crosshair box size (a slider, `2.5% · 24 px`, disabled while UI protection is Off), Status. Multiplier and the rows after it are disabled while the switch is off. The rail summary reads
"off", "N×" or "target N". Priority help: *Low latency adds the least
delay, but motion can stutter when the game's frame times jitter. Smoothness spaces the
frames perfectly evenly and adds about one game frame of delay.*

`Why two rows:` the user, after testing the single combined choice: *"There should be a
single toggle to enable and disable framegen in general, and then below that should just
be the multiplier. It shouldn't be combined into one."* Internally `fghost::Config` gained
`bool enabled` (packed word bit 26, `Enabled()`/the gate follow it) and `Mode::Off` stays
in the enum only because `pacing.h`'s `Mode` mirrors it (`steamcompmgr.cpp` static_asserts
the two match); the config never produces it.

`Why:` the library's expert dials (searchPenalty, smoothBonus, sceneCutSad, globalFallback)
are deliberately not exposed -- only the library's defaults were visually reviewed, and raw
sliders would invite settings nobody has looked at.

**Status line** (always one line):

- generating, target: `60->280 · target 280 (×4.6) · +13 ms` (game fps -> presented fps,
  the target, the effective multiplier to one decimal, the delay in whole ms);
- generating, fixed: `60->120 · 2× · +8 ms`; when the display cannot show N x the game the
  effective multiplier follows: `60->280 · 8× (×4.6) · +13 ms`;
- the FG / UI GPU times are **not** on the line any more (`Facts` rows have no details pane);
  they remain in the `framegen` debug log and `RenderStatus::lastPairGpuMs` / `lastUiMs`;
- otherwise the reason (renderer's first, then pacing's), `Off`, or `Waiting for frames`.

It writes `->` and 1/2 in help text because the overlay font atlas is Latin-1 only (no
U+2192 or the vulgar-fraction glyphs); `·` and `×` are Latin-1 and render. `Why:` the line
was trimmed 2026-10-04 because the user found it overloaded: *"The Status line is pretty
overloaded at the moment. Reduce it to (example values): 60->280 * target 280 (x4.6) * +13ms"*.
**Status
semantics:** `presentedFps` counts output frames actually presented (generated frames plus
the real frames pacing decided to show) and **not** repaints of an already-shown output
(cursor, overlay) -- it used to be a paint count; `activeN` = the effective multiplier
rounded, at least 2 while generating and 0 when passing through, so "`activeN >= 2`" means
"generating" (the HUD's *FPS shown* Output / Both choices rely on that, via
`GetPacingStatus().presentedFps`); `chosenN` = the fixed N, 0 in target mode; `delayMs` = D.
A vblank that holds a real frame while the next one is still to come re-presents it and is
not counted, so under Low latency with a saturated display the distinct-frame rate is a
little below the refresh (Smoothness fills it).

### Logging

`framegen` (renderer) lines come from `FrameGenHost.cpp`; `framegen_pacing` (pacing/status)
lines from `steamcompmgr.cpp`:

- info, on every change of mode / multiplier / target / priority, of generating vs passing
  through, of the pass reason or of the renderer's reason: `frame generation: <mode>,
  <priority>, game .. fps, presented .. fps (x, generating|passing through), delay .. ms,
  pacing: "<reason>" (n), renderer: <text>`;
- the status lines also carry the motion blur part (`motion blur off` / `motion blur 4
  samples, 50% shown frame, gaussian, window 8.3 ms`) and are keyed on blur's settings and
  whether it is applied too;
- info on `frame generation: on, <mode>, <priority>, motion blur on|off` at start and
  `frame generation and motion blur: off` when both are off;
- debug, at most every 5 s while on: the same numbers (`frame generation status: ...`).

Not keyed on the rounded multiplier: a game at a fractional ratio would flap between two
values and spam the log.

## When it passes real frames through

| Situation | Result |
| --- | --- |
| the game's rate reaches the output rate (refresh or target) | real frames, renderer inert, no added latency |
| per-pair GPU time over 25% of the game interval | output rate lowered, then pass-through; retried every 10 s |
| YCbCr, a pixel format outside 8-bit / 10-bit / fp16 RGB, a 10-bit / fp16 format the GPU cannot do, under 16x16, init failure | real frame, status names the reason |
| focus/size/format change, gap over 100 ms, fade, Steam UI, streaming client | one frame without FG, then resumes after warm-up |

## Interaction with the Frame limiter

The Frame limiter caps the *game*; FG multiplies the capped rate. refresh/N is the natural
pairing (e.g. 144 Hz, limit 48, 3x), but is no longer required: a game running anywhere
below the refresh generates. FG never caps the game itself. See
[resolution-and-refresh](resolution-and-refresh.md).

## Costs

Library measurements on a 7900 XTX, ms of GPU per real pair:

| Resolution | 2x / 3x / 4x, defaults | 2x / 3x / 4x, Performance |
| --- | --- | --- |
| 1080p | 0.35 / 0.42 / 0.49 | 0.19 / 0.26 / 0.31 |
| 1440p | 0.50 / 0.62 / 0.71 | 0.25 / 0.36 / 0.46 |

(about 0.07-0.1 ms per extra synth.) About 12 MB VRAM at 1080p plus the frame ring (three
slots, about 8 MB each at 1080p) and up to three pooled outputs.

UI protection: see [its section](#ui-protection) (Crosshair +0.03 ms, Whole screen +0.05 ms
at 1440p; 3 clean copies, 44 MB at 1440p, +15 MB for Whole screen).

## Limitations

- YCbCr (video) layer 0 is not offered (NV12 / P010 are not built); everything RGB runs, SDR or HDR, see
  [HDR and 10-bit games](#hdr-and-10-bit-games).
- Compute on the same queue, no async: FG time adds to composite time.
- Adds delay (Low latency: `max(0, i - o)` plus at most 1 ms, i.e. about (N-1)/N of a game
  frame at a fixed N; Smoothness: about one game frame); not suited to twitch shooters.
- Fast flicks can leave seams; a semi-transparent HUD over motion artifacts; scene cuts
  hold the nearer real frame. A game's own still crosshair / solid HUD is kept exact by [UI
  protection](#ui-protection) except when semi-transparent or changing.
- With several virtual connectors (VR) an output repaint could be swallowed by the shared
  force-repaint flag.
- Tearing for generated frames works on DRM and, when the host offers `wp_tearing_control` and allows it, on nested Wayland (see "Latency"); it is a request the host may ignore.
- Under VRR, FG paints on timer ticks while generating, so the display runs at the timer's
  rate rather than following the game's frame times; FG does not turn VRR off.
- With Pause at refresh rate off, going past the refresh works only on nested Wayland and (untested)
  a tearing DRM display; elsewhere it behaves as On.

## Build

The library has no root `meson.build`, and Meson's sandbox forbids handing files under
`subprojects/` to the parent project (`Sandbox violation: Tried to grab file ... from a
nested subproject`), so `subproject()` is impossible. Its 23 shaders (11 for the interpolation, 9 for UI protection -- `ui_v2_update`, `ui_v2_mask`, `ui_v2_lumafill` added with Crosshair V2 -- 3 for motion blur --
`blur_lookup`, `blur_error`, `blur_resolve`, with the include `blur_common.glsl`; the
test-only `gpu/blendbench.comp` is not built), plus 12 colour-shader variants each for `rgb10` and `f16` (`ui_v2_update` and `ui_v2_lumafill` are per-format) (`-DFG_FORMAT_<FMT>=1`, `<name>_<fmt>_spv`), are compiled with
`custom_target()` (same glslang flags and `--vn <name>_spv` embedded headers as gamescope's
own) using absolute paths, and `framegen.cpp` is compiled through the wrapper
`src/FrameGen/FrameGenLib.cpp`. `Why:` bumping the submodule pulls upstream library work
with no copy to drift. `fgtest` is not built. See [build-and-tooling](build-and-tooling.md).

## Testing

Steady-state numbers measured from inside the compositor (headless sway, nested `-r 120`,
vkcube held at 30 fps by MangoHud, the `framegen_pacing` status line enabled with
`gamescopectl log_framegen_pacing debug` against the private instance): fixed 2x presented
about 60 (62 commits/s), fixed 8x about 120 (122), target 120 about 121 for both priorities
(122 / 123), and fixed 8x with Pause at refresh rate off about 226 (the host discards the
rest).

The pacer's tests are the library's (`gpu/pacingtest.cpp`, built from `subprojects/FrameGen`);
gamescope's own copy (`tests/test_framegen_pacing.cpp`) was deleted 2026-10-04 with the pacer.
For reference, the deleted file covered the pure pacer: the content time and `t` for both
priorities (and that tau never goes backwards), a fixed multiplier saturating at the
refresh with no step-down, the Bresenham cadence giving exactly N x fps below it, Target
mode following the latest interval on the next pair, pass-through only at game fps >= the
output rate with its ratio band, Low latency snapping on an early frame and Smoothness
holding at t = 1 on a late one, the cost guard (lowering the rate, passing through, the
probe, no guard without timings), the synth clamp, presented fps ignoring repaints, timer
ticks only while generating under VRR/tearing (`PaintTick`), the D12 resets and the interval
estimator. Pause at refresh rate: the uncapped output interval, the extra timer being
requested (Off, fixed 4x at 200 fps on 144 Hz = 800/s; target 500), Off on a backend that cannot
exceed the refresh being capped like On, On unchanged, the cost guard still bounding Off, and
`PaintTick` with the extra timer. `tests/test_config.cpp` covers the `framegen` keys (including
`ui_protection`, absent -> `crosshair`, and `pause_at_refresh`, defaulting to true on a legacy config, and `safety: off`) and the
legacy-multiplier migration. The renderer needs a GPU and is verified by eye on a real game (status line +
MangoHud output timing).

## Related

[lag-spike-buffer](lag-spike-buffer.md) · [motion-blur](motion-blur.md) · [shader-effects](shader-effects.md) · [compositing-vulkan](compositing-vulkan.md) ·
[resolution-and-refresh](resolution-and-refresh.md) · [backend-drm](backend-drm.md)
