# ImGui Overlay — Visual Design Guide

Distilled from the design handoff `Game Overlay UI Mockups-handoff.zip` (Claude Design canvas export,
extracted to scratchpad, not committed to this repo). This file captures **visual language only** —
colors, type, spacing, component chrome, iconography rules, motion, and ImGui feasibility. It does
**not** transcribe the mockup's feature set; our actual feature list lives in `superdoc/features/`.
Where the mockup shows a control for something not on our roadmap, only its *styling* is captured here.

The mockup's own build-spec sheet (option `2b` in the handoff) is unusually well-organized — most
values below are taken directly from it and cross-checked against the rendered mockups (`1a`–`1f`, `2a`).

## Inventory of the handoff (what was in the zip, one line each)

- `1a` — Hero: three floating windows (Shaders, Gamescope-style panel, LSFG-VK-style panel) + FPS HUD + centered dock, 1920×1080.
- `1b` — Alternate shader panel: dense instrument-row layout.
- `1c` — Alternate shader panel: staged groups with an A/B compare strip, all sliders exposed.
- `1d` — FPS HUD: config window plus five backdrop/size treatment samples (none/shadow/solid/blur/additive).
- `1e` — Bottom dock: three treatments (A "glass dock", B "keycaps" bevelled, C "bare" no container).
- `1f` — Theme sampler: same panel rendered in three accent hues (cold cyan / ember / signal-green).
- `2a` — Final composite mockup: four windows + FPS HUD + dock A + cyan theme, laid out clear of each other.
- `2b` — Build spec sheet: tokens, control metrics, window inventory, layout/behaviour notes (primary source below).

Left deliberately unused by us: the mockup's specific panels are named for *its* imagined feature set
(ReShade-style "Adaptive brightness/Vibrancy/Sharpness", gamescope scaling filter switcher, LSFG-VK
frame-gen panel, PipeWire-style audio mixer with L/R meters, a settings/profiles panel it says is
"not yet designed"). We take the chrome and control styling from these, not the panels or their contents
— our own feature set decides what windows/controls actually exist.

## Color palette

Single dark theme, accent-hue-swappable (mockup demonstrates cyan/ember/green — cyan is the default;
no light theme exists or was designed).

| Token | Value | Role |
|---|---|---|
| `surface` | `#090a0c` @ 88% alpha (`rgba(9,10,12,.88)`) | Window/panel background, glass base |
| `raised` | `#fff @ 5%` (`rgba(255,255,255,.05)`) | Header bar tint, grouped-block background |
| `raised-subtle` | `#fff @ 2–3%` | Nested group backgrounds, footer strips |
| `hairline` | `#fff @ 10%` (`rgba(255,255,255,.10)`) | Default borders/separators |
| `hairline-strong` | `#fff @ 14–18%` | Interactive-element borders (checkbox, dock idle) |
| `accent` | `oklch(.74 .12 218)` (cyan, ≈ `#4fb8d6`-ish) | Primary accent: focus ring, active state, slider fill/handle glow, status dot |
| `accent-hi` | `oklch(.86–.9 .07–.08 218)` (near-white cyan) | Slider/toggle handles/knobs — the brightest accent tone |
| `accent @ 12–24%` | translucent accent | Active-state fills (segmented control, toggle track, group left-edge highlight) |
| `ok` (signal) | `oklch(.78 .16 145)` (green) | Status dots only — "connected/hooked" indicators |
| `spike` | `oklch(.72 .17 55)` (amber/orange) | Frametime-spike bar in the FPS graph only |
| Text primary | white @ 92% | Panel titles, values, primary labels |
| Text secondary | white @ 60–72% | Parameter labels, body text |
| Text meta | white @ 26–42% | Units, hints, disabled/read-only values, sub-labels |
| Alt accents (theme variants, not used by default) | Ember `oklch(.74 .13 58)`, Signal-green `oklch(.78 .16 145)` | Full alt themes swap accent hue + tint the glass warm/cool; base structure identical |

Notes:
- All colors are defined in `oklch()` or alpha-blended `rgba(255,255,255,X%)` / `rgba(0,0,0,X%)` —
  there is no independent "light surface" scale; everything is white-on-black opacity layering.
  ImGui's `ImVec4` colors are plain RGBA — the oklch values must be converted to sRGB hex/float once
  and hardcoded (no runtime oklch needed since accent is user-configurable at most, not computed).
- No error/danger red is defined anywhere in the handoff. If our feature set needs a destructive/error
  state, that color must be chosen fresh — flag this as an open question.
