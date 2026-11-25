# Frame generation: V3.5 and V3.5 Hybrid

Part of [Frame generation](frame-generation.md). Two experimental entries in the **Motion version**
list (2026-11-25, library pin `1ccc602`), built exactly like [V3 Hybrid](frame-generation-v3-hybrid.md):
each is `Motion::V3` plus extra settings, so every rule of [Motion version 3](frame-generation-motion-v3.md)
applies.

- **V3.5**: V3 with improved motion detection (the library's `v3Motion = 1`: its block search with
  confidence-weighted regularisation of the block fields, and the dense field blended against a locally
  linear field). Smoother, connected motion, fewer wrong vectors on flat surfaces, camera turns and zooms.
- **V3.5 Hybrid**: V3.5 plus V3 Hybrid's learned blend (Fluidity or Consistency, the existing
  `framegen.v3_hybrid_variant`).

## Config and mapping

`framegen.motion_version` accepts two more strings, **`v3_5`** and **`v3_5_hybrid`** (agreed with the
library's maintainers). No new key. `motion_blur.motion_version` does **not** get them: it rejects them
like any unknown string (it keeps `v1`), as it does `v3_hybrid`.

| `motion_version` | variant | `fghost::Motion` | `Config::v3Blend` | `Config::v3Motion` |
|---|---|---|---|---|
| `v3` | any | `V3` | 0 | 0 |
| `v3_hybrid` | fluidity / consistency | `V3` | 1 / 2 | 0 |
| **`v3_5`** | any | `V3` | 0 | **1** |
| **`v3_5_hybrid`** | fluidity / consistency (other: Fluidity) | `V3` | 1 / 2 | **1** |
| `v1` / `v2` / `learned` | any | `V1` / `V2` / `Learned` | 0 | 0 |

`fghost::MotionFromKeys()` (`FrameGenV3.h`; `struct MotionChoice` gained `v3Motion`). `v3Motion` is bit 56 of
the V3 word (`PackV3()` / `UnpackV3()`; anything but 1 packs as 0, matching the library's `kV3MotionMax` = 1),
so a change reaches the library through the ordinary runtime `setSettings()`. `ToLibrarySettings()` copies it
to `framegen::Settings::v3Motion`. Motion blur's own V3 (own settings on) zeroes it, like the blend.

## Overlay

**Motion version** has two more options after V3 Hybrid, labelled **V3.5** and **V3.5 Hybrid**
(`kMotionKeys` indices 5 and 6 in `PanelFrameGen.cpp`). Every V3 row rule applies to both
(`EffectiveMotionIsV3()`), and the **Hybrid variant** row shows for `v3_hybrid` and `v3_5_hybrid`. The
**Colour-weighted matching** row has no effect while V3.5 runs (the library ignores `v3ColourWindow` then), so
it is greyed with "Not used by V3.5 ..." (`ColourWindowUsable()`); there was no earlier precedent for a
Hybrid greying a V3 row. The help text marks both experimental. From the command line:
`gamescopectl overlay_e2_set "framegen.motion_version 5"` (V3.5) / `6` (V3.5 Hybrid).

## The failure path

The library has no silent fallback: if V3.5's pipelines cannot be created, `init()` / `setSettings()` return
false. The host never leaves frame generation on a refused request:

- `ResolveMotion()`: if `settingsError()` refuses the V3 request and `v3Motion` is set, it retries the same
  request with `v3Motion = 0` (plain V3 / V3 Hybrid) and logs one warning line (`V3.5 refused, Motion version 3
  runs with its own motion detection: <reason>`).
- `EnsureReady()`: if `setSettings()` itself fails with `v3Motion` set (pipeline creation), it retries once with
  `v3Motion = 0` and logs one error line (`the library could not create V3.5 (its pipelines); running Motion
  version 3 with its own motion detection`).

In both cases the status reads `V3.5(off)` (see below) because `v3MotionActive()` is false.

## First use and cost

The four V3.5 pipelines (`v35_stats`, `v35_rgn`, `v35_reg`, `v3_dense_35`) are created lazily inside the first
`setSettings()` with `v3Motion != 0`. First switch 0 to 1 on a running interpolator: about 275 ms with an empty
shader cache, 0.33 ms warm, later 0 ms. We accept this one hitch (no extra machinery). Cost per real pair at
1080p, 8-bit, V3 to V3.5: about +0.5 ms (2x 0.95 to 1.47 ms, 4x 1.43 to 1.93 ms), about +0.63 ms at 1440p;
the Hybrids the same amount on top of V3 Hybrid.

## Where it shows

| State | Status line | Stats overlay line 1 | Log line | Capture header |
|---|---|---|---|---|
| V3.5 running | `motion=V3.5` | `... motion V3.5` | `Motion version 3 motion detection V3.5` | ` v3motion=V3.5` |
| requested, not running | `motion=V3.5(off)` | `... motion V3.5(off)` | `V3.5 requested but not active (V3's own runs)` | ` v3motion=V3.5(off)` |

`PublishBlend()` (`FrameGenHost.cpp`) asks `Interpolator::v3MotionActive()` at the end of every `EnsureReady()`
and publishes it (`g_bV35Active`, read through `fghost::V35Active()`); `fghost::V35Label()` formats it. The
capture dump lists `setting v3Motion=N`.

## Library contract (pin `1ccc602`)

`framegen::Settings::v3Motion`: `uint8_t` at offset 155 (a former padding byte before `flowScale`), so
`sizeof(Settings)` stays 164 and the two `static_assert`s are unchanged. `kV3MotionClassic` = 0 (byte-identical
to before), `kV3MotionV35` = 1, `kV3MotionMax` = 1; larger values are refused. Only `MotionVersion::V3` reads it.
Motion guard (both modes), Motion blur, UI protection and the Motion debug view work with it; the learned blend
behaves as before (Rgba8 / Bgra8 / Rgb10 nets; other formats and Detail from one frame run the rule blend with
V3.5's motion); `v3ColourWindow` is ignored.

**Build:** this repo does not use the library's `gpu/meson.build`; `src/meson.build` lists the shaders itself. The
pin added `v35_stats`, `v35_rgn` and `v35_reg` (plain compiles, in the `framegen_shaders` name list) and
`v3_dense` with `-DV35=1` into `v3_dense_35_spv` (in the `-DV3J=1` / `-DV3L=1` / `-DV3B=1` list). Without them
the build fails (the library includes them unconditionally).

## Honest state

Experimental. On real footage the picture-score gain over V3 is small (about +0.1 dB); just inside the outline
of a still object or character the vectors are slightly worse than V3's; very fast whip pans fall back as in V3.

## Tests

`tests/test_framegen_v3.cpp` (the mapping, bit 56 round trip and range, `V35Label()`), `tests/test_config.cpp`
(both strings round-trip, Motion blur rejects them, `v3` / `v3_hybrid` unchanged), `tests/test_framegen_capture.cpp`
(settings dump count). `v3MotionActive()` needs a GPU: checked in headless nested runs.
