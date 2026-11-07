# Shader effects: Adaptive Brightness (local adaptation, preview, pulse)

Adaptive Brightness, part 2: Local adaptation, the Inspector preview, the pulse and the history texture. Continues [part 1](shader-effects-adaptive-brightness.md). Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

#### Local adaptation (`local_strength`, 2026-09-07) — one curve per neighbourhood

`requests-2026-09-08.md`: *"we need to somehow make it more aggressive/adapt better. It
doesn't really seem like it can handle a lot of different brightness differences on the
screen."*

**The diagnosis, confirmed by capture before anything was built.** Everything above is a
*global* curve: one gain, one gamma, one shoulder for the whole frame. On a frame that is
half dark interior and half bright sky there is no such curve — lifting the interior blows
the sky, protecting the sky leaves the interior black. Measured on the new `halfsplit` scene
(left half the dark scene's 5 / 8 / 12 / 16 / 20 bands, right half the bright scene's
200 / 215 / 230 / 245 / 255): with Dynamic on and the global curve alone, the frame's p98
comes from the bright half, the gain settles at **0.90**, and the dark half reads
**12 / 17 / 23 / 28 / 34** — barely distinguishable from the effect being off
(`split-off.png` vs `split-local-0.png` in the captures directory are nearly the same
picture). More aggression in a global curve cannot fix that; it only moves which half is
wrong.

**The operator.** Three pieces, all inside the two dispatches that already exist:

1. **A 16×16 map of local mean luminance**, written by `cs_effects_measure.comp` into rows
   1..16 of the same history texture (which grew from 8×1 to 16×17; row 0 is still the
   statistics). *It costs no taps at all*: the measure pass is 16×16 threads and thread
   (tx, ty) already owns exactly the 8×8 block of the 128×128 tap grid covering image cell
   (tx, ty), so the per-thread sum it computes for the mean's tree reduction **is** that
   cell's mean. `Why the arithmetic mean of the encoded luma and not a log mean:` the pass
   works in encoded space on purpose and an encoded value is already perceptually spaced —
   a log of it would apply a second perceptual curve — and `HISTORY_MEAN` is the arithmetic
   mean of the same quantity, so local ÷ global is a ratio of like for like.
2. **Smoothing, spatial then temporal.** Spatially, a separable 5-tap binomial blur over
   that 16×16 grid in shared memory, run at strides 4, 2, 1, 1 (the à-trous trick: a
   stride-*k* pass costs the same five taps but carries variance *k*²), total variance 22,
   i.e. **σ ≈ 4.7 of the 16 cells** — getting on for a third of the frame. Temporally, the
   **same EMA with the same asymmetric `up_speed`/`down_speed`** every other statistic gets,
   per cell.
3. **A per-pixel shift of the frame's own histogram.** The apply pass samples the map
   bilinearly (by hand, on the unpacked floats — the texels are float bits spread over four
   UNORM8 lanes, so hardware filtering would interpolate the *bytes*), forms
   `r = clamp(local / global_mean, 0.25, 4)`, blends it as `r_eff = r^strength`, and feeds
   `p98·r_eff`, `p50·r_eff`, `p2·r_eff` into the **same** `ab_dyn_gain` / `ab_dyn_gamma` /
   `ab_dyn_curve` as before. Nothing about the curve changed.

```
r      = clamp(local_mean / mean, 0.25, 4.0)      // effects_curve.h, ab_local_ratio
r_eff  = r ^ local_strength                       //                  ab_local_shift
gain   = ab_dyn_gain (p98 * r_eff, p50 * r_eff, target, min_gain, max_gain)
gamma  = ab_dyn_gamma(p2 * r_eff, p50 * r_eff, gain, target, min_gain, max_gain)
```

- `Why shift the global histogram rather than measure a local one:` a 16×16 cell holds one
  number, not a histogram. "This corner has the frame's shape at a lower level" is the
  cheapest assumption available and it is *exactly right* when the frame is uniform.
- **Strength 0 is the old behaviour, exactly.** `r_eff` is then the literal 1.0 and
  `p98 * 1.0` is exact for every finite float, so the same bits reach the same functions.
  The apply pass's `if (u_abLocal > 0.0)` skips only the four map fetches; it is not what
  makes the identity hold. Pinned in `tests/test_effects_curve.cpp`.
- **The user's bounds are never widened.** `ab_dyn_gain` still clamps to
  `[min_gain, max_gain]` and `ab_dyn_gamma` to `[gmin, gmax]` *after* the shift, so a
  locally-adapted pixel can never leave the range the user set — local adaptation
  redistributes gain **inside** those bounds. Asserted over every scene × bound × ratio ×
  strength combination on the CPU.
