# Frame generation: V3 Hybrid

Part of [Frame generation](frame-generation.md). An experimental entry in the **Motion version**
list (2026-11-23): [Motion version 3](frame-generation-motion-v3.md) with its motion search
unchanged, but the hand-written rule that mixes V3's colour samples per pixel replaced by a small
learned blend (a tiny per-pixel network). Two variants, **Fluidity** and **Consistency** (since
2026-11-24, library pin `d9ee7c7`), so the user can compare them with each other and with plain V3
while a game runs.

| Variant | `v3Blend` | Stored string | Library constant | Character |
|---|---|---|---|---|
| (none, plain V3 colour rule) | 0 | | | V3 as before |
| **Fluidity** (default) | 1 | `fluidity` | `framegen::kV3BlendFluidity` | sharper, can show double contours at edges |
| **Consistency** | 2 | `consistency` | `framegen::kV3BlendConsistency` | fewer double contours at edges, a little softer |

`Why two:` the library carried fifteen trained variants (2026-11-23 .. 2026-11-24) so the owner could
rate them in a game; they kept the two that looked best and named them: *"visually the best models
are A-C, AR4-C ... call AR4-C something like Consistency and A-C something like Fluidity"*
(2026-11-24). The library did the same cut and proved the new 1 / 2 give byte-identical frames to the
old 6 / 15.

### Saved profiles of the fifteen-variant period

Until 2026-11-24 `framegen.v3_hybrid_variant` held one of fifteen strings (`a`, `b`, `c`, `a_a` ..
`a_c`, `ar_a` .. `ar_c`, `ar3_a` .. `ar3_c`, `ar4_a` .. `ar4_c`; default `a`). The mapping on load
(`fghost::HybridKeyFromStored()`, pure, next to the table, pinned by `tests/test_framegen_v3.cpp`):

