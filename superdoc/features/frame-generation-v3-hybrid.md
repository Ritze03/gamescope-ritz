# Frame generation: V3 Hybrid

Part of [Frame generation](frame-generation.md). An experimental entry in the **Motion version**
list (2026-11-23): [Motion version 3](frame-generation-motion-v3.md) with its motion search
unchanged, but the hand-written rule that mixes V3's colour samples per pixel replaced by a small
learned blend (a tiny per-pixel network). Fifteen variants in five weight sets exist so the user
can compare them with each other and with plain V3 while a game runs: three nets **A / B / C** in each
of the sets below (the library's pin `aefad17` and newer; `1e68c8d` accepts `v3Blend` 1..6, `88e071e` the same).
`Why:` the user asked for the second set as "A-<Model>" (A-A, A-B, A-C) next to A / B / C so the two
sets can be compared directly (2026-11-23). On 2026-11-24 they found A-C "clearly the best model so far"
and asked for further trained sets to rate in a game; they named the first of them AR (AR-A, AR-B, AR-C).

| Set | Labels | `v3Blend` | Stored strings | What it was trained on |
|---|---|---|---|---|
| base | A, B, C | 1..3 | `a`, `b`, `c` | the library's first round (2026-11-23) |
| A- | A-A, A-B, A-C | 4..6 | `a_a`, `a_b`, `a_c` | the base set fine-tuned on other footage |
| AR | AR-A, AR-B, AR-C | 7..9 | `ar_a`, `ar_b`, `ar_c` | from scratch on the owner's recorded game footage, real frames only |
| AR3 | AR3-A, AR3-B, AR3-C | 10..12 | `ar3_a` .. `ar3_c` | the A- set trained further on the owner's real frames |
| AR4 | AR4-A, AR4-B, AR4-C | 13..15 | `ar4_a` .. `ar4_c` | the A- set trained further with a penalty on double contours at edges; its C differs most, its A and B are close to A-A and A-B |

Library `5df970e` (2026-11-24) dropped the former fourth set (AR2) and added AR4: values 1..9 are unchanged, 10..12 are now AR3 (they were 13..15), 13..15 are AR4. A stored `ar2_a` / `ar2_b` / `ar2_c` is unknown and loads as the default `a` (`HybridBlendFromKey()` returns 0 in `ConfigManager.cpp`'s load, no migration, the fallback is intended). Index 9..14 of `overlay_e2_set "framegen.v3_hybrid_variant <index>"` changed meaning on 2026-11-24.

AR, AR3 and AR4 are experimental, for comparison by eye: on held-out footage their scores are within
0.02 dB of the base set (library measurement). The nets of all sets cost the same (about 0.07 ms above
V3's rule per real pair at 1080p, 2x).

> **Nine more nets (library `aefad17`, 2026-11-24).** Against `88e071e` only the weights shader include
> (`v3_blend_weights.glsl`), `kV3BlendMax` (6 to 15), the library's docs and weight files changed; no
> interface change. `v3Blend` 1..6 give byte-identical pictures, 7..15 nine further ones, 16 is refused by
> the library's `validate`. Our build needed nothing but the gitlink (the two learned-blend synth shader
> headers recompile) and `kHybridSetsActive` 2 to 5.

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
| `framegen.v3_hybrid_variant` | `"a"` (default) / `"b"` / `"c"`, then `"a_a"` .. `"a_c"`, `"ar_a"` .. `"ar_c"`, `"ar3_a"` .. `"ar3_c"`, `"ar4_a"` .. `"ar4_c"`; absent, wrong type or unknown loads `"a"` | which variant (order as the table above = index 0..14), read only while `motion_version` is `v3_hybrid` |

An old file without the new key and with `"v3"` loads as before (V3 stays V3, no blend).
`motion_blur.motion_version` does **not** get the value: Motion blur's own list is unchanged and
`"v3_hybrid"` there is rejected like any unknown string (it keeps `v1`).

Config to host (`fghost::MotionFromKeys()` in `FrameGenV3.h`, pinned by `tests/test_framegen_v3.cpp`):

| `motion_version` | variant | `fghost::Motion` | `Config::v3Blend` |
|---|---|---|---|
| `v3` | any | `V3` | 0 |
| `v3_hybrid` | `a` .. `ar3_c`, the 15 strings above (other: A) | `V3` | 1 .. 15 |
| `v1` / `v2` / `learned` | any | `V1` / `V2` / `Learned` | 0 |

`Why the host enum is unchanged:` `fghost::Motion` is packed into two bits of the main config word
(bits 28-29) and two of the blur word, so a fifth value does not fit. Instead V3 Hybrid is
`Motion::V3` plus a small separate value `Config::v3Blend` (0 = off, 1..15 = A .. AR3-C in the table's order), packed into
bits 52-55 of the V3 word (`PackV3()`; two bits until 2026-11-23, three until the variant table, see below; 4..6 did not
fit two, bit 54 was unused so no other field moved; unpacking a 7 gives 0). Every existing `Motion::V3` check (rows shown, fallbacks,
scene-cut default, HUD protection, cost) therefore applies to V3 Hybrid without touching it. The
config-string checks in the overlay panel (`EffectiveMotionIsV3()`, which feeds the Expert rule,
`V3GuardOn()` and the V3 rows) count `"v3_hybrid"` as V3 too. Settings export / import work on the
section's leaves, so both keys travel with the Frame generation group without a change.

## How the variant list is built

One table, `kHybridSets` in `src/FrameGen/HybridVariants.h` (pure, no host includes, so Config, the
overlay panel and the host all use it): per weight set a label prefix and a stored-string prefix, five
sets named (`""`/`""`, `A-`/`a_`, `AR-`/`ar_`, `AR3-`/`ar3_`, `AR4-`/`ar4_`). The library value is
`v3Blend = 1 + 3 * set + net` (net 0 / 1 / 2 = A / B / C), so the stored strings are `a`, `b`, `c`, `a_a`
.. `ar4_c` and the labels A, B, C, A-A .. AR4-C (15 values). Two constants: `kHybridSetsTotal` = 5 (the
table's size) and `kHybridSetsActive` = 5 (how many sets exist at the current library pin). The active
count decides the options of the Hybrid variant row, which stored strings the config accepts (a string of
a set beyond it, an unknown one or a wrong type loads `"a"`), how far `PackV3()` writes (`kHybridBlendMax`
= 3 x active; anything else packs as 0) and the host's clamp in `FrameGenHost.cpp`. `BlendLabel()` /
`BlendText()` and everything built on them name all 15 values whatever the active count.
The library publishes its own bound, `framegen::kV3BlendMax` (3 x its weight sets; 6 at pin `88e071e`,
15 at `aefad17` with five sets); `validate` refuses a `v3Blend` above it. `kHybridSetsActive` stays hand-set:
`HybridVariants.h` cannot include the library header (it pulls in the Vulkan headers, which Config and
tests need not have), so `FrameGenHost.cpp` holds two `static_assert`s instead: `kV3BlendMax` is a
multiple of 3 and at least 3, and `kHybridBlendMax == min( kV3BlendMax, kHybridBlendCount )`. To offer
more sets once a pin ships them: bump the pin; the build then stops there with a message; set
`kHybridSetsActive` to `min( kV3BlendMax / 3, kHybridSetsTotal )` in `HybridVariants.h` (a library with
more than five sets also needs a table row). Tests that name the count follow it.
The capture header's `blend=` token takes its name from the same table (`BlendToken()` in
`FrameGenCapture.cpp`; it was a six-name array until the pin `88e071e`).
Headless proof at pin `aefad17` (private headless compositor, nested, 2x, 8-bit and 10-bit clients): each
of the 15 variants logs `learned blend <name>`, shows `blend=<name>` in the status line and generates
(about 2x presented); plain V3 shows no blend and switching back restores it; no settings error. Repeated at `aefad17` with all 15: every variant logged its name and showed `blend=<name>` (no `(off)`) at about 2x presented (116-135 fps for a 61 fps client) in 8-bit and 10-bit; a stored `ar3_b` came back as `AR3-B` after a restart. Only the first run after a pin bump stalled: the first use of the new weights compiled shaders for about 15 s (a cold shader cache), so a 4 s step per variant can pass before the first one generates; rerun with the cache warm.
Headless proof at pin `5df970e` (2026-11-24, same method): all 15 variants, `gamescopectl overlay_e2_set "framegen.v3_hybrid_variant <0..14>"`, logged `learned blend <name>` with the new names (AR3-A at index 9, AR4-C at index 14) and showed `blend=<name>` (no `(off)`) at about 2x presented (113-128 fps for a 61 fps client) in 8-bit and 10-bit; no `validate` or settings error. The weights header changed, so the first use compiled the learned-blend shader again: one picture stall (`max draw` about 12.7 s in 8-bit, 11.9 s in 10-bit), later variants 2 to 4 ms. A stored `ar4_c` came back as `AR4-C` after a restart; a stored `ar2_b` started with `learned blend A`.
`Why a check and not a derivation:` a header-only derivation would make every Config or test target
depend on Vulkan; a loud build failure costs one line at a pin bump and cannot be missed.

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
- **Hybrid variant** (`image.framegen.v3_hybrid_variant`, a 15-option Choice A .. AR4-C, drawn as a dropdown: the styling guide's 5-option cap on segmented groups applies; the popup shows at most 8 rows and scrolls inside, so 15 options stay usable) sits directly
  under it, shown only while V3 Hybrid is the selected version, and **without** Expert settings.
  It is greyed while frame generation is off. `Why:` the user wants to compare three variants live
  (2026-11-23), so it is not a tuning row; once one net wins the row goes away.
- Every V3 row acts under V3 Hybrid exactly as under V3, including the Expert rule (tuning rows
  only with Expert on; defaults act with it off; the Motion guard is no Expert row). The variant is not one of the 14 tuning values,
  so `config::FrameGenEffective()` never resets it.

From the command line (the same binding a click writes through; the value is the option's id, as
for every Choice): `gamescopectl overlay_e2_set "framegen.motion_version 3"` (V3),
`"framegen.motion_version 4"` (V3 Hybrid), `"framegen.v3_hybrid_variant 0"` .. `14` (A, B, C, A-A .. A-C, AR-A .. AR-C, AR3-A .. AR3-C, AR4-A .. AR4-C).
The variant row is addressable only while V3 Hybrid is selected (hidden rows do not exist for this
command).

## Where it shows

The status shows what really runs, not just what is selected (2026-11-23):

| State | Status line mode text | Capture / watch header | One-time log line |
|---|---|---|---|
| Plain V3 (or another version) | nothing extra | no token | `learned blend off` (only on a change from a variant) |
| Variant selected, blend in use | `blend=A` (or `A-A` .. `AR4-C`) | ` blend=A` | `learned blend A` |
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
unchanged). Values 0..15 (`kV3BlendMax`; 0..6 until `88e071e`): 0 is the rule (byte-identical to the pin before, as are V1, V2 and Learned);
1 / 2 / 3 are nets A / B / C and each further three the same nets with the next weight set (same structure, cost, formats and fallbacks). The library refuses a value above `kV3BlendMax` in `settingsError` (init and `setSettings()` fail).

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

The host sets it in `ToLibrarySettings()` (`s.v3Blend = uint8_t(c.v3Blend)`, clamped 0..`kHybridBlendMax` (15); the library refuses 16 and above). It is part
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
