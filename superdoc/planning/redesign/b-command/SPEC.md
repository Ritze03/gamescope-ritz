# Direction B — "Command-first": the Ritz Console

**Date:** 2026-08-23 · **Status:** proposal, one of five parallel directions · **Scope:** the
settings overlay only (`src/Overlay/`, `src/SettingsOverlay.cpp`). The always-on System Monitor
HUD drawn over the game is untouched by this document except where its *settings* live.

> **Core idea in two sentences.** Every setting in gamescope-ritz registers itself once as a
> searchable, addressable entry; that registry is simultaneously what search queries, what
> browse walks, what the gamepad drives, and what the renderer draws. There is exactly one
> surface — the Console — and exactly one row grammar inside it, so an inconsistent screen is
> not something a call site is *able* to build.

Read alongside:

- `API.md` — the helper layer, C++ sketches, call sites.
- `FEASIBILITY.md` — honest assessment against immediate-mode ImGui, `InputText`, gamepad, migration cost.
- `index.html` — interactive mockup with real project content.

Departures from `superdoc/planning/ui-mockup-precise-spec.md` and `slider-widget-spec.md` are
listed explicitly in §5. Everything not listed there is inherited unchanged.

---

## 0. What this replaces, and why

Today: a bottom dock launching six free-floating windows (`chrome::BeginPanelWindow`,
`chrome::DrawDock`), each window hand-laying-out its own content, three of them carrying their
own `ImGui::BeginTabBar` with unrelated internal shapes. The user's diagnosis is exact: *"Every
module/window looks soooo different. Nothing is really consistent."*

The structural cause is not sloppiness — it is that **each panel is a free-form drawing
surface**. `PanelDisplay.cpp` (778 lines), `PanelConfig.cpp` (914), `FpsDisplay.cpp` (2469) each
decide their own layout, their own spacing, their own grouping, their own use of stock vs.
custom widgets. `slider-widget-spec.md` §8 records the consequence in one line: seven sliders in
the System Monitor are still stock `ImGui::SliderFloat()` while every other panel routes through
`widgets::SliderControl()`. That divergence is not a bug anyone introduced; it is what a free-form
surface produces over time.

This direction removes the free-form surface. There is one list, one row, one control column.
A panel author no longer *has* a canvas.

### 0.1 The four things that make this direction cohere

1. **The blur is a full-screen compositor pass, not a per-window effect.**
   `PanelConfig.cpp` drives `FrameInfo_t::blurRadius` and a darkening multiply on the *game
   layer*. There is no per-window `backdrop-filter` and there never will be. Six floating windows
   over a uniformly blurred field is fighting the technology; **one centred surface over it is
   exactly what the technology draws well.**
2. **The overlay already has layout-correct text input.** `wlserver_dispatch_key()` resolves
   UTF-8 via `xkb_state_key_get_utf8()` against the real keyboard and hands it to
   `SettingsOverlay_QueueKeyEvent(…, sUtf8Text)`, which the consumer feeds to
   `io.AddInputCharactersUTF8()`. Typing works today, on any layout. A search-first design is
   not asking for new plumbing; it is asking to *use* plumbing that already exists and is
   currently used by exactly one widget (the profile-name field).
3. **Mid-game, the user wants one setting, not a tour.** The dock is optimised for exploring six
   subsystems. The actual mid-game act is "the game looks soft — sharpen it" or "cap it at 90".
   Two keystrokes beats four clicks and a drag across a floating window.
4. **A registry that powers search is the same registry that enforces consistency.** This is the
   direction's structural advantage and the reason it can deliver the user's *actual* goal (a
   helper layer that makes it easy for an AI to extend the UI consistently). One declaration
   feeds search, browse, gamepad, rendering, reset-to-default, config routing, and the docs.

---

## 1. Navigation

### 1.1 The surface

**Exactly one window.** A centred Console, `min(900px × display_scale, 0.62 × output width)`
wide, `min(auto, 0.82 × output height)` tall, anchored at 9 % from the top of the output. Not
draggable, not resizable, not closable-individually. It has three fixed regions, top to bottom:

