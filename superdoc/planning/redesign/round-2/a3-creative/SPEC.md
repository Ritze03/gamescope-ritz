# A3 — "BLOOM"

**Direction A, reinvented.** One reel per section, no tabs and no pages. Every row at
rest is `label ………… value`. The row you are pointing at **blooms** into a full-size
instrument, drawn *over* its neighbours and never reflowing them. While you hold a
control, the console fades and the compositor blur lifts so you watch the **game**.

Date: 2026-08-23. Companion files: `API.md`, `FEASIBILITY.md`, `index.html` (interactive).

---

## 0. What survives from A, and why

The two things the user named are the constraint, not the starting point:

> *"The simplicity is amazing, since it doesnt look intimidating."*
> *"Looks clean and user friendly."*

A's simplicity came from **one pane, one row grammar, one place you can be**. All three
survive. What A spent that simplicity on was a *desktop settings dialog* — tabs, a fixed
narrow control column, tooltips for depth. BLOOM spends it differently:

| A kept | A dropped |
|---|---|
| one non-movable pane, no window manager | the sub-tab bar (§1.2) |
| one row grammar, kit owns all geometry | the fixed 224px control column (§2) |
| accent budget, OKLCH hue as the only theming axis | the three-level depth stack, drill screens, sheets (§1.3) |
| dark-only, composited over a game | the toggle switch as a distinct control (§2.4) |
| no pixel in any public API | alpha-based text colours (§4.2) |

The *net* simplicity goes up, not down: at rest a BLOOM screen has **no control chrome on
it at all** — no boxes, no chevrons, no switch tracks, no segmented cells. Just labels and
values. Everything that makes a settings UI look busy has moved into the one row you are
actually touching.

---

## 1. Navigation

### 1.1 Anatomy

```
┌───────────────────────────────────────────────────────────────────────────────┐
│ ● gamescope   DISPLAY          142 fps  7.0 ms  2560×1440  HDR  [writes app…] │ 52
├──────────────────────┬────────────────────────────────────────────────────────┤
│ ▣ Display            │  UPSCALING ─────────────────────────────────────── 3   │
│    │ UPSCALING       │  Filter                                          fsr   │
│    ▌ OUTPUT          │  Scaler                                         auto   │
│    │ FRAME LIMITER   │ ┌────────────────────────────────────────────────────┐ │
│    │ HDR             │ ▐ Sharpness                                    8      │ │
│ ◈ Shaders            │ ▐ FSR / NIS sharpening strength. Higher is crisper… │ │ ← BLOOM
│ ◍ Monitor            │ ▐ [ off | ████ 8 ]  ▁▂▃ preview ▃▂▁                 │ │
│ ♪ Audio              │ └────────────────────────────────────────────────────┘ │
│ ⚙ Config             │  Output mode                        2560×1440 @165 Hz  │
│ ▤ Log                │                                                        │
│ v3.16.14             │  OUTPUT ────────────────────────────────────────── 4   │
├──────────────────────┴────────────────────────────────────────────────────────┤
│ ↑↓ move   ←→ adjust   ⏎ open   R reset   ^K find anything   esc close          │ 44
└───────────────────────────────────────────────────────────────────────────────┘
```

Fixed size `min(284u, 96vw) × min(182u, 92vh)`, centred, never moved, never resized,
never more than one. `u = 4px × display_scale`.

### 1.2 There are no tabs

The four tabs the user disliked — *Upscaling · Display · Frame Limiter · HDR* — are gone as
a widget. They still exist as **structure**: they became **group headings inside one
continuous reel**, and they became **the rail's table of contents**.

- Selecting a section swaps the reel. Six sections; that is the only page switch in the
  product.
- Inside a section you **scroll**. Groups are separated by a heading and a rule.
- The rail shows the current section's groups as an indented list with a **scroll-spy
  tick** that slides as you scroll. Clicking one smooth-scrolls the reel to it.

> **Why:** a tab bar makes you *decide* before you can *look*. A reel lets you look first.
> It also removes a whole widget whose visual design was the complaint, and turns the same
> information into something the rail was already shaped to carry. The measurable win:
> 21 "screens" collapse to 6 reels and 0 tab bars.

### 1.3 There is no depth stack

A had L0/L1/L2, breadcrumbs, depth pips, drill screens and sheets. BLOOM has **one level**.
Everything a drill screen or a sheet used to hold now lives in the bloom:

| A mechanism | Where it went |
|---|---|
| sub-tab bar | group heading in the reel + rail TOC |
| drill screen | nothing — there was never content that needed one; the two real cases (profile list, stream list) are long `Choice` entries, which bloom into a scrolling option list |
| option sheet | the bloom's segmented bank or option list |
| colour sheet | the bloom's hue ribbon |
| multi-select sheet | the bloom's chip bank |
| confirm modal | hold-to-confirm inside the bloom's action button |
| Find sheet | the command palette (§6), the only overlay in the design |
| tooltip | the bloom's help paragraph — always visible on the focused row |

**Consequences:** no breadcrumb, no depth pips, no back semantics, no push/pop animation,
no focus trapping, no Z-order. `Esc` closes the overlay; there is nothing else it could do.

### 1.4 Input — mouse and keyboard only

Gamepad support is out of scope (dropped 2026-08-23; gamescope has no gamepad input path).
Every affordance here is designed for a pointer and a keyboard, not derived from a pad.

| Input | Action |
|---|---|
| hover a row | it blooms (no click, no delay beyond one animation frame) |
| click inside the bloom | operate the control directly |
| drag a slider | adjust, and **peek** (§5) for the duration of the drag |
| `↑` `↓` | move the bloom one row; the reel scrolls to keep it whole |
| `←` `→` | adjust the bloomed row in place — slider step, choice cycle, anchor move |
| `Shift` + `←→` / `Shift`-drag | fine step (1/10) |
| `⏎` | commit a text field, run an action, cycle a choice |
| `R` | reset the bloomed row to its default |
| `Tab` / `Shift+Tab` | previous / next section |
| `Ctrl+K` or `/` | command palette |
| `Esc` | dismiss the palette, else close the overlay |

**Hover and keyboard focus are the same state.** A had to keep them visually distinct
because a controller and a mouse could both be live. With no controller, one state is
correct, and one state is simpler: there is exactly one bloomed row, always, and it is
either where the mouse is or where the keyboard left it, whichever moved last.

### 1.5 The rail never jitters

The user: *"I like, that the sidebar contracts, when i open the log, but the symbols
shouldnt shift around."*

The rail is two columns: a **fixed 14u icon gutter** and a **42u label column**.
Collapsing removes width from the *label column only*. The icon gutter is never animated,
never re-measured, and never centred against a changing width — so no glyph moves by a
single pixel, in either direction, at any `display_scale`.

```
expanded              collapsing            collapsed
[ ▣ ][ Display    ]   [ ▣ ][ Displ ]        [ ▣ ]
  ^ x is constant       ^ same x              ^ same x
```

The label text cross-fades (alpha ramp) rather than clipping mid-glyph. The group TOC
collapses with the label column, since it is a label-column citizen.

Try it: `collapse rail` in the mockup's demo strip.

---

## 2. Controls, designed first

The user: *"Sliders, Multiselectors and stuff dont use as much space, as they deserve.
Also, they differ in size too much."*

Both halves are answered by the same move: **stop trying to fit every control into one
column, and stop showing controls that nobody is touching.**

### 2.1 Two states, and only two

| State | What it is |
|---|---|
| **Rest** | `[label] …………………… [value]` — 10u tall, no control chrome, identical for every kind. |
| **Bloom** | the full instrument, at one of four heights, drawn over the neighbours. |

Exactly one row is bloomed at any time. Never zero (the overlay opens with the first row
bloomed), never two.

### 2.2 Rest: the value carries the type

At rest, typography does the work a widget used to:

| Kind | Rest value |
|---|---|
| bool / choice | the option name — `fsr`, `on`, `auto` |
| slider / number | the number + a Meta-coloured unit — `8`, `400 nits`, `Unlimited` |
| multi | `4 of 6` |
| anchor | `top right` |
| action | the verb, uppercase — `DELETE` |
| readout | the value, **in Meta grey, with a status dot** |

**The accent rule that makes this legible:** *a coloured value is a value you can change.*
Read-only values are Meta grey and have no accent anywhere. This is a stricter version of
A's accent budget and it replaces an entire visual affordance (the "does not look
pressable" box) with a colour the eye reads before it reads the word.

### 2.3 Bloom: four heights, chosen by the kit

Height is a property of the **kind**, never of the call site. Four classes exist:

| Class | Height | Kinds |
|---|---|---|
| `compact` | 22u (88px) | plain readout |
| `standard` | 28u (112px) | bool, choice ≤5 short options, multi, hue, action, text without suggestions, readout with sparkline |
| `instrument` | 34u (136px) | **slider**, number with preview, text with suggestions |
| `canvas` | 42u (168px) | anchor 3×3, choice with long or many options |

