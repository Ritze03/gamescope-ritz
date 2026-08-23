# Feasibility — E1 against stock (non-docking) Dear ImGui 1.92.9b

Honest assessment. Manual layout, `ImDrawList`, animation via `io.DeltaTime`, **no
docking branch**. Where this is weak, it says so. Deltas from E's own FEASIBILITY are
called out, because most of E's assessment survives and repeating it would hide what
actually changed.

---

## 1. The three-region split — unchanged, and still easy

"Three resizable regions" *sounds* like it needs docking. It does not.

```cpp
const ShellLayout L = ComputeLayout( ImGui::GetIO().DisplaySize );

ImGui::SetNextWindowPos ( L.slab.Min );
ImGui::SetNextWindowSize( L.slab.GetSize() );
ImGui::Begin( "##bench", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
    | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus );

DrawSlabChrome();
ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,   ImVec2( 0, 0 ) );
ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );

ImGui::BeginChild( "##rail",  L.rail.GetSize()  ); DrawRail();  ImGui::EndChild();
ImGui::SameLine( 0, 0 );
ImGui::BeginChild( "##sheet", L.sheet.GetSize() ); DrawSheet(); ImGui::EndChild();
if ( !L.bInspectorDrawer ) { ImGui::SameLine( 0, 0 );
    ImGui::BeginChild( "##insp", L.inspector.GetSize() ); DrawInspector(); ImGui::EndChild(); }

ImGui::PopStyleVar( 2 );
if ( L.bInspectorDrawer ) DrawInspectorDrawer();   // same body, PushClipRect + overdraw
ImGui::End();
```

`BeginChild` gives, correctly and for free, the four things a hand-rolled region gets
wrong: an independent scroll position, automatic clipping, a per-region ID scope, and
correct hit-testing where regions meet. Region boundaries are one `AddLine` each on the
slab's draw list *after* the children, so a neighbour cannot clip them away.

**Risk: none material.** Every pre-docking ImGui tool is built this way.

**Not resizable, deliberately.** Three discrete widths (`ui.inspector ∈ {hidden, normal,
wide}`, `ui.rail ∈ {icons, labels, auto}`), no continuous splitter. A splitter is ~30
lines to write and a permanent liability to keep correct — this repo's #33 (collapse),
#34 (measure-and-grow sizing) and #42 (hand-rolled drag) are all the same machinery
failing. Discrete states survive a `display_scale` change trivially (base units multiplied
at use) and are screenshot-testable. Real capability loss: nobody can have a 600px
inspector for one session.

---

## 2. The rail rule — the one genuinely new implementation concern

Right-binding every control is **easier** in immediate mode than left-binding at a
percentage, because ImGui already hands you a rect and every existing `widgets::` painter
already draws into a computed box. The concern is not drawing; it is **hit-testing and
ID scoping** when a control is narrower than its zone.

Three concrete points:

1. **`ItemAdd` must cover the row, not the control.** Selection is per-row, so the row's
   `ItemAdd` uses the full row rect. The control then adds its own smaller item *inside*
   it. ImGui handles nested items fine, but the row's `IsItemHovered()` must be sampled
   **before** the control's `ItemAdd`, or the control steals the hover and the row
   background stops painting. One ordering rule in `BeginRow`, easy to get wrong once and
   then never again.

2. **`PushClipRect` at the rail costs one clip-rect change per row.** `ImDrawList` merges
   consecutive commands with identical clip rects; alternating row/control clip rects
   would double the draw-command count. Fix: push the clip rect once per *sheet column*,
   not per row — the rail is a column property, not a row property, and `Place()` already
   clamps to it. This is why the rail is defined as `content right − 36` rather than
   per-row.

3. **Fit-width controls need a measured width before placement.** `Place( w )` needs `w`,
   and for a stepper `w` depends on the formatted value's text width (`"Unlimited"` is
   wider than `"60 fps"`). Steppers are therefore **fixed at 136 base**, not
   content-sized, and long words ellipsise. That is a small, deliberate loss that keeps
   the rail exact and avoids a value feeding back into layout — the same class of bug as
   #34.

**Verdict: lower risk than E's 44% label column**, which needed the same measurement work
*and* left the right edge ragged.

