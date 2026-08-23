# Feasibility — E2 against stock (non-docking) Dear ImGui 1.92.9b

Honest assessment. Manual layout, `ImDrawList`, `io.DeltaTime` animation, no docking
branch, no gamepad path. Where E2 is weak, it says so — including where it is weaker
than E.

---

## 1. The three regions, and why E2 needs *less* from ImGui than E did

E's `BeginChild` × 3 pattern is unchanged and unproblematic:

```cpp
const ShellLayout L = ComputeLayout( ImGui::GetIO().DisplaySize );

ImGui::SetNextWindowPos ( L.slab.Min );
ImGui::SetNextWindowSize( L.slab.GetSize() );
ImGui::Begin( "##bench", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
    | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus );

DrawSlabChrome();
ImGui::BeginChild( "##rail",  L.rail.GetSize()  ); DrawRail();  ImGui::EndChild();
ImGui::SameLine( 0, 0 );
ImGui::BeginChild( "##sheet", L.sheet.GetSize() ); DrawSheet(); ImGui::EndChild();
if ( L.eInspectorHost == Host::Column ) {
    ImGui::SameLine( 0, 0 );
    ImGui::BeginChild( "##insp", L.inspector.GetSize() ); DrawInspector(); ImGui::EndChild();
}
ImGui::End();
```

`BeginChild` supplies, correctly and free, the four things a hand-rolled region gets
wrong: independent scroll per region, clipping to the rect, per-region ID scope, and
correct hit-testing near a neighbour's scrollbar. Docking would add user-rearrangeable
regions, tab-merged regions and floating a region into an OS window — all
meaningless-to-harmful in a game overlay.

Three E2-specific simplifications relative to E:

- **No gamepad**, so no cross-region D-pad routing, no `ImGuiNavMoveFlags` nudging, and
  no third focus scope to reconcile. E's own §6.3 named this its riskiest area; E2
  deletes the risk rather than mitigating it.
- **One row height (44)**, so `ImGuiListClipper` is exact everywhere with no prefix-sum
  and no `IncludeItemsByIndex` special-casing (§3).
- **One row layout mode.** E had three (one-line, two-line at `W < 300`, inspector rows).
  E2's narrowest real case is 2.0× at 804 base with a 394-base control zone
  (`SPEC.md` §8.3), which never triggers two-line. `Row.cpp` loses a branch.

**Risk: none material.** Region boundaries are one `AddLine` each on the slab's draw list
after the children, so they are not clipped by either neighbour. The one gotcha is zeroing
`WindowPadding`/`ItemSpacing` around the children — a single `PushStyleVar` pair.

---

## 2. The right-bound allocator against ImGui's cursor model

`RowCtx::Place()` returning a right-anchored `ImRect` is the load-bearing mechanism of
fix #2, and it fits ImGui *better* than the cursor-relative style, because it bypasses
the cursor entirely:

```cpp
const ImRect bb = AllocateRow( 44.f * scale );      // ItemSize + ItemAdd on the row
ImGui::PushID( pEntry->Id() );                      // stable string id, not the index
const ImRect ctl = Place( 30.f );                   // right-anchored inside bb
ImGui::ItemAdd( ctl, ImGui::GetID( "##sw" ) );      // the control's own interactive id
widgets::Toggle( ctl, bind );                       // paints into the rect
```

Consequences to plan for, all real:

- **Every `widgets::` painter must take an `ImRect`.** `SliderControl()` currently reads
  `GetContentRegionAvail()` — one signature change, already flagged in E's API §8.
  `Toggle`, `SegmentedControl` and `PositionGrid` already draw into a computed box and
  move over untouched. `Checkbox` is deleted.
- **`PushID` must use the entry's stable string id**, never the loop index. The palette
  filters and the `Repeat` collections change list contents between frames; index-derived
  ids would make ImGui think a slider being dragged became a different widget mid-drag and
  silently drop the drag. This is Direction B's finding and it applies verbatim here.
- **Grep-able invariant.** `Areas/` includes only `Registry.h`; a float literal in
  `Areas/` is a review failure. `Row.cpp`, `Controls.cpp` and `Composite.cpp` are the only
  files containing geometry constants.

**Risk: low.** This is more manual than idiomatic ImGui, but it is the manual style the
project already uses in `Widgets.cpp`.

---

## 3. Clipping, scrolling, and the composite band

**Sheets (≤ ~60 rows).** Plain `BeginChild` scroll; no clipper needed. Keyboard selection
uses `ImGui::SetScrollHereY( 0.5f )` when selection leaves the visible band.

**The LOG (up to ~40 000 lines).** `ImGuiListClipper` at a uniform 20 base per line —
exact, whole buffer costs one screenful. The severity minimap on the scrollbar is drawn
from a pre-computed index of error/warn line numbers maintained by `LogCapture` on append;
it never iterates the buffer per frame.

