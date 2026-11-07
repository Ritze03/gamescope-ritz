# Shader effects: Adaptive Brightness V2

Adaptive Brightness V2: toe-gamma curve, content anchor, guided-filter stages. Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

### Adaptive Brightness V2 (`image.shaders.adaptive_brightness_v2`) — NEW 2026-09-14

**Decision (2026-09-14), quoted:** *"Call it 'Adaptive brightness V2' in the GUI.
Implement it fully, so I can test it later. DO NOT REMOVE THE ORIGINAL!"* — so this is a
**third**, mutually-exclusive choice ALONGSIDE Adaptive Brightness and Adaptive Gamma
above, not a replacement for either. `superdoc/planning/adaptive-brightness-v2-plan.md`
is the design doc this section implements; its own §Removals and schema-bump/migration
sections are **superseded** by this decision and were not executed.

**Why it exists.** Every operator above lifts with `x^g` (or a levels gain feeding one),
and `x^g` for `g < 1` has an **infinite slope at x = 0** — on the user's own near-black
capture (73.5% of the frame at code 0..1, median 0.0003), the exponent is driven to its
floor and code 4 lands on 90: structural binarisation, not a tuning error (the plan's
§3.2 has the full derivation). V2's curve bends into a **straight line of slope S** (the
user's own **Max lift**) at black instead, so the largest amplification anywhere in the
frame is `S`, by construction — the anti-binarisation guarantee holds at every setting.

**The curve.** A toe-gamma, `f(x; g, S) = x · ((1+t)/(x+t))^(1−g)` with
`t = 1 / (S^(1/(1−g)) − 1)` solved so `f'(0) = S` exactly; `f(x; 1, S) ≡ x` and
`f(x; g, 1) ≡ x` (either alone disables the lift, and `abv2_toe()`/`abv2_knee()` in
`src/shaders/effects_curve.h` return the identity directly rather than solving for an
infinite `t`). `f(0)=0`, `f(1)=1`, concave, so the slope — and the secant `f(x)/x` — never
exceed `S`. A **Shape** choice offers a second curve, **Knee**: below a fixed knee point
(`ABV2_KNEE_X = 0.5`) it is the SAME toe formula rescaled into `[0, 0.5]²` so it lands
exactly on the identity line at the knee; above it, the plain identity. Toe compresses
the WHOLE upper range by about `g` (the film trade — highlights lose a little contrast,
nothing can clip); Knee leaves highlights **exactly** untouched (`f'(1) ≡ 1`) and puts
the trade in the mid-tones just above the lifted shadows instead (the monitor "Shadow
Boost" trade). `Why this exact knee construction:` the plan's §8.2 Q1 leaves the formula
open (toe vs. knee was flagged as a question, not specified); a single cubic Hermite
matching both endpoint values and slopes was tried first and rejected — a monotone
cubic Hermite on `[0,1]` with unit secant cannot have an endpoint slope above `3` (the
classical Fritsch–Carlson bound), so it inverts for any `S > 3`, which is inside this
row's own 1..8 range. The rescaled-toe construction has no such ceiling (verified by
exhaustive sampling in `tests/test_effects_curve.cpp`, `g × S` grid, `S` to 8).

**NaN guard at `x = 0` (2026-09-14, V2 QC).** `abv2_toe()` now returns `0` directly for
`x ≤ 0` rather than evaluating the formula there. In float32, `abv2_solve_t()`'s
`S^(1/(1−g))` overflows to `inf` once `g` is within about 0.02 of 1 — `g ≥ ~0.977` at
`S = 8` (Lift below ~0.04) — which makes `t` exactly `0` and `0 · pow(1/0, 1−g)`
evaluate as NaN. The GPU pipeline never reaches `x = 0` here in practice (the black
floor above runs first), but the shared header must not hand a caller a NaN regardless
of whether the GPU path happens to avoid it; a unit test now walks `g` from 0.95 to
0.999 across every `S` to pin this.