---

## 3. Composites and `ImGuiListClipper`

E tied its "exactly two row heights, ever" rule to the clipper. E1 adds `RowBlock` (112)
for the position grid, so that claim needs re-examining rather than restating.

- **Settings sheets do not need a clipper.** After SPEC §3 the largest sheet is the System
  Monitor: 4 list rows + 4 control rows + one composite ≈ 12 rows ≈ 200 `ImDrawList`
  primitives. Drawing all of it costs less than the clipper's own bookkeeping.
- **The LOG does need one**, and the LOG has no composites: every line is 20 base units,
  so the clipper is exact and a 40 000-line buffer costs one screenful.
- **If a sheet ever does need one**, `ImGuiListClipper` supports manual step counts, and
  the Sheet already knows every row's height because it assigned it — a prefix-sum
  rebuilt on row-count change makes an exact clipper possible. Deferred until measured.

So the rigidity still buys the thing it needs to buy, in the one place it is load-bearing.
The honest cost of `RowBlock` is that "two heights" became "two heights plus one named
exception", which is weaker as a slogan and identical in practice, since `RowBlock` is
unreachable from a call site.

**The composite's own implementation is trivial**: draw the head row, draw the parts with
`NoHairline()`, then one `AddLine` and one `AddRectFilled` (the state edge) spanning
`top` to `GetCursorScreenPos().y`. Two saved cursor positions, no measurement pass.

---

## 4. Selection, focus and nav

**Recommendation, unchanged from E and now cheaper: accept ImGui's nav wholesale.**
Selection *is* nav focus, so `Tab`, arrows and `Enter` all come from ImGui and the
Inspector simply reflects the focused row's entry. This deletes a whole custom focus
system.

Dropping gamepad removes E's worst-rated risk outright (E's own §6.3: *"three focus scopes
is genuinely harder for gamepad… the part of this proposal most likely to need a second
pass after contact with a Steam Deck"*). What is left is:

- **Cross-region `←/→`** still needs `ImGuiNavMoveFlags` nudging at region edges. Known,
  documented, and now only has to be right for a keyboard.
- **`←/→` inside a control vs. crossing a region** is genuinely ambiguous and needs a
  rule: *if the focused item is a control that consumes horizontal input (slider,
  segmented, stepper, grid), `←/→` adjusts; otherwise it crosses.* Decidable from the
  entry kind, so the shell decides it.
- **`Ctrl+K` / `Ctrl+I` / `Ctrl+D` / `Ctrl+B`** must be consumed by the overlay and not
  forwarded — the existing capture path already does this for everything while the overlay
  is open.

**Palette IDs, from B's write-up and worth keeping:** palette rows must `PushID` the
entry's **stable string id**, not the loop index, or a filtered list changing every
keystroke makes ImGui think a dragged widget became a different widget mid-drag.

---

## 5. Width budget, measured

Base units: rail 240 (icons 64), inspector 384, sheet minimum 560, content column
`min(720, sheet − 48)`. Surface 1920×1080, slab `min(1728, max(1560 × scale, 1180))`.

| Scale | Slab px | Slab base | Rail | Insp | Sheet base | Step | Columns | Content col | Label zone |
|---|---|---|---|---|---|---|---|---|---|
| 0.5× | 1180 × 470 | 2360 × 940 | 240 | 384 | 1736 | 3 | 2 | 720 | 352 |
| 0.75× | 1180 × 705 | 1573 × 940 | 240 | 384 | 949 | 0 | 1 | 720 | 352 |
| **1.0×** | **1560 × 928** | 1560 × 928 | 240 | 384 | 936 | 0 | 1 | 720 | 352 |
| 1.25× | 1728 × 928 | 1382 × 743 | 240 | 384 | 758 | 0 | 1 | 710 | 343 |
| 1.5× | 1728 × 928 | 1152 × 619 | **64** | 384 | 704 | 1 | 1 | 656 | 300 |
| 2.0× | 1728 × 928 | 864 × 464 | **64** | drawer | 800 | 2 | 1 | 720 | 352 |