**Composite bands and the clipper.** A band is `n × 44`, so it is *n* clipper items whose
first item paints the entire band and whose items 2..n paint nothing. The clipper's
uniform step survives untouched. This is the concrete payoff of the height-quantisation
clause in `SPEC.md` §4.2 — E's `RowTall` 76 next to `Row` 44 could not do this and its
feasibility doc had to defer a prefix-sum solution.

**Inline expansion and the clipper.** Expansion state lives in `m_Expanded` and is known
*before* `clip.Begin()`, so the total item count is computable up front:

```cpp
int nItems = 0;
for ( const Entry &e : rows )
    nItems += e.Lines() + ( m_Expanded.contains( e.Id() ) ? e.ParamCount() : 0 );
clip.Begin( nItems, 44.f * scale );
```

No mid-frame height change, therefore no clipper corruption. **Scroll anchoring is the
real cost**: expanding a row above the viewport shifts everything below it. Mitigation is
ImGui-native — record `GetScrollY()` and the expanded row's `bb.Min.y` before the toggle,
restore the delta after — but it is fiddly and it is the one place inline mode will need a
second pass. Honest severity: a polish bug, not a correctness one.

**A real ImGui footgun, unchanged from E.** ImGui's auto-scroll-to-focused-item fights a
custom selection model. **Recommendation stands: accept ImGui's nav wholesale** —
selection *is* nav focus, the Inspector reflects `ImGui::GetFocusID()`'s row, and the whole
custom focus system disappears. Dropping gamepad makes this cheaper than it was for E,
because `Tab`/arrows are exactly what ImGui nav already does.

---

## 4. The Inspector as a pure function — is it actually cheap?

`Inspector.cpp` reads the selected entry's registration and renders. Costs, measured in
`ImDrawList` primitives per frame at 1.0×:

| Region | Primitives |
|---|---|
| Sheet, 40 rows × (hairline + label + control ≈ 12) | ≈ 500 |
| Rail, 11 items + 5 headers | ≈ 60 |
| Inspector — Explain (prose + 7-row fact grid + 2 links + reset) | ≈ 130 |
| Inspector — Configure (≤ 6 rows) | ≈ 90 |
| Inspector — Diagnose (≤ 4 facts + 1 animated element + 5 log lines) | ≈ 180 |
| **Worst frame** | **≈ 750** |

Against the low thousands the current six-window layout already produces, and against the
Vulkan composite this overlay runs every frame, this is not a concern.

**Two things in Diagnose that are *not* free and need an index:**

1. **`DrawKeyLogTail( entryId, 5 )`** — "the last 5 log lines containing this key". A
   naïve implementation scans a 40 000-line ring buffer every frame. It must be a
   `unordered_map<string_view, small_vector<int,8>>` built incrementally by `LogCapture`
   on append, keyed by the registry's ids (~115 of them). Cost: one substring scan per
   appended line against 115 needles — an Aho–Corasick automaton built once at
   registration, or, honestly, a plain loop, because log append rate is tens of lines per
   second, not thousands. **Cost: ~80 LOC. If it is not built, the feature must be cut,
   not shipped naïvely.**
2. **Transient live content** (a shader preview, an L/R meter) runs a `.Live()` lambda per
   frame. Safe because the lambda runs **only for the selected entry** — E's insight,
   kept — and because `SPEC.md` §5.4 caps Diagnose at one animated element per selection.
   `ui_lint` warns above the cap.

---

## 5. The registry / immediate-mode fusion, honestly

Adopting B's registry into E's shell has one genuine seam: **B's registry is built once at
startup; three of E2's panels are collections of live objects** (shader effects, PipeWire
streams, saved profiles) whose membership changes at runtime.

`Repeat( idPrefix, count, build )` (`API.md` §8) re-expands its sub-entries only when
`count()` changes. That is correct and cheap, but it has consequences worth stating:

- **Ids must be derived from the object's identity, not its index.** `shaders.fx.<name>`,
  not `shaders.fx.3`. Otherwise reordering the effect list silently re-points every
  binding. This is a discipline point the registration assert can only partially catch
  (it can reject a purely numeric leaf; it cannot verify stability).
- **A `Repeat` whose count changes while a param is being dragged** must not invalidate
  the dragged binding. Stable string ids solve it, per §2.
- **The palette index rebuilds on re-expansion.** ~150 entries, a few microseconds; only
  on topology change.

**Risk: medium-low, and it is the newest machinery in the proposal.** It is also the only
part that neither E nor B had to build, because E drew collections per-frame and B had no
collections.

---

## 6. What breaks if the Inspector is collapsed

The mandated question, answered concretely. Three hosts, with what is lost at each.

