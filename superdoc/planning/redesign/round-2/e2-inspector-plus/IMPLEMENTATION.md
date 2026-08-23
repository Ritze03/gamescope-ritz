# E2 — implementation log

What has actually been built in C++, phase by phase, and what a later phase can
rely on. `SPEC.md` and `API.md` remain the contract; this file records where the
code *departed* from them and why, so the next agent does not have to
re-derive it.

Phase plan: `../../AUTONOMOUS-DECISIONS.md` D10. Judgement calls taken during
P1: the same file, D11.

---

## 2026-08-23 — closing the three gaps the pre-P5 test pass left open

Also not a phase: the three items `SHELL-TEST-REPORT.md` found and recommended
be closed **before** P5 deletes the legacy UI. Decisions:
`../../AUTONOMOUS-DECISIONS.md` **D20**. Build clean; **68/68** meson tests plus
**six** new cases — `overlay_ui` 56 cases / 977 assertions, `overlay_shell`
25 cases / 290 assertions.

### 1. The rail icon set — SPEC §8.0, and the user's own critique point

The rail drew the area title's **first character**. At ladder step ≥ 1 the label
disappears and the mark carries the item's entire meaning, and three pairs
collided there: `Mixer`/`Monitor` (`M`), `Profiles`/`Per-game` (`P`),
`Shaders`/`Shell` (`S`).

**New files:** `src/Overlay/UI/Icons.h` and `Icons.cpp`. Eleven glyphs as a
`constexpr` table on SPEC §8.0's 24-unit grid, transcribed from `index.html`'s
own `ICONS` object. `glyph::RailIcon()` in `Controls.cpp` turns a shape into
`ImDrawList` calls and **contains no coordinate of its own** — so a glyph can be
re-proportioned without touching a draw call and vice versa.

| Decision | Why |
|---|---|
| **Drawn, not baked** | A bar chart and a droplet have no code point a wider font range could reach; SPEC §8.0 forbids an icon font outright; the atlas is rebuilt per effective scale, so every baked range is paid again at every scale change. Same argument D18 recorded for the chevron and the lock, only stronger. |
| **Geometry is imgui-free** | Same reason `Lane.h` and `Layout.h` are: a test should not need a graphics context to ask whether all eleven exist, stay in their box, and differ. |
| **Keyed by area id in one file, not an `Area::Icon()` declaration** | The declaration is tidier but would edit six panel files that are P5's deletion targets, for no behavioural gain. A missing icon falls back to the initial, so a forgotten glyph degrades to the old behaviour for one item rather than blanking the rail. |
| **Fill only where it means something** | SPEC §8.0's own rule. Three fills in the set: HDR's half-disc, Monitor's bars, Per-game's corner fold. Mixer's fader caps are deliberately **outlined** — that one difference is what keeps it clear of Monitor's solid bars at 12 px. |
| **`IconOp::Teardrop` is constructed, not transcribed** | Appearance's droplet is the one shape needing a real curve. Its tangent feet are computed (`α = acos(r/d)`), so the straight and curved parts meet exactly at every size; a hand-placed pair leaves a kink at 48 px and a gap at 12. |

`setup.shell` is the one glyph with **no mockup original** — `index.html`'s
eleventh area is `display.output`, which this build does not register. Drawn as
the shell itself: a framed window with a rail down its left edge.

**Tests (2, `overlay_ui`):** every registered area has a glyph; no two glyphs are
the same drawing (the failure message names the colliding pair); every
coordinate stays inside the 24-unit box, circles and teardrops checked at their
extents. Both mutation-checked.

### 2. The multi-column sheet — the seventh "renders but does nothing"

`nColumns` was computed by `Solve()`, printed by `shell.layout`, and read by
nothing. `DrawSheetBody()` now lays out that many columns.

- **`LayOutSheetColumns()` (`Layout.cpp`) is the only place a column's geometry
  is decided**, using index.html's own formula
  `colW = (sheet − 2·pad − (cols−1)·gutter) / cols`, pad and gutter both 24.
- **The unit of packing is a GROUP, never a row** — the mockup's own greedy
  balance by line weight. A group split across a column boundary either orphans
  rows under no heading or forces the band to repeat, making one declared group
  look like two. Keeping a group whole costs some balance and keeps the row
  grammar intact.
- **Each column gets its own lane**, so SPEC §2.2's two hard vertical lines exist
  once *per column* and right-binding holds within each.
- **D17's drawer occlusion became a per-column question**, computed from each
  column's own right edge against the drawer's left edge. It reduces to exactly
  the old single subtraction at one column — pinned by a test — so the two are
  one rule rather than two that can drift.
- **`Solve()` gained `bUnsplittable`** for escaped legacy panels (they lay
  themselves out with ImGui's own cursor) and content bodies (one scrolling list
  cannot be halved). Decided in `Solve()` because `shell.layout` prints
  `Solve()`'s number — answering it at the drawing site would recreate the exact
  defect being removed.

**Verified live:** 3 columns at 0.5×, 2 at 0.75× and 1.0×, 1 at 1.25×, matching
SPEC §8.3's table; 1 column at 2.0× with the drawer, lane clear of it;
`system.log` reports `1 col` where `system.monitor` reports `3 col` at the same
0.5×. **Tests (4, `overlay_shell`).**

### 3. The Reachability Law's mechanism — built, not spec'd away

SPEC §6.3 says a row owning Params renders them inline in the Sheet whenever the
Inspector is unavailable. It was never built, **and the comment above
`DrawExplainPage()` claimed it was.**

Built rather than removed from the spec: §6.3's three-clause argument, §8.2's key
table (twice), §6.4's "honest cost" and §2.4's amendment all depend on it, and
clause 1 asks for one property — "one code path" — which `DrawInlineParams()`
satisfies literally by allocating a `RowCtx` from the same lane and calling the
same `DrawSharedControl()` the sheet row and the Inspector call.

| Piece | Behaviour |
|---|---|
| Host | `InlineMode()` — the Hidden host only; with a column or drawer the params are already on screen, and drawing them twice would be two painters for one declaration. |
| Disclosure | The §2.4 chevron points **right** collapsed, **down** open. Its own hit box exists only in inline mode, where it is a real control. |
| Expand | `→` (after the row's own adjuster declines), `Space` (last in the activate chain, so a switch still toggles), or a click on the chevron. |
| Navigate | `↓`/`↑` walk **into** the expansion before moving to the next row; `←/→` adjust the focused param through the same `AdjustValue`/`RunBankKeyboard` the Inspector uses. |
| Collapse | `←` on the row, `Esc` (the ladder rung that had nothing behind it), or selecting another row. |
| Scope | **One expansion at a time** — §6.4 concedes the reflow §8.3 forbids only because it is user-initiated, and one open row bounds it. |

**A spec ambiguity resolved:** §8.2 gives `←/→` three jobs without saying which
wins on a row that is both adjustable and deep. **Adjusting wins**, reusing the
"didn't move" signal the region-edge rule already runs on — otherwise giving a
slider a parameter would silently disable that slider's arrow keys.

**Verified live** on `image.shaders.adaptive_brightness` (the Six Budget's live
maximum, 6 params): expanded from the sheet, focus walked into the params,
`Target brightness` adjusted 0.5 → 0.508 with the Inspector hidden, collapsed by
`Esc` with the row still selected.

The comment now says what is true **and records that it used to lie**, so the
correction is auditable.

---

## 2026-08-23 — the pre-P5 shell test, and the eight defects it fixed

Not a phase: an exhaustive test pass over everything P1–P4.1 built, before P5
deletes the legacy UI. Full evidence, with counts and what could **not** be
tested: `SHELL-TEST-REPORT.md`. Decisions taken without the user:
`../../AUTONOMOUS-DECISIONS.md` **D19**. Build clean; **68/68** meson tests plus
one new `overlay_atoms` case (that suite: 8 → 9 cases, 79 assertions).

Method: 23 nested sessions driven by `overlay_e2_key` and the other console
surfaces, 85 `grim` captures. No pointer input (D4).

**Fixed (commit `885242e`):**

| # | Defect | Where |
|---|---|---|
| 1 | `overlay_e2_palette query shell.` **aborted the compositor** — `FormatLadder()` called `ImGui::GetIO()` from the console thread. The surface size is now published into two atomics by `Draw()`. | `Shell.cpp` |
| 2 | An armed `Delete saved config` **survived `Esc` and survived moving the selection**, so one press afterwards deleted the file. SPEC §3.9 and `Select()`'s own comment both already claimed otherwise. | `Shell.cpp` |
| 3 | `controls::Bank` **laid its full measured run out from the lane's left edge**, so an over-wide bank ran 104 px past the sheet at 2.0× with the drawer open. Scaled into the lane; pinned by a mutation-checked test. | `Controls.cpp` |
| 4 | A Bank was **the only control with no keyboard route at all**. `←/→` now move a chip cursor and `Space`/`Enter` toggles it, in both hosts, with SPEC §7.3's focus ring. | `Shell.cpp`, `Controls.cpp` |
| 5 | **`RowCtx::Affordance()` finally has call sites.** SPEC §2.4's chevron and lock were specified in P1 and drawn by nobody through P4 (D18's own still-open list). | `Shell.cpp`, `Controls.cpp` |
| 6 | The sheet header was missing **`differs N`** and **`inspector hidden`**, the only two chips §1.1's D9 amendment permits it besides the breadcrumb. | `Shell.cpp` |
| 7 | A composite's value was **the raw axis-A int** in the palette and in Details' binding grid, contradicting the band two lines above. | `Shell.cpp` |
| 8 | `overlay_e2_select` was **a second selection path** — it skipped `Select()`'s focus reset, explain-page close and disarm, so console-driven keyboard tests exercised a state the product never reaches. | `Shell.cpp` |