```
┌──────────────────────────────────────────────────────────────┐
│ ⌕  [Display ×]  sharp▌                          1 match      │  Query line   14u
├──────────────────────────────────────────────────────────────┤
│                                                              │
│   DISPLAY  4                                                 │  Body         flex
│ ● Sharpness                                5  ▇▇▇▁▁▁▁▁       │
│   Pre-Sharpen                            off  ▭              │
│   …                                                          │
│                                                              │
├──────────────────────────────────────────────────────────────┤
│ FSR / NIS sharpening strength. Higher is sharper; too high…  │  Inspector    16u
│ display.sharpness   0 – 20 · step 1   default 2              │
├──────────────────────────────────────────────────────────────┤
│ ↑↓ move   ←→ adjust   ↹ scope   ⏎ open   esc clear           │  Legend        7u
└──────────────────────────────────────────────────────────────┘
```

The query line and the Inspector **never move and never change height**. Only the Body swaps
content. That is the entire "where am I" mechanism: the chips plus the query text always read out,
literally, what is on screen.

### 1.2 Query grammar

| Input | Effect |
|---|---|
| *(empty)* | **Home**: Recent (this session) → Pinned → Area tiles with live summaries |
| `sharp` | fuzzy filter across every entry's title, then keywords/area/id |
| `↹` on a result | promotes that result's **area** to a chip; query clears; body becomes that area's full ordered list |
| `⌫` at column 0 | pops the last chip |
| `>` | commands (`Reset this area`, `Reload ReShade effects`, `Take screenshot`, `Copy log`, `Close overlay`) |
| `/` | LOG filter — the body becomes the Stream Pane, the query line *is* the log filter |
| `#` | pinned entries only (the Quick Wheel contents) |
| `?` | sigil legend + keybind sheet |
| `esc` | clear query → pop chip → close console (three presses maximum, from anywhere) |

**Browsing is search with an empty query.** There is no separate browse code path, no separate
browse layout, no separate browse styling. This is the single most important navigational claim
in this document, because it is what guarantees the fallback path cannot drift from the primary
path.

### 1.3 Discovery — the answer to "settings I can't name"

A search-first UI fails when the user does not know the word. Four mechanisms, all registry-derived:

1. **Home names everything.** The empty query is not a blank slate; it lists all six areas with
   an entry count and a live one-line summary (`Display · 24 entries · fsr · stretch · 184 fps`).
2. **Scoped browse is a complete list.** Entering an area shows every entry in registered order
   under group headers — the same information density as today's panel, with none of today's
   per-panel layout variance.
3. **Keywords are a first-class registry field.** `.Keywords("sharpen fsr rcas cas crisp clarity")`
   on `display.sharpness` means "crisp" finds it. Matching a keyword (rather than the title)
   scores lower and highlights nothing, so title matches always float above.
4. **Fuzzy is subsequence, not prefix.** `flm` finds "FPS Limit"; `advbrt` finds "Adaptive
   Brightness". Word-start and consecutive-character bonuses keep the ranking sane.

### 1.4 Detail views: Panes

Some content is not a row. A **Pane** replaces the Body — *not* the query line, *not* the
Inspector — and pushes a chip so `esc`/`B` pops it. Six pane kinds exist and no seventh may be
added without extending `PaneCtx` (§4):

| Pane | Used by |
|---|---|
| **List pane** | choice with > 8 options; profiles; audio streams. Rows, searchable via the same query line. |
| **Stream pane** | LOG. Monospace lines, severity gutter, `/`-filtered. |
| **Gauge pane** | System Monitor live read-out. Card grid, hero numbers, sparklines. |
| **Grid pane** | the 3×3 position picker at full size. |
| **Text pane** | the one place `ImGui::InputText` is used — profile naming, with a suggestion list. |
| **Confirm pane** | destructive actions. Two rows: `Delete permanently` (danger) / `Cancel`. |

### 1.5 Input paths

