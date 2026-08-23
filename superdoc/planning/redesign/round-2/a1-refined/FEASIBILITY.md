# Feasibility — Console, refined (A1) in immediate-mode ImGui

Dear ImGui **1.92.9b, stock non-docking branch**. No docking, no tables, manual layout,
`ImDrawList` painting, animation via `io.DeltaTime`.

**Verdict up front:** every one of the five fixes is *cheaper* than the thing it replaces,
and two of them delete code outright. The only genuinely risky item is the one A already
flagged — the System Monitor's live HUD preview — and the risk register got shorter, not
longer, because gamepad plumbing (A's risk #4) is gone entirely.

This file records the **delta** from `a-console/FEASIBILITY.md`. Anything not discussed
here is unchanged from that assessment and still holds.

---

## 1. Fix by fix — what it actually costs in ImGui

### 1.1 Deleting the tab bar (fix 1) — **negative cost**

The sub-tab bar was a hand-painted segmented control at a fixed position, plus its own
keyboard axis (`LT/RT`, then `PgUp/PgDn`), plus a per-screen "which tab am I on" index in
the nav state. All three are deleted.

What replaces it is **more of a widget the kit already draws**: the rail item. Zone 2 is
`DrawRailZone1()` with a different height constant, a different indent, and a count on the
right. Roughly 40 lines, reusing the same paint function.

| | Before | After |
|---|---|---|
| Draw code | tab bar paint + hit-test + overflow handling | reuse the rail item paint |
| Nav state | `nSection`, `nTab`, `nRow` | `nSection`, `nScreen`, `nRow` (same three ints — but now two of them live in one column) |
| Keyboard | `Tab`, `PgUp/PgDn`, `↑↓` across two axes | `↑↓` runs the rail continuously; `Tab`/`PgUp/PgDn` are jumps |
| Overflow at 0.5× | >6 tabs would need scroll or ellipsis — undesigned | the rail already scrolls its zone-2 list |

The one new thing is **continuous `↑↓` across two zones**, which in ImGui means the rail is
one nav layer containing both zones' items in DOM order. Stock nav does this for free —
they are simply sequential items in the same window region. Zone 1 items rebuild zone 2
when focus lands on them (`if ( ImGui::IsItemFocused() && changed ) SelectSection(...)`),
which is a one-line pattern.

**Risk: low.** One edge case to QA: changing section while focus is in zone 2 must clamp
the screen index, since the new section may have fewer screens.

### 1.2 The three fill classes (fix 2) — **negative cost**

A's `Choice()` measured its options at draw time to pick a normal-vs-wide control column.
That measurement was already flagged in A's own feasibility (§6) as a hazard: a font-atlas
rebuild frame can change the measurement mid-drag and flip the decision. The fix deletes
the measurement:

```cpp
Class c = kClassOf[ e.kind ];                       // a table lookup
ImRect bbCtl = CtlRect( row, c );                   // arithmetic, no text measurement
```

Segmented-vs-Picker is now decided **at registration** from the registered strings
(`count ≤ 5 && longest label ≤ 8 chars`), so it is a startup-stable property of the entry
rather than a per-frame decision. The hysteresis workaround A proposed is not needed and
not written.

Segmented cells at equal width is `flCtlW / n` — simpler than measuring each label and
laying them out, which is what the shipped `SegmentedControl()` does today.

**Risk: none.** This is strictly less code doing strictly less work.

One real consequence to accept: **a 5-cell segmented control at 0.5× on a narrow surface
gives each cell ~24px.** The responsive ladder (SPEC §2.4) drops the column to 60u there,
so cells get ~19px — too narrow for `uncharted2`. The ladder must therefore also **demote
Segmented → Picker below 108u of stage width**. That is one line in `kClassOf` resolution,
but it must be written; forgetting it is the most likely way to ship a broken 0.5× screen.

### 1.3 The contrast roles (fix 3) — **trivial to apply, cheap to enforce**

Applying it is editing `Palette.h`'s alpha constants. The shipped code already has the
right shape — `kRailAlpha`, `kMarkAlpha`, `kMetaTextAlpha` are named tiers introduced by
issue #62 — so this is changing numbers in one file, not chasing call sites.

`ui.contrast 1` is the new part. Implementation:

```cpp
// Theme.cpp -- debug-only, behind a ConVar
void AuditText( ImU32 col, const ImRect &bb, Role role )
{
    const ImVec4 fg = ImGui::ColorConvertU32ToFloat4( col );
    const ImVec4 bg = SurfaceUnder( bb );        // the kit KNOWS its own surface stack
    const float r = WcagRatio( Composite( fg, bg ), bg );
    if ( r < kFloor[ (int)role ] )
        ImGui::GetForegroundDrawList()->AddRect( bb.Min, bb.Max, IM_COL32( 255,60,60,255 ) );
}
```

`SurfaceUnder()` is the reason this is cheap here and would be expensive in a
free-form UI: the console has **four** surfaces (base, rail, focused row, sheet/palette),
all painted by the kit, so the kit can answer "what is under this text" from its own state
without reading pixels back. A per-panel UI could not.

**Cost:** ~80 lines. **Risk: none** — it is a debug overlay.

The 360-hue accent sweep in `SelfTest()` is ~20 lines of the same maths at startup.

### 1.4 The rail icons (fix 4) — **negative cost**

A's rail collapse animated width *and* re-laid-out its items (centring on collapse,
`display:none` on the labels). The refined rule removes the re-layout:

