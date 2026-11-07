# Shader effects: colour, shadow and sharpen effects

Shadow Control, Saturation, Vibrancy, Color Boost and Pre-Sharpen. Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

### Shadow Control (`image.shaders.shadow_lift`)

Added 2026-09-04 (request #3: *"a darkness booster for dark games"*). Brightens dark
areas so detail becomes visible while leaving bright areas essentially alone — **not** a
global brightness control and **not** shadow-crushing.

**Config**: `ReshadeShadowLiftSettings` — `enabled` (default false), `strength` (0.0..1.0,
default 0.0/neutral). Purely additive keys; no migration.

```
exponent = 1.0 - 0.5 * strength   // 1.0 (identity) down to 0.5 (sqrt) at strength=1.0
output   = pow(saturate(color), exponent)
```

`Why a gamma curve:` 0.0 and 1.0 are fixed points of *any* power curve, so black and white
never move — the "leave highlights alone" requirement guaranteed mathematically. The
curve's effect concentrates at the low end (`0.1 → 0.316` vs `0.9 → 0.949` at full
strength). The exponent floor of 0.5 is the same shape a "raise gamma to ~2.0" boost
applies. `Why first:` a tone/exposure adjustment; lift-before-saturate is the conventional
grading order, and it means Vibrancy's grey target is computed from the lifted colour.

### Saturation (`image.shaders.saturation`) — renamed from "Vibrancy" 2026-09-08

