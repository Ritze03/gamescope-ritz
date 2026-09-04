# FPS display (the HUD)

One integer, drawn well, over the game. `src/Overlay/FpsDisplay.cpp` /
`FpsDisplay.h`; config lives in `config::FpsDisplaySettings`
(`src/Config/ConfigSchema.h`); settings area `system.hud`
(`FpsDisplay_RegisterArea()`).

This is the **HUD** (see `superdoc/meta/TERMINOLOGY.md`) — distinct from the
**Shell**/**Click-UI** settings surface that hosts its settings tab, and from
the unrelated **Launcher**. It has its own visibility flag and its own
render pipeline (own ImGui context, offscreen texture, timeline semaphore),
entirely independent of the settings panel, so it keeps drawing over the
game while the panel is closed.

## History: two phases, both 2026-09-03

**Phase 1** stripped what used to be a small performance profiler (CPU/GPU
load, a frametime graph, a percentile row, Now Playing — see
`superdoc/meta/TERMINOLOGY.md`'s "profiler" entry) down to a single FPS
integer, positioned by a 9-point anchor plus pixel margins, at a
user-chosen font size. Those three settings (Placement, Font size) are
unchanged since Phase 1.

**Phase 2** (this page) added everything else, following the user's own
spec verbatim: hide-above-X, three update modes, a plain backdrop, a
two-way text-colour choice with lag-spike reactions, and a black outline
sized in pixels (a drop shadow until 2026-09-03, when the outline
replaced it).
The user's own framing: *"it is nice, instead of feature bloat"* — one
number, drawn well, not a second profiler.

## The settings, top to bottom

`system.hud`'s row order matches this list exactly (`FpsDisplay_RegisterArea()`):

| Row | Config field(s) | What it does |
| --- | --- | --- |
| Show HUD | `enabled` | Master switch. Not gated by itself (SPEC §3.13). |
| Placement | `anchor`, `margin_x`, `margin_y` | 9-point anchor + pixel margins (Phase 1, unchanged). |
| Font size | `font_size` | Text size in px. |
| Update mode | `update_mode` | Smoothing / Update every second / Immediate — see below. |
| Hide above X | `hide_above_enabled`, `hide_above_fps` | Switch + threshold `.Param()` — see Hysteresis below. |
| Backdrop opacity | `backdrop_opacity` | 0–1; **0 means no backdrop at all**, not a separate switch. |
| Text colour | `color_mode` | Fixed / Inverted — see below. |
| Lag spike detection | `lag_detection_enabled` | Master switch for the whole spike reaction. Default **on**. |
| Outline size | `outline_strength` | 0–4 px of black outline; 0 means no outline drawn at all. |

Every row is gated `DisabledUnless(MonitorOn, "the HUD is off")` except the
master switch itself.

## Update modes

Three ways to turn the raw per-commit frametime
(`g_ulLastAppFrametimeNs`, DECISIONS.md #16/#17) into the number on screen.
All three are kept live simultaneously in `UpdateAndGetDisplayFps()`
regardless of which is selected, so switching modes in the settings panel
never has to "warm up" a stale average:

- **Smoothing** (default) — the pre-existing single-pole EMA (α = 0.10)
  over the raw frametime. Eases between readings; never jitters.
- **Update every second** — accumulates every raw sample's frametime and
  count since its last publish, and only turns that into a displayed
  value once a full second has elapsed
  (`frames-in-window / total-time-in-window`, a real windowed average,
  not a resampled EMA). The digits visibly hold still between updates.
- **Immediate** — the latest single frame's own instantaneous rate
  (`1000 / frametime`), no averaging, held between samples so an idle
  client doesn't show 0.

## Hide if FPS above X — with hysteresis

Modeled as a `Switch` (`hud.hide_above`) with a threshold `Param`
(`hud.hide_above.fps`) — the same `.Param()` idiom `hud.anchor` already
uses for its margins.

A plain `hidden = fps > X` flips every frame the reading sits on either
side of X, which at a stable frame rate is real visible flicker (ordinary
float jitter crosses an exact threshold constantly). `DrawReadout()`
instead runs a one-sided Schmitt trigger:

- While **shown**, only hides once fps climbs **5 fps past** X.
- While **hidden**, only reappears once fps drops back **to or below** X
  itself.

So "hide above X" still means what it says (X is the line that matters
going from shown to hidden), while the reverse crossing needs 5 fps of
margin — enough that ordinary frame-to-frame variance right at the line
can't retrigger it. The 5 fps band is a fixed constant in
`FpsDisplay.cpp`, not a setting — the user asked for hysteresis, not a
second number to tune.

## Backdrop

A plain rectangle behind the number, sized to the text plus a fixed
padding (`backdrop_padding`, not user-facing). **Never rounds its
corners** — the user was explicit about this being a plain rectangle, so
`DrawModuleBackdrop()` always draws with `0.0f` rounding, not a config
value; Phase 1's `backdrop_rounding` field is gone outright rather than
kept-but-unwired, so an old config's stale value can never silently
contradict this.

`backdrop_opacity` (0–1) is the only control: 0 **is** "no backdrop",
folding what used to be a separate `backdrop_enabled` switch into a
single number. An old config that had the backdrop switched off via that
removed field will show the backdrop again after upgrading if its opacity
was left non-zero — a deliberate behaviour change, noted in
`CHANGELOG.md`, not an oversight.

## Text colour: Fixed vs. Inverted

**Fixed** — the UI's own accent colour (`Palette.h`'s `kAccentValue`,
overridable by the unexposed `color_fps` field the same way it always
was). On a detected lag spike, the resolved colour is inverted
(`1 - r, 1 - g, 1 - b`) for the hold window — literally "just invert the
text colour", per the user's own spec.

**Inverted** — a **true per-pixel invert** of the game's own colour under
each glyph pixel (Phase 3, 2026-09-03). This used to be a fake — Fixed-mode
colour picking derived from the backdrop instead, documented here as an
honest limitation — but it is now real, implemented one layer down in the
Vulkan compute-composite shader rather than in this file's own ImGui draw
pass:

- This file just draws the digits as **plain opaque white** and hands the
  HUD's single layer a new blend mode, `rendervulkan.hpp`'s
  `ALPHA_BLENDING_MODE_INVERT` (`FpsDisplay_AddLayer()`). Opaque, not
  `text_opacity`-scaled — a partial alpha here would only dilute the
  invert, mixing in un-inverted background (see the gating rule below).
  White specifically: it is also the marker the shader uses to tell the
  digits apart from the backdrop and the outline, see
  [What Inverted mode does *not* invert](#what-inverted-mode-does-not-invert).
- The actual invert happens in `src/shaders/alphamode.h`'s `BlendLayer()`,
  which every composite call site shares (plain blit, FSR/RCAS, both blur
  passes) — one function, one edit, all paths covered. For each pixel:
  `1.0 - c` on the real background colour it's compositing onto, gated on
  this layer's own alpha so only glyph pixels are touched:
  `outputValue.rgb = mix(outputValue.rgb, inverted, layerAlpha)`.
  Fully-transparent HUD-texture pixels (`layerAlpha == 0`) pass the
  background through completely unchanged.
- **Mid-grey guard**: a literal invert's luma is exactly `1.0 - bgLuma`
  (the Rec.709 weights `0.2126/0.7152/0.0722` sum to 1.0), which collapses
  to **zero** luma separation from the background right at `bgLuma ==
  0.5` — digits would vanish over any surface near mid-grey. When the
  separation is below **0.40** (raised 2026-09-03 from an initial 0.25;
  chosen from a measured on-screen check at mid grey — background
  RGB(188,188,188) inverted to a marginal grey-on-grey RGB(138,138,138)
  at 0.25, versus a comfortably-readable RGB(91,91,91) at 0.40 — not
  derived from a formal contrast spec), the inverted colour is pushed
  uniformly toward black or white — away from the background's own luma
  — by just enough to clear that floor, and no more. That engages for
  backgrounds with luma roughly in `(0.30, 0.70)` (was `(0.375, 0.625)`
  at 0.25) — outside that band around mid-grey, the true inversion
  survives completely untouched. Raising the floor further keeps buying
  legibility inside that band at the cost of how much of the true
  inverted colour survives there — the deliberate tension in this knob.
- **Blend-space / HDR caveat**: `BlendLayer()` runs *after*
  `apply_layer_color_mgmt()` and *before* `encodeOutputColor()` — i.e. in
  linear-light blend space, not the final encoded output. Under HDR/PQ,
  colours here are not bounded to `[0, 1]`, so the background is clamped
  to `[0, 1]` before inverting; skipping that clamp could hand `1.0 - c` a
  negative or wildly out-of-range result.
- **What it inverts against**: `BlendLayer()` inverts whatever has already
  been folded into `outputValue` by the time the HUD's layer is reached —
  i.e. every layer pushed *before* it, not the whole frame (see
  [compositing-vulkan.md](compositing-vulkan.md#layer-order-zpos)'s push-order
  note). Since the 2026-09-03 layer-order fix the HUD is pushed before the
  settings overlay/Shell, so Inverted mode inverts the **game (plus cursor
  and mura correction) alone** — it no longer sees or inverts the Shell,
  which composites on top of the HUD now.

Inverted mode can't "invert" already-inverted text to signal a lag spike
— doing that would show nothing against itself. Instead, **a spike tints
the backdrop** toward a muted warning red for the hold window. It only
does so when a backdrop is actually being drawn: with `backdrop_opacity`
at 0 there is no spike indication in Inverted mode at all.

> **Why:** this used to *force* a faint backdrop visible for the hold
> window even at opacity 0, so an ordinary frame hitch made a backdrop
> the user had switched off appear on screen — reported as "backdrop
> opacity 0 doesn't turn the backdrop off in Inverted mode" and fixed
> 2026-09-03. Losing the spike hint when there is no backdrop is the
> honest price of letting the opacity setting mean what it says.

### What Inverted mode does *not* invert

Only the **digits' fill**. The backdrop composites normally and the
outline stays black — neither is inverted (both were, until 2026-09-03).

All three still live in **one** composite layer, and the shader tells
them apart by the layer's own brightness. `alphamode.h`'s invert branch
computes a selector from the layer texel's Rec.709 luma —
`smoothstep(0.25, 0.80, layerLuma)` in linear-light blend space — and
mixes between the texel's own colour (selector 0, bit-for-bit the
`alpha_mode_coverage` blend) and the inverted destination (selector 1).
The HUD's contract with it: the digits' fill is drawn **pure opaque
white** (luma 1.0) and everything that must not invert is drawn far
darker — the backdrop base is linear ~0.003, its warning-red spike tint
~0.05, the outline pure black. Nothing lands in the transition band
except antialiased glyph edges, which cross-fade, which is what they
should do.

> **Why one layer and not two.** This was briefly split across two
> layers — a normally blended backdrop + outline layer with an
> invert-blended glyph-only layer above it, sampling a double-height
> texture — and that **broke Inverted mode outright** the same day
> (2026-09-03): with `backdrop_opacity` at its 0.5 default, the lower
> layer painted a dark box over the game exactly where the digits were
> about to land, so the upper layer inverted *the HUD's own backdrop*
> instead of the game and the digits came out a constant near-white. An
> outline made it worse still: four black glyph copies 1px out cover the
> glyph body completely, so the invert read pure black and returned pure
> white. The general rule the split violated: **a blend that reads the
> destination cannot be split across two layers that overlap.** Whatever
> the lower one paints becomes the "background" the upper one inverts.
>
> The colour-marker approach above was considered and rejected when the
> split was written, on the grounds that antialiased glyph edges are
> mid-grey rather than white and would be misclassified. That objection
> assumed a hard threshold; a `smoothstep` cross-fade turns the same
> pixels into a gradient between the outline's black and the inverted
> colour, which is ordinary antialiasing rather than a misclassification.
> The backdrop's 12%-white border was the other objection — at 0.12
> encoded it is linear ~0.014, nowhere near the 0.25 selector floor.

**Layer budget.** Because there is only ever one layer again, the HUD has
no special layer-budget behaviour: `k_nMaxLayers` / `VKR_MAX_LAYERS` are
both 6, `LayerStack_t::push()` returns `nullptr` when the frame is full,
and the HUD then simply draws nothing, with no assert and no log line —
the same as every other overlay layer. `paint_all()` reserves slots for
the cursor and the overlay planes but **not** for the HUD, the toasts or
the Shell, so a busy frame (base + override + external overlay + Steam
overlay + cursor + mura is already six) can silently lose them. There is
no degraded single-layer fallback path any more, because there is nothing
to degrade from.

**Interaction with the outline.** None, now. The outline is black, the
selector leaves black alone, and the digits invert the game regardless of
how thick the outline is.

## Outline

`outline_strength` (row "Outline size", 0–4 **pixels**, step 0.25,
default 0 = off). Its name kept the `_strength` suffix from the few hours
it spent as a 0–1 opacity on 2026-09-03; a config written in that window
still loads, and its 0–1 value is simply read as a thin (sub-pixel to
1px) outline. It replaced the earlier drop shadow the same day at the
user's request, and the old config key `shadow_strength` is no longer
read at all (an old file falls back to the default).

The geometry: the digits stamped again in solid black on **concentric
rings** around the real text position, the fill then drawn on top. Ring
spacing is capped at 1px (`ceil(radius)` rings), and each ring's stamps
are spaced at most ~0.75px apart along it (`ceil(2πr / 0.75)`, clamped to
8–48 per ring). A 1px outline is therefore a single 8-stamp ring — as
cheap and as crisp as the four-offset version it replaced — while a 4px
outline is four rings of up to ~34 stamps, ~90 text draws of at most
three glyphs.

> **Why rings and not four axis-aligned offsets:** four offsets only look
> solid while the radius is about a pixel. Past that the diagonals open
> up and the outline reads as a cross, not a stroke. The 1px ring spacing
> and the ~0.75px stamp spacing are what keep a 4px outline continuous.
>
> **Why a size and not an opacity:** the user asked for a *size* ("the
> max outline size should be 4.0"). It is also the only version that can
> work — stacking translucent offset copies saturates the alpha byte long
> before the ring closes, and an outline that fades as it grows reads as
> blur, which is exactly what the drop shadow was rejected for.
>
> **Why 4px is the ceiling:** `backdrop_padding` is 6px, so the outline
> stays inside the backdrop box at any setting and growing it never
> changes the readout's footprint.

> **Why each stamp's offset is rounded to a whole pixel (2026-09-04
> fix):** the stamp angles are a full, evenly-spaced sweep, so the *ideal*
> (unrounded) stamp cloud is provably centred on the text position — but
> Dear ImGui's `ImFont::RenderText()` independently floors every
> `AddText()` call's position to a whole pixel before drawing it
> (`imgui_draw.cpp`'s "Align to be pixel perfect"), and flooring does not
> distribute over a fractional offset: `floor(textPos + r)` is not always
> `floor(textPos) + r`. That let individual stamps floor to a different
> pixel than the fill did, reading as the outline sitting up-left of the
> digits — worse at a bigger radius (more, farther-flung stamps) and a
> bigger font (the same misalignment is a larger fraction of a thinner
> stroke). Rounding the stamp's *offset* from the text position to a
> whole pixel first makes its own floor an exact no-op relative to the
> fill's, for any text position, font size or radius.

> **Sub-pixel radius (below 1px) is alpha, not smaller geometry
> (2026-09-04 fix, part 2):** the whole-pixel rounding above fixes the
> lean, but it also means every stamp on a below-1px ring rounds to
> `(0, 0)` — right on top of the fill — so radii below 0.5 drew no
> outline at all, a regression from the pre-rounding version's faint
> sub-pixel outline. There is no such thing as a smaller-than-1px stroke
> on a whole-pixel grid, so a sub-pixel radius is rendered as what it
> physically is: a faint 1px outline. Below radius 1, the code stamps the
> same whole-pixel radius-1 ring the solid path uses at
> `outline_strength` 1, and scales that ring's **alpha** by the radius
> instead of shrinking its geometry — alpha 64 at 0.25, 128 at 0.5, 191
> at 0.75. At radius 1.0 the alpha is exactly 255, bit-for-bit the solid
> path's own colour, so the transition across 1px is continuous; radii at
> or above 1 take the untouched solid-black geometry unchanged. Safe
> under Inverted mode's luma selector (`alphamode.h`'s
> `alpha_mode_invert`) for the same reason plain black already was:
> blending pure black at any alpha can only pull the layer's own RGB (and
> so its luma) toward zero, never up, so a faint outline cannot cross
> `smoothstep(0.25, 0.80, layerLuma)` into invert-select territory no
> matter how low its alpha goes — no separate handling needed for that
> mode.

## Lag-spike detection

Switchable (`lag_detection_enabled`, row "Lag spike detection", default
**on** so an existing config keeps today's behaviour). With it **off**
there is no spike reaction of any kind: Fixed mode never flips the
number's colour and Inverted mode never tints the backdrop. Nothing else
in the tab depends on it, so no row greys out when it is off.

> **Why the detector keeps running while the switch is off:** the
> frametime history is a handful of floats per frame, later work wants it
> anyway, and keeping it warm means switching detection back on reacts to
> the very next spike instead of waiting for a window of samples to
> refill.

The per-frame frametime ring buffer (`s_flFrametimeHistoryMs`, 240
samples, kept since Phase 1 unused until now) feeds a fixed heuristic —
**no user-facing threshold**, deliberately: the user asked for the
feature, not a second slider to tune it with.

A frame counts as a spike (`ComputeIsSpike()`) when it is **both**:

- at least **1.75x** the median of the last 30 prior samples
  (`kSpikeFactor`), and
- at least **4ms** slower in absolute terms (`kSpikeMinDeltaMs`).

Relative alone would trip constantly on a very fast, very stable game (a
240fps game's own frame-to-frame jitter is easily +75% of its ~4ms
median without anything being wrong); absolute alone would never trip on
a game already running slow, where every frame is "big" in milliseconds
but nothing has changed. The **median** (not the mean) is the baseline so
one prior spike doesn't drag it down enough to mask the next.

Detected state holds visible for **700ms** (`kSpikeHoldNs`) after the
triggering frame — a single bad frame lasts a fraction of a millisecond
on screen otherwise, which isn't "perceptible", it's a flicker the eye
filters out.

## No layout jitter: pinned digit width

The box is sized off a `"%3d"`-formatted (right-justified, blank-padded)
3-character field (0–999), measured in Geist Mono, which is genuinely
monospaced — so a fixed-width formatted string is tabular by construction
and the readout's own box size never changes as the number goes from 1 to
2 to 3 digits. This is a carry-forward of Phase 1's own choice, not a
Phase 2 change — recorded here because the quality bar asked for it to be
stated explicitly, and because it's the reason the box's own width is
stable enough for the backdrop and outline above to sit tight against the
text without hunting for a new size every frame.

What's actually *drawn*, though, is the plain unpadded digits (`"%d"`),
not the blank-padded string — drawing the padded string put the leading
blank glyph's advance inside the text draw, which left a visible empty
gutter on the left of a two-digit number and shoved the digits against
the box's right edge (fixed 2026-09-03). `MeasureFpsModule()` measures
both the padded field and the plain digits and derives `flTextOffsetX`,
half the width difference, added to the text origin so the digits sit
centred in the pinned-width box. The outline and the digits themselves
both draw at that same offset origin, so they track together rather
than the old padded position.