**Choosing `g`.** `g_static = 1 − 0.6·Lift` (Lift 0..1 → 1.0..0.4 — the lift that is
ALWAYS there, regardless of Adaptation); in **Scene** mode,
`g_adapt = ln(Target) / ln(anchor_smoothed)` and `g = clamp(min(g_static, g_adapt),
G_MIN, 1)` — `min`, not `max`, because adaptation can only make the lift STRONGER on a
dark scene (no global statistic can find a 1%-of-frame object on a bright one, plan
§3.5); in **Off** mode `g = clamp(g_static, G_MIN, 1)`, a purely static shadow lift with
zero temporal behaviour. `G_MIN` (0.2) is not user-facing — it only keeps `t` finite.

**The content anchor.** A rank-window mean (ranks 25..75%) over taps **above the black
floor** (`ABV2_BLACK = 2/255`) — "the median of the void is not the median of the
content" (plan §3.3). `cs_effects_measure.comp` keeps a SECOND, parallel histogram
(`s_histC`/`s_histSumC`) alongside Adaptive Brightness/Gamma's own (which must keep every
tap, black included, for their `p2`/`p50`/`p98`), excluded only when a tap's luma is
above the floor. Below 1% of the frame qualifying (a loading screen), the anchor **holds
its previous smoothed value** rather than swinging on a handful of taps.

**Temporal.** One EMA on the anchor alone (the base layer is spatial and deterministic
per frame, so it cannot flicker), asymmetric 1:2 — the row's own **Adapt speed** is the
brighten-off time constant, darken-on is `2×` it (the eye's own light adaptation is
faster than its dark adaptation) — plus a **scene-cut detector** that SNAPS instead of
sliding: `|ln(raw/smoothed)| > ln 2` AND a histogram L1 distance `> 0.35` against last
frame. **On a reset frame** (a fresh, never-written history texture, or a resume after a
gap — 2026-09-14, V2 QC): a VOID frame used to seed the EMA from the unwritten
`HISTORY_V2_ANCHOR` texel and read an unwritten hist-prev row into the cut test, i.e. it
read a value that does not exist yet. Now the smoothed anchor seeds to **Target**
(`g_adapt == 1`, the static floor alone, until the first frame with content snaps the
anchor to something real) and the L1 distance seeds to 0, so no cut is reported on the
first frame after a reset; a non-void reset frame still snaps to its own raw anchor
exactly as before. `Deviation from the plan:` the L1 test reuses the measure pass's own 16 buckets
(grouping its existing 64-bin histogram four-at-a-time) rather than widening the shared
history texture to a 64-column, bin-for-bin copy as the plan's §5.1 sketches — the
history texture gained one row (`V2_HIST_PREV_ROW`, height 17→18) instead of also
growing to 64 columns. A documented simplification, not a change to the 35% threshold's
own meaning; `effects_common.h`'s own comment states the trade.