**The seventh "renders but does nothing", found and NOT fixed:** `Solve()`
computes `nColumns` (3 at 0.5×, 2 at 0.75×/1.0×/1.75× for a 25-row area),
`shell.layout` prints it, and `DrawSheetBody()` draws **one column at every
scale**. `grep -n nColumns src/Overlay/UI/` returns the assignment, the field and
the `printf`, and nothing else. Left for P5 with the evidence attached — D19.9
says why guessing at it here would have repeated D16.2's mistake.

**Also still open, and named rather than glossed:** the rail draws letters
instead of SPEC §8.0's eleven icons (and at ladder step ≥ 1 three pairs of areas
become indistinguishable); `Kind::Meter` has zero registrations; SPEC §6.3's
inline param expansion does not exist (the comment above `DrawExplainPage()`
claiming "params render inline (P3)" is **wrong** — the Reachability *guarantee*
holds through `Ctrl+/` and `Ctrl+K`, the *mechanism* is unbuilt); the Overview
card and Details' binding grid are both partial against §5.5/§5.1; and
`widgets::Checkbox` is still declared with no callers, which §3.1 says should be
deleted. All in `SHELL-TEST-REPORT.md` §6–§8.

**Verified good:** config safety (a seeded pre-existing `global.json`,
`games/*.json` and `profiles/*.json` all keep their sha256 **and** their mtime
across a full navigation session, unknown keys included); legacy mode (five
`overlay_e2` flips leave the dock pixel-identical and the log assert-free); and
the HUD (legacy vs E2 differ by **less** than two legacy frames differ from each
other).

---

## 2026-08-23 — P4.1, the three deferred defects

D17's 2.0× lane fix, the glyph decision, and P4's five keyboard gaps. Build clean;
**68/68** meson tests — nine new cases: `overlay_ui` 47 → **54** (468 assertions),
`overlay_shell` 19 → **21** (255 assertions).

Decisions taken without the user: `../../AUTONOMOUS-DECISIONS.md` **D18**.

### 1 — the drawer no longer covers the sheet's controls

One clamp, at the **top** of `Lane::ForColumn()`, before every derivation:

```
ForColumn( flWidthBase, flOccludedRightBase = 0 )
    flWidth = flWidthBase - flOccluded - kM      (when occluded)
    ... Lw, ctl zone, affordance all follow from the REDUCED width
```

`Shell::DrawSheetBody()` passes `rcSheetBody.x1 - rcInspector.x0` (less one sheet pad)
when the ladder's host is `Drawer`, and takes `rcCol`'s right edge from `lane.flWidth`
so bands and content bodies retreat with the rows. **The regions are untouched** — the
drawer still floats, still costs no relayout.

*Why the whole width and not just the control zone:* holding `Lw` at its full-width
value leaves a control zone of **zero** at 2.0× (Lw 348 vs 368 visible). D18.1.

| At 2.0× | drawer closed | drawer open |
|---|---|---|
| column width, base | 756 | 368 |
| `Lw` | 348 | 200 |
| control zone | 368 | **128** |
| control right edge, px | 1624 | **848** (drawer starts at 928) |

Tests: three in `overlay_ui` pinning the arithmetic open and closed, two in
`overlay_shell` that **derive** the occlusion from the real `Slab`/`Solve`/`Regions`
— so the ladder's numbers moving underneath the fix fails too, which is the shape
the original defect had.

### 2 — the shell draws its icons

`Fonts.cpp` bakes U+0020..U+00FF. Extending that range **cannot** fix `▸`/`⌕`: no
bundled Geist face contains either codepoint (D18.2). Three sites are now stroked
paths in `ui::glyph` (`Chevron`, `Magnifier`, authored on a unit square):

| Was | Now | Why it was wrong |
|---|---|---|
| `"inspector ›"` (spine) | drawn chevron, pointing **down** | U+203A is outside the baked range — the one real box glyph. Down is what the mockup *shows*: its `›` is rotated by `writing-mode: vertical-rl`. |
| `">"` (palette prompt) | drawn magnifier | P4's substitute for U+2315, which the font lacks anyway |
| `"v"` (dropdown caret) | drawn chevron | a letter pretending to be a triangle, sized by a text measurement |

**Sweep result:** `›` was the only non-Latin-1 character the shell could emit. `▸`,
`⌕` and the arrows are *not in the C++ at all* — they are `index.html`'s, and
`RowCtx::Affordance()` (the depth affordance `▸` belongs to) **has no call sites**.
`…`/`→` appear only in comments. Kept swept by `fonts::FirstUnbakedCodepoint()` +
`overlay_e2_glyphs`, which walks every registered title, help sentence, option label
and unit. Live answer: clean.

### 3 — the keyboard, and the ability to press it

`overlay_e2_key "<chord>..."` sends **real key events** into the overlay's own
`s_InputQueue` — the queue `wlserver_dispatch_key()` writes to — so they take the
identical path a physical press takes and cannot leave the overlay. This is not D4's
banned synthetic input; see D18.7 for the full argument.

| Gap | How it closed |
|---|---|
| Inspector rows | one index: −1 strip, 0 the entry's row, 1..n params; focus is drawn |
| CONFIGURE/DETAILS strip | index −1; Left/Right pick a cell, by direction (D16.6) |
| Dropdown popups | Enter opens, Up/Down highlight, Enter commits, Esc dismisses — **only** for a Choice that really drew as one (D18.6) |
| `Ctrl+/` and `?` | SPEC §6.3's full-sheet Configure+Details page, back crumb, Esc returns |
| Text rows | Enter begins entry (`controls::Text` only ever entered from a click) |

**Four defects only a keypress could find** — all four are the "renders but does
nothing" class, all four had survived multiple phases:

1. **`KEY_SLASH` was missing from `ImGuiKeyForKeycode`**, so `Ctrl+/` produced no
   `ImGuiKey` — unreachable from a real keyboard too. Punctuation still *typed* via
   `AddInputCharactersUTF8()`, which is exactly what hid it.
2. **The dropdown list never rendered.** ImGui closes a popup whose parent is not
   focused; the slab carries `NoBringToFrontOnFocus` by design. Now drawn by the
   shell in a sibling window — the palette's own documented pattern.
3. **Left/Right was a dead key on the sharpness slider**: 21 real notches behind a
   declared `0..100`, so a step of 1 round-tripped to the same value. `.Step(5)`.
4. **ImGui's keyboard nav was a second focus model** over the same keys. E2
   implements §8.2 in full, so it disables nav for its frames.

### Still open after this

- `RowCtx::Affordance()` has **no call sites** — SPEC §2.4's depth affordance is
  specified and unbuilt. That is why `▸` was never needed.
- The palette's footer legend overlaps its last row at 2.0× (pre-existing, cosmetic).
- Only sharpness was proven to have the quantised-binding problem; other converting
  sliders were not audited.
- `Escape()` untouched — P5's.

---

## 2026-08-23 — P4, the command palette and the keyboard