| Host | When | Params | Explain | Diagnose | Overview |
|---|---|---|---|---|---|
| **Column** | ladder 0–1 (≤ 1.5× on 1920) | in Inspector | in Inspector, **simultaneous with editing** | in Inspector | on no selection |
| **Drawer** | ladder 2 (1.75×–2.0×) | in Inspector, overlaying 400 of 804 base | same, but covers half the sheet | same | same |
| **Inline** | ladder 3, or persisted `ui.inspector = hidden` | **inline in the sheet**, ≤ 6 rows, one level, `▸` disclosure | **full-sheet Explain page** via `?` / `Ctrl+/`, with a back crumb | **full-sheet Diagnose page**, same route | replaces the sheet on `Ctrl+/` with nothing selected |

**Nothing becomes unreachable, and it is a test, not a promise** (`API.md` §5.1):

```
] ui_lint --host=inline
  entries with a rect  74 / 74     params with a rect  41 / 41     unreachable 0
```

**What is genuinely lost in Inline, stated plainly:**

1. **Simultaneity.** You cannot read a setting's help while adjusting it. Explain becomes a
   page you go to and come back from. This is the one capability the Inspector column buys
   that nothing else can, and it is why Inline is a *degraded* mode rather than an equal one.
2. **The sheet reflows.** Expansion pushes rows down — the one licensed violation of
   "regions never move" (`SPEC.md` §8.4). It is user-initiated, never spontaneous, and it
   carries the scroll-anchoring caveat from §3.
3. **The Overview card has no home.** Category-level provenance, the `differs` list and
   the effective-path readout are only reachable via `Ctrl+/`. A user in Inline mode who
   never presses it never learns which file their settings write to. **This is the
   strongest argument for defaulting `ui.inspector = column` and for the ladder, not the
   user, being the usual reason it collapses.**

**And what does *not* break:** every Param remains findable via `Ctrl+K` in all three
hosts, with the `param` chip, jumping to the parent row and focusing the param. Depth is
never the same as hidden.

### 6.1 The 2.0× arithmetic that makes the question tractable

Slab on 1920 at 2.0× = 1728 physical = **864 base**. Icon rail 60, drawer 400 (overlay,
not subtracted), sheet **804 base**, one column, `Lw = 370`, control zone 394, affordance
28. A 44-tall row is 88 physical px. Nothing wraps; the two-line escape hatch is never
reached.

The counter-intuitive result — **804 base at 2.0× versus 750 at 1.25×** — holds because
collapsing the rail (232 → 60) and floating the inspector (400 → 0 subtracted) returns
572 base units, which is more than the scale-up costs. The direction that looks most
width-hungry has the most usable content column at the extreme.

---

## 7. Where E2 is worse than E — candidly

1. **Deleting the checkbox costs vertical density.** A 7-member set was 7 × 28 = 196 base
   as checkbox rows; as switch rows it is 7 × 44 = **308 base**, +57%, or 12% of a
   928-base sheet. This is a real cost paid to fix "switches and checkboxes are mixed",
   and it is why `GroupCount()` exists (the `4 / 7` chip and `all` / `none` in the band
   header remove the need to count switches by eye). No set in the product exceeds seven
   members today; one that did would want to be a Choice or a category.

2. **Deleting `.Hint()` removes at-a-glance guidance.** *"higher = sharper"* used to be
   readable while scanning; now it costs a click. This is the direct trade the user asked
   for — *"this type of information shouldn't leave the UI, but rather wander into the
   Inspector Rail"* — but it is a trade, and a first-time user scanning Upscaling gets
   less than E gave them. Mitigations: Explain is one click and always populated, and the
   Inspector defaults to open.

3. **Raising the slider rail from 16% to 34% deviates from `slider-widget-spec.md`,
   which the user has already signed off.** The justification is measurement (1.60:1 →
   3.07:1, `SPEC.md` §7.4), not taste, but it is a visible change to an approved widget
   and needs explicit re-approval rather than being slipped in. Everything else in the
   slider spec is preserved to the pixel.

4. **`ui_lint --host=inline` is not free to build.** A headless render pass needs a real
   `ImGui::NewFrame`/`EndFrame` with the shell forced into Inline and drawing into a
   scratch draw list. ~120 LOC plus a `--headless` guard so it does not disturb the live
   overlay. If it is not built, the Reachability Law degrades back into E's promise — so
   it is not optional, and it must land in the same PR as `.Param()`.

5. **The `Repeat` machinery is new** (§5) and is the only part of the proposal with no
   prior art in either parent direction.

6. **First impression is still heavy.** Three regions and a rail on top of a running game
   is a DAW dropped on Elden Ring. E2 improves this measurably — 23 sheet text lines down
   to 8 on the Monitor screen, one row height, one control line, a near-monochrome
   at-default state — but it does not change the shape. A user who opens the overlay twice
   a session still pays 232 base units of rail for navigation they did not need. `Ctrl+K`
   is the honest answer and it is one keystroke.

