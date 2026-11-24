# Frame generation: V3 Hybrid

Part of [Frame generation](frame-generation.md). An experimental entry in the **Motion version**
list (2026-11-23): [Motion version 3](frame-generation-motion-v3.md) with its motion search
unchanged, but the hand-written rule that mixes V3's colour samples per pixel replaced by a small
learned blend (a tiny per-pixel network). Six variants exist so the user can compare them with
each other and with plain V3 while a game runs: **A / B / C**, and **A-A / A-B / A-C**, a second set
of weights for the same three nets, fine-tuned by a contributor on other footage (2026-11-23).
`Why:` the user asked for the second set as "A-<Model>" (A-A, A-B, A-C) next to A / B / C so the two
sets can be compared directly (2026-11-23). The second set needs a library pin that accepts `v3Blend` 4..6 (`1e68c8d` and newer);
pins before `1e68c8d` refuse those values.

> **Second weight set (library `1e68c8d`, 2026-11-23).** `v3Blend` 4 / 5 / 6 = the nets A / B / C with a
> second set of weights, fine-tuned by a contributor on other footage. The library's own numbers: `v3Blend`
> 0..3 are byte-identical to the pin before; 4..6 match its reference to 1e-6 in 8-bit and 10-bit; on one
> frame pair of a racing-game recording A-A differs from A on about 5 % of the pixels and A-C from C on
> about 14 %. Cost at 1080p, 2x (library measurement): 0.90 ms (rule), 0.95 / 0.96 ms (A / C), 0.96 /
> 0.97 ms (A-A / A-C). Under `gpu/` only the weights, the two blend shader sources (the variant is read from
> three synth mode bits instead of two), the settings validation and comments changed; our build needed
> nothing but the gitlink.

> **The weights are trained (first round, 2026-11-23).** Library `19b15ef` carries the trained weights of
> the three nets, in `v3_blend_weights.glsl` only (no header, struct, build entry or API changed against
> `34d8bc1`; before that the weights were zero).

### What to expect

The library's own measurement, not ours: on sampled pixels of a racing-game recording the nets gain
0.03-0.05 dB over V3's rule. Against the rule's picture, A and B change about 1 % of the pixels by more
than 2 levels on an easy frame pair and about 6-8 % on a hard one (mean change 0.2-1.0 levels). C changes
a little more (it also mixes in the two unwarped frames). The shader weights equal the library's
reference to 1.1e-6 in 8-bit and 10-bit.

`Why it matters:` this first round came out close to V3's rule, so a user comparing A / B / C against
plain V3 should expect small differences: A and B look almost like V3, C is the most likely to differ.
That is the training result, not a wiring fault (`blend=A` in the status shows the blend really runs).

Final result of the round (library, 2026-11-23): on whole pictures against real frames A and B score
+0.0 to +0.2 dB over the rule, C is ahead on six of seven clips by up to 0.3 dB. The library's diagnosis:
the per-pixel inputs are used up. A next step would change what the net sees (neighbourhood, extra
candidate motions) and arrive as a new `v3Blend` variant, not as a change to A / B / C.

## Config and mapping

| Key | Values | Meaning |
|---|---|---|
| `framegen.motion_version` | `v1` / `v2` / `learned` / `v3` / **`v3_hybrid`** | `v3_hybrid` is the fifth value, after `v3` |
| `framegen.v3_hybrid_variant` | `"a"` (default) / `"b"` / `"c"` / `"a_a"` / `"a_b"` / `"a_c"`; absent, wrong type or unknown loads `"a"` | which variant (order A, B, C, A-A, A-B, A-C = index 0..5), read only while `motion_version` is `v3_hybrid` |

An old file without the new key and with `"v3"` loads as before (V3 stays V3, no blend).
`motion_blur.motion_version` does **not** get the value: Motion blur's own list is unchanged and
`"v3_hybrid"` there is rejected like any unknown string (it keeps `v1`).

Config to host (`fghost::MotionFromKeys()` in `FrameGenV3.h`, pinned by `tests/test_framegen_v3.cpp`):