- Theme = "accent hue swap only" per the spec sheet: swapping accent also nudges the glass tint warm/cool
  (ember panel background is `rgba(13,10,9,.88)` vs cyan's `rgba(9,10,12,.88)`) and border tint shifts to
  match. If we ever support multiple accent colors, replicate this pairing, not just the accent swap alone.

## Typography

- **Families:** IBM Plex Sans (400/500/600) for prose/labels, IBM Plex Mono (400/500/600) for every
  number, unit, path, and state word. Loaded via Google Fonts in the mockup (`fonts.googleapis.com`).
  **Licensing:** IBM Plex is open-source (SIL OFL 1.1) — free to bundle. ImGui needs a real font atlas
  (TTF/OTF), so both Plex Sans and Plex Mono static weights (400/500/600) must be bundled as font files
  and baked into the atlas at build/init time; no web-font loading applies here.
- **Hard rule from the spec:** never mix a number into a sans run — numerals are always Mono and always
  tabular (`font-variant-numeric: tabular-nums`). ImGui has no tabular-nums toggle; achieve this by using
  a genuinely monospaced font for all numeric/value text (Plex Mono already is monospaced, so this is
  free as long as numbers are rendered with the Mono font, not Sans).
- **Scale observed:**
  - Window title: Mono 600, 10.5–11px, letter-spacing ~.15–.16em, uppercase.
  - Group/section name: Sans 500, 12–13px (also sometimes Mono 500 uppercase w/ letter-spacing for
    sub-group headers, e.g. "ADAPTIVE BRIGHTNESS").
  - Parameter label: Sans 400, 11.5–12.5px.
  - Value readout: Mono 500, 12–13px, tabular, usually accent-colored.
  - Meta/status line: Mono 400, 9.5–11px, low opacity (26–42%).
- Line-heights are tight throughout: 1 for single-line labels/values, 1.2–1.65 for wrapped meta text.

## Spacing & layout

- Window corner radius: **3–4px**. Control corner radius: **0px** (flat) except sliders (3px track) and
  a few chip/badge elements (1–2px). This is a hard rule: windows are barely rounded, controls are square.
- Border width: **1px hairline** everywhere (`rgba(255,255,255,.06–.14)`, accent-tinted at 30–65% when a
  control is "on").
- Window padding: **14px**. Between control groups: **12–13px**. Label→control gap: **5px** (compact
  rows) or laid out on a grid (label / slider-track / value columns, e.g. `1fr 150px 52px`). Group
  internal padding: **12px**.
- Grouped block = 1px hairline box on ~2% white fill; the *active/focused* group gets a **2px accent
  left-edge stripe** (`border-left`) instead of a background change — this is the primary "this group is
  live/relevant" affordance.
- Title bar height: **30–34px** (34px in the final composite, 30px in the earlier variant — treat 32px as
  a safe default), horizontal padding 10–12px, gap 9–10px between status dot / title / meta / controls.
- Elevation / shadow: `0 28px 70px -14px rgba(0,0,0,.8)` on normal windows; focused window adds an accent
  glow (`0 0 40px -20px accent/.6`) plus an accent border at ~42% opacity. Unfocused windows sit at 94%
  opacity, no glow.
- Backdrop: `blur(20–22px) saturate(1.1–1.15)` behind every floating window and the dock — true glass.
- Dock: centered horizontally, **38px** from the bottom edge, 6px container padding, 4px radius, **5px**
  gaps between 54×54px square buttons, a 1px divider before the trailing close button.

## Component styling

**Window/panel chrome**
- Background `rgba(9,10,12,.88)` + backdrop blur/saturate (see above). 1px hairline border, 3–4px radius.
- Header: 30–34px tall, subtle top-to-bottom gradient (`rgba(255,255,255,.06)→.015`), bottom hairline
  border. Contains: 6×6px square status dot (accent = active/live, dim white = idle) with an accent glow
  shadow when lit → uppercase Mono title (letter-spacing .15–.16em) → dim Mono meta text (subsystem name)
  → flex spacer → 18×18px icon-button cluster (collapse "–", close "×", both drawn as 1–1.5px line glyphs,
  ~45% white opacity, no background/border — bare glyph buttons).
- Focused window: accent border at ~42% opacity, extra accent-tinted header gradient, drop shadow gains
  an accent-colored outer glow. This is the *only* focus indicator — no focus ring around individual
  controls is shown in these mockups beyond the widget's own active-state styling.

**Tabs / segmented controls** (used for mode pickers like filter type, curve type, A/B/C options)
- Row of equal-flex segments, ~3px gap, each segment padded ~6–7px vertical. Inactive: `rgba(255,255,255,.04–.045)`
  fill, 1px `rgba(255,255,255,.07–.08)` border, Mono 500 text at ~45–50% white. Active segment: accent
  fill at **20–24% alpha**, accent border at ~60% alpha (or `inset 0 0 0 1px` in one variant), Mono
  **600** weight text in bright accent color (`oklch(.9 .07 218)`).

**Buttons** — no filled rectangular "button" component appears standalone in this handoff; the closest
analogs are: dock buttons (see below), bare icon-glyph buttons (collapse/close, ~18×18px hit area, no
chrome, just a line-drawn glyph that dims/brightens), and segmented-control cells acting as button groups.
If our overlay needs a conventional push-button, extrapolate from the segmented-control cell styling
(flat rect, hairline border, accent fill + bright text when active/pressed) — **this is an extrapolation,
not something shown directly**, flag it as such to implementers.

**Sliders**
- Track: full-width flex, **5px** height (2b) / 3px height (1a — treat 5px as canonical per the build
  spec), 3px radius, `rgba(255,255,255,.09–.1)` background. Filled portion: linear gradient
  `accent/.5 → accent` (full opacity at the handle end), same radius.
- Handle: **8×18px** rectangle (not a circle), 1px radius, bright accent-hi fill, centered on the track,
  with an accent glow shadow (`0 0 12px accent/.8`). Drag or scroll-wheel to adjust per the spec.
- Value readout sits above-right or grid-aligned to the right of the label, Mono 500 13px, tabular,
  accent-colored. Min/max hints below the track at 9.5px, 26% white.
- Disabled/inactive slider (e.g. NIS sharpness when FSR is selected): whole control drops to **34%
  opacity** and the fill/handle desaturate to plain white instead of accent — this is the standard
  "control present but currently inert" treatment, reusable anywhere.
- **The handle width rule (2026-09-06 fix, requests item 12/D4): constant, whatever the range or
  step.** ImGui's own `SliderBehavior` widens a `SliderInt`'s grab past the token width on purpose,
  to "represent one unit" of a coarse range — the Crosshair panel's Outline Width, Dot > Size and
  Line > Width sliders (small integer ranges) all shipped with an oversized, lopsided handle as a
  result, while every float `Slider` looked correct by coincidence. `Controls.cpp`'s `SliderGrab()`
  now re-centres the returned grab rect to the token width (`kHandleW`) after `SliderBehavior` has
  already resolved the frame's click/drag/keyboard step, so only the *painted* width changes — see
  `ui::controls::ConstantWidthGrab()` (pure arithmetic, pinned in `test_overlay_ui.cpp` without an
  ImGui context) and `Controls.h`'s own comment on it for the full mechanism.

**Toggles (switches)**
- **30×15px** track (26×13 or 24×12 in denser variants — 30×15 is the canonical size per 2b), no radius
  (square), 1px border. Off: track `rgba(255,255,255,.09)`-ish / here specifically shown always in an
  "available" accent-tinted state — track `accent/.3` fill, `accent/.65` border even when off, because in
  this design toggles use accent for the *track* regardless of state and the **knob position** (not track
  color) carries on/off. Knob: **11×11px** square, accent-hi fill, slides to the right edge when on
  (1px inset padding). *(If a true "off" visual distinct from "on" is required, darken the track to the
  hairline-gray family when off — the handoff's toggles are all shown mid-"on" state; treat the off-state
  track color as an inferred gap, not a captured value.)*

**Checkboxes**
- **12×12px** square box, 1px accent border at 70% alpha, `accent/.2` fill. Checked mark: centered
  **5×5px** solid accent-hi square (not a checkmark glyph — a filled square). Unchecked: plain
  `rgba(255,255,255,.18)` border, `rgba(255,255,255,.04)` fill, no inner mark.

**Dropdowns / selects** — not directly present as a native `<select>`-style control; the design uses
segmented controls for closed small option sets (≤5 options) instead. No true scrolling dropdown/combo
box appears anywhere in the handoff. **Gap**: implementers must design this control fresh, matching the
flat/hairline/accent-active language (e.g. a segmented-style flat row with a caret glyph, popup list
using the same panel chrome). Flag as not covered by the mockup.

**Text inputs** — not present in the handoff at all (every value is slider or segmented-control driven;
"live apply, no apply button, no free text entry" per the build spec's Live-apply note). Any text field
we need (e.g. profile name) is an unstyled gap — reuse the flat-hairline-box treatment used for numeric
readouts (`rgba(255,255,255,.05)` fill, 1px `rgba(255,255,255,.08)` border, Mono value text) as the
closest analog.

**Scrollbars** — not present; no scrolling content appears in any mockup (windows are fixed-height,
content-sized). Undefined by the handoff — will need ImGui default styling reskinned to match the
hairline/flat language if any window ends up needing to scroll.

**Tooltips** — one instance, on dock-button hover (`1e`, treatment C only): solid panel
`rgba(6,8,10,.94)`, 1px `rgba(255,255,255,.12)` border, no blur, Mono 500 10px letter-spaced label,
positioned above the anchor element, no arrow/caret drawn.

**Separators** — 1px horizontal rule, `rgba(255,255,255,.06–.07)`, full width within a group, no margin
tricks beyond the surrounding group padding.

**List rows** — no scrolling list exists in the handoff; the closest repeating-row patterns are the
checkbox row list (FPS HUD's row toggles: 11×11 checkbox + 12px Sans label, 7px vertical gap) and the
label/value definition-list pattern used in the build-spec sheet itself (grid: 104px label column +
flexible value column, 11–14px row gap). Use the checkbox-row pattern for any settings list we build.

**Status readouts / meters** (not a classic "component" but reused constantly — worth capturing)
- Numeric hero value: Mono 600, 18px, tabular, white.
- Bar-graph mini charts (frametime history, L/R audio peak meters): thin vertical bar array, 1–1.5px
  gaps, accent color at 60–85% alpha for normal bars, `spike` amber for outlier bars, dim
  `rgba(255,255,255,.09)` for below-threshold/silent segments.
- "Before → after" value pairs (e.g. FPS 118→236): two Mono 500 17px values separated by a dim arrow
  glyph, second value in accent color.

**Iconography**
- Style: geometric line-drawn glyphs, **1–1.5px stroke**, built from simple rects/circles/triangles —
  no rounded corners on the glyphs themselves (sharp geometric shapes), sized to a **16–20px grid**
  inside larger hit areas (dock buttons are 54×54px containers, icon content ~16–20px centered).
  Outline style dominates (unfilled shapes), with small solid-fill accents for emphasis (e.g. a filled
  triangle/wedge, a filled half-circle). Monochrome only — glyph color is white at 45–75% opacity idle,
  brightens to accent-hi when the button is active/open.
- **The mockup states explicitly that these are geometric placeholders**, not a finished icon set:
  "Dock icons in this mockup are geometric placeholders — replace with a 1-bit 16×16 set on the pixel
  grid." So no real icon design exists to copy — only the *rules* above (stroke weight, grid size,
  outline-first, 1-bit/monochrome, sharp corners, centered in a square hit area) transfer.
- Icons our feature set will need (derive fresh SVGs at 16×16/20×20 following the above rules): settings
  (gear), display/scaling (monitor or scan-lines glyph), shaders/color grading (the mockup's half-filled
  circle "brightness/contrast" glyph is a reusable metaphor), audio (speaker + waveform, mockup already
  has a speaker-cone glyph), performance/FPS (bar-chart glyph, mockup has one), profiles/game-config (the
  mockup's "three horizontal lines with tick marks" glyph reads as a config/preset list — reusable),
  reset/restore (not present — needs a fresh circular-arrow glyph in the same stroke weight), close (×,
  present), collapse/minimize (–, present), dock overflow/more (not present — needs a fresh glyph).

## Controls added since this handoff, not covered by it

The handoff's own gaps above ("List rows" has no scrolling list; "Text inputs" section
notes no dropdown/scrollbar/free-text design exists at all) are exactly what the Profiles
area's rebuild needed widgets for. `ListBox` and `Modal` were added directly to
`src/Overlay/UI/Controls.{h,cpp}` on 2026-09-06; `Dropdown` followed the same day, once
user feedback on the Inherits row ("a dropdown, not multiple buttons") made the auto-
downgrade-only dropdown `Choice` already had not enough — a caller needed to be able to
ask for one outright. This is their entry per this doc's own convention — when to use,
keyboard behaviour — the API contract itself lives in Controls.h's comments.

### `ui::controls::ListBox` — a tall, scrollable list of items

**When to use:** a single-select list of named items too numerous or too tall for an
ordinary row — the Profiles area's list of saved profiles is the first and, so far, only
user. Not a substitute for `Choice` (a handful of mutually-exclusive options still belongs
on a segmented control or dropdown) or for `Bank` (a multi-select set of independent
switches) — `ListBox` is for a genuinely *long*, single-selection list.

**Styling:** wire lines only — a 1px hairline frame, 1px row separators, the selected row
outlined in the accent at full strength (the sketch that specified this widget drew it that
way) **and** filled with `Accent(0.10f)` — the same accent-soft backdrop `DrawRail()` paints
behind the selected rail item. The outline-only look shipped first and read as
under-selected against the Profiles list's busy rows (requests-2026-09-06.md item 3: "not
visible enough... use the properly colored backdrop"); the fill was added 2026-09-06,
keeping the outline rather than replacing it. An optional muted prefix *tag* (`[Game]`) and
an optional right-aligned
*secondary* string (`inherits Comp`) per item; the secondary is the one thing dropped when
a row is too narrow to hold all three without clipping the label — never the label itself.

**Keyboard:** Up / Down / Home / End move the selection; Enter, or a click, "activates" it
(the same event — Controls.h: *"Enter = activate = same as click"*). All of it applies
**only while the pointer hovers the list** — this kit deliberately never turns on ImGui's
own keyboard nav (see Shell.cpp's dropdown-nav comment on why: it would hand every arrow
key in the shell to ImGui's nav and take SPEC §8.2's row-adjust grammar away), and a
standalone widget has no ID-based keyboard-focus system of its own to hook a "this list
owns the keyboard right now" state into. A future host that wants Up/Down to reach the
list from somewhere else (a search box above it, say) has to forward those keys itself —
flagged here rather than silently assumed.

**Scrolling:** capped at `nMaxVisibleRows` (10 by default) before a thin accent-on-track
scrollbar appears; the mouse wheel scrolls it while hovered and touches nothing outside
the list's own rect, so it can never fight a host region's own scrolling.

### `ui::controls::Dropdown` — a real dropdown, forced

**When to use this vs a segmented `Choice`:** a `Kind::Choice` already auto-downgrades
to a dropdown-shaped control when a segmented strip would not fit (more than 5 options,
a label over 8 characters, or the measured group too wide for its lane) — a caller never
picks that, the measurement does. `Entry::Dropdown()` is the one override: it forces the
dropdown presentation regardless of whether the option set would technically fit
segmented. Use it when the option set is **user-created or unbounded** — one row per
saved profile, one row per detected display, anything whose count and labels the user
controls rather than the product — even if today it happens to be short enough to fit
five segments. Segmented stays the right call for a **fixed set of ≤5 short words** the
product itself defines (a filter type, an anchor corner): those are enumerable at design
time and reads faster as a row of buttons than as a click-to-open list. Feedback that
sent this in, verbatim: *"The inheritance selector should be a dropdown. Not multiple
buttons."* — `profiles.inherits` is exactly the user-created case (one option per saved
general profile) that had been fitting into a segmented strip by accident of having few
profiles, not because the option set was actually fixed.

**Closed state:** one box spanning the row's control zone — current value right-aligned
in Mono 500, a chevron at the right edge, same border/height vocabulary as `Text` and
`Choice`'s own dropdown branch (they share one drawing function). Hairline on hover;
accent tint and border while its popup is open.

**Open state:** a popup list anchored under the box (flipped above it when the slab has
no room below), drawn in its own top-level window exactly the way `ui::DrawModal()` is —
never clipped by the sheet's child window, and above every ordinary row. It reuses
`controls::ListBox` for the items, current value preselected, so it inherits that
widget's own scrolling (capped at 8 visible rows here), wheel and click behaviour
outright rather than a second implementation of a list. Up/Down/Home/End move, Enter or
a click picks, Esc or a click outside the box and the list closes it without changing
anything. Only one `Dropdown` popup is open at a time; a `Modal` opened on top closes it
outright, and the command palette closes it on its own opening edge.

**Why a distinct entry point from `Choice`, not a parameter on it:** `Choice`'s popup
(the auto-downgrade case) is owned by the shell's own `s_sOpenDropdown` state machine,
keyed by a Registry id string and resolved back through the Registry to write a value.
`Dropdown`'s popup is entirely self-contained inside `Controls.cpp` — no Registry lookup,
keyed by the caller's own `ImGuiID` — because its open/commit state has to survive past
the one frame the caller's own `int*` binding is valid for (the popup draws from a
separate top-level window, after the row that owns it has already returned). That means
a picked value lands in the caller's `int*` one frame after the click rather than the
same frame — imperceptible at any real frame rate, and the same trick `Text`/`Stepper`'s
own editing-state already use to cross a frame boundary through caller-owned storage.
One consequence worth knowing if this control gets a second user: its state is keyed by
the full `ImGuiID` (window stack included), not by the bare id string, specifically
because the **same** row can draw twice in one frame under the same string id — the
Sheet's own copy and the Inspector's copy of a selected row's CONFIGURE page both do —
and a string key cannot tell those two apart (the popup opened at the wrong box's
position the first time this was tried, caught by this feature's own mandatory capture).

### The Inspector's before/after comparison strip (2026-09-07)

**When to use:** a row whose whole subject is *how the picture looks*, where a number
cannot answer "is this setting right". Adaptive Brightness is the first and, so far,
only user (`requests-2026-09-08.md`). Not a general-purpose picture slot: a row declares
it by *name* — `Entry::Preview( PreviewKind::AdaptiveBrightness )`, an enum in
`Registry.h` — and `Shell.cpp` decides what that name draws. `Why an enum and not a
`std::function<void(ImRect)>`:` a callback here would be a general custom-draw escape
hatch in the registry, which is precisely the door SPEC §5.2's registration laws exist
to keep shut. Adding a second preview is a deliberate act in two files.

**Shape.** One block, laid out by `controls::LayoutComparePreview()`, reserved by
`controls::ComparePreviewHeight()`:

```
BEFORE                              AFTER     <- 14px label line, TextMeta
                                              <- 4px gap
+-------------------+-------------------+
|                   |                   |     <- one picture, 16:9,
|   captured frame  ‖  same frame with  |        1px Role::Line hairline
|                   ‖  the effect on    |
+-------------------+-------------------+
                    ^ divider, on the midpoint
```

- **Width** is the Inspector's content width, **capped at 240 logical px**. The block
  sits *above* the VALUES/params, so every pixel of it pushes them down; 320 px (tried
  first) cost four param rows of visible space at 1280×720, 240 costs three.
- **The two labels sit outside the picture, one per half**, each aligned to its own
  half's outer edge. `Why not inside the picture:` a `TextMeta` label is only quiet if
  it is legible, and inside it would be sitting on arbitrary game content.
- **The divider is two coats, not a hairline**: a 4px black at 55 % with a 1px white at
  85 % down its middle, so a dark fringe survives on each side of the bright line. Same reason — a single hairline of any one colour disappears
  against half the content it can land on. This is a deliberate exception to the
  1px-hairline rule, and the only one: it is a *boundary between two images*, not a
  boundary between two UI surfaces.
- **The empty state is a bordered box with one centred `TextMeta` sentence**, on the
  `SurfaceRaised` fill — never a black rectangle, which reads as broken rather than as
  empty. `Controls.h`'s `ComparePreviewStatusFor()` is the whole state machine and
  every not-ready branch names itself; see `superdoc/features/shader-effects.md` for the
  three messages and their ordering.

**Where the pixels come from, and the refresh rule** are the feature's own, not this
guide's: `shader-effects.md`, "The Inspector's before/after preview".

**Known limitation.** The strip scrolls with the rest of the CONFIGURE page, so at
1280×720 — where this row's eight params already overflowed the Inspector body — the
lowest params cannot be dragged while watching it. Pinning it above the scrolling body
is the fix and is deliberately not in this change.

### `ui::Modal` — a small centred dialog

**When to use:** a short, focused task that needs the user's full attention before
anything else continues — Create/Copy/Edit's field-entry dialogs and Delete's
confirmation prompt, per the Profiles sketch. Not a place to put a whole settings surface
(that is what the sheet and the Inspector are for) — if a "modal" would need to scroll or
carry more than a handful of rows, it is the wrong control.

**Composition:** a title, a **body** of ordinary rows (drawn with the exact same row
allocator the sheet uses — `ModalNextRow()`/`ModalNextBlock()` hand out a `RowCtx`/`ImRect`
the same way `RowCtx::ForRow()` does for a sheet row, so `controls::Switch`,
`controls::Text` and the rest work inside it completely unchanged), and a footer with
**Cancel** and one caller-labelled primary button (red-tinted when `bPrimaryDanger` is
set — Delete's own colour, matching `Verb`'s existing `Intent::Danger`, since no separate
danger colour exists anywhere else in this doc's palette to draw from instead).

**Keyboard:** **Esc** always cancels. **Enter** confirms (fires the primary) whenever no
field is currently being edited — this is a deliberate simplification, not the literal
"Enter in the LAST Text field" the Profiles sketch describes: this kit has no cross-field
tab order for a modal to know which field is "last," and building one was out of this
task's scope (Controls.h's `ModalSpec::fnPrimary` comment records the reasoning). Tab-
between-fields does **not exist** for the same reason — the shell's own `Text` control has
no focus-traversal system to extend, only a per-field click-to-edit toggle.

**Scrim and layering:** reuses the exact scrim fill Shell.cpp's command palette already
dims the shell with, rather than inventing a second "surface behind me is dimmed" look. In
the shell, `ui::DrawModal()` is drawn from its own top-level ImGui window (`SetNextWindowFocus`,
no `NoBringToFrontOnFocus`) opened after the slab and before the palette, so it sits above
every sheet/Inspector content and below the palette — matching the palette's own
documented layering reasoning exactly.

**One at a time:** a second `OpenModal()` while one is already open is a programming
error (`IM_ASSERT()` in a build with assertions compiled in — this repo's own
`build-release` does not currently pass `-DNDEBUG`, so that guard fires there today too,
not only in a `build/` debug tree); the already-open modal is left untouched either way.

## Motion / interaction feel

The handoff is static HTML/CSS mockups — **no transition durations, easing curves, or animation timing
are specified anywhere** in the build spec or the markup (no `transition:` properties present at all).
The only interaction behaviors stated are functional, not animative:
- Sliders: drag or mouse-scroll to adjust (no stated ramp/inertia).
- "Live apply" — every control commits immediately on change, no debounce or apply button mentioned.
- Hover-triggered tooltip (dock button, treatment C) — no fade timing given.
- `SHIFT+TAB` toggles the whole overlay open/closed — no stated open/close animation (appears instant in
  the mockup, though a fade or scale-in would be a reasonable, undocumented embellishment).

**ImGui reality check:** immediate-mode UI has no built-in tweening; anything beyond instant show/hide
requires manually driving alpha/scale over frames (e.g. lerping a window's alpha across N frames after a
toggle key). Given the source design specifies zero motion values, the safe interpretation is: **ship
with no animation** (instant state changes) rather than inventing timing values the design never asked
for. If a later pass wants a toggle fade, treat it as a new decision, not a mockup requirement.

## ImGui feasibility notes

This is the most load-bearing section for implementers — what's achievable natively vs. needs custom
draw calls vs. should be dropped.

| Design element | ImGui native? | Verdict |
|---|---|---|
| Flat colors, hairline borders, square corners | Yes | Native `ImGuiCol_*` + `PushStyleVar(FrameRounding, 0)` / `WindowRounding` cover this directly. |
| Window corner radius (3–4px) | Yes | `style.WindowRounding` — trivial. |
| Backdrop blur behind glass windows | **No** | ImGui draws opaque quads; real backdrop blur needs a custom post-process pass sampling the framebuffer behind each window (a blur-and-composite render pass gamescope would need to add). **Custom-draw job**, and a real cost (extra render pass per visible overlay window) — likely the single biggest scope item in this design. Approximation: fake it with a semi-opaque flat fill (current `surface` alpha already does most of the visual work even without blur) and drop true blur for v1. |
| Accent glow / box-shadow (slider handle glow, focused-window glow, status-dot glow) | **No** | ImGui has no shadow/glow primitive. **Approximation**: draw a few soft concentric circles/rects at decreasing alpha behind the element via `ImDrawList::AddCircleFilled`/`AddRectFilled` with blurred-looking falloff — cheap and close enough for small glows (status dot, slider handle). Large soft window shadows are more expensive to fake convincingly; **consider dropping** the big drop-shadow-with-blur and keep just the accent border for focus. |
| Gradient fills (header bar gradient, slider fill gradient) | Partial | `ImDrawList::AddRectFilledMultiColor` supports 4-corner gradients natively — **native**, just needs the two colors picked. |
| Linear-gradient gauge glow under slider handle | Native (approx.) | Covered by the same multicolor-rect trick above. |
| Custom widget geometry (rectangular slider handle instead of circular, square toggle knob instead of pill, custom checkbox mark) | Yes, with custom draw | ImGui's built-in `SliderFloat`/`Checkbox` render circular/rounded widgets by default; matching this design means writing custom widget draw code using `ImDrawList` primitives instead of stock widgets (still "ImGui", just not `ImGui::SliderFloat`'s default look). Budget real implementation time here — nearly every control in this design deviates from stock ImGui rendering. |
| Bar-graph mini charts (frametime history, audio peak meters) | Yes | Straightforward `AddRectFilled` loops — native, easy. |
| Segmented control / tab strip with active-state accent fill | Yes, custom draw | No stock ImGui widget matches this exactly; implement as a row of custom buttons with manual active-state coloring. Straightforward. |
| Tabular/monospaced numeric alignment | Yes | Solved by using the Mono font for all numeric text; no ImGui-side trick needed. |
| Variable font weights (400/500/600 Sans, 400/500/600 Mono) | Partial | ImGui font atlas needs one *baked* font per (family, weight, size) combination actually used — no runtime weight interpolation. Bake the ~4–6 distinct family/weight/size combos actually seen in the spec (not every theoretical weight) into the atlas at startup. Flag as an up-front font-loading/atlas-size decision, not a blocker. |
| Letter-spacing on uppercase titles (.15–.16em tracking) | **No** | ImGui text has no built-in tracking/kerning control. **Approximation**: manually insert space glyphs or draw glyph-by-glyph with an added x-advance offset via `ImDrawList::AddText` per-character. Cheap enough to implement once as a helper (`DrawTrackedText(...)`) and reuse everywhere titles/labels need it. |
| Drop-shadow text (FPS HUD "shadow" backdrop mode) | Yes, custom draw | Draw the text twice (offset dark copy behind, bright copy on top) — a standard cheap trick, native to implement. |
| `mix-blend-mode: screen` (additive-blend HUD text option) | Partial | Real blend-mode compositing against the game framebuffer needs the renderer to draw that text in a separate pass with additive blending enabled (`ImDrawList` supports custom `ImDrawCallback` to change blend state) — doable but is a genuine render-state customization, not a style tweak. **Custom-draw job**, moderate effort. |
| Free-floating, draggable, overlapping windows with persisted position/open-state per game | Yes | This is exactly what ImGui windows do natively (`ImGuiWindowFlags_NoResize`, position via `SetNextWindowPos` once then let the user drag, persist via `ImGui::SaveIniSettingsToMemory` or our own per-game config). Native, low risk. |
| No-resize, collapse/close only | Yes | `ImGuiWindowFlags_NoResize` + custom collapse/close glyph buttons in place of stock ones (stock ImGui collapse/close exist but won't match this design's glyph style — reuse the tracked-text/custom-draw approach). |

