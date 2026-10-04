# Frame generation (`image.framegen`)

Shows extra frames on screen between the game's own, by synthesising them with
optical-flow interpolation: a fixed multiplier of 2x to 8x, or a **Target fps**. Added
2026-10-04, pacing reworked the same day (per-vblank fractional pacing, Target fps, Low
latency / Smoothness, up to 8x). Off by default, per profile, DISPLAY rail group directly
below Shaders.

Code map:

| What | Where |
| --- | --- |
| The library (optical flow + synth shaders) | `subprojects/FrameGen/` (git submodule, MIT, `github.com/Ritze03/FrameGen`) |
| Library build glue | `src/FrameGen/FrameGenLib.cpp` + the `custom_target()`s in `src/meson.build` |
| Renderer host (`fghost`) | `src/FrameGen/FrameGenHost.{h,cpp}` |
| Pacing (`fgpacing::Pacer`) | `src/FrameGen/Pacing.h` (header-only) |
| Pacing glue + the main-loop gate | `src/steamcompmgr.cpp` (`FrameGen_OnArrival` / `FrameGen_PrePaint` / `FrameGen_PostPaint` / `FrameGen_TimerPaced`, `PaintTick` in the paint decision) |
| Composite hook | `src/rendervulkan.cpp` (`vulkan_composite()`, `fghost::RecordBaseLayer`) |
| Frame-ring copy shader | `src/shaders/cs_fg_copy.comp` |
| Settings area | `src/Overlay/PanelFrameGen.{h,cpp}` |
| Tests | `tests/test_framegen_pacing.cpp` (pacing), `tests/test_config.cpp` (the `framegen` keys) |

Not to be confused with `lsfg-vk` (an external Vulkan layer): nothing in this tree uses
it. FG here runs inside the compositor.

## What it does, and where it runs in the frame

