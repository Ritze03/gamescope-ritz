# Direction E3 — **The Bench**

A reinvention of Direction E (Inspector Rail) for the gamescope-ritz settings overlay.
Date: 2026-08-23. Design only — no repository code was modified.

Companion files: `API.md` (the helper layer), `FEASIBILITY.md` (honest ImGui assessment),
`index.html` (self-contained, strongly interactive mockup at 1920×1080).

---

## 0. The one-paragraph version

The overlay is two slabs pinned to opposite edges of the screen with the **game left
uncovered between them**. On the left, the **Spine**: one accordion column that is the
category list and the settings list at the same time. On the right, the **Scope**: the
kept idea — a persistent place where depth lives — rebuilt as a *live instrument* rather
than a paragraph of help text. Its top third always shows what the selected setting
*does*: a before/after wipe of the actual frame, a frametime trace with your cap drawn on
it, the PQ curve with SDR white marked, an audio meter, the OKLCH accent family, or a
draggable 1:12 miniature of the real output. Every control lives in a fixed **Lane**
flush to its container's right edge, so alignment is the only geometry available rather
than a habit that drifts.

---

## 1. What is kept from Direction E, and what is not

The user's verdict on E, quoted in full because it is the design constraint:

> *"The Inspector Rail is amazing. It cleans up the main screen, while still being able to
> provide additional information and storing a lot of settings, without cluttering the UI.
> I really want to emphasize, that this is very good."*

**Kept, unchanged in substance:**

- A persistent region whose entire job is depth, so the main surface stays clean.
- It is driven by **selection**, never by hover. Hover-driven detail flickers over a
  moving game and is unusable.
- **It is never empty.** With nothing selected it shows the session, not a "select
  something" placeholder.
- **The Inspector Contract**: the main surface alone is a complete UI. The depth region
  adds explanation, provenance, defaults and *expert* parameters — never *access*.
  Closing it must never make a setting unreachable.

**Changed:**

| E | E3 | Why |
|---|---|---|
| Rail + Sheet + Inspector — three regions | **Spine + Scope** — two | E's own feasibility doc named three focus scopes as its weakness. Merging the rail into the sheet deletes a scope and stops two-setting categories from looking lost in a 934-unit sheet. |
| Inspector = help text, defaults, provenance | **Scope = a live instrument** + those facts | The overlay lives inside the compositor. It *has* the frame, the frametime history, the audio graph, the display's PQ curve. Reading them out is a text panel's job; showing them is what only this program can do. |
| One centred 1560-wide slab | **Two edge-pinned slabs, centre of the frame uncovered** | If a setting previews its own effect, covering the thing it affects is self-defeating. |
| Segmented control *or* dropdown | **One Selector, two densities** | "Two looks for one meaning" is exactly the complaint that killed switch-vs-checkbox. |
| Switch = setting, Checkbox = set member | **Switch is the only boolean. `widgets::Checkbox` is deleted.** | Issue #60 records it already has zero callers. One meaning, one control, taken literally. |
| Per-row 2px "differs" edge | Per-row edge **plus the Ledger** | A per-row edge only tells you about rows you can currently see. The Ledger is the same information for the entire product at once. |
| `differs N` chip per category | Session card + Ledger | Same reason. |

---

## 2. Navigation

### 2.1 The shell

```
 0                                                                          1920
 ┌────────────────────────────────────────────────────────────────────────────┐
 │                     ░░░ the game, blurred at the edges ░░░                 │
 │  ┌──────────────────────┐                          ┌────────────────────┐  │
 │  │▌ gamescope-ritz    ✕ │                          │ SCOPE  display/… ✕ │  │
 │  │▌ ELDEN RING  app …   │      the game, at         ├────────────────────┤  │
 │  │▌ ⌕ search  Ctrl K    │      full clarity,        │  ◤ live probe ◥    │  │
 │  │▌─────────────────────│      still running        │  (wipe/trace/…)    │  │
 │  │▌ DISPLAY             │                           ├────────────────────┤  │
 │  │▌  ▾ Upscaling        │      ← this is the        │  Sharpness         │  │
 │  │▌    Filter    [ ▏fsr]│        largest preview    │  help, 3 sentences │  │
 │  │▌    Sharpness  ═══◆══│        surface in the     ├────────────────────┤  │
 │  │▌  ▸ Frame Limiter    │        design             │  BINDING           │  │
 │  │▌  ▸ HDR              │                           │  key/default/range │  │
 │  │▌ IMAGE               │                           │  writes …json      │  │
 │  │▌  ▸ Shaders      1on │                           ├────────────────────┤  │
 │  │▌ …                   │                           │  expert params     │  │
 │  │ TAB scope  ^D reset  │                           │  this category     │  │
 │  └──────────────────────┘                          └────────────────────┘  │
 │   ▲ the Ledger          Spine 552                          Scope 432       │
 └────────────────────────────────────────────────────────────────────────────┘
```

