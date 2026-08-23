# Direction E1 — Inspector Rail, refined

A disciplined refinement of Direction E for the gamescope-ritz settings overlay.
Date: 2026-08-23. Design only — no repository code was modified.

Companion files: `API.md` (the helper layer + registry), `FEASIBILITY.md` (honest
ImGui assessment), `index.html` (self-contained interactive mockup).

---

## 0. What changed, and what did not

**Not changed — deliberately.** The three regions, their order, their fixed widths, the
Inspector Contract, the Category Card, the responsive ladder, the one-slab-no-windows
decision, the OKLCH accent family, the measured slider geometry. The inspector rail is
the reason this direction survived review; nothing below weakens it, and three of the
five changes *strengthen* it by giving it more to hold.

**Changed — the five things review found wrong:**

| # | Critique | Fix | Where |
|---|---|---|---|
| 1 | switches and checkboxes mixed | **The checkbox is deleted from the taxonomy.** Every binary is a switch. | §2.2 |
| 2 | controls left/right/centre "basically random" | **Right-bound rail rule**: every control's *right* edge is at `row right − 36`. E's 44% label column produced *left*-bound controls at a consistent offset — the exact thing complained about. | §2.1 |
| 3 | intimidating density | **`Readout()` no longer exists on `Sheet`.** Read-only rows are structurally impossible in a sheet; they live in the Category Card. Plus `Depth::Expert`. | §3 |
| 4 | the inspector is amazing | protected, and given more work to do | everywhere |
| 5 | multi-line controls look orphaned | **The composite row group**: one head row + part rows, one state edge, one closing hairline. | §2.4 |

**Two scope changes since E.** Gamepad support is dropped entirely — mouse and keyboard
only, which deletes E's own worst-rated risk ("three focus scopes is genuinely harder
for gamepad"). And registration now follows `b-command/API.md`: one declarative registry,
built at startup, feeding the row, the palette entry and the inspector page (§6).

---

## 1. Navigation

