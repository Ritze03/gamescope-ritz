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
- **Handle and fill share one value -> x map (2026-10-04 fix).** The re-centring above fixed the
  handle's *width* but kept ImGui's int-grab *centre*, whose travel is inset by half of a grab
  widened to track / (range + 1) — so on a small-range int slider the handle sat far from where the
  fill ended (Motion blur Samples 2..8: 1/14 of the track off at each end; the user: *"the dragger is
  wrong compared to where the line actually fills to"*). Now `ui::controls::SliderValueX()` places
  the fill end, the handle centre and the default tick alike, travel `[min + handle/2 .. max -
  handle/2]`, and `SliderGrab()` always drives `SliderBehavior` with a float (`SliderInt` rounds via
  `"%.0f"`), so ImGui's click/drag travel is that same map. `ConstantWidthGrab()` was removed.
  `Why the fill ends under the handle, not at the full-width fraction:` the full-width map is exact
  at 0/1 but drifts up to half a handle off it in between. Exactly at 0 and 1 the fill snaps to
  empty / full instead, because a disabled slider's handle is translucent and the half-handle stub
  showed through it as fill on an empty slider.

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

**Every animated value must call `force_repaint()` until it lands** (2026-09-29). `Why:` the
rail-width animation (`s_flRailAnim`, `Shell.cpp`) advanced its `Approach()` only on frames the
loop happened to draw anyway and could stall mid-transition until the next input woke it — the
same idiom the rail accordion's `StepRailAccordionAnim()` and `SettingsOverlay.cpp`'s
`UpdateFadeAlpha()` already followed. Fixed by requesting a repaint whenever the value is still
short of its target and stopping once it snaps.

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

### Rail groups (2026-09-06; regrouped to six 2026-09-27, I7; seven since MOTION 2026-10-04)

The rail's group headers mark **seven** fixed groups today, in this order: **DISPLAY**
(General, Resolution, Upscaling, Frame limiter, HDR, Shaders), **MOTION** (Frame generation,
Motion blur -- added 2026-10-04; Frame generation moved here out of DISPLAY, where it had sat
below Shaders for a day), **OVERLAY** (HUD, Crosshair,
Zoom, Cursor), **INPUT** (General, Autoclicker, Null binds), **MISC** (Friends, Mixer),
**SETTINGS** (Profiles, System, Appearance, Keybinds), **OTHER** (Log, Changelog). The
icon-collapsed rail draws one icon button per group (see "Rail accordion" below) rather than
a bare divider now, since I2's own accordion rework.

`Why:` (I7) the user's own table (lightly reflowed from its original bulleted form): *"lets
reorder some of the categories: Display: General, Resolution, Upscaling. Overlay: HUD,
Crosshair, Zoom, Cursor. Input: General (Force grab cursor [remove from Display > General],
Force grab keyboard), Autoclicker, Null binds. Misc: Friends, Mixer."* MISC had grown to seven
areas spanning three unrelated concerns --
things drawn over the game (HUD/Crosshair/Zoom/Cursor), things that intercept or rewrite input
(Autoclicker/Null binds, soon General's own grab toggles), and one unrelated social feature
(Friends) -- so OVERLAY and INPUT split those two concerns into their own tabs, leaving MISC
as the true two-item leftover. `input.general` is a NEW area a sibling task adds (this task
only reserved its rail slot); until it lands, INPUT shows Autoclicker and Null binds only --
`Registry::RailAreas()` skips a slot whose id `FindArea()` cannot resolve rather than
crashing or leaving a gap, which is what let the table and the area land in separate commits.

**Originally** (2026-09-06, requests-2026-09-06.md item 4): four groups -- DISPLAY (as
above), MISC (HUD, Mixer, Crosshair), SETTINGS (Profiles, System, Appearance, Cursor), OTHER
(Log, Changelog) -- which itself replaced an earlier three-way `DISPLAY` / `SYSTEM` / `SETUP`
grouping keyed off each area's own `Section` at registration. That enum (`Registry.h`) still
exists (every `reg.Add()` call still takes one) but nothing draws from it. The groups are
their own fixed table (`RailGroup`, `RailOrder()`, `RailGroupFor()` in `Registry.h`/`.cpp`,
walked by `Registry::RailAreas()`), independent of both `Section` and each area's own
registration order, and kept in `Registry.cpp` rather than `Shell.cpp` (where the rail is
actually drawn) so a plain unit test can pin it without an ImGui context -- see
`test_overlay_ui.cpp`'s "rail:" test cases.

### Rail accordion (2026-09-27, I2, D2/D6)

The four groups above are unchanged; what changed is how the rail draws them. The section
"### Rail groups" above describes a flat list -- every group's header always visible, every
group's areas always drawn beneath it. That overflows the rail: 19 areas in 4 fixed groups
already total ~896px of content (four 30px headers plus nineteen 40px rows plus the rail's
own top/bottom pad) against ~878px available at 1080p/scale 1 (`0.85 x 1080` for the slab,
minus the slab bar's 40px), and a 20th area (Null binds, MISC) landed the same week. The
user, dictating the fix: *"make the categories that we have now look like tabs and when one
of them is pressed, it extends the options for the category like tabs below it. This would
allow us to make the category smaller too."* Offered a plain tab strip as the alternative;
the user picked the accordion.

**The rule.** Each group header is now a clickable tab. Only one group's areas are ever drawn
at once; opening a group closes whichever other group was open. Clicking the OPEN group's own
header collapses it -- every group closed (`RailGroup::Nothing`) is a legal state, not just an
intermediate one. A collapsed group's area rows are not drawn and take no height at all (they
are skipped inside `DrawRail()`'s own measuring/drawing walk, not merely hidden by a clip
rect), which is the entire saving: with one group open the rail's content height is a handful
of headers plus that one group's rows, comfortably inside the available height regardless of
how many areas pile up in the *other* three groups.

**The open group always matches the selection.** Selecting an area -- from anywhere: a rail
click, a command-palette/launcher jump, `StepArea()` (both Up/Down in the rail and
Ctrl+Left/Right), the `overlay_e2_select` console command, or wlserver's area-request path
(the friends binding) -- opens that area's group, closing whatever else was open. Every one of
those call sites now routes through one function, `Shell.cpp`'s `SetSelectedArea()`, so this
cannot be forgotten at a future seventh call site. On the Shell's own opening edge
(`ResetTransient()`) the open group is likewise re-derived from the current selection rather
than restored from whatever a header click had last left it at -- the open-group state is not
persisted across a close, by design (see that function's own comment): the next open should
land on the area you were last looking at, not on whichever group you happened to be poking at
before you closed the overlay.

**Look (I2 first cut).** A header is a pill/button, visually distinct from an area row: a
`▸`/`▾` chevron (the same drawn-disclosure convention an inline-expandable Sheet row already
uses -- right while closed, down while open), the group's own name, a flat `Role::SurfaceRaised`
tint at rest (the same "control box, inactive segment" tone Controls.h already uses, so a closed
header reads as pressable even before you notice the chevron), and the SAME accent
wash-plus-2px-left-bar an active area row gets when it is the OPEN one -- one "this is the
active thing" convention, not two. The icon-collapsed (60px) rail replaces its old bare
divider rule with one icon BUTTON per group, sized like an area's own icon-mode row (a full
40px item slot, not the old 20px gap) so its glyph sits in the same box an area icon does;
`Icons.h`'s `IconForRailGroup()` is a new, separate four-entry table (monitor-on-a-stand for
DISPLAY, three dots for MISC, a four-spoke dial for SETTINGS, a two-compartment archive box
for OTHER) rather than being folded into the area icon table, because these are group buttons,
not areas, and mixing them in would break `test_overlay_ui.cpp`'s "every registered area has
exactly one icon" bijection. **Superseded by the I3 polish pass immediately below** -- the
paragraph above is kept as the record of what I2 actually shipped, since the QC finding that
triggered I3 is only legible against it.

### Rail accordion polish (2026-09-27, I3, post-QC)

A vision-QC pass over I2's own captures (`build-release/verify-shots/rail-accordion-2026-09-27/`)
found the accordion "genuinely more compact and less cluttered, but it does not yet read as
tabs," with four concrete complaints. Each is addressed here, in the same file the accordion
itself lives in (`Shell.cpp`'s `DrawRail()`), plus one unrelated bug the QC pass's own fourth
finding led to (see "Sheet body: the mouse wheel now scrolls it" below).

**1. Open vs. closed barely differed.** I2's open header used the SAME `Accent(0.10f)` wash
an active area row gets, at the same alpha -- correct in principle ("one 'this is the active
thing' convention, not two") but too faint against the rail's own already-dark background to
read as unmistakably accent-coloured, and a closed header's `SurfaceRaised` tint (a flat 6%
white wash) was close enough in luminance that only the chevron direction told the two apart.
Fixed two ways: the open header's wash rose to `Accent(0.16f)` AND gained a full 1px accent
`Outline()` around the whole pill (not just the 2px left bar) -- the SAME "fill alone
under-read, add the outline too" fix this guide's own `ListBox` section already records
(*"The outline-only look shipped first and read as under-selected... the fill was added,
keeping the outline rather than replacing it"*), applied in the opposite direction here since
the header's first cut had the gap in the other place (a bar, not a ring). The open group's
child rows also gained an unconditional left indent (`tok::kS`, 8px in non-icon mode, 0 in icon
mode) matching the header pill's own inset, so the rows read as nested UNDER their open tab
rather than merely drawn below it -- the "subtle indent or grouping" the finding asked for.
The active-row accent bar moved in by the same indent so it stays attached to the icon/label it
flags instead of dangling in the new margin.

**2. Headers read as dividers, not tabs.** I2's header was a full-bleed `rc.x0..rc.x1`
rectangle with square corners, flush against the rail edges and (nearly) flush against its
neighbours -- SPEC's general "control corner radius: 0px" hard rule, correctly followed, but a
tab needs SOME visual separation from the surface it sits on to read as a button rather than a
rule. Fixed: every header (non-icon mode only -- the 60px icon rail has no edge to spare, and
its group buttons already read as buttons via IconForRailGroup()'s own box) is now inset
`tok::kS` off both the left and right rail edges, and drawn with a small `2px` corner radius --
treated as the guide's own **chip/badge** allowance (*"a few chip/badge elements (1-2px)"*)
rather than a violation of the flat-control rule, since a rail-group header is closer kin to a
badge than to a slider or a switch. `railmetrics::kHeaderGap` doubled `4 -> 8` (`tok::kXS` ->
`tok::kS`) for a visible gap between adjacent pills; there was no height budget to weigh this
against -- the accordion only ever draws ONE group's rows, so even the busiest real group
(MISC, 6-7 areas) lands at roughly a third of the rail's available height at 1080p/scale 1 (see
the "busiest group still fits" test), nowhere near the ceiling this constant is checked against.
Both the fill and the interactive `InvisibleButton` use the SAME inset rect -- a real tab's
click area is its own visible bounds, not a wider hit target behind a narrower pill.

**3. Icon-rail group glyphs were guesswork.** `IconForRailGroup()`'s I2 table redrawn:
**Misc**'s three same-size dots (the standard overflow-menu glyph almost everywhere else, the
wrong association for a group that opens INLINE rather than into a hidden menu) became a
six-ray asterisk/sparkle (three lines crossing through one centre point -- no other glyph in
either icon table draws crossing diameters). **Settings**'s ring-with-four-detached-ticks (read
as "a dim target," and sat close to `system.crosshair`'s own AREA icon -- a big ring with four
lines through it, which CAN appear on screen at the same time as a closed Settings header)
became a hex-nut: a six-sided outline with a round hole at its centre, the bolt-head/mechanism
read "settings" already carries elsewhere, two shapes total (one `Loop`, one `Circle`) and no
other glyph anywhere draws a hexagon. **Other**'s two-compartment archive box (read as "a card,"
too close to `setup.profiles`' own two-offset-cards glyph) became a folder: a small tab rect
sitting on a larger body rect, the plain filesystem "everything else" mark. **Display**'s
monitor-on-a-stand is unchanged -- QC did not flag it. Every closed group's icon box also grew
a small drawn corner chevron (`▸`, `▾` once open -- `glyph::Chevron` at a sixth of the box's
size, bottom-right corner) that no AREA icon draws, so "this is a GROUP button" is legible from
the glyph's own box before the viewer has matched the silhouette to a name at all; hovering a
group's icon button now shows `ImGui::SetTooltip( RailGroupName( eGroup ) )` -- the same
tooltip mechanism the guide's own Tooltips styling (`Widgets.cpp`) already sets up for
`ImGui::SetTooltip()` generally, and this is that mechanism's first caller in the icon rail (no
AREA icon had a hover tooltip before this, or has one now -- only the four group buttons do,
since only they lose their label entirely in icon mode).

