# Direction E — "Inspector Rail" (working name: **the Bench**)

A UI redesign proposal for the gamescope-ritz settings overlay.
Date: 2026-08-23. Design only — no repository code was modified.

Companion files: `API.md` (the helper layer — the real deliverable),
`FEASIBILITY.md` (honest ImGui assessment), `index.html` (self-contained mockup,
seven screens at 1920×1080).

---

## 0. The one-paragraph version

Kill the dock and the six floating windows. Replace them with **one fixed slab**
divided into three regions that never move: a **Rail** of categories on the left, a
**Sheet** of settings in the middle, and an **Inspector** on the right that always
describes whatever row is selected. Every setting in the product lives in exactly one
place in that grid, every control is drawn by a helper that owns its geometry, and
the answer to "where am I" is legible from three independent places at once. The
overlay stops being a desktop you arrange and becomes an instrument panel you read.

**Why this and not the current design.** The user's complaint — *"every module/window
looks soooo different, nothing is really consistent"* — is not a styling problem. It is
a structural one: six panels each own their own window, their own width, their own tab
bar, their own ad-hoc row layout, and their own idea of where a label sits. Restyling
six independent containers keeps six independent containers. One container with one
row grammar removes the *capacity* to be inconsistent.

---

## 1. Navigation

### 1.1 The shell

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ● GAMESCOPE-RITZ                                    app 1174180   global.json ✕│  title 40
├────────────┬──────────────────────────────────────────┬───────────────────────┤
│            │ DISPLAY / Upscaling      differs 2   ⌕   │ Upscaling             │  header 56
│  RAIL 240  │                                          │  ────────────────     │
│            │              SHEET  (flex)               │   INSPECTOR 384       │
│  sections  │       rows, groups, lists, meters         │  contextual detail    │
│  + items   │                                          │  provenance, help,    │
│            │                                          │  expert parameters    │
│            ├──────────────────────────────────────────┤                       │
│            │ reset category   CTRL+K search · TAB region│                      │  footer 44
└────────────┴──────────────────────────────────────────┴───────────────────────┘
```

All numbers in this document are **base units**. On screen, `px = base × display_scale`
(0.5–2.0). Nothing in the codebase is allowed to write a raw pixel number; see §4.6.

**Slab** — centred on the surface, not draggable, not resizable, no per-panel positions
to remember. Width `min(surfaceW × 0.90, max(1560 × scale, min(surfaceW × 0.90, 1180)))`,
height `min(surfaceH × 0.86, 940 × scale)`. On 1920×1080 at 1.0×: **1560 × 928**, which
leaves 180px of game on each side and 76px top and bottom. That margin is deliberate:
the game is still visible, still running, still the thing you came from.

*Why one slab and not draggable windows:* every window-position bug this project has
had (#33 collapse, #34 measured sizing, #42 the hand-rolled drag rewrite) comes from
windows being independently placed and sized. A fixed slab has no drag, no resize, no
per-panel first-use position, no stacking order, no "panel buried under another panel".
It deletes an entire bug class rather than fixing it again.

### 1.2 What the Rail contains

Icons **and** labels, grouped under section headers. Two levels, no more — a third
level would be navigation hiding depth, which is precisely what this direction exists
not to do.

```
  ⌂  DISPLAY
       Upscaling            ← was GAMESCOPE panel, tab 1
       Output               ← was GAMESCOPE panel, tab 2 ("Display")
       Frame Limiter        ← was GAMESCOPE panel, tab 3
       HDR                  ← was GAMESCOPE panel, tab 4
  ◈  IMAGE
       Shaders          3   ← trailing count chip: 3 effects, 1 on
  ♪  AUDIO
       Mixer            2   ← 2 active streams
  ▤  SYSTEM
       Monitor          4   ← 4 modules
       Log             12!  ← 12 new lines, ! = an error since last visit
  ⚙  SETUP
       Profiles
       Per-Game
       Appearance           ← was CONFIG tab "General" + "Notifications"