- `Why r^strength and not mix(1, r, strength):` the ratio divides into the white point, so
  the gain goes as 1/r and a linear blend gives a wildly uneven slider. Measured on the
  split scene (r at its 0.25 clamp), strengths 0 / 25 / 50 / 75 / 100 %: linear gave gains
  **0.90 / 1.11 / 1.44 / 2.06 / 3.60** — three quarters of the effect in the last quarter
  of the travel — and `r^s` gives **0.90 / 1.27 / 1.80 / 2.55 / 3.60**, every step a
  constant 1.41× (half a stop). Both are exactly 1 at 0 and exactly *r* at 1; only the
  middle differs.

##### Halo control: the make-or-break, and what the radius actually buys

A local tone operator's signature artefact is a bright rim around a dark object on a bright
field. Two scenes were added to measure it rather than argue about it: **`halobox`** (a flat
200 field with one flat 10 box, 320×320 — a quarter of the frame across) and **`haloinv`**
(the inverse, a 220 box on a 15 field), sampled along a line straight out from the box's
right edge. This is a deliberately adversarial case: a hard, straight, high-contrast edge on
a perfectly flat field is the worst thing a local operator can be shown.

**There is no ring, at any strength, by construction and by measurement.** The map is a
non-negative, symmetric, unimodal blur of the image, and such a kernel maps a step to a
*monotone* ramp — it cannot overshoot. Every profile below is monotone out from the edge;
`effects-regression.sh` asserts that at every strength, precisely so that a later
"improvement" (a sharper map, an edge-aware filter) that reintroduces a rim fails loudly.

What *does* exist is a broad gradient, and its amplitude is what the radius buys.
`halobox`, at Local adaptation 100 %, field value at 12 px from the box edge vs 460 px away:

| blur σ (cells) | beside the box | far field | amplitude | what the capture shows |
| --- | --- | --- | --- | --- |
| 1.4 (first attempt) | 246 | 189 | **+57** | an unmistakable white glow ringing the box |
| 2.2 | 243 | 190 | +53 | barely better — see below |
| **4.7 (shipped)** | **219** | **204** | **+15** | a faint frame-wide shading; no glow |
| 4.7, at the 50 % default | 216 | 208 | **+8** | not visible on the flat field |

`haloinv` mirrors it: −56 codes at σ 1.4, **−14** at σ 4.7 / 100 %, **−7** at the default.

`Why widening from 1.4 to 2.2 did almost nothing, and 4.7 did everything:` right *at* a step
edge a blurred step always sits halfway between the two sides, whatever the σ — widening the
kernel widens the ramp without lowering it. What changes the amplitude is making the kernel
much **larger than the object**, so the object contributes its share of the kernel's *area*
instead of half a step. A 4-cell box inside a σ-4.7 kernel is a small fraction of it.

`Why that costs the feature almost nothing:` the case this exists for is *big*. A
half-dark/half-bright frame is an 8-cell feature, and a Gaussian this wide still passes
~90 % of an 8-cell step while passing ~30 % of a 4-cell box. Measured, on the split scene at
100 %: widening 1.4 → 4.7 cost the dark half's lift **34 → 28 codes** (a fifth) and removed
**three quarters** of the halo. The coarse 16×16 grid and this blur are the same decision
twice — the operator is deliberately incapable of resolving an object's outline, and just
capable of resolving which half of the screen you are in. That asymmetry *is* the design,
and it is why the slider's full 0..100 % range is shippable rather than needing a cap.

##### Measured: the split scene, off vs on

`scripts/effects-regression.sh`'s `halfsplit`, Dynamic, defaults, strength 1.0. "Local 0" is
the global curve — the same numbers this feature exists to fix. Captures:
`build-release/verify-shots/adaptive-local-2026-09-07/split-local-{0,50,100}.png`.

| Local adaptation | dark half 5 / 8 / 12 / 16 / 20 | bright half 200 / 215 / 230 / 245 / 255 | bright half's spread |
| --- | --- | --- | --- |
| effect off | 5 / 8 / 12 / 16 / 20 | 200 / 215 / 230 / 245 / 255 | 55 |
| 0 % (global curve) | 12 / 17 / 23 / 28 / 34 | 195 / 207 / 217 / 228 / 235 | 40 |
| 25 % | 14 / 20 / 27 / 34 / 39 | 182 / 191 / 199 / 207 / 212 | 30 |
| **50 % (default)** | **18 / 25 / 33 / 39 / 46** | **169 / 176 / 182 / 187 / 191** | **22** |
| 75 % | 22 / 31 / 39 / 47 / 53 | 157 / 163 / 166 / 170 / 172 | 15 |
| 100 % | 28 / 37 / 47 / 55 / 62 | 146 / 150 / 152 / 154 / 155 | 8 |