Overall risk ranking for planning purposes: **backdrop blur** is the one item that could meaningfully
change gamescope's render pipeline (extra compositing pass) — everyone else on this list is either
free (native ImGui) or a bounded amount of custom `ImDrawList` code.

## Technical integration research (ImGui/Vulkan backend + prior art)

Authorized as an addition to this scout's brief mid-task; added via WebSearch/WebFetch against primary
sources plus a few grep checks against this repo's own Vulkan/input code. External claims are marked
with their source and the date checked (2026-08-21); repo claims are marked "(verified in this repo)"
and point at the exact file/line. This section is research to inform later implementation planning — it
does not change the visual-design content above, and nothing in this bullet list has been implemented.

**ImGui's Vulkan backend requirements** (source: `ocornut/imgui` master, `backends/imgui_impl_vulkan.h`
and `.cpp`, GitHub, checked 2026-08-21):
- Needs a `VkQueue` + its queue-family index. *(Verified in this repo: gamescope already has a general
  queue family with `VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT` — `src/rendervulkan.cpp:362`,
  `:542`, `:549` — reusable for ImGui's submissions rather than needing a new queue.)*
- Needs either a `VkRenderPass` **or** dynamic rendering (`UseDynamicRendering` +
  `PipelineRenderingCreateInfo`) in the newer API. *(Verified in this repo: gamescope's device is already
  created with `dynamicRendering = VK_TRUE` — `src/rendervulkan.cpp:621` — so ImGui's dynamic-rendering
  path applies directly; no render-pass plumbing needs to be introduced just for the overlay.)*
- Needs a `VkDescriptorPool` (either supplied with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`,
  or have the backend create its own via `DescriptorPoolSize`). *(Verified in this repo: gamescope
  already owns a descriptor pool for its own compositing sets — `src/rendervulkan.cpp:910-925`.
  Recommend giving ImGui its **own** dedicated pool rather than sharing gamescope's — different
  allocation/churn pattern — which is also ImGui's documented default when `DescriptorPoolSize` is set.)*
- Needs `MinImageCount`/`ImageCount` matching the target swapchain, and an MSAA sample count (not
  independently verified against gamescope's current compositing target — check at implementation time).
- Font atlas: as of ImGui's mid-2025 texture-management rework (`ImGuiBackendFlags_RendererHasTextures`),
  `ImGui_ImplVulkan_CreateFontsTexture()`/`DestroyFontsTexture()` were **removed** — font texture
  creation/upload now happens automatically on first use, via the backend's own internal command
  buffer + fence and the queue given at `Init`. This is simpler than older ImGui versions (no manual
  upload step, no dedicated transfer queue needed) but is **version-dependent** — confirm whichever
  ImGui commit/tag we vendor actually has this behavior before assuming it.

**Docking / multi-viewport** (source: `ocornut/imgui` master, `docs/FAQ.md`, GitHub, checked 2026-08-21):
- The `docking` branch bundles both docking (tabbed/snapped panels) and multi-viewport (ImGui windows
  escaping into separate OS-level windows) together, kept in sync with upstream master.
- Multi-viewport requires the *platform* backend to create/manage real OS windows — a poor fit for
  gamescope, which **is** the compositor, not a desktop app spawning windows on one. Recommend not using
  multi-viewport.
- Docking's tiling/snap-into-tabs behavior doesn't match this design either — the build spec (§06,
  above) calls for free-floating, overlapping, titlebar-dragged windows with per-game persisted position,
  which is what plain upstream ImGui (`ImGui::Begin` + `SetNextWindowPos`, no docking branch) already
  does natively. **Recommendation: use stock ImGui, not the docking branch, for this design.**

**Input-capture prior art:**
- MangoHud (Vulkan/OpenGL implicit layer; source: `flightlessmango/MangoHud` GitHub + its community wiki,
  checked 2026-08-21) is mostly a display-only HUD; since it's injected into an arbitrary target
  process via the Vulkan loader's implicit-layer mechanism, it can't assume it owns input, so its
  hotkey-toggle detection hooks the platform directly (X11 key-grab, Wayland protocol hooks, or
  `GetAsyncKeyState` on Windows) fully outside the host app's input loop. **Not directly transferable**
  — gamescope doesn't have this problem.
- vkBasalt (source: `DadSchoorse/vkBasalt` GitHub, checked 2026-08-21) is a pure post-processing layer
  with no interactive UI upstream — config is a text file, hot-reloaded via a hotkey. Forks that add an
  ImGui config UI (`vkBasalt_overlay`, `BettervKBasalt`) are third-party; their input handling was not
  independently verified here (source not fetched) — flagging as unconfirmed, not a claim.
- **The actually-relevant precedent is this repo, not an external layer**: gamescope already owns
  keyboard/mouse focus routing end-to-end (`wlserver_keyboardfocus()` / `wlserver_mousefocus()` in
  `src/wlserver.cpp`, plus steamcompmgr's focus-window bookkeeping) *(verified in this repo)*. This
  session's own git log shows active, ongoing work on exactly that focus path — `fcc1341` "OpenVRBackend:
  take input focus when a connector becomes visible", `1f0321c` "steamcompmgr: cope with a missing input
  focus window", `396794a` "steamcompmgr: reclaim keyboard focus when it lands on None", `0f8dc34`
  "steamcompmgr: re-apply keyboard focus to the preserved subwindow" *(verified: `git log --oneline -5`
  in this repo, 2026-08-21)*. That existing focus-routing machinery is the natural integration point for
  "SHIFT+TAB redirects input to the overlay" — a materially easier position than either MangoHud's or
  vkBasalt's, since neither of those projects owns input focus the way gamescope-as-compositor does.
- `ValveSoftware/gamescope#1537` ("[PoC] Steam overlay support for Gamescope WSI", GitHub, checked
  2026-08-21) is a different, adjacent problem — getting the *external* Steam overlay process to coexist
  with gamescope's HDR/XWayland-bypass swapchain — not an in-compositor ImGui overlay, and not directly
  reusable. Worth remembering only as a reminder that gamescope has had prior friction specifically
  around overlay-and-swapchain interaction (HDR color correctness, XWayland bypass timing) that a native
  ImGui overlay composited into the same output should stay clear of.

**Sources consulted (checked 2026-08-21):**
- https://github.com/ocornut/imgui/blob/master/backends/imgui_impl_vulkan.h
- https://github.com/ocornut/imgui/blob/master/backends/imgui_impl_vulkan.cpp
- https://github.com/ocornut/imgui/blob/master/docs/FAQ.md
- https://github.com/flightlessmango/MangoHud
- https://github.com/DadSchoorse/vkBasalt
- https://github.com/ValveSoftware/gamescope/issues/1537

## Open questions for the user

1. **Backdrop blur** is the design's signature look (every panel + the dock use `blur(20–22px)`) but is
   the one thing ImGui cannot do without a real render-pass change in gamescope. Is a flat semi-opaque
   fallback (no blur) acceptable for v1, or is blur worth the extra compositing work?
2. The handoff is **dark-theme only** — no light variant was designed, and no error/danger color exists
   anywhere in the palette. Do we need a light theme, and do we need a danger/error color added, or does
   the overlay stay dark-only with success/neutral status colors only?
3. **Font shipping**: IBM Plex Sans + IBM Plex Mono (OFL-licensed, safe to bundle) is the design's stated
   typeface. Confirm we're bundling both families (several weights each) as part of the build, rather
   than substituting a system/existing font — the "never mix a number into a sans run, always tabular
   mono" rule depends on Plex Mono specifically being monospaced and shipped.
4. Text inputs, dropdown/combo menus, and scrollbars have **no design coverage at all** in this handoff
   (the mockup avoids them by using sliders/segmented-controls/toggles for everything). If our feature
   set needs free-text entry (e.g. naming a profile) or a long scrolling list, should implementers
   extrapolate from the flat-hairline-box language documented above, or is a fresh design pass needed?
5. **Accent color**: is cyan (`oklch(.74 .12 218)`) locked in as the permanent brand accent, or should
   the overlay expose the ember/signal-green alternates shown in `1f` as a user-facing theme picker (which
   would mean also tinting the glass warm/cool per-theme, not just swapping one color)?
6. The mockup's toggle switch always renders its track in accent color regardless of on/off state
   (only the knob position differs) — is that genuinely intended (matches the source), or should "off"
   get a visually distinct neutral-gray track? The handoff never shows an off-state toggle to confirm.
7. **Motion**: the source design specifies zero transition/animation timing. Should the overlay open
   (`SHIFT+TAB`) instantly as the design implies, or is a brief fade/scale-in wanted even though nothing
   in the handoff calls for it?

### The List band, its verb strip, and the inherited/overridden dot (2026-09-06, Profiles)

`CompositeKind::List` hosts `ListBox` in the sheet: a **six-line band** (`tok::kListBandLines`;
every other composite is 2 or 3), edge to edge -- the one band that spends the label column,
because the Profiles sketch has the list *leading* the sheet rather than sitting in a row's
control column (Band.cpp's List case gives up SPEC §4.2's clauses 2 and 4 for it, on
purpose). Lines 1–5 are whole list rows (the box is sized to `floor(h / kControlH)` rows so no
blank strip is left under a fractional remainder); line 6 is the **verb strip**
(`controls::VerbStrip`): N *equal-width* verb chips, `kGapSeg` apart, in `Verb`'s own accent /
danger colours -- equal rather than measured widths because the sketch draws four same-size
buttons and a strip whose chips resized as their labels changed would jitter. No row-selection
wash behind the band (a 6-line accent slab under a box with its own frame read as an error);
the state edge stays. The Inspector's CONFIGURE page does **not** redraw a List band (it did,
and put a second list with a second Create/Copy/Edit/Delete beside the real one) -- it prints
the selection and the verbs' disabled reasons instead.