Adaptive saturation with an optional skin-tone damper. **Renamed from "Vibrancy"
2026-09-08** — the user's own observation: this effect is a flat multiplier, the same
relative boost applied to every pixel regardless of how saturated it already is, which
is what an iPhone "Saturation" slider does, not that app's "Vibrancy". The maths below
is **byte-for-byte unchanged** by the rename — only the id, the config key and the
label moved. See [the Vibrancy section](#vibrancy-imageshadersvibrancy--new-2026-09-08)
below for the new effect that took the freed-up "Vibrancy" name, and
["Verified: the rename changed nothing"](#verified-the-rename-changed-nothing-and-the-new-effect-works-as-designed-2026-09-08)
for the measurement that backs "byte-for-byte" up.

**Config**: `ReshadeSaturationSettings` — `enabled`, `strength` (float),
`protect_skin_tones` (default true). (`ReshadeVibrancySettings` is now a *different*
struct — the new effect's, below.)

**`strength` — a true saturation multiplier, 0.0..3.0, neutral at 1.0** (changed
2026-09-04, request #2, from an additive -1.0..+1.0 boost):

```
mix   = min(strength, 1.0)
boost = max(strength - 1.0, 0.0) * (1.0 - saturation) * skin_protect
output = lerp(luma, color, mix + boost)
```

`mix` alone reaches both endpoints (0.0 = full grey, 1.0 = unchanged) with a plain blend
toward luma, carrying none of the adaptive shaping — at 0.0 *every* pixel must land on the
same grey. `boost` is zero at and below neutral and picks up the adaptive shape above it.
`Why 0.0..3.0 with neutral at 1.0:` the user chose to keep desaturation reachable
(`requests-2026-09-04.md` #2). Note that `boost`'s own `(1.0 - saturation)` term means
that *above* neutral (`strength` > 1), this effect already leans muted-first — an
already-saturated pixel picks up less of the extra boost than a duller one does; only
the `mix`-only regime (`strength` <= 1) is a genuinely flat, saturation-independent
multiplier. See the measured ratio table below.

#### Migration: an existing config's old value, twice

`kCurrentSchemaVersion` 1 → 2; `Migrate_1_to_2()` (`src/Config/ConfigManager.cpp`)
transforms `reshade.vibrancy.strength` once, on load: `new = clamp(old + 1.0, 0.0, 3.0)`.
`Why +1.0:` it carries old-neutral (0.0) onto new-neutral (1.0), so an untouched config
does not open in black and white; a customised value keeps its displacement from neutral.
A config saved under schema 2 round-trips unmigrated — `tests/test_config.cpp`.

`kCurrentSchemaVersion` 3 → 4 (2026-09-08): `Migrate_3_to_4()` renames the JSON object
`reshade.vibrancy` to `reshade.saturation` in place, once, on load — a pure key rename,
no value transform (the 1→2 step already handled the value's meaning; this step only
runs *after* it, so an ancient schema-0/1 file gets both in the right order). `Why not
just leave the old key readable too:` the new "Vibrancy" effect below needed the
`reshade.vibrancy` key for itself, so an old file's `reshade.vibrancy` object had to
stop meaning the old effect **before** it could start meaning the new one, or an old
config would silently load as the wrong effect (a saturation multiplier reread as a
punch-boost strength — wrong shape, wrong scale, no crash to notice it by). The general
policy this follows: **migrate on load, not "just ignore the old key"** — an old file's
saved value is real user data (their `strength`, their `protect_skin_tones`), and
dropping it silently would be a bigger surprise than a one-line rename. Saving after
load naturally rewrites the file under the new key (`SectionsToJson()` only ever emits
the current field names), so an old profile self-heals the first time anything saves it
— it does not need to be touched by hand. `tests/test_config.cpp`'s "a schema-3
profile's vibrancy key renames to saturation on load and rewrites on save" pins this;
so does the fact that this session's own real `~/.config/gamescope-ritz/profiles/*.json`
files were rewritten this way (by hand, with the user's explicit permission, since only
the key needed to change and a full save-through-the-app round trip risked touching
unrelated formatting) — see the CHANGELOG.md Info entry and this doc's own history.

### Vibrancy (`image.shaders.vibrancy`) — NEW 2026-09-08

The user's request, verbatim: *"Our current 'Vibrancy' behaves like 'Saturation' when
editing images on an iphone. The color highlighter should make already punchier colors
even more punchy. So it should behave more like the 'Vibrancy' option on an iphone."*
Built exactly as described: this effect's boost **rises with a pixel's own existing
saturation** — a near-neutral pixel is left close to untouched, and an already-punchy
pixel is pushed further.

**Discrepancy, flagged rather than silently resolved:** on Apple's own Photos app, the
control actually named *Vibrance* conventionally does the **opposite** of what is built
here — it boosts *muted* colours more and *protects* already-saturated ones (skin tones
in particular), which is precisely the shape [Saturation](#saturation-imageshaderssaturation--renamed-from-vibrancy-2026-09-08)'s
`boost` term already had, above neutral, *before* this rename. So the user's stated
definition of "Vibrancy" is the inverse of the word's usual photo-editing meaning. This
doc builds exactly what was asked — more boost the more saturated a colour already is —
and says so plainly rather than "correcting" it to match Apple's convention. If the
direction ever feels backwards in practice, that is the thing to revisit; the maths
below is not a misunderstanding, it is the literal spec.

**Config**: `ReshadeVibrancySettings` — `enabled` (default false), `strength` (float,
0.0..3.0, default 0.0/neutral). One param — no skin-tone toggle: skin tones sit at a
moderate, not extreme, saturation, so this effect already gives them a moderate rather
than maximal boost on its own; adding a second toggle to suppress an effect this mild on
skin was not worth a second control (see the panel's own comment for the same point).

```
sat  = max(c.r, c.g, c.b) - min(c.r, c.g, c.b)   // 0..1, the pixel's own chroma
gain = 1.0 + strength * sat                       // >= 1.0 always
out  = clamp(luma + (c - luma) * gain, 0.0, 1.0)
```

`Why this shape:` `gain` is `1.0` (identity) at `sat = 0` regardless of `strength` — a
grey pixel has `c == luma` exactly, so it is an *exact* no-op there, not an
approximation, at every strength. `gain` grows linearly with the pixel's own saturation,
so the punchiest colours (`sat` near 1) get pushed by up to `1 + strength`, while a
barely-tinted colour (`sat` near 0) gets almost no push — "already punchier colors even
more punchy" stated as a formula. `Why linear in `sat`, not `sat²` or a curve:` this is
the cheapest shape that satisfies the request (one multiply, one madd inside a function
already computing `luma` and `sat` for other reasons — see `effects_common.h`'s
`grade()`), it is monotonic by construction, and the measured table below shows it
already produces a clearly increasing boost across four bands without needing a steeper
curve. `Why `1.0 + strength * sat` and not, say, `strength ^ sat`:` the additive-gain
form keeps `strength = 0` an exact identity at every saturation (multiplying by `0^sat`
would not), which is what "0 is off" has to mean for a switch's paired param.
`Why the final clamp, and why it can slightly desaturate the punchiest colours:` the
same reason every other effect in `grade()` ends on `clamp(..., 0.0, 1.0)` — `c - luma`
scaled by a `gain` > 1 can overshoot 0 or 1 on a channel that started near the gamut
edge, and clamping is what "never wraps hue" means in practice: the *direction* away
from `luma` is preserved exactly (each channel's sign never flips), only the *magnitude*
is capped. A pixel that is already at the sRGB gamut boundary for its hue (one channel
at 0, one at 255 — `sat = 1.0` exactly) is *already as saturated as an 8-bit encoding can
represent*, so no `strength` can push its measured chroma any further; this is not a bug,
it is the ceiling every colour effect in this pipeline runs into eventually.

**Range `0.0..3.0`, additive on top of the always-present `1.0`:** `strength` is added to
a neutral `1.0` gain, so `0.0` here is simply "off" and needs no second special value,
unlike Saturation's `0.0..3.0` multiplier, which has to reach `0.0` (full grey) as one of
its endpoints. At `strength = 3.0` the most saturated pixels get four times their original
chroma before clamping (`gain = 4`).

`Why:` raised from 2.0 to 3.0 on 2026-10-04 at the user's request (at 3.0 the gain is 4).

**Where it sits in the Effects band, and why:** registered immediately after Saturation
(`PanelShaders.cpp`), so the two colour-intensity effects read together — a user
comparing "the old slider" against "the new one" finds them adjacent rather than
scattered among Shadow Control / Pre-Sharpen / Adaptive Brightness. The Effects band went
from 4 switch rows to **5**; this row's own Inspector column has **1** param (`strength`),
nowhere near `kParamBudget`'s 8 (Adaptive Brightness still owns that ceiling, unchanged).

#### Verified: the rename changed nothing, and the new effect works as designed (2026-09-08)

Measured with `scripts/effects-regression.sh`'s `colors` scene (added for this change:
`tests/effects_scene_client.c`'s `kColorBands`, five horizontal RGB bands instead of the
grey levels every other scene there uses — grey has `sat = 0`, so it is an exact no-op
for both colour effects and could never have tested either one). Every scene capture
sampled and checked against the closed-form formulas above by
`scripts/effects_regression_sample.py`'s `colorcheck`/`colorshape` subcommands — not eyeballed:

| band (input RGB, `sat`) | off | Saturation 1.0 (unchanged) | Saturation 2.0 | Vibrancy 0.5 | Vibrancy 1.0 |
| --- | --- | --- | --- | --- | --- |
| 0: (128,128,128), 0.000 | (128,128,128) | (128,128,128) | (128,128,128) | (128,128,128) | (128,128,128) |
| 1: (148,134,120), 0.110 | (148,134,120) | (148,134,120) | (158,132,105) | (149,134,119) | (149,134,118) |
| 2: (178,140,92), 0.337 | (178,140,92) | (178,140,92) | (199,136,56) | (183,139,83) | (189,138,74) |
| 3: (214,118,54), 0.627 | (214,118,54) | (214,118,54) | (242,110,22) | (237,111,27) | (255,105,0) |
| 4: (255,60,0), 1.000 | (255,60,0) | (255,60,0) | (255,60,0) | (255,34,0) | (255,9,0) |

Every one of those 25 cells matched its formula's prediction to within capture rounding
(worst deviation 0.5 of 255 counts, `colors-saturation-*`/`colors-vibrancy-*`, all PASS)
— **Saturation at 1.0 reproduces the input exactly** (the identity case, proving the
rename moved nothing), and the pure-grey band (0) is untouched by either effect at every
strength tested, including Vibrancy at 2.0 (not shown above) — confirming the "grey stays
grey" invariant both formulas share.

**The headline shape check** (`colors-shape`, PASS) makes the qualitative difference a
number rather than an impression: captured saturation ÷ input saturation, per band —

| | band 1 | band 2 | band 3 | band 4 |
| --- | --- | --- | --- | --- |
| Saturation @ 0.5 (pure `mix`, no `boost`) | 0.50 | 0.50 | 0.50 | 0.50 |
| Vibrancy @ 1.0 | 1.11 | 1.34 | 1.59 | 1.00 |

Saturation's ratio is **exactly flat** (spread 0.00) — the same relative change for
every pixel regardless of its own saturation, which is the literal meaning of "behaves
like Saturation." Vibrancy's ratio **strictly increases** across bands 1-3 — the more
saturated the input, the larger the relative boost — before band 4 lands back at 1.00,
which is the gamut-clamp ceiling described above, not a break in the trend: band 4's
input, `(255, 60, 0)`, already has a channel at each end of the 0..255 range, so its
chroma cannot be measured any higher than 255 no matter how large `gain` gets. (Note the
comparison deliberately uses Saturation at `0.5`, its pure-multiplier `mix`-only regime,
rather than a strength above neutral — see the formula section above for why `boost`
would make even the *old, renamed* effect's own ratio non-flat there, which would have
muddied exactly the comparison this check exists to make.)

Captures: the untracked `vibrancy-split-2026-09-08` capture run (`effects-regression-captures/10-colors-*.png`);
full numeric results: that directory's `results.txt`.

### Color Boost (`image.shaders.color_boost`) — NEW 2026-10-29

The user's request, verbatim: *"Add another shader: "Color Boost". It should make all of
the colors more vibrant with a gamma (maybe inverted?) curve, so low saturation stuff gets
greatly boosted and already high saturation stuff doesnt "blow out"."*

**Config**: `ReshadeColorBoostSettings` — `enabled` (default false), `strength` (float,
0.0..3.0, default 1.0; 0 is an exact identity). JSON `reshade.color_boost`. Additive key,
no schema bump (Vibrancy's precedent): an older profile has no such object and resolves
to these defaults. One param, no skin-tone toggle (YAGNI).

**Against its neighbours.** Saturation is a flat multiplier (every colour gains the same
factor, so it clips the strong ones first). Vibrancy (this fork's, the user's own
definition) boosts already-saturated colours *more*. Color Boost goes the other way: the
weaker a colour, the more it gains, and a colour already at its strongest gains nothing.

```
d    = c - luma                      // chroma vector; luma = effects_luma(c), Rec.601, encoded
t    = max over channels of  d>0 ? d/(1-luma) : -d/luma    // 1/gmax, in [0,1]
t'   = 1 - (1 - t)^k,   k = 1 + strength
out  = luma + d * (t' / t)           // t == 0 (grey, t < 1e-5): out = c
```

`t` is the colour's chroma as a **fraction of the largest chroma its own hue can have at
this luma** (`gmax` = the biggest `g` with `luma + d*g` still inside `[0,1]` on every
channel; `t = 1/gmax`). 0 means grey, 1 means a channel already sits at 0 or 1.

`Why an inverted gamma curve on that fraction, not on raw saturation:` the user asked for
"low saturation greatly boosted, high saturation does not blow out". `t' = 1 - (1-t)^k` has
slope `k` at `t = 0` (muted greatly boosted), slope `0` at `t = 1` (nothing at the edge),
and `t' <= 1` for every `t`, so the output **cannot leave the gamut by construction**:
saturated colours approach the edge asymptotically instead of hitting a clamp. Measuring
against the *gamut fraction* rather than `max-min` makes "already high saturation" mean
"close to clipping for this hue and brightness" — a dark, strong colour and a light,
strong colour both count as strong when they are near their own edge, which plain
`max-min` cannot tell. A plain gamma on `max-min` has no such guarantee and needs a clamp,
which is exactly the blow-out being avoided.

Properties (pinned by the closed-form checks): grey stays exactly grey; hue (the
direction of `d`) and luma are preserved (`d` is only scaled and `effects_luma` is
linear); `strength 0` is `t' = t`, an exact identity; the output never leaves `[0,1]` (the
shader's final `clamp` only absorbs float rounding); monotonic — a more saturated input
stays more saturated.

`Why after Vibrancy in grade():` both are colour-intensity steps; running Color Boost last
lets it act on the picture Saturation/Vibrancy produced and means whatever those two
pushed toward the edge is left alone by it, rather than being boosted and then having to
be clamped. The panel row sits right after Vibrancy for the same reason.

**Numbers** (closed form on the `colors` scene's five bands, strength 1.0, so k = 2;
input gamut fraction `t` -> output, ratio `out_t/in_t`): band 1 `0.121 -> 0.228` (1.88),
band 2 `0.369 -> 0.602` (1.63), band 3 `0.645 -> 0.874` (1.36), band 4 (255,60,0, already
on the edge) `1.000 -> 1.000` (1.00); grey band unchanged. The ratio strictly *decreases*
across bands, the mirror image of Vibrancy's rising ratio. A 200 000-colour random sweep
at strengths 0.5/1/3 never left `[0,255]`. Regression: `colors-colorboost-<S>`
(per-band closed form, S = 0.5/1.0/3.0) and `colors-colorboost-shape` in
`scripts/effects-regression.sh`; the live capture was **not run** when this was written
(needs a GPU session), only the closed-form side was checked offline.

### Pre-Sharpen (`image.shaders.presharpen`) — now RCAS

**Config**: `ReshadePreSharpenSettings` — `enabled`, `strength` (`optional<float>`,
0.0..2.0, default 0.5).

The `.fx` used a plain 4-tap unsharp mask. The native pass **reuses FSR1's RCAS**
(`src/shaders/ffx_fsr1.h`, `FSR_RCAS_F`) instead: the same 5-tap cross, but clip-aware
(its lobe is limited so a sharpened value cannot overshoot the local min/max ring) and
normalised, so it does not ring on hard edges at high strength. `FsrRcasLoadF(p)` is
defined as a clamped `texelFetch` on slot 0 **with Shadow Control and Vibrancy applied to
each tap**, so the sharpen sees the graded image exactly as the `.fx`'s pass order did;
when sharpen is off the centre tap is used directly. `FsrRcasInputF` is empty, as in
`cs_composite_rcas.comp` — input is already encoded.

**Slider → `con.x` mapping**, so today's slider feel is preserved: RCAS scales its lobe by
`con.x ∈ 0..1` (`FsrRcasCon()` derives it as `exp2(-stops)`). With `k` the slider value,

```
con.x = clamp( k / (0.75 * (1 + k)), 0, 1 )      // 0→0 (off), 0.5→0.444, 1→0.667, 2→0.889
```

monotonic and saturating, so the top of the slider is "as sharp as RCAS goes" rather than
a cliff. Passed as float bits (`u_rcasCon`), like `RcasPushData_t::u_c1`.

## History

- **Color Boost** (glossary detail): **Color Boost** — the Shaders effect added 2026-10-29 (`image.shaders.color_boost`, config `reshade.color_boost`, `src/shaders/effects_common.h`'s `grade()` step 4, right after Vibrancy), from the user's *"make all of the colors more vibrant with a gamma (maybe inverted?) curve, so low saturation stuff gets greatly boosted and already high saturation stuff doesnt "blow out""*: an inverted gamma curve (`t' = 1 - (1-t)^k`, `k = 1 + Strength`) on each colour's **gamut fraction** `t` (its chroma as a fraction of the largest chroma its hue can have at that luma), so muted colours gain most, colours already at the edge gain nothing, and the result cannot leave the gamut (no clipping). The opposite direction to **Vibrancy** (which boosts already-saturated colours more) and not Saturation's flat multiplier.
