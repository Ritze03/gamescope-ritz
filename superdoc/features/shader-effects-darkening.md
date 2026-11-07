# Shader effects: Adaptive Brightness V2 Darkening

The two-sided Darkening curve of [Adaptive Brightness V2](shader-effects-adaptive-brightness-v2.md) and its 2026-09-15 redesign. Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

### Darkening (2026-09-14; REDESIGNED 2026-09-15 — a true S-curve fixed at Target)

**Why (2026-09-14).** Every guarantee above this heading was built for one direction only:
`f(0) = 0`, `f(1) = 1`, concave, so `f(x) >= x` everywhere — a lift, never a darken. The
user's own request, verbatim: *"Make it able to make the image darker (both full and on
parts of the image)"*. This section is that feature: it answers the plan's own open
question about knee/toe symmetric behaviour (§8.2 Q1 — see below for the answer this pass
gave).

**Why REDESIGNED (2026-09-15 QC, verified numerically).** The first cut composed the
darken half on the lift curve's own CONTINUATION above Target — the pivot's height was
`L(Target)`, not `Target` itself. The QC finding, in one sentence: *`F(x >= Target) >= x`
for every shipped setting, so "darkening" only ever undid the lift curve's own highlight
raise (skyfore's sky band read 225 raw, 233.7 lift-only, 225.0 with the first cut's own
Darken 0.5 / Max darken 2 — back to raw, never below it), and `L(Target)` (code 122
against Target's own code 89 at the shipped defaults) was a floor nothing above Target
could ever be darkened past, whatever `D` said.* This section rewrites the pivot so it
sits EXACTLY on Target on both sides — see **THE FIXED POINT** below — closing that gap by
construction rather than documenting it as a deliberate trade. Nothing about `f_dark`
itself (the closed form immediately below, its `1/D` secant proof) changed; only where it
is evaluated did.

**The closed form, and why it is exact, not a mirror-and-hope.** `abv2_toe(x; g, t)` is
`x * ((1+t)/(x+t))^(1-g)`. Differentiating the GENERAL form (any real `g`, `t > 0`, not
just `0 < g < 1`) gives `f''(x)` the SAME SIGN as `(g - 1)` at every `x` in `[0, 1]` — the
family is concave (the toe, lift, `g < 1`) or convex (`g > 1`) by one sign flip, nothing
else. Writing the convex branch with a positive exponent to avoid a negative `pow`
(`gg - 1` instead of `1 - gg`) gives a NEW, independent function:

```
f_dark(x; g, t) = x * ((x + t) / (1 + t)) ^ (g - 1),   g > 1
f_dark'(0) == 1/D   <=>   t = 1 / (D ^ (1/(g-1)) - 1)
```

— literally `abv2_solve_t()`'s own derivation, target `1/D` in place of `S`. Two facts
follow from general convexity, not from anything specific to this formula: `f_dark(0) = 0`,
`f_dark(1) = 1` by direct evaluation, so Jensen's inequality on those two endpoints gives
`f_dark(x) <= x` for every `x` (convexity is *literally* "lies on or under the chord"); and
for any convex function through the origin the secant `f(x)/x` is NON-DECREASING (the
tangent-line inequality at `y = 0` gives `f'(x)*x >= f(x)`), so its minimum over `(0, 1]`
is its own limit at `x -> 0`, which is `1/D` by construction. Hence `f_dark(x)/x >= 1/D`
everywhere, tight at black — the EXACT mirror of the toe's own `S` proof, not an
approximation: same family, same closed form, one sign flip. `abv2_toe_dark()` in
`src/shaders/effects_curve.h` is this function, guarded the same way `abv2_toe()` is
(`g <= 1` or `D <= 1` each alone return the identity directly).

**Choosing the darken exponent — the mirror of `abv2_g()`, its scene aim REDERIVED
2026-09-15:**

```
g_static_dark = 1 + 0.6 * Darken                          // Darken 0..1 -> 1.0..1.6
g             = clamp(max(g_static_dark, g_adapt_z), 1, G_MAX)   // Scene
g             = clamp(g_static_dark, 1, G_MAX)                   // Off
G_MAX         = 1 / G_MIN   // 5.0 -- reciprocal of the lift floor
```

`max`, not `min`: adaptation can only make the darken STRONGER on a bright scene (a
LARGER `g`); on a dark scene the static floor is what darkens the one bright thing in it,
so the floor must never be relaxed by "the scene looks dark" — the exact mirror of
`abv2_g()`'s own `min()` argument.

**`g_adapt_z`, not the first cut's `g_adapt = ln(Target)/ln(anchor)`.** That formula
solves "raw anchor to the `g`-th power lands on Target" — correct reasoning for a curve
whose domain genuinely is `[0, 1]`, wrong once the darken half's own domain is the
RESCALED `w = (1 - anchor) / (1 - Target)` (the fixed-point combiner below runs `f_dark`
on `w`, not on raw `anchor`). Worked example, Target 0.35, anchor 0.7 (a scene reading
twice Target): the old formula gives `g = ln(0.35)/ln(0.7) = 2.943` — the exponent for a
curve that maps 0.7 straight to 0.35, which is not what `f_dark`'s own `w`-domain does
with it at all. The new derivation solves the equation the curve actually evaluates:

```
w         = (1 - anchor) / (1 - Target)
g_adapt_z = ln(w) / ln(1 - Target)
```

chosen so the boundary limits are the ones adaptation actually needs: `w -> 1` (anchor at
Target, the pivot) gives `g_adapt_z -> 0`, so `max(g_static_dark, g_adapt_z)` always falls
back to the static floor exactly at the pivot — "no adaptive darken needed, the scene IS
at Target" — and `w -> 0` (anchor at white) gives `g_adapt_z -> +infinity`, saturating at
`G_MAX` — "as much darken as this shape allows" at the brightest a scene can read. (Same
worked example: `w = 0.4615`, `g_adapt_z = ln(0.4615)/ln(0.65) = 1.795` — already a
meaningfully deeper exponent than the static-only floor at Darken 0, climbing toward
`G_MAX` as anchor approaches 1, unlike the old formula's `2.943`, which was neither
derived from nor bounded by this curve's own domain.) The lift side's own `g_adapt` gets
the identical treatment — `z = anchor/Target`, `g_adapt_z = ln(Target)/ln(z)` — needed for
exactly the same reason once its own half is rescaled below (see THE FIXED POINT). Both
closed forms, their boundary-limit proofs and worked numbers live in
`src/shaders/effects_curve.h`'s own comments next to `abv2_g_adapt_dark_z()` /
`abv2_g_adapt_lift_z()`.

**The compute shader** (`cs_effects_layer0.comp`) calls a NEW function,
`abv2_g_lift_scurve()`, for the lift side's exponent — it delegates byte-for-byte to the
unchanged `abv2_g()` whenever Max darken is at its floor, and only switches to the
rescaled `g_adapt_z` once darkening is genuinely active. **FIXED (V2 darken QC,
2026-09-15, same day):** the settings overlay's own split-screen Preview
(`Overlay/EffectPreviewMath.h`) now calls `abv2_g_lift_scurve()` too — it had briefly
called the plain `abv2_g()` on this half even with darkening on, a documented, narrow gap
between the Preview's picture and the GPU's noted the same day this section was written;
closed before it shipped in a release. It still does not model Stage 2's guided filter
(the same base-only approximation as the lift-only picture), which is unrelated to this
gap and remains open.

**Which direction is "attack" now (asymmetry, plan 4.8).** The anchor's own EMA
(`cs_effects_measure.comp`) is completely UNCHANGED: **Adapt speed** (`tau_up`) is still
used while the anchor RISES, its 2× (`tau_down`) while it FALLS. A rising anchor is
"brighten": it takes the LIFT off (`min(g_static, g_adapt)` relaxes toward 1) and puts the
DARKEN on (`max(g_static_dark, g_adapt)` deepens away from 1) in the SAME direction, at
the SAME `tau_up` — so "attack" is "the anchor is rising", whichever of the two halves is
the one actually engaging, and "decay" (`tau_down`, 2× slower) is the anchor falling back.
There is no second, independent speed for the darken half to need.

**The EMA's own RELEASE, under the redesign.** A scene that walks from full daylight
(engaging Darken) back into shade releases at `tau_down` — the SAME 2×-slower constant
the lift side's own release already uses, since it is the identical anchor and the
identical EMA driving both `g_adapt_z` terms (only the CONSUMER — `min()` for lift,
`max()` for darken — differs). Nothing about the redesign changes which constant governs
release: `abv2_g_adapt_dark_z()` and `abv2_g_adapt_lift_z()` are both PURE functions of the
(already-smoothed) `anchor`, called fresh every frame — the redesign only changed how
each turns `anchor` into an exponent, not how `anchor` itself is smoothed or which
direction is attack versus decay. One practical consequence worth stating plainly: while
Darken is releasing (anchor falling back toward Target), the LIFT half's own `g_adapt_z`
is simultaneously moving toward its OWN engaged state (a falling anchor is "darken",
i.e. Lift's own attack direction) — so a scene cutting from bright back to dark can have
one half releasing while the other attacks, both riding the SAME single anchor value,
which is exactly the "one frame, two halves" design point (THE FIXED POINT, above) rather
than a new interaction this redesign introduced.

**THE FIXED POINT (2026-09-15 redesign) — both halves rescaled against Target itself.**
The per-pixel base `B` (the guided filter's local mean) already varies spatially; making
its curve two-sided is what lets a bright region's base darken while a dark region's
lifts, in the SAME frame, with `Detail` preserving texture in both:

```
x <= Target:  F(x) = Target * L(x / Target)
x >  Target:  F(x) = Target + (1 - Target) *
                     K( (x - Target) / (1 - Target) ; gDark, D )
```

where `L(x) = abv2_curve(x, gLift, S, bKnee)` (the existing toe/knee, byte-for-byte
untouched) and `K = abv2_toe_dark` (also untouched). `L(1) = 1` by construction, so
`F(Target) = Target * 1 = Target` EXACTLY — the fixed point the plan's own §4.3 prose
asked for (`f(Target) = Target` literally), closed by construction this time, not merely
approached. `K(0) = 0` for any `g, D`, so the `x > Target` branch agrees with it too.

**Why rescaling the LIFT half is now unavoidable.** `L` is a genuine lift curve (concave,
`f(x) >= x`), so `L(Target)` is generally ABOVE `Target` itself — the first cut's own
gap. Landing exactly ON `Target` at `x = Target` requires the `x <= Target` branch to hit
`Target` there too, which the un-rescaled `L(x)` cannot do for any real Lift setting: a
fixed point at Target and byte-identical un-rescaled shadows are mutually exclusive once
darkening is genuinely active. This redesign picks the fixed point — the explicit ask —
and pays for it with an OFF SWITCH (below) instead of the first cut's own pivot gap.

**THE BOUND, both halves, exactly — the new guarantee.**

- `x <= Target`: `F(x)/x = Target * L(z) / x = L(z)/z` (`z = x/Target`) `<= S` — `L`'s own
  secant bound, inherited unchanged through the rescale (`L(z) <= S*z` for `z` in `[0,1]`
  gives `Target*L(z) <= Target*S*z = S*x`).
- `x >  Target`: `F(x) <= x` — `K(z) <= z` (`K` convex, matching endpoints — the SAME
  Jensen argument as `f_dark` above, now on `z = (x-Target)/(1-Target)`), so
  `F(x) = Target + span*K(z) <= Target + span*z = x`. And `F(x)/x >= 1/D` by the same
  non-decreasing-secant argument, tight in the limit at `x -> Target+`. **Nothing above
  Target can exceed its own raw value any more** — the first cut's own "never below raw"
  gap, closed.

**THE OFF SWITCH — the backward-compatibility rule, exactly.** `D <= 1` (Max darken at
its own floor) returns `abv2_curve(x, gLift, S, bKnee)` DIRECTLY — the plain, un-rescaled
single-sided curve, not a limit of the two-sided one. Gating on `D` ALONE, not also on
Darken, is deliberate and STRICTLY STRONGER than the stated "Max darken 1 AND Darken 0":
`abv2_toe_dark()` already returns the identity whenever `D <= 1` regardless of `gDark`, so
the `x > Target` branch already telescopes to `F(x) = x` there for any Darken value — but
the `x <= Target` branch would still be the RESCALED `Target*L(x/Target)`, which is NOT
byte-identical to the un-rescaled `L(x)` whenever Lift > 0. Gating the WHOLE dispatch on
`D` is what keeps the shadows byte-identical too, and subsumes the stated requirement
rather than merely meeting it — verified in the unit tests as an exact float comparison
against `abv2_curve()`, not an approximation (`tests/test_effects_curve.cpp`'s
"byte-identical to abv2_curve at Max darken 1 / Darken 0, over a wide grid", unchanged
from the first cut, still passes against the new code unmodified).

**A visible consequence, stated plainly, and PROVEN, not just measured:** raising Max
darken above 1 changes the BELOW-Target picture too, even with Darken left at 0 — the
rescale, not the darken exponent, is what moves it, and it moves it in exactly ONE
direction: `Target · L(x/Target) <= L(x)` for EVERY `x` in `[0, Target]` and every real
Lift setting, by concavity alone (`L` concave means `L(T·(x/T) + (1-T)·0) >=
T·L(x/T) + (1-T)·L(0)`, i.e. `L(x) >= T·L(x/T)`, for any `T` in `(0,1)`). So turning the
S-curve on can only pull the shadows DOWN relative to the lift-only picture, never up —
worked numbers below: code 64, well under Target's own code 89, reads 96.05 lift-only but
70.63 once Max darken is 2 (Darken still 0.5's own default in that specific example — see
the table's own note on which knobs are non-default); measured on the user's own real
capture (below), where Scene adaptation drives a much smaller `g` than this worked
example's static 0.7 (the frame is extremely dark overall, so the curve is far steeper),
the SAME inequality pulls a mid-shadow statistic (the 90th percentile, itself still deep
in shadow) down by over 30 codes from the lift-only picture, though it stays net ABOVE
raw. This is the direct, unavoidable cost of the fixed point above, not a bug — the "Max
darken" panel row's own help text says so, and it is why "corridor unchanged" — true of
the FIRST CUT, and this redesign's own original prediction before it was actually
measured — is NOT true of a real, very dark scene under this redesign: see the capture
section below for the numbers.

**Continuity: C0 achieved at every setting, C1 only when darkening is off.** Both
branches agree exactly at `x = Target` — not merely close, EXACTLY, by the fixed-point
construction above. The slope arriving from below is `L'(1)` (the toe/knee's OWN
highlight slope, `x/Target` having reached 1 — a DIFFERENT point on `L` than the first
cut's own `L'(Target)`, since the rescale moves where on `L` the pivot's left slope is
read from); the slope leaving above is `K'(0) = 1/D` EXACTLY. The kink's RATIO is
`(1/D) / L'(1)`, not the first cut's plain `1/D` — **measured at the shipped Lift 0.5**
(`g_lift` 0.7, Scene off) **/ Max lift 4 / Darken 0.5** (`g_dark` 1.3) **/ Max darken 2 /
Target 0.35**: left slope **0.7030** (matches this file's own `f'(1) ≈ g` approximation),
right slope **0.5000** (`1/D` exactly), ratio **0.7113** — CLOSER to 1 (less
discontinuous) than the first cut's own plain-`1/D` ratio of 0.5 at the same `D`, since
`L'(1) < 1` always (the toe/knee's own highlight guard) makes `(1/D)/L'(1)` strictly
larger than `1/D` alone — checked across the test grid, not only at this example
(`tests/test_effects_curve.cpp`'s two new `[darken]` cases: the worked ratio, and `L'(1)
<= 1` swept over `g x S x Knee`). Quantised to 8 bits, the kink steps **exactly 1 code**
either side of the pivot at these defaults (code 88→89, 89→90 — see the harness/quantise
note below), well inside the "≤ 1 code" bound this redesign targets — this is the SAME
"monotone but not smooth" trade the Knee variant's own kink at `ABV2_KNEE_X` is accepted
for (guarantee 3 asks for monotone, not smooth). At `D == 1` the two slopes match trivially
(`K'(0) == 1`), because the whole dispatch has already returned the un-rescaled `L`, whose
own continuity is unaffected by any of this.

**Knee's own mirror — the escape hatch taken.** The darken half is ALWAYS toe-shaped,
whatever Shape says. A darkening knee construction (leave shadows exactly untouched,
compress only highlights above the pivot) is a genuinely different curve family from the
toe mirror above, and this pass did not build a second one: the plan's own escape hatch —
*"if that is genuinely awkward, make Knee use the toe-mirror for darkening and say so"* —
is what shipped. This ALSO answers the plan's §8.2 Q1 "toe or knee?" open question for the
darken direction specifically: toe, unconditionally, is the shape darkening uses.

**Detail's secant bound — TIGHTER than stated, now that `F(x) <= x` holds unconditionally
above Target.** `abv2_secant2()`'s CODE still clamps to `max(SLift, D)` (unchanged,
still a safe ceiling), but the bound that actually holds after this redesign is `S`
ALONE, on BOTH sides of Target, not `max(S, D)`: below Target from `L`'s own secant bound
inherited through the rescale; above Target because `F(B) <= B` itself now (this
redesign's own guarantee), so `F(B)/B <= 1 <= S` always (`S >= 1` by Max lift's own range
floor). So a darkened region's detail SHRINKS in step with its own base — `F(B)/B <= 1`
there, so the secant factor is at most `Detail` on the darkening side, proportionally
smaller the darker the region gets (down to `Detail/D` at the darkest, from the same
non-decreasing-secant argument as `F(x) >= x/D`). `max(S, D) * Detail` is simply a looser
number the same clamp expression also happens to satisfy — the clamp's own code was left
alone because `S` alone already bounds it, so changing the expression buys nothing.
Byte-identical to `abv2_secant()` at `D <= 1` (`max(SLift, 1) == SLift`) for the same
algebraic reason `abv2_curve2()` telescopes there.

**Colour, and the out-of-gamut guard.** `k = Y'/Y` is at most 1 wherever darkening is the
active side (`Y' <= Y`), so `RGB' = RGB * k` is component-wise `<= RGB`, which is already
in `[0,1]` — the out-of-gamut guard (`if (m > 1.0 ...)`) can never fire on the darkening
side; it exists only for the lift case (`k > 1`), unchanged. The black-floor/void gate
(`abv2_is_void(B)`) is keyed on `B` alone and untouched by any of the above.

**Worked numbers** (`g = 1.3` — Darken 0.5's own static floor, `D = 2.0` — Max darken 2;
codes in, codes out, `abv2_toe_dark()` in isolation):

| code | 255 | 230 | 215 | 200 | 128 | 64 | 32 | 16 | 8 | 4 | 1 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `abv2_toe_dark` | 255 | 224 | 205 | 187 | 107 | 46 | 20 | 9 | 4 | 2 | 1 |

Every code stays within a factor of `D` of its input (`223.7/230 = 0.972`, well above
`1/D = 0.5`), and the whole grid is asserted in `tests/test_effects_curve.cpp`'s
exhaustive `g x D` sampling.

And the FULL two-sided S-curve (`abv2_curve2()`, REDESIGNED numbers, 2026-09-15 — Lift
0.5 → `g_static` 0.7, Max lift 4, Target 0.35 → code 89.25, Darken 0.5 → `g_static_dark`
1.3, Max darken 2), against the lift-only curve it replaces:

| code in | 255 | 245 | 230 | 215 | 200 | 128 | 90 | 89 | 64 | 32 | 16 | 8 | 4 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| S-curve (Darken 0.5) | 255.0 | 242.4 | 224.0 | 206.1 | 188.8 | 116.5 | **89.6** | **89.1** | 70.6 | 43.3 | 26.4 | 16.0 | 9.6 | 3.2 | 0.0 |
| lift-only (Max darken 1) | 255.0 | 247.9 | 237.2 | 226.2 | 215.0 | 156.9 | 122.4 | 121.4 | 96.1 | 58.5 | 35.2 | 20.9 | 12.0 | 3.6 | 0.0 |

Every code above Target (245..90) now ends AT OR BELOW its own raw code (242.4 ≤ 245,
224.0 ≤ 230, ... 89.6 ≤ 90) — the new guarantee the first cut's own table could not show
(its own 245 → 246 was ABOVE raw). Bold-in-the-old-table's claim that codes below Target
are bit-identical between the two rows **no longer holds once Max darken is above its
floor** — every code here, including 64/32/16/8/4/1, moved from the lift-only row, because
the LIFT half is now rescaled too (see "A visible consequence", above): code 64 reads
96.1 lift-only but 70.6 in the S-curve row, entirely from the rescale, not from `gDark` at
all. The two rows ARE still bit-identical whenever Max darken is at its floor (1.0) —
that byte-identical case is the one asserted exhaustively in the unit tests, not this
table, which is deliberately showing the darkening-ON numbers throughout. Codes 90 and 89
(straddling Target's own 89.25) land within 0.5 code of Target and of each other — the
pivot's own kink, visibly small at this resolution.

**The row-budget: eight to ten, not a fold (2026-09-14).** Two new params — **Max darken**
and **Darken** — take Adaptive Brightness V2 from eight `Param`s to ten, at
`Registry.cpp`'s `kParamBudget`, which the codebase's own history (see "The second raise"
above) explicitly warns against raising a third time FOR THE SAME ROW's growth. This is a
different row hitting the SAME shared ceiling for a genuinely new capability, not
Adaptive Brightness asking again — and it was checked, not assumed, before raising:
`test_overlay_shell.cpp`'s "an entry at the six-param budget overflows the drawer at 2.0x"
reads `ui::ParamBudget()` LIVE rather than a hardcoded number, so it already proves the
Inspector's Configure body SCROLLS past whatever the budget is, and keeps proving exactly
that at 10 with no test edit needed — a taller row costs a scrollbar, not a layout break,
at every scale that test already pins. The alternative — folding an existing param to
free a slot — was weighed the way Adaptive Brightness's own 2026-09-06 history weighs it,
and rejected the same way: **Adapt speed** could fold into the **Adaptation** Choice (Off
/ Slow / Normal / Fast) as the task itself offered as a fallback, but that trades a
continuous 0.1..5 s dial most players never touch for four fixed points, a real capability
loss for the sake of not moving a number that the scroll test already proves is free —
no honest fold, so the raise stands. `kParamBudget` is **10**; see `Registry.cpp`'s own
comment for the exact reasoning recorded there.

**Params, and help text — REWRITTEN 2026-09-15 to what actually holds.** **Target
brightness** now says it is the fixed point: *"The one brightness this effect leaves
EXACTLY alone — everything darker is lifted toward it, everything brighter is darkened
toward it once Darken is on."* **Lift**: *"...on the darker side of Target brightness...
Everything darker than Target moves; Target itself never does."* **Max darken**
(`max_darken`, 1.0..4.0, default 1.0 — OFF): *"The hardest anything above Target may be
darkened, anywhere in the frame — the mirror of Max lift, on the OTHER side of Target. 1.0
means 'do not darken at all', and this whole row is a no-op, whatever Darken is set to;
higher lets a washed-out bright map be pulled back down, never below raw/Max-darken, so a
dark enemy silhouette in front of it keeps its own contrast instead of vanishing into the
glare. Above 1.0, the picture above Target can end up DARKER than the raw scene, not
merely less bright than the lift curve alone would have made it — and Target becomes the
exact brightness nothing ever moves past, which pins the shadows below it a hair
differently than at 1.0 even at Darken 0."* **Darken** (`darken`, 0.0..1.0, default 0.0 —
off): *"The darken that is ALWAYS there, whatever Adaptation says: how much a bright area
(sky, a lit wall) is pulled down below its own raw value, once Max darken is raised above
1.0 (like Lift, this needs its own Max slider off the floor to do anything — at Max darken
1.0 this is a no-op, whatever it is set to). 0 is off. Mirrors Lift; the two apply on
opposite sides of Target brightness, in the same frame, and Target itself never moves
either way. With Max darken raised, going above 0 here is still its own visible change:
everything brighter than Target starts being COMPRESSED toward it instead of merely having
its own lift-curve overshoot undone."* — the switch-point the task asked this row's own
help to name. Both are Params on the same row, which this raise puts at the ceiling: 10
of 10. *(Correction, 2026-09-16: "Adaptive Brightness's own row and this one are now the
only two at it" was written on 2026-09-14, when that row was 8 of the then-ceiling 8;
since its dark floor folded back in as a `Param` on 2026-09-16 it is 9 of 10, so this row
is the only one at the ceiling — see "The panel rows (post-split)" above.)*

**Verified (2026-09-14, the darkening pass; ADDED TO 2026-09-15, the S-curve redesign).**
The 2026-09-14 pass's eleven `[darken]`-tagged cases in `tests/test_effects_curve.cpp`
(endpoints, the closed-form `f'(0) = 1/D` proof, the secant/convexity/monotone guarantee
over the whole `g x D` grid, no NaN/inf near every degenerate edge, the darken binding
readout's own four codes) are UNCHANGED and still pass unmodified against the redesigned
code — `f_dark`/`abv2_toe_dark()` themselves were never touched. Seven NEW `[darken]`
cases added 2026-09-15, exercising exactly the properties the redesign adds or changes:
**(1)** `F(Target) == Target` exactly over the whole `g x S x gDark x D x target` grid
(the fixed point, where the old C0 test only checked the two branches AGREED with each
other, not what they agreed ON); **(2)** `F(x) <= x` and `F(x) >= x/D` for every `x` above
Target (the new guarantee the first cut could not make); **(3)** `F(x)/x <= S` below
Target, inherited through the rescale; **(4)** the pivot kink's ratio matches
`(1/D)/L'(1)` at the shipped defaults and is closer to 1 than the first cut's own plain
`1/D`; **(5)** `L'(1) <= 1` holds over a `g x S x Knee` grid (what makes (4) generally
true, not only at that one example); **(6)** quantised to 8 bits, the kink steps at most 1
code either side of the pivot, over a grid of `g/S/gDark/D/target`; **(7)**
`abv2_g_adapt_lift_z()`/`abv2_g_adapt_dark_z()`'s own boundary limits (defer to static at
the pivot, saturate at the internal ceiling/floor at the far edge, monotone in between)
and `abv2_g_lift_scurve()`'s byte-identical delegation to `abv2_g()` at Max darken 1. The
2026-09-14 byte-identical grids for `abv2_curve2`/`abv2_secant2` at Max darken 1 / Darken 0
and the C0-at-the-pivot/bounded/monotone/no-NaN sweep also pass UNMODIFIED against the new
dispatch (verified directly: the redesigned `abv2_curve2()` still returns exactly
`abv2_curve()` when `D <= 1`, and is still bounded/monotone/continuous by the same kind of
argument, just proved differently — see the new comments in `effects_curve.h`). Full
suite: **538 test cases, 15,952,695 assertions, all pass** (the whole repository's tests,
not only this feature's; `./build-release/tests/gamescope_tests`, `XDG_CONFIG_HOME`
isolated per the project's own hard rule).

**Harness (2026-09-14; RE-AIMED 2026-09-15).** `abv2-darken-bright` (`bright` scene,
Adaptation Scene + Max darken 2 vs Max darken 1) and `abv2-darken-sky` (`skyfore` scene,
Darken 0.5 + Lift default vs Darken 0) both gained a NEW assertion this pass: every band
above Target must now end AT OR BELOW its own RAW value (the redesign's own new
guarantee), not merely below the lift-only capture the way the first cut's own checks
read — the first cut's checks are kept too (order preserved, nothing below `raw/D`), so
both are now dual-guarantee checks. A THIRD check, **`abv2-darken-halo`**, is new
2026-09-15: a vertical profile across `skyfore`'s own hard sky/ground step (a straight
`FillRect` boundary in `tests/effects_scene_client.c`'s `PaintSpecial()`, `nSpecial == 6`)
which straddles Target — the guided filter's own box can blend a pixel's base across the
pivot within its own radius of the line, and this checks that switch does not show up as
an EXTRA halo (the on/off delta close to the line must not exceed the delta the sky/ground
bands already show far from it by more than 4 codes, the same bound the lift-only
`halobox`/`haloinv` checks use). A FOURTH addition reuses the existing `abv2-static`
sampler (guarantee 5, `<= 2` counts peak-to-peak) on a still frame captured WITH
darkening genuinely engaged (Darken 0.5 / Max darken 2 on `skyfore`), not only at the
lift-only defaults the first cut's own static/pan checks ran at — the Scene term's own EMA
is a fresh source of drift the lift-only checks never exercised. All FOUR checks reuse
scenes already in the abv2 block's own ring (no new ring needed, per the SCENES-ring
rule). See the block's own header comment in `scripts/effects-regression.sh` and
`effects_regression_sample.py`'s `cmd_abv2_darken_bright`/`cmd_abv2_darken_sky`/
`cmd_abv2_darken_halo` for exactly what each asserts. `halfsplit` (also named in the
task) remains NOT added as a fifth darken check — it is not in this ring's own scene list,
and `bright` + `skyfore` + the new horizon profile already exercise global darkening,
the local lift-below/darken-above split, and the pivot's own spatial boundary; a
`halfsplit` darken check remains a small, low-risk follow-up, not a gap in the maths.

**Full regression run, first cut (2026-09-14, darkening pass, `scripts/
effects-regression.sh`, 633 s): 88 PASS, 1 FAIL, 30 INFO.** The one FAIL was
`abv2-darken-bright`'s own first cut asserting the raw-255 band came DOWN, fixed the same
day (`f(1) = 1` is an exact fixed point; the check now asserts white stays `<= 255`).

**Full regression run, S-curve redesign (2026-09-15, `scripts/effects-regression.sh`,
two runs — see below for why two).** First run (**630 s**): every pre-existing check
reproduced its numbers to within capture rounding — `abv2-capture-nobinarise`
(`pct128=4.18%`, `sep off=62.5 on=87.0`, bit-identical to the pre-redesign baseline, since
this capture runs at DARKENING'S OWN DEFAULTS, i.e. through the off switch), `abv2-gpu-time`
(`mean=186.5us`, budget 2000us), both `abv2-halo-*-stretch` INFO lines unchanged
(`+3.5`/`-11.0` codes) — and `abv2-darken-bright` PASSED outright on the new "at or below
raw" assertion (`Max darken 1` 214.9/226.0/237.0/247.9/255.0 → `Max darken 2`
179.0/197.0/217.0/239.0/254.9, every one of the first four now BELOW its own raw value
200/215/230/245, order kept, nothing below `raw/D`). Two things this run's own NEW checks
caught, both fixed the same session: **(1)** `abv2-darken-halo` (new this pass) raised a
`ZeroDivisionError` in its own sampler — a thin horizontal strip's `region_mean()` call
used the SAME default `inset=4` a box-shaped region wants (to dodge an anti-aliased edge)
on a region only 8px tall to begin with, dividing its own sample count to 0; fixed by
setting `inset=0` for this strip (it needed no inset at all — the strip's own bounds
already avoid the edge by construction). **(2)** `abv2-darken-sky` FAILED on its own
FIRST-CUT assertion, carried over unchanged from before this redesign — "ground_l/
ground_r/fig8/fig26 unchanged by Darken" — because that claim was true only of the FIRST
CUT's pivot (composed on `L(Target)`, leaving `L(x)` for `x <= Target` byte-for-byte
alone regardless of Max darken). This redesign's own fixed point rescales the lift half
too once Max darken is above its floor, so comparing this capture (Max darken 2) against
`off` (Max darken 1) crosses the OFF SWITCH on purpose — measured: `ground_l` 32.0 → 24.0,
`ground_r` 42.0 → 31.0, `fig8` 19.8 → 15.0, `fig26` 50.8 → 37.8, a consistent ~25% pull
matching `Target·L(x/Target)` vs `L(x)` at these low, well-under-Target inputs (see
"A visible consequence", above) — not a leak of the darken exponent, and not a regression:
a NEW, correct assertion (still lifted above raw, raw ordering preserved) replaced the
stale one, verified directly against the SAME screenshots the failing run produced
(`python3 scripts/effects_regression_sample.py abv2darkensky <off> <on>` and
`abv2darkenhalo <off> <on>` both report PASS on the exact files
`31-skyfore-v2-default.png` / `31-skyfore-v2-darken-on.png` from that run) before the
second, clean run below. **Second run (630 s): 91 PASS, 0 FAIL, 30 INFO** — the
authoritative one. `abv2-darken-sky` (new assertion): sky/cloud1/cloud2 came down BELOW
raw (233.7→218.0 vs raw 225, 241.0→229.8 vs raw 235, 225.7→206.4 vs raw 215 — the new
guarantee) while ground_l/ground_r/fig8/fig26 stayed lifted above their own raw values
(24.0/31.0/15.0/37.8 against raw 14/20/8/26) with their ordering preserved.
`abv2-darken-halo` (new check): `far_sky_delta=-14.8`, `far_ground_delta=-9.5`,
`near_line_deltas=-16.0 -16.5 -9.2 -8.5`, **excess 1.8 codes** — well inside the 4-code
budget, no extra halo from the pivot straddling the horizon. `abv2-static-darken-on` (new
check, guarantee 5 with darkening genuinely engaged): **worst pixel delta 0.00** over 2
captures — the EMA's own Scene-mode drift, even feeding a second exponent now, still
settles to a bit-stable picture.

**Verified on the user's own real capture, through the GPU** (`--image`, private headless
sway + nested `gamescope --backend wayland`, the same recipe `scripts/effects-
regression.sh` uses, standalone; captures under
the untracked `abv2-darken-2026-09-14` capture run). Defaults (`capture-defaults.png`):
identical to the numbers already in this doc's own table above (`pct128=4.18%`,
`sep off=62.5 on=87.0` — the SAME harness numbers, confirmed a second time from a
standalone instance). Darken 0.5 / Max darken 2 (`capture-darken-0.5.png`): looked at
directly — the two images are visually close at a glance (this corridor is mostly
near-black, the case Darken has nothing to do since it only acts above Target), and a
pixel diff confirms it precisely: only 2.3% of pixels moved by more than 2 codes, and
the top 0.5% brightest pixels in the frame (the ceiling lamp fixture and its glow, the
one region genuinely above Target) came down from a mean of 227.8 to 220.8 — about 7
codes, a real but modest pull-down, with the corridor itself (below Target, hence on the
untouched lift branch) unchanged. Matches the task's own prediction exactly: "the
lamp/lit ceiling come down a little, corridor still readable."

**GUI verification.** A literal Inspector screenshot was attempted (open the shell via
`overlay_e2_key "ctrl+alt+shift+o"`, the reserved chord) and did NOT work: that
ConCommand queues into the settings overlay's OWN widget input queue
(`SettingsOverlay_QueueKeyEvent()`), which `DrainInputQueue()` only pumps "while
visible/fading" — so a key queued while the shell is fully CLOSED is never consumed, and
the reserved chord itself is matched at `wlserver_dispatch_key()`'s own path
(`Keybinds.cpp`'s `ProcessKey()`), which is deliberately outside what any `overlay_e2_*`
console command can reach (see `overlay_e2_key`'s own docstring: "cannot reach any
window, client or seat outside this overlay"). There is no headless recipe in this
codebase for opening the shell from closed without real input into `wlserver`'s own key
path, which this project does not expose to scripts — a real gap for future headless GUI
verification, not something this pass worked around. Verified instead with
`overlay_e2_dump_keys` (read-only, safe over a script) against a throwaway instance: all
ten params present under `image.shaders.adaptive_brightness_v2` with the exact ids,
ranges, steps and defaults this section documents, `max_darken` (`slider`, `1..4` step
`0.5`, default `1`) and `darken` (`slider`, `0..1` step `0.05`, default `0`) included.

**Re-verified on the same real capture, S-curve redesign (2026-09-15)** — same recipe,
own standalone instance (the untracked `abv2-scurve-2026-09-15` capture run), `XDG_
CONFIG_HOME` isolated this time (an own `mktemp -d` profile, never the user's real one —
see the config-write mistake note below for why that matters). **Defaults** (Max darken
1, `capture-defaults.png`): `pct128=4.18%`, zombie/floor separation `sep=87.0` (floor
100.5, zombie 13.5) — an EXACT match to this session's own automated harness run on the
identical image (`abv2-capture-nobinarise`), confirming the redesign changes nothing
through the whole GPU pipeline when darkening is off, on this frame as on every
synthetic one.

**Darken 0.5 / Max darken 2** (`capture-darken-0.5.png`) — looked at directly: the two
images are visually close at a glance (this corridor is mostly near-black and the void
guard holds it exactly, 74.7% of pixels are below the black floor and literally
unchanged), but unlike the first cut, the difference is NOT confined to the lamp any
more. Measured:

- `pct128` (pixels >= 128) drops from **4.18% to 0.90%**, and `pct250` from 0.082% to
  0.030% — the picture is markedly LESS washed out with darkening engaged, not merely a
  little.
- The floor walkway (`CAPTURE_FLOOR`, above Target) falls from **100.5 to 69.5** (-31
  codes); the zombie figure itself (`CAPTURE_ZOMBIE`, below Target) ALSO falls, from
  **13.5 to 9.2** (-4.3 codes) — the rescale's own effect on shadows (the PROVEN
  `Target·L(x/Target) <= L(x)` inequality above), not a leak of the darken exponent.
  Separation (`sep`) narrows from 87.0 to **60.3** — the zombie is still clearly readable
  against its floor, just by a smaller margin than the lift-only picture gave it.
- Against the TRUE RAW source image (not the lift-only capture): percentiles up to about
  p95 stay net ABOVE raw (p75 raw 7.1 -> 8.6, p90 raw 58.2 -> 68.4, p95 raw 81.0 -> 86.6 —
  still lifted, just less than the lift-only picture would be), and from p98 up they cross
  BELOW raw (p98 raw 108.1 -> 105.0, p99 raw 128.7 -> 119.6, p99.5 raw 169.0 -> 160.0). The
  TOP 0.5% BRIGHTEST pixels by raw value (the lamp fixture and its ceiling glow) read
  **207.3 raw -> 197.3 with darkening on** — genuinely below raw, by about 10 codes.

**This CONTRADICTS the task's own prediction** ("corridor unchanged") — that prediction
described the FIRST CUT's behaviour (composing the darken half on `L(Target)`, which left
`x <= Target` byte-for-byte alone regardless of Max darken). This redesign's own fixed
point requires rescaling that half too, and on a frame this dark — Scene adaptation drives
the lift exponent far lower than the static-only worked example above, since the content
median itself is deep in shadow (the plan's own §3.1 measured `p50 = 0.00035` on this
exact image) — the rescale's effect on an ALREADY-STRONGLY-LIFTED picture is large, not
subtle. What DOES hold, and is the more accurate summary: **the lamp and its ceiling glow
end up below their own raw value** (confirmed above), and **the corridor stays readable
and still net-lifted above raw**, just by less than the lift-only curve would give it —
"corridor darkened toward raw, not below it" is the honest description on this
frame, not "corridor unchanged".

**GUI verification, re-run with the intended recipe (2026-09-15).** The first cut's own
attempt (`overlay_e2_key "ctrl+alt+shift+o"`) could not open the shell from closed, as
documented below; this pass used the recipe the task named instead —
`toggle_settings_overlay` (a ConCommand, opens/closes the shell directly, bypassing the
reserved-chord input-queue problem entirely) followed by `overlay_e2_select "image.shaders
image.shaders.adaptive_brightness_v2"` and `overlay_e2_scroll 300` — and it WORKS
headless: `inspector-v2-top.png` and `inspector-v2-scroll-300.png` both show the row open,
"Parameters 10 of 10", Adaptation "Scene", Max darken slider at 2, Darken slider at 0.5,
all ten rows present and legible after the scroll. Looked at directly (not merely
enumerated by `overlay_e2_dump_keys`, though that was cross-checked too and agrees) —
confirms both that the GUI recipe this task named is real and reachable headless, and
that the actual panel reflects the state the console commands set.

**A real config-write mistake, caught and fixed.** The manual GPU-capture script above
does not isolate `XDG_CONFIG_HOME` the way `scripts/effects-regression.sh` deliberately
does — an oversight in a one-off script, not a product bug — so its `overlay_e2_set`
calls persisted into the user's OWN `profiles/Default.json`, leaving
`adaptive_brightness_v2` turned on with `darken 0.5` / `max_darken 2.0` baked in. Caught
by inspecting the file's own mtime against the script's run time, and reverted by hand
to the compiled defaults (`enabled: false`, `darken: 0.0`, `max_darken: 1.0`, every other
field already matched the compiled default, which is why every field but the three this
script explicitly set was left alone) within the same session. No other profile file was
touched (`CS2.json`'s own `max_darken 4.0` is Adaptive Gamma's pre-existing, unrelated
field). Recorded here rather than only in the commit, since a doc is where the next
agent finds it: any future one-off manual-capture script against this codebase MUST set
its own isolated `XDG_CONFIG_HOME` (`mktemp -d`), the way the real regression script
already does, or it will write test values into the user's live settings.