| `motion_version` | variant | `fghost::Motion` | `Config::v3Blend` |
|---|---|---|---|
| `v3` | any | `V3` | 0 |
| `v3_hybrid` | `a` / `b` / `c` / `a_a` / `a_b` / `a_c` (other: A) | `V3` | 1 / 2 / 3 / 4 / 5 / 6 |
| `v1` / `v2` / `learned` | any | `V1` / `V2` / `Learned` | 0 |

`Why the host enum is unchanged:` `fghost::Motion` is packed into two bits of the main config word
(bits 28-29) and two of the blur word, so a fifth value does not fit. Instead V3 Hybrid is
`Motion::V3` plus a small separate value `Config::v3Blend` (0 = off, 1..3 = A..C, 4..6 = A-A..A-C), packed into
bits 52-55 of the V3 word (`PackV3()`; two bits until 2026-11-23, three until the variant table, see below; 4..6 did not
fit two, bit 54 was unused so no other field moved; unpacking a 7 gives 0). Every existing `Motion::V3` check (rows shown, fallbacks,
scene-cut default, HUD protection, cost) therefore applies to V3 Hybrid without touching it. The
config-string checks in the overlay panel (`EffectiveMotionIsV3()`, which feeds the Expert rule,
`V3GuardOn()` and the V3 rows) count `"v3_hybrid"` as V3 too. Settings export / import work on the
section's leaves, so both keys travel with the Frame generation group without a change.

## How the variant list is built