All numbers are **base units**; `px = base × display_scale` (0.5–2.0). Nothing in the
codebase writes a raw pixel; see `API.md` §6.

**Neither slab is draggable, resizable, closable-into-a-taskbar or stackable.** They are
pinned to their screen edges with a `56` base margin and are `1080 − 152` tall. That is
the same bug-class deletion Direction E argued for (of 41 fix commits touching
`src/Overlay/`, 39% were pure floating-window management) — it simply happens twice
instead of once.

*Why two slabs and not one, given that "six floating windows" is the thing being
deleted:* a floating window is defined by having a **position that can differ** — from
run to run, from user to user, from a bug. These two have no position at all: their rects
are a pure function of `(surface, display_scale, ladder step)`. Two rects computed by one
function is not two windows; it is one layout with a hole in the middle. The hole is the
point.

### 2.2 The Spine — the rail and the sheet, merged

One scrolling column. Categories are **accordion headers**; exactly one is open. Opening
one closes the others.

```
DISPLAY                      ← section header, Mono 10 UPPER, TextMeta
  ▾ Upscaling                ← open: accent left edge, accent icon, TextPrimary label
      SCALING FILTER ──────  ← group band
      Filter        [‹▏▏fsr▏▏›]
      Sharpness              7
                  ══════◆═══
      Scaler        [‹▏auto▏▏›]
      PRESENTATION ────────
      Allow Tearing      [○  ]
      Force Grab Cursor  [○  ]
  ▸ Frame Limiter        ›
  ▸ HDR                  ›
IMAGE
  ▸ Shaders         1 on ›
```

**Why an accordion instead of a rail + a sheet.**

1. It removes a focus scope. Keyboard nav is one list: `↑ ↓` walks headers and rows
   alike, `→` opens a header, `←` closes it. There is no cross-region arrow-key
   negotiation to hand-tune.
2. A two-setting category no longer sits alone in a sheet built for a forty-setting one.
   Frame Limiter's three rows appear *between* their neighbours, which is the honest
   picture of how small they are.
3. "Where am I" and "where else could I go" are answered by the same pixels. A rail
   answers the second; a breadcrumb answers the first; an accordion answers both without
   spending width on either.
4. It is the single cheapest thing to implement correctly in immediate mode: one
   `BeginChild`, one loop, one `int m_openCategory`.

**Cost, stated honestly:** you cannot see two categories' settings at once, and a long
category pushes its successors far down the scroll. Mitigations: `Ctrl+K` reaches
anything in one keystroke; the Ledger shows changed settings across *all* categories at
once; and the open category is remembered per session.

**Anatomy.** Section header 26 tall, Mono 500 10 UPPER `TextMeta`, 16 above / 5 below.
Category header 42 tall: 18px icon at x=22, Sans 14 label at x=51, optional Mono 11 count
at the lane's right edge, chevron last. Open: `white@5.5%` fill, 2px accent left edge,
accent icon, label to `TextPrimary` weight 500.

### 2.3 The Scope — the kept idea, rebuilt as an instrument

Fixed 432 base wide. Three stacked blocks, in this order, always:

1. **The Probe** — a live visualisation of the selected setting (§4).
2. **The Statement** — the setting's name at Sans 500 17, then its help at Sans 14
   `TextBody`, capped at three sentences.
3. **The Facts** — `key`, `current`, `default`, `range`, `options`, `writes`, `reason`,
   read from the binding and never typed by a caller; then `reset to default` when it
   differs; then the expert parameters; then *Related* jump links; then **This category**
   (settings count, changed-here count, destination file, `reset category`,
   `save as preset`); then the setting's search terms.

**With nothing selected**, the Scope shows the **Session card**: the game, the destination
file and its routing reason, a live frametime trace, and every setting that differs from
stock as a click-to-jump list. This is the most useful screen in the overlay for someone
who just launched a game and wants to know what state they are in.

**The Scope is closable** (`Ctrl+I`, or the ✕ in its header, persisted as
`ui.scope = docked | hidden`). Per the Inspector Contract, nothing becomes unreachable
when it is closed — expert parameters are, by definition, tuning you do once, not
settings you change during a session, and the helper is what decides that (`API.md` §3.4).

### 2.4 The Ledger

