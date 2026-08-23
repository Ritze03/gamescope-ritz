# Feasibility — Direction E against stock (non-docking) Dear ImGui 1.92.9b

Honest assessment. Where this direction is weak, it says so.

---

## 1. Building the three-region split without the docking branch

This is the easiest part of the whole proposal, and it is worth saying plainly because
"three resizable regions" *sounds* like it needs docking. It does not.

```cpp
const ShellLayout L = ComputeLayout( ImGui::GetIO().DisplaySize );

ImGui::SetNextWindowPos ( L.slab.Min );
ImGui::SetNextWindowSize( L.slab.GetSize() );
ImGui::Begin( "##bench", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
    | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus );

DrawSlabChrome();                                        // backdrop, border, focus glow, title

ImGui::BeginChild( "##rail",      L.rail.GetSize(),      ImGuiChildFlags_None );  DrawRail();      ImGui::EndChild();
ImGui::SameLine( 0.0f, 0.0f );
ImGui::BeginChild( "##sheet",     L.sheet.GetSize(),     ImGuiChildFlags_None );  DrawSheet();     ImGui::EndChild();
ImGui::SameLine( 0.0f, 0.0f );
ImGui::BeginChild( "##inspector", L.inspector.GetSize(), ImGuiChildFlags_None );  DrawInspector(); ImGui::EndChild();

ImGui::End();
```

