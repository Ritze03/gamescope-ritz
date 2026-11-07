# Cursor pipeline: locked pointer and the SDL3 device

Part of [Cursor pipeline](cursor-pipeline.md). Never an absolute event under a pointer lock, and the device SDL3 remembers.

## Locked pointer => never an absolute event -- fixed 2026-09-06 (CS2 mouse look drifting to centre)

Report, verbatim: *"In CS2, my mouse did work in the menus, but ingame, it behaved like a
joystick almost (going back to center, looked almost like it would, when the mouse didnt hide
properly (constantly moving back to center))."*

### What a game in play is, to the compositor

A first-person game in play hides its cursor and warps it to the window centre (SDL's relative
mouse mode). Under Xwayland that becomes a **`zwp_locked_pointer_v1`** request to gamescope --
`handle_pointer_constraint()` / `wlserver_constrain_cursor()` -- and from then on the client reads
**relative motion only** (`zwp_relative_pointer_v1`, republished by Xwayland as XI2 raw motion).
The menus are the other state: cursor shown, no constraint, an absolute pointer. That is why the
report splits along menu/in-game.

### Why one absolute event is a "joystick"

Xwayland republishes **every** pointer event it gets as an XI2 raw event, and SDL's relative mode
reads raw valuators as deltas. Xwayland's pointer device has *absolute* axes, so a
`wl_pointer.motion` delivered to a locked client arrives at the game as a delta the size of the
**position**, not of any movement. Measured with `tests/pointer_lock_client.c` (a real SDL2
window in relative mode, inside gamescope's Xwayland): one absolute sample at the output centre
reached the client as `xrel=640 yrel=360`; the pre-fix binary delivered 6 of 6 absolute samples
to a locked client as `xrel=426 / 853` jumps. A stream of those, one per host sample, is a mouse
that "moves back to centre" on its own.

### The emitters

`wlserver_mousemotion()` has always dropped its `wl_pointer.motion` for a LOCKED constraint
(`wlserver_apply_constraint()` returns false) -- upstream behaviour, kept verbatim. But that is
only the relative path. **`wlserver_mousewarp()`** sends the same event from a different origin
and never checked the lock, in upstream or here:

- a **host absolute sample** (`wlserver_touchmotion()` -> `wlserver_mousewarp()`), which the
  nested backends keep delivering with force-grab off until the *host* itself has confirmed
  relative mode -- and always, if it never does;
- the item-10 **re-sync** above (`wlserver_resync_absolute_pointer()`, added 2026-09-05): a
  mapping change while the last input was an absolute sample replays that sample as a warp.
  Confirmed on the pre-fix binary: with the pointer locked, two scaler toggles and two nested-mode
  changes delivered 2 motion lines to the client (`build-release/verify-shots/pointer-regression/`,
  the `--gamescope gamescope-before-fix` runs). This is the fork's own addition and the one the
  2026-09-04/05 batch introduced; the host-sample path is upstream's, but it is the same class of
  event and the same fix closes it;
- the focus-change **warp to centre** (`bResetToCenter` / `bResetToCorner` in
  `determine_and_apply_focus()`), synthetic, upstream.

### The fix

One gate, at the emitter: `wlserver_mousewarp()` returns without moving or notifying when
`wlserver_pointer_is_locked()` (a LOCKED constraint on the seat). `wlserver_resync_absolute_pointer()`
bails on the same test first, so the *suppressed* counter says "re-sync reached and refused" rather
than "re-sync sent". The position is left where the lock froze it, exactly as the relative path
leaves it; on unlock `wlserver_warp_to_constraint_hint()` places the pointer from the client's
hint as before.

**Why the rule is absolute, not "only synthetic warps":** the protocol says a locked pointer
gets no motion events, full stop; the client has told us it is reading relative motion, and any
absolute event it gets is misread as movement. Gating only the synthetic warps would have left the
host-sample path open, which is the larger emitter whenever the host is slow to (or does not)
confirm relative mode. The one behaviour this removes that upstream had: a touchscreen tap on a
locked game (the Deck's DRM path) no longer jumps the pointer -- which was an absolute event to a
relative-only client, i.e. the bug, on a device this fork does not target.

**Why not per-frame, why exactly once:** the re-sync still fires only from
`update_touch_scaling()`'s change detection (the four cached floats vs the freshly painted
layer), so an unlocked client gets exactly one re-sync per real mapping change, none on a no-op
set, none at rest. `tests/test_pointer_mapping.cpp` pins the comparison and the two degenerate
cases (a same-aspect nested mode under Auto does not move the mapping; the output centre maps to
the window centre under every scaler) so the regression script's "exactly one" means something.

### Measured (`scripts/pointer-regression.sh`, 2026-09-06)

Headless (private sway + nested `--backend wayland`, the pixel-regression recipe), client
`pointer_lock_client --lock`, motion driven by `wlserver_debug_mouse_motion` /
`wlserver_debug_absolute_motion`, mapping changes by `overlay_e2_set display.filter.scaler` and
`steamcompmgr_debug_set_nested_mode`, counters from `wlserver_pointer_stats`:

| check | pre-fix binary | fixed |
| --- | --- | --- |
| 10 relative injections, locked | (lock flaky to detect without stats) | client saw 10, all `xrel=5` |
| 6 absolute samples, locked | client saw **6** (`xrel=426/853`) | client saw 0; `motions_locked=0` |
| 2 scaler toggles + 2 mode changes, locked | client saw **2** (the re-syncs) | client saw 0; `resyncs=0`, `warps_suppressed_locked` +4 (one per change) |
| unlocked: 1 sample, 2 s idle, Stretch, Stretch again, 1280x1024 | n/a | 1 / 0 / **1** / 0 / **1** re-syncs, matching client motions |

### Diagnosing it live

`gamescopectl wlserver_pointer_stats` prints `constraint=locked|confined|none`, whether the last
input was absolute, the mapping in use, and the counters. With a game in play: `constraint=locked`
and `motions_locked=0` is the rule holding. `warps_suppressed_locked` climbing while the mouse
moves means the **host** is still delivering absolute samples to a locked client -- i.e. the
nested backend was not put into relative mode (`bImageEmpty && bHasPointerConstraint` in
`steamcompmgr.cpp`'s paint loop) -- which is a different bug to chase, now visible instead of
felt. `relatives` (added with the next section) must never be lower than `motions`.

## The device SDL3 remembers -- fixed 2026-09-06, second attempt (CS2 "still broken")

The user, same day, after the section above shipped: *"Still broken. Same behavior."* The warp
gate was correct and is kept; it was not the emitter. Nothing absolute reaches a locked CS2 any
more -- the drift comes from how the **relative** deltas are read, and that is decided in the
**menu**, before the lock exists.

### What Xwayland does with the two channels

Xwayland (24.1, `hw/xwayland/xwayland-input.c`) has **two X slave pointer devices**:
`xwayland-pointer` with absolute axes (`Abs X`/`Abs Y`), fed by `wl_pointer.motion`, and
`xwayland-relative-pointer` with relative axes, fed by `zwp_relative_pointer_v1.relative_motion`
(bound unconditionally at seat setup). Both post through the master `Virtual core pointer`, and
**the master's axis classes are a copy of whichever slave posted last** (X's device-changed
mechanism). Per frame, `dispatch_pointer_motion_event()`:

| frame carries | raw XI2 event | sprite |
| --- | --- | --- |
| relative only | deltas, from the relative slave | moved by the delta only under a warp emulator (a hidden-cursor window) |
| absolute only | **positions, from the absolute slave** | set from the position |
| both | deltas, from the relative slave | set from the position, posted through the **relative** slave with `POINTER_NORAW` -- no second raw event |

So a compositor that sends both channels with every movement -- every wlroots host, Mutter, KWin,
and upstream gamescope's `wlserver_mousemotion()` -- keeps the master on the relative slave for
ever, and nothing is delivered twice. A compositor that sends absolute-only movements flips the
master to the absolute slave, and its raw events carry positions.

### What SDL3 does with the master

SDL3's X11 driver (`SDL_x11xinput2.c`) selects `XI_RawMotion` on the root for `XIAllMasterDevices`
at init, so raw events arrive with the *master's* device id in the menu as well as in play. On the
**first** raw event it sees it queries the master's classes (`xinput2_get_device_info`) and caches
`relative[axis]`; `XI_DeviceChanged` is commented out, so the cache is never refreshed. A relative
axis is passed through as a delta; an "absolute" axis is **differenced** (`value - prev_value`).
CS2 is an SDL3 game.

### The sequence, measured

`tests/pointer_grab_client_x11.c` -- native SDL3, `SDL_SetWindowRelativeMouseMode()`, an XI2 tap
through `SDL_SetX11EventHook()` printing every raw event with its device and the master's axis mode
as SDL3 saw it, and `--lock-after S` so it is a **menu first**. Pre-fix binary (`17:06`, the one the
user tested), 4 absolute samples then the lock then 10 x `dx=5`
(`build-release/verify-shots/pointer-regression/x11-explore-20260906-171610-cur-lockafter2/`):

```
DEVICE id=2 name=Virtual core pointer axis0=abs axis1=abs      <- cached during the menu
RAW dev=2 src=6 v0=469.0 v1=264.0 ... v0=597.0 v1=312.0        <- menu: absolute slave, positions
LOCKED
RAW dev=2 src=7 v0=5.0 v1=0.0   x10                           <- play: relative slave, deltas
MOTION x=0.0 y=0.0 xrel=-592.0 yrel=-312.0                     <- SDL: 5 - 597, 0 - 312; then 5-5=0, dropped
RAW dev=2 src=7 v0=0.0 v1=7.0   x5
MOTION xrel=-5.0 yrel=7.0                                      <- a change of direction = a kick back
RAW dev=2 src=7 v0=5.0 v1=0.0   x5
MOTION xrel=5.0 yrel=-7.0
```

Ten movements of 5 px became one jump of -592 px, then nothing; every change of direction a kick
the size of the previous delta, backwards. That is a mouse that "moves back to centre on its own"
while the physical mouse moves smoothly, in a match and not in the menus, with force-grab on **or**
off -- with force-grab on the menu's movements came from `wlserver_mousemotion()` which, since
`32b98bb`, sent them absolute-only; with it off they came from `wlserver_touchmotion()` ->
`wlserver_mousewarp()`, which always did.

**Why the 2026-09-06 gate could not catch it:** `pointer_lock_client --lock` locks before it ever
sees a pointer event, so the first raw event SDL3 saw was already a relative delta, the master was
cached as relative, and all ten `xrel=5` arrived. The order was the bug.

### The fix

Relative motion accompanies **every** movement, as it did upstream and as every real host does:

- `wlserver_mousemotion()`: `wlserver_perform_rel_pointer_motion()` is unconditional again
  (`32b98bb`'s "only while LOCKED" gate removed, the rest of the function untouched);
- `wlserver_mousewarp()`: before its absolute notify, a relative event carrying the **clamped
  travel** (`new - previous` surface position). Upstream never had this, and it is what makes the
  force-grab-off path -- host samples through `wlserver_touchmotion()` -- keep the master on the
  relative slave too. The travel rather than the raw host delta so that a client which integrates
  relative motion (Xwayland's warp emulator, for a hidden-cursor window) lands where the absolute
  event says. Synthetic warps (focus reset to centre, the item-10 re-sync) get it as well: a
  game's first pointer event is often gamescope's own warp to centre, and an absolute-only one
  would seed SDL3's cache with the absolute slave before the user ever touched the mouse.
- `wlserver_pointer_is_locked()` and the warp gate stay exactly as the previous section left them.
- `wlserver_pointer_stats` gains `relatives=`; the invariant is `relatives >= motions`.

Same client, same order, fixed binary (`x11-explore-20260906-171925-fixed-lockafter/`): `DEVICE id=2
... axis0=rel axis1=rel`, menu raw events from `src=7` (deltas), then 10 x `MOTION xrel=5.0` and 5 x
`yrel=7.0`, one per injection. Through the nested backend's **real host path** -- a
`zwlr_virtual_pointer_v1` mouse (`tests/virtual_pointer_tool.c`) plugged into the private sway,
force-grab off: host absolute samples in the menu, the client locks, gamescope locks the host
pointer, sway confirms, host relative motion -- 10 x `xrel=5.0` (`...-172527-fixed-host-fgoff/`);
force-grab on from the start, the same (`...-172550-fixed-host-fgon/`).

**Why this and not "refresh SDL's cache":** SDL3 is the game's; every SDL3 game on this host would
carry the same fault, and the host-side contract that makes it safe is the one every other
compositor already honours. The 08-28 concern -- a client reading raw deltas *and* the sprite --
cannot arise from these two channels: Xwayland posts the absolute half with `POINTER_NORAW` when
both are present. The one behaviour this gives back that `32b98bb` had removed: an X client
reading XI2 raw motion **without** a grab gets deltas again instead of positions -- which is what
it gets on every other compositor.

### Gate

`scripts/pointer-regression.sh` grew three checks (`x11-menu-lock`, `x11-locked-abs`, `x11-host`)
that replay this order with the SDL3 client, by ConCommand and through the virtual host mouse with
force-grab off then on, and assert the cached axis mode, the per-injection `xrel`, and
`relatives >= motions`. The pre-existing 20 checks are unchanged and still pass.

### What could not be measured here

CS2 itself (not installed on the desktop, anti-cheat and no GPU headroom on the laptop). The
reproduction is the same SDL3 X11 driver CS2 ships, in the same order, through the same Xwayland,
with the real host path exercised by a virtual pointer. If the user's match still drifts,
`gamescopectl wlserver_pointer_stats` during the match tells the two remaining stories apart: in
the menu `motions` and `relatives` advance together; in the match only `relatives` advances and
`motions_locked` stays 0. `relatives` not advancing while the mouse moves in the match means the
host is not delivering relative motion (the nested backend's lock was not confirmed by Hyprland);
`motions_locked` advancing means an absolute emitter this document does not know about.
