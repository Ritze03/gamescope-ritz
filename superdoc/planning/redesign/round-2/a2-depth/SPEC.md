# Direction A2 — "CONSOLE + LEDGE"

**A's one pane, kept. The depth that needed a tooltip moved into the band the key
legend was already wasting.**

Date: 2026-08-23. Refines `superdoc/planning/redesign/a-console/`. Companion files:
`API.md`, `FEASIBILITY.md`, `index.html` (interactive).

Two premises changed since Direction A: **gamepad support is dropped entirely** (mouse
and keyboard only), and **Direction B's registry and command palette are adopted as a
feature**, not as the navigation model.

---

## 0. The problem this file exists to solve

> *"The simplicity is amazing, since it doesn't look intimidating. But for some stuff, it
> makes it more complex, since we'll have to rely on tooltips and such."*

That is correct and it is structural. A achieves calm by removing surface, but the
information does not stop existing — it gets pushed into the two worst homes available:

- **Tooltips.** Transient, undiscoverable, mouse-only, un-scannable, and impossible to
  read while you are dragging the slider they describe.
- **Help sub-lines under the label** (A's stated answer, `a-console/SPEC.md` §2.3). This is
  worse than it looks: a sub-line grows the row from 44px to ~64px, so *the screens that
  need the most explanation become the densest screens on the product*. HDR would go from
  16 rows to 16 rows-and-a-half-again of prose. That is exactly backwards.

So the question is: **where does explanatory depth live in a single-pane console without
making the screen busier?**

### 0.1 The answer: the Ledge

> **The Ledge is the bottom band of the console. It always describes whatever currently
> has focus, in one line. Press `?` and it rises into a full detail region — help,
> defaults, provenance, related settings, and that setting's expert parameters — and stays
> risen until you lower it.**

The move that makes this cost nothing: **the Ledge replaces the key legend.** A's legend
(`a-console/SPEC.md` §1.1, 52px, `Ⓐ Adjust  Ⓑ Back  Ⓨ Reset…`) is a static row of hints
that a user reads twice in their life and then never again — the lowest-information-density
surface in the entire design, held permanently at full brightness. With gamepad dropped,
half of it (`Ⓐ Ⓑ Ⓧ Ⓨ`) no longer even applies.

So the Ledge occupies the same 13u band, at the same place, with the same border. The key
hints do not disappear — they shrink to three contextual glyphs at the far right of the
same band. **Net new surface for the entire depth mechanism: zero pixels.**

### 0.2 Why not the obvious alternatives

| Considered | Why it lost |
|---|---|
| **A persistent right-hand inspector** (Direction E's answer, which the user liked) | Three problems specific to A. (1) It costs ~384 of 1188px — 32% of the pane — permanently, and A's whole identity is *one* region. (2) It steals from the same axis as the control column, which critique #2 already says is too cramped; the Ledge steals from scroll length instead, and A's screens are short. (3) A's `Stage::Wide` (LOG) collapses the rail to get full width — a right rail fights that directly, while a bottom band works for LOG unchanged (it decodes the selected log line). E should absolutely keep the inspector; that is E's design. A's honest answer is the same *idea* rotated 90° onto the surface A was already wasting. |
| **Expanding / accordion row** | The content pushes every row below it downward, so holding `↓` makes the whole screen pump. It also breaks the uniform row height that `ImGuiListClipper` needs (`e-inspector/SPEC.md` §2.1 flagged the same constraint). A moving list over a moving game is the one thing this UI must not do. |
| **Tooltips** | The premise's villain. Also: a tooltip cannot host a control, and part of the answer is *moving controls* out of the row list (§2.4). |
| **A help sub-screen per setting** | Three keystrokes to read one sentence, and it takes the value you are editing off-screen while you read about it. It would also make L2 a help level, breaking A's "there is no L3". |
| **Keeping help sub-lines as the primary channel** | §0 above — it makes complex screens the densest ones. Retained for exactly one case: a **disabled** row must state its reason inline, because you have to be able to read it while *scanning*, without focusing it. That reason renders on the same line as the label, not below it, so the row stays 44px. |

### 0.3 Why this stays calm

Five properties, each load-bearing:

1. **Zero new surface.** It is the legend's band, at the legend's height, with the legend's
   border. Resting, the console has exactly as many regions as A did.
2. **Resting is one line.** Not a paragraph, not a card. One sentence, ellipsised, plus a
   value/default/destination strip in Mono. It reads as a *status bar*, which is the one
   piece of chrome people already know how to ignore.
3. **Raising is a deliberate act.** Default is resting. The user who wants calm never
   presses `?` and never loses a pixel. The user who wants E's inspector turns on
   *Config › General › Keep the Ledge raised* and gets a permanent detail region docked to
   the bottom — a real setting in the mockup, because both answers are legitimate.
4. **It removes ink from the stage, net.** Help sub-lines are gone from every enabled row
   (−1 line per explained setting), and expert parameters move out of the row list
   entirely (§2.4). HDR loses three rows; Adaptive Brightness loses six. The stage is
   *emptier* than A's, not fuller.
5. **The accent budget is untouched.** The Ledge introduces no new colour role, no new
   elevation step, and no new type role. Its raised body is drawn with the same Row
   grammar as the stage — literally the same function.

---

## 1. Navigation

### 1.1 Anatomy

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ■  CONSOLE › GAMESCOPE › HDR              [app 1245620]  fps 142  frame 7.0ms │ header 14u
├──────────────────┬───────────────────────────────────────────────────────────┤
│ CONSOLE          │ UPSCALING   DISPLAY   FRAME LIMITER   ─HDR─      PgUp/PgDn │ subtabs
│ ▎▣ Gamescope 2/23│ ────────────────────────────────────────────────────────── │
│   ▣ Shaders    9 │  OUTPUT ───────────────────────────────────────────────────│
│   ▣ System M. 21 │  HDR                                    [ on ▐ ]           │ stage
│   ▣ Audio      4 │ ▎Tonemap Operator      [none][reinhard][aces][unch][hable] │
│   ▣ Config    10 │  SDR Gamut Wideness    ▰▰▰▰▱▱▱▱▱▱▱▱ ·│           0.35      │
│   ▣ Log          │  …                                                         │
│ v3.16.14         │                                                            │
├──────────────────┴───────────────────────────────────────────────────────────┤
│ ▲  Tonemap Operator — How out-of-range highlights are folded back into what   │ LEDGE
│    the display can show.        aces · def reinhard · app 1245620.json   ? ^K │ 13u
└──────────────────────────────────────────────────────────────────────────────┘
                                    press ?  ↓
┌──────────────────────────────────────────────────────────────────────────────┐
│ ▼  Tonemap Operator — How out-of-range highlights are folded…    ⌄ esc  ^K    │ 13u
├──────────────────────────────────────────────────────────────────────────────┤
│  Tonemap Operator   [CHOICE]                            RESET TO REINHARD     │
│  Applied when the game asks for more luminance than the connector can         │
│  produce. none hard-clips… aces is the filmic curve most engines already      │ LEDGE
│  target… the three constants below are theirs.                                │ raised
│  DEFAULT reinhard   OPTIONS none · reinhard · aces · uncharted2 · hable        │ 52u
│  WRITES app 1245620.json   ID hdr.tonemap                                     │ (≤34vh)
│  RELATED [ HDR ] [ HDR Input Gain ]                                           │
│  EXPERT · TONEMAP OPERATOR ───────────────────────────────────────────────    │
│   Shoulder strength                   ▰▰▱▱▱▱▱▱▱▱▱▱               0.22         │
│   Toe strength                        ▰▰▰▱▱▱▱▱▱▱▱▱               0.30         │
│   Linear white point                  ▰▰▰▰▱▱▱▱▱▱▱▱               11.2         │
└──────────────────────────────────────────────────────────────────────────────┘
```

Console size, rail, stage, sheets, wells and the three body kinds (`List` / `Wide` /
`Split`) are **unchanged from `a-console/SPEC.md` §1–2** except where stated below. This
document specifies the deltas.

### 1.2 The depth stack, restated for mouse + keyboard

Three levels as before (L0 rail, L1 stage row, L2 drill/sheet). The Ledge is **not** a
level — it is a peripheral display of the current focus, plus an optional focus *scope*.

| Level | Focus is on | Ledge shows |
|---|---|---|
| **L0 Rail** | a section | the **section card**: blurb, screen list, setting count, how many differ from default (each named and clickable), destination file |
| **L1 Stage** | a row | that setting |
| **L1′ Ledge** | an expert row *inside* the raised Ledge | that expert setting; the originating row keeps a dimmed **parent tick** so you never lose your place |
| **L2 Sheet / Drill** | an option / a sub-screen row | that option / that row |
| **Palette open** | a search result | that result — so you can read depth without leaving the search |

The Ledge is **never empty**. There is always exactly one focused thing (A's §1.4 rule:
"never zero and never two"), and at L0 the section card covers the case where the focused
thing is a whole section. There is no "select something" placeholder anywhere in the design.

### 1.3 Keys

Gamepad is gone; every gamepad verb had a keyboard twin already, so nothing is lost.

| Key | Action |
|---|---|
| `↑` `↓` | move focus one row |
| `←` `→` | **adjust the focused control in place** — slider step, choice cycle, switch flip |
| `Enter` | activate: drill, open sheet, run action, begin inline edit |
| `Esc` | leave the Ledge → lower the Ledge → close a sheet → close the console (in that order) |
| `Tab` / `Shift+Tab` | previous / next **section** |
| `PgUp` / `PgDn` | previous / next **sub-tab** |
| **`?`** | **raise / lower the Ledge** |
| **`Ctrl+↓`** | step focus **into** the raised Ledge's expert rows (only when the focused row has them) |
| `Ctrl+D` | reset the focused row to its default |
| **`Ctrl+K`** or `/` | **command palette** |
| `Ctrl+Shift+O` | close the console |

`?` is chosen deliberately: it is the universal help key, it is a single unshifted-meaning
character, and it is not bound by ImGui. `Ctrl+↓` reads as "go down past the last row",
which is where the Ledge physically is.

**Pointer**: click a rail item; click a row (the whole row is one hit target — clicking
focuses it *and* operates its control, one click, no select-then-edit tax); click the
Ledge's resting line to raise it; click the `▲` caret; drag a slider; click a sub-tab;
click a `RELATED` chip to jump. Hover and focus stay visually distinct (hover = fill only;
focus = fill + accent tick), because a mouse and the keyboard are both live at once.

### 1.4 The Ledge Contract

Borrowed from E's Inspector Contract, because it is the rule that stops a detail region
from becoming a dumping ground — and it is the rule that keeps A's calm honest:

> **The stage alone is a complete UI.** Every setting a user changes during a normal
> session is reachable and editable in the row list without ever raising the Ledge.
> The Ledge adds *depth* — help, defaults, provenance, related links — never *access*.

One licensed exception, and it is what makes the mechanism structural rather than
decorative: **`.Expert()` parameters** (§2.4). These are not session settings; they are
tune-once constants. A caller cannot decide ad hoc to hide a setting in the Ledge — it is
declared `Expert` at registration or it is not, and the palette finds it either way, so
"hidden in the Ledge" never means "unreachable".

Consequences the implementation must honour:
- A screenshot of the stage with the Ledge resting is a legitimate answer to "what can I
  set here", except for expert constants.
- Lowering the Ledge never disables a control; it only stops explaining it.
- The Ledge is never the only place a *reason* appears: a disabled row states its reason
  inline (§2.2).

---

## 2. Option presentation

### 2.1 The row rule, amended

A's rule stands: every setting is a Row, `[tick] [label] ……… [control in the control
column]`, and the caller supplies semantics while the kit supplies pixels. Three amendments.

**Amendment 1 — help sub-lines are gone.** `Opt::pszHelp` no longer renders under the
label. It renders in the Ledge. Rows are 11u (44px), always, with no growth case. This is
the change that pays for the Ledge twice over: it removes a line from every explained row
*and* it restores the uniform row height `ImGuiListClipper` wants.

**Amendment 2 — the disabled reason renders inline, on the label's line.** `Meta` type,
`text/meta` alpha, immediately after the label, ellipsised. A disabled row must explain
itself to someone *scanning*, not only to someone who focused it. Row height unchanged.

**Amendment 3 — the delta pip.** A row whose value differs from its registered default
gets a 1.5u accent dot between the label and the control column. This is the one thing the
row *gains*, and it costs 6px. It answers "what have I changed here" in peripheral vision,
and it is what makes the section card's `2 of 23 differ` computable.

### 2.2 Control sizing — the answer to "sliders and multi-selectors don't use the space they deserve, and differ in size too much"

The rule is mechanical, not aesthetic:

> **Every control's *right edge* is the control column's right edge. Controls that can
> stretch, stretch to the column's full width. Controls that cannot are right-aligned
> against it. No control is ever sized by its content.**

| | A | A2 |
|---|---|---|
| Control column `--ctl-w` | 56u (224px) | **88u (352px)** |
| Wide variant `--ctl-wide-w` | 76u, kit-chosen per row | **deleted** — there is one width |
| Segmented cells | content-sized, so a 3-option and a 5-option control were visibly different objects | `flex:1` inside the fixed column, 0.75u gaps. **A 3-cell and a 5-cell segmented are the same total width**, and every cell boundary in a screen of segmented rows lines up or is at least bounded by the same two edges |
| Slider track | ~110px after the value slot | `flex:1` inside 88u, minus a **fixed 17u value slot** → ~284px of track, 2.6× A's |
| Picker / text entry | content box | fills the column |
| Switch / action / readout / drill / chips | right-aligned | unchanged — a 352px-wide switch would be absurd, and right-alignment already gives it a shared edge |

Two consequences worth stating. First, the "kit measures the label widths and picks a wide
column" logic from A's §2.1 is **deleted**, along with the font-atlas-rebuild hazard it
created (`a-console/FEASIBILITY.md` §6, last row: a rebuild frame could flip the decision
mid-drag). One width means no measurement, no hysteresis, no flip. Second, at 352px a
five-option segmented gets ~66px per cell at 1.0×, which fits `uncharted2` at `Value` size
without ellipsis — the case that previously forced the wide variant.

Floor: the column shrinks with the console but never below 46u (184px); below that the
console has already dropped its rail labels (§3.4).

### 2.3 Sub-tabs — the answer to "the tabs look bad"

Delete the pill buttons. A sub-tab is a **text run with a sliding accent underline**:

- `Title` role (Mono 600, 0.78 × base, UPPER, +0.04em tracking), 5u apart, sitting on a
  1px `line/hair` rule that runs the full stage width.
- Inactive `text/meta`; hover `text/label`; active `text/primary` with a 0.5u accent
  underline flush to the rule, plus the accent glow the design already uses for the focus
  tick. The underline slides between tabs at 16/s using the existing `Approach()`.
- Max 6 tabs, same as A.

Why this and not restyled buttons: four bordered boxes across the top of every screen were
competing with the bordered boxes in the control column of every row directly beneath them,
at the same size, in the same colour family — the eye could not tell navigation from
control. The underline is a *different kind of mark* from anything a row can contain, so
the distinction becomes structural instead of a matter of styling the box better. It also
removes eight borders and four fills from the top of every screen, which is a net
reduction in ink on the busiest part of the layout.

### 2.4 `.Expert()` — depth that is a control, not prose

The half of the problem prose cannot solve: some settings have *parameters* that are only
meaningful once you understand the setting. A puts them in the row list (HDR would carry
three tonemap constants; Adaptive Brightness six tuning values), where they are noise for
99% of sessions.

An entry may declare other entries as its expert parameters. They render **only inside the
raised Ledge**, below that entry's detail, using the identical Row grammar — the same
`Row()` call, the same tick, the same control column (72u there rather than 88u, because
the Ledge is not indented by the rail). Nothing new to learn.

What this buys, measured on the mockup's own screens:

| Screen | Rows in A | Rows in A2 | Moved to the Ledge |
|---|---|---|---|
| Gamescope › HDR | 19 | 16 | 3 tonemap constants |
| Shaders › Adaptive Brightness | 9 | 3 | 6 tuning values |

The stage gets *quieter* while the product gets *more* configurable. That is the structural
payoff, and it is why the answer had to be a region that can host controls rather than a
region that can only host text.

### 2.5 Control taxonomy

Unchanged from `a-console/SPEC.md` §2.3, with three edits:

- **#13 Tabs** — the sub-tab bar is now the underline run of §2.3.
- **#4 Slider** — the "fine step on a trigger held" becomes `Shift` for fine, `Ctrl` for
  coarse. The **default tick** (a 1px `#FFF @45%` mark on the rail at the registered
  default) is added, so "how far from stock am I" is readable without a number. That mark
  exists because the registry now guarantees a default exists.
- **Everything gamepad** — the `A/B/X/Y` column of the taxonomy table maps to
  `Enter / Esc / Ctrl+D / —`.

---

## 3. Styling and theming

Colour roles, elevation, the OKLCH accent family, and the motion language are unchanged
from `a-console/SPEC.md` §3 except for the text alphas below, which are changed **because
they were measured and three of them failed**.

### 3.1 The measurement model

The console sits over an arbitrary game, so "the background" is a range, not a value. The
effective background is computed as:

```
game frame
  → compositor pass:  rgba(4,6,9, .62)     (blur 1.0 + darkening 0.8, approximated)
  → console base:     rgba(8,9,11, .86)
  → optional lift:    white @ 3% (Ledge resting / rail / well)
                      white @ 5% (Ledge raised)
                      white @ 6% (focused row)
```

Worst case for light-on-dark text is the **brightest** possible game, so all ratios below
are measured against a **white snow field at full brightness**. The three probes:

| Game behind | Effective console base | Relative luminance |
|---|---|---|
| Black night scene | `rgb(7.5, 8.7, 10.9)` | 0.0026 |
| Blown-out orange fire | `rgb(20.4, 16.7, 13.0)` | 0.0057 |
| **White snow field (worst)** | **`rgb(20.8, 21.8, 23.8)`** | **0.0079** |

Every number in §3.2 is that worst case. On the other two backdrops every ratio is higher.

### 3.2 Measured contrast table

Text is `#EFF5FB` at the stated alpha. **Body text must clear 4.5:1; UI components and
large text must clear 3:1.**

| Role | Alpha | Over base (worst) | Over Ledge raised | Over focused row | Needs | |
|---|---|---|---|---|---|---|
| `text/primary` | 93% | **14.35:1** | 12.77:1 | 12.43:1 | 4.5 | PASS |
| `text/label` | **72%** | **8.93:1** | 8.18:1 | 8.01:1 | 4.5 | PASS |
| `text/meta` | **56%** | **5.85:1** | 5.52:1 | 5.43:1 | 4.5 | PASS |
| `text/faint` | **38%** | **3.37:1** | 3.31:1 | 3.28:1 | 3.0 | PASS (non-body only) |
| *A's `text/label`* | *68%* | *8.08:1* | | | 4.5 | pass |
| *A's `text/meta`* | *46%* | ***4.35:1*** | | | 4.5 | **FAIL** |
| *A's `text/disabled`* | *30%* | ***2.57:1*** | | | 4.5 | **FAIL** |

**That is the user's complaint, measured.** `text/meta` at 46% is the alpha A used for help
sub-lines, group headings and every read-only value — the exact text the user said was hard
to read. It misses by 0.15. `Palette.h`'s issue #62 had already moved the shipped
`kMetaTextAlpha` up once after three "too dark" complaints; 46% was still not enough over a
bright game. **56% is the smallest value that clears 4.5:1 with margin on all three
backdrops**, and it is now the only meta value in the design.

`text/faint` (38%) is restricted by rule to **non-body** runs: log timestamps, chevrons,
breadcrumb separators, the palette's screen-path column, and field captions in the raised
Ledge. Each is a short Mono or UPPER run and each clears the 3:1 large/UI floor.

**Accent tokens** — measured across **all 360 hues** and all three backdrops, taking the
minimum:

| Token | OKLCH | Worst ratio | At hue |
|---|---|---|---|
| `accent/value` | `.84 .10 h` | **10.46:1** | 18° |
| `accent/text` | `.82 .10 h` | **9.94:1** | 353° |
| `accent/seg` | `.90 .07 h` | 12.76:1 | 18° |
| `accent/edge` | `.80 .12 h` | 9.15:1 | 19° |
| `accent/base` | `.74 .12 h` | 7.43:1 | 353° |

Fixing L and C per token and letting only the hue move is what makes a *single* table valid
for every theme the user can select. This is the strongest argument for keeping `Palette.h`
exactly as it is.

**State colours** (hue-fixed, do not follow the accent):

| Role | OKLCH | Ratio |
|---|---|---|
| `state/ok` | `.78 .16 145` | 9.59:1 |
| `state/warn` | `.74 .17 60` | 7.52:1 |
| `state/danger` (fill / border) | `.70 .17 25` | 6.29:1 |
| `state/danger-text` | `.78 .14 25` | 8.37:1 |

A's `state/warn` moves from `.72 .17 55` to `.74 .17 60` — the old value measured 6.4:1 and
sat close enough to the danger hue to be confusable at small sizes.

**UI component boundaries** (WCAG 1.4.11, floor 3:1):

| Element | Value | Ratio | |
|---|---|---|---|
| Off-switch border | white @ **36%** | **3.34:1** | PASS |
| Segmented inactive border | white @ **36%** | 3.34:1 | PASS |
| Off-switch knob vs its own track | white 62% on white 10% | 5.68:1 | PASS |
| Segmented inactive label vs its fill | 62% on white 6% | 6.32:1 | PASS |
| `line/hair` (console + region borders) | white @ 8.5% | 1.26:1 | decorative by rule* |
| `line/row` (row dividers) | white @ **10%** | 1.32:1 | decorative by rule* |

\* Hairlines and dividers carry no information that is not already carried by position and
spacing: remove every divider and the layout is still unambiguous, because the row grid is
uniform. They are explicitly exempt, and that exemption is stated so nobody later "fixes"
them up to 33% and turns the console into graph paper. A's `line/strong` at 16% (1.62:1)
was **not** exempt — it was the off-switch border, which is the sole indicator of an
off switch's existence — so it moves to 36%.

**Disabled rows — the rule changes, not just the number.** A dimmed the whole row to 38%
alpha, which puts the label at an effective 27% → **2.34:1**, failing even the 3:1 floor.
That is precisely backwards: a disabled row is the row you most need to *read*, because it
is the one telling you why you cannot do the thing you came to do.

> **A disabled row's text does not dim. The label drops from `text/label` to `text/meta`
> (5.85:1) and the reason renders beside it at the same alpha. Only the *control*
> desaturates** — slider fill and handle go neutral white (28% / 45%), the switch drops to
> 55% opacity, the segmented loses its accent.

Both signals survive: it looks inert, and it is legible.

**Enforcement.** `ui.contrast 1` (ConVar) walks every text run drawn this frame, composites
its colour against the surface it landed on, and flags anything under its role's floor.
The floors are constants next to the role table, so this file and the code cannot drift.

### 3.3 The rail collapse — the answer to "icons must not shift position"

The rail collapses for `Stage::Wide` (LOG). The rule that fixes it is arithmetic, not
tuning:

```
rail_collapsed_w  =  2 × ( stage_pad_x + icon_w / 2 )
                  =  2 × ( 6u + 4.5u/2 )  =  16.5u  =  66px @ 1.0×
```

At 248px the icon's left edge is at `pad_x` = 6u and its centre at 8.25u. At 16.5u the icon
is centred in the strip — which is *the same 8.25u*. **The icon's centre is identical in
both states, at every `display_scale`, because both are derived from the same two
constants.** The collapse animation therefore moves the rail's right edge and nothing else.

What actually changes during the 12/s width animation:
- The label and count **fade by alpha only** (`Approach`ed to 0, stop drawing under 0.05).
  They never reflow, never truncate, never clip mid-glyph — `a-console/FEASIBILITY.md` §4.3
  called this out and it is now a hard rule.
- The `CONSOLE` group caption and the version footer fade with them.
- The selected item's 0.75u accent left edge is at x=0 in both states, so the "you are
  here" mark does not move either.

A debug assert (`ui.audit 1`) compares the icon's painted centre against
`stage_pad_x + icon_w/2` every frame and fires if any rail state disagrees. The bug class
cannot come back silently.

### 3.4 Scaling 0.5× – 2.0×

As A, plus:
- The **raised Ledge is clamped to `min(52u, 34vh)`**. At 2.0× on a 1080p output, 52u would
  be 416px against a stage that is already at its clamp; 34vh caps it at ~367px and the
  stage keeps its scroll. The resting Ledge is never clamped — 13u at 2.0× is 104px, which
  is one comfortable line.
- The control column floor is 46u; the slider's value slot never falls below 6 monospace
  characters.
- Below 900px of stage width the Ledge's key hints drop to glyphs only; below 700px they
  drop entirely (the resting sentence is worth more than the hints).

---

## 4. The registry and the palette (Direction B, as a feature)

Adopted wholesale from `b-command/API.md` §3, not reinvented. Every setting registers once
with an id, a bind, a default, a range, keywords and **`.Help()`**. What changes here is
only *which surfaces consume it*: A2 keeps the rail as primary navigation and adds the
palette as a jump.

**The interaction with the depth problem is the point.** A registry entry is already the
natural home for help text — B makes `.Help()` a registration-time assert precisely because
"the Inspector has nowhere else to look". A2 makes the same declaration feed **six**
surfaces from one string set:

| Registry field | Row | Ledge resting | Ledge raised | Palette | Reset | Section card |
|---|---|---|---|---|---|---|
| `title` | label | subject | heading | result title | — | — |
| `Help()` | — | **the sentence** | first line | **the subtitle** | — | — |
| `Detail()` | — | — | **the paragraph** | — | — | — |
| `Default()` | delta pip, slider tick | `def …` | `DEFAULT`, RESET button | — | the value | `n differ` |
| `Range()`/`Step()` | slider geometry | — | `RANGE` / `STEP` | ←→ step | — | — |
| `Writes()` | — | destination chip | `WRITES` | — | — | destination |
| `Keywords()` | — | — | — | fuzzy match | — | — |
| `Related()` | — | — | jump chips | — | — | — |
| `Expert()` | *absent from the row list* | — | **hosted rows** | still findable | — | — |
| `EnabledWhen()` | inline reason | "Unavailable — …" | warn line | — | — | — |

**Palette** (`Ctrl+K` or `/`): a centred overlay over the rail and stage, leaving the header
and **the Ledge visible**. Each result is `title` + the registry's one-line `Help()` as its
subtitle + the screen path in `text/faint` + **the live control on the right**. `↑↓` browses,
`Enter` jumps and lands focus on that exact row, `←→` adjusts in place without leaving, `Esc`
closes. Because the Ledge stays visible and follows the highlighted result, you can search,
read the depth, and adjust the value without ever navigating — while the *rail* remains the
thing you use when you know where you are going.

Landing on an `Expert()` entry from the palette does the right thing automatically: it
navigates to the **host** row, raises the Ledge, and focuses the expert row inside it. The
palette result labels it `· expert of Tonemap Operator` so the jump is not a surprise.

---

## 5. Screen inventory

Unchanged from `a-console/SPEC.md` §4, with the expert-parameter moves of §2.4. Six rail
sections, one layout language, one depth region.

The **LOG** deserves a note, because it is the screen where the Ledge earns its place for
free: LOG has no rows at all, so under A it had no help channel whatsoever. In A2 the Ledge
describes the **selected log line** — its scope, level and timestamp resting; a plain-English
decode of what that scope's message means when raised (*"Atomic commit overran the frame
budget: the kernel took longer to accept the commit than one refresh interval allows…"*),
plus `copy line` and `filter to [scope]`. Same band, same rule, no new mechanism, and it
turns the log from a wall of text into something a non-expert can read.