**The inherited/overridden dot:** a `Px(2.5)` accent circle `kS` after a row's label, on
*overridden* rows only, while the session profile inherits. It is the design guide's "status
dot" at its smallest; it shares neither the state edge's slot (D6: differs-from-default) nor
the affordance column (SPEC §2.4: one glyph, by priority), so it can displace nothing.
Inherited rows draw nothing -- the parent's values are the baseline, and marking the majority
would bury the deviations the dot exists to show. The words (`inherited from Comp`,
`overridden` + a neutral **Reset to inherited** chip) live in the Inspector's CONFIGURE page.

### Rail groups (2026-09-06)

The rail's small uppercase section dividers (`TypeRole::Section`, `Col(Role::TextMeta)` --
the same label vocabulary the group/section headers elsewhere in this guide use) mark four
fixed groups, in this order (requests-2026-09-06.md item 4): **DISPLAY** (General, Resolution,
Upscaling, Frame limiter, HDR, Shaders), **MISC** (HUD, Mixer, Crosshair), **SETTINGS**
(Profiles, System, Appearance, Cursor), **OTHER** (Log, Changelog). The icon-collapsed rail
keeps the divider as a bare rule (§8.0's collapse is about width, and a heading is the one
thing that cannot survive it) exactly as the section dividers already did.