Read that as the answer to #50: **the three-region layout survives intact to 1.5× and to
2.0× with the inspector as a drawer**, and at 2.0× the sheet has *more* base width (800)
than at 1.25× (758), because collapsing the rail and floating the inspector returns more
space than the scale-up consumes.

At 2.0× the drawer covers 384 of the sheet's 800 base units (48%) while open. Acceptable
for a transient detail panel and unacceptable for a required one — which is exactly what
SPEC §1.4's Inspector Contract is for.

**Vertical is now the tighter budget, not horizontal.** At 2.0× the slab is 464 base tall;
minus title 40, header 56 and footer 44 leaves 324 — about seven 44-unit rows. Every sheet
in the design fits in seven rows *except* the System Monitor (12) and the Audio mixer (4
composites × 3 parts + head = 16). Those scroll. That is fine, but it is worth naming: the
sheets that scroll at 2.0× are exactly the two that are lists, and lists scroll gracefully
where a form does not.

**0.5× remains the under-discussed risk.** A 1180×470 slab on a 1920 screen looks small,
and two columns of 720 leaves 296 base units of gutter. The 1180 physical floor and the
hard two-column clamp keep it from becoming a spreadsheet, but this is the one scale that
needs a real screenshot before shipping.

---

## 6. Contrast, atlas, blur

**The contrast changes are free.** Every value in SPEC §5 is an alpha in `Theme.h`; none
of them is a new mechanism, a new draw call or a new texture. The only *work* is
propagating `LineControl` (white@34%) into the five `widgets::` painters that currently
hardcode their border alpha, which is a five-line change per painter and is the same
change that makes them rect-taking anyway.

**One real consequence:** raising the control border from 18% to 34% makes a dense screen
visibly busier, which pulls against SPEC §4.1's "colour is state" discipline. The
mitigation is that borders are *neutral* white, not accent, so they read as structure
rather than as state — but if the first screenshot over a real game looks noisy, the
honest fallback is to raise `Surface` opacity (0.88 → 0.92) instead, which lowers the
worst-case background luminance and lets the border alpha come back down. That trade is
worth measuring on hardware; both ends of it clear 3:1.

