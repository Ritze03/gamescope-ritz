# Status line (`ritz_status`) and the judder measure

A diagnostic for the journal, added 2026-11-22. Not UI, no config key, no changelog entry.
`Why:` frame generation's status line (see
[frame-generation-host-scanout.md](frame-generation-host-scanout.md)) exists only while frame
generation or motion blur is on, so with both off nothing in the log said what the game asked for,
how gamescope painted it, what the host showed, or why [Auto V-Sync](display-general.md#auto-v-sync-displayauto_vsync)
did not limit. On a real desktop nobody watches the screen, so smoothness has to be a number.

## Switching it on

ConVar `ritz_status` (int seconds, default 0 = off), live:
`gamescopectl ritz_status 5` logs two lines every 5 s through the `framegen_pacing` log scope,
both starting with `status:` (grep for it). The first interval after switching it on is only a
baseline and prints nothing. `StatusLine_Tick()` in `src/steamcompmgr.cpp`, called once per
main-loop iteration; read-only, it changes no pacing or presentation.

## Line 1: state

Stable order, `|`-separated groups, values over the interval unless "now":

```
status: game 60.0 fps fifo 100% wants-async 0% (now fifo 1 async 0) | paints 62.5/s async 0.0/s
no-free-image 0.0/s | host sent 62.5 shown 62.5 discarded 0.0 zero-copy 0.0 torn 62.5 /s, feedback
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
  flag (on nested Wayland: the tearing hint `async`); `no-free-image` = paints dropped because the
  host held every output image.
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
  `ritz_host_vsync_test`, then `ritz_fg_host_vsync` and `FrameGen_HostVsync()` (`fg-cap`).
- `limiter`: the user's own limit (as Auto V-Sync last saw it), the effective
  `g_nSteamCompMgrTargetFPS`, the two limiter-file words (on, pace fps) and the mode: `off`,
  `grid` (compositor holds FIFO commits to the vblank grid) or `layer` (the WSI layer paces).
  See [frame-limiter.md](frame-limiter.md).
- `auto-vsync`: the switch, then `limit N` or `no limit: <first blocking condition>`, from
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
