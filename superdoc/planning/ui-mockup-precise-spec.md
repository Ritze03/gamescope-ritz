# UI Mockup — Pixel-Exact Specification

Date: 2026-08-21. Companion to `ui-design-guide.md` (descriptive level); this file is the
number level. Implementation should follow this file; on conflict with the design guide,
this file wins (it was measured, not estimated).

**2026-08-23 — issues #61/#62, slider geometry/spacing and dark-grey text/rail brightness.**
§7's "Slider" entry below is now superseded by a dedicated, separately-measured page:
`planning/slider-widget-spec.md`, measured from a real render of the shipped
`widgets::SliderControl()` (not this file's mockup-derived numbers). It documents #61's
label→track/track→mark gap raise (6px→8px, "a bit more vertical spacing") and #62's
brightened rail/mark-text alpha (`Palette.h`'s new `kRailAlpha`/`kMarkAlpha`/
`kMetaTextAlpha`, 9%→16% / 26%→38% / 34%→44%). Track/handle proportions themselves were
re-verified against a real render and left unchanged. Do not re-derive slider numbers from
this file's §7 below — read the dedicated page instead.

**2026-08-22 — issue #23, deliberate baseline departure from this file's own numbers.**
Every font size in §2's table and every control-geometry constant in §3/§5/§6/§7 (except
the dock, which is explicitly out of scope for #23 and still matches this file exactly)
now ships in `src/Overlay/Fonts.cpp`/`Widgets.cpp`/`Chrome.cpp` **20–25% larger** than the
pixel-measured values recorded below, per an explicit user request to make fonts and
controls "a little bigger" by raising the origin constants themselves, not by layering a
scale multiplier over them. Do not "fix" the code back down to match this table — this
file's own numbers are still correct as *what the mockup measured*, they are simply no
longer what shipped. If this file is ever regenerated from a fresh mockup measurement,
re-apply the same ~20-25% uplift to the new baseline rather than reverting to 1:1.
Additionally, since #24, every one of these constants also scales live with
`OverlaySettings::display_scale` (0.5–2.0×) the same way the dock's own geometry already
scaled with `dock_scale` — so the on-screen size at the default 1.0× display scale is this
20–25%-raised baseline, and at other display-scale values it is that baseline times the
live scale factor, not this table's own numbers at any setting.

**2026-08-22 — issue #53, typeface swapped from IBM Plex to Geist, sizes left unchanged.**
`src/Overlay/Fonts.cpp`'s kSpecs table now bakes Geist Sans/Geist Mono
(`src/Overlay/fonts/Geist-*.ttf`/`GeistMono-*.ttf`, SIL OFL 1.1 — see
`src/Overlay/fonts/LICENSE-OFL.txt`, which is now Geist's own license text, copied
verbatim) instead of IBM Plex, at the *same* pixel sizes #23 already raised above this
file's own numbers — no further re-tuning turned out to be needed. Checked by rendering
both side by side (same `--filter fsr` Upscaling panel, same window geometry, real
screenshots, not eyeballed): Geist and Plex read as visually the same size/weight at
identical pixel values here, unlike the concern this issue was scoped expecting ("Geist
at the same pixel sizes will not look identically sized as Plex did") — that concern
doesn't hold for this specific weight/size combination. The segmented filter row
(`linear`/`nearest`/`fsr`/`nis`/`pixel`) — the row #46's shrink-to-fit logic was tuned
against Plex's advance widths for, and this issue's own highest-risk area — still fits
`nearest` with room to spare at both 1.0× and 2.0×, so #46's constants needed no change
either; the shrink-to-fit logic itself is metric-derived (measures the real font's
advance widths at draw time), so it would have adapted on its own even if Geist's metrics
had differed more. If a future pass finds a role that *does* need re-tuning, change only
that role's line in kSpecs and record the delta and why here, next to this note — do not
revert this note's "no change needed" finding for the roles that were actually checked.

**Provenance and method.** The handoff (`Game Overlay UI Mockups-handoff.zip`) is a Claude
Design bundle whose mockups are literal HTML/CSS — every dimension and color below is read
from that source, not eyeballed. The authoritative artboards are **2a** (final composite,
1920×1080 @ 1:1) and **2b** (the mockup's own build-spec sheet); **1d** supplies the FPS
config window, **1e** the tooltip. The earlier 1a-series uses smaller metrics (30px title
bar, 3px slider track, 5×14 handle, 26×13 toggle) — those are superseded by 2a/2b and must
NOT be used. All oklch values were converted to sRGB with the reference OKLab matrices and
then **verified by rendering the mockup in Chromium and sampling pixels** (e.g. the status
dot sampled `#36bddd` = computed `oklch(.74 .12 218)` exactly; slider handle sampled
`#bae7f4`; dock active icon `#a2e3f6` vs computed `#a3e3f6` — 1/255 anti-aliasing noise).
All numbers are at UI scale 1.0 on a 1920×1080 surface; multiply by UI scale, and keep the
54px dock button ≥44px physical (2b §Scaling).

**Cross-check of exported tokens vs rendered mockup:** the 2b sheet and the 2a render agree
on every token (same CSS source feeds both). The only internal disagreements in the handoff
are 2a-vs-1a metric drift (listed above; 2a wins per the 2b sheet's own numbers) and one 2b
prose item — it says "slider … handle 8×18, drag or scroll" while its own demo swatch omits
the handle glow; the 2a windows all draw the glow, so the glow stands.

---

## 1. Color tokens (canonical)

Accent family, converted from oklch (all verified by pixel sampling):

| Token | Source (oklch) | sRGB hex | Used for |
|---|---|---|---|
| `accent` | `.74 .12 218` | **`#36BDDD`** | status dot, slider fill (solid end), active borders, dock top edge base |
| `accent-edge` | `.8 .12 218` | `#4FD0F1` | dock active 2px top edge |
| `accent-grad-lo` | `.74 .12 218 @ 50%` | `#36BDDD` @ 50% | slider fill gradient start (left end) |
| `accent-grad-hi` | `.78 .12 218` | `#47CAEA` | slider fill gradient end (right/handle end) |
| `accent-value` | `.84 .1 218` | `#78DBF6` | numeric value readouts |
| `accent-value-big`| `.84 .11 218` | `#6CDCFA` | large "after" number in 118→236 pairs |
| `accent-text` | `.82 .1 218` | `#71D4EF` | chip text (AUTO, HOOKS…), link-style labels, "−6.2 dB" |
| `accent-seg-text` | `.9 .07 218` | `#A9EAFD` | active segmented-control label |
| `accent-knob` | `.86 .08 218` | `#93DEF4` | toggle knob, checkbox inner mark |
| `accent-icon` | `.88 .07 218` | `#A3E3F6` | active dock icon strokes |
| `accent-handle` | `.9 .05 218` | `#BAE7F4` | slider handle fill |
| `accent-link-dim` | `.8 .1 218` | `#6BCDE9` | footer action text ("reset", "save as preset", "live") |
| `ok` (status green)| `.78 .16 145` | **`#6ED274`** | status dots in read-only fact lists only |
| `spike` (amber) | `.72 .17 55` | **`#F3821D`** | frametime-outlier bar in FPS graph only |

Neutrals — these are alpha layers, and the alpha is load-bearing (panels are translucent):

| Token | Value | Used for |
|---|---|---|
| `surface` | `rgba(9,10,12, .88)` (`#090A0C` @ 88%) | window/panel/dock background |
| `surface-hud` | `rgba(9,11,14, .60)` | FPS HUD background (blur mode; see §12 for no-blur alpha) |
| `raised` | `#FFFFFF @ 4–5%` | inactive segment fill (.04), toggle-area fills (.045), input-style boxes (.05) |
| `raised-subtle` | `#FFFFFF @ 2.2–3.2%` | group-block fills (.022 default, .032 active), readout strips (.03) |
| `hairline` | `#FFFFFF @ 10%` | window border, title-bar bottom border |
| `hairline-dim` | `#FFFFFF @ 6–8%` | group borders (.06–.07), segment borders (.08), separators (.07) |
| `hairline-strong` | `#FFFFFF @ 18%` | unchecked checkbox border |
| `track` | `#FFFFFF @ 9%` | slider track, meter "unlit" segments |
| shadow | `rgba(0,0,0,.8)` | window drop shadow `0 28px 70px -14px` |

Accent alpha steps used throughout (source `oklch(.74 .12 218 / A)` — implement as
`#36BDDD` at alpha A): **.13** (dock active fill), **.16** (chip fill), **.2** (checkbox
fill), **.24** (active segment fill), **.3** (toggle track), **.42** (focused window
border), **.5** (dock active border, accent left-edge on readout strips), **.6** (active
segment border), **.65** (toggle border), **.8** (group active left edge, handle glow).

Text tints: the mockup uses three near-white bases — `#EBF2F8`/(235,242,248) in title
bars, `#E8F0F7`/(232,240,247) for body, `#F0F6FC`/(240,246,252) for emphasized text. The
difference is ≤3/255 per channel and invisible; **implement as one base `#EFF5FB`** with
these alphas:

| Role | Alpha | Effective use |
|---|---|---|
| primary | 90–95% | group names, hero numbers, active window title |
| title (unfocused window) | 86% | title-bar text |
| label | 60–68% | parameter labels, checkbox/toggle row labels |
| segment-inactive | 48–50% | inactive segmented label |
| icon idle | 45–62% | title-bar glyphs (.45–.5), dock idle icons (.62) |
| meta | 30–36% | units, hints, read-only system values, min/max |
| meta-faint | 26–28% | slider min/max scale marks, sub-hints |

Alt themes (1f; accent hue swap only, only if/when a theme picker ships): ember accent
`oklch(.74 .13 58)` = **`#E79551`** with surface `rgba(13,10,9,.88)`; signal-green accent
`#6ED274` with surface `rgba(9,12,10,.88)`. All alpha steps identical.

---

## 2. Typography

The mockup itself was built against IBM Plex Sans + IBM Plex Mono, and the table below
records what was *measured* from it — but as of issue #53 the actually-bundled family is
Geist Sans + Geist Mono (`src/Overlay/fonts/`), not Plex; see this file's 2026-08-22 #53
note above for why the swap needed no numeric changes here. Mono carries **every number,
unit, path, state word, and title**; Sans carries prose labels only. The Mono family is
monospaced (true of both Plex Mono and Geist Mono), so tabular figures are automatic —
but only if numbers are never set in Sans (hard rule from 2b).

| Role | Family/weight | Size | Case | Color | Notes |
|---|---|---|---|---|---|
| Window title | Mono 600 | 11px | UPPER | `#EFF5FB` @ 86% (94% focused) | letter-spacing .16em — skipped per DECISIONS §"letter-spacing" note below |
| Title-bar meta | Mono 400 | 10.5px | lower | @ 30–34% | subsystem name next to title |
| Group name | Sans 500 | 13px | Sentence | @ 90–92% | line-height 1 |
| Parameter label | Sans 400 | 11.5px | Sentence | @ 60–62% | 12.5px in checkbox/toggle prose rows @ 68% |
| Value readout | Mono 500 | 13px | — | `#78DBF6` | tabular; right-aligned |
| Hero number | Mono 500 | 30px (audio %), 17px (fps pair) | — | @ 90–95% white | tabular |
| HUD FPS number | Mono 600 | 18px | — | `#FFFFFF` | line-height 1.2 |
| Chip/badge | Mono 400–500 | 9.5–10px | UPPER | `#71D4EF` on accent @ 16% | 3px 5px padding |
| Segment label | Mono 500 (600 active) | 11.5px (11px in small sets) | lower | see §7 | |
| Meta/status line | Mono 400 | 10.5px | lower | @ 30–36% | line-height 1 or 1.4 wrapped |
| Scale min/max | Mono 400 | 9.5px | — | @ 26% | under slider |
| Dock hotkey glyph | Mono 500 | 8px | UPPER | @ 40% (30% idle) | bottom-right of button |
| Dock hint line | Mono 400 | 10.5px | mixed | @ 42% + text shadow `0 1px 4px rgba(0,0,0,.9)` | |

Font sizes appearing in the design, for atlas planning: 8, 9, 9.5, 10, 10.5, 11, 11.5,
12, 12.5, 13, 17, 18, 26, 30 px (× weights 400/500/600, two families). Bake only the
combinations actually used.

---

## 3. Spacing scale

There is no strict 4px grid; the working scale is **1 / 2 / 3 / 5 / 7 / 10 / 12 / 14**:

- Window padding: **14px** all sides. Gap between top-level items in a window: **12px**
  (13px in the Shaders window — treat 12 as canonical, the 13 is the mockup violating its
  own scale).
- Group-block internal padding: **12px**; gap between rows inside a group: **10px**.
- Label→slider vertical gap: **5px**. Slider→min/max row gap: **5px**.
- Section-header (e.g. "SCALING FILTER") → control gap: **7px**; sub-list row gap: **6px**.
- Horizontal gap inside a row (dot–title, toggle–label, checkbox–label): **10px** in
  title bars and group headers, **9px** for checkbox rows, **8px** for status-dot rows and
  meter label→bars.
- Segment gap: **3px**. Dock button gap: **5px**. Dock container padding: **6px**.
- Title-bar horizontal padding: **12px**; window-control glyph cluster gap: **2px**
  (3px between the TUNE/PRESETS tabs).

---

## 4. Window / panel chrome

Composition top-to-bottom: title bar (34px) → optional media strip (Shaders A/B, 78px) →
content column (14px padding, 12px gaps) → optional footer strip.

- Widths are fixed per window, height auto: Shaders 500, Display/Gamescope 470,
  Audio 440, FPS-config 430. Free-floating, draggable by title bar only, no resize.
- Background `rgba(9,10,12,.88)`. **Border 1px `#FFFFFF` @ 10%**. **Corner radius 4px**
  (content clipped to it). Shadow `0 28px 70px -14px rgba(0,0,0,.8)` — in ImGui
  approximate with 3–4 stacked `AddRectFilled` expansions (offset +6/+14/+24px, alpha
  .25/.12/.05, black) or skip; the focus glow matters more than the drop shadow.
- Backdrop blur `22px, saturate 1.1`: **dropped** per DECISIONS §5 (flat translucency +
  overlay fade instead). If gamescope's native blur pass becomes available to the new
  presentation layer, restore at blur≈22px behind windows/dock, 12px behind the HUD.
- **Focused window:** border becomes `#36BDDD` @ 42%; title bar gets the accent gradient
  (§5); glow `0 0 40px -20px #36BDDD @ 60%` — approximate as 2 rect expansions (+4px @
  .10, +10px @ .04 accent). Focused window renders last (on top).
- **Unfocused windows: whole window at 94% opacity** (multiply all alphas by .94).

## 5. Title bar

- Height **34px**, horizontal padding 12px, item gap 10px, radius follows window top.
- Fill: vertical gradient `#FFFFFF @ 6%` (top) → `#FFFFFF @ 1.5%` (bottom)
  (`AddRectFilledMultiColor`). Bottom border 1px `#FFFFFF @ 10%`.
- Focused: gradient becomes `#36BDDD @ 16%` → `#FFFFFF @ 2%`; bottom border
  `#36BDDD @ 30%`.
- Contents, left→right: **6×6px square status dot** — accent `#36BDDD` when the
  subsystem is live (glow `0 0 8px accent @ 80%` ≈ one 10×10 rect behind it @ 25%
  accent), `#EFF5FB @ 35%` when idle → title (Mono 600 11 UPPER) → meta (Mono 400 10.5
  @ 30%) → spacer → optional right-aligned tab pair (see §7 small variant; TUNE/PRESETS:
  4px 8px padding, Mono 500 10px, letter-spacing .08em dropped) → glyph buttons.
- Glyph buttons: **18×18px** hit boxes, gap 2px, no fill/border. Collapse = 9×1px line;
  close = two 11×1px lines at ±45°. Stroke color `#EFF5FB @ 45%` (50% on the focused
  window). No hover state was designed — use @ 80% on hover as the in-style invention.

## 6. Group blocks (settings sections inside a window)

- Fill `#FFFFFF @ 2.2%`, border 1px `#FFFFFF @ 6%`, padding 12px, internal row gap 10px,
  **square corners**.
- The *featured/active* group instead: fill `#FFFFFF @ 3.2%`, border 1px `#FFFFFF @ 7%`,
  and a **2px accent left edge** `#36BDDD @ 80%` replacing the left border.
- Group header row: toggle (if any) / group name (Sans 500 13 @ 92%) / chips / spacer /
  right-aligned toggle. Gap 10px.
- Plain (non-boxed) sections use a Mono 400 10.5 UPPER header @ 36% (letter-spacing
  .12em — skipped) with 7px gap to the control.
- Separator between plain sections: 1px `#FFFFFF @ 7%`, full content width.

## 7. Controls

### Slider
- Row layout: label line above (label left, value right, baseline-aligned), then the
  track line, then optional min/max line. Label→track gap 5px.
- Track: full width, **5px tall, 3px corner radius**, fill `#FFFFFF @ 9%`.
- Filled portion: horizontal gradient `#36BDDD @ 50%` → `#47CAEA` (left→right), same
  radius.
- Handle: **8×18px rectangle, 1px radius**, fill `#BAE7F4`, centered vertically on the
  track (row hit-height 18px), glow `0 0 12px #36BDDD @ 80%` — approximate with one
  16×26px rect behind @ 18% accent + one 12×22px @ 30%.
- Value: Mono 500 13 `#78DBF6`, tabular, right-aligned at the row's right edge.
- Min/max marks: Mono 400 9.5 @ 26%, left/right under the track (a centered third item
  like "measured 0.31" is allowed).
- Interaction: drag or mouse-wheel; commits live.

### Toggle (switch)
- **30×15px**, square, 1px inset content padding. Track fill `#36BDDD @ 30%`, border 1px
  `#36BDDD @ 65%`. Knob **11×11px** square `#93DEF4`, right = on, left = off.
- The off-state track color is **not captured anywhere in the handoff** (every toggle is
  shown on). In-style invention for off: track `#FFFFFF @ 7%`, border `#FFFFFF @ 18%`,
  knob `#EFF5FB @ 55%`. Flag: invented, not measured.

### Checkbox
- **12×12px** square. Checked: border 1px `#36BDDD @ 70%`, fill `#36BDDD @ 20%`, centered
  **5×5px** solid mark `#93DEF4` (a filled square, not a check glyph). Unchecked: border
  1px `#FFFFFF @ 18%`, fill `#FFFFFF @ 4%`, no mark.
- Label: Sans 400 12.5 @ 68%, 9px gap; optional trailing Mono meta @ 28%.
- (1d's FPS-rows list uses an 11×11 box with 4×4 mark and 8px gap at 7px row spacing —
  a densified variant of the same control; using 12×12/5×5 everywhere is acceptable.)

### Segmented control
- Row of equal-width cells, **3px gaps**, cell padding **7px vertical** (6px in compact
  10–11px-text variants), square corners, **labels lowercase**.
- Inactive cell: fill `#FFFFFF @ 4%`, border 1px `#FFFFFF @ 8%`, text Mono 500 11.5
  `#EFF5FB @ 50%`, centered.
- Active cell: fill `#36BDDD @ 24%`, border 1px `#36BDDD @ 60%`, text Mono **600** 11.5
  `#A9EAFD`.
- Small right-aligned variant (title-bar tabs, blend-mode pickers): cells 4px 8–10px
  padding, 10–11px text, otherwise identical colors.

### Chip / badge (AUTO, CAS, HOOKS GAMESCOPE)
- Padding 3px 5px, square, no border. Accent chip: fill `#36BDDD @ 16%`, text Mono 9.5–10
  `#71D4EF`. Neutral chip: fill `#FFFFFF @ 6%`, text @ 45%.

### Read-only readout strip
- Full-width box, fill `#FFFFFF @ 3%`, padding 6px 9px (9px 10px for the taller fps-pair
  variant), **no border** except an optional 1px accent left edge `#36BDDD @ 50%`.
- Content is Mono 400 10.5 @ 34% (system-measured values are always 34% mono — "never
  show a value the overlay didn't verify").
- Boxed hint rows (e.g. "Frame generation…") add a 1px `#FFFFFF @ 6%` border and pair a
  Sans 11.5 @ 60% label with an accent Mono action on the right.

### Footer bar (Shaders window)
- Full-width strip: padding 8px 12px, fill `#FFFFFF @ 2.5%`, top border 1px `#FFFFFF @ 7%`.
  Left: Mono 10.5 @ 34% status. Right: Mono 10.5 actions — neutral @ 50%, accent action
  `#6BCDE9`. Gap 10px. (In 2a this strip is unboxed inside the padding; either form is
  in-mockup.)

### Meters and graphs
- Frametime graph (HUD): vertical bars, **1px gaps**, aligned to bottom of an 18px-tall
  strip; normal bar `#36BDDD @ 60%`, outlier bar `#F3821D @ 80%`, bar heights 33–100% of
  strip.
- L/R peak meters (Audio): 20 segments per channel, **1.5px gaps** (use 1–2px), **5px
  tall**; lit `#36BDDD @ 85%`, unlit `#FFFFFF @ 9%`; 8px-wide Mono 9.5 @ 30% channel
  letter, 8px gap; 4px between the two channel rows.
- Before→after pair: Mono 500 17 tabular; first value @ 90% white, arrow Mono 11 @ 35%,
  second value `#6CDCFA`.

## 8. Dock

- Position: centered horizontally, container bottom **38px** above the screen edge.
- Container: padding **6px**, fill `rgba(9,10,12,.86)`, border 1px `#FFFFFF @ 11%`,
  **radius 4px**, shadow `0 22px 60px -18px rgba(0,0,0,.9)`.
- Buttons: **54×54px squares** (square corners), gap **5px**.
  - Idle: fill `#FFFFFF @ 4.5%`, border 1px `#FFFFFF @ 10%`, icon strokes `#EFF5FB @ 62%`.
  - Open/active: fill `#36BDDD @ 13%`, border 1px `#36BDDD @ 50%`, icon `#A3E3F6`, plus a
    **2px accent top edge** `#4FD0F1` flush with the button's top border (inset 8px from
    each side, i.e. 38px wide), glow `0 0 10px #36BDDD @ 90%` ≈ one 42×6px rect under it
    @ 25% accent.
  - Close button (after divider): fill `#FFFFFF @ 3%`, border `#FFFFFF @ 8%`, 16×16 ×
    glyph at 1.5px stroke @ 50%.
- Divider before the close button: 1px × 34px, `#FFFFFF @ 10%`, 3px horizontal margin on
  each side (participates in the 5px gap → 12px total between neighbors).
- Hotkey glyph: Mono 500 **8px**, bottom-right of each button at 4px right / 3px bottom
  inset; `#EFF5FB @ 40%` on active buttons, @ 30% idle. (Our binding is `Ctrl+Shift+O`
  for the overlay itself; per-window hotkeys only if they exist — do not print F-keys we
  don't bind.)
- Icon content: 16–20px box centered; 1–1.5px strokes (our SVG set in `data/icons/`).
- Hint line: centered under the container, **10px gap**: Mono 400 10.5 @ 42% with text
  shadow `0 1px 4px rgba(0,0,0,.9)`. Copy pattern: "SHIFT+TAB toggle overlay · game input
  paused" → ours: `CTRL+SHIFT+O toggle overlay · game input paused`.

## 9. Tooltip (dock hover — from 1e-C, the only tooltip designed)

Solid fill `rgba(6,8,10,.94)` (no blur), border 1px `#FFFFFF @ 12%`, padding 4px 7px,
square, positioned centered above the anchor with 6px clearance, label Mono 500 10
`#EFF5FB @ 90%`, no arrow.

## 10. FPS HUD (the always-on element)

Default (backdrop = blur mode, as selected in the final mockup):
- Container: padding **8px 11px**, min-width 186px, fill `rgba(9,11,14,.6)` + blur 12px,
  border 1px `#FFFFFF @ 12%`, **square corners** (radius 0 — note: unlike windows).
  **Without blur** the .6 fill is too weak over bright games; use `rgba(9,11,14,.78)`
  as the no-blur approximation (flagged: invented compensation, not a mockup value).
- Default anchor: top-right, offset 32/32 (2a shows 36/32; 1d's config says "offset
  32 / 32" — treat 32/32 as the intended default).
- Row 1 (baseline-aligned, 6px gaps): FPS number Mono 600 18 `#FFFFFF` tabular → "FPS"
  Mono 400 10 @ 50% white → spacer → frametime Mono 500 12 `#89E0F8` (`oklch(.86 .09
  218)`).
- Row 2: frametime graph per §7-meters, 18px tall, 4px above/below gaps (column gap 4px).
- Row 3: percentile row, 10px gaps: Mono 400 10 @ 55% white — "1% 168", "0.1% 121", "74°".
- Other backdrop modes (from 1d samples): `none` = bare text, white, text-shadow none;
  `shadow` = text-shadow `0 2px 4px rgba(0,0,0,.95)` (draw text twice: black offset copy
  then white); `solid` = fill `rgba(6,8,10,.86)`, padding 7px 10px, no border.

## 11. FPS config window (from 1d, restyled to final metrics)

Standard window chrome (§4–5) at 430px wide; title "FPS DISPLAY"; title-bar right slot
shows "live" in Mono 10 `#6BCDE9`. Content: font-size slider → BACKDROP segmented
(none/shadow/solid/blur) → backdrop-opacity slider → blend-mode small segmented →
separator → two-column area: ROWS checkbox list (left, flex) | 1px `#FFFFFF @ 6%`
vertical divider | ANCHOR block (right, 150px): 3×3 grid of 30×30px cells, 3px gaps
(cells: fill `#FFFFFF @ 5%`, border `#FFFFFF @ 9%`; selected: fill `#36BDDD @ 30%`,
border `#36BDDD @ 70%`), then offset/meta lines Mono 10 @ 30%. Footer meta: "sampling
500 ms · 240-frame window" @ 30% above a 1px @ 7% top border.

## 12. States summary

| State | Treatment |
|---|---|
| Focused window | accent border @ 42%, accent header gradient, accent glow, on top |
| Unfocused window | whole window × 94% opacity |
| Active/open dock button | accent fill/border + 2px top edge + bright icon |
| Active segment | accent 24% fill, 60% border, 600-weight `#A9EAFD` text |
| Enabled toggle/checkbox | accent track/fill + bright knob/mark |
| Disabled-but-visible control | **whole row × 34% opacity**, fill/handle turn plain white (`#FFFFFF @ 30%` fill, `@ 45%` handle) — e.g. NIS slider while FSR active |
| Read-only system value | Mono @ 34%, never editable-looking |
| Hover / pressed / keyboard focus | **never designed** (the handoff's own "try next" list admits it). In-style inventions: hover = +6% white on fills, glyphs to @ 80%; pressed = +accent 8% on fills; keyboard focus = 1px accent @ 60% border. Flagged as inventions. |

## 13. Out of scope / cannot reproduce (with approximations)

- **Backdrop blur** — dropped (DECISIONS §5); flat `rgba(9,10,12,.88)` carries the look.
  May return via gamescope's native blur pass (22px windows/dock, 12px HUD).
- **Letter-spacing** (.06–.16em on titles/chips) — skipped per DECISIONS; do not fake.
- **Soft shadows/glows** — no ImGui primitive; use the layered-rect recipes given inline
  (§4, §5, §7, §8). Skip the large window drop shadow if it looks banded.
- **Gradients** — native via `AddRectFilledMultiColor` (title bar vertical, slider fill
  horizontal).
- **`mix-blend-mode` additive/screen HUD text** — needs a blend-state `ImDrawCallback`;
  defer unless the backdrop "additive" mode ships.
- **Mockup-only features** (Shaders A/B strip, LSFG panel, PipeWire audio panel contents,
  adaptive brightness): styling harvested above; the panels themselves follow our feature
  set (DECISIONS §1, §14, §22–23).

## 14. Explicitly unmeasured / uncertain

- Toggle off-state track color (never shown) — invented value in §7.
- Hover/pressed/focus states (never designed) — invented values in §12.
- No-blur HUD alpha .78 — invented compensation.
- Error/danger color — does not exist anywhere in the handoff.
- The radial vignette behind the overlay (`radial-gradient 120% 100% at 50% 60%,
  rgba(4,6,9,.28)→.62`) is part of the mockup scene; whether we dim the game while the
  overlay is open is a product decision, not a measured requirement.

---

## Gap list

Current implementation (screenshot `verify-after/screenshot.png`, sampled 2026-08-21)
vs this spec, ordered by visibility. Current values below were pixel-measured.

1. **Accent color is wrong everywhere.** Current `#4FB8D6` (the design guide's "≈-ish"
   estimate got implemented literally). Target `#36BDDD`, with the full derived family in
   §1 (`#47CAEA` gradient end, `#78DBF6` values, `#A9EAFD` segment text, `#BAE7F4`
   handles, `#93DEF4` knobs, `#A3E3F6` icons).
2. **Sliders are stock-ImGui.** Current: ~20px-tall filled frame with the value text
   *inside* the bar ("100% sharper"), accent-bordered, ~8px square grab. Target: §7 —
   label+value line above, 5px track (3px radius), gradient fill, 8×18 `#BAE7F4` handle
   with glow, value right-aligned in `#78DBF6`, min/max @ 26% below.
3. **Panels are too transparent and the base is wrong.** Current fill measured ≈`#121314`
   @ ~65% alpha (game clearly visible through the body). Target `rgba(9,10,12,.88)` —
   darker base, notably more opaque.
4. **Title bar chrome missing.** Current: 30px tall, flat fill identical to body, no
   status dot, no meta text, no gradient, no collapse glyph, close glyph only. Target §5:
   34px, white 6%→1.5% gradient, 6×6 accent status dot + glow, Mono 600 11 title, 10.5
   meta @ 30%, 18×18 collapse+close at 45%.
5. **Focus treatment wrong.** Current: a 1px full-height accent stripe down the window's
   left edge (not in the mockup) and a neutral `#232426`-composited border. Target:
   remove the stripe; focused = accent border @ 42% + accent header gradient + glow;
   unfocused = 94% opacity.
6. **FPS HUD is an unstyled chip.** Current: solid `#232424` box (70×30px, no border),
   plain white 13px text, top-left 16/16. Target §10: `rgba(9,11,14,.78)` no-blur fill,
   1px `#FFFFFF @ 12%` border, 8/11 padding, Mono 600 18 number + accent ms readout +
   graph + percentile row per config, default anchor top-right 32/32.
7. **Dock active marker.** Current: full 1px `#4FB8D6` border + `#1C2F38` fill (fill
   composite is right). Target: border `#36BDDD @ 50%`, fill @ 13%, plus the signature
   **2px accent top edge** (inset 8px) with glow. Also: container corners square →
   radius 4; button gap 6px → 5px; no hotkey glyphs (add 8px Mono bottom-right, only
   for keys we actually bind); no hint line under the dock (add §8 copy).
8. **No group blocks.** Current windows are a flat control list with plain separators.
   Target §6: hairline boxes on 2.2% white, 12px padding, active group gets the 2px
   accent left edge.
9. **Segmented control text.** Current: Title-case labels ("Linear", "Nearest"), active
   label near-white, inactive ≈#575858-on-#191A1B (too dim). Target: lowercase, active
   Mono 600 `#A9EAFD`, inactive Mono 500 @ 50% white on 4% fill. Geometry (24px cell
   height, 3px gaps) is already close — keep.
10. **Window padding & metrics.** Current content inset ≈11px → target 14px; title bar
    30 → 34px. Toggle geometry (30×15, 11px knob) already matches — recolor only.
11. **App-id row styling** (our feature, not in mockup): keep, but restyle as a §7
    read-only strip (Mono 10.5 @ 34%) instead of a full-brightness row, and the leading
    dot should be the 6×6 square status dot, not a circle.