A 6-base strip down the **outer** edge of the Spine. One 4px accent tick per setting that
differs from its default, positioned at that setting's normalised index across the whole
registry — including categories that are closed. Hover names it and its value; click
opens its category and selects it.

*Why:* E answered "what have I changed" twice — a per-row 2px edge and a per-category
`differs N` chip. Both are local. The Ledger is the same question answered for the entire
product in one glance, in six pixels of width, and it is the only device in the design
that shows you something about a category you are not looking at. It costs
`n_changed × 1 AddRectFilled`.

### 2.5 Peek

**Hold `Alt`:** both slabs drop to 10% opacity and the veil to 15% for as long as you
hold. Release restores.

*Why this exists:* the overlay's whole justification for previewing an effect is that you
can see the effect. A wipe in a 390-wide probe is a sample; the actual frame at 1920×1080
is the truth. Peek is one `ui::Anim` lerp and it is the single cheapest large improvement
available to an overlay that sits on top of a running game. It is also the honest answer
to "the slabs cover part of the frame": yes, and here is the key that uncovers it.

### 2.6 Keyboard — the whole model

Gamepad support is out of scope (dropped 2026-08-23). Mouse and keyboard only.

| Key | Action |
|---|---|
| `Ctrl+Shift+O` | toggle the overlay (unchanged) |
| `↑` `↓` | move selection through the Spine — headers and rows in one list |
| `→` | open a closed category header; otherwise **increase the selected value** |
| `←` | close an open category header; otherwise **decrease the selected value** |
| `Enter` / `Space` | toggle a switch, begin text entry, enter a composite's grab mode |
| `Esc` | leave a composite's grab mode → close the palette → close the Scope → close the overlay |
| `Tab` / `Shift+Tab` | Spine ⇄ Scope |
| `Ctrl+K` | command palette (§5) |
| `Ctrl+D` | reset the selected row to its default |
| `Ctrl+I` | show / hide the Scope |
| `Alt` (hold) | peek |
| `Ctrl+C` | copy the selected log line / the selected value |

**One keypress changes a value.** There is no select-then-grab tax: `←/→` adjusts the
selected row directly. The single exception is a composite that needs both axes — the
Stage — which takes `Enter` to grab, after which all four arrows move the anchor and
`Shift`+arrows nudge the margins. That exception is stated once, shown in the Scope's
footer while the row is selected, and applies to exactly one control.

### 2.7 The responsive ladder

One pure function of `(surface, display_scale, ui.gutter)`. Ordered so the cheapest loss
happens first. Every step is discrete and screenshot-testable; there is **no continuous
splitter anywhere in the design** (see `FEASIBILITY.md` §3 for why).

| Step | Trigger | Result |
|---|---|---|
| **0 — open** | `1920 − (spine + scope + 2×margin) ≥ 240 × scale` | Both slabs at their edges, game visible and unveiled between them |
| **1 — docked** | still fits within `surface − 40` | Scope docks flush to the Spine's right; they read as one slab; game returns to the outer margins |
| **2 — overlay** | does not fit | Scope slides over the Spine's right half; `Esc` / `Ctrl+I` dismisses it |
| **hidden** | user choice, or `ui.scope = hidden` | Spine only. The Inspector Contract makes this survivable |

On 1920×1080: 0.5×–1.25× is step 0. 1.5× is step 1. 1.75×–2.0× is step 2. `ui.gutter =
off` forces step 1 at any scale, for a user who would rather have one panel.

---

## 3. The control taxonomy

### 3.1 The Lane Law

> **Every row reserves a lane of exactly 190 base units, flush to its container's right
> inner edge. Every control's right edge is the lane's right edge. Nothing is ever drawn
> outside the lane.**

That is the user's own sentence — *"All elements should be rightbound"* — turned into
geometry rather than into a convention. It is enforced structurally, not by review:

- `Row::Begin()` is the only function in the tree that produces a control rect. It
  returns `{ row, label, lane }`.
- Every control painter's signature takes `const ImRect &lane` as its first argument.
  None of them calls `GetContentRegionAvail()`, `SameLine()`, `PushItemWidth()` or
  `SetCursorPosX()` — a grep for any of those inside `UI/Controls/` is a review failure,
  and `ui_lint` reports it.
- There is **no alignment parameter anywhere in the public API.** A call site cannot ask
  for left, centre or right, because it cannot express the question.

Consequences the user asked for, obtained for free: switches, tracks, sliders, steppers
and text boxes in a column all terminate on the same vertical line; a switch that is
narrower than the lane is still right-bound because it is right-bound *to the lane*, not
centred in it.

