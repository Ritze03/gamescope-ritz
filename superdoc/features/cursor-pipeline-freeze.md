# Cursor pipeline: the overlay freeze

Part of [Cursor pipeline](cursor-pipeline.md). The 2026-08-28 freeze where the overlay never asked for a frame, with the measurements and dead ends.

## The freeze -- found 2026-08-28: the overlay never asked for a frame

**Resolved.** The compositor was not wedged and no thread was blocked; gamescope
simply had no reason to draw. `steamcompmgr`'s main loop paints only when a dirty
flag is set (`bShouldPaint = vblank && ( hasRepaint || hasRepaintNonBasePlane ||
bForceSyncFlip )`, `steamcompmgr.cpp`), and **nothing in the settings overlay ever
set one**. Every frame the overlay appeared to draw was a frame the *game* had
asked for. Under a game that renders continuously this is invisible; the moment the
client stops producing frames -- an idle or paused game, a menu, a stalled Proton
client -- the overlay stops updating entirely and only twitches when some unrelated
event happens to dirty the frame.

Input capture makes it strictly worse rather than causing it. Upstream's pointer
path always ends at `wlserver_oncursorevent()`, which sets `hasRepaint`. The
capture gates in `wlserver.cpp` return *before* that point -- they have to, the
event belongs to the overlay and not to the seat -- so while the overlay owns input,
the one repaint source that used to cover pointer movement is gone too. That is why
the report arrived as "it freezes when I move the mouse" and, earlier, "the image
only updates when I click": a click still reached the game (which then redrew), a
movement swallowed by the overlay reached nothing at all.

### The measurement

Headless backend, one idle client, overlay open, `overlay_e2_trace` counting frames.
Same instance, same command, only the fix differing:

| probe | before | after |
| --- | --- | --- |
| overlay open, idle 2-5 s | 0 frames | 0 frames (no busy loop) |
| opening the overlay (100 ms fade) | 1 frame | 7 frames -- the fade runs |
| one real relative motion event through `wlserver_mousemotion()`, 5 runs | 0, 0, 0, 0, 0 | 3, 3, 3, 1, 1 |
| one real key through `wlserver_debug_key` | 0 frames | 1 frame |
| `debug_force_repaint` (control) | 1 frame | 1 frame |

`wlserver_debug_mouse_motion <dx> <dy> [count]` was added for this (next to
`wlserver_debug_key`, `wlserver.cpp`): it enters at `wlserver_mousemotion()`, the
same function a real grabbed pointer does, on **this** compositor's seat, so the
capture gate can be exercised without touching the host's pointer. `overlay_e2_pointer`
cannot see this defect at all -- it bypasses `wlserver` by design.

### The fix

`SettingsOverlay.cpp` gains one private `RequestRepaint()` and calls it from the
three places overlay state can change without a game frame: `QueueEvent()` (every
captured key, button, wheel and motion), the `settings_overlay_visible` ConVar
callback (opening and closing), and `UpdateFadeAlpha()` while a fade is in flight
(which re-arms itself per frame and stops on its own when the fade lands).

It calls `force_repaint()`, **not** `hasRepaint = true`. Both mark the frame dirty,
but the main loop clears `hasRepaint` immediately *after* `paint_all()` returns, so
a request raised from inside `paint_all()` -- which is where `UpdateFadeAlpha()`
runs -- is swallowed on the same iteration. Measured: with `hasRepaint`, opening the
overlay over an idle client drew exactly 1 frame instead of the fade. `g_bForceRepaint`
is consumed at the *top* of the loop instead, survives the round trip, and works
under every flip type.

This also closes the second candidate below: the input queue can no longer grow
unbounded, because every enqueue now schedules the frame that drains it.

### 2026-08-28: the same fix, applied to the FPS HUD -- and why a per-frame
### gate isn't enough for a slower-than-vblank cadence

`FpsDisplay.cpp` (`src/Overlay/FpsDisplay.cpp`) has the exact same shape as the
overlay did: it updates per frame from inside `paint_all()`'s own call to
`FpsDisplay_AddLayer()`, but never asked for one, so it froze right along with the
game the moment the client went idle -- same root cause, same "does not free-run"
story above.

