# Cursor pipeline: doubled pointer speed and the absolute notify

Part of [Cursor pipeline](cursor-pipeline.md). One channel per movement, and why the absolute notify stays unconditional for an unlocked client.

## The doubled pointer speed -- fixed 2026-08-28: one channel per movement

> **Superseded 2026-09-06.** The rule this section introduced -- relative motion only
> while LOCKED -- was the CS2 "mouse springs back to centre" bug and has been reverted
> to upstream's unconditional relative send; see "The device SDL3 remembers" at the end
> of this document. The "2.0x" below added the X sprite's travel to the raw deltas of the
> *same* movement: Xwayland never delivered anything twice (when a frame carries both
> channels it posts the absolute half with no raw event). Kept for the measurements and
> the history of the two failed attempts.

Reported repeatedly across 2026-08-27/28: with force-grab on, in-game look/aim
sensitivity is roughly doubled. Four rounds of hypothesis-driven patching failed before
the behaviour was pinned down by measuring what Xwayland actually asks for, rather than
assuming it.

**Mechanism.** `wlserver_mousemotion()` used to deliver every movement on **two channels
at once**:

1. `wlserver_perform_rel_pointer_motion()` -- a `zwp_relative_pointer_v1` event, sent
   *unconditionally*;
2. `wlr_seat_pointer_notify_motion()` -- the ordinary absolute `wl_pointer.motion`.

Xwayland republishes (1) as XI2 raw motion and (2) as the X sprite's position. A client
reading only one is fine. Several Wine/Proton raw-input paths read **both** and sum them:
exactly 2x.

### What Xwayland actually does -- measured, not assumed

Headless gamescope, `WAYLAND_DEBUG=1` on the whole tree, a purpose-built XI2 client
inside its Xwayland selecting `XI_RawMotion` on the root and optionally taking an
`XGrabPointer`. Motion injected with the `wlserver_debug_mouse_motion` ConCommand, which
enters at `wlserver_mousemotion()` itself -- the real path, and the reason this round saw
what earlier ones could not (`overlay_e2_pointer` bypasses `wlserver` entirely).

- **Xwayland binds `zwp_relative_pointer_manager_v1` and creates a
  `zwp_relative_pointer_v1` at seat setup, unconditionally**, before any X client exists.
  So an unconditional relative send always reaches it, always becomes XI2 raw motion, and
  is never simply ignored.
- **Xwayland requests `zwp_pointer_constraints_v1.lock_pointer` as soon as an X client
  takes an active pointer grab** (`XGrabPointer` with a confine window -- what Wine does
  for `ClipCursor`), and gamescope activates it (`zwp_locked_pointer_v1.locked` goes back
  out). While locked, Xwayland drives the X sprite from the *relative* channel itself.

So the two channels are genuinely meant to be exclusive, and Xwayland already tells us
which one it wants.

### The asymmetry that was the bug

`wlserver_apply_constraint()` implements one half of the exclusivity: a LOCKED constraint
returns false, and the absolute notify is skipped. **The mirror half was missing** --
relative motion went out whether or not anyone had locked. So an unlocked client received
the same movement on both channels.

`wlserver_mousemotion()` now sends relative motion **only while a LOCKED constraint is
active**, which is the exact complement of that early return:

| client state | relative | absolute |
| --- | --- | --- |
| LOCKED constraint | yes | no (`wlserver_apply_constraint` returns false) |
| CONFINED constraint | no | yes, clipped to the confine region |
| no constraint | no | yes |

Exactly one channel carries each movement, in every state.

**Why force-grab is what exposed it.** With force-grab *off*, the nested backends feed
absolute host motion into `wlserver_touchmotion()`, which never emits relative motion at
all. Switching force-grab on moves input to `wlserver_mousemotion()` and used to *add* a
second channel on top of the absolute one the client was already getting. The invariant
this restores is the one the 4583d6f post-mortem below already named: **force-grab must
not change what the client receives.**

### Measured, before and after