**Keyboard** (primary): `Ctrl+Shift+O` opens with the query line focused. `↑↓` move, `←→` adjust
the selected value **in place**, `↹` scope, `⏎` open pane / run action, `esc` unwind, `^P` pin,
`^R` reset entry to default, `^⇧R` reset area, `^C` copy the selected entry's id (or log line).

> **Deliberate trade:** `←→` belong to *value adjustment*, not to the text caret. In a command
> palette you type and backspace; you do not edit mid-string. Caret movement, when needed, is
> `Home`/`End`/`^←`/`^→`. This buys single-keystroke value adjustment while typing — the single
> most valuable interaction in the whole design — at the cost of an interaction nobody performs.
> It is also the reason the query line is hand-rolled rather than `ImGui::InputText` (see
> `FEASIBILITY.md` §2).

**Pointer**: rows are clickable (click selects), controls in the control column are directly
draggable/clickable, chips have an `×`, area tiles are buttons, clicking outside the console
closes it. A pointer user never has to type: Home's area grid is the front door.

**Gamepad**: see §3. Short version — the pad never types, and the pad opens a different *posture*
of the same console.

---

## 2. Option presentation

### 2.1 The Row — the rule that makes inconsistency hard

> **Every option is one Row. Every Row is the same three-column grid at one of exactly three
> heights. A control type may only change what is painted in the control column — never the row
> height, the label position, the padding, or where help text goes.**

```
grid-template-columns:  6u gutter | flex label | 50u control
heights:                10u compact  ·  14u expanded (selected + type opts in)  ·  0 hidden
```

- **Gutter (6u)** — status affordance only: a 6×6 `accent/base` square when the entry has been
  changed this session, or when the row is selected. Nothing else may occupy the gutter. This is
  the only per-row decoration in the entire design.
- **Label (flex)** — Geist Sans 14 @ `text/label`, ellipsised, plus an optional `path` suffix in
  Mono 11.5 @ 28 % when the list is unscoped (`Sharpness · display`).
- **Control (50u fixed)** — right-aligned, Mono for everything numeric. Fixed width is what makes
  147 rows form a clean vertical value column instead of a ragged edge.

**Help text is never in a row.** It is in the Inspector, for whatever is selected. There are
**no tooltips anywhere in this design** — tooltips are the classic mechanism by which two screens
end up looking different, and they are unreachable on a gamepad anyway.

### 2.2 Complete control taxonomy

| Type | Control column | `←→` semantics | `⏎` |
|---|---|---|---|
| **Switch** | state word (`on`/`off`, Mono 11.5 @ 44 %) + 30×15 track, 11×11 knob | flips | flips |
| **Action** | verb chip (`SAVE`, `RUN`, `COPY`) — accent-16 % fill, Mono 500 11.5 | — | runs |
| **Destructive action** | same, `state/danger` roles | — | opens Confirm pane |
| **Segmented** (≤ 4 opts) | inline cells, 3u gaps, lowercase labels, active = accent-24 % fill / accent-60 % border / Mono 600 `accent/onfill` | moves one cell | moves one cell |
| **Choice** (> 4 opts) | current value Mono 500 16 `accent/value` + `▾` | cycles in place | opens List pane (searchable) |
| **Slider, collapsed** | value + unit + **118u mini-track** (6u tall, 3u radius, gradient fill, 6×14 handle) | steps by `.Step().coarse` | expands |
| **Slider, selected** | row grows to 14u; label column carries a full-width track with the shipped `SliderControl()` geometry (6px track, 10×22 handle, glow); control column carries value Mono 600 18 above min/max marks Mono 11.5 @ 38 % | steps; `L2`/`Shift` = fine step | collapses |
| **Stepper** | value (or `.ZeroMeans()` word, neutral) + `− +` glyphs | ± one step | opens numeric entry (keyboard only) |
| **Multi-select** | **chip bank** — one small chip per option, on-chips accent-22 % | moves the *chip cursor*; `space`/`X` toggles | opens List pane in multi mode |
| **Hue** | value `218°` + 118u OKLCH hue rail with a white cursor | ± 2° | opens Colour pane |
| **3×3 position** | word (`top-right`, neutral) + a 3×3 dot proxy glyph | cycles the nine in reading order | opens Grid pane |
| **Text** | value Mono 13 ellipsised + `✎` | — | opens Text pane |
| **Read-out** | value at `text/meta` alpha, never editable-looking; optional 6×6 status dot in the gutter | — | — |
| **Pane** | summary (`6 gauges`) + `▸` | — | opens the pane |

