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
spec verbatim: hide-above-X, update modes (three then, two since 2026-09-05),
a plain backdrop (removed again 2026-09-09 — see [Backdrop: removed](#backdrop-removed)),
a two-way text-colour choice with lag-spike reactions, and a black outline
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
| Update mode | `update_mode` | Smoothing / Immediate — see below. ("Update every second" was folded into Smoothing 2026-09-05.) |
| Hide above X | `hide_above_enabled`, `hide_above_fps` | Switch + threshold `.Param()` — see Hysteresis below. |
| Text colour | `color_mode` | Fixed / Inverted — see below. |
| Number colour | `color_fps` | Fixed mode only — see "Number colour and Text opacity" below. |
| Text opacity | `text_opacity` | Fixed mode only — see "Number colour and Text opacity" below. |
| Lag spike detection | `lag_detection_enabled` | Master switch for the whole spike reaction. Default **on**. |
| Outline size | `outline_strength` | 0–4 px of black outline; 0 means no outline drawn at all. |

Every row is gated `DisabledUnless(MonitorOn, "the HUD is off")` except the
master switch itself.

### Number colour and Text opacity (2026-09-07)

Both fields existed in the schema and were already read by the renderer
(`color_fps` since issue #29, `text_opacity` since Phase 2) but had no row
of their own — a static cross-check
(`build-release/verify-shots/settings-audit-2026-09-07/static-crosscheck.md`)
caught this. They now sit right below "Text colour", in the shape their
siblings use:

- **Number colour** (`hud.color_fps`) is a `Composite(Color)` row with the
  same custom/accent-follow idiom as `PanelCursor.cpp`'s
  `cursor.outline_color`: the band shows and edits the UI's own accent
  colour whenever `color_fps` is unset, and a `custom` `.Param()` switch
  toggles between `std::nullopt` (follow the accent) and a captured literal
  — sharing the row's own `Key()` so a stored `null` reads as "the switch's
  own write", not a stale copy contradicting the band. `PackColorRgb()`
  (`FpsDisplay.cpp`, mirrors `PanelCursor.cpp`'s own `PackRgb()`) packs the
  accent's `ImU32` down to the same `0xRRGGBB` shape `UnpackColorRgb()`
  already read.
- **Text opacity** (`hud.text_opacity`) is a plain 0–1 `Slider`, the same
  shape as the crosshair's own opacity sliders.

**Both are gated to Fixed mode only**, sharing one predicate
(`FixedColorApplies` in `FpsDisplay.cpp`, `MonitorOn && color_mode !=
"inverted"`) and one disabled reason ("text colour is set to Inverted").
Neither has any effect in Inverted mode: that mode draws the digits as an
opaque magenta *marker* the compositor's shader replaces per-pixel (see
"Text colour: Fixed vs. Inverted" below) — an explicit colour override
would never be seen, and a partial alpha would only dilute the invert
(already documented, before this pass, in that section's "Opaque, not
`text_opacity`-scaled" note). Rather than silently doing nothing, both rows
grey out the moment Inverted is selected, the way every other conditionally-
irrelevant row in this codebase does.

Rendered proof (`build-release/verify-shots/hud-missing-rows-2026-09-07/`):
against a flat `#202020` background, `color_fps` unset measured the digit
fill at `(120, 219, 246)` (the UI accent); setting it to `0xFF3B30` over the
real `overlay_e2_set` path measured `(255, 70, 48)`, matching within
antialiasing tolerance. With that colour held fixed, `text_opacity 1.0`
measured the same `(255, 70, 48)` (fully opaque) and `text_opacity 0.4`
measured `(70, 30, 28)` — markedly pulled toward the background, proving
the slider reaches the render. (The exact blend arithmetic isn't
hand-verified against a formula here — like the Inverted-mode contrast
guard above, this compositor blends in linear light and re-encodes, which a
naive sRGB lerp doesn't reproduce — but the two opacities produce two
clearly different, correctly-ordered results.)

## What is measured — and why it counts instead of timing

The number is the **commit rate of the focused window**: how many frames
the game handed the compositor per second. `commit_t::Signal()`
(`src/commit.cpp`) increments `g_ulAppCommitCount` once per focused-window
commit (the same `m_bMangoNudge` gate that feeds mangoapp), and
`UpdateAndGetDisplayFps()` reads that counter at paint time and computes
`Δcount × 1e9 / Δns` over a window of its own. The pure arithmetic lives in
`FpsDisplay.h`'s `gamescope::fpsmath` and is unit-tested
(`tests/test_fps_counter.cpp`).

> **Why counting replaced timing (2026-09-05, the "999 FPS" bug).** Until
> then the rate came from the per-commit *frametime*
> (`g_ulLastAppFrametimeNs`, `now − last`), of which the HUD consumed at
> most one new sample per paint, clamped to `[0.1, 2000] ms`. steamcompmgr
> drains every finished commit in one loop, and when two or more land in a
> batch — `lsfg-vk` presenting a real frame and a generated one back to
> back guarantees it, and any game briefly out-running the compositor can
> do it — the *last* `Signal()` in the batch measures `≈ 0 ms`, clamped to
> 0.1 ms, i.e. 10 000 fps. Because the HUD only ever saw that last write,
> every mode sat on the old 999 clamp, in Rust for one. A count is immune:
> two commits in a batch are two commits, whenever they arrived.
>
> The frametime is kept for the **lag-spike detector only** (below). Note
> that batching fools it too — a batch of two writes a ~0 ms sample and the
> next real frame then measures about twice the median. Accepted for now:
> it only reacts to that pattern under frame generation, where the
> frametime is already not the game's own.

**Inherent limit to tell the user:** with `lsfg-vk` at ×2 the counter shows
the **generated** rate. The compositor only sees post-frame-gen presents —
the same thing MangoHud shows when loaded after the layer — so the
pre-frame-gen rate is not observable from here.

## Update modes

Two ways to turn the commit count into the number on screen. Both are kept
live in `UpdateAndGetDisplayFps()` regardless of which is selected (the
bookkeeping is two timestamps and two counts), so switching modes never
shows a stale value:

- **Smoothing** (default) — the user's own spec, verbatim: every
  **1000 ms** the window rolls over and `target = Δcount / Δt` is taken;
  the shown value then **glides from what was on screen to `target` over
  300 ms** with `smoothstep`, and **holds for the remaining 700 ms**.
  `DrawReadout()`'s `lround()` walks the digits through every intermediate
  integer during the move. State: `s_flShownFrom`, `s_flShownTo`,
  `s_ulGlideStartNs`; `fpsmath::GlideValue()` / `GlideMoving()` are the
  pure functions. Until the first window completes it shows the Immediate
  value, so the first second is a real reading rather than a made-up 60.
- **Immediate** — the count over the last **100 ms**, republished each time
  that window rolls over. Jittery by design; reacts to a rate change within
  ~100 ms.

**"Update every second" is gone** (2026-09-05): Smoothing now *is* a
one-second windowed rate, with the glide on top, so the old mode had
nothing left to offer. A stored `"per_second"` (or any unrecognised value)
loads as Smoothing — `fpsmath::UpdateModeToInt()`'s fallback rule, the same
legacy-value idiom `ParsePlacement()` uses — so existing configs are not
rewritten and do not break. Covered by `test_config.cpp`'s "legacy
per_second loads as Smoothing".

### Repaint cadence for the glide

A 300 ms movement needs frames. A game at ≥ 60 fps already paints every
vblank, so nothing extra happens in the normal case. For an **idle**
client (paused, alt-tabbed) the HUD's repaint-timer thread
(`EnsureRepaintTimerThread()`) is what drives paints, and since 2026-09-05
it is **adaptive**: it wakes every ~16 ms but only calls `force_repaint()`
every 500 ms — *unless* `s_bGliding` is set, in which case every wake
requests a paint, so the glide gets ~18 frames instead of one.
`s_bGliding` is set/cleared by `UpdateAndGetDisplayFps()` itself (true
while `GlideMoving()` and Smoothing is the selected mode), i.e. by the
paints the thread provokes, and the paint that sees the glide reach its
target clears it.

> **Why off-thread, and why not the lag-spike hold's mechanism:** the
> lesson recorded at the top of `FpsDisplay.cpp` applies — a gate
> evaluated only inside `paint_all()` cannot manufacture repaints on a
> clock nothing is driving. And the lag-spike hold does *not* carry its own
> 700 ms either (an earlier assumption): it relies on the game still
> committing. So the glide's clock lives in the timer thread. A 16 ms wake
> that does an atomic load is cheap; the thing rationed is
> `force_repaint()`, which costs a composite. With an idle client the
> 1-second window itself is only checked at each paint, so the hold can
> run up to 500 ms long there — accepted; the glide itself is still smooth.

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

## Backdrop: removed (2026-09-09)

The HUD used to draw a plain unrounded rectangle behind the number, sized
to the text plus a fixed 6 px padding, at a configurable opacity where 0
meant "no backdrop". The user asked for it to go, so it is gone outright —
the drawing (`DrawModuleBackdrop()`), the "Backdrop opacity" row, and both
schema fields (`backdrop_opacity`, `backdrop_padding`). What opacity 0
already did is now the only behaviour: the HUD draws the digits and their
outline and nothing else.

**A saved backdrop setting no longer does anything.** A profile written by
an older build still carries `"backdrop_opacity"` and `"backdrop_padding"`;
neither is read any more, the file loads unchanged, and both keys disappear
the first time anything writes that profile — the same "just stop reading
an old key" precedent `friends_lookup_names`, `backdrop_enabled`,
`backdrop_rounding` and `shadow_strength` set. Pinned by `test_config.cpp`'s
"a config carrying the removed backdrop keys loads and drops them", which
asserts both halves: the live keys in that same file still load, and the
removed ones are absent after the next write.

> **Why `backdrop_padding` went too, rather than staying at 6 px.** With
> nothing drawn behind the text it had no job left — it existed to give
> the rectangle breathing room around the digits. It was also a pure no-op
> on placement, provably: the readout's box grew by `2 × padding` and the
> draw origin moved in by `padding`, so on a centred axis the two cancel
> (`(display − content − 2p)/2 + p == (display − content)/2`) and on a
> hugging axis `EdgeShift()` cancelled it again by construction. Removing
> it moved no pixel — the margin matrix below measures identically with and
> without it.

> **What the removal cost.** Two things, both accepted. Inverted mode has
> **no lag-spike indication at all** now: it cannot invert an
> already-inverted digit against itself, so its spike reaction was a red
> tint *on the backdrop* — see the Inverted section below. And the number
> has nothing but its outline to separate it from bright, busy content;
> "Outline size" is the control that job belongs to now.

## Text colour: Fixed vs. Inverted

**Fixed** — the UI's own accent colour (`Palette.h`'s `kAccentValue`,
overridable by the "Number colour" row, `color_fps` — see "Number colour and
Text opacity" above; unexposed in the UI until 2026-09-07, though the field
and the render-side read of it existed since issue #29). On a detected lag
spike, the resolved colour is inverted
(`1 - r, 1 - g, 1 - b`) for the hold window — literally "just invert the
text colour", per the user's own spec.

**Inverted** — a **true per-pixel invert** of the game's own colour under
each glyph pixel (Phase 3, 2026-09-03). This used to be a fake — Fixed-mode
colour picking derived from the backdrop instead, documented here as an
honest limitation — but it is now real, implemented one layer down in the
Vulkan compute-composite shader rather than in this file's own ImGui draw
pass:

- This file just draws the digits as **plain opaque magenta**, `(255, 0,
  255)`, and hands the HUD's single layer a new blend mode,
  `rendervulkan.hpp`'s `ALPHA_BLENDING_MODE_INVERT` (`FpsDisplay_AddLayer()`).
  Opaque, not `text_opacity`-scaled — a partial alpha here would only dilute
  the invert, mixing in un-inverted background (see the gating rule below).
  Magenta specifically: its **zero green channel** is the marker the shader
  uses to tell the digits apart from the outline and the crosshair, see
  [What Inverted mode does *not* invert](#what-inverted-mode-does-not-invert).
  (Opaque white until 2026-09-06, when the marker replaced a brightness
  selector — same section.)
- The actual invert happens in `src/shaders/alphamode.h`'s `BlendLayer()`,
  which every composite call site shares (plain blit, FSR/RCAS, both blur
  passes) — one function, one edit, all paths covered. For each pixel:
  `1.0 - c` on the real background colour it's compositing onto, weighted
  by the digit coverage `d` the marker recovers (below), so only glyph
  pixels are touched: `outputValue.rgb = bg * (1 - layerAlpha) + inverted * d`.
  Fully-transparent HUD-texture pixels (`layerAlpha == 0`) pass the
  background through completely unchanged.
- **Contrast guard (perceptual, 2026-09-05)**: a literal invert can land
  too close to the background to read. The shader judges "too close" in
  **encoded (sRGB) space**, because that is the scale the eye compares
  on: it encodes both the background and its true invert
  (`linearToSrgb`), takes each one's Rec.709 luma on the encoded
  channels (`0.2126/0.7152/0.0722`, i.e. Y'), and requires
  `|Y'(inverted) − Y'(bg)| ≥ 0.40` (≈100/255). Where the true invert
  already clears that, it is left **exactly** as it is. Where it does
  not, the inverted colour is moved uniformly in every channel (so
  whatever hue survives is kept) to sit exactly 0.40 from the
  background's Y' on the side *away* from the background — below it when
  the background is brighter than perceptual mid grey (`Y' > 0.5`),
  above it otherwise — then decoded back to linear for the blend. The
  floor is not arbitrary: at `Y'(bg) = 0.5` (encoded 128) the true
  invert's Y' is ~0.896, a gap of ~0.40, so **0.40 is exactly what a true
  invert gives at perceptual mid grey**. Consequences a reader can check
  from one screenshot: every background darker than encoded 128 keeps
  its true inversion untouched (the digit is a light colour ≥ ~0.40
  above it); over anything brighter, the push engages (encoded ~128–229
  for greys) and the digit is a **dark** colour 0.40 below the
  background. The one hard transition in the mapping — light digits
  flipping to dark — sits at encoded 128, perceptual mid grey, where any
  minimum-gap rule must have one. Expected values are tabulated under
  [Verifying Inverted mode](#verifying-inverted-mode-pixel-recipe).

  > **Why perceptual, and why not the linear rule it replaced.** The
  > guard was originally (2026-09-03) computed in *linear* light:
  > `|(1 − bgLuma) − bgLuma| ≥ kMinLumaSeparation`, floor 0.25 then
  > raised the same day to 0.40 from a single on-screen check at
  > encoded 188 (188 inverted to a marginal grey-on-grey 138 at 0.25,
  > to a readable 91 at 0.40). That rule only engages for **linear** luma
  > in `(0.30, 0.70)` — encoded ~149–225 — and the linear invert of the
  > mid-tones that dominate real game scenes lands just outside it:
  > encoded 148 is linear 0.30, its true invert linear 0.70 encodes to
  > ~218, and the guard, seeing a linear gap of 0.40, did nothing. The
  > result over encoded ~120–190 was a faint light grey at ~215–225 that
  > reads as "white text that ignores the background" — reported
  > 2026-09-05 as "the colour inversion doesn't work anymore". A bisect
  > found no regression (identical pixels at `0ff8a55` and HEAD); the
  > defect had been there since the 0.40 floor. Linear-light separation
  > is simply not what the eye judges. Do not revert to a linear rule
  > for "purity": the true inversion is still what you get everywhere it
  > reads (all of the dark half and the very bright end); the guard only
  > touches the band where the true invert *cannot* be read.

  > **Cost.** The guard adds two `linearToSrgb` evaluations (three
  > `pow`s each) plus a `srgbToLinear` (three more) when the push
  > engages — a dozen ALU ops, and only on pixels the HUD texture
  > actually covers: the invert branch returns early where the layer's
  > alpha is 0 (bit-identical to what the blend produced there anyway),
  > so the rest of the frame pays nothing. Not measurable.

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

**Inverted mode has no lag-spike indication (2026-09-09).** It can't
"invert" already-inverted text to signal a spike — doing that would show
nothing against itself — so its reaction used to be a muted warning red
tinted into the backdrop, and only when one was actually drawn. With the
backdrop gone there is nothing left to tint, so a spike does nothing in
this mode; "Lag spike detection" is a Fixed-mode feature now, which its
help text says. That is the second thing removing the backdrop cost (see
[Backdrop: removed](#backdrop-removed)), and the same trade the setting
already made at opacity 0 since 2026-09-03.

### What Inverted mode does *not* invert

Only the **digits' fill**. The outline stays black and is not inverted
(it was, until 2026-09-03), and neither is the
[crosshair](crosshair.md), in whatever colour and opacity it was given.
(The backdrop was a third such element until it was removed, 2026-09-09.)

All of it lives in **one** composite layer, and the shader tells the
digits apart from everything else by a **marker in the texel** (2026-09-06):

- The digits' fill is drawn pure opaque **magenta**, `(255, 0, 255)`, and
  everything that may end up *under a digit's antialiased edge* is pure
  **black**: the outline stamps and the cleared texture. Removing the
  backdrop (2026-09-09) took one such element away — it was drawn pure
  black in this mode, with a green-free spike tint, precisely to satisfy
  this rule — so it can only have made the marker safer, never weaker:
  the set of things a digit's edge can mix with shrank, and everything
  left in it still has `G == 0`. Re-measured after the removal; see
  [Verifying Inverted mode](#verifying-inverted-mode-pixel-recipe).
- ImGui's straight-alpha blend over black or clear therefore leaves a digit
  texel as `d · (1, 0, 1) + (a − d) · black` with **G exactly 0**, where
  `d` is the digit's own coverage and `a` the texel's alpha. So in
  `alphamode.h`'s invert branch: `G == 0` ⇒ digit; the encoded R *is* `d`;
  the pixel becomes `bg · (1 − a) + inverted · d`, the black share
  contributing nothing. That reconstructs the layering **exactly** —
  an edge over the outline is a clean ramp from black to the inverted
  colour (measured: `0, 22, 45, 46` across a stroke over encoded 148 with
  a 2 px outline, back when a 50 % backdrop sat under it too), a full digit texel is exactly the
  inverted colour, and an outline-only texel is exactly the coverage
  blend of black.
- `G > 0` ⇒ not a digit; the texel blends **bit-for-bit as
  `alpha_mode_coverage`** would. That is the crosshair (and, until
  2026-09-09, the backdrop's 12 %-white border). A crosshair colour with no green at all
  (pure red, blue, magenta, black) is nudged from `G = 0` to `G = 1` while
  it shares an Inverted layer (`CrosshairFrame::bReserveInvertMarker`,
  `Crosshair.cpp`'s `ReserveInvertMarker()`) — one count, not visible —
  and every other colour is used exactly as configured.
- **16 bits per channel for that one pairing.** The nudge is stored
  premultiplied, and `1/255 × opacity` rounds back to 0 in an 8-bit
  texture below 50 % opacity — a faint red crosshair would invert. So
  `ResolveTextureFormat()` makes the HUD texture `R16G16B16A16_UNORM`
  exactly when the readout is Inverted **and** the crosshair is on (the
  marker keeps `G ≥ 1/514 × opacity` there, i.e. down to ~0.4 % opacity,
  below which the crosshair adds under half a count anyway), and
  `B8G8R8A8_UNORM` in every other configuration, which therefore pays
  nothing. ImGui's pipeline names its attachment format, so `EnsureTexture()`
  rebuilds it (`ImGui_ImplVulkan_CreateMainPipeline`) on a switch, after
  the previous submission has drained. The bytes in that configuration
  equal what the retired double-height 8-bit texture used.

> **Why a marker, and why this one.** The selector before it (2026-09-03 to
> 2026-09-06) was the texel's own *brightness*: `smoothstep(0.25, 0.80,
> luma)` on white digits, with the backdrop and outline dark. It could
> not tell a white digit from a white crosshair, so the crosshair had to
> leave the layer — the "split mode" below — and that cost a layer slot.
> Any per-texel encoding has to survive ImGui's over-blend, which keeps
> `rgb ≤ a` and mixes the digit's edge with whatever is under it; a
> brightness or ratio test therefore cannot separate a digit's edge over
> the black outline from a grey crosshair of that same value (both are
> `(v, v, v, 1)`), and an 8-bit colour restriction of ±2 counts on the
> crosshair drowns in premultiplied quantisation below ~50 % opacity.
> What *does* survive mixing with black is a **zero channel**: `0 · cov +
> 0 · (1 − cov)` is 0. Hence "G == 0 marks a digit", black for everything
> under a digit, `G ≥ 1` for everything else, and 16 bits so `G ≥ 1`
> stays non-zero after premultiplication. Magenta rather than red so the
> marker reads as a marker (the same `(1, 0, 1)` `composite.h` uses for
> its plane-border debug view) if the texture is ever dumped; the shader
> reads only R and G.
>
> **Why one layer and not two.** This was briefly split across two
> layers — a normally blended backdrop + outline layer with an
> invert-blended glyph-only layer above it — and that **broke Inverted
> mode outright** the same day (2026-09-03): with `backdrop_opacity` at
> its 0.5 default, the lower layer painted a dark box over the game
> exactly where the digits were about to land, so the upper layer
> inverted *the HUD's own backdrop* instead of the game and the digits
> came out a constant near-white. The general rule the split violated:
> **a blend that reads the destination cannot be split across two layers
> that overlap.** Whatever the lower one paints becomes the "background"
> the upper one inverts.

**Known limitation — the readout drawn *on top of* the crosshair.** The
marker only survives mixing with *black*. If the readout is anchored so
its digits overlap the crosshair (anchor `center`, small margins — the
crosshair sits at the game rect's centre), a digit's antialiased edge
pixels that land on a green/coloured arm pick up that arm's `G > 0`,
are classified "not a digit", and composite as their own magenta-tinted
colour: a few magenta fringe pixels along the digit's edge where it
crosses the arm (measured 2026-09-06 on encoded 148 with the default green
crosshair: 35 such pixels in the glyph pair,
`build-release/verify-shots/split-retire/zoom/overlap-mid.png`). The
digit's interior over the arm inverts correctly; the crosshair everywhere
else is untouched. The split mode rendered this configuration cleanly
(separate layers), so this is the trade for the layer slot — accepted
because an FPS number placed on the crosshair is not a configuration
anyone runs on purpose, and every other anchor never overlaps it.
Drawing the crosshair *after* the readout in this mode would move the
fringe to a translucent arm over a digit instead; left as is.

**Layer budget.** The HUD is **one layer in every configuration**, so it
has no special layer-budget behaviour: `k_nMaxLayers` / `VKR_MAX_LAYERS`
are both 6, `LayerStack_t::push()` returns `nullptr` when the frame is
full (and, since 2026-09-06, counts the drop for `layer_budget_stats`),
and the HUD then draws nothing that frame with a rate-limited warning.
`paint_all()` reserves slots for the cursor and the overlay planes but
**not** for the HUD, the toasts or the Shell, so a busy frame (base +
override + external overlay + Steam overlay + cursor + mura is already
six) can lose them.

**The crosshair shares this layer (2026-09-05).** The
[crosshair](crosshair.md) is drawn into this same ImGui frame and texture,
before the readout, and rides the same `Layer_t` — the layer budget above
is exactly why it has no layer of its own. Two consequences for this file:
`FpsDisplay_AddLayer()` builds the layer when the readout **or** the
crosshair is on (and still nothing at all when neither is), and the 500 ms
repaint-timer flag is recomputed (`UpdateTimerFlag()`) from that same
predicate. See
[crosshair.md](crosshair.md#where-it-is-drawn-the-huds-layer).

> **The retired split mode (2026-09-05 → 2026-09-06).** With Inverted text
> colour *and* the crosshair on, the brightness selector of the time would
> have inverted a bright crosshair, so the texture was rendered at twice
> the output height — readout top, crosshair bottom — and two `Layer_t`s
> sampled the two halves (INVERT then COVERAGE, the second's `offset.y`
> the output height). That was one of the six layer slots spent on a HUD:
> measured with both on from startup, the frame's high-water mark was 5
> layers on the split build and is 4 now (`scripts/pixel-regression.sh`'s
> `layer-budget` check pins the 4). The marker above made it unnecessary.

**Interaction with the outline.** None, now. The outline is black, the
selector leaves black alone, and the digits invert the game regardless of
how thick the outline is.

### Verifying Inverted mode (pixel recipe)

A screenshot that *shows* the digits proves nothing about inversion: over a
dark game (vkcube's flat `(51,51,51)`) an inverted digit is `(251,251,251)`
and a plain white one is `(255,255,255)` — indistinguishable by eye, and
the one accepted on 2026-09-04 (`verify-shots/crosshair/16-inverted-hud.png`)
was exactly that. Over a **bright** background the two diverge completely,
so that is the check. On the laptop (`scripts/remote-test.sh`), with a
scratch `XDG_CONFIG_HOME` holding `fps_display.color_mode = "inverted"`,
`outline_strength 0`, `font_size 48`, anchor
`top-center`:

```
gamescope-ritz -W 1280 -H 720 --force-windows-fullscreen -- \
    xterm -bg '#bcbcbc' -fg '#bcbcbc' +sb -e sleep 600
gamescopectl screenshot "/path/shot.png 4"      # one quoted argument
```

then sample the digit core and the flat background beside it. Repeat the
xterm run at `-bg '#949494'` (148) and `-bg '#727272'` (114) — those are
the mid-tones the old guard failed on. Measured 2026-09-05 at `f9e8c84`
(the perceptual guard; Intel/ANV, nested Wayland, captures in
`build-release/verify-shots/inversion-perceptual/`, crosshair off **and**
on — the two were pixel-identical in every run). The "before" column is
what the old linear guard measured at both `0ff8a55` and `773b7e9`
(captures in `build-release/verify-shots/inversion/`); the 219 row is
computed from the rule, not yet measured:

| background (encoded) | inverting digit core (measured) | how | before (linear guard) | not inverting |
|---|---|---|---|---|
| `(51,51,51)` vkcube | `(251,251,251)`, gap 200 | true invert, untouched (gap 0.78) | `(251,251,251)` | `(255,255,255)` |
| `(114,114,114)` xterm `#727272` / vkcube + Shadow Control | `(235,235,235)`, gap 121 | true invert, untouched (gap 0.47) | `(235,235,235)` | `(255,255,255)` |
| `(148,148,148)` xterm `#949494` / vkcube's cube face | `(46,46,46)`, gap 102 | pushed dark to `bg − 0.40` | `(218–221)` — faint | `(255,255,255)` |
| `(188,188,188)` xterm `#bcbcbc` | `(86,86,86)`, gap 102 | pushed dark to `bg − 0.40` | `(90,90,90)` | `(255,255,255)`, or the accent colour |
| `(219,219,219)` | `(117,117,117)` expected | pushed dark to `bg − 0.40` | `(147,147,147)` | `(255,255,255)` |

Every row's gap is ≥ ~100 encoded, so **any** of these backgrounds now
discriminates at a glance; the 148 row is the one that failed before.
Tolerance: ±3 per channel (the blend runs in linear light and is
re-encoded; ANV's `pow` is not bit-exact) — the measured values landed
exactly on the computed ones. Sampling note: take the digit core as the
most common non-background colour inside the glyph's own rows; the
nested window's top edge (a black row and a ~26-row darker band at the
very top of the capture) is not the HUD. Repeat with the crosshair on
(it shares the layer) and, when the user's config is known, with the user's own
settings — the 2026-09-05 report was investigated under FSR + STRETCH +
all three native effects + font 13 + outline 1 + active profile with
auto-save, and every combination inverted identically at the baseline and
at HEAD, on every path (config file, `overlay_e2_set`, the Shell row by
keyboard and by pointer click, the palette's `adjust`).

**This whole recipe is now automated:** `scripts/pixel-regression.sh` runs it
headlessly (a private, invisible sway hosting a nested gamescope, no laptop round
trip) and fails a commit on a regression instead of relying on a human re-measuring
by hand — see `scripts/README.md`'s "Pixel regression" section.

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
> **Why 4px is the ceiling:** the user asked for exactly that ("the max
> outline size should be 4.0"). It used to be justified by
> `backdrop_padding`'s 6px — the outline stayed inside the backdrop box at
> any setting — but with the backdrop gone (2026-09-09) there is no box to
> stay inside, and the range needs no such justification: `EdgeShift()`
> takes the outline's own geometric reach, so a thicker outline pulls the
> digits *in* rather than pushing past the margin. Verified at outline 2 at
> every corner in the margin matrix below.

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
> under Inverted mode's marker (`alphamode.h`'s `alpha_mode_invert`) for
> the same reason plain black already was: black at any alpha has
> `R == G == 0`, so it is never read as digit coverage and never un-marks
> a digit edge drawn over it — no separate handling needed for that mode.

## Lag-spike detection

Switchable (`lag_detection_enabled`, row "Lag spike detection", default
**on** so an existing config keeps today's behaviour). With it **off**
there is no spike reaction of any kind: Fixed mode never flips the
number's colour — which since 2026-09-09 is the whole of the reaction, the
Inverted-mode backdrop tint having gone with the backdrop. Nothing else in
the tab depends on it, so no row greys out when it is off.

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

## No layout jitter: pinned digit width — and no 999 ceiling

The box is sized off a run of `'0'` cells **as long as the current digit
count, never fewer than 3** (`fpsmath::PinnedDigitCount()`:
`max(3, digits(fps))`, capped at 7), measured in Geist Mono, which is
genuinely monospaced — so `'0'` measures the same as any digit and a
fixed cell count is tabular by construction. 0–999 share one 3-cell box
that never resizes as the number goes from 1 to 2 to 3 digits; a
four-digit rate widens it to 4 cells, a five-digit one to 5.

**The `std::clamp(nFps, 0, 999)` is gone (2026-09-05).** It turned the
mis-sampled rate the counting rewrite fixed into a plausible-looking fake
(the "999 FPS in Rust" report), and it turned a genuine 1200 fps — an
uncapped mailbox-mode client on a 60 Hz nested output really does this —
into 999 too. Only the floor at 0 remains.

> **Why the pin follows the number rather than fixing 4 or 5 cells:** a
> fixed 5-digit pin would make the normal 2–3-digit readout sit in a box
> twice too wide. **Why the floor is 3 and not the exact count:** a
> 2-digit reading would otherwise shrink the box every time the game
> dipped below 100, which is the jitter the pin exists to prevent. The
> growth to 4+ cells is therefore the one deliberate resize, and it only
> happens for a reading that needs it.

What's actually *drawn* is the plain unpadded digits (`"%d"`), not the
padded string — drawing a padded string put the leading blank glyph's
advance inside the text draw, which left a visible empty gutter on the
left of a two-digit number and shoved the digits against the box's right
edge (fixed 2026-09-03). `MeasureFpsModule()` measures both the pinned
field and the plain digits and derives `flTextOffsetX`, the width
difference (or half of it for a centre anchor), added to the text origin
so the digits sit at the side of the pinned-width box that faces the
anchor. The outline and the digits themselves both draw at that same
offset origin, so they track together.

**Digits hug the anchor's side, not always the box's centre (2026-09-06
fix).** `flTextOffsetX` used to always be half the pinned-vs-unpadded
width gap, i.e. always centred, regardless of anchor — so with a
right-hand anchor a reading like `60` (2 digits in the pinned 3-cell box)
sat centred rather than flush with the screen edge, and visibly drifted
sideways as the digit count changed (e.g. `60` → `144`). `MeasureFpsModule()`
now takes the anchor's horizontal side (`ParsePlacement()`'s `nHoriz`) and
places the digits flush left, flush right, or centred to match — left and
right anchors flush, centre anchor unchanged. The box itself (size and
`ResolveAnchoredOrigin()`'s placement of it) is untouched by this; only
where the digits sit inside it changed. `fps_display_force "<n>"` (a debug
ConCommand, `-1` or no argument releases it) forces the displayed reading
to an exact integer, bypassing smoothing, for pixel-measuring this without
waiting on a real frame rate — see `build-release/verify-shots/hud-align/`
for the measured before/after edge positions.

## Margin

**Definition:** `margin_x`/`margin_y` is the distance, in pixels, from the
screen edge to the **nearest drawn pixel** of the HUD — whatever is
outermost in the current configuration. With margin 0 that outermost pixel
touches the screen edge exactly; with margin 10 there are exactly 10 blank
pixels between the edge and it. Same rule on both axes, at every anchor.

**Which element the measurement lands on:**

| Outline | Outermost element | How exact |
|---|---|---|
| on | the outline's outer ring | Exact at the ink's true (antialiased) boundary; the *solid* ring starts one AA-fringe pixel further in — see the two-assertion scheme below. |
| off | the glyph ink | Same. |

There is no third row any more. A drawn backdrop used to be one — its
`AddRectFilled` edge took ImGui's `PrimRect` fast path with no antialiasing
fringe at all, so it landed on the margin exactly and pinned it for
everything else — and it was the only configuration with a crisp edge. It
went with the backdrop (2026-09-09), which is why the checks had to change
shape rather than just lose a case; see below.

**The bug (fixed 2026-09-07):** nothing pinned the digits' own ink to the
invisible box at all. They sat inset from it by `backdrop_padding` (6px,
always added whether or not a backdrop was actually drawn; removed
altogether 2026-09-09) plus each glyph's own **side bearing** /
**cap-height gap**: `ImFont::CalcTextSizeA()` measures the ADVANCE box (pen
cell width, full ascent-to-descent line height), not the tight box the
glyph's own ink occupies, and a digit's ink starts a little in from the
left of its cell and stops a little short of the font's full ascent — gaps
`CalcTextSizeA()` cannot see and the old code never corrected for. Measured
2026-09-07 at font size 36, backdrop off, outline off, top-left anchor
(`build-release/verify-shots/hud-margin-2026-09-07/`):

| margin | measured left | measured top |
|---|---|---|
| 0 | 7 | 14 |
| 5 | 12 | 19 |

— i.e. +7px horizontally (`padding(6) + ~1px` bearing) and +14px vertically
(`padding(6) + ~8px` of cap-height headroom the digits' own ink never
reaches). The padding half of that is gone now; the bearing half is what
`EdgeShift()` still cancels. Both **before-fix screenshots showed the readout sitting visibly
off the corner even at margin 0**, when it should have been flush.

**The fix:** `MeasureFpsModule()` measures the true ink bounding box
directly off the font's own baked glyphs — until 2026-09-14 the metric box
`ImFontGlyph::X0/Y0/X1/Y1`, since then the glyph's atlas bitmap through a
visibility floor (see *2026-09-14* below), both via `ImFont::GetFontBaked()`
(public API, no `imgui_internal.h` needed) — for
the pinned `'0'`-run string (the same reference the pinned-width box
sizing already uses, so this stays as jitter-free as that scheme: every
digit shares this font's tabular bearings by construction). On whichever
axis the anchor actually hugs an edge — not the centred axis, which has no
margin claim to satisfy — it shifts the digits by exactly enough to cancel
that bearing (and, until 2026-09-09, the padding with it), so the ink (or
the outline's own outer ring, when one is drawn) lands flush at the margin. The pure arithmetic is
`fpsmath::EdgeShift()` (`FpsDisplay.h`), covered by
`tests/test_fps_counter.cpp`; `MeasureFpsModule()` supplies the font's own
measured bearings and the outline's geometric reach (never less than 1px
once an outline is drawn at all, matching the sub-pixel-radius path's own
whole-pixel ring — see the Outline section above).

**The antialiasing fringe, and how the check stays exact without a
backdrop (2026-09-09).** A rendered glyph's edge is antialiased, so its true
boundary carries a fractional-coverage fringe — a pixel differing from its
flat background by a single count, right where the ink is supposed to
start. Until the backdrop was removed, the backdrop rect was the one
fringe-free case and therefore the one measured at tolerance 0, with the
ink and outline cases allowed 1px.

With every case now a fringe case, the answer was **not** to leave
everything at tolerance 1 — that accepts a real 1px drift in either
direction. `pixel-regression.sh`'s `check_hud_margin()` **splits** the
assertion instead, running both on the same capture:

- **`*-edge`** — the bounding box of every pixel that differs from the flat
  background *at all* (`diff_thresh 0`, so the sub-count fringe counts).
  That fringe pixel **is** the glyph's true geometric boundary, so this is
  asserted at **tolerance 0** — exactly the standard the backdrop rect used
  to meet, it just needed a sensitive enough threshold to see.
- **`*-ink`** — the solid ink at the usual "not the background" threshold,
  one fringe pixel further in, at tolerance 1.

Together they pin *both* ends of the fringe: the ink cannot creep inward
while the fringe alone lands right, and the fringe cannot creep outward.
`tests/test_fps_counter.cpp` pins `EdgeShift()`'s arithmetic exactly, as
before.

**One known 1px case, and why it is left alone.** `MeasureFpsModule()` takes
its bearings from the **pinned `'0'`-run reference**, and a round glyph like
`'0'` carries the font's cap-height *overshoot* — its ink starts about a
pixel higher than a flat-topped `'1'` or `'4'`. So a reading made only of
flat-topped digits (`144`, `1147`, …) sits up to 1px further **in** than the
reference predicts on the vertical axis; `60` and `1234` both contain a
round glyph and measure exactly. The direction is the safe one — the
reference is the *maximum* ink extent, so a reading never pokes *outside*
the margin. Measuring the digits actually drawn instead would fix the pixel
and reintroduce exactly the jitter the pinned scheme exists to prevent (the
readout would bob vertically as the number crossed `144` → `155`), so the
pin stays. Measured and recorded in
`build-release/verify-shots/hud-backdrop-removal-2026-09-09/results.txt`.

**Measured after the fix**, and re-measured after the backdrop removal
(font 36, flat 148 background, `*-edge` — the fringe's own outer pixel):

| margin | measured left | measured top |
|---|---|---|
| 0 | 0 | 0 |
| 5 | 5 | 5 |

Verified across all four corners, all four edge-centre anchors, margins
0/1/5/20, outline off and on, and 2-/3-/4-digit readings — 152 assertions,
148 exact and the 4 flat-digit ones above. **That "exact" held only for the
one font size and the one background it was measured on** (36 px on flat
148 grey, where a faint glyph row still registers as a one-count
*darkening*); on a dark game the same row vanishes and the digits sit 1 px
in from the margin — see *2026-09-14* below. `build-release/verify-shots/
hud-backdrop-removal-2026-09-09/` has the full table, the raw log and
8×-zoomed corner crops at margin 0 and margin 5, with and without the
outline. `scripts/pixel-regression.sh`'s `check_hud_margin()` keeps a
compact, permanent subset of that matrix green: all four corners at margins
0 and 8, outline off and on (the outline-on case replaced the retired
backdrop-on one), plus one 4-digit reading.

### 2026-09-14: the margin is measured to the first row that can be seen

**The report.** The user: the HUD sits 1 px too far from the bottom edge at
a bottom anchor. A font-size sweep (margin 8, outline 0, zero-tolerance diff
of the on-screen capture against a no-HUD capture of the same `dark` scene,
`build-release/verify-shots/fps-hud-bottom-2026-09-14/measurements.txt`)
made it a property of the size, not the edge: sizes 12 and 14 → bottom gap
9 (top exact); 18 and 36 → top gap 9 (bottom exact); every other size from
16 to 72 exact on both. Outline 2 changed nothing about which sizes flip.
In every off case the on-screen ink was exactly **one row shorter** than
`MeasureInkExtent()` predicted, never taller — so not a whole-layer shift
(that would move both edges), but one edge row going missing.

**The cause.** The missing row is a real row of the baked glyph that cannot
survive the composite. The pinned reference `'0'` is round, so it carries
the font's cap-height and baseline *overshoot*; at each size the rasteriser
clips whatever slice of that curve falls into the last pixel row, and at
some sizes that slice is a few percent. Read straight out of the ImGui CPU
atlas (Geist Mono SemiBold, `PixelSnapH`, the shipping configuration —
peak alpha of the `'0'`'s edge rows, out of 255):

| size | top row | next row | last row | row before |
|---|---|---|---|---|
| 12 | 152 | 234 | **15** | 245 |
| 14 | 181 | 253 | **19** | 252 |
| 18 | **2** | 235 | 27 | 254 |
| 20 | 21 | 255 | 41 | 255 |
| 36 | **8** | 249 | 89 | 255 |
| 48 | 189 | 255 | 134 | 255 |

The same happens horizontally: at 26–28 px the `'0'` bakes a left column
of 7, 3 and **0** (an entirely empty column inside its metric box), at
33/40/47/54 px a right column of 3. The metric box `X0/Y0/X1/Y1` includes
all of these.

Such a row is in the HUD's own texture (a GPU readback of
`s_pOverlayTexture` shows it), but in the default configuration — Fixed
colour, no outline — it never reaches the screen. ImGui's straight-alpha
pipeline blends the glyph over the cleared texture with
`(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`, so the texel is stored as *colour ×
coverage* with alpha = coverage; the composite's
`ALPHA_BLENDING_MODE_COVERAGE` (`alphamode.h`) then decodes that colour from
sRGB and multiplies it by the alpha *again*. A white row of coverage `c`
lands on a dark game at roughly `encode(decode(c)·c)`: for 8/255 that is
+0.1 of a count over the scene's 5-grey, for 15/255 −0.3 of a count over
20-grey — both round back to the background. The model reproduces every
capture pixel-for-pixel (at 20 px the `'0'`'s 41/255 bottom row predicts
+10.2 over 5-grey and measures +10; the `'6'`'s 49/255 predicts +15 and
measures +15). The margin was being measured to a row nobody could see.

**Not the compositor.** The previous pass concluded `rendervulkan.cpp`
shifts the layer. It does not: `FpsDisplay_AddLayer()` pushes the HUD at
offset 0, scale 1, and a shift would move both edges. The data — ink one
row *shorter*, on whichever end the overshoot phase made faint — is the
signature of a faint edge row, and the atlas numbers above are that row.

**The rule.** The margin is now measured to the first row / column of the
reference glyph that can *actually show*: one whose strongest pixel can
change what is on screen by at least **1/16 of full scale** over its most
favourable background. That one criterion gives a different coverage floor
per blend path, because the three paths turn coverage into on-screen change
differently (`fpsmath::InkCoverageFloor()`, `FpsDisplay.h`):

| outermost element | blend | on-screen change of coverage `c` | floor |
|---|---|---|---|
| Fixed digits, no outline | coverage blend, colour already × coverage | `encode(decode(c)·c)` over black | **47/255** |
| the outline ring (either mode) | black stamps, straight alpha | measured on white: darkens by about `c` counts (15→13, 27→42, 54→65, 89→119) | **16/255** |
| Inverted digits, no outline | `alphamode.h` recovers `c`, applies it in linear light | `encode(c)` over black | **2/255** |

Why the floor has to depend on the mode: in Inverted mode that same 8/255
top row at 36 px lands near **49/255** on black — plainly visible — because
the invert path applies coverage as a linear-light mixing ratio, which is
what coverage is; only the Fixed path multiplies it in twice. One floor for
all three would either keep placing Fixed digits a pixel in, or start
placing Inverted ones a pixel out. `tests/test_fps_counter.cpp` derives all
three constants from the blend formulae, so a change to either the floors
or the shaders that disagrees with the other fails a test.

**The measurement.** `MeasureInkExtent()` now takes the floor and boxes the
atlas pixels at or above it — `fpsmath::ScanInk()`, a pure function over an
8-bit coverage bitmap (Alpha8 or the alpha byte of RGBA32), unit-tested on
a synthetic glyph with a faint top row, a faint bottom row, an empty left
column and a faint right column. The glyph's atlas rect comes from its UVs
(`ImTextureData::GetPixelsAt()`, CPU pixels ImGui keeps for its dynamic
atlas), and the box is mapped back onto the metric box, which stays as the
fallback for a glyph whose bitmap cannot be read. With an outline the ring
is the same bitmap stamped out by the radius, so its visible edge is the
same floor on the same bitmap and `EdgeShift()` adds the radius as before.

**Measured after the fix** (`build-release/verify-shots/
fps-hud-bottom-2026-09-14/after/`, table in `measurements.txt`): sizes 12,
14, 16, 18, 20, 24, 36 and 48 at margins 0 and 8, outline 0 (dark scene)
and 2 (bright scene — a black ring cannot register on near-black),
top-left and bottom-left, plus bottom-right at 12 and 36. The harness
measures by the code's own rule translated to the screen — a pixel is ink
when it differs from the no-HUD baseline by at least what the floor
coverage produces on that background pixel — and prints the zero-tolerance
gap beside it with the peak change of every excluded row, so the fringe it
discounts is on record: on the dark scene those rows are at most a few
counts. 67 of the 72 visible-rule gaps equal their margin; the 5 others are
the reference-glyph limitation above straddling the floor (the `'6'`'s
bottom row is 49/255 at 20 px where the `'0'`'s is 41, its left column
50/255 at 24 px where the `'0'`'s is 29; the `'0'`'s right column at 12 px
is 17/255 against the ring's 16) — a 7–16-count fringe one pixel off,
listed in `measurements.txt`.

**What is deliberately not exact.** Zero-tolerance diff tools will still
find the excluded fringe one pixel *outside* the margin on backgrounds
where it registers at all (a one-count darkening on mid grey, +1 to +4 on
dark). That is the trade: the row is placed where a person would say the
digits start. `scripts/pixel-regression.sh`'s `check_hud_margin()` used to
assert its `*-edge` box at tolerance 0 on flat 148 grey, where the 36 px top
row showed as exactly that one-count darkening one pixel outside the
margin — not changed in this pass, which owned only the HUD's own files.
**Fixed 2026-09-14 (separate pass):** `*-edge` now applies the same
visibility floor, converted to a pixel-count delta for its own
colour/background via `pixel_regression_sample.py`'s
`coverage_blend_expected()` (`HUD_MARGIN_DIFF_EDGE_DIGIT`/`_OUTLINE` in the
script) — see that script's own comment above `assert_hud_margin()` for the
derivation.

## Warm-up: `FpsDisplay_WarmUp()`

ImGui 1.92 bakes glyphs lazily, per (font, size), and
`ImGui_ImplVulkan_UpdateTexture()` then does a blocking `vkQueueWaitIdle()`
on the first frame that shows a never-before-drawn glyph — the same first-
frame hitch the Shell's startup warm-up (`SettingsOverlay.cpp`, issue #30)
and `Notifications::WarmUp()` exist to pre-pay. `FpsDisplay_WarmUp()`
(2026-09-05) is the HUD's version, meant to be called from the Shell's
startup warm-up block. Its steps, exactly:

1. `EnsureConfigLoaded()`; return if the HUD is **off** (an off HUD
   allocates nothing — this file's standing guarantee; enabling it later
   pays the one-time cost on that first frame, as before) or the output
   size is not yet known (try again next call).
2. `EnsureImguiInit()` → `EnsureTexture(output w, h)`, with the same
   ImGui-context save/restore `FpsDisplay_AddLayer()` uses.
3. One hidden frame: `NewFrame`, `AddText("0123456789")` in the **Hero**
   face at the configured `font_size` — the readout draws digits and
   nothing else, and the fill and every outline stamp use that same face
   and size; the crosshair draws rects only and has no glyphs to warm.
   Opaque colour on purpose: `AddText()` early-outs on alpha 0 and would
   bake nothing.
4. `ImGui::Render()` + `RenderAndSubmit()` — the atlas upload, and its
   `vkQueueWaitIdle`, happen here.
5. **Stop. No `layers.push()`.** Nothing reaches the screen; the next real
   frame's `LOAD_OP_CLEAR` wipes the texture. This is the whole contract: a
   pushed layer would composite the warm-up frame, and a second layer on
   the stack every frame forces the full composite path for every frame
   after.

One-shot once init has been attempted. A later font-size change bakes the
new size on its first frame; that one hitch (on a slider release, not at
game start) is accepted.
