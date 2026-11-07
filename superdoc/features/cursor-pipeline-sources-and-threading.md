# Cursor pipeline: sources and threading

Part of [Cursor pipeline](cursor-pipeline.md). The one-cursor rule, why a live system cursor under a grab is impossible, and which thread owns `MouseCursor`.

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

**Under the Zoom (`Overlay/Zoom.{h,cpp}`), the drawn cursor and the position the
GAME receives can legitimately disagree, by design (2026-09-27).** While the
cursor is visible and sits inside the zoom's projector, `wlserver.cpp`'s
`wlserver_send_absolute_motion()` sends the game a remapped point -- the spot the
magnified picture is showing under the arrow -- while the arrow drawn by source
(1) above keeps moving at plain, unmapped, 1:1 speed. This is the one place in
the whole pipeline where "what's drawn" and "what the client was told" are
deliberately different values, rather than the same value read from two places.
See `superdoc/features/zoom.md`'s "Clicks land on what the projector shows" for
the user's report, the formula, and why (never touching
`wlserver.mouse_surface_cursorx/y` itself, so this invariant's "exactly one
cursor is visible" still holds -- the disagreement is only about where a CLICK
lands, never about what is drawn or how many cursors there are).

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
