# Cursor pipeline: hotspot, stretched resolution and oversized windows

Part of [Cursor pipeline](cursor-pipeline.md). The hotspot fix, the absolute pointer under a stretched resolution, and a window larger than the screen.

## The hotspot was the path vertex, not the visible tip -- fixed 2026-09-02

The user's report: clicks register slightly inside the drawn arrow's visible point,
not exactly on it. Cause: `kTipX`/`kTipY` (`CursorArt.h`) name a vertex of the
*path* the triangle is built from, and both renderers report that vertex as the
hotspot -- but the outline is a **centred** stroke, drawn half in and half out of
that path, so the *visible* point of the arrow sits outside the path vertex by
roughly half the outline width. The old `hotspot (2,2)` figure quoted above (in
"Use everywhere" 's own verification, at that era's default scale/width) was exactly
this bug's signature: it was measuring the path vertex, not where the ink actually
ends.

**Why "half the outline width" is not the right offset, and what is.** At a sharp
corner, a centred stroke's outer boundary is a miter, not a straight offset: it is
pushed out along the corner's bisector by `(outlineWidth/2) / sin(theta/2)`, where
`theta` is the interior angle at the tip -- a real factor (~1.35x for this
triangle's ~43.5 deg tip), not a rounding error, and one that does not cancel out
across the outline-width (1.0-6.0) or cursor-scale (0.5-3.0) ranges the Cursor tab
exposes. A fixed fudge constant would have been wrong the moment either slider
moved.

**The two renderers don't draw the same join, so they can't share one offset
number -- only one offset *computation*.** `CursorArt_Draw()` strokes with ImGui's
`AddPolyline()`, which genuinely miters (verified against `imgui_draw.cpp`'s own
`IM_FIXNORMAL2F` join math -- the two formulas agree to the last decimal for this
triangle, this is not an approximation). `CursorArt_Rasterise()` is a signed-
distance fill (`SignedDistanceToTriangle()`), which always **rounds** a corner: for
a point beyond the tip vertex's edge-normal cone, the nearest boundary point is
unambiguously the vertex itself, so the level set at `flOutlineHalf` is a circular
arc of radius `outlineWidth/2` centred on the vertex -- reaching only
`outlineWidth/2` out, never the larger miter distance (confirmed by construction:
`SegDistSq()`'s per-edge `t` clamps to an endpoint for any point on the outward
bisector, so the "nearest edge" is always the vertex there). Neither renderer's
actual drawing changed for this fix -- same fill, same stroke, same colours, same
antialiasing -- only the reported hotspot moved to match what each one already
draws.

`CursorArt.cpp`'s `TipBisector()` computes the corner's outward direction and
`sin(theta/2)` once, from the same `kTipX`/`kFootX`/`kWingX` constants both
renderers already share, so the two magnitude formulas below can't disagree about
the corner itself:
- `CursorArt_TipMiterOffset()` -- `(outlineWidth/2) / sin(theta/2)` along that
  direction, for `CursorArt_Draw()`.
- `CursorArt_TipRoundOffset()` -- `outlineWidth/2` along the same direction, for
  `CursorArt_Rasterise()`.

`CursorArt_Draw()` shifts the whole triangle's draw origin backward by its offset
(so the *visible* tip, not the path vertex, lands on `io.MousePos`);
`CursorArt_Rasterise()` adds its (smaller) offset to the path vertex's position
within the padded bitmap to get `pnHotX`/`pnHotY`. The rendered shape is otherwise
byte-for-byte what it always was -- confirmed by aligning a before/after screenshot
pair on their respective measured tips and diffing: same silhouette, same pixel
count within AA-fringe noise, only the origin moved.

**Verified headless** (`--backend headless`, isolated `XDG_RUNTIME_DIR`/
`XDG_CONFIG_HOME`, a `glxgears` client to keep frames pumping -- an idle overlay
with no client never repaints, confirmed by waiting 18s for nothing), positioning
the overlay's own pointer with `overlay_e2_pointer move <x> <y>` and measuring the
outermost accent-coloured pixel in a `gamescopectl screenshot "<path> 4"`
(`screen_buffer`) capture, against the commanded coordinate:

| outline width | cursor scale | tip-vs-pointer offset before | after |
| --- | --- | --- | --- |
| 2.0 (default) | 1.0 (default) | ~2.2px | 0px |
| 1.0 (min) | 0.5 (min) | (predicted ~0.7px, not separately measured) | 0px |
| 6.0 (max) | 3.0 (max) | ~23.8px (predicted 24.3px) | 0px |

The default-case "before" and the max-case "before" both land within a pixel of
the analytic miter prediction, cross-checking the formula against the actual
renderer rather than just against itself.

## The absolute pointer under a stretched resolution -- item 10, 2026-09-05

Requests 2026-09-05 item 10: *"When using a stretched res and not having 'force grab
cursor' enabled, the game recognizes the position wrong. This may be only the case when it is
toggled while the game is running."*

### Where the mapping lives

With force-grab **off** the nested backends deliver **absolute** host positions
(`SDLBackend.cpp`'s `SDL_MOUSEMOTION` when not grabbed, `WaylandBackend.cpp`'s
`Wayland_Pointer_Motion`, real touch), normalised to the output, into
`wlserver_touchmotion()` / `wlserver_touchdown()` (`wlserver.cpp`). Both go through one helper,
`wlserver_absolute_to_surface()`: output pixels -> the focused base layer's transform ->
clamped to the surface's bounds (`wlserver_get_cursor_bounds()`). The transform is four floats,
`focusedWindowScaleX/Y` + `focusedWindowOffsetX/Y` (`steamcompmgr.cpp`, `update_touch_scaling()`),
copied from the last painted input-focus layer, whose scale is `1 / calc_scale_factor()` and
whose offset is the negated centring -- **per axis**, so under `--scaler stretch` a 1280x960
game in a 1920x1080 window maps X by 1/1.5 and Y by 1/1.125. The arithmetic (scaler ratios,
layer transform, output -> surface and its inverse) is the pure header `src/PointerMapping.h`,
pinned by `tests/test_pointer_mapping.cpp`; `calc_scale_factor_scaler()` wraps
`ComputeScalerRatios()` with the globals, so what is drawn and where the pointer lands come
from one formula. Under stretch the nested size cancels (`out/src` per axis); under the
letterboxing scalers it does not, which is why `steamcompmgr_set_nested_mode()` writes
`g_nNestedWidth/Height` while the Deck's atom path does not.

With force-grab **on** the host delivers relative deltas into `wlserver_mousemotion()`, which
adds them to the surface-space position unscaled -- no mapping at all, which is why the report
only shows without force-grab.

### What a runtime change did not do

The cache is refreshed every painted frame, so a runtime resolution change (item 7) *does*
reach it once the resized game window commits a buffer -- there is no launch-time-only copy.
What nothing did was tell the **client**. The host pointer sits still while the mapping under
it changes; the client keeps the game-space position from the old mapping until the next host
motion event re-derives it. Under stretch that is a different point on every axis (the unit
test "a runtime mode change moves the mapping under a stationary pointer": output (1440, 810)
was game (1440, 810) and is now (960, 720)). The same gap opens on any window resize and when
a fit override appears or goes. At launch the mapping never moves, so nothing was ever wrong.

### The fix

`update_touch_scaling()` now compares the new layer transform with the cached one and, only
when it changed, writes the four floats **under the wlserver lock** (the readers hold it; the
compare itself is race-free because paint_all's thread is the only writer) and calls
`wlserver_resync_absolute_pointer()`. That replays the last absolute host sample --
`wlserver.flLastAbsolutePointerX/Y`, stored by `wlserver_touchmotion()` after orientation --
through the new mapping and `wlserver_mousewarp()`s the client's pointer there, synthetically
(no cursor un-hide). It bails when the pointer is **locked** (added 2026-09-06 -- see the next
section; a locked client reads relative motion only, so there is nothing to re-sync), when the
last input was **relative** (`wlserver_mousemotion()` clears `bAbsolutePointerCurrent`: after a
grab the host decides where the pointer reappears and its first absolute sample re-syncs on its
own), when the overlay owns the pointer, when the touch mode never warps the cursor (Passthrough /
Trackpad / Disabled), or when nothing would move.

*Force-grab toggle:* on -> relative motion invalidates the sample, nothing stale can replay;
off -> SDL warps the host pointer back and emits a motion, the Wayland backend gets the host's
next `wl_pointer.motion`, and either arrives as a fresh absolute sample through the normal path.
No explicit re-sync on the toggle -- the only position we could replay is the pre-grab one, which
is exactly the wrong one.

*Not covered, X-side:* a game window **larger than the new root** (a windowed game that keeps
its 1920x1080 window after a 1280x960 mode) has its X sprite clamped to the root by the X server
itself; gamescope maps to the window's full extent, X cuts it off. That is inherent to a screen
smaller than a window and is what `--force-windows-fullscreen` exists for.

### Driving the absolute path without OS-level input

`wlserver_debug_absolute_motion "<x> <y>"` (`wlserver.cpp`, next to
`wlserver_debug_mouse_motion`/`_button`) is the sanctioned way to exercise this exact path from
`gamescopectl` in a test: it takes normalised output coordinates (0.0-1.0 per axis) and enters
at `wlserver_touchmotion()` with touch id 0 and no connector, precisely as `SDLBackend.cpp`'s
ungrabbed `SDL_MOUSEMOTION` case and `WaylandBackend.cpp`'s pointer-motion handler do for a
real host pointer with force grab off -- so it records `flLastAbsolutePointerX/Y` and
`bAbsolutePointerCurrent` and runs the sample through the real `wlserver_absolute_to_surface()`
mapping, letting `wlserver_resync_absolute_pointer()`'s behaviour above be verified on a laptop
with no host input device involved.



## A window larger than the screen -- fixed 2026-09-08 (Rust: "changing the resolution breaks the mouse positioning")

Report, verbatim: *"Changing the Resolution breaks the mouse positioning in Rust."* CS2 had passed
a mid-match change the same day, so this was game- or state-specific. Rust is a Unity game
under Proton: the client gamescope's Xwayland talks to is **Wine**, not SDL.

### Reproduced -- with Wine, headless

`tests/pointer_probe_win32.c` is a Win32 program built with the mingw cross compiler and run
with the system `wine` (11.16) inside gamescope's own Xwayland, in the private sway of
`scripts/pointer-regression.sh`: a screen-sized popup (which Wine marks
`_NET_WM_STATE_FULLSCREEN` and clips the cursor to), Unity's Locked mode on request
(`ShowCursor(FALSE)` + `ClipCursor(window)` + `SetCursorPos(centre)` every frame, `WM_INPUT`
raw deltas), and a line per `WM_MOUSEMOVE` in client and screen coordinates. Alongside it,
`xdotool`/`xprop` polled the X root, the window's geometry and its `_NET_WM_STATE` five times a
second, and temporary logging in `DetermineAndApplyFocus()` recorded every resize decision.
Runs: the untracked `pointer-regression` capture run (`explore-20260906-21*-wine-*`).

The sequence, output 1280x720, nested mode changed by `steamcompmgr_debug_set_nested_mode`:

| step | X root | Wine window (X) | gamescope | what the game got for a sample at (0.95, 0.95) |
| --- | --- | --- | --- | --- |
| start | 1280x720 | 1280x720, FULLSCREEN | mapping 1.0 | -- |
| mode 1280x1024 | 1280x1024 | gamescope resizes it to 1280x1024; **Wine puts it back to 1280x720**, drops `_NET_WM_STATE_FULLSCREEN`, sets fixed size hints | `isFullscreen=0 hints=1 req=1280x720` -> the size-hints branch keeps 1280x720 | consistent (window smaller than the root) |
| mode 960x540 | 960x540 | stays 1280x720, larger than the screen | mapping 1.0 (the window fills the output) -> cursor (1216, 684) | **(959, 539)** -- the root corner |

The last row is the bug. gamescope composites the whole 1280x720 buffer over the whole output
and maps the host pointer onto all of it, but the X server confines the sprite to the 960x540
root: the right and bottom quarter of the picture can be seen and never pointed at, and every
host position in that band lands on the root edge instead. From the user's chair: the cursor
they see (the host's) and the click the game gets disagree over a quarter of the screen. Wine
additionally confines its own cursor to the new 960x540 virtual screen.

The same happens to a native client left with a plain window carrying size hints (the SDL3
client after `--fullscreen`, which recreates its window that way), so the rule below is not
Wine-specific; Wine is just what every Proton game is.

*Why Wine does that:* it decides "fullscreen" by comparing the Win32 window rect with the
monitor. The RandR change makes the monitor 1280x1024 while the game's window is still
1280x720, so the window is no longer fullscreen to Wine, and a non-fullscreen Win32 window of
fixed size gets `WM_NORMAL_HINTS` and its own size re-asserted through a ConfigureRequest,
which gamescope (the WM) honours. The game itself never hears of it: no `WM_DISPLAYCHANGE`,
no `WM_SIZE` (measured -- `GetSystemMetrics` did change). This is Wine's window-state
machinery, not Proton's, so it is what Rust runs on.

### The fix

`DetermineAndApplyFocus()` (`steamcompmgr.cpp`, the `win_has_game_id()` branch) now gives a
focused game window that is **heading for a size larger than the screen** the same treatment
as a fullscreen one: resize to the root. "Heading for" is its size hints when it has them,
its geometry otherwise -- a window just shrunk to the root still asks for its old size through
`WM_NORMAL_HINTS`, and judging the geometry alone sent it into the size-hints branch, which
grew it back, at frame rate (the first cut of the fix; measured as `SIZE 1280x720 <-> 960x540`
flapping and `resyncs=70` in six seconds). Wine accepts the second resize (it did every pass
under `--force-windows-fullscreen`, which is the same resize applied unconditionally), and the
window converges: the X window is 960x540, the Win32 window still believes 1280x720, the game
draws its top-left 960x540 into it, and that is exactly the region the pointer can reach.

A window **smaller** than the root is left alone: it is fully reachable, and forcing it up
would hand a Wine game an X window bigger than the surface it draws (the upsize direction,
1280x720 -> 1280x1024, stays as it was: Wine's window stays 1280x720, non-fullscreen, drawn
by the scaler, pointer consistent).

*What this does not do:* make the game render at the new mode. A game that keeps its window
is cropped to the new screen, honestly -- as any X desktop would show it -- with a working
pointer; a game that follows the display change (a borderless "fullscreen window" that
resizes on `WM_DISPLAYCHANGE`, or Proton's emulated exclusive modes re-fitting the window)
adopts the new mode and never hits this rule. Which of the two Rust is could not be measured
here: no Proton, no Rust. `--force-windows-fullscreen` (the Quick toggles switch of the same
day) resizes every game window to the root regardless of size and ends in the same state.

### Also measured, and left as is

- **A Wine menu that has not been moved yet is a LOCK to Xwayland.** Wine clips the cursor to
  a fullscreen window from the moment it is foreground; the X cursor is only defined once
  Wine handles a `WM_SETCURSOR`, i.e. after the first mouse move. Confining grab + no cursor is
  Xwayland's lock rule, so `wlserver_pointer_stats` says `constraint=locked` before anything
  has moved, and the 2026-09-06 gate refuses absolute samples until the first relative one
  (which the host delivers, since gamescope holds the host lock) lets Wine set its cursor.
  Then `confined`, then `none`. One movement; self-healing; the same after a game unlocks
  (Unity's `CursorLockMode.None`): three absolute samples were refused, one relative nudge
  and the next absolute sample landed where mapped. `wine-lock-mode` pins that order.
- **Relative motion across a mode change while locked is untouched**: 10 x `dx=5` before and
  after, `motions_locked=0`.

### Gate

`scripts/pointer-regression.sh`: `oversized-window` (SDL3, no Wine needed -- the resize
happens once, no flapping over 2 s, mapping 0.75, the sample reaches the client at 912,513 and
that equals gamescope's own cursor), `wine-mode` (the Wine menu, same assertions) and
`wine-lock-mode` (locked across the change, then the unlock order above). The wine checks
need `x86_64-w64-mingw32-gcc` at build time and `wine` at run time and SKIP otherwise; a
private `WINEPREFIX` is booted per run. 32 -> 50 checks, all passing on the desktop.