```cpp
const float x = railX + m.flStagePadX;          // identical in both states
DrawIcon( x, y, e.icon );
DrawLabel( x + m.flIcon + m.u * 3, y, e.szTitle, Anim().flLabelAlpha );  // alpha only
```

The label is skipped entirely below `alpha < 0.05` (A's §4.3 note, kept). The zone labels
and footer reserve their height unconditionally; only their alpha moves. Two floats total
(`flRailW`, `flLabelAlpha`), both `Approach()`ed — inside A's five-float budget, not on
top of it.

**Risk: none.** The property that makes it work is arithmetic, not discipline: the icon's
x is a single expression that has no state-dependent term in it.

### 1.5 The detail bar (fix 5) — **cheap; the dependency is the registry**

Painting it is three text runs and a fixed key-hint row in a fixed band. The band already
existed as the legend.

The dependency worth being explicit about: **the bar needs the focused entry's
declaration, which is exactly why rows had to become declarative.** With A's imperative
`ui::Switch(...)` the kit only knows about a row *while it is being drawn*, so the bar at
the bottom of the frame would be describing whatever was focused *last* frame — a real
one-frame lag on every focus move, visible as text that trails the highlight. Declarations
remove the problem rather than papering over it.

**Cost:** ~90 lines including `Facts()` formatting. **Risk: low.**

Two details:

- The bar's height must be **fixed**, not content-derived, or a long `Help()` string
  resizes the console body and shifts every row. It is `min 15u` with the help line
  clipped to two lines and ellipsised. Long explanations belong in a Drill screen.
- Hover-preview vs focus in the same bar needs a one-frame precedence rule (hover wins
  while the pointer is over a row; focus otherwise), or the bar flickers when the pointer
  crosses the list. One `if`.

---

## 2. The registry in immediate mode — the one architectural question

Direction B answered this and the answer holds. Restated for A1, because A1 is a *hybrid*
(declared rows, imperative Wells) and the seam is where bugs would live:

**The registry holds declarations, never widget state.** It is built once at startup by
`RegisterAll()`. Per frame the kit walks the current screen's entry span and calls
`Rows::Draw(e)`. There is no retained widget tree, no dirty flags, no invalidation.

Three things that a naive implementation gets wrong:

1. **IDs must be the entry's stable string id**, not the loop index. The palette's filtered
   list changes contents every keystroke; index-derived IDs would make ImGui think a slider
   being dragged became a different widget mid-drag and drop the drag.
   `PushID("upscale.sharpness")` survives any re-sort.
2. **`AvailableWhen()` predicates must be evaluated in draw order and must be cheap.** They
   run every frame for every entry on the current screen (≈ 20). A predicate that queries
   PipeWire or the DRM state per frame is a performance bug; those belong behind a cached
   value refreshed in `OnEnter()`.
3. **`OnEnter()` is where side effects go.** A's §2.5 found the real blocker for a full
   search index: screen draw functions today are *not* side-effect-free
   (`PanelShaders` calls `EnsureEffectLoaded()`, `PanelAudio` enumerates PipeWire nodes).
   The registry makes this a non-issue by construction — **declarations have no side
   effects, so the palette can index every setting at startup without drawing anything.**
   A1 therefore gets the complete search index that A had to degrade to a lazy one. This
   is the single biggest technical win from adopting B, and it is worth more than the
   palette itself.

**Cost of the registry layer:** ~450 lines (`Registry.h/.cpp` + `Bind.h`), of which the
fluent builder is mechanical.

---

## 3. The palette

Mechanically the easiest new screen in the design, because it reuses everything:

- **Focus trapping and Esc**: stock `BeginPopupModal` (do *not* hand-roll — that means
  poking `g.NavWindow`, which is internal and version-fragile).
- **Text input**: `InputText` with `ImGuiInputTextFlags_AutoSelectAll`, reading
  `io.InputQueueCharacters`, which `wlserver` already fills with layout-correct UTF-8 via
  `xkb_state_key_get_utf8()`.
- **Rows**: `Rows::DrawPaletteRow()` = the normal row frame with a path caption under the
  label and the control forced to the Half class. No new geometry.
- **Scoring**: B's `Match.cpp`, ~120 lines, recomputed only when the query changes.
  ≈ 90 entries × a fuzzy scorer is well under 50 µs; it does not need to be incremental.
- **Landing**: `GoTo()` sets three ints and one "flash this row" id with a timestamp. The
  flash is `Approach()`ed alpha on the row fill — a sixth animation float, and the only
  addition to A's motion budget.

**One real trap:** the palette's inline `←→` adjustment writes through `Bind::Set()`, which
may fire a side effect (`ApplyVolume()`, `SetRuntimeUniformFloat()`) at key-repeat rate.
The change hook must debounce writes the same way `SaveScope` debounces config saves — one
apply per frame, not one per repeat tick. The shipped code already has this shape in
`QueueSave()`.

**Cost:** ~280 lines. **Risk: low.**

---

## 4. Unchanged assessments carried over from A

These were assessed in `a-console/FEASIBILITY.md` and are unaffected by the refinement:

| Item | Status |
|---|---|
| One window, fixed pos/size, `NoDecoration\|NoMove\|NoResize` | easy; deletes `Chrome.cpp`'s drag/collapse/Z bookkeeping |
| Row frame via `ItemSize`/`ItemAdd`/`ButtonBehavior` over the whole row | easy; the pattern `Widgets.cpp` already uses |
| Toggle / segmented / chips / readout / action / drill painting | easy; all exist today |
| Slider, with the `GrabMinSize`-vs-drawn-handle invariant | carried over **verbatim** from `slider-widget-spec.md` §3; issue #23 cannot recur |
| Scrolling stage (`BeginChild`), `SetScrollHereY` for nav-follow | stock |
| LOG virtualisation (`ImGuiListClipper`) | stock; handles 100k lines |
| Sheet as a stock modal | recommended; do not hand-roll focus trapping |
| Stage push/pop: draw only the incoming screen, offset + alpha | keep A's decision; a true cross-fade double-fires side effects |
| Font atlas: 6 roles ≈ 8 baked faces (down from 10) | modest win, as A said — not a halving |
| `Approach()` animation, no per-widget state | unchanged; now six floats instead of five |
| OKLCH accent flow from `Palette.cpp` | unchanged |
| Row-level clipping with an animated stage offset | fiddly, not novel; QA at both scale extremes |
| Log find highlighting in monospace | arithmetic, not measurement |

---

## 5. Still the one risky item: the System Monitor live preview

Unchanged from A's §5, and still the only thing I would spike before committing.

`FpsDisplay.cpp` (2,469 lines) owns its own ImGui context, and atlases are per-context, so
you cannot draw its widgets into the console's draw list. Previewing it means refactoring
the module draws from "draw into my context at my anchor" to
`Draw( ImDrawList *, ImVec2 origin, float flScale )` with fonts resolved from the calling
context. Mechanical but broad; **1–1.5 sessions on its own**, and it must not regress the
real HUD.

Two compromises already designed in, both kept:

1. **"Approximate scale"** is printed in the Well header. The preview draws at the console
   atlas's nearest available size rather than re-baking per drag frame (which issue #51
   forbids). It still shows placement, backdrop, colours and which modules are on — 90 % of
   what the preview is for.