```

Eleven items in five sections. Every tab bar in the product is gone: four Display tabs
and two of the three Config tabs became rail items; System Monitor's six tabs and
Shaders' effect groups became **list + inspector** (§1.4); Log's two sources became a
segmented *value* in the log filter bar.

**Rail item anatomy** — 40 tall, 20px icon at x=16, Sans 14 label at x=48, optional
trailing Mono 11.5 count chip right-aligned at x=224. Section header 26 tall, Mono
500 10.5 UPPER @44%, 12 above / 4 below. Active item: accent@10% fill, a **2px accent
left edge**, label @92%. Idle @62%. Hover +6% white over 90ms.

**Collapsed rail** (64 wide) at ≥1.5× or narrow surfaces: icon only, centred, section
headers become a 1px @8% divider, tooltips carry the label. The active item keeps its
2px accent left edge — the "you are here" signal survives the collapse, which is the
whole point of it being an edge and not a text weight.

### 1.3 What drives the Inspector

**One rule: the Inspector always shows the current selection, and the current selection
is always a Sheet row.** Not a hover target (hover-driven inspectors flicker and are
unusable with a gamepad). Not a separate mode. Selection moves with click, with arrow
keys, and with gamepad D-pad; it persists per category, so returning to Shaders
re-selects the effect you were tuning.

**When nothing is selected, the Inspector shows the Category Card** — never a "select
something" placeholder, never empty:

- Category name + one-sentence description.
- **`3 of 11 settings differ from default`** with the three named, each a click-to-jump link.
- **Writes to:** `games/1174180.json` (or `global.json`), with the routing reason
  ("Override Global Config is on for this game") — the exact information PanelConfig's
  badge already computes.
- Category-level actions: `reset category`, `copy from another game`, `save as preset`.
- Live facts the category owns: for Upscaling, the current effective composite path;
  for HDR, the focused app's HDR metadata read-only block.

That card is genuinely the most useful screen in the overlay for someone who just
launched a game and wants to know what state they are in.

### 1.4 The list-and-inspector pattern (this direction's engine)

Three of the six panels are really *collections of things with parameters*. They all
get the same shape, and it is the shape a professional tool uses:

| Panel today | Sheet becomes | Inspector shows |
|---|---|---|
| SHADERS (3 stacked groups) | list of effects: name, on/off switch, live-value meta | that effect's full parameter set |
| SYSTEM MONITOR (6 tab bar) | list of modules: FPS/CPU/GPU/Media, on/off, live preview | that module's rows, colours, format |
| AUDIO (bespoke stream UI) | mixer: one strip per stream, fader + meter | that stream's routing, match reason, override |
| CONFIG/Profiles (list + form) | list of saved profiles | that profile's contents, diff vs current, apply/rename/delete |
| LOG (2 tabs, flat text) | raw line stream + filter bar | the selected line, wrapped, with timestamp and copy |

The System Monitor conversion is the clearest win: six tabs (General, FPS, CPU, GPU,
Media, Statistics) become one list of four modules plus two rail-level rows (General
settings live at the top of the Monitor sheet; Statistics is a read-only block at the
bottom). A user comparing "what does the CPU module show vs the GPU module" no longer
has to remember what was in the other tab — both are on screen, one click apart, with
the parameters of whichever is selected in a fixed place.

### 1.5 The Inspector Contract

The reason a dense three-region UI usually fails is that the inspector becomes the
dumping ground for anything the designer could not place, and the main area becomes
useless without it. So this is a hard rule, policed by the helper (`API.md` §6):

> **The Sheet alone is a complete UI.** Every setting a user needs to change during a
> normal session is reachable and editable in the Sheet. The Inspector adds *depth* —
> help text, provenance, defaults, ranges, expert parameters, per-item detail — never
> *access*. Closing the Inspector must never make a setting unreachable.

Consequences: the Inspector is closable (`Tab` twice, or the ✕ in its header, or
`ui.inspector = hidden` persisted); at 2.0× it degrades to an overlay drawer without
loss; a screenshot of just the Sheet is a legitimate answer to "what can I set here".

The one licensed exception is **per-item expert parameters** (Adaptive Brightness's six
tuning values, a module's colour override). These are *not* "settings you change during
a session"; they are tuning you do once. The helper marks them `ui::Depth::Expert`,
which is what places them Inspector-only — a caller cannot make that choice ad hoc.

### 1.6 Input paths

**Pointer.** Click a rail item → Sheet swaps (160ms cross-fade of content only; the
regions never move). Click a row → it selects, Inspector updates. Click a control →
it edits. Selecting and editing are the same click: clicking a slider both selects the
row and starts the drag. There is no "select first, then edit" tax.

**Keyboard.** The whole surface is one three-scope focus model.

| Key | Action |
|---|---|
| `Ctrl+Shift+O` | toggle the overlay (unchanged) |
| `Tab` / `Shift+Tab` | cycle region: Rail → Sheet → Inspector → Rail |
| `↑ ↓` | move within the focused region's list |
| `← →` | at a region edge, cross into the neighbour; inside a control, adjust it |
| `Enter` / `Space` | activate / toggle / begin text or numeric entry |
| `Ctrl+←/→` | previous / next rail item without leaving the Sheet |
| `Ctrl+K` | **command palette** — fuzzy search every registered setting by label, key, and category |
| `Ctrl+D` | reset the focused row to default |
| `Ctrl+I` | toggle the Inspector |
| `Esc` | close palette → close Inspector drawer → close overlay (in that order) |

The command palette is the direction's cheap superpower: because every setting is
registered with a stable id, label and category (`API.md` §2), a fuzzy search over all
of them is ~40 lines and turns an eleven-item rail into a one-keystroke jump to
anything. `Ctrl+K → "sharp" → Enter` selects Display/Upscaling ▸ Sharpness with the
Inspector already showing its help and default.

**Gamepad.** ImGui's nav gives most of this; the routing is explicit:

| Input | Action |
|---|---|
| `LB` / `RB` | previous / next rail item |
| `LT` / `RT` + `LB/RB` | previous / next rail **section** |
| D-pad / left stick | move selection within the Sheet |
| `A` | activate / toggle / grab a slider |
| Left stick ←→ while a slider is grabbed | adjust; `RT` held = ×10 coarse, `LT` held = ×0.1 fine |
| `B` | release a grabbed control; if none, close the Inspector; if closed, close the overlay |
| `Y` | toggle the Inspector |
| `X` | reset the selected row to default |
| Right stick | scroll the focused region |

Note that the gamepad never needs to *reach* the Inspector to use the overlay — the
Inspector Contract guarantees that. `Y`/`B` are affordances, not requirements.

**"Where am I" is answered three times, redundantly**, because on top of a moving game
one signal is not enough:
1. The rail item carries a 2px accent left edge and an accent-tinted fill.
2. The sheet header carries a breadcrumb `DISPLAY / Upscaling` plus the destination
   file chip and a `differs 2` chip.
3. A 1px accent hairline is drawn from the active rail item's right edge into the
   sheet header's left edge — a literal thread connecting where you are to what you
   are looking at. It costs one `AddLine` and it is the design's signature.

---

## 2. Option presentation — the control taxonomy

This is the section the direction lives or dies on. The governing idea is that
**callers never choose a layout**; they name a *kind* of setting and the helper draws
the only permitted appearance for that kind.

### 2.1 The Row is the atom

Everything — every control, every readout, every list entry — is a Row. A Row occupies
the full width of its sheet column and has a fixed anatomy:

```
│▌│  Label text                    │  [ control                 ]  │  ⟡ │
 2   12        44% of W                   remainder − 36              28
 │    │                             │                                 │
 │    label zone                     control zone                 affordance
 state edge