`Ctrl+K` over every Entry **and every Param**, the shared arrow-key adjuster behind
it, and the rest of SPEC §8.2's key table. Build clean; **68/68** meson tests — the
67 P3c left, plus a new `overlay_palette` suite (19 cases, 68 assertions).

Decisions taken without the user: `../../AUTONOMOUS-DECISIONS.md` **D16**.

### The two briefed defects were already fixed

P4 was briefed to fix a `display_scale 2.0` console abort and a companion
"the shell does not re-scale". **Both were already closed by P3c's `5582437`**, and
both were re-verified by triggering them rather than by reading the diff — see
D16.1. The route is `overlay_e2_set` → the registered setter →
`fonts::RequestRebuild()` (an atomic, pumped by the render thread at the top of its
next frame) and `QueueGeneralSave()` → `PushLiveTheme()` →
`palette::g_LiveTheme.flDisplayScale`, which is the single value `Shell::Draw()`
pushes into `ui::SetScale()` each frame. Same commit, same fix, both symptoms.

**A different 2.0× defect is now on the record and is NOT fixed *in this phase*:** at
step 2 the Inspector becomes a drawer and paints over the sheet's entire control
column. It matches SPEC §8.3's table, so it is a gap in the design rather than a slip
in the code — D16.2 says why guessing at it here would have been worse than reporting
it. *(Decided by **D17**, fixed in **P4.1** above — the sheet's lane gives way while
the drawer is open.)*

### What the palette is made of

| Piece | Where | Notes |
|---|---|---|
| `Score()` | `CommandPalette.cpp` | Five bands, `index.html`'s `score()` as the tiebreaker. Pure: three strings in, an int out. |
| `Build()` | `CommandPalette.cpp` | Walks the registry, emits one item per Entry and one per Param, **stable**-sorts by score. |
| `WorstCharsToReach()` | `CommandPalette.cpp` | Direction B's discoverability gate as a function. Live registry today: **2**. |
| `AppendUtf8` / `PopUtf8` / `PopWord` | `CommandPalette.cpp` | The hand-rolled query field's accumulator (D16.3). Backspace deletes a **character**, not a byte. |
| `Adjustable` / `AdjustValue()` | `Registry.cpp` | The **one** stepper, shared by the palette and the Sheet (D16.6). |
| `DrawPalette()` | `Shell.cpp` | The panel. Query line, count chip, ≤60 results in a 9-row window, legend. |
| `RunPaletteKeyboard()` | `Shell.cpp` | `↑↓` move, `←→` adjust in place, `Enter` jump & select, `Esc` dismiss. |
| `PaletteJump()` | `Shell.cpp` | Selects the parent row, forces **Configure** for a Param, and promotes a hidden Inspector to a drawer so the landing is visible. |
| `overlay_e2_palette` | `Shell.cpp` | The console surface. Same state the keys drive; `list` prints the ranking (D16.8). |

**No call site was touched.** All 102 live results come from what P3a–P3c already
registered. The only registry change is two getters — `Entry::KeywordText()` and
`Parameter::KeywordText()` — because `.Keywords()` had shipped since P1 as a setter
with **no reader at all** (D16.5).

### Three defects found by running it, and what each taught

1. **The palette drew UNDERNEATH the sheet.** `DrawSheetBody` and the Inspector
   body are ImGui **child** windows, and a child's draw list is emitted after its
   parent's regardless of fill order — so "draw it last inside the slab" put it
   behind the very rows it covers. Fixed by giving the palette its own top-level
   window opened after the slab's `End()`.
2. **...and then still drew underneath**, because that new window inherited
   `ImGuiWindowFlags_NoBringToFrontOnFocus` from the slab, which pins a window to
   the back and made `SetNextWindowFocus()` a no-op. The palette is now the one
   window in the shell that does not carry that flag.
3. **Three glyphs rendered as fallback boxes.** `Fonts.cpp` bakes
   `GetGlyphRangesDefault()` — Basic Latin + Latin-1 only, because this UI is
   English-only. The mockup's `▸` (U+25B8), `⌕` (U+2315) and the legend's arrows
   are all outside it. The separator is now `»` (U+00BB, in range), the prompt is
   `>`, and the legend spells its keys out. **Rule for anyone porting more of
   `index.html`: the mockup is authored in a browser with the whole of Unicode
   available and the overlay is not.**

### What is verified, and what is not

- **Ranking and adjustment** — `tests/test_overlay_palette.cpp`, no window needed,
  plus `overlay_e2_palette list` against the live registry.
- **Drawing** — screenshots at browse, mid-query and post-adjust.
- **Key bindings** — **not** exercised by real keypresses. `ydotool` is banned and
  no other injection route respects that ban, so the bindings are unit-tested and
  console-equivalent-tested only. Stated rather than glossed; see D16's
  "still open". *(Superseded in **P4.1**: `overlay_e2_key` sends real key events
  into the overlay's own input queue without touching the seat — D18.7. Every
  binding here has since been exercised by an actual keypress, which promptly found
  four defects the console equivalents could not.)*

### Keyboard: what P4 added, and what is still unreachable

`RunKeyboard()` handled three keys before this (`Ctrl+I`, `Tab`, `Esc`). It now
covers `Ctrl+K`, `Ctrl+D` (reset row **and** its params), `Ctrl+←/→` (rail item
without leaving the sheet), `↑↓` (row, and rail items when the rail has focus),
`←→` (adjust the focused control, through `AdjustValue()`), `Space`/`Enter`
(toggle a switch, fire an Action — **still arming** a destructive one, so the
keyboard is not a route around `Confirm()`), and `Esc` (drawer → selection).

**Still not reachable by keyboard**, and each is a real gap:

- **The Inspector's own rows.** `Tab` moves focus to `Region::Inspector`, but
  nothing consumes arrows there — a Param can be *jumped to* by the palette and
  *adjusted* from it, but not walked with `↑↓` once the Inspector has focus.
- **The mode strip.** `CONFIGURE` / `DETAILS` cannot be switched from the keyboard;
  the mode is whatever `ModeFor()` picked, or what `PaletteJump()` forced.
- **A dropdown popup.** A downgraded `Choice` opens on click only; `←→` steps the
  underlying value without opening it, which works but means the popup itself is
  pointer-only.
- **`Ctrl+/` and `?`** — SPEC §8.2's "Configure + Details as one full-sheet page"
  is unimplemented, along with the inline expansion (§6.3) it belongs to.
- **Text entry on a `Kind::Text` row.** Editing still begins with a click.

---

## 2026-08-23 — P3 part C, the composite band, Monitor and Log

The last of the area ports, plus the two pieces of debt the earlier parts left.
**`EscapeCount()` reaches 0** — no area is escaped, and `Escape()` itself now has
no call site anywhere in `src/`. Build clean; **67/67** meson tests
(`overlay_ui` 45 → 47, `overlay_atoms` 6 → 8).

Decisions taken without the user: `../../AUTONOMOUS-DECISIONS.md` **D15**.

### `Kind::Composite` — built, and what it is made of

D14.10 recorded that the shell rendered no composite at all: the kind was in the
taxonomy, `Band.cpp` had its geometry from P1, `controls::AnchorGrid()` existed,
and `DrawEntryRow` dropped the whole kind through `default: break`. That is why
P3b downgraded the anchor grid to a nine-option Choice and dropped the accent
hue's gradient — a control that registers correctly and draws nothing is exactly
issues #25 and #68.

| Piece | Where | Notes |
|---|---|---|
| `DrawCompositeBand()` | `Shell.cpp` | Deliberately shaped like `DrawEntryRow` (D15.1). Same fill, hairline, label/value split, disabled handling. |
| `LinesFor()` | `Shell.cpp` | The one answer to "how tall is this declaration". Both y-cursors (sheet, Configure) go through it, so they cannot disagree. |
| `CompositeValue()` | `Shell.cpp` | Clause 2's resolved value. The anchor's margins are read from the **Params**, so there is no second copy to drift. |
| `controls::Rail()` | `Controls.cpp` | A gradient track whose fill is **sampled** from a colour function, never interpolated between endpoints — issue #37's rule, so the strip cannot disagree with the accent it picks. |
| `controls::HueBody()` | `Controls.cpp` | Rail + eight 45° preset swatches. Swatches set the *same* value the rail does; they are shortcuts, not a second setting. |
| `controls::ColorBody()` | `Controls.cpp` | Swatch + L/C/H rails, sharing one `Rail()`. |
| `controls::GraphBody()` | `Controls.cpp` | Read-only sparkline, with the **two** graph conventions made explicit (see below). |
| `palette::ImU32ToOklch()` | `Palette.cpp` | The inverse of `OklchToImU32()`, so the Colour composite edits OKLCH while its binding stays a packed sRGB int. Round-trip test pins the two together. |