This replaced an earlier three-way `DISPLAY` / `SYSTEM` / `SETUP` grouping keyed off each
area's own `Section` at registration -- that enum (`Registry.h`) still exists (every
`reg.Add()` call still takes one) but nothing draws from it any more. The four groups above
are their own fixed table (`RailGroup`, `RailOrder()`, `RailGroupFor()` in `Registry.h`/`.cpp`,
walked by `Registry::RailAreas()`), independent of both `Section` and each area's own
registration order, and kept in `Registry.cpp` rather than `Shell.cpp` (where the rail is
actually drawn) so a plain unit test can pin it without an ImGui context -- see
`test_overlay_ui.cpp`'s "rail:" test cases. `setup.cursor` is not named by the request's four
groups; it kept its former `Setup` neighbours (Profiles, Appearance) under SETTINGS rather
than a placement the request never specified.

### Selection follows edit (2026-09-06/07/08, requests-2026-09-07.md item 8, requests-2026-09-08.md item 2)

**The rule:** any press that lands on a Sheet row's own control selects that row, the
same as clicking the row does -- whether or not it moves a value. A slider handle
pressed where it already sits, a dropdown *opened*, a hue rail, a colour picker's R/G/B
rail, an anchor-grid cell, a list row, a switch, a stepper's -/+ or typed commit, a
segmented Choice cell, an Action press: all of them select. The user's own framing:
*"editing any element should automatically select it, so it also pops up in the
inspector rail."*