```

- **State edge** — 2px at x=0. Accent@80% when the value differs from default;
  accent@40% when the row is the current selection; invisible otherwise. This single
  2px column is how a dense sheet stays scannable: *what have I changed here* is
  answered by peripheral vision.
- **Label zone** — starts at x=12, width `round(0.44 × W) − 12`. **Fixed per column,
  not content-derived.** Every control's left edge is on the same vertical line down
  the entire sheet. This is rule #1 of the taxonomy and it is the single biggest reason
  the current UI reads as unaligned.
- **Control zone** — from `0.44 × W` to `W − 36`, minimum 180 base. If 0.44 W leaves
  less than 180, the label zone yields first; if `W < 300`, the row switches to
  **two-line** layout (label line, then control line) — the third and last permitted
  layout mode, used in the Inspector and at 2.0×.
- **Affordance zone** — the last 28px: the reset-to-default dot (only when the row
  differs), a chevron when the row has Inspector depth, a lock glyph when the row is
  read-only.

**Row heights: exactly two classes.** `Row` = 44 base. `RowTall` = 76 base (slider with
marks, meter with legend, any two-line content). Two-line inspector rows = 64. Log
lines = 20. There is no fourth height, ever — and this is not aesthetic pedantry:
`ImGuiListClipper` needs uniform heights, so the taxonomy rule *is* the performance
rule (`FEASIBILITY.md` §4).

**Rows are separated by a 1px `#FFFFFF @ 6%` hairline, not by spacing.** Density reads
as a *table* — deliberate, engineered — rather than as a pile of controls that
happen to be close together. Groups get air; rows do not.

### 2.2 Grouping and rhythm

- **Group** — a labelled band: Mono 500 10.5 UPPER @44% header, 16 above, 8 below,
  rows flush underneath with their hairlines. **No group box.** This is a deliberate
  departure from `ui-mockup-precise-spec.md` §6's group blocks (2.2% fill, 6% border,
  12px padding): a box inside a region inside a slab is three nested containers, and
  nested containers with slightly different fills are exactly the visual noise the
  user is complaining about. The band header plus the hairline table carries the same
  grouping with one-third the ink.
- **Section separator** — 1px @10% full-column rule with 24 above / 16 below, used only
  between groups that are conceptually different subsystems.