Registry changes the band required:

- **`BindingB()` / `DefaultValueB()`**, and `HasDefault`/`IsAtDefault`/
  `ResetToDefault` now read **both** axes. An anchor is one setting whose value
  is a pair; reading one binding made the sheet's D6 edge lie (D15.3).
- **`Entry::Samples()`** — the Graph's value, in exactly `Meter`'s shape
  (`std::function` returning a read, no setter anywhere), because a graph is
  read-only by construction, not by convention.

**Does the anchor grid earn its place now?** Yes, and the mockup's own framing is
why: it is a three-line band that reads as an ordinary row until the eye reaches
the control column, its value line says `bottom-right · 64 / 32` so the margins
never need two steppers on the sheet, and those margins live in Configure as
Params. The original critique's "weirdly placed, almost orphaned" is answered by
the band participating in the same four column lines as every switch above it.

**The 16:9 miniature (`e3-creative`) is a natural later addition, and the band is
why.** Everything that would change is inside one `case` of
`DrawCompositeBand`'s body switch: `Band.cpp` already right-binds a body rect and
guarantees its height, and the two-axis binding already carries "which anchor",
with the margins as Params that a drag would write. A miniature would be a new
`CompositeKind` plus one body function — no change to the band rule, the row
grammar, or any call site. Worth doing; out of scope here.

### `GraphBody` has two axis conventions, and neither is a call site's choice

`nAxisSlots == 0` rolls (newest right-aligned) — the frametime strip.
`nAxisSlots > 0` is a **fixed axis filled from the left**, with the remainder
left blank — issue #40's explicit requirement that a partially-filled 60-second
window must never read as a complete one. Right-aligning a handful of samples
across the full width is precisely what that issue forbade.

### D6's other half — the accent left edge

SPEC §1's row ink budget always gave the state-edge slot three values
(`Accent` / `Accent@45%` / nothing); only the first was ever drawn.
`StateEdgeColor()` is now the single place that decides which, for rows and for
bands. Selection outranks "differs", because the two share the slot and a
selected row has an open Inspector showing its reset link anyway. This closes
D14.8's "still open" note: the sheet can now answer *what have I changed here*
without opening every row in turn.

### Monitor — before and after

