# Direction A — "CONSOLE"

**One pane. No windows. Depth by drilling in, never by opening something else.**

Date: 2026-08-23. Companion files: `API.md` (the helper layer), `FEASIBILITY.md` (what
fights immediate mode), `index.html` (the mockup — five real screens).

---

## 0. The idea, and what it deletes

Today the overlay is a *desktop*: a dock spawns floating windows, each window has a title
bar, a drag handle, a collapse glyph, a close glyph, a focus state, a tiled default
position, and its own internal layout language. Six windows × six layout languages is
exactly the "every module looks soooo different" complaint. The windowing system is not
carrying its weight — nobody arranges gamescope panels the way they arrange a desktop.

So this direction deletes the window manager entirely.

There is **one console**: a fixed, centred, non-movable, non-resizable pane. It has a
**Rail** (the master column of sections) and a **Stage** (the detail area). You are always
in exactly one place. You get there by moving down the rail and into the stage; you get
deeper by drilling into a row; you get back with B/Esc. That is the whole interaction
model, and it is the model a Steam Deck, a TV picture menu and a game options screen all
already use, because it is the one that works with a controller in your hands.

What disappears with the windows: title bars, drag, collapse, close-per-panel, focus
rings between windows, tiled default positions, the dock, the "which window is on top"
Z-order problem, per-panel opacity, and roughly 1,600 lines of `Chrome.cpp`.

