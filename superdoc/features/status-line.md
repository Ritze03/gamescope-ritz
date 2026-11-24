# Status line (`ritz_status`) and the judder measure

A diagnostic for the journal, added 2026-11-22. Not UI, no config key, no changelog entry.
`Why:` frame generation's status line (see
[frame-generation-host-scanout.md](frame-generation-host-scanout.md)) exists only while frame
generation or motion blur is on, so with both off nothing in the log said what the game asked for,
how gamescope painted it, what the host showed, or why [Auto V-Sync](display-general.md#auto-v-sync-displayauto_vsync)
did not limit. On a real desktop nobody watches the screen, so smoothness has to be a number.

## Switching it on

ConVar `ritz_status` (int seconds, default 0 = off), live:
`gamescopectl ritz_status 5` logs three lines every 5 s through the `framegen_pacing` log scope,
all starting with `status:` (grep for it). The first interval after switching it on is only a
baseline and prints nothing. `StatusLine_Tick()` in `src/steamcompmgr.cpp`, called once per
main-loop iteration; read-only, it changes no pacing or presentation.

## Line 1: state

Stable order, `|`-separated groups, values over the interval unless "now":

```
status: game 60.0 fps fifo 100% wants-async 0% (now fifo 1 async 0) | paints 62.5/s async 0.0/s
composite 100% gate 1 no-free-image 0.0/s | host sent 62.5 shown 62.5 discarded 0.0 zero-copy 0.0 torn 62.5 /s, feedback
refresh 0.000 Hz | refresh timer 124.9/s nested 120.000 output 120.000 Hz | vrr adaptive_sync 0
host-vrr 1 active 0 lead measured 1.50 ms | tearing enabled 0 supported 1 hint vsync | verdict
host-vsync 0 samples 0 (total 0) test -1 fg_host_vsync 1 fg-cap 0 | limiter user 60 target 60 file
1,0 grid | auto-vsync off no limit: switch off | fg off blur off
```

(one line in the log; wrapped here.)

- `game`: new base-plane commits that reached a paint, per second (the focused app's frames gamescope
  actually painted, not what it rendered). `fifo` / `wants-async`: share of paints whose base commit
  was FIFO / asked for async (`commit_t::fifo` / `::async`, from the present mode our WSI layer
  reports, `wlserver_surface_is_async()`); `now` = the held base commit at the time of the line.
- `paints`: paints that reached the backend's `Present()`; `async` = of those, sent with the async
  flag (on nested Wayland: the tearing hint `async`); `composite` = share of them the backend
  composited (`WasCompositing()` right after `Present()`; a composited native paint is never flipped
  async), `gate` = Auto V-Sync's debounced "gamescope composites its paints" now
  (`autovsync::CompositeGate`, which also counts paints with an overlay window up; added
  2026-11-22, when this turned out to be the deciding input); `no-free-image` = paints dropped
  because the host held every output image.
- `host`: the `fghostshown` counters (`src/FrameGen/HostShown.h`, nested Wayland only), with their
  own baseline so frame generation's line keeps its interval. `feedback refresh` = the refresh the
  last `presented` reported; 0 = none yet, or the host reports none, which gamescope reads as "host is
  VRR".
- `refresh`: vblank-timer iterations per second, `g_nNestedRefresh`, `g_nOutputRefresh`.
- `vrr`: the `adaptive_sync` ConVar, `host-vrr` (the Wayland connector's
  `HostCompositorIsCurrentlyVRR()`: the last `presented` carried refresh 0), `active` =
  `IsVRRActive()` (both), and the wake-up lead the vblank timer used last (`measured` = Low-latency
  wake-up, `vrr` = upstream's VRR lead, `default`). `active 1` also means: the main loop treats every
  iteration as a vblank, flips as `VRR` (never `Async`), and the limiter always paces in the layer.
- `tearing`: `tearing_enabled`, `SupportsTearing()`, and the hint the toplevel carries now.
- `verdict`: the raw `bHostVsync`, `samples` = `presented` feedbacks of async-hinted commits fed to
  the verdict this interval (`total` since start): 0 means the verdict cannot move. `test` =
  `ritz_host_vsync_test`, then `ritz_fg_host_vsync` and `FrameGen_HostVsync()` (`fg-cap`; 1 needs Auto V-Sync on, the test ConVar and the verdict). While frame generation is active the auto-vsync part reads `frame generation: capped at the refresh` / `no cap (...)`.
- `limiter`: the user's own limit (as Auto V-Sync last saw it), the effective
  `g_nSteamCompMgrTargetFPS`, the two limiter-file words (on, pace fps) and the mode: `off`,
  `grid` (compositor holds FIFO commits to the vblank grid) or `layer` (the WSI layer paces).
  See [frame-limiter.md](frame-limiter.md).
- `auto-vsync`: the switch, then `limit N: <cause>` (`autovsync::LimitCause()`: `tearing is not
  requested`, `the host does not tear`, `gamescope draws every frame itself, one per refresh`) or
  `no limit: <first blocking condition>`, from
  `autovsync::Reason()` (`src/AutoVsync.h`) evaluated with exactly the inputs of the last
  `AutoVsync_Apply()`, so it cannot disagree with it: `switch off`, `not nested Wayland`,
  `VRR in use`, `frame generation or motion blur on`, `no refresh known`,
  `the host tears (verdict)`, `the user's own limit applies`.
- `fg`: frame generation on/off, `generating` / `passing through` while active; motion blur on/off.

## Line 2: judder

```
status: judder shown 125 (62.5/s) repeat 0 unknown 0 judged 125, step err mean 0.01 p99 0.12
max 0.13 ms, uneven 0.0/s, gap 62.5/s
```

For every `presented` feedback of the toplevel: the presented step (time since the previous
presented frame, from the feedback's timestamp) against the content step (game time between the
frame shown now and the one shown before). Pure arithmetic in `fgjudder::Meter`
(`src/FrameGen/Judder.h`, `tests/test_judder.cpp`).

- Content time: a native paint = the arrival of the base plane's game frame
  (`commit_t::present_time`, when its acquire fence signalled). A frame-generation paint = the
  pacer's decision: a pair at `t` between the two frames' arrivals, else the arrival of the real
  frame shown (the capture's `Arrivals()` table, `src/FrameGen/FrameGenCapture.h`); an id no longer
  in that table = unknown. A paint without a base plane = unknown.
- Travel: `paint_all()` stores it in `fghostshown::Get().ulPaintContentNs` right before
  `Present()`; `CWaylandPlane::Present()` queues it beside the commit's feedback object, and the
  `presented` / `discarded` handlers take their entry back (bounded at 64).
- `shown` = presented frames; `repeat` = judged frames with content step 0 (the same game frame
  again); `unknown` = without a content time (counted, not judged, nor is the step after one);
  `step err` = mean / 99th percentile / max of |content step - presented step|; `uneven` = per second
  with that error above half a refresh; `gap` = presented steps above 1.5 refreshes per second.
  Steps over 250 ms count as idle and are not judged. The refresh is the feedback's, else
  `g_nOutputRefresh`.
- Reading it: smooth = step err near 0, uneven 0. A stream beating against the host's refresh shows
  periodic uneven steps. `gap` is literal: with paints below the refresh rate every longer step counts
  even when gamescope simply had nothing new, so read it against `paints`.

**Limits.** Only the toplevel (plane 0) has feedback. In the direct-plane path (no full composite)
headless sway answered every toplevel commit with `discarded` (cause not established; plane 0 then
holds the black backing or the game's buffer, the game on a subsurface), so the judder line and the
verdict get nothing there; whether a real host does the same is untested. On headless sway async
commits are all `discarded` too. Measured on headless sway with `composite_force 1`; a real host was not tested.

## Line 3: timer (2026-11-23)

The vblank timer against the host's feedback: what decides the tick rate. Added for the field
test of "The refresh timer and the host's feedback"
([frame-generation-host-scanout.md](frame-generation-host-scanout.md)), kept as the timer's
diagnostics. Counters: `CVBlankTimer::TakeStats()` (`src/vblankmanager.cpp`, relaxed atomics),
the paint and commit counts in `StatusCounters_t` (`steamcompmgr.cpp`). Rates per second.

```
status: timer ticks 120.0 wakeups 120.0 short-gap 0.0 min-gap 8.16 ms target-short 0.0 missed 0.0
| paints tick 69.0 extra 1874.8 other 0.0 | lead measured 1.50 ms ring-max 0.68 max-draw 1.03 ms,
draws 1943.8 non-tick 0.0 (max 0.00 ms) skipped 0.0 | feedback 125.0 close 0.0 off-grid 123.0
phase mean +0.112 abs 2.062 ms rearm 2.0 ignored 123.0 | rearms 2.0 shift mean +0.066 ms earlier
0.0 later 0.0 overdue 0.0 kept 0.0 | retarget 2.0 | refresh 0.000 Hz | latency min 0.03 mean 0.31
max 0.78 ms early 125.0 same-buffer 0.0 | mark host 125.0 present-wait 0.0 drm 0.0 openvr 0.0 |
host commits 1943.8 no-new-buffer 0.0 | game commits new 2865.7 same-buffer 0.0 recommit 0.0
dropped 0.0 | arrivals new 2864.7 recommit 0.0 same-as-last 0.0 | switches draw_per_tick 0
feedback_anchor 1 grid_tolerance 1% one_per_vblank 1
```

(headless sway, nested 120 on a 120 Hz output, frame generation on; one line in the log.)

- `ticks`: refresh-timer ticks the main loop processed (the same count as line 1's `refresh
  timer`); `wakeups`: timerfd wake-ups.
- `short-gap` / `min-gap`: wake-ups less than 0.9 refresh after the previous one, and the
  smallest such gap (a lead that grew also shortens one gap, harmlessly).
- `target-short`: wake-ups aimed at a vblank less than 0.9 refresh after the previous one's (a
  real extra tick); `missed`: refreshes between two aimed vblanks that no wake-up served (a paint
  longer than a refresh, a postponed tick).
- `paints tick / extra / other`: paints started by a refresh tick, by frame generation's extra
  timer, or by anything else (async, forced, arrival).
- `lead`: the current wake-up lead and its kind; `ring-max`: the 60-draw maximum it is sized
  from; `max-draw`: the largest draw time reported in the interval.
- `draws`: draw times reported by the backends; `non-tick (max)`: those from a paint that was
  not the first after a wake-up, and their maximum; `skipped`: of those, dropped
  (`ritz_timer_draw_per_tick 1`).
- `feedback`: host `presented` events; `close`: less than half a refresh after the previous one;
  `off-grid`: further than `grid_tolerance` from the timer's grid; `phase mean / abs`: their
  signed (negative = early) and absolute distance from it.
- `rearm`: forced re-arms by an accepted feedback; `ignored`: feedbacks not taken as the anchor.
- `rearms`: forced re-arms of an armed timer (any source); `shift mean`: how far they moved its
  wake-up (+ = later); `earlier / later`: by more than 1 % of a refresh; `overdue`: the replaced
  wake-up was already due (a lost tick); `kept`: re-arms skipped because the wake-up was due.
- `retarget`: schedules that aimed at the vblank the last wake-up served (moved on when
  `one_per_vblank` is 1).
- `refresh`: the refresh the host's feedback reports (0 = none, read as host VRR).
- `latency min / mean / max`: presented time minus our commit time; a fixed-refresh host spreads
  it over a refresh, a host that shows frames at once gives small tight values. `early`: under a
  quarter refresh; `same-buffer`: feedbacks of commits that attached no new buffer.
- `mark`: `MarkVBlank()` calls by source (`present-wait` is the default source; its only caller is
  the Vulkan swapchain output's present-wait thread).
- `host commits`: toplevel commits that asked for feedback; `no-new-buffer`: of those, without an
  attach (plane 0 only, so a black backing plane counts every time).
- `game commits`: the focused game's commits: a new buffer, a buffer already queued
  (`same-buffer`), a re-commit (`commit_t::bRecommit`), or `dropped` (duplicate without feedback).
- `arrivals`: base-plane arrivals of the focused window by the same split (counted with frame
  generation off too); `same-as-last`: the same buffer as the previous arrival.
- `switches`: `ritz_timer_draw_per_tick`, `ritz_timer_feedback_anchor`,
  `ritz_timer_grid_tolerance`, `ritz_timer_one_per_vblank` (help strings in `vblankmanager.cpp`;
  the defaults are the fix, `0`/`0` for anchor and one-per-vblank restores the old behaviour).

Reading it: `ticks` should equal the refresh (the nested one when set) with `target-short` 0;
`feedback` mostly on the grid on a fixed-refresh host (`phase abs` microseconds), mostly off it
and `ignored` where the host shows frames at once or the nested refresh is not the host's.
