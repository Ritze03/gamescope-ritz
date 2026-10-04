# Frame generation (`image.framegen`)

Shows extra frames on screen between the game's own, by synthesising them with
optical-flow interpolation: a fixed multiplier of 2x to 8x, or a **Target fps**. Added
2026-10-04, pacing reworked the same day (per-vblank fractional pacing, Target fps, Low
latency / Smoothness, up to 8x). Off by default (a plain on/off switch, separate from the multiplier choice), per profile, DISPLAY rail group directly
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
   (The game's own still HUD is kept pixel-exact by [UI protection](#ui-protection).)

`Why:` before the effects, because the effects then treat generated frames like real ones
with no special case (adaptive effects measure generated frames too, which is harmless).
After the upscaler is impossible: the library works at the game's own resolution. Drawing
the fork's own Crosshair/HUD after FG keeps them sharp, while the game's own HUD may
artifact unless UI protection holds it.

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
- **Logs**: the renderer logs under the `framegen` scope (ready / unavailable / UI protection
  warnings); the pacing glue logs under **`framegen_pacing`** (see
  [Logging](#logging)). They are two scopes because every `LogScope` registers a
  ConCommand of its own name and the console asserts on a duplicate -- two scopes called
  `framegen` aborted the binary in static init.

## UI protection

The **UI protection** row (`framegen.ui_protection`: Off / **Crosshair** (default) / Whole
screen) keeps a still crosshair, or with Whole screen every solid still HUD element,
pixel-exact on generated frames. It is **independent of Static HUD protection**.

**It lives in the FrameGen library, not in this repository.** The library work is
[Ritze03/FrameGen PR #1](https://github.com/Ritze03/FrameGen/pull/1) (branch
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
`round(sqrt(uiCrosshairArea * w * h))` (`uiCrosshairArea` 0.03, i.e. 332 px at 2560x1440);
Whole screen watches every pixel in cells and protects the still ones, giving up on a frame
where more than `uiMaxStillFraction` (0.75) of it is still (a paused game, a loading
screen). Per observed frame the library keeps its own **clean copy** with the protected
pixels inpainted (reach `uiInpaintReach` 12 px); estimate and synth run on the clean copies,
and every synth ends with `out = mix(out, real pixel, mask)`, reading the real pixels from
the `curr` view it is given. All of these are runtime `Settings` fields; the host sets only
`uiProtection` and leaves the rest at the library's defaults.

**Host wiring** (`FrameGenHost.cpp`):

- `recordObserve(cb, ringSlotView)` is called **once per new real frame**, in the command
  buffer that copied it into the ring, right after the ring copy and its barrier, **also
  for frames that are only passed through** (the stillness counters compare consecutive real
  frames; a gap costs only the exact "8 in a row"). Off: the call is skipped entirely.
- The **ring holds the real, un-inpainted frame**: `recordSynth` must get the real `curr`
  to paste from. The library keeps `kUiFrames = 3` clean copies, LRU by view; the 3-slot
  ring re-observes exactly one slot per new frame, so the library holds exactly the ring's
  three frames and a pair whose frames are both in the ring is always protected. A pair
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

**Timing.** The library adds `ui_detect` and `ui_inpaint` (per observe) and `ui_patch`
(per synth) to its profile, and an observe opens a profile that the next estimate
continues. `HarvestProfile()` therefore classifies stamps by name: the observe's two
passes are added to the **estimate's** time (per real frame, like the estimate) and
`ui_patch` to each **synth's**, so the cost guard and the Status line's "FG X ms" include
them; the UI share alone is published as `RenderStatus::lastUiMs` (one observe plus one
patch) and the Status line appends `· UI 0.05 ms` only while UI protection is on and has
been measured. Idle gaps are the hazard: a delta whose previous stamp was recorded in an
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
- **Output interval `o`** (with Pause at refresh rate on; off: see below). Fixed N: `max(interval / N, 1 / refresh)`. Target: `1 /
  min(target, refresh)`, target 0 = the display's refresh. *refresh* is the rate the
  vblank timer ticks at (`g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh`, which is
  what `CVBlankTimer::GetRefresh()` returns -- it has no VRR branch, so under VRR it is the
  mode's nominal (maximum) refresh). **Cadence:** when `o` is longer than a vblank only the
  vblanks where a phase accumulator crosses `o` get a new image (Bresenham; the accumulator
  never lags more than a vblank, so slots that lapsed while a real frame was held are not
  made up with a burst); the others re-present the previous output. So a fixed multiplier
  never steps down: 2x at 100 fps on 144 Hz simply fills every refresh the content allows.
- **Delay `D`, per Priority.**
  - **Low latency** (default): `D = (smoothed interval - o) + margin`, floored at 0 (and at
    one `o` while the ratio is >= 1.1). At a fixed N that is the old (N-1)/N of a game frame
    for a steady game. The smoothed (median) interval is replaced by the latest one only
    when that is genuinely early or late (more than `max(2 ms, 15%)` off): such a frame
    *snaps* the content time to the new pair instead of sliding (D9's latency-first rule);
    only the newest pair is ever played. `margin` = the largest ordinary arrival jitter in
    the window (deviations past the snap threshold are hitches and left out; at most 4 ms)
    plus the lead by which the paint wakes before its vblank (`V - now`, at most 4 ms): 0 for
    a perfectly steady game. `Why` default: the user asked for latency first. `Why` the
    smoothed interval and the margin (2026-10-04, from a steady-state smoke: vkcube held at
    30 fps on a nested 120 Hz, fixed 8x / target 120 gave about 105 output frames a second
    instead of 120): with `D = latest interval - o`, D jumped from pair to pair with ordinary
    arrival jitter, and the real frame of a pair sits exactly where the next real frame is
    expected, so any late arrival -- and the paint wakes about 4 ms before the vblank it is
    for, so a frame arriving in that lead is not seen by that paint either -- left a vblank
    with nothing new to show and lost an output. The margin buys exactly that slack, no more.
    `Why` the floor of one `o` at ratio >= 1.1: a game just under the output rate (the
    user's CS2: 212 fps on about 280 Hz) has `i - o` below one output interval, so no
    generated frame fitted between two real ones and Low latency presented FEWER frames than
    the game; the floor makes room for one.
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
- **Cadence-skip vblanks do not paint** (2026-10-04). When `o` is longer than a vblank and no
  new output is due, the pacer says `Decision::skip` (only if nothing but frame generation
  asked for the paint: no UI / cursor / fade repaint, no repaint that is not the arrival of a
  real frame or its own `force_repaint()`): `FrameGen_PrePaint()` returns true, the loop does
  not call `paint_all()` and nothing is committed; the host keeps the last buffer. A skip
  consumes nothing (the fresh real frame stays pending for the due paint) and asks for the
  next vblank. `Why`: the previous behaviour re-presented the cached output on every such
  vblank, which made a 30 fps game at 2x commit 83 times a second instead of 60 (host and
  GPU work for an identical image). A genuine repaint -- or one that cannot be told apart from
  the pacer's own forced repaint in the same instant -- waits at most until the next due
  vblank.
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
- **V** for the paint is the timer's target time (`Inputs::vblankNs`), so `tau = V - D` is
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
| `framegen.mode` | `fixed` (default) / `target` | which kind of multiplier, the row "Multiplier": 2x..8x / Target fps. Never `off` any more |
| `framegen.multiplier` | 2..8 (default 2) | fixed mode's multiplier; normalised to 2..8 on load (below 2 -> 2, above 8 -> 8) |
| `framegen.target_fps` | 0, 30..1000 | target mode: 0 = the display's refresh; below 30 clamps to 30, above 1000 to 1000 |
| `framegen.priority` | `low_latency` (default) / `smoothness` | what pacing trades under jittery frame times |
| `framegen.pause_at_refresh` | `true` (default) / `false` | stop generating once the game reaches the refresh (on), or keep generating above it (off); additive, an older config loads `true` |
| `framegen.quality` | `quality` / `performance` | Performance = flow scale 4, sub-pixel off: about a third cheaper, can miss thin fast detail |
| `framegen.safety` | `off` / `low` / `default` (default) / `high` | trust ramp (16,56) / (12,40) / (8,28): how readily doubtful pixels fall back to the real frame. `off` disables every fallback: trust (254,255), `globalFallback` 1.0, `sceneCutSad` 255 -- never the real frame, not on fast flicks or scene cuts (smoothest, visible smearing and blended cuts). The others keep the library's 0.15 / 30 |
| `framegen.hud_protection` | `off` / `normal` / `strong` | zero-vector bonus 0 / 1 / 2.5: a soft "prefer still" bias for see-through HUD. It no longer gates anything else (until 2026-10-04 it also switched the crosshair protection on) |
| `framegen.ui_protection` | `off` / `crosshair` (default) / `whole_screen` | the library's [UI protection](#ui-protection); additive (schema stays 5), an older config loads `crosshair`, an unknown string keeps it |

**Migration** (on load; save writes `enabled`, `mode`, `multiplier`, `target_fps` and never
`"off"` again): `mode: "off"` -> `enabled = false`, `mode = "fixed"`, the stored
multiplier kept (normalised to 2..8); `mode: "fixed"` / `"target"` with no `enabled` ->
`enabled = true` (a `fixed` with multiplier below 2 stays disabled, as it used to load as
off); no `mode` and no `enabled` (written before Target fps) derives from the old
`multiplier`: 0 -> disabled, 2..8 -> enabled + `fixed`, so an old 3x profile stays 3x. An
explicit `enabled` always wins. Unknown strings keep the default.

Rows, in order: **Frame generation** (a switch, `framegen.enabled`; help *Generates extra
frames between the game's own frames. Adds delay, so it is not suited to twitch
shooters.*), **Multiplier** (2x / 3x / 4x / 5x / 6x / 7x / 8x / Target fps; it sets `mode`,
and `multiplier` for a fixed choice, and is keyed to `framegen.mode` -- the Shell's
overridden-dot follows one key per row; disabled while the switch is off), **Target fps**
(a slider, 0 shown as "Display refresh", disabled unless Multiplier is Target fps; a drag
into 1..29 snaps to 0 or 30), **Priority** (Low latency / Smoothness), **Pause at refresh
rate** (a switch), Quality, Artifact safety (Off / Low / Default / High), Static HUD
protection, UI protection (Off / Crosshair / Whole screen), Status. Multiplier and the rows after it are disabled while the switch is off. The rail summary reads
"off", "N×" or "target N". Priority help: *Low latency adds the least
delay, but motion can stutter when the game's frame times jitter. Smoothness spaces the
frames perfectly evenly and adds about one game frame of delay.*

`Why two rows:` the user, after testing the single combined choice: *"There should be a
single toggle to enable and disable framegen in general, and then below that should just
be the multiplier. It shouldn't be combined into one."* Internally `fghost::Config` gained
`bool enabled` (packed word bit 26, `Enabled()`/the gate follow it) and `Mode::Off` stays
in the enum only because `Pacing.h`'s `Mode` mirrors it (`steamcompmgr.cpp` static_asserts
the two match); the config never produces it.

`Why:` the library's expert dials (searchPenalty, smoothBonus, sceneCutSad, globalFallback)
are deliberately not exposed -- only the library's defaults were visually reviewed, and raw
sliders would invite settings nobody has looked at.

**Status line** (always one line):

- generating, fixed: `game 60 fps -> presented ~120 fps · 2× · FG 0.42 ms · +8.3 ms delay`;
  when the display cannot show N x the game: `... · 2× (1.4× actual) · ...`
- generating, target: `game 100 fps -> presented ~240 fps · target 240 (2.4×) · FG 0.50 ms
  · +6.3 ms delay`
- with UI protection on, `· UI 0.05 ms` follows the FG time (that share is already inside it)
- otherwise the reason (renderer's first, then pacing's), `Off`, or `Waiting for frames`.

It writes `->` and 1/2 in help text because the overlay font atlas is Latin-1 only (no
U+2192 or the vulgar-fraction glyphs); `·` and `×` are Latin-1 and render. **Status
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

UI protection: see [its section](#ui-protection) (Crosshair +0.03 ms, Whole screen +0.05 ms
at 1440p; 3 clean copies, 44 MB at 1440p, +15 MB for Whole screen).

## Limitations

- SDR, 8-bit only.
- Compute on the same queue, no async: FG time adds to composite time.
- Adds delay (Low latency: (N-1)/N of a game frame at a fixed N; Smoothness: about one game
  frame); not suited to twitch shooters.
- Fast flicks can leave seams; a semi-transparent HUD over motion artifacts; scene cuts
  hold the nearer real frame. A game's own still crosshair / solid HUD is kept exact by [UI
  protection](#ui-protection) except when semi-transparent or changing.
- With several virtual connectors (VR) an output repaint could be swallowed by the shared
  force-repaint flag.
- Under VRR, FG paints on timer ticks while generating, so the display runs at the timer's
  rate rather than following the game's frame times; FG does not turn VRR off.
- With Pause at refresh rate off, going past the refresh works only on nested Wayland and (untested)
  a tearing DRM display; elsewhere it behaves as On.

## Build

The library has no root `meson.build`, and Meson's sandbox forbids handing files under
`subprojects/` to the parent project (`Sandbox violation: Tried to grab file ... from a
nested subproject`), so `subproject()` is impossible. Its 17 shaders (11 for the interpolation, 6 for UI protection) are compiled with
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

`tests/test_framegen_pacing.cpp` covers the pure pacer: the content time and `t` for both
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

[shader-effects](shader-effects.md) · [compositing-vulkan](compositing-vulkan.md) ·
[resolution-and-refresh](resolution-and-refresh.md) · [backend-drm](backend-drm.md)