- **Spacing scale — six steps, and callers never type any of them.**
  `XS 4 · S 8 · M 12 · L 16 · XL 24 · XXL 32`. The current 1/2/3/5/7/10/12/14 scale is
  eight values inside a 14px range; nothing distinguishes 5 from 7 in practice, so an
  agent picks arbitrarily and drift is guaranteed. Six values, each meaningfully
  different, chosen *by the helper* per context.

### 2.3 The complete control set

Every row kind in the product, with its appearance, its selection rule, and where
label / value / help go. **The selection rules are the important part** — they are
what make an inconsistent screen hard to build, because they are decidable from the
data, and the helper decides them.

#### 1. Switch — a boolean *setting*
30×15 track, 11×11 knob (existing `widgets::Toggle` geometry, kept), drawn at the
**left edge of the control zone** so a column of switches aligns. On: accent@30% track,
accent@65% border, `#93DEF4` knob right. Off: white@7% track, white@18% border,
`#EFF5FB @55%` knob left. Optional one-line hint under the label at Mono 11.5 @44%
(promotes the row to `RowTall`). Full help → Inspector.
*Use when:* a single independent on/off. Examples: `Allow Tearing`, `Force Grab Cursor`,
`VRR / Adaptive Sync`, `Override Global Config`, `Show System Monitor`, `auto-scroll`.

#### 2. Checkbox — a boolean *member of a set*
12×12 box, 5×5 filled square mark (existing `widgets::Checkbox`, kept). **Only ever
appears inside a multi-select list** (rule 3 below). Never on a standalone row.
*The rule:* **switch = a setting; checkbox = a member of a set.** Today the codebase
uses both interchangeably (FPS-HUD rows use checkboxes, everything else uses toggles,
with no stated reason). Stating the reason makes it enforceable and makes the two
controls carry information rather than noise.

#### 3. Multi-selector — an ordered set of booleans
A list of checkbox rows (28 base each, tighter than a Row because they are a unit) with
a 6-dot drag handle at the right when order matters, a `4 / 7` count chip in the group
header, and `all` / `none` text actions in the header's right slot.
*Use when:* HUD rows (`Frame rate`, `Frametime readout (ms)`, `Frametime graph`,
`Percentile row (1% / 0.1% / avg)`), enabled monitor modules, log severity filters.

#### 4. Segmented control — mutually exclusive, ≤5 short options
Equal-width cells, 4px gaps, square, **lowercase labels**, 28 tall. Inactive: white@4%
fill, white@8% border, Mono 500 11.5 @50%. Active: accent@24% fill, accent@60% border,
Mono **600** 11.5 `#A9EAFD`. (Existing `widgets::SegmentedControl`, kept.)
*The rule:* **≤5 options AND every label ≤8 characters AND the set is static** →
segmented. Otherwise dropdown. **The helper measures and auto-downgrades** — a caller
passing six options gets a dropdown and cannot accidentally ship a cramped segmented
row. Examples: filter `linear/nearest/fsr/nis/pixel`, scaler `auto/fit/fill/stretch/
integer`, backdrop `none/shadow/solid/blur`, blend `alpha/additive/inverted`, log
source `gamescope/game`.
*And:* **a segmented control sets a value; it never navigates.** There is no tab bar
in this design and no `BeginTabBar` in the helper API, so this rule cannot be broken
by accident.

#### 5. Dropdown — mutually exclusive, many or dynamic options
Full-control-zone box, 28 tall, white@5% fill, white@8% hairline, Mono 11.5 value
left-aligned, 7px chevron right. Popup: same width, anchored under the box, rows 26
tall, hover accent@10%, selected accent@20% + 2px accent left edge, a 1px @8% divider
before any "clear/none" action, max height 280 then scrolls with a clipper.
*Use when:* the audio stream picker (`334 - Floorp (floorp) [pid 3922017]`), profile
picker, `copy another game's config`, tonemap operator.

#### 6. Slider — a bounded continuous value
Keeps the shipped, measured `slider-widget-spec.md` geometry exactly: 6px track / 3.5
radius, 10×22 handle with the two-rect glow, `#36BDDD@50% → #47CAEA` gradient fill,
8px label→track and track→mark gaps, `kRailAlpha 16%` rail, `kMarkAlpha 38%` marks.
That spec was measured from a real render and the user has already signed off on it —
it moves into the new shell unchanged. **What changes is its placement, not its
pixels:** label + value share the label-zone line (value right-aligned in the control
zone, `#78DBF6` Mono 500 16), the track spans the control zone, marks sit under it.
Two additions:
- **Default tick** — a 1px `#FFFFFF @30%` mark on the rail at the default value. "How
  far from stock am I" becomes readable without a number.
- **Modifiers** — `Shift`-drag = ×0.1 fine, `Ctrl`-click = convert the value readout
  into an inline numeric entry, mouse wheel = one step. Documented once, works on all.

