# Cursor pipeline: the overlay pointer and "Use everywhere"

Part of [Cursor pipeline](cursor-pipeline.md). The overlay pointer, the Cursor tab, the three attempts, the opt-in game-side pointer and its fixes.

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

Both colours are edited with `ui::CompositeKind::Color` (`Controls.cpp`'s
`ColorBody()`): a swatch preview plus three plain sRGB rails, R/G/B, each
0-255 -- request #5 (2026-09-04) replaced this control's former OKLCH L/C/H
rails with it. `Why:` 0-255 RGB is the convention a new user already
recognises, where OKLCH's lightness/chroma/hue axes are not. The stored value
is unchanged either way -- a packed `0xRRGGBB` int -- only the picker's own
three sliders changed what they mean; `ColorBody()` never touches OKLCH at
all any more. Each rail carries its own one-letter "R"/"G"/"B" label
(`TypeRole::Meta`, `Role::TextMeta`) in a fixed-width column between the
swatch and the rail -- gap-fill for request #5, whose R/G/B rails originally
shipped with no label at all, told apart only by order and end colour. The
column is sized from a single `MeasureText()` of "R" (the Meta face is
monospace, so all three letters measure the same) and only moves the rails'
*left* edge over by that width plus two gaps; the right edge, vertical
position and hit rect are untouched. No numeric 0-255 readout: the row is
52px tall for three rails, so each rail is ~14.7px high -- enough for the
13px Meta label but not for a second line, and not enough vertical room for
the 16.5px `Value` face a numeric readout would need. The **accent hue** slider (`HueBody()`, the "Accent colour" row
in `setup.appearance`) is a *different* control and stayed exactly as it was:
it drives the whole derived OKLCH palette from one hue, and turning it into
RGB would mean rebuilding that model, which request #5 explicitly excluded.

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

### Style: Default and Pointy (2026-11-08)

The Cursor tab's first row is **Style**, a Multiselector (`cursor.style`):
`Default` (the classic triangle above, still the default) or `Pointy`. Stored as
`OverlaySettings::cursor_style`, JSON key `overlay.cursor_style`, `"default"` or
`"pointy"`; any other string loads as `"default"`. `CursorArt_Draw()` reads it through
`CursorArt_IsPointy()` (declared in `CursorArt.h`, defined in `PanelCursor.cpp` beside
the setting, since `CursorAppearance` lives in `PanelCursor.h`).

Pointy is the design workbench's icon path `M5 3 L15 9 L10.5 10.5 L8.5 15 Z` (20x20
box): tip, right wing, inner notch, bottom wing. The vertices (`kPointyX/Y`) are
translated so the tip is the origin and scaled by `kPointyFit = kFootY / 12`, so it is
exactly as tall as the Default pointer at the same scale. It is concave, and the
bundled ImGui has no concave fill, so the inlay is two triangles (tip/wing/notch and
tip/notch/bottom wing) drawn with the draw list's anti-aliased fill flag cleared so no
seam shows on the shared diagonal; the closed `AddPolyline` outline, centred on the
true edge, covers the hard fill edges. The hotspot is the visible tip: the path is
shifted back by the Pointy tip's own miter offset (`CursorArt_PointyTipMiterOffset()`,
same formula as the Default's). Colours, outline width and scale are shared with
Default. The game-side rasterised fallback (`CursorArt_Rasterise()`, "Use everywhere")
and the rail icon still draw the Default shape.

Why: the user saw this icon in the design workbench and asked for it "only as an
option, so the current one should still be the default ... like a pointy version. So
add like a style selector."

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

**2026-09-04: that larger surgery was done too, as its own opt-in.** A game
setting its own cursor beat this fallback in exactly the way predicted here,
reported against Rust's menus. `cursor_override_game` -- a separate toggle, not a
widening of this one -- does substitute unconditionally on the live compositing
path; see "Override game cursor: reaching the live compositing path" further down
for what changed and why it stayed a second opt-in rather than folding in here.

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
code. Confirmed a second time, independently, chasing an unrelated font-doubling
report: with the overlay's render texture cleared to opaque red before compositing --
forcing the composite to show that layer alone if it were captured at all,
`gamescopectl screenshot` still saved a plain, unmodified game frame with no red in
it. See `../planning/redesign/round-2/e2-inspector-plus/IMPLEMENTATION.md`'s
2026-08-29 "font artifact" entry -- treat "screenshot the overlay to check a
rendering bug" as a dead end on this rig, and query state directly instead.
`XFixesGetCursorImage()` queried directly against the Xwayland ctx's own
display (the exact call `MouseCursor::getTexture()` itself makes) is authoritative
instead, and was used to confirm all four cases: off/closed is the plain 24x24
`left_ptr` (green/black, no accent colour anywhere); on/closed is an 18x24 bitmap
at hotspot (2,2) whose only two colours are the live accent (`0x36BDDD`) and black,
matching `CursorArt.cpp`'s geometry exactly; toggling live between the two states
takes effect within one frame in both directions.

