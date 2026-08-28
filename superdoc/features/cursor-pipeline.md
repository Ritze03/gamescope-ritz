# Cursor pipeline

What gets drawn as "the mouse pointer," where that decision is made, and the two
invariants that block the obvious-looking shortcuts (a live host cursor under a
pointer lock; touching a cursor from the wrong thread).

**gamescope draws a pointer only for its own settings overlay.** That pointer is a
plain triangle, accent-coloured outline, black inlay, defined in
`src/Overlay/CursorArt.cpp`. **While the overlay is closed gamescope does not touch
the cursor at all** -- the game keeps whatever it or the host set, exactly as
upstream does. Getting to that took three attempts; see "Three attempts at the
overlay pointer" below for what each cost.

## Three cursor sources, one rule: exactly one on screen

`src/CursorPolicy.h` names them and states gamescope's own invariant plainly:

1. **The composited cursor plane** (`MouseCursor::paint()`, `src/steamcompmgr.cpp`) --
   gamescope's own Vulkan layer, drawn from whatever the current X11 cursor image is
   (`XFixesGetCursorImage()`, re-read every frame in `MouseCursor::getTexture()`).
2. **The overlay's own cursor** -- drawn into the settings overlay's own texture
   (`SettingsOverlay.cpp`) as vector geometry by `src/Overlay/CursorArt.cpp`, and
   drawn *only while the overlay is open*. ImGui's built-in software cursor
   (`ImGuiIO::MouseDrawCursor`) is never used; it is set false at init *and* per
   frame so nothing can switch it back on and put two pointers on screen.
3. **A nested backend's real host-level cursor** -- `SDL_SetCursor()` /
   `wl_pointer_set_cursor()`, drawn by the *host* compositor, not gamescope.

`NestedHostCursorUsable()`/`OverlayShouldDrawSoftwareCursor()` in that header are the
one place that picks between (2) and (3) for the overlay; **the invariant matters
more than which one wins: exactly one cursor is visible, and never zero.**

## Why "the default cursor while force-grab is on" isn't a substitution

`force_grab_cursor` (`g_bForceRelativeMouse`) does **not** swap in a different cursor
image. `ShouldDrawCursor()` just stops deferring to the nested backend's own
host-cursor logic and unconditionally composites source (1) instead -- which shows
whatever the *actual* current X11 cursor is: the game's own image when it has set
one, or gamescope's fallback root-window cursor when it hasn't. That fallback is set
once, at Xwayland-ctx creation, and is **upstream's code unmodified**: a live
snapshot of the real host cursor (`GetX11HostCursor()`, nested mode with an outer
X11 display to snapshot from), else `setCursorImageByName("left_ptr")` through
libXcursor. **A window that later calls `XDefineCursor` always wins** (ordinary X11
cursor inheritance), so none of this overrides a game's own cursor.

This fork briefly replaced that fallback -- first with a forced Xcursor-theme lookup,
then with its own rasterised triangle -- so that the pointer would look the same with
the overlay open and closed. Both were removed on 2026-08-28 at the user's request:
*don't override the game's cursor at all*. The agreement the earlier versions were
chasing is not worth taking over something the game and the host own.

## Why "a live system cursor while grabbed" is impossible, not just nested-only

This was the actual fork investigated for this feature, and the honest answer is
narrower than "only exists in nested mode": `CursorPolicy.h`'s
`NestedHostCursorUsable()` hard-requires the pointer be **unlocked** to show a real
host cursor, with the reasoning stated right there -- "a pointer constraint is
active (a grabbed game). The host cursor is then pinned and hidden by the host, so
it would not track anything even if we asked for it." Force-grab's actual mechanism
*is* that pointer lock (`SetRelativeMouseMode(true)` → `SDL_SetRelativeMouseMode` /
Wayland `zwp_pointer_constraints_v1_lock_pointer`). So a live, tracking system
cursor is impossible for the entire time force-grab is on, in nested mode too -- the
host OS/protocol itself hides and pins it as part of what "relative/locked mouse
mode" means. This is why force-grab delivers a **fallback image** of our own, not a
live host pointer -- the latter genuinely cannot coexist with the lock, by design,
not by omission.