Two heights, and no third: **Row = 42** base, **RowTall = 70** base. `ImGuiListClipper`
requires uniform heights, so the taxonomy rule *is* the performance rule.

### 3.2 The atomic controls

#### 1. Switch — **the only boolean control in the product**
34×17 track, 11×11 knob, flush to the lane's right edge. On: accent@42% track,
accent@85% border, `kAccentKnob` knob right. Off: white@10% track, white@42% border,
`#EFF5FB@72%` knob left.

`widgets::Checkbox` is **deleted**, not repurposed. Direction E's answer was "switch = a
setting, checkbox = a member of a set", which is defensible but still ships two looks for
one meaning — the exact shape of the original complaint. A set of booleans is a group of
switch rows with a `4 of 7` count in its group header and `all · none` actions in the
header's right slot. It is two rows taller than a checkbox list and it is the same
control everywhere.

*Use for:* `Allow Tearing`, `Force Grab Cursor`, `VRR / Adaptive Sync`, `HDR`,
`Show System Monitor`, `Mute`, `Override Global Config`, every HUD module, every HUD row.

#### 2. Selector — **the only mutually-exclusive-choice control**, at two densities
A lane-wide track divided into N cells. Active cell: accent@30% fill, accent@90% inset
border, `kAccentSegText`, Mono 600 11. Inactive: `TextMeta` on white@4.5%, separated by
white@22% hairlines.

**The helper measures the labels against the lane and picks the density. A caller never
chooses and never gets a cramped row:**

- **Expanded** — every label fits its cell. `alpha | additive | inverted`,
  `gamescope | game`.
- **Compressed** — a label does not fit. The active cell expands to hold its label; the
  others become 6px slivers, and `‹ ›` cycling arrows appear at the track's ends.
  `‹ ▏▏ fsr ▏▏ ›` for `linear/nearest/fsr/nis/pixel`; `‹ ▏▏▏ 301 · wine64-preloader ▏▏▏▏▏ ›`
  for nine audio streams.

*Why this and not "segmented ≤5, dropdown otherwise":* a dropdown is a **different
control** — a box with a caret — for the identical meaning. That is the switch-vs-checkbox
sin repeated. The compressed Selector is the *same* control at a different density, and
it carries information a dropdown box cannot: how many options exist, and where you are
among them. Clicking the active cell still opens a searchable list for very long sets
(>12 options), but the row itself never changes shape.

*And:* a Selector sets a value. It never navigates. There is no `BeginTabBar` in the API,
so this rule cannot be broken by accident. All four existing tab bars are deleted.

#### 3. Slider — a bounded continuous value
`RowTall`. Value + unit on the label line, right-aligned to the lane. The measured
`slider-widget-spec.md` geometry moves in unchanged: 6px track, 10×22 handle with the
two-rect glow, `#36BDDD@50% → #47CAEA` gradient fill. Min/max marks under the track.
Two additions:

- **The default tick** — a 1px `white@55%` mark on the rail at the default value.
- **The Trail** — a faint accent band between the default tick and the handle. "How far
  have I moved this, and in which direction" becomes readable without reading a number,
  which is what you want when the number is in a 390px panel two feet away and the game
  is moving.

`Shift`-drag = ×0.1 fine. `Ctrl`-click converts the value readout into inline numeric
entry. Wheel = one step. Documented once; true of every slider.

#### 4. Stepper — an exact, unbounded or discrete value
The **same track shape as the Selector**, with `−` and `+` cells at the ends and the
value centred in Mono 500 14 `kAccentValue` with its unit in Mono 10.5 `TextMeta`. Click
the value to type. Holding accelerates after 400 ms. A distinguished value renders as a
word: `0` shows as `Unlimited` for `FPS Limit`.

Sharing the track shape with the Selector is deliberate: the whole lane vocabulary is
"one 26-tall bordered track", and a stepper is that track with two arrow cells.

*The rule the helper applies:* bounded and continuous → Slider. Unbounded, or discrete
and exact → Stepper.

#### 5. Text — free text
The same 26-tall track, Mono 11.5 left-aligned, caret in accent, a 1px accent bottom edge
while focused, placeholder in `TextMeta`. A validation failure puts the message under the
track in `Danger` and turns the track's border `Danger`.

#### 6. Readout and Meter — outputs, not inputs
`Readout`: Mono 12 `TextMeta`, right-aligned in the lane, with a 6px `Ok` dot when the
value is live. `Meter`: a 28-segment bar filling the lane, lit accent@85%, hot segments
in `Warn`. **Both have no hover highlight and no state edge** — that absence is the
structural expression of *never show a value the overlay didn't verify*.