"They differ in size too much" becomes: they differ in exactly four documented steps, and
a slider is always in the biggest class that isn't a canvas. A slider is now **3.4× the
vertical space it had in A**, and it is full stage width instead of 224px.

Every bloom has the same three bands, in the same order, always:

```
 ┌─ header ──────────────────────────────────────────────  value ─┐
 │  Sharpness                                                 8    │   ← name + big Mono value
 │  FSR / NIS sharpening strength. Higher is crisper; too         │   ← help, ALWAYS visible
 │  high rings around high-contrast edges.                        │
 ├─ instrument ───────────────────────────────────────────────────┤
 │  [ preview strip, if the kind has one ]                        │
 │  ├──────────────████████████●──────────────────────────┤       │
 │  0                    ╵default                        20       │
 ├─ meta ─────────────────────────────────────────────────────────┤
 │  default 2    range 0 – 20    writes app 1245620   display.sharpness │
 └────────────────────────────────────────────────────────────────┘
```

The meta band is the answer to *"for some stuff, it makes it more complex, since we'll have
to rely on tooltips and such."* **There are no tooltips.** Depth has a home: help text,
default value, valid range, config routing and the stable id are all on screen, on the row
you are touching, at all times. Nothing is hidden behind a hover delay.

### 2.4 The complete taxonomy — nine kinds

| # | Kind | Rest | Bloom instrument | `←→` |
|---|---|---|---|---|
| 1 | **Choice** | option name | equal-width **segmented bank**, full width, 11u cells | cycles |
| 2 | **Choice (long)** | option name | scrolling **option list**, 6 rows visible, `●` on current | cycles |
| 3 | **Slider** | number + unit | 4u track, gradient fill, 3×8u knob, **default notch**, min/max end labels, optional preview strip above | steps; `Shift` = 1/10 |
| 4 | **Number** | number, or `ZeroMeans` word | `[−] [ big value ] [+]` steppers + optional preview | steps |
| 5 | **Text** | the string, or `empty` | one field + a **suggestion chip row** | — |
| 6 | **Multi** | `4 of 6` | wrapping **chip bank**, 9u chips | — |
| 7 | **Anchor** | `top right` | 3×3 grid over a 16:9 ghost of the real HUD, which jumps | moves |
| 8 | **Action** | the verb | wide button; destructive ones **hold-to-confirm** (900 ms, the fill sweeps) | — |
| 9 | **Readout** | value in Meta + status dot | optional 48-sample sparkline with outliers in `state/warn` | — |

**There is no colour picker either.** Accent Color is a `Slider` over hue `0–360` whose
preview is `AccentSweep` — the OKLCH ribbon *is* the track, and dragging it recolours the
whole overlay live. One fewer kind, and the preview and the control are the same object.

**There is no toggle switch.** A bool is `Choice{off, on}` and renders as a two-cell
segmented bank.

> **Why:** a switch is a small target that only says *flip me*, and it was the single
> worst offender for "controls that don't use the space they deserve" — 44×24 in a 224px
> column. A two-cell bank is full width, is two real targets, lets you click the state you
> want rather than toggling blind, reads identically to every other choice, and deletes a
> whole control kind from the taxonomy, the theme and the API. The cost is that the
> familiar iOS-style switch is gone; the mockup makes the trade visible.

**Sub-rule that makes an inconsistent screen hard to build:**

- A value is never in a Sans run. Every value is Mono, tabular-lined. (Kept from A — the
  best rule in the original spec.)
- A disabled row stays visible, is dimmed to `text/off`, and its bloom replaces nothing —
  it **adds** a warn-edged reason line. `Disabled` without a reason is a debug assert.
  *"Tearing cannot be enabled while VRR / Adaptive Sync is on — the display is already
  refreshing on demand."*
- The reason line is `text/meta` (**7.96:1**), never dimmed. A disabled control may be
  quiet; the explanation for it may not.

### 2.5 A setting can show its own effect

Eight previews exist. The vocabulary is **closed** — a caller picks one from an enum, and
adding a ninth is a deliberate, visible change to the kit (see `API.md` §4).