## Threading: MouseCursor is steamcompmgr-thread-owned -- don't touch it from the other one

Per [architecture/overview.md](../architecture/overview.md#threading-model), gamescope
runs two long-lived threads: the **main thread** (named `"gamescope-wl"`, runs
`wlserver_run()`, dispatches Wayland protocol requests -- including
`gamescope_private_execute()`, the handler `gamescopectl` calls through), and the
**steamcompmgr thread** (owns X11 window management and the render/present loop,
including every per-frame cursor read: `getTexture()`, `paint()`).

Before this feature, every write to a `MouseCursor`'s Xlib/Vulkan state (its X11
`Cursor` resource, its `CVulkanTexture`, and its plain -- non-atomic -- `m_dirty`/
`m_imageEmpty` bookkeeping) happened exclusively on the steamcompmgr thread: once at
ctx creation (`init_xwayland_ctx()`, itself always called from that thread) and every
frame after. **First draft of this feature broke that invariant**: routing the
sub-option's live effect straight through `steamcompmgr_set_force_relative_mouse()`
made `ApplyDefaultCursorPolicy()` call into `MouseCursor::setCursorImageByName()`
directly -- fine when reached from the overlay's own Switch (steamcompmgr thread, per
`PanelDisplay.cpp`'s own threading comment), but that same setter is also
`cc_debug_set_force_relative_mouse`'s body, which `gamescopectl` reaches through
`gamescope_private_execute()` on the **main** thread -- a genuine cross-thread data
race on `MouseCursor`'s plain fields, not merely a style concern.