## Override game cursor: reaching the live compositing path -- added 2026-09-04

Request #6, `superdoc/planning/requests-2026-09-04.md`: reported as "the custom cursor
doesn't work in Rust -- the game still somehow overrides it." Scouting found two
distinct cases folded into one report -- see that tracker entry for the full
scouting record -- and this section covers the one that turned out to be fixable:
Unity-under-Proton setting a real X11 cursor on its own menu window, which always
wins over `cursor_everywhere`'s root-only fallback per ordinary X11 cursor
inheritance (the "Scope" note two sections up already named this limit; this is
where it stopped being accepted).

**What changed.** `config::OverlaySettings::cursor_override_game` (`ConfigSchema.h`),
surfaced as the Cursor tab's "Override game cursor" switch (`PanelCursor.cpp`, area
`setup.cursor`, group "Reach", entry id `cursor.override_game`) -- a SEPARATE opt-in
from `cursor_everywhere`, off by default. When on, `MouseCursor::getTexture()`
(`steamcompmgr.cpp`) substitutes `CursorArt_Rasterise()`'s bitmap for whatever
`XFixesGetCursorImage()` reports, on every repaint -- the live compositing path this
doc's own "Scope" note above said doing this would mean touching, and until now
never did.

**Why a second toggle, not widening the first.** `cursor_everywhere` only ever
replaces an *absent* cursor (the root window's fallback); a game that sets its own
cursor for a reason -- an RTS's unit-select arrow, a strategy game's build cursor --
keeps it regardless, and that was deliberate: "everywhere" always meant every
*place*, never every possible cursor source. `cursor_override_game` breaks that
promise on purpose, so it cannot ride along on the existing switch -- anyone who
already has "Use everywhere" on for the fallback-only behaviour must not wake up
with their RTS's unit cursor silently replaced too. The setting's own `.Help()`
states this plainly rather than leaving it to be discovered by testing an RTS.

**Where it's placed, and why there -- this is the constraint that matters most.**
`getTexture()` proves whether the client's cursor is empty (`bNoCursor`, from the
alpha scan already there) *before* the override ever runs, and the existing
`if (m_imageEmpty) { ...; return false; }` early-return -- unchanged -- still fires
first. The override block sits strictly *after* that return, so it only ever
substitutes a cursor the game **set**; it can never resurrect one the game
**hid**. A game that deliberately hides its cursor -- every first-person title
during actual gameplay -- still gets no cursor: overriding that would paint the
triangle in the middle of a crosshair the game drew into its own frame, exactly the
outcome this had to avoid. Both consumers of the substituted image inside
`getTexture()` were kept consistent with each other: the Vulkan-plane texture (the
new code) and the pre-existing `GetNestedHints()->SetCursorImage()` call a few
lines later, which used to read the real image's `nDesiredWidth/Height` and
`image->xhot/yhot` directly -- switched to `nContentWidth/Height` and
`m_hotspotX/Y` instead, values that already equal the old ones in the unoverridden
case (both branches above keep them in sync) and correctly reflect the override
when it's on, so the nested-host-cursor path can't end up disagreeing with the
composited one about what the cursor currently is.

**Hotspot.** `CursorArt_Rasterise()` already reports the *visible* tip as its
hotspot (see "The hotspot was the path vertex" section below), the same one
`SetDefaultCursorImage()`'s "Use everywhere" path already trusts -- reused
directly (`m_hotspotX`/`m_hotspotY` set from its `pnHotX`/`pnHotY` out-params), so
clicks land where the drawn tip is, not on the rasteriser's padded-bitmap origin.

**What it cannot do.** Locked-pointer gameplay is out of reach for this or any
compositor: `wlserver_apply_constraint()` returns false for `LOCKED`, the absolute
pointer freezes, and every cursor source -- this one included -- correctly stands
down; there is no cursor layer at all to substitute into. The setting's own
`.Help()` says this in plain language rather than letting a player read "Override
game cursor" as "fixes the cursor in every game."

**Verification.** Built and unit-tested (`overlay.cursor_override_game` round-trips
through `SaveGlobal`/`LoadGlobal`, `tests/test_config.cpp`); Rust itself cannot run
on the test laptop (anti-cheat, weak CPU), so the fix for the reported game is
unverified against the actual report -- only against synthetic reproductions of the
same X11 mechanism (a client that calls `XDefineCursor` on its own window, and one
that hides its cursor via an empty-alpha image), which is a narrower claim than
"fixes Rust" and is reported as exactly that narrower claim.

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
