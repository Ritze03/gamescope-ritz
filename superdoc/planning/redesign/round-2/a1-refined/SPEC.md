# Direction A — CONSOLE, refined (A1)

**A disciplined refinement. A's structure is kept; the five things the user called out
are fixed at the system level, and nothing else is touched.**

Date: 2026-08-23. Companion files: `API.md` (the helper layer + the registry),
`FEASIBILITY.md` (immediate-mode ImGui and migration cost), `index.html` (the interactive
mockup — every number below is live in it).

Baseline: `superdoc/planning/redesign/a-console/`. Read that first; this document records
**only what changes and why**, plus the parts that had to be restated because they moved.

---

## 0. What is preserved, deliberately

The user's two positives are the constraint, not a compliment:

> *"The simplicity is amazing, since it doesnt look intimidating."*
> *"Looks clean and user friendly."*

So these are untouched, and every fix below was checked against them:

- **One pane, no windows.** No dock, no title bars, no Z-order, no per-panel opacity.
- **One row grammar** — `[tick] [label] ……… [control]`, one hit target, one focus stop.
- **The accent budget.** Accent means exactly two things: *this is where you are* and
  *this is a live number*. Nothing decorative is accent.
- **No boxes around groups.** A group is a heading, a hairline and space. The console is
  the box; drawing twelve more inside it turns one clean pane into a scrapbook.
- **Wells** as the answer to "preview without a second window".
- **Dark only.** A light theme over a game at night is a flashbang.
- **The motion budget** — four motions, five floats, nothing per-widget, values never animate.
- **Sheets** as the single overlay mechanism for picking one thing.

The net change in *ink* is negative: this refinement deletes a whole row of chrome (the
sub-tab bar) and the entire gamepad legend, and adds one bar that was already there in a
less useful form. **If a change made the screen busier, it was the wrong change.**

**Gamepad support is out.** Dropped by the user on 2026-08-23. Every trace is gone: the
button legend, the LB/RB hints, "adjust with ←→ so you never enter a control" as a
*controller* argument (the interaction survives on its own merits for the keyboard), and
the `44 physical px` touch floor. Mouse and keyboard only.

---

## 1. Fix 1 — the tabs

> *"The tabs themselves look kinda bad (Example: Upscaling, Display, Frame Limiter, HDR)."*

### 1.1 The diagnosis

They looked bad because of what they *were*, not how they were painted. Four bordered
mono-uppercase chips of unequal width, sitting in their own band, one hairline below a
breadcrumb that already named the same thing. That is:

- a **second navigation axis** (horizontal) for the same job the rail already does
  vertically — two grammars for "choose where you are";
- a **ragged edge** — chip width follows label length, so "HDR" and "Frame Limiter" can
  never line up, and no amount of restyling fixes that;
- **the busiest object on a calm screen**: eight borders and four fills, spent on
  navigation, above content that has almost no borders at all;
- a **duplicate** of the breadcrumb's last segment.

Restyling them (underline strip, quieter fills, segmented control) fixes the *look* and
leaves the duplication and the second axis in place. So:

### 1.2 The decision: the tab bar is deleted. Screens move into the rail.

The rail becomes **two zones in one column**:

```
┌──────────────────────┐
│ CONSOLE              │  zone label — reserved height, always
│ ▎▣ Gamescope      12 │  ┐
│   ▣ Shaders        9 │  │
│   ▣ System Monitor21 │  │  ZONE 1 · sections
│   ▣ Audio          5 │  │  fixed set, fixed height, fixed positions.
│   ▣ Config        18 │  │  NOTHING in this zone ever moves.
│   ▣ Log              │  ┘
│ ───────────────────  │  hairline, fixed y
│ GAMESCOPE            │  zone label — the selected section's name
│    Upscaling      10 │  ┐
│   ▎Display         6 │  │  ZONE 2 · the screens of that section
│    Frame Limiter   4 │  │  content changes; the zone's top edge does not
│    HDR            13 │  ┘
│                      │
│ gamescope-ritz 3.16  │
└──────────────────────┘
```

Why this and not a segmented control, a sub-rail column, or an accordion:

| Candidate | Why not |
|---|---|
| Restyled tab strip (underline) | Fixes the paint, keeps the duplication and the second axis. Cheapest, weakest. |
| A third column (sub-rail) | Three columns is *more* furniture on a screen whose virtue is having little. Costs ~180px of stage. |
| Accordion — expand the selected section in place | Every icon below the expanded section moves down. That is exactly complaint 4, applied to the whole rail. Disqualified by the user's own note. |
| **Two fixed zones (chosen)** | One navigation grammar, one axis, one focus model. Icons cannot move (§4). The stage starts with *content*. Net ink goes down. |

What it buys, concretely:

- **The stage begins at the first group heading.** 44px of vertical room returns to the
  content, and the first thing your eye meets is a setting, not chrome.
- **One keyboard axis.** `↑↓` runs through the rail continuously — sections, then that
  section's screens — instead of `Tab` for one axis and `PgUp/PgDn` for another. (Both
  shortcuts survive as *jumps*; they are no longer the only way.)
- **Counts become useful.** Each screen shows how many settings it holds, so "where is
  the dense screen" is answerable before you open it. A tab strip has nowhere to put that.
- **The breadcrumb stops duplicating.** It reads `CONSOLE › GAMESCOPE › Upscaling`, and
  every one of those three words is highlighted *somewhere in the rail* — the breadcrumb
  is now a caption for the rail's state rather than a competing control.

### 1.3 The one exception: `Stage::Wide`

LOG contracts the rail to icons, so zone 2 is not on screen. Its screen switcher moves
into the wide toolbar as the **segmented control that was already there** (`gamescope` /
`game`). No new mechanism: a Wide screen's switcher is a Segmented control in its toolbar,
in the first slot, always. This is the only place a screen switcher is not in the rail,
and it is the only place the rail cannot show one.

---

## 2. Fix 2 — control sizing

> *"Sliders, Multiselectors and stuff dont use as much space, as they deserve. Also, they
> differ in size too much."*

### 2.1 The diagnosis

Two separate faults with one root:

1. **Too small.** The control column was 56u (224px). A slider spent 17u of that on its
   value slot, leaving ~140px of track — the same width the *shipped* overlay gives it
   (`slider-widget-spec.md` measures the shipped track as the full row width, which was
   the one thing the shipped slider got right and A regressed by boxing it).
2. **Inconsistent.** Controls sized themselves from their content, so two pickers with
   different words were different widths, and `Choice()` silently swapped to a "wide
   control column" (76u) when it measured wide options — meaning a *screen* could contain
   two different control-column widths.

Root cause: **content decided width.** So two controls of one kind could differ, and the
kit had a second, wider column as an escape hatch.

### 2.2 The fix: one column, three fill classes, assigned by kind

There is exactly **one control column**, `--ctl-w = 96u` (384px @ 1.0×), flush to the
stage's right edge. There is no wide variant. Inside it, every kind belongs to one of
three fill classes, and **the class is a property of the kind, chosen by the kit** — a
call site cannot name it, override it, or measure its way into a different one:

| Class | Geometry | Kinds |
|---|---|---|
| **FILL** | spans the full 96u column | Slider · Segmented · Chips · Text |
| **HALF** | exactly 48u, right-aligned in the column | Picker · Stepper · Colour · Multi-select · Action |
| **MARK** | intrinsic width, right edge flush to the column's right edge | Toggle (fixed 11u × 6u) · Readout · Drill |

The invariant that fixes "they differ in size too much", stated once:

> **Every control's right edge is the control column's right edge. Its left edge is
> decided by its class. Therefore two controls of the same kind occupy the same box on
> every screen, at every scale, forever — and the screen has exactly three vertical
> alignment lines instead of one ragged one.**

Consequences worth naming:

- **The slider gets 292px of track** at 1.0× (384 − 80 value slot − 12 gap), up from ~140.
  A 0–1000 nits slider and a 0–1 opacity slider are the same width, pixel for pixel.
- **The value slot is fixed at 20u (80px)** — seven monospace digits, enough for
  `1000 nits` and `-1.00`. It never resizes to fit, so slider handles at the same
  fraction land at the same x on every row of a screen.
- **Segmented cells share the column equally** (`flex:1`), so a 3-cell and a 5-cell
  control are the same total width and their outer edges align. **The kit resolves a
  registered `Choice` to Segmented or Picker once, at registration, from the option
  strings**: `count ≤ 5 && longest label ≤ 8 characters` → Segmented, otherwise Picker.
  So `Filter` (linear/nearest/fsr/nis/pixel) is a segmented control and `Tonemap Operator`
  (…/uncharted2/…) is a picker, and neither call site got a vote. Deciding it from the
  registered strings — not by measuring text at draw time — removes a known hazard: a
  font-atlas rebuild frame could otherwise flip the decision mid-drag.