### 1.1 The shell — unchanged

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ ● GAMESCOPE-RITZ  settings                app 1174180   global.json  ▥  ✕     │ 40
├────────────┬──────────────────────────────────────────┬───────────────────────┤
│            │ DISPLAY / UPSCALING   differs 2   ⌕      │ Category · Upscaling  │ 56 / 48
│  RAIL 240  │                                          │                       │
│            │   SHEET (flex) · content column ≤ 720    │   INSPECTOR 384       │
│  11 items  │   rows · groups · lists · composites     │   help · provenance   │
│  5 sections│                                          │   live facts · expert │
│            ├──────────────────────────────────────────┤                       │
│            │ reset category   CTRL+K · TAB · CTRL+D   │                       │ 44
└────────────┴──────────────────────────────────────────┴───────────────────────┘
```

All numbers are **base units**; `px = base × display_scale` (0.5–2.0). No call site
writes a pixel. Slab width `min(surfaceW × 0.90, max(1560 × scale, min(surfaceW × 0.90, 1180)))`,
height `min(surfaceH × 0.86, 940 × scale)` — 1560 × 928 at 1.0× on 1920×1080.

### 1.2 The Rail — unchanged

Eleven items in five sections (`DISPLAY` ×4, `IMAGE`, `AUDIO`, `SYSTEM` ×2, `SETUP` ×3).
Item 40 tall, 20px icon at x=16, Sans 14 label at x=48, optional Mono 11.5 count chip
right-aligned. Active item: accent@10% fill and a **2px accent left edge**. Section
header 26 tall. Collapsed rail = 64 wide, icons only, the 2px edge survives — which is
why "you are here" is an edge and not a text weight.

The 1px accent thread from the active rail item into the sheet breadcrumb is kept. It is
three `AddLine` calls and it is the design's signature.

### 1.3 What drives the Inspector — unchanged rule, wider remit

> **The Inspector always shows the current selection, and the current selection is
> always a Sheet row.** Never hover. Selection persists per category.

When nothing is selected the Inspector shows the **Category Card**, which is now the
single most useful screen in the overlay because §3 moved real content into it:

1. one-sentence category description;
2. `N of M settings differ from default`, each a click-to-jump link showing `old → new`;
3. **Live** — the read-only facts this category owns (effective composite path, connector
   and mode, PipeWire backend, frametime statistics, atlas size). *This block is what
   used to clutter the sheet.*
4. **Writes to** — `global.json` or `games/1174180.json`, plus the routing reason;
5. **Expert** — the `Depth::Expert` settings, editable here;
6. category actions: `reset category`, `save as preset`, `apply profile…`.

### 1.4 The Inspector Contract — tightened

E's contract:

> **The Sheet alone is a complete UI.** Every setting a user needs during a normal
> session is reachable and editable in the Sheet. The Inspector adds *depth* — help,
> provenance, defaults, ranges, expert parameters, per-item detail — never *access*.

E then carved an exception for `Depth::Expert`, which quietly broke the contract: with
the Inspector closed, an expert setting was unreachable. E1 closes that hole:

> **An `Expert` entry is reachable from the Sheet by its owning group's chevron, and
> that chevron opens the Inspector if it is closed.** Closing the Inspector never removes
> access; it makes expert access cost one click that restores the Inspector.

Consequences unchanged: the Inspector is closable (`Ctrl+I`, the ✕, or persisted
`ui.inspector = hidden`); at 2.0× it degrades to a drawer with no loss; a screenshot of
just the Sheet is a legitimate answer to "what can I set here".

### 1.5 The list-and-inspector pattern — unchanged

| Panel today | Sheet becomes | Inspector shows |
|---|---|---|
| SHADERS (3 stacked groups) | list of effects: name, switch, live meta | that effect's parameters |
| SYSTEM MONITOR (6 tabs) | list of 4 modules + shared appearance + placement | that module's rows, colour, window |
| AUDIO (bespoke) | one fader composite per stream | node detail, match reason, override |
| CONFIG/Profiles | list of profiles | contents, diff vs current, actions |
| LOG (2 tabs) | Raw line stream + filter bar | the selected line, wrapped, explained |

### 1.6 Input — mouse and keyboard only

| Key | Action |
|---|---|
| `Ctrl+Shift+O` | toggle the overlay |
| `Tab` / `Shift+Tab` | cycle region: Rail → Sheet → Inspector → Rail |
| `↑ ↓` | move selection within the focused region |
| `← →` | at a region edge, cross into the neighbour; inside a control, adjust it |
| `Enter` / `Space` | activate / toggle / begin entry |
| `Ctrl+←/→` | previous / next rail item without leaving the Sheet |
| `Ctrl+K` | **command palette** over the whole registry |
| `Ctrl+D` | reset the selected row to default |
| `Ctrl+I` | toggle the Inspector · `Ctrl+B` toggle the Rail |
| `Esc` | close palette → clear selection → close Inspector → close overlay |

Clicking a control both selects the row and edits it — there is no "select first" tax.

**Gamepad is out of scope.** gamescope has no gamepad input path today (libinput ignores
gamepads; the overlay enables keyboard nav only), so any controller design needed a new
evdev reader plus Steam Input integration. Dropping it removes the routing table, the
third focus scope, and the `KeyboardOnly()` / `Suggestions()` assertion pair that B
needed for text fields.

---

## 2. The control taxonomy

### 2.1 The Row, and the one alignment rule

```
│▌│  Label text                     │        [ control ]  │  ⟡  │
 2   12       remainder                    control zone      36
 │   │                                │         ▲          │
 │  label zone (flexible)             │   RIGHT EDGE HERE  affordance gutter
 state edge                                = row right − 36
```

- **State edge** — 2px at x=0. accent@80% when the value differs from default;
  accent@40% when the row is selected; invisible otherwise.
- **Label zone** — starts at x=12 and takes *whatever is left*. It is no longer a fixed
  percentage, because a fixed percentage is what produced the misalignment: it pins the
  control's **left** edge, and controls have wildly different intrinsic widths.
- **Control zone** — `width = clamp(180, 0.50 × rowWidth, 320)` base, **right edge fixed
  at `rowWidth − 36`**. At the 720-unit content column that is 320 wide; at a 512-unit
  column it is 256. The *width* varies with room; the *right edge never moves*.
- **Affordance gutter** — the last 36: reset dot when the row differs, chevron when the
  row has Inspector depth, lock glyph when read-only, drag handle in a switch-list.

> ### THE RAIL RULE
> **Every control is drawn inside a box whose right edge is the sheet rail. A control
> chooses a *width*; it never chooses an *x*.**
>
> Two width classes and no third:
> - **Fit** — intrinsic width, flush against the rail. Switch (30), stepper (136),
>   3×3 grid (96), swatch strip, button rows.
> - **Fill** — the whole control zone. Segmented, dropdown, text entry, slider track,
>   hue strip, meter.
>
> The value readout of a slider is right-aligned to the same rail, above its track.
> The same rule, with the Inspector's own rail, governs the Inspector.

**Why it cannot be violated by a call site.** `RowCtx` exposes exactly one placement
method, `ImRect Place( float flWidth )`, which returns `ImRect( rail − w, y0, rail, y1 )`.
There is no `x` parameter anywhere in the public API, no `SameLine`, no `SetCursorPosX`.
Belt and braces: `BeginRow` pushes a clip rect ending at the rail, so a control that
somehow drew past it is visibly truncated rather than silently misaligned; and the
`ui_rulers` ConVar draws the rail over every sheet, so one screenshot audits the whole
product. (The mockup has the same switch — press `R`.)

**Row heights: two classes.** `Row` = 44, `RowTall` = 76 (slider with marks, hue strip,
meter stack). The **one** exception is `RowBlock` = 112, which exists solely because the
3×3 position grid is 96 tall and is being kept pixel-for-pixel; it is named here, it has
exactly one control in it, and it cannot be reached from a call site. LOG lines are 20.
Inspector rows are two-line, 64.

**Rows are separated by a 1px `#FFFFFF @ 6%` hairline, not by spacing.** Groups get air;
rows do not.

