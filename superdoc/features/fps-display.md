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
two-way text-colour choice with lag-spike reactions, and a drop shadow.
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
| Shadow strength | `shadow_strength` | 0–1; 0 means no shadow drawn at all. |

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
  HUD's layer a new blend mode, `rendervulkan.hpp`'s
  `ALPHA_BLENDING_MODE_INVERT` (`FpsDisplay_AddLayer()`). Opaque, not
  `text_opacity`-scaled — a partial alpha here would only dilute the
  invert, mixing in un-inverted background (see the gating rule below).
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
  separation is below **0.25** (chosen as a conservative "clearly
  legible" floor, not derived from a formal contrast spec), the inverted
  colour is pushed uniformly toward black or white — away from the
  background's own luma — by just enough to clear that floor, and no
  more. Outside that narrow band around mid-grey, the true inversion
  survives completely untouched.
- **Blend-space / HDR caveat**: `BlendLayer()` runs *after*
  `apply_layer_color_mgmt()` and *before* `encodeOutputColor()` — i.e. in
  linear-light blend space, not the final encoded output. Under HDR/PQ,
  colours here are not bounded to `[0, 1]`, so the background is clamped
  to `[0, 1]` before inverting; skipping that clamp could hand `1.0 - c` a
  negative or wildly out-of-range result.

Inverted mode can't "invert" already-inverted text to signal a lag spike
— doing that would show nothing against itself. Instead, **a spike tints
the backdrop** toward a muted warning red, and forces at least a faint
backdrop visible for the hold window even if the user's own opacity
setting is 0 — a temporary exception for the hold window only, never a
change to their stored setting.

## Shadow

`shadow_strength` (0–1, 0 = off). A fixed 2px offset, not scaled with
font size — a shadow that grows with a 48px font reads as blur, not
depth, which is explicitly not what the user asked for ("a shadow that
reads as depth rather than blur"). Alpha scales with strength up to a
0.85 ceiling. Applies the same way regardless of text-colour mode (Fixed
or Inverted) — both draw through the same single `AddText()` call now
that Inverted mode's colour comes from the compute-composite shader
rather than from a separate draw-time technique.

## Lag-spike detection

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
stable enough for the backdrop and shadow above to sit tight against the
text without hunting for a new size every frame.

What's actually *drawn*, though, is the plain unpadded digits (`"%d"`),
not the blank-padded string — drawing the padded string put the leading
blank glyph's advance inside the text draw, which left a visible empty
gutter on the left of a two-digit number and shoved the digits against
the box's right edge (fixed 2026-09-03). `MeasureFpsModule()` measures
both the padded field and the plain digits and derives `flTextOffsetX`,
half the width difference, added to the text origin so the digits sit
centred in the pinned-width box. The shadow and the digits themselves
both draw at that same offset origin, so they track together rather
than the old padded position.