*Use when:* the range is bounded and the exact number rarely matters — `Sharpness 0–20`,
`Strength −1.00–1.00`, `SDR Gamut Wideness`, `Volume`, opacity/blur/darkening, `UI Scale`,
`Font size`, `Hue`.

#### 7. Numeric entry / stepper — an exact or unbounded value
28-tall box, `−` and `+` 20px hit zones at each end, Mono 500 16 value centred, unit
suffix Mono 11.5 @44%. Click the value to type. Holding a stepper accelerates after
400ms. A distinguished value gets a word instead of a number (`0` renders as
`Unlimited` for FPS Limit).
*The rule:* bounded + continuous → slider; unbounded, or discrete and exact → stepper.
Examples: `FPS Limit`, `Vertical margin`, `Horizontal margin`, `Module spacing`,
notification/HUD offsets.

#### 8. Text entry — free text
Same 28-tall box, Mono 11.5 left, caret accent, a 1px accent bottom edge while focused,
placeholder @30%. Validation message appears **under** the box in `kDanger @80%` Mono
11.5 and the box's border turns `kDanger @50%` — e.g. PanelConfig's existing *"Profile
name can't be empty (letters/digits/space/-/_ only)."*
*Use when:* `New profile name`, a config path.

#### 9. Multi-line text view — the LOG
A `Raw` sheet: no rows, no label column, no group bands. Anatomy:
- **Sticky filter bar** (44 tall, at the top of the sheet body, 1px @10% bottom rule):
  source segmented `gamescope | game` · text filter entry · severity chips
  `err 2` `warn 7` `info` (toggleable) · `auto-scroll` switch · `copy` button.
- **Stream**: Mono 11.5, line-height 1.45 → 20 base per line. 44-wide line-number
  gutter @30%, then a **2px severity left edge** per line (error `kDanger`, warn
  `#F3821D`, info none), then the text. No wrapping — horizontal scroll instead, so
  line numbers stay meaningful; the Inspector wraps the selected line.
- **Scrollbar with a severity minimap**: 1×3px ticks at every error/warn position over
  the whole buffer, so a 40 000-line log shows *where* the problems are without
  scrolling.
- **Selection**: click a line to select (2px accent edge, accent@8% fill), drag to
  select a range, `Ctrl+C` copies.
- **Inspector for the LOG**: the selected line, wrapped, with its timestamp, severity,
  source, a `copy line` button, and — when the line matches a known pattern — a
  one-line explanation. Also `matches: 34` when a filter is active.
- **Empty state**: *"Not capturing — no game was launched this session."* per §2.3.17.

#### 10. Colour picker — the accent hue, and per-module colours
Never an ImGui colour wheel. Two forms, both built from `palette::OklchToImU32`:
- **Hue rail** (the accent) — a 20-tall full-width strip whose pixels *are* the real
  accent at each hue (sample OKLCH at the token's own L/C, so the strip cannot lie
  about what you will get), a slider handle, 8 preset swatches 18×18 below, and a Mono
  readout `oklch(.74 .12 218)  #36BDDD`.