| Stored | Loads as |
|---|---|
| `a_c` | `fluidity` |
| `ar4_c` | `consistency` |
| any other string (the thirteen other old ones such as `a`, `b`, `ar3_b`, `ar2_b`; unknown, empty or wrong-typed) | `fluidity` (the default, through the loader's existing unknown-string rule) |

After the load the value in memory is the **new** string, so the next save writes `fluidity` /
`consistency` (`tests/test_config.cpp` loads each case, saves and reads the file back).

### What to expect

The library's own measurement, not ours: the nets gain a little (tenths of a dB) over V3's rule on
whole pictures against real frames, and cost about 0.06 ms more per real pair than the rule at 1080p,
2x (0.88 ms rule, 0.94-0.95 ms with a net, 8-bit; 4x about 1.3 vs 1.46-1.51 ms). The difference to the
rule is subtle; `blend=Fluidity` in the status shows the blend really runs.

## Config and mapping

| Key | Values | Meaning |
|---|---|---|
| `framegen.motion_version` | `v1` / `v2` / `learned` / `v3` / **`v3_hybrid`** | `v3_hybrid` is the fifth value, after `v3` |
| `framegen.v3_hybrid_variant` | `"fluidity"` (default) / `"consistency"`; absent, wrong type or unknown loads `"fluidity"` (old strings: see above) | which variant (order as the table = index 0..1), read only while `motion_version` is `v3_hybrid` |

An old file without the key and with `"v3"` loads as before (V3 stays V3, no blend).
`motion_blur.motion_version` does **not** get the value: Motion blur's own list is unchanged and
`"v3_hybrid"` there is rejected like any unknown string (it keeps `v1`).

Config to host (`fghost::MotionFromKeys()` in `FrameGenV3.h`, pinned by `tests/test_framegen_v3.cpp`):

| `motion_version` | variant | `fghost::Motion` | `Config::v3Blend` |
|---|---|---|---|
| `v3` | any | `V3` | 0 |
| `v3_hybrid` | `fluidity` / `consistency` (other: Fluidity) | `V3` | 1 / 2 |
| `v1` / `v2` / `learned` | any | `V1` / `V2` / `Learned` | 0 |

`Why the host enum is unchanged:` `fghost::Motion` is packed into two bits of the main config word
(bits 28-29) and two of the blur word, so a fifth value does not fit. Instead V3 Hybrid is
`Motion::V3` plus a small separate value `Config::v3Blend` (0 = off, 1 / 2 = the variants), packed
into bits 52-55 of the V3 word (`PackV3()`; four bits, so the layout did not change when the range
shrank; bits 56-63 stay unused; the host writes 0 for anything outside 1..2). Every existing
`Motion::V3` check (rows shown, fallbacks, scene-cut default, HUD protection, cost) therefore applies
to V3 Hybrid without touching it. The config-string checks in the overlay panel
(`EffectiveMotionIsV3()`, which feeds the Expert rule, `V3GuardOn()` and the V3 rows) count
`"v3_hybrid"` as V3 too. Settings export / import work on the section's leaves, so both keys travel
with the Frame generation group without a change.

## The variant table

`src/FrameGen/HybridVariants.h` (pure; it cannot include the library's header, which pulls in the
Vulkan headers that Config and tests need not have): `kHybridVariants`, a plain array of
`{ label, stored string, v3Blend }`, two entries, and `kHybridBlendMax` (its size). Small functions
on it: `HybridLabel( blend )`, `HybridKey( blend )` ("" outside 1..2), `HybridBlendFromKey( key )`
(0 when unknown) and `HybridKeyFromStored( stored )` (the legacy mapping above). Everything that
prints a name uses `HybridLabel()`: the log line `learned blend <name>`, the status text
`blend=<name>`, the capture header token and the stats overlay.

`Why a check in the host:` `FrameGenHost.cpp` holds `static_assert`s that the table's values equal
`framegen::kV3BlendFluidity` / `kV3BlendConsistency` and its count equals `framegen::kV3BlendMax`. A
pin that renumbers, adds or drops a blend then fails the build there with a message (add or remove a
table row) instead of silently showing the wrong names.

## Live change

A blend change (Fluidity to Consistency, or V3 to V3 Hybrid) is part of the V3 word, so it reaches
the library through the ordinary runtime `setSettings()` in `EnsureReady()` at the next paint. The
library motion version is the same (V3), so nothing is torn down, no GPU wait happens and the
estimate is kept (the library applies the blend at the next synthesis without a new estimate). The
host logs `frame generation: Motion version 3 learned blend Fluidity|Consistency|off` once per
change. If V3 falls back to V1 (unsupported GPU or format) the blend is ignored; V3's own fallback
text applies.

Motion blur: with "Use Frame generation's settings" on it follows what frame generation runs, so it
runs the blend too (and its row shows "V3"). With the switch off and the blur on, its own V3 is
plain V3: `RecordBaseLayer()` zeroes the blend then (its V3 settings otherwise come from Frame
generation's rows, as for every V3 value).

## Overlay

- **Motion version** gets a fifth option **V3 Hybrid**, listed right after V3.
- **Hybrid variant** (`image.framegen.v3_hybrid_variant`, a two-option Choice Fluidity / Consistency)
  sits directly under it, shown only while V3 Hybrid is the selected version, and **without** Expert
  settings. It is greyed while frame generation is off. Drawn as **segmented buttons**: a plain
  Choice picks segmented vs dropdown by measurement and by the 8-character label rule
  ([styling guide](../ui/STYLING-GUIDE.md): "Segmented vs dropdown"), "Consistency" is 11
  characters, so the row says `.Segmented()` (which lifts only that rule; the fit test stays, a
  too-narrow lane would still fall back to a dropdown), as the other rows with slightly longer
  labels do. `Why:` the user wants to compare the variants live (2026-11-23), so it is not a tuning
  row; once one wins the row goes away.
- Every V3 row acts under V3 Hybrid exactly as under V3, including the Expert rule (tuning rows
  only with Expert on; defaults act with it off; the Motion guard is no Expert row). The variant is
  not one of the 14 tuning values, so `config::FrameGenEffective()` never resets it.

From the command line (the same binding a click writes through; the value is the option's id, as
for every Choice): `gamescopectl overlay_e2_set "framegen.motion_version 3"` (V3),
`"framegen.motion_version 4"` (V3 Hybrid), `"framegen.v3_hybrid_variant 0"` (Fluidity) / `1`
(Consistency). The variant row is addressable only while V3 Hybrid is selected (hidden rows do not
exist for this command).

## Where it shows

The status shows what really runs, not just what is selected (2026-11-23):

| State | Status line mode text | Capture / watch header | One-time log line |
|---|---|---|---|
| Plain V3 (or another version) | nothing extra | no token | `learned blend off` (only on a change from a variant) |
| Variant selected, blend in use | `blend=Fluidity` (or `Consistency`) | ` blend=Fluidity` | `learned blend Fluidity` |
| Variant selected, library runs V3's rule | `blend=Fluidity(off)` | ` blend=Fluidity(off)` | `learned blend Fluidity not active: format` / `: detail split` / (no reason when not known cheaply, e.g. V3 itself fell back) |

The status line (`steamcompmgr.cpp`, `FrameGen_LogStatus()`) prints a new line when the variant or
its active state changes. The header token follows `fallback_v1=` (only for a variant), formatted by
`BlendToken()` in `FrameGenCapture.cpp`; the status text by `fghost::BlendLabel()` in `FrameGenV3.h`
(pinned by `tests/test_framegen_v3.cpp` and `tests/test_framegen_capture.cpp`).

How the fact travels: the render thread owns the interpolator, so `PublishBlend()` in
`FrameGenHost.cpp` asks `Interpolator::v3BlendActive()` at the end of every `EnsureReady()` (after
init, resize, a settings change, a rebuild for a changed game format) and stores it in
`g_bBlendActive` / `g_pszBlendWhy` (atomics, like `g_pszMotionNote`); the compositor thread reads
them through `fghost::BlendActive()` / `BlendWhy()`. The capture start, which runs on the render
thread, asks the interpolator directly (`Env::bBlendActive`). `Why:` the library falls back to V3's
rule without an error (no blend build for the format, detail split on, V3 itself fell back), so the
selection alone said nothing about what ran. The setting dump of a capture lists the library's
fields and carries `v3Blend` (`FormatSettingLines()`, `setting v3Blend=N`).

## Library contract (pin `d9ee7c7`)

`framegen::Settings::v3Blend`: `uint8_t`, directly after `v3DetailSplit` at offset 142 (existing
padding, so `sizeof(Settings)` stays 164 and the `static_assert` in `FrameGenCapture.cpp` is
unchanged). Values 0..`kV3BlendMax` (2): 0 is the rule (byte-identical to V1, V2, Learned and plain
V3 without a blend), 1 = `kV3BlendFluidity`, 2 = `kV3BlendConsistency`. The library refuses a larger
value in `settingsError` (init and `setSettings()` fail). The values were 0..15 (and 0..6 before)
until `d9ee7c7`; the old 6 is the new 1, the old 15 the new 2.

The library runs its blend synthesis pipeline (`P_V3_SYNTH_B`) when `v3Blend != 0`, the format is
Rgba8 / Bgra8 or Rgb10 and `v3DetailSplit` is off; otherwise V3's rule runs, without an error (f16,
NV12 and P010 have no blend build). It takes effect at the next synthesis, with no new estimate.
`bool Interpolator::v3BlendActive() const` says whether it runs: true exactly when the object is
initialised, the motion version is V3, `v3Blend != 0`, the detail split is off and the format has a
blend build.

`Why the 10-bit build exists:` a 10-bit SDR output makes the game's frames 10-bit (found in the
field 2026-11-23: frame generation ran in the 10-bit format, and the 8-bit-only blend silently ran
V3's rule).

The host sets it in `ToLibrarySettings()` (`s.v3Blend = uint8_t(c.v3Blend)` for 1..`kHybridBlendMax`,
else 0). It is part of the V3 word of the applied-config key, so a change goes through
`setSettings()` (runtime), never `reconfigure()`: no rebuild, no "frame generation ready" again.

**Build**: `src/meson.build` compiles `v3_synth.comp` with `-DV3B=1` into `v3_synth_b_spv.h`
(variable `v3_synth_b_spv`, mandatory: `framegen.cpp` includes it unconditionally) and with
`-DFG_FORMAT_RGB10=1 -DV3B=1` into `v3_synth_b_rgb10_spv.h` (variable `v3_synth_b_rgb10_spv`; rgb10
only). Both include `shaders/v3_blend.glsl` and `shaders/v3_blend_weights.glsl`; both are listed by
the compiler's depfile like every other include, so a changed weights file rebuilds the headers. A
weights-only pin needs nothing but the new gitlink (no shader was added, removed or renamed at
`d9ee7c7`; the two learned-blend synth headers recompile).

`The __has_include trap:` the library's `framegen.cpp` includes `v3_synth_b_rgb10_spv.h` only
behind `__has_include` and then defines `FG_HAVE_V3B_RGB10`. A build without our meson entry still
compiles, and 10-bit silently runs V3's rule. So the entry is not optional, and the status token
above is what shows a miss: a 10-bit game with the variant selected would read
`blend=Fluidity(off)`. Check a new pin by running a 10-bit client under a nested instance (`format
10-bit` in the `frame generation ready` line) and reading `blend=Fluidity` (not `(off)`).

**One-time stall:** the learned-blend pipelines are created when the interpolator is created (every
V3 use), so after a shader change the first start compiles them on a cold driver cache: the library
measured about 1.2 s with two nets (12.6 s with fifteen). Later starts hit the cache.

**Detail split**: while "Detail from one frame" is on, the Hybrid variant has no effect (library
fallback); the row's help says so ("Has no effect while Detail from one frame is on.").

## Tests

`tests/test_config.cpp` (round trip, absent key, wrong-typed value, every old stored string loading
its successor and the next save writing the new one, Motion blur rejecting the value, Expert not
resetting the variant), `tests/test_framegen_v3.cpp` (the packed bits and "0 outside 1..2", the
table, the legacy mapping, `MotionFromKeys()`, `BlendLabel()` active / not active / plain),
`tests/test_framegen_capture.cpp` (header text, `blend=Consistency(off)`), `tests/test_fps_counter.cpp`
(stats text). `v3BlendActive()` itself needs a GPU (the library's `.cpp` is not linked into the unit
tests): it is checked in a headless nested run with an 8-bit client and with a 10-bit
(`A2B10G10R10`, sRGB) client.
