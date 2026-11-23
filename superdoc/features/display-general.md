# Display > General

The Display > **General** area (`display.general`, `src/Overlay/PanelDisplay.cpp`,
`RegisterGeneral()`) holds the switches a user flips mid-game, so it is registered first and sits above
the other Display areas. Its Summary line reads like `VRR off · tearing off`, plus
` · nested windows maximized` when that switch is on and ` · auto v-sync N` while Auto V-Sync
holds the game at N fps. Five switches persisted per profile in the
`gamescope.*` config block and mirrored into a live global:

| Row id | Label | Config key | Live target | Launch flag (locks the row) |
|---|---|---|---|---|
| `display.adaptive_sync` | Adaptive sync (VRR) | `gamescope.vrr_enabled` | `cv_adaptive_sync` | `--adaptive-sync` |
| `display.allow_tearing` | Allow tearing | `gamescope.tearing_enabled` | `cv_tearing_enabled` | `--immediate-flips` |
| `display.auto_vsync` | Auto V-Sync | `gamescope.auto_vsync` | `cv_ritz_auto_vsync` (`ritz_auto_vsync`) | none |
| `display.low_latency_wakeup` | Low-latency wake-up | `gamescope.low_latency_wakeup` | `vblank_measured_lead` | none |
| `display.force_windows_fullscreen` | Force maximize nested window | `gamescope.force_windows_fullscreen` | `steamcompmgr_set_force_windows_fullscreen()` | `--force-windows-fullscreen` |

Block direct scanout and Compress output images lived here until 2026-11-08; they moved to the
Compatibility area ([diagnostics.md](diagnostics.md)) with their history. General is one column of
"Quick toggles" again.

The locks are the `Adaptive sync`, `Allow tearing` and `Force maximize` rows of the flag table in
[launch-option-lock.md](launch-option-lock.md). `Force grab cursor` used to be a fifth switch here; it
moved to `input.general` on 2026-09-27 ([input-general.md](input-general.md)).

## Adaptive sync (VRR) (`display.adaptive_sync`)

Default Off. Writes `gamescope.vrr_enabled` and `cv_adaptive_sync` (`src/steamcompmgr.cpp`) through
`ApplyEdit()`; `main.cpp`'s startup apply and `PushCachedSettingsToLiveState()` re-push it on startup
and on a profile switch, unless `--adaptive-sync` was given (`LaunchOptions::Opt::AdaptiveSync`, which
locks the row). The help text: matches the screen's refresh rate to the game; needs a screen and cable
that support VRR (FreeSync or G-Sync). The ConVar is a *preference*: steamcompmgr publishes it as the
`GAMESCOPE_VRR_ENABLED` root-window property, and an external tool writing that property sets it back,
but it is not kept in sync with whether the backend is currently using VRR (the DRM backend sets the
CRTC's `VRR_ENABLED` only when the connector is VRR-capable and `frameInfo->allowVRR` is set). It feeds `frameInfo.allowVRR` and the flip-type choice (Normal / Async /
VRR) in `steamcompmgr.cpp`. The Frame limiter and Low-latency wake-up behave differently under VRR; see
[frame-limiter.md](frame-limiter.md) and the section below.

## Allow tearing (`display.allow_tearing`)