| Preview | Draws | Used by |
|---|---|---|
| `Sharpen` | a detail patch, split off / on | Sharpness, Pre-Sharpen strength |
| `Saturation` | a hue ramp through the actual gain | Vibrancy max/min gain, SDR gamut wideness |
| `LuminanceRamp` | a black→white ramp through the actual curve | HDR gains, SDR-on-HDR brightness, adaptive target |
| `TextSample` | `142 FPS · 7.0 ms` at the literal chosen pixel size | HUD font size |
| `FrametimeRuler` | 40 recent frametimes with the target line; bars over the line turn `warn` | FPS Limit |
| `HudGhost` | the HUD plate with the real backdrop/padding/rounding/opacity | every Monitor appearance setting |
| `AudioMeters` | live L/R peak segments | Volume |
| `AccentSweep` | the OKLCH hue ribbon, draggable | Accent Color |

This is the part of the design that is *specific to a game overlay*. A desktop settings
dialog has no reason to show you what sharpening looks like; this one is standing on top
of a frame that already demonstrates it.

### 2.6 Sticky canvas

At most **one** per section, pinned to the top of the reel and never scrolling: Audio's
L/R meters, Log's toolbar. It is not a row and contains no rows. Everything else that A
put in a "Well" is now a bloom preview instead, which is strictly better — the preview
follows the setting you're editing rather than sitting in a fixed corner.

---

## 3. The bloom, precisely

### 3.1 It overlays. It never reflows.

The bloom expands **symmetrically** from the rest row's rect and draws over its
neighbours, with the panel colour behind it, a 1px `line/component` border, a 0.75u accent
left edge, and a drop shadow. The list geometry is byte-identical whether anything is
bloomed or not.

> **Why not an accordion that pushes rows down:** hover-driven accordions oscillate. You
> hover row 5, it opens, row 6 slides up under the cursor, row 6 is now hovered, it opens,
> row 5 slides back — the cursor never moved. Overlaying cannot produce that. It also
> means `ImGuiListClipper` sees a uniform row height forever, which is what makes a
> 200-entry reel free (`FEASIBILITY.md` §2.2).

### 3.2 It is above in draw order, not in hit order

Only the bloom's **actual controls** take the pointer. Its header, help paragraph and meta
band are transparent to hit-testing, so the row list underneath still decides which row is
hovered. Recentring the bloom therefore can never change what is hovered — the feedback
loop that causes oscillation does not exist.

### 3.3 Clamping

If a bloom would leave the reel's visible rect, its top is clamped to the visible rect
(minus 2u) rather than the reel scrolling. So the bloom is always whole, always on screen,
and the reel never scrolls just because you moved the mouse.

### 3.4 Motion — five floats, total

| Motion | Speed | State |
|---|---|---|
| bloom top / height slide between rows | 22 /s | 2 floats |
| rail label-column width (collapse) | 12 /s | 1 float |
| peek ramp (§5) | 9 /s | 1 float |
| rail TOC scroll-spy tick | 20 /s | 1 float |

One helper: `Approach(cur, target, speed, dt) = cur + (target-cur)*(1 - expf(-speed*dt))`.
No per-widget animation, no ID-keyed store, nothing retained. **Values never animate** — a
settings console that eases its numbers is lying about latency.

---

## 4. Theming, and the contrast problem

### 4.1 The contrast guard

The user: *"The contrast is kinda bad."* The structural reason is that the effective
background is **not a constant** — it is the game, times the compositor's darkening,
underneath the panel's alpha. Any palette tuned against one game is wrong against another.

So the panel's alpha is not a free choice. It is a **function of the darkening the
compositor is already applying**:

```cpp
// Theme.cpp — the only place panel alpha is decided.
float GuardAlpha( float flDarkening )
{
    return std::clamp( 0.90f + ( 0.80f - flDarkening ) * 0.09375f, 0.88f, 0.99f );
}
// The user's Transparency setting is remapped into [guard, 1.0]. It can make
// the panel MORE opaque. It cannot take it below the floor.
float PanelAlpha( float flDarkening, float flTransparency01 )
{
    const float g = GuardAlpha( flDarkening );
    return g + ( 1.0f - g ) * ( 1.0f - flTransparency01 );
}
```

Drop `background_darkening` to 0 and the panel silently goes from `α .90` to `α .975`. The
composite floor does not move. **This is measurable, and it is the difference between pass
and fail:** without the guard, over a white game with darkening at 0, `text/dim` measures
**4.23:1** — a WCAG AA failure. With it, **5.16:1**. The mockup's `contrast` button shows
this computing live; drag the darkening slider and watch the panel compensate.

### 4.2 No text token has alpha

