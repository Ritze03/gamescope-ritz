# Cursor pipeline

What gets drawn as "the mouse pointer," where that decision is made, and the two
invariants that block the obvious-looking shortcuts (a live host cursor under a
pointer lock; touching a cursor from the wrong thread).

**By default, gamescope draws a pointer only for its own settings overlay.** That
pointer is a plain triangle, accent-coloured outline, black inlay, defined in
`src/Overlay/CursorArt.cpp`. **While the overlay is closed and the Cursor tab's
"Use everywhere" toggle is off (the default) gamescope does not touch the cursor at
all** -- the game keeps whatever it or the host set, exactly as upstream does.
Getting to that took three attempts; see "Three attempts at the overlay pointer"
below for what each cost -- and see "Use everywhere: reinstating the game-side
pointer as an opt-in" further down for why a fourth attempt, now opt-in only, was
added on top of that history rather than replacing it.

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

`cursor_scale`'s default is `0.8` (`ConfigSchema.h`), not `1.0` -- moved 2026-08-29
at the user's request. The range is unchanged (0.5-3.0); anyone who had already set
their own value keeps it, since a saved `global.json` always overrides the struct's
default on load. The Cursor tab's own slider default (`PanelCursor.cpp`'s
`.Default(...)`) reads `config::OverlaySettings{}.cursor_scale` directly rather than
repeating the literal, so there was exactly one place to change.

The tab's rail icon (`UI/Icons.cpp`, area id `setup.cursor`) reuses
`CursorArt.h`'s `kTipX`/`kFootX`/`kFootY`/`kWingX`/`kWingY` constants -- moved from
`CursorArt.cpp`'s file-local anonymous namespace to the header for exactly this --
at a **fixed** icon-local scale/offset, deliberately not `GetCursorAppearance().flScale`:
an icon that resized with a live setting would be the wrong kind of "live". Drawn as
a plain stroked outline (`IconOp::Loop`), not the two-tone accent-outline/black-inlay
look `CursorArt_Draw()` uses, so it follows the icon set's own single-colour
selected/dimmed convention instead of standing out as the one glyph hardcoded to the
accent.

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
exactly as upstream leaves it -- **by default.**

## Use everywhere: reinstating the game-side pointer as an opt-in

2026-08-28, after the above. The user's original ask ("don't override the game's
cursor") was about the *default*, not a ban -- a later request asked for a toggle
that opts back into attempt 3's game-side half, off by default so nobody's setup
changes. `config::OverlaySettings::cursor_everywhere` (`ConfigSchema.h`), surfaced
as the Cursor tab's "Use everywhere" switch (`PanelCursor.cpp`, area `setup.cursor`,
group "Reach", entry id `cursor.everywhere`).

**Recovered rather than rewritten.** `CursorArt_Rasterise()` -- the ARGB32
premultiplied signed-distance rasteriser deleted in `63b6fed` -- is back in
`Overlay/CursorArt.cpp`, reading `PanelCursor.h`'s `GetCursorAppearance()` exactly
the way `CursorArt_Draw()` already did, so **both renderers share one definition of
the shape, the scale, the outline width and the two colours** -- the corner
constants (`kTipX`/`kFootY`/`kWingX`) are still defined exactly once. A live
`cursor.scale`/outline-colour change reaches the rasterised bitmap immediately
(verified: scale 1.0 -> 24x24 bitmap footprint, scale 2.0 -> 33x46, same ratio).