**Before** (`FpsDisplay_DrawSettingsPanel`, six tabs, issue #59). 25 controls:

| Tab | Controls |
|---|---|
| General | Show System Monitor · 3×3 placement grid · vertical margin · horizontal margin · module spacing · font size · blend mode · text opacity · backdrop · backdrop opacity · backdrop rounding · backdrop padding |
| FPS | FPS module · "FPS" label · frametime readout · frametime graph · percentile row · FPS colour |
| CPU / GPU / Media | module toggle + colour override, each |
| Statistics | 60s graphs: CPU load, GPU busy, GPU temp, GPU power, frame rate (+ warm-up readout) |

**After** (`FpsDisplay_RegisterArea`, one sheet, six groups). Every one of the 25
is present:

| Group | Rows |
|---|---|
| Monitor | `monitor.enabled` |
| Modules (`N / 7`) | `mod_fps` (+ Param `label` — #73) · `mod_frametime` · `mod_graph` · `mod_pct` · `mod_cpu` · `mod_gpu` · `mod_media` |
| Placement | `monitor.anchor` **Composite(Anchor)** + Params `margin_v`, `margin_h` |
| Appearance | `font_size` · `module_spacing` · `blend_mode` · `text_opacity` · `backdrop` (+ Params `opacity`, `rounding`, `padding`) |
| Module colours | `color_fps` · `color_cpu` · `color_gpu` · `color_media`, each **Composite(Color)** + Param `custom` |
| Diagnostics | `monitor.sampling` (Facts, 5 Live rows) · `monitor.frametime_graph` **Composite(Graph)** |
| Statistics | `stats_collect` · `stats_window` (Facts) · 5 **Composite(Graph)** rows |

Three deliberate re-homings, all in D15: #73's label became a Param so the band's
count keeps meaning *modules*; #29's colours became their own group because a
Composite cannot be a Param; #40's gating became a stated switch.

**#72 (uniform module width), #77 (media-title cap) and #80 (shrink-to-content)
are not settings and did not move.** They are HUD render behaviour in
`MeasureFpsModule()`/`DrawModule()`, above the line this port touched.

**Only the settings half moved.** The HUD keeps its own ImGui context, offscreen
texture and submission path, untouched. The legacy six-tab panel is also kept
**verbatim**, because `SettingsOverlay.cpp` still hosts it under `overlay_e2 0`
and that shell must stay byte-identical; both halves read and write the one
`s_Settings`, so neither can drift into a private copy of the config. Verified by
capture: the HUD under `overlay_e2 1` and under `overlay_e2 0` differ by 0.05% of
pixels in the HUD crop — the frame-rate digits.

### Log — before and after

**Before** (`PanelLog_DrawBody`, two tabs): Gamescope tab · Game tab (with a
"not capturing" explanation) · clipper'd line view with `[scope]` prefixes and
priority colouring · horizontal scroll · stick-to-bottom · **Copy to clipboard**
· line count.

**After** (`PanelLog_RegisterArea`, one area): everything above, plus filters that
did not exist:

| Group | Rows |
|---|---|
| Filter | `log.sources` **Bank** (gamescope, game) · `log.severity` **Bank** (error, warn, info, debug) · `log.filter` **Text** · `log.autoscroll` **Switch** |
| Diagnostics | `log.buffer` (Facts, 3 Live rows) · `log.copy` **Action, disabled** |
| body | `Area::Content()` — the captured text, clipper'd, scope-prefixed, severity-coloured, stick-to-bottom |

The two tabs became the two members of `log.sources` (D7's rule: one setting
whose value is a set). Filter state is **session-only and no config key was
added** — a filter that survived a restart would hide lines for a reason nobody
remembers setting.

### `Area::Content()`

The one genuinely new registry capability, and explicitly not `Escape()` renamed
— see D15.7. It hands the shell **data** (a function returning `ContentLine`s),
never a draw callback, so SPEC §5.2 clause 0 holds: a category file still cannot
place a pixel. A content area still declares ordinary rows, drawn above the body,
so "rows or content" is never the choice.

### Two defects found by running it

- **`Kind::Bank` and `Kind::Text` drew nothing.** Unhandled enumerators in
  `DrawEntryRow`'s switch. Both atoms had existed since P1 with tests; the Log is
  simply the first area to use either, so the taxonomy claimed eleven kinds while
  the shell rendered nine. Wired into `DrawSharedControl` so a promoted Param
  gets them on the same path (SPEC §5.3).
- **`overlay_e2_set overlay.display_scale 2.0` aborted the compositor.**
  `fonts::RebuildAll()` mutates font state and assumes the render thread inside a
  live frame; a registration setter is reachable from the **console thread**, so
  it cleared an atlas mid-draw and the next glyph needing a bake walked a freed
  `Sources` vector. **A/B'd against the P3b merge, which survives it** — the
  faulty call is P3b's; this work made it deterministic. Fixed with
  `fonts::RequestRebuild()` / `PumpRequestedRebuild()`, performed on the render
  thread beside the `ApplyPendingRebuild()` that already exists for the same
  class of problem. Right-aligned text also now falls back to left alignment once
  it no longer fits, because clipping the *head* of a value rendered
  `bottom-right · 64 / 32` as `.ght`.

### Known-open

`overlay_e2_set overlay.display_scale` changes the stored value and re-bakes the
atlas but does **not** re-scale the shell — the slab stays 1560 px where
`Slab::For()` says 2.0× gives 1728. P3b's row, out of this part's scope. The
band's 2.0× geometry is pinned by test instead (`band: the four clauses hold at
every display scale`).

### Can `Escape()` be deleted?

**Yes, now** — `EscapeCount()` is 0 and there is no `.Escape(` call site left in
`src/`. It was left in place deliberately: removing it means deleting
`Escape()`, `EscapeBody()`, `IsEscaped()`, `EscapeCount()`, `Law::Escaped`, the
shell's escape branch in `DrawSheetBody()` and three tests, which is an API
removal rather than an area port. **P5 should do it as one atomic change**; it is
trivially safe at any point from here, since nothing calls it.

---

## 2026-08-23 — P3 part B, Audio and Config, plus the Inspector's scroll

**Status: `audio.mixer` and the Config panel are real registrations.** `EscapeCount()`
**4 → 2** — only Monitor and Log still host a legacy body. The rail is now the **eleven**
items SPEC §8.1 names. No config key, value range, format or setter changed; the legacy
panels still draw unchanged under `overlay_e2 0`.

Judgement calls: `../../AUTONOMOUS-DECISIONS.md` **D14**.

### The Inspector could not scroll — fixed first, deliberately

P3a found it and left it. At 2.0× an entry on the six-parameter budget (adaptive
brightness, D13.4) overflowed the drawer and the last row clipped. Nothing failed; the
pixels just stopped.

The cause was **not** the row grammar. Each body laid out with an absolute `y` seeded from
the *region's* `y0` — a fixed screen coordinate — and nothing ever told ImGui how tall the
result was. So no scroll range existed, and a fixed origin would not have moved if one had.

Both halves are fixed inside the body child, **without a second layout path**:

- the origin is now the **child's own cursor**, from which ImGui has already subtracted the
  scroll offset. The existing absolute arithmetic therefore pans correctly — every row,
  control and hairline derives from it.
- each body **returns its bottom edge**, which becomes a `Dummy`, which is the content size
  ImGui measures a scroll range from.

*Rejected:* rewriting the bodies onto ImGui's cursor with `Dummy`/`SameLine` spacing. That
would leave one layout model in the sheet and another in the Inspector — precisely what
SPEC §5.3 forbids, since a promoted parameter has to land in the sheet unchanged.

`ui::ConfigureRowsHeight()` (`Layout.h`) makes the overflow a **number**, so "this body is
taller than its region" is a comparison a unit test makes with no window open. It is a
deliberate *lower bound* — the help paragraph needs font metrics — which is the safe
direction. Two shared constants (`shelltok::kSectionLine`, `kTitleLine`) replace the bare
`20`s and `24`s so the arithmetic and the drawing cannot drift.

**The margin was 0.8 px.** At 2.0× the six param rows alone came to 736.0 against a 736.8
body — which is exactly why only the *last* row clipped rather than the whole block.

### Dynamic areas: the registry's answer to a row set that changes

Audio is the first area whose rows are **not known when `RegisterAll()` runs**. A row
exists because an application is making a sound right now, and streams appear and disappear
while the overlay is open.

`ui::Area::Rebuilds( generation, builder )` declares such an area. The registry rebuilds it
when the generation moves, **releasing the ids the previous build claimed first**. This is
P1's deferred `Area::Repeat()`, landed — P1 noted it "needs the shell's frame loop to have a
topology-change hook"; `Registry::SyncDynamicAreas()`, called once at the top of `Draw()`,
is that hook.

**Rejected: a fixed pool of N positional slots**, each bound to "whichever stream is at
index *i*" and greyed when there are fewer. It needs no registry change at all — and it is
wrong. Slot identity would be *positional*, so a stream ending shifts every stream below it
up one slot and the slider under the pointer silently starts controlling a different
application. That is not cosmetic; it is the volume of the wrong program moving. Identity
has to come from the stream, so the row id does too: `audio.node.<pipewire-node-id>`.

How the four laws survive:

| Law | How |
|---|---|
| **ID uniqueness** | a rebuild releases before it builds; node ids are unique among live nodes |
| **The Prefix Law** | untouched — a rebuilt Entry mints Params through the same synthesis |
| **The Six Budget** | untouched — checked per Entry as it is built, exactly as at startup |
| **Help is required** | **the one that changes.** `SelfTest()` runs once after `RegisterAll()`, so a row built later never sees it. A rebuild now re-runs the help and prefix checks over its own area. |

**The consequence, stated rather than hidden:** a violation aborts (D11.6) and a rebuild
happens mid-session, so a malformed dynamic row is a **mid-session abort**, not a boot
failure. That is a real widening of when the guillotine can fall. It is acceptable only
because a dynamic row is *generated* — no human types one — so one unit test against a
fabricated stream list covers every row the generator will ever emit. Five do, including
the **empty** stream set, which is the common case at startup and the reason the help hole
needed closing at all: with no streams, the row-building code never runs.

`Law::Dynamic` keeps an area escaped **or** dynamic, never both — the same argument as
`Law::Escaped`.

### The setting inventory, before and after

Nothing was dropped. Prose status lines became `.Live()` facts (D13.5's precedent).

| Legacy panel / tab | Setting | Now | Config key (unchanged) |
|---|---|---|---|
| Audio | Game volume + Mute | `audio.volume` + **1 param** (only when detected) | — (live PipeWire state) |
| Audio | per-stream slider + mute (#36) | `audio.node.<id>` + **1 param**, one per live stream | — |
| Audio | manual picker combo + 2 buttons | `audio.stream`, one Choice; "Automatic" *is* the old clear | `audio.manual_node_binary` |
| Audio | *wpctl-missing / detection / multi-candidate / stale-override prose* | → `audio.server` `.Live()` ×6 | — |
| Audio | *greyed unpinnable combo rows + tooltip* | → `audio.server` `.Live()` "not pinnable" | — |
| Config ▸ Per-Game | Override Global Config | `setup.pergame` ▸ `config.override` | session routing |
| Config ▸ Per-Game | Copy another game's config | `config.copy` + `source` param | — |
| Config ▸ Per-Game | Delete Saved Config + modal | `config.delete`, armed by `Confirm()` | — |
| Config ▸ Per-Game | *app-id / "Editing:" / layer prose* | → **`Area::Badge`** + `config.routing` `.Live()` ×6 | — |
| Config ▸ Per-Game | profile picker + Apply | `setup.profiles` ▸ `profiles.list`, `profiles.apply` | — |
| Config ▸ Per-Game | new name field + Save | `profiles.name` (validated) + `profiles.save` | — |
| Config ▸ Per-Game | *last applied profile* (#43 #10) | → `profiles.facts` `.Live()` | `last_applied_profile` |
| Config ▸ General | Accent hue | `setup.appearance` ▸ `overlay.accent_hue` | `overlay.accent_hue` |
| Config ▸ General | Dock / Display / Notification scale | `overlay.display_scale` + **2 params** | `overlay.*_scale` |
| Config ▸ General | 4 opacity sliders | 4 rows under ▸ Transparency | `overlay.opacity_*` |
| Config ▸ General | Blur / Darkening | 2 rows under ▸ Backdrop | `overlay.background_*` |
| Config ▸ General | *config directory readout* | → `overlay.appearance_facts` `.Live()` | — |
| Config ▸ General | 4 per-group **reset links** | → **`Entry::ResetToDefault()`** in the Inspector | — |
| Config ▸ Notifications | 3×3 placement grid | `overlay.notification_placement`, one 9-option Choice | `overlay.notification_placement` |
| Config ▸ Notifications | Mute toggle | `notifications.muted` | `notifications.muted` |
| Config ▸ Notifications | Send test notification | `notifications.test` | — |

**Two affordances changed shape** (both in D14, with reasons): the accent **hue gradient
strip and swatch** are not carried over — the hue *setting* is, as a slider — and the
placement **3×3 grid** is a nine-option Choice, because `Kind::Composite` is in the taxonomy
but the shell does not render one yet, so an Anchor would register correctly and draw
nothing (issues #25, #68).

### Reset finally exists

D6 decided reset moves into the Inspector; **no phase had implemented it**, so E2 could not
reset anything at all, and migrating Config would have silently dropped its four per-group
links. It is per-row **and covers the row's parameters**, which is what makes it the
successor to a *group* link: the old "UI Scale" group *is* the `UI scale` row plus its dock
and notification params, so one reset restores exactly what the old link did. A row with no
declared `Default` shows no affordance rather than resetting to zero. Float comparison uses
a tolerance, or a config that had merely been saved and reloaded would light the link
forever.

D6's other half — the accent left edge marking "differs from default" **on the sheet** — is
still unimplemented, and is now the only way that decision is incomplete.

### Deletion is armed, never automatic

The user, after an agent wiped one of their configs: *"There can be a button for it, but
never delete configs automatically."* `Entry::Confirm( prompt )` makes the two-press flow a
property of the **declaration**: the first press swaps the verb and reddens the chip, only
the second performs it, and it disarms on a timeout so a walked-away-from overlay is never
one click from destroying a file.

It is in the registry rather than at the call site because a confirmation a call site has to
remember to build is one the next call site forgets — and a category file cannot open a
modal anyway (SPEC §5.2 clause 0). `config.delete` is the only action in the product that
destroys anything, and it exists only when there is a saved config to destroy.

### A crash, found by `overlay_e2_set`

A registration's setter is reachable from the **console thread**, which has no ImGui
context. `PushLiveTheme()` wrote `ImGui::GetIO().FontGlobalScale` unguarded, so
`overlay_e2_set overlay.display_scale 1.0` killed the compositor. That line is purely the
live **drag preview**, and a console write is not a drag, so it is now guarded; the value
still reaches the UI through `g_LiveTheme`, and `fonts::RebuildAll()` already tolerates a
null current context by design.

This is the second time D13.8's command has paid for itself, and it generalises: **any**
binding that touches ImGui state is now reachable off the draw thread.

### Verified

- `nice -n 19 ninja -C build` clean; **67/67** (`overlay_ui` 34 → 45, `overlay_shell` 16 →
  19, `config` 38 → 40).
- Live session under `scripts/with-gamescope-lock.sh`, `DISABLE_LSFG=1`, no `--backend sdl`,
  no pointer injection — driven entirely by `overlay_e2_*` ConVars and `grim -g` bounded to
  the tracked PID's own window.
- **Audio names, against real streams from two applications.** A `pw-play` stream with
  `application.name` deliberately **empty** and a `media.name` set (issue #63's bluetooth
  case) resolved to *"iPhone von Moritz (codec AAC)"* — the `media.name` tier rescuing it —
  rather than its raw `bluez_input.AA_BB_…` node name. mpv, TeamSpeak, Spotify and
  speech-dispatcher all named correctly alongside it. Audio was **silent** on purpose: the
  point is the metadata, not the sound.
- **A stream ending does not disturb the others.** With five rows on screen, killing mpv
  removed *its* row and left every other row's identity **and value** untouched (TeamSpeak
  97 %, Spotify 52 %), and the counts fell 5 → 4 streams, 7 → 6 rows. That is the property
  the positional-slot design would have broken.
- **All three badge values** seen on screen: `global` (no app id), `app 1174180` (override
  on) and `global only` (Appearance).
- **Inspector scrolling**, at 2.0× on adaptive brightness: the scrollbar appears and **Max
  gain**, the sixth parameter, is reachable. The whole body pans as one; the lane and row
  grammar are unchanged.
- **An existing config loads untouched, verified on disk not on screen.** A pre-E2
  `global.json` with eleven non-default values loads with every one intact **and with its
  mtime, size and bytes unchanged** — reading a config must not write one. A per-game file
  survives the override being turned off and is *restored* when it is turned back on; only
  the explicit delete removes it. Both are permanent tests in `tests/test_config.cpp`.
- **Legacy UI unchanged under `overlay_e2 0`.** Toggled off, on, and off again: every opaque
  UI region (panel title bar, tab strip, dock, hint line) is **pixel-identical**. A
  full-frame compare is meaningless here because vkcube animates behind a translucent
  overlay. The stronger evidence is the diff: the **only** line removed from any legacy file
  in this whole part is the `GetIO()` crash fix.

### Known rough edges, recorded rather than hidden

- **A dynamic area is empty until the overlay has been drawn once.** `SyncIfStale()` runs in
  `Draw()`, so `overlay_e2_select` from the console lists no `audio.node.*` rows until the
  overlay has been opened. Harmless for a user (the rows exist whenever they can be seen)
  but it surprises a script, which is why it is written down.
- **`audio.volume` and `profiles.list` exist conditionally** — the first only when detection
  resolved a game stream, the second only when a profile is saved. `overlay_e2_select`
  correctly reports "no such E2 row" otherwise; that is the registration being honest, not a
  failure.
- **The accent hue gradient and the placement grid** are the two affordances that lost
  fidelity. Both are listed above with their reasons.

---

## 2026-08-23 — P3 part A, Display and Shaders

**Status: `display.gamescope` and `image.shaders` are real registrations.** `EscapeCount()`
**6 → 4**. No config key, value range, format or setter changed — this is a presentation change,
and the legacy panels still draw unchanged under `overlay_e2 0`.

Judgement calls taken during P3 part A: `../../AUTONOMOUS-DECISIONS.md` **D13**.

### The setting inventory, before and after

Nothing was dropped. The two panels' 17 settings map 1:1; three pieces of *prose* became
`.Live()` facts rather than rows (D13.5), and the frame limiter's two controls became one
(D13.3).

| Legacy tab / group | Setting | Now | Config key (unchanged) |
|---|---|---|---|
| Upscaling | Filter | `display.upscaling` ▸ Scaling filter | `gamescope.filter` |
| Upscaling | Sharpness | ″ (disabled unless FSR/NIS) | `gamescope.sharpness` |
| Upscaling | Scaler | ″ | `gamescope.scaler` |
| Upscaling | *Steam-override banner* | → `display.upscaling_facts` `.Live()` | — |
| Display | Allow Tearing | `display.upscaling` ▸ Presentation | `gamescope.tearing_enabled` |
| Display | Force Grab Cursor | ″ | `gamescope.force_grab_cursor` |
| Display | VRR / Adaptive Sync | `display.frame_limiter` | `gamescope.vrr_enabled` |
| Frame Limiter | Unlimited **+** FPS Limit | `display.fps_limit`, **one** Stepper | `gamescope.fps_limit` |
| HDR | HDR | `display.hdr` ▸ Output | `gamescope.hdr_enabled` |
| HDR | SDR Gamut Wideness | ″ | `gamescope.sdr_gamut_wideness` |
| HDR | SDR-on-HDR Brightness | ″ | `gamescope.sdr_on_hdr_brightness_nits` |
| HDR | HDR Input Gain | `display.hdr` ▸ Input gain | `gamescope.hdr_input_gain` |
| HDR | SDR Input Gain | ″ | `gamescope.sdr_input_gain` |
| HDR | *app metadata strip* | → `display.hdr_facts` `.Live()` ×4 | — |
| HDR | *tonemap "deferred" note* | → `display.hdr_facts` `.Live()` | — |
| Shaders | Vibrancy + 2 controls | `image.shaders.vibrancy` + **2 params** | `reshade.vibrancy.*` |
| Shaders | Pre-Sharpen + 1 control | `image.shaders.presharpen` + **1 param** | `reshade.pre_sharpen.*` |
| Shaders | Adaptive Brightness + 6 | `image.shaders.adaptive_brightness` + **6 params** | `reshade.adaptive_brightness.*` |

### Tabs became areas, not groups

`display.gamescope` no longer exists. SPEC §8.1's rail is the product's only navigation and
`index.html` declares Upscaling, Frame limiter and HDR as separate rail items, so a four-tab
panel becomes three areas — a four-group sheet would be the same tab bar redrawn as headings.
The old **Display** tab has no successor: presentation joins Upscaling, VRR joins the limiter.
`display.output` from the mockup was deliberately **not** built — there are no such settings in
this codebase. D13.1, D13.2.

### The Six Budget, reported rather than routed around

**Adaptive brightness owns exactly six params — the budget, with zero headroom.** It fits. It is
called out in `PanelShaders.cpp` and shown to the user as `PARAMETERS 6 of 6`, because the next
parameter added to that effect is not an overflow to work around: it is the signal that the
effect has become a *category*. D13.4.

### What the kit gained (the shared prerequisite)

P2's shell could draw Switch, Choice, Action and Facts — the four kinds `setup.shell` used. A
populated registry needs more:

- **`DrawSharedControl()`**, one painter templated over `Entry` *and* `Parameter`. They are
  distinct types on purpose, but SPEC §5.3 requires a Param to render identically to the Entry it
  could be promoted into — one function over both is what guarantees that.
- **Slider, SliderInt, Stepper, Meter** rows; int-vs-float is decided by what the binding holds.
- **Group bands** (SPEC §2.5), including `GroupCount()`'s `n / m`, computed from the band's own
  switch rows so the number cannot be typed.
- **Units and zero-words** — `ZeroMeans("Unlimited")` replaces the value, the unit is appended.
  Never baked into a value by a call site.
- **`ui::ScopedDim`**, under `Col()`/`Accent()`. SPEC §3.13's "row × 0.55" includes the control,
  and the atoms paint on the draw list where ImGui's disabled alpha never reaches. D13.6.
- **The Choice dropdown**, for when the segmented group does not fit its lane. P2 ignored
  `bWantsPopup`; a five-option filter row in a narrow sheet needs it. D13.7.
- **`Parameter`'s read side** — `HasRange/Lo/Hi/StepSize/Unit/ZeroWord/UsesValue/DisabledReason`,
  matching `Entry`'s name for name. `Range()` now also resolves the kind to Slider.

### Verified

`ninja -C build` clean. `meson test -C build` **67/67**; `[overlay_ui]` 30 → **34** cases,
285 assertions. Mutation-checked: removing `Range()`'s kind resolution fails the new param test.

Launched under `scripts/with-gamescope-lock.sh` at display_scale **1.0× and 2.0×**, in CONFIGURE
and DETAILS, on all four migrated areas, against a temporary `XDG_CONFIG_HOME`. Group bands, the
`0 / 3` effect count, `PARAMETERS 6 of 6`, dimmed disabled rows with their amber reason, units
(`25 %`, `203 nits`, `1 x`, `30 fps`) and the Facts/Live blocks all render as the mockup shows.

**A binding was proven to drive the compositor, not just the UI.** Writing through
`overlay_e2_set` — the same `Binding().Set()` a click calls — moved gamescope's own frame pacing
every time:

```
display.fps_limit = 60  ->  Swapchain received new refresh cycle: 16.67ms
display.fps_limit = 30  ->  Swapchain received new refresh cycle: 33.33ms
display.fps_limit = 5   ->  clamped to 10, cycle 100.00ms   <- issue #67's floor holds
display.fps_limit = 0   ->  Swapchain received new refresh cycle: 3.57ms (uncapped)
```

That is issue #25's exact failure mode tested directly: the control writes, and this time the
value sticks.

### Known rough edges, recorded rather than hidden

- **The Inspector does not scroll.** At 2.0× the ladder gives a drawer, and Adaptive brightness's
  six params overflow its bottom edge — the last row is clipped. P2 never hit this because its
  one real area had three rows and no params. `DrawConfigure()` lays out with absolute `y` on the
  draw list rather than through ImGui's cursor, so a scrollbar is real work, not a flag. Belongs
  to the P3 part that has the most params, not to this one.
- **`display.gamescope` as an id is gone.** Anything scripted against
  `overlay_e2_select display.gamescope` needs the three new ids.

### Still deferred

Unchanged from P2's list: the eleven rail icons, `Ctrl+K`, multi-column sheets, a sixth baked font
style, `Cfg()`, `Repeat()`, `ui_lint`, `ui_snapshot`. The four remaining escaped areas
(`audio.mixer`, `system.monitor`, `system.log`, `setup.config`) are the later parts of P3.

---

## 2026-08-23 — P2, the shell

**Status: the shell exists and is off by default.** `overlay_e2 0` (the default) draws the
legacy dock and its five floating windows exactly as before; `overlay_e2 1` replaces all of
it. No on-disk config key or format changed.

Phase plan: `../../AUTONOMOUS-DECISIONS.md` D10. Judgement calls taken during P2: **D12**.

### What was added

| File | What it owns | ImGui? |
|---|---|---|
| `UI/Layout.h/.cpp` | the slab, SPEC §8.3's ladder, the region rects, mode selection, the mode-strip counters | **no** |
| `UI/Shell.h/.cpp` | the slab window, the three regions, the keyboard map, the registry | yes |
| `tests/test_overlay_shell.cpp` | `[overlay_shell]` — the ladder table, region arithmetic, mode selection, `Law::Escaped` | no |

Plus: `Area::Escape()` and `Law::Escaped` in `Registry.*`; the registry's read side
(`Verb()`, `Invoke()`, `SummaryText()`, `Scalar()`, `Area::Available()`,
`Registry::FindArea()`, `KindName()`); a `Panel*_DrawBody()` on each of the five panels; and
`chrome::EnsureThemeLoaded()`.

### The three-region split, and why it is shaped this way

ImGui here is **1.92.9b stock, non-docking** — no dock-space, no splitter. But the reason
the split is hand-rolled is not availability: **a splitter is stored, mutable, per-user
geometry**, which is the exact category of state whose bug history motivated the redesign.

- **One top-level window — the slab.** `NoMove | NoResize | NoCollapse | NoSavedSettings |
  NoBringToFrontOnFocus`, no title bar, positioned and sized every frame from `Layout.h`.
  Its geometry is a pure function of surface size and `display_scale`; there is no stored
  layout to corrupt, migrate or reset.
- **The regions are child windows inside it.** A child gives clipping and independent
  scrolling — all the split needs — and gives no drag, no resize and no z-order, which is
  all the split must never acquire.

**Shell state, in full:** selected area, selected row, host preference, mode override. No
window positions, no sizes, no z-order, no open/closed set. That list *is* the difference
between this and what it replaces.

**Z-order is bought with begin-order, deliberately.** The drawer must paint over the sheet;
ImGui renders every child after its parent's own commands, so an inspector background on the
slab's draw list loses to the sheet's child no matter how late it is drawn. The whole
inspector region is therefore one child begun *after* the sheet's. `ImDrawListSplitter` was
rejected — it would make paint order a thing every future edit has to know about.

### `Escape()` — the migration seam

```cpp
reg.Add( "display.gamescope", "Gamescope", Section::Display )
   .Escape( []{ PanelDisplay_DrawBody(); } );
```

The shell pushes the padding legacy code expects, runs the body in the sheet's child, pops.
Six areas use it. **It looks wrong on purpose** — the sheet head labels the category
`legacy body` in `WarnText` and Overview says the shell cannot see inside it. There is no
styling that tries to blend a legacy body into an E2 sheet, because a half-convincing blend
is how a migration stops being urgent.

Two limits, both `Law::Escaped`, both in `AUTONOMOUS-DECISIONS.md` D12.4:

- **Sheet only.** No Inspector equivalent, ever — that would be SPEC §5.2 clause 0's
  forbidden fifth generator.
- **Legacy or E2, never both.** Escaping a populated area and populating an escaped one both
  fire. The half-migrated area is the shape that survives P3 indefinitely.

`Registry::EscapeCount()` is what a future `ui_lint` reports as severity `migration`.
It is **6** today and P3 drives it to zero, deleting `Escape()` with the last one.

### What the shell registers

Seven areas in SPEC §8.1's three sections (after D8's fold): `display.gamescope`,
`image.shaders` | `audio.mixer`, `system.monitor`, `system.log` | `setup.config`,
`setup.shell`. The first six are escaped. `setup.shell` is genuinely E2 and exists so both
Inspector modes ship exercised rather than as dead code — see D12.3. **Every one of its
bindings is to shell runtime state or a ConVar, never to a config field.**

### Verified

`ninja -C build` clean. `meson test -C build` **67/67** — P1's 66 plus `overlay_shell`.

`[overlay_shell]` pins SPEC §8.3's worked table row by row across `display_scale`
0.5×–2.0×, the ladder's step *order*, host preference in both directions, the content cap on
columns, the slab's surface clamp and purity, region rects tiling the slab at four scales in
all three hosts, the drawer overlapping where a column does not, the spine carved out rather
than laid over, mode selection per kind, the derived counters, and `Law::Escaped` in both
directions. Verified to fail under a deliberate mutation (swapping the ladder's two steps
fails at 1.5×, the only scale that separates them).

Visually confirmed under `scripts/with-gamescope-lock.sh` at 1.0× and 2.0×, in all three
inspector hosts, in both modes, on an escaped area, and with the ConVar toggled off again —
the dock returns identically, with no leftover state. At 2.0× the ladder reproduces §8.3's
row exactly: slab 1728 × 929 px (864 × 464 base), rail 60, drawer 400, sheet 804, step 2.

### Deferred to P3, deliberately

- **SPEC §8.0's eleven rail icons.** The rail draws initials. D12.8.
- **The command palette, `Ctrl+K`.** It walks a populated registry; there is one real area.
- **Multi-column sheets.** `LadderResult::nColumns` is computed and tested; the sheet draws
  one column, because the only area with rows has three of them.
- **A sixth baked font style.** Six type roles map onto five. D12.11.
- **`Cfg()`, `Repeat()`, `ui_lint`, `ui_snapshot`.** Unchanged from P1's list — all need a
  populated registry.

### Known rough edges, recorded rather than hidden

- A legacy body wider than its sheet column gets a **horizontal scrollbar**. The panels were
  authored for a ~440-wide resizable window. An unreachable control is worse than a
  scrollbar in a region that will not have one after P3.
- **`Tab` region cycling is tracked but not yet wired to ImGui focus.** `s_eFocusRegion`
  moves; nothing reads it. Arrow-key navigation within a region is P3's, with the rows.

---

## 2026-08-23 — P1, the kit foundation

**Status: new code only. Nothing here is called by the running UI.** The only
existing files touched are `src/meson.build` and `tests/meson.build`, both for
build wiring.

### Where it lives, and why there

`src/Overlay/UI/` — the location `API.md`'s own "Proposed location" section
names, together with the filenames it lists. Deviating would have made every
path in the contract wrong on day one.

| File | What it owns | ImGui? |
|---|---|---|
| `Tokens.h/.cpp` | geometry, spacing scale, type roles, `display_scale`, motion | **no** |
| `Lane.h/.cpp` | SPEC §2.1's four columns, as pure arithmetic | no |
| `Row.h/.cpp` | `RowCtx` — the one right-bound allocator | ImRect only |
| `Band.h/.cpp` | SPEC §4.2's n × 44 composite band rule | ImRect only |
| `Registry.h/.cpp` | `Area` / `Entry` / `Parameter`, the four laws | **no** |
| `Colors.h/.cpp` | SPEC §7.1's colour roles | yes (accent is hue-live) |
| `Controls.h/.cpp` | the control atoms | yes |

**Why the split runs along the ImGui line.** Everything with real arithmetic or
a real rule in it is free of an ImGui context, a font atlas and the overlay's
live-theme globals. That is what makes the laws and the lane cheap to test —
`tests/test_overlay_ui.cpp` needs no window. Only the two files that pick
colours and hand rects to stock ImGui primitives need a context, and those are
covered by a headless harness (`tests/test_overlay_atoms.cpp`).

### The four laws, and where each is enforced

| Law | Enforced | How |
|---|---|---|
| **Prefix Law** (§5.2 cl. 2) | construction | `Param()` takes a *leaf*; the id is synthesised as `<parent>.<leaf>` and there is no other route. Re-checked in `Registry::SelfTest()` so a test can observe it. |
| **One Level** (§5.2 cl. 3) | **compile time** + runtime | `Parameter` has no nesting operation; its `.Param()` is a *sibling* factory on the parent Entry. A dot in a leaf — the only way to smuggle nesting into an id — is a runtime violation. |
| **Six Budget** (§5.2 cl. 3) | registration | the 7th `.Param()` reports `Law::SixBudget` and is not added. |
| **id uniqueness** | registration | one registry-wide claim list; areas, entries and params share the namespace, because the palette and `ui_snapshot` address all three by id. |
| **`Help()` required** (§5.2 cl. 1) | call + `SelfTest()` | empty/null text fires at the call; *never called at all* can only be decided once registration is over, so it fires in `SelfTest()`. |
| **Details is read-only** (§5.2 cl. 4) | **compile time** | `.Live()` takes `std::function<Fact()>` and has no `Bind` overload. |
| **`DisabledUnless` needs a reason** | **compile time** + runtime | no overload without the string; an empty string is the residual hole and is closed. |

**What happens when a law fires.** The project builds with `-fno-exceptions`,
so a violation cannot be thrown. The default handler prints and `abort()`s — a
malformed registry is a boot failure, not a runtime condition anyone handles.
`ui::LawRecorder` is the one seam that changes what happens *after* a violation
fires, never whether it fires; the tests install one so they can assert *which*
law caught the thing without terminating the binary. Nothing in the shipping
build constructs one.

### Left alignment is unrepresentable

`RowCtx` exposes `Place( widthBase )`, `PlacePx( widthPx )` and `PlaceFull()`.
None takes an x. Both public entry points route through one rect construction
whose `Max.x` is the lane's control edge, unconditionally, with no flag to
change it. There is no `Left()`, no `Centre()`, no `SameLine`, no
`SetCursorPosX`, and no atom touches ImGui's cursor — so a competing layout
system has nothing to act on even if someone called into it.

`SplitLabelZone()` returns the label rect and the value rect from **one**
subdivision, so the two cannot be computed inconsistently.

### The drawn-vs-hit-tested divergence (#23's bug class)

Two structural rules, both in `Controls.cpp`:

1. **One `ImRect` per atom.** `Begin()` registers the rect with `ItemAdd()` and
   hands the *same object* back for painting. An atom never recomputes its own
   geometry.
2. **The slider's handle is not a constant.** `SliderGrab()` is the only code
   in the kit that names a grab width. It pushes that width into
   `GrabMinSize`, calls `SliderBehavior()`, pops, and returns the grab rect
   `SliderBehavior` itself produced. `PaintSlider()` draws that rect and is
   never told how wide a handle should be. There is one number and the drawing
   code cannot see it, so a future edit moves the visible handle and the
   draggable target together or not at all.

The same shape covers the measured atoms: `MeasureCells()` is the single
function that decides how wide a segmented group or a chip bank is, and *both*
the "does it fit the lane" predicate and the per-cell layout read its output. A
control cannot be laid out to a width its own fit test never saw.

### Tests

| Suite | Covers |
|---|---|
| `tests/test_overlay_ui.cpp` (`[overlay_ui]`) | token derivation across `display_scale` 0.5–2.0, the hairline floor rule, lane arithmetic against SPEC §8.3's worked example, the allocator's right-edge invariant, the label/value split, band quantisation, and every law made to **fire** |
| `tests/test_overlay_atoms.cpp` (`[overlay_atoms]`) | ImGui run headlessly — no window, no renderer. Asserts the registered hit box equals the allocated rect at four scales, that every atom leaves the style/colour/ID/item-flag stacks balanced, that a click lands on the lane rect and *not* where a left-aligned control would have been, and that `Choice` downgrades to a dropdown on all four of its conditions |

Both suites were verified to fail under a deliberate mutation (making `Place()`
left-bound) before being accepted.

**The harness needs one hack, recorded here so it is not mistaken for a
pattern:** `palette::g_LiveTheme` is defined in `Chrome.cpp`, so the test binary
defines the storage itself rather than linking 1,600 lines of legacy dock
machinery to read one float. The kit never reads that global — `ui::SetScale()`
is its one scale input — so this only satisfies `Palette.cpp`'s accent
recomputation.

### Deferred to later phases, deliberately

Not omissions; each has a reason.

- **The dropdown popup and the text field's validation plumbing.** `index.html`
  itself calls the popup "shell furniture, not a control atom". The closed
  states of both atoms are complete; the popup, its clipper and the
  Configure-hosted validation message belong to the shell (P2).
- **`Area::Repeat()`** (dynamic collections). It is a rebuild *policy*, not a
  foundation law, and it needs the shell's frame loop to have a topology-change
  hook to attach to.
- **`Cfg()`** — schema-backed bindings that know their destination file.
  `AnyBind` covers pointer and getter/setter binding today; `Cfg()` is the
  config seam and lands with P2, where `ConfigManager`'s write queue is wired.
- **`Escape()`** (API.md §13's migration seam) — it exists to host legacy panel
  bodies inside the sheet, which requires the sheet.
- **`ui_lint` / `ui_snapshot` / `--host=inline`.** They walk a *populated*
  registry; there are no areas until P3.

### Judgement calls

Recorded in `../../AUTONOMOUS-DECISIONS.md` under **D11**, with the reasoning.
The two a reader is most likely to trip over:

- **`SPEC.md` §2.2's lane clamp is arithmetically self-contradictory** and was
  resolved against the mockup and §8.3's worked numbers. See `Lane.h`'s header
  comment for the full derivation.
- **The class is `ui::Parameter`, not `ui::Param`.** C++ forbids a member
  function whose name matches its enclosing class, and API.md §7's chained
  `.Param()` declarations require exactly that member. The concept and the laws
  are unchanged.