**Font atlas.** Seven roles instead of ten, re-baked on `display_scale` change via the
existing non-idempotent `fonts::Load()` path (#38). No new mechanism, slightly smaller
atlas.

**Hairlines at 0.5×.** `1 × 0.5 = 0.5px` aliases away. Rule:
`ImMax( 1.0f, ImFloor( 1.0f * scale ) )` — 1px at 0.5–1.99×, 2px at 2.0×. One helper,
`ui::Hairline()`, and no call site computes it.

**Backdrop blur.** Unchanged, and strictly simpler than six floating windows: one rect to
blur behind instead of six, and no overlapping-blur double-darkening where windows stack.
Per-region alpha (rail darker than sheet) is a fill inside the same texture — free.

**`ImDrawList` budget.** A sheet at 1.0× after SPEC §3 is roughly 12 rows × ~12 primitives
≈ 150, plus rail ~60, inspector ~250, chrome ~60 — well under 700 per frame, against the
low thousands the current six-window layout already produces. The mockup's Category Card
is the heaviest single region and it is still smaller than one of today's panels.

---

## 7. What this direction is worst at — candidly

E's list, updated. Two items got better, one got worse, three stand.

1. **First impression — improved, still the heaviest.** SPEC §3 removed 32 read-only rows
   and 8 expert settings from the sheets, and one column replaced two. Upscaling is now
   five rows. But it is still a rail plus a table plus a metadata panel: the console
   direction wins the first five seconds and this one does not. Mitigations are real and
   partial — the overlay reopens on the last category, `Ctrl+K` is one keystroke to
   anything, the FPS HUD's quick toggles never require opening the slab, and an unmodified
   screen is nearly monochrome.

2. **You always pay for navigation.** 240 base units of rail every frame, including when
   the user came to flip one switch. Fixed cost is the price of "everything is always in
   the same place", and it only pays off for someone who opens the overlay often.

3. **The Inspector will attract junk — the weakest guarantee, slightly stronger now.**
   Making `Readout()` Card-only means read-only content lands in a *generated* surface
   rather than a hand-authored one, which is structural. But `Depth::Expert` is still a
   field an author sets, and three releases from now the Inspector is where settings go to
   hide. `ui_lint` and `ui_snapshot` are the defence and they are process, not structure.

4. **The Category Card is now load-bearing, which is new risk.** E's card was a nicety;
   E1's card is where every read-only fact in the product lives. If it is slow to read, or
   if a user does not discover that clearing the selection shows it, the density fix
   becomes a density *hide*. Mitigation: the card is the default state of every category
   (no selection = card), and `Esc` clears the selection to get back to it. Worth one
   round of real use before committing.

5. **More code than the alternatives.** A rail, a sheet, an inspector, a card, a ladder, a
   composite system, a registry, a palette and a lint command is more surface than a single
   scrolling column. The helper pays it back on every subsequent setting; the first landing
   is still the largest of the directions.

6. **Density can outrun the content — worse than in E, and this is the item that
   regressed.** Frame Limiter is three rows. Per-Game is two switches and two buttons. In a
   936-unit sheet next to a 384-unit inspector, with the second column now *gone*, small
   categories look emptier than they did in E. Partly answered by the Category Card filling
   the Inspector with something worth reading; not fully. The honest options are to merge
   thin categories (Frame Limiter into Output) or to accept the air. **Recommendation:
   accept the air** — a calm sheet was the entire point of critique (3), and inventing
   filler to occupy it would re-create the problem.

7. **`Escape()` is a hole for as long as it exists.** A hosted legacy panel body ignores
   the rail, the taxonomy and the contrast floor. It is visibly wrong on purpose, and
   `ui_lint` counts it, but a half-migrated overlay looks worse than either endpoint.

---

## 8. Migration cost

**Estimate: ≈ 2 weeks of focused work, six PRs, net roughly LOC-neutral.**

| PR | Work | New | Deleted |
|---|---|---|---|
| 1 | `UI/Shell` — slab, three regions, `ComputeLayout`, ladder, rail, thread, breadcrumb, footer, `Escape()`. Every existing panel hosted verbatim. **Ships as a working overlay on day one.** | ~650 | 0 |
| 2 | `UI/Registry` + `UI/Bind` — Area/Entry/Composite/List builders, registration assertions, `ui_snapshot`. No rendering yet. | ~700 | 0 |
| 3 | `UI/Row` + `UI/Controls` + `UI/Composite` — the rail rule, the 15 control kinds, the four composites, `ui_lint`, `ui_rulers`. Ports `Widgets.cpp`'s painters behind rect-taking signatures and applies the §5 contrast values. | ~1300 | ~140 |
| 4 | Convert Display (4 areas), Shaders, Mixer. `Inspector` + `Card`. | ~800 | ~1400 |
| 5 | Convert Config (3 areas) and System Monitor (list + inspector; six tab bars go). | ~850 | ~1900 |
| 6 | Convert LOG (`PaneCtx`, filter bar, minimap, line inspector); command palette + `Match`; delete `Chrome.cpp`'s dock/window/drag/collapse/tiling; delete `Escape()`. | ~550 | ~1100 |
| | **Total** | **≈ 4850** | **≈ 4540** |

**Delta vs E: about +650 new lines**, all of it in PR 2 — the registry that E did not have.
That is the price of the palette actually working, of `gamescopectl ui set`, and of the
Category Card being generated rather than hand-written per area. E's palette registered
rows during drawing, so it only ever knew about categories the user had already opened;
that is a bug, and the registry is what fixes it.

The estimate's real content is the sequencing. PR 1 is the one that matters: because
`Escape()` hosts unmigrated panel bodies unchanged, the shell ships and can be lived with
*before* a single panel is rewritten. If the density still feels oppressive over a game,
the cost sunk is one week rather than the whole redesign — the right shape of bet for the
heaviest of the directions.

Two things are cheaper than they look: `Widgets.cpp` moves almost intact (the measured
slider spec, already signed off, is preserved to the pixel, and only its fill gradient
start and rail tick alpha change), and the four tab bars and six floating windows are
*deleted*, not ported. Two are more expensive: System Monitor (six tabs into
list+inspector is an information-architecture rewrite, not a re-layout) and the Category
Card, which is new surface with no equivalent in the current code.