### 2.2 Binaries — one control, one meaning

> **Every binary setting is a Switch. There is no Checkbox in the taxonomy, and
> `Area::Checkbox()` does not exist.**

E kept a checkbox for "a boolean that is a member of a set" and used it for HUD rows,
chain order and module rows. That was a regression against the shipping product, where
the sweep in issue #60 already removed every caller and left `widgets::Checkbox` with
**zero call sites**. The mockup reproduced a problem the codebase had already solved.

Set-membership is now carried by **grouping, not by glyph**: a *switch list* is a
composite (§2.4) whose head row states the count (`3 of 4`) and carries `all` / `none`,
and whose parts are ordinary rows with an indented sub-label and a switch on the rail.
That is strictly more information than a checkbox conveyed, in one glyph instead of two.

**The audit — every binary in the design, and its control:**

| Binary | Control | Why |
|---|---|---|
| `Allow Tearing`, `Force Grab Cursor`, `VRR`, `Force composite`, `Low-latency pacing`, `HDR Output`, `Force HDR10 PQ`, `Show System Monitor`, `Override Global Config`, `Apply on launch`, `Scroll long titles`, `Upscale before overlay`, `Protect skin tones`, `Auto-scroll` | Switch | a boolean setting |
| effect on/off in the Shaders list; module on/off in the Monitor list | Switch (in the list row's rail slot) | a boolean setting that happens to live on a list row |
| chain-order members; FPS/CPU/GPU module rows | Switch (switch-list parts) | a boolean setting; the *set* is expressed by the composite head |
| **LOG severity filters `err 2` / `warn 7` / `info`** | **toggle chip** | **the one exception, below** |

**The one licensed exception, enumerated so it cannot spread.** The LOG filter bar's
three severity chips are toggle chips, not switches, because they are **view filters, not
settings**: they are not registered, not persisted, not resettable, not searchable, and
they carry a live count in their own label. They exist in exactly one place — a `Raw`
sheet's filter bar — and `PaneCtx` is the only surface that can produce one. Everything
else that looked like a candidate was demoted to a text action instead (`show inactive
streams`, `all`, `none`), which is not a binary control at all.

**One more deletion:** the trailing `on` / `off` word that E drew beside each switch is
gone. It is redundant with the switch, and under the rail rule it would have to sit to
the *left* of the switch, which reads backwards. The word survives where it is genuinely
needed — in the palette's value column and in the Inspector's `default off`.

### 2.3 The complete control set

Each entry names the control, its width class, and the rule that selects it. **The rules
are decidable from the declaration, and the helper decides them** — that is what makes an
inconsistent screen hard to build.

1. **Switch** — *Fit*, 30×15 track, 11×11 knob. Existing `widgets::Toggle` geometry kept,
   with the off-state border raised to `LineControl` (§5). *Use when:* any boolean setting.

2. **Segmented control** — *Fill*, equal cells, 4px gaps, lowercase, 28 tall.
   *Rule:* `options ≤ 5` **and** every label `≤ 8` chars **and** the set is static.
   The helper measures and **auto-downgrades to a dropdown** otherwise; a caller passing
   six options cannot ship a cramped row. *And a segmented control sets a value, never
   navigates:* there is no `BeginTabBar` anywhere in the API.

3. **Dropdown** — *Fill*, 28-tall box, chevron right; popup anchored to the rail, rows 26
   tall, max 280 then clipper-scrolled. *Use when:* many or dynamic options — the audio
   stream picker, mode lists, profile picker.

4. **Slider** — *Fill*, `RowTall`. Keeps `slider-widget-spec.md` geometry exactly: 6px
   track / 3.5 radius, 10×22 handle with the two-rect glow, 8px gaps. Value Mono 500 16
   right-aligned on the rail above the track; min/mid/max marks below; **default tick**
   on the rail; `Shift` = ×0.1 fine, `Ctrl`-click = inline numeric entry, wheel = one step.
   *Rule:* bounded **and** continuous.

5. **Stepper / numeric entry** — *Fit* (136 wide), `−` / `+` 26px zones, Mono 500 14
   centred, unit suffix Mono 11 meta, hold-to-accelerate after 400 ms, `ZeroMeans()` word
   substitution (`0` → `Unlimited`). *Rule:* unbounded, or discrete and exact.

6. **Text entry** — *Fill*, 28-tall, accent bottom edge while focused, validation message
   **under the box, right-aligned**, border to `Danger@50%` while invalid.

7. **Buttons** — *Fit*, 28 tall, Mono 11.5 lowercase, at most two per row, right-bound.
   Three intents: `neutral`, `accent` (≤1 per group), `danger` (the only red; requires a
   confirm step, enforced by the type system — §API 3.5).

8. **Meter / graph** — *Fill*, 5px segmented bar (20 segments, 8px pitch) or a graph
   strip. No hover, no state edge: it is an output.

9. **Readout** — *Inspector only.* See §3.

10. **Chips and badges** — status only, never interactive, except the enumerated LOG
    severity filters (§2.2).

11. **Empty state** — a 120-tall centred band, Mono 11.5 meta one-liner, optionally one
    accent button.

12. **Progress** — a 3px bar in the control zone with a Mono 11.5 caption. No spinners:
    a spinner over a game is a moving object competing with the game.

13. **Disabled** — the label and control dim to 34%; **the mandatory reason string does
    not**, and is drawn full-strength on its own line under the row. `.DisabledUnless(
    bool, const char *reason )` has no overload without the reason.

14. **Composite** — §2.4.

15. **Raw sheet** — the LOG. No label column, no row grammar: sticky filter bar (44),
    20-unit lines with a 52-unit number gutter and a 2px severity edge, no wrapping,
    severity minimap on the scrollbar, click to select, the Inspector wraps and explains
    the selected line.

### 2.4 Composite controls — the fix for "orphaned"

The named offender was **Monitor → Placement → Anchor**: E drew a 112-tall row whose
control zone held a column of two steppers *beside* a right-aligned 3×3 grid, with the
label alone at the top-left. Three objects, three alignments, one label between them —
it reads as debris, and every other multi-part control in E had its own ad-hoc
arrangement.

> ### THE COMPOSITE RULE
> **A composite is not a special layout. It is a row *group*.**
>
> - One **head row** (`Row`, 44): the label in the label zone (optionally with one status
>   chip and one sub-line), the composite's **summary value** on the rail, the affordance
>   in the gutter.
> - Then one or more **part rows**, each an *ordinary row* — same grammar, same rail —
>   whose label is a sub-label indented by 12 and drawn in `TextMeta`.
> - **No hairline between parts.** One hairline closes the whole group.
> - **One state edge spans the whole group**, lit if any part differs from default.
> - Parts are not separately selectable; selecting any of them selects the composite,
>   and the Inspector shows the whole composite.
> - Part heights come from the same two classes (44 / 76), plus the single `RowBlock`
>   (112) that exists only for the 3×3 grid.

Anchor becomes, literally:

```
│▌│ Anchor                                            top-right · 32 / 32   ⟡│
│▌│   Corner                                      ┌───┬───┬───┐              │
│▌│                                               ├───┼───┼─█─┤              │
│▌│                                               └───┴───┴───┘              │
│▌│   Vertical margin                             [ −   32 px   + ]          │
│▌│   Horizontal margin                           [ −   32 px   + ]          │
└──────────────────────────────────────────────────────────────────────────────
```

Every element is labelled, everything ends on the rail, the group is one bounded object,
and the summary on the head row means you can read the answer without parsing the parts.

**Applied to every composite in the design, with no per-site variation:**

| Composite | Head value | Parts |
|---|---|---|
| **Anchor** (Monitor placement, Notification placement — two call sites, one implementation) | `top-right · 32 / 32` | Corner (grid, `RowBlock`) · Vertical margin (stepper) · Horizontal margin (stepper) |
| **Fader** (one per audio stream) | `78%` | Volume (slider, `RowTall`) · Output level (L/R meters, `RowTall`) · Actions (`mute` `solo`, `Row`) |
| **Switch list** (chain order, module rows) | `all` / `none` actions; count chip in the label zone | one switch row per member, drag handle in the gutter when order matters |
| **Accent colour** (theme, per-module colour) | `218°` | Hue (OKLCH strip, `RowTall`) · Presets (8 swatches, `Row`) |

The old §2.3.10 "colour picker, two forms" collapses into one composite used twice. The
old §2.3.3 "multi-selector" collapses into the switch list. The old §2.3.11 "position
grid, promoted to `RowTall` with the steppers to its left" is replaced outright.

### 2.5 Where label, value and help go

| Thing | Where, always |
|---|---|
| Label | Label zone, left, Sans 400 14 `TextLabel` (`TextPrimary` 500 when selected) |
| Sub-label (composite part) | Label zone, indented 12, Sans 400 13 `TextMeta` |
| Value | On the rail. Mono 500 16 `AccentValue` for sliders; Mono 500 14 for composite heads |
| Unit | Immediately after the value, Mono 400 11 `TextMeta`, never inside the number's run |
| One-line hint | Under the label, Mono 400 11.5 `TextMeta`, ≤ 64 chars (`ui_lint` enforces) |
| Full help | **Inspector only**, Sans 400 14 `TextLabel`, ≤ 3 sentences |
| Default | Inspector `default 5`, plus the rail tick and the reset dot |
| Provenance | Inspector `writes global.json` + the routing reason |
| Related | Inspector, jump links |
| Disabled reason | Full-strength line under the row, `Warn` |
| Error | Under the control, right-aligned, `Danger@80%` |
| Read-only fact | **Category Card only** (§3) |

Numbers are always Geist Mono; prose is always Geist Sans; the helper picks the font per
zone, so a caller cannot put a number in Sans.

---

## 3. Density — what moved to the Inspector, and how it was decided

The user's critique named the fix: *"This type of information shouldn't leave the UI, but
rather wander into the Inspector Rail. This is what it is designed for."* The problem is
that "move some stuff" is a taste judgement that drifts. So it is a decision rule, and
then a structural enforcement.

### 3.1 The Placement Test

For each piece of information, in order:

1. **Can it be changed?** No → it is a **Readout** → Category Card. *Unless* it is the
   live level of the very control it sits under (a fader's L/R peak meter), in which case
   it is a *part* of that control, not a row.
2. **Would a player change it during a session?** No → `Depth::Expert` → Inspector,
   reachable from the sheet by the group's chevron.
3. **Does it explain rather than set?** (help, provenance, defaults, ranges, match
   reasons, node metadata, "why is this greyed out") → Inspector.

Everything else stays in the Sheet. The test is answered from the *declaration* — a
`Readout` entry has no bind, an `Expert` entry says so, help is a field — so the helper
applies it, not the author.

### 3.2 The structural enforcement

> **`Sheet` has no `Readout()` factory.** `Readout()` exists on `Card` (the Category
> Card) and on `Panel` (the Inspector) and nowhere else.

That is the whole mechanism. A read-only row cannot appear in a sheet because there is no
call that produces one. E's Inspector Contract survives untouched — a readout was never
*access*, so moving it costs a user nothing, and the Category Card is on screen by
default whenever no row is selected.

Second rule, for the same reason: **a Sheet may not contain a `Note()` longer than two
lines**; anything longer is `Help()` and therefore Inspector.

### 3.3 The measured result

| Category | E sheet | E1 sheet | Moved to the Card / Inspector |
|---|---|---|---|
| Display / Upscaling | 5 controls + 4 readouts + 1 note | **5 controls** | effective output, game surface, composite path, preemptive-upscale flag, the Pixel note (→ `Filter.Help()`) |
| Display / Output | — | **4 controls** | connector, display name, modes offered, bit depth; colour range → Expert |
| Display / Frame Limiter | 2 controls + 1 readout | **3 controls** | VRR range, current pacing; pacing headroom → Expert |
| Display / HDR | 3 controls | **3 controls** | display peak, focused-app metadata, inverse-tonemap state; tonemap operator + force-PQ → Expert |
| Image / Shaders | 3-item list + 3 checkbox rows + **5 readouts + 1 meter row + 2 preset rows** in a second column | **3-item list + one switch-list composite** | effect file, compile time, hooks, colour space, GPU time |
| Audio / Mixer | 4 faders + picker | **4 fader composites + picker** | PipeWire backend, default sink, match reason (node table was already there) |
| System / Monitor | 4-item list + **5 statistics readouts + a progress row** + 6 appearance rows + a cramped anchor | **4-item list + 4 appearance rows + the Anchor composite** | average / 1% / 0.1% / outliers / sampling window / module width; blend mode, text opacity, module spacing → Expert |
| Setup / Per-Game | — | **2 switches + 2 actions** | resolved app id, config file, key counts |
| Setup / Appearance | — | **accent composite + 4 sliders + notification composite + 1 stepper** | font atlas, draw-primitive count |

Thirty-two read-only rows and eight expert settings left the sheets. The largest sheet
in the product is now the System Monitor at 4 list rows + 4 control rows + one composite.
Nothing became unreachable, because nothing that moved was reachable-and-changeable.

**The mockup proves both halves of this.** The `show what moved` button re-renders every
moved item back into the sheet, dashed and tagged, so the before/after is one click apart.

### 3.4 The counter-risk, stated honestly

E's own FEASIBILITY §6.4 said *"the Inspector will attract junk"*, and that is a
*process* defence, not a structural one. E1 makes it slightly more structural — `Readout`
being Card-only means the Card, not the Inspector, is where read-only content lands, and
the Card is regenerated from the registry rather than hand-authored — but the risk is
real and `ui_lint` (§API 7) is still the main guard.

---

## 4. Styling

### 4.1 Colour roles

| Role | Value | Used for |
|---|---|---|
| `Surface` | `rgba(9,10,12,.88)` | the slab base |
| `SurfaceRail` | black@22% over Surface | the rail (recessed; navigation never shows game through it) |
| `SurfaceSheet` | Surface | the sheet |
| `SurfaceInspector` | white@3% over Surface | the inspector (raised) |
| `SurfaceRaised` | white@5% | control fills, inactive segments |
| `Line` | white@6% | row hairlines |
| `LineGroup` | white@10% | group rules, header/footer rules |
| `LineRegion` | white@16% | region boundaries |
| **`LineControl`** | **white@34%** | **every control's own boundary — new, contrast-derived (§5)** |
| `TextPrimary` | `#EFF5FB @92%` | selected labels, group names, hero numbers |
| `TextLabel` | `#EFF5FB @62%` | parameter labels, body prose |
| **`TextMeta`** | **`#EFF5FB @48%`** | units, hints, marks, sub-labels, line numbers, chips — **was 44%** |
| `Ornament` | `#EFF5FB @30%` | **non-informational glyphs only** — breadcrumb `/`, dot leaders |
| `Accent*` | OKLCH family, hue-live | state: active, changed, selected |
| `Ok` | `#6ED274` | liveness dots |
| `Warn` | `#F3821D` | outliers, warn lines, disabled reasons |
| `Danger` | `#EF6B5A` `oklch(.66 .17 27)`, hue-fixed | destructive actions, errors, invalid input |

Two roles from E are **deleted**: `LineStrong` (folded into `LineRegion` / `LineControl`,
which had different jobs and one value) and `TextFaint` (measured 2.52:1 — see §5;
everything that used it is now `TextMeta`, except decoration, which is `Ornament`).

**Accent budget, unchanged and load-bearing:** accent is spent on *state only* — active
rail item, selected row, changed value, active segment, on-switch, slider fill, focus
ring. Never decoration, never a header, never a border that is not communicating state.
A screen where nothing has been changed is nearly monochrome, and that is correct.

### 4.2 Elevation, type, motion

Three elevation levels as ±3% lightness of the same base plus a 1px `LineRegion`. **No
shadows inside the slab.** Popups (dropdown, palette) share one recipe:
`SurfaceInspector` + 1px `LineControl` + one 8px black@25% expansion.

Type — Geist, seven roles: `Title` Mono 600 11 UPPER · `Section` Mono 500 10.5 UPPER ·
`Label` Sans 400 14 · `LabelStrong` Sans 500 14 · `Value` Mono 500 16 · `Meta` Mono 400
11.5 · `Hero` Mono 600 30. Weight carries hierarchy and nothing else.

Motion — three durations, one easing `1 − (1 − t)³`, lerped against `io.DeltaTime`:
**90 ms** state (hover, knob travel, segment fill, focus ring) · **160 ms** region
(inspector open/close, rail collapse, sheet cross-fade) · **240 ms** surface (overlay
open/close). Two prohibitions: **values never animate** (a slider snapping to a typed or
reset value; animated values over a game read as input lag) and **regions never move
except when opening or closing** (no content-driven resizing — that is issue #34's bug).

### 4.3 The responsive ladder

One pure function computes the layout; nothing else makes a width decision.

| Step | Trigger | Change |
|---|---|---|
| 0 | fits | Rail (labels) + Sheet + Inspector |
| 1 | `rail + 560 + inspector > available` | Rail collapses to icons (240 → 64) |
| 2 | still over | Inspector becomes an overlay drawer, `Esc` / `Ctrl+I` dismisses |
| 3 | sheet ≥ 1520 base | Sheet gains a **second** column (max two, and only here) |

Measured, on 1920×1080:

| Scale | Slab px | Slab base | Rail | Inspector | Sheet base | Step | Content column |
|---|---|---|---|---|---|---|---|
| 0.5× | 1180 × 470 | 2360 × 940 | 240 | 384 | 1736 | 3 | 2 × 720 |
| 0.75× | 1180 × 705 | 1573 × 940 | 240 | 384 | 949 | 0 | 720 |
| **1.0×** | **1560 × 928** | 1560 × 928 | 240 | 384 | 936 | 0 | 720 |
| 1.25× | 1728 × 928 | 1382 × 743 | 240 | 384 | 758 | 0 | 710 |
| 1.5× | 1728 × 928 | 1152 × 619 | 64 | 384 | 704 | 1 | 656 |
| 2.0× | 1728 × 928 | 864 × 464 | 64 | drawer 384 | 800 | 2 | 720 |

**E used two columns from 0.75× to 1.0× to absorb density. E1 does not need to**, because
§3 removed the density; a settings sheet is one column of ≤ 720 base units, and the space
to its right is deliberate air. The label-to-control distance therefore never grows past
720 − 12 − 320 − 36 = 352, which is the second half of the fix for critique (5): the
"orphaned" feeling comes as much from a 900-unit gap between a label and its control as
from the control's own arrangement.

The LOG (a `Raw` sheet) and the list sheets use the full sheet width; only row sheets are
capped.

---

## 5. Contrast — measured, not asserted

This project has had **three separate "too dark" complaints**, and the shipped fix (issue
#62, `kMetaTextAlpha` 30% → 44%) was a judgement call rather than a measurement. Here it
is a measurement, and the values in §4.1 are derived from it.

### 5.1 The worst-case background

The overlay sits over an arbitrary game, so the correct test is the *brightest* thing that
can be behind it:

```
pure-white game frame          rgb(255,255,255)
  ↓ background darkening 0.80   (the user's chosen default; = veil rgba(4,6,9,.80))
                               rgb( 54, 54, 54)   approx
  ↓ slab Surface rgba(9,10,12,.88)
  = rgb( 14, 15, 17)  #0E0F11   relative luminance L = 0.00475
```

Every ratio below is against **#0E0F11**, computed with the WCAG 2.1 formula
`(L₁+0.05)/(L₂+0.05)`. A darker game behind only *raises* every number, so this is the
floor. (The mockup's `game behind → pure white (worst case)` button renders exactly this.)

### 5.2 Text — floor 4.5:1

| Role | Composited | Ratio | |
|---|---|---|---|
| `TextPrimary` 92% | `#DDE2E8` | **14.78 : 1** | ✅ |
| `TextLabel` 62% | `#9A9EA2` | **7.02 : 1** | ✅ |
| `TextMeta` **48%** on Surface | `#7A7D81` | **4.62 : 1** | ✅ |
| `TextMeta` **48%** on an inactive segment fill (white@4%) | | **4.54 : 1** | ✅ **worst measured** |
| `AccentValue` `#78DBF6` | | **12.16 : 1** | ✅ |
| Active segment `#A9EAFD` on accent@24% | | **9.36 : 1** | ✅ |
| Accent chip `#71D4EF` on accent@16% | | **8.71 : 1** | ✅ |
| `Ok` `#6ED274` | | **10.28 : 1** | ✅ |
| `Warn` `#F3821D` | | **7.30 : 1** | ✅ |
| `Danger` `#EF6B5A` | | **6.36 : 1** | ✅ |
| ~~`TextMeta` **44%** (E's value)~~ | | **4.03 : 1** | ❌ *this is why it moved to 48%* |
| ~~`TextFaint` 30% (E's role)~~ | | **2.52 : 1** | ❌ *role deleted* |

48% is the smallest alpha that clears 4.5:1 on the *worst* surface a meta string can land
on. It is not a fourth guess; it is the answer.

### 5.3 UI components and state indicators — floor 3:1

| Element | Against | Ratio | |
|---|---|---|---|
| `LineControl` white@34% — switch track, box, segment, grid cell, button | Surface | **3.08 : 1** | ✅ **worst measured** |
| Switch-on border accent@65% | Surface | **4.20 : 1** | ✅ |
| Slider handle `#BAE7F4` | slider rail white@20% | **7.93 : 1** | ✅ |
| Slider fill (accent → accent-hi) | slider rail | **4.75 – 5.53 : 1** | ✅ |
| Meter lit accent | meter unlit white@14% | **5.89 : 1** | ✅ |
| Default tick white@60% | slider rail | **3.93 : 1** | ✅ |
| State edge / rail active edge accent@80% | Surface | **5.82 : 1** | ✅ |
| ~~switch off border white@18% (E's value)~~ | Surface | **1.69 : 1** | ❌ *raised to 34%* |
| ~~slider fill starting at accent@65%~~ | slider rail | **2.31 : 1** | ❌ *now starts at full accent* |

**Deliberately exempt, and why.** Row hairlines (white@6% = 1.14:1), group rules and
region boundaries are *separators*: WCAG 1.4.11 covers what is needed to *identify a
component and its state*, and a table rule identifies neither — the control's own
`LineControl` boundary does. Making separators 3:1 would produce a loud grid and undo
§4.1's density rules. **Disabled controls** are exempt per 1.4.3 / 1.4.11 ("inactive user
interface components") — which is exactly why §2.3.13 pulls the mandatory reason string
*out* of the dimmed row and draws it full-strength: the *why* is never exempt.

### 5.4 Per-region and scale rules

- The rail gets **darkening + 0.06** over the sheet's: navigation must never have game
  showing through it, or the "where am I" edge stops reading.
- Every hairline is `ImMax( 1.0f, ImFloor( 1.0f × scale ) )` — 1px from 0.5× to 1.99×,
  2px at 2.0× — so a rule never vanishes at 0.5× nor becomes a bar at 2.0×.
- Slab focused 1.00 / unfocused 0.90; blur 1.0; darkening 0.80 — the last is the value
  §5.1 measures against, so lowering it in Appearance lowers every ratio. `ui_lint`
  warns below 0.65, where `TextMeta` drops under 4.5:1.

---

## 6. Registration drives everything

Adopted from `b-command/API.md` as a *feature*, not as the navigation model.

E registered a row for the palette **inside `BeginRow`**, every frame, as a side effect of
drawing — which meant the palette only ever knew about categories you had already
visited. That is a real bug in E's design, not a stylistic difference, and it is why the
registry is worth taking wholesale.

> **One `Entry`, declared once at startup, yields three surfaces:**
>
> | Surface | Fields it consumes |
> |---|---|
> | **the Sheet row** | `kind`, `title`, `bind`, `range`, `unit`, `hint`, `enabledWhen`, `depth`, `group`, `order` |
> | **the palette entry** | `id`, `title`, `keywords`, its `Area`'s section and title, and `bind.Get()` for the value column |
> | **the Inspector page** | `help`, `default`, `range`, `unit`, `writes`, the `enabledWhen` reason, `related`, and the same `kind` + `bind` re-rendered two-line |
>
> Nothing is typed twice, and forgetting one is not possible, because forgetting it means
> not registering the setting at all.

`Ctrl+K` therefore searches every registered entry from the first frame, including
composite parts, expert settings and list-item switches — 99 entries in the mockup's
registry — with no per-category bespoke index. Selecting a result switches category,
selects the row and shows its Inspector page, in one keystroke.

Registration is also what `gamescopectl ui get/set/search <id>` addresses, what
`ui_snapshot` dumps, and what the Category Card counts to produce `N of M settings differ
from default`. See `API.md`.

---

## 7. What this replaces

| Today | Becomes |
|---|---|
| Bottom dock, 7 buttons, hint line | The Rail |
| 6 floating windows, drag / collapse / close / tile / measure-and-grow | One fixed slab; ~900 LOC of `Chrome.cpp` deleted |
| 4 tab bars | Rail items and list+inspector; **no `BeginTabBar` in the API** |
| Per-panel ad-hoc row layout | One Row grammar, one **right** rail, two heights |
| `BeginGroupBlock` nested boxes | Group bands + hairline table |
| Tooltips as the only help channel | The Inspector as a permanent help channel |
| Stock `ImGui::SliderFloat` still live in `FpsDisplay.cpp` (7 sliders) | One `Slider()` |
| `widgets::Toggle` / `Checkbox` used interchangeably | **Switch only; `Checkbox` deleted** |
| Read-only diagnostics scattered through six panels | The Category Card |
| Ad-hoc multi-part controls | The composite row group |
| Per-panel opacity, width, padding constants | The shell owns all geometry |

Kept verbatim because they were measured and are liked: the **slider**
(`slider-widget-spec.md` in full), the **toggle** geometry, the **segmented control**, the
**3×3 position grid**, and the **OKLCH accent family** with its live hue. Three widgets
gain a contrast-derived border alpha (§5.3); nothing else about them changes.