**Fix, matching the codebase's own established idiom** (`MakeFocusDirty()` +
`nudge_steamcompmgr()` for cross-thread focus changes, same overview doc): the setter
now only flips an `std::atomic<bool>` dirty flag from whichever thread calls it;
`ProcessPendingCursorFallbackPolicy()`, called once per frame from the steamcompmgr
loop itself (right beside the existing per-frame cursor-suspension check,
`steamcompmgr.cpp`'s main `for(;;)` body), is the only place that actually touches a
`MouseCursor`. One frame of lag between toggling and seeing it take effect, same
trade gamescope already accepts for `SetFpsLimit()`'s X11-property round-trip.

**If you add another live-toggle entry point that reaches into `MouseCursor` (or any
other steamcompmgr-thread-owned state) from a ConCommand, a `gamescope_control`
request, or anything else dispatched by `gamescope_private_execute()`: it is on the
main thread, not the steamcompmgr thread, even though ConCommands "feel" like a
plain global synchronous call.** Verify which thread actually calls your code before
assuming a same-thread reentrant call is safe just because another same-named
function elsewhere in the file already establishes the pattern -- the overlay's own
Switch setters are steamcompmgr-thread-safe *because* they're invoked from
`paint_all()`, not because they're "a UI callback."

## The overlay pointer

A plain triangle -- vertical left edge, long hypotenuse back to the tip -- outlined
in the live accent colour with a black inlay. The black-inside-colour combination is
the point: it keeps the pointer readable over both dark and bright game content
without a drop shadow.

It is drawn as ImGui vector geometry into the foreground draw list, at
`io.MousePos`, tip-on-hotspot (`CursorArt_Draw()`). Nothing is rasterised, no texture
is uploaded, and the font atlas is not touched -- see the third attempt below for why
that last point is deliberate.

`CursorArt_AccentRgb()` exposes the current live accent colour (`palette::kAccent`,
unconditionally -- it does not know about the Cursor tab's override) for other
overlay code.

### The Cursor tab -- `Overlay/PanelCursor.{h,cpp}`

A Setup-section tab lets the player resize the pointer, change its outline
thickness, and pick its outline/inlay colours (outline colour can instead lock
to the live accent, which is the default). Fields live in
`config::OverlaySettings` (`cursor_scale`, `cursor_outline_width`,
`cursor_outline_color` -- `std::optional<int>`, unset = follow accent --
`cursor_inlay_color`), global.json-only like every other field in that struct.

`PanelCursor.h`'s `gamescope::GetCursorAppearance()` is the read side: a
cached, load-once accessor (same shape as `PanelCursor.cpp`'s own row
bindings) that resolves `cursor_outline_color` against the live accent
itself, so callers get one already-final `uOutlineRgb` regardless of whether
the tab's "custom" toggle is on. `CursorArt_Draw()` calls it once per draw
and: multiplies its own `flScale` parameter by `.flScale`, uses
`.flOutlineWidth` in place of a fixed constant, and fills/strokes with
`.uInlayRgb`/`.uOutlineRgb` instead of the old hardcoded black and
`palette::kAccent`. `.flScale` and `.flOutlineWidth` are clamped at that call
site (0.5-3.0, 1.0-6.0) rather than trusted from config, since a hand-edited
global.json bypasses the tab's own slider ranges.

## Three attempts at the overlay pointer

All three were on the same underlying question -- what should the pointer be while
force-grab is on and the overlay is open -- and each failed for a *different* reason,
which is why all three are worth remembering.

**Attempt 1: a "Use system cursor theme" toggle** (`display.force_grab_cursor.system_theme`,
2026-08-27, deleted 2026-08-28). It did not fix anything; it made the broken case
optional. An option is a bad place to record a bug -- the toggle existed because the
themed cursor did not work while the overlay was open, and shipping the switch meant
nobody had to fix that.

**Attempt 2: draw the desktop's Xcursor theme in the overlay too**
(`Overlay/ThemeCursor.cpp`, 2026-08-28, deleted the same day). This one was correct
on the diagnosis -- ImGui's built-in arrow really was what drew while the overlay
was open, confirmed by pixel probe -- and it passed every check that was run on it:
the theme image appeared, the hotspot landed right, all three degradation paths
behaved. **The user's verdict on the real thing was "the mouse cursor is just
entirely broken."**

The lesson is about the *shape* of the verification, not its rigour. Every probe
sampled a single screenshot at a commanded pointer position. That answers "is the
right image at the right place in one frame" and says nothing about behaviour over
time, over motion, or across a font-atlas rebuild -- and `ThemeCursor.cpp` packed its
image into the overlay's live font atlas, re-registering a custom rect whenever the
atlas texture moved, with no upper bound on how often that could happen. A still
frame cannot see a problem of that shape. **A probe that only ever samples one frame
should not be read as evidence about anything that varies between frames.**

**Attempt 3: gamescope's own triangle, drawn in both places** (2026-08-28). Vector
geometry for the overlay, and the same shape rasterised into an X11 cursor for the
game side, so the two could not disagree. No atlas coupling this time. Rejected for a
reason none of the measurements were ever going to catch: the user does not want the
game's cursor touched at all, whatever it is replaced with. *Consistency between the
two states was never the requirement -- not overriding the game was.*

What stands now is attempt 3 minus the game side: vector geometry, overlay only,
nothing rasterised, nothing uploaded, no atlas coupling, and the game's cursor left
exactly as upstream leaves it.

## The freeze, and what is still not known

Reported 2026-08-27, still reported after `66d619d` and `6614d67`: with force-grab
on, clicking appears to hang the compositor, and afterwards "the image only updates
when I click again."

`66d619d` fixed a real and measured defect with exactly that signature -- the client
was receiving no pointer motion at all, and a button event flushed the seat's pointer
state, so the pointer advanced one step per click. That fix is verified and still
holds (see the section below). **It was evidently not the whole cause.**

**Still not reproduced**, as of 2026-08-28, now also after removing the game-side
cursor override and the cross-thread policy machinery that served it (six more
click+motion rounds with the overlay toggling: output kept updating every time).
The user reports it now happens *less reliably*, which means a race. Measured in a
private headless sway across:

| configuration | composited output across a click |
| --- | --- |
| force-grab, overlay closed, `vkcube` (FIFO Vulkan client) | keeps updating |
| force-grab, overlay open | keeps updating |
| force-grab with a *real host pointer lock* granted by the host compositor | keeps updating |
| 6 rounds of click + motion, overlay toggling in and out | keeps updating |

Method, so the next attempt does not repeat the mistake above: screenshot
gamescope's own composition (`gamescopectl screenshot <path> 3`) repeatedly on a
timer against a client with a continuously moving image, and count *distinct* frame
hashes. Identical hashes -- or screenshots that never get written, since the
screenshot is serviced by the composite pass itself -- is the frozen signature.
Checking pointer position instead, as the earlier probes did, is precisely the
measurement that passes while the image is frozen.

Two rig details that matter, both learned the hard way:

- A headless sway with no input devices gives gamescope no `wl_pointer` at all, so
  `SetRelativeMouseMode()` early-returns and **the host pointer lock never engages** --
  the entire force-grab mechanism is inert and the rig proves nothing about it. A
  persistent `wlr-virtual-pointer` client fixes this; confirm the lock by checking
  that an *absolute* host move is ignored.
- That sway also has no keyboard, so `Wayland_RelativePointer_RelativeMotion()` drops
  every event for want of focus. `gamescopectl wayland_mouse_relmotion_without_keyboard_focus 1`
  is the escape hatch.

**`gdb -p` does not work on this machine** (`kernel.yama.ptrace_scope = 1`: only
descendants of the tracer may be attached). To get a backtrace out of a frozen
gamescope here, either launch it under `gdb --args` so gdb is its ancestor, or read
`/proc/<pid>/task/*/wchan` and `/proc/<pid>/task/*/status`, which need no ptrace.

Untested, and the most likely place the difference lives: the user runs a real game
under a real host compositor with real input devices. The remaining candidates are a
Proton/Xwayland client's own reaction to the click (an `XGrabPointer` and the pointer
constraint Xwayland then requests), and frame-callback starvation that only bites a
client which actually blocks on presentation.

### 2026-08-28: the motion path is ruled out by diff

The freeze was reported again, this time on **plain mouse movement with no click** --
which removes button handling, `XGrabPointer`-on-click and click-driven constraint
changes from suspicion and points at the motion path itself.

That path was then audited function-by-function against `fcc1341` (see the next
section for the method and the ancestry finding). The result: **the fork changes
nothing in the game-side motion path.** `wlserver_mousemotion()`'s only fork-side
additions are the overlay-capture early return -- an atomic load and, only while the
overlay is actually capturing, a `std::mutex`-guarded queue push that calls nothing
back into wlserver -- and, until it was reverted, the constraint gate. With the gate
gone, everything from `wlserver_perform_rel_pointer_motion()` to
`wlr_seat_pointer_notify_frame()` is upstream verbatim, and the callsite set that
feeds it is identical to upstream's.

**Why this matters for the search:** the freeze cannot be explained by fork-side
motion code, because there is none left to blame. The remaining fork-side candidates
that a mouse movement can reach are the per-frame `PresentOverlayCursor()` call
`paint_all()` makes (which under the Wayland backend reaches `wl_pointer_set_cursor()`
from the steamcompmgr thread, where upstream only touches host cursor state from the
backend's own thread) and the overlay input queue's unbounded growth if capture is on
while nothing drains it. Neither is reproduced; both are recorded here so the next
attempt starts from candidates rather than from hypotheses about motion.

## The doubled pointer speed -- upstream behaviour, not fixed here

Reported four times across 2026-08-27/28: with `--force-grab-cursor` on, in-game
look/aim sensitivity is roughly doubled. Still open. What follows is the mechanism,
the one measurement that was taken, and the diff evidence that placed the behaviour
in upstream rather than in this fork.

**Mechanism.** `wlserver_mousemotion()` delivers every movement on **two channels at
once**:

1. `wlserver_perform_rel_pointer_motion()` -- a `zwp_relative_pointer_v1` event, sent
   *unconditionally*;
2. `wlr_seat_pointer_notify_motion()` -- the ordinary absolute `wl_pointer.motion`.

Xwayland turns (1) into XI2 raw motion and (2) into the X sprite's position. A client
reading only one is fine. Several Wine/Proton raw-input paths read **both** and sum
them: exactly 2x.

**Measured.** Private headless sway, `--force-grab-cursor`, overlay closed, 200px of
injected relative motion, reading both channels separately (a small XI2 client
counting `XI_RawMotion` valuators, plus `XQueryPointer` for the sprite):

| build | sprite | raw events | raw valuator values |
| --- | --- | --- | --- |
| upstream `fcc1341` | +200px | 20 | `10 10 10 10 …` -- **deltas**, summing to 200 |
| the fork, with the (now reverted) gate | +200px | 20 | `10 20 30 40 …` -- **positions**, no relative channel |

Upstream carries the full 200px on *both* channels, so a client reading both applies
400px. With the gate in place the relative channel was gone and only the sprite
carried the movement -- and the user still reported doubling, so this rig did not
capture their case.

Two controls make that reading solid rather than inferred:

- Gating gamescope's relative motion off entirely
  (`wayland_mouse_relmotion_without_keyboard_focus 0` with no keyboard focus) dropped
  **both** sprite and raw to **0**. That proves `wlserver_mousemotion()` is the sole
  input path once the host pointer is locked, so anything it emits is the whole story.
- The raw *values* -- deltas versus accumulating positions -- distinguish the two
  sources directly. Summed magnitudes alone cannot: Xwayland synthesises raw events
  from absolute motion too, so `raw_dx` being non-zero proves nothing on its own. An
  earlier round of this investigation was misled by exactly that.

**The fix was reverted on 2026-08-28, and the motion path is upstream verbatim again.**

The gate that was tried here --

```c
if ( wlserver.GetCursorConstraint() )
    wlserver_perform_rel_pointer_motion( dx, dy );
```

-- did not cure the report. The user still saw doubling after it shipped, so the rig
above did not capture their case, and the gate was left standing as a private
divergence on the exact code path that was failing. It is gone; the line is
upstream's unconditional call again.

### Why this is upstream behaviour, not fork divergence

Established by diff rather than by hypothesis, which is what four rounds of
hypothesis-driven fixes had failed to do:

- **`3.16.25` (`17baf4a`) is a direct ancestor of this fork's base `fcc1341`.**
  `git merge-base` returns `17baf4a` itself; `fcc1341` is `3.16.25` + 40 commits, with
  zero commits the other way. The installed `/usr/bin/gamescope` the user calls
  "working vanilla" is the *older* of the two, not a divergent branch.
- **The pointer-motion path is byte-identical across those 40 commits.**
  `wlserver_mousemotion()`, `wlserver_apply_constraint()`,
  `wlserver_perform_rel_pointer_motion()`, `wlserver_mousewarp()`,
  `wlserver_touchmotion()` and `wlserver_handle_pointer_motion()` compare equal
  function-body-for-function-body between `17baf4a` and `fcc1341`, and neither
  `SDLBackend.cpp`, `WaylandBackend.cpp` nor `steamcompmgr.cpp` changed a single
  pointer-motion, relative-motion, `SetRelativeMouseMode` or constraint line between
  them. The 40 commits are focus/override/painting work plus keyboard hotkey
  bookkeeping.
- **The fork changes nothing else on that path.** Per-function comparison of the fork
  against `fcc1341` leaves exactly six differing functions in `wlserver.cpp`
  (`wlserver_mousemotion`, `wlserver_touchmotion`, `wlserver_mousebutton`,
  `wlserver_mousewheel`, `wlserver_handle_pointer_button`,
  `wlserver_handle_pointer_axis`) and every difference in them is the overlay's
  capture/routing gate. `wlserver_apply_constraint()`,
  `wlserver_perform_rel_pointer_motion()`, `wlserver_mousewarp()`,
  `wlserver_clampcursor()`, `wlserver_mousefocus()` and
  `wlserver_update_cursor_constraint()` are identical. The set of callsites that
  deliver motion (`wlserver_mousemotion`/`wlserver_touchmotion`/`wlserver_mousewarp`)
  is identical between fork and upstream -- the fork adds no second channel.
  `LibInputHandler.cpp` and `InputEmulation.cpp` are untouched.

So `3.16.25` doubles exactly as `fcc1341` was measured to, given the same conditions.
The condition it needs is **force-grab actually engaging**, and that is the one thing
the fork really does change: upstream reads `g_bForceRelativeMouse` only once, at
backend startup, where `CWaylandBackend::SetRelativeMouseMode()` early-returns if
`m_pPointer` is still null -- and the per-frame path that would call it again is gated
off by `!g_bForceRelativeMouse`. This fork's issue-#68 fix
(`steamcompmgr_set_force_relative_mouse()`, plus the `debug_set_force_relative_mouse`
ConCommand) re-pushes the mode at runtime, when the pointer does exist.

**Why:** that makes the fork's contribution *exposure*, not breakage. Vanilla has no
runtime switch at all, so the user has never had force-grab genuinely engaged on
`3.16.25` -- which is why it looks fine there and broken here, with identical motion
code on both sides. A real fix therefore has to be upstream-shaped (upstream's own
two-channel delivery is what doubles), and this fork will not carry a private
divergence on this path again.

## Why the absolute notify must stay unconditional

`wlserver_mousemotion()` (`src/wlserver.cpp`) delivers every pointer movement to the
focused client twice over, and **both deliveries are required**:

1. `wlserver_perform_rel_pointer_motion()` -- a `zwp_relative_pointer_v1` relative-motion
   event, sent unconditionally at the top of the function.
2. `wlr_seat_pointer_notify_motion()` -- the ordinary absolute-position `wl_pointer.motion`,
   sent at the bottom.

That is not a bug; it is what every wlroots compositor does. The two are different
protocols for different consumers, and a client picks one according to whether it holds a
pointer constraint. **(2) is the only thing that moves the pointer for a client that has
not requested a constraint of its own** -- which is every Xwayland client that isn't in a
raw-input mode, so in practice most of what runs here.

The one legitimate reason to withhold (2) is a client that asked for relative-only input
by locking the pointer, and `wlserver_apply_constraint()`'s early return above already
covers exactly that case (`WLR_POINTER_CONSTRAINT_V1_LOCKED` -> return false -> the
function returns before (2)). There is no second condition to add.

### The 2026-08-27 regression (commit 4583d6f, reverted 2026-08-28)

`4583d6f` withheld (2) whenever `g_bForceRelativeMouse` was set, on the theory that
force-grab implies the client reads relative motion only. **That theory is wrong**, and
this is the mistake to not repeat:

- `g_bForceRelativeMouse` (`--force-grab-cursor`, and the overlay's Force Grab Cursor
  switch) describes gamescope's relationship with the **host compositor**, not with the
  game. Its only real effects are `CWaylandBackend::SetRelativeMouseMode()` /
  `CSDLBackend` / `COpenVRBackend` asking the *host* to lock gamescope's own pointer and
  feed gamescope relative deltas, plus making `ShouldDrawCursor()` unconditionally true.
  It says nothing at all about how the focused client reads input.
- The stated root cause did not even match the gate: (1) is sent unconditionally whether
  or not force-grab is on, so if double-counting were real it would be equally real with
  force-grab off.

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
