# Direction E2 — "Inspector Rail, deepened" (working name: **Depth on Demand**)

A refinement of Direction E for the gamescope-ritz settings overlay.
Date: 2026-08-23. Design only — no repository code was modified.

Companion files: `API.md` (the helper layer), `FEASIBILITY.md` (ImGui assessment),
`index.html` (self-contained interactive mockup).

---

## Amendments

### 2026-08-24 — second type raise: `Meta` is the Log/Changelog body, and it moves most

Direct user feedback, one round after the raise recorded below: *"Make the Log and
Changelog font 1-2px bigger. In fact, we should increase all of the smaller font sizes by
1-2px."* Same discipline, same place — `src/Overlay/UI/Tokens.cpp` only; §7.6 below stays
the original measured spec.

**Why one raise wasn't enough.** The first pass deliberately froze `Meta` on the theory
that a quiet auxiliary tier (units, marks, chips) must not out-rank the labels it
annotates. That is true of `Meta`'s auxiliary job — but `Meta` is *also the entire body
text of both surfaces the user just named*. `Shell.cpp`'s `DrawContentBody()` draws every
Log and Changelog line — number, timestamp, scope tag **and the message itself** — in
`TypeRole::Meta`. So the one role the first pass held still was precisely the role the
complaint was about. There is no separate mono body role and no hardcoded size at either
call site; P1's "no magic numbers at call sites" guarantee holds, and the fix is entirely
a token change.

| Role | Was | Now | Delta | Gap to role above |
|---|---|---|---|---|
| `Meta` | 11.5 | **13.0** | +1.5 | — (the floor) |
| `Section` | 12.0 | **13.5** | +1.5 | `Meta`→`Section` 0.5 |
| `Title` | 13.0 | **14.5** | +1.5 | `Section`→`Title` 1.0 |
| `Label` | 15.0 | **16.0** | +1.0 | `Title`→`Label` 1.5 |
| `Body` | 15.0 | **16.0** | +1.0 | tied to `Label` |
| `Value` | 16.0 | **16.5** | +0.5 | `Label`→`Value` 0.5 |

**Why tapered rather than a flat +1.5.** The request is for the *small* sizes, and a flat
raise is "just adjust the scale" in another guise (the move issue #23 rejected) — it
inflates the whole slab to fix the bottom of it. Tapering keeps the top of the ladder
nearly still, which is what keeps `kRowH` (44) and `kControlH` (28) valid at 2.0x without
a second geometry change, while the floor moves the full 1.5. `Value` moves at all only
because `Label` passing it would invert the ladder; +0.5 is the least that keeps `Value`
on top.

Ascending order preserved, no collisions: `Meta` 13.0 < `Section` 13.5 < `Title` 14.5 <
`Label`/`Body` 16.0 < `Value` 16.5. Every adjacent gap is ≥ 0.5 base units — the floor the
first pass set. Both 0.5 gaps sit across a register boundary, so neither carries the
distinction on size alone: `Meta`→`Section` is lowercase → UPPERCASE with 0.10em tracking,
and `Label`→`Value` is Sans → Mono.

Consequences checked:

- **Row geometry untouched.** `kRowH` (44) and `kControlH` (28) still clear every size with
  headroom; `shelltok::kSectionLine` (20) and `kTitleLine` (24) still clear `Section` 13.5
  and `Title` 14.5. Nothing had to grow. The Log's own line height was already derived
  (`MeasureText(Meta,"Xg").y + 3`), so it followed the token with no edit.
- **0.5x / 1.0x / 2.0x** — before/after pairs on the Log, the Changelog and the System
  Monitor. At 0.5x the Log's line-number and timestamp columns still fit (both are measured
  from their widest possible value, so they cannot clip). At 2.0x nothing overflows its
  lane or its row.
- **Contrast** — no colour token changed, and all six sizes remain under the WCAG
  large-text threshold (18.66px normal / 14px bold) at 1.0x, so the 4.5:1 small-text floor
  still applies to all of them rather than the relaxed 3:1; raising a size only widens the
  margin. The two tightest cases on record are unchanged and still clear it: `TextMeta`
  (52%, **5.28:1**) — which is what the Log body is drawn in, so the surface that got
  bigger is the one whose margin matters most — and `TextSegInactive` (50%, **4.96:1**).