Every text colour in A was `rgba(239,245,251, .46)`-style. Alpha text composites against
whatever is behind it, which is the game — so its rendered colour, and therefore its
contrast, is different in every scene. **BLOOM's text tokens are opaque hex.** The
numerator of the contrast ratio is then fixed by construction, and only the denominator
moves — across the narrow range the guard permits.

That single change is most of the fix. It costs one thing: text can no longer be dimmed by
lowering alpha, so a "dimmer" role must be a *new opaque colour*, which is why there are
five text tokens and not a slider.

### 4.3 Measured contrast table

`WORST` = white game (`rgb 255,255,255`), `background_darkening 0`, guard alpha `.975`
→ composite `rgb(18.1, 20.0, 23.9)`.
`BEST` = black game, `background_darkening 0.8`, alpha `.90` → composite `rgb(10.8, 12.6, 16.2)`.

| Token | Value | vs WORST | vs BEST | Required |
|---|---|---|---|---|
| `text/primary` | `#F4F8FC` | 17.27:1 | 18.27:1 | 4.5:1 |
| `text/label` | `#D2DCE6` | 13.27:1 | 14.04:1 | 4.5:1 |
| `text/meta` | `#9EACBA` | 7.96:1 | 8.42:1 | 4.5:1 |
| `text/dim` | `#7C8996` | **5.16:1** | 5.46:1 | 4.5:1 |
| `text/off` (disabled) | `#5F6B77` | 3.38:1 | 3.58:1 | 3.0:1 (exempt, met anyway) |
| `line/component` | `#606B79` | 3.40:1 | 3.60:1 | 3.0:1 |
| `track/empty` | `#5A6675` | 3.15:1 | 3.34:1 | 3.0:1 |
| `line/hair` | `#333B45` | 1.63:1 | 1.72:1 | n/a — decorative only |
| `accent/value` | `oklch(.86 .09 h)` | **11.33:1** worst hue 18° | 12.54:1 | 4.5:1 |
| `accent/edge` | `oklch(.80 .11 h)` | 9.36:1 worst hue 0° | 10.38:1 | 3.0:1 |
| `accent/base` | `oklch(.74 .12 h)` | 7.52:1 worst hue 350° | 8.46:1 | 3.0:1 |
| `accent/on` (text on an accent fill) | `oklch(.90 .07 h)` | 6.78:1 over the 34 % fill, worst hue | 7.0:1 | 4.5:1 |
| `state/ok` | `oklch(.78 .16 145)` | 9.77:1 | 10.34:1 | 4.5:1 |
| `state/warn` | `oklch(.74 .17 55)` | 7.61:1 | 8.05:1 | 4.5:1 |
| `state/danger` | `oklch(.66 .20 25)` | **5.39:1** | 5.70:1 | 4.5:1 |

**Worst case anywhere in the design: 3.15:1** (`track/empty`, a UI boundary, floor 3:1).
**Worst case for any text: 5.16:1** (`text/dim`, floor 4.5:1). Accent hue is swept across
all 360° and the worst hue is reported, so no hue choice can break it — this is what the
fixed per-role L and C buy, and it is the part of A's palette worth keeping unchanged.

### 4.4 The halo

The panel edge still has to survive against a white game. One blurred black rounded rect,
inset −7u behind the panel, blur 34, alpha .78. It costs a single `AddRectFilled` into a
pre-blurred layer (or, cheaper, three concentric rects at decreasing alpha — see
`FEASIBILITY.md` §3.1). Without it the 1px border disappears into a snow field; with it
the panel reads as an object at every game brightness in the mockup's switcher.

### 4.5 The accent budget

Accent means exactly two things:

1. **this value is editable** — every Mono value at rest, every big value in a bloom;
2. **this is the thing you are on** — the bloom's left edge, the rail's current section,
   the active segmented cell, the scroll-spy tick.

Nothing decorative is accent. Read-only is never accent. `state/danger` is **not**
hue-linked — if the user's accent lands within 25° of the danger hue, danger drops its
lightness (`.66 → .60`) and raises chroma (`.20 → .22`) so destructive stays
distinguishable from decorative. Four lines in `UpdateAccentFamily()`.

### 4.6 Typography — five roles

`base = 15px × display_scale`. Geist Sans for prose, Geist Mono for every number, id,
uppercase label and log line.