**It was first written as "any VALUE CHANGE selects", and that was not enough** -- the
user's reply was *"Nope, doesnt work. Even editing a slider should make it select the
line."* Measured with one real click or drag per atom kind against a running binary
(`build-release/verify-shots/selection-follows-edit-2026-09-08/`), the value-change rule
left 6 of 13 cases dead:

| what was pressed | before | after |
|---|---|---|
| Slider (click, and drag), Switch, Stepper (-/+ and typed), segmented Choice, Action | selects | selects |
| Dropdown -- opening it | **no** (opening changes no value) | selects |
| Dropdown -- committing a pick | **no** (the commit happens in `DrawDropdownList`, after the slab, outside the row painter's return) | selects |
| Composite band -- accent hue rail | **no** (hue moved 218 deg to 60 deg, selection stayed on the row above) | selects |
| Composite band -- a colour picker's R/G/B rails | **no** | selects |
| Composite band -- anchor grid cell | **no** | selects |
| Composite band -- list row | **no** | selects |

Note *which* cases those are. A composite band's body is where the user's "slider"
actually lives -- the accent hue rail and a colour picker's three R/G/B rails **are**
sliders -- and `DrawCompositeBand` returned nothing but its own bare `##band` click,
which D22's AllowOverlap rule guarantees never fires for a press that lands on the body.

**Why this needed a fix at all, not just a wire-up:** D22's own AllowOverlap rule (see
its comment on `DrawEntryRow` in `Shell.cpp`) means a press that lands ON an atom --
the slider handle, the switch, the stepper's glyphs, a segmented cell, the dropdown --
resolves ImGui's hit test to *that atom*, never to the row's own full-width selector
button underneath it. So before this fix, dragging a slider (or flipping a switch, or
picking a Choice) on a row that was not already selected changed the value but left
whatever row *was* selected highlighted, with the Inspector still showing the wrong
one -- selection only ever moved on a raw click that missed every control.

