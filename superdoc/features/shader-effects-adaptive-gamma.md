# Shader effects: Adaptive Gamma and Leave dark scenes alone

Adaptive Gamma and the per-effect dark-scene floor shared by Adaptive Brightness and Adaptive Gamma. Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

### Adaptive Gamma (`image.shaders.adaptive_gamma`) — NEW 2026-09-08

The user's request, verbatim: *"Make something similar, but make it gamma based. Call it
adaptive gamma."* — "similar" meaning
[Adaptive Brightness](shader-effects-adaptive-brightness.md#adaptive-brightness-imageshadersadaptive_brightness) above.

Built as the **exponent-only** operator that description implies: the same measured
statistics, and the whole effect is *one exponent*. No levels gain, no white point, no
shoulder, no shadow cap. It is not a cheaper Adaptive Brightness — it is a different
thing that a gain cannot be, for a reason that is arithmetic rather than taste (below).

**A near-black scene has a floor of its own** (2026-09-14) — see
[Leave dark scenes alone](#leave-dark-scenes-alone-2026-09-14--one-per-effect)
near the bottom of this page: below this effect's own `dark_floor` param (its eighth,
"Leave dark scenes alone") the exponent fades to 1 (the identity) rather than being driven
to its floor by a median near zero, which is exactly the shape a near-black scene forced it
into before this existed. Adaptive Brightness has the identical control, as its own ninth
param since 2026-09-16 (a standalone row before that) — the two started as one shared
control and split into independent ones the same day; see that section for why.

**Config**: `ReshadeAdaptiveGammaSettings` (`ConfigSchema.h`) — `enabled` (false),
`target_luminance` (0.5), `max_lift` (4.0), `max_darken` (1.5), `strength` (1.0),
`adapt_up_speed` (1.0) and `adapt_down_speed` (1.0) (both added 2026-09-09, see
[Adaptation speed](#adaptation-speed-2026-09-09--adaptive-gammas-own-pair)),
`local_strength` (0.0). Purely additive keys, so `kCurrentSchemaVersion` stays **4** and
there is no migration: an old profile has none of them and takes these compiled-in
defaults, exactly the shape [Shadow Control](shader-effects-color.md#shadow-control-imageshadersshadow_lift) was
added in.

#### The formula

`src/shaders/effects_curve.h`'s `ag_*` block — the same header the GLSL pass and
`tests/test_effects_curve.cpp` both compile, so the properties below are asserted on the
code the GPU runs. Encoded space in, encoded out, per channel:

```
gmin = clamp(1 / max_lift,  0.25, 1.0)          // the user's own bounds
gmax = clamp(max_darken,    1.0,  4.0)
r    = ab_local_shift(local_mean, mean, local_strength)   // exactly 1.0 when off
g    = clamp( ln(target) / ln(p50 * r), gmin, gmax )
out  = mix(x, x^g, strength)
```

`g` is the exponent that lands the smoothed **median** on Target. Median, not mean, for
the reason Dynamic mode gives: a few bright windows in a dark room must not read as "the
room is lit".

#### Why an exponent alone is a different effect

For `x` in `[0, 1]` and any `g > 0`, `x^g` is again in `[0, 1]`, with **0 and 1 as exact
fixed points** and strictly increasing in between. Three consequences, and each is why a
piece of Adaptive Brightness's machinery is *absent* here rather than merely omitted:

- **It cannot clip, at any setting** — so it needs no shoulder, no knee and no white
  point. Adaptive Brightness needs all three because its levels gain multiplies and can
  push a value past 1.0; there is nothing here to protect the highlights *from*. Measured:
  on the dark reference scene the 240 highlights read **251** at the defaults and never go
  above 251 at any setting tried, while Whole-image mode drives that same region to a
  **clipped 255**. A near-white value stays near-white *and still distinct from white*.
- **It changes contrast, not exposure.** A gain moves every value by the same factor; an
  exponent moves the mid-tones a lot and the two ends not at all. So the picture's black
  and white points are untouched by construction and only the shape between them adapts.
  That is a different look — flatter blacks stay black, which on a dark game is either
  exactly what you want (no milky lifted blacks) or not enough (the deepest shadows are
  never raised off the floor). The doc's job is to say which it is, not to pretend the
  trade does not exist.
- **It is cheaper**: one `log` and one `pow` per channel, no gain, no shoulder branch, and
  with Local adaptation off the exponent is a frame constant.

#### The bounds are the user's own — and why that is the whole point

Target reaches the picture **only** through this exponent. So whatever clamps the exponent
also decides where Target stops working — and the most expensive lesson of 2026-09-08 was
that [a clamped slider looks exactly like a working one](shader-effects-adaptive-brightness.md#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08).
With a fixed `[0.5, 1.5]` — the constants Adaptive Brightness used to carry — Target on
this row would go inert above `sqrt(p50)` with nothing on screen saying so and no control
to reach past it. That is the exact bug, rebuilt.

So the two bounds are **params**, `Max lift` and `Max darken`, and the header's hard
clamps are the panel's own `Range()` ends (`AG_LIFT_MAX` / `AG_DARKEN_MAX`, both 4.0) —
asserted equal in `test_effects_curve.cpp`, so a slider always reaches the bound it says
it does and there is no second, hidden ceiling underneath it.

- **Max lift** `L` sets the exponent **floor** to `1 / L`. `1.0` means "do not brighten",
  exactly (pinned by test at every target on every scene). The default **4.0** gives a
  floor of 0.25 — the same floor `AB_DYN_GAMMA_MIN` allows Adaptive Brightness at
  `max_gain` 4.0, one shared constant with one justification: below a fourth-root lift a
  single code of near-black lands above 90 and sensor noise is the whole picture.
- **Max darken** `D` **is** the exponent ceiling. `1.0` means "do not darken", exactly.
  The default is **1.5**, not 4.0: a darkening exponent crushes shadows by nature and this
  operator has no shadow cap to hold them up (that is Adaptive Brightness's `min_gain`,
  which has no counterpart here), so the shipped default stops where Adaptive Brightness's
  own `GAMMA_MAX` stops. Measured cost of going past it, bright reference scene: at
  `max_darken` 1.5 the 30-value shadows read **10**; at 3.0 they read **0**. Ordering is
  kept either way (the gate asserts it), but detail is genuinely gone — available on
  purpose, not a default.

**The residual ceiling, stated plainly.** Because the exponent is the only mechanism, the
highest target it can reach on a frame is **`p50 ^ (1 / max_lift)`**. On the textured dark
scene (`p50` = 0.076) with Max lift at the top of its slider that is **0.52**, so Target
above about 0.5 does nothing there — measured: 0.2 / 0.35 / 0.5 move the frame mean
**55.1 → 91.3 → 128.0**, and 0.7 produces the same picture as 0.5 with the readout saying
`Max lift -- Target brightness does no more here`. Adaptive Brightness gets further on the
same frame because its gain carries what the exponent cannot; that is the honest price of
"only an exponent", and it is *named* rather than silent.

#### Which limit is binding

`ag_binding()` / `ag_binding_text()`, the same two-part pattern
[`ab_dyn_binding()`](shader-effects-adaptive-brightness.md#which-limit-is-binding-2026-09-08) uses — codes in the shared header
because it compiles as GLSL too, wording on the C++ side — so the panel and the
`effects_ab_log` trace print one string and cannot drift:

| code | shown as |
| --- | --- |
| `AG_BIND_NONE` | `none -- the mid-tones are on Target brightness` |
| `AG_BIND_LIFT` | `Max lift -- Target brightness does no more here` |
| `AG_BIND_DARKEN` | `Max darken -- Target brightness does no more here` |
| `AG_BIND_STRENGTH` | `Strength is 0 -- nothing is applied` |

Every control this row owns can be inert, and each of those states has a code: Target when
the exponent is clamped (LIFT / DARKEN); Max lift and Max darken when the target is
already reached (NONE — "nothing is stopping the picture", so raising the limit that is
not binding buys nothing); Strength at 0, which makes *all* of them inert and is the one
case a limit code would blame the wrong control for. `Why a fourth code rather than
reusing Adaptive Brightness's six:` the wordings name controls, and this row has no "Max
gain" or "Min gain" — pointing a user at a slider that is not on the page is precisely the
near-miss the readout exists to prevent. `test_effects_curve.cpp` asserts the equivalence
the readout claims: whenever it says a limit binds, a further step of Target really does
change nothing, and whenever it says NONE, Target really does still move the picture.

The line appears in both places Adaptive Brightness's does — appended to every
`effects_ab_log` line as `bind=<n> (<text>)`, and as the Shaders area's **Pipeline** facts
row `adaptive limit`. That row now serves **both** adaptive effects and never has to
choose between them, because they cannot both be on.

#### Local adaptation — included, and it is the same operator

`local_strength` reuses [`ab_local_shift()`](shader-effects-adaptive-brightness-local.md#local-adaptation-local_strength-2026-09-07--one-curve-per-neighbourhood)
verbatim: the measure pass's 16×16 map, the `r = clamp(local/mean, 0.25, 4)` deviation
clamp, the `r^strength` geometric blend, the hand-rolled bilinear sample. Only what the
shifted median feeds changes — `ag_gamma(p50 * r_eff, …)` instead of the gain/gamma pair.

`Why include it at all, on an effect whose selling point is simplicity:` the map costs the
measure pass **nothing** (it is the per-thread tap sums it already computes), the apply
pass pays four cached `texelFetch`es *only above 0*, and without it a half-dark /
half-bright frame is unfixable by any single exponent — the same failure that made local
adaptation necessary for Adaptive Brightness. Reusing the existing operator means one
definition of "how bright is this part of the frame", one halo argument, and one set of
measurements; inventing a second would be two things to keep true.

`Why the default is 0.0, where Adaptive Brightness defaults to 0.5:` this effect's whole
identity is the cheap, purely global exponent, so that is what it should be out of the
box — and it makes the shipped default exactly the operator the user asked for, "only an
exponent", with no per-pixel work at all. Local adaptation is available and opted into.
The consequence, stated rather than buried: **on a mixed-brightness frame Adaptive Gamma
does nothing about the mix until you raise this slider**, and on an evenly-lit frame the
slider does nothing at all (every cell's ratio is 1 by construction — a no-op that is
correct, not broken, and is why it is not classified as a "binding limit" above).

Bounded exactly as Adaptive Brightness's is: `ag_gamma` clamps *after* the shift, so a
locally-adapted pixel can never leave `[1/max_lift, max_darken]` — local adaptation
redistributes inside the user's bounds and never widens them. Asserted over every
scene × bound × ratio × strength combination on the CPU.

#### Adaptive Gamma vs Adaptive Brightness: mutually exclusive

**Decided and enforced, not left undefined.** Both effects aim the frame's mid-tones at a
Target, and both read the **same** statistics — the measure pass grades its taps but knows
nothing about either effect, so its `p50` is always the *pre-effect* median. Run together,
the second operator fits its curve to a median the first has already moved and the
correction is applied **twice**. On the textured dark scene that is not "both, a bit": the
frame Adaptive Brightness settles at a mean of ~129 would be taken to about **212** by
Adaptive Gamma's exponent (0.269) on top of it — a washed-out picture no setting of either
effect fixes.

So turning either switch on turns the other off (`PanelShaders.cpp`'s
`SetAdaptiveBrightnessEnabled` / `SetAdaptiveGammaEnabled`), and `EffectsPushData_t` drops
Adaptive Gamma's flag and neutralises its uniforms whenever Adaptive Brightness is also
on, so a **hand-edited config** with both cannot produce the double correction either
(Adaptive Brightness wins: it is the older, richer effect and the one every existing
config, capture and doc points at).

`Why a radio and not a greyed-out switch:` greying Adaptive Gamma out while Adaptive
Brightness is on leaves a config that somehow has both **stuck** — neither row toggleable
— and greying only one of the pair is an asymmetry a user cannot infer. Turning one on
turning the other off is a familiar interaction and it is *visible*: both switches sit in
the same Effects band, so the other one goes dark in the same frame. `Why not compose them
and document the result:` the composed result is not a look anybody would choose, and
"both on does something surprising" is exactly the class of undefined behaviour this
project keeps having to go back and fix.

Measured (`effects-regression.sh`'s `ag-exclusive`, driven through `overlay_e2_set`, i.e.
through the same setter a click uses): with Adaptive Gamma on, turning Adaptive Brightness
on leaves `image.shaders.adaptive_gamma` reading **off**; turning Adaptive Gamma back on
leaves `image.shaders.adaptive_brightness` reading **off**. The three captures either side
of those flips read frame means **128.2 | 129.1 | 128.3** — one effect's picture or the
other's, and back again, never the ~212 a compounded frame would be.

#### Adaptation speed (2026-09-09) — Adaptive Gamma's own pair

The user's request, verbatim: *"For adaptive gamma, there should also be some value, to
adjust the speed of it."*

**First, the correction, because the previous text implied something that was not true.**
Adaptive Gamma shipped on 2026-09-08 with no speed setting at all. It was not sharing
Adaptive Brightness's on purpose, and it was not "using the defaults" — the measure pass's
EMA simply read `u_abUp` / `u_abDown` unconditionally, i.e. `reshade.adaptive_brightness`'s
`adapt_up_speed` / `adapt_down_speed`, whatever effect was on. Because the two effects are
[mutually exclusive](#adaptive-gamma-vs-adaptive-brightness-mutually-exclusive), that meant
Adaptive Gamma's adaptation rate was governed by **two sliders that are not reachable while
Adaptive Gamma is the effect running**: to change how fast it followed the scene you had to
turn it off, switch to Adaptive Brightness, move a slider there, and switch back — and
nothing in the panel said so. That is an omission with a user-visible consequence, so it is
filed under *Fixed* as well as *Added* in the changelog.

**Two controls, not one.** The request says "some value", singular, and Adaptive Gamma's
whole identity is being the effect with fewer knobs — so this was decided against that
grain rather than by symmetry with Adaptive Brightness:

- The **plumbing is free either way.** The EMA already takes two time constants; a single
  combined speed would be implemented as `up = down = one_value`, i.e. a *restriction* of
  what the shader can already express, not a simplification of anything. Shipping one
  would cost a capability and save no code.
- The **asymmetry is measurable and large.** With `Adapt to brighter` at 0.3 s and
  `Adapt to darker` at 3.0 s, a brightening step settles in **320 ms** and a darkening step
  in **10 495 ms** — a factor of 33 on the same picture
  ([`ag-speed-asymmetry`](#measured-the-speed-control-2026-09-09)). "Quick to react when a
  light comes on, slow to open up as it goes dark" is a real setting; one number cannot
  express it, and it is precisely the argument that kept Adaptive Brightness's own pair
  intact when [the budget was tight](shader-effects-adaptive-brightness.md#the-budget-decision-seven-params-not-six-2026-09-0607).
- **This row already pairs its directions.** `Max lift` and `Max darken` are one bound
  each way, deliberately, for exactly the reason a floor and a ceiling are not one number.
  A single speed would have been the only control on the row that collapsed a direction.
- **What one would have cost, stated rather than waved at:** nothing in code, and the
  whole of the 33× above in behaviour. What *two* costs is one Inspector row.

**The params**: `Adapt to brighter` (`up_speed`) and `Adapt to darker` (`down_speed`),
both `0.1 .. 5.0 s`, step 0.1, default **1.0** — the same range, step, unit, titles and
help wording as Adaptive Brightness's, and the same defaults **on purpose**: switching
between the two mutually exclusive effects must not change how fast the picture follows
the scene, and a user who has tuned one row's feel should find the other starting from the
same place. `test_effects_curve.cpp` asserts the two defaults are equal to Adaptive
Brightness's rather than just equal to 1.0, so they cannot drift apart silently.

**The plumbing: one EMA, the host picks.** `EffectsPushData_t` sets
`u_adaptUp` / `u_adaptDown` from Adaptive Gamma's pair when Adaptive Gamma is the effect
running and from Adaptive Brightness's otherwise. `Why that is safe and not a race:` the
measure pass smooths statistics **neither effect owns** (it grades its taps and knows
nothing about either), the exclusion is resolved in the same constructor that drops
Adaptive Gamma's flag, and `g_nativeEffects` is read once into a local `state` per
composite on the steamcompmgr thread — so there is no frame that wants both pairs and no
torn read that could produce one. A second EMA, a second history row and a second dispatch
were all avoidable, and were avoided. **With neither effect on**, the EMA keeps Adaptive
Brightness's pair: the history is still tracked whenever any effect runs the pre-pass, and
that is the row whose sliders a user can see. Both values are clamped to the panel's own
0.1 s floor in the host — the same rule `u_agStrength`, `u_agLocal`, `u_abLocal` and the
three Bloom fields already follow in that constructor, so the uniform says exactly what
the frame did and a hand-edited value below any reachable slider position cannot become a
speed nobody can see or set — and `ema_alpha()` floors tau again in the shader, so even a
`0` is "as fast as the control goes", never a frozen history and never a divide by zero.

**The binding readout is unchanged, and that is a decision.** [`ag_binding()`](#which-limit-is-binding)
has four codes because every control on the row *could be inert*: Target when the exponent
clamps, the two limits when it does not, Strength at 0. **A speed cannot be inert.** It
clamps nothing and gates nothing — it only decides *when* the exponent arrives at a value
the other controls already determined, and the slider's own floor of 0.1 s (with the host's
and `ema_alpha()`'s floors under it) means no reachable setting stops the picture adapting.
`test_effects_curve.cpp`'s "no setting can freeze the history" case pins that, including
for a hand-edited `0` or a negative. So there is no fifth code: adding one would have to
say "your speed is fine", which is not a limit and is not something the row's other
controls need warning about. The trace *does* now report the pair (`tau=<up>/<down>` on
every `effects_ab_log` line), because "what speed did this frame actually run at" is a
measurement question, not a UI one.

##### Measured: the speed control (2026-09-09)

`scripts/effects-regression.sh`'s new `ag-speed-*` checks, same headless recipe as
everything else here. The step is the client's `dark` → `bright` switch; `t0` is the frame
the raw statistics jump (found in the data, not assumed from a sleep) and **t95** is the
first frame after it within 5 % of where the trace lands. Captures and traces:
the untracked `adaptive-gamma-speed-2026-09-09` capture run.

| `Adapt to brighter` = `Adapt to darker` | closed form (3 tau) | **statistic** (smoothed p50) | **picture** (the exponent) |
| --- | --- | --- | --- |
| 0.3 s (fast) | 900 ms | **907 ms** | **336 ms** |
| 1.0 s (default) | 3000 ms | **3000 ms** | **1094 ms** |
| 3.0 s (slow) | 9000 ms | **8511 ms** | **3319 ms** |

Two columns because they answer two questions, and reporting only one of them would
mislead. The **statistic** is the EMA's own output and lands **exactly** on the closed
form at the default (3000 against 3000) and within 1 % at the fast end; the slow row reads
5 % short only because an 800-frame trace is 13 s and the reference "settled" value is the
last frame of it. The
**picture** arrives *sooner* than the statistic on this particular step because the
exponent saturates against `Max darken` (1.5) partway through — the frame is on the
brightest reference scene, where the target asks for an exponent of about 6.4 — so the
visible change is over before the median has finished moving. That is the clamp working,
not the EMA running fast, and it is why the exponent's times come out at a near-constant
**1.1 × tau** rather than 3 ×. What matters for the control is that both columns scale
**linearly with the slider**, which they do: 336 / 1094 / 3319 is 1.12, 1.09, 1.11 seconds
of settling per second of tau.

**The asymmetry** (`ag-speed-asymmetry`, PASS), with the two set differently — brightening
measured on `dark` → `bright` (the statistics **rise**, so `Adapt to brighter` applies) and
darkening on `bright` → `mid` (they **fall**), both single scene hops:

| direction | setting | closed form | **picture** t95 |
| --- | --- | --- | --- |
| brightening (`Adapt to brighter` 0.3 s) | fast | 900 ms | **319 ms** |
| darkening (`Adapt to darker` 3.0 s) | slow | 9000 ms | **10 495 ms** |

A factor of **33** between the two directions of the same effect, from two sliders — the
capability a single combined speed could not have. (The darkening leg reads slightly
*above* its closed form because its exponent starts pinned at `Max darken` and has to come
off the clamp before it starts moving; the brightening leg reads below it for the mirror
reason.)

Every trace carries the time constants the frame actually used, appended to each
`effects_ab_log` line as `tau=<up>/<down>`, and the check **asserts** that they match the
setting it claims to have measured — so a slider that silently did not take shows up as a
wrong tau rather than as a mysterious settling time.

##### Stability at the fast end

The [2026-09-07 pulse fix](shader-effects-adaptive-brightness-local.md#the-pulse-rank-cuts-on-a-bimodal-histogram-2026-09-07)'s bar,
rerun at **0.1 s — the fastest the slider goes**, which is where an oscillation would
appear first because that is where the EMA hides the least:

| `texdark`, Local 0 % | raw p98 p2p | exponent p2p | output pixel p2p | smoothed p98 p2p |
| --- | --- | --- | --- | --- |
| still frame, tau 0.1 s | **0** | **0.000000** | **0** | 0.000 codes |
| panning (`--periodic`), tau 0.1 s | 11.3 codes | **0.00051** | 76 (the probe moves with the texture) | 3.41 codes |
| panning, tau 1.0 s (for comparison) | 11.3 codes | 0.00007 | 147 | 0.57 codes |

A still frame is **exactly constant at the fastest setting** — peak-to-peak zero on the
raw measurement, on the exponent and on the output pixel — and under a pan the exponent
moves by five ten-thousandths, two orders of magnitude below the check's own bound and
three below what the pulse looked like. **No pulse at any speed.**

The one number that does grow is the **smoothed p98's** spread, 0.57 → 3.41 codes, and
that is arithmetic rather than a regression: for `x' = (1 - a) x + a m` with white
measurement noise the settled output's standard deviation is `sigma * sqrt(a / (2 - a))`,
so a smoothed statistic's spread is a property of **the smoothing**, and asking for a
tenth of the smoothing gets about 3.2 × the noise through (measured 6 × on peak-to-peak,
which is the noisier statistic of the two). Holding a 0.1 s trace to a 1 s bound would
therefore have failed the EMA for doing exactly what the slider asked. So
`effects_regression_sample.py`'s `agpan` check **scales that one bound by the trace's own
tau** (`ema_noise_gain()`), and leaves the exponent's and the raw estimator's bounds
absolute — the picture must not move at any speed, and the raw measurement has no EMA in
it. At the default 1 s the scaled bound is exactly the 3 codes it always was, so nothing
about the existing checks changed.

**Adaptive Brightness is unaffected**, measured rather than assumed: its `transition-gain`,
`temporal-band2`, `stability-pan` and three `stability-static-local*` checks all pass with
the same numbers as before this change, and its own two sliders still drive it — the host
only ever hands the shader Adaptive Gamma's pair on a frame where Adaptive Gamma is the
effect running.

#### The Inspector's before/after preview

The row declares the **same** `PreviewKind::AdaptiveBrightness` strip, deliberately: the
two effects are mutually exclusive, so only one of them can ever be the thing being
previewed, and `EffectPreview.cpp` picks whichever is on. That made extending it cheap —
`EffectPreviewMath.h`'s `Params` gained `bGamma`, `flMaxLift` and `flMaxDarken` and
`ApplyPixel()` gained one branch that calls `ag_gamma` / `ag_curve`; nothing in
`Shell.cpp`, `Registry.h` or the capture pass changed, and the 256-entry LUT fast path
applies unchanged (Adaptive Gamma's exponent is a frame constant whenever Local adaptation
is off, so `IsUniform()` is true and a re-grade is ~45 µs rather than ~2 ms). The
placeholder sentence for "switched off" now names both effects.

#### Measured (desktop, headless, `scripts/effects-regression.sh`, 2026-09-08)

Same recipe as everything above (private headless sway, nested `gamescope --backend
wayland`, `gamescopectl screenshot "<path> 4"`, `tests/effects_scene_client.c` as the
game). Adaptive Gamma alone, every other effect off, defaults unless stated. Captures:
the untracked `adaptive-gamma-2026-09-08` capture run.

| dark scene / region (input) | Off | Whole image | Dynamic | **Adaptive Gamma** |
| --- | --- | --- | --- | --- |
| bands 5 / 8 / 12 / 16 / 20 | 5/8/12/16/20 | 20/32/48/64/80 | 88/107/127/143/157 | **95 / 107 / 119 / 128 / 135** |
| 240 highlights | 240 | **255 — clipped** | 254 | **251** |
| pure black | 0 | 0 | 0 | **0** |

The dark scene is where the difference in *shape* is visible rather than argued: Adaptive
Gamma's five bands span 40 codes where Dynamic's span 69 — the exponent flattens the very
bottom of the range as it lifts it, because a toe that steep has less slope left over —
while the highlights come out two counts lower than Dynamic's and nine below white. Whole
image is the blown-out case both exist to avoid.

| setting (dark scene) | bands | 240 highlights |
| --- | --- | --- |
| defaults (target 0.5, lift 4.0, darken 1.5, strength 1.0, local 0) | 95 / 107 / 119 / 128 / 135 | 251 |
| Target 0.3 | 53 / 64 / 75 / 84 / 92 | 249 |
| Target 0.9 | 95 / 107 / 119 / 128 / 135 (**identical** — `Max lift` binds) | 251 |
| Strength 0.5 | 50 / 58 / 65 / 72 / 77 | 246 |
| Max lift 1.5 | 19 / 25 / 33 / 40 / 47 | 245 |
| Max darken 4.0 | 95 / 107 / 119 / 128 / 135 (identical — slack on a dark scene) | 251 |
| Local adaptation 1.0 | 95 / 107 / 119 / 128 / 135 (identical — this frame is evenly lit) | 251 |

| sweep | frame mean |
| --- | --- |
| Max lift 1.5 → 2 → 3 → 4 (dark chart, where it binds at every setting) | 34.7 → 55.7 → 90.9 → **116.9** |
| Target 0.2 → 0.35 → 0.5 (textured dark, continuous histogram) | 55.1 → 91.3 → **128.0** |
| Strength 0 → 0.5 → 1.0 (textured dark) | 25.7 → 77.0 → **128.4** |
| Max darken 1.0 → 1.5 → 2 → 3 (bright scene, where Target is pinned) | 221.2 → 209.3 → 199.1 → **181.5** |

| bright / mid scene | Off | **Adaptive Gamma** |
| --- | --- | --- |
| bright: bands 200 / 215 / 230 / 245 / 255 | 200/215/230/245/255 | **177 / 197 / 218 / 240 / 255** |
| bright: 30 shadows | 30 | **10** (and **0** at Max darken 3.0) |
| mid: 26 / 77 / 128 / 179 / 230 | identical | **25 / 76 / 127 / 178 / 230** (near-identity, worst 1) |

**No clipping, proved rather than asserted.** `effects-regression.sh`'s `noclip-*` checks
sample seven settings on the dark scene and two on the bright one and require, every time:
each band at least 2 counts above the one below it (nothing compressed into its
neighbour), the 240 highlights below 254 *and* above every band, pure black ≤ 2, and — on
the bright scene, whose top band already **is** 255 — the 245 band strictly below it. All
PASS. Note the one place the property genuinely does not extend: the **shadow** side has no
cap, so a high `Max darken` can take a deep shadow to 0 (the bright scene's 30-rectangles
at `max_darken` 3.0). Ordering survives; detail does not. That is a documented trade of
having no `min_gain`, not a clip, and the gate asserts the ordering rather than pretending
the darkening is free.

**Stability, to the standard the [2026-09-07 pulse fix](shader-effects-adaptive-brightness-local.md#the-pulse-rank-cuts-on-a-bimodal-histogram-2026-09-07)
set.** `ag-stability-static` / `ag-stability-pan`, 300 frames each off the same
`effects_ab_log` readback, watching the **exponent** (this effect's entire state) where the
Adaptive Brightness checks watch the gain:

| | raw p98 p2p | smoothed p98 p2p | exponent p2p | output pixel p2p |
| --- | --- | --- | --- | --- |
| still frame, Local 0 % | **0** | 0.015 codes | 0.00006 | **0** |
| still frame, Local 100 % | **0** | 0.000 codes | 0.000000 | **0** |
| panning (`--periodic`), Local 0 % | 11.3 codes | 0.59 codes | 0.00009 | 147 (the probe pixel moves with the texture) |

A still frame is exactly constant — peak-to-peak zero on the raw measurement and on the
output pixel — and the panning case's exponent moves by nine hundred-thousandths, three
orders of magnitude below what the pulse looked like. Nothing here is new machinery: the
estimator and the EMA are shared with Adaptive Brightness, which is why a regression in
either fails both effects' checks at once.

**The settings audit.** `scripts/settings-audit.sh` covers all six new rows in all
three routing situations (400 settings, 300 passed, 0 failed, 100 not covered). It took
one change to the audit itself, recorded here rather than buried: the first run reported
three FAILs, one per situation, all of them the same thing — setting
`image.shaders.adaptive_gamma` on also writes `reshade.adaptive_brightness.enabled`,
which the audit's "nothing else moved" rule correctly flags as a collateral write. That
write **is** the exclusion working, so it was declared in `scripts/settings_audit.py`'s
existing `SIBLING_KEYS` table ("a row whose own write legitimately moves a second key" —
the mechanism the Resolution rows' locked aspect ratio already uses), in both directions.
`Why declare rather than redesign:` the alternative that avoids the cross-write is greying
the switch, which is the shape rejected above; and an audit that reports the same
intentional write as a bug on every future run trains people to ignore it.

**Row count and the parameter budget.** The Effects band went from 5 switch rows to
**6**. Adaptive Gamma owns **7** params — Strength, Target brightness, Max lift, Max
darken, **Adapt to brighter**, **Adapt to darker** (both added 2026-09-09) and Local
adaptation — against `kParamBudget`'s 8. It was **5** when the effect shipped on
2026-09-08; the two speeds took it to seven, one under the ceiling, and **the budget was
not raised** for either change. It still has fewer knobs than Adaptive Brightness because
it has fewer mechanisms (no gain to bound, no shadow cap, no mode), and the two rows now
differ by exactly the one param Adaptive Brightness has and this one cannot: Mode. The Effects band's row count
went from 6 to **7** the same day, when [Bloom](shader-effects-bloom.md#bloom-imageshadersbloom--new-2026-09-08)
landed after it.

### Leave dark scenes alone (2026-09-14) — one per effect

`Split into two fields the same day:` shipped as ONE field shared by both effects (below,
kept as the historical record of why the feature exists and how the shared version was
built and measured), then split into `adaptive_brightness.dark_floor` and
`adaptive_gamma.dark_floor` a few hours later at the user's own follow-up request, quoted
verbatim: *"Make the 'Leave dark scenes alone' part individual settings for both Adaptive
Gamma and Adaptive Brightness."* Nothing about the FORMULA or the SHADER changed — see
`src/shaders/effects_curve.h`'s `dark_weight()`, still the one function both effects call,
and the shader's `u_darkFloor` uniform, still a single number per frame — only where the
number is STORED and how it reaches the panel did. `rendervulkan.cpp`'s
`EffectsPushData_t` is the one place that now resolves "whichever effect is running" into
that shared uniform, reading `NativeEffectsState_t::flAbDarkFloor` or `::flAgDarkFloor`
depending on `s.bAdaptiveBrightness` / `bAdaptiveGamma`. `ConfigSchema.h`'s
`kCurrentSchemaVersion` went 4 → 5 for the split; `ConfigManager.cpp`'s `Migrate_4_to_5()`
copies an old schema-4 file's single shared value into BOTH new keys, so a profile tuned
before the split keeps exactly that strength under whichever effect the user re-enables,
rather than silently resetting to the compiled-in default. See "The panel rows" below for
where each of the two ended up and why they differ.

`Why:` the user, verbatim: *"the adaptive brightness and the adaptive gamma both completely
destroy REALLY dark images. ... Is there some kind of filter, that keeps really dark stuff
really dark or something?"* — with a real capture attached: a near-black CS2 zombie-mode
corridor, a hanging lamp and a HUD. With either effect on, the frame did not merely wash out
— it **binarised**: pure black held exactly where it was, and everything even slightly above
it (the pipes, the floor grating, the character silhouettes) was slammed toward white, like a
threshold rather than a grade.

That is not a rounding error, and it is not a missing clamp. Measured off the raw capture
with PIL, on the same rank-window-mean statistic `cs_effects_measure.comp` computes (Rec.601
luma on the encoded picture, mean of the taps ranked 25–75 % for the median):

| statistic | value (0–1) | encoded (0–255) |
| --- | --- | --- |
| mean | 0.0552 | 14.1 |
| p2 | 0.0000 | 0.0 |
| **p50 (median)** | **0.0003** | **0.1** |
| p98 | 0.4336 | 110.6 |

With the median a fraction of a code above zero, **both** operators were already doing
exactly what their own user-facing bounds say to do. Adaptive Gamma's exponent is
`ln(target)/ln(p50)`, which for `p50 -> 0` is driven straight to its floor, `1 / max_lift`
(0.25 at the shipped `max_lift` 4.0), regardless of how dark any individual pixel is —
`ag_gamma(0.0003, 0.5, 4.0, 1.5)` clamps to 0.25, and `0.02^0.25 = 0.376` (code 96) for a
1-code input. Adaptive Brightness Dynamic's gain is pinned to `max_gain` by the same
`p50 -> 0` limit (`ab_dyn_binding()` returns `AB_BIND_LIFT`) and its gamma to the same
floor, and its `top = gain^gamma` then exceeds 1.0, so the shoulder engages and compresses
everything above its knee toward white as well. Both floors were already **honoured
correctly** — `ag_gamma_min()` / `ab_gamma_min()` and their callers' own `clamp()`s hold even
as `p50 -> 0` (`tests/test_effects_curve.cpp`'s "the pre-existing exponent/gain bounds
already hold as the median -> 0" sweeps every `(min_gain, max_gain)` / `(max_lift,
max_darken)` pair at `p50 = p2 = p98 = 0` and asserts neither exponent nor gain ever leaves
the user's own range) — so the fix could not be a bound fix. It needed a **third** control
that fades the whole operator toward the untouched picture on a scene this dark, rather than
a fourth clamp on an already-correctly-clamped one.

**The formula.** Let `m` be the same smoothed statistic each operator already keys its curve
on — the median, `HISTORY_P50` — and `dark_floor` the new user-facing parameter (encoded
units, same scale as Target brightness):

```
w = smoothstep(dark_floor / 2, dark_floor, m)      // 0 below half, 1 at/above the full value
dark_floor <= 0  =>  w = 1                          // the floor is off
```

and each operator blends its own parameter(s) toward the identity by `w`, rather than being
clamped:

```
Adaptive Gamma:              gamma_eff = mix(1, gamma, w)     // x ^ gamma_eff
Adaptive Brightness Whole image:  gain_eff  = mix(1, gain,  w)     // c * gain_eff
Adaptive Brightness Dynamic:      gain_eff  = mix(1, gain,  w), gamma_eff = mix(1, gamma, w)
```

Dynamic blends **both** of its parameters, not only the gain: `ab_dyn_curve(x, 1, 1)` is
`y = x` exactly (its own top, `1^1`, never exceeds 1.0, so the shoulder never engages), which
is what makes `w = 0` a bit-exact identity for the whole curve rather than a partial one.
`src/shaders/effects_curve.h`'s `dark_weight()` is the one function both effects call
(`cs_effects_layer0.comp`); it and its whole derivation — why a `smoothstep`, why the span is
centred on `dark_floor` rather than running `0..dark_floor`, why the weight is computed from
the **raw**, un-shifted median rather than the one Local adaptation shifts per pixel, and why
Adaptive Brightness Whole image is keyed on the median too even though its own gain reads the
mean — are documented in that header's own DARK FLOOR block; this page states the outcome,
that header carries the proof.

**Why p50 and not the mean, even for Whole image's own gain:** the capture above is the
concrete counter-example. Its median is 0.0003 (a literal-black majority) but its **mean is
0.055** — 180× larger — because a lamp, a HUD and chat text are small in area but pull an
*average* far more than they move a *median*. A floor keyed on the mean would barely engage
on the exact frame this feature exists for. `p50` is computed unconditionally every frame
regardless of mode (`cs_effects_measure.comp`'s own header), so reading it for Whole image's
floor too costs nothing extra and is a strictly better "is this scene dark" discriminator
than the statistic that mode's own gain happens to use.

**The default, and why it is small.** `0.03` (encoded — the ramp runs code 4 to 8), measured
against the two existing reference scenes that already have their own regression contracts:

| scene | p50 (encoded) | at `dark_floor` 0 (today) | 0.03 (shipped) | 0.05 | 0.10 | 0.15 |
| --- | --- | --- | --- | --- | --- | --- |
| `dark` (bands 5/8/12/16/20 + 240 highlights) | 12.1 | full effect | **full effect** | full | ~62 % | 0 |
| `texdark` (textured dark-game stand-in) | 20.0 | full effect | **full effect** | full | full | ~56 % |
| `mid` (0.1..0.9 bands) | 128.0 | untouched | **untouched** | untouched | untouched | untouched |
| report's own capture | 0.1 | destroyed | **neutralised** | neutralised | neutralised | neutralised |
| `blackout` (new, see below) | 1.5 | destroyed | **neutralised** | neutralised | neutralised | neutralised |

(the `w` weight at each setting, computed by `dark_weight()` on the measured `p50` above, is
what "full effect" / "untouched" / "neutralised" means in this table —
`tests/test_effects_curve.cpp`'s "at the shipped default, the report's near-black capture and
the `blackout` scene are fully neutralised while the existing `dark` and `texdark` reference
scenes keep full strength" pins the four numbers that matter.) `0.10` was the original
candidate — it sits inside the brief's own "probably 0.05–0.10" guess — but it would have
put `scripts/effects-regression.sh`'s **pre-existing** `dark-dynamic` contract ("band 5 must
lift to ≥ 30") at only 62 % strength, silently softening an existing regression check as a
side effect of a new feature it was never told about. `0.03` leaves both existing scenes at
their full, unmodified behaviour and still fully neutralises anything anywhere near the
report's own capture — deliberately narrow, because the report was about scenes far darker
than "any dark map", not about dark maps in general.

**The `blackout` reference scene** (`tests/effects_scene_client.c`, added for this feature,
its own ring — it is not in `SCENES_DEFAULT`, for the reason that comment gives: a scene
appended to the shared ring shifts every `advance N` a later block depends on). Unlike the
existing `dark` scene (five equal-sized bands, no large black area — its own median, 12.1,
turned out to be an order of magnitude brighter than the report's capture), `blackout`
reproduces the report's actual *shape*: ~90 % of pixels hashed to code 0–6 (a near-literal-
black majority, matching a genuinely dark corridor), ~9 % a textured 10–40 band (the "slightly
above black" surfaces — pipes, grating), ~1 % lights 200–255. Measured off the real GPU
capture (`scripts/effects-regression.sh`, `darkfloor-2026-09-14`): median 1.5 (0.0058
encoded), mean 5.8 — safely inside the fully-neutralised regime at the shipped default, and
close to the report's own capture's own 0.1/14.1. `scripts/effects_regression_sample.py`'s
`darkfloor` subcommand asserts the two properties the report actually named on this scene, at
the shipped default: graded stays within 3 counts of raw on a coarse sampling grid, and
nothing that started under code 64 comes out at 128 or above — the "binarised toward white"
failure, as a number rather than an impression. `scripts/effects-regression.sh`'s
`dark-floor-blackout-ag-default` / `-ab-default` checks run it for both effects and both
passed at **0.0 counts** worst deviation (the floor is an EXACT identity here, not merely
close); `dark-floor-means` reports the frame mean at `dark_floor` 0 beside the shipped
default as INFO — **5.8 unfloored-off / 57.4 Adaptive Gamma floor-0 / 5.8 Adaptive Gamma
floor-default / 79.8 Adaptive Brightness floor-0 / 5.8 Adaptive Brightness floor-default** —
so the fix is legible as a measured before/after (57.4 and 79.8 are the reported failure,
reproduced on demand) rather than only as a pass. Full run: 87 checks, 0 failed, 578 s
wall time.

**A scene-motion trap this check fell into once, worth recording:** `blackout` is a TEXTURED
scene (`PaintTexture()`, like `texdark`), so — unlike the flat `dark` scene — it scrolls
under this harness's `--motion 3` unless paused. The first version of this check did not
pause it, and the "off" and "on" captures were therefore two different frames of a moving
picture graded two different ways, not one frame graded two ways: one coarse-grid cell read
raw 5 / graded 203, which looked exactly like a binarisation bug and was not one. `Why this
block runs LAST, its own restarted instance:` Bloom's block
(immediately after Adaptive Gamma's) continues the PREVIOUS instance rather than restarting
— it needs the client already sitting on `texdark` with motion paused, exactly where Adaptive
Gamma's block leaves it — so a block placed between them that restarts with a different ring
breaks that continuity for everything from Bloom onward (measured: 15 unrelated checks failed
the first time this landed there). Last is the only position that restarts a ring nothing
downstream depends on.

**The panel rows (post-split).** Both copies are a normal `Param` under their own effect's
Switch, and read identically — same label ("Leave dark scenes alone"), help text, range
(0.0–0.5, step 0.01, `ZeroMeans("Off")`) and default (0.03) as the shared control had
before the split.

- **Adaptive Gamma's copy** (`image.shaders.adaptive_gamma.dark_floor`) has been one since
  the split: that row was at 7 params, so the eighth (this one) fit inside
  `kParamBudget`'s then-ceiling of 8 with zero left over.
- **Adaptive Brightness's copy** (`image.shaders.adaptive_brightness.dark_floor`) became
  one on **2026-09-16**. From 2026-09-14 to then it was a standalone `Slider` entry
  (`image.shaders.adaptive_brightness_dark_floor`) sitting directly under the Adaptive
  Brightness Switch and labelled "Leave dark scenes alone (Adaptive Brightness)" — **not**
  because it differed from Adaptive Gamma's in any way, but purely because that row was
  already AT the ceiling (8 of 8) and a ninth `Param` was unrepresentable
  (`Registry.cpp`'s `AddParam()` refuses it, the SixBudget law). *Why it moved:* Adaptive
  Brightness V2 raised `kParamBudget` 8 → 10 on 2026-09-14/15 for its own two darkening
  params, which freed the slot this control had been denied — so the constraint that
  forced the standalone row no longer existed, while the cost of it did: on screen a
  dimmed, full-width slider row under the switch read as a peer *effect* rather than as
  one of Adaptive Brightness's own tunables. It is now this row's ninth `Param` of ten,
  and drops the "(Adaptive Brightness)" disambiguator, which is redundant inside the row.
  It keeps its `DisabledUnless` on Adaptive Brightness specifically (not "either effect",
  the way the pre-split shared row did) — it is the one param of the nine that does
  nothing at all while the effect is off.

Neither the config key (`reshade.adaptive_brightness.dark_floor`), the schema version, nor
the shader changed in that move: it was a UI placement fix only.

Both are config-schema fields on their own effect's struct now (`ReshadeAdaptiveBrightnessSettings::dark_floor` / `ReshadeAdaptiveGammaSettings::dark_floor`), not a bare
top-level key on `ReshadeSettings` — see the split note above this section.

## History

- **Adaptive Gamma** (glossary detail): **Adaptive Gamma** — the Shaders effect added 2026-09-08 (`image.shaders.adaptive_gamma`, `src/shaders/effects_curve.h`'s `ag_*` block), from the user's *"Make something similar, but make it gamma based"*. It reuses **Adaptive Brightness**'s measured statistics but its whole operator is one exponent — `x^g` with `g` chosen to land the smoothed median on Target — so it adjusts *contrast* and cannot clip, where Adaptive Brightness's levels gain adjusts *exposure* and needs a shoulder to stop it blowing out. Its bounds are the user-facing **Max lift** and **Max darken** params, not constants. Since 2026-09-09 it also has its **own** adaptation speeds (**Adapt to brighter** / **Adapt to darker**): before that the shared measure pass smoothed its statistics at Adaptive Brightness's speeds, i.e. at sliders unreachable while this effect was the one running. Since 2026-09-14 it also shares **Leave dark scenes alone** with Adaptive Brightness, but the two keep independent copies, not one shared row (schema 4→5, commit 8660451): a briefly-shared `reshade.dark_floor` key (schema 4) split into `reshade.adaptive_gamma.dark_floor` — this row's own eighth `Param` — and `reshade.adaptive_brightness.dark_floor`, which was at first a standalone row placed under Adaptive Brightness's switch (GUI id `image.shaders.adaptive_brightness_dark_floor`) rather than a ninth `Param`, because that row was already at the then-8-`Param` budget with none spare — **folded into that row as its ninth `Param` on 2026-09-16** (GUI id `image.shaders.adaptive_brightness.dark_floor`, label shortened to plain "Leave dark scenes alone") once **Adaptive Brightness V2** had raised `kParamBudget` 8 → 10 for its own darkening params and the constraint was gone, since a standalone dimmed slider under the switch read as a peer *effect* rather than as one of that effect's tunables; the config key and schema 5 were untouched by the move; `ConfigManager.cpp`'s `Migrate_4_to_5()` copied the old key's value into both. Each fades its own effect's exponent/gain back to the untouched picture once the smoothed scene is darker than its own floor — without it, a near-black scene drove either effect's exponent/gain to its user-set ceiling regardless of the pixel, which the user reported as the picture being "destroyed"/binarised rather than merely lifted.