- **Full colour** (a module's custom colour) — **Inspector-hosted**, three rails
  (L, C, H) plus a hex entry and a live 44×44 preview swatch. Inspector-hosted means
  no popup window, no second floating surface, no focus-stealing modal.

#### 11. Position grid — pick one of nine
The existing `widgets::PositionGrid` (3×3 of 30×30, 3px gaps; cell white@5%/white@9%,
selected accent@30%/accent@70%), promoted to a standard `RowTall` row: the grid
right-aligned in the control zone, the two offset steppers to its left, and the current
anchor named in Mono 11.5 @44% under it — `top-right · 32 / 32`. Same widget, same row
shape, for both notification placement and System Monitor placement. Today these are two
call sites that drifted; here they cannot.

#### 12. Buttons
Flat, 28 tall, hairline border, Mono 11.5 **lowercase**, right-aligned in the control
zone, at most **two per row**. Three intents and no others:
- `neutral` — white@4% fill, white@10% border, text @62%.
- `accent` (primary, at most one per group) — accent@16% fill, accent@50% border,
  text `#A9EAFD`.
- `danger` — the only red in the design. Requires `kDanger` to be added to the palette:
  `oklch(.66 .17 27)` = **`#EF6B5A`**, hue-fixed (it does not follow the accent hue —
  a green "delete permanently" is a bug, not a theme). Fill `kDanger@14%`, border
  `kDanger@45%`, text `#F2A99E`. Every danger button is required by the API to carry a
  confirm step. Examples: `Delete Permanently`, `Delete Saved Config…`.

#### 13. Lists — three kinds, one appearance
- **Selectable list** — 44-tall rows; optional 6×6 leading status dot; Sans 14 primary
  label; trailing Mono 11.5 @44% meta; selected = 2px accent left edge + accent@8% fill.
  Used for shader effects, monitor modules, audio streams, profiles, log lines.
- **Checkbox list** — see rule 3.
- **Readout list** — 28-tall, non-interactive: Sans 14 @62% label left, Mono 11.5 @44%
  value right, optional 6×6 status dot (`ok` green `#6ED274` when live). **No hover
  highlight and no state edge** — that absence is the structural expression of *"never
  show a value the overlay didn't verify"*. Used for the focused app's HDR metadata,
  GPU sensor readouts, `sampling 500 ms · 240-frame window`, the resolved app id.

All three share the row grid, so a sheet can interleave them without a seam.

#### 14. Meters and graphs
5px-tall segmented bars (20 segments, 1.5px gaps, lit accent@85%, unlit white@9% —
existing spec) or a graph strip, drawn in the control zone with a Mono 500 16 value
right-aligned. Audio L/R peaks, CPU/GPU load, VRAM, the frametime graph. Same
no-hover/no-edge rule as readouts: they are outputs.

#### 15. Chips and badges — status only, never interactive
Padding 3×5, square, no border. Accent chip: accent@16% fill, Mono 10 `#71D4EF` —
`AUTO`, `HOOKS`, `live`, `differs 2`. Neutral chip: white@6%, @45% — `global.json`,
`app 1174180`. Warn chip: `#F3821D@16%`. Danger chip: `kDanger@16%`.
The **`differs` chip** and the row state edge are the same information at two zoom
levels, and they are the two devices that make a dense sheet navigable.

#### 16. Disabled state
Whole row × 34% opacity, fill/handle turn plain white (existing spec §12), **plus a
mandatory reason string** in the row's hint slot. The API makes this impossible to skip:
`.DisabledUnless( bool ok, const char* reason )` — there is no overload without the
reason. The existing good examples become universal: *"HDR is off — these controls do
nothing until it's on."*, *"Effects are SDR-only for now — disable HDR to use them."*,
*"Install WirePlumber's CLI (wpctl) to control per-app volume."*

#### 17. Empty states
A rule, because dense layouts look broken when empty: a 120-tall centred band, Mono
11.5 @44% one-liner, optionally one `accent` button. Existing copy already fits:
*"No profiles saved yet."*, *"No PipeWire audio streams found on the system right now."*,
*"Capturing — nothing printed to stdout/stderr yet."*

#### 18. Progress / indeterminate
One form only: a 3px bar in the control zone, accent@70% fill on white@9% track, with a
Mono 11.5 @44% caption. Used for *"Collecting… 42s of 60s"*. No spinners — a spinner
over a game is a moving object competing with the game.

### 2.4 Where label, value and help go — the invariants

| Thing | Where, always |
|---|---|
| Label | Label zone, left-aligned, Sans 400 14 @62% (@92% when the row is selected) |
| Value | Control zone, Mono 500 16 `#78DBF6`, right-aligned to the control zone's right edge |
| Unit | Immediately after the value, Mono 400 11.5 @44%, never inside the number's run |
| One-line hint | Under the label, Mono 400 11.5 @44%; promotes the row to `RowTall` |
| Full help | **Inspector only**, Sans 400 14 @62%, wrapped, max 3 sentences |
| Default | Inspector, as `default 5` Mono @44%, plus the rail tick and the reset dot |
| Provenance | Inspector, as `writes global.json` / `writes games/1174180.json` |
| Related | Inspector, as a list of jump links (`Sharpness` ↔ `Filter`) |
| Error | Under the control, `kDanger @80%` Mono 11.5 |

Numbers are **always** Geist Mono; prose is **always** Geist Sans. This existing hard
rule survives intact and is enforced structurally: the helper picks the font per zone,
so a caller cannot put a number in Sans.

---

## 3. Styling and theming

### 3.1 Colour roles (call sites never touch hex)

`Palette.h` today exposes tokens (`kAccentValue`, `White(a)`, `Text(a)`). The redesign
adds a thin **role** layer above them, because a role is the thing a caller means:

| Role | Value | Used for |
|---|---|---|
| `Surface` | `rgba(9,10,12,.88)` | the slab base |
| `SurfaceRail` | `Surface` − 3% lightness (black@10% over it) | the rail (recessed) |
| `SurfaceSheet` | `Surface` | the sheet (base level) |
| `SurfaceInspector` | white@3% over `Surface` | the inspector (raised) |
| `SurfaceRaised` | white@5% | control boxes, inactive segments |
| `Line` | white@8% | row hairlines, control borders |
| `LineStrong` | white@14% | region boundaries, unchecked checkbox border |
| `TextPrimary` | `#EFF5FB @92%` | selected labels, group names, hero numbers |
| `TextLabel` | `#EFF5FB @62%` | parameter labels, body |
| `TextMeta` | `#EFF5FB @44%` | units, hints, read-only values, marks |
| `TextFaint` | `#EFF5FB @30%` | line numbers, disabled meta, ghost placeholders |
| `Accent*` | existing OKLCH family, hue-live | state: active, changed, selected |
| `Ok` | `#6ED274` | liveness dots only |
| `Warn` | `#F3821D` | frametime outliers, warn log lines |
| `Danger` | **`#EF6B5A`** `oklch(.66 .17 27)` — new, hue-fixed | destructive actions, errors, invalid input |

`TextMeta` at 44% rather than the original spec's 30–36% is not a new decision — issue
#62 already moved the shipped `kMetaTextAlpha` there after three separate "too dark"
complaints. The role table just makes 44% the *only* meta value, so the fourth
complaint cannot happen at a call site that missed the memo.

**Accent flow.** One user-chosen OKLCH hue drives the whole family; L and C stay fixed
per token so no hue can wash out or blow out (existing `UpdateAccentFamily()` behaviour,
kept verbatim). What changes is the *budget*: accent is spent on **state only** —
active rail item, selected row, changed value, active segment, on-switch, slider fill,
focus ring. It is never used for decoration, never for a header, never for a border
that is not communicating state. In a dense layout that discipline is the difference
between a control room and a Christmas tree.

### 3.2 Elevation

Three levels, expressed as ±3% lightness of the *same* base colour plus a 1px `Line`
between regions. **No shadows inside the slab.** The only shadow in the design is the
slab's own drop shadow (the existing 3-rect approximation) and the accent focus glow.
Popups (the dropdown menu, the command palette) are `SurfaceInspector` + a 1px
`LineStrong` + a single 8px black@25% expansion — one recipe, used twice.

### 3.3 Type scale — Geist, seven roles

| Role | Family / weight | Size (base) | Use |
|---|---|---|---|
| `Title` | Mono 600 | 11 UPPER | slab title, region titles |
| `Section` | Mono 500 | 10.5 UPPER | group band headers, rail section headers |
| `Label` | Sans 400 | 14 | parameter labels, prose, list primaries |
| `LabelStrong` | Sans 500 | 14 | selected row label, category card title |
| `Value` | Mono 500 | 16 | every numeric/state readout |
| `Meta` | Mono 400 | 11.5 | units, hints, marks, line numbers, chips |
| `Hero` | Mono 600 | 30 | the one big number per screen (volume %, fps) |

Seven roles, down from the ten `fonts::Style` entries today (`SegmentLabel`,
`SegmentActive`, `ScaleMark`, `DockHotkey` collapse into `Meta`/`Section` with a weight
flag). Fewer atlas entries, fewer decisions, same visual result.

**Weight carries hierarchy and nothing else.** 500/600 exist only for section headers,
the active segment, and a selected row's label. Everything else is 400. In a dense
layout, weight variety is noise; the eye should be sorting by *position* and *colour
state*, which are both already load-bearing.

### 3.4 State treatment

| State | Treatment |
|---|---|
| Idle | as specified per control |
| Hover | +6% white on fills, +8% on text alpha, 90ms |
| Pressed | +8% accent on fills, instant (no animation on press — latency reads as lag) |
| Keyboard/gamepad focus | 1px accent@60% ring inset 1px around the control, plus the row's state edge at accent@40% |
| Selected row | accent@8% row fill, 2px accent@40% state edge, label to `TextPrimary` |
| Changed from default | 2px accent@80% state edge, reset dot in the affordance zone, `differs` counted in the header chip |
| Disabled | row × 34%, plain-white fill/handle, mandatory reason string |
| Read-only | no hover, no state edge, `TextMeta` value, lock glyph in the affordance zone |
| Region focused | that region's 1px boundary line goes accent@30% |
| Slab unfocused | whole slab × 0.90 (the user's chosen unfocused opacity) |

### 3.5 Motion

Three durations, one easing (`1 − (1 − t)³`), all lerped against `io.DeltaTime`:

- **90ms — state.** Hover fades, switch knob travel, segment fill, focus ring.
- **160ms — region.** Inspector open/close and drawer slide, rail collapse/expand,
  sheet content cross-fade on category change.
- **240ms — surface.** Overlay open/close: slab scales 0.98→1.00 with alpha 0→1, and
  the game's blur/darkening ramps in over the same window.

Two prohibitions:
- **Values never animate.** A slider handle moving to a typed or reset value snaps.
  Animated values over a running game read as input lag.
- **Regions never move except when they are being opened or closed.** No reflow, no
  content-driven resizing, no "the sheet got taller so the footer jumped". The current
  design's measured-and-grown window sizing (#34) is exactly the behaviour this
  forbids.

### 3.6 Density without noise — the four devices

A dense professional layout goes wrong in one specific way: too many separators, too
many weights, too many boxes. The counters, stated as rules:

1. **One rule weight.** All separators are 1px. Row hairlines @6%, group rules @10%,
   region boundaries @14%. Three alphas, one thickness, no double rules ever.
2. **No nested containers.** Region → group band → row. Three levels, and only the
   region has a fill of its own. (This is why §2.2 kills the group box.)
3. **Alignment does the work grouping usually does.** One label column, one control
   column, one affordance column, down the whole sheet. Aligned things read as related
   without any ink at all.
4. **Colour is state.** If something is accent-coloured, it is active, selected, or
   changed. A screen where nothing is changed is almost monochrome — and that is the
   correct look for "everything is at default".

### 3.7 Over an arbitrary game, at 0.5× to 2.0×

- **Backdrop blur stays and is the signature.** Per-region alpha at the user's chosen
  defaults: slab focused 1.00 / unfocused 0.90, blur 1.0, darkening 0.8. The **rail
  gets darkening + 0.06** — navigation must never have a game showing through it, or
  the "where am I" edge stops reading.
- **Contrast floor.** Every text role at or above `TextMeta` must stay legible over a
  pure-white game frame *and* over a pure-black one. That is what the `Surface` @ 88%
  plus 0.8 darkening buys, and it is why `TextMeta` sits at 44% and not 30%.
- **Everything scales, nothing is exempt.** Every number in this document is `× scale`,
  including hairlines (`max(1px, round(1 × scale))` so a 1px rule at 2.0× is 2px and
  stays a hairline rather than disappearing at 0.5×).
- **The atlas** bakes seven roles × the effective scale, as today (`Fonts::Load`,
  re-bake on scale change, issue #38's non-idempotent path). Seven roles instead of ten
  makes the re-bake cheaper.

### 3.8 The responsive ladder (see `FEASIBILITY.md` §5 for the arithmetic)

One function computes the whole layout from the available width; every consumer reads
its output and nothing else makes a width decision. The ladder is deterministic and
ordered so that the *cheapest loss happens first*:

| Step | Trigger | Change |
|---|---|---|
| 0 | fits | Rail (labels) + Sheet + Inspector |
| 1 | rail+sheet_min+inspector > avail | **Rail collapses to icons** (240 → 64) |
| 2 | still over | **Inspector becomes an overlay drawer** over the sheet's right, dismissed with `Esc`/`Ctrl+I` |
| 3 | still over | **Inspector becomes a bottom strip**: one line of selection summary + expand |
| 4 | sheet content < 420 per column | **Dense sheets drop to one column**; rows switch to two-line layout |
| −1 | lots of room (0.5×) | **Dense sheets gain a third column** (max 3) |

On 1920×1080: 1.0× is step 0. 1.5× is step 1 (icon rail, all three regions still
visible). 2.0× is step 2 (icon rail, full-width sheet, inspector as a drawer). 0.5× is
step −1 (three columns of dense rows). Nothing about the design breaks at any point on
that ladder, and each step is a discrete state that can be screenshot-tested.

---

## 4. What this replaces, concretely

| Today | Becomes |
|---|---|
| Bottom dock, 7 buttons, hint line | The Rail (always visible, always in the same place) |
| 6 floating windows, drag / collapse / close / tile / measure-and-grow | One fixed slab; ~900 LOC of `Chrome.cpp` deleted |
| 4 tab bars (`GamescopeTabs`, `ConfigTabs`, `LogTabs`, `SystemMonitorTabs`) | Rail items and list+inspector; **no `BeginTabBar` in the API at all** |
| Per-panel ad-hoc row layout | One Row grammar, one label column, two heights |
| `BeginGroupBlock` nested boxes | Group bands + hairline table |
| Tooltips as the only help channel | The Inspector as a permanent help channel |
| Stock `ImGui::SliderFloat` still live in `FpsDisplay.cpp` (7 sliders) | One `Slider()` — the drift that produced them is structurally gone |
| `widgets::Toggle` / `Checkbox` used interchangeably | Switch = setting, Checkbox = set member |
| Per-panel opacity, width, padding constants | The shell owns all geometry |

Five things are kept verbatim because they were measured and are liked: the **slider**
(`slider-widget-spec.md` in full), the **toggle** and **checkbox** geometry, the
**segmented control**, the **3×3 position grid**, and the **OKLCH accent family** with
its live hue. This is not a from-scratch redraw — it is the same widget vocabulary
placed in a structure that can hold it consistently.
