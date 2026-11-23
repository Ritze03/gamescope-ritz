# Frame generation: V3 Hybrid

Part of [Frame generation](frame-generation.md). An experimental entry in the **Motion version**
list (2026-11-23): [Motion version 3](frame-generation-motion-v3.md) with its motion search
unchanged, but the hand-written rule that mixes V3's colour samples per pixel replaced by a small
learned blend (a tiny per-pixel network). Three variants, **A / B / C**, exist so the user can
compare them with each other and with plain V3 while a game runs.

> **Library status at this commit: the library field is not wired yet, so every variant still
> shows plain V3.** The pinned library has no `Settings::v3Blend`; the host carries the value up
> to where `framegen::Settings` is filled (`ToLibrarySettings()` in `src/FrameGen/FrameGenHost.cpp`,
> a one-line comment marks the spot) and stops there. The follow-up that bumps the pin removes
> this note.

## Config and mapping

| Key | Values | Meaning |
|---|---|---|
| `framegen.motion_version` | `v1` / `v2` / `learned` / `v3` / **`v3_hybrid`** | `v3_hybrid` is the fifth value, after `v3` |
| `framegen.v3_hybrid_variant` | `"a"` (default) / `"b"` / `"c"`; absent, wrong type or unknown loads `"a"` | which variant, read only while `motion_version` is `v3_hybrid` |

An old file without the new key and with `"v3"` loads as before (V3 stays V3, no blend).
`motion_blur.motion_version` does **not** get the value: Motion blur's own list is unchanged and
`"v3_hybrid"` there is rejected like any unknown string (it keeps `v1`).

Config to host (`fghost::MotionFromKeys()` in `FrameGenV3.h`, pinned by `tests/test_framegen_v3.cpp`):

| `motion_version` | variant | `fghost::Motion` | `Config::v3Blend` |
|---|---|---|---|
| `v3` | any | `V3` | 0 |
| `v3_hybrid` | `a` / `b` / `c` (other: A) | `V3` | 1 / 2 / 3 |
| `v1` / `v2` / `learned` | any | `V1` / `V2` / `Learned` | 0 |

`Why the host enum is unchanged:` `fghost::Motion` is packed into two bits of the main config word
(bits 28-29) and two of the blur word, so a fifth value does not fit. Instead V3 Hybrid is
`Motion::V3` plus a small separate value `Config::v3Blend` (0 = off, 1..3 = A..C), packed into
bits 52-53 of the V3 word (`PackV3()`). Every existing `Motion::V3` check (rows shown, fallbacks,
scene-cut default, HUD protection, cost) therefore applies to V3 Hybrid without touching it. The
config-string checks in the overlay panel (`EffectiveMotionIsV3()`, which feeds the Expert rule,
`V3GuardOn()` and the V3 rows) count `"v3_hybrid"` as V3 too. Settings export / import work on the
section's leaves, so both keys travel with the Frame generation group without a change.

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
- **Hybrid variant** (`image.framegen.v3_hybrid_variant`, a segmented A / B / C Choice) sits directly
  under it, shown only while V3 Hybrid is the selected version, and **without** Expert settings.
  It is greyed while frame generation is off. `Why:` the user wants to compare three variants live
  (2026-11-23), so it is not a tuning row; once one net wins the row goes away.
- Every V3 row acts under V3 Hybrid exactly as under V3, including the Expert rule (tuning rows
  only with Expert on; defaults act with it off). The variant is not one of the 15 tuning values,
  so `config::FrameGenEffective()` never resets it.

From the command line (the same binding a click writes through; the value is the option's id, as
for every Choice): `gamescopectl overlay_e2_set "framegen.motion_version 3"` (V3),
`"framegen.motion_version 4"` (V3 Hybrid), `"framegen.v3_hybrid_variant 0"` / `1` / `2` (A / B / C).
The variant row is addressable only while V3 Hybrid is selected (hidden rows do not exist for this
command).

## Where it shows

The frame generation status log line (`steamcompmgr.cpp`, `FrameGen_LogStatus()`) names the variant
in the mode text (`target 240 fps, blend=B`) and a variant change prints a new line; plain V3 says
nothing extra. The `ritz_fg_capture` header and watch header carry ` blend=A|B|C` after
`fallback_v1=` (only for a variant). The setting dump of a capture lists the library's fields and
gains `v3Blend` with the follow-up.

## Library contract (as announced, not yet in the pin)

`framegen::Settings::v3Blend`, 0..3: 0 is byte-identical to today's rule; 1 / 2 / 3 are nets A / B / C;
only with `motionVersion = V3`. Rgba8 / Bgra8 only for now: other formats and `v3DetailSplit` on
fall back to the rule inside the library, without an error. It takes effect at the next synthesis.
The follow-up adds the one assignment into `framegen::Settings`, the `static_assert` on the
struct's size and the setting dump.

## Tests

`tests/test_config.cpp` (round trip, absent key, invalid variant, Motion blur rejecting the value,
Expert not resetting the variant), `tests/test_framegen_v3.cpp` (the packed bits, the mapping),
`tests/test_framegen_capture.cpp` (header text).
