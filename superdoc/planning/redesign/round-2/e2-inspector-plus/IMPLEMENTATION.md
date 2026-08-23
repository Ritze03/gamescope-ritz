# E2 — implementation log

What has actually been built in C++, phase by phase, and what a later phase can
rely on. `SPEC.md` and `API.md` remain the contract; this file records where the
code *departed* from them and why, so the next agent does not have to
re-derive it.

Phase plan: `../../AUTONOMOUS-DECISIONS.md` D10. Judgement calls taken during
P1: the same file, D11.

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