Both halves are serviceable from 50 % on: the dark half's darkest band goes from 12 (a value
that reads as black) to 18, and its top band from 34 to 46, while the bright half comes down
from "near white" to a light grey that still has 22 codes of internal separation across its
five bands. The `effects_ab_log` readback on the same frame shows what the operator is
actually doing: the smoothed map spans **24.7 .. 228.1 codes** and the per-pixel gain it
produces spans **3.600 .. 0.476** — a 7.6× range across one frame, entirely inside the user's
own `[0.3, 4.0]`.

**`Why the default is 50 % and not 100 %`**, decided from those captures and not from taste:
the second failure mode of any local operator is the opposite of a halo — pushing every
neighbourhood toward its own target flattens the contrast *inside* each one, and the last
column shows exactly that. At 100 % the bright half's five bands collapse to 8 codes apart
and the picture reads flat and grey; at 50 % they keep 22, more than half of what the global
curve left them, while the dark half is already visibly readable. 50 % is also where the
halo amplitude on the adversarial scene (+8 / −7 codes) is below what shows on a flat field.
100 % remains available and remains ring-free; it is a stronger look, not a broken one.
`effects-regression.sh` guards the collapse directly ("the bright half keeps ≥ 6 counts of
internal contrast").

##### Stability: it does not reintroduce the pulse

The [2026-09-07 pulse fix](#the-pulse-rank-cuts-on-a-bimodal-histogram-2026-09-07) set the
standard — a still frame must produce a *constant* output, peak-to-peak zero — and the local
map is held to it, at three strengths, by `stability-static` in the gate:

| Local adaptation | raw p98 p2p | smoothed p98 p2p | gain p2p | output pixel p2p |
| --- | --- | --- | --- | --- |
| 0 % | 0 | 0.038 codes | 0.00093 | **0** |
| 50 % | 0 | 0.000 codes | 0.000010 | **0** |
| 100 % | 0 | 0.000 codes | 0.000000 | **0** |

Under a **pan** (the same `--periodic` scene, whose *global* statistics are constant by
construction) the probe cell's gain does move: p2p **0.009 / 0.072 / 0.103** at 0 / 50 /
100 % over 300 frames. That is reported, not asserted, and it is the operator working
rather than a pulse: `--periodic` fixes the frame's histogram, not its *layout*, so a cell
genuinely sees different content as the texture scrolls past and its gain is supposed to
follow. The movement is a ~5 % drift over five seconds with the EMA's own time constant, not
a frame-to-frame flicker — and the still-frame column above is exactly zero, which is the
test that would catch a real oscillation.

##### Cost, stated plainly

- **Measure pass**: zero extra taps and zero extra texture fetches — the cell means are the
  per-thread sums it already computes. Added: four blur passes (each two half-passes of five
  shared-memory reads per thread, 16 barriers in total) and one `imageStore` per thread.
- **Apply pass**, and only when Local adaptation > 0: **four extra `texelFetch`es** of a
  16×17 texture per pixel, plus three `mix`es and one `pow`. The two `log`s were already
  per-pixel (the gain and gamma were never frame constants here), so the curve maths costs
  nothing new — it just gets different inputs.
- **Memory**: the history texture 8×1 → 16×17 `ABGR8888` (32 bytes → 1088), and the
  `effects_ab_log` staging copy the same.
- **There is still no GPU-timestamp instrumentation in `vulkan_composite()`**, so there is
  no measured microsecond figure for any of this, and none is claimed. What can be said is
  that the whole pre-pass is sub-millisecond on this desktop's GPU and that the added work
  is a few hundred shared-memory reads in one workgroup plus four cached fetches per pixel.

##### The white point (0.9) and the knee (0.7), reconsidered

The standing *"some things are still overblown"* complaint pointed at these two constants,
and they were re-examined against the new measurements. **Local adaptation makes them
materially less critical, and neither was changed.** The reason the bright reference scene
compresses hard is not the white point — it is the *global* gamma riding `GAMMA_MAX` 1.5
(see "Why min_gain 0.3, max_gain 4.0" above), and a local operator attacks that at its
source: a bright region now gets its own `r > 1`, its own lower gain and its own gamma
instead of one compromise for the frame. On the split scene the bright half's five bands
keep 22 codes of separation at the default where the global curve left them 40 and a
*stronger* global compression would have left them fewer.

Where a highlight control would still help is the case local adaptation cannot reach: a
frame that is uniformly bright, where every neighbourhood has the same level and `r ≡ 1` by
definition, so the local path is the global path. **A "Highlight headroom" control (the knee,
or the white point, exposed) is still worth having and is recommended — but not built here**,
and deliberately not built in the same change as the local operator, so the two can be judged
against each other rather than as one indivisible "it looks different now".

#### The Inspector's before/after preview (2026-09-07)

`requests-2026-09-08.md`: *"You could maybe add a small comparison picture to the
inspector rail, that is split in half (left/right) with one being the original image
(captured frame, when the UI was opened) and the one modified by the adaptive
brightness."*

Selecting the **Adaptive Brightness** row now puts a small 16:9 picture in the
Inspector's CONFIGURE page, above the VALUES block: **one frozen frame from the game,
split down the middle — the left half as the picture reaches Adaptive Brightness, the
right half with the effect applied at whatever the sliders currently say.** Moving a
slider re-grades the frozen frame, so a setting can be judged immediately instead of by
hunting for a scene that shows it. It is a **read**: nothing in it touches the
compositing path.

##### Where the pixels come from

Two things are captured, **on one composite, together**:

1. **The picture.** `src/shaders/cs_effects_preview.comp` — a new 256×144 compute
   dispatch — box-averages the base layer down to that size and runs
   `effects_common.h`'s **`grade()`** on every tap, so what it writes is *exactly the
   image `cs_effects_layer0.comp`'s Adaptive Brightness block is about to receive*:
   Shadow Control and Vibrancy already applied, in encoded space, on the same slot-0
   raw-UNORM binding the other two dispatches use. Pre-Sharpen is deliberately not
   applied (a 5-tap cross at source resolution is invisible after a ~7× downscale, and
   it does not move the statistics either).
2. **The statistics.** The `effectsHistory` texture `cs_effects_measure.comp` wrote for
   *that same frame* — mean, p2, p50, p98 and Local adaptation's 16×16 map — copied out
   beside the pixels.

`Why capture the statistics rather than re-estimate them on the CPU:` this is the whole
reason the right half agrees with a screenshot to the code value. The measure pass's
estimator is a 128×128 tap grid, a 64-bin histogram and rank-window means, followed by
an EMA (see [The statistics](shader-effects-adaptive-brightness.md#the-statistics-both-modes)); a CPU re-implementation of
all that would be a second definition to keep in step, and would differ from the shader
by whatever the two implementations disagreed about. Copying the history is exact and
free. It also satisfies "the stats come from the captured frame itself": both halves of
the capture are from one composite and are frozen together, so the preview never drifts
with what the live game is doing behind the UI.

The apply half is `src/Overlay/EffectPreviewMath.h`, which runs
**`effects_curve.h` — the same text the GPU compiles** — over the captured pixels: the
same `ab_local_shift` / `ab_dyn_gain` / `ab_dyn_gamma` / `ab_dyn_curve`, in the same
order, with the same clamps, plus a line-for-line port of `ab_local_sample()`'s
hand-rolled bilinear.

##### The refresh rule

- **A frame is captured when the strip appears on screen**, and only then. "Appears"
  is defined as *not drawn for 250 ms* — which covers the overlay being opened,
  another row or rail area being selected, the DETAILS page, the Inspector being
  hidden, and the strip being scrolled out of view. One rule for all of them, and it
  is also what makes a stale frame from an earlier session structurally impossible:
  nothing older than a quarter-second-before-it-appeared can be displayed.
- **The right half is re-graded when a parameter changes**, or when a new frame
  arrives. Nothing else redraws it; a still overlay costs nothing.
- **The capture is armed only while the effect's own switch is on**, so the preview
  never asks the compositor for work on a frame the pre-pass would not otherwise run.

##### Degradation

Every not-ready case draws a bordered box with one muted sentence in it, never a black
rectangle and never a stale picture. Ordered by what the user can act on:

| Case | What is shown |
| --- | --- |
| Base layer is not SDR RGB | "Preview unavailable: these effects only run on an SDR picture." |
| Adaptive Brightness switched off | "Turn Adaptive Brightness on to preview it." |
| Armed, no frame yet (no game, HDR frames, allocation failed) | "Open the overlay over a game to preview." |

`Why not-SDR outranks switched-off:` the switch is greyed out under the `kSdrOnly`
reason in that state, so "turn it on" would be advice the user cannot take. A texture
allocation failure disarms rather than retrying every composite, and a resolution
change cannot break anything: the capture is a fixed 256×144 downscale, so the strip's
size never depends on the game's.

##### Measured: the right half is the effect, not an impression

`build-release/verify-shots/ab-preview-2026-09-07/`, headless, `pixel-regression.sh`'s
recipe, `tests/effects_scene_client.c` as the game, Adaptive Brightness alone (the other
three off), Dynamic, schema defaults. The strip is sampled in the capture and compared
against a `gamescopectl screenshot "<p> 4"` of the *same scene with the effect on and
the overlay closed*:

| Scene | Preview, left half (before) | Preview, right half (after) | The real effect, same scene | Error |
| --- | --- | --- | --- | --- |
| `dark`, bands 5 / 8 / 12 / 16 / 20 | 5 / 8 / 12 / 16 / 20 | **71 / 90 / 111 / 128 / 143** | 71 / 90 / 111 / 128 / 143 | **0 counts** |
| `halfsplit`, bright half 200…255 | (its own dark half) | **196 / 207 / 218 / 229 / 236** | 196 / 207 / 218 / 229 / 236 | **0 counts** |
| `texdark` (a textured frame), mean | 24.9 | **124.1** | 121.1 | 3.0 counts |

The two flat scenes are exact to the code value. `texdark`'s 2-count difference is the
one honest inexactness and it is inherent to previewing a *downscaled* frame: the strip
grades the box mean of each 7×5 source block, while the real frame grades each source
pixel and is then averaged by the measurement — `curve(mean(x))` against `mean(curve(x))`
on a non-linear curve. It is a property of the thumbnail, not of the maths, and at 3
counts on the hardest content the test client has it is below what a user can see.

Two more behaviours measured in the same run, both from the strip itself:

- **Strength → 0 makes the right half byte-identical to the left** (5/8/12/16/20 on both
  sides) — the slider-change proof, and the identity property asserted on the CPU as
  well.
- **Local adaptation 0 → 50 % moves the right half on `halfsplit`** (196/207/218/229/236
  → 173/181/187/192/195) and **does not move it at all on `dark`** — correct in both
  cases: a uniform frame has `r ≡ 1` by construction, so there is nothing local to
  adapt to.

##### Cost, stated plainly

- **The capture**: one 256×144 dispatch (36,864 invocations, 16 texelFetches each), two
  small image copies, and one `g_device.wait()` — the same one-GPU-frame stall
  `effects_ab_log` takes, paid **once per time the strip appears**, not per frame. Every
  other frame the pipeline is never bound.
- **The re-grade**, measured on this desktop (`-O2`, 256×144, the right half only —
  18,432 pixels):

  | Case | Per update |
  | --- | --- |
  | Whole image | **32 µs** |
  | Dynamic, Local adaptation 0 | **47 µs** |
  | Dynamic, Local adaptation > 0 | **2.1 ms** |

  The first two are cheap because the effect is then a pure function of one byte — gain
  and gamma are frame constants — so `Compose()` evaluates the curve **256 times** into
  a table instead of 55,296 times. The table is built by calling the *same*
  `ApplyPixel()`, and `test_overlay_ui.cpp` asserts the two paths are equal for every
  byte value in both modes, so it is a speed-up and not an approximation.

  With Local adaptation on, every pixel has its own gain and the table cannot apply, so
  it is ~2.1 ms — six transcendentals per pixel, run on the steamcompmgr thread inside
  the overlay's own draw. **That is the honest worst case**, and it is bounded: it
  happens only on frames the overlay is *already* redrawing, only when a value actually
  changed (a still overlay recomposes nothing), and only in Dynamic mode above 0 %
  Local adaptation. No throttle was added on purpose — throttling risks the strip
  showing the *previous* slider position if the overlay then stops redrawing, and a
  preview that lies is worse than a 2 ms frame. If it ever needs to be faster, the
  table generalises by quantising the local ratio, at the price of the exactness the
  table has today.
- **Memory**: a 256×144 storage texture plus two host-mappable staging copies (147 KB +
  147 KB + 1 KB), created on the first capture and kept.

##### What it deliberately does not show

- **Only Adaptive Brightness.** Shadow Control and Vibrancy are baked into *both* halves
  by the capture's `grade()` call, so the strip isolates this effect — which is what a
  before/after for this row should do.
- **The same frame on both sides, not two crops.** The divider is the midpoint of one
  picture, so a feature straddling it shows its own before and after touching each
  other. On a scene that is itself spatially split (`halfsplit`), that means each side
  of the strip shows a different part of the frame — inherent to the shape the request
  asked for.
- **Not 16:9 content.** The capture is a fixed 256×144, so a 16:10 or ultrawide frame is
  stretched to the strip's shape rather than letterboxed. Tone, which is what the strip
  is for, is unaffected.

##### The size, and what it costs the params

The block is the Inspector's content width capped at **240 logical px** (a 135-px
picture plus a 14-px label line and a 4-px gap: 153 px total). `Why capped, and why
there:` the strip is drawn *above* the params so it is on screen while they are reached
for, which means every pixel of it pushes them down — and at 1280×720 this row's eight
params already filled the Inspector body exactly. Measured at that size: 320 px wide
(the first size tried, measured during this task and not kept) left three params visible;
240 leaves four, and is still legible enough to judge tone on.

**The honest limitation**: at 1280×720 the CONFIGURE page scrolls, and scrolling down to
Min gain or Local adaptation takes the strip off the top of the page with it — so the
lowest params cannot be dragged while watching the preview. At 2560×1440 everything is
on screen together. Pinning the strip above the Inspector's scrolling body would fix it
and is the obvious follow-up; it was not done here because it is a change to
`DrawInspector()`'s region split, with nothing to do with this feature's own risk.

#### The pulse: rank cuts on a bimodal histogram (2026-09-07)

`requests-2026-09-08.md` item 6: *"For the dynamic mode, sometimes the whole image
pulsates for some reason."* Found by measurement, not argument — the `effects_ab_log`
readback (below) was built for it and is what the numbers here come from
(`build-release/verify-shots/adaptive-pulse-2026-09-07/`, `results.txt` has every run).

**What it was not.** A completely still frame (`tests/effects_scene_client.c`'s `texdark`,
no motion) logged for 400 composites at both `max_gain` 2 and 4: raw and smoothed
statistics, gain, gamma and the output pixel **identical every frame** — peak-to-peak
zero, to the last float bit. That rules out the feedback-loop hypothesis outright (the
measure pass reads `layer0.tex`, the game's own buffer, and the pre-pass block is guarded
against ever seeing `effectsOutput` or a pre-upscaled texture; the log confirms the loop
is open), and with it any limit cycle of the EMA, the gain/gamma coupling, the shoulder
or the shadow cap switching: nothing internal moves without the input moving.

**What it was.** A percentile is a rank *cut*: the value of the k-th brightest tap. On a
histogram with an empty stretch — a dark scene with a few lights, a half-sky/half-ground
view: precisely the scenes Dynamic exists for — the cut sits in the gap, and **one tap**
moving from one mode to the other flips it from the top of the lower mode to the bottom
of the upper one. Under camera motion the tap grid lands on different pixels every frame,
so the count straddling the cut jitters by ±√(N·p·q) ≈ 9 taps and the cut telegraphs
between the two modes at random. The EMA cannot hide a square wave of that size; it turns
it into a slow wander, which the gain (`0.9 / p98`) and the gamma (from `p50`) turn into
the picture breathing. Measured on `texdark` with **2 %** light cells (so the 98th
percentile sits in the gap) panning at 3 px/frame, 340 frames:

| | raw p98 p2p | smoothed p98 p2p | gain p2p @ max 2 | gain p2p @ max 4 |
| --- | --- | --- | --- | --- |
| rank cut, 64×64 taps (before) | **107 codes** (0.217 ↔ 0.636) | 16.8 codes | 0 (pinned at 2.0) | **0.81** (3.19..4.00, 25 %) |
| rank-window mean, 64×64 | 46 codes | 9.6 codes | 0 (pinned) | 0.22 (2.16..2.38) |
| rank-window mean, 128×128 (after) | 36 codes | 17.6 codes* | 0 (pinned) | 0.45* |
| … same, `--periodic` (true statistics constant) | 11 codes | **0.6 codes** | — | **0.013** (2.24..2.25, 0.6 %) |

And the same cliff on the **median**: `texsplit` 50/50 panning, rank cut: raw p50 p2p
**98 codes** (0.214 ↔ 0.599), smoothed 33 codes, **gamma 0.86..1.18** with the gain not
moving at all — the midtones alone breathing between a lift and a darkening. After: raw
7.5 codes, smoothed 2.5, gamma p2p 0.02; periodic: smoothed 0.15 codes, gamma p2p 0.0013.

\* The non-periodic pan is not a fair "noise" number: the texture scrolling in at the
frame edge changes the scene's true light population, which the effect is *supposed* to
follow, and the window mean now follows it continuously instead of in jumps. The
`--periodic` row (the same texture repeating every frame width, so a pan changes nothing
but where the taps land) is the sampling noise alone — and that is what the tap count
buys: 64×64 → 128×128 halves the raw spread (26.8 → 11.3 codes, the expected 1/√4) and
the smoothed gain's (0.025 → 0.013). At `max_gain` 2 the dark scene sits on the clamp and
the pulse was invisible; **at 4.0 it is worse than what the user saw**, since a 25 %
swing at gain 4 is the whole picture moving a stop — the widening landed the same day, so
the report predates it and the fix was measured at both.

**The fix**, in `cs_effects_measure.comp` only: the three percentiles became the
rank-window means described above (`histogram_window_mean()`, from a histogram of counts
and fixed-point value sums), and the tap grid went 64×64 → 128×128 (four times the
fetches in the same single workgroup; still nowhere near a millisecond). The EMA, its
asymmetric time constants, `dt`, the curve and the history contract are untouched, so
**adaptation is exactly as fast as before** — the trade-off chosen is *no* added lag: the
residual (a ≤ 1 % gain wander with the EMA's own correlation time on the pathological
scene, zero on a still one) was judged below notice, while a deadband or a longer smoothing
would have been visible as steps or sluggishness on every real transition. `Why not more
taps still:` on spatially coherent content the taps are not independent samples (the
16-px cells of the test texture already show 128×128 not gaining the full 2× on the
non-periodic pan), so past this point taps buy little; the estimator's continuity is what
mattered. `Why the same numbers on the reference scenes:` see the statistics section —
the flat scenes have no gap, so `effects-regression.sh`'s dark/bright/mid tables above
are unchanged to the code value by this fix.

**Guarded by** three new `effects-regression.sh` checks, all on the per-frame readback:
`transition-gain` (monotone, no overshoot, settling time reported), `stability-pan` (the
2 %-lights scene, periodic, panning: gain p2p ≤ 0.06, smoothed p98 ≤ 3 codes, raw p98
≤ 40 codes — the rank cut read 0.81 / 16 / 107) and `stability-static` (the same scene
held still via `SIGUSR2`: raw p98 p2p **exactly 0**, output pixel exactly 0, smoothed p98
< 0.1 code and gain < 0.002 — the EMA's last fraction of a percent of convergence after
the pan stops). Measured on the final run: 0.025 / 1.16 / 11.3 and 0 / 0 / 0.013 / 0.0003.

**Per-frame cost.** The measure pass is still one 16×16 workgroup over 4096 taps; the
histogram adds one shared-memory `atomicAdd` per tap and a 64-iteration walk on one
invocation. The per-pixel pass, in Dynamic mode, adds two `log`s per invocation (gain and
gamma are frame constants) and three `pow`s plus three divides per pixel. Neither is
measurable against the ~sub-millisecond pre-pass on this desktop's GPU; there is no
GPU-timestamp instrumentation in `vulkan_composite()` to give a finer number.

**Resets on resume, does not track while off.** The measure pass runs whenever the
pre-pass runs at all (any of the four switches on) and never looks at the Adaptive
Brightness flag itself; only the per-pixel gain is gated. But when Adaptive Brightness is
the *only* switch on and it is turned off, `NativeEffectsState_t::AnyEnabled()` goes
false and the whole pre-pass — measure dispatch included — stops running, so the
history freezes at its last values instead of continuing to track the scene.

`vulkan_composite()` handles this by remembering, across calls, whether the measure
dispatch ran the *previous* time this code path was reached
(`s_bEffectsPassRanLastTime`). Whenever it resumes after not having run — the switch
flipped back on, or content came back to SDR RGB after a stretch of HDR/passthru or
YCbCr frames that skipped the pass — the pre-pass sets `kResetHistory` for that frame
exactly as it does for a freshly-created history, so the very first re-enabled frame
writes the current measurement straight into the history instead of blending with the
stale one. Re-enabling on a changed scene is therefore instant and correct, with no
clipped first frame and no re-convergence ramp.

`Why not track while off (the `.fx` did):` the alternative — running the measure dispatch
even with every switch off, so the history stays warm — would require
`vulkan_native_effects_active()` (`AnyEnabled()`) to report "active" purely to keep a
disabled effect's history fresh. The backends OR that into `bNeedsFullComposite`
(`WaylandBackend.cpp`, `DRMBackend.cpp`, `OpenVRBackend.cpp`), which defeats DRM direct
scanout for a feature the user has switched off. Reset-on-resume gets the same "instant"
result — no stale-gain slam, no ramp — for zero cost while off, at the price of one
`kResetHistory` frame instead of a warm history; the two are indistinguishable to the
user (both an immediately-correct gain on re-enable), so nothing is lost.

When *no* effect is on and the pre-pass never runs, the history simply holds its last
value until the pass resumes and resets it.

**`dt`** is host-side: `vulkan_composite()` keeps the `get_time_in_nanos()` of the previous
effects dispatch (a function-local static) and passes the difference in seconds as
`u_abDt`, **clamped to 0.25 s**. `Why wall time and not a per-frame constant:` the pass
runs once per *composite*, and a `gamescopectl screenshot` frame composites twice (see
"Exactly once per composite" above), so the measure dispatch runs again a fraction of a
millisecond after the presented one. With wall-time `dt` that second step is worth exactly
that fraction, and the next frame's step is shorter by the same amount — the EMA
integrates elapsed time, so adaptation over any interval is unchanged and the screenshot's
Adaptive Brightness gain is within a sub-millisecond step of the screen's. `Why the
clamp:` a hitch, a pause, or the first frame after a long idle would otherwise slam the
EMA to the new measurement in one step; a quarter-second nudge keeps the transition
smooth. The first dispatch passes `0`.

#### The history texture — persistence as a contract, not luck

`g_output.effectsHistory` is a **16×17 `ABGR8888` storage+sampled texture**.
**Row 0** is one texel per smoothed statistic — `HISTORY_MEAN`, `_P2`, `_P50`, `_P98` —
plus, from `HISTORY_RAW` = 4, the same four unsmoothed, for the readback only.
**Rows 1..16** are Local adaptation's 16×16 map of smoothed local mean luminances
(`AB_LOCAL_ROW`, `AB_LOCAL_GRID`); `kEffectsHistoryWidth`/`Height` on the host mirror the
shader's `HISTORY_TEX_W`/`_H`. It is created once by `update_effects_history()` and kept
for the life of the output (it does not depend on the game's size, and its contents *are*
the effect's state — so, unlike the `.fx`, a resolution change does not reset it). It was
1×1 until Dynamic mode (2026-09-06) needed the percentiles, 4×1 until the pulse
investigation (2026-09-07) wanted measured next to smoothed, and 8×1 until Local adaptation
the same day. `Why the map rides in the same texture:` `dst` in `descriptor_set.h` is a
single `rgba8` target and `dispatch()` binds one, so a second storage image would mean
teaching the shared descriptor set a second target for the sake of one pass — where one
extra sampler-slot-free texture row costs nothing and keeps both dispatches on one bind.

**Storage format — RGBA8 bytes, not R32F.** Each float is spread bit-for-bit over the four
8-bit channels of its texel (`history_pack` = `unpackUnorm4x8(floatBitsToUint(v))`, `history_unpack` =
`uintBitsToFloat(packUnorm4x8(t))`, `effects_common.h`). `Why:` `dst` in `descriptor_set.h`
is declared `rgba8` and `CVulkanCmdBuffer::dispatch()` binds one RGB target there; an
`r32f` second-target path through the shared descriptor set, for one texel used by one
pass, was judged more plumbing than four exact byte lanes. `Why it is lossless:`
`unpackUnorm4x8` yields `b/255`; UNORM8 storage rounds `b/255·255` back to `b` (float→UNORM
conversion is round-to-nearest by spec); the fetch returns `b/255` again and `packUnorm4x8`
rounds it back to `b`. Both sides go through the same raw UNORM view, so the format's channel
order cancels. `Why not plain 8-bit:` an EMA step of `alpha·(measured − adapted)` at
`alpha ≈ 0.016` (60 fps, τ = 1 s) is below `1/255` whenever the gap is under 0.25, so the
value would freeze short of its target.

**Discard-safe binding.** `CVulkanCmdBuffer::prepareDestImage()` marks a target it sees for
the first time in a command buffer `discarded`, and `insertBarrier()` then emits
`oldLayout = UNDEFINED`, which *permits the driver to drop the contents*. The `.fx` version
relied on that never actually happening (DECISIONS.md #14: "confirmed empirically on
RADV") — luck, not contract. The native pass binds the history as a **source first**: in the
measure dispatch it is sampler slot `VKR_EFFECTS_HISTORY_SLOT` (= 1) *and* the storage
target, and `dispatch()` runs `prepareSrcImage()` (tracks it with `discarded = false`) before
`prepareDestImage()` (returns early for a tracked image). So the barrier for it is either
nothing (steady state) or `GENERAL → GENERAL`; never `UNDEFINED`. `Why self-sampling is
safe:` row 0's texels are touched by invocation 0 alone, and its fetches precede its stores
in program order; every *other* invocation touches exactly one local-map texel, its own,
and reads it before writing it, so no two invocations address the same texel either way.
The layout is `GENERAL` for both bindings. The `.fx` did the same.

The one legitimate `UNDEFINED` is the creation frame: the fresh `VkImage` really is in
`UNDEFINED` layout, so `vulkan_composite()` calls `discardImage()` on it *before* the
dispatch (its `emplace` wins, so the later `prepareSrcImage` no-ops), the barrier is
`UNDEFINED → GENERAL`, and `kResetHistory` (`1<<31`) tells the shader to write `measured`
directly rather than blend with undefined bits. The shader additionally snaps a NaN/inf
history to the measurement and clamps to 0..1, so the value can never poison later frames.

**Barrier sequence** (the full write-up is the comment above the measure dispatch in
`rendervulkan.cpp`): (1) measure dispatch — history tracked as source, no `UNDEFINED`;
(2) `dispatch()` ends with `markDirty(history)`; (3) the per-pixel dispatch binds it as a
source again, `insertBarrier()` sees `dirty` and emits `SHADER_WRITE → SHADER_READ`,
`GENERAL → GENERAL`, `ALL_COMMANDS → ALL_COMMANDS` — the read-after-write between the two
dispatches; (4) across frames, that same barrier made frame N's write available to all
later reads on the queue, and the write-after-read (frame N+1's measure overwriting what
frame N's per-pixel pass read) is ordered because `insertBarrier()` records a
`vkCmdPipelineBarrier ALL_COMMANDS → ALL_COMMANDS` unconditionally, even with zero image
barriers, and pipeline barriers order against all earlier work on the queue across command
buffers. Every `vulkan_composite()` runs on the compute queue; nothing else touches the
history.

**No lag, by ordering.** Measure runs before apply on the same command buffer, so the gain a
frame receives is computed from that frame's own measurement (as the `.fx`'s pass order
did). If the history allocation ever fails, the pre-pass still runs with the Adaptive
Brightness flag masked off and the measure dispatch skipped.

After the per-pixel dispatch slot 1 is unbound, so the FSR/NIS/blit dispatches that follow
(which bind layers `0..n-1`) do not carry a stray history descriptor on single-layer frames.

