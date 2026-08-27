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