**4. Sheet clipped/no scrollbar at the c2 (1100x900) capture size.** Investigated and fixed --
see the dedicated section immediately below, since the root cause and the fix are not rail code
at all and stand on their own.

**Verification.** Recaptured all four I2 views plus a pointer-hover shot of a closed header in
each rail width (`overlay_e2_pointer move <x> <y>`, then a screenshot) under
`build-release/verify-shots/rail-polish-2026-09-27/`: open vs. closed now reads unmistakably at
a glance (filled+outlined accent pill vs. flat quiet tint), the pills read as inset, rounded,
gapped buttons rather than a continuous banded divider, the icon-rail tooltip fires correctly on
hover ("SETTINGS" over the hex-nut glyph), and the sheet-scroll fix is visible directly in the
`g`/`i2` pair (identical view, before/after a wheel scroll, now showing the previously-clipped
"Lag spike detection"/"Outline size" rows). `575` test cases / `15,953,226` assertions still
pass (`tests/gamescope_tests`) -- none of I3's own tests changed shape, since the polish is
presentation-only over the SAME `AccordionOnHeaderClicked`/`VisibleRailAreas`/
`RailContentHeightPx` logic I2 already pinned.

### Rail accordion: icons, livelier styling, animated expand, even spacing (2026-09-27, I5)

I3's own polish pass made open vs. closed unmistakable, but the user came back with a fuller
list. `Why:` the user, verbatim: *"the categories look a bit 'dull' compared to the individual
modules inside of them. Plus, there is no animation, when opening them, which looks a bit sad.
Also, the spacing towards the last shown module and the next category is zero, but it should get
half of the spacing thats on the top (top decreases, bottom increases). Add icons for the
individual categories and style them a little nicer."* Five things, all in `Shell.cpp`'s
`DrawRail()` plus `Registry.h`/`.cpp`'s pure half:

**1. Icons in the full-width rail.** A header now draws its group's own glyph -- the SAME
`IconForRailGroup()` table the icon-collapsed rail already used (I2) -- between the chevron and
the label, at `Px(18.0f)` (a notch under an area row's own `tok::kIconBox` = 24, since `kHeaderH`
is only 26px and a full-size glyph would leave almost no margin). Tinted the same rule an area
row's own icon already follows: `Role::AccentIcon` on the OPEN header, the header's own text
colour (see below) at rest otherwise -- one state job, not a second convention.

**2. Livelier styling.** Three changes, all through existing tokens/roles, nothing hard-coded:
- **Label colour**: `Role::TextLabel` (68%) in place of `Role::TextMeta` (52%) for a closed
  header's icon and text -- SPEC §7.1's own contrast table names `TextMeta` for "units, marks,
  chips, placeholders", not a first-class control label; a closed header sitting at that alpha
  next to an area row's own 68% label is a real part of what read as "dull".
- **A resting outline.** A closed header now draws a quiet `Role::Line` (10% white) hairline
  around its own pill at rest, not only once hovered or open -- so it reads as its own bounded
  shape (a real tab) from the first frame, rather than only gaining an edge once touched.
- **Hover and pressed states.** The open pill's accent alpha now steps `0.16 -> 0.20 (hover) ->
  0.26 (pressed, IsItemActive())` instead of sitting static -- even the OPEN tab now answers the
  pointer. A closed header keeps the existing white-13 hover wash (the SAME literal every area
  row's own hover already uses, reused rather than re-invented) and gains a matching
  `Accent(0.22f)` press fill, a preview of what release turns it into.

**3. Expand/collapse animation.** Reuses Tokens.h's existing "one easing, three durations" motion
system verbatim -- `tok::kDurRegion` (160ms, SPEC §8.4 already names this duration for "rail
collapse") and the `Approach()`/`Ease()` pair `s_flRailAnim` already drives the rail's own WIDTH
with -- rather than inventing a new duration or easing. One ABSOLUTE pixel height
(`s_flRowsBlockAnim`, not a 0..1 fraction: switching groups mid-transition has to ease
continuously from whatever height was already on screen, and a fraction re-based against a
DIFFERENT group's full height would jump) `Approach()`es whichever group's rows are meant to be
showing, every frame. `s_eDisplayedRowsGroup` can lag `s_eOpenRailGroup`: closing a group must not
blank its rows the instant the click lands, so the DISPLAYED group stays put and shrinks to
nothing before the state clears; opening a DIFFERENT group swaps the displayed rows immediately
and only the height keeps easing, so the same physical "slot" both shrinks and grows in one
motion. Per-row consequence: `Registry.h`'s `RowVisibleHeightPx()` (pure, unit-tested) turns that
one animated height into "how much of THIS row, at this position in its group, is on screen right
now" -- 0 (skipped entirely), a partial clip (drawn, but cut off, and NOT interactive -- "hit-
testing only for fully visible rows", the simpler of the two options the brief offered, since
`VisibleRailAreas()`/keyboard nav never see a mid-animation frame at all), or the full row.
Repaint: gamescope does not free-run (`SettingsOverlay.cpp`'s own Issue #100 comment spells out
why), so `DrawRail()` now calls `force_repaint()` whenever the animated height has not yet landed
on its target, the identical idiom `UpdateFadeAlpha()` already uses for the overlay's own open/
close fade -- self-terminating the moment the snap-to-target threshold lands, never re-arming
after. Selection-follow scrolling (the rail's own wheel/keyboard-follow) reads a SEPARATE, REST-
state pass of the same `Walk()` (every row of `s_eOpenRailGroup` at its full, unanimated height) --
so it never jitters against the animation in flight, and `RailContentHeightPx()` (the "does the
busiest group fit" test) stays exactly the rest-state number it always was.

**4. Spacing.** `Why:` the user's own words above. `railmetrics::kHeaderGapOpen` (`kHeaderGap *
0.5`) replaces the OLD behaviour -- the full gap sat entirely above an open group's first row,
and nothing at all sat between its last row and the next header -- with half above and half
below: the open header's own row-block advance uses `kHeaderGapOpen` instead of `kHeaderGap`, and
a NEW `kHeaderGapOpen` gap is inserted after an open group's own rows, before the next header (no
trailing gap when the open group is the LAST one in the table -- there is no next header to space
it from). A closed-to-closed header gap is untouched (`kHeaderGap`, I3's own "visible gap between
adjacent pills" fix) -- only the OPEN group's own two edges move. `Registry.cpp`'s
`RailContentHeightPx()` mirrors the identical arithmetic, so the "top decreases, bottom increases,
total stays the same" property is pinned by a unit test (`tests/test_overlay_ui.cpp`) working out
the exact pixel heights both ways, not merely eyeballed off a screenshot.

**5. The icon-only rail.** Unaffected in structure (I2's icon-mode header/row sizing, and 184a04a's
badge/tooltip fixes, are untouched code paths) but gets the SAME `Walk()`-level animation and
spacing logic passed through it (icon mode's own zero-gap header advance is preserved -- the
`bIcons` guard on every new gap constant above).

**Verification.** A private headless sway (`WLR_BACKENDS=headless`) plus a nested `--backend
wayland` gamescope, this task's own harness modelled on `scripts/pixel-regression.sh`, captured
under `build-release/verify-shots/rail-style-2026-09-27/`: DISPLAY open and MISC open at
1920x1080 (both show the new icons, the brighter closed-header text, the visible top/bottom
spacing split around the open group), a closed-header hover (the DISPLAY pill visibly lighter
than the untouched MISC pill beside it, zoomed crop confirms it), and the 1100x900 icon rail with
MISC open. A mid-animation capture was attempted (switch group, screenshot with no settle delay,
relying on the async screenshot pipeline's own ~50-150ms latency against the 160ms duration) but
landed on the settled frame both times tried -- the brief's own "otherwise skip it and say so"
clause; no debug convar was added to slow the animation down for a guaranteed catch, since that
would be scaffolding kept for one verification pass. `577` test cases / `15,953,241` assertions
pass (`tests/gamescope_tests`), including two new ones this pass added: a pure `RowVisibleHeightPx()`
census (clip/gate arithmetic at several block heights) and an exact-pixel pin of the spacing split
(the "top decreases, bottom increases, total unchanged" claim, computed both ways and checked
against each other).

### Rail accordion: headers read first-class (2026-09-27, I6, post-QC round 3)

I5 shipped icons, an animation and a spacing split, and the user still did not get what they
asked for. `Why:` the user, verbatim, unchanged since I5 and still the brief: *"the categories
look a bit 'dull' compared to the individual modules inside of them. Plus, there is no animation,
when opening them, which looks a bit sad. Also, the spacing towards the last shown module and the
next category is zero, but it should get half of the spacing thats on the top (top decreases,
bottom increases). Add icons for the individual categories and style them a little nicer."* A
vision QC of I5's own captures put the residue plainly: *"the two shots are nearly
indistinguishable at header level"*, the hover state could *"only just"* be told apart, the header
and module icon/label columns were 3-7px out of step, the icon rail's corner chevron badge *"reads
as a clipped artifact"*, and the spacing split measured 4px against 5px — a change *"barely
visible"*.

The animation and the split's *arithmetic* were fine and are untouched. Everything else was a
contrast problem, and every fix below is a change of **which existing role/token is named**, not a
new colour:

**1. The header is now the brightest, heaviest thing in the rail.** Three changes, one direction:
- **Text**: `Role::TextPrimary` (92%) for every header, open or closed — I5 drew closed headers at
  `Role::TextLabel` (68%), the SAME tier as the module labels beneath them, which is a hierarchy
  with no hierarchy in it. 92% sits one tier above even an *active* module row's own label.
- **Type**: `TypeRole::Title` (Mono 600 14.5 UPPER) in place of `TypeRole::Section` (Mono 500
  13.5) — heavier *and* larger, the role SPEC already assigns to "slab title, region titles".
- **Height**: `railmetrics::kHeaderH` 26 → 30, which is what buys the Title size its margin and
  lets the header carry the module rows' own 24px icon box (see 3). The "busiest group still fits
  at 1080p" test pins the cost: MISC's 7 areas land at ~472px against 878px available. (I8,
  2026-09-27, raised this again to `= kItemH` (40) — 30 fit the icon box but still read shorter
  than the rows themselves; see that section below.)

**2. Fills and states that survive a glance.** Every rung of the state ladder used to differ from
the last by a few percent of **white**, which on a near-black rail is invisible without a colour
picker. Each rung now differs by an accent **tint** (a hue change) or by an outline **role**:

| state | fill | ring |
|---|---|---|
| closed, rest | `Role::SurfaceRaised` | `Role::LineControl` |
| closed, hover | + `Accent(0.14)` | `Role::AccentBase` |
| closed, pressed | + `Accent(0.26)` | `Role::AccentBase` |
| open | `Accent(0.22 / 0.28 / 0.34)` | `Role::AccentBase` + a 3px accent tab bar |

`Role::LineControl` (white 42%) is SPEC §7.1's own role for "EVERY interactive boundary"; I5's
resting ring used `Role::Line` (white 10%, "row separators ... decorative") — the role for a thing
you are *not* meant to click — and that alone is most of why a closed pill dissolved into the
rail. The open tab's 3px accent bar is the SAME `SPEC §8.1` accent state edge an active area row
already draws, at the SAME x (the pill's left inset equals the rows' own group indent), so the
open category and the selected module inside it read as one continuous accent column.

**3. One icon column, one label column.** `Why:` QC's finding 3 — header icons at x≈177/text
x≈197 against module icons at x≈180/text x≈204. Two causes, both removed: I5's header glyph was
18px against the rows' 24px `tok::kIconBox` (same box origin, different centres and label
offsets), and the chevron sat *before* the icon, eating the left pad. The header now runs the
identical arithmetic the item lambda does — `x0 = left edge + flPadX`, a `tok::kIconBox`-wide box,
label at `x0 + tok::kIconBox + tok::kM` — and, since the header pill's inset (`tok::kS`) equals
the rows' own indent (`tok::kS`), the two columns coincide exactly rather than nearly. The
chevron moved to the pill's right edge. Deliberately *not* an intentional offset: a category and
its modules reading off one left edge is what makes the rail a list rather than a stack of bands.

**4. The icon rail's category cells.** The corner chevron badge is **gone**, not re-inset a third
time. `Why:` a 6px glyph parked in the corner of a 60px cell has no inset at which it looks
deliberate — at that size the mark carries no shape, it is three lit pixels near an edge, which is
exactly what QC kept reading as a clipping artifact. Its job (*"this button opens a group"*) is now
the **cell's**: a category cell is a boxed pill (a `tok::kXS` inset all round) running the exact
same state ladder as the full-width header, while an area row stays a bare, unboxed, full-size
glyph on the plain rail. **Boxed = category, bare = module**, legible before a single glyph has
been identified. The group glyph is drawn at 20px (a notch under the rows' 24) so the pill stays
visible around it — the pill, not the glyph, carries the weight. The cell's *hit* rect stays the
full 60px cell, deliberately breaking I3's "a real tab's click area is its own visible bounds"
rule: in a 60px column that rule would spend 8 of 60px of target width on a margin that draws
nothing, and there is no neighbouring control a wider target could steal a click from.

**5. Spacing, perceptibly.** The split's arithmetic (half above the open group's first row, half
below its last) was right; the BASE was too small for it to show — half of 8 is 4, and 4px reads
as no gap at all. `railmetrics::kHeaderGap` 8 → 12 (`tok::kM`) makes `kHeaderGapOpen` 6. Measured
against the rail the user complained about: the header-to-first-row gap goes 8 → **6** ("top
decreases"), the last-row-to-next-header gap goes 0 → **6** ("bottom increases"), and the
closed-to-closed gap goes 8 → 12, which is itself part of finding 1 — four pills with 12px of
rail between them read as four tabs; with 8px they read as one banded column. `Registry.cpp`'s
`RailContentHeightPx()` reads the same constants, so the unit test that pins the exact pixel
arithmetic needed no numeric edit.

**Verification.** A private headless sway (`WLR_BACKENDS=headless`) plus a nested `--backend
wayland` gamescope, captured under `build-release/verify-shots/rail-style-v3-2026-09-27/`. The
claims above are *measured* off those captures, not eyeballed:
- **Columns** (finding 3), `b-misc-open.png` at 1920x1080: the DISPLAY header's icon ink spans
  x 172..187 and the HUD/Mixer rows' icon ink spans x 171..188 — the same centre, 179.5, the
  1px each side being the monitor glyph simply being narrower than the bar-chart one. First
  label ink is at **x 205 for the header and x 205 for both module rows**. QC measured the
  predecessor at 177/197 against 180/204.
- **Spacing** (finding 5), same capture: header pills are 30px tall (129..158, 171..200,
  453..482, 495..524). Closed-to-closed gap **12px** (158 → 171), open header to first row
  **6px**, last row to next header **6px** (446 → 453 measured at 7 with the outline). The
  predecessor measured 26px pills, a 9px closed gap and 4px/4px.
- **States** (finding 2): `crops/c-states-rest-hover-pressed-4x.png` puts rest, hover and
  pressed side by side at 4x — grey ring / accent ring + faint accent fill / accent ring +
  strong accent fill. No colour picker needed.
- **Icon rail** (finding 4): `crops/d-iconrail-before-after-4x.png`. The predecessor's corner
  chevrons are visible sitting on the cell borders; the new column has four outlined pills and
  six bare glyphs and no badge at all.
- 577 test cases / 15,953,243 assertions pass, including this pass's two new pins
  (`kHeaderGapOpen >= 5.0f` and `kHeaderGapOpen < kHeaderGap`) and the unchanged
  "busiest group still fits at 1080p" test.

### Rail regroup, animation fix, full-height indicator (2026-09-27, I7)

Three independent fixes to the rail accordion, all in `Shell.cpp`'s `DrawRail()` plus
`Registry.h`/`.cpp` and `Icons.cpp`, landed together.

**1. The animation only worked in one direction.** `Why:` the user, verbatim: *"When i open a
category above the currently open one, it animates nicely, but when i open one below, it
doesnt. Fix that."*

Root cause: I5's design (see its own section above) shared ONE animated height across
whichever group's rows were "currently displayed" (`s_flRowsBlockAnim`/
`s_eDisplayedRowsGroup`), reused as a single physical slot that both shrank (the closing
group) and grew (the opening group) in one motion, always relocating to sit under the
NEWLY-opened header. The group that had just closed vanished from `DrawRail()`'s own y-walk
**instantly** on the very next frame — there was no second height left to animate it with.
Whether that instant collapse was visible depended on table order: a group **below** the
newly-opened one collapsing is off-screen from where the eye is looking (nothing between the
top of the rail and the just-opened header changed), so "opening above" read as a clean
reveal. A group **above** the newly-opened one collapsing shifts every header after it —
including the one just clicked — up by the old group's full height in the very same frame the
click lands, so "opening below" read as the clicked header teleporting before anything had a
chance to animate.

Fix: `Registry.h`'s new `RailAccordionAnim` struct tracks an **opening** group (always eases
0 → its own rest height) and, separately, a **closing** group (whatever was open a moment
ago, eases its own last height → 0) **concurrently**. `StepRailAccordionAnim()` (pure,
unit-tested, `Registry.cpp`) is the whole state machine: on a change of which group is
commanded open, the group that WAS open takes the closing slot at whatever height it had
already reached (so clicking a header shut before it finishes opening reverses smoothly
rather than jumping to full height first), and the newly-commanded group always starts its
own grow from 0. Neither height's arithmetic knows or cares where the other sits in the
table — `DrawRail()`'s `Walk()` lambda now takes both an opening `(group, height)` pair and a
closing `(group, height)` pair, and for every group it visits top-down it just asks "is this
the opening one, the closing one, or neither", so a closing group above the opening one
shrinks in place exactly like a closing group below it does. Both independently push or pull
every header after them by their own currently-animated height. `RowVisibleHeightPx()` (the
per-row clip/reveal arithmetic) is unchanged and reused for both roles.

Known tradeoff (documented in `RailAccordionAnim`'s own comment): only one closing slot
exists. A third header click landing while an earlier switch's closing group has not yet
reached 0 (two switches inside one ~160ms window) makes the still-shrinking group snap shut
rather than finish easing — not a new limitation, the old single-slot design already
re-targeted its one slot on rapid re-clicks; it is just now visible on the closing side too.
Requires two DIFFERENT headers clicked inside one animation's duration to see at all.

**2. The open header's accent bar was cut short.** `Why:` the user, verbatim: *"when a
category is expanded, the active indicator on the left (colored vertical line) doesnt span
the full width of the category box, fix that"* — by "width" they mean the bar's own long
axis (vertical), i.e. the pill's full height top to bottom, not its horizontal extent.

I6's bar was inset by `tok::kXS` at both the top and bottom "so it never fights the pill's
rounded corners" — at an 8px inset on a 30px-tall pill, that is a third of the bar simply
missing, which is exactly what read as "cut short". Fixed properly instead of just deleting
the inset (a flush 90° corner sitting inside a ROUNDED pill corner would poke a small square
nub past the pill's own curve): the bar now runs the pill's full `y0..y1`, with its own left
two corners rounded at the SAME radius (`flHdrRound`) the pill itself uses via a new `eFlags`
parameter on `Shell.cpp`'s `Fill()` helper (`ImDrawFlags_RoundCornersLeft`) — so it traces the
pill's own rounding exactly rather than stopping short of it or poking past it. Verified by
sampling pixel columns off a real capture: the bar's colour is present at the pill's very
first and very last row, softened only by the shared corner's own anti-aliasing curve, not by
a hard 8px gap (`build-release/verify-shots/rail-regroup-2026-09-27/crops/
d-accent-bar-full-height-4x.png`).

**3. The regroup.** Six groups now, DISPLAY / OVERLAY / INPUT / MISC / SETTINGS / OTHER --
see "Rail groups" above for the full membership and the user's own words. `RailGroup` gained
`Overlay` and `Input` (kept as `Nothing`, not `None` — Xlib's own `#define None 0L` still
applies to this enum, same reason noted where it was first added). `IconForRailGroup()`
gained two new freehand glyphs in the same style as the other four: OVERLAY is a large square
frame ("the screen") with a small plain "+" reticle inside it, unconnected to the frame's own
edges — distinct from `system.crosshair`'s own AREA glyph (a ring with arms crossing ITS
edge) and from MISC's asterisk (diagonal lines with no enclosing frame at all). INPUT is a
single wide keycap (one rounded rectangle) with a small filled legend mark low on its face —
distinct from `setup.keybinds`' AREA glyph (three keycaps plus a spacebar, five shapes) and
`system.null_binds`' AREA glyph (two separate keycaps joined by a chevron, four shapes).