7. **Small categories look emptier than in E.** Frame Limiter is two settings; stripping
   hints and notes leaves two 44-tall rows in a 928-base sheet. The Overview card and the
   `Summary()` line carry it, but the smallest categories will always look sparse in a
   shell built for the largest — and E2 made them sparser.

---

## 8. Other ImGui-specific concerns

**Backdrop blur.** Unchanged and simpler than the status quo: one rect to blur behind
instead of six, no overlapping-blur double-darkening. Per-region alpha (rail darker by
0.06) is a fill inside the same texture — free.

**Font atlas.** Six roles (down from E's seven, today's ten), re-baked on `display_scale`
change through the existing non-idempotent `fonts::Load()` path (#38). Smaller atlas,
same mechanism.

**Hairlines across the scale range.** `ui::Hairline() = ImMax( 1.f, ImFloor( 1.f * scale ) )`
— 1px from 0.5× to 1.99×, 2px at 2.0×. One helper; no call site computes it.

**Animation.** One shared `ui::Anim( float &cur, float target, float durationMs )` lerped
against `io.DeltaTime`, available for exactly the three durations in `SPEC.md` §8.4 and
nowhere else, so a caller cannot animate a value. The Inspector's host change (160 ms) is
a width lerp on `L.inspector`, which means `ComputeLayout()` must accept an animated
width — the one place the "pure function of surface size" property is compromised, and it
is compromised deliberately and in one line.

**Input capture.** Unchanged; the overlay already takes input when open. `Ctrl+K`,
`Ctrl+I`, `Ctrl+D` and `Ctrl+/` must be consumed and not forwarded, which the existing
capture path already does for everything while the overlay is open.

**ID collisions.** Stable string ids per entry inside per-region children eliminate the
current `"Strength##vibrancy"` / `"Strength##presharpen"` / `"Strength##adaptivebrightness"`
manual disambiguations — those become `image.shaders.vibrancy.strength` etc. by
construction.

---

## 9. Migration cost

**Estimate: ≈ 2–2.5 weeks of focused work, landable as six PRs, net roughly LOC-neutral.**
Slightly more than E because of `.Param()`, the inline host and `ui_lint --host=inline`;
slightly less because gamepad is gone and the taxonomy is smaller.

| PR | Work | New | Deleted |
|---|---|---|---|
| 1 | `UI/Shell` — slab, three regions, `ComputeLayout`, ladder, rail, breadcrumb, footer, `Escape()`. Every existing panel hosted verbatim. **Ships as a working overlay on day one.** | ~700 | 0 |
| 2 | `UI/Row` + `UI/Controls` — the four columns, the right-bound allocator, the 10 taxonomy kinds, `Bind`/`Cfg`, the contrast palette pass. Ports `Widgets.cpp` behind rect-taking signatures; **deletes `Checkbox`**. | ~1200 | ~180 |
| 3 | `UI/Registry` + `Inspector` — Area/Entry/Param, the four generators, the Prefix Law and Six Budget asserts, Explain/Configure/Diagnose/Overview, `ui_lint`, `ui_snapshot`, **`ui_lint --host=inline`**, the inline host. | ~1100 | 0 |
| 4 | Convert Display (4 areas), Shaders (`Repeat`), Audio (`Repeat` + Strip composite). | ~600 | ~1300 |
| 5 | Convert Config (3 areas) and System Monitor (six tab bars → one area + `Facts` + Anchor composite). | ~800 | ~1900 |
| 6 | Convert LOG; command palette + `Match`; delete `Chrome.cpp`'s dock/window/drag/collapse/tiling; delete `Escape()`. | ~450 | ~1100 |
| | **Total** | **≈ 4850** | **≈ 4480** |

The sequencing matters more than the numbers. **PR 1 ships a working overlay before a
single panel is rewritten**, because `Escape()` hosts unmigrated bodies unchanged; every
later PR is independently revertible. If the density still reads as oppressive over a game
after PR 1, one week is sunk, not the redesign.

**PR 3 is the one that must not be trimmed.** `.Param()` without
`ui_lint --host=inline` is E's unenforceable contract wearing a new name. The
anti-junk-drawer law and the Reachability Law are the same rule (`SPEC.md` §6.3), and they
land together or neither is real.

Cheaper than it looks: `Widgets.cpp` moves almost intact, the four tab bars and six
floating windows are *deleted* rather than ported, and the gamepad pass is gone entirely.
More expensive than it looks: System Monitor (six tabs into one area with `Facts` and a
composite is an information-architecture rewrite, not a re-layout) and the inline host's
scroll anchoring (§3).
