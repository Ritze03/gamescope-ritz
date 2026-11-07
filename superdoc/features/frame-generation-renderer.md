# Frame generation: renderer and HDR / 10-bit

Part of [Frame generation](frame-generation.md). The renderer host (`fghost`) and how HDR and 10-bit games are handled.

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
  [Logging](frame-generation-settings.md#logging)). They are two scopes because every `LogScope` registers a
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