Between two real game frames P (previous) and C (newest) the library can make a generated
frame at **any** t in (0,1) -- one motion estimate per pair, any number of synths after it.
Pacing decides, for every output frame, which t to show (see [Pacing](#pacing-model-fgpacingpacer)).

Pipeline order inside `vulkan_composite()`:

1. FG substitutes the raw **layer 0** (the game) with the output frame to show
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
which FG cannot substitute per output frame.

## Renderer (`fghost`)

- **Off = zero cost**: one relaxed atomic load per frame; no Interpolator, no ring, direct
  scanout stays possible. The host object is created lazily on first use; turning FG off
  frees everything after a `g_device.waitIdle()`.
- **Backends force a full composite while `fghost::RenderWanted()`** -- FG enabled, *or* it
  still holds resources -- so turning FG off releases them: the next composite runs
  `RecordBaseLayer`, which tears down, instead of direct scanout skipping it forever.
  (`SetConfig` to Off also asks for that composite with `force_repaint()`.)
- **API**: `SetFrame(FrameRequest{ newestId, prevId, currId, outId, t })`. `t < 0` is inert
  (not even the ring copy: settled pass-through), `t >= 1` the real frame (layer 0; the ring
  copy still happens), `0 < t < 1` a synth between `prevId` and `currId`. The same `outId`
  is the same image, so a repaint (cursor, overlay, screenshot) reuses the cached output.
  `Why` the pair is named by id and not "the newest pair": Smoothness may still be playing
  the pair before the newest one.
- **Own command buffer**, submitted just before the composite on the same queue.
  `Why:` a GPU wait (needed before a resize/reconfigure) in the middle of the composite
  would reset the device's upload-buffer offset and corrupt it, and the hook must run
  before the ReShade block, which comes before the composite command buffer exists. A
  pass-through of an already-seen frame records nothing.
- **Private 3-slot frame ring**, always `B8G8R8A8_UNORM`, filled by the tiny compute copy
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
- **Skipped** (the real frame is shown and the status says why) for HDR output or content,
  YCbCr, non-8-bit formats, a frame below 16x16, and init/record failure. `Why:` the library
  is 8-bit SDR only, and a hard failure would black the screen.
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
- **Logs**: the renderer logs under the `framegen` scope (ready / unavailable / crosshair
  errors); the pacing glue logs under **`framegen_pacing`** (see
  [Logging](#logging)). They are two scopes because every `LogScope` registers a
  ConCommand of its own name and the console asserts on a duplicate -- two scopes called
  `framegen` aborted the binary in static init.

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
   saved to that ring slot's **patch** texture (`.rgb` original, `.a` mask), then the masked
   pixels of the ring slot are overwritten in place with a 1/distance^2-weighted average of
   the nearest unmasked pixel along each of 8 directions (reach 12 px). Motion estimation
   and the warp therefore see plain background where the crosshair was. The previous
   frame's slot was inpainted in its own turn and is only ever `prev`, so nothing is redone
   for it; the next detect compares against the previous slot's patch texture's ORIGINAL
   pixels, never the inpainted ring. `Why` a directional search and not a push-pull
   pyramid: a crosshair is thin, and this is one dispatch with no scratch levels. A pixel
   with no unmasked neighbour in reach (the whole ROI is still) is left as it is.
4. **Patch back** (`cs_fg_crosshair_patch.comp`): after each `recordSynth` into its output
   texture, `out = mix(out, patch.rgb, mask)` over the ROI, with the patch of the real frame
   the synth leads up to (its `curr`). Outputs are cached per output frame, so repaints
   keep the crosshair at no extra cost. The real frame shown at t = 1 is the game's own
   texture and is never touched.

Textures: two ping-pong counter textures and **three patch textures** (one per ring slot,
so a synth of the older pair still gets its own frame's crosshair), all ARGB8888 ROI-sized
(about 0.44 MB each at 1440p), created lazily and released with the ring (after
`g_device.waitIdle()`). The counters restart from 0 on any gap: `fghost::Reset()`, a
size/format change, FG toggled, an inert (pass-through) stretch, HUD protection switched
Off and on.

Limits: a semi-transparent crosshair (its pixels change with the background, so it never
reaches the mask); a crosshair that changes shape (spread, hit markers) stays unmasked
while it changes; off-centre reticles; a still camera masks the background too, so a big
moving object crossing the ROI of an otherwise still scene is interpolated from slightly
smeared neighbours. The fork's own Crosshair is drawn after FG and is unaffected.

## Backends

Generated frames must go through the composite, so full composite is forced while FG is on
(or still holds resources, see above). DRM also disables partial composite (and therefore
direct scanout): a scanned-out buffer never passes `vulkan_composite()`, and a partial
composite has already dropped the base layer. Wayland and OpenVR force full composite; SDL
always composites. See the backend pages.

## Pacing model (`fgpacing::Pacer`)

Pure decision logic, no clock or Vulkan of its own (times are passed in), so the tests
drive it with a fake clock; `steamcompmgr.cpp` feeds it arrivals and paints and takes back
what to show.

**Per-vblank fractional pacing.** Every output frame is generated for its own moment
between the two latest real frames; no multiplier is chosen or held. The user's spec:

> *"Also add something like a dynamic mode that lets me set a target FPS, and then it
> should either set the multiplier dynamically and let the system handle the rest. So, for
> example, I drop below 120 FPS, and I have a 2x multiplier. Right now, because my frame
> target is 240, I drop to, for example, 100 FPS, then it should just switch to 3x
> multiplier on the fly. It should do that based on the frame times, so it reacts quickly,
> not like full second accuracy, but per frame accuracy. There should be two modes, one for
> low latency and one for maximum smoothness, basically, so the user can take the path that
> he wants."*

*"Shouldn't it only turn off when my raw FPS is equal to my refresh rate and not at 60% of
my refresh rate?"* -- yes. *"Also let me change the multiplier to up to 8x."* *"It's fine if
we generate way too many frames on a fixed multiplier, this system should handle that as I
told you earlier."*

- **Content time.** A real frame carries its **arrival time** `A` (the commit's acquire
  fence signalling, `commit_t::present_time`, set in `handle_done_commit`) -- not the vblank
  latch. `Why:` latch times are vblank-quantised (about +/-7 ms at 60 fps on a 144 Hz
  display), which would wreck the interval estimate. For the output frame painted at vblank
  time **V** the content time is `tau = V - D`, and with the two real frames around it
  `t = (tau - A0) / (A1 - A0)`. `t >= 1` (tau has caught up with the newest real frame, or
  `t >= 0.98` of the newest pair) shows the real frame itself; otherwise a synth at `t`
  (clamped to 0.02..0.98). `tau` is **monotonic**: never older than the last output's.
  `Why` this shape: "how many generated frames per game frame" now falls out of `t` from
  the latest arrivals -- the first pair after 120 -> 100 fps against a target of 240
  already gets 2.4 outputs on average -- which is the "per frame accuracy" asked for.
- **V** is the vblank timer's predicted target for the paint
  (`g_SteamCompMgrVBlankTime.schedule.ulTargetVBlank`, never in the past: `max(target,
  now)`; a missing or implausible value, or a non-vblank paint, falls back to `now`).
- **Output interval `o`.** Fixed N: `max(interval / N, 1 / refresh)`. Target: `1 /
  min(target, refresh)`, target 0 = the display's refresh. *refresh* is the rate the
  vblank timer ticks at (`g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh`, which is
  what `CVBlankTimer::GetRefresh()` returns -- it has no VRR branch, so under VRR it is the
  mode's nominal (maximum) refresh). **Cadence:** when `o` is longer than a vblank only the
  vblanks where a phase accumulator crosses `o` get a new image (Bresenham; the accumulator
  never lags more than a vblank, so slots that lapsed while a real frame was held are not
  made up with a burst); the others re-present the previous output. So a fixed multiplier
  never steps down: 2x at 100 fps on 144 Hz simply fills every refresh the content allows.
- **Delay `D`, per Priority.**
  - **Low latency** (default): `D = latest real interval - o`, floored at 0. At a fixed N
    that is the old (N-1)/N of a game frame. A real frame arriving early or late *snaps*
    the content time to the new pair instead of sliding (D9's latency-first rule); only the
    newest pair is ever played. Uneven motion on jittery frame times is accepted. `Why`
    default: the user asked for latency first.
  - **Smoothness**: `D = median interval + margin`, margin = `max(2 ms, spread of the recent
    intervals)` (second-largest minus median, so one hitch is ignored), at most half an
    interval. Content time advances evenly at the output rate; the pair played may be one
    **older** than the newest (about one game frame is queued). A late real frame holds at
    t = 1 instead of hitching; an early one is absorbed. `Why` the ring has three slots.
- **Pass-through** (real frames shown at once, renderer inert) **only** when the game's rate
  reaches the output rate -- game interval / `o` below **1.02**, generating again above
  **1.10** (a ratio band, not a time hold; 98% .. 91% of the refresh at a fixed
  multiplier) -- or the renderer is unavailable (HDR, 10-bit, ...; it is still driven, so it
  re-checks), or the cost guard trips. `Why` the old refresh-fit step-down (game fps x N >
  1.2 x refresh) and its 0.5 s hysteresis are gone: it passed through a 144 Hz game at 86 fps
  with 2x, which the user rejected; a fixed multiplier now just saturates at the refresh.
- **Game interval** (status line, Smoothness, the pass-through ratio) = median of the last 8
  arrival intervals, clamped to 4-50 ms, trusted after 4 intervals (warm-up: real frames
  are copied meanwhile so the first pair has its previous frame).
- **Reset** (no FG for the next frame, the previous real frames are forgotten) on a
  focus/window change, size/format change, a gap over 100 ms, fades, Steam UI windows and
  streaming clients. `Why:` frames are keyed by commit id, not texture pointer, because
  gamescope reuses one texture per `wl_buffer` -- pointer equality misses re-commits, and
  without a reset two unrelated images would be blended.
- **Cost guard.** The renderer reports the estimate's time and one synth's. A pair's cost
  is `estimate + g x synth`; when that exceeds 25% of the game interval the **output rate is
  lowered** (`o` raised to `interval / (g_max + 1)`) until it fits, and if not even one
  generated frame per pair fits it passes through (reason CostGuard). A probe retries every
  10 s with one generated frame per pair, judged on the next fresh measurement (the
  renderer's sequence number), and gives up after 3 s without one. It never changes the
  user's Quality preset (a flow-scale change needs a GPU wait, which would stutter if it
  flapped). No guard without timestamps.
- **Synth clamp**: at most 24 generated frames per pair (`kMaxSynthsPerPair`); a pair at
  its cap repeats its last output.

### VRR and tearing: timer-paced while generating

While FG is **generating**, gamescope paces its own output frames on the **vblank timer**,
exactly as on a fixed-refresh display, even with VRR or tearing enabled -- the main loop's
`PaintTick()` (`Pacing.h`) lets only timer ticks paint, and the flip is a normal one;
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

**Phase B (planned, not done)**: a per-output timer that is not the vblank timer (a
timer-paced present path). The `tau = V - D` model generalises to it unchanged -- `V` is just
that timer's present time -- so only the source of `V` and the repaint trigger change.

## Settings and status

Area `image.framegen` ("Frame generation"), per profile (game profiles inherit), no
keybind, config section `framegen` (schema stays 5; additive).

| Key | Values | Meaning |
| --- | --- | --- |
| `framegen.mode` | `off` (default) / `fixed` / `target` | the row "Frame generation": Off / 2x..8x / Target fps |
| `framegen.multiplier` | 0, 2..8 | fixed mode's multiplier; 1 reads as 0 (a `fixed` mode with it loads as `off`), above 8 clamps to 8 |
| `framegen.target_fps` | 0, 30..1000 | target mode: 0 = the display's refresh; below 30 clamps to 30, above 1000 to 1000 |
| `framegen.priority` | `low_latency` (default) / `smoothness` | what pacing trades under jittery frame times |
| `framegen.quality` | `quality` / `performance` | Performance = flow scale 4, sub-pixel off: about a third cheaper, can miss thin fast detail |
| `framegen.safety` | `low` / `default` / `high` | trust ramp (16,56) / (12,40) / (8,28): how readily doubtful pixels fall back to the real frame |
| `framegen.hud_protection` | `off` / `normal` / `strong` | zero-vector bonus 0 / 1 / 2.5: keeps a static HUD from wobbling; any value but `off` also switches on [Crosshair protection](#crosshair-protection) |

**Migration:** a config with no `mode` (written before Target fps) derives it from the old
`multiplier`: 0 -> `off`, 2..8 -> `fixed` -- an old 3x profile stays 3x. Unknown strings keep
the default.

Rows, in order: **Frame generation** (Off / 2x / 3x / 4x / 5x / 6x / 7x / 8x / Target fps; it
sets `mode`, and `multiplier` for a fixed choice, and is keyed to `framegen.mode` -- the
Shell's overridden-dot follows one key per row), **Target fps** (a slider, 0 shown as
"Display refresh", disabled unless the mode is Target; a drag into 1..29 snaps to 0 or 30),
**Priority** (Low latency / Smoothness), Quality, Artifact safety, Static HUD protection,
Status. The sub-rows are disabled while Off. Priority help: *Low latency adds the least
delay, but motion can stutter when the game's frame times jitter. Smoothness spaces the
frames perfectly evenly and adds about one game frame of delay.*

`Why:` the library's expert dials (searchPenalty, smoothBonus, sceneCutSad, globalFallback)
are deliberately not exposed -- only the library's defaults were visually reviewed, and raw
sliders would invite settings nobody has looked at.

**Status line** (always one line):

- generating, fixed: `game 60 fps -> presented ~120 fps · 2× · FG 0.42 ms · +8.3 ms delay`;
  when the display cannot show N x the game: `... · 2× (1.4× actual) · ...`
- generating, target: `game 100 fps -> presented ~240 fps · target 240 (2.4×) · FG 0.50 ms
  · +6.3 ms delay`
- otherwise the reason (renderer's first, then pacing's), `Off`, or `Waiting for frames`.

It writes `->` and 1/2 in help text because the overlay font atlas is Latin-1 only (no
U+2192 or the vulgar-fraction glyphs); `·` and `×` are Latin-1 and render. **Status
semantics:** `presentedFps` counts output frames actually presented (generated frames plus
the real frames pacing decided to show) and **not** repaints of an already-shown output
(cursor, overlay) -- it used to be a paint count; `activeN` = the effective multiplier
rounded, at least 2 while generating and 0 when passing through, so "`activeN >= 2`" means
"generating" (the HUD's *Count generated frames* option relies on that, via
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
- info on `frame generation: on, <mode>, <priority>` and `frame generation: off`;
- debug, at most every 5 s while on: the same numbers (`frame generation status: ...`).

Not keyed on the rounded multiplier: a game at a fractional ratio would flap between two
values and spam the log.

## When it passes real frames through

| Situation | Result |
| --- | --- |
| the game's rate reaches the output rate (refresh or target) | real frames, renderer inert, no added latency |
| per-pair GPU time over 25% of the game interval | output rate lowered, then pass-through; retried every 10 s |
| HDR output/content, YCbCr, non-8-bit, under 16x16, init failure | real frame, status names the reason |
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

Crosshair protection (estimated from the texel-fetch count, not measured on a GPU): per real
frame about 0.03 ms typical and up to about 0.1 ms on a still scene at 1440p (~110k ROI
pixels; detect 4 fetches, mask 26, inpaint up to 96 on masked pixels only), plus a ~110k-pixel
load/store per generated frame; about 1.5 MB VRAM.

## Limitations

- SDR, 8-bit only.
- Compute on the same queue, no async: FG time adds to composite time.
- Adds delay (Low latency: (N-1)/N of a game frame at a fixed N; Smoothness: about one game
  frame); not suited to twitch shooters.
- Fast flicks can leave seams; a semi-transparent HUD over motion artifacts; scene cuts
  hold the nearer real frame. A game's own still centred crosshair is protected (see
  above) except when semi-transparent or changing shape.
- With several virtual connectors (VR) an output repaint could be swallowed by the shared
  force-repaint flag.
- Under VRR, FG paints on timer ticks while generating, so the display runs at the timer's
  rate rather than following the game's frame times; FG does not turn VRR off.
- Phase B (a timer that is not the vblank timer) is not implemented yet.

## Build

The library has no root `meson.build`, and Meson's sandbox forbids handing files under
`subprojects/` to the parent project (`Sandbox violation: Tried to grab file ... from a
nested subproject`), so `subproject()` is impossible. Its 11 shaders are compiled with
`custom_target()` (same glslang flags and `--vn <name>_spv` embedded headers as gamescope's
own) using absolute paths, and `framegen.cpp` is compiled through the wrapper
`src/FrameGen/FrameGenLib.cpp`. `Why:` bumping the submodule pulls upstream library work
with no copy to drift. `fgtest` is not built. See [build-and-tooling](build-and-tooling.md).

## Testing

`tests/test_framegen_pacing.cpp` covers the pure pacer: the content time and `t` for both
priorities (and that tau never goes backwards), a fixed multiplier saturating at the
refresh with no step-down, the Bresenham cadence giving exactly N x fps below it, Target
mode following the latest interval on the next pair, pass-through only at game fps >= the
output rate with its ratio band, Low latency snapping on an early frame and Smoothness
holding at t = 1 on a late one, the cost guard (lowering the rate, passing through, the
probe, no guard without timings), the synth clamp, presented fps ignoring repaints, timer
ticks only while generating under VRR/tearing (`PaintTick`), the D12 resets and the interval
estimator. `tests/test_config.cpp` covers the `framegen` keys and the legacy-multiplier
migration. The renderer needs a GPU and is verified by eye on a real game (status line +
MangoHud output timing).

## Related

[shader-effects](shader-effects.md) · [compositing-vulkan](compositing-vulkan.md) ·
[resolution-and-refresh](resolution-and-refresh.md) · [backend-drm](backend-drm.md)
