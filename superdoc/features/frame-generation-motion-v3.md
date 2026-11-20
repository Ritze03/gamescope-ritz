# Frame generation: Motion version 3

Part of [Frame generation](frame-generation.md). The library's third motion algorithm, the one built for the best picture.

## Motion version 3

(2026-11-19, library `acab885` on its `dev` branch; the build list below is meant to survive a later pin.) The library's `MotionVersion::V3`: a gather-based,
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

## The four settings, two switches and the Motion guard

All six are runtime (`setSettings()`; no buffers re-created, the estimate is kept). Group "Motion
version 3" in the Frame generation area (column 1, after "Learned motion"); sliders with
step 0.05; **shown** only with Expert settings on and V3 the version the interpolator actually
runs (`ShowV3Rows()`; see [Expert settings](frame-generation-settings.md#expert-settings)), and
**enabled** only while Frame generation or Motion blur is on (`V3RowsUsable()`; Motion blur's own
version counts when it is on with its own settings). Motion blur running V3 on the shared
interpolator takes the values from Frame generation's config (one interpolator), like the reach
values.

| Key (under `framegen`) | Library field | Range | Default | Row | Meaning |
|---|---|---|---|---|---|
| `v3_smear` | `v3Smear` | 0-1 | 0.35 | Smear | how strongly unsure areas are smeared along the motion; 0 = none (sharper, wrong spots show as sharp ghosts) |
| `v3_occl_soft` | `v3OcclSoft` | 0.25-4 | 1.0 | Occlusion softness | width of the ramp at the edge of a hidden area; higher = softer, more one-sided |
| `v3_unsure` | `v3Unsure` | 0.25-4 | 1.0 | Unsure sensitivity | scales the poor-match ramp of the unsure measure; higher = trusts worse matches (less smear, more risk) |
| `v3_scene_cut` | `v3SceneCut` | 0-4 | 0 (Never) | Scene cut threshold | histogram distance above which a pair is a scene cut (nearer real frame shown); 0 = never (default since 2026-11-19). Applies from the next estimate |

| `v3_colour_window` | `v3ColourWindow` | on / off | off | Colour-weighted matching | the motion search weighted by colour similarity; can sharpen clearly coloured moving objects, can break areas next to flat dark regions; +0.05 ms. Applies from the next estimate |
| `v3_detail_split` | `v3DetailSplit` | on / off | off | Detail from one frame | only the coarse picture is blended, the fine detail comes from one frame; slightly crisper texture and text; +0.06 ms per generated frame. Applies from the next generated frame |

Both switches are experiments to compare by eye, on and off, while a game runs (the rows are
switches, reachable by `overlay_e2_set`); with both off V3's output is byte-identical to V3 without
them. Additive (schema stays 5): an absent key loads the default; a value is clamped to its range on
load; a non-finite or wrong-typed value takes the default (the bit-pattern test, because the
project builds with `-ffast-math`); written rounded to hundredths, like `search_penalty`.

**Host path.** Overlay row -> `config::FrameGenSettings` -> `fghost::Config` (`v3Smear`,
`v3OcclSoft`, `v3Unsure`, `v3SceneCut`, constants `kV3*` in `FrameGenHost.h`) -> a **sixth word**,
`g_ulV3`, a `std::atomic<uint64_t>` (`FrameGen/FrameGenV3.h`, `PackV3()` / `UnpackV3()`):

- bits 0-11 smear, 12-23 occlusion softness, 24-35 unsure sensitivity, 36-47 scene cut, each in
  hundredths;
- bit 48 `v3ColourWindow`, bit 49 `v3DetailSplit`; bits 50-51 `v3Guard` (0 Off, 1 Guard, 2 V2 motion; 3
  unpacks as 0); bits 52-63 unused (written 0, ignored).

`Why a new word:` the main, UI-box, Learned, reach and blur words and the 64-bit applied key are
full. `Why hundredths:` 0.35 and 0.45 survive `value / 100.0` -> float exactly (proved in
`tests/test_framegen_v3.cpp`), so the defaults change nothing. `EnsureReady()` compares the word
with `Host_t::ulAppliedV3` (set in the init path and at the end of the apply block, reset in
`Teardown()`), so a change of any value re-applies the settings, once. `ToLibrarySettings()` sets
the seven fields (the four clamped and finite-tested; `v3Guard` mapped to `framegen::V3Guard`); the `setSettings()` that `SyncUiHistory()` derives from
`H.interp.settings()` carries them along.

## Rows hidden or greyed under V3

Under V3 (the effective version: Motion blur's own one when it is on with its own settings), only
rows that drive nothing V3 uses are left out, per the library's table of ignored settings. Since
2026-11-20 the four search rows are not shown (they are Expert settings rows, hidden unless that
switch is on) while the Motion guard is Off: with the guard on they drive its V2 estimate
(`V3GuardOn()` in `PanelFrameGen.cpp`, effective guard). The table's "hidden" is the shown rule
(`ShowRadiusRows()` / `ShowPenaltyRows()`, Expert on); a shown row is still greyed while Frame
generation and Motion blur are off:

| Row | Library fields it sets | Under V3 |
|---|---|---|
| Search radius, Search radius (coarsest level) | `searchRadius`, `searchRadiusTop` | hidden with the guard Off, shown with it on (`ShowRadiusRows()`; never under Learned) |
| Pyramid smallest level | `pyramidMinSide` | hidden with the guard Off (`ShowPenaltyRows()`): V3 builds its own pyramid; a change would still rebuild the shared buffers without changing V3's picture. With the guard on it sets the pyramid of the guard's V2 search (structural: `reconfigure()`, as everywhere) |
| Search penalty | `searchPenalty` | hidden with the guard Off, shown with it on (`ShowPenaltyRows()`) |
| Static HUD protection | `hudBonus` (0 / 1.0 / 2.5) | **enabled**: V3 honours it since library `c9c6718`, as V1 does (see "Static HUD protection under V3") |
| Artifact safety | `trustLow` / `trustHigh` / `sceneCutSad` **and `globalFallback`** | **left enabled**: `globalFallback` (Off sets it to 1.0) is V3's "estimate failed" threshold |
| Quality | `flowScale` (and `subpixel`, `v2Mode`) | enabled: V3 honours `flowScale` |
| Limit to GPU speed, UI protection, Crosshair box size, Motion blur rows | pacing / `ui*` / `blur*` | enabled: V3 honours them |
| Learned rows | `learned*` | shown only under Learned (and Expert settings on) |

There is no row for `v2StaticMask` (the library default, honoured by V3), `smoothBonus`
(ignored) or `subpixel` alone. `Why only these:` the brief asked to leave out only what is cheap with a
`DisabledUnless` predicate and to keep a row enabled if anything it sets is honoured; Artifact
safety is the case where that matters. Its help text carries one sentence for V3 (no mismatch
ranges there; only Off matters).

## Motion guard

(2026-11-20, library `f301d7d`.) One more V3 setting, `v3_guard` (`framegen::Settings::v3Guard`): a Choice
row **Motion guard** in the group "Motion version 3", after the two switches, segmented, Off / Guard / V2 motion,
default Off, usable while V3 runs (`V3RowsUsable()`), changeable while a game runs. With **Off** nothing changes
(the library's output is byte-identical to V3 without it; no pass, no memory).

- **Guard**: V2's motion search also runs on every real pair and replaces V3's vector (both directions) where V2's
  explains the two frames clearly better and differs by more than 2.5 flow pixels; V3 keeps its own motion elsewhere.
  It never replaces a vector V3 holds still, so Static HUD protection stays in force. About +0.34 ms per real
  pair and 14 MiB.
- **V2 motion**: V2's vectors everywhere, V3's occlusion handling, fill and picture generation on top. About +0.31 ms
  per real pair. No HUD exemption; it gives up V3's advantage on fast motion (looks like V2 there).
- Quality, measured in the library on nine real pairs (1080p, `flowScale` 2): mean quality Guard 28.19 dB, the same as
  Off; V2 motion 27.55 dB. Guard replaces 1 to 12 % of the vectors per pair. A gun held in front of a moving
  background improves (error 20.97 to 16.94 levels), a frame edge improves (6.40 to 5.38), a helmet edge partly
  (51.18 to 42.29); a radar barely changes and a minimap gets slightly worse (20.37 to 22.85). V2 motion
  repairs the gun more (11.32) but its fast pairs fall to V2's level.
- Cost (RX 7900 XTX, 1080p, `flowScale` 2, 2x, per real pair): Off 1.02 ms (Rgba8) / 1.13 ms (F16 linear), Guard
  1.36 / 1.46, V2 motion 1.33 / 1.43.

**Search rows.** While the guard is on, `searchRadius`, `searchRadiusTop`, `pyramidMinSide`, `subpixel` and
`searchPenalty` act on the guard's V2 estimate exactly as under V2 (V3 itself still ignores them). They are passed to the
library under V3 whatever the guard (`ToLibrarySettings()` sets them for every version), and the rows are
usable under V3 only while the guard is on (see the table above). A pyramid change goes the usual structural route
(`reconfigure()`, GPU wait).

**Memory, refusal, descriptors.** The first time the guard is on, `setSettings()` creates V2's estimate pipelines and
two buffers (14.06 MiB at 1080p `flowScale` 2). This host never calls `releaseUnusedMotion()` (see the note in
`EnsureReady()`), so they stay until a resize or teardown. A device that cannot run V2 is refused by
`settingsError()` with V2's reason: `ResolveMotion()` then runs V1 with that reason in the Status line and the log,
like any refused version; a later allocation failure inside `setSettings()` keeps the previous settings and sets
the note "the library could not create the Motion guard (V2's pipelines or buffers)". The host records one estimate
per `beginFrame()`, so the guard's two descriptor sets per estimate (at most 16 estimates per `beginFrame()`) are
never near the limit. The guard's value is part of the V3 word's comparison, so a change reaches `setSettings()`
once.

`Why:` the user found V3's large artifacts worse than V2's and proposed V2's motion detection with V3's picture
generation. The library implements exactly that as a setting; it is an experiment with three values to compare by eye,
so the default stays Off.

## Static HUD protection under V3

Since library `c9c6718` V3 honours `hudBonus`; the row works under V3 exactly as under V1 (Off / Normal /
Strong = 0 / 1.0 / 2.5, `ToLibrarySettings()` sets it for every version, and the level is part of the
applied-config key, so a change or a live version switch re-applies it with `setSettings()`). The bonus
acts only where the motion it would undo is a real one (about 4 px per frame pair and up) and both frames
show the same detail at the same place, so slow motion never freezes at any level.

- **Off** is byte-identical to V3 before this pin. **Normal** is nearly neutral on real footage (mean
  quality over nine test pairs unchanged) and steadies small fixed elements such as a ping readout or an
  ammo panel. **Strong** also steadies weapon lists and costs one of nine test pairs about 0.3 dB.
- It does not fix a radar or minimap whose content moves (those defects come from the fast
  background's motion estimate); a radar gets slightly worse at Strong.
- This project's default is Strong, so V3 runs with `hudBonus` 2.5 by default from this pin on and its
  default picture changes slightly (Strong steadies weapon lists, costs one of nine test pairs about
  0.3 dB, and a radar gets slightly worse); Normal is the nearly neutral level. Intended; the default is
  unchanged for every version.

## Build

`src/meson.build`'s FRAME GENERATION block compiles, with the same glslang flags as the rest:

- **14 plain compiles** (no define; 16 with the motion guard's two, below), added to the plain list: `v3_synth`, `v3_planes`, `v3_pyr`,
  `v3_hist`, `v3_search`, `v3_search_top`, `v3_dense`, `v3_wide`, `v3_stats`, `v3_occl`,
  `v3_fill`, `v3_project`, `v3_enclose`, `v3_decide` -> `v3_<n>_spv.h`, variable `v3_<n>_spv`.
  `framegen.cpp` includes them unconditionally, so the build fails without them.
- **Per colour format**: `v3_planes` and `v3_synth` are added to `framegen_colour_shaders`, compiled
  with `-DFG_FORMAT_RGB10=1` and `-DFG_FORMAT_F16=1` -> `v3_planes_<fmt>_spv.h`,
  `v3_synth_<fmt>_spv.h` (4 more compiles). The library finds them through `__has_include`. NV12 /
  P010 stay unbuilt (see [limits](frame-generation-limits.md#build)); without a format's variants
  the library refuses V3 for it with a reason and the host falls back to V1.
- **The two switches' builds** (library `acab885`, second compiles of existing sources with one
  more define): `v3_search`, `v3_search_top`, `v3_dense` with `-DV3J=1` -> `v3_search_j_spv.h`,
  `v3_search_top_j_spv.h`, `v3_dense_j_spv.h`; `v3_synth` with `-DV3L=1` -> `v3_synth_l_spv.h`;
  and per format (`rgb10`, `f16`) `v3_synth` with `-DFG_FORMAT_<FMT>=1 -DV3L=1` ->
  `v3_synth_l_<fmt>_spv.h` (variable `<name>_spv`). The first four are mandatory (the library
  includes them unconditionally); a format without its `v3_synth_l` variant has V3 refused.
- **The motion guard's two shaders** (library `f301d7d`): plain compiles in the same list, no define, no
  per-format variants (they work on the motion field): `v3_anchor`, `v3_guard` -> `v3_anchor_spv.h`,
  `v3_guard_spv.h`. Mandatory (the library includes them unconditionally).
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

Measured in the library (RX 7900 XTX, 1080p, flowScale 2, per real pair, back to back, fast pair),
both switches off / colour window on / detail split on / both on, in ms:

| Format | 2x | 3x | 4x |
|---|---|---|---|
| Rgba8 | 0.976 / 1.023 / 1.035 / 1.077 | 1.196 / 1.242 / 1.314 / 1.356 | 1.425 / 1.468 / 1.593 / 1.652 |
| F16 linear (worst) | 1.068 / 1.131 / 1.145 / 1.210 | 1.326 / 1.400 / 1.488 / 1.552 | 1.581 / 1.665 / 1.819 / 1.893 |

The Motion guard adds about +0.34 ms (Guard) or +0.31 ms (V2 motion) per real pair and 14 MiB, see "Motion guard".

Static HUD protection adds about 0.02 ms per real frame pair at Normal and Strong and 0.006 ms at Off (Rgba8 2x: 1.00 to 1.02 ms; F16 linear: 1.10 to 1.12 ms).

(Before the library's picture tuning, Rgba8 off was 0.89 / 1.07 / 1.26.) Earlier measurements,
before the tuning: flowScale 4 0.44 ms; flowScale 1 2.84 ms; 1440p flowScale 2 1.55 ms. V1 is about 0.30 ms, V2 Quality 0.27. Memory about 56 MiB at 1080p flowScale 2 (224 MiB at
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

## Scene cut default: Never

Since 2026-11-19 (library `d61951c`) `v3_scene_cut` defaults to 0 = Never, the library's own default
too. `Why:` on fast motion that is not a cut the cut decision fired and V3 showed the previous real
frame unchanged, up to five generated frames in a row on a fast pan at a 30 fps-like base, which looks
like lag; the user: "scene cuts arent as important as fluid gameplay". With 0 a real cut shows one
cross-faded frame. The same pin adds a frame-edge band to the synthesis and a projection fix.

Saved profiles: the row existed for hours with 0.45 as its default, so profiles saved in that window hold
`"v3_scene_cut": 0.45` explicitly. On load a value of 0.45 (compared in hundredths, not as a float) is
read as 0. Deliberate simplification: exactly 0.45 can no longer be kept (0.40 and 0.50 can); remove the
mapping once no profile from 2026-11-19 is around. No schema version change.

## Known limits

From the library: the static-structure mask is luma-based; fast pans at a 30 fps base look soft
(the smear); a scene cut between two scenes with equal level statistics is not seen (the histogram
distance stays low). `v3SceneCut` is read by the estimate, so a change applies from the next real
pair.