**The mechanism: engagement, read off ImGui's `ActiveId`.** Both row painters --
`DrawEntryRow` *and* `DrawCompositeBand` -- snapshot `ImGui::GetActiveID()` immediately
before submitting their control and again immediately after it, and return "select me"
when `ui::controls::ControlEngaged( before, after )` says a control took the press right
there: `after != 0 && after != before`. Every atom in the kit goes through
`Controls.cpp`'s `Begin()` -> `ItemAdd()` + `ButtonBehavior()`/`SliderBehavior()`, so a
press *always* takes `ActiveId` -- which is why one test covers a rail, a grid cell, a
swatch, a list row and a plain switch alike, instead of each atom kind having to
remember to report itself upward.

The two halves of the test each carry their own weight, and getting either wrong is
silent. Dropping `!= before` would make **every** row drawn during a drag claim the
press, so the selection would follow the mouse down the column; dropping `!= 0` would
make the frame a drag *ends* re-select whichever row drew first.

The earlier routes are kept, not replaced: `ui::controls::ShouldSelectRow( bClicked,
bValueChanged, bControlEngaged )` is the OR of all three, so a raw click on the row and a
value change (e.g. one arriving from the keyboard) still select exactly as before. The
rule lives in `Controls.h`/`.cpp` for the same reason `ConstantWidthGrab()` does:
`Shell.cpp`'s row painters are file-private by design (`Shell.h`'s own header comment --
"this is the whole of its public surface... deliberately") and unreachable from a test,
so the logic is named and pinned in `test_overlay_ui.cpp` (the truth tables) and
`test_overlay_atoms.cpp` (a real press per atom kind, in the headless ImGui harness)
rather than living unexplained at the call site.