Two types are **abolished outright**:

- **Tabs.** All fifteen tab items across four `BeginTabBar()`s become areas, sub-areas or group
  headers. Segmented controls survive only as *value* pickers, never as navigation. This removes
  the single largest source of "these two screens look different".
- **Bare checkbox.** `widgets::Checkbox()`'s only caller is the System Monitor's module list,
  which becomes a chip bank. One boolean affordance in the whole UI: the switch.

### 2.3 Grouping

Rows are grouped by the `group` string on the entry. A group header is 8u tall, Mono 10.5 @ 36 %,
uppercase, indented to the label column, with an optional count at 22 %. **Group blocks —
`widgets::BeginGroupBlock()`'s bordered, filled boxes — are gone.** A box around a group is
redundant when every row already shares a rigid grid, and boxes are precisely what let each panel
invent its own nesting depth. Separation is by header + rhythm, not by chrome.

Ordering inside an area is `.Order()`, defaulting to registration order. Ordering *between*
areas is registration order in `RegisterAll()`.

### 2.4 Spacing rhythm

One unit: **`u = 4px × display_scale`**. Every dimension is an integer multiple, and **no call
site ever names a pixel**.

| | |
|---|---|
| Row height | 10u compact / 14u expanded |
| Row vertical padding | 1u |
| Gutter width | 6u |
| Control column | 50u (58u at `display_scale ≥ 1.6`) |
| Console padding (h) | 3.5u |
| Query line | 14u |
| Group header | 8u, +1.5u gap below |
| Inspector | 16u (12u at `display_scale ≤ 0.7`, dropping to 2 lines) |
| Legend bar | 7u |
| Stream line | 5u |
| Gauge card gap | 0.75u |
| Mini-track | 118u × 1.5u, radius 0.75u |

### 2.5 Disabled, not hidden

An entry whose `.EnabledWhen()` predicate is false renders at **34 % row opacity** with neutral
(white) control fills — the existing rule from `ui-mockup-precise-spec.md` §12 — and the
Inspector appends *"— inert: HDR Output is off."* It is still searchable and still selectable.

**Rationale:** hiding a setting the user can *search for* destroys the trust a search-first UI
runs on. "I typed tonemap and got nothing" is indistinguishable from "the feature doesn't exist".
Showing it inert, with the reason, answers the real question.

---

## 3. The gamepad

This is the hardest question for this direction and it gets three answers, in the order a player
actually reaches for them. Note first: **`ImGuiConfigFlags_NavEnableGamepad` is never set in this
codebase and no pad reader is wired to the overlay** — gamepad support is net-new under *any*
direction. This direction's claim is that it specifies it once, for one row grammar, instead of
six times for six panel layouts.

### 3.1 Path 1 — the Quick Wheel (the real mid-game path)

**Hold `L2`.** The console does not open. Up to eight **pinned** entries appear as a radial at
screen centre, over the still-running, un-blurred game. Right stick selects a spoke; stick-X
applies the *same* `←→` semantics the keyboard uses, live. Release commits.

- Pins are a registry flag (`.Pinnable()`, toggled with `^P`/`Y`). Home's "Pinned" section and
  the wheel are the same eight entries.
- Each spoke is drawn by the entry's **own value renderer** — the same function the row uses,
  called with a radial layout box. No second implementation of "how a slider looks".
- Value changes commit live (every setting in this overlay already commits live), so a spoke is
  a real-time control, not a form.

This is the answer to "adjusting a setting mid-game with a controller". It is faster than the
current dock and faster than typing.

### 3.2 Path 2 — Browse posture

