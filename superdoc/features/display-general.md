# Display > General

The Display > **General** area (`display.general`, `src/Overlay/PanelDisplay.cpp`,
`RegisterGeneral()`) holds the switches a user flips mid-game, so it is registered first and sits above
the other Display areas. Its Summary line reads like `VRR off · tearing off`, plus
` · nested windows maximized` when that switch is on and ` · direct scanout blocked` when Block direct
scanout is on. Four switches persisted per profile in the `gamescope.*` config block and mirrored into a
live global, plus one global, Wayland-only switch (last row):

| Row id | Label | Config key | Live target | Launch flag (locks the row) |
|---|---|---|---|---|
| `display.adaptive_sync` | Adaptive sync (VRR) | `gamescope.vrr_enabled` | `cv_adaptive_sync` | `--adaptive-sync` |
| `display.allow_tearing` | Allow tearing | `gamescope.tearing_enabled` | `cv_tearing_enabled` | `--immediate-flips` |
| `display.low_latency_wakeup` | Low-latency wake-up | `gamescope.low_latency_wakeup` | `vblank_measured_lead` | none |
| `display.force_windows_fullscreen` | Force maximize nested window | `gamescope.force_windows_fullscreen` | `steamcompmgr_set_force_windows_fullscreen()` | `--force-windows-fullscreen` |
| `display.block_direct_scanout` | Block direct scanout | `overlay.diagnostics_block_scanout` in `global.json` (global, not per profile) | `Diagnostics_BlockScanout()` | none; shown on the Wayland backend only |

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

## Block direct scanout (`display.block_direct_scanout`)

Default Off. Config `overlay.diagnostics_block_scanout` in `global.json` (the key kept its early name
from when the switch sat in Diagnostics). On, gamescope itself keeps a nested Wayland host from
putting its window on a display plane. It applies live: the setter (`Diagnostics_SetBlockScanout()`,
`src/Overlay/Diagnostics.cpp`) stores an atomic, writes `global.json` and calls `force_repaint()`;
the Wayland present path reads only `Diagnostics_BlockScanout()` (an atomic) every frame, and turning
it off detaches the buffer on the next frame. Mechanism:
[backend-wayland.md](backend-wayland.md#block-direct-scanout-from-gamescope).

**Global, not per profile.** `Why:` the host setup belongs to the machine, not to a game. The row
carries the global config key via `.Key( "overlay.diagnostics_block_scanout" )` (like Filter game
profiles); the Shell's per-profile dot and "Reset to inherited" only know settings keys, so the row
shows neither.

**Wayland only.** The row has `.ShownWhen( Diagnostics_ScanoutApplicable() )` (the Wayland backend,
`HostMayScanOutOutputImages()`; `GS_RITZ_DIAGNOSTICS_ASSUME_WAYLAND=1` forces it for tests). On SDL,
DRM, headless and OpenVR it is neither on the sheet nor in the command palette. An SDL window on a
Wayland desktop gets no row either: the blocker exists only in `CWaylandConnector::Present()`, so
there is nothing to block ([overlay-shell.md](overlay-shell.md#inspector-only-switch)'s row-level hide).

`Why:` a user on GNOME (Mutter) saw the whole picture, menu included, scrambled into fine block noise
after tabbing out and back in, and we suspect (unproven) host direct scanout is involved; GNOME has no
reliable user switch for scanout, so the switch has to be on gamescope's side, both as a test and as a
workaround. The brief artifacts on entering and leaving fullscreen under Hyprland and GNOME are the
other reason ([TODO](../planning/TODO.md)). `Why Display > General:` the user asked for a normal GUI
feature rather than a Diagnostics extra; it first lived in the Inspector of Diagnostics' Direct scanout
row.

Diagnostics: while it is on the Direct scanout row reads "Blocked by gamescope" and nothing nags (no
toast, no yellow dot); see [diagnostics.md](diagnostics.md#block-direct-scanout).

**Cost:** with scanout blocked the host composites again, so with Frame generation the shown rate falls
back (about 160 uncapped against 261-272 with scanout). The host must offer
`wp_single_pixel_buffer_manager_v1`; without it the switch does nothing and logs once.

**Not verified:** whether Hyprland or Mutter actually refuses scanout with the extra surface needs a
live test. Hyprland's `solitaryBlockedBy` `OPAQUE, SURFACES` supports it there; for Mutter the
single-surface requirement is from memory, not checked.

## Related

- [resolution-and-refresh.md](resolution-and-refresh.md) — the Resolution area these switches were split from, and the nested resolution / refresh.
- [launch-option-lock.md](launch-option-lock.md) — the launch-flag locks on three of these rows.
- [input-general.md](input-general.md) — Force grab cursor, moved out of this area.
- [frame-generation-pacing.md](frame-generation-pacing.md) — what the wake-up lead means for frame generation.

## History

- **Nested window** (glossary detail): **Nested window** — a client window running *inside* gamescope's own embedded Xwayland session (an app or game window, addressed by `xwayland_ctx_t`/`steamcompmgr_win_t`) — not gamescope's own outer window, which **Nested mode** (running gamescope as a window inside a desktop session; defined in [`superdoc/meta/TERMINOLOGY.md`](../meta/TERMINOLOGY.md)) is about. The General area's "Force maximize nested window" toggle (`display.force_windows_fullscreen`, mirrors `--force-windows-fullscreen`) names this sense: it makes every such window open at the full nested canvas size regardless of its own requested size.
- **Low-latency wake-up** (glossary detail): **Low-latency wake-up** — the Display > General switch (`display.low_latency_wakeup`, config `gamescope.low_latency_wakeup`, default On, 2026-10-04) that sizes the vblank timer's *wake-up lead* — how long before each refresh gamescope wakes to draw — from the measured draw time (`max of the last 60 draws + 0.75 ms`, at least 1.5 ms, at most one refresh) instead of upstream's fixed-ish ~4 ms (`rollingMaxDrawTime + redZone`). Does not apply under VRR, which keeps upstream's fixed 2.70 ms lead whatever the setting (a measured VRR lead was tried 2026-10-06 and reverted the same day: it raced the feedback-driven re-arm and presented fewer, less even frames). Off = upstream's cautious timing, even with frame generation. Until 2026-10-04 this was frame-generation-only and not a setting.
