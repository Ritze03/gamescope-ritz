# Frame generation: pacing

Part of [Frame generation](frame-generation.md). The per-vblank pacer, VRR and tearing, latency, hysteresis, pause rules and the dated history of each change.

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

## What gamescope does with the pacer

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

## The dynamic mode and the two priorities (the user's spec)

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

## Cost guard and its switch

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

## VRR and tearing: timer-paced while generating

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

## Latency: Low latency, the wake-up lead and tearing (2026-10-04)

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
   [display-general.md](display-general.md#low-latency-wake-up-displaylow_latency_wakeup)),
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
   ms)` or `(default)` (`(vrr)` too, since 2026-10-06).
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

   **Nested Wayland, output timer (2026-10-05 fix).** `bTearing` also demands that the *game's*
   base commit asked for async (IMMEDIATE/MAILBOX present mode, `bSurfaceWantsAsync`). A
   vsynced (FIFO) game never does, so with the output timer running (Target mode, or Pause at
   refresh off; `FrameGen_CanExceedRefresh()` is always true nested) the extra output frames
   were committed with the `vsync` hint and the host discarded them: Hyprland reported
   `tearingHint: 0`, `tearingBlockedBy: window settings`, with `allow_tearing true` on the
   monitor. Now the hint is a **state**, `fgtear::Hint` (`src/FrameGen/TearHint.h`, main loop next to
   `bFGTearPresent`): it goes `async` the first loop iteration the output timer paces the output
   with `Allow tearing` on, `SupportsTearing()` and nested Wayland (independent of the game's
   present mode), stays async on every paint, vblank-driven ones included, and returns to
   `vsync` only once the output timer has been off for 250 ms. An overlay or a fade is an
   immediate exception (vsync at once, async again after 250 ms without one). A genuine forced
   repaint is not an exclusion on this backend. The backend logs each change:
   `wayland: tearing hint -> async` / `vsync` (info, `xdg_backend`), now once per real
   transition. `Why:` the first version recomputed the hint per paint from
   `bFGTimerPaced && !forced repaint && ...`; with the pacer's plan flickering between
   generating and passing through around the output rate, and forced repaints from the
   HUD/overlays arriving between timer paints, it flipped **22,995 times in 44 minutes** of one
   Forza Horizon 6 session (2026-10-05, ~23/s, only while generating), each a
   `wp_tearing_control` request. The hint only tells the host how it may schedule the buffer; it
   is not a per-frame decision. The game's own async request while frame generation paces on
   the vblank timer is still per paint, unchanged. `Why:` the game's present mode says how the GAME wants its
   own frames shown; frames generated between vblanks have no meaning to the host unless it is
   told to show them at once, so gating that on the game is wrong. `Allow tearing` still gates
   it: with it off the user has asked for no tearing, and the output then stays capped by the
   host's vsync. Real frames from a game that does not request async still never tear.
   **Hyprland needs** `allow_tearing = true` (general), the gamescope window **fullscreen**
   (Hyprland tears only a fullscreen window) and a hint or rule: the hint now arrives, so no
   rule is needed, but `windowrule = immediate` (Lua: `hl.window_rule({ match = { class =
   "^gamescope$" }, immediate = true })`) forces it for any window state. Not fullscreen =
   no tearing whatever the hint says.

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

## A steadier generate / pass decision (2026-10-05, library PR #15)

A game near the target (CS2 at 210-330 fps on 280 Hz, Target = refresh, Smoothness, Pause at
refresh off) used to switch between generating and passing through about once a second, so the
HUD's Both readout kept flipping. The library now decides on a steady EMA of the game rate
instead of the 8-frame median, with a 1 s dwell to stop and 0.3 s to start (flips in the
simulation of that setup: hundreds a minute to under 1), and adds `Report::generating` /
`steadyMultiplier` for a HUD. Details and measurements: `subprojects/FrameGen/INTEGRATION.md`,
"The generate / pass decision". gamescope only plumbs the two new fields into `PacingStatus`.

**Target start band (2026-10-06, library PR #19).** Target mode's generate/pass band is now
stop 1.005 / start 1.04 (was 1.02 / 1.10), and the first decision is re-taken until the
steady rate is mature. `Why:` the user reported *"the adaptive mode sometimes for some
reason doesn't boost up to the target FPS"*; a game at 255-274 fps on a 280 target stayed
passed through (it had to be about 9% under), and about a quarter of simulated sessions got
stranded on a premature first decision. After the fix 262-275 fps games generate 100% and
present 280. No host code changed.

## Pacing fixes (2026-10-05, library PR #16)

Pinned at `981c844` (the library's local-only `ml-fg-testing` branch; main was `b77d058` with PRs #15-#19 merged; #19 is the Target start band, below; `0015453` was PRs #15-#18, #18 being the crosshair box margin below; `fb34454` was PRs #15-#17; same tree as the former PR-branch head `98292d8`, which was `c60e199` plus PR #17: `kExtraLeave` 1.02 to 0.995, so the output timer no longer stays on once the target is back at the display refresh). Three things: the Lag spike buffer grows at once after a stall bigger
than the applied delay, and its spike threshold is 1.3x the median interval with at least 2 ms
excess (see [lag-spike-buffer](lag-spike-buffer.md)); the extra output timer got only tests and
docs; and `Report::generating` is now just "Generate state and a generated frame presented
within 1 s", with no 1.2 / 1.1 multiplier gate (see [fps-display](fps-display.md)). The host
changes are help-text and comment wording only.

## Pause while cursor visible (`framegen.pause_when_cursor_visible`)

Row **Pause while cursor visible**, per profile, default **Off**, right after Pause at
refresh rate. While on and the game's cursor is visible, `FrameGen_PrePaint` feeds the pacer
`in.frameGen = false` (real-rate planning, the same input the library gets when Frame
generation is off), so real frames are shown and nothing is interpolated; it resumes by
itself when the cursor hides. `framegen.enabled` is untouched. "Visible" is the
Crosshair's signal, `wlserver.bCursorHasImage && !bMouseConstraintLocked`, read on the
steamcompmgr thread where `bCursorHasImage` is written. The flag crosses to the host in
bit 31 of the packed main config word (`FrameGenHost.cpp` `Pack`/`Unpack`); the library
never sees it. Motion blur and the Lag spike buffer are unaffected (they keep their own
switches). `Why:` menus and settings screens show the cursor, and interpolating UI
smears it; the user asked for *"a setting to framegen, that allows the user to turn it
off, while the mouse cursor is visible. (To avoid artifacting in settings menus)"*.

## Paints behind the game's GPU burst (2026-11-04)

For two days of measurements the paints that showed a generated frame lost their deadline while
the GPU had room: `exec` stayed about 0.5 ms and `wait` ran up to the length of the game's frame.
The readout's "queues behind the game's GPU burst" was right, but the reason was ours, not the
hardware's: **the HUD**. `FpsDisplay_AddLayer()` drew its texture on the **general queue**, the
one the game renders on, at every paint, and the composite (compute queue) waits for that draw
(`FpsDisplay_WaitForRender()`), so every paint queued behind whatever the game had in flight
there. A paint right after the game's present waited for all of its frame; a paint in the gap
did not wait. Why `exec` gave it away: real contention for the GPU would stretch the time between
the composite's timestamps. It did not, the composite just started late.

The fix (`FpsDisplay.cpp`): the draw data of the frame (and `Crosshair_RasterGeneration()`, the
stretched crosshair's raster) is hashed, and with nothing new the texture is left as it is:
nothing goes on the general queue, and the composite's wait is on a point already passed. The
HUD still costs a wait when its digits change, on that one paint. `Why` not two textures, so the
composite never waits at all: the misses were already 0-2 of 60, so it would buy nothing measured.
The settings overlay and the toasts still draw per paint while they are on screen.

Measured (Minecraft, 280 Hz, VRR on, Target = refresh, Smoothness, motion blur 2 samples, Learned
e2v5 half, HUD and Crosshair on, fixed view, worktree build, the same session and two 25 s runs
each, switched live; output 311 is the extra timer's deliberate 0.9-tick oversupply; the second
uncapped run with the fix, with compilers running beside it, gave output 281-311, and both limit
120 runs with the fix had compilers beside them, hence the game's spread):

| Game limit | HUD drawn at every paint | HUD drawn when it changes |
| --- | --- | --- |
| none | game 225-238, output 226-246, missed 50-60/60, draw 4.4-4.7 ms, `other` wait 1.7-2.1 mean | game 223-226, output 311, missed 0/60, draw 0.65 ms, `other` wait 0.1 mean |
| 120 | game 118-125, output 240-256, missed 26-30/60, `other` wait 3.4 mean | game 107-129, output 311, missed 0-2/60, `other` wait 0.4-0.5 mean |

The final build from the ritz-get install (LTO), same settings, 30 s runs without compilers
running beside it, render distance 16 unless noted; every row missed 0-4 of 60 paints:

| Case | Game | Output |
| --- | --- | --- |
| Fullscreen, VRR, uncapped | 216-231 | 305-311 |
| Fullscreen, VRR, limit 90 / 120 / 160 | 89-93 / 117-124 / 158-166 | 311 |
| Window, no VRR, uncapped | 229-237 | 309-311 |
| Window, no VRR, limit 120 | 119-124 | 310-311 |
| Fullscreen, VRR, render distance 40, uncapped, GPU busy 100% | 151-156 | 311 |

What is left of the stall: when the digits change, that one paint still waits for the game's
frame (`wait` max 3-9 ms, about 1% of paints in the render distance 40 scene). No longer visible
in `missed`.

`Why` it hid so long: with the game limited by its own limiter and light shaders the bursts are
short and fall at random phases, so most paints missed them; heavier shaders or a limit that frees
the game at a paint made the same wait look like a GPU limit. Raising the GPU clocks changed
nothing and realtime queue priority barely helped, because neither is what the paint waited for.
Early pair, tried the same day, moved the estimate out of the paint and the wait rose by the same
amount, and was reverted: on the new limiter, output 242-244 with it, 246-249 without.

## Hold game while generating (removed 2026-11-04)

From 2026-11-03 to 2026-11-04 a per-profile switch, `framegen.hold_game`, held the game in the
WSI layer after each present and released it on a schedule from an AIMD rate controller
(`HoldController.h`), over a `frame_gate` event and a `register_window` request on
`gamescope_swapchain`. All of it is gone: the switch, the controller, the gate, and the protocol
is back to version 1. A profile that still holds the key loads normally; the key is ignored and
the next save drops it, like `framegen.v2_mode`.

`Why:` the controller's signal was wrong. Paint deadline misses ran at about one per real game
frame at any game rate, so "misses, slow down" could only ratchet the game to the floor (280 / 8
= 35 fps). That number was not the GPU being over-subscribed: the misses came from the HUD's
per-paint general-queue draw, see [Paints behind the game's GPU burst](#paints-behind-the-games-gpu-burst-2026-11-04).
The first design, releasing at the end of the pair, collapsed the game to its 50 ms timeout (221
fps unheld, 20-25 fps held). `Why` nothing replaced it: with the stall gone, the uncapped game
and Frame generation share the GPU without a cap, and a fixed limit is the Frame limiter's job.

## Paint split readout (diagnostic, 2026-11-04)

The status line carries `paint split: pair n=170 miss=160 cpu 0.3/0.9 exec 4.1/5.2 wait 0.2/6.8, other n=1000 miss=20 cpu ... ms` (mean/max over the line's period, two buckets: paints that consumed a new real frame, and all others; no behaviour change). `Why:` Minecraft on 280 Hz lost about one paint per real frame while the GPU was only half busy, and two causes look identical in `draw max`: gamescope's own work is long (the pair's flow estimate runs in the paint that first sees the frame), or the composite is submitted on time but queues behind the game's GPU burst. `exec` is GPU execution of frame generation's command buffer plus the composite's (timestamp pairs, `vulkan_paint_split_*` in `rendervulkan.cpp`, read a paint later, never blocking); `wait` is first submit to GPU-finished minus `exec`, so high `exec` means suspect 1 and high `wait` with low `exec` means suspect 2. `cpu` is vblank wake to the composite's submit. Caveats: "GPU finished" is the CPU time `vulkan_wait()` returned (every backend waits right after the composite; a late wait would inflate `wait`); `miss` is wake to GPU-finished past the wake-up lead, so it can undercount versus the timer's draw time (Present's tail is not in it); `cpu` is long for a paint not woken by the vblank timer.

## Pause at refresh rate (`framegen.pause_at_refresh`)

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

**Disabled in Target mode (2026-10-05).** The row is grayed out (`DisabledUnless(
PauseRowUsable )`, enabled only with the switch on and Multiplier not Target; the stored
value is untouched and the library is unchanged) and its help says it has no effect with a
Target. The user: *"Then 'gray' out (disable) it, if adaptive mode is on."* ("Adaptive
mode" is their name for Target fps.) `Why:` with a Target the output interval is already
`1 / target` (`OutputIntervalMs`, target 0 = the refresh), so in the user's setup (Target
0, 280 Hz) the switch did nothing and a live-looking switch that does nothing misleads.
**Ignored in Target mode (2026-10-05, same day).** The switch is not only grayed, it has
no effect there: `fghost::EffectiveCapAtRefresh( pause, mode )` (`FrameGenHost.h`, applied
in `Pack()`, so every reader of `GetConfig()` including the extra-timer test and the pacer
inputs sees it) is `pause && mode != Target`, i.e. in Target mode the library always runs as
"Pause off". The one case where the stored value used to matter was an *explicit* target
above the refresh (400 on 280 Hz) on a backend that can show past the refresh (nested
Wayland, tearing): Pause on clamped it to 280, Pause off let 400 stand. Now 400 stands
whatever was stored. On a backend that cannot show past the refresh the library still
clamps (`Pacer::CapAtRefresh` is `pauseAtRefresh || !canExceedRefresh`), so on a normal
display the refresh is the limit. Target 0 is still the refresh; fixed multipliers keep
the switch exactly as before. `Why:` the user, on being offered "usable only when the target
is above the refresh": *"No, it shouldn't be affected by the slider as well, it should then
just have no effect rather."* Done in the host, not the library: it is a GUI-setting
mapping. Pinned by `tests/test_framegen_format.cpp`.

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

**At the refresh itself (2026-11-03).** `ExtraTimer()` stops at `o < 0.98 tick`, so Target
"display refresh" (`o == tick`) and a fixed multiplier landing on the refresh were driven by the
vblank timer, where a paint that overruns its vblank loses that output for good (the cadence in
`PaintGenerated()` is per vblank slot). A user's journal (nested Wayland, 280 Hz, tearing hint on,
not VRR, Forza at 60 fps, Smoothness): *"game 59.8 fps, presented 219.8 fps (3.67x), ... draw mean
2.59 / max 6.29 ms ... missed 17/60"*, always 211-238 presented, i.e. `280 x (1 - missed/60)`. The
misses are the game's GPU burst delaying our queued composite (no mid-draw preemption): max draws
of 6-10 ms against a 2.6 ms mean. A target of 320 reached the full 280 because it is extra-timer
driven: a late paint is presented at once and the next follows, and the host discards the surplus.
Fix, host side only (`src/FrameGen/AtRefreshExtra.h`, used by `FrameGen_ExtraPaced()` /
`FrameGen_ArmExtraTimer()`): when the last paint could exceed the refresh (`bCanExceed`), the cap is
off and `o` is within 2% of a tick, the extra timer drives it too, ticking every 0.9 tick
(`kOversupply`, ~311 Hz at 280). `Why` the oversupply and not just `o == tick` on the extra timer:
the host shows only the newest buffer per vblank, so a timer at exactly the refresh still loses an
output wherever a late paint and its successor land in one host vblank and has no slack to catch
up; the surplus is what the 320 workaround amounted to (0.875 tick). The library's plan (`o`, delay,
content time) is untouched; only the timer's spacing differs. Unchanged: Pause at refresh rate on
(Multiplier mode), backends that cannot exceed the refresh (SDL, non-tearing DRM), VRR handling,
`o` further from a tick. Not verified live on the user's setup.

**Oversupply re-check (2026-11-04): 0.9 kept.** Test: nested Wayland on the 280 Hz screen,
Minecraft, Target = display refresh, limit 120, 20 s per value switched live in one session
(temporary ConVar and a temporary counter of the host's presentation feedback, both removed),
four interleaved repeats of 0.90 / 0.95 / 0.98 / 1.00. Frames actually shown to the host (the
`wp_presentation` `presented` feedback count per second, vs `discarded`) in the undisturbed
repeats: 0.90 168, 169; 0.95 171, 166, 181; 0.98 169, 169, 171; 1.00 173, 173, 169, 176 per
second, game 120-122 fps, missed 0/60 everywhere, GPU busy 86-89% at every value. No
difference beyond scene noise, and no periodic loss at 0.98 or 1.00. So 0.95-1.00 are as good
as 0.9 in this case, but nothing measurable is saved either (GPU busy equal within 1-3 points),
and this case never makes a paint late, which is what the surplus is for (the Forza journal
above), so 0.9 stays. Not concluded: uncapped (game 60-200 fps, other compiles running on the
machine, runs discarded) and a heavy GPU-saturated case. Also seen: the host shows only about
170 of the roughly 280-311 frames sent per second (the rest are `discarded`), at every value
(explained the same day: the host's draw waits for the game's GPU job, not gamescope; see
"Frames the host shows" under Logging).
`Why:` do not lower the value on this evidence; revisit with a late-paint workload.

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