Default Off. Writes `gamescope.tearing_enabled` and `cv_tearing_enabled` (`src/steamcompmgr.cpp`); a
`--immediate-flips` flag (`LaunchOptions::Opt::ImmediateFlips`) wins and locks the row. The help text:
shows new frames the instant they are ready instead of waiting for the screen; more responsive, but
fast camera movement can show a faint horizontal line. It only takes effect when the backend
supports tearing (`IBackend::SupportsTearing()`) **and** the focused surface asked for an async flip
(`bSurfaceWantsAsync`, from the game's present mode); a compositing frame is always a sync flip. The
`GAMESCOPE_ALLOW_TEARING` root-window property writes the same ConVar. Under the Wayland backend with
frame generation the host tearing hint has its own rules, in
[backend-wayland.md](backend-wayland.md#tearing) and [frame-generation-pacing.md](frame-generation-pacing.md).

## Auto V-Sync (`display.auto_vsync`)

**Also gates frame generation's refresh cap (2026-11-22).** `FrameGen_HostVsync()` =
`autovsync::FrameGenCap( ritz_auto_vsync, ritz_fg_host_vsync, verdict "host vsync" )`: on a nested
Wayland host found not to tear, frame generation's output is held at the refresh only while Auto
V-Sync is on (and the test ConVar `ritz_fg_host_vsync`, default 1, is not 0). With the switch off
(the default) frame generation runs to its target as before the 2026-11-21 cap. `Why:` the user's
request: one switch for "do not send more than the refresh while the host cannot show more", and
off by default because on their desktop the uncapped output was fine. When the tearing hint is not
requested nothing changes (`FrameGen_CanExceedRefresh()` is untouched). The `status:` line's
auto-vsync part, while frame generation is active, reads `frame generation: capped at the refresh`,
`... no cap (the host tears)` or `... no cap (test switch off)` (`autovsync::FrameGenText()`); the
limiter itself still applies no limit then.

Added 2026-11-22, directly after Allow tearing. **Default Off**, config key `gamescope.auto_vsync`
(per profile, additive, absent = false, in the `display.general` export group), mirrored into the
ConVar `ritz_auto_vsync` the same plain way as Low-latency wake-up (`main.cpp` at start-up,
`PushCachedSettingsToLiveState()` on a profile switch), so a test can flip it with `gamescopectl`.

While on, the game is limited to the refresh **while the output cannot show frames faster than the
refresh**, and the limit lifts as soon as it can. The rule is the pure
`autovsync::EffectiveLimit()` (`src/AutoVsync.h`, unit-tested in `tests/test_limiter_pace.cpp`):

- Only on the nested Wayland backend (`FrameGen_IsNestedWayland()`), not under VRR, and not while
  frame generation or motion blur is on (`fghost::Active()`). `Why` not with frame generation: it
  makes the limiter pace on the layer's timer (not locked to the host's refreshes, so it would beat
  anyway); the same switch instead gates frame generation's own refresh cap (below). Other backends: no effect (a `ponytail:` note names DRM without tearing
  and SDL as the upgrade path).