- **Multi-select is a HALF box** ("3 of 6 ›") that opens the sheet, and the sheet is
  where the room actually is. The user's complaint about multi-selectors was that a
  cramped inline widget pretended to be enough; it is not, so it stops pretending.
- **Chips are FILL** and right-aligned within it, because a chip bank is a summary that
  wants the whole column.

### 2.3 Metrics (everything derives from `u = 4px × display_scale`)

| Thing | Value @1.0× |
|---|---|
| Control column `--ctl-w` | 96u — 384 |
| Half class `--ctl-half` | 48u — 192 |
| Slider value slot `--val-w` | 20u — 80 |
| Gap label ↔ control | 12u — 48 |
| Row height (single line) | 11u — 44 |
| Stage edge padding | 6u — 24 |
| Rail, expanded | 62u — 248 |
| Rail, contracted | 17u — 68 (= 6u pad + 5u icon + 6u pad) |
| Rail section item | 12u — 48 |
| Rail screen item | 9u — 36 |
| Header | 14u — 56 |
| Detail bar (min) | 15u — 60 |
| Console | min(297u, 94 %) × min(194u, 90 %) |

Width arithmetic at 1.0×: `1188 = 248 rail + 940 stage`; `940 − 48 padding = 892 content`;
`892 = 460 label + 48 gap + 384 control`.

### 2.4 The responsive ladder

Everything is proportional to `u`, so 0.5×–2.0× needs no special cases. The only ladder is
for a *small surface*, where the console hits its 94 %/90 % clamp and the stage narrows:

| Stage content width | Control column | Value slot |
|---|---|---|
| ≥ 132u | 96u | 20u |
| 108u – 132u | 76u | 18u |
| < 108u | 60u | 14u |

One function computes it; nothing else makes a width decision. The half class is always
exactly half of whatever the column currently is, so the alignment lines survive every step.

### 2.5 The complete control taxonomy

| # | Kind | Class | Rendering | `←→` | `Enter` |
|---|---|---|---|---|---|
| 1 | **Toggle** | MARK | 11u × 6u track, 4u knob. On: accent @62 % fill, 1px accent border, `accent-knob` knob right. Off: white @8 % fill, `line/strong` border, white @72 % knob left. | flips | flips |
| 2 | **Segmented** | FILL | equal cells, 0.75u gap. Active: accent @55 % fill, 1px accent border, `accent-seg` label. Inactive: white @5 % fill, `line/hair` border, `text/label`. Chosen over Picker by the §2.2 rule. | cycles | cycles |
| 3 | **Picker** | HALF | value in `accent-value` Mono in a white @6 % box + `›`. | cycles | opens the sheet |
| 4 | **Slider** | FILL | groove white @22 % · accent gradient fill · 2.5u × 5u handle with glow · fixed 20u value slot. | steps (fine with `Shift`) | begins keyboard drag |
| 5 | **Stepper** | HALF | number in a box; `0` renders as the registered `ZeroMeans` word ("unlimited"). | steps | inline edit in place |
| 6 | **Text** | FILL | value in a box, edits in place. No popup, no second layout. | — | begins edit |
| 7 | **Action** | HALF | uppercase Mono verb in an accent outline. Destructive uses `state/danger` and needs a second press within 2 s. | — | runs |
| 8 | **Drill** | MARK | right-aligned Mono summary + `›`. **The summary is mandatory.** The chevron column is what makes every drill align. | — | pushes the screen |
| 9 | **Readout** | MARK | Mono `text/meta`, optional 1.5u status dot. Never looks pressable; focusable so it can be copied. | — | copies |
| 10 | **Multi-select** | HALF | "3 of 6 ›"; the sheet holds the checkbox rows. | — | opens the sheet |
| 11 | **Colour** | HALF | 7u × 5u swatch + hue in Mono + `›` inside the half box. | shifts hue 2° | opens the hue sheet |
| 12 | **Chips** | FILL | chip bank, accent-filled when on, neutral when off. Read-mostly. | — | opens the sheet |
| 13 | **Anchor 3×3** | Well only | nine cells drawn **over the live preview**; the HUD jumps to the cell. | moves | confirms |
| 14 | **LogView** | Wide body | virtualised monospace list. Not a row. | — | — |
| 15 | **Meter / Graph** | Well only | never a row. | — | — |