2. **The backdrop is representative, not the literal frame.** Punching an unblurred hole in
   the compositor's blur/darken pass would need rect exclusion, which does not exist today.

**Fallback if the spike says no:** the Well degrades to the anchor grid over a neutral
backdrop with a to-scale *outline* of the HUD's measured footprint
(`MeasureFpsModule()` and siblings already exist). Two hours, still better than today's
no-preview floating window. **Design the fallback in from the start.**

A1 adds one *argument* for doing the preview properly: the contrast audit (SPEC §9) found
that the HUD's shipped backdrop default of 0.50 puts its white text at **3.7:1** over a
white game — the only real contrast failure in the product, and the one place a role table
cannot fix, because the HUD sits on raw game pixels with no console surface underneath.
The preview is where a user can see and fix that. Raising the default to 0.60 (5.2:1) is a
one-line change that should land regardless of whether the preview does.

---

## 6. What still fights immediate mode

| Item | The fight | Resolution |
|---|---|---|
| Cross-fading two stages | needs both screens drawn per frame; side effects double-fire | don't — offset + alpha the incoming page only |
| Focus trapping (sheet, palette) | hand-rolling needs internal nav state | stock `BeginPopupModal` for both |
| Per-widget animation | needs an ID-keyed persistent store | design says no — six global floats |
| Cross-context HUD preview | atlases and draw lists are per-context | refactor the module draws, or degrade to schematic (§5) |
| A detail bar that describes the focused row | in pure immediate mode the kit only knows a row while drawing it — a one-frame lag | **declarative rows**; the bar reads the declaration, not the draw (§1.5) |
| A complete search index | immediate mode has no retained tree, and screen draws are not pure | **declarations are pure**; index at startup (§2.3) — A's lazy-index compromise is not needed |
| `←→` adjust vs ImGui nav | ImGui may consume arrows for `NavMoveRequest` at a scroll edge | one item per row + `NavMoveRequestCancel()`; QA at the top/bottom of a scrolled list |
| Segmented cell width at 0.5× | equal-width cells get too narrow | the responsive ladder demotes Segmented → Picker below 108u (§1.2) — **must be written** |

