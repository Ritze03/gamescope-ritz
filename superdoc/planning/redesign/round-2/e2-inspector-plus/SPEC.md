# Direction E2 — "Inspector Rail, deepened" (working name: **Depth on Demand**)

A refinement of Direction E for the gamescope-ritz settings overlay.
Date: 2026-08-23. Design only — no repository code was modified.

Companion files: `API.md` (the helper layer), `FEASIBILITY.md` (ImGui assessment),
`index.html` (self-contained interactive mockup).

---

## 0. The thesis in one paragraph

E's Inspector is the part of the design the user singled out — *"it cleans up the main
screen, while still being able to provide additional information and storing a lot of
settings, without cluttering the UI"* — and the part they prescribed as the cure for
E's own remaining density: *"this type of information shouldn't leave the UI, but
rather wander into the Inspector Rail. This is what it is designed for."* **E2 takes
that instruction literally and to its limit.** Everything that explains, derives,
qualifies, tunes or measures leaves the Sheet. What remains in the Sheet is the
irreducible minimum: *what this setting is called, what it is set to, and how to change
it.* The Sheet becomes a single-height, single-alignment table you can read in
peripheral vision. The Inspector becomes the whole rest of the product — with three
modes, and with a **structural law** that makes it incapable of turning into a junk
drawer.

The one-sentence difference from E: **E's Inspector is a description of the selection;
E2's Inspector is the second half of every row.**

---

## 1. What a Sheet row is allowed to show — the Row Ink Budget

This is the calm rule, and it is subtractive. A Sheet row may contain **at most** these
five things, and there is no sixth:

| Slot | Content | Font / role |
|---|---|---|
| State edge | 2px, at x=0 | `Accent` / `Accent@45%` / nothing |
| Label | one line, ≤ 28 chars (lint-capped) | `Label` Sans 400 14 `TextLabel` |
| Value | *only when the control cannot show its own* | `Value` Mono 500 15 `AccentValue` |
| Control | one control from the taxonomy | per §3 |
| Affordance | exactly one glyph | per §2.4 |

**Five things left the Sheet in E2 that were in E.** Each is named, with where it went:

| Left the Sheet | Was | Now |
|---|---|---|
| `.Hint()` — the one-line hint under a label | promoted rows to `RowTall` 76 | **deleted from the API.** Inspector ▸ Explain. |
| `.Note()` — two lines of prose per group | `TextMeta`, full column width | **deleted from the Sheet API.** Inspector ▸ Explain (it belongs to a row, not a group). |
| Readout runs | 5–9 non-interactive rows (HDR metadata, GPU sensors, `sampling 500 ms · 240-frame window`) | **one `Facts` row** — label + a summary in the value column + a chevron. Inspector ▸ Diagnose. |
| Expert parameter groups | `ui::Depth::Expert`, collapsed group inside the Inspector | **`.Param()`** — a first-class child of its parent row. Inspector ▸ Configure. |
| Units, ranges, defaults, destination files | printed as chips and meta text next to controls | derived from the binding, shown in Inspector ▸ Explain's fact grid. Never printed in the Sheet. |

**The consequence is that there is exactly one Sheet row height: 44.** `RowTall` is
gone. (Composites are not rows; see §4.) That is not aesthetic tidiness — it is what
makes `ImGuiListClipper` exact without a prefix-sum, and it is what makes the label,
value, control and affordance columns four unbroken vertical lines from the top of any
sheet to the bottom.

### 1.1 The measurement

The mockup ships an **E-grammar / E2-grammar** toggle and counts the sheet's text lines
live in the header chip, so this is measured rather than asserted. Same registry, same
settings, both renderings:

| Category | Settings | E grammar | E2 grammar | Params moved to Configure |
|---|---|---|---|---|
| Display ▸ Upscaling | 7 | **20 text lines**, 3 row heights | **7 text lines**, 1 row height | 3 |
| Display ▸ HDR | 4 | **13 text lines**, 3 row heights | **4 text lines**, 1 row height | 3 |
| Image ▸ Shaders | 3 | **8 text lines**, 3 row heights | **3 text lines**, 1 row height | 9 |
| Audio ▸ Mixer | 4 | **9 text lines**, 3 row heights | **4 text lines**, 1 row height | 4 |
| System ▸ Monitor | 13 | **26 text lines**, 3 row heights | **13 text lines**, 1 row height | 6 |

**Text lines is the honest metric** — it is what *"densely packed with (nice to have, but
for noobs overwhelming) information"* actually measures. Across the five categories the
sheet drops from **76 text lines to 31**, a 59% reduction, and **the number of settings is
unchanged**: nothing was deleted, and by §6 nothing became unreachable. In every category
E2 draws exactly one text line per setting — which is the definition of the Row Ink
Budget holding.

---

## 2. The Row grammar — four columns, two of them right-bound

### 2.1 The rule the fix comes from

> **Fix #2 — "control alignment is inconsistent, left / right / centre basically
> random."**

E aligned control *left* edges at `0.44 × W`. That puts a 30px switch in the middle of
a 400px control zone with 370px of nothing to its right, next to a slider that fills
the zone — which is exactly the "basically random" reading. E2 inverts it:

```
 x=0  x=12                                 Lw   Lw+12                    W−28    W
  │▌│  Label text ................ 5      │  ⟨ gap ⟩ ................ [control] │ ⟡ │
   2    left-bound              right-bound            right-bound to W−28      28
        └────── label column ──────┘      └─────── control column ───────┘   afford.
                     Lw = round(0.46 × W)
```