#### 7. Actions
Flat 26-tall buttons, Mono 11 lowercase, right-aligned in the lane, at most two per row.
Three intents: `neutral` (white@4.5% fill, white@42% border), `accent` (at most one per
group), `danger`. `Danger` is the only red and it is hue-fixed — a green "delete
permanently" is a bug, not a theme. Every danger action is required by the type system to
carry a confirm step (`API.md` §3.5).

### 3.3 Composites — the part E called "orphaned"

The user's complaint: *"For some 'multiline' elements, like Monitor > Placement > Anchor
settings, the elements look weirdly placed, almost orphaned."*

The diagnosis: the 3×3 grid was floated in the middle of a row because it was too tall for
the row grammar, so the grammar was abandoned for it. The rule that fixes it:

> **A composite occupies the lane's full width and grows downward inside it. It never
> spans the label column, and it never leaves the lane.**

and, because a composite is by definition too big to be good at 190 base:

> **Every composite has a compact form in the Spine's lane and a full form in the Scope's
> probe. Selecting the row is what promotes it.**

That is the user's own prescribed cure — *"This type of information shouldn't leave the
UI, but rather wander into the Inspector Rail"* — applied to a control instead of to a
paragraph.

#### The Stage — `Monitor ▸ Placement`, and the showcase of this direction