`BeginChild` gives, for free and correctly, the four things a hand-rolled region would
get wrong: an independent scroll position per region, automatic clipping to the region
rect, a per-region ID scope (so `PushID( rowIndex )` in the sheet cannot collide with
the rail's ids), and correct hover/hit-testing when regions overlap the sheet's
scrollbar.

**What docking would have added and we do not want:** user-rearrangeable region order,
tab-merging of regions, and floating a region out into an OS window. All three are
meaningless-to-harmful in a game overlay. The *only* docking feature this design would
have used is the splitter, and §2 argues we should not have one anyway.

Region boundaries are drawn manually — one `AddLine` per boundary on the slab's draw
list, after the children, so the line is not clipped away by either neighbour. Cheap
and exact.

**Risk: none material.** This pattern is used by essentially every ImGui tool that
predates the docking branch. The single gotcha is that `SameLine( 0, 0 )` plus
`ImGuiStyleVar_WindowPadding`/`ItemSpacing` need to be zeroed around the children or the
regions drift by a few pixels — one `PushStyleVar` pair in `DrawShell()`.

---

## 2. Are the regions resizable, and what would it cost?

**Recommendation: no. Three discrete widths, no continuous splitter.**

A splitter in stock ImGui is not hard to *write* — an `InvisibleButton` on the boundary,
`IsItemActive()`, accumulate `io.MouseDelta.x`, clamp, done, ~30 lines. It is hard to
*keep correct*, and this repo has the scar tissue to prove it:

- **#34** — windows sized by measuring their own content and growing, which is a
  layout value feeding back into the thing that produced it.
- **#42** — the window drag had to be hand-rolled per-frame with `SetWindowPos` deltas
  because `NoMove` blocks `StartMouseMovingWindow()`; a splitter drag is the same
  machinery, with the same failure modes.
- **#33** — collapse had to be reimplemented because the native mechanism was wired to a
  title bar that no longer exists.

A splitter adds, specifically: a persisted float that must be re-interpreted when
`display_scale` changes (store base units or physical? get it wrong and 2.0× users
find their inspector 4px wide); clamping that must interact with the responsive ladder
(what happens when the user drags the inspector to 800 and then raises the scale?); a
drag that must not be captured by the row under the cursor; and a resize path that
invalidates `ImGuiListClipper` assumptions mid-frame.

Instead: **`ui.inspector_width ∈ { hidden, normal, wide }`** (0 / 384 / 520 base) and
**`ui.rail ∈ { icons, labels, auto }`**, both persisted in `global.json`, both toggled
by a control in the slab title bar and by `Ctrl+I` / `Ctrl+B`. Discrete states are
screenshot-testable, survive a scale change trivially (they are base units multiplied
at use), and compose with the ladder by simple precedence: an explicit user choice wins
until the ladder says it does not fit, at which point the ladder's step applies and the
user's preference is remembered for when it fits again.

This is a real capability loss — someone who wants a 600px inspector for one session
cannot have it. It buys the removal of the highest-risk interaction in the design.

---

## 3. Scroll handling in dense lists

Three cases, three answers:

**Sheet (≤ ~120 rows).** A plain `BeginChild` scroll. No clipper needed; drawing 120
rows costs a few hundred `ImDrawList` primitives, which is nothing next to the Vulkan
composite this overlay already runs. Keyboard/gamepad selection uses
`ImGui::SetScrollHereY( 0.5f )` when the selection moves outside the visible band —
ImGui's own scroll-to-item mechanism, no manual maths.

**Long uniform lists (LOG, up to ~40 000 lines).** `ImGuiListClipper`, which requires
uniform item height. The two-row-height rule from `SPEC.md` §2.1 exists partly for this
reason: the LOG's `Raw` rows are all 20 base, so the clipper is exact and the whole
buffer costs the same as one screenful. The severity minimap on the scrollbar is drawn
separately from a pre-computed index of error/warn line numbers, updated when
`LogCapture` appends — it does not iterate the buffer per frame.

**Mixed-height sheets.** A sheet with both 44 and 76 rows breaks a naïve clipper. Two
mitigations, in order: (a) sheets are short enough not to need one; (b) if one ever
isn't, `ImGuiListClipper` supports `IncludeItemsByIndex`/manual step counts and the
Sheet already knows every row's height because it assigned it — so a prefix-sum of row
heights, rebuilt only when the row count changes, makes an exact clipper possible.
Deferred until measured; not a blocker.

**A real ImGui footgun to plan for:** ImGui's auto-scroll-to-focused-item fights a
custom selection model if both are active. The Sheet must own selection and set
`ImGuiWindowFlags_NoNavInputs` on regions it is driving manually, or accept ImGui's nav
wholesale and derive selection from `GetFocusID()`. **Recommendation: accept ImGui's
nav wholesale** — selection *is* nav focus, so `Tab`/arrows/gamepad all come from ImGui,
and the Inspector simply reflects `ImGui::GetFocusID()`'s row. This deletes a whole
custom focus system and is the single most important implementation decision in the
proposal. It does constrain the design: hover cannot drive the Inspector (it does not,
by design) and cross-region `←/→` needs `ImGuiNavMoveFlags` nudging, which is a known,
documented area.

---

## 4. The 2.0× width budget — the arithmetic

Base units: rail 240 (icons 64), inspector 384, sheet minimum 560, slab max 1560, slab
physical floor 1180. Surface 1920×1080, slab width = `min(1728, max(1560 × scale, 1180))`.

| Scale | Slab px | Rail px | Inspector px | Sheet px | Sheet base | Ladder step | Result |
|---|---|---|---|---|---|---|---|
| **0.5×** | 1180 | 120 | 192 | 866 | 1732 | **−1** | three dense columns |
| **0.75×** | 1180 | 180 | 288 | 710 | 947 | 0 | two columns |
| **1.0×** | 1560 | 240 | 384 | 934 | 934 | 0 | two columns |
| **1.25×** | 1728 | 300 | 480 | 946 | 757 | 0 | one column |
| **1.5×** | 1728 | 96 | 576 | 1054 | 703 | **1** | icon rail, all three regions |
| **1.75×** | 1728 | 112 | drawer 672 | 1614 | 922 | **2** | icon rail, inspector overlays |
| **2.0×** | 1728 | 128 | drawer 768 | 1598 | 799 | **2** | icon rail, inspector overlays |

Read that table as the design's answer to #50: **the three-region layout survives to
1.5× intact and to 2.0× with the inspector as a drawer.** At 2.0× the sheet gets 799
base units of width — *more* than it has at 1.25× — because collapsing the rail and
floating the inspector returns more space than the scale-up consumed. The direction that
looks most width-hungry is, at the extreme, the one with the most usable content column,
precisely because it has two regions it can spend.

At 2.0× the drawer covers 768 of the sheet's 1598px (48%) while open. That is
acceptable for a transient detail panel and unacceptable for a required one — which is
exactly why `SPEC.md` §1.5's Inspector Contract exists. The contract is not a nicety;
it is what makes 2.0× work.

**0.5× is the under-discussed risk.** Everything fits with room to spare, and the danger
is the opposite one: a 780px slab on a 1920 screen looks lost, and three columns of
tiny rows look like a spreadsheet. Hence the 1180px physical floor and the hard clamp of
three columns. Worth a real screenshot check before shipping; it is the one scale where
this design could look silly.

---

## 5. Other ImGui-specific concerns

**Backdrop blur.** Unchanged by this proposal. The blur is produced by gamescope's own
composite pass behind the overlay texture, not by ImGui, so a one-slab design is
strictly *simpler* for it than six floating windows: one rect to blur behind instead of
six, and no overlapping-blur double-darkening where windows stack. Per-region alpha
(rail darker than sheet) is a fill drawn inside the same texture — free.

**Font atlas.** Seven roles instead of ten, re-baked on `display_scale` change via the
existing non-idempotent `fonts::Load()` path (#38). No new mechanism. Slightly smaller
atlas.

**Hairlines at 0.5×.** `1 × 0.5 = 0.5px` disappears or aliases. Rule: every hairline is
`ImMax( 1.0f, ImFloor( 1.0f * scale ) )`, which is 1px at 0.5–1.99× and 2px at 2.0×. One
helper, `ui::Hairline()`, and no call site computes it.

**`ImDrawList` budget.** A dense sheet at 1.0× is roughly: 40 rows × (1 hairline + 1
label + 1 control ≈ 12 primitives) ≈ 500, plus rail ~60, inspector ~200. Under 1000
primitives per frame, versus the low thousands the current six-window layout already
produces. Not a concern.

**ID collisions.** Every row does `PushID( m_nRowIndex++ )` inside a per-region child,
so two rows with the same label in different groups cannot collide — a real bug class in
the current code (`"Strength##vibrancy"`, `"Strength##presharpen"`,
`"Strength##adaptivebrightness"` are manual disambiguations that this removes).

**Input capture over a game.** Unchanged: the overlay already takes input when open.
The one new consideration is `Ctrl+K`, `Ctrl+I`, `Ctrl+D` — these must be consumed by
the overlay and not forwarded, which the existing capture path already does for
everything while the overlay is open.

---

## 6. What this direction is worst at — candidly

1. **First impression.** It looks like a DAW dropped on top of Elden Ring. A user who
   opens the overlay to nudge sharpness meets eleven rail items, a table of rows, and a
   panel of metadata. The console and radial directions win the first five seconds and
   this one does not. Mitigations are real but partial: the overlay reopens on the last
   category, `Ctrl+K` is one keystroke to anything, the FPS HUD's quick toggles never
   require opening the slab at all, and a screen where nothing has been changed is
   nearly monochrome and therefore calmer than the mockup implies. It is still the
   heaviest of the five directions and pretending otherwise would be dishonest.

2. **You always pay for navigation.** 240 base units of rail is spent every frame,
   including when the user came to flip one switch. The console direction spends zero.
   Fixed cost is the price of "everything is always in the same place", and that trade
   only pays off for a user who opens the overlay often. For a user who opens it twice
   per session, the rail is overhead.

3. **Three focus scopes is genuinely harder for gamepad than one list.** Cross-region
   navigation with a D-pad is the classic place console UIs feel wrong. §3's
   recommendation (accept ImGui nav wholesale) helps but does not eliminate it — the
   `←/→`-crosses-regions rule needs hand-tuning and real controller testing, and it is
   the part of this proposal most likely to need a second pass after contact with a
   Steam Deck.

4. **The Inspector will attract junk.** Every future setting with no obvious home has a
   tempting empty column to go into, and three releases later the Inspector is where
   settings go to hide. The Inspector Contract and `ui_lint` are the defences; they are
   process defences, not structural ones, which makes this the proposal's weakest
   guarantee.

5. **More code than the alternatives.** A rail, a sheet, an inspector, a ladder, a
   palette and a lint command is more surface than a single scrolling column would be.
   The helper layer pays that back on every subsequent setting, but the first landing is
   the largest of the five.

6. **Density can outrun the content.** Several categories are genuinely small — Frame
   Limiter is two settings. Two rows in a 934-unit sheet next to a 384-unit inspector
   looks empty in a way the current 470px window does not. Partly answered by
   `Density::Comfort` (bigger rows, one column) and by the Category Card filling the
   Inspector with something worth reading, but the smallest categories will always look
   sparse in a shell built for the largest.

---

## 7. Migration cost

**Estimate: ≈ 2 weeks of focused work, landable as six PRs, net roughly LOC-neutral.**

| PR | Work | New | Deleted |
|---|---|---|---|
| 1 | `UI/Shell` — slab, three regions, `ComputeLayout`, ladder, rail, breadcrumb, footer, `Escape()`. Every existing panel hosted verbatim as a `Raw`/escaped category. **Ships as a working overlay on day one.** | ~700 | 0 |
| 2 | `UI/Row` + `UI/Controls` — row grammar, the 18 control kinds, `Bind`/`Cfg`, `ui_lint`, `ui_snapshot`. Ports `Widgets.cpp`'s painters (slider, toggle, checkbox, segmented, position grid) behind rect-taking signatures. | ~1400 | ~120 |
| 3 | Convert Display (4 categories), Shaders, Audio. | ~650 | ~1300 |
| 4 | Convert Config (3 categories) and System Monitor (list + inspector; the six tab bars go). | ~900 | ~1900 |
| 5 | Convert LOG (raw sheet, filter bar, minimap, line inspector). | ~300 | ~185 |
| 6 | Delete `Chrome.cpp`'s dock/window/title-bar/tiling/drag/collapse machinery; delete `Escape()`; command palette; gamepad pass on hardware. | ~250 | ~1000 |
| | **Total** | **≈ 4200** | **≈ 4500** |

The estimate's real content is the sequencing, not the numbers. PR 1 is the one that
matters: because `Escape()` hosts unmigrated panel bodies unchanged, the shell can ship
and be lived with *before* a single panel is rewritten, and every later PR is
independently revertible. If the user tries PR 1 and finds the density oppressive over
a game, the cost sunk is one week, not the whole redesign — which is the right shape of
bet for the riskiest of the five directions.

Two things are cheaper than they look: `Widgets.cpp` moves almost intact (the measured
slider spec, which the user has already approved, is preserved to the pixel), and the
four tab bars and six floating windows are *deleted*, not ported. Two things are more
expensive than they look: System Monitor (six tabs into a list+inspector is a genuine
information-architecture rewrite, not a re-layout) and the gamepad pass (needs real
hardware and will not be right the first time).