**What is deliberately NOT covered, and why.** Selection is a *Sheet* concept, so only
the Sheet's own row loop acts on the painters' return value:

- **The Inspector's own-row copy** of the selected Entry (`bAffordance == false`)
  discards it entirely. Editing there must not look like a fresh selection -- that is
  what keeps a value changed in the Inspector from resetting the Inspector's own focus.
- **Parameter rows**, wherever they are drawn -- inline under an expanded row
  (`DrawInlineParams`) and in the Inspector's VALUES block. A parameter is not a row and
  cannot be selected at all; there is nothing for the selection to move *to*. (The
  2026-09-06 pass excluded these on the grounds that "inline mode has no inspector";
  that reasoning was wrong -- inline mode is a layout, not the reason -- but the
  exclusion itself is right, for the reason given here.)
- **`Text`** (a profile/game name field): its `DrawSharedControl` case returns `false` by
  its own design and typing in it is not "editing a control" in the sense above.
  Clicking into the field still selects the row through the engagement route, because
  the field takes `ActiveId` like any other control -- which is the behaviour that was
  wanted anyway.

**The bug this rule's OWN fix nearly reintroduced (item 9/B):** `DrawEntryRow` is called
twice per frame for the currently-selected Entry -- once as the Sheet's own row, once
again inside the Inspector to draw "the row's own control" at the top of its VALUES
block (`Shell.cpp`'s `DrawInspector`) -- and both calls share the exact same `Id()`.
Typed entry's one bit of state (`s_sEditingText`, keyed by id alone) could not tell
those two copies apart: opening the Sheet's Stepper field one frame set the global to
that id, and the Inspector's copy -- drawn moments later the SAME frame, reading the
now-set global back as "I am the one being edited" -- opened ITS OWN field too and won
the actual ImGui keyboard focus (`SetKeyboardFocusHere()`), so the visible caret ended
up in the Inspector's copy rather than the Sheet field the user clicked. Fixed the same
way `s_sOpenDropdown`/`s_eOpenDropdownRegion` already solved the identical disease for
dropdowns: a companion `Region s_eEditingRegion` qualifies every read and write of
`s_sEditingText`, so the Sheet's field and the Inspector's copy of the same id are
independently open/closeable. The Inspector's own-row `DrawEntryRow` call discards its
return value entirely, which is what keeps a value change made there from re-selecting
anything or resetting the Inspector's own scroll/focus -- editing IN the Inspector's
copy must never look like a fresh selection.

### Window transparency: 1.0 means opaque (2026-09-07, requests-2026-09-07.md item 9/C)

**The mapping:** `overlay.window_opacity` (0.3..1.0, `PanelConfig.cpp`) sets the FINAL
drawn alpha of the slab background and the Inspector's own fill directly -- at 1.0 the
surface is the theme colour at alpha 255, no residual blend, and the game cannot show
through at all; at the slider's 0.3 floor the chrome is dim but still legible, not
invisible (kept as-is; no floor change was needed).

**Why `Dim()` was the wrong function:** `Dim( col, factor )` *scales* whatever alpha
`col` already carries. `Role::Surface`'s own literal is baked at 88% alpha (its
designed glass look -- see this guide's own colour palette table above), so
`Dim( Col( Role::Surface ), 1.0f )` returned alpha ≈224, not 255: "no transparency"
was still visibly see-through. `Colors.h` gained `WithAlpha( col, alpha )`, which
replaces the alpha channel outright instead of scaling it -- the opposite contract from
`Dim()`, used everywhere else Sheet rows dim for disabled state. Both `Shell.cpp` call
sites (the slab's `ImGuiCol_WindowBg` and the Inspector's `ImGuiCol_ChildBg`) now use
`WithAlpha( Col( Role::Surface / SurfaceInspector ), palette::WindowOpacity() )`.
Pinned in `test_overlay_atoms.cpp` ("colors: WithAlpha..." / "colors: window_opacity
1.0 renders the slab fully opaque").

