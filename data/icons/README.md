# Overlay icon set

SVG glyphs for the gamescope-ritz ImGui settings overlay (dock buttons and
window-chrome buttons). Style rules are derived from
`superdoc/planning/ui-design-guide.md` § Iconography; the icon list matches
`superdoc/planning/SPEC.md` § "UI structure" → "Icon list to author as SVG".

## Conventions

- **`viewBox="0 0 20 20"`**, no `width`/`height` attributes — every icon
  shares this viewBox so the set drops into any container size uniformly.
- **Grid:** glyph geometry sits inside the 20-unit square, generally leaving
  a 3–4 unit margin so nothing touches the edge (matches the guide's
  "16–20px grid inside larger hit areas" rule — dock buttons are 54×54px
  containers, the icon content itself reads at roughly 16–20px inside them).
- **Stroke:** `stroke-width="1.5"`, `stroke="currentColor"`,
  `stroke-linecap="square"` (never `round`), no `stroke-linejoin="round"`.
  Sharp geometric shapes only — no rounded corners anywhere on a glyph,
  including implicit rounding from stroke caps/joins.
- **Corners:** `rx`/`ry` are never used on `<rect>` elements. Windows in the
  wider design get 3–4px rounding; icon glyphs never do.
- **Fill vs. outline:** outline dominates (`fill="none"` on the outer body
  shape), with small **solid** `fill="currentColor"` accents used sparingly
  for emphasis — a hub, a wedge, a filled half-circle, a tick mark — per the
  guide's "outline-first, small solid-fill accents" rule. No icon is fully
  outline-only or fully solid-fill; each mixes the two the way the guide's
  reference glyphs (half-filled brightness circle, speaker cone) do.
- **Color:** every stroke/fill is `currentColor`. No hardcoded colors
  anywhere in these files. The host UI sets color via CSS/ImGui tinting
  (idle ≈ white @ 45–75% opacity, active/open ≈ accent-hi, per the guide) —
  none of that is baked into the SVG.
- **No cruft:** no `<metadata>`, no editor namespaces (`inkscape:`,
  `sodipodi:`), no embedded raster/base64 data, no external references. Each
  file is a bare `<svg>` with plain shape elements.

## The set

| File | Covers | Metaphor |
|---|---|---|
| `settings.svg` | Dock entry point / overlay identity | Gear: outline body ring, solid hub, 8 solid teeth |
| `display.svg` | Gamescope panel (display/scaling) | Monitor + stand, two solid scan-line bars |
| `shaders.svg` | Shaders panel | Half-filled circle (brightness/contrast metaphor from the guide) |
| `performance.svg` | FPS HUD panel | Ascending solid bar chart |
| `audio.svg` | Audio panel | Solid speaker cone + two outline sound-wave arcs |
| `profiles.svg` | Config/Profiles panel | Three outline rows, each with a solid tick dot |
| `reset.svg` | Reset/restore action | Open circular arrow (outline arc + solid arrowhead wedge) |
| `close.svg` | Window chrome | × |
| `collapse.svg` | Window chrome | – |
| `dock-more.svg` | Dock overflow (only needed if the fixed dock button count is ever exceeded) | Three solid dots |
| `checkbox-mark.svg` | Checked/on state, reusable wherever a checkbox glyph is needed as an icon rather than a live ImGui widget | 12×12 outline box + centered 5×5 solid mark, matching the design guide's checkbox spec exactly |

`profiles.svg` covers both the spec's "profiles" and "per-game config" needs
— the UI structure section defines these as one Config/Profiles panel with
one dock icon, not two.

## Adding a new icon in-style

1. Start from the same header: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20" fill="none">`.
2. Build the glyph from rects, circles, lines, and polygon/path wedges —
   simple geometric primitives only, no rounded corners, no bezier
   flourishes.
3. Use `stroke="currentColor" stroke-width="1.5" stroke-linecap="square"`
   for outline strokes; use `fill="currentColor"` (no stroke) for solid
   accent shapes.
4. Keep the design legible with the outer ~3–4 units of the viewBox empty —
   that margin is what keeps the glyph from touching its hit-area edge at
   54×54px dock size.
5. Render it at both 20px and 64px (`rsvg-convert -w 20 -h 20 -b '#12141a' icon.svg -o test.png`,
   likewise at `-w 64 -h 64`) and eyeball both — a shape that's fine at 64px
   can turn to mush at 20px (thin parallel bars merging, a small triangle
   vanishing). If detail disappears at 20px, simplify rather than thin the
   stroke further; the 1.5 stroke weight is fixed across the set.
6. Validate the file is well-formed XML (`xmllint --noout icon.svg`) before
   committing.

## Notes on legibility at UI size (16–20px)

All eleven icons stay readable at 20px. Two needed a second pass to get
there:

- **`display.svg`** — the first draft used three thin scan-line bars packed
  close to the screen's top border; at 20px they visually merged with the
  frame. Reduced to two bars with wider vertical spacing and margin from the
  border; now reads clearly as a monitor with distinct scan-lines.
- **`reset.svg`** — hardest icon in the set to keep legible. The circular
  arrow's arrowhead is the one part of any glyph here built from three
  freehand points rather than a straightforward primitive, and at 20px a
  small arrowhead is the first thing to disappear into anti-aliasing. It
  was enlarged until the notch stays visible at 20px, but it reads mostly
  as "a circle with a gap" at the smallest size — recognizable as a
  reset/refresh symbol on its own, but the directional arrowhead itself is
  subtle until ~64px. If this proves too subtle in the actual overlay,
  the fallback is to drop the arrowhead's subtlety concern entirely and
  render reset as a plain broken-ring glyph — the open-circle shape alone
  already carries the "reset" reading in most icon languages.