Headless gamescope, 20 injections of `dx=10` (200px), counted off the wire in the
`WAYLAND_DEBUG` log and read back with the XI2 probe plus `xdotool getmouselocation`.

| client state | build | `wl_pointer.motion` | `relative_motion` | sprite | XI2 raw values | delivered |
| --- | --- | --- | --- | --- | --- | --- |
| no X grab (unlocked) | before | 20 | 20 | +200px | `10 10 10 …` deltas | 400px -- **2.0x** |
| no X grab (unlocked) | after | 20 | 0 | +200px | `10 20 30 …` positions | 200px -- **1.0x** |
| `XGrabPointer` (locked) | before | 0 | 20 | +200px | `10 10 10 …` deltas | 200px -- 1.0x |
| `XGrabPointer` (locked) | after | 0 | 20 | +200px | `10 10 10 …` deltas | 200px -- 1.0x |

The raw *values* -- deltas versus accumulating positions -- are what distinguish the two
sources; summed magnitudes alone cannot, because Xwayland synthesises raw events from
absolute motion too. An earlier round of this investigation was misled by exactly that.

The locked column is unchanged, which is the safety check that matters: a grabbed game
keeps precisely the input it had.

### Known cost

An X client that reads XI2 raw motion **without** taking a pointer grab no longer gets
delta-valued raw events; it gets the absolute-derived raw stream and the sprite. That is
the same thing it gets with force-grab off, so nothing regresses relative to the
force-grab-off baseline -- but on the DRM/embedded path, where `wlserver_mousemotion()`
is the *only* pointer path, it is a real narrowing. It is accepted because every client
that actually wants relative-only input asks for it: Xwayland locks on grab (measured
above), and native Wayland games lock directly.

### Why this is an upstream bug, not fork divergence

Established by diff rather than by hypothesis:

- **`3.16.25` (`17baf4a`) is a direct ancestor of this fork's base `fcc1341`.**
  `git merge-base` returns `17baf4a` itself; `fcc1341` is `3.16.25` + 40 commits, with
  zero commits the other way. The installed `/usr/bin/gamescope` the user calls
  "working vanilla" is the *older* of the two, not a divergent branch.
- **The pointer-motion path is byte-identical across those 40 commits**, and the fork
  changes nothing on it beyond the overlay's capture/routing gate -- per-function
  comparison against `fcc1341` leaves `wlserver_apply_constraint()`,
  `wlserver_perform_rel_pointer_motion()`, `wlserver_mousewarp()` and
  `wlserver_update_cursor_constraint()` identical, and the fork adds no second motion
  channel.

What the fork really changes is that force-grab *engages*: upstream reads
`g_bForceRelativeMouse` once at backend startup, where
`CWaylandBackend::SetRelativeMouseMode()` early-returns if `m_pPointer` is still null,
and the per-frame path that would call it again is gated off by `!g_bForceRelativeMouse`.
This fork's issue-#68 fix (`steamcompmgr_set_force_relative_mouse()`, plus the
`debug_set_force_relative_mouse` ConCommand) re-pushes the mode at runtime, when the
pointer does exist.

**Why:** that makes the fork's contribution *exposure*, not breakage -- vanilla has no
runtime switch, so force-grab has probably never genuinely engaged there. The fix is
therefore shaped as something upstream would take: it completes upstream's own
relative/absolute exclusivity rule in `wlserver_mousemotion()`, using upstream's own
constraint state, with no fork-specific flag anywhere in the condition. In particular it
does **not** key off `g_bForceRelativeMouse` -- see the 4583d6f post-mortem for why that
flag can never appear in this decision.

### The two failed attempts, kept so they are not repeated

- **`4583d6f` (reverted)** withheld the *absolute* notify whenever `g_bForceRelativeMouse`
  was set. Detail below; the short version is that the flag describes gamescope's
  relationship with the host, not the client's input mode, so clients got no motion at
  all.