**Compact form (in the Spine's lane, 190 × 107):** a 16:9 miniature of the output with
the nine anchor thirds ruled in white@16%, and the monitor block drawn at its real
position and its real relative size. Not an abstraction of the placement — the placement,
at 1:10.

**Full form (in the Scope's probe, 390 × 219):** the same miniature at 1:5, over a
dimmed thumbnail of the actual frame, with:

- the **nine anchors as real 9px click targets**, the active one filled;
- the **monitor block drawn to scale**, including its module rows, so a taller stack
  looks taller and a bigger font looks bigger — the block is generated from the same
  `hudGeom()` the real HUD uses, not drawn twice;
- **draggable**: drag the block anywhere; the nearest of the nine anchors wins, and the
  distance from the block to that anchor's edges *becomes* `margin_v` and `margin_h`,
  clamped to 0–128;
- **live margin rulers** — dashed accent lines from the block to the two edges it is
  anchored to, labelled `48 px` / `32 px`. Centre anchors draw no ruler on that axis,
  because a centred thing has no margin, and showing a disabled margin control there was
  one of the original design's small lies;
- keyboard: `Enter` grabs, arrows move the anchor, `Shift`+arrows nudge margins by 1,
  `Esc` releases.

**One control replaces three rows** — the 3×3 grid, `Vertical margin`, and
`Horizontal margin` — and it is the only place in the product where those four values can
be set, so the two call sites that drifted (notification placement and monitor placement)
cannot drift again.

**And it previews itself.** Because the block is computed by the same function that
positions the real HUD, moving it in the Scope moves the real readout over the game as
you drag. This is the thesis of the whole direction in one control: the compositor knows
where the thing is, so the setting should show you where the thing is.

#### The Hue rail — `Appearance ▸ Accent hue`
**Compact:** a 22-tall strip whose pixels *are* the real accent at each hue (sampled at
the token's own OKLCH L/C, so the strip cannot lie about what you will get), plus eight
preset swatches. **Full (Scope):** the entire accent family — `kAccent`, `kAccentEdge`,
`kAccentValue`, `kAccentSegText`, `kAccentKnob`, `kAccentHandle` — recomputed live at the
hue under your cursor, each labelled with its token name, plus the `oklch(.74 .12 h)`
readout. Never an ImGui colour wheel, never a popup, never a second floating surface.

#### The Log — `System ▸ Log`
A `Raw` category: no lane, no label column, because there are no settings in it. A sticky
filter bar (source Selector, text filter, `2 err` / `2 warn` severity chips, `auto-scroll`
switch), then the stream: Mono 11.5, 20 base per line, a 34-wide line-number gutter, a 2px
severity left edge per line, no wrapping (horizontal scroll instead, so line numbers stay
meaningful). The **Scope wraps the selected line** and gives it its timestamp, severity,
source and a copy action. The scrollbar carries a severity minimap so a 40 000-line buffer
shows *where* the problems are without scrolling.

### 3.4 Where label, value and help go — the invariants

| Thing | Where, always |
|---|---|
| Label | Label zone, left, Sans 400 13.5 `TextBody` (500 `TextPrimary` when selected) |
| Value | Inside the lane, Mono 500, `kAccentValue`, right-aligned to the lane's right edge |
| Unit | Immediately after the value, Mono 400 10.5 `TextMeta`, never inside the number's run |
| One-line hint | Under the label, Mono 400 11 `TextMeta` |
| Disabled reason | Under the label, Mono 400 11 **`Warn`**, prefixed `⚠` |
| Full help | **Scope only**, Sans 400 13 `TextBody`, wrapped, max 3 sentences |
| Default, range, destination file | Scope, in the Facts grid, read from the binding |
| Related settings | Scope, as jump links |
| Validation error | Under the control, Mono 11 `Danger` |

Numbers are **always** Geist Mono; prose is **always** Geist Sans. Enforced structurally:
the helper picks the font per zone, so a caller cannot put a number in Sans.

### 3.5 Disabled — and why it is not dimmed

Direction E, and the shipping code, dim a disabled row to 34% opacity. **That is what
breaks contrast**, and this project has had three "too dark to read" complaints. So:

> **A disabled row is never dimmed.** Its label stays at full `TextBody`, its control
> drops to a plain-white non-accent treatment at 50% and stops taking input, and a
> **mandatory reason string** appears under the label in `Warn` (8.88:1).

The API makes the reason impossible to skip: `EnabledWhen( pred, reason )` has **no
overload without the reason**. Existing copy already fits: *"HDR is off — these controls
do nothing until it's on."*, *"the scaling filter is not fsr, nis or pixel"*,
*"Install WirePlumber's CLI (wpctl) to control per-app volume."*

### 3.6 Empty and indeterminate

Empty state: a 120-tall centred band, Mono 11 `TextMeta` one-liner, optionally one accent
button. *"No profiles saved yet."*, *"No PipeWire audio streams found on the system right
now."* Progress: a 3px bar in the lane with a Mono 11 caption — *"Collecting… 42s of
60s"*. **No spinners**: a spinner over a game is a moving object competing with the game.

---

## 4. The Probe — what the Scope shows

The Scope's top block is always a live instrument. Six kinds, a **closed set** — a call
site picks one from the set or picks nothing, and cannot draw.

| Probe | Shown for | What it draws |
|---|---|---|
| **Frame** | Filter, Sharpness, SDR Gamut Wideness, every ReShade effect | A 240×135 region of the **live frame**, resampled/processed both ways, split by a draggable wipe handle, tagged `linear` \| `fsr`. Dragging the slider updates the right half in real time. |
| **Trace** | FPS Limit, VRR, Adaptive Brightness | The last 240 frames of frametime, with **your cap drawn on it as an orange line**, so you can watch the trace flatten onto the cap you are setting. VRR variant plots refresh following presentation. Adaptive Brightness plots measured scene luminance against applied gain. |
| **Curve** | SDR-on-HDR Brightness, tonemap operator | The PQ transfer curve with SDR white marked on it. |
| **Meter** | Volume, Mute, Stream | Live L/R peak from the selected PipeWire node, hot segments in `Warn`, dB readout. |
| **Stage** | Placement | §3.3. |
| **Swatches** | Accent hue | §3.3. |
| **Delta** *(default)* | **everything else** | Generated from the binding alone: the current value at Mono 600 30, then a number line with the default ticked, the current value marked, and the span between them filled. |

**The Delta probe is the load-bearing one.** It is what makes "the Scope is always a live
instrument" a guarantee rather than an aspiration: because `Binding` already knows the
value, the default and the range, a setting with no declared probe still gets a real
visualisation, and **no selection can ever produce an empty Scope**. A caller who forgets
to think about the Scope still gets a good Scope.

*Cost, stated honestly:* the Frame probe is the ambitious one. See `FEASIBILITY.md` §5 —
it needs one small readback or a shared sampler from the composite pass, it is the single
thing in this proposal most likely to be cut, and the design is built so that cutting it
degrades to the Delta probe with no structural change.

---

## 5. The command palette (adopted from Direction B)

`Ctrl+K` opens a fuzzy search over every registered entry, matched against label, key,
category and keywords, with the matched characters highlighted and the current value
shown on the right. `↑↓` moves, `Enter` opens that setting's category, selects the row,
and fills the Scope. It reports `4 of 57` so the size of the product is legible.

This is adopted as a **feature**, exactly as `redesign/README.md` records: the registry
idea is portable, the navigation model is not. **One registration yields the Spine row,
the palette entry and the Scope page** — that is the property the API is designed around
(`API.md` §2), and it is why an eleven-item accordion is not a navigational cost.

---

## 6. Theming

### 6.1 Colour roles — call sites never touch hex

| Role | Value | Used for |
|---|---|---|
| `Slab` | `rgba(12,14,17,.94)` | both slabs |
| `Sunk` | `white@4.5%` | control track fills |
| `Lift` | `white@7.5%` | hover fills |
| `Hair` | `white@10%` | row separators (decorative) |
| `Rule` | `white@14%` | group rules |
| `Edge` | `white@20%` | slab borders |
| **`Ctl`** | **`white@42%`** | **every control boundary that identifies a control** |
| `CtlHover` | `white@55%` | the same, hovered |
| `TextPrimary` | `#EFF5FB @96%` | selected labels, titles, hero numbers |
| `TextBody` | `#EFF5FB @78%` | labels, prose, list primaries |
| `TextMeta` | `#EFF5FB @60%` | units, hints, marks, line numbers, chips |
| `Accent*` | the existing OKLCH family, hue-live, unchanged | **state only** |
| `Ok` | `#6ED274` | liveness dots only |
| `Warn` | `#FFA94D` | disabled reasons, frametime outliers, warn log lines |
| `Danger` | `#FF8E7E` `oklch(.72 .16 27)`, hue-fixed | destructive actions, errors, invalid input |

**There is no fourth text role.** E's `TextFaint` at 30% measured 3.79:1 and was cut
rather than argued for. Line numbers, placeholders and disabled meta all use `TextMeta`.

`TextBody` at 78% and `TextMeta` at 60% are both raised from E's 62% / 44%. Issue #62
already moved the shipped meta alpha to 44% after three "too dark" complaints; this goes
further and makes 60% the floor, so a fourth complaint has nowhere to originate.

**Accent budget.** One user-chosen OKLCH hue drives the whole family; L and C stay fixed
per token (existing `UpdateAccentFamily()`, kept verbatim). Accent is spent on **state
only** — open category, selected row, changed value, active selector cell, on-switch,
slider fill, focus ring, Ledger tick. Never on decoration, never on a header, never on a
border that is not communicating state. A screen where nothing has been changed is almost
monochrome, and that is the correct look for "everything is at default".

### 6.2 Measured contrast (sRGB, WCAG 2.1)

Measured, not asserted, against two composited backgrounds:

- **nominal** — the slab over a typical darkened frame: **`#101216`**
- **worst case** — the slab over a blown-out white frame at the shipped blur (1.0) and
  darkening (0.8) defaults: **`#1A1D22`**

| Role | Alpha | vs nominal | **vs worst case** | Needs |
|---|---|---|---|---|
| `TextPrimary` | .96 | 15.74 | **14.25** | 4.5 |
| `TextBody` | .78 | 10.57 | **9.77** | 4.5 |
| `TextMeta` — the floor | .60 | 6.63 | **6.31** | 4.5 |
| `kAccentValue` `#78DBF6` | 1.0 | 11.84 | **10.67** | 4.5 |
| `kAccentText` `#71D4EF` | 1.0 | 11.04 | **9.95** | 4.5 |
| `kAccentSegText` `#A9EAFD` | 1.0 | 14.17 | **12.77** | 4.5 |
| `Ok` `#6ED274` | 1.0 | 9.94 | **8.96** | 4.5 |
| `Warn` `#FFA94D` | 1.0 | 9.85 | **8.88** | 4.5 |
| `Danger` `#FF8E7E` | 1.0 | 8.41 | **7.58** | 4.5 |
| **`Ctl` — control boundary, idle** | .42 | 4.10 | **4.04** | 3.0 |
| `CtlHover` | .55 | 6.20 | **5.94** | 3.0 |
| Switch-off border / knob | .42 / .72 | 4.10 / 9.12 | **4.04 / 8.51** | 3.0 |
| Switch-on border / knob | .85 acc / 1.0 | 6.38 / 12.49 | **5.86 / 11.26** | 3.0 |
| Selector active border | .90 acc | 7.03 | **6.42** | 3.0 |
| Slider fill / handle | 1.0 | 9.73 / 14.13 | **8.77 / 12.73** | 3.0 |
| Focus ring `#4FD0F1` | 1.0 | 10.38 | **9.35** | 3.0 |
| Stage grid line / anchor dot | .42 / .50 | 4.10 / 4.95 | **4.04 / 4.80** | 3.0 |
| Ledger tick | 1.0 | 8.46 | **7.63** | 3.0 |

**Worst case anywhere in the design: 4.04:1** for a control boundary (needs 3.0), and
**6.31:1** for the dimmest text that carries meaning (needs 4.5).

Three elements sit below 3:1 *on purpose* and are listed so they are judged rather than
discovered: the slider's unlit rail (2.53:1), the switch-on track fill (2.41:1) and the
selector's active-cell fill (1.83:1). None of them identifies its control — WCAG 1.4.11
requires 3:1 only for the parts *needed to identify* a component and its state, and in
every case that job is done by an element measured above: the slider by its 12.73:1
handle and 8.77:1 fill, the switch by its 5.86:1 border and 11.26:1 knob, the selector
cell by its 6.42:1 border and 12.77:1 label.

**Two design rules follow directly from this table and are not negotiable:**

1. **Disabled rows are not dimmed** (§3.5). Multiplying a row by 34% takes `TextMeta`
   from 6.31:1 to roughly 1.6:1. Dimming is how the previous complaints happened.
2. **`ui.darken` is clamped by the helper.** Backdrop darkening below the value at which
   `TextMeta` would fall under 4.5:1 over a white frame is refused, with the reason shown
   in the row. The contrast budget is a real constraint on a user-facing slider, so the
   helper owns it rather than trusting the user not to break their own UI.

### 6.3 Type — Geist, six roles

| Role | Family / weight | Size | Use |
|---|---|---|---|
| `Title` | Mono 600 | 11 UPPER | slab titles, region titles |
| `Section` | Mono 500 | 10 UPPER | section headers, group bands, Scope block headers |
| `Label` | Sans 400 | 13.5 | labels, prose, list primaries (500 when selected) |
| `Statement` | Sans 500 | 17 | the Scope's setting name |
| `Value` | Mono 500 | 14–15 | every numeric / state readout |
| `Meta` | Mono 400 | 10.5–11 | units, hints, marks, line numbers, chips |

Six roles, down from ten `fonts::Style` entries today. Weight carries hierarchy and
nothing else; everything not listed above is 400.

### 6.4 Elevation, motion, density

**Elevation:** two levels. The slabs, and popups (the palette, a long Selector's list) at
`Slab` + a 1px `Edge` + one 8px black@25% expansion. **No shadows inside a slab.**

**Motion:** three durations, one easing (`1 − (1 − t)³`), all lerped against
`io.DeltaTime`.

- **90 ms — state.** Hover, switch knob travel, selector cell fill, focus ring.
- **160 ms — region.** Accordion open/close, Scope dock/overlay/hide, peek in and out.
- **240 ms — surface.** Overlay open/close: slabs slide 24 base in from their edges with
  alpha 0→1 while the veil ramps.

Two prohibitions. **Values never animate** — a slider handle moving to a typed or reset
value snaps; animated values over a running game read as input lag. **Regions never move
except when opening or closing** — no reflow, no content-driven resizing.

**Density without noise, four rules:** one rule weight (all separators 1px; three alphas);
no nested containers (slab → group band → row, and only the slab has a fill); alignment
does the work grouping usually does (one label column, one lane, down the whole Spine);
and colour is state.

---

## 7. What this replaces, concretely

| Today | Becomes |
|---|---|
| Bottom dock, 7 buttons, hint line | The Spine's accordion (always visible, always in the same place) |
| 6 floating windows: drag / collapse / close / tile / measure-and-grow | Two rects from one pure function; ~900 LOC of `Chrome.cpp` deleted |
| 4 tab bars (`GamescopeTabs`, `ConfigTabs`, `LogTabs`, `SystemMonitorTabs`) | Accordion categories; **no `BeginTabBar` in the API at all** |
| Per-panel ad-hoc row layout | One Row grammar, one Lane, two heights |
| `widgets::Toggle` / `widgets::Checkbox` used interchangeably | One switch. `Checkbox` deleted (issue #60: zero callers) |
| Segmented control *or* an ad-hoc combo | One Selector, two densities, chosen by measurement |
| 3×3 grid + 2 margin sliders, drifting across two call sites | One Stage composite, one call site |
| Stock `ImGui::SliderFloat` still live in `FpsDisplay.cpp` (7 sliders) | One `Slider()`; the drift that produced them is structurally gone |
| Tooltips as the only help channel | The Scope as a permanent help *and preview* channel |
| Per-panel opacity, width, padding constants | The shell owns all geometry |

Five things are kept verbatim because they were measured and are liked: the **slider**
(`slider-widget-spec.md` in full), the **toggle** geometry, the **segmented control**'s
cell painting (reused as the Selector's expanded density), the **3×3 position grid**'s
hit model (reused as the Stage's nine targets), and the **OKLCH accent family** with its
live hue. This is not a from-scratch redraw — it is the same widget vocabulary placed in
a structure that can hold it consistently, plus one genuinely new thing (the Probe).