Two hard vertical lines run the whole sheet: the **value right edge at `Lw`** and the
**control right edge at `W − 28`**. Every control's right edge is on the second line —
a 30-wide switch, a 96-wide stepper, a full-bleed slider and a 3×3 anchor grid all end
there.

### 2.2 Why a call site cannot violate it

The row context exposes **one** allocator and it is right-anchored:

```cpp
ImRect RowCtx::Place( float flWidthBase );   // right edge is ALWAYS ctl.Max.x
ImRect RowCtx::PlaceFull();                  // == Place( zone width )
```

There is no `Left()`, no `Centre()`, no `SameLine`, no `SetCursorPosX`. A control
painter receives a rect and draws inside it. Left-alignment is not a discouraged choice;
it is an unrepresentable one. Full-bleed controls (slider, segmented, dropdown, text)
call `PlaceFull()` and therefore satisfy the rule for free.

`Lw` is computed once per column by the shell:
`Lw = clamp( round(0.46 × W), W − 420, W − 200 )`. Content never moves it — two sheets
of the same width have their columns in the same place regardless of the longest label,
which is the property E's spec asked for and E2 keeps.

**Every control's width is a constant in `Controls.cpp`, not a caller's choice.** Widths
are base units and are clamped to the control zone; the *right* edge is invariant:

| Control | `Place(w)` | Why not full-bleed |
|---|---|---|
| Switch | 30 | it is a state, not a range |
| Stepper | 96 | `− 000 +` is its natural width |
| Segmented | ≤ `n × 92` | cells wider than that are just padding |
| Dropdown | ≤ 280 | a long value ellipsises rather than stretching a 470-wide box |
| Text | ≤ 320 | ditto |
| Slider, Meter, Sparkline | `PlaceFull()` | the track *is* the range; it must span the zone |
| Composite body | `PlaceFull()` or its own size | §4 |

Without the caps a 30-wide switch and a 470-wide slider in the same column look like two
different alignment systems even though both are right-bound — the cap is what makes the
right rule *read* as a rule.

### 2.3 The value column

Used **only** when the control cannot display its own value, which the helper decides
from the control kind, never the caller:

| Control kind | Value column |
|---|---|
| Slider, Meter, Sparkline, Composite | **yes** — right-bound at `Lw` |
| Switch, Segmented, Dropdown, Stepper, Text | **no** — the control *is* the readout |
| Facts | **no** — the summary is a *string*, not a value; it sits right-bound in the **control zone**, in `TextMeta`, because opening Diagnose is what that row's control zone does |

Value type is always Mono 500 15 `AccentValue` when the row differs from default, Mono
500 15 `TextPrimary` when it is at default. Unit follows in Mono 400 11.5 `TextMeta`,
outside the number's run.

### 2.4 The affordance column — one glyph, fixed priority

28 base units, and it holds **exactly one** glyph, chosen by this order (first match
wins, never two):

1. `⟡` reset dot — `Accent`, shown when the row differs from default. Click = reset.
2. `›` depth chevron — `TextMeta`, shown when the row owns Params or Facts.
3. `⌷` lock — `TextMeta`, shown when the row is read-only.
4. nothing.

A row that both differs *and* has depth shows the reset dot; the chevron's information
is redundant because the row is selected the moment you click it anyway. Fixed priority
is what keeps the column a column instead of a pile.

### 2.5 Groups and rhythm

- **Group band** — Mono 500 10.5 UPPER `TextMeta`, 16 above / 8 below, no box, no fill,
  no border. Right slot of the band carries at most one thing: a `4 / 7` count for a
  switch set, or `all` / `none` actions, or nothing.
- **Row separator** — 1px `#FFFFFF @ 10%`, no spacing between rows. (Raised from E's 6%;
  see §7.4.)
- **Spacing scale** — `XS 4 · S 8 · M 12 · L 16 · XL 24 · XXL 32`, chosen by the helper,
  never typed by a caller.

---

## 3. The control taxonomy

Ten kinds. E had eighteen; the difference is Checkbox (deleted), Hint-bearing rows
(deleted), Note (moved), readout lists (collapsed into `Facts`), progress (folded into
Meter) and chips (no longer a control — they are painted by the shell, not requested).

### 3.1 Switch — **every** binary in the product

> **Fix #1 — "switches and checkboxes are mixed, looks unprofessional."**