What replaces "two panels side by side": nothing side-by-side. Two mechanisms instead —
the **header live cluster** (always-visible cross-cutting readouts: fps, frametime, output
resolution, HDR state) and the **Well** (a screen-declared, always-visible region pinned
above that screen's scrolling row list — System Monitor's live HUD preview, Audio's peak
meters, Shaders' A/B strip). A screen that needs a companion view *owns* one, instead of
the user having to arrange two windows to get it.

**Dark only.** A light theme is explicitly out of scope and is not a hedge: this UI is
composited over a game at whatever brightness the game happens to be, usually in a dark
room, usually at night. A light console is a flashbang. The accent hue is the user-facing
theming axis; the surface stays near-black.

---

## 1. Navigation

### 1.1 Anatomy

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ ■  CONSOLE › GAMESCOPE › Upscaling   ▮▮▯      [app 1245620]  fps 142 ...     │ header 56px
├──────────────────┬──────────────────────────────────────────────────────────┤
│ CONSOLE          │ [Upscaling][Display][Frame Limiter][HDR]  LB/RB · section │ subtabs
│ ▎▣ Gamescope  12 │ ─────────────────────────────────────────────────────────  │
│   ▣ Shaders    9 │  SCALING ───────────────────────────────────────────────  │
│   ▣ System M. 21 │ ▎Filter                       [linear][nearest][fsr]…     │ stage
│   ▣ Audio      5 │  Scaler                                    auto ›         │
│   ▣ Config    18 │  Sharpness                    ▰▰▰▰▱▱▱▱  40 % sharper      │
│   ▣ Log          │  …                                                        │
│                  │                                                           │
│ v3.16.14         │                                                           │
├──────────────────┴──────────────────────────────────────────────────────────┤
│ Ⓐ Adjust  Ⓑ Back  Ⓨ Reset to default  Ⓧ Save profile  / Find    CTRL+SHIFT+O │ legend 52px
└─────────────────────────────────────────────────────────────────────────────┘
```

Fixed size: `min(1188px, 94vw) × min(768px, 90vh)` at `display_scale` 1.0, both derived
from the spacing unit (§2.1) so they scale with everything else. Centred. Never moved,
never resized, never more than one.

### 1.2 The depth stack

Exactly three levels, and the header's **depth pips** (`▮▮▯`) say which one you're on.

| Level | What has focus | Rail | Back goes to |
|---|---|---|---|
| **L0 Rail** | a section in the rail | expanded, focused item has the accent tick | closes the console |
| **L1 Stage** | a row in the current screen | expanded, current section shows `sel` (accent fill + edge, no tick) | L0 |
| **L2 Drill / Sheet** | a row in the pushed screen, or an option in a sheet | see below | L1 |

L2 comes in two flavours, and the kit picks — the caller never does:

- **Drill screen** — a whole sub-screen (`ui::Drill(...)` → another registered screen).
  The breadcrumb grows a segment. If the screen declares `Stage::Wide` (LOG), the rail
  animates from 248px down to a 64px icon strip so the content gets the full width.
- **Sheet** — a right-hand overlay panel for *picking one thing*: dropdown options,
  colour, a long list, a multi-select. It slides in from the right over a scrim, traps
  focus, and dismisses on B/Esc/pick. Sheets never stack — a sheet cannot open a sheet.

There is no L3. If a screen wants to go deeper, that is a signal it should have been two
sections.

### 1.3 Input paths

**Gamepad** is the primary target; everything else is derived from it.

| Input | Action |
|---|---|
| D-pad / left stick ↑↓ | move focus one row (or one rail item at L0) |
| D-pad ← → | **adjust the focused control in place** — slider step, segmented cycle, switch flip. No "enter the control" step. |
| A | activate: drill, open sheet, run action, begin inline text edit |
| B | back one level; at L0 close the console |
| X | screen's context action (Copy in LOG, Save profile in Config, Apply in Profiles) |
| Y | reset the focused row to its default (a per-row facility the kit gets for free from the config schema) |
| LB / RB | previous / next **section** — jumps the rail from anywhere, including L2 |
| LT / RT | previous / next **sub-tab** within the current screen |
| Start / Select | close the console |

**Keyboard** mirrors it: ↑↓ rows, ←→ adjust, Enter = A, Esc = B, Tab/Shift+Tab = section,
PgUp/PgDn = sub-tab, `Y` = reset, `/` = **Find**.

**Find** (`/`) is the sleeper feature this architecture unlocks: because every row goes
through one registered call, the kit knows every setting's label and which screen it lives
on. `/` opens a sheet, you type "sharp", it lists *Sharpness (Gamescope › Upscaling)*,
*Pre-Sharpen strength (Shaders)*, *Backdrop rounding* — pick one and the console navigates
there and lands focus on that exact row. (See `FEASIBILITY.md` §4 for how the index is
built without a side-effect-free probe pass.)

**Pointer**: click a rail item, click a row (a row is one hit target — clicking anywhere
on it focuses it and activates its control), scroll the stage, drag a slider, click a
sub-tab. Nothing is smaller than 44 physical px at `display_scale` ≥ 0.85; below that the
console is being used with a mouse on a desktop anyway.

Mouse hover and gamepad focus are **visually distinct** (hover = fill only; focus = fill +
accent tick) so both can be on screen simultaneously without lying about where input will go.

### 1.4 "Where am I" — three redundant answers, always

1. **Breadcrumb** in the header: `CONSOLE › GAMESCOPE › Upscaling` (`› HDR metadata` at L2).
2. **Rail state**: the current section is accent-filled with a 3px accent left edge.
3. **Focus tick**: exactly one row on screen has the 3px accent left edge + glow + raised
   fill. There is never zero and never two.

Plus the depth pips and the routing badge (`app 1245620` / `global`), which answers the
*other* "where am I" — where does this edit land.

---

## 2. Option presentation

### 2.1 The one rule

> **Every setting is a Row. A Row is `[tick] [label (+help)] ……… [control in the control
> column]`. The caller supplies the label, the value pointer and (optionally) a help
> string. The kit supplies everything else: height, padding, fonts, colours, the control
> column's width and position, the divider, the focus treatment, the disabled treatment.**

The control column is a fixed width (`--ctl-w` = 56u = 224px @1.0) flush to the right edge
of the stage. Every control kind renders *inside that column*, right-aligned. That single
constraint is what makes the screens rhyme: a screen of twelve settings has twelve
identical label positions and twelve identical control positions, no matter that it mixes
switches, sliders, pickers and readouts.

Consequences that are load-bearing:
- There is **no way to place a control anywhere else.** The kit exposes no cursor, no
  `SameLine`, no `Dummy`, no width argument.
- A row with a long control (5-segment segmented control, text entry) gets a **wide control
  column** (76u) — decided by the kit from the measured content, not requested by the caller.
- Rows are `44px` tall (11u) — one line. With a help sub-line they grow to the content;
  they never shrink.

### 2.2 Spacing rhythm

One unit: `u = 4px × display_scale`. Nothing in the console is a non-multiple of u.

| Thing | Value |
|---|---|
| Row height (no help) | 11u (44) |
| Row inner vertical padding | 1.5u |
| Row horizontal padding inside the stage | 3u |
| Stage edge padding | 6u (24) |
| Gap above a Group heading | 6u |
| Gap below a Group heading | 1u |
| Rail item height | 12u (48) |
| Header height | 14u (56) |
| Legend height | 13u (52) |
| Control gap inside the control column | 2u |
| Row divider | 1px `#FFF @ 4.5%`, full stage width |

**Grouping**: rows are grouped by a `Group` heading — Mono 500, uppercase, meta alpha,
followed by a hairline rule that runs to the right edge. Groups are separated by *rhythm*,
not by nested boxes.

> **Departure from `ui-mockup-precise-spec.md` §6 (group blocks):** the boxed group with
> its own fill, border and 12px padding is dropped. Inside a floating window a card
> makes sense — the window is the room and the card is the furniture. Inside a full-bleed
> console the card is a second frame drawn inside the first frame, and twelve of them turn
> one clean pane into a scrapbook. The console *is* the box. Sections inside it are
> separated by a heading and space. The "featured/active group" 2px accent left edge role
> is not lost — it moves to the focused **row**, which is a more useful place for it.

### 2.3 Complete control taxonomy

Every kind, what it looks like, and where it can appear.

| # | Kind | Control-column rendering | Adjust with ←→ | Activate with A |
|---|---|---|---|---|
| 1 | **Switch** | 44×24 track, square, 16×16 knob. On: accent@30 fill, accent@55 border, `--accent-knob` knob right. Off: white@7 fill, white@16 border, white@55 knob left. | flips | flips |
| 2 | **Segmented** | equal cells, 3u gap, lowercase Mono. Active: accent@30 fill / accent@55 border / `--accent-seg` 600. Inactive: white@4 fill, white@8 border, text @50%. | cycles | cycles |
| 3 | **Picker** | current value in Mono `--accent-value` inside a white@5 box + `›`. | cycles through options | opens the **option sheet** |
| 4 | **Slider (compact)** | `[track ────▰──] value` — 6px track, gradient fill, 10×20 handle with glow, Mono accent value right-aligned in a fixed 17u value slot. | steps (fine step on a modifier / trigger held) | begins a drag-with-stick mode |
| 5 | **Number / text entry** | value in Mono right-aligned in a white@5 box. | — | cell becomes an inline edit field *in place* (no popup); Enter commits, Esc reverts |
| 6 | **Action** | uppercase Mono verb in an accent-tinted outline button ("RESET", "COPY"). Destructive variant uses the `danger` role. | — | runs; destructive actions require a second A within 2s ("press again") |
| 7 | **Drill** | right-aligned Mono **summary** (dim) + `›`. **The summary is mandatory** — a drill row must say what's inside ("on · 0.50", "4 · Handheld 40 Hz active"). | — | pushes the sub-screen |
| 8 | **Readout (read-only)** | Mono @46% right-aligned, optional 6×6 square status dot (`ok` / `warn` / `idle`) before it. Never looks pressable. Still focusable (so it can be copied), but its focus tick is dim. | — | copies the value |
| 9 | **Multi-select** | summary "3 of 6" + `›`; the sheet holds checkbox rows. | — | opens sheet |
| 10 | **Colour** | hex/OKLCH in Mono + a 28×20 swatch + `›`; the sheet is a hue strip (L and C locked per token — see §3.5). | shifts hue by 2° | opens sheet |
| 11 | **Chips row** | a row of chips, accent-filled when on, neutral when off ("FPS CPU GPU media"). Read-mostly summary of a multi-toggle set. | — | opens the corresponding sheet |
| 12 | **Position grid (3×3)** | **never in a control column.** It is a *Well control* — a full-width block of nine cells drawn **over the live preview** in the Well (§2.5). | moves the selection | confirms |
| 13 | **Tabs** | **never inside the row list.** The only tab mechanism is the **sub-tab bar** pinned under the breadcrumb, max 6 items, declared by the screen. | — | LT/RT |
| 14 | **Multi-line text view (LOG)** | not a row at all — a `Stage::Wide` **Body kind** (§2.6). | — | — |
| 15 | **Meters / graphs** | Well content only (§2.5). Never a row. | — | — |

Rules that make an inconsistent screen hard to build:

- A number never appears in a Sans run. Every value is Mono. (Kept verbatim from the
  existing spec — it is the single best rule in there.)
- **Help text goes on the sub-line, never in a tooltip.** Tooltips don't exist on a
  controller. If a control's meaning needs more than a label, it gets a help line; if it
  needs more than a help line, it needs a drill screen.
- **Disabled rows stay visible at 38% and explain themselves.** `bDisabled` without
  `pszWhyDisabled` is a debug-build assert. "Allow Tearing — *Unavailable while VRR /
  Adaptive Sync is on.*"
- A row's control never overflows the control column. Debug builds draw the column
  boundary and assert on overflow (`ui.audit 1`).

### 2.4 Sheets

One overlay mechanism, used by picker / colour / multi-select / Find / confirm.

- Right-anchored, 92u wide (368px @1.0), full stage height, `rgba(10,12,15,.97)`, 1px
  white@16 left border, scrim `rgba(3,4,6,.55)` over the stage.
- Header: title (Mono 600 upper) + `B / ESC — BACK` hint.
- Body: `opt` rows — value in Mono, an optional one-line description in Sans meta, a `●`
  mark and accent left edge on the current one.
- Slides in over 160ms (`Approach`, §3.6). Focus is trapped. Picking dismisses.

### 2.5 Wells

A **Well** is a screen-declared, non-scrolling region between the sub-tab bar and the row
list. It is the answer to "how do you preview something without a second window".

- Chrome: white@3 fill, 1px white@8.5 border, a small header row (square status dot,
  uppercase Mono caption, right-aligned meta).
- **System Monitor's Well is the preview.** It shows a patch of the frame with the real
  HUD drawn into it at real scale, and the 3×3 anchor grid overlaid *on the preview
  itself* — the nine cells are the drop zones, and the HUD visibly jumps to the cell you
  pick. You are not configuring an abstraction of the HUD; you are looking at it. Margin,
  font-size, opacity, backdrop and module toggles all move it live in the same frame.
- **Audio's Well** is the L/R peak meters, 20 segments, 2px gaps.
- **Shaders' Well** is the A/B strip.
- A screen may declare at most one Well. A Well never scrolls and never contains Rows.

### 2.6 Body kinds

A screen declares one of three stage shapes; the shape is a property of the screen, not a
per-frame layout decision:

| Kind | Rail | Layout |
|---|---|---|
| `Stage::List` | expanded | Well (optional) + scrolling row list. The default; every settings screen. |
| `Stage::Wide` | collapses to a 16u icon strip | a full-width toolbar + a full-bleed body. **LOG uses this.** |
| `Stage::Split` | expanded | Well takes the top 40u and is pinned; rows scroll below. System Monitor uses this. |

**LOG in detail** (`Stage::Wide`): a toolbar row — source segmented (`gamescope` / `game`),
a Find field with match count, a level segmented (`all` / `warn` / `error`), a `FOLLOW`
chip, a `COPY` action — then a virtualised monospace list (`ImGuiListClipper`). Each line
is `timestamp` (dim) · `[scope]` (accent, or warn/danger by level, fixed 32u column) ·
message (label alpha, wraps). Find matches are highlighted with an accent@30 background.
Because the rail collapses, LOG gets ~1120px of width at 1.0× — wider than any floating
log window ever was.

---

## 3. Styling and theming

### 3.1 Colour roles (roles, not hexes)

| Role | Value | Means |
|---|---|---|
| `surface/base` | `rgba(8,9,11,.86)` over blur(22) saturate(1.1) | the console body — the only opaque-ish thing on screen |
| `elev/1` | white @ 3% | rail, Well, toolbar — "attached furniture" |
| `elev/2` | white @ 6% | focused row |
| `elev/3` | `rgba(10,12,15,.97)` | sheet (it must be readable over its own scrim) |
| `line/hair` | white @ 8.5% | console border, header/rail/well borders |
| `line/row` | white @ 4.5% | row dividers |
| `line/strong` | white @ 16% | off-switch border, sheet left border |
| `text/primary` | `#EFF5FB` @ 93% | focused row label, breadcrumb current, sheet option |
| `text/label` | `#EFF5FB` @ 68% | row labels, log message body |
| `text/meta` | `#EFF5FB` @ 46% | help sub-lines, group headings, readout values |
| `text/disabled` | `#EFF5FB` @ 30% | separators in the breadcrumb, chevrons, timestamps |
| `accent/*` | the existing OKLCH family, unchanged | see §3.5 |
| `state/ok` | `oklch(.78 .16 145)` | read-only status dots only |
| `state/warn` | `oklch(.72 .17 55)` | frametime outliers, warn log lines |
| `state/danger` | `oklch(.70 .17 25)` | **new.** Destructive actions only. |

`ui-mockup-precise-spec.md` §14 notes error/danger "does not exist anywhere in the
handoff". It has to now: this console has a Danger Zone group with "Delete saved config",
"Clear HDR overrides", "Reset all". Rendering those in accent would be a lie. The value is
built from the same OKLCH L/C shape as the accent family so it sits in the same world.

**The accent budget.** Accent means exactly two things and nothing else:
1. *this is where you are* — focus tick, selected rail item, active segment, on-switch;
2. *this is a live number* — every Mono value readout.

Nothing decorative is accent. That rule alone kills most of the "everything looks
different" problem, because it removes the biggest free variable panels currently spend
differently.

### 3.2 Elevation

Four steps, and elevation is *only* white alpha over the blurred base — never a different
hue, never a shadow. The one shadow in the design is the sheet's, because the sheet must
read as being in front of something rather than part of it. (The game behind is already
blurred and darkened by the compositor; a drop shadow against that is noise.)

### 3.3 Typography — 6 roles, all derived from one number

`base = 15px × display_scale`. Geist Sans for prose, Geist Mono for everything numeric,
uppercase or code-ish.

| Role | Family / weight | Size | Used for |
|---|---|---|---|
| `Display` | Mono 600 | 1.6 × base (24) | hero numbers (HUD preview, statistics) |
| `Title` | Mono 600 | 0.78 × base (11.7), UPPER | breadcrumb, sheet header, group headings, legend keys |
| `Body` | Sans 400 | 1.0 × base (15) | row labels, sheet option names |
| `Value` | Mono 500 | 0.90–0.96 × base (13.5–14.4) | every number, every readout, log lines |
| `Meta` | Sans 400 | 0.80 × base (12) | help sub-lines |
| `Micro` | Mono 400 | 0.68–0.74 × base (10–11) | chips, legend labels, timestamps, rail footer |

> **Departure from the current `Fonts.h`:** ten enumerators collapse to six. `Section`,
> `Label`, `SegmentLabel`, `SegmentActive`, `ScaleMark`, `DockHotkey`, `Hero`, `Title`,
> `Value`, `Meta` become `Display/Title/Body/Value/Meta/Micro`. Segment labels are `Value`
> (weight comes from the style, not from a separate baked face — but see FEASIBILITY §3:
> Geist Mono 500 vs 600 *are* separate atlas entries, so it is 6 roles × the weights each
> role actually uses, ~8 bakes, still down from today's 10). Fewer roles is not
> minimalism for its own sake: with ten roles, "which one is this?" is a decision, and
> every decision is a chance for two panels to disagree.

The hard rule survives intact: **never a number in a Sans run.**

### 3.4 State treatment

| State | Treatment |
|---|---|
| **Idle** | nothing |
| **Hover (pointer)** | row fill white @ 3.5%. No tick. |
| **Focus (nav)** | row fill `elev/2` (6%) + 3px accent left tick with a soft accent glow + label lifts to `text/primary`. |
| **Active / pressed** | fill gains accent @ 8%; the tick brightens to `accent-edge`. |
| **On (switch/segment/selected option)** | accent @ 30% fill, accent @ 55% border, `accent-knob`/`accent-seg` content. |
| **Disabled** | whole row × 38% alpha; slider fill/handle turn neutral white (30%/45%); the help line is replaced by the reason. |
| **Read-only** | Mono @ 46%, no box, no border — a value that visibly cannot be pressed. |
| **Destructive** | `state/danger` text + a danger-tinted outline; second-press confirmation. |

Hover and focus are deliberately *not* the same treatment. Today's design has no designed
hover at all (spec §12 admits it); on a console where a mouse and a controller are both
plausible, conflating them is how you get a screen showing two "current" rows.

### 3.5 How the OKLCH accent flows through

Unchanged from `Palette.h`, and this is one of the parts of the current design that is
genuinely good: per-token L and C are fixed, only hue is user-tunable, so no hue can wash
out or blow out a token. The console adds nothing to the family; it *reduces* the number
of places it is spent (§3.1's accent budget).

`state/danger` is deliberately **not** hue-linked — it must stay red even when the user
picks a red accent, because "destructive" is not a decorative property. If the accent hue
lands within 25° of the danger hue, the danger role shifts its chroma up (`.17 → .20`) and
its lightness down (`.70 → .64`) so it stays distinguishable. That is 4 lines in
`UpdateAccentFamily()`.

### 3.6 Motion language

Four motions, one helper, no animation system.

```cpp
inline float Approach( float cur, float target, float speed, float dt )
{   return cur + ( target - cur ) * ( 1.0f - expf( -speed * dt ) ); }
```

| Motion | Speed | State it needs |
|---|---|---|
| Focus tick slides between rows / rail items | 20 /s | one float per stack level (y), one for height |
| Stage page push / pop | 16 /s | one float (x offset 0→24u) + one alpha |
| Sheet slide-in from the right | 18 /s | one float |
| Rail collapse / expand (`Stage::Wide`) | 12 /s | one float (width) |

That is **five floats of animation state for the entire UI**, all living in one
`ConsoleAnim` struct next to the nav state. Nothing per-widget, nothing retained, nothing
that immediate mode has to be tricked into.

Everything else is instantaneous. Switch knobs snap. Sliders snap. Values snap. A settings
console that animates its values is a settings console that lies about latency.

### 3.7 Scaling 0.5× – 2.0×

Everything above is `u` and `base`. The clamps:

- Console size is `min(297u, 94% of surface) × min(192u, 90% of surface)`. At 2.0× on a
  1080p output the console hits the 94% clamp and stops growing — it does not overflow.
- Rail floor 200px logical; below that (0.5× on a small surface) it drops its labels and
  becomes the icon strip permanently.
- Control column floor 180px; the slider's value slot never falls below 6 monospace
  characters.
- Legend collapses to glyph-only under 900px of stage width.
- Font atlas re-bakes at the effective scale on slider release, exactly as today
  (`fonts::RebuildAll` + `ApplyPendingRebuild`). Six roles instead of ten makes this
  cheaper, not different.

### 3.8 Over an arbitrary game

The console is large, so it needs more coverage than a small window did: `surface/base` at
`.86` over the compositor's own `blur 1.0` + `darkening 0.8` pass. Checked against the
three worst cases (near-black night scene, blown-out orange fire, white snow field) in the
mockup's game toggle — the border, the row dividers and the meta-alpha text all survive
because the base is a *dark* translucency, not a light one, and the blur removes the
high-frequency detail that would otherwise fight the 1px hairlines.

**Mapping the user's established opacity defaults onto the new surfaces:**

| Existing setting | Was | Now |
|---|---|---|
| `opacity_windows_focused` 1.00 | focused panel window | **the console** |
| `opacity_windows_unfocused` 0.90 | other panel windows | **sheets + their scrim** (the only "behind" surface left) |
| `opacity_dock` 0.70 | dock container | **retired** — no dock. Repurpose the slider as "Well opacity" or drop it. |
| `opacity_notifications` 0.90 | toasts | unchanged (toasts are outside the console) |
| `background_blur` 1.0 | compositor blur | unchanged — the signature stays |
| `background_darkening` 0.8 | compositor dim | unchanged |
| `dock_scale` | dock geometry | **becomes `rail_scale`** (rail width + rail item height only) |

---

## 4. Screen inventory (where today's content lands)

| Rail section | Sub-tabs | Stage | Well |
|---|---|---|---|
| **Gamescope** | Upscaling · Display · Frame Limiter · HDR | List | — |
| **Shaders** | Vibrancy · Pre-Sharpen · Adaptive Brightness | List | A/B strip |
| **System Monitor** | General · FPS · CPU · GPU · Media · Statistics | Split | live HUD preview + 3×3 anchor |
| **Audio** | Output · Streams | List | L/R peak meters |
| **Config** | Per-Game · General · Notifications | List | — |
| **Log** | Gamescope · Game | **Wide** | — |

Six rail items, 21 screens, one layout language.

Notes on two placements:
- The **notification 3×3 position grid** (Config › Notifications) gets the same Well
  treatment as the HUD's: a preview of a toast in a nine-cell grid. Same widget, same
  component, second use — which is the point of it being a Well control rather than a
  bespoke block.
- **Statistics** (the 60-second stat rows) is a `List` screen of `Readout` rows with
  inline sparklines in the control column — sparkline is a Readout variant, not a new kind.