- **Crispness at 2.0x** — unaffected. The atlas bakes a fresh `ImFontBaked` per exact pixel
  size (#38) and its rebuild stays deferred past the requesting frame (#51); neither path
  was touched.

**Shipped with it: ellipsis at the shell's clip point** (conformance-audit divergence 10).
`Controls.cpp`'s `DrawText()` was the single place every label, value and log line is
clipped, and it clipped hard — the Log Inspector's Buffer facts row read `51 lines · 2 er:`
in the narrow lane, mid-word truncation the user has objected to before (#46). Raising the
ladder makes more strings overflow more often, so the marker had to land in the same
change. It now truncates to the last glyph that fits and appends `...` (three ASCII
periods, not U+2026 — `Fonts.cpp` bakes Basic Latin + Latin-1 only and the bundled Geist
faces carry no ellipsis glyph, the same constraint behind D18's drawn chevron). Buffer now
reads `31 lines · ...`.

One trap worth recording: several rects in the kit are sized *from* the same measurement
they later clip (`RowCtx::SplitLabelZone()` builds the value rect as `Lw - measured .. Lw`,
and `a - (a - b)` is not exactly `b` in float at screen-sized coordinates). A strict
overflow test therefore fired on a ~6e-5 px difference and turned the Monitor's `18 px`
into `1...`. `DrawText()` now requires **one physical pixel** of overflow before it
truncates; below that, clipping is invisible and a marker would be a lie.

See `src/Overlay/UI/Tokens.cpp`'s `Type()` and `Controls.cpp`'s `DrawText()` for the
rationale inline at the value, and `AUTONOMOUS-DECISIONS.md` (D27) for the decision record.

### 2026-08-24 — shipped type scale departs from §7.6 at the small end

> **Superseded by the block above (same day, second round of feedback).** Every size in
> this entry's table has since moved again. Kept for the reasoning, not the numbers — in
> particular, the "`Meta` unchanged" call recorded here is the one the next pass had to
> reverse, and the block above explains why.

Direct user feedback: *"All of the descriptive fonts (control elements names/labels) and
category labels are really small (including titles and such). So we probably need to
increase the smallest of the used fonts by 1-2px."* Applied in `src/Overlay/UI/Tokens.cpp`
only — this file's §7.6 table below is left as the original measured spec, and the
departure is recorded here and at the value, the same discipline issue #23 set for the
control-height baseline (see that entry's note in `superdoc/planning/ui-mockup-precise-spec.md`):
raise the origin constants themselves, don't paper over it with a runtime multiplier, and
write down why so a later pass doesn't "correct" it back.

The complaint named three roles: row/control labels, category (group/section) labels, and
titles. Those three move; `Value` (already the largest, unnamed) and `Meta` (the
deliberately-quietest auxiliary tier — units, marks, chips — unnamed) do not:

| Role | §7.6 spec | Shipped | Delta |
|---|---|---|---|
| `Title` | 11 | **13** | +2.0 (+18%) |
| `Section` | 10.5 | **12** | +1.5 (+14%) |
| `Label` | 14 | **15** | +1.0 (+7%) |
| `Body` | 14 | **15** | +1.0 (tied to `Label`, not independently named — see below) |
| `Value` | 15 (16 shipped, §7.6's own mockup-tiebreak note) | 16 | unchanged |
| `Meta` | 11.5 | 11.5 | unchanged |

`Body` is not named by the complaint but was kept in lockstep with `Label`: the two have
always shared one literal (Sans 400 14) because row labels and Inspector prose read as the
same register, and moving one without the other would newly mismatch two things the table
has always tied together.

New ascending order: `Meta` 11.5 < `Section` 12 < `Title` 13 < `Label`/`Body` 15 < `Value`
16 — `Meta` becomes the numeric floor (it was third-smallest before), which is the right
outcome: its job is to read quieter than the labels and titles a user reads for
navigation, not to out-rank them in size. Every adjacent gap is >= 0.5 base units, and
`Section`→`Title` itself widened from 0.5 to 1.0, so raising the small end did not
collapse it into the next role up.

Consequences checked, not just asserted:

- **Row/control geometry is untouched.** `kRowH` (44) and `kControlH` (28) both clear
  every new size with more headroom than before; nothing needed to grow to fit the larger
  type.
- **0.5x and 2.0x** — before/after screenshots on `system.monitor` (dense: row labels,
  group labels, a breadcrumb title, and the Inspector) confirm the increase reads at the
  smallest absolute size (0.5x) and that nothing overflows its row or lane at the largest
  (2.0x).
- **Contrast** — every role's colour token is unchanged; all six sizes stay well under the
  WCAG large-text threshold, so the 4.5:1 small-text floor still applies to all of them
  (never the relaxed 3:1) and raising a size only widens that margin. The worst text case
  on record, `TextSegInactive` at 4.96:1 (§7.3, this table's `Section` role in its inactive
  segmented-cell form), is unaffected and still clears the floor.
- **Crispness at 2.0x** — unaffected: the atlas bakes a fresh `ImFontBaked` per exact pixel
  size (`Fonts.cpp`, #38), so a new `flSizeBase` gets baked crisp on first use rather than
  resampled from a fixed bake.

See `src/Overlay/UI/Tokens.cpp`'s `Type()` for the full rationale inline at the value, and
`superdoc/planning/redesign/AUTONOMOUS-DECISIONS.md` (2026-08-24) for the decision record.

### 2026-08-23 — user critique, second pass

Applied to this document and to `index.html` together. Sections below have been rewritten
in place; this block is the summary of *what changed and why*.

1. **The Inspector has two modes, not three.** `EXPLAIN / CONFIGURE / DIAGNOSE` became
   **`CONFIGURE / DETAILS`**. Configure is *what this setting does*, in one short
   paragraph, followed by every value you can set — the row's own control first, then its
   parameters. Details is everything technical the shell can derive. The structural rule
   is unchanged and is the whole point: **the Inspector still has no authoring API.** Two
   generators instead of three; not two panels anyone writes into. See §5.
2. **The thread is deleted.** E's signature line from the active rail item into the sheet
   header is gone; the rail's own selected state and the breadcrumb already answer "where
   am I", and nothing else depended on it.
3. **The rail carries drawn icons.** Single monospace glyphs at 14px became an inline SVG
   set on one 24-unit grid, 1.7 stroke, miter joins, fill used only where a fill carries
   meaning. Legible from 0.5× to 2.0×; no icon font, no external asset.
4. **The hidden Inspector adopts E1's visual** — a 20-base named vertical spine on the
   right edge reading `inspector ›`, which restores the region on click (also `Ctrl+I`).
   The sheet lane is reduced by 20 so nothing is ever covered.
5. **The accent is one token.** Every accent-derived colour in the mockup is
   `rgba(var(--accRGB), α)`; the only accent literals left are the eight token
   declarations, which the hue slider overwrites. One hue change repaints everything.
6. **The controls are direction B's**, reproduced from B's own stylesheet and reconciled
   with E2's lane rule. See §3, and §3.0 for the one control-height system they imply.
7. **A control-height system, because the switch was too small.** `--H = 28` base units is
   *the* control height and every control's hit box honours it. B's geometry is uplifted
   ~25%, the same uplift shipping issue #23 applied to the handoff baseline at this user's
   request. The switch is now **40 × 20 with a 16 knob**. See §3.0.
8. **`INCONSISTENCIES.md`** records the full audit — 30 fixes and 5 open questions. The
   items that changed rules stated *here* are C1–C8 in that file.

Later the same day, the five questions item 8 left open (`INCONSISTENCIES.md` §F) were
decided in `AUTONOMOUS-DECISIONS.md` (D5–D9) and applied to this document and to
`index.html`:

9. **D5 — area ids stay; they're UI grouping only.** No config key renamed. The palette's
   path column now shows the config key instead of the area id/title. See §2.6.
10. **D6 — one "differs" encoding on the sheet; reset moves to the Inspector.** Dropped the
    value-column accent recolour; kept the 2px edge; the affordance column now leads with
    the depth chevron instead of a reset dot that displaced it. See §2.3, §2.4.
11. **D7 — the chip bank stays**, unchanged, as the taxonomy's eleventh kind. See §3.12.
12. **D8 — `IMAGE` folds into `DISPLAY`, `AUDIO` folds into `SYSTEM`.** Five rail sections
    become three. See §8.1.
13. **D9 — the sheet header's dev chips are stated, explicitly, as mockup instrumentation
    that must not ship.** See §1.1.

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
peripheral vision. The Inspector becomes the whole rest of the product — with two
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
| `.Hint()` — the one-line hint under a label | promoted rows to `RowTall` 76 | **deleted from the API.** Inspector ▸ Configure. |
| `.Note()` — two lines of prose per group | `TextMeta`, full column width | **deleted from the Sheet API.** Inspector ▸ Configure (it belongs to a row, not a group). |
| Readout runs | 5–9 non-interactive rows (HDR metadata, GPU sensors, `sampling 500 ms · 240-frame window`) | **one `Facts` row** — label + a summary in the value column + a chevron. Inspector ▸ Details. |
| Expert parameter groups | `ui::Depth::Expert`, collapsed group inside the Inspector | **`.Param()`** — a first-class child of its parent row. Inspector ▸ Configure. |
| Units, ranges, defaults, destination files | printed as chips and meta text next to controls | derived from the binding, shown in Inspector ▸ Details' binding grid. Never printed in the Sheet. |

**The consequence is that there is exactly one Sheet row height: 44.** `RowTall` is
gone. (Composites are not rows; see §4.) That is not aesthetic tidiness — it is what
makes `ImGuiListClipper` exact without a prefix-sum, and it is what makes the label,
value, control and affordance columns four unbroken vertical lines from the top of any
sheet to the bottom.

### 1.1 The measurement

The mockup ships an **E-grammar / E2-grammar** toggle and counts the sheet's text lines
live in the header chip, so this is measured rather than asserted. Same registry, same
settings, both renderings:

> **Stated 2026-08-23 (D9, decided in `AUTONOMOUS-DECISIONS.md`; raised as an open
> question in `INCONSISTENCIES.md` §F.5).** The sheet header's `text lines`, `row
> heights`, `col`, `ladder` and `sheet 804b` chips, and the E-grammar/E2-grammar toggle
> itself, are **mockup instrumentation, not product chrome.** They exist to make this
> table's numbers measured rather than asserted, and they stay in `index.html` for
> exactly that reason. **They must not be implemented in the shipping overlay.** A later
> reader porting this mockup to ImGui must not treat it as pixel-exact here: the sheet
> header in the real product carries the breadcrumb, the `differs N` chip, and the
> `inspector hidden` chip — never a live line-count, a row-height count, a column count,
> a ladder-step number, or a raw pixel-budget readout. Treat this paragraph as the
> checklist item: the implementation PR that ports the sheet header is not done until it
> has confirmed none of these five chips crossed over.

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

> **Amended 2026-08-27.** `Lw` is a **floor** for the value's right edge, not always its
> exact position. Switch, Stepper and a Composite's own fixed-width body (the Anchor
> grid) are narrow, right-bound atoms that end well inside the control zone, short of
> `W − 28`. Anchoring their value at `Lw` — as every other value-bearing kind does,
> because a full-bleed control's left edge *is* `Lw` — stranded the value in the row's
> middle, far from the control it describes. The value's anchor is now
> `max( controlLeft − gutter, Lw )`, where `controlLeft` is that row's own control rect
> (from `RowCtx::Place`/`PlaceFull`). This still satisfies "content never moves it": the
> anchor is a function of the **control kind's** fixed width (a constant in
> `Controls.cpp`/`Tokens.h` for Switch, Stepper and the Anchor grid, per the table
> above), never of the value text's length, so two sheets of the same width still put
> every row's value in the same place regardless of the longest label or the longest
> value string. What
> changed is that "the same place" is no longer a single column for every row kind —
> Switch/Stepper/Anchor-grid rows get their own, control-relative column, one gutter
> left of their control, while Slider/Meter/Composite-with-a-full-bleed-body rows keep
> `Lw` exactly as before (their `controlLeft` already equals `Lw`, so the `max()` is a
> no-op for them).
>
> Why: Slider and Meter fill the whole control zone, so their control's left edge
> coincides with `Lw` and the value looked correct at `Lw` by accident, not by a general
> rule. Switch, Stepper and the Anchor grid are right-bound but fixed-width, so `Lw` and
> their control's left edge are two different points — the gap between them is exactly
> the dead space the value used to sit stranded in. Hue, Strip, Graph and Color
> composites use a full-bleed body and were already correct under the old, single-`Lw`
> rule. Vertical placement is unchanged: the value still sits on line 1, same as before.
> See `CHANGELOG.md`'s `[0.3.5] – 2026-08-27` Fixed entry for the user-facing
> description; the anchor itself is `ValueAnchorPx()` and `RowCtx::SplitLabelZone()`'s
> two-argument overload in `src/Overlay/UI/Shell.cpp` and `src/Overlay/UI/Row.cpp`.

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
| Switch, Slider, Stepper, Meter, Sparkline, Composite | **yes** — right-bound at `Lw` |
| Segmented, Dropdown, Text | **no** — these display their own value inside the control and must never duplicate it |
| Facts | **no** — the summary is a *string*, not a value; it sits right-bound in the **control zone**, in `TextMeta`, because opening Details is what that row's control zone does |

> **Amended 2026-08-23.** The first version of this table put Switch and Stepper in the
> "no" column on the grounds that *the control is the readout*. Both were wrong. A
> stepper drawn to direction B's design is two glyphs, `−` and `+`, and carries no number
> at all; and a switch that shows nothing leaves the design's own thesis — *what it is
> called, **what it is set to**, and how to change it* — unmet on the most common row in
> the product. A switch now reads `on` / `off` in the value column, which is also what
> B's `.state` atom does.

Value type is Mono 500 **16** `TextPrimary`, on the Sheet, **always** — B's `.val` size,
re-based onto a token that clears 4.5:1 (B's own `neutral` at 44% measures 4.09:1). Unit
follows in Mono 400 11.5 `TextMeta`, outside the number's run. The value column ellipsizes
at 60% of the label+value zone, so a 40-character option name cannot squeeze the label to
nothing.

> **Amended 2026-08-23 (D6).** This cell previously read `AccentValue` when the row
> differs from default. That accent recolour is **deleted** — see §2.4's amendment for
> why. The Inspector's own value line (§5.3) still recolours on diff; only the Sheet's
> value column stopped.

### 2.4 The affordance column — one glyph, fixed priority

28 base units, and it holds **at most one** glyph, chosen by this order (first match
wins, never two):

1. `›` depth chevron — `TextMeta`, shown when the row owns Params or Facts.
2. `⌷` lock — `TextMeta`, shown when the row is read-only.
3. nothing.

> **Amended 2026-08-23 (D6 — the most consequential of the five open questions in
> `INCONSISTENCIES.md` §F, decided in `AUTONOMOUS-DECISIONS.md`).** This column
> previously led with a `⟡` reset dot for a differing row, ahead of the chevron. That was
> a real bug, not a style choice: a row that both **differed** *and* **owned depth** — the
> exact combination the affordance column exists to advertise — showed the dot and lost
> the chevron, so its Params became invisible from the Sheet. Three encodings of "differs
> from default" coexisted (edge, value colour, reset dot); this drops the design to
> **one** — the 2px accent left edge (§2.1), unchanged — and **reset moves into the
> Inspector**: the selected row's own Configure line carries a reset dot next to its
> value, and the Configure body's action row carries a `reset to default` verb: both
> already existed there (§5.3) and needed no new code, only removal from the Sheet.
> `Ctrl+D` still resets the selected row (and its Params) with the Inspector closed, so
> the Reachability Law is untouched — resetting was never exclusive to a dot in the Sheet.
> The one real cost: reset now takes selecting the row first, one extra click for a rare,
> destructive-ish action, paid to keep depth always visible. Reset is deliberately
> **Configure-only, not Details** — Details is read-only by type (§5.2 clause 4, a pure
> derived readout with no authoring surface of any kind, including a reset action), so
> putting a mutating verb there would be the one inconsistency the mode split forbids.
> The same rule now applies uniformly to Params rendered inline in the Sheet (§6.3): they
> gained the accent edge they never had, and lost the reset dot they did have, so a
> Param-in-Sheet reads exactly like a top-level row.
>
> **Found while verifying this decision, and fixed:** the Inspector's own reset dot
> (`.irow .rst`) was unclickable on a **wide** row (slider, text, bank, hue, strip,
> anchor) — a stale `position:absolute` rule aimed it at the whole two-line row instead of
> the value line it sits on, landing it under the control. Invisible before D6, because
> the Sheet's own dot was always a working fallback; load-bearing now that it is the only
> route. Fixed by scoping the absolute rule to single-line rows (`.irow:not(.wide) .rst`);
> a wide row's dot now sits in normal flow next to its value, as designed. See
> `TEST-REPORT.md`'s 2026-08-23 re-verification for how this was found and confirmed
> fixed.

### 2.5 Groups and rhythm

- **Group band** — Mono 500 10.5 UPPER `TextMeta`, 16 above / 8 below, no box, no fill,
  no border. Right slot of the band carries at most one thing: a `4 / 7` count for a
  switch set, or `all` / `none` actions, or nothing.
- **Row separator** — 1px `#FFFFFF @ 10%`, no spacing between rows. (Raised from E's 6%;
  see §7.4.)
- **Spacing scale** — `XS 4 · S 8 · M 12 · L 16 · XL 24 · XXL 32`, chosen by the helper,
  never typed by a caller.

### 2.6 Area ids vs. config keys — a stated non-promise

> **Added 2026-08-23 (D5, decided in `AUTONOMOUS-DECISIONS.md`; the question was raised
> in `INCONSISTENCIES.md` §F.1).** A rail area's id (`system.monitor`, `setup.pergame`,
> `setup.appearance`, `setup.profiles`, …) is a **UI grouping label**, chosen for where a
> category sits on the rail. It carries **no promise** that the settings inside share that
> prefix as their config key. Four of eleven areas don't: `system.monitor` holds
> `monitor.*` keys, `setup.pergame` holds `config.*`, `setup.appearance` holds
> `overlay.*`, `setup.profiles` holds `profiles.*`. This is deliberate, not drift — the
> config keys are on disk in the user's `global.json` and `games/*.json`, and renaming
> them to match the rail would be a migration for a cosmetic mismatch. **The Prefix Law
> (§5.2 clause 2) applies to Params, not to areas** — a Param's id must be
> `<parent id>.<leaf>`, and that rule is unaffected and unrelaxed.
>
> The one place the mismatch was visible — the command palette's path column, which
> showed `AREA-SECTION / Area Title` — now shows the **config key** instead (`e.id` for a
> setting, `p.id` for a Param; see §5.2's searchability guarantee). The key is what a
> reviewer checks against the on-disk JSON, so it is the more useful thing to show there
> regardless of the area-id question.

---

## 3. The control taxonomy

Eleven kinds. E had eighteen; the difference is Checkbox (deleted), Hint-bearing rows
(deleted), Note (moved), readout lists (collapsed into `Facts`), progress (folded into
Meter) and chips (no longer a control — they are painted by the shell, not requested).
The eleventh is `Bank` (§3.12), adopted from direction B.

**Every control below is direction B's**, measured from B's own stylesheet, uplifted
~25% per §3.0, and reconciled with E2's lane rule. Where B and E2 genuinely conflict,
E2's layout law wins and the conflict is named in the control's entry.

### 3.0 One control height — the system every control honours

> **Fix — "switches are really small, for example."**

The first version of this design had five control heights: 26 for segmented / dropdown /
stepper / text, 30 × 15 for the switch, 22 for the slider hit box, 5 for the meter and 30
for the anchor grid's cells. Nothing agreed with anything, and the switch — the most
common control in the product — was the smallest thing on screen.

```
  --H   = 28   THE control height.  Every control's HIT BOX is exactly --H tall.
  --swW = 40   switch track width   (B's 30 x 1.25 = 37.5, snapped to the 4u grid)
  --swH = 20   switch track height  (B's 15 x 1.25 = 18.75 -> 20, keeps B's 2:1)
  --swK = 16   switch knob          (B's 11 x 1.25 = 13.75 -> 16 = swH - 4)
  --swT = 20   knob travel          (swW - swK - 4)
  --trk =  8   slider / hue / meter track  (B's 6 x 1.25 = 7.5 -> 8)
  --hndW=  8   slider handle width  (B's 6 x 1.25)
  --hndH= 20   slider handle height (B's 14 x 1.25 = 17.5 -> 20)
  --row = 44   THE sheet row height.  There is exactly one, everywhere, including
               the Inspector (see the C1 amendment in INCONSISTENCIES.md).
```

**The rule, stated so it can be checked mechanically:** *every control occupies an
`--H`-tall hit box. A control with a box border fills it; a control whose graphic is
deliberately shorter — switch, slider, meter — is centred in it.* A lint pass can assert
this by measuring rendered hit boxes; it needs no judgement.

**Why 25%, and why not 1:1 with B.** Shipping issue #23 raised every font and control
constant in `src/Overlay/` 20–25% above the pixel-measured handoff baseline, at this
user's explicit request to make fonts and controls "a little bigger", by raising the
origin constants rather than layering a multiplier. `ui-mockup-precise-spec.md` records
that departure and instructs: *"if this file is ever regenerated from a fresh mockup
measurement, re-apply the same ~20-25% uplift."* B's control table **is** that handoff
baseline (B's switch is exactly the handoff's 30 × 15 / 11). So the uplift is re-applied
here rather than shipping a mockup the code would immediately contradict.

**Why the switch is 20 tall and not 28.** A 28-tall switch at B's 2:1 aspect is 56 wide,
which dominates a 44-tall row. The switch reads at the same optical weight as its
neighbours at 40 × 20 in a 28 hit box — 71% of the control height, the same proportion a
slider's 8 track holds. What matters for consistency is that the *hit box* agrees, and it
does; at 0.5× the switch is still a 20 × 14 physical target, and at 2.0× it is 80 × 40 and
does not look clumsy.

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

Geometry (§3.0): **40 × 20 track, 16 × 16 knob, 20 travel**, in a 28-tall hit box,
right-bound so the track's right edge is at `W − 28`. Colours are B's verbatim except the
off-state border: on — track `Accent @ 30%`, border `Accent @ 65%`, knob `AccentKnob`;
off — track `#FFFFFF @ 7%`, border `#FFFFFF @ 42%`, knob `#EFF5FB @ 55%`.

> **Deviation from B.** B's off border is `#FFFFFF @ 18%`, which measures **1.69:1**
> against the worst-case background — the boundary of an interactive control, below the
> 3:1 floor. It is raised to `--lineCtl` (42%, 4.09:1). Every other value is B's.

The value column reads `on` / `off` (§2.3), which is B's `.state` atom relocated.

### 3.2 Segmented — mutually exclusive, ≤ 5 options, ≤ 8 chars each, static set

B's segmented, verbatim: **content-sized cells** (not equal-width), 3 gap, `--H` tall,
9 horizontal padding, lowercase Mono 500 11.5. Inactive: fill `#FFFFFF @ 4%`, border
`#FFFFFF @ 42%`, text `#EFF5FB @ 50%`. Active: fill `Accent @ 24%`, border `Accent @ 60%`,
text Mono **600** `AccentSeg`. The group is right-bound; its cells are not stretched to
fill the lane, because B does not stretch them and a stretched cell set reads as a tab bar.

> **Deviation from B.** B's inactive border is `#FFFFFF @ 8%` = **1.21:1**. Raised to
> `--lineCtl`. Everything else — sizes, weights, the 50% inactive text (4.96:1) — is B's.

**The helper measures and auto-downgrades to a dropdown** if any of the three conditions
fails *or if the measured group does not fit the lane*; a caller passing six options gets
a dropdown and cannot ship a cramped row. **One helper for both hosts**: a choice that
renders segmented in the Sheet renders segmented in the Inspector when it fits there too.
(The first version forced a dropdown in the Inspector with a hardcoded flag, so the same
setting looked like two different controls in two regions — A4 in `INCONSISTENCIES.md`.)
A segmented control sets a value and never navigates — `BeginTabBar` does not exist.

### 3.3 Dropdown — mutually exclusive, many or dynamic

B's dropdown is **not a box**: the resolved value in Mono 500 16 followed by a `▾`
chevron in Mono 400 11 `TextMeta`, 8 gap, right-bound, with a `--lineCtl` hairline
appearing on hover and focus. `--H` tall hit box. The value ellipsizes from the left of
the group so the chevron's right edge stays on the lane. Popup anchored under the trigger,
right-aligned to it, rows `--row`, max height 280 then a clipper, current option marked
with a dot. `Esc` and any outside click close it.

Because the control shows its own value, the value column stays empty for this kind
(§2.3) — which is B's grammar and removes the "lone caret stranded at the lane edge"
that a boxless dropdown otherwise produces when the value is placed elsewhere.

### 3.4 Slider — bounded continuous

B's paint at §3.0's sizes: track **8 tall, 4 radius**, fill a left-to-right gradient
`Accent @ 50% → AccentGradHi`, handle **8 × 20, 1 radius, `AccentHandle`**, with B's
`0 0 0 2px Accent @ 18%` halo. Hit box `--H`.

> **Deviation from B — the one E2's layout law forces.** B's `.mini` is a fixed **118px**
> track, because B's control column is a fixed 200u slot. E2 binds every control to its own
> lane, so the track is `PlaceFull()`. B's rendering is kept; B's width is not. This is the
> one place the two directions genuinely conflict, and E2's lane rule wins.

> **Second deviation.** B's unfilled track is `#FFFFFF @ 16%` = **1.57:1**. It moves to
> `--trackOff` (34%, 3.07:1), which is the same deviation `slider-widget-spec.md` already
> carried for `kRailAlpha`; the rail is the part of the control that tells you where the
> range ends.

The value lives in the value column at `Lw`; a 1px `#FFFFFF @ 52%` default tick sits on
the track. `←→` = one step, `Shift+←→` = ×0.1, wheel = one step, click positions.

### 3.5 Stepper — exact or unbounded

B's stepper is **borderless**: `−` and `+` glyphs in Mono 400 15 `#EFF5FB @ 40%`
(3.58:1 — a UI glyph, not body text), 18 wide each, 8 gap, `--H` hit boxes, right-bound.
It carries **no number**; the number is in the value column (§2.3), which is exactly how
B draws it (`<span class="val">Unlimited</span><span class="step">− +</span>`).
`ZeroMeans("Unlimited")` renders the word. `←→` steps; `Step()` accelerates after 400 ms.

This replaces the first version's 96-wide bordered box, which was E2's own invention and
did not match any control in either direction.

### 3.6 Text — free text

B's text field is the current value in Mono 500 16 followed by a `✎` glyph, `--H` hit
box, hairline on hover — the same grammar as the dropdown. Clicking swaps in a real input:
caret `Accent`, 1px `Accent` bottom edge, `--raised` fill, `Enter` commits, `Esc` reverts,
an outside click commits. Placeholder in `TextMeta` (not `TextFaint` — that role no longer
exists, §7.1). Because the field shows its own value, the value column stays empty (§2.3).

Validation runs **as you type** and its message sits **in the Inspector's Configure body**,
not under the box; the box border turns `Danger` and the affordance column shows nothing
(a validation error is not a state you reset). Dependent controls re-evaluate on every
keystroke, so a *save* action gated on a valid name enables and disables live.

### 3.7 Facts — the readout-run collapse

```cpp
s.Facts( "Signal", &HdrSignalSummary, &DrawHdrSignalLive );
```

One 44 row. Label left. A one-line summary (`PQ · 1000 nits · BT.2020`) sits
right-bound in the control zone in `TextMeta`. Affordance: chevron. **No state edge and
no reset dot** — the absence is the structural expression of *"never show a value the
overlay didn't verify"*. Selecting it puts the Inspector in **Details**, where the seven
underlying readouts live.

A Facts row whose source is not present renders `—` and says so in Details, rather than
inventing a value or hiding the row. `Output ▸ Second connector` is that state, drawn.

(Facts rows *do* hover and select — an earlier draft of §3.8 claimed "no hover", which was
never true and could not be: selecting the row is the only way to reach Details.)

This single rule is responsible for most of §1.1's line-count drop.

### 3.8 Meter — a live scalar

`--trk` segmented bar (20 segments, 1.5 gaps) in an `--H` hit box, `PlaceFull()`, value
in the value column. Read-only: no state edge, no reset dot. Indeterminate progress is a
Meter with a sweeping lit segment and a `TextMeta` caption; there are no spinners.

`display.budget_meter` is the drawn instance — the first version declared this kind and
registered nothing that used it.

### 3.9 Action row — buttons

B's **verb chip**: `--H` tall, 9 horizontal padding, Mono 500 11.5, no border, fill
`Accent @ 16%`, text `AccentText`. Right-bound, **at most two per row**. Three intents:
`accent` (≤ 1 per group), `danger` (fill `Danger @ 14%`, text `DangerText`), and `neutral`
— which is the one place a border is added, because a neutral verb's text is dimmer than
an accent one and the fill alone would not identify it.

`Danger()` returns a type whose only method is `Confirm()`, so a destructive button
without a confirmation does not compile. On screen that is a **two-stage arm**: the first
click turns the chip into `confirm — delete` on a stronger danger fill, the second fires,
and `Esc` disarms. The first version declared this rule and drew no confirmation anywhere.

An action row whose action is unavailable is disabled with a mandatory reason like any
other row — *"no profiles are saved"*, *"type a name first"*.

### 3.10 Composite — a control taller than one row

Its own section, because it is fix #3. See §4.

### 3.12 Bank — a multi-select whose value is a set

> **Decided 2026-08-23 (D7, decided in `AUTONOMOUS-DECISIONS.md`; raised as an open
> question in `INCONSISTENCIES.md` §F.3).** The chip bank is **kept**, unchanged, as the
> taxonomy's eleventh kind. The governing rule below is exactly what stops it becoming a
> checkbox by another name — that rule, not the bank's existence, is the thing a reviewer
> must hold the line on.

> Adopted from direction B (`.bank`). Cells 3 gap, `--H` tall, 8 padding, Mono 500 11,
> inactive fill `#FFFFFF @ 5%` / border `--lineCtl` / text `TextMeta`, active fill
> `Accent @ 22%` / border `Accent @ 55%` / text `AccentSeg`. B's inactive border (8%) and
> text (42%) are raised to clear the floors; everything else is B's.

**The rule that keeps this from reintroducing the checkbox**, stated so a reviewer can
apply it without judgement:

> A **Bank** is *one setting whose value is a set*. **N independent binaries are N switch
> rows.** If the members can be enabled and disabled for unrelated reasons, they are not a
> set — they are N settings, and they get N rows.

`log.sources` and `log.severity` are the drawn instances: one decision each ("which
subsystems am I looking at"), stored as one config key, resettable in one action. The
Monitor's seven modules are *not* a bank, because each is independently meaningful — they
stay seven switch rows with a `4 / 7` count and `all` / `none` on the group band.

### 3.13 Disabled, empty, error

- **Disabled** — row × **0.55** (not E's 0.34, which measures 2.6:1 and is unreadable
  over a bright frame; 0.55 measures 3.27:1, §7.2) **plus a mandatory reason string**
  shown in Configure. `.DisabledUnless(bool, const char*)` has no overload without a
  reason. There is exactly **one** disabled mechanism: a predicate returning a reason or
  nothing. A parameter inherits its parent's reason, *except* when it is the cause of it —
  otherwise turning Mute on would disable the Mute switch, which was a real bug in the
  first version.
- **Empty** — a 96-tall centred band, `TextMeta`, one line, optionally one accent button.
- **Error** — `Danger` border on the control, message in Configure.

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
| Accent hue | 2 | hue rail + 8 swatches | `218°` | `l`, `c` (expert) |
| Audio strip | 2 | fader + L/R meter | `-7.5dB` | `routing`, `match_rule` |
| Frametime graph | 3 | 240-sample sparkline | `7.04 ms · 3 outliers` | — (Details only) |
| Colour override | 2 | L/C/H rails + swatch | `#6ED274` | `hex` |

Four composites are drawn in `index.html` (Anchor, Accent hue, Audio strip, Frametime
graph); **Colour override is documented here but not registered in the mockup** — there
is no per-game colour-override setting in the demo registry for it to attach to. It is
kept in this table as the fifth instance the band rule already covers, not as a claim
about what the mockup renders; do not re-add "five composites" language without also
adding the registration. One band rule either way, no call site choosing geometry.
Today's two Position Grid call sites (notification placement, monitor placement) that
drifted apart cannot.

> **Built 2026-08-23 (P3 part C).** The band is implemented and **four of the five are
> registered in the product**: Anchor (`monitor.anchor`), Accent hue
> (`overlay.accent_hue`), Frametime graph (`monitor.frametime_graph` plus the five
> `monitor.stats_*` history graphs), and **Colour override** — issue #29's four
> per-module colours (`monitor.color_fps` / `_cpu` / `_gpu` / `_media`), which is the
> registration the paragraph above asks for. Its binding stays the existing packed
> `0xRRGGBB` integer; the control edits OKLCH and converts back, so no config format
> changed for it to exist.
>
> **Audio strip is still not registered** — P3b left the mixer's fader as a Slider row,
> and nothing declares `CompositeKind::Strip`. The band's body switch therefore draws
> nothing for it, deliberately, rather than inventing a body no declaration asks for.
> Same rule as above: do not describe the strip as built without also registering it.

> **Amended 2026-08-23 (post-audit).** The Accent hue and Audio strip Line-1 values above
> were corrected to match what the mockup actually renders. The previous text showed
> `oklch(.74 .12 218)` and `−7.5 dB · −12/−14` — the first was the pre-B7-fix debug
> string `INCONSISTENCIES.md` §B7 already replaced with a plain `218°`; the second baked
> the L/R meter's own readout into the value column, which §2.3 reserves for the control's
> resolved value alone. Both were doc drift, not mockup bugs — the code was already right.

---

## 5. The Inspector — two modes and the law that governs them

### 5.1 Modes

> **Amended 2026-08-23.** Three modes became two. `EXPLAIN` was never a mode in the sense
> the other two were: it was prose plus a metadata grid, and users reading it were doing
> one of two different things — *understanding what this is*, or *looking up how it is
> bound*. The split now matches those two intents, and the help prose lands where it is
> most useful: immediately above the values it explains.

The Inspector's header is a **two-cell** strip. **The cells are not tabs the designer
fills; they are a readout of what depth this selection actually has** — each carries the
count of what it holds, so the strip stays continuous, visible proof of how much is (and
is not) hiding behind the row.

| Mode | Content, all derived from the registration |
|---|---|
| **CONFIGURE** | the short **description of what this setting or feature does** (`.Help()`, required, ≤ 3 sentences, 240-char lint cap); the disabled reason or validation error if there is one; then **the values you can set** — the row's own control drawn as an Inspector row, followed by its ≤ 6 parameters in the same Row grammar at the same `--row` height; a reset action; a copy-key action. For a read-only row (`Facts`, `Meter`, `Graph`) the values block is replaced by one sentence saying so and pointing at Details — the cell is marked `ro` and its counter reads `ro`. |
| **DETAILS** | the derived binding grid `now / default / range / options / kind / key / writes / applies / parent`; `related` jump links; the keyboard line for this control kind; the read-only `.Live()` block; at most one animated element (sparkline, L/R meter); and **the captured log lines whose text contains this entry's key**. |
| **OVERVIEW** | nothing selected — replaces the strip entirely; §5.5 |

Mode selection is automatic and stateless: selecting a `Facts`, `Meter` or `Graph` row
opens **Details**; everything else — including arriving from the palette on a parameter —
opens **Configure**. The user can switch modes; the choice is not remembered, because *the
Inspector holds no state* (§5.2 clause 0).

**Why two generators and not two panels.** The change is a re-sort of what already
existed, not a new surface. `.Help()` still feeds prose, binding metadata is still typed by
nobody, `.Param()` still feeds rows, `.Live()` still feeds readouts. There are four
generators and two modes; a category file still cannot type a character that lands here.

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
> `.Help(text)` → Configure's description. Binding metadata (default, range, unit, key,
> destination) → Details' binding grid, typed by nobody. `.Param(...)` → Configure rows.
> `.Live(fn)` → Details readouts. Adding a fifth generator is a change to
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
> **4. Details is read-only by type.** `.Live()` accepts `std::function<Value()>` and
> nothing else. It has no `Bind` overload. A control cannot be *constructed* in Details,
> so "I'll just put this one setting in the diagnostics panel" is not a shortcut a
> reviewer has to catch — it is a compile error.

**Two further consequences worth stating**, because they are what make the law credible
rather than merely strict:

- **Everything in the Inspector is searchable.** Params register like any other entry, so
  `Ctrl+K → "denoise"` finds `display.sharpness.rcas_denoise` and jumps to it — selecting
  the parent row, opening Configure, focusing the param. A setting in the Inspector is
  therefore *one keystroke* from anywhere, which is a stronger reachability guarantee
  than the Sheet gives most rows. The palette also shows each entry's **live value** and
  **adjusts it in place** with `←→`, so a known setting can be changed without leaving the
  list at all.
- **The Overview card advertises the count.** Every category card ends with
  `sheet 9 rows · inspector 14 params · 0 unreachable`. If the Inspector ever *did*
  become a junk drawer, the number would say so on the screen you land on.

### 5.3 What Configure looks like

Inspector rows are **`--row` (44) tall**, single column, label left-bound at `x = --ipad`,
control right-bound at `IW − --ipad`. Same grammar, same right-bound law, same height,
different width. A parameter therefore looks and behaves identically in the Inspector, in
the inline fallback (§6) and in the Sheet if it is ever promoted — moving one costs one
word.

> **Amended 2026-08-23.** This section previously specified **40**, and then claimed in
> the next sentence that a parameter "looks and behaves identically" in all three hosts.
> Both could not be true. 44 wins, because it is the Sheet's height and the Sheet is the
> host a promoted parameter ends up in. A control wider than the Inspector lane (slider,
> dropdown, text, bank, hue, fader) gets a two-line variant: label + value on top, control
> full-lane below — still one height rule, one extra line of it.

### 5.4 What Details can hold — the transient content question

Details is the one mode allowed to hold content that changes every frame: a shader's
live preview thumbnail, a stream's L/R peak meter, the frametime sparkline, GPU sensors.
It is safe to be permissive here **precisely because clause 4 makes it read-only** — a
region that cannot contain a control cannot contain a hidden setting, so its only failure
mode is being uninteresting.

Two rules keep it cheap: the `.Live()` lambda runs **only for the selected entry** (E's
insight, kept), and Details paints at most one animated element per selection.

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
> selected row opens Configure **and** Details **as one full-sheet page with a back crumb**, replacing the
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

**Added 2026-08-23 — direction B's control atoms, measured against the same background.**
The background is a pure-white game frame under `darkening 0.80` and the slab's
`rgba(9,10,12,.88)`, i.e. `rgb(14, 15, 17)`, exactly as §7.2 defines it. Accent-derived
values are swept over all 360° of hue and the **worst** hue is reported, because the hue is
user-controlled.

| Element | Value | Ratio | Floor |
|---|---|---|---|
| Value column, at default | `TextPrimary` | **14.79:1** | 4.5 |
| Segmented, inactive text (B's 50%) | `#EFF5FB @ 50%` | **4.96:1** | 4.5 |
| Every control boundary | `--lineCtl` `#FFFFFF @ 42%` | **4.09:1** | 3.0 |
| Switch knob, off (B's 55%) | `#EFF5FB @ 55%` | **5.78:1** | 3.0 |
| Switch knob, on | `AccentKnob` | **12.79:1** worst hue | 3.0 |
| Switch border, on (B's 65%) | `Accent @ 65%` | **3.91:1** worst hue 353° | 3.0 |
| Segmented border, on (B's 60%) | `Accent @ 60%` | **3.50:1** worst hue 353° | 3.0 |
| Bank border, on (B's 55%) | `Accent @ 55%` | **3.12:1** worst hue 353° | 3.0 |
| Slider / hue / meter unfilled track | `--trackOff` `#FFFFFF @ 34%` | **3.07:1** | 3.0 |
| Stepper `−` / `+` glyph (B's 40%) | `#EFF5FB @ 40%` | **3.58:1** | 3.0 (UI glyph) |
| Verb chip text on its own 16% fill | `AccentText` | **8.20:1** worst hue 350° | 4.5 |
| Segmented active text on its own 24% fill | `AccentSeg` | **13.46:1** worst hue 22° | 4.5 |
| Slider handle | `AccentHandle` | **13.90:1** worst hue 344° | 3.0 |
| Rail icon, active | `AccentIcon` | **12.72:1** worst hue 21° | 3.0 |

**Worst case anywhere in the design after the change: 3.07:1** for a control boundary and
**4.96:1** for text. Both clear their floors at every hue.

**Three of B's own values were rejected and are recorded here so nobody re-introduces
them:** `#FFFFFF @ 8%` segmented border = **1.21:1**; `#FFFFFF @ 16%` slider track =
**1.57:1**; `#FFFFFF @ 18%` switch-off border = **1.69:1**. B's `.val.neutral`
(`#EFF5FB @ 44%` = **4.09:1**) is also rejected for a 16px value.

The slab's own 1px `Accent @ 42%` outer border measures 2.30:1 at its worst hue; it is
**decorative** — the slab is identified by its fill, backdrop blur and shadow, not by that
hairline — and is excluded from the floor deliberately, not by omission.

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
value, active segment, on-switch, slider fill, focus ring, the selected rail item's edge
and its icon. Never a header, never a border that is not communicating state. A category
where nothing has been changed is almost monochrome — and that is the correct look for
"everything is at default".

---

## 8. Navigation, scale and motion

### 8.0 The rail icon set

Eleven icons, **inline SVG**, one **24-unit grid**, stroke **1.7**, `miter` joins, `butt`
caps, `currentColor`. Fill is used only where a fill carries meaning — HDR's half-filled
disc, the Monitor's bar chart. The icon inherits the rail item's colour: `TextLabel` at
rest, `AccentIcon` when the item is active, so the icon is one of the accent's state jobs
(§7.7) and not decoration.

They are a *set*: every glyph is built from the same 24-unit rectangle, the same stroke
weight and the same corner treatment, so the rail reads as one family rather than eleven
found symbols. At `display_scale 0.5×` the 24-unit box is 12 physical px with a 0.85px
stroke and every glyph is still one recognisable silhouette; at 2.0× it is 48px and no
glyph gains detail it does not have — which is the test a monospace glyph set fails, since
a glyph's weight is chosen by the typeface, not by the design.

No icon font and no external asset: the artifact CSP blocks both, and so does a compositor
that has to draw its own UI without a font cache.

### 8.05 The collapsed Inspector

Adopted from **E1**. When the Inspector is hidden — by `Ctrl+I`, by the `✕` in the mode
strip, or by the ladder at step 3 — the region collapses to a **20-base vertical spine**
on the slab's right edge: `#FFFFFF @ 4%` fill, a region hairline on its left, and the word
`inspector ›` set in `writing-mode: vertical-rl` at `TextMeta`. Hovering tints it
`Accent @ 16%` and lifts the text to `AccentSeg`; clicking restores the region.

Two reasons it beats a bare edge or an icon: it **names itself**, so the region is
discoverable by someone who never learned `Ctrl+I`; and it **holds its own width**, so the
sheet lane shrinks by 20 rather than the sheet being overlapped by an invisible hit strip.

### 8.1 The shell

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ● GAMESCOPE-RITZ                     app 1174180  games/1174180.json  ⌕ ▤ ✕  │ 40
├───────────┬──────────────────────────────────────┬───────────────────────────┤
│           │ DISPLAY / Upscaling   differs 3  ⌕   │  CONFIGURE 4    DETAILS 9 │ 56
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

> **Amended 2026-09-02 — the slab is a flat 85% of the surface, no absolute pixel cap.**
> The formula above (and its 1180/1560/940 constants below in §8.3's table) is superseded.
> The `min(…, 1560)`-shaped cap pinned the slab to the same fixed design size on any surface
> once the surface was large enough to hit it — a 4K output got the identical slab a 1080p
> one did, which read as "the UI is too small" at high resolution. The current formula is
> `Slab = surfaceW × 0.85 × surfaceH × 0.85`, with no upper pixel bound at all (only the
> pre-existing "never bigger than the surface itself" clamp remains). The independence from
> `scale` described above is unaffected — this only changes how the *surface* maps to the
> slab's px size, not whether `scale` does. See `Layout.cpp`'s `Slab::For()` and
> `tests/test_overlay_shell.cpp`'s worked table for current figures.

Rail: eleven items in **three** sections — `DISPLAY`, `SYSTEM`, `SETUP` — drawn icons
(§8.0) and labels, two levels, no tab bar anywhere in the product. Active item carries a
2px `Accent` left edge that survives the icon collapse. **The counter on a rail item means
exactly one thing — the number of settings in that area that differ from default.** The
single alternate form is a captured severity count, drawn in `Danger` with a `!`. (The
first version used that one slot for five different quantities; see A1 in
`INCONSISTENCIES.md`.)

> **Amended 2026-08-23 (D8, decided in `AUTONOMOUS-DECISIONS.md`; raised as an open
> question in `INCONSISTENCIES.md` §F.4).** Five sections became three: `IMAGE` (one
> area, `Shaders`) folds into `DISPLAY`; `AUDIO` (one area, `Mixer`) folds into `SYSTEM`.
> A section header that groups exactly one item buys nothing at 1.0× and costs a line at
> 0.5×, where vertical space is tightest — a header is only earning its keep once it is
> disambiguating between two or more items. If a second `IMAGE`- or `AUDIO`-shaped area
> ever appears, its section comes back with it; the fold is a consequence of the current
> registry's shape, not a rule against those categories existing. Area **ids** and titles
> are unchanged (`image.shaders` still reads "Shaders" wherever it's named) — only the
> rail's `sec` grouping moved, so nothing about §2.6's key-prefix statement is affected.
> Section order stays deliberate: `Shaders` sits at the end of `DISPLAY` (after `HDR`,
> where its image-processing kin already are), and `Mixer` leads `SYSTEM` (ahead of
> `Monitor` and `Log`, its original relative position).

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
| `Ctrl+D` | reset selected row **and its parameters** to default |
| `← →` in the palette | adjust the highlighted entry's value **in place**, without leaving the list |
| `Space` on a switch | toggle without leaving the row |
| `Ctrl+I` | cycle Inspector host: **column → drawer → hidden** |
| `Ctrl+/` or `?` | Configure + Details for the selected row (full-sheet page when the Inspector is hidden) |
| `Esc` | dismiss the transient layer in front, else **close the overlay** — see the amendment below |

**Amendment, 2026-08-24 (AUTONOMOUS-DECISIONS D26) — `Esc` closes the UI.**
This row used to read *"palette → drawer → inline expansion → overlay"*, and the user's
report retired it: *"pressing escape should close the UI."* The ladder made `Esc` a general
undo of the last navigation — three presses to get back to the game from a fresh open, each
one silently rearranging the shell instead of leaving.

`Esc` now dismisses only a **transient layer** — the command palette, an open dropdown, a
text field mid-edit, or an armed destructive action — and closes the overlay from everywhere
else. The drawer, the explain page, the inline expansion and the selection are the shell's
own arrangement, not layers on top of it; they persist, they have their own controls
(`Ctrl+I`, the back crumb), and `Esc` from any of them closes the UI. An armed action is
disarmed **unconditionally**, before any other rung, so it can never survive an `Esc` and
fire on a later press. This matches the launcher (D25), which already gave the game straight
back on `Esc`.

**Selection and editing are the same click.** Clicking a slider both selects the row and
starts the drag; there is no select-then-edit tax.

### 8.3 The responsive ladder, and the 2.0× arithmetic

Base budget: rail 232 (icons 60), inspector 400 (wide 544), sheet minimum 560,
column minimum 420. Slab width on 1920 = `min(surfaceW × 0.90, max(1560 × scale, 1180))`.
The ladder is one comparison applied twice: `rail + inspector + sheetMin ≥ slabBase`
collapses the rail; if it still holds with the icon rail, the inspector becomes a drawer.

> **Amended 2026-09-02 — slab width formula superseded, see §8.1's amendment above.**
> "Slab width on 1920" is now `surfaceW × 0.85` (1632, not 1560), with no cap for any
> surface. The worked table immediately below therefore describes the *pre-2026-09-02*
> pixel-capped slab and is kept as history, not as the current numbers — it still proves
> the table's own headline claim (2.0× beats 1.25× on sheet space) because that property
> comes from the ladder's comparison, not from the specific slab width. For the current
> 1920×1080-surface table (1632 px wide, 0.5×–2.0×), see `tests/test_overlay_shell.cpp`'s
> "the responsive ladder reproduces SPEC 8.3's worked table" test, which is recomputed for
> the new formula and is the source of truth going forward.

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
| E's `.Hint()`, `.Note()`, readout runs, expert groups | Configure / Details |
| E's `ui::Panel` inspector authoring API, B's `PaneCtx` | **Deleted.** Four generators, Prefix Law, Six Budget |
| E's Inspector Contract (a promise) | The **Reachability Law** (a rendering fallback, headlessly testable) |
| `TextMeta` 44%, `TextFaint` 30%, disabled × 0.34 | 52%, role deleted, × 0.55 — nothing below 3.07:1 |

Kept verbatim because they were measured and liked: the **slider** (with one contrast
deviation, §3.4), the **toggle** geometry, the **segmented control**, the **3×3 position
grid**, and the **OKLCH accent family** with its live hue.