- **`63b6fed` (reverted)** gated relative motion on `wlserver.GetCursorConstraint()` being
  non-null. Right instinct, but it was shipped without a measurement through
  `wlserver_mousemotion()` and reverted as an unjustified divergence during the upstream
  audit. The current fix is the same instinct with the evidence attached, and narrowed
  from "any constraint" to "LOCKED" so that a CONFINED client does not end up on both
  channels.

## Why the absolute notify must stay unconditional for an unlocked client

`wlr_seat_pointer_notify_motion()` is **the only thing that moves the pointer for a client
that has not locked the pointer** -- which is every Xwayland client that is not in a
raw-input grab, so in practice most of what runs here. The one legitimate reason to
withhold it is a client that asked for relative-only input by locking, and
`wlserver_apply_constraint()`'s early return already covers exactly that case
(`WLR_POINTER_CONSTRAINT_V1_LOCKED` -> return false). There is no second condition to add.

### The 2026-08-27 regression (commit 4583d6f, reverted 2026-08-28)

`4583d6f` withheld the absolute notify whenever `g_bForceRelativeMouse` was set, on the theory that
force-grab implies the client reads relative motion only. **That theory is wrong**, and
this is the mistake to not repeat:

- `g_bForceRelativeMouse` (`--force-grab-cursor`, and the overlay's Force Grab Cursor
  switch) describes gamescope's relationship with the **host compositor**, not with the
  game. Its only real effects are `CWaylandBackend::SetRelativeMouseMode()` /
  `CSDLBackend` / `COpenVRBackend` asking the *host* to lock gamescope's own pointer and
  feed gamescope relative deltas, plus making `ShouldDrawCursor()` unconditionally true.
  It says nothing at all about how the focused client reads input.
- The stated root cause did not even match the gate: relative motion was sent whether or
  not force-grab was on, so if double-counting were real it would have been equally real
  with force-grab off. (It was -- for an unlocked client. The cure was to stop sending the
  *relative* channel to a client that never locked, not to stop sending the absolute one.)

**Symptom it caused**, as reported: with force-grab on, clicking appeared to hang the
compositor, after which "the image only updates when I click again". The mechanism is
that the client's pointer stopped moving entirely -- motion accumulated only in
gamescope's own `wlserver.mouse_surface_cursorx/y` (which drives the *composited* cursor,
so gamescope looked alive) and the client saw nothing until a button event flushed the
seat's pointer state, which is what made the picture appear to advance one step per click.

**Observed, not inferred.** Reproduced and then A/B-verified in a private headless sway,
with the numbers below. The technique is worth keeping: gamescope's own **libei socket**
(`$XDG_RUNTIME_DIR/gamescope-0-ei`, present whenever it is built with `input_emulation`)
routes `EIS_EVENT_POINTER_MOTION` straight into `wlserver_mousemotion()` and
`EIS_EVENT_BUTTON_BUTTON` into `wlserver_mousebutton()` (`src/InputEmulation.cpp`). A
~60-line libei sender is therefore a direct, headless, session-safe way to drive exactly
these functions -- no host pointer lock, no visible window, no `ydotool`. The client
pointer position was read back with `xdotool getmouselocation` on gamescope's own
Xwayland display.

| after ... (force-grab on) | 4583d6f | reverted |
| --- | --- | --- |
| 12 relative moves of (9,6) | `x:0 y:0` -- nothing delivered | `x:108 y:72` |
| one click | `x:108 y:72` -- jumps only now | `x:108 y:72` -- unchanged, correct |
| 12 more relative moves | `x:108 y:72` -- frozen again | `x:216 y:144` |

With force-grab **off**, the reverted build behaves identically to the force-grab-on
column, which is the invariant that matters: force-grab must not change what the client
receives.

**On the doubled-sensitivity report that motivated `4583d6f`:** it was never measured
before or after (that commit says so itself), and the fix it shipped could not have been
the right one for the reason above. If the report resurfaces, treat it as unexplained and
start from a measurement, not from this gate.
