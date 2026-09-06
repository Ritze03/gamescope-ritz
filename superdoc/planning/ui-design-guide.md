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
area's rebuild needed a widget for. Two were added directly to `src/Overlay/UI/Controls.{h,cpp}`
(2026-09-06); this is their entry per this doc's own convention — when to use, keyboard
behaviour — the API contract itself lives in Controls.h's comments.

### `ui::controls::ListBox` — a tall, scrollable list of items

**When to use:** a single-select list of named items too numerous or too tall for an
ordinary row — the Profiles area's list of saved profiles is the first and, so far, only
user. Not a substitute for `Choice` (a handful of mutually-exclusive options still belongs
on a segmented control or dropdown) or for `Bank` (a multi-select set of independent
switches) — `ListBox` is for a genuinely *long*, single-selection list.

**Styling:** wire lines only — a 1px hairline frame, 1px row separators, the selected row
outlined in the accent at full strength with **no fill** (this is the one place in the kit
a "selected" state is shown by an outline alone, because the sketch that specified it drew
it that way). An optional muted prefix *tag* (`[Game]`) and an optional right-aligned
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