---

## 7. Migration cost

Current `src/Overlay/`: **10,384 lines** (measured 2026-08-23).

**New code — the kit:**

| File | Est. lines |
|---|---|
| `Console.cpp` (shell, header, two-zone rail, stage, detail bar, nav, anim) | 750–900 |
| `Registry.h/.cpp` + `Bind.h` (declarations, fluent builder, asserts) | 400–500 |
| `Rows.cpp` (12 kinds + the row frame + the class table + LogView) | 550–700 |
| `Sheet.cpp` | 180–250 |
| `Palette.cpp` + `Match.cpp` | 280–400 |
| `Theme.h/.cpp` (roles, `Paint*`, `ui.contrast`) | 320–420 |
| `Metrics.h` | 90 |
| **Kit total** | **~2,570–3,260** |

Larger than A's estimate (~1,800–2,300) by roughly 800 lines. That is the registry, the
palette and the contrast checker — bought deliberately, and it buys back the complete
search index and the detail bar, neither of which A could have.

**Converted screens** — smaller than A's estimates, because declarations are denser than
imperative draw code:

| Today | Lines | After | Note |
|---|---|---|---|
| `PanelShaders.cpp` | 367 | ~120 | shader plumbing stays; only the UI collapses |
| `PanelDisplay.cpp` | 778 | ~230 | 4 screens |
| `PanelAudio.cpp` | 431 | ~200 | PipeWire logic untouched, + the Well |
| `PanelConfig.cpp` | 914 | ~320 | profiles/copy/delete become Drill + Sheet |
| `PanelLog.cpp` | 185 | ~110 | gains find/levels/follow |
| `FpsDisplay.cpp` config UI | ~700 of 2,469 | ~250 | the HUD renderer itself untouched |
| `Chrome.cpp` | 1,627 | ~320 | **~1,300 deleted** — windows, title bars, drag, collapse, dock, tiling, Z-order. Icons (~300) survive |
| `Widgets.cpp` | 942 | ~0 | absorbed into `Rows.cpp` / `Theme.cpp` |
| `Fonts.cpp` | 460 | ~400 | 10 roles → 8 bakes |
| `Palette.cpp` (colour) | 127 | ~150 | + `state/danger`, + the hue sweep in `SelfTest` |
| `Notifications.cpp` | 787 | unchanged | outside the console |
| `LogCapture.cpp` | 244 | unchanged | |

