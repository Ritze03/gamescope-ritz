# Cursor pipeline

What gets drawn as "the mouse pointer," where that decision is made, and the two
invariants that block the obvious-looking shortcuts (a live host cursor under a
pointer lock; touching a cursor from the wrong thread). Written while adding the
"Use system cursor theme" sub-option under Force grab cursor
(`display.force_grab_cursor.system_theme`, `src/Overlay/PanelDisplay.cpp`).

## Three cursor sources, one rule: exactly one on screen

`src/CursorPolicy.h` names them and states gamescope's own invariant plainly:

1. **The composited cursor plane** (`MouseCursor::paint()`, `src/steamcompmgr.cpp`) --
   gamescope's own Vulkan layer, drawn from whatever the current X11 cursor image is
   (`XFixesGetCursorImage()`, re-read every frame in `MouseCursor::getTexture()`).
2. **ImGui's software cursor** -- the settings overlay's own pointer, drawn into its
   own texture (`SettingsOverlay.cpp`).
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

The "Use system cursor theme" sub-option (`g_bForceGrabCursorUseTheme`, default
**on**) chooses which of those two fallback sources to prefer *while grabbed*: off
keeps today's nested-mode host-snapshot preference; on always takes the
Xcursor-theme route, even in nested mode. **It only changes anything in nested
mode** -- embedded mode already takes the left_ptr-via-Xcursor path unconditionally
(there's no host display to prefer over it there), so the option is a no-op in
embedded/DRM. Graceful degradation is inherited from libXcursor itself: an unset or
missing theme silently falls back to the plain arrow rather than a blank/broken
cursor -- there's no path here that can fail loudly.

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
mode" means. This is why the sub-option delivers a themed **fallback image**
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

## Why the sub-option's fallback doesn't apply to the overlay's own pointer

Reported 2026-08-27: with "Use system cursor theme" on, the pointer still looks
generic *while the settings overlay itself is open*; closing it shows the themed
one. Confirmed (via `gamescopectl debug_set_force_relative_mouse 1` and log output)
that `SetDefaultCursorImage()` still runs and logs "Using left_ptr from the desktop's
Xcursor theme" regardless of whether the overlay is open -- the X11-side
substitution is never skipped. **This is not a bug in the substitution; it's a
different cursor source taking over while the overlay owns input**, and it was true
before this option existed:

- Source (1), the composited plane this option actually controls, is unconditionally
  suppressed whenever `SettingsOverlay_IsCapturingInput()` is true (the "Redundant-
  cursor fix" in `steamcompmgr.cpp`, guarding against issue #69's stale-ghost-cursor
  bug -- see that comment for why it can't just be turned back on for this case).
- With source (1) out of the picture, D29's invariant ("exactly one cursor, never
  zero") falls to source (2) or (3). Source (3), a real host cursor, requires the
  pointer to be *unlocked* (`NestedHostCursorUsable()`) -- impossible while grabbed,
  same reasoning as the "why a live system cursor is impossible" section above. So
  source (2), ImGui's own built-in software cursor, draws instead -- and ImGui's
  cursor is a fixed set of vector-drawn shapes (`ImGuiMouseCursor_Arrow` etc.) with no
  concept of an Xcursor theme at all.

So while the overlay is open and the pointer is locked, **no code path here is even
theoretically able to show a themed cursor** -- not this option's, not the host's.
Making ImGui draw a themed bitmap instead of its built-in arrow is a real, separate
feature (load the same Xcursor image as a texture, draw it manually instead of
relying on `MouseDrawCursor`) with real rendering-correctness risk that couldn't be
verified without a visible window -- out of scope for a fix landed sight-unseen.
Fixed instead by making the option's own help text say so plainly.

## Force-grab's doubled pointer speed (pre-existing, not introduced by this option)

Reported alongside the above, same day: with the mouse grabbed and the overlay
closed, in-game look/aim sensitivity is roughly doubled; with the overlay open
(so the game isn't receiving input at all), it isn't. Confirmed via
`git diff e236051 HEAD -- src/wlserver.cpp` (empty before this fix) that this
predates the cursor-theme option entirely -- it is a pre-existing force-grab defect,
not a regression from this feature, in code neither this option nor its crash fix
ever touched.

Root cause, in `wlserver_mousemotion()` (`src/wlserver.cpp`): every relative motion
was delivered to the focused client **twice** -- once correctly as a relative-motion
event (`wlserver_perform_rel_pointer_motion()`), then *unconditionally* again as a
regular absolute-position `wlr_seat_pointer_notify_motion()`, gated only by
`wlserver_apply_constraint()`. That gate only short-circuits for a client holding its
own `wlr_pointer_constraint_v1` -- a protocol only *native Wayland* clients request.
An Xwayland/X11 game (the overwhelming majority of what gamescope runs, and
`--force-grab-cursor`'s actual use case) never holds one, so for it the absolute
notify was always also sent. A client that only reads absolute position never
noticed; several Wine/Proton raw-input paths read *both* and summed them, applying
the same physical movement twice -- exactly a 2x factor. The overlay-open/closed
asymmetry falls out for free: while the overlay captures input, `wlserver_mousemotion()`
returns immediately after queueing to the overlay's own input queue, before either
delivery -- the game gets neither the correct signal nor the doubled one, so there
was nothing to compare on equal terms.

**Fix:** withhold the second, absolute notify while `g_bForceRelativeMouse` is on --
the client is expected to be reading the relative event only in that mode. The
position bookkeeping (`wlserver.mouse_surface_cursorx/y`) that positions gamescope's
own composited cursor still runs unconditionally either way; only the redundant
protocol notification to the client is skipped.

**Verification status, stated plainly:** confirmed the exact double-delivery
mechanism by reading `wlserver_apply_constraint()`'s actual conditions (not
guessed), confirmed the fix removes exactly that second call, and confirmed via a
private headless-sway + nested-Wayland smoke test (many force-grab/overlay toggle
cycles through `gamescopectl`, `meson test` 70/70) that input still flows and
nothing crashes or hangs. **Not verified**: actual before/after pointer-speed
measurement -- there is no debug ConCommand for synthesizing relative motion, and
this task's constraints forbid launching a visible window or injecting real input,
so the doubling itself was never observed or re-measured directly. Confidence is
from the code mechanism matching the reported symptom exactly (asymmetry,
exact-2x), not from a repro.