Opening with the pad (`Select+Start`, or the Deck's `…`) opens **the same console** with the
query line collapsed to a breadcrumb bar and the body starting on the area grid. Five verbs,
identical on every row — a promise only a uniform taxonomy can make:

| Button | Verb | Why it is total |
|---|---|---|
| D-pad `↑↓` | move | every row is 10u and vertically stacked; there is never a horizontal row group |
| D-pad `←→` | **change this value in place** | every value type defines a total order and a step. Sliders step, choices cycle, switches flip, chip banks move the chip cursor, the 3×3 grid cycles in reading order. Nothing has to be "entered" first, so there is no in-control/out-of-control mode |
| `A` | open pane / run | the row *shows* whether it has one (`▸`, `▾`, verb chip) |
| `B` | back — pops one chip, then closes | the chip stack **is** the nav stack; there is no other |
| `X` | reset to default | every entry must declare `.Default()` — the registry refuses one that does not |
| `Y` | pin / unpin | pinning is a registry flag, not a per-panel feature |
| `L1`/`R1` | previous / next group header | groups come from the registry, so paging is uniform |
| `L2` + `←→` | fine step | every numeric entry declares `.Step(coarse, fine)` |

The "no modes" property is the crux. In a conventional settings UI a pad user must focus a
control, press A to enter it, adjust, press B to leave. Here `←→` always adjusts whatever is
selected, because the taxonomy guarantees every type is adjustable by a signed step.

### 3.3 Path 3 — searching *with* a pad, when you want to

Every area's last row is **`Search within… [A]`**. It opens a three-ring letter wheel: left stick
picks a ring of 9, right stick picks the letter, `A` commits, `B` backspaces. ≈ 0.6 s per
character.

That is only acceptable because search narrows fast, and the registry lets us *prove* how fast:
over the 147 shipping entries, **2 characters median / 3 characters at p90** puts the target in
the top three results. The live match count sits next to the wheel so the user stops the moment
it reads `1 match`, and `X` accepts the top match and drops straight into adjusting it.

**This claim is enforced, not asserted.** `Registry::SelfTest()` computes the collision profile at
startup (debug builds) and fails if any new entry's 3-character prefix set collides with more
than three others without a distinguishing keyword. Search quality becomes a build-time
invariant rather than a hope.

### 3.4 Text on a pad

Two hard rules, enforced by the API:

- **`area.Text()` requires either `.Suggestions(fn)` or `.KeyboardOnly()`.** There is no third
  option; the builder asserts. Profile naming supplies generated suggestions (`Cyberpunk –
  Quality`, `Cyberpunk – 60 fps`, `Cyberpunk – Battery`), selectable without typing a character.
- **There is no "type a number" control.** `area.Number()` renders a stepper; raw numeric entry
  is a keyboard-only affordance behind `⏎`.

Net effect: a pad user can reach and change **every entry in the registry** without ever typing.

---

## 4. Styling and theming

### 4.1 Colour roles

Call sites name a **role**; roles resolve through the live OKLCH accent family (`Palette.cpp`,
hue from `OverlaySettings::accent_hue`) or the neutral alpha ladder. **The helper exposes no
colour literal to a call site.** That is not a guideline; `PaneCtx` has no `ImU32` parameter
anywhere in its surface.

| Role | Value | Use |
|---|---|---|
| `surface/base` | `rgba(9,10,12, opacity_console)` | the console |
| `surface/raised` | white 5 % | query line, gauge cards, chips |
| `surface/selected` | accent 10 % + 2u `accent/base` left edge | the selection cursor |
| `surface/pressed` | accent 16 % | active/pressed |
| `line/hairline` | white 10 % | every divider |
| `line/dim` | white 6 % | card borders |
| `text/primary` | `#EFF5FB` @ 92 % | selected label, hero numbers |
| `text/label` | @ 62 % | every unselected label |
| `text/meta` | @ 36 % | group headers, ids, units, paths |
| `text/mark` | @ 38 % (`kMarkAlpha`) | slider min/max |
| `rail` | white 16 % (`kRailAlpha`) | unfilled track |
| `accent/base` | `oklch(.74 .12 h)` | fills, edges, dots |
| `accent/gradHi` | `oklch(.78 .12 h)` | track gradient end |
| `accent/value` | `oklch(.84 .10 h)` | **every number** |
| `accent/text` | `oklch(.82 .10 h)` | chip text, link-ish labels |
| `accent/onFill` | `oklch(.90 .07 h)` | text on an accent fill |
| `accent/handle` | `oklch(.90 .05 h)` | slider handle, switch knob |
| `state/ok` | `oklch(.78 .16 145)` | live/healthy dots |
| `state/warn` | `oklch(.72 .17 55)` | frametime outliers, log warnings |
| **`state/danger`** | **`oklch(.68 .18 25)` — NEW** | destructive actions, log errors |
| **`match/highlight`** | **`accent/value` + 1.5px underline @ 45 % — NEW** | matched characters |

`state/danger` is new because the source spec explicitly records (§14) that no error colour
exists anywhere in the handoff. A search-first UI that surfaces `Delete Saved Config` alongside
`Save as new profile` in the same list *must* differentiate them visually — the affordance is no
longer buried in a panel the user deliberately opened.

`match/highlight` uses an underline rather than a weight change: no second Sans weight to bake
into the atlas, and it survives at `display_scale 0.5` where a 400→500 weight step is invisible.

### 4.2 Elevation: exactly two levels

- **Level 0 — the game**, blurred (`FrameInfo_t::blurRadius`) and veiled, by the native
  compositor pass that already exists and is already user-tunable (`background_blur` 1.0,
  `background_darkening` 0.8).
- **Level 1 — the Console.** Panes live *inside* the console body; they do not float.

That is the complete list. Consequences worth naming:

- No z-order, no per-window focus, no focused-vs-unfocused opacity pair, no drop shadows stacking
  on drop shadows, no "which window is on top" bug class.
- `opacity_windows_focused` (1.00) and `opacity_windows_unfocused` (0.90) collapse into one
  `opacity_console`. `opacity_dock` (0.70) retires with the dock. `blur` and `darkening` keep
  their exact current meaning and default.
- **Selection *is* focus.** Keyboard, gamepad and pointer share one cursor, so the invented
  "keyboard focus = 1px accent @ 60 % border" state from spec §12 is not needed and is dropped.

### 4.3 Type scale (Geist)

Nine roles, two families, three weights — two *fewer* than `Fonts.h`'s current ten, because the
8px `DockHotkey` face retires with the dock, paying for two new roles.

| Role | Face | px @ 1.0× | Use |
|---|---|---|---|
| `Query` | Mono 400 | **20** | the typed query — **NEW**; the largest text in the UI because it is the focus |
| `Display` | Mono 600 | 30 | gauge-card hero numbers |
| `Hero` | Mono 600 | 18 | HUD FPS number, expanded slider value |
| `Value` | Mono 500 | 16 | every number in a control column *(existing — `slider-widget-spec.md` §7)* |
| `Label` | Sans 400 | 14 | every row label *(existing)* |
| `Section` | Sans 500 | 13 | Inspector help text |
| `Stream` | Mono 400 | **12.5** | LOG lines — **NEW** |
| `Chip` | Mono 500 | 11.5 | area chips, segment cells, state words, verb chips |
| `Meta` | Mono 400 | 11.5 | group headers, ids, units, min/max marks |

**Hard rule inherited unchanged:** Mono carries every number, unit, state word, path and id; Sans
carries prose only. Because Geist Mono is genuinely monospaced, the value column is tabular for
free — which is why a 147-row list does not shimmer while a slider is being dragged.

### 4.4 State treatment

| State | Treatment |
|---|---|
| idle | row transparent; label `text/label`; control at resting colours |
| hover | row fill white 4 %; nothing else |
| **selected** | row fill accent 10 %, 2u accent left edge, label → `text/primary`, gutter dot lit |
| active/pressed | row fill accent 16 %; **no geometry change** |
| disabled | whole row × 0.34 alpha, control fills neutral white; never hidden; Inspector states the failing predicate |
| changed this session | 6×6 `accent/base` dot in the gutter |

### 4.5 Motion

Three durations — `instant` 0, `quick` 90 ms, `settle` 160 ms — one easing, applied
frame-rate-independently as `1 − exp(−dt · rate)` rather than a naive lerp against
`io.DeltaTime` (which changes perceived speed between 30 and 240 fps — a real hazard here).

**Exactly four animations exist and the helper owns all four:**

1. selection cursor sliding between rows — `settle`
2. row expanding 10u → 14u on selection — `settle`
3. a value bar filling to a new value — `quick`
4. console fade-in on open — `quick`

An animation not on this list **cannot be written**, because `PaneCtx` exposes no timing
primitive and no call site has a draw list. Query → results is deliberately *not* animated: text
you are reading must not move.

### 4.6 Scale: 0.5× – 2.0×

Everything is `u = 4px × display_scale`, so the whole design scales by construction. Two
documented departures at the extremes, **both decided by the helper, not by a caller**:

- `display_scale ≤ 0.7` — Inspector drops 3 lines → 2; group-header counts suppressed.
- `display_scale ≥ 1.6` — control column widens 50u → 58u so the expanded slider's min/max marks
  fit without ellipsis.

Console geometry is `min(900px × scale, 0.62 × output width)` by `min(auto, 0.82 × output
height)`, so at 2.0× on 1080p it still clears the System Monitor HUD in its default top-right
anchor.

### 4.7 Legibility over an arbitrary game

The console sits on `surface/base` at `opacity_console` (default 1.00) over a game layer that is
blurred *and* darkened by the compositor pass. Three guarantees:

1. Every text role is a near-white or accent tone at ≥ 36 % over a ≥ 88 %-opaque near-black
   surface — worst-case contrast is a surface property, not a game property.
2. The only place game pixels reach the eye near text is outside the console, where nothing is
   drawn.
3. If `opacity_console` is lowered by the user, the helper raises `text/label` from 62 % → 72 %
   and `text/meta` from 36 % → 46 % on a ramp below 0.85. Readability is not left to the user's
   opacity slider.

---

## 5. Departures from the existing specs

| Existing spec | This direction | Why |
|---|---|---|
| §4/§8 floating windows + dock | one Console; dock removed | the blur is a full-screen pass; one surface is what it draws well, and one surface cannot be internally inconsistent |
| §5 title bar (34px, dot, meta, collapse/close glyphs) | removed | there is one window; it has no title bar, no collapse, no close glyph. `Esc` closes |
| §6 group blocks (fill/border boxes) | removed; headers + rhythm only | boxes are what let each panel invent nesting depth |
| §7 slider | **kept exactly** as the *expanded* form (`slider-widget-spec.md` geometry, 6px track / 10×22 handle / 8px gaps / `kRailAlpha` / `kMarkAlpha`); a new **collapsed** 118u mini-track form added | a result row must show a slider's value without costing 14u |
| §7 checkbox | removed | one boolean affordance (the switch); the sole caller becomes a chip bank |
| §7 segmented | kept, ≤ 4 options only | > 4 goes to a searchable Choice pane |
| §9 tooltip | removed | Inspector replaces it; tooltips are unreachable on a pad and are a consistency leak |
| §11 FPS config window | dissolved into `monitor` area rows + one Gauge pane | separates the live display from its configuration |
| §12 keyboard-focus ring | dropped | selection *is* focus |
| §1 no error colour | `state/danger` added | destructive actions now appear in general search results |
| §2 typography (10 roles) | 9 roles; `Query` and `Stream` added, `DockHotkey` removed | net atlas saving |
| §14 "whether we dim the game is a product decision" | **decided: yes** | already implemented and user-tunable (`background_darkening` 0.8) |

Inherited without change: the OKLCH accent family and its live hue, the alpha ladder, the
Mono-carries-numbers rule, `kRailAlpha`/`kMarkAlpha`/`kMetaTextAlpha`, disabled-row dimming at
34 %, the letter-spacing skip, the "never show a value the overlay didn't verify" read-out
treatment.