`widgets::Checkbox` is **deleted**, not deprecated. It already has zero callers (#60).
E kept it and justified it with *switch = a setting, checkbox = a member of a set* —
E2 rejects that rule on the grounds that it makes the user learn a distinction in order
to read a screen, in exchange for information they can already see from the grouping.

**One binary affordance in the entire product.** A set of binaries is a group of switch
rows whose band header carries the count:

```
 HUD ROWS                                                    4 / 7   all · none
 Frame rate                                                          ●━━
 Frametime readout (ms)                                              ●━━
 Frametime graph                                                     ━━○
 Percentile row (1% / 0.1% / avg)                                    ●━━
```

Geometry: 30 × 15 track, 11 × 11 knob, right-bound so the knob's right edge is at
`W − 28`. Contrast-corrected off-state (§7.2): track `#FFFFFF @ 10%`, border
`#FFFFFF @ 42%`, knob `#EFF5FB @ 68%`. On: track `Accent @ 30%`, border `Accent @ 75%`,
knob `AccentKnob`.

### 3.2 Segmented — mutually exclusive, ≤ 5 options, ≤ 8 chars each, static set

Equal cells, 4 gap, 26 tall, lowercase Mono. **The helper measures and auto-downgrades
to a dropdown** if any of the three conditions fails; a caller passing six options gets
a dropdown and cannot ship a cramped row. A segmented control sets a value and never
navigates — `BeginTabBar` does not exist in the API.

### 3.3 Dropdown — mutually exclusive, many or dynamic

26 tall box, `PlaceFull()`, value Mono left, chevron right. Popup anchored under the
box, rows 26, max height 280 then clipper.

### 3.4 Slider — bounded continuous

`slider-widget-spec.md` geometry preserved to the pixel, with **one deviation**: the
unfilled rail moves from `kRailAlpha 16%` to **34%**, because 16% measures 1.7:1 against
the sheet and the rail is the part of the control that tells you where the range ends
(§7.3). Marks stay at 38%. Track `PlaceFull()`; the value lives in the value column at
`Lw`; a 1px `#FFFFFF @ 52%` default tick sits on the rail. `Shift` = ×0.1, wheel = one
step, `Ctrl`-click = inline numeric entry.

### 3.5 Stepper — exact or unbounded

96 wide, `Place(96)`, `−`/`+` 20px zones, value Mono 500 15 centred, unit suffix.
`ZeroMeans("Unlimited")` renders the word. `Step()` accelerates after 400 ms.

### 3.6 Text — free text

`PlaceFull()`, 26 tall, caret `Accent`, 1px `Accent` bottom edge when focused,
placeholder `TextMeta` (not `TextFaint` — that role no longer exists, §7.1). Validation
message sits **in the Inspector's Explain body**, not under the box; the box border
turns `Danger` and the affordance column shows nothing (a validation error is not a
state you reset).

### 3.7 Facts — the readout-run collapse

```cpp
s.Facts( "Signal", &HdrSignalSummary, &DrawHdrSignalDiagnose );
```

One 44 row. Label left. A one-line summary (`PQ · 1000 nits · BT.2020`) sits
right-bound in the control zone in `TextMeta`. Affordance: chevron. **No state edge and
no reset dot** — the absence is the structural expression of *"never show a value the
overlay didn't verify"*. Selecting it puts the Inspector in **Diagnose**, where the seven
underlying readouts live.

This single rule is responsible for most of §1.1's line-count drop.

### 3.8 Meter — a live scalar

5px segmented bar (20 segments, 1.5 gaps), `PlaceFull()`, value in the value column.
Same no-hover / no-edge rule as `Facts`. Indeterminate progress is a Meter with a
sweeping lit segment and a `TextMeta` caption; there are no spinners.

### 3.9 Action row — buttons

26 tall, hairline border, Mono lowercase, right-bound, **at most two per row**. Three
intents: `neutral`, `accent` (≤ 1 per group), `danger`. `Danger()` returns a type whose
only method is `Confirm()`, so a destructive button without a confirmation does not
compile.

### 3.10 Composite — a control taller than one row

Its own section, because it is fix #3. See §4.

### 3.11 Disabled, empty, error

- **Disabled** — row × **0.55** (not E's 0.34, which measures 2.6:1 and is unreadable
  over a bright frame; 0.55 measures 3.27:1, §7.2) **plus a mandatory reason string**
  shown in Explain. `.DisabledUnless(bool, const char*)` has no overload without a reason.
- **Empty** — a 96-tall centred band, `TextMeta`, one line, optionally one accent button.
- **Error** — `Danger` border on the control, message in Explain.

---

## 4. Composites — the Anchor fix

> **Fix #3 — "composite controls look orphaned; the named case is Monitor ▸ Placement ▸
> Anchor (the 3×3 grid)."**

### 4.1 Why it looked orphaned

E's Anchor row was a `RowTall` (76) carrying a 3×3 grid *and* two offset steppers side
by side, with the label to their left. Three unrelated shapes on one line, none of them
sharing an edge with anything above or below. It read as a foreign object dropped into
a table — correctly, because it was.

### 4.2 The rule

> **A control taller than one row is not a row. It is a `Composite` — a full-width band
> whose height is exactly `n × 44` base, which participates in the row table's columns,
> and whose body may contain only controls from the taxonomy.**

Four clauses, all mechanical:

1. **Height quantisation.** `n × 44`, `n ∈ {2, 3}`. Nothing else. The clipper's uniform
   step survives (a band is *n* clipper items whose first item paints the whole band).
2. **Line 1 reads as a row.** Label in the label column, **resolved value** in the value
   column, hairline above. Scanning the sheet, a composite is indistinguishable from a
   row until your eye reaches the control column.
3. **The body is right-bound to `W − 28`** and spans lines 1..n. Same vertical line as
   every switch and slider in the sheet.
4. **Nothing else may occupy the band's label column on lines 2..n.** It is air. If
   something wanted to live there, it is a Param (§5.3) — which is exactly how the
   orphaned offset steppers get a home.

### 4.3 Anchor, applied

```
 PLACEMENT
┌──────────────────────────────────────────────────────────────────────────┐
│▌ Placement                          top-right · 32 / 32          ┌──┬──┬──┐│  line 1
│                                                                  ├──┼──┼──┤│  line 2
│                                                                  ├──┼──┼──┤│  line 3
│                                                                  └──┴──┴──┘│
└──────────────────────────────────────────────────────────────────────────┘
   n = 3 · body 96×96 right-bound at W−28 · offsets are Params, in Configure
```

The two offset steppers become `monitor.anchor.margin_v` and `monitor.anchor.margin_h`
— child keys of `monitor.anchor`, satisfying the Prefix Law (§5.2), living in the
Inspector's Configure mode, and rendered inline beneath the band when the Inspector is
hidden (§6). The value column already tells you they are `32 / 32` without showing two
steppers, which is the whole calming move in miniature.

### 4.4 Every composite in the product, same rule

| Composite | n | Body (right-bound) | Line-1 value | Params |
|---|---|---|---|---|
| Anchor / Placement | 3 | 96×96 grid | `top-right · 32 / 32` | `margin_v`, `margin_h` |
| Accent hue | 2 | hue rail + 8 swatches | `oklch(.74 .12 218)` | `l`, `c` (expert) |
| Audio strip | 2 | fader + L/R meter | `−7.5 dB · −12/−14` | `routing`, `match_rule` |
| Frametime graph | 3 | 240-sample sparkline | `7.04 ms · 3 outliers` | — (Diagnose only) |
| Colour override | 2 | L/C/H rails + swatch | `#6ED274` | `hex` |

Five composites, one band rule, no call site choosing geometry. Today's two Position
Grid call sites (notification placement, monitor placement) that drifted apart cannot.

---

## 5. The Inspector — three modes and the law that governs them

### 5.1 Modes

The Inspector's header is a three-cell segmented strip. **The cells are not tabs the
designer fills; they are a readout of what depth this selection actually has.** A cell
with no content is drawn dimmed and is not selectable — so the strip is continuous,
visible proof of how much is (and is not) hiding behind the row.

| Mode | Shown for | Content, all derived |
|---|---|---|
| **EXPLAIN** | every row — always non-empty, `.Help()` is required at registration | help prose (≤ 3 sentences, 240-char lint cap); the derived fact grid `now / default / range / unit / key / writes / applies`; `related` jump links; the keyboard line for this control kind; a reset action |
| **CONFIGURE** | rows that own `.Param()` | the parent's ≤ 6 child parameters, drawn with the same Row grammar, same right-bound rule |
| **DIAGNOSE** | rows that declare `.Live()` — `Facts`, `Meter`, and any entry that opts in | read-only readouts, meters, sparklines, transient live content (a shader's preview, a stream's L/R meter), and **the last 5 captured log lines whose text contains this entry's key** |
| **OVERVIEW** | nothing selected — replaces the strip entirely | §5.5 |

Mode selection is automatic and stateless: arriving via the chevron opens Configure;
selecting a `Facts` or `Meter` row opens Diagnose; everything else opens Explain. The
user can switch modes; the choice is not remembered, because *the Inspector holds no
state* (§5.2 clause 0).

### 5.2 The Attachment Law — the structural anti-junk-drawer rule

E's FEASIBILITY named its own weakest guarantee: *"The Inspector will attract junk …
The Inspector Contract and `ui_lint` are the defences; they are process defences, not
structural ones."* E2 is moving more into the Inspector, so a process defence is not
good enough. This is the replacement, in five clauses, each enforced by a type or an
assert rather than by discipline:

> **0. The Inspector has no authoring API.**
> `ui::Panel`, `PaneCtx`, `Inspect( lambda )` and every other "draw into the inspector"
> hook from E and B are **deleted**. A category file cannot type a single character
> that lands in the Inspector except through the four generators below. The Inspector is
> rendered *by the shell*, as a pure function of the selected entry's registration.
> Same selection ⇒ same Inspector, always, everywhere. It holds no state of its own.
>
> **1. Four generators, and no fifth.**
> `.Help(text)` → Explain prose. Binding metadata (default, range, unit, key,
> destination) → Explain's fact grid, typed by nobody. `.Param(...)` → Configure rows.
> `.Live(fn)` → Diagnose readouts. Adding a fifth generator is a change to
> `Registry.h`, visible in a diff, landing in this table.
>
> **2. The Prefix Law.** A `Param`'s stable id **must** be `<parent id>.<leaf>`.
> Asserted at registration, in every build. `display.sharpness` may own
> `display.sharpness.rcas_denoise`; it may not own `display.hdr_mode`. To hide an
> unrelated setting in the Inspector you must first give it a config key that lies about
> what it belongs to — which is visible in `ui_snapshot`, visible in the on-disk JSON,
> and wrong in a way a reviewer sees without opening the UI.
>
> **3. The Six Budget and the One-Level Rule.** A row may own **at most 6** Params, and
> **a Param may not own Params**. The 7th `.Param()` is a registration abort whose
> message is *"`monitor.modules` has 7 parameters — promote it to a category."* Junk-
> drawer pressure is thereby converted into a structural decision that shows up in the
> rail, not into an invisible accumulation.
>
> **4. Diagnose is read-only by type.** `.Live()` accepts `std::function<Value()>` and
> nothing else. It has no `Bind` overload. A control cannot be *constructed* in Diagnose,
> so "I'll just put this one setting in the diagnostics panel" is not a shortcut a
> reviewer has to catch — it is a compile error.

**Two further consequences worth stating**, because they are what make the law credible
rather than merely strict:

- **Everything in the Inspector is searchable.** Params register like any other entry, so
  `Ctrl+K → "denoise"` finds `display.sharpness.rcas_denoise` and jumps to it — selecting
  the parent row, opening Configure, focusing the param. A setting in the Inspector is
  therefore *one keystroke* from anywhere, which is a stronger reachability guarantee
  than the Sheet gives most rows.
- **The Overview card advertises the count.** Every category card ends with
  `sheet 9 rows · inspector 14 params · 0 unreachable`. If the Inspector ever *did*
  become a junk drawer, the number would say so on the screen you land on.

### 5.3 What Configure looks like

Inspector rows are 40 tall, single column, label left-bound at x=16, control right-bound
at `IW − 16`. Same grammar, same right-bound law, different width. A parameter therefore
looks and behaves identically in the Inspector, in the inline fallback (§6) and in the
Sheet if it is ever promoted — moving one costs one word.

### 5.4 What Diagnose can hold — the transient content question

Diagnose is the one mode allowed to hold content that changes every frame: a shader's
live preview thumbnail, a stream's L/R peak meter, the frametime sparkline, GPU sensors.
It is safe to be permissive here **precisely because clause 4 makes it read-only** — a
region that cannot contain a control cannot contain a hidden setting, so its only failure
mode is being uninteresting.

Two rules keep it cheap: the `.Live()` lambda runs **only for the selected entry** (E's
insight, kept), and Diagnose paints at most one animated element per selection.

### 5.5 Overview — what the Inspector does with nothing selected

Never a placeholder, never empty. The Category Card, extended from E's version:

```
  DISPLAY / UPSCALING
  How the game's image is resampled to the output resolution.

  3 of 11 settings differ from default
    → Filter          fsr        (default linear)
    → Sharpness       5          (default 2)
    → Scaler          integer    (default auto)

  WRITES TO   games/1174180.json
              Override Global Config is on for this game.

  EFFECTIVE PATH
    3840×2160 ← FSR EASU ← 2560×1440 ← game   ·  RCAS 5  ·  2 composite passes

  sheet 9 rows · inspector 14 params · 0 unreachable
  [ reset category ]  [ copy from another game ]  [ save as preset ]
```

This is the screen a user meets on opening the overlay mid-game, and it is the most
useful one in the product: it answers *what state am I in and where is it stored* before
you have clicked anything.

---

## 6. The contract — **amended**, and why

### 6.1 E's Inspector Contract

> *The Sheet alone is a complete UI. Every setting a user needs to change during a normal
> session is reachable and editable in the Sheet. The Inspector adds depth — never
> access.*

with one licensed exception for expert parameters.

### 6.2 Why it cannot survive E2 unchanged, and why the exception was already a hole

E2 moves ~14 parameters per category into Configure. Under E's wording those are
"access", so E2 would violate the contract on every category. But the wording was already
strained in E: E licensed `ui::Depth::Expert` as Inspector-only and defended it with a
claim about *user behaviour* — "these are not settings you change during a session; they
are tuning you do once." That is a promise, not a mechanism, and it is exactly the class
of guarantee E's own feasibility doc admitted was its weakest.

The constraint the contract exists to protect is real and must survive: **at 2.0× on
1920px the Inspector is a drawer that covers 48% of the sheet, and a user may
legitimately persist `ui.inspector = hidden`.** Nothing may become unreachable there.

### 6.3 The amendment — the Reachability Law

> **Every setting in the product is editable with the Inspector closed.**
>
> Depth chooses the Inspector as a *preferred host*, never an *exclusive* one. A row
> that owns Params renders those Params **inline in the Sheet, beneath itself, in the
> Sheet's own Row grammar**, whenever the Inspector is unavailable — collapsed by
> default, expanded with `→` / click / `Space`, and marked with a `▸` disclosure in place
> of the chevron.
>
> Explanation follows the same law: with the Inspector closed, `?` or `Ctrl+/` on a
> selected row opens Explain **as a full-sheet page with a back crumb**, replacing the
> sheet content for as long as you read it.

Three things make this a mechanism rather than a promise:

1. **One code path.** Params render with the Row grammar in either host. The shell picks
   the host from the ladder; the painter does not know which it is in.
2. **It is testable headlessly.** `ui_lint --host=inline` walks the registry, renders
   every category with the Inspector forced off, and asserts that **every registered
   entry — Params included — received a rect**. A setting that became unreachable fails
   the build, not a review.
3. **It is only tractable because of the anti-junk law.** An inline expansion of ≤ 6
   rows, one level deep, is a legitimate sheet element. If depth were arbitrary or
   nested, the inline fallback would be an unusable tree, and the contract would have to
   go back to being a promise. **The Six Budget and the Reachability Law are the same
   rule seen from two ends** — which is the argument for the amendment: it does not
   loosen E's contract, it replaces an unenforceable clause with an enforceable one and
   pays for it with a cap on depth.

### 6.4 The honest cost

Inline expansion **reflows the sheet**, which §8.3 otherwise forbids ("regions never
move"). So inline is explicitly a *degraded* mode: rows below the expansion shift, and
the row you were reading can move under the cursor. We accept it because the alternative
is either an unreachable setting or a modal, and because expansion is user-initiated —
the sheet never reflows on its own.

The capability that is genuinely lost with the Inspector hidden is **simultaneity**: you
can no longer read a setting's help while adjusting it. That is a real loss and it is the
one thing the Inspector column buys that nothing else can.

---

## 7. Colour, type and the measured contrast table

### 7.1 Roles

Four text roles — down from E's five — because **`TextFaint` is deleted**. Its measured
2.52:1 is the direct ancestor of this project's three "too dark to read" complaints, and
there is no content in the product that is worth drawing but not worth reading.

| Role | Value | Used for |
|---|---|---|
| `Surface` | `rgba(9,10,12,.88)` | slab base |
| `SurfaceRail` | `Surface`, darkening +0.06 | rail (recessed, never shows game through it) |
| `SurfaceInspector` | `#FFFFFF @ 3%` over `Surface` | inspector (raised) |
| `SurfaceRaised` | `#FFFFFF @ 6%` | control boxes, inactive segments |
| `Line` | `#FFFFFF @ 10%` | row separators |
| `LineControl` | `#FFFFFF @ 42%` | **every interactive boundary** — switch off-border, box borders, stepper frame |
| `LineRegion` | `#FFFFFF @ 22%` | region boundaries |
| `TextPrimary` | `#EFF5FB @ 92%` | selected labels, hero values, group names |
| `TextBody` | `#EFF5FB @ 72%` | Inspector prose |
| `TextLabel` | `#EFF5FB @ 68%` | parameter labels |
| `TextMeta` | `#EFF5FB @ 52%` | units, marks, line numbers, chips, placeholders |
| `Accent*` | OKLCH family, hue-live | **state only** — active, selected, changed, focused |
| `Ok` | `#6ED274` | liveness dots |
| `Warn` | `#F3821D` fills / `#F7A85C` text | outliers, warn log lines |
| `Danger` | `#EF6B5A` fills / `#F2A99E` text — hue-fixed | destructive actions, errors |

### 7.2 The backdrop model the numbers are measured against

Worst case is a **pure-white game frame**, because it is the lightest thing that can show
through the slab and light text loses contrast against it:

```
game (255,255,255) → darkening 0.80 → (51,51,51)
                   → slab #090A0C at 88% over it
                   → composited sheet background #0E0F11   (L = 0.0067)
rail: darkening 0.86 → #0C0D0F        selected row: Accent@8% over sheet → #111D21
```

Every number below is that composite, measured with the WCAG 2.x relative-luminance
formula. Small text (< 18.66px / 14pt bold) needs **4.5:1**; UI components and large text
need **3:1**.

### 7.3 The measured table

| Element | Composited | Ratio | Floor | |
|---|---|---|---|---|
| `TextPrimary` 92% | `#DDE3E8` | **14.79** | 4.5 | ✅ |
| `TextBody` 72% (Inspector prose) | `#B0B5B9` | **9.25** | 4.5 | ✅ |
| `TextLabel` 68% (row labels) | `#A7ABB0` | **8.34** | 4.5 | ✅ |
| `TextMeta` 52% | `#83878B` | **5.28** | 4.5 | ✅ |
| `TextMeta` 52% on selected row | `#858D92` | **5.11** | 4.5 | ✅ |
| `TextMeta` 52% on `SurfaceRaised` | `#8A8D91` | **5.07** | 4.5 | ✅ |
| `TextMeta` 52% on rail | `#82868A` | **5.28** | 4.5 | ✅ |
| Disabled row (`TextLabel` × 0.55) | `#626568` | **3.27** | 3.0 | ✅ |
| `AccentValue` `#78DBF6` | — | **12.12** | 4.5 | ✅ |
| `AccentText` `#71D4EF` on accent chip | — | **8.72** | 4.5 | ✅ |
| `AccentSeg` `#A9EAFD` on active segment | — | **9.36** | 4.5 | ✅ |
| `AccentBase` `#36BDDD` (state edge, 2px) | — | **8.66** | 3.0 | ✅ |
| Focus ring `Accent @ 85%` | `#30A3BE` | **6.49** | 3.0 | ✅ |
| Switch **off** border `LineControl` 42% | `#737475` | **4.09** vs sheet · **3.19** vs track | 3.0 | ✅ |
| Switch off knob 68% vs off track | `#AEB2B7` | **7.10** | 3.0 | ✅ |
| Switch **on** border `Accent @ 75%` | `#2C91AA` | **5.27** | 3.0 | ✅ |
| Control box border `LineControl` 42% | `#737475` | **4.09** | 3.0 | ✅ |
| Slider rail 34% (raised from E's 16%) | `#606162` | **3.07** | 3.0 | ✅ |
| Slider marks 38% | `#6A6A6B` | **3.55** | 3.0 | ✅ |
| Slider fill `#47CAEA` | — | **9.96** | 3.0 | ✅ |
| `Ok` `#6ED274` | — | **10.17** | 3.0 | ✅ |
| `Warn` `#F3821D` fill · `#F7A85C` text | — | **7.33** · **9.79** | 3.0 · 4.5 | ✅ |
| `Danger` `#EF6B5A` fill · `#F2A99E` text on danger chip | — | **6.32** · **8.46** | 3.0 · 4.5 | ✅ |

**Worst case in the design: 3.07:1** (slider rail, UI floor 3:1).
**Worst case for any text: 5.28:1** (`TextMeta`, floor 4.5:1).

### 7.4 What E fails, and what changed because of it

| E value | Measured | E2 value | Measured |
|---|---|---|---|
| `TextMeta` @ 44% | **4.09 — fails 4.5** | 52% | 5.28 |
| `TextFaint` @ 30% (log line numbers) | **2.52 — fails 4.5** | role deleted; line numbers use `TextMeta` | 5.28 |
| Disabled row × 0.34 | **≈ 2.6 — fails 3.0** | × 0.55 | 3.27 |
| Switch off border 18% | **1.71 — fails 3.0** | 42% | 4.09 |
| Control box border 8% | **1.19 — fails 3.0** | 42% | 4.09 |
| Slider rail `kRailAlpha` 16% | **1.60 — fails 3.0** | 34% | 3.07 |
| Row hairline 6% | 1.16 | 10% | 1.28 |

**Two declared exemptions**, stated rather than glossed: the **row separator** (1.28:1)
and the **region boundary** (1.96:1) are decorative — WCAG 1.4.11 exempts elements not
required to identify a component or its state, and both rows and regions are identified
by their content, their alignment and their background delta. They are the only
sub-3:1 elements in the design and neither carries information.

### 7.5 Hue independence — the accent can rotate 360° without a failure

The accent family fixes L and C per token and varies only H, so contrast is nearly
hue-invariant; gamut clipping is the only risk. Swept at 5° increments across all 360°:

| Token | oklch(L C) | Worst ratio over the sweep | Floor |
|---|---|---|---|
| `AccentValue` | `.841 .100` | **11.09** (h = 20°) | 4.5 ✅ |
| `AccentText` | `.820 .100` | **10.51** (h = 355°) | 4.5 ✅ |
| `AccentSeg` | `.900 .070` | **13.49** (h = 20°) | 4.5 ✅ |
| `AccentBase` | `.741 .120` | **7.90** (h = 355°) | 3.0 ✅ |
| `AccentKnob` | `.859 .080` | **12.05** (h = 20°) | 4.5 ✅ |

The user's hue slider cannot produce an unreadable overlay. `Danger` is deliberately
hue-fixed and excluded from the family — a green "delete permanently" is a bug.

### 7.6 Type — six roles

| Role | Family / weight | Size | Use |
|---|---|---|---|
| `Title` | Mono 600 | 11 UPPER | slab title, region titles |
| `Section` | Mono 500 | 10.5 UPPER | group bands, rail sections, inspector mode strip |
| `Label` | Sans 400 | 14 | row labels, list primaries |
| `Body` | Sans 400 | 14 | Inspector prose only |
| `Value` | Mono 500 | 15 | every numeric or state readout |
| `Meta` | Mono 400 | 11.5 | units, marks, line numbers, chips |

Six, down from E's seven and today's ten. `LabelStrong` is gone: a selected row is
signalled by its state edge, its fill and its label going to `TextPrimary`, which is
three signals already; a weight change was a fourth doing nothing. Numbers are always
Geist Mono, prose always Geist Sans, and the helper picks the font per zone so a caller
cannot put a number in Sans.

### 7.7 Accent budget

Accent is spent on **state and nothing else**: active rail item, selected row, changed
value, active segment, on-switch, slider fill, focus ring, the hairline thread from rail
to breadcrumb. Never a header, never a border that is not communicating state. A category
where nothing has been changed is almost monochrome — and that is the correct look for
"everything is at default".

---

## 8. Navigation, scale and motion

### 8.1 The shell

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ● GAMESCOPE-RITZ                     app 1174180  games/1174180.json  ⌕ ▤ ✕  │ 40
├───────────┬──────────────────────────────────────┬───────────────────────────┤
│           │ DISPLAY / Upscaling   differs 3  ⌕   │ EXPLAIN CONFIGURE DIAGNOSE│ 56
│ RAIL 232  │                                      │  ───────                  │
│           │            SHEET (flex)              │      INSPECTOR 400        │
│ sections  │   44-tall rows · 4 columns · one      │  derived from selection   │
│ + items   │   right-bound control line            │  never authored           │
│           ├──────────────────────────────────────┤                           │
│           │ reset category    ^K search  ^I depth│                           │ 40
└───────────┴──────────────────────────────────────┴───────────────────────────┘
```

Slab `min(surfaceW × 0.90, max(1560 × scale, 1180)) × min(surfaceH × 0.86, 940 × scale)`.
Not draggable, not resizable, no per-panel positions — E's argument stands unchanged, and
it deletes the bug class that produced 39% of `src/Overlay/`'s fix commits.

Rail: eleven items in five sections, icons and labels, two levels, no tab bar anywhere in
the product. Active item carries a 2px `Accent` left edge that survives the icon collapse.

### 8.2 Keyboard — mouse and keyboard only (gamepad dropped)

Dropping gamepad removes the objection E's own feasibility raised against three focus
scopes, and it lets `Tab` mean region and arrows mean movement without compromise.

| Key | Action |
|---|---|
| `Ctrl+Shift+O` | toggle overlay |
| `Tab` / `Shift+Tab` | cycle region: Rail → Sheet → Inspector |
| `↑ ↓` | move selection within the focused region |
| `← →` | inside a control: adjust. At a region edge: cross. On a row with depth: expand / collapse (inline mode) or focus Configure (column mode) |
| `Enter` / `Space` | activate / toggle / begin entry |
| `Ctrl+←/→` | previous / next rail item without leaving the Sheet |
| `Ctrl+K` | **command palette** — every entry *and every Param*, fuzzy over label, key, keywords |
| `Ctrl+D` | reset selected row to default |
| `Ctrl+I` | cycle Inspector host: **column → drawer → hidden** |
| `Ctrl+/` or `?` | Explain the selected row (full-sheet page when the Inspector is hidden) |
| `Esc` | palette → drawer → inline expansion → overlay |

**Selection and editing are the same click.** Clicking a slider both selects the row and
starts the drag; there is no select-then-edit tax.

### 8.3 The responsive ladder, and the 2.0× arithmetic

Base budget: rail 232 (icons 60), inspector 400 (wide 544), sheet minimum 560,
column minimum 420. Slab width on 1920 = `min(surfaceW × 0.90, max(1560 × scale, 1180))`.
The ladder is one comparison applied twice: `rail + inspector + sheetMin ≥ slabBase`
collapses the rail; if it still holds with the icon rail, the inspector becomes a drawer.

| Scale | Slab px | Slab base | Rail | Inspector | Sheet base | Step | Result |
|---|---|---|---|---|---|---|---|
| **0.5×** | 1180 | 2360 | 232 | 400 | 1728 | **−1** | up to three columns — capped by content |
| **0.75×** | 1180 | 1573 | 232 | 400 | 941 | 0 | two columns |
| **1.0×** | 1560 | 1560 | 232 | 400 | 928 | 0 | two columns |
| **1.25×** | 1728 | 1382 | 232 | 400 | 750 | 0 | one column |
| **1.5×** | 1728 | 1152 | **60** | 400 | 692 | **1** | icon rail, all three regions |
| **1.75×** | 1728 | 988 | 60 | drawer 400 | 928 | **2** | icon rail, inspector overlays |
| **2.0×** | 1728 | **864** | 60 | drawer 400 | **804** | **2** | icon rail, inspector overlays |
| any | — | — | 60/232 | **hidden** | +400 | **3** | **inline depth** — user choice, `Ctrl+I` |

Two things this table is meant to prove:

- **2.0× gives the sheet 804 base units — more than 1.25× gives it (750)** — because
  collapsing the rail and floating the inspector returns more space than the scale-up
  consumes. The direction that looks most width-hungry has the most usable content
  column at the extreme.
- **At 804 base with one column**: `Lw = clamp(round(0.46 × 804), 384, 604) = 370`.
  Label column 358, control column 394, affordance 28. A 44-tall row at 2.0× is 88
  physical pixels with a 28px label and a 394-base control zone. Nothing wraps, nothing
  goes two-line, and E's `W < 300` two-line escape hatch is never reached — so E2 has
  **one row layout mode**, not three.

Step 3 is deliberately reachable by *choice* and persisted, not only by width. That is
what keeps the Reachability Law exercised daily instead of only on a 2.0× machine.

**Columns are capped by content, not only by width:**
`columns = min( widthAllows, ceil( rows / 12 ) )`. A 7-row category never spreads into
two columns just because it fits — half-empty columns were the worst thing about E's
dense sheets, and this is a one-line rule that removes them. It is also why the segmented
control auto-downgrades on a 13-row category (narrower columns) and not on a 7-row one:
the helper measures the column it actually got.

**Every hairline is `max(1, floor(1 × scale))`** — 1px from 0.5× to 1.99×, 2px at 2.0×,
so a rule never disappears at 0.5× nor thickens into a border at 2.0×.

### 8.4 Motion

Three durations, one easing (`1 − (1 − t)³`), lerped against `io.DeltaTime`:

- **90 ms — state.** Hover, switch knob travel, segment fill, focus ring.
- **160 ms — region.** Inspector host change, rail collapse, sheet cross-fade on category
  change, inline expansion.
- **240 ms — surface.** Overlay open/close: slab 0.98 → 1.00 with alpha 0 → 1.

Two prohibitions: **values never animate** (a slider snapping to a typed or reset value —
animated values over a running game read as input lag), and **regions never move** except
when opening or closing. The single licensed reflow is user-initiated inline expansion
(§6.4).

---

## 9. What this replaces

| Today / in E | In E2 |
|---|---|
| Bottom dock + 6 floating windows, drag / collapse / tile / measure-and-grow | One fixed slab; ~900 LOC of `Chrome.cpp` deleted |
| 4 tab bars | Rail items; **no `BeginTabBar` in the API** |
| `widgets::Checkbox` and `widgets::Toggle` used interchangeably | **Checkbox deleted.** One switch, product-wide |
| Controls aligned left / right / centre per call site | One right-bound allocator; left alignment is unrepresentable |
| Anchor grid + steppers crammed on one tall row | `Composite` band, `n × 44`; offsets are Params |
| E's `RowTall` 76, two-line mode, inspector 64 | **One row height: 44** |
| E's `.Hint()`, `.Note()`, readout runs, expert groups | Explain / Configure / Diagnose |
| E's `ui::Panel` inspector authoring API, B's `PaneCtx` | **Deleted.** Four generators, Prefix Law, Six Budget |
| E's Inspector Contract (a promise) | The **Reachability Law** (a rendering fallback, headlessly testable) |
| `TextMeta` 44%, `TextFaint` 30%, disabled × 0.34 | 52%, role deleted, × 0.55 — nothing below 3.07:1 |

Kept verbatim because they were measured and liked: the **slider** (with one contrast
deviation, §3.4), the **toggle** geometry, the **segmented control**, the **3×3 position
grid**, and the **OKLCH accent family** with its live hue.
