# Cursor pipeline

What gets drawn as "the mouse pointer," where that decision is made, and the two
invariants that block the obvious-looking shortcuts (a live host cursor under a
pointer lock; touching a cursor from the wrong thread).

**There is no setting for any of this.** A "Use system cursor theme" toggle
(`display.force_grab_cursor.system_theme`) existed briefly on 2026-08-27 and was
deleted on 2026-08-28 -- see "The toggle that shouldn't have existed" below.

## Three cursor sources, one rule: exactly one on screen

`src/CursorPolicy.h` names them and states gamescope's own invariant plainly:

1. **The composited cursor plane** (`MouseCursor::paint()`, `src/steamcompmgr.cpp`) --
   gamescope's own Vulkan layer, drawn from whatever the current X11 cursor image is
   (`XFixesGetCursorImage()`, re-read every frame in `MouseCursor::getTexture()`).
2. **The overlay's own cursor** -- drawn into the settings overlay's own texture
   (`SettingsOverlay.cpp`). Two possible images, decided before each frame starts:
   the desktop's Xcursor-theme pointer (`src/Overlay/ThemeCursor.cpp`) when one can
   be loaded, else ImGui's built-in vector arrow (`ImGuiIO::MouseDrawCursor`).
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
`g_bForceRelativeMouse` alone (`SetDefaultCursorImage()`'s `bPreferThemeCursor`):
**while grabbed, always the Xcursor-theme route**, because a live host cursor cannot
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
mode" means. This is why force-grab delivers a themed **fallback image**
(`XcursorShapeLoadCursor`), not a live host pointer -- the latter genuinely cannot
coexist with the lock, by design, not by omission.

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

## The overlay's own pointer: why it was wrong, and what draws it now

Reported 2026-08-27, still open on 2026-08-28: the pointer looks generic *while the
settings overlay is open*, and correct once it is closed.

**Cause, verified rather than reasoned.** Both states were photographed by
screenshotting gamescope's own composited output (`gamescopectl screenshot <path> 3`)
in a private headless sway and reading the pixels at a commanded pointer position
(`gamescopectl overlay_e2_pointer "move 400 300"`). This machine's theme
(`Hack-C-scaled`) draws a green arrow, `#00de00`, which makes the two candidates
trivially separable:

| force-grab on | pixels at the pointer | drawn by |
| --- | --- | --- |
| overlay closed | `(0,222,0)` green arrow | composited plane, source (1) |
| overlay open (before) | `(255,255,255)` fill, `(0,0,0)` border | ImGui's built-in arrow |
| overlay open (after) | `(0,222,0)` green arrow | `ThemeCursor.cpp`, source (2) |

So the second-hand claim was right, and here is why it happened -- it was true before
the deleted toggle ever existed:

- Source (1), the composited plane, is unconditionally suppressed whenever
  `SettingsOverlay_IsCapturingInput()` is true (the "Redundant-cursor fix" in
  `steamcompmgr.cpp`, guarding issue #69's stale-ghost-cursor bug). It is positioned
  from `wlserver.mouse_surface_cursorx/y`, which stops updating the moment the overlay
  takes input, so painting it would leave a frozen ghost. **That suppression is still
  in place and was not touched.**
- Source (3), a real host cursor, requires the pointer to be *unlocked*
  (`NestedHostCursorUsable()`) -- impossible while grabbed, per the section above.
- What was left was source (2), and source (2) was only ever ImGui's built-in arrow:
  a fixed set of vector shapes with no concept of an Xcursor theme.

**Fix:** teach source (2) the theme. `src/Overlay/ThemeCursor.cpp` loads the theme's
`left_ptr` with `XcursorLibraryLoadImage()` -- which reads `XCURSOR_THEME`/
`XCURSOR_SIZE` off disk and, unlike the display-bound `XcursorShapeLoadCursor()` the
composited plane uses, **needs no X connection**, which is what makes it reachable
from the overlay at all. The image is un-premultiplied (Xcursor ships premultiplied
alpha; ImGui blends straight alpha) and packed into the overlay's existing font atlas
as an ImGui 1.92 custom rect, so it rides the texture the overlay already uploads
instead of introducing a second one. It is then drawn into the foreground draw list
at `io.MousePos`, hotspot-corrected -- the same list and position ImGui's own cursor
used.

*Why the decision is made before `NewFrame()`:* `ThemeCursor_Prepare()` touches the
font atlas, so it cannot run mid-frame (Fonts.cpp documents at length what a
mid-frame atlas mutation does to a live draw pass), and `CursorPolicy.h`'s "exactly
one cursor, never zero" invariant has to be *settled* before the frame rather than
discovered halfway through it. `SettingsOverlay.cpp` therefore computes
`bThemedCursor` up front and sets `io.MouseDrawCursor` to its complement, so exactly
one of the two paths can ever run in a frame.

*Custom rects do not survive a font rebuild* (`Fonts.cpp`'s `Load()` calls
`ClearFonts()`, which destroys the packer) and their UVs are invalidated by any atlas
repack or resize. `ThemeCursor_Prepare()` therefore records the atlas, the
`ImTextureData`, its dimensions and the rect id, re-registers and re-blits whenever
any of them changes, and re-reads the UVs through `GetCustomRect()` every frame.

**Degradation, measured** (same screenshot probe; this now has no toggle to switch
off, so the failure path is the only escape hatch):

| environment | result |
| --- | --- |
| normal theme | theme arrow, no ImGui arrow |
| `XCURSOR_THEME` set to a nonexistent name | theme arrow -- libXcursor's own inheritance still resolves one |
| `XCURSOR_PATH` pointed at nothing (`XcursorLibraryLoadImage` returns NULL) | ImGui's plain arrow |

Never blank, never two at once, in all three.

## The toggle that shouldn't have existed

`display.force_grab_cursor.system_theme` ("Use system cursor theme", default on) was
added on 2026-08-27 and deleted on 2026-08-28, along with its config field
(`force_grab_cursor_use_theme`), its load/save, its UI registration and
`steamcompmgr_set_force_grab_cursor_theme()`. `SetDefaultCursorImage()`'s
`bPreferThemeCursor` is now `g_bForceRelativeMouse` alone.

**Why:** it was never a preference. It existed because the themed cursor did not work
while the overlay was open, and an option is a bad place to record a bug. The
2026-08-27 write-up in this file argued the overlay case was "a real, separate
feature ... with real rendering-correctness risk that couldn't be verified without a
visible window -- out of scope for a fix landed sight-unseen", and shipped a help-text
disclaimer instead. That reasoning was wrong on the verification point, which is the
part worth remembering: the result **is** checkable without a visible window, by
screenshotting gamescope's own composited output through `gamescopectl` and reading
the pixels. A visual claim being hard to eyeball is not the same as it being
unverifiable.

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