A debug-only ConVar, `overlay_e2_rail_anim_scale` (default 1.0, multiplies
`tok::kDurRegion`), was added for this task's own verification — slows the 160ms accordion
animation down enough that a screenshot reliably lands mid-transition instead of racing the
async screenshot pipeline's own latency (I5's own attempt at this landed on the settled frame
both times it tried, with no debug aid). Kept rather than removed: it is a small, clearly
documented, always-1.0-by-default knob, useful for the next person who needs to catch this
animation on camera again.

**Verification.** A private headless sway plus a nested `--backend wayland` gamescope
(`build-release/verify-rail-regroup.sh`, this task's own script, modelled on
`scripts/pixel-regression.sh`'s recipe), captured under
`build-release/verify-shots/rail-regroup-2026-09-27/`: DISPLAY open (`a-display-open.png`,
6 rows: General/Resolution/Upscaling/Frame limiter/HDR/Shaders), OVERLAY open
(`b-overlay-open.png`, 4 rows: HUD/Crosshair/Zoom/Cursor), INPUT open (`c-input-open.png`,
Autoclicker/Null binds — `input.general` not yet registered, confirmed NOT to break the rail),
the 4x accent-bar crop above, and — at `overlay_e2_rail_anim_scale 12` — genuine
mid-transition frames in BOTH directions: `e-anim-opening-below-mid.png` (Display → Overlay,
the direction that was broken) shows DISPLAY's own header already closed-styled while its
General/Resolution rows are STILL ON SCREEN (mid-shrink, not yet vanished) at the same moment
OVERLAY is showing only HUD/Crosshair/Zoom — Cursor not yet revealed (mid-grow) — the two
animating concurrently, proving the fix; `g-anim-opening-above-mid.png` (Other → Display, the
direction that already worked) shows the symmetric case: DISPLAY (opening, at the top of the
table) mid-grow at 4 of 6 rows while OTHER (closing, at the bottom) still shows its own Log
row mid-shrink. `e2`/`g2` show both pairs fully settled. 579 test cases / 15,953,365
assertions pass (`tests/gamescope_tests`), including two new pure state-machine tests for
`StepRailAccordionAnim()` and all 118 `[overlay_ui]` cases; one run this task also saw 13
`test_steam_friends.cpp` failures on an unrelated subprocess-capture race (that file is
untouched by this diff, and a clean re-run passed all 579).

### Rail category headers as tall as the module rows (2026-09-27, I8)

`Why:` the user, verbatim: *"The individual categories seem kind of small, tallness-wise,
compared to the actual tabs below them, which looks kind of off."*

I6 raised `railmetrics::kHeaderH` 26 → 30, sized to fit the module rows' own 24px
`tok::kIconBox` glyph with a little margin — not to *match* `kItemH` (40), the height of the
row underneath it. That closed the icon/label alignment gap QC had measured (finding 3) but
left the pill itself visibly shorter than the rows it sits above, which is exactly the
"off" the user is describing here: not a misalignment, a height mismatch between the tab and
the page under it.

Fix: `kHeaderH` is now `= kItemH` (Registry.h), a derived constant rather than a second
literal, so header and row height cannot drift apart a third time the way 26 → 30 → ? already
did once. Nothing else needed a coordinate change — every draw inside the header pill in
`Shell.cpp`'s `DrawRail()` (the icon, the chevron, the open pill's own left accent bar) is
already positioned off `rcHdr`'s own `y0`/`y1` or its vertical centre, and `Label()`/
`DrawText()` (`Controls.cpp`) already vertically centres text in whatever rect it is given
(`rcClip.Min.y + ( rcClip.GetHeight() - size.y ) * 0.5f`). Raising one constant re-centres the
whole pill with it. `TypeRole::Title` (Mono 600 14.5) is kept, not stepped up to `Label`
(Sans 400 16, what the rows themselves use): a module row's own 40px `rcItem` already carries
its (smaller) Label text with generous headroom above and below, so a header pill with the
same kind of headroom around its (smaller, by design — I6's own deliberate header/row type
distinction) Title text matches the rail's existing rhythm rather than reading as empty.
Stepping the role up would also blur that bold-mono-uppercase-vs-plain-sans distinction for
no gain on the actual complaint, which was tallness, not glyph size.