Rules that make an inconsistent screen hard to build (unchanged from A, plus two):

- **A number is never in a Sans run.** Every value is Mono. (A's best rule, kept verbatim.)
- **A row's control cannot overflow its class box.** Debug builds assert (`ui.audit 1`).
- **`Help()` is required at registration** — see §5. A row with no explanation cannot exist.
- **A disabled row must say why.** `bDisabled` without a reason is a debug-build assert.
- **NEW: a call site cannot name a class.** `CLASS[kind]` is a table in the kit. Adding a
  kind means adding a row to that table, which is a visible change in this document.

---

## 3. Fix 3 — contrast, measured

> *"The contrast is kinda bad, so some stuff is kinda hard to read."*

This project has had three "too dark to read" complaints already (issue #62 and the two
it consolidated). Fixing them per-site is what produced three of them. This section fixes
it at the **role** level and states the arithmetic, so a fourth complaint has a number to
argue with.

### 3.1 The model

The console never sits on a known background, so "measure it" needs a defined chain. This
one is used identically by this document and by `index.html`, so they cannot disagree:

```
game pixel
  → compositor pass:  blur 1.0 + darkening 0.8, modelled as rgb(4,6,9) @ 62 %
  → surface/base:     rgba(8,9,11, .86)
  → elevation:        white @ 3.5 % (rail) / 6 % (focused row) / 22 % (slider groove)
```

Four game extremes are evaluated and the **worst of all four** is reported:

| Game | Effective console background |
|---|---|
| night scene rgb(12,26,38) | rgb(8,10,13) |
| blown-out fire rgb(217,118,42) | rgb(19,15,12) |
| white snow field rgb(255,255,255) | rgb(21,22,24) |
| pure black | rgb(7,8,10) |

**The finding that makes this tractable:** near-white text at alpha *α* over a ≥ 86 %-opaque
near-black surface has almost **game-independent** contrast, because the text composites
against the same surface the ratio is measured against — as the background darkens, so
does the text. The shipped `text/meta` at 46 % measures 4.14:1 over a white game and
4.32:1 over a black one. **Worst case is therefore a property of the surface, not of the
game** — which is why one role table can be correct everywhere, and why the fix is a role
change rather than a per-screen one.

### 3.2 The measured table

Targets: **4.5:1** for text, **3:1** for UI parts that carry state and for large text.
Every figure is the worst over the four games *and* over the three surfaces text can land
on (base, focused row +6 %, rail +3.5 %). Accent figures are the worst over all 360 hues.

**Text roles** — `#EFF5FB` at alpha:

| Role | α | Worst | Target | Used for |
|---|---|---|---|---|
| `text/primary` | 96 % | **13.16:1** | 4.5 | focused row label, breadcrumb current, sheet/palette selection, hero numbers |
| `text/label` | 82 % | **9.96:1** | 4.5 | every row label, log message body — the main reading role |
| `text/meta` | 62 % | **6.32:1** | 4.5 | help text, group headings, read-only values, log timestamps |
| `text/faint` | 44 % | **3.91:1** | 3.0 | **glyphs only** (`›`, `·`, separators). Never prose — see §3.3 |
| *was* `label` | 68 % | 7.30:1 | 4.5 | passed, but only just, and it read grey next to accent |
| *was* `meta` | 46 % | **4.14:1** ✗ | 4.5 | **this is the complaint** |
| *was* disabled row | 68 % × 0.38 | **2.09:1** ✗ | 4.5 | the worst thing in the old design |

**Accent family** (OKLCH, L and C fixed per token, hue user-tunable):

| Token | Worst | at hue | Target |
|---|---|---|---|
| `accent` (focus tick, borders, fills) | **6.38:1** | 353° | 3.0 |
| `accent-edge` | 7.86:1 | 19° | 3.0 |
| `accent-value` (every number) | **8.98:1** | 18° | 4.5 |
| `accent-text` | 8.54:1 | 353° | 4.5 |
| `accent-knob` (slider handle) | 9.80:1 | 18° | 3.0 |
| `accent-seg` | 10.95:1 | 18° | 4.5 |
| `accent-seg` on an accent @55 % fill | **4.12:1** | 25° | 4.5 |
| `accent-knob` on an accent @62 % fill | **3.13:1** | — | 3.0 |

**State-carrying UI parts** (WCAG 1.4.11):

| Part | Worst | Target | Note |
|---|---|---|---|
| Focus tick vs the focused row's fill | **6.38:1** | 3.0 | the actual focus indicator |
| Toggle ON fill, accent @62 % vs surface | **3.32:1** | 3.0 | raised from @30 %, which measured **1.68:1** ✗ |
| Toggle OFF knob, white @72 % vs surface | **10.28:1** | 3.0 | raised from @55 % |
| Active segment / chip border (accent) | **6.38:1** | 3.0 | the indicator; the fill only reinforces it |
| Slider fill vs groove | **3.68:1** | 3.0 | the value indicator |
| Slider handle vs groove | 5.66:1 | 3.0 | |

**Hue-fixed state roles:**

| Role | Value | Worst |
|---|---|---|
| `state/ok` | `oklch(.78 .16 145)` | 8.24:1 |
| `state/warn` | `oklch(.72 .17 55)` | 5.96:1 |
| `state/danger` text | `oklch(.82 .09 25)` | 8.62:1 |
| `state/danger` fill/border | `oklch(.70 .17 25)` | 5.40:1 |

**Other surfaces:**

| Where | Worst |
|---|---|
| Palette / sheet `rgba(10,12,15,.97)` — primary / label / meta | 16.14 / 11.84 / 7.08:1 |
| Rail seen *behind* the palette's scrim | 2.97:1 — deliberately receded, still visible (§6) |
| **HUD preview text on its own backdrop @0.60 over a white game** | **5.21:1** |

### 3.3 The three exemptions, stated rather than hidden

1. **Row divider, white @7 % → 1.14:1.** A separator between two rows carries no
   information; WCAG 1.4.11 exempts pure decoration. Raised from 4.5 % anyway so it is
   *visible*, which was the actual reason it read badly.
2. **Slider groove, white @22 % → 1.88:1.** The groove is the *container*; the state (the
   value) is carried by the fill (3.68:1 against the groove) and the handle (5.66:1).
   Reaching 3:1 on the groove itself needs white @34 %, which reads as a bright bar
   dominating a calm row. Raised from the shipped 16 % to 22 % and stopped there,
   deliberately.
3. **`text/faint` at 3.91:1.** Below the 4.5 text bar, so it is **structurally forbidden
   from carrying a word**: chevrons, middots, breadcrumb separators. Log timestamps —
   which are semi-readable content — were moved up to `text/meta` because of this rule.

### 3.4 Two role changes that are behaviour, not colour

**Disabled rows keep their luminance and lose their accent.** The old treatment multiplied
the whole row by 0.38, landing its label at 2.09:1 — a row that explains why it is
unavailable, rendered too dark to read the explanation. Now:

> A disabled row's label drops from `text/label` to `text/meta` (**6.32:1**), its reason
> line renders at `text/meta`, and its control is repainted in neutral white instead of
> accent. **"Disabled" is signalled by removing colour, never by removing luminance.**

**Contrast does not depend on the opacity slider.** Below `opacity_console` 0.85, the kit
ramps `text/label` 82 → 88 % and `text/meta` 62 → 70 %. Readability is a helper
responsibility, not something the user can slide away.

### 3.5 The measurement is a build artefact, not a one-off

- `ui.contrast 1` (ConVar) composites every painted text run against its actual surface
  and flags anything below its role's floor. This is what stops complaint number four.
- `Registry::SelfTest()` (debug builds) sweeps the accent family across all 360 hues at
  startup and asserts each token's floor, so a future palette edit cannot quietly
  introduce a hue that fails.

---

## 4. Fix 4 — the rail icons must not shift

> *"I like, that the sidebar contracts, when i open the log, but the symbols shouldnt shift
> around, like they do now."*

They shifted because the contracted rail *centred* its icons (`justify-content:center`),
so every icon slid left by a different amount than the rail's edge did, and because the
zone label and footer were `display:none`d in the contracted state, which pulled every
item below them upward.

Three mechanisms, all structural:

**1. The contracted width is derived from the icon, not chosen.**

```
--rail-cw  =  --pad-x  +  --icon  +  --pad-x   =   6u + 5u + 6u = 17u (68px)
```

The icon keeps its `padding-left: --pad-x` in **both** states and is never centred. It
*looks* centred when contracted because the right padding equals the left padding by
construction. **The icon's x is literally the same number in both states, so there is
nothing for the animation to interpolate.**

**2. Nothing above an icon may change height.** Zone labels ("CONSOLE", "GAMESCOPE") and
the footer keep their reserved box in the contracted state; only their **opacity**
animates, 1 → 0 over 120 ms. `display:none` is banned anywhere in the rail.

**3. Only two properties animate, and neither is a position.** The rail's `width`
(62u ↔ 17u, `Approach` at 12/s) and the label/count `alpha`. Section icons are painted at
`railX + pad-x` every frame in both states; screen-zone rows fade with the labels.

Verify it in the mockup: turn on **icon-shift proof** in the demo strip. Two guides are
pinned at the icon's left and right x. Switch to LOG and back — the icons stay between the
guides while the rail wall moves.

---

## 5. Fix 5 — where explanation lives, if not in a tooltip

> *"The simplicity is amazing, since it doesnt look intimidating. But for some stuff, it
> makes it more complex, since we'll have to rely on tooltips and such."*

### 5.1 The room that just opened up

Dropping gamepad support empties the bottom bar. It held `Ⓐ Adjust · Ⓑ Back · Ⓨ Reset ·
Ⓧ Save profile · / Find` — 52px of the console spent on glyphs for a device that does not
reach the overlay. That space is now the answer.

### 5.2 The Detail bar

The bottom band becomes a **detail line that always describes the focused row**:

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Sharpness — FSR / NIS sharpening strength. Higher is sharper; too high rings  │
│ around high-contrast edges.                                                   │
│ range 0–20 · default 2 · writes 1245620.json     ↑↓ row  ←→ adjust  ⏎ open ⋯  │
└──────────────────────────────────────────────────────────────────────────────┘
```

- **Line 1** — `text/label`, the row's name in `text/primary` + its registered `Help()`.
- **Line 2** — `text/meta` Mono: range, default, option count, and **where the edit lands**
  (`writes 1245620.json` / `writes global.json` / `read-only`). That last item answers the
  question the old routing badge answered vaguely, precisely and per-row.
- **Right** — the fixed key hints. These never change, so the only moving text in the bar
  is the part you asked for by moving focus.

Why this is the *modest* answer and not a structural one (A2 is exploring that):

- **It adds no furniture.** The band already existed and already had a border. Object
  count goes *down*: five glyph pairs and a hint become two lines of prose.
- **It is stationary.** It does not open, hover, follow the pointer, or cover anything.
  A tooltip is a surprise; this is a fixture you learn to glance at.
- **It cannot be missed and cannot be hunted for.** There is no discovery problem: the
  explanation for whatever you are touching is always in the same place.
- **Rows stay one line.** The 44px row and the calm list survive, because the explanation
  moved *out* of the list rather than into every row as a sub-line.
- **Pointer users get it too.** Hovering a row previews it in the bar; clicking commits
  focus. Same information, no tooltip, no delay, no dismissal.

### 5.3 The inline sub-line is now reserved

A row's second line is used for exactly one thing: **conditional text** — why a row is
disabled, or what it depends on. So a sub-line always means "something is different about
this row right now", which makes it worth reading. Static explanation never appears there.

### 5.4 The rule that makes the bar reliable

**`Help()` is required at registration.** Registering a setting without one is a startup
assert, not a lint. The bar therefore has content for every row that exists, and
"undocumented setting" is not a state the product can reach.

Tooltips are not in the API. There is no call to make one.

---

## 6. Adopted from Direction B — the registry and the palette

> *"I definitely want B, but rather as a feature, than the entire GUI."*

### 6.1 The registry

Every setting registers **once**, declaratively, exactly in B's shape (`API.md` §2). One
declaration feeds: the rail's screen list and its counts, the row's rendering, the detail
bar, reset-to-default, config write routing, `gamescopectl ui get/set`, and the palette.
Nothing in a declaration describes appearance — no size, no colour, no font, no position.

Browsing the rail stays the **primary** navigation. The registry is what browsing reads
from; it is not a second way to navigate.

### 6.2 The command palette — `Ctrl+K`

A centred card over the console with a scrim. It:

- searches labels and registered keywords, with the match highlighted in `accent-value`;
- shows each result's **path** (`Gamescope › Upscaling`) so a jump is never disorienting;
- renders the result's **real control** on the right, in the HALF-class slot, so `←→`
  adjusts a setting **without leaving the palette** — flip a toggle, nudge a slider,
  cycle a picker, and keep typing;
- on `Enter`, **navigates the rail**: it selects the section, selects the screen, closes,
  scrolls the row into view and flashes it.

### 6.3 How it integrates without disturbing the rail

This is the whole point of taking B as a feature rather than as the GUI:

1. **The palette is transient; the rail is the destination.** Every `Enter` ends *in the
   rail*, on a browsable screen, with focus on the row. You are never left inside a search
   result set — there is no "search mode" to be stuck in and no back-stack to unwind.
2. **The rail stays visible behind it.** The scrim recedes it to 2.97:1 rather than hiding
   it, so the palette reads as something *in front of* your place, not instead of it.
   Watching the rail's selection change as you pick is what teaches the palette's effect.
3. **It occupies no permanent pixels.** No search field in the header, no persistent query
   line, no filter chips. The console at rest looks exactly as it did.
4. **It reads the same registry the rail reads.** There is no second index to keep in sync,
   so a setting cannot be findable but unbrowsable, or the reverse.
5. **It borrows the row grammar wholesale.** A palette result is a row with a path caption.
   No new control kinds, no new sizing rules, nothing new to learn.

The one thing it does *not* do is become the front door. `Ctrl+Shift+O` opens the console
on the rail, as before. The palette is a shortcut for people who already know the name of
the thing they want.

---

## 7. Navigation, restated

### 7.1 The depth stack

Three levels; the header pips say which.

| Level | Focus is on | Back goes to |
|---|---|---|
| **L0 Rail** | a section (zone 1) or a screen (zone 2) | closes the console |
| **L1 Stage** | a row on the current screen | L0 |
| **L2 Sheet / Drill** | an option in a sheet, or a row in a pushed screen | L1 |

There is no L3. Sheets never stack. If a screen wants to go deeper, it should have been
two screens.

### 7.2 Input — keyboard and mouse only

| Key | Action |
|---|---|
| `↑` `↓` | move row focus (or rail focus at L0) |
| `←` `→` | **adjust the focused control in place** — no "enter the control" step |
| `Shift` + `←` `→` | fine step |
| `Enter` | activate: open a sheet, drill, run an action, begin an inline edit |
| `Esc` | back one level; at L0, close the console |
| `Tab` / `Shift+Tab` | previous / next **section** — jumps from anywhere |
| `PgUp` / `PgDn` | previous / next **screen** in the section |
| `Y` | reset the focused row to its registered default |
| `Ctrl+K` | command palette |
| `/` | focus the screen's find field, where the screen has one (LOG) |
| `Ctrl+Shift+O` | close the console |

**Pointer:** a row is one hit target; clicking anywhere on it focuses it and activates its
control. Hovering previews the row in the detail bar. Hover (fill only) and keyboard focus
(fill + accent tick) stay visually distinct, so both can be on screen without lying about
where input will land.

### 7.3 "Where am I" — four answers, always on

1. **Breadcrumb** — `CONSOLE › GAMESCOPE › Upscaling`.
2. **Rail zone 1** — the section, accent-filled with a 3px accent left edge.
3. **Rail zone 2** — the screen, `elev/2` filled with the accent edge.
4. **Focus tick** — exactly one row on screen has the accent left edge. Never zero, never two.

Plus the detail bar's `writes …`, which answers the *other* "where am I": where does this
edit land.

---

## 8. Theming, restated where it changed

### 8.1 Colour roles

| Role | Value | Change |
|---|---|---|
| `surface/base` | `rgba(8,9,11,.86)` over blur(22) saturate(1.1) | — |
| `elev/1` | white @3.5 % | rail, well, bands (was 3 %) |
| `elev/2` | white @6 % | focused row |
| `elev/box` | white @6 % | control boxes (was 5 %) |
| `line/row` | white @7 % | row dividers (**was 4.5 %**) |
| `line/hair` | white @10 % | console and region borders (was 8.5 %) |
| `line/strong` | white @16 % | off-toggle border, sheet left border |
| `rail/track` | white @22 % | slider groove (**was 16 %**) |
| `text/primary` | `#EFF5FB` @96 % | **was 93 %** |
| `text/label` | `#EFF5FB` @82 % | **was 68 %** |
| `text/meta` | `#EFF5FB` @62 % | **was 46 %** |
| `text/faint` | `#EFF5FB` @44 % | **was 30 %**, and renamed from `disabled` because it no longer means disabled |
| `accent/*` | the existing OKLCH family | unchanged; `accent@30` fills raised to `@55` (segments, chips) and `@62` (toggles) |
| `state/ok · warn` | unchanged | |
| `state/danger` | `oklch(.70 .17 25)` fill, `oklch(.82 .09 25)` text | new in A; the text tint is new here so danger prose clears 4.5:1 |

`state/danger` stays hue-fixed: it must remain red even when the accent hue is red. If the
accent lands within 25° of it, danger shifts `C .17 → .20` and `L .70 → .64`. Four lines in
`UpdateAccentFamily()`.

### 8.2 Typography — six roles, hierarchy by size, not by darkness

This is the load-bearing consequence of §3. Raising `meta` from 46 % to 62 % would flatten
the hierarchy if contrast were the only thing separating the roles. It is not:

| Role | Family / weight | Size | Worst contrast |
|---|---|---|---|
| `Display` | Mono 600 | 1.6 × base | 13.2:1 |
| `Title` | Mono 600, UPPER, +6 % tracking | 0.78 × base | 6.3:1 (meta) |
| `Body` | Sans 400 | 1.0 × base | 10.0:1 (label) |
| `Value` | Mono 500 | 0.90 × base | 9.0:1 (accent-value) |
| `Meta` | Sans 400 | 0.80 × base | 6.3:1 |
| `Micro` | Mono 400 | 0.70 × base | 6.3:1 (never below meta) |

> **Hierarchy comes from size, weight, family and case. Contrast is a floor, not a
> ranking device.** A role is never made quieter by making it harder to read.

### 8.3 Elevation and motion

Unchanged from A. Elevation is white alpha over the blurred base — never a hue, never a
shadow, except the sheet's and the palette's, which must read as being in front of
something. Four motions, five floats, `Approach()`; values never animate.

---

## 9. Screen inventory

| Rail section | Screens (rail zone 2) | Stage | Well |
|---|---|---|---|
| **Gamescope** | Upscaling · Display · Frame Limiter · HDR | List | — |
| **Shaders** | Vibrancy · Pre-Sharpen · Adaptive Brightness | List | A/B strip |
| **System Monitor** | General · FPS · CPU · GPU · Media · Statistics | Split | live HUD preview + 3×3 anchor |
| **Audio** | Output · Streams | List | L/R peak meters |
| **Config** | Per-Game · General · Notifications | List | — |
| **Log** | Gamescope · Game | **Wide** | — |

Six sections, 21 screens, one layout language, zero tab bars.

One finding worth carrying into the product from §3: the HUD's own **backdrop opacity
default moves 0.50 → 0.60**, because at 0.50 its white text measures **3.7:1** over a
white game — a real failure, in the one part of the product that sits on raw game pixels
with no console surface beneath it. At 0.60 it measures 5.21:1, and that is now the
thinnest margin anywhere in the design. The System Monitor's Well is where a user can
actually see this, which is the strongest argument for keeping the live preview.

---

## 10. What changed, in one table

| # | Complaint | Fix | Kind of fix |
|---|---|---|---|
| 1 | tabs look bad | tab bar deleted; screens become the rail's second zone | structural — one nav axis, less ink |
| 2 | controls too small / inconsistent | one 96u column, three fill classes assigned by kind | systemic — width no longer follows content |
| 3 | contrast is bad | every role re-measured; label 68→82 %, meta 46→62 %, toggle fill 30→62 %, groove 16→22 %, disabled row treatment replaced | role-level, with a build-time checker |
| 4 | icons shift | contracted width derived from the icon; reserved boxes; only width and alpha animate | structural — nothing left to interpolate |
| 5 | tooltip reliance | the freed bottom bar becomes an always-on detail line; `Help()` required at registration | reuse — no new furniture |
| — | gamepad | removed entirely | scope |
| — | B as a feature | registry + `Ctrl+K` palette that always lands you back in the rail | additive |