The overlay's own fix doesn't transplant directly, though: the overlay only needs a
frame on a handful of *discrete* events (a keypress, a visibility toggle, an
in-flight fade), while the HUD's readout is continuously changing and has to keep
refreshing on its own **sampling cadence** (~500ms, matching `SystemStats.cpp`'s own
2Hz poll thread and `RecomputePercentilesIfDue()`'s recompute interval) without
ever free-running the render loop.

The first attempt hung a `force_repaint()` off `FpsDisplay_AddLayer()` itself, gated
to at most once per 500ms (the same shape as `RecomputePercentilesIfDue()`'s own
interval check). Measured headless, idle client, HUD on: it produced exactly **one**
extra frame and then went silent forever -- not a bug in the gate, a structural
dead end. `force_repaint()` only reaches the *next* vblank: steamcompmgr re-arms its
vblank timer on every tick regardless of demand (`steamcompmgr.cpp`, ~16ms later at
a 60Hz default), so the flag it sets is consumed almost immediately, not held for
500ms. `FpsDisplay_AddLayer()` only ever runs from inside a paint that already
happened, sees its own last request was under 500ms ago, correctly declines to
re-arm yet (the no-busy-loop guarantee working as intended) -- and then nothing is
left to wake the loop up again at the 500ms mark, because the only thing that ever
calls this function is a paint. A once-per-interval gate evaluated purely from
inside the paint path can *throttle* requests; it cannot *manufacture* one on a
clock nothing is driving.

The fix that actually sustains the cadence lives outside `paint_all()` entirely: a
small dedicated thread (`EnsureRepaintTimerThread()`), sleeping in 500ms steps and
calling `force_repaint()` only while the HUD is enabled -- the same shape
`SystemStats.cpp`'s own background poll thread already uses for exactly the same
cadence, just driving a repaint instead of a sysfs read. Started lazily, once, from
`EnsureConfigLoaded()`, which runs unconditionally the moment anything touches this
feature's config -- a paint, opening the settings panel, the toggle command -- so it
comes up even in a session where the game client never renders a single frame.
`s_bHudEnabledForTimer`, an `std::atomic<bool>`, mirrors the enabled flag for the
thread to read without touching `s_Settings` itself (kept file-local, single-writer
elsewhere in this file). The toggle sites also fire one immediate `force_repaint()`
of their own on top of the timer thread, so flipping the HUD on/off shows up on the
next frame rather than after up to 500ms of nothing.

Measured, headless + an idle client, same instance, only the fix differing:

| probe | before | after |
| --- | --- | --- |
| idle, HUD off, 4s | 0 frames | 0 frames (no regression) |
| idle, HUD on, 4-6s | 0 frames | ~2/sec (matches the 500ms cadence) |
| HUD on, real rendering client (`glxgears`), 2s | ~60/sec | ~60/sec (unchanged) |

Idle CPU with the HUD on: ~1.2%, comparable to the overlay fix's own ~1.8% reading
-- consistent with "a periodic wakeup", not a busy loop.

### The investigation, kept for its dead ends

Reported 2026-08-27, still reported after `66d619d` and `6614d67`: with force-grab
on, clicking appears to hang the compositor, and afterwards "the image only updates
when I click again."

Everything from here down is the record of four rounds that did **not** find it. It
is kept because each round bought a real fact, and because the reason they all missed
is worth remembering: every rig ran a client that kept rendering, which is exactly the
condition under which this defect is invisible. The reproduction only appeared once
the client was *idle*.

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

*(Written before the cause was found. The second guess was the right shape and the
wrong direction: it is not the client starving of frame callbacks, it is the
compositor never being asked for a frame -- so a client that has stopped rendering
takes the overlay down with it.)*

#### 2026-08-28: the motion path is ruled out by diff

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

**Why this mattered for the search:** the freeze could not be explained by fork-side
motion code, because there was none left to blame -- which is what finally pointed at
the *absence* of a repaint request rather than the presence of a bad one.

The leading candidate at the time -- `paint_all()`'s per-frame `PresentOverlayCursor()`
supposedly reaching `wl_pointer_set_cursor()` from the steamcompmgr thread as a
cross-thread libwayland violation -- **was wrong on both halves, and is recorded here
so it is not chased again**:

- It is not per-frame. `CWaylandBackend::PresentOverlayCursor()` (`WaylandBackend.cpp`)
  only calls `UpdateCursor()` when `m_bOverlayCursorActive` actually *changes*; a
  steady mouse movement issues no libwayland request at all.
- It is not cross-thread. `CWaylandBackend::PollState()` -- which owns
  `wl_display_prepare_read`/`read_events`/`dispatch_pending` on the **default** queue --
  is called from the steamcompmgr thread (`steamcompmgr.cpp`'s main loop, and again at
  the tail of `CWaylandConnector::Present()`). The steamcompmgr thread *is* the default
  queue's owner. `CWaylandInputThread` runs on its own `wl_event_queue` with its own
  proxy wrappers, which is libwayland's supported multi-queue pattern. Upstream already
  calls `SetCursorImage()` -> `UpdateCursor()` -> `wl_pointer_set_cursor()` from the
  same steamcompmgr thread (`steamcompmgr.cpp`, the cursor-image path), so touching host
  cursor state from there is upstream-normal, not fork divergence.
