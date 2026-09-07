# Crosshair

A crosshair gamescope draws over the game: four arms, an optional centre
dot, an optional outline, an optional auto-hide while the right mouse
button is held (animated both ways), and an optional per-axis stretch to
match a stretched game. `src/Overlay/Crosshair.{h,cpp}` (config, settings area, right-click
state, the draw), `src/Overlay/CrosshairMath.h` (the geometry and the hide
animation, pure and unit-tested in `tests/test_crosshair.cpp`), config in
`config::CrosshairSettings` (`src/Config/ConfigSchema.h`, JSON key
`crosshair`), settings area `system.crosshair` ("Crosshair" in the rail,
right after the HUD). Default **off**.

> **Why (2026-09-05):** the user runs frame generation externally —
> `lsfg-vk` as a Vulkan layer inside the game's own process. It
> interpolates the game's own frame, so anything drawn *inside* it — above
> all the in-game crosshair — smears between real frames. A crosshair
> gamescope draws is composited **after** interpolation, over the finished
> frame, and cannot smear. See
> [fidelityfx-opticalflow-framegen.md §5.3](../planning/fidelityfx-opticalflow-framegen.md)
> for the layer analysis that makes the compositor the right place for it.

## The settings, top to bottom

`system.crosshair` renders as **two columns** at a wide enough shell width
(Shell.cpp's greedy group packer, `superdoc/architecture/overview.md`'s Shell
notes): **left** Crosshair, Line, Dot; **right** Outline, Auto-hide, Scaling
(2026-09-06, request #2, `requests-2026-09-07.md` item 2 — supersedes the
prior day's "Dot ahead of Line" instruction with a full layout). The row
order within a group, and within a narrow single-column shell, matches this
table top to bottom (`Crosshair_RegisterArea()`). Every row except the
master switch is greyed with a reason while the crosshair is off; each
element's own rows are additionally greyed while that element is off ("the
dot is off", and so on).

> **The declaration order is not the reading order.** There is no per-group
> column API (`Registry.h` packs whole groups into columns by a greedy
> shortest-column algorithm, in declaration order — see
> `superdoc/features/shader-effects.md`'s note on the same mechanism for
> Adaptive Brightness's budget, and `Crosshair.cpp`'s own comment above
> `Crosshair_RegisterArea()`), so hitting this exact two-column split means
> declaring the six groups Crosshair, **Outline**, **Line**, **Auto-hide**,
> **Dot**, Scaling — not the left-then-right reading order. This is fragile
> to a future row-count change in any one group: adding or removing a row
> can tip the greedy balance and silently move a group to the other column.
> Verify with a screenshot after touching any group's row count.

| Group | Column | Row | Config field | Notes |
| --- | --- | --- | --- | --- |
| Crosshair | Left | Show crosshair | `enabled` | Master switch. Default off. |
| Line | Left | Show lines | `line_enabled` | The four arms. |
| | Left | Length | `line_length` | px, 1–64. Each arm's own length. |
| | Left | Width | `line_width` | px, 1–16. **1 is exactly one pixel** — see 1px mode. |
| | Left | Gap | `line_gap` | px, 0–64. Hole = `2*(gap-1)+width`, **always symmetric** (2026-09-08, see [Gap](#gap)) — gap 1 puts the arms right at the crossing's own edges. 0 joins the arms into a solid plus. |
| | Left | Colour | `line_color` | `0xRRGGBB`, the shared RGB colour picker (`CompositeKind::Color`, as `PanelCursor.cpp` uses). |
| | Left | Opacity | `line_opacity` | 0–1. The user's word is "transparency"; the row is labelled Opacity because a slider whose 0 means invisible reads backwards under the other name. `transparency` is a search keyword. |
| Dot | Left | Show dot | `dot_enabled` | |
| | Left | Size | `dot_size` | px, 1–16. Always a **square** — see geometry. |
| | Left | Colour / Opacity | `dot_color`, `dot_opacity` | as for the line |
| Outline | Right | Show outline | `outline_enabled` | |
| | Right | Width | `outline_width` | px, 1–8 |
| | Right | Opacity / Colour | `outline_opacity`, `outline_color` | |
| Auto-hide | Right | Hide while holding right-click | `hide_on_right_click` | |
| | Right | Hide mode | `hide_mode` | Choice. **Stored** in config as a stable string key (`"fade"` / `"focus"` / `"shrink"`, `CrosshairSettings::hide_mode`); the **row is int-backed** like every registry Choice, so `overlay_e2_set crosshair.hide_mode N` takes the option index -- `0` fade, `1` focus, `2` shrink -- and a word is parsed as 0 (fade). `Crosshair.cpp`'s `HideModeToInt()`/`HideModeFromInt()` are the two-way map. |
| | Right | Time to hide | `hide_time_ms` | ms, 0–2000; 0 hides (and comes back) at once |
| | Right | Animate back | `hide_animate_back` | Default **on** (2026-09-06, request #13). Release plays the hide backwards from wherever it was; off restores instantly. See [Auto-hide](#auto-hide-while-holding-right-click). |
| Scaling | Right | Apply scaling | `apply_scaling` | see [Two rendering paths](#two-rendering-paths) |

Pixel sizes are **ints**, not floats: the whole point of the 1px mode is
that "1" is exactly one pixel, so a fractional size has no meaning here.
Every setter persists through `config::EnqueueRoutedWrite()` (per-game
snapshot when one is active, `global.json` otherwise — a normal per-layer
section, copied by `ApplyProfile()` like `fps_display`) and calls
`force_repaint()`, so an edit shows on the next frame even with an idle
game.

## Where it is drawn: the HUD's layer

The crosshair has **no layer of its own**. `FpsDisplay_AddLayer()`
(`src/Overlay/FpsDisplay.cpp`) draws it into the FPS HUD's ImGui frame, into
the background draw list, before the readout, and the HUD's single
`Layer_t` carries both.

> **Why:** `k_nMaxLayers` / `VKR_MAX_LAYERS` is 6, a busy frame already
> fills it (base + override + external overlay + Steam overlay + cursor +
> mura), and `LayerStack_t::push()` fails **silently** when full — see
> [compositing-vulkan.md](compositing-vulkan.md#layer-order-zpos). The HUD's
> layer already spans the whole output and sits above the game and the
> cursor, below the toasts and the Shell, which is exactly where a
> crosshair belongs.

Consequences for the HUD's own logic, all in `FpsDisplay.cpp`:

- The layer exists when the readout **or** the crosshair is on, and not
  at all when neither is (`bReadout || bCrosshair`), so a run with both
  off still never creates an ImGui context, a texture or a layer.
- The 500 ms repaint-timer thread's enable flag (`s_bHudEnabledForTimer`)
  is recomputed by `UpdateTimerFlag()` as that same predicate, so "no
  layer" and "no keepalive repaints" cannot disagree. A static crosshair
  needs no keepalive of its own; counting it costs two idle repaints a
  second while it is on.
- **Sharing an Inverted layer (2026-09-06).** The readout's *Inverted*
  text colour puts the layer in `ALPHA_BLENDING_MODE_INVERT`, whose shader
  (`src/shaders/alphamode.h`) tells the digits apart from everything else
  by a **marker in the texel**: the digits are magenta with `G == 0`, and
  a texel with any green at all composites exactly as coverage would
  ([fps-display.md](fps-display.md#what-inverted-mode-does-not-invert) has
  the whole scheme). The crosshair's only obligation is therefore to never
  put a `G == 0` texel in that layer: `FpsDisplay_AddLayer()` sets
  `CrosshairFrame::bReserveInvertMarker` while the readout is Inverted, and
  `ReserveInvertMarker()` then nudges a colour with no green — pure red,
  blue, magenta, black — from `G = 0` to `G = 1`, on both rendering paths
  (the raster path keys its rebuild on the flag too). One count; every
  other colour is used exactly as configured, and Fixed text colour or no
  readout leaves all of them alone. The HUD renders the shared texture at
  16 bits per channel in that pairing so the nudge survives
  premultiplication at low opacity. Measured: a `(0,255,0)` arm at 50 %
  over encoded 51 is `(35, 99, 35)` in this mode, identical to the
  Fixed-mode/split-mode value (`scripts/pixel-regression.sh`,
  `inversion-crosshair-alpha`), and the outline is `(0, 1, 0)` — the
  nudge. One output-sized texture and **one layer in every combination**.

  > **What this replaced.** From 2026-09-05 to 2026-09-06 the shader chose
  > by *brightness*, a bright crosshair would have inverted the game, and
  > so with Inverted text colour the HUD rendered a double-height texture
  > (readout top, crosshair bottom) and pushed a second `Layer_t` sampling
  > the bottom half — "split mode", one of the six layer slots spent on
  > the HUD. Known limitation of the marker in exchange: a readout
  > anchored *on* the crosshair shows a few magenta fringe pixels where a
  > digit's edge crosses an arm — see fps-display.md's note.

## Geometry

All of it is `crosshair::Build()` in `CrosshairMath.h`; the tests in
`tests/test_crosshair.cpp` pin the exact pixels.

**Centre.** The centre of **layer 0's on-screen rect**, not the output's,
so a letterboxed or offset game still gets the crosshair on the game.
`paint_all()` has already pushed the base plane as layer 0 when the HUD
runs, and `Layer_t::offset`/`scale` *are* the sampling mapping, so the
base's on-screen rect is `[-offset, -offset + tex.size / scale)` in output
pixels (`ResolveCrosshairFrame()`). With no base plane this frame it falls
back to the output centre.

**Apply Scaling off:** every size is an output pixel and the crosshair is
drawn square whatever the game's aspect. **On:** sizes are *game* pixels
and the crosshair is stretched per axis by "output pixels per game pixel"
= layer 0's on-screen size divided by the game's own committed buffer size
— so a 4:3 image stretched to 16:9 gets a horizontally stretched
crosshair, exactly like a stretched in-game one. *How* it is stretched is
the subject of [Two rendering paths](#two-rendering-paths) below. The
game's buffer size comes from `g_uBaseLayerSourceWidth/Height`
(`steamcompmgr.hpp`), published by `paint_window_commit()` from the raw
commit rather than read off layer 0's texture, because that texture may
already be gamescope's pre-emptively upscaled copy
(`ShouldPreemptivelyUpscale()`) whose size says nothing about the game's.
`CrosshairFrame::uGameWidth/Height` carries it to the draw; 0 (no base
plane yet) makes Apply Scaling fall back to the pixel path at scale 1.

**1px mode.** Every primitive — arm, dot, outline — is an axis-aligned
`AddRectFilled` on **whole-pixel** coordinates, drawn with
`ImDrawListFlags_AntiAliasedFill` (and `…Lines`) cleared for exactly those
draws and restored afterwards, so width 1 is one solid pixel with no
half-alpha neighbours. Sizes snap with `lround` after scaling (so under
Apply Scaling a 1px game-space line becomes `round(scale)` output pixels,
still whole); a thickness never snaps below 1; an arm whose length snaps
below 1 is omitted. The centre snaps by parity (`detail::SnapCenter`): an
odd thickness centres on a pixel, an even one on a pixel edge, so every
element is exactly its thickness and mirror-symmetric about the snapped
centre.

### Gap

**Each arm's own inset from the crossing is `(gap − 1)`, for BOTH arms of
an axis, always** (2026-09-08, revised the same day — see below). The hole
across the centre is therefore

    hole = 2 * (gap - 1) + line_width      (for gap >= 1)
    gap 0 -> no hole at all, a solid plus  (unchanged)

which at line width 1 gives gap 0 → 0 px, gap 1 → 1 px, gap 2 → 3 px, gap 3
→ 5 px, gap 4 → 7 px. **This is always exactly symmetric** — the two arms
of an axis are always equidistant from the centre, at every gap value, odd
or even; there is no left/right or top/bottom bias anywhere.

- **Gap 0** — no hole at all: the arms meet and the centre pixel is drawn,
  a solid plus (`Build()`'s pre-existing "join the crossing square" logic,
  unchanged — see below).
- **Gap 1** — the arms sit right at the crossing's own edges; at width 1
  that is exactly **one** pixel missing, the centre pixel itself.
- **Gap *N* (N ≥ 1)** — `2*(N-1) + width` pixels missing, centred on the
  crossing, split evenly on both sides.

**Why this shape (the user, 2026-09-08):** *"Make sure that the crosshair
gaps middle pixel is counted twice. So a gap of 1px actually results in a
single pixel missing in the middle."* — fixed once that same day by making
`crossing::HoleSplit()` split `(gap − crossing_width)` in half per arm
instead of the OLD code adding the raw `gap` value symmetrically on top of
the crossing's own width.

**Why it changed again, same day.** That first fix gave an EVEN gap's
unsplittable remainder pixel to the higher-coordinate side (right on X,
down on Y) — reasonable-sounding on paper, but the user tested it and sent
a picture: a staggered, lopsided crosshair at gap 2 next to a correct,
symmetric one, and rejected the biased version outright. Their own words
for the actual fix: *"Basically like the old formula, just with the gap
with 1 deducted."* That is exactly the formula above — per-side inset =
`gap - 1`, the SAME expression for both arms, so there is no remainder left
to bias one way or the other. `crosshair::HoleSplit()` (`CrosshairMath.h`)
is now a one-line `nLow = nHigh = max(0, gap - 1)` — simpler than the
biased version it replaced, not just more correct.

**This changes what an existing saved gap value looks like**, twice over
one day: pre-2026-09-08 a `line_gap` of *N* drew `2N + width` pixels
(`8` → ~19px at width 3); the first same-day fix briefly drew `N` exactly
(ignoring width, with the rejected bias on even values); the current rule
draws `2*(N-1) + width`. No migration rewrites the stored number at either
step — the value is interpreted under whichever rule is current, so a gap
tuned to match an earlier look will read differently now and may need
raising or lowering to match intent again.

The join-into-a-solid-plus behaviour at gap 0 is untouched by any of this:
`Build()` still draws the crossing square explicitly whenever either axis'
gap is exactly 0 (`HoleSplit()` returns `(0, 0)` for any gap ≤ 1 pixel of
inset, i.e. gap ≤ 1, and Build() only draws the join square at gap exactly
0 — an odd crossing width cannot split into two equal non-negative shares
on its own, and self-tiling would have made one arm's own length one pixel
shorter than its opposite for a *symmetric* gap of 0, which the "arm length
still counts outward from the hole's edge" rule below forbids).

**Arm length still counts outward from the hole's edge**, width, outline
expansion, the dot and the hide-animation phases are all unaffected by this
change — only where the two arms' own near edges sit relative to the
crossing moved.

**1px mode under Apply Scaling** (the pixel-path fallback only, see below):
sizes snap with `lround` after scaling, so a 1px game-space line would
become `round(scale)` output pixels, still whole. The raster path does not
snap at all — that is its point.

**The dot is always a square**, at every size. *Why:* a circle cannot be
pixel-exact at small sizes, the outline/union arithmetic below works
uniformly on rects, and the small square is the conventional shape; a
round dot was not worth a second code path.

**Outline.** `outline_width` px around every arm and the dot, computed as
`expand(all fills) − union(all fills)` — the stroke sits **strictly
outside** the fill and is never drawn underneath it, so a translucent line
shows the game through it, not the outline. *Why the union/difference
decomposition (`crosshair::Decompose`):* with a small gap the arms'
outlines overlap each other and the dot's, and at gap 0 the arms overlap
outright; drawing overlapping rects at a partial alpha leaves visibly
darker squares where two meet. Each element's rects are rebuilt as
non-overlapping bands instead, so every pixel of an element is painted
exactly once. Draw order: outline, arms, dot (the dot's colour wins where
it overlaps an arm).

## Two rendering paths

`Crosshair_Draw()` (`Crosshair.cpp`) has two ways of putting the same
`crosshair::Build()` geometry on screen, chosen by `apply_scaling`:

**Pixel path — Apply Scaling off** (`DrawPixelPath()`). Every rect from
`Build()` is an `AddRectFilled` on whole output pixels with AA off, as
described under Geometry. Nothing about this path changed when the raster
path was added (2026-09-05); its output was measured pixel-for-pixel on
the laptop (`build-release/verify-shots/crosshair/`) and must not move.
It is also the fallback for Apply Scaling *on* when the game's buffer size
is unknown, or when the raster texture could not be created — then sizes
are scaled per axis and snapped, the pre-2026-09-05 behaviour.

**Raster path — Apply Scaling on** (`ComputeRasterPath()` +
`Crosshair_RecordUpload()`). "Render it normally, then scale it": the
crosshair is `Build()` at the **game's** resolution
(`crosshair::GameFrame()`: scale 1, centre = the game buffer's own centre,
so parity snapping behaves exactly as an in-game crosshair at screen
centre of a buffer that size would) with the pixel path's own geometry —
same gap, length, width, outline, dot — rasterised on the CPU
(`crosshair::Rasterize()`) over just its bounding box plus a one-texel
transparent margin (`crosshair::RasterRect()`, `kRasterMargin = 1`), then
stretched to the output by a **CPU bilinear resample**
(`crosshair::ResampleToOutput()`, placed by `crosshair::ScaledQuad()`) at
layer 0's per-axis scale, and **copied straight into the HUD texture**
ahead of the HUD's render pass. Nothing of it goes through ImGui.

> **Why a copy and not an image quad (2026-09-06, request #14).** From
> 2026-09-05 to 2026-09-06 the raster was a small `B8G8R8A8` texture drawn
> as one `AddImage` quad through ImGui's linear sampler. The stretch itself
> was right — at 2x the arm started at output 658 = 642 + 8·2 and ran 24
> px — but everything ImGui draws goes through its `SRC_ALPHA /
> ONE_MINUS_SRC_ALPHA` blend onto the cleared texture, so a sampled edge
> texel of coverage *w* landed **premultiplied**, `(c·w, w)`, and the
> composite — which reads a HUD texel as *straight* alpha (the [Known
> limitation](#known-limitation-pre-existing-shared-with-the-hud) below) —
> showed it at `c·w·w`. A 1 px game line stretched 2x is two rows at 75 %
> coverage and two at 25 %; through that blend they came out at 56 % and
> 6 %: measured `(23, 169, 23)` for a `(0, 255, 0)` line over `(51, 51, 51)`
> on the core rows, the fringe rows nearly invisible, the arm's 56 %-covered
> end pixels dim enough that the gap read a pixel too wide on each side.
> That is the user's *"doesn't properly get thicker and sub-pixel blurry
> … the gap doesn't work right"*: a dimmer, thinner line, not a stretched
> one. No encoding of the source texels can undo it — the blend multiplies
> RGB by the *interpolated* alpha, and Vulkan clamps the fragment's colour
> to [0, 1] before blending on a UNORM target — so the only way past the
> blend is to not go through it. The resampled texels are written into the
> HUD texture as they are: RGB = the colour the pixel path's blend would
> have written for that element (colour × opacity — the pixel path's
> quirk, kept so "normally" means *exactly* the unscaled look), A =
> bilinear coverage × opacity. The composite then shows a soft edge as
> `colour·w + background·(1−w)` in linear light, which is what the game's
> own scaler does to an in-game crosshair. Measured after the change
> (`scripts/pixel-regression.sh crosshair-scaled`, 640×360 client stretched
> 2x onto 1280×720, width 1, gap 8, length 12): arms **24** long, inner
> ends **30** apart (hole = `2*(8-1)+1` = 15 game pixels × scale 2,
> [Gap](#gap)'s current formula, scaled by the same factor as everything
> else — a pre-2026-09-08 build measured 34 apart here (`2·16 + the 2px
> centre column`, the OLD per-side formula), and the first same-day
> 2026-09-08 attempt measured 16 (`gap × scale` directly, ignoring width)),
> **2** wide by ≥ 50 % coverage, with **0.25** coverage beside each arm and
> **0.19** (= 0.25 × 0.75) past each end — the bilinear model exactly, and
> the two arms equidistant from the centre (`scaled_axis`'s own symmetry
> check).

> **Why linear, not pixel-snapped** (the user, 2026-09-05: *"it should
> blur a bit and mix colors, instead of being just perfect pixels"*): the
> reference is a stretched in-game crosshair. That is a raster at game
> resolution stretched with the frame by the scaler, so a 1 px game line
> spans ~1.5 output px *softly* on a 4:3→16:9 stretch, and the outline's
> black mixes into the fill's green at the edge. A vector re-drawn at
> output resolution and snapped to whole pixels (what Apply Scaling did
> before) is crisp, which is exactly *not* the stretched look. Rendering
> at game resolution and stretching with a linear filter reproduces the
> reference by construction.

Machinery, and why this much and no more:

- **The game-resolution raster** (`crosshair::Rasterize()`): straight-alpha
  `0xAARRGGBB` texels (`crosshair::PackArgb`; the little-endian memory
  order of B8G8R8A8), painted outline → arms → dot with the dot composited
  *over* (`detail::Over`), matching the vector path's `SRC_ALPHA` blend,
  at each element's configured opacity × the hide fade. The colour bleed
  into transparent neighbours it also does is harmless now and kept only
  because the tests pin it: the resample below weights RGB by alpha, so a
  transparent texel's RGB never reaches an edge.
- **The resample** (`crosshair::ResampleToOutput()`): for every output
  pixel of the quad's footprint (`ScaledQuad()`, floored/ceiled to whole
  pixels), a four-tap bilinear at texel coordinate
  `u = (ox + 0.5 − q.x0) / scale − 0.5` — texel *k*'s centre at `u = k`,
  the same mapping the GPU sampler used — done **premultiplied** (each
  tap's weight carries its alpha) and un-premultiplied at the end. The
  source value per tap is the pixel path's *HUD texel* for that element,
  `(c·a, a)`, so the output is `RGB = c·a`, `A = coverage·a`: an interior
  pixel of a translucent line is the pixel path's exact texel and an edge
  pixel of an opaque line is `(c, coverage)`. Texels outside the raster are
  transparent. Cost: the footprint is the crosshair's size in output
  pixels (a few thousand texels), and it is only recomputed when the key
  changes.
- **The copy** (`Crosshair_RecordUpload( cmdBuffer, hudTexture )`, called
  by `FpsDisplay.cpp`'s `RenderAndSubmit()` after `DrainPrevSubmission()`
  and after the HUD texture's initial layout barrier, before
  `vkCmdBeginRendering`): when this frame's `Crosshair_Draw()` produced a
  raster, it is encoded into a small **host-visible staging buffer this
  file owns** (`EnsureStaging()`, grown in powers of two from 64 KiB, never
  shrunk; `B8G8R8A8` as-is, `R16G16B16A16_UNORM` as each channel × 257
  when the HUD is 16-bit — see the Inverted note above), rewritten only
  when the pixels or the format changed (the drain guarantees the previous
  submission, the buffer's last reader, is done), and the command buffer
  gets: a `COLOR_ATTACHMENT_OUTPUT → TRANSFER` image barrier (that source
  stage is the one the HUD's Issue-#22 semaphore wait is attached to, so
  the clear cannot start before the previous composite has finished
  reading the texture — `TRANSFER` alone is not in that wait mask), a
  `vkCmdClearColorImage` of the whole texture, a `TRANSFER → TRANSFER`
  barrier, a `vkCmdCopyBufferToImage` of the footprint clipped to the
  texture (`bufferRowLength` = the unclipped width), and a `TRANSFER →
  COLOR_ATTACHMENT_OUTPUT` barrier. The hook returns **true** and
  `RenderAndSubmit()` then opens its render pass with `LOAD_OP_LOAD`
  instead of `CLEAR`, so the readout still draws **over** the crosshair
  exactly as it does on the pixel path; a frame without a raster records
  nothing and the pass clears as before. The HUD texture carries
  `bTransferDst` for this. Why its own staging buffer and not
  `g_device.uploadBufferData()`: that bump allocator is only reset by a
  device wait, and with the fade baked into the texels an animation
  re-uploads every frame. No texture, no descriptor set, no ImGui object
  is owned any more.
- **Hide animation.** Every mode changes the pixels now (the fade is baked
  in, there is no quad to tint), so all three rebuild per animation frame:
  a few thousand texels, ≤ 2 s.

**Re-render policy.** `RasterKey` holds everything the *pixels* depend on:
the element switches, every size, gap, colour and opacity, the hide
state's gap, length **and alpha** multipliers, the game's buffer size, and
the frame's centre and per-axis scale (the output raster depends on where
it lands). The raster is recomputed and re-encoded only when the key
differs from the last frame's; a static crosshair costs one clear + one
small copy a frame and no CPU work.

**The pixel-centre mapping.** `composite.h`'s `sampleLayerEx` samples
layer 0 at texel `t = (o + offset) * scale` for output position `o`, so a
game pixel `g` sits at `o = g * s + origin`, where `s` is "output px per
game px" (`CrosshairFrame::flGamePixelScale`) and `origin = -offset` is
the game rect's top-left. `ResolveCrosshairFrame()` hands over the rect's
*centre* rather than its origin, and the centre is `gameW/2` game pixels
in, so `origin = centre − (gameW/2)·s`. Texel `k` of the raster is game
pixel `texRect.x0 + k`, so the quad is
`[origin + texRect.x0·s, origin + texRect.x1·s)` per axis — and then texel
centres `(k + 0.5)` land exactly where the composite puts game pixel
centres `(texRect.x0 + k + 0.5)·s + origin`. There is no half-pixel term:
ImGui and the composite both treat integer coordinates as pixel edges
(pixel *i* covers `[i, i+1)`), so the identity is exact. A half-pixel
error here would read as a blur *offset* rather than a blur — the test
"ScaledQuad puts every raster texel centre on the game pixel centre the
composite samples" pins the mapping against the `sampleLayerEx` inverse
for both a stretched and a letterboxed layer.

## Auto-hide while holding right-click

The animation is a progress `f`, 0 (shown) … 1 (hidden), driven by an
**integrator** (`crosshair::HideAnim` / `AdvanceHide()`, 2026-09-06): while
the button is held `f` climbs at `1 / hide_time_ms` per ms; released, it
descends at the same rate with **Animate back** on, or snaps to 0 with it
off. `hide_time_ms ≤ 0` means at once, both ways. The button edges carry
their timestamps (one atomic word: bit 0 = held, the rest the edge's ns),
so the first frame after a press covers exactly `now − press` — the same
value the old `HideProgress()` (still there, still tested) gave — and a
release seen a frame late is accounted from its own instant. Multipliers
(`crosshair::EvaluateHide`), applied before `Build()`, with `p` the phase
split below:

| Mode | first phase | second phase |
| --- | --- | --- |
| **Fade out** | `alpha = 1 − f` | (same, continuous) |
| **Focus** (`p = 0.5`) | `gap = 1 − 2f`, alpha 1 | gap 0, `alpha = 2 − 2f` |
| **Shrink** (`p = gap / (gap + length)`) | `gap = 1 − f/p` | gap 0, `length = 1 − (f − p)/(1 − p)`, **dot size × the same** |

`alpha` scales every element's opacity, outline included. In **Shrink**
the dot shrinks with the arms over the second phase — a dot left behind
would defeat the point of hiding (the in-game scope has its own reticle).
At `f = 1` nothing is drawn.

**Shrink's phase split** (`crosshair::ShrinkSplit()`, 2026-09-06, request
#11). *Why:* with a fixed 50/50 split the gap closed at `gap / (T/2)` px/s
and the arms shortened at `length / (T/2)` px/s, so with the defaults (gap
3, length 6) the second phase ran twice as fast as the first — the user's
*"the gap part moves half as fast as the shrinking lines part"*. The split
is now `gap : length`, which makes the **visible edge move at one speed**
throughout: the inner end travels `gap` px in the first phase, the outer
end `length` px in the second, each in time proportional to its distance
(`(gap + length) / T` px/s overall; a straight line of travel against
time, pinned by the tests). No gap → the whole time shrinks the arms; no
length → the whole time closes the gap. Focus keeps 50/50: its second
phase is a fade, there is no edge speed to match. Measured
(`scripts/pixel-regression.sh crosshair-shrink-rate`, gap 8, length 12,
4000 ms): gap rate **5.00 px/s**, arm rate **5.00 px/s**; at 50 % the gap
is 0 and the arms are 10 of 12 px, on the model.

**Animate back** (`hide_animate_back`, default **on**, 2026-09-06, request
#13: *"an option for the auto-hide animations to reverse again, so it
doesn't just instantly pop in again"*). On release the same animation
plays backwards **from wherever it was**, at the same rate — a release at
50 % plays the first half of the hide in reverse; a press during that
reveal resumes the hide from the reveal's current progress; there is never
a jump in either direction. Every mode, since it is the one `f` that runs
back. *Why on by default:* the user asked for the reversal and there is
one switch, not a second time slider — the mirrored time read right in the
captures, so a separate "Show time" was not worth a row. *Why the switch
exists at all* (the pre-2026-09-06 rationale, now the **off** case): the
moment the player comes off the sights they may want the crosshair back
to re-acquire, and an ease-in there is the one place a delay is felt.
Measured (`crosshair-reverse`, Shrink, 2000 ms, released at 1002 ms,
captured at 1503 ms): gap **3** of 8, length 12 — `f = 0.25`, exactly the
model — and fully back (gap 8) at 2603 ms.

**Repaints.** `Crosshair_Draw()` returns "still animating"
(`crosshair::HideAnimating()`: `f < 1` while held, `f > 0` while released),
and `FpsDisplay_AddLayer()` then calls `force_repaint()` for one more
frame — per frame, the way the HUD's lag-spike hold does, but at frame
rate. Idle, fully hidden or fully back, it asks for nothing. The press and
release themselves call `force_repaint()` too, so the animation starts
(and the reveal begins) on an idle game without waiting for it to commit.

### The right-click hook and its gating

`wlserver_dispatch_mouse_button()` (`src/wlserver.cpp`) is the single
fork between "the Shell/Launcher is capturing input" and "this goes to
the game". On the **game** branch only, after the seat notify, a
`BTN_RIGHT` press calls `Crosshair_NotifyRightButton(true)` and the
matching release `…(false)`. The event itself is neither consumed nor
altered — the game gets it exactly as before. Because releases are paired
to the press's destination by the existing tracking set
(`s_setMouseButtonsForwardedToGame`), a right button pressed in the game
and released after the Shell opened still restores the crosshair, and a
click captured by the Shell never starts a hide. The hook runs on the
wlserver thread and touches one atomic (the latest edge: bit 0 = held,
the rest its timestamp; only the first press of a hold is recorded) plus
`force_repaint()`; it never reads the config cache — what a release
*means* (reverse, or instant) is the render side's decision, which reads
the atomic and the config together.

## Verification

Nothing here needs OS-level input injection (banned — see
`AUTONOMOUS-DECISIONS.md` D4): `wlserver_debug_mouse_button` (a sibling of
`wlserver_debug_key` / `wlserver_debug_mouse_motion`, `src/wlserver.cpp`)
enters at `wlserver_mousebutton()`, the exact function every real backend
uses, so it reaches the capture gate, the game's seat and this hook like a
real click. Through `gamescopectl` the arguments are **one quoted
argument** (`273` is `BTN_RIGHT`):

```
gamescopectl wlserver_debug_mouse_button "273 1"     # press: hide starts
sleep 0.1
gamescopectl wlserver_debug_mouse_button "273 0"     # release: instant restore
```

Live checklist (the lead runs this on the laptop; the geometry tests cover
the arithmetic):

- Centre on the game rect: a letterboxed client (`-w 1280 -h 720 -W 1920
  -H 1080`, or any non-matching size) gets the crosshair on the *game*, not
  the output centre.
- 1px: width 1 is exactly one pixel, no anti-aliasing fringe, in a zoomed
  capture (Apply Scaling off, or on with the game drawn 1:1).
- Apply Scaling: a 4:3 game on a 16:9 output with `--scaler stretch` (or
  any per-axis stretch) gives a horizontally stretched crosshair; off keeps
  it square. **On**, an 8× zoom of a width-1 line with the outline on shows
  **soft** edges spanning ~1.5 px horizontally, with dark-green
  intermediate values between the black outline and the green fill; **off**
  the same capture is pixel-exact, identical to
  `build-release/verify-shots/crosshair/03-outline-zoom.png`. The centroid
  must not shift between the two modes.
- Hide modes still animate with Apply Scaling on (all three rebuild the
  raster per frame).
- Hide modes at 100 % of Time to hide: all gone (dot included). Focus at
  50 %: gap closed at full opacity. Shrink at `gap / (gap + length)` of the
  time: gap closed at full length; the arms then shorten at the same px/s
  the gap closed at.
- Release with Animate back on: the animation runs backwards from where it
  was, no jump; a press mid-reveal resumes the hide from there. Off:
  instant, no fade-in.
- No hide while the Shell is open (the right-click goes to the Shell, not
  the game); a press made in the game and released after opening the Shell
  still restores.
- Colours and opacity per element; the outline sits outside the fill and a
  translucent line shows the game, not black, through it.
- Inverted HUD text colour + crosshair: the digits still invert the game
  and the crosshair keeps its own colour, at full and at partial opacity
  (they share one layer). **Pixel-sample it; do not eyeball it.** `verify-shots/crosshair/16-inverted-hud.png`
  (2026-09-04) showed a bright digit over vkcube's dark grey and was
  accepted as proof — it proved nothing, because over a dark background an
  inverted digit and a plain white one look the same, and the user then
  reported "inversion doesn't work" the next day. The recipe that does
  discriminate is in
  [fps-display.md](fps-display.md#verifying-inverted-mode-pixel-recipe):
  a bright flat client, and the digit core must come out *dark*.

**Automated:** `scripts/pixel-regression.sh` runs the inversion-plus-crosshair
pixel-sample above, the crosshair's own arm/gap/outline geometry, the [Gap](#gap)
invariant AND its symmetry at width 1 and 2 / gap 0–4 / outline off and on
(`crosshair-gap-invariant`), and the same invariant plus symmetry scaled by
Apply Scaling (`crosshair-scaled`, `crosshair-scaled-gap`), headlessly on
every run — no laptop, no eyeballing. See
`scripts/README.md`'s "Pixel regression" section.

## Known limitation (pre-existing, shared with the HUD)

The ImGui Vulkan backend blends `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` onto a
texture cleared to `(0,0,0,0)`, i.e. the HUD texture ends up
**premultiplied**, while the layer composites as
`ALPHA_BLENDING_MODE_COVERAGE` (straight alpha). A pixel at opacity `a`
therefore lands at `c·a·a + bg·(1−a)`, slightly darker and thinner than
`a` says. This is the HUD's existing behaviour (its backdrop at 0.5 has
always been affected the same way); the crosshair's opacity sliders
inherit it. Switching the layer to `ALPHA_BLENDING_MODE_PREMULTIPLIED`
would fix both but changes the HUD's look, so it is left for a deliberate
HUD-level decision. The raster path (Apply Scaling on) reproduces it on
purpose for *opacity* — its texels carry `colour × opacity` as the pixel
path's blend would have written them, so the two paths agree at any
opacity — but its stretched **edges** do not go through the ImGui blend
at all (they are copied into the texture as straight coverage, see the
raster path's *Why a copy* note), which is what makes them soft instead
of crushed. The exact value is pinned by `scripts/pixel-regression.sh`'s
`inversion-crosshair-alpha` check (`pixel_regression_sample.py`'s
`coverage_blend_expected()` is the arithmetic): a `(0,255,0)` arm at 50 %
over encoded 51 measures `(35, 99, 35)`, not the `(35, 190, 35)` an ideal
half-blend would give — so whoever makes that decision changes that
check's formula in the same commit, with the new measurement, and drops
the `× opacity` from `ResampleToOutput()`'s RGB in the same breath.