| Role | Family / weight | Size | Used for |
|---|---|---|---|
| `Value` | Mono 600 | 1.85 × base | the bloom's big value only |
| `Body` | Sans 400/500 | 0.96–1.06 × base | row labels, bloom titles |
| `Mono` | Mono 500 | 0.84–0.90 × base | every rest value, segmented cells, option lists, log |
| `Meta` | Sans 400 | 0.82 × base | help paragraphs, reasons |
| `Micro` | Mono 400/500 | 0.68–0.74 × base | group headings, rail TOC, meta band, end labels, timestamps |

Five roles, ~7 baked faces. A had six roles / eight bakes; today's `Fonts.h` has ten.

### 4.7 Scaling 0.5× – 2.0×

Everything derives from `u` and `base`. Clamps:

- Panel `min(284u, 96 % of surface) × min(182u, 92 % of surface)`.
- Rail: below 200 logical px of label column it collapses permanently to the icon gutter.
  The gutter is `14u` and never falls below 44 physical px at `display_scale ≥ 0.8`.
- A bloom taller than the reel's visible height falls back to its next-smaller height
  class, then to `standard`. At 2.0× on a 720p surface a `canvas` bloom degrades to a
  scrolling option list — one branch, deterministic.
- The four bloom heights are in `u`, so the slider instrument stays proportionally the
  biggest control at every scale.

Verify with the mockup's `scale` slider, which drives `--u` and `--fs` and nothing else.

---

## 5. Peek — what the game behind the panel is *for*

Hold any control — drag a slider, hold the hue ribbon, hold a stepper — and for the
duration of the hold:

- the compositor's blur ramps from `background_blur` toward ~0 and its darkening toward ~0;
- the panel's own alpha ramps to ~.10;
- the header, rail, legend and every un-bloomed row ramp to 12 % opacity;
- **the bloom stays**, at ~.90 alpha, with a heavier shadow.

Release and it all comes back over ~110 ms.

> **Why:** the single most common failure of a game settings overlay is that it hides the
> thing you are adjusting. Sharpness, vibrancy, HDR gain, HUD placement, brightness — every
> one of them is a judgement about the *frame*, and every existing design makes you commit
> blind, close the menu, look, and re-open. Peek costs one animated float and turns the
> blurred backdrop from decoration into the actual subject.

Peek is **not** a preference and **not** a mode. It is bound to "a control is being held",
which is exactly the interval during which the menu is not what you are looking at.

Try it in the mockup: drag the Sharpness track.

---

## 6. The command palette (B, adopted as a feature)

`Ctrl+K` or `/`. The registry (`API.md`) already holds every entry's id, title, group,
section, help and keywords, so the palette is a filter over a list, not a subsystem.

- Fuzzy subsequence match over `title + group + section + keywords + id`, exact-substring
  boosted. ~60 entries, well under 50 µs, recomputed only when the query changes.
- Each result shows `Section › Group`, the title, and **the live value**.
- `↑↓` picks. `⏎` jumps: the reel switches section, scrolls, and blooms that exact row.
- `←→` **adjusts the highlighted result in place, without leaving the palette.** Two
  keystrokes from anywhere to any setting, changed, without losing where you were.

The palette is the **only** overlay in the whole design — the only thing that ever draws a
scrim. That is the budget: one bloom mechanism, one overlay mechanism, zero windows.

Browsing stays primary. The palette is a shortcut for people who know the name, not the
navigation model.

---

## 7. Section inventory

| Section | Groups (were tabs) | Body | Sticky canvas |
|---|---|---|---|
| **Display** | Upscaling · Output · Frame Limiter · HDR | reel, 15 entries | — |
| **Shaders** | Vibrancy · Pre-Sharpen · Adaptive Brightness | reel, 11 entries | — |
| **Monitor** | General · Appearance · Modules · Statistics | reel, 15 entries | — |
| **Audio** | Output · Streams | reel, 6 entries | L/R peak meters |
| **Config** | Per-Game · Appearance · Danger Zone | reel, 11 entries | — |
| **Log** | — | stream | source / level / follow / copy toolbar |

Six sections, six reels, zero tab bars, zero sub-screens, zero sheets, one overlay.

Placements worth noting:

- The **3×3 anchor** is a bloom canvas with the HUD ghost in it, not a fixed well — so it
  is right where you are editing placement, and the margin sliders next to it drive the
  same ghost through `HudGhost`.
- **Statistics** are `Readout` entries with sparklines. A sparkline is a readout variant,
  not a tenth control kind.
- **Log** is the only section that is not a reel. It declares `Section::Stream`, the one
  fenced escape hatch in the API.