One table, `kHybridSets` in `src/FrameGen/HybridVariants.h` (pure, no host includes, so Config, the
overlay panel and the host all use it): per weight set a label prefix and a stored-string prefix, five
sets named (`""`/`""`, `A-`/`a_`, `AR-`/`ar_`, `AR2-`/`ar2_`, `AR3-`/`ar3_`). The library value is
`v3Blend = 1 + 3 * set + net` (net 0 / 1 / 2 = A / B / C), so the stored strings are `a`, `b`, `c`, `a_a`
.. `ar3_c` and the labels A, B, C, A-A .. AR3-C (15 values). Two constants: `kHybridSetsTotal` = 5 (the
table's size) and `kHybridSetsActive` = 2 (how many sets exist at the current library pin). The active
count decides the options of the Hybrid variant row, which stored strings the config accepts (a string of
a set beyond it, an unknown one or a wrong type loads `"a"`), how far `PackV3()` writes (`kHybridBlendMax`
= 3 x active; anything else packs as 0) and the host's clamp in `FrameGenHost.cpp`. `BlendLabel()` /
`BlendText()` and everything built on them name all 15 values whatever the active count.
To offer more sets once the pin ships them: change `kHybridSetsActive` in `HybridVariants.h` (and bump
the pin); tests that name the count follow it.

`Why a table:` each new weight set used to mean edits in six places (config list, row options, key
array, label array, pack bound, clamp); now the count is one constant.

V3 word bits: `v3Blend` has bits 52-55 (four bits, 0..15; it had 52-54 for 0..6). Bits 55-63 were unused before,
so no other field moved; bits 56-63 stay unused. `PackV3()` / `UnpackV3()` take the bound as a parameter
(default `kHybridBlendMax`) so `tests/test_framegen_v3.cpp` covers the 15-value case now.

## Live change

A blend change (variant A to B, or V3 to V3 Hybrid) is part of the V3 word, so it reaches the
library through the ordinary runtime `setSettings()` in `EnsureReady()` at the next paint. The
library motion version is the same (V3), so nothing is torn down, no GPU wait happens and the
estimate is kept (the library applies the blend at the next synthesis without a new estimate). The
host logs `frame generation: Motion version 3 learned blend A|B|C|off` once per change. If V3 falls
back to V1 (unsupported GPU or format) the blend is ignored; V3's own fallback text applies.

Motion blur: with "Use Frame generation's settings" on it follows what frame generation runs, so it
runs the blend too (and its row shows "V3"). With the switch off and the blur on, its own V3 is
plain V3: `RecordBaseLayer()` zeroes the blend then (its V3 settings otherwise come from Frame
generation's rows, as for every V3 value).

## Overlay

- **Motion version** gets a fifth option **V3 Hybrid**, listed right after V3.
- **Hybrid variant** (`image.framegen.v3_hybrid_variant`, a six-option Choice A / B / C / A-A / A-B / A-C, drawn as a dropdown: the styling guide's 5-option cap on segmented groups applies) sits directly
  under it, shown only while V3 Hybrid is the selected version, and **without** Expert settings.
  It is greyed while frame generation is off. `Why:` the user wants to compare three variants live
  (2026-11-23), so it is not a tuning row; once one net wins the row goes away.
- Every V3 row acts under V3 Hybrid exactly as under V3, including the Expert rule (tuning rows
  only with Expert on; defaults act with it off; the Motion guard is no Expert row). The variant is not one of the 14 tuning values,
  so `config::FrameGenEffective()` never resets it.

From the command line (the same binding a click writes through; the value is the option's id, as
for every Choice): `gamescopectl overlay_e2_set "framegen.motion_version 3"` (V3),
`"framegen.motion_version 4"` (V3 Hybrid), `"framegen.v3_hybrid_variant 0"` .. `5` (A, B, C, A-A, A-B, A-C).
The variant row is addressable only while V3 Hybrid is selected (hidden rows do not exist for this
command).

## Where it shows

The status shows what really runs, not just what is selected (2026-11-23):

| State | Status line mode text | Capture / watch header | One-time log line |
|---|---|---|---|
| Plain V3 (or another version) | nothing extra | no token | `learned blend off` (only on a change from a variant) |
| Variant selected, blend in use | `blend=A` (or `A-A` .. `A-C`) | ` blend=A` | `learned blend A` |
| Variant selected, library runs V3's rule | `blend=A(off)` (likewise `A-A(off)`) | ` blend=A(off)` | `learned blend A not active: format` / `: detail split` / (no reason when not known cheaply, e.g. V3 itself fell back) |

The status line (`steamcompmgr.cpp`, `FrameGen_LogStatus()`) prints a new line when the variant or its
active state changes. The header token follows `fallback_v1=` (only for a variant), formatted by
`BlendToken()` in `FrameGenCapture.cpp`; the status text by `fghost::BlendLabel()` in `FrameGenV3.h`
(pinned by `tests/test_framegen_v3.cpp` and `tests/test_framegen_capture.cpp`).

How the fact travels: the render thread owns the interpolator, so `PublishBlend()` in `FrameGenHost.cpp`
asks `Interpolator::v3BlendActive()` at the end of every `EnsureReady()` (after init, resize, a settings
change, a rebuild for a changed game format) and stores it in `g_bBlendActive` / `g_pszBlendWhy`
(atomics, like `g_pszMotionNote`); the compositor thread reads them through `fghost::BlendActive()` /
`BlendWhy()`. The capture start, which runs on the render thread, asks the interpolator directly
(`Env::bBlendActive`). `Why:` the library falls back to V3's rule without an error (no blend build for the
format, detail split on, V3 itself fell back), so the selection alone said nothing about what ran. The setting dump of a capture lists the library's fields and
carries `v3Blend` (`FormatSettingLines()`, `setting v3Blend=N`).

## Library contract (pin `1e68c8d`)

`framegen::Settings::v3Blend`: `uint8_t`, directly after `v3DetailSplit` at offset 142 (existing
padding, so `sizeof(Settings)` stays 164 and the `static_assert` in `FrameGenCapture.cpp` is
unchanged). Values 0..6: 0 is the rule (byte-identical to the pin before, as are V1, V2 and Learned);
1 / 2 / 3 are nets A / B / C and 4 / 5 / 6 the same three with the second weight set (same structure, cost, formats and fallbacks). The library refuses a value above 6 in `settingsError` (init and `setSettings()` fail).

The library runs its blend synthesis pipeline (`P_V3_SYNTH_B`) when `v3Blend != 0`, the format is
Rgba8 / Bgra8 or Rgb10 and `v3DetailSplit` is off; otherwise V3's rule runs, without an error (f16, NV12
and P010 have no blend build). It takes effect at the next synthesis, with no new estimate.
`bool Interpolator::v3BlendActive() const` says whether it runs: true exactly when the object is
initialised, the motion version is V3, `v3Blend != 0`, the detail split is off and the format has a blend
build. Cost at 1080p per real pair (the library's measurement, one high-end desktop GPU, blend off / A / B / C):

| Case | off | A | B | C |
|---|---|---|---|---|
| 2x, 8-bit | 0.881 ms | 0.941 | 0.942 | 0.952 |
| 2x, 10-bit | 0.901 ms | 0.954 | 0.956 | 0.969 |
| 4x, 8-bit | 1.302 ms | 1.457 | 1.458 | 1.506 |

`Why the 10-bit build exists:` a 10-bit SDR output makes the game's frames 10-bit (found in the field
2026-11-23: frame generation ran in the 10-bit format, and the 8-bit-only blend silently ran V3's rule).

The host sets it in `ToLibrarySettings()` (`s.v3Blend = uint8_t(c.v3Blend)`, clamped 0..6 since 2026-11-23; the library refuses 7 and above). It is part
of the V3 word of the applied-config key, so a change goes through `setSettings()` (runtime), never
`reconfigure()`: no rebuild, no "frame generation ready" again.

**Build**: `src/meson.build` compiles `v3_synth.comp` with `-DV3B=1` into `v3_synth_b_spv.h` (variable
`v3_synth_b_spv`, mandatory: `framegen.cpp` includes it unconditionally) and, since `34d8bc1`, with
`-DFG_FORMAT_RGB10=1 -DV3B=1` into `v3_synth_b_rgb10_spv.h` (variable `v3_synth_b_rgb10_spv`; rgb10 only).
Both include `shaders/v3_blend.glsl` and `shaders/v3_blend_weights.glsl`; both are listed by the compiler's
depfile like every other include, so a changed weights file rebuilds the headers. A weights-only pin
needs nothing but the new gitlink (checked at `19b15ef` and again at `1e68c8d`, where `v3_synth.comp` itself changed:
of the 28 `v3_*_spv.h` headers only these two differ in content after the rebuild).

`The __has_include trap:` the library's `framegen.cpp` includes `v3_synth_b_rgb10_spv.h` only behind
`__has_include` and then defines `FG_HAVE_V3B_RGB10`. A build without our meson entry still compiles, and
10-bit silently runs V3's rule. So the entry is not optional, and the status token above is what shows a
miss: a 10-bit game with the variant selected would read `blend=A(off)`. Check a new pin by running a
10-bit client under a nested instance (`format 10-bit` in the `frame generation ready` line) and
reading `blend=A` (not `A(off)`).

Field check (2026-11-23, pin `19b15ef`, a 10-bit SDR game at 1440x1080 generating to a 280 Hz
refresh): switching V3 -> Hybrid A -> B -> C -> V3 live, the status read `blend=A`, `blend=B`,
`blend=C` (all active), presented stayed at the refresh (273-280 fps), no error lines. The cost was
not measurable there: another job loaded the machine during the run. The picture was not judged.

**Detail split**: while "Detail from one frame" is on, the Hybrid variant has no effect (library
fallback); the row's help says so ("Has no effect while Detail from one frame is on.").

## Tests

`tests/test_config.cpp` (round trip, absent key, invalid variant, Motion blur rejecting the value,
Expert not resetting the variant), `tests/test_framegen_v3.cpp` (the packed bits, the mapping),
`tests/test_framegen_capture.cpp` (header text, `blend=C(off)`); `BlendLabel()` active / not active /
plain in `tests/test_framegen_v3.cpp`. `v3BlendActive()` itself needs a GPU (the library's `.cpp` is not
linked into the unit tests): it was checked in a headless nested smoke with an 8-bit client and with a
10-bit (`A2B10G10R10`, sRGB) client.
