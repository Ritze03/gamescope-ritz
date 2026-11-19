# Frame generation: Motion version 3

Part of [Frame generation](frame-generation.md). The library's third motion algorithm, the one built for the best picture.

## Motion version 3

(2026-11-19, library `c2644af` on its `dev` branch; the pin moves again with a later brief, and the
build list below is meant to survive that.) The library's `MotionVersion::V3`: a gather-based,
two-flow, occlusion-aware interpolator. It estimates motion in both directions, finds the pixels
that one of the two real frames hides, fills the vectors in those hidden strips from their
neighbours and builds each generated pixel from both real frames along both vectors. Where it
cannot trust the motion it **smears along the motion** (a short blur along the vector, strongest
where the match is poor); it never holds a patch of a real frame, and the only real frame it shows
is a whole-frame scene cut. Same latency as V1 and V2. V1, V2 and Learned are unchanged and the
default stays `v1`. Library details: `subprojects/FrameGen/INTEGRATION.md`, "Motion version 3".

- **Selecting it.** Frame generation's **Motion version** row gains a fourth choice, **V3**
  (`framegen.motion_version` = `v3`); Motion blur's own Motion version row (when its "Use Frame
  generation's settings" is off) offers it too (`motion_blur.motion_version` = `v3`). Host enum
  `fghost::Motion::V3` = 3 (the library's value and the overlay index). It fits the two bits the
  main config word (bits 28-29) and the blur word (bits 15-16) already give the motion version,
  so no word grew. Switching at run time is an ordinary `setSettings()` (no GPU wait; the
  library drops its estimate, and the host drops its cached outputs, the same block as for any
  version switch). The help line: "V3: most fluid; smears where it is unsure; costs about three
  times V1."
- **Segmented or dropdown.** Both Motion version rows are plain `Choice` rows with no
  `.Segmented()`, so the shell measures them: four short labels may still fit as segments,
  otherwise the row becomes a dropdown ([STYLING-GUIDE](../ui/STYLING-GUIDE.md), "Segmented vs
  dropdown"). Nothing was hand-tuned for four options.
- **Quality row.** V3 honours `flowScale` (1, 2, 4), so it follows the Quality row like V1: Quality
  = flowScale 2, Performance = flowScale 4 (0.89 ms against 0.44 ms per real pair at 1080p). A change
  while V3 runs is a structural `reconfigure()` (GPU wait, estimate dropped), as for V1.
  `Why:` forcing 2 as for V2 and Learned would have hidden the library's cheapest knob; V2 and Learned
  force it only because their own mode / nets need it. `ResolveMotion()` therefore checks V3 with
  the Quality row's own flowScale (`EnsureReady()` leaves it alone for V3), and the host leaves
  `subpixel` as the row gives it (V3 ignores it).

## The four settings

All four are runtime (`setSettings()`; no buffers re-created, the estimate is kept). Group "Motion
version 3" in the Frame generation area (column 1, right after "Learned motion"); sliders with
step 0.05; enabled only while V3 is the version the interpolator actually runs and Frame
generation or Motion blur is on (`V3RowsUsable()`, the same rule as the Learned rows: Motion blur's
own version counts when it is on with its own settings). Motion blur running V3 on the shared
interpolator takes the values from Frame generation's config (one interpolator), like the reach
values.

| Key (under `framegen`) | Library field | Range | Default | Row | Meaning |
|---|---|---|---|---|---|
| `v3_smear` | `v3Smear` | 0-1 | 0.35 | Smear | how strongly unsure areas are smeared along the motion; 0 = none (sharper, wrong spots show as sharp ghosts) |
| `v3_occl_soft` | `v3OcclSoft` | 0.25-4 | 1.0 | Occlusion softness | width of the ramp at the edge of a hidden area; higher = softer, more one-sided |
| `v3_unsure` | `v3Unsure` | 0.25-4 | 1.0 | Unsure sensitivity | scales the poor-match ramp of the unsure measure; higher = trusts worse matches (less smear, more risk) |
| `v3_scene_cut` | `v3SceneCut` | 0-4 | 0.45 | Scene cut threshold | histogram distance above which a pair is a scene cut (nearer real frame shown); 0 = never. Applies from the next estimate |

Additive (schema stays 5): an absent key loads the default; a value is clamped to its range on
load; a non-finite or wrong-typed value takes the default (the bit-pattern test, because the
project builds with `-ffast-math`); written rounded to hundredths, like `search_penalty`.

**Host path.** Overlay row -> `config::FrameGenSettings` -> `fghost::Config` (`v3Smear`,
`v3OcclSoft`, `v3Unsure`, `v3SceneCut`, constants `kV3*` in `FrameGenHost.h`) -> a **sixth word**,
`g_ulV3`, a `std::atomic<uint64_t>` (`FrameGen/FrameGenV3.h`, `PackV3()` / `UnpackV3()`):

- bits 0-11 smear, 12-23 occlusion softness, 24-35 unsure sensitivity, 36-47 scene cut, each in
  hundredths;
- **bits 48 and 49 are reserved** for the library's two bools `v3ColourWindow` and
  `v3DetailSplit`. They are **not exposed**: at library `c2644af` they are accepted and do nothing
  (output is identical on and off). A later brief fills them; until then `PackV3()` writes 0 there
  and `UnpackV3()` ignores them.

`Why a new word:` the main, UI-box, Learned, reach and blur words and the 64-bit applied key are
full. `Why hundredths:` 0.35 and 0.45 survive `value / 100.0` -> float exactly (proved in
`tests/test_framegen_v3.cpp`), so the defaults change nothing. `EnsureReady()` compares the word
with `Host_t::ulAppliedV3` (set in the init path and at the end of the apply block, reset in
`Teardown()`), so a change of any value re-applies the settings, once. `ToLibrarySettings()` sets
the four fields (clamped, finite-tested); the `setSettings()` that `SyncUiHistory()` derives from
`H.interp.settings()` carries them along.

## Rows greyed under V3

Under V3 (the effective version: Motion blur's own one when it is on with its own settings), only
rows that drive nothing V3 uses are greyed, per the library's table of ignored settings:

| Row | Library fields it sets | Under V3 |
|---|---|---|
| Search radius, Search radius (coarsest level) | `searchRadius`, `searchRadiusTop` | greyed (`RadiusRowsUsable()`, which also covers Learned) |
| Pyramid smallest level | `pyramidMinSide` | greyed (`PenaltyRowsUsable()`): V3 builds its own pyramid; a change would still rebuild the shared buffers without changing V3's picture |
| Search penalty | `searchPenalty` | greyed (`PenaltyRowsUsable()`) |
| Static HUD protection | `hudBonus` only | greyed (`HudRowUsable()`) |
| Artifact safety | `trustLow` / `trustHigh` / `sceneCutSad` **and `globalFallback`** | **left enabled**: `globalFallback` (Off sets it to 1.0) is V3's "estimate failed" threshold |
| Quality | `flowScale` (and `subpixel`, `v2Mode`) | enabled: V3 honours `flowScale` |
| Limit to GPU speed, UI protection, Crosshair box size, Motion blur rows | pacing / `ui*` / `blur*` | enabled: V3 honours them |
| Learned rows | `learned*` | already enabled only under Learned |

There is no row for `v2StaticMask` (the library default, honoured by V3), `smoothBonus`
(ignored) or `subpixel` alone. `Why only these:` the brief asked to grey only what is cheap with a
`DisabledUnless` predicate and to keep a row enabled if anything it sets is honoured; Artifact
safety is the case where that matters. Its help text still talks about the V1 ramp; under V3 only
the whole-frame fallback of its levels applies.

## Build

`src/meson.build`'s FRAME GENERATION block compiles, with the same glslang flags as the rest:

- **14 plain compiles** (no define), added to the plain list: `v3_synth`, `v3_planes`, `v3_pyr`,
  `v3_hist`, `v3_search`, `v3_search_top`, `v3_dense`, `v3_wide`, `v3_stats`, `v3_occl`,
  `v3_fill`, `v3_project`, `v3_enclose`, `v3_decide` -> `v3_<n>_spv.h`, variable `v3_<n>_spv`.
  `framegen.cpp` includes them unconditionally, so the build fails without them.
- **Per colour format**: `v3_planes` and `v3_synth` are added to `framegen_colour_shaders`, compiled
  with `-DFG_FORMAT_RGB10=1` and `-DFG_FORMAT_F16=1` -> `v3_planes_<fmt>_spv.h`,
  `v3_synth_<fmt>_spv.h` (4 more compiles). The library finds them through `__has_include`. NV12 /
  P010 stay unbuilt (see [limits](frame-generation-limits.md#build)); without a format's variants
  the library refuses V3 for it with a reason and the host falls back to V1.
- **Includes, no compile of their own** (resolved relative to each `.comp`, listed by the
  depfile): `v3_common.glsl`, `v3_format.glsl`, `v3_weights.glsl`, `v3_maps.glsl`, `v3_est.glsl`,
  `v3_search.glsl`, `v3_static.glsl`.
- V3 reuses V2's plain `v2_static` build. No new library `.cpp`, include path or define.
- **ccache hazard.** The variants are found by `__has_include`, which a dependency file cannot
  record when the header did not exist at the last compile. `src/FrameGen/FrameGenLib.cpp` carries
  a comment naming the variant set: touch it when the set changes (this change did), and check
  the objects after a build (`nm` on the FrameGen object for `v3_*_spv`).

## Fallback

`ResolveMotion()` asks `settingsError()` with the settings that will be applied; a refusal (missing
variants for the format, size limits) runs **V1**, sets the Status note (`g_pszMotionNote`) to
the library's reason and logs `frame generation: Motion version 3 refused, running V1: <reason>`
once per change of reason. A `setSettings()` that still fails sets "the library could not create
Motion version 3 (its pipelines or buffers)". `initError()` is already logged when init fails.

## Cost and memory

Measured in the library (RX 7900 XTX, 1080p, flowScale 2, Rgba8, per real pair, back to back):
0.89 ms at 2x, 1.07 at 3x, 1.26 at 4x (each generated frame beyond the first about 0.18 ms); worst
format F16 linear 0.99 ms at 2x; flowScale 4: 0.44 ms; flowScale 1: 2.84 ms; 1440p flowScale 2:
1.55 ms. V1 is about 0.30 ms, V2 Quality 0.27. Memory about 56 MiB at 1080p flowScale 2 (224 MiB at
flowScale 1), plus 7.9 MiB per interior motion-blur sample.

**Pacing needs no V3 entry.** Neither the pacer nor the cost guard holds a per-version cost table
or constant: the guard budgets from the GPU timings the renderer measures every pair (the
estimate's and the synths' time, `g_flLastEstimateMs` / `g_flLastSynthMs`, handed to the library's
`pacing.h` as `Inputs::estimateMs` / `synthMs`), so V3's higher cost lowers the output rate or
passes through by itself exactly as a large search radius does for V1. One host piece is V3-aware: the profile reader in `FrameGenHost.cpp` splits
the library's timestamp chain into estimate and synth by stamp name, and knew only V1's names. V3's
synth stamps are `project` (when it has interior samples) then `synth` / `blur synth` (one stamp
for a whole blurred output); they are now recognised as synth starts. `Why:` without it every V3
stamp after `luma` counted as estimate time, idle gap included: the Status line read "estimate 15.95 ms,
synth n/a" at 1080p-class load instead of about 0.6 ms (500x500 test window), which would have made
the cost guard lower the rate for nothing. V2's `splat` / `coverage` / `morph` stamps are still unknown to
this parser (V2 showed "estimate 24 ms, synth n/a" in the same test): left alone because nothing may change for V2 in this
change; see the report note. (The "V1 budget 0.33 ms"
in the reach help text is the library's own figure for V1, not a host constant.)

## Known limits

From the library: the static-structure mask is luma-based; fast pans at a 30 fps base look soft
(the smear); a scene cut between two scenes with equal level statistics is not seen (the histogram
distance stays low). `v3SceneCut` is read by the estimate, so a change applies from the next real
pair.