**Scope: the fallback only, never a cursor the game itself sets.** `SetDefaultCursorImage()`
(`steamcompmgr.cpp`) still only ever calls `XDefineCursor` on the Xwayland ctx's
*root* window -- the same call the deleted code made, and the same one upstream's
own fallback path made before that. Per ordinary X11 cursor inheritance, a window
that calls `XDefineCursor` on itself always wins over whatever the root has, so a
game with a meaningful custom cursor (an RTS's unit-select arrow) keeps it
regardless of this toggle. Replacing a cursor the game explicitly sets would mean
not reading `XFixesGetCursorImage()` in `MouseCursor::getTexture()` at all and
substituting our own texture unconditionally on every repaint -- a change to the
*live compositing path*, not to a fallback, and a materially larger and riskier
surgery than reinstating deleted code. This toggle does the honest, narrower thing:
"everywhere" means every *place* (nested or embedded, grabbed or not, overlay open
or closed) rather than every possible cursor source. The tradeoff is stated here on
purpose, not left for someone to discover by testing an RTS.

**Threading.** Exactly the hazard `0d99251` already fixed once, reintroduced by
the same shape of mistake if the toggle's setter called into `MouseCursor`
directly: `PanelCursor.cpp`'s row setters are reachable from `overlay_e2_set`,
which runs on the **console** thread, while `MouseCursor`'s X11 Cursor resource,
Vulkan texture and non-atomic `m_dirty`/`m_imageEmpty` fields are steamcompmgr-
thread-owned. `PanelCursor.cpp`'s `QueueSave()` -- called by every setter in the
tab, not just this one -- now also calls `steamcompmgr_notify_cursor_appearance_changed()`
(`steamcompmgr.hpp`), which only flips `g_bCursorFallbackPolicyDirty`, an
`std::atomic<bool>`. `ProcessPendingCursorFallbackPolicy()`, called once per frame
from the steamcompmgr loop (right beside the pre-existing per-frame cursor-
suspension check), is the only place that ever calls `SetDefaultCursorImage()`
live. It also polls the resolved `CursorAppearance` once per frame and compares it
against the previous frame's, the same shape the original accent-tracking code
used -- needed because the outline colour can be set to follow the live accent hue,
which changes from `PanelConfig.cpp`'s hue slider, a setter this file has no reason
to know about and that never calls the notify function.

**Verified by direct X11 query, not by compositor screenshot.** `gamescopectl
screenshot` did not capture ANY extra composited layer in this session's headless
rig -- not the settings overlay, not the FPS HUD, not even the pre-existing,
untouched cursor plane in the *off* state -- so it could not be used to tell the
four cases apart here; that is a property of this sandbox's headless Vulkan path,
reproduced with two features this change never touched, not evidence about this
code. `XFixesGetCursorImage()` queried directly against the Xwayland ctx's own
display (the exact call `MouseCursor::getTexture()` itself makes) is authoritative
instead, and was used to confirm all four cases: off/closed is the plain 24x24
`left_ptr` (green/black, no accent colour anywhere); on/closed is an 18x24 bitmap
at hotspot (2,2) whose only two colours are the live accent (`0x36BDDD`) and black,
matching `CursorArt.cpp`'s geometry exactly; toggling live between the two states
takes effect within one frame in both directions.

## "Use everywhere" didn't reach the *overlay-open* host cursor -- fixed 2026-08-29

The verification above only ever exercised the two *closed* states (source (1),
the composited plane / `SetDefaultCursorImage()`). It missed a second place a host
cursor can come from while the overlay is open: source (3), a nested backend's own
`wl_pointer_set_cursor()` / `SDL_SetCursor()`. Reported by the user: with force-grab
**off** and the overlay **open**, "Use everywhere" had no effect -- the host's plain
system cursor kept showing instead of the custom one.

**Why.** `CursorPolicy.h`'s `NestedHostCursorUsable()` -- the one predicate both
nested backends and `SettingsOverlay.cpp` share, precisely so they can't disagree --
took no opinion on this toggle at all. With force-grab off the pointer isn't locked,
so a host cursor was always "usable" by the original three-term test, and
`OverlayShouldDrawSoftwareCursor()` correctly deferred to it, exactly as D29 says it
should when a *real* host cursor exists. "Use everywhere" changed what the *game*
falls back to (`SetDefaultCursorImage()`) but never told this predicate the host
cursor should stop counting as usable in the first place.

**Fix.** `NestedHostCursorUsable()` now takes the toggle as a fourth term:
`!bCursorEverywhere && bHavePointer && !bPointerLocked && bHaveCursorImage`. Both
call sites already fed from the same accessor path (`GetCursorAppearance().bEverywhere`),
so this is one predicate change, not two independent ones. The full truth table
(`tests/test_cursor_policy.cpp`, 16-way exhaustive over all four booleans) still
holds the #69 invariant -- exactly one cursor, never zero, never two -- in every
combination.

**Not just the predicate's return value -- the backends had to stop actually
painting the host cursor too**, or the overlay's own `CursorArt_Draw()` pointer and
the host's system one would both land on screen at once:

- **Wayland** (`CWaylandBackend`): `PresentOverlayCursor()` now also re-runs
  `UpdateCursor()` when `bCursorEverywhere` changes (not just when
  `bOverlayActive` does, since the toggle can flip live while the overlay stays
  open) -- and `UpdateCursor()`'s existing "no usable host cursor" branch
  (`wl_pointer_set_cursor(..., nullptr, 0, 0)`, hide) already does the right
  thing once `HasUsableHostCursor()` says false for this reason too.
- **SDL** (`CSDLBackend`): this one needed a real behaviour change, not just a
  predicate change. Its `GAMESCOPE_SDL_EVENT_CURSOR` handler always called
  `SDL_SetCursor(SDL_GetDefaultCursor())` whenever the overlay was active,
  independent of whether a host cursor was "usable" -- correct before this fix,
  because the only existing "not usable" case (a locked pointer) already hides
  the OS cursor by an unrelated mechanism (`SDL_SetRelativeMouseMode`), so the
  image being *set* never mattered. "Use everywhere" broke that coincidence: an
  unlocked pointer with the toggle on is "not usable" but nothing was hiding the
  cursor. Fixed by mirroring the usability result into a second atomic
  (`m_bOverlayHostCursorUsable`, set from `PresentOverlayCursor()` the same way
  `m_bOverlayCursorActive` already is) and having the event handler call
  `SDL_ShowCursor(SDL_DISABLE)` instead of setting the default cursor when it's
  false.

**Verification.** The policy itself is covered exhaustively by the unit test above.
Runtime wiring was smoke-tested headless (`overlay_e2_set`, `debug_set_force_relative_mouse`,
`settings_overlay_visible` cycled through every combination, no crash) -- headless has
no `GetNestedHints()` at all (`CBaseBackendConnector`'s default returns `nullptr`), so
it cannot show source (3) either way and can't pixel-verify *this specific* gap; that
would need a real nested Wayland/SDL host, which per this project's hard safety rules
is out of scope for an unattended run on the user's own machine.

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

## The doubled pointer speed -- fixed 2026-08-28: one channel per movement

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