- "Cannot show faster", any one of (the first that holds is the cause, `autovsync::LimitCause()`):
  1. Allow tearing is off (or the backend cannot tear);
  2. the host was found to show our tearing-hinted commits on its vblank anyway (the raw verdict
     `fghostshown::Get().bHostVsync`, see [frame-generation-pacing.md](frame-generation-pacing.md),
     "A host that does not tear"; deliberately not `FrameGen_HostVsync()`, whose
     `ritz_fg_host_vsync` is frame generation's test switch);
  3. **gamescope composites its paints** (2026-11-22): `autovsync::CompositeGate`, fed after every
     paint with "this paint can never flip async, whatever the game asks": the backend composited
     it (`GetVBlankTimer().WasCompositing()`, set from `bNeedsFullComposite` after each Present) or
     an overlay window was up (`bHasOverlay`). Both are the main loop's own reasons to force a
     `Normal` flip and neither depends on the game's present mode, which the limit itself turns to
     FIFO. Left out: `nIgnoredOverlayRepaints` and a fade (one-off paints; the debounce covers them).
     Debounce: on once such paints kept coming (no gap over 500 ms) for 250 ms, so a lone
     composited paint never engages it; off once none came for 500 ms. The limit and its log line
     therefore do not toggle per paint. Measured headless (sway, 120 Hz, vkcube, tearing on):
     `composite_force` 1 -> 0 -> 1 live gives exactly one log line per change, the game at the
     tick rate under the limit (`limiter ... grid`, divisor 1) and about 1500 fps without it. At
     start-up the first paints composite before the game's window can be scanned out directly, so
     a game that is not composited later shows one engage / lift pair shortly after it starts.
     `Why:` found in the field 2026-11-22: with scaling or a Shaders effect on (also the HUD, the
     Crosshair, the settings overlay, ReShade, a colour slider, HDR ITM), every native paint is
     composited, and a composited paint is never flipped async (`WasCompositing()` in the `bTearing`
     branch). gamescope then paints once per vblank-timer tick and flips in sync, so it never sends
     more than one frame per refresh, whatever the host would do with a torn frame; game frames above
     the refresh are dropped inside gamescope, the very beat this switch removes. The hint never
     went async, the verdict never sampled and stayed at "tearing", and the switch did nothing (a
     game at about 220 fps on 280 Hz with tearing on). The host's verdict is irrelevant in that
     state.
- The refresh is the one the vblank timer ticks at (`g_nNestedRefresh`, else `g_nOutputRefresh`),
  the value the limiter's main-loop code and frame generation read.
- A user limit at or below the refresh is left alone; no limit or one above the refresh becomes the
  refresh. Applied in `update_app_target_refresh_cycle()` (`steamcompmgr.cpp`) as a runtime overlay on
  `g_nSteamCompMgrTargetFPS` after it was computed from the user's own limit, so the stored
  `gamescope.fps_limit`, the stepper, `--framerate-limit`, the `gamescope_control` protocol and
  `debug_set_fps_limit` are never rewritten; when it lifts, the user's limit (or none) applies again.
  Also re-evaluated on every vblank, so it follows the verdict while nothing paints.
- The tearing hint stays `async` while Auto V-Sync holds the game on a host that does not tear
  (`bAutoVsyncAsync` beside `paint_all()`): the held game reports FIFO, so without this the hint would
  turn to vsync, the verdict would stop sampling and the limit could never lift when the host starts
  to tear (e.g. the window made fullscreen). Overlay up or a fade: vsync, as for every hint.
  Only for cause 2 (`s_eAutoVsyncCause == Cause::HostVsync`): a limit held because gamescope
  composites has nothing to sample, and its composited paints stay the sync flips they always were,
  so what is sent to the host does not change. The verdict is fed only while gamescope does not
  composite and the unlimited game asks for async: then cause 3 lifts after its debounce, the paints
  flip async, the verdict samples, and causes 2 / none decide as before. A vsynced game (FIFO by its
  own choice) on a host that does not tear, with nothing composited, still never feeds the verdict:
  it is limited by its own vsync then anyway.
- One log line per change of state, e.g. `auto v-sync: limiting the game to 280 fps (the host does
  not tear)` / `(tearing is not requested)` / `(gamescope draws every frame itself, one per
  refresh)` and `auto v-sync: limit lifted (the host tears and gamescope no longer draws every frame
  itself)`.
- Why it does not limit right now: the `auto-vsync` field of the `status:` line (`ritz_status`,
  [status-line.md](status-line.md)) names the first blocking condition (or, while it limits, the
  cause), its `composite N% gate N` the share of composited paints and the debounced state, and its
  `verdict samples` whether the verdict is fed at all.

The limit equals the refresh, so the existing limiter keeps it **on the vblank grid** with divisor 1
(see [frame-limiter.md](frame-limiter.md)): a Vulkan game through our layer is told FIFO and the
compositor releases one held commit per vblank; the vblank timer takes the host's `presented`
times as its anchor when they lie on its grid, so it stays phase-locked to a fixed-refresh host
(and free-runs on its own grid where the host has none; see "The refresh timer and the host's
feedback" in [frame-generation-host-scanout.md](frame-generation-host-scanout.md)). A game without our layer (OpenGL through
Xwayland) gets nothing from divisor 1 (frame callbacks are sent every vblank anyway), so Auto V-Sync
does not limit it; not tested.

Field, 2026-11-22 (a DirectX 12 game through Proton in the Steam runtime container): the limit
reached the game only after two fixes of the same day, the layer's activation inside the container
([vk-wsi-layer.md](vk-wsi-layer.md)) and the limiter file's location
([frame-limiter.md](frame-limiter.md)); before them no limit of any kind did. With both, a
layer-paced limit of 100 held 100.0 fps. The grid limit follows the vblank timer, which ran fast in
that session (nested 180 on a 280 Hz host), so Auto V-Sync's 180 held 206 fps. Fixed 2026-11-23:
the host's feedback no longer re-anchors a grid whose refresh is not the host's ("The refresh
timer and the host's feedback" in [frame-generation-host-scanout.md](frame-generation-host-scanout.md);
headless, nested 90 on 120 Hz: 90.0 ticks/s). Not yet re-measured in the field.

`Why:` the user's request (2026-11-22): on a desktop that does not tear for gamescope's window
(some desktops tear only a fullscreen window), native game frames above the refresh beat against the host
and stutter; frame generation's own cap does not cover them. It must be automatic because other
desktops (and the same desktop with the window fullscreen) do tear, and there the limit would only
cost frames.

Tested headless only (sway, 120 Hz, vkcube): switch on with tearing off holds the game at 120; with
tearing on and the verdict forced to vsync (test ConVar `ritz_host_vsync_test`, `WaylandBackend.cpp`;
headless sway sends no `presented` event for async-hinted commits, so the real verdict cannot form
there) it holds 120 and the hint stays async; forcing the verdict back to tearing lifts it. Not tested:
a real host that reports vsync, 280 Hz, a real game.

## Force maximize nested window (`display.force_windows_fullscreen`)

Tracker: `../planning/requests-2026-09-08.md` item 3. A Quick toggles switch in
`display.general` (`src/Overlay/PanelDisplay.cpp`, alongside Adaptive sync / Allow
tearing / Force grab cursor), persisted as `gamescope.force_windows_fullscreen` and
mirroring the pre-existing `--force-windows-fullscreen` CLI flag (upstream's own, unused
by this fork's UI until now). See TERMINOLOGY.md's **Nested window** entry for what
"nested window" means here — a client window inside gamescope's own Xwayland session, not
gamescope's own outer window (**Nested mode**).

**What it actually does.** `xwayland_ctx_t::force_windows_fullscreen` (`steamcompmgr.cpp`)
is read in exactly two places: `determine_and_apply_focus()`'s `win_has_game_id()` branch
(a focused Steam-tracked game window) and `handle_desktop_window()` (every other mapped
top-level window). Both already treat "is fullscreen" and "the flag is set" identically —
either resizes the window to the nested canvas (`root_width x root_height`) instead of the
size the window itself requested (`sizeHintsSpecified` / `requestedWidth/Height`). The
toggle changes nothing about *how* those two functions resize a window; it only supplies
the boolean they already read.

**Genuinely live, not startup-only** — determined by reading every consumer before
wiring the row, per this feature's own precedent (issues #25 and #68 both shipped a
control that rendered and silently did nothing). Two things made this the small, safe
case rather than the "label it startup-only" case:

1. The flag already had a live path *upstream never used from this UI*: an external tool
   writing the `GAMESCOPE_FORCE_WINDOWS_FULLSCREEN` root-window X11 property is caught by
   a `PropertyNotify` handler (`steamcompmgr.cpp`) that writes the ctx's flag and calls
   `MakeFocusDirty()`.
2. `MakeFocusDirty()` bumps a serial every `global_focus_t` checks once per frame
   (`focus_t::IsDirty()`), on the very same steamcompmgr thread `PanelDisplay_Draw()`
   itself runs on (see this file's own "Thread safety" header comment) — so a dirtied
   focus reaches `determine_and_apply_focus()` the next frame, for every focused window,
   the exact same effect the atom path gets from an external tool.

So `steamcompmgr_set_force_windows_fullscreen( bool )` (new, `steamcompmgr.cpp`/`.hpp`)
just does in-process what the atom handler does for an external one: sets every currently
live Xwayland ctx's flag (there is normally one; more under `--xwayland-count`) and calls
`MakeFocusDirty()`. `steamcompmgr_get_force_windows_fullscreen()` reads ctx 0 back for the
row's own state and the area's Summary line. Measured (headless nested gamescope, a raw
Xlib client with fixed `WM_NORMAL_HINTS` so it takes the `handle_desktop_window()` branch):
toggling the row while that same client keeps running moves it between its own requested
size and the nested canvas size with no relaunch — the untracked `force-maximize-2026-09-08` capture run (`results.txt`).

**CLI still wins over config, the same way `-w/-h/-r` beat `nested_width/height/refresh_hz`
— but the mechanism differs because this flag's own CLI parse lives somewhere else.**
`-w/-h/-r` and their config fields are both read inside `main.cpp`, so "config first, CLI
after" is one function's own call order. `--force-windows-fullscreen` has never been
parsed there: it is parsed inside `steamcompmgr_main()`'s *own*, separate
`getopt_long()` pass over the same `argv` (`steamcompmgr.cpp`) — a second, independent
parse that several long-only flags this fork's UI doesn't touch already go through. So
the config value cannot be written straight into a live global the way `g_nNestedWidth`
is; instead `apply_ritz_config_to_startup_state()` (`main.cpp`, still runs before *both*
getopt passes) seeds a small bridge global, `g_bForceWindowsFullscreenStartup`, and
`steamcompmgr_main()`'s own local `bForceWindowsFullscreen` (previously always
`false`-initialized) now seeds from it before its getopt loop runs. Since the flag is
`no_argument` (`--force-windows-fullscreen` can only ever set it *true*, there is no
`--no-force-windows-fullscreen`), an explicit CLI flag still forces the local variable
`true` unconditionally regardless of what the config seed was — the same one-directional
"CLI wins" guarantee, just enforced across two getopt passes instead of one. Verified
(same headless run): config `false` + `--force-windows-fullscreen` on the command line
still starts the probe client maximized.

Also live-applied on every non-startup config reload path that already carries
`force_grab_cursor` (`ritz_apply_config_live()` in `main.cpp`, and, since 2026-09-27,
`PanelInput.cpp`'s own `EnsureConfigLoaded()` — the row moved out of `display.general`
to `input.general`/`PanelDisplay.cpp` to `PanelInput.cpp`, see
[input-general.md](input-general.md), and `PanelDisplay.cpp`'s
`PushCachedSettingsToLiveState()` no longer carries this field), so a profile switch or
a per-game config swap while running picks it up immediately, not only at the next
launch.

**Re-pinned to the origin (2026-11-02).** `determine_and_apply_focus()`'s placement
(`!w->placed` once-only move of a root-sized focus window to 0,0, upstream `8e72b0a`)
now also runs every pass while this flag is set. `Why:` X confines the pointer to the
root, so a root-sized window sitting off the origin has an unreachable strip; a client
(Minecraft/GLFW) that moves its window after we resized it keeps the same size, so
`placed` stayed true and nothing corrected it (user: "the mouse cant reach all of the
space"). Cause inferred from code, not reproduced live.

## Low-latency wake-up (`display.low_latency_wakeup`)

Added 2026-10-04. Display > General switch, **default On**, config key
`gamescope.low_latency_wakeup` (per profile, like VRR / Allow tearing / Force maximize; additive,
schema stays 5, absent = true). It mirrors into the `vblank_measured_lead` ConVar (the same
plain-mirror pattern as `cv_tearing_enabled`: `ApplyEdit` writes config + ConVar, and
`main.cpp`'s `ritz_apply_config_live` / `PanelDisplay.cpp`'s `PushCachedSettingsToLiveState`
re-push it on startup and profile switch). There is no launch flag, so no launch lock; the
ConVar stays usable from the debug console for a session-only override until the next profile
push.

`CVBlankTimer` (`src/vblankmanager.{hpp,cpp}`) wakes ahead of each vblank by an offset.
- **Off (upstream)**: `offset = rollingMaxDrawTime + redZone`; the draw time is seeded at 3 ms and
  floored at 2.4 ms while compositing, the red zone is 1.65 ms on internal screens (nested
  mode included), so a compositing frame wakes about 4.05 ms ahead, clamped to one refresh
  (3.57 ms at 280 Hz).
- **On**: `offset = min(max(M, 0.75 ms) + 0.75 ms, one refresh)` where `M` is the largest of the
  last 60 wake-up -> present draw times (`UpdateLastDrawTime()`; 3 ms assumed until one is
  measured) -- `CVBlankTimer::MeasuredLead()`, a pure static, unit-tested. A 1.1 ms worst draw
  gives 1.85 ms. The 0.75 ms floor on `M` keeps the lead at **1.5 ms minimum**: a direct-scanout
  frame measures tiny draws, and a lead sized from those would miss the first refresh after an
  overlay or composite appears. Applies with or without compositing and with or without frame
  generation, motion blur or the lag buffer.
- **VRR** (On or Off): upstream's fixed lead (compositing floor 2.4 ms while compositing +
  0.3 ms flush = 2.70 ms), whatever the draws cost; the switch does not apply under VRR. The
  status line says `(measured, ...)` only when the timer really used the measured lead, else
  `(vrr)` or `(default)` (`CVBlankTimer::GetLastLeadKind()`). `Why:` the measured lead under
  VRR was tried 2026-10-06 (`e8e6424`) and reverted the same day. At 280 Hz with max draws of
  3-6 ms it sat at the full-refresh cap (3.57 ms) 99% of the time; under VRR the timer is
  re-armed from each presentation feedback (`WaylandBackend.cpp`), and a full-refresh lead
  makes the next wake "feedback + one refresh", which races the preemptive re-arm. A timing
  model over 16 host-latency combinations gave 257.5 fps presented with 9.8% of frames longer
  than 1.5 refresh intervals, against 272.6 fps and 2.6% with the 2.70 ms lead; the user's
  session measured 250 +/- 19 presented, matching the measured-lead rows. User report: "it's
  even worse than before ... sometimes the FPS I'm actually achieving are even lower ...
  less smooth ... more prone to stutters". Without VRR both leads cap at 3.57 ms at 280 Hz,
  so only VRR differed.

**Trade-off** (stated in the help text): a draw that suddenly takes longer than the recent
maximum misses the refresh, so the previous image shows once more (one stutter); the window is a
rolling max, so it widens at once and narrows only after 60 clean draws. Valve's cautious values
were tuned mainly for the Steam Deck. `Why` default On: the user's priority is latency, and the
lead is delay no later stage can recover -- a real frame landing inside it is shown one refresh
late (see [frame-generation.md](frame-generation.md)'s Latency section for the headless
numbers: 4.05 -> about 1.9 ms). A read-only **Wake-up lead** Facts row under the switch shows the
current lead and the recent max draw; the frame-generation debug status line carries the same
text.

**The lead and the timer's grid (2026-11-23).** The lead only places the wake-up *before* a grid
point (anchor + whole periods); it does not set the tick rate. Because the measured lead changes
from one draw to the next, a lead that shrinks after a paint by more than that paint took would
put the next wake-up before the *same* vblank again (the tick's paint reports its draw time before
the tick's own re-arm): a second tick for one refresh. `ritz_timer_one_per_vblank` (default on)
makes every schedule aim at least half a period after the vblank the last wake-up served. `Why:`
on a 280 Hz host this gave 2-65 double ticks/s, more the more the lead moved (field, 2026-11-23).
Which host feedback may anchor the grid is in "The refresh timer and the host's feedback",
[frame-generation-host-scanout.md](frame-generation-host-scanout.md). `ritz_timer_draw_per_tick`
(default off) lets only the first paint after a wake-up feed the draw window: an async paint
without frame generation otherwise measures "time since the last wake-up", up to a whole period,
which pins the lead at the cap (seen headless, 8.35 ms samples at 120 Hz; not seen on a real host,
whose async paints were all wake-up paints).

## Related

- [resolution-and-refresh.md](resolution-and-refresh.md) — the Resolution area these switches were split from, and the nested resolution / refresh.
- [launch-option-lock.md](launch-option-lock.md) — the launch-flag locks on three of these rows.
- [input-general.md](input-general.md) — Force grab cursor, moved out of this area.
- [frame-generation-pacing.md](frame-generation-pacing.md) — what the wake-up lead means for frame generation.

## History

- **Nested window** (glossary detail): **Nested window** — a client window running *inside* gamescope's own embedded Xwayland session (an app or game window, addressed by `xwayland_ctx_t`/`steamcompmgr_win_t`) — not gamescope's own outer window, which **Nested mode** (running gamescope as a window inside a desktop session; defined in [`superdoc/meta/TERMINOLOGY.md`](../meta/TERMINOLOGY.md)) is about. The General area's "Force maximize nested window" toggle (`display.force_windows_fullscreen`, mirrors `--force-windows-fullscreen`) names this sense: it makes every such window open at the full nested canvas size regardless of its own requested size.
- **Low-latency wake-up** (glossary detail): **Low-latency wake-up** — the Display > General switch (`display.low_latency_wakeup`, config `gamescope.low_latency_wakeup`, default On, 2026-10-04) that sizes the vblank timer's *wake-up lead* — how long before each refresh gamescope wakes to draw — from the measured draw time (`max of the last 60 draws + 0.75 ms`, at least 1.5 ms, at most one refresh) instead of upstream's fixed-ish ~4 ms (`rollingMaxDrawTime + redZone`). Does not apply under VRR, which keeps upstream's fixed 2.70 ms lead whatever the setting (a measured VRR lead was tried 2026-10-06 and reverted the same day: it raced the feedback-driven re-arm and presented fewer, less even frames). Off = upstream's cautious timing, even with frame generation. Until 2026-10-04 this was frame-generation-only and not a setting.