The icon-only rail (60px, forced below ~1100px width or similar) needed no change: its own
header cell already used `flItemH` directly (`const float flHdrH = bIcons ? flItemH :
flSecH;`, `Shell.cpp`), never `kHeaderH` — so a category cell there was already exactly as
tall as a module icon row, the visible pill inside it inset by `tok::kXS` on every side
either way.

**Fit budget.** The "busiest group still fits at 1080p" test's own arithmetic changes with
the constant (it reads `railmetrics::kHeaderH` directly, so no test file needed a numeric
edit) — DISPLAY (now the busiest group at 6 areas, since I7's regroup) opened lands at
`kPad + (kHeaderH+kHeaderGapOpen) + 6*kItemH + kHeaderGapOpen + 5*(kHeaderH+kHeaderGap) + kPad`
= 8 + 46 + 240 + 6 + 260 + 8 = **568px** against 878px available at 1080p/scale 1 — well
inside budget even with every header now 10px taller than before.

**Verification.** A private headless sway (`WLR_BACKENDS=headless`) plus a nested `--backend
wayland` gamescope (`build-release/verify-rail-tall-headers.sh`, this task's own script,
modelled on `verify-rail-regroup.sh`'s recipe — two full sway+gamescope sessions rather than
one, since sway TILES a single toplevel to fill its own output regardless of the client's
`-W`/`-H` request, so forcing the icon rail needs the *output* itself at ~1100 wide, not just
the gamescope window), captured under
`build-release/verify-shots/rail-tall-headers-2026-09-27/`:
- **`a-overlay-open-1920x1080.png`**: OVERLAY open. Pixel-sampled (not eyeballed): the open
  header pill's own accent outline runs from screen row **y=181 to y=220** (40 rows
  inclusive, both edges the pill's own 1px ring), and the HUD row's active-wash directly below
  it runs **y=227 to y=266** (also exactly 40 rows) — the pill and the row it owns measure
  **identically 40px**, matching `kHeaderH == kItemH == 40` to the pixel. The 6-7px gap between
  them (220 → 227) is `kHeaderGapOpen` (6, at this capture's ~1.0 scale).
- **`crops/b-header-vs-row-3x.png`**: a 3x nearest-neighbour crop of the OVERLAY pill directly
  over the HUD row, at the same left edge — same height, icon columns and label columns both
  line up exactly, no stagger.
- **`c-icon-rail-1100x900.png`**: the icon-only rail at 1100x900. DISPLAY (closed) and OVERLAY
  (open, boxed pill with its own accent outline) sit above the bare HUD/Crosshair/Zoom icon
  rows — boxed-vs-bare hierarchy intact, same cell height throughout (icon mode was already
  correct, `flHdrH = bIcons ? flItemH : flSecH`, untouched by this fix).
- 585 test cases / 15,953,511 assertions pass (`tests/gamescope_tests`) — no test pinned a
  literal `kHeaderH` value, so none needed editing; the arithmetic tests that reference
  `railmetrics::kHeaderH` by name picked the new constant up automatically.

### Sheet body: the mouse wheel now scrolls it (2026-09-27, I3, found via rail-polish finding 4)

Pre-existing, unrelated to the accordion (confirmed via `git log` -- the "make the sheet
actually scroll" commit and D26's `ScrollView` mechanism both predate the accordion by several
commits, and neither the accordion nor this fix touches the other's code) but found while
investigating this task's own finding 4 (the c2-size capture showing the Sheet's "Lag spike
detection" row clipped by the footer bar, with no reachable scrollbar). D26 (see the section
below) made the Sheet's ROWS follow a scroll offset once ImGui applies one, via the same
`ScrollView` the Inspector body uses -- but nothing ever made ImGui actually apply one here from
a mouse wheel, so a Sheet taller than its region was reachable by NOTHING: no wheel, no drag (no
scrollbar is ever drawn over the Sheet), permanently clipped.

**Confirmed live**, not just read off the code: a private headless sway + nested gamescope
instance (this task's own harness, modelled on `scripts/pixel-regression.sh`), `system.hud`
selected at a forced 1100x500 output (severe, unmissable overflow), a temporary debug log inside
`DrawSheetBody()` showing `wheel=30.00 hovered=1 scrollY=0.00 scrollMaxY=421.00` on the SAME
frame `BeginChild` ran -- the wheel event correctly reaches `io.MouseWheel` as nonzero, the
child IS the hovered window, `ScrollMaxY` is genuinely nonzero (a real overflow, correctly
measured) -- and `GetScrollY()` never leaves `0.00`. Dear ImGui's own automatic "apply the wheel
to the hovered scrollable window" pass (`UpdateMouseWheel()`, called once from `NewFrame()`)
does not move this child's scroll through this overlay's queued/drained input path. **The
diagnosis stops there** -- why that native pass does not fire is unproven, and the Inspector
body (D26's other half, a few hundred lines down in the same file) has the IDENTICAL structure
and the IDENTICAL reliance on that same native pass, with only `cv_overlay_e2_scroll` (a debug
convar, *"so the Inspector's scrolling is verifiable without pointer input"*) as a working
bypass -- so this is not proven to be Sheet-specific, and a next agent chasing the Inspector's
own wheel behaviour should start from that convar's own existence as a clue, not assume it was
only ever a testability convenience.

**The fix does not depend on the diagnosis.** `DrawSheetBody()` now applies the wheel BY HAND,
the same way `DrawRail()` already does for its own (never-a-`BeginChild`, always hand-rolled)
scroll: `ImGui::SetScrollY( ImGui::GetScrollY() - ImGui::GetIO().MouseWheel * Px( tok::kRowH )
* 3.0f )` while the child is hovered and the wheel is nonzero -- three 44px rows per notch, a
plain desktop-scroll amount. `SetScrollY()` clamps to `[0, ScrollMax]` internally, so this
cannot send the view negative or past the bottom even on the child's first frame, before its own
`ScrollMax` has settled. Verified by the same live instance: the identical wheel command that
previously left the view frozen now reveals the previously-clipped rows (`build-release/
verify-shots/rail-polish-2026-09-27/g-sheet-after-scroll.png` and `i2-forced-overflow-after-
scrolldown.png`).

**Not fixed, and not a bug:** the Placement row's `24 / 24` secondary text degrading to `...`
at the c2 width is `Controls.cpp`'s own D27 ellipsis mechanism (see this guide's "Component
styling" -- *"a string overflowing... reads as 'there is more' instead of stopping mid-word"*)
doing exactly its documented job at a genuinely narrow lane. The QC finding's own wording named
it alongside the clipping bug, but it is unrelated and working as designed.

**Follow-up (2026-09-27, same day): the Inspector body got the identical fix.** This section's
own "start from that convar's own existence as a clue" pointer above turned out right --
`cv_overlay_e2_scroll` had been the Inspector body's *only* working scroll path (no wheel, no
drag, same as the Sheet before this fix) because `##inspbody` has the exact same
`BeginChild()`-relies-on-ImGui's-native-wheel-pass structure `##sheetrows` did. The hand-applied
wheel check above is now `ApplyChildWheelScroll()`, a small shared helper (`Shell.cpp`, right
before `DrawSheetBody()`) both bodies call -- D26 already made the Sheet and the Inspector share
the LAYOUT half of this mechanism (`ScrollView`, `Layout.h`); this is the matching shared INPUT
half, so a third scrolling body gets working wheel scroll by calling the helper rather than by a
third hand-rolled copy. `IsWindowHovered()` is only ever true for whichever ImGui window
currently owns the mouse, so calling the same helper from both bodies' `BeginChild` blocks can
never apply one wheel event to two regions -- the two are mutually exclusive by construction,
not by a flag guarding them.

`Why:` (the native-pass diagnosis, revisited) -- a further **static** read of ImGui's own
`UpdateMouseWheel()`/`FindBestWheelingWindow()` (`subprojects/imgui/imgui.cpp`) narrows, but does
not close, the earlier "unproven" verdict. `FindBestWheelingWindow()` bubbles a wheel event from
`g.HoveredWindow` UP to a parent only when the hovered window itself has `ScrollMax == 0` on
that axis or carries `NoScrollWithMouse` itself -- neither is true here (the Sheet's own log had
`scrollMaxY=421` on the exact frame the native pass still failed to move it, and neither
`##sheetrows` nor `##inspbody` sets that flag), so the walk should select the child directly and
never reach `##e2slab`'s own `NoScrollWithMouse` at all. That rules out the slab's own flag as
the mechanism, contrary to what this section's own earlier phrasing suspected -- but it does not
explain why `SetScrollY()` inside that native path still leaves `GetScrollY()` at `0.00` on a
frame where every precondition the source reads (`hoveredWindow == this child`, nonzero wheel,
nonzero `ScrollMax`) already holds. Confirming the remaining candidate -- whether `g.HoveredWindow`
at the point `NewFrame()`'s `UpdateMouseWheel()` runs is genuinely this exact child pointer, or a
stale one from this overlay's queued/drained (not per-real-input-event) frame timing -- needs a
live instrumented build logging `g.HoveredWindow`'s identity at that exact point, not another
static read; a next agent chasing this should start there rather than re-deriving the
elimination above. The hand-applied helper does not depend on ever answering it.

**Rail corner-chevron badge + group tooltip (2026-09-27, same commit, from a vision QC of I3's
own captures).** Two follow-on polish fixes to the SAME icon-rail group header this section's
neighbour ("Icon rail: `1e`/`1f`...") describes, found by looking at the rendered captures
rather than the code: the corner chevron's fixed `flCorner * 0.7` inset only cleared
`glyph::Chevron`'s SHORT half-extent (its 0.26-of-size axis); whichever axis carried the LONG
half-extent (0.50-of-size) for a given direction -- height for the closed `▸`, width for the
open `▾` -- ran past that inset and read as a clipped speck against the rail's own right edge or
the row divider below it. `Why` an asymmetric glyph needs an asymmetric fix: insetting by the
glyph's own longer half-extent (`flCorner * 0.5`) plus a real margin, instead of one arbitrary
fraction of the corner size applied to both axes, is what clears BOTH chevron directions from
the same one-line call site. Separately, the group tooltip (`ImGui::SetTooltip()`) rendered as
an unpadded 1px box: this section's own earlier claim that "the guide's own Tooltips styling
(`Widgets.cpp`) already sets up `ImGui::SetTooltip()`" was wrong in one specific way --
`Widgets.cpp`'s `ApplyStyle()` does set `ImGuiCol_PopupBg`/`PopupBorderSize` globally, but the
slab (`##e2slab`, `Shell.cpp`) pushes `ImGuiStyleVar_WindowPadding(0,0)` for its own chrome and
does not pop it until after its own `ImGui::End()` -- so every tooltip drawn anywhere inside the
slab, this being the first, inherited that zero padding from the still-open style-var scope.
Fixed locally at the one call site (a real `BeginTooltip()`/`EndTooltip()` pair with its own
scoped `WindowPadding`/`PopupBg`/`Border` push, positioned off the rail's own right edge via
`SetNextWindowPos()` rather than following the mouse) rather than by reworking the slab's
push/pop pairing, which is out of this fix's scope and would need its own audit of every other
caller between that push and its pop.

**Keyboard.** Up/Down in the rail and Ctrl+Left/Right both still call the one `StepArea()`
they always did, walking every area in `RailOrder()`'s order (not just the open group's) --
unchanged from before the accordion. What is new is a single line inside `StepArea()`'s
selection write: landing on a new area now opens ITS group the same way a click does. That is
what lets stepping off the last area of an open group spill into the next group's first area
and open it, rather than stopping dead at a boundary the accordion happens to have collapsed
-- "moving onto another group's area opens that group" falls out of the one shared write, not
out of a second keyboard-specific rule. **Deliberately not built:** keyboard focus ON a
header itself (so Enter/Space could toggle it without a mouse). The existing focus model never
gave a header focus -- only areas ever received it -- and every header is already reachable by
mouse, so adding a second, header-only focus state for one keystroke's worth of convenience
was judged not worth the new state; a future pass can add it if it is missed in practice.

**Pure logic, no ImGui.** Exactly like `RailOrder()`/`RailGroupFor()` before it, the accordion's
actual rule lives in `Registry.h`/`.cpp`, not in `Shell.cpp` where the rail is drawn --
`AccordionOnHeaderClicked()` (the toggle), `VisibleRailAreas()` (which rows a given open group
draws) and `RailContentHeightPx()` (the fits-the-rail arithmetic, reading the same
`railmetrics::` constants `DrawRail()` itself does, so the two can never silently disagree) are
all plain functions over `RailGroup`/`Area*`, callable from `test_overlay_ui.cpp` with no
ImGui context and no live registry. See that file's "rail accordion:" test cases, including one
that builds all 19 real areas and asserts every group's own content height fits the rail at
1080p/scale 1 -- the property this whole feature exists to guarantee.

**Naming note:** the sentinel for "every group closed" is `RailGroup::Nothing`, not `None` --
`None` collides with X11's `#define None 0L` (`Xlib.h`), which a translation unit that
transitively includes this header (`steamcompmgr.cpp`) pulls in; `Registry.h` already avoids
the same trap for `PreviewKind` and `InheritState`, and this follows the same convention.

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

### The frame rule: every rule pixel is drawn exactly once (2026-09-14)

The user's report, verbatim: "there is no separator line in the click GUI between the
options and the inspector rail. Add a separator line and also make sure that on the
spots where it intersects, the line doesn't draw on top of another line because right
now the color is basically doubled since it's transparent. Also, when selecting
something in the left tab selector, the right separator line is colored according to
the backdrop."

**The rule, going forward: every hairline/region-boundary pixel is painted by exactly
one draw call.** `Role::Line`/`Role::LineRegion` are translucent white (10%/22%) *by
design*, so they can sit over any surface colour in the kit -- but that same
translucency means two of them stacked on the same pixel do not "still look like a
line", they composite to a visibly different, brighter tone. The existing
`DrawRail()`/`DrawSheetHead()` boundary (issue #96, pre-dates this fork's own history
in this doc) already followed the discipline of keeping adjacent rules on disjoint
pixel columns/rows rather than double-drawing a corner; 2026-09-14 extended the same
discipline to the sheet/Inspector divider and fixed two ways it was breaking it:

- **The divider was invisible almost everywhere it mattered.** `DrawInspector()`
  pushes one `ImGuiCol_ChildBg` for the whole Inspector column (the docked fill --
  see the transparency section above) and does not pop it until after BOTH the outer
  `"##insp"` child AND the nested, scrollable `"##inspbody"` child have closed. Since
  `PushStyleColor`'s scope is the style stack, not the `BeginChild` call it sat next
  to, `"##inspbody"` silently re-painted the *same* translucent fill a second time
  over its own rect -- which is most of the column's height -- erasing whatever had
  been drawn on the parent's draw list first, divider included. Fixed by pushing a
  transparent `ChildBg` around `"##inspbody"` specifically, so the region gets its one
  intended fill and nothing repaints over it.
- **The divider crossed the mode strip's own bottom rule.** Both `DrawModeStrip()` and
  `DrawContentModeStrip()` draw an `HLine` across the Inspector's full width at the
  strip's bottom edge, in the same `Role::LineRegion` the divider itself uses. A single
  vertical rect run through that row double-composited with it. Fixed by splitting the
  vertical into two segments -- one stopping a Hairline() above the rule, one resuming
  a Hairline() below it -- so the rule supplies that one row's pixel and the two
  segments supply the rest.
- **The rail's own divider took the selected row's accent tint.** `DrawRail()` filled
  each row's accent/hover wash across the row's *full* width, including the column the
  divider already occupied, and did so *after* the divider had been drawn for that
  frame -- so a selected row's `Accent(0.10f)` band painted over the line, and it read
  as "coloured by the backdrop" exactly where a row was selected. Fixed two ways
  together: the row washes now stop one `Hairline()` short of the rail's right edge
  (never touching the divider's column at all), and the divider itself moved to draw
  *after* every row in the walk, so it is the top-most thing at that x regardless.

**Why this approach and not baking every rule as a pre-composited opaque colour:** the
opaque-colour approach (draw the "final blended" tone directly, so a repeat draw is a
harmless no-op) would also fix any future double-draw, but it means a rule's colour has
to be authored per background it can appear over -- rail bg, sheet bg, Inspector bg,
whatever a future region adds -- multiplying one token into several. The
"never-overlapping segments" approach keeps the single translucent token
(`Role::Line`/`Role::LineRegion`) working over any surface, matches the discipline the
rail/sheet boundary already established (issue #96), and is what this pass used. A
future rule that crosses another one only needs to ask the same question this fix did:
which of the two pixel ranges yields, so the shared cell is painted once.

Verified headless (`build-release/verify-shots/shell-separators-2026-09-14/`):
before/after screenshots and pixel samples at every intersection named above, plus the
1280x720 narrow-slab case. `Role::LineRegion` blended once over the Inspector's own
background reads `(56,60,63)`; before the fix, the mode-strip intersection read
`(100,103,105)` (visibly brighter, the doubled composite) and the divider's mid-run
value was `(0,5,9)` (the plain background -- no line drawn at all, the ChildBg-leak
bug). After the fix both read `(56,60,63)`, matching the plain single-draw value
everywhere else on the same row.

**QC round 2 (same day): two more sites found the same way.** `DrawSlabBar()`'s bottom
rule and the command palette's query-divider and footer-divider `HLine`s each ran the
rule's full `rc.x0..rc.x1` straight into the frame's own border stroke (the slab
window's `ImGuiCol_Border` for the bar, the palette's own `AddRect` outline for the
two dividers) -- the identical corner-square defect this section already named, just
at a frame border instead of another in-house rule. Fixed the same way: each `HLine`
now starts at `rc.x0 + Hairline()` and ends at `rc.x1 - Hairline()`, yielding its two
end columns to the border rather than drawing over them. The palette's two dividers
measured a clear before/after change (the footer corners read a doubled `(50,102,115)`
before, the single-draw border value `(27,85,99)` after); the slab-bar corners
measured no change at this window's exact pixel geometry (content already starts one
whole pixel inset from the border column at 1920x1080 centred), so the inset is a
no-op here and a guard against the same defect at a geometry where the two do land on
the same column.

### The "LAUNCH OPTION" tag (2026-09-29)

A small filled badge (`DrawLaunchLockTag()`, `Shell.cpp`), drawn after a row's label
exactly where the inherited/overridden dot above sits, for a different fact: this row's
live value was pinned by a `gamescope` launch flag and will not accept an edit again
this session (`superdoc/features/launch-option-lock.md`). `Why:` the user, verbatim --
*"Make sure that when a value is overwritten, as a startup argument ... that it can't be
modified inside of the game anymore. Just to avoid confusion with the user, just add
like a small red or yellow label behind it that warns the user about it being set
through a launch argument."*

Modelled directly on the inherited/overridden dot: same placement rule (right after the
measured label, clamped to the label lane's own right edge so a long label cannot push
it off-row), same "quiet, small mark" register -- but a short filled **badge** with
text, not a dot, because the point here is to be *read*, not merely noticed: a player
who has never opened this row's Inspector before still has to understand, at a glance,
why it's greyed. `Role::Warn` fill / `Role::WarnText` text -- amber, this kit's "needs
attention, not broken" hue (SPEC §7.5), deliberately not `Role::Danger`: a launch-locked
row isn't in an error state, it's just not editable *here*. When both marks would apply
to the same row (a game profile overriding a key a launch flag then locks live -- rare,
but not unrepresentable), the tag's own x-offset adds the dot's footprint first so
neither draws on top of the other.

**Full opacity on a dimmed row, on purpose.** Every launch-locked row is, by
construction, also a *disabled* row (`Entry::DisabledReason()` returns the launch
wording), so by the time `DrawLaunchLockTag()` runs it is already inside that row's
`ScopedDim(bDisabled)` at 0.55 alpha. A tag whose entire job is explaining *why* the row
is dim would defeat itself by fading into the same grey it's explaining -- so this is
the one deliberate exception to "disabled means 0.55" anywhere in the kit, made with a
new `ui::ScopedUndim` (`Colors.h`/`.cpp`, `Colors.cpp`'s file-static dim factor pushed
back to 1.0 for the tag's own draw and restored after), rather than by special-casing
`Col()`/`Accent()` themselves.

**Two independent disabled slots, on purpose.** `Entry::LockedByLaunchOption()` is a
*second* predicate/reason pair, not folded into the existing `DisabledUnless()` --
several real rows already carry their own gate (Sharpness's "only FSR/NIS sharpen",
the Custom-resolution steppers' "pick Custom above", ...), and `DisabledUnless()`
*overwrites* whatever predicate/reason a row already holds (a single `m_Enabled`/
`m_sReason` slot). Registering the launch lock as a second call rather than combining
it by hand at each of a dozen call sites is what keeps neither clobbering the other;
`Entry::DisabledReason()` simply ORs the two and prefers the launch wording when both
apply, since it names an actual fix ("remove it from the launch options") where a
row's own gate reason usually just explains why the *control* is inert right now.
`Entry::IsLaunchLocked()` is the narrower query `DrawLaunchLockTag()` actually asks --
true only while the row is disabled *specifically* because of a flag, so a row
disabled for an unrelated reason (HDR off, wrong filter, ...) still greys out but
draws no tag.

