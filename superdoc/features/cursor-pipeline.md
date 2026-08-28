# Cursor pipeline

What gets drawn as "the mouse pointer," where that decision is made, and the two
invariants that block the obvious-looking shortcuts (a live host cursor under a
pointer lock; touching a cursor from the wrong thread).

**There is no setting for any of this**, and the pointer is gamescope's own art
rather than the desktop's -- a plain triangle, accent-coloured outline, black inlay,
defined once in `src/Overlay/CursorArt.cpp`. A "Use system cursor theme" toggle
(`display.force_grab_cursor.system_theme`) existed briefly on 2026-08-27 and was
deleted on 2026-08-28; the Xcursor-theme lookup that replaced it was itself replaced
on 2026-08-28 -- see "One pointer, two renderers" and "Two rejected attempts" below.

## Three cursor sources, one rule: exactly one on screen

`src/CursorPolicy.h` names them and states gamescope's own invariant plainly:

1. **The composited cursor plane** (`MouseCursor::paint()`, `src/steamcompmgr.cpp`) --
   gamescope's own Vulkan layer, drawn from whatever the current X11 cursor image is
   (`XFixesGetCursorImage()`, re-read every frame in `MouseCursor::getTexture()`).
2. **The overlay's own cursor** -- drawn into the settings overlay's own texture
   (`SettingsOverlay.cpp`) as vector geometry by `src/Overlay/CursorArt.cpp`.
   ImGui's built-in software cursor (`ImGuiIO::MouseDrawCursor`) is never used;
   it is set false at init *and* per frame so nothing can switch it back on and
   put two pointers on screen.
3. **A nested backend's real host-level cursor** -- `SDL_SetCursor()` /
   `wl_pointer_set_cursor()`, drawn by the *host* compositor, not gamescope.

`NestedHostCursorUsable()`/`OverlayShouldDrawSoftwareCursor()` in that header are the
one place that picks between (2) and (3) for the overlay; **the invariant matters
more than which one wins: exactly one cursor is visible, and never zero.**

## Why "the default cursor while force-grab is on" isn't a substitution

`force_grab_cursor` (`g_bForceRelativeMouse`) does **not** swap in a different cursor
image. `ShouldDrawCursor()` (`steamcompmgr.cpp:2918`) just stops deferring to the
nested backend's own host-cursor logic and unconditionally composites source (1)
instead -- which already always shows whatever the *actual* current X11 cursor is:
the game's own image when it has set one, or gamescope's own **fallback** root-window
cursor when it hasn't. That fallback comes from `SetDefaultCursorImage()`
(`steamcompmgr.cpp`, factored out of the old inline `init_xwayland_ctx()` code): a
live snapshot of the real host cursor (`GetX11HostCursor()`, nested mode with an
outer X11/XWayland display to snapshot from) or, if there's no host display to
snapshot -- always true in embedded/DRM mode -- `setCursorImageByName("left_ptr")`,
which resolves through libXcursor (`XcursorShapeLoadCursor`) against whatever
`XCURSOR_THEME`/`XCURSOR_SIZE` the process's environment carries, falling back to
X11's plain compiled-in arrow if no theme is set or found. **A window that later
calls `XDefineCursor` always wins over this fallback** (ordinary X11 cursor
inheritance), so none of this ever overrides a game's own cursor -- only what's shown
in its absence.

Which of those two fallback sources is preferred is decided by
`g_bForceRelativeMouse` alone (`SetDefaultCursorImage()`'s `bUseOwnCursor`):
**while grabbed, always gamescope's own art**, because a live host cursor cannot
exist under the lock at all (next section); while not grabbed, the host snapshot,
which is the better image when it is actually available. **This only changes
anything in nested mode** -- embedded mode already takes the left_ptr-via-Xcursor
path unconditionally (there's no host display to prefer over it there). Graceful
degradation is inherited from libXcursor itself: an unset or missing theme silently
falls back to the plain arrow rather than a blank/broken cursor -- there's no path
here that can fail loudly.

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

## One pointer, two renderers

The pointer is a plain triangle -- vertical left edge, long hypotenuse back to the
tip -- outlined in the live accent colour with a black inlay. The black-inside-colour
combination is the point: it is what keeps the pointer readable over both dark and
bright game content without a drop shadow.

Its geometry lives once, in `src/Overlay/CursorArt.cpp`, because it has to appear in
two pipelines that share nothing:

| where | how | entry point |
| --- | --- | --- |
| settings overlay | ImGui vector geometry into the foreground draw list | `CursorArt_Draw()` |
| game side (X11) | rasterised to premultiplied ARGB32, uploaded as an X11 cursor | `CursorArt_Rasterise()` -> `MouseCursor::setCursorImage()` |

Both read the same three corner constants and the same stroke width, so they cannot
drift apart -- and *drift is the bug this design exists to prevent*: what the user
reported was the overlay and the game disagreeing about what the pointer looked like.

The rasteriser is a signed-distance evaluation per pixel rather than a scanline fill,
which buys the antialiased edge and the outline/inlay split from the same number: the
silhouette is `d <= +halfWidth`, the inlay is `d <= -halfWidth`, and both boundaries
fade across one pixel. `PictStandardARGB32` is premultiplied, so coverage is folded
into the colour at write time.

**The accent is live.** `ProcessPendingCursorFallbackPolicy()` re-reads
`CursorArt_AccentRgb()` every frame and rebuilds the X11 cursor when it changes,
rather than the overlay's hue slider calling into steamcompmgr. That direction was
chosen deliberately: the reverse coupling needs `steamcompmgr.hpp` in `Palette.cpp`,
which drags the generated Wayland protocol headers into the overlay's build and
breaks the test targets' link. Polling one aligned 32-bit global per frame is
cheaper than the plumbing, and one frame of staleness on a colour change is not
observable.

*Verified* by screenshot probe (see below): overlay open, tip exactly on the
commanded pointer position, accent outline with black inlay; overlay closed, the
same triangle from the X11 path; and after `overlay_e2_set overlay.accent_hue 25`
the game-side cursor is warm-coloured with no cyan pixels left anywhere.

## Two rejected attempts, and what they cost

Both were on the same underlying question -- what should the pointer be while
force-grab is on and the overlay is open -- and both are worth remembering because
each failed for a *different* reason.

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

What replaced it deliberately has no such coupling: vector geometry drawn into a draw
list, and a bitmap built once per accent change. Nothing touches the font atlas.

## The freeze, and what is still not known

Reported 2026-08-27, still reported after `66d619d` and `6614d67`: with force-grab
on, clicking appears to hang the compositor, and afterwards "the image only updates
when I click again."

`66d619d` fixed a real and measured defect with exactly that signature -- the client
was receiving no pointer motion at all, and a button event flushed the seat's pointer
state, so the pointer advanced one step per click. That fix is verified and still
holds (see the section below). **It was evidently not the whole cause.**

**Not reproduced.** Measured in a private headless sway across:

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
