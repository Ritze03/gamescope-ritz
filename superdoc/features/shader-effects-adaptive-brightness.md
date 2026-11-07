# Shader effects: Adaptive Brightness (modes, ceiling)

Adaptive Brightness, part 1: the budget decision, statistics, Whole image and Dynamic modes, gain limits and the ceiling fix. Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

### Adaptive Brightness (`image.shaders.adaptive_brightness`)

**A near-black scene has a floor of its own** (2026-09-14) — see
[Leave dark scenes alone](shader-effects-adaptive-gamma.md#leave-dark-scenes-alone-2026-09-14--one-per-effect)
near the bottom of this page: below this effect's own `dark_floor` (its ninth `Param`,
"Leave dark scenes alone" — a standalone panel row from 2026-09-14 to 2026-09-16, see
that section's placement note) the whole curve fades to the
untouched picture, so a truly dark scene is not driven to its gain/gamma ceiling by a
median near zero. Adaptive Gamma has the identical control (its own field, its own row) —
the two started as one shared control and split into independent ones the same day; see
that section for why.

**A plain switch, with Mode as a Param** (request #17, 2026-09-07,
`requests-2026-09-07.md` item 7: *"the adaptive brightness mode selector should be
inside of the inspector rail. In the main view, it should still only be a switch."*
This supersedes the previous day's three-way-Choice shape — see the history note
below). The sheet row is back to a plain **Switch**, on/off, in the shared **Effects**
`GroupCount` band alongside Vibrancy, Pre-Sharpen and Shadow Control (the pre-request-16
shape, restored). The mode lives in the Inspector's **Configure** page as the row's own
first `Param` — a two-way Choice, `Whole image` | `Dynamic` — ahead of the same six
existing params: `strength`, `target`, `up_speed`, `down_speed`, `min_gain`, `max_gain` —
plus, from 2026-09-07, an eighth, `local_strength`
([Local adaptation](shader-effects-adaptive-brightness-local.md#local-adaptation-local_strength-2026-09-07--one-curve-per-neighbourhood)).
**Config**: `ReshadeAdaptiveBrightnessSettings` — `enabled` (bool) and `mode`
(`"whole_image"` | `"dynamic"`, default `whole_image`, an unknown value on disk resolves
to it) are independent fields, written independently: the Switch's setter only ever
touches `enabled`, the Mode param's setter only ever touches `mode`, so switching the
effect off and back on leaves the mode exactly where the user left it. The panel's
`.Default()`s still read `ReshadeAdaptiveBrightnessSettings{}` (the `PanelCursor.cpp`
pattern) rather than repeating literals.

#### The budget decision: seven params, not six (2026-09-06/07)

Putting the mode back where a knob belongs (the Inspector's params column) made it a
genuine **seventh** `Param` on a row that already had six — a real registration abort
(SPEC §5.2 clause 3, `Registry.cpp`'s `kParamBudget`), not a styling choice. Two ways out
were weighed, honestly, against the existing six:

- **Merge or relocate one of the six.** No candidate survived scrutiny. `strength` and
  `target` are the effect's core dial and target and apply to both modes; `min_gain` and
  `max_gain` are independently meaningful bounds (a floor and a ceiling are not one
  number); `up_speed`/`down_speed` were the closest candidate — one "Adaptation speed"
  in place of two — but they are a **deliberate, documented asymmetry** ("Adapt to
  brighter" vs "Adapt to darker", retitled 2026-09-06 specifically because the old
  labels described the wrong direction — see [above](#the-statistics-both-modes)), not
  two names for one idea. Collapsing them would be a real capability loss (no more
  "react fast to a sudden bright flash, ease into darkness"), not a tidy-up. No merge
  was honest.
- **Raise the shared budget.** `Registry.cpp`'s `kParamBudget` went `6 → 7`, with its
  own comment carrying the same reasoning. This is a **one-time, evidenced exception**,
  not a standing invitation — the law's name and enum (`Law::SixBudget`) are unchanged,
  and the next param added anywhere is still a signal to promote, not to raise the
  number again.

Verified by capture before committing to it, not just argued: at 2560×1440 (2x-ish
shell width) the Inspector's Configure page renders all 7 rows —  Mode, Strength,
Target brightness, Adapt to brighter, Adapt to darker, Min gain, Max gain — with well
over half the panel's height still empty below them
(the untracked `requests-07-item2-7` capture run, `03-shaders-ab-selected-whole.png`). No
crowding at this width; a narrow drawer at high UI scale (D13.4's 2.0x case,
`tests/test_overlay_shell.cpp`) still needs to scroll, exactly as it already did at six.

**Follow-up fixed 2026-09-07 (requests-2026-09-07.md item 12):** `Shell.cpp`'s Inspector
header used to hardcode the denominator — `snprintf(..., "PARAMETERS   %d of 6", ...)`
— so a capture of this very row showed the header reading **"PARAMETERS 7 of 6"**,
literally wrong once the ceiling moved to 7. `Registry.h`/`.cpp` gained `ParamBudget()`
(reads `kParamBudget` outside its former anonymous namespace) and `Shell.cpp` now builds
the header via `controls::ParametersHeaderText( nCount, nBudget )` (`Controls.h`/`.cpp`,
pure formatting, pinned in `test_overlay_ui.cpp`) instead of a second hand-copied
literal — the header and the registry constant cannot silently disagree again.
Verified: selecting this row now reads **"PARAMETERS 7 of 7"**
(the untracked `accent-header-2026-09-07` capture run, `02-adaptive-brightness-inspector.png`).

`Why the declaration order matters elsewhere too:` groups in a sheet area are packed
into columns by a greedy shortest-column algorithm over declaration order, not by an
explicit column API (`Registry.h`; see `crosshair.md`'s note on the same mechanism) —
unrelated to the budget, but the same "the shell has no per-group/per-param placement
API, only ordering" shape shows up twice this cycle.

The id `image.shaders.adaptive_brightness` and every config key from the Choice-row era
are unchanged; `overlay_e2_set image.shaders.adaptive_brightness 1` still means "on",
and the mode is now set the same way any Param is —
`overlay_e2_set image.shaders.adaptive_brightness.mode 1` for Dynamic (`Registry::FindParam()`
resolves a Param id exactly like an Entry id).

<details>
<summary>History: the three-way Choice (2026-09-06, superseded the next day)</summary>

Request #16 first asked only for a mode, and the row briefly became a three-way
**Choice** — `Off` | `Whole image` | `Dynamic` — in its own **Adaptive Brightness** band
below Effects, to stay under the six-param budget without a seventh param: on/off and
mode were treated as one decision ("which adaptation, if any"). It needed its own band
because the Effects band's `n / m` count is computed from **Switch** rows only
(`Shell.cpp`'s `DrawGroupBand`), and a Choice row among the switches would have made the
count read one short. Request #17 the next day asked for the opposite layout, which is
what the rest of this section describes.

</details>

#### The statistics (both modes)

Two dispatches per frame. **The measure pass** (`cs_effects_measure.comp`,
`SHADER_TYPE_EFFECTS_MEASURE`) takes a **128×128 grid of taps** over the whole image (16×16
threads, 8×8 per thread, 16384 taps — 64×64 / 4096 until 2026-09-07, see
[the pulse](shader-effects-adaptive-brightness-local.md#the-pulse-rank-cuts-on-a-bimodal-histogram-2026-09-07) below), each run
through `grade()` so it measures the Shadow-Control/Vibrancy-graded image the `.fx`
measured (its `PreSharpenOut` texture) — sharpening is not applied to the taps; it does
not move the statistics. From those taps it derives four numbers on the **encoded**
Rec.601 luma (`(.299, .587, .114)`, as the `.fx` computed it):

| History texel | Statistic | Read by |
| --- | --- | --- |
| `HISTORY_MEAN` (0) | arithmetic mean (shared-memory tree reduction, as before) | Whole image |
| `HISTORY_P2` (1) | mean of the taps ranked 1 %..3 % — the shadows | Dynamic (shadow cap) |
| `HISTORY_P50` (2) | mean of the taps ranked 25 %..75 % — the mid-tone anchor | Dynamic (gamma) |
| `HISTORY_P98` (3) | mean of the taps ranked 97 %..99 % — the highlights | Dynamic (levels gain) |
| `HISTORY_RAW` + 0..3 (4..7) | this frame's unsmoothed measurement of the same four | nothing — the `effects_ab_log` readback only |

The three ranked statistics are **rank-window means** — the mean luma of the taps whose
rank, sorted from black, falls in a window centred on the named percentile — computed
from a **64-bin histogram in shared memory** of counts *and* value sums (two
`atomicAdd`s per tap, then invocation 0 walks the bins once). The part of a bin that
falls inside the window contributes that many taps at the bin's own mean value, so on a
dense histogram this *is* the interpolated percentile at the window's centre: the flat
reference scenes below measure identically to the rank-cut percentiles they replaced, to
the code value. Where the histogram has an empty stretch the two differ, and that is the
point — a rank *cut* jumps across the gap on a single tap, a window *mean* moves by
`gap / window taps` (see the pulse section). `Why 64 bins:` 16384 taps over 64 bins is
256 per bin on a flat image; finer bins would be mostly empty and the in-bin mean already
recovers sub-bin precision. `Why the median (well, its window), not the mean, for
Dynamic:` a few bright windows in a dark room must not read as "the room is lit" — the
`.fx`'s 25 fixed taps could be swung by one highlight, the mean by a few percent of the
image, the 25..75 % window by neither (anything in the top quarter is outside it).

**Each statistic is smoothed with the `.fx`'s EMA**, unchanged, per statistic:

```
tau      = measured > adapted ? up_speed : down_speed
alpha    = ema_alpha(dt, tau)                     // effects_curve.h, unit-tested
adapted' = mix(adapted, measured, alpha)          // written to its history texel
```

**Whose `up_speed` / `down_speed`: the *active* effect's** (`u_adaptUp` / `u_adaptDown`,
renamed from `u_abUp` / `u_abDown` 2026-09-09). There is **one** EMA over statistics
neither effect owns, and the two adaptive effects are mutually exclusive, so the host
simply hands the shader whichever pair applies — see
[Adaptation speed](shader-effects-adaptive-gamma.md#adaptation-speed-2026-09-09--adaptive-gammas-own-pair) for the
correction this was, and note that it is *not* a case of Adaptive Gamma "sharing"
Adaptive Brightness's speeds deliberately: until 2026-09-09 it had none of its own and
was driven by sliders unreachable while it was the effect that was on.

`up_speed` is the time constant while a statistic **rises** (the scene got brighter; the
picture will be dimmed), `down_speed` while it **falls** (the scene got darker; the picture
will be lifted). `tau` is **seconds to ~63 % of a step**, so the settling time a user
observes is **3 tau** — measured at 907 / 3000 / 8511 ms for tau 0.3 / 1.0 / 3.0 s
([the table below](shader-effects-adaptive-gamma.md#measured-the-speed-control-2026-09-09)) against a closed form of
900 / 3000 / 9000. The panel now says exactly that — "Adapt to brighter" / "Adapt to darker",
retitled 2026-09-06 from "Brighten speed" / "Darken speed", whose help text described the
opposite direction to the one the code used. Defaults are symmetric (1 s / 1 s); the eye
adapts to brightness faster than to darkness, and a game wants the blown-out case fixed
fast too, so a user who wants asymmetry sets `up_speed` shorter. Every statistic is
measured and smoothed **whatever the mode**, and the measure pass never looks at the
Adaptive Brightness flags, so a mode switch needs no re-convergence.

#### Whole image (`mode: whole_image`) — the original behaviour

```
// cs_effects_layer0.comp, per pixel:
gain = clamp(target / max(adapted_mean, 0.001), min_gain, max_gain)
out  = mix(c, c * gain, strength)
```

One gain for the frame from the smoothed mean, ported 1:1 from the `.fx`'s
`PS_AdaptiveBrightnessApply`. Its known weakness is the reason Dynamic exists: in a dark
scene the gain hits `max_gain` and every value above `1 / max_gain` **clips** — the
"blown-out parts" the user asked to be rid of (measured below).

#### Dynamic (`mode: dynamic`) — levels + gamma + shoulder

The curve lives in **`src/shaders/effects_curve.h`**, a header of pure scalar functions
that the GLSL pass and the C++ unit tests (`tests/test_effects_curve.cpp`) compile from the
same text, so the properties below are asserted on the code the GPU runs. Encoded space
in, encoded out, per channel, from the smoothed `p2 / p50 / p98`:

```
   gmin = clamp(1 / max_gain, 0.25, 1.0)     // the gamma bounds are the user's own
   gmax = max(min(1.5, 1 / min_gain), 1.0)   //   gain bounds -- see "The ceiling" below
1. GAIN      G = clamp(max(0.9 / p98,             // levels: p98 -> WHITE
                           target^(1/gmin) / p50), //   ... or what Target needs
                       min_gain, max_gain)
2. GAMMA     g = ln(target) / ln(p50 * G), clamped to [gmin, gmax]  // median -> target
             if g > 1: g = min(g, ln(p2 * min_gain) / ln(p2 * G))   // shadow cap
3. SHOULDER  y = (x * G)^g;  top = G^g
             if top > 1 and y > 0.7:  u = y - 0.7;  y = 0.7 + u / (1 + u / c)
             with c = (top - 0.7)(0.3) / (top - 1), so x = 1 lands exactly on 1.0
out = mix(c, curve(c), strength)
```

- **Levels**: the smoothed 98th percentile is pulled toward `WHITE = 0.9`, as far as the
  user's gain bounds allow — a dark scene gets `max_gain`, a bright one a touch under 1.
  `Why no black-point subtraction, though "levels" usually has one:` pulling p2 toward black
  crushes the deepest shadows, which is the opposite of what a dark map needs, and capped
  small enough to be harmless on a mid scene it is also too small to do anything. Every
  step passes through (0, 0), so black stays black without a floor being needed.
- **Target's own demand on the gain** (2026-09-08): `target^(1/gmin) / p50` — the exposure
  the gain must supply so that the gamma, *at its floor*, can still land the median on the
  target. The **larger** of it and the white point wins, so the white point is a floor on
  the gain (a bright scene is never dimmed past putting p98 at 0.9) and Target keeps moving
  the picture once the gamma floor is reached instead of going flat. See
  [The ceiling](#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08).
- **Gamma**: the exponent that lands the smoothed **median** on `target` after the gain,
  bounded to `[gmin, gmax]`, both derived from the user's own gain bounds. `Why
  gmin = 1/max_gain:` Max gain then means "how bright may it go" in *both* channels — the
  exact mirror of `min_gain`, which already means "how dark may it go" through the shadow
  cap — and `max_gain` 2.0 reproduces the historical fixed floor 0.5 exactly, which is why
  every pre-2026-09-08 measured number is still reproducible at that setting. It is clamped
  to `[0.25, 1.0]`: below 0.25 a single code of near-black lands above 90 and sensor noise
  is all you see; 1.0 is "no lift at all", so `max_gain` 1.0 really does not brighten.
  `Why gmax = min(1.5, 1/min_gain):` a darkening gamma crushes shadows by nature and beyond
  1.5 the shadow cap is all that keeps detail; the `1/min_gain` half is the mirror again
  (`min_gain` 1.0 = "do not darken") and is slack at every shipped default (1/0.3 = 3.33),
  so it changes no measured number. **The shadow cap** is `min_gain`'s Dynamic meaning: a
  darkening gamma may not push the smoothed 2nd percentile below `p2 × min_gain` — "how
  dark may it go", applied to the shadows.

#### Why min_gain 0.3, max_gain 4.0 (2026-09-07 request: *"make min gain 0.3, max gain
4.0"*)

Ranges widened from 0.5..1.0 / 1.0..2.0 to **0.3..1.0 / 1.0..4.0**, and the schema
**defaults moved to the new extremes** — `min_gain` 0.5 → 0.3, `max_gain` 2.0 → 4.0
(`ConfigSchema.h`) — the plain reading of "make min gain 0.3, max gain 4.0". An existing
config keeps whatever value it already stored; only a config that never set these keys
(or a fresh install) sees the new numbers. `GAMMA_MIN`, `GAMMA_MAX`, `KNEE` and `WHITE`
(`effects_curve.h`) are deliberately **unchanged** by this request — a separate
highlight-rolloff change touching those four is being designed against the user
separately, and keeping them fixed here lets the two be judged independently.

Two consequences checked by measurement rather than assumed:

- **The shadow cap loosens.** `min_gain` is also "how dark may the 2nd percentile go"
  inside the gamma shadow cap (above). On the bright reference scene (p2=30, p50=225,
  p98=250, target 0.5, gain 0.918 unaffected since it is not at either bound): at
  `min_gain` 0.5 the cap itself was the binding constraint and held the shadows at
  exactly `p2 × 0.5` = **15**; at `min_gain` 0.3 the cap's own ceiling (`ln(p2 × 0.3) /
  ln(p2 × gain)` ≈ 1.503) now exceeds `GAMMA_MAX` 1.5, so **`GAMMA_MAX` becomes the
  binding constraint instead of the cap**, and the shadows land at **9** — measured
  identically off the GPU (`scripts/effects-regression.sh`'s `bright-dynamic`: rect
  9.0) and from the pure curve function (`tests/test_effects_curve.cpp`'s dedicated
  "min_gain 0.3 loosens the shadow cap" case). The cap's own invariant (never below
  `p2 × min_gain` = 9.0 exactly) still holds — 9.0..9.05 is within float rounding of
  it — so nothing is mathematically broken, but the **margin above "crushed" is real
  and roughly halved** (15 vs a floor of "distinguishable from black" versus 9 vs the
  same floor). Visually (the untracked `adaptive-gain-2026-09-07` capture run,
  bright scene, min_gain 0.5 vs 0.3, both at `max_gain` 4.0): the 30-value shadow
  rectangles read as very dark grey in both captures, not literally crushed to black,
  but the 0.3 capture is visibly the darker of the two — the loosened cap is a real,
  if secondary, effect of this request and not just a formula footnote.
- **The dark scene's shadows lift much further, without crushing anything.** In the
  dark reference scene the darkening branch of the gamma never engages (the scene needs
  a *lift*, not a darken), so the shadow cap is irrelevant there; the whole change is
  driven by `max_gain` alone. See the re-measured table below.
- **Shoulder**: only when the curve's own top (`G^g`, its value at x = 1) exceeds 1.0 —
  i.e. only when something *would* clip. Reinhard-shaped on the excess above the knee
  `0.7`, with `c` chosen so x = 1 lands exactly on 1.0: slope 1 at the knee (no visible
  break), monotonic for any overshoot, and it degenerates to the identity as the top → 1,
  so a scene that needs no compression gets none. `Why not a fixed asymptote below 1:` a mid
  scene's whites would dim for nothing; adaptive knee-to-top mapping keeps near-identity
  scenes near-identical.
- **Strength** blends the curve with the identity; at 0 the picture is untouched (tested).
- `Why encoded space, not linear light:` the pre-pass runs in encoded space on purpose (see
  "Encoded space, on purpose" above); the statistics, the sibling effects and `target`
  (an encoded mean target in Whole image mode) all live there; and the gain/gamma bounds are
  perceptual numbers — a 2× gain in encoded terms is a 2× perceptual step, which is what a
  user-facing "Max gain" should mean. The hard properties (monotonic, bounded, 0 → 0) hold
  in either space.

**Properties asserted on the CPU** (`[effects_curve]`, 25 cases, 7.94 M assertions over
seven scenes × five targets × twenty gain-bound pairs — widened 2026-09-07 from nine
pairs / 1.26 M assertions to cover the full 0.3..1.0 / 1.0..4.0 panel ranges, gain 4.0
included at every gamma in range): output in
`[0, 1]` and finite; monotonic in the input; `0 → 0`; `x = 1 → exactly 1` whenever the
shoulder is active; identity at strength 0; the mid reference scene is the identity; the
gamma clamps and the shadow cap hold; the shoulder is C0/C1-continuous at the knee; the
shadow cap at min_gain 0.3 is measurably looser than at 0.5 yet still holds its own
invariant (2026-09-07 case). Plus, from 2026-09-08: the gamma bounds come from the gain
bounds and can never invert; `max_gain` 2.0 reproduces the pre-2026-09-08 curve exactly;
`max_gain` 1.0 brightens **nothing**, at any target, on any scene; Target moves a realistic
dark frame monotonically and by more than 20 counts from 0.5 to 0.7; Max gain moves it by
more than 15 counts per step and monotonically over the whole panel range; and
`ab_dyn_binding()` returns exactly one in-range code with a non-empty wording for every
combination. Plus the config round-trip of `mode` and its unknown-value fallback.

#### Measured (desktop, headless, `scripts/effects-regression.sh`, re-measured 2026-09-07
at the new min_gain 0.3 / max_gain 4.0 defaults; superseded numbers at the old 0.5 / 2.0
defaults are kept alongside for comparison)

The recipe is `pixel-regression.sh`'s (private headless sway, nested `gamescope --backend
wayland`, `gamescopectl screenshot "<path> 4"`), with `tests/effects_scene_client.c` as
the game: an SDL2 window painting three synthetic scenes from flat regions at known pixel
positions — **dark** (bands 5/8/12/16/20, a pure-black corner, six 240 squares ≈ 1 % of the
image), **bright** (bands 200/215/230/245/255, six 30-valued rectangles ≈ 4 %, so p2 *is*
the shadows) and **mid** (26/77/128/179/230) — advanced with `SIGUSR1`. Every number is the
mean grey of a region's interior, all six Adaptive Brightness params at their defaults,
strength 1.0. Captures: the untracked `effects-regression` capture run (`20260907-055139`)
and, for a direct old-vs-new side-by-side at fixed scenes,
the untracked `adaptive-gain-2026-09-07` capture run.

> **2026-09-08 note.** The `Dynamic (new)` column below is the **2026-09-07** curve. The
> ceiling fix ([The ceiling](#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08))
> changed exactly one row of it — the dark scene's bands, which now read
> **88 / 107 / 127 / 143 / 157** with the 240 highlights and pure black unchanged at 254
> and 0. Every other row here was re-measured after the fix and is bit-identical.

| Scene / region (input) | Off | Whole image (new) | **Dynamic (new)** | Dynamic (old 0.5/2.0) | Dynamic must |
| --- | --- | --- | --- | --- | --- |
| dark: darkest band (5) | 5 | 20 | **71** → 88 (2026-09-08) | 50 | be readable: ≥ 30 |
| dark: bands 8 / 12 / 16 / 20 | 8/12/16/20 | 32/48/64/80 | **90 / 111 / 128 / 143** → 107 / 127 / 143 / 157 | 64/78/90/101 | keep their order |
| dark: 240 highlights | 240 | **255 — clipped** | **254** | 253 | stay < 255, above every band |
| dark: pure black | 0 | 0 | **0** | 0 | stay ≤ 2 |
| bright: bands 200 / 215 / 230 | 200/215/230 | 115/124/132 | **152 / 169 / 187** | 165/180/196 | keep their order |
| bright: 245 band (p98 region) | 245 | 141 | **206** | 213 | come down below 235 |
| bright: white (255) | 255 | 146 | **218** | 224 | — |
| bright: 30 shadows (p2) | 30 | 17 | **9** | 15 | not crushed: ≥ 8, and ≥ 30 × min_gain (0.3 → 9.0) |
| mid: 26 / 77 / 128 / 179 / 230 | identical | 26/77/128/178/229 | **25 / 75 / 126 / 177 / 228** | 25/75/126/177/228 | near-identity: worst ≤ 6 (measured 2) |

Widening `max_gain` to 4.0 roughly doubles how far Dynamic lifts a dark scene: the
darkest band goes from 50 to **71** (a √2× step, since the curve's gamma floor stays at
0.5 and the gain doubled), and every darker band lifts proportionally more — genuinely
more readable shadow detail, still monotonic, still nowhere near clipping (254, two
counts under white, essentially unchanged from the old 253). Whole image on the dark
scene is still the blown-out case the user described, more so now: the gain hits the new
`max_gain` 4 and the 240 highlights still go to 255 with far more of the image clipping
above it, while the darkest band only reaches 20 (was 10 at the old `max_gain` 2). This is
exactly the contrast Dynamic exists to fix, sharper than before.

On the **bright** scene the honest, less flattering half of this change: `max_gain` 4.0
does not touch the bright scene at all (its gain, 0.918, sits well inside both the old and
new bounds), but `min_gain` 0.3 pulls every highlight number **down** relative to the old
0.5 default — the 245-band figure that "comes down below 235" now reads **206** instead of
213, and white itself lands at **218** instead of 224. That is *more* compression of the
highlights, not less: the user's standing complaint is "some things are still overblown",
and on this scene alone the new defaults compress the top end **harder**, not softer — a
real trade-off of the wider range, not a pure win. It happens because the same gamma that
governs the highlight fall-off is now clamped to `GAMMA_MAX` 1.5 (unchanged, per the
task's constraint) instead of the old, tighter shadow cap — see "Why min_gain 0.3, max_gain
4.0" above for the mechanism. The shadow cap itself is why: at `min_gain` 0.5 the cap held
gamma to 1.273; at `min_gain` 0.3 the cap's own ceiling (≈1.503) now exceeds `GAMMA_MAX`
1.5, so gamma rides the fixed 1.5 ceiling instead — a bigger darkening exponent, which is
what drags every bright-scene number down together. Whole image is unaffected on this
scene either way (INFO only, not asserted) since its one linear gain, 0.567, is inside
both ranges. `Why the 2nd-percentile shadows are the sharpest change (15 → 9):` the shadow
cap is `min_gain`'s own knob for that value specifically — see above.

**Temporal** (dark → bright under Dynamic, `tau` 1 s; the "requested at" times are
measured from the `SIGUSR1` to the screenshot request, the screenshot itself adds a
frame): the middle (230) band read **254** at 0.37 s, **245** at 1.28 s, **198** at 3.35 s,
and **187** settled — monotonic, no overshoot, within 12 counts of settled at 3 s (down
from 252/245/205/196 at the old 0.5/2.0 defaults — the settled value is lower for the same
"compresses the highlights harder" reason as the table above). The first second looks slow
because while the smoothed gain is still above 1 the shoulder pins the bright bands near
white; once the gain drops under 1 the curve releases them. No oscillation was seen in any
capture. Per frame (`transition-gain`, the `effects_ab_log` trace the script takes across
the same switch): the gain goes 4.000 → 0.905 monotonically, never below its settled value,
within 5 % of it at **1.73 s**; the 230-band pixel goes 254 → 188 monotonically, within 5 %
at 3.9 s (the shoulder holds it near white while the gain is still above 1). These are the
EMA's numbers and the 2026-09-07 estimator change did not touch the EMA, so they are the
same before and after that fix.

#### Does Dynamic actually work above `max_gain` 2.0? (2026-09-07 — measured, yes, **on this scene only** — superseded 2026-09-08)

> **Superseded.** Everything below is still true *of the flat dark band chart*, and that
> is the problem: the answer generalised from a scene whose statistics are nothing like a
> game's. See [The ceiling](#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08)
> immediately after it. The numbers in the table below are the pre-2026-09-08 curve's and
> are kept as the "before" side of that measurement.


Asked directly (*"Does the adaptive brightness even work above 2.0 gain? Check that."*),
and answered by sweeping the knob on the dark reference scene rather than by reading the
code. Dynamic, everything else at the schema defaults, Local adaptation forced to 0 so
this is the global curve alone; the applied gain is `effects_ab_log`'s, the band values
are the capture's
(the untracked `adaptive-local-2026-09-07` capture run, `dark-maxgain-{1.5,2,3,4}.png`):

| `max_gain` | applied gain | gamma | dark bands 5 / 8 / 12 / 16 / 20 | 240 lights |
| --- | --- | --- | --- | --- |
| 1.5 | **1.500** | 0.500 | 44 / 55 / 68 / 78 / 87 | 252 |
| 2.0 | **2.000** | 0.500 | 50 / 64 / 78 / 90 / 101 | 253 |
| 3.0 | **3.000** | 0.500 | 62 / 78 / 96 / 111 / 124 | 254 |
| 4.0 | **4.000** | 0.500 | 71 / 90 / 111 / 128 / 143 | 254 |

**The gain reaches its bound exactly, at every setting, and the picture keeps getting
brighter past 2.0** — nothing on the path clamps earlier. Checked one by one: `ab_dyn_gain`
clamps to `[min_gain, max_gain]` and lands *on* `max_gain` here (a 20-code p98 wants
`0.9 / 0.078` = 11.5, far above any bound); the gamma clamps do not touch the gain; the
shadow cap only exists for a *darkening* gamma (`g > 1`) and this scene's gamma is 0.5, its
opposite bound; the shoulder compresses only above the 0.7 knee, and the 240 lights land at
252–254 rather than clipping; `EffectsPushData_t` passes `flAbMaxGain` through untouched;
and the panel's own `Range( 1.0f, 4.0f )` is the only ceiling in the system, at 4.0.

**But the returns are square-root, and that is worth knowing.** On any scene dark enough to
want maximum lift, the gamma is pinned at `GAMMA_MIN` 0.5, so the curve is `(x·G)^0.5` and
doubling `G` multiplies the output by only **√2**: 50 → 71 for a 2× gain step, not 50 → 100.
That is the honest shape of "does it work above 2" — it works, and each further stop of Max
gain buys 41 % rather than 100 %, because the *gamma floor is already saturated*, not because
anything clamps the gain. Raising `GAMMA_MIN` would be the lever there, and it is deliberately
not touched (see the constant's note in `effects_curve.h`).

#### The ceiling: Target brightness and Max gain went inert (2026-09-08)

> *"It still feels like anything above target brightness 0.5 and max gain 2.0 does
> anything at all"* — read with the missing negation: raising either appears to do
> nothing. Reported straight after the table above had "proved" Max gain works.

Both halves were true, for two independent reasons, and the section above is exactly how
the second one stayed hidden: **a flat band chart flatters this operator.**

##### What was measured, before anything was changed

`effects_curve.h` was evaluated on the CPU — the same header text the GPU compiles — over
seven scenes × the whole 0.1..0.9 Target range × Max gain 1.5/2/3/4, with a mirror of
`cs_effects_measure.comp`'s tap grid, 64-bin histogram and rank-window means producing each
scene's statistics. The harness reproduces the GPU's dark-scene capture **code for code**
(71/90/111/128/143), which is what makes the rest of its numbers usable. Beside the four
synthetic scenes it was run on two **real photographs** resized to 1280×720 — a night-city
frame and a moonlit one — because a continuous histogram is the thing the band charts do
not have; no real game frame existed among the untracked verification captures to use instead.

| scene (measured p50 / p98) | `0.9/p98` — all Max gain ever saw | Target inert above | which clamp binds there |
| --- | --- | --- | --- |
| dark band chart (0.047 / 0.153) | **5.90** | 0.436 | gamma at its 0.5 floor |
| textured dark, 2 % lights (0.075 / 0.318) | **2.83** | 0.461 | gamma floor |
| night-city photograph (0.141 / 0.378) | **2.38** | 0.581 | gamma floor |
| moonlit photograph (0.076 / 0.440) | **2.05** | 0.394 | gamma floor |
| half-dark/half-bright (0.449 / 1.000) | 0.90 | 0.635 | gamma floor |
| mid (0.502 / 0.902) | 1.00 | 0.708 | gamma floor |
| bright (0.898 / 1.000) | 0.90 | 0.899 (and inert *below* 0.726) | shadow cap / `GAMMA_MAX` |

- **Target** reached the picture *only* through `g = ln(target) / ln(p50·G)`, clamped to a
  fixed `[0.5, 1.5]`. Solving `g = 0.5` gives `target = sqrt(p50·G)` — every target above
  that produces the identical clamped gamma. The shipped default 0.5 already sat on the
  dead side of that threshold on three of the four dark scenes.
- **Max gain** only ever fed `clamp(0.9/p98, min, max)`. A realistic frame has enough
  bright content that the demand is ~2, so **above it the clamp never binds and the slider
  is an exact no-op** — not diminishing returns, zero. The band chart's p98 of 0.15 asks
  for 5.9, which is why every setting up to 4.0 bit there and the 2026-09-07 table read as
  a clean pass.

Confirmed on the GPU with the *old* binary, `effects-regression.sh` on the textured dark
scene, frame mean: Target 0.3 / 0.5 / 0.7 read **83.0 / 112.1 / 112.1** (the last step
`+0.0`), and Max gain 3 vs 4 read **112.06 vs 112.06** — byte-identical.

##### The fix, and why this shape and not the obvious ones

1. **`gamma_min = clamp(1 / max_gain, 0.25, 1.0)`** replaces the fixed 0.5. Max gain now
   governs "how bright may it go" in both channels — the exact mirror of `min_gain` through
   the shadow cap — and this is what makes Max gain move a *realistic* frame, whose
   white-point demand saturates around 2. On the moonlit frame a 5-code input reads
   24 / 50 / 77 / 77 at Max gain 1.5 / 2 / 3 / 4 where it read **51 at every one of them**
   before. `Why 2.0 is the anchor:` `1/2.0` is exactly the old floor, so every measured
   number taken before this change is still reproducible at that setting — asserted in
   `tests/test_effects_curve.cpp`. `Why the 0.25 floor:` at 0.25 with a 4× gain, one code
   of near-black lands at 90; lower than that and shadow noise is the whole picture.
   `Why the 1.0 ceiling:` it removes a real lie — at Max gain 1.0, "do not brighten", the
   old gamma still lifted a 20-code band to **71**.
2. **The gain also carries `target^(1/gamma_min) / p50`**, the smallest exposure that
   leaves the target reachable at the gamma floor, and the larger of it and the white point
   wins. Past the floor the gain takes over, so Target keeps moving the picture.
   `Why not the simpler target / p50:` that is a *bigger* gain, and moving lift out of the
   toe into a linear gain darkens the shadows at the same median — measured, it dropped the
   half-dark/half-bright scene's dark half from 12/17/23/28/34 to **6/9/13/18/22**. The
   smallest-sufficient form leaves that scene bit-identical.
3. **`ab_dyn_binding()`** — see [Which limit is binding](#which-limit-is-binding-2026-09-08)
   below. Rejected alternatives: *lowering `GAMMA_MIN` alone* fixes neither half (Max gain
   stays inert wherever the gamma is free, and a fourth-root floor for everyone is milky);
   *making Target a direct exposure* costs the bright scene its highlights (245-band
   206 → ~142, the blown-out look Dynamic exists to avoid) and the split scene its shadow
   lift; *a new control* was refused on the parameter budget (`kParamBudget` is at 8 and
   already owes a rail-area split) and would not have been honest anyway — the sliders that
   say they do this had to start doing it.

##### After: what each slider is worth now (GPU, `effects-regression.sh`, frame mean)

| sweep, textured dark scene | before | after |
| --- | --- | --- |
| Target 0.3 → 0.5 → 0.7 (Max gain 4) | 83.0 → 112.1 → **112.1** | 82.8 → 129.4 → **175.4** |
| Max gain 1.5 → 2 → 3 (Target 0.5) | 90.0 → 103.1 → 112.1 | 65.7 → 103.0 → 129.4 |
| Max gain 3 → 4 (Target 0.5) | 112.06 → 112.06 | 129.39 → 129.39 (**still equal** — see below) |

And the reference scenes at the shipped defaults, off the same two runs:

| scene | before | after |
| --- | --- | --- |
| dark: bands 5/8/12/16/20 | 71 / 90 / 111 / 128 / 143 | **88 / 107 / 127 / 143 / 157** |
| dark: 240 highlights, pure black | 254, 0 | 254, 0 — unchanged |
| bright: 200/215/230/245/255, 30-shadows | 152/169/187/205/218, 9 | **identical** |
| mid: 26/77/128/179/230 | 26/77/128/178/229 | **identical** |
| halfsplit at Local 0 and 50 % | 12/17/23/28/34 · 195/207/217/228/235 | **identical** |
| halo amplitude, halobox / haloinv at 50 % and 100 % | +8 / +15, −7 / −14 | **identical** |
| still-frame peak-to-peak at Local 0 / 50 / 100 | 0 | **0** |
| panning gain spread (300 frames) | 0.0324 | 0.0191 |

The dark scene is the only thing that moved, and it moved brighter: at Max gain 4 the
gamma the median asks for (0.417) is no longer clamped up to 0.5.

##### The residual ceiling, stated plainly

**Once the mid-tones are on Target, Max gain is no longer a brightness control.** The
median is pinned by construction, so a higher Max gain only shifts the same mid-tone level
out of the toe and into the linear gain: the deepest shadows come out slightly darker and
everything above the mid-tones slightly brighter (dark chart at target 0.5, inputs 5 / 20:
Max gain 3 → 93 / 152, Max gain 4 → 84 / 157). That is why Max gain 3 and 4 still produce
the same textured-dark picture at the default target, and why the readout below names
**Target brightness** in that state — it is the control that can still move the picture.
Target itself is limited by the user's own Max gain: on the flat dark chart at Max gain 4
it stops mattering above 0.70 (up from 0.436), on the moonlit frame above 0.75.

##### Which limit is binding (2026-09-08)

The most expensive part of this was never the ceiling — it was that **a clamped slider
looks exactly like a working one**, so a session went into dragging controls that could
not respond. `ab_dyn_binding()` classifies, from the same three statistics and the same
bounds the curve uses, which constraint is currently stopping the picture, and
`ab_binding_text()` is the single wording of the six codes:

| code | shown as |
| --- | --- |
| `AB_BIND_NONE` | `none -- the mid-tones are on Target brightness` |
| `AB_BIND_GAIN_MAX` | `gain is at Max gain` |
| `AB_BIND_GAIN_MIN` | `gain is at Min gain` |
| `AB_BIND_LIFT` | `Max gain -- Target brightness does no more here` |
| `AB_BIND_DARKEN` | `the darkening limit -- Target brightness does no more here` |
| `AB_BIND_SHADOW` | `Min gain, holding the shadows up` |

It appears twice, from one function, so a trace and the panel cannot disagree: appended to
every `effects_ab_log` line as `bind=<n> (<text>)`, and as a third `LIVE` line —
`adaptive limit` — on the Shaders area's **Pipeline** facts row
(`AbPreview_BindingLine()`, which classifies the frame the Inspector's before/after preview
already captures and arms that capture itself, so the row fills in on its own).
`Why the frame's global curve and not the pixel's:` Local adaptation redistributes gain
*inside* these same bounds and never widens them, so the frame's answer is the one that
tells the user which knob to reach for. In Whole image mode the row names that mode's own
single gain bound instead of pretending the Dynamic classifier applies. Captured at three
settings in the untracked `adaptive-ceiling-2026-09-08` capture run.

