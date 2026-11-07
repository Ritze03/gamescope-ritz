# Shader effects: Bloom and the Brightness Map history

Bloom (the spatial effect with its own dispatches) and the removed Brightness Map experiment. Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

### Bloom (`image.shaders.bloom`) — NEW 2026-09-08

The user's request, verbatim: *"Add a bloom shader for more casual games"*. A glow
around bright areas — aimed at looking good rather than at competitive clarity, so it
ships **off** and its help text says what it is for.

**Config**: `ReshadeBloomSettings` (`ConfigSchema.h`) — `enabled` (false), `threshold`
(0.75), `intensity` (0.8), `radius` (0.5). Purely additive keys, so
`kCurrentSchemaVersion` stays **4** and there is no migration: an old profile has none
of them and takes these compiled-in defaults, exactly the shape
[Shadow Control](shader-effects-color.md#shadow-control-imageshadersshadow_lift) and
[Adaptive Gamma](shader-effects-adaptive-gamma.md#adaptive-gamma-imageshadersadaptive_gamma--new-2026-09-08) were added
in.

**This is the first effect here that is not a per-pixel function.** Every other one is
`f(pixel)`; bloom needs to know what is *near* a pixel, which is why it is the only one
that adds dispatches and memory rather than a flag bit and a uniform.

#### The pipeline: three extra dispatches, one ping-pong pair

```
layer0.tex (game, source res)
   │
   ├─► cs_effects_bloom_down.comp   ─► bloomA   (W/8 × H/8)
   │      grade() + bright pass per tap, 8×8 box mean per output texel
   │
   ├─► cs_effects_bloom_blurh.comp  bloomA ─► bloomB   (17-tap Gaussian, x)
   ├─► cs_effects_bloom_blurv.comp  bloomB ─► bloomA   (17-tap Gaussian, y)
   │
   ▼
cs_effects_layer0.comp  ── samples bloomA bilinearly, composites (below)
```

- **Dispatches added: 3.** They are recorded only on frames Bloom is on; `bBloom` gates
  the recording itself, not just a uniform.
- **Memory added: two `ABGR8888` textures at ⌈W/8⌉ × ⌈H/8⌉.** At 1920×1080 that is
  240×135×4 = 130 KB each, **259 KB** for the pair; at 2560×1440, 461 KB. Pooled like
  `effectsOutput`: re-created only when the base layer's source size changes, never
  freed.
- **Work added, in taps.** The bright pass reads every source pixel exactly once (an 8×8
  box, 64 `texelFetch`es per glow texel), so it roughly doubles the pre-pass's
  full-resolution texture reads. The two blur passes are 17 taps each over 1/64 of the
  frame's pixels — together about **0.53 taps per source pixel**. The composite adds
  four cached fetches of a tiny texture and three `pow`s per pixel.
- **`Why 1/8 and not 1/2 or 1/4:`** the blur is what costs, and it costs per glow texel,
  so each doubling of the reduction takes three quarters off both blur passes. A glow has
  no high frequencies to lose, which is what makes it the one effect here that can be
  computed small. 8 is the largest reduction that still leaves the falloff smooth: the
  buffer's texels are already an 8-pixel box, the smallest σ the slider can ask for is
  one texel, and the bilinear upsample interpolates between values that differ by less
  than the eye resolves.
- **`Why the bright pass still reads every source pixel:`** a sparse sampling would make
  a small bright object appear and disappear as the grid slid over it under camera
  motion — the shimmer the threshold's own shape exists to avoid, reintroduced at a
  different scale.
- **`Why separable, and why two shaders instead of one with a direction uniform:`** a
  17×17 two-dimensional kernel is 289 taps per texel against 34 for two passes. The
  direction cannot be a uniform because `effects_t` is uploaded **once** per frame and
  read by every dispatch of the pre-pass; making it a uniform would mean three uploads a
  frame and a field whose value differs between dispatches documented to share one. Two
  three-line files including `effects_bloom_blur.h` is the shape
  `cs_composite_blur.comp` / `cs_gaussian_blur_horizontal.comp` already uses.
- **`Why 8-bit glow buffers:`** `dst` in `descriptor_set.h` is declared `rgba8` and
  `dispatch()` binds one RGB target, the same constraint the history texture works
  under. What it costs: a contribution below 1/255 quantises to zero — in practice a
  single isolated bright pixel inside an 8×8 block, whose glow would be invisible anyway.
  Above that, the bilinear upsample interpolates *between* quantised texels, so the
  falloff the user sees is continuous rather than stepped and there is no banding to
  trade against.

**No GPU-timestamp instrumentation exists in `vulkan_composite()`**, so there is no
measured microsecond figure for any of this and none is claimed — the same statement
Local adaptation's cost section makes. What can be said from the tap counts is that the
added work is one extra full-resolution read of the base layer plus about half a tap per
source pixel, on a pre-pass that is already sub-millisecond on this desktop's GPU — so
the figure below is an **estimate from the tap counts, not a measurement**, and is
labelled as one rather than invented. Nothing in the headless gate's wall-clock timing
can substitute for it: that run is dominated by its own sleeps.

#### Where it sits in the order, and why

**After Pre-Sharpen, before both adaptive effects**:

> Shadow Control → Saturation → Vibrancy → (Pre-Sharpen) → **Bloom** → Adaptive
> Brightness *or* Adaptive Gamma → `saturate`

- **After `grade()`**, so the glow is built from the same graded colour the picture
  carries — a saturation-boosted red lamp glows the boosted red. Gating on the *ungraded*
  luma would also make the Threshold slider mean a different brightness than the one on
  screen, because Shadow Control moves luma.
- **After Pre-Sharpen**, but the glow's own source is *not* sharpened: a 5-tap cross is
  invisible after an 8× box reduction, which is the same argument
  `cs_effects_preview.comp` and the measure pass already make for skipping it.
- **Before the adaptive block**, for two reasons that are about this pipeline rather than
  about the textbook. First, **there is no feedback**: `cs_effects_measure.comp` reads the
  *raw* base layer and grades its taps with `grade()` alone, so the statistics never see
  the glow and a bloomed frame can never turn its own bloom down. Second, the adaptive
  operators then treat the bloomed picture as *the* picture — the glow scales with the
  exposure they choose instead of floating on top at a fixed level, and Dynamic's shoulder
  rolls the added highlights off for free.
- **On the textbook.** Bloom conventionally happens before a tone curve, and it does here:
  the tone curve is the adaptive block. The other half of the convention — "in linear
  HDR" — this pipeline does not have, by DECISIONS.md #15 and the encoded-space decision,
  and that is exactly what shapes the bright pass below. There is nothing above 1.0 to
  use as a natural "this is emissive" line.

#### The bright pass: why the threshold is not a gate

In an HDR renderer a bloom threshold has an unambiguous meaning — only values above 1.0
emit, and an ordinary lit surface never reaches one. Here every pixel is already inside
[0, 1], so a plain "above this, glow fully" gate makes a bright wall emit exactly as hard
as a lamp, and a scene that is mostly bright turns into uniform haze. Arithmetically, at
the shipped threshold of 0.75, a 200-code surface contributes **2 %** of its colour under
the weight below and would contribute **100 %** under a gate whose knee ended beneath it.
The measured consequence of the shipped weight is in the tables further down: the `bright`
scene's 30-code shadow rectangles read **48** at the defaults rather than being washed most
of the way up to the bands around them.

```
x = clamp( (luma - threshold) / max(1 - threshold, 0.001), 0, 1 )
w = x * x
contribution = colour * w            // per tap, before the 8x8 box mean
```

- `w` is **zero and flat at the threshold**. That is where the shimmer question is
  decided: a pixel wandering across the boundary under a pan must not change its
  contribution abruptly. A hard gate makes that first step 1.0 and a linear ramp makes it
  `1/(255·headroom)`; the square makes it at most a **twentieth** of the linear ramp's
  anywhere in range. That factor is the anti-shimmer margin and is what
  `tests/test_effects_curve.cpp` asserts, rather than an absolute number that would drift
  with the threshold.
- `w` still **rises right up to white** — no plateau. A smoothstep would be flat at both
  ends, which would make a 250-code pixel and a 255-code one emit identically and throw
  away the "brighter things glow more" the effect is about.
- **Gate per tap, average after.** Gating the block *mean* would make a single 250-code
  lamp inside an otherwise black 8×8 block fall under any useful threshold and vanish,
  while a uniformly mid-grey block sailed over it.
- **At threshold 1.0 nothing glows, at any luma** — the right meaning for the top of that
  slider, and what the guarded divide produces rather than a NaN.

#### The composite: how it is kept from clipping

Adding light to bright pixels is the one operation in this pipeline that naturally blows
highlights out. It is prevented by the *shape of the operator*, not by a clamp afterwards:

```
out = 1 - (1 - base) * (1 - glow)^intensity          // effects_curve.h's bloom_apply
```

- At `intensity = 1` this is exactly the familiar screen, `base + glow(1 - base)`.
- For `base, glow ∈ [0, 1]` and any `intensity ≥ 0` the result is in `[base, 1]`, and it
  is **strictly** below 1.0 wherever `base` was — so a bright source below 255 stays
  below 255, at every setting of every knob.
- It is **linear in intensity where the glow is faint** — `(1-g)^k ≈ 1 - kg` — so on a
  dark field with a small light, the regime the effect actually lives in, the slider
  behaves like a plain gain. It saturates instead of clipping only where the glow is
  strong.
- `intensity = 0` is an exact identity for every glow, and `glow = 0` an exact identity
  for every intensity.

**`Why not the obvious screen(base, glow × intensity)` — it clips.** That was built
first. Above intensity 1 the product runs past 1.0, the clamp that has to follow pins it
there, and every neighbourhood whose blurred glow exceeds `1/intensity` goes to pure
white. Quantified from the shipped form's own captures rather than from a second gate run
— the blurred glow at each sampled region is recovered by inverting `bloom_apply` on the
measured output, then both composites are evaluated on it:

| `bright` scene region | recovered glow | shipped `(1-g)^k` | `screen(base, g×k)` |
| --- | --- | --- | --- |
| 245 band, Intensity 2.0 | 0.684 | **254** | **255 — clipped** |
| 200 band, Threshold 0 + Intensity 2.0 | 0.484 | 240 | 253 |
| 215 band, Threshold 0 + Intensity 2.0 | 0.606 | 249 | **255 — clipped** |

So on the brightest scene the multiply form merges **four** of the five bands onto white
where the shipped form merges two, and it does so one whole slider position earlier.
Moving the intensity inside the exponent removes the clamp entirely.

`tests/test_effects_curve.cpp` asserts the property exhaustively over all 65,536 8-bit
`(base, glow)` pairs at six points on the Intensity slider — in range, never darkening,
monotone in the glow, in the base and in the intensity, and strictly below white wherever
the picture was. The gate measures the same thing on real captures
(`bloom-noclip-*`, plus Adaptive Gamma's stricter `noclip-*-bloom-default`).

#### The params, and the row count

Three: **Threshold** (0..1, 0.75), **Intensity** (0..2, 0.8), **Radius** (0..1, 0.5) —
"what glows", "how much", "how far". Against `kParamBudget`'s 8 that is comfortable, and
deliberately so; the two candidates for a fourth were weighed and rejected:

- **A knee / falloff shape.** There is no separate knee to expose — the contribution is
  already a smooth function of how far above the threshold a pixel is — and the only
  thing a control there could do is make it *harder*, which is the setting that shimmers.
- **A glow colour / tint.** The glow is built from `grade()`'s own output, so it already
  carries the picture's colour; a tint would be a second, contradicting answer to a
  question Saturation and Vibrancy already own.

Radius maps to the separable blur's σ as `1 + 2·radius` **glow-buffer texels**, i.e.
**8..24 source pixels**, so a nominal 3σ reach of 24..72 pixels. Measured on the
reference source below — where the glow's own amplitude is only about 15 counts, so it
disappears into the field well before 3σ — the reach is **15 / 28 / 43 px** at Radius
0 / 0.5 / 1. `Why the floor is 1 and not 0:` the buffer is an 8×8 box average, so its
own texels are already an 8-pixel-wide feature; a σ below one texel would leave that box
structure visible as blocking after the bilinear upsample instead of a smooth falloff.

**The Effects band went from 6 switch rows to 7** (Saturation, Vibrancy, Pre-Sharpen,
Bloom, Shadow Control, Adaptive Brightness, Adaptive Gamma), and the area's summary reads
`n of 7 effects on` (it briefly read **8** while the now-removed Brightness Map effect was
shipped, 2026-09-09 to 2026-09-14). Bloom is registered immediately after Pre-Sharpen because
those two are the only **spatial** effects in the band — the only ones that read a
pixel's neighbours — and because the pipeline runs Pre-Sharpen and then Bloom, so the
pair reads in pipeline order in the panel too. The budget was **not** raised.

#### The Inspector's before/after preview: deliberately not extended

Bloom's row declares **no** `Preview`, so the strip is simply not drawn for it — the
cleanest possible degradation, since there is no placeholder to get wrong. `Why not, when
extending it looked cheap:`

1. **`EffectPreviewMath.h`'s whole contract is per-pixel.** `ApplyPixel()` is
   `(u, v, stats, params) → rgb`, and `Compose()`'s 256-entry LUT fast path — asserted
   equal to `ApplyPixel()` for every byte value in `test_overlay_ui.cpp` — exists because
   the adaptive effects are a pure function of one byte whenever the gain is a frame
   constant. A spatial operator breaks that at the root, not at the edges.
2. **The capture cannot say how big the glow should be.** The strip is a fixed 256×144
   downscale of a frame of *unknown* source size, and Radius is defined in source pixels.
   A CPU mirror would have to guess the reduction factor and would draw a glow of the
   wrong radius. That is precisely the "a preview that lies is worse than no preview"
   line the Adaptive Brightness strip's own design took.
3. **The capture pass is armed by the adaptive effects.** `cs_effects_preview.comp` runs
   only when `NeedsStatistics()` is true and applies `grade()` alone; making it serve
   Bloom would mean a second capture path, a second arming rule and a CPU blur — a
   feature of its own, not an extension.

Bloom is also the effect that needs the strip least: unlike a tone curve, its result is
visible on the game the moment the switch is flipped, at the size and radius it will
actually have.

#### Measured (desktop, headless, `scripts/effects-regression.sh`, 2026-09-08)

Same recipe as everything above (private headless sway, nested `gamescope --backend
wayland`, `gamescopectl screenshot "<path> 4"`, `tests/effects_scene_client.c` as the
game, 1280×720). Bloom alone, every other effect off, defaults unless stated. Captures:
`build-release/verify-shots/bloom-2026-09-08/`.

The line profiles are measured on the existing **`haloinv`** scene — a flat 220 box,
320×320, centred on a flat 15 field — which was built for the Local-adaptation halo
checks and happens to be exactly what a bloom test wants: a distinct bright source on a
dark field with a hard, straight edge at a known x. The profile walks out from that edge
along the box's centre line; `bloom-off-flat` is the control that says the field really
is flat before any of it means anything (measured: spread **0.0 counts**, 15 at every
distance from 4 px to 460 px).

##### The three sliders, as three different line profiles

The field value at each distance in source pixels out from the box's right edge. "Extent"
is where the profile falls back to within 2 counts of the far field, linearly
interpolated. Everything else at the shipped defaults.

**Radius** (`bloom-radius`, PASS) — the glow must reach **further**:

| Radius | d4 | d8 | d16 | d32 | d64 | d128 | far (d460) | extent |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.0 (σ 8 src px) | 26.1 | 21.5 | 16.1 | 15.0 | 15.0 | 15.0 | 15.0 | **15 px** |
| 0.5 (σ 16, default) | 28.9 | 25.6 | 20.5 | 16.0 | 15.0 | 15.0 | 15.0 | **28 px** |
| 1.0 (σ 24 src px) | 29.6 | 27.5 | 23.6 | 18.0 | 15.0 | 15.0 | 15.0 | **43 px** |

The reach nearly triples while the amplitude beside the source barely moves (+11.1 →
+13.9 → +14.6) — which is exactly what a wider Gaussian does to the same amount of light,
and is why Radius is checked on the extent and Intensity on the amplitude. Every profile
is **monotone decreasing** out from the edge: no ring, no overshoot.

**Intensity** (`bloom-intensity`, PASS) — the glow must get **brighter**:

| Intensity | d4 | d8 | d16 | d32 | far | amplitude at d4 |
| --- | --- | --- | --- | --- | --- | --- |
| 0.4 | 22.0 | 20.4 | 17.8 | 15.6 | 15.0 | **+7.0** |
| 0.8 (default) | 28.9 | 25.6 | 20.5 | 16.0 | 15.0 | **+13.9** |
| 1.6 | 41.6 | 35.8 | 26.4 | 17.4 | 15.0 | **+26.6** |

Doubling the slider almost exactly doubles the added light (+7.0 → +13.9 → +26.6), which
is the "linear where the glow is faint" property of the composite showing up as a
measurement rather than as an argument.

**Threshold** (`bloom-threshold`, PASS) — raising it must make **less** glow. The box is
220 (0.863 encoded), so 0.5 is well below its emission and 0.85 almost above it:

| Threshold | d4 | d8 | d16 | d32 | far | amplitude at d4 |
| --- | --- | --- | --- | --- | --- | --- |
| 0.5 | 50.5 | 42.5 | 29.4 | 16.9 | 15.0 | **+35.5** |
| 0.7 | 34.9 | 30.5 | 23.2 | 16.5 | 15.0 | **+19.9** |
| 0.85 | 15.9 | 15.4 | 15.0 | 15.0 | 15.0 | **+0.9** |

At 0.85 the source has essentially stopped emitting — the slider does turn the glow off
from above, which is the property that makes "what glows" a real control rather than a
brightness knob by another name.

##### Nothing away from a source moves

`bloom-unchanged-dark` (PASS) compares the `dark` scene with Bloom off against the same
scene with it on at the defaults. The bands are sampled at x 1150..1270 and the nearest
240-code highlight ends at x = 1070 — 80 source pixels away, five σ at the default
Radius:

| region | off | on | change |
| --- | --- | --- | --- |
| bands 5 / 8 / 12 / 16 / 20 | 5 / 8 / 12 / 16 / 20 | 5 / 8 / 12 / 16 / 20 | **0.00** |
| pure black corner | 0 | 0 | **0.00** |

Worst change anywhere away from a source: **0.00 counts**. Dark and mid-tone regions are
untouched, measured rather than asserted.

##### No clipping

`bloom-noclip-*` (all PASS) — the bands stay strictly ordered and nothing whose input was
below white comes out on white. `noclip-*-bloom-default` runs Adaptive Gamma's stricter
"and at least 2 counts apart" check on top, at the shipped defaults, and also passes.

| scene / setting | bands | the other region |
| --- | --- | --- |
| dark, off | 5 / 8 / 12 / 16 / 20 | 240 highlights: 240 |
| dark, defaults | 5 / 8 / 12 / 16 / 20 | **243.3** |
| dark, Intensity 2.0 | 5 / 8 / 12 / 16 / 20 | **246.9** |
| bright, off | 200 / 215 / 230 / 245 / 255 | 30 shadows: 30 |
| bright, defaults | **201.1 / 219.1 / 237.1 / 251.0 / 255.0** | **48.4** |
| bright, Intensity 2.0 | **202.3 / 224.2 / 244.1 / 254.0 / 255.0** | **72.7** |

The 240-code highlights on a dark field are the case a naive additive bloom destroys:
here they come out at 243 and, at the very top of the Intensity slider, at 247 — still
eight counts below white and clearly distinguishable from it. On the bright scene the
245 band lands on 251 at the defaults and 254 at Intensity 2.0: the gap to white narrows,
but nothing crosses.

**And exactly what the extreme costs, reported rather than hidden.** With Threshold at
0.0 — *every* pixel emitting, which is the setting a user reaches for when they want the
whole picture to glow — and Intensity at 2.0:

| scene | bands | the other region |
| --- | --- | --- |
| dark | 5 / 8 / 12 / 16 / 20 (**unchanged**) | 240 highlights: **249.7** |
| bright | 240.3 / 248.8 / 253.1 / **255.0** / 255.0 | 30 shadows: **116.8** |

On the dark scene nothing is lost: a band sitting at 0.02 of full scale contributes its
square, which is nothing, so the bands do not move and the highlights stop at 250. On the
**bright** scene the 245 band does reach 255 — `bloom_apply` is still strictly below 1.0
there, but by less than half of one 8-bit code, so the capture rounds it onto white. That
is the honest limit of the effect: *at the very top of two sliders at once, on a frame
that is already almost all white, the two brightest bands merge.* It is a setting the user has to go out of their way to
reach, it is not the default, and it is the only combination in the whole matrix that
does it — which is why it is an INFO line in the gate rather than a threshold picked so
that it passes. The 30-code shadows going to 117 in that state is the same statement from
the other side: a strong bloom on a bright frame is veiling glare, and veiling glare is
what bloom *is*.

##### Stability under a pan: no shimmer

The threshold question, as a number. `bloom-stability-static` (PASS) holds `texdark`
perfectly still for 300 composites with Bloom on and reads the graded output pixel out of
the `effects_ab_log` readback: **peak-to-peak exactly 0**, and the raw measurement
peak-to-peak exactly 0 too. The three extra dispatches are a deterministic function of
the frame, to the code value.

`bloom-stability-pan` (PASS) is the one that matters. Six captures of the same `texdark`
scene panning at 3 px/frame with `--periodic` — so the frame's *light population* is
identical every frame and only the layout moves — with the effect off and then on:

| | frame means | peak-to-peak |
| --- | --- | --- |
| Bloom off | 25.73 25.73 25.73 25.75 25.74 25.72 | **0.024 counts** |
| Bloom on (defaults) | 26.49 26.49 26.60 26.49 26.49 26.48 | **0.114 counts** |

**The frame-to-frame variation with Bloom on is 0.114 counts of 255** — roughly a ninth
of one code — against 0.024 with it off. So the effect does add a little frame-to-frame
movement, as any operator that reads a moving source must, and the amount is about a
tenth of a code on a frame whose mean is 26: far below what a human can see, and below
the rounding of the 8-bit capture it was measured in. The quadratic weight is why: a
pixel arriving at the threshold contributes a second-order amount, so the population
crossing the cut in any one frame moves the total by almost nothing. Bloom also adds a
constant **+0.76 counts** to this scene's frame mean, which is the glow itself and is
supposed to be there.

`Why the frame mean and not a probe pixel:` a probe pixel on a panning scene moves with
the texture and would measure the pan, not the effect (the Adaptive Gamma pan check reads
147 counts of probe-pixel movement for exactly that reason). The whole-frame mean is the
one quantity `--periodic` holds constant by construction, which is what makes a residual
in it attributable to the operator.

**What was NOT measured:** a real game frame. Everything here is the synthetic client, at
1280×720, on this desktop's GPU. The shape of the operator is pinned by the unit tests and
its behaviour on flat and textured synthetic scenes by the gate; how it *looks* on a real
game with real lights is a judgement only the user can make.

##### The settings audit

`scripts/settings-audit.sh` covers all four new rows (the switch and its three params) in
all three routing situations: **412 settings audited (138 unique ids across 3 situations,
412 rows), 312 passed, 0 failed, 100 not covered** — up from 400 / 134 / 300 before this
change, i.e. the twelve new rows all pass. Unlike Adaptive Gamma, Bloom needed **no**
change to the audit itself and no `SIBLING_KEYS` entry: its four writes each touch
exactly their own declared key and nothing else, because it has no exclusion rule with
any other effect and nothing about it is derived from another row's state.

##### Looked at, not just measured

Captures in `build-release/verify-shots/bloom-2026-09-08/`, graded by eye rather than by
checklist:

- **`17-haloinv-bloom-intensity-1.6.png`** — the clearest single answer. A soft,
  symmetric halo around the bright box, brightest right at the edge and fading smoothly
  outward with no ring and no blockiness, while the box's own interior stays where it
  was. This looks like bloom, not like a blur bug.
- **`17-haloinv-bloom-off.png`** vs the radius trio — off is a hard-edged white box on
  black; 0.0 is a tight rim; 1.0 is a wide, even haze. The difference between the three
  is legible at a glance, which is the test of whether the slider is worth having.
- **`16-texdark-bloom-off-1.png`** vs **`16-texdark-bloom-on-1.png`** — the closest thing
  here to a game frame: a dark textured field with scattered lights. Off, the lights are
  hard squares; on, each one has its own halo and the scene reads as if the lights are
  actually emitting. This is the capture that says the effect does what the request asked
  for.
- **`19-bright-bloom-worstcase.png`** — the unflattering one, and it is in the directory
  on purpose: threshold 0 and Intensity 2.0 on the brightest scene is a hazy, washed-out
  picture where the top two bands have merged. Nobody would ship that, and nothing in the
  defaults goes near it.


### Brightness Map — History (2026-09-09 to 2026-09-14)

An experimental spatial local tone operator, `image.shaders.brightness_map`
(`ReshadeBrightnessMapSettings`): it built a low-pass of the frame's own luminance — the
*brightness map*, at a user-controlled blur of 4..96 source pixels — and divided the
picture by it against a mid-grey target, so a dark player model on a bright field got its
own exposure instead of the field's, at a scale Adaptive Gamma's Local adaptation (a much
coarser 16×16 grid) could not resolve. Five params: Strength, Radius, Target brightness,
Min brightness, Max brightness.

**Removed 2026-09-14** at the user's request ("You can actually remove the brightness map
experimental shader, I don't need it anymore.") — old profiles carrying a `brightness_map`
object are simply no longer read, the same precedent the removed HUD backdrop keys
followed. The Adaptive Brightness V2 plan's guided-filter base/detail split
(`superdoc/planning/adaptive-brightness-v2-plan.md`) supersedes the idea: a base/detail
decomposition serves the same "correct an object independently of its background" goal
without a dedicated effect, a separate halo dial, or a second spatial pipeline running
alongside Bloom's.

**What was measured, worth keeping in mind for that plan:** the halo/resolution trade-off
was real and unavoidable by construction (a finer map resolves smaller objects but rings
harder around a hard edge; a map equal to the picture is not "pixel perfect", it is the
image gone, since the operator divides the picture by a blur of itself); a 16-bit packed
map was necessary because an 8-bit one produced a visible contour on smooth gradients; and
a coarser downsample grid (rather than a wider blur kernel) was the affordable way to
cover a wide range of blur widths from one small separable kernel.