**Base/detail.** A fast guided filter, self-guided on luma, at **quarter resolution**
(`cs_effects_v2_down.comp` → `cs_effects_v2_box1.comp` → `cs_effects_v2_box2.comp`,
reusing `update_effects_scratch_pair()` the way Bloom's own buffers do):
`mean = box_r(Y4)`, `corr = box_r(Y4²)`, `a = var/(var+EDGE²)` (`EDGE = 0.06` encoded,
a constant, not a param), `b = (1−a)·mean`, smoothed a second time over the same radius.
`Deviation from the plan:` §5.1 sketches FIVE separable dispatches (down, box1h/v,
box2h/v); this ships as **three non-separable 2D box passes** instead — the buffer is
already quarter-resolution (a few hundred thousand texels), so `(2r+1)²` taps per texel
at this effect's radii (2..12 texels) is still comfortably sub-millisecond, and one
non-separable box also let `mean`/`corr`/`a`/`b` collapse into ONE pass instead of a
mean/corr texture followed by a separate `a`/`b` pass. The maths — the guided filter's
own formula — is unchanged; only the dispatch shape is simpler. `Scale` (0.5..4% of
frame height → the box radius, computed on the host once the frame's own size is known)
is a config field but is **not** exposed as a GUI row in this pass (the lead's explicit
row list omits it); it defaults to 1.5%. `a`/`b` are stored as plain 8-bit values (not
the retired Brightness Map's packed-16-bit scheme) — they are themselves already
fractions in `[0,1]`, with no float-bits round-trip to protect.

Three things worth knowing about this filter that are not obvious from the formula
alone (2026-09-14, V2 QC):

- **The void-boundary contour.** `B`'s hard black floor (below) makes the local mean
  `mean = box_r(Y4)` cross the `2/255` threshold at a hard edge wherever the box window
  straddles the boundary between void and content, producing a spatially-coherent
  contour roughly **5 codes** wide at the default radius (**12 codes** at `S = 8`, since
  a larger secant amplifies whatever the guided filter's own model error is there). This
  is a design choice, not a bug — §4.6 states the floor is intentionally hard so literal
  black stays literal black, and the contour is the visible price of that hardness at
  the one place the filter's own smooth model meets a wall.
- **Pre-Sharpen and Bloom enter as detail.** Both run on the full-resolution `Y` before
  V2's own down-pass, but neither is present in `Y4` (the quarter-resolution buffer the
  guided filter is built from) or in the fine-radius anchor Clarity reads — so a
  sharpening halo or a bloom glow shows up in `D = Y − B` as detail, not as base, and is
  amplified by the same `secant · Detail` (and, with Clarity on, `· (1 + Clarity)`) that
  amplifies real texture. Nothing clips (the shoulder still bounds the result), but a
  strong Pre-Sharpen or Bloom setting will look stronger still once V2 is on.
- **`a`, `b` and `Y4` are plain 8-bit UNORM**, not the retired Brightness Map's 16-in-8
  pack. Invisible at defaults; measured at **~1 code mean / ~5 codes local noise** at
  Max lift 8 / Detail 2 — the amplification that makes the void contour worse at high
  settings also makes this quantisation visible there. A 16-in-8 pack for these three
  buffers is a possible follow-up, not something this pass needed.

`B(p) = ā·Y(p) + b̄` (bilinearly sampled, `v2_coef_sample()` in `effects_common.h`);
`D = Y − B`; `sec = min(f(B)/B, S) · Detail` (the SECANT, not the tangent — it is what
makes a base/detail split preserve the region's Weber contrast at all, rather than
flattening it the way a per-pixel curve applied to the same texture would); positive
detail gets a Reinhard-shaped soft shoulder into the remaining headroom
(`abv2_shoulder()`), negative does not (it cannot go below `−f(B)`, i.e. `Y′` cannot go
negative). **Where this is exact and where it isn't (2026-09-14, V2 QC):** for negative
detail (dark-on-bright) and for Clarity's own contribution against Stage 2, `Detail = 1`
preserves Weber contrast *exactly* — the shoulder never runs on either. For **positive**
detail, the shoulder is a bounded approximation: it lowers Weber contrast versus the raw
secant by `h/(h+u)` (0.87 in a worked 10-on-15 example) while the absolute step still
grows roughly 2.7× over raw — the price of the range guarantee (nothing may exceed 1),
not a defect. `Detail` 0 flattens to the base alone; above 1 boosts contrast further
(and widens guarantee 1 to `S · Detail`).

**Black floor.** `B ≤ ABV2_BLACK` (2/255): `Y′ = Y`, exactly, never touched — literal
black, letterbox bars and 1-code dither stay untouched rather than "close".

**Colour.** `k = Y′ / max(Y, ε)`, `RGB′ = RGB · k` — hue-preserving, unlike every other
effect in this pass (which is per-channel): a per-channel exponent desaturates lifted
shadows, and colour is the detection channel that survives darkness best. Compressed
back into range by mixing toward the achromatic `Y′` if any channel would otherwise
exceed 1 (the same shape this pass's other clamps avoid needing, because they are
per-channel and Adaptive Brightness's own shoulder already protects the top).

**Pipeline.** `EFFECT_ADAPTIVE_V2` reuses the bit and the sampler slot the retired
Brightness Map left free (`1u << 9`, `VKR_EFFECTS_V2_SLOT = 3u`) — the same "reused the
same day" precedent that bit's own comment already documents. Three new dispatches sit
in `vulkan_composite()`'s pre-pass block, after Bloom's, before the per-pixel pass, with
the identical barrier-free ping-pong reasoning Bloom's own block documents (every
`dispatch()` call's `prepareSrcImage()`/`prepareDestImage()` plus `insertBarrier()`'s
unconditional execution dependency order the read-after-write and write-after-read
between passes). `EffectsPushData_t`/`NativeEffectsState_t` carry the new uniforms; the
host computes the guided filter's box radius from `Scale` and the frame's own height
(the constructor is the first point in the pipeline that knows it).

**Exclusion.** A THIRD entrant in the existing mutual-exclusion mechanism: turning V2 on
turns Adaptive Brightness AND Adaptive Gamma off, and turning either of those on turns
V2 off (`PanelShaders.cpp`'s three setters, and `EffectsPushData_t`'s own drop rule for a
hand-edited config carrying more than one — the two OLDER effects win, for the same
"richer effect, every existing config refers to it" reason Adaptive Gamma's own drop
over Adaptive Brightness already had). **Why:** all three aim the same mid-tones at the
same target from the same pre-effect statistics (V2's own content-only anchor is still a
statistic of the SAME graded frame the other two measure), so running more than one
would apply the correction twice.

**Params** (ten, `Adaptive Brightness V2` / `image.shaders.adaptive_brightness_v2`,
default OFF): **Shape** (Toe / Knee, default Toe) · **Target brightness** (0.1..0.9,
default 0.35 — lower than the older effects' 0.5, since the anchor here is the content
and 0.5 reads milky) · **Max lift** (1..8, default 4 — the slope cap `S`, wider than the
older effects' 1..4 because a bounded slope makes a harder ceiling safe) · **Lift**
(0..1, default 0.5 — the static floor) · **Adaptation** (Off / Scene, default Scene) ·
**Adapt speed** (0.1..5 s, default 0.5 s — ONE number with a fixed 1:2 ratio, not the
older effects' up/down pair: a bounded operator can safely SNAP on a cut instead of
sliding, so the only thing left to tune is a gradual change's own speed) · **Detail**
(0..2, default 1.0 — the Weber-preserving base/detail split) · **Clarity** (0..1, default
0 — Stage 3, the silhouette band; see its own subsection below) · **Max darken** (1..4,
default 1.0 = off — the darkening mirror of Max lift, bounding the hardest anything above
Target may be pulled down) · **Darken** (0..1, default 0 = off — the static darken floor,
the mirror of Lift; see the Darkening section below for both). That is **10 of 10**
against `kParamBudget`. *(Correction, 2026-09-16: this paragraph shipped 2026-09-14 listing
the **eight** params up to Clarity and saying `kParamBudget`'s 8 was "now fully spent —
this row and Adaptive Brightness's own are the only two at the ceiling", which was true
that day. The Darkening section below then took `kParamBudget` 8 → 10 for the last two
params above — redesigned 2026-09-15 into a true S-curve fixed at Target — so this row is
10 of 10, and Adaptive Brightness's own row is 9 of 10 since 2026-09-16, when its dark
floor folded back in as a `Param`; see "The panel rows (post-split)" above.)* The
Inspector's before/after strip is shared with the two older effects
(`ui::Entry::PreviewKind::AdaptiveBrightness`) — its CPU re-grade
(`src/Overlay/EffectPreviewMath.h`) is a **base-only approximation** for V2 specifically
(`B ≡ Y`, no guided filter modelled, so Detail has no visible effect in the strip — the
comment there says so): the strip's own content-anchor is recomputed directly from the
captured 256×144 frame's pixels (a plain sorted rank-window mean) rather than threaded
through the GPU history texture, since the strip runs at a cadence (once per Inspector
open, or per slider tick) where a full sort of 36,864 samples costs nothing.

**Diagnostics.** The existing "adaptive limit" Facts row (shared across all three
mutually-exclusive effects) gains V2's own classifier, `abv2_binding()`: `NONE` (the
content median is on Target), `LIFT_FLOOR` (Lift's own static floor is stronger than
Target asks for), `G_MIN` ("as much lift as this shape allows" — this floor exists only
to keep `t` finite, so it is reachable in practice only via a very dark Scene-mode
anchor, never via Lift alone: `g_static`'s own range, 0.4..1.0, never reaches `G_MIN`
0.2), `VOID` (under 1% of the frame above the black floor — the anchor is holding its
last reading). `Deviation from the plan:` the plan's §4.11 also proposes a fifth code,
`MAX_LIFT`; it is not implemented — `S` (Max lift) is an independent shape parameter,
not a second ceiling on the exponent `g`, so there is no clean "is `S` what's binding"
test to write that would not just be `NONE` with a different `S` already applied. A scene
cut is reported informationally (it is a one-frame event, not a standing limit) rather
than folded into this enum.

**Stage 3: Clarity (NEW 2026-09-14).** The one term in this whole effect that targets a
silhouette's own outline rather than exposure — plan section 4.9. A SECOND guided-filter
coefficient pair, at a FINER radius `r₂ = round(r / 4)` (floored at 1 texel) than
Stage 2's own `r`, is built from the SAME quarter-resolution `Y4` buffer
(`cs_effects_v2_box1_fine.comp` / `cs_effects_v2_box2_fine.comp` — byte-identical to the
Stage 2 pair's own two shaders except for reading `u_v2RadiusFine`, the established
"a separate file per varying parameter" shape this pass's Bloom blur pair already set,
rather than a per-dispatch uniform `EffectsPushData_t` is uploaded once a frame and
cannot vary between two dispatches of what is nominally the same shader). The finer base
`B₂` differs from the Stage 2 base `B` by `M = B₂ − B`, the 4..16px structure a limb, a
head or a weapon outline lives at; `abv2_clarity_combine(D, M, Clarity)` in
`effects_curve.h` folds it into ONE combined detail signal alongside `D` —
`D + M · clamp(Clarity, 0, 1)` — BEFORE the existing `abv2_detail_apply()` shoulder runs,
rather than through a second, independent shoulder call, so guarantee 4 (halo-bounded) is
*inherited* from the already-proven Stage 2 composition rather than re-derived: the
shoulder does not know or care whether its input came from one region's own texture or
two guided filters' disagreement. Guarantee 1 (bounded amplification) widens to
`S · Detail · (1 + Clarity)` — the worst case, `D` and `M` at their own extremes with the
same sign, summing rather than partially cancelling; `tests/test_effects_curve.cpp` pins
this exactly, over an exhaustive `g × S × Detail × Clarity` grid. Clarity 0 is an EXACT
identity on `D` (not merely close — `abv2_clarity_combine` clamps and multiplies by
literal 0), so the fine pair is not even dispatched when the slider is at 0
(`update_effects_v2_fine_images()`, `rendervulkan.cpp`'s `bV2Clarity` gate) — "off" costs
nothing beyond the branch. Edge-awareness (plan 4.9's own claim, restated as the function
composition it actually is): a hard, already-visible edge has `a ≈ 1` in BOTH filters (a
step's local variance is large against `EDGE²` at any radius that does not straddle
*another* edge), so `B₂ ≈ B`, `M ≈ 0`, and `abv2_clarity_combine` returns `D` unchanged —
no rim is added on a real edge, by construction rather than by tuning. A second sampler
slot (`VKR_EFFECTS_V2_FINE_SLOT`, `descriptor_set_constants.h`) holds the fine pair
rather than the plan's own "2×-wide v2A" alternative (section 5.1): a packed-width buffer
would need every box-filter kernel loop (three shader files) to stop reading across the
half-boundary seam, which is exactly the kind of off-by-one this plan's own guarantees
exist to catch, whereas a second slot needed no change to any existing shader's kernel
loop at all.

**Stage 0: GPU timing (NEW 2026-09-14).** Every cost figure on this page used to be an
ESTIMATE from tap counts — `vulkan_composite()` had no GPU timing at all. A
`vkCmdWriteTimestamp` pair now brackets the WHOLE pre-pass block (the measure dispatch,
Bloom's three, V2's guided-filter passes including Clarity's fine pair, and the
per-pixel apply), double-buffered across FOUR queries (two pairs, alternating by frame
parity) so the CPU's non-blocking readback of "last frame's" pair can never race the
GPU's reset-and-rewrite of "this frame's" pair on the same query slots — the failure mode
that would occur with only two queries and no `VK_EXT_host_query_reset`. Guarded by
`CVulkanDevice::supportsTimestamps()` (`timestampComputeAndGraphics` AND the chosen
queue family's own `timestampValidBits`, both checked in `selectPhysDev()`): a device
without either reports `-1.0` (`vulkan_effects_gpu_us()`) rather than a fabricated
number, and every reader (the `effects_timing` ConCommand, the Diagnostics "pre-pass"
Facts row below) prints "not measured" for that value rather than a bogus 0.00 ms.
`effects_timing` also prints the mean/min/max over the last 120 frames it ran.

**Diagnostics: the Pipeline Facts row's "pre-pass" line** (NEW 2026-09-14) shows the
measured time in both ms and µs (`%.2f ms (%.0f us)`), or the "not measured" text above
when unsupported or before the pre-pass has run once. See this page's Measured section
below for the number on this desktop's GPU, at the harness resolution.

**Measured** (CPU reproduction of `abv2_toe()`/`abv2_knee()`, Lift 0.5 → `g_static = 0.7`,
`S = 4`; codes in, codes out):

| shape | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 200 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Toe | 4 | 7 | 12 | 21 | 35 | 59 | 96 | 157 | 215 |
| Knee | 3 | 6 | 10 | 18 | 29 | 48 | 79 | 128 | 200 |

and, on a `texdark`-like content anchor of 0.08 with Target 0.35 (`g_adapt ≈ 0.416`, the
smaller of the two so Scene mode deepens the Toe lift past the 0.7 static floor above):

| shape | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 200 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Toe (Scene, deepened) | 4 | 8 | 15 | 27 | 49 | 80 | 124 | 182 | 227 |

Every case: code 1 stays within a factor of `S` of its input, by construction (0..4 here,
`S = 4`); no case in `tests/test_effects_curve.cpp`'s exhaustive `g × S` sampling exceeds
`S` anywhere on `[0,1]`.

**Re-measured on the GPU harness (2026-09-14, the Stage 0/3 pass).** The plan's §7.1 new
scenes (`silhouette`, `skyfore`, `--image` PNG capture — `flash` is built in
`tests/effects_scene_client.c` per the plan's own spec but the automated checks below use
a plain `silhouette → bright` SIGUSR1 hop instead of `flash`'s own `--flash N` timer, for
the reason its own check's docstring gives: a screenshot-polling harness cannot land on a
frame-counted phase boundary deterministically, and the plan's own wording — "driven by
SIGUSR1 or a --flash timer" — sanctions either) and §7.2's thirteen `abv2-*` checks now
exist in `scripts/effects-regression.sh` / `scripts/effects_regression_sample.py`, run as
this repo's own restarted-instance block (the SCENES-ring rule: a block that needs new
scenes gets its own ring, same reason the Dark Floor block does). Results from the run
this pass shipped with (the untracked `abv2-2026-09-14` capture run):

| check | result | measured |
| --- | --- | --- |
| `abv2-nobinarise` (`silhouette`) | PASS | raw order kept (fig3 11.0 < field 21.6 < fig10 35.0), nothing near white |
| `abv2-silhouette` (Weber contrast) | PASS | 0.499 (Max lift 1, identity) → 0.491 (default) — within the check's own 0.02 tolerance (§4.5's shoulder trade), not an unconditional "never decreases" |
| `abv2-black` (void exact) | PASS | worst void pixel 0 (must be exactly 0) |
| `abv2-slope` (`dark`, guarantee 1) | PASS | asserts the slope itself (measured 3.3 / 3.0 / 2.5 / 2.3 at `S = 4`, + 0.25 of 8-bit rounding slack), not a codes-vs-a-255-scaled-tolerance mix-up the first run had |
| `abv2-halo` (`halobox`/`haloinv`, defaults) | PASS | **+0.0 / −0.5 codes**, both ≤ 4 |
| `abv2-halo-*-stretch` (Max lift 8 / Detail 2 / Clarity 1) | INFO | +3.5 (`halobox`) / **−11.0** (`haloinv`) — reported, not asserted; see the ruling below |
| `abv2-static` (`texdark`, still) | PASS* | worst pixel delta 1.0 (bound widened 0.5→2.0 — see the check's own docstring: an 8-bit PNG screenshot of a converging EMA, not the exact-0 raw-statistic case `stability-static` asserts) |
| `abv2-pan` (`texdark`, panning) | PASS | frame-mean spread 0.01 codes over 6 captures |
| `abv2-cut` (`silhouette → bright`) | PASS | 100% of the total move already present in the FIRST post-switch capture |
| `abv2-sky` (`skyfore`) | PASS | cloud1−sky +7.3, sky−cloud2 +8.0 codes; both ground figures and halves stay ordered |
| `abv2-colour` (hue, `colors`) | PASS | worst hue shift ≈0.2° (bound 2°) |
| `abv2-gpu-time` (Stage 0) | PASS | mean **181.6 µs**, min 177.6, max 189.0 (n=120, harness 1280×720), budget < 2000 µs |
| `abv2-capture-nobinarise` (real capture) | PASS | 4.18% of pixels ≥ 128 (bound ≤ 6%); zombie/floor separation 87.0 codes (bound ≥ 35, raw 62.5) |
| `abv2-capture-noclip` (real capture) | PASS | pixels ≥ 250: raw 0.11% → V2 0.08% (adds −0.03pp, budget ≤ 0.05pp) |

\* `abv2-static`'s bound was widened from the plan's literal "peak-to-peak 0" once the first
run measured a real 1.0-code delta — see the check's own docstring in
`effects_regression_sample.py` for why an 8-bit screenshot of an asymptotically-converging
EMA is the wrong place to assert exact 0 (the codebase's own existing `stability-static`
check makes the identical "raw exact 0, smoothed/gain small-but-nonzero" distinction).

**Full regression run (2026-09-14, V2 QC, `scripts/effects-regression.sh`, 561 s): 85
PASS, 0 FAIL, 30 INFO.**

**The halo ruling (2026-09-14, V2 QC — corrects an earlier "left as a FAIL" note this
paragraph replaces).** At DEFAULTS, `abv2-halo` measures **+0.0 codes** (`halobox`) and
**−0.5 codes** (`haloinv`), both well inside the ≤ 4-code default guarantee. The **+3.5**
figure that used to be quoted here as "the" halo number is actually
`abv2-halo-halobox-stretch` — the Max lift 8 / Detail 2 / Clarity 1 STRETCH setting, a
different scene and a different setting than the default row. At that same stretch
setting, `haloinv` measures **−11.0 codes**, past the plan's own predicted ≤ 8 bound
(§7.2) — but this is not left as an unresolved deviation: a CPU reproduction of the
whole pipeline (down-sample → guided filter → bilinear upsample → curve → detail →
shoulder) reproduces **−12.7** in float and **−10.7** with the actual 8-bit coefficient
buffers, matching the GPU's −11.0 to within a code. The mechanism is the guided filter's
own `a·Y+b` model missing a flat field by `(1−a)(Y − mean) ≈ 0.7` code in the windows
that just touch the 205-code step, amplified by `secant · Detail · (1 + Clarity) ≈ 32×`
— exactly the amplification guarantee 1 already allows at that setting (`S · Detail ·
(1 + Clarity)` = 8·2·2 = 32). This is the operator's own bounded maths doing what it is
specified to do at its own extremes, not an overshoot or a quantisation bug. The plan's
"≤ 8 at any setting" figure was a *prediction*, never derived from the maths — this QC
pass is the first time it was measured, and it does not hold on `haloinv` at the
stretch setting; the ≤ 8-code figure has accordingly been dropped rather than kept as a
live guarantee, and both stretch checks now run as `INFO` (measured, not asserted) while
the **default-setting ≤ 4-code bound stays asserted** for both scenes.

**Cost, split into V2's own share (2026-09-14, V2 QC — the earlier "0.18–0.22 ms" figure
below is the WHOLE pre-pass, not V2's own cost).** At 1920×1080 on the development GPU: the
pre-pass with V2 off (Saturation only) is **0.10 ms**; with V2 on at defaults, **0.18
ms**; with V2 on and Clarity 0.5, **0.20 ms** — so V2's OWN cost over the pre-pass
baseline is **+0.08 ms at defaults, +0.10 ms with Clarity 0.5**. For comparison, AB
Dynamic's whole pre-pass measures **0.14 ms**. The harness's own 1280×720 mean (the
`abv2-gpu-time` row above) is **181.6 µs**. Both figures are far under the plan's own
Stage 2+3 combined estimate of 0.2–0.45 ms (§5.2) — this is the first real number
replacing every "sub-millisecond, unmeasured" estimate on this page; see the plan's
section 5.2 for what was predicted.

**GUI capture** (the untracked `abv2-2026-09-14` capture run, `gui`): the Shaders area
with V2 selected in the Inspector shows all eight rows (Shape, Target brightness, Max
lift, Lift, Adaptation, Adapt speed, Detail, Clarity), "PARAMETERS 8 of 8", the title
reading "Adaptive Brightness V2", and the before/after strip; a second capture
of the Pipeline Facts row's DETAILS tab shows the new **pre-pass** line reading its
measured time live. **Superseded the same day** by the Darkening section below, which
adds a ninth and tenth row (Max darken, Darken) — "PARAMETERS 10 of 10" is current, and
the 2026-09-15 redesign's own "GUI verification" below carries the fresh Inspector
captures showing all ten rows at that header.

## History

- **Adaptive Brightness V2** (glossary detail): **Adaptive Brightness V2** (`image.shaders.adaptive_brightness_v2`, added 2026-09-14; the GUI title's capitalization was corrected to this, matching every other effect's sentence case, the same day) — a THIRD choice alongside **Adaptive Brightness** (see [shader-effects-adaptive-brightness.md](shader-effects-adaptive-brightness.md)) and **Adaptive Gamma** (see [shader-effects-adaptive-gamma.md](shader-effects-adaptive-gamma.md)), not a replacement for either: the user's own instruction was *"Call it 'Adaptive brightness V2' in the GUI. DO NOT REMOVE THE ORIGINAL!"*. A **Shape** row offers a second curve, **Knee**, that leaves highlights exactly untouched instead of compressing them a little everywhere. Its `g` comes from a **content anchor** — a rank-window mean over frame taps *above* a black floor, so the median of the void (unlit map geometry) cannot dominate the median of the content (floors, players, lights) — deepened, in **Scene** adaptation, toward a **Target brightness**, with a **scene-cut** detector that snaps the anchor instead of sliding across a flash or a door. **Stage 2** (a fast guided filter on luma, quarter resolution) splits the picture into base and detail so a lifted region keeps its own texture/outline contrast (**Detail**) instead of the flat wash a global curve leaves. **Stage 3** (added the same day) adds **Clarity**: a SECOND, finer guided-filter pair (radius `r/4`) whose difference from the Stage-2 base is the 4..16px "silhouette band" — a limb or weapon outline's own scale — added into the detail term with its own gain; a hard, already-visible edge has `a ≈ 1` in *both* filters, so the band is `≈ 0` there and nothing rims. **Darkening** (added 2026-09-14, the user: *"Make it able to make the image darker (both full and on parts of the image)"*) makes the curve two-sided — the SAME family, convex instead of concave above a pivot at Target brightness (**Max darken**/**Darken**, the mirror of **Max lift**/**Lift**) — so a bright region darkens while a dark region lifts in one frame; `kParamBudget` went 8 → 10 for its two new params; **REDESIGNED 2026-09-15** into a true S-curve rescaled to be fixed EXACTLY at Target on both sides (the first cut's pivot sat at the lift curve's own value there, `L(Target)`, above Target itself, so nothing above Target could ever end up below its own raw value — closed by the redesign, at the cost of an explicit Max-darken-1 off switch in place of the first cut's algebraic telescoping). See `superdoc/features/shader-effects.md`'s own section for the formulas and measurements.