**Net: roughly −1,100 to −1,500 lines** once the port completes, including the kit. Less of
a saving than A claimed (−1,400 to −1,800) because the registry and palette are real code.
Honest trade: ~800 more lines of infrastructure for a startup-complete search index, a
detail bar with no frame lag, build-time contrast enforcement, and `gamescopectl ui
get/set` for free.

**Effort:**

| Phase | Sessions |
|---|---|
| Registry + Bind + Metrics + Theme + row frame | 1.5 |
| Rows: Toggle / Slider / Readout / Segmented, and the class table | 0.5 |
| Shaders pilot end-to-end | 0.5 |
| Remaining kinds + Sheet | 0.75 |
| Two-zone rail + detail bar + contrast checker | 0.75 |
| Gamescope (4 screens) + Config (3 screens) | 1.5 |
| Audio + first Well; Log + first Wide | 1 |
| Palette | 0.5 |
| System Monitor (schematic Well) | 1 |
| System Monitor live preview (`FpsDisplay` draw-list refactor) | 1–1.5 *(optional; spike first)* |
| Polish, 0.5×/2.0× QA, delete `Chrome.cpp`'s window code | 1 |
| **Total** | **~9–10 sessions**, of which ~8 gets a complete shippable console without the live preview |

Half a session to a session more than A, for the registry and palette.

**Risk register, ranked:**

1. **System Monitor live preview** — the only item that might not be achievable as
   designed. Mitigated by a designed-in fallback.
2. **`←→` adjust vs ImGui nav** at the edges of a scrolled list — likely fine, needs QA.
3. **The Segmented → Picker demotion at narrow widths** — cheap, easy to forget, and
   forgetting it ships a broken 0.5× screen. Put it in the ladder function, not in a kind.
4. **`FpsDisplay.cpp` refactor blast radius** — only if the live preview is attempted.
5. **Registration-time asserts on an existing codebase** — every setting must acquire a
   `Help()` and a `Default()`. That is ~90 short strings someone has to write. Not hard,
   but it is real work that does not look like work, and skipping it is not an option
   because the asserts are the mechanism (`API.md` §6).

**Gone from A's register:** gamepad plumbing (A's risk #4 — *"confirm gamescope actually
routes a gamepad into the overlay's ImGui context"*). Support was dropped, so the question
does not need answering and the design no longer leans on an unverified input path.
