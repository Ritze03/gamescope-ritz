# Frame generation: Learned motion

Part of [Frame generation](frame-generation.md). The opt-in learned-network motion estimator, its nets and rounds.

## Learned motion

(2026-10-26, experimental, **opt-in**, branch `ml-fg-testing`; round 1 pinned FrameGen
`fd5c163`, round 2 pins `6133a4a`, round 3 `4bae33a`, all FGR's local-only `ml-fg-testing` branch -- see
`ML-FG-TESTING.md` in the repo root for how to build and test it.) The library's
`MotionVersion::Learned`: **V1 with its motion search replaced by a small learned conv net**
(ml-fg's nets, a 5-level coarse-to-fine net on the flow-resolution luma). It writes what V1's
search writes (the per-pixel half-vector field and the scene-cut SAD), so V1's synthesis,
trust ramp, fallbacks, motion blur and UI protection are untouched. Defaults are unchanged:
`v1`; once Learned is chosen, net `e2v5`, full precision, zero-motion test on, static guard off.

- **Rows** (Frame generation area): **Motion version** gains a third choice, **Learned**
  (`framegen.motion_version` = `learned`). Below it, four rows, greyed out unless Learned is
  what actually runs (`LearnedRowsUsable()`: Frame generation or Motion blur on, and the
  effective Motion version `learned`): **Learned net** (`framegen.learned_net`, in GUI order
  `e2v5` default / `e1v5` the small cheap net / `e2v4` / `e1v4` / `e2v3` / `e1v3`, the last four
  being earlier rounds' nets kept for comparing), **Learned precision** (`framegen.learned_half`, Full default /
  Half), **Zero-motion test** (`framegen.learned_zero_test`, default on) and **Static guard**
  (`framegen.learned_static_guard`, default off). Additive config, schema stays 5; unknown
  strings keep the default. `Why` the guard is a separate opt-in switch: the user wanted it
  optional (it costs a full-resolution pass per generated frame and, per the library's own
  measurements, it does not repair a HUD on fast-moving scenery -- the zero test does that).
- **Round 2 (library `6133a4a`)**: `LearnedNet` grew to `E2v3 = 0, E1v3 = 1, E2v4 = 2,
  E1v4 = 3` (e2v4 is e2v3 fine-tuned by ml-fg at the same cost, and the new library default;
  e1v4 likewise for e1v3). `Settings::learnedZeroTest` (default true) is ml-fg's zero-motion
  test: the vector becomes 0 where keeping the pixel still fits as well as the net's motion;
  it fixes much of the round-1 static-HUD warp and costs ~0.006 ms per pair and no memory.
  `Settings::learnedStaticGuard` (default false) is a pass over each finished generated frame
  that shows the plain time mix `(1 - t) A + t C` where both real frames are locally
  identical and the motion is small, ~0.05 ms per generated frame at 1080p. Both are runtime
  (`setSettings()`, no GPU wait, no allocation).
- **Round 3 (library `4bae33a`)**: `LearnedNet` grew by `E2v5 = 4` (the new library default:
  e2v4 fine-tuned further by ml-fg on more varied footage, same architecture, size and cost)
  and `E1v5 = 5` (the new small net, likewise from e1v4); old values keep their numbers and
  `kLearnedNets` is 6. No shader changed. ml-fg's 14-clip score at 2x with the zero test:
  e2v5 31.23 dB (+0.04 over e2v4), e1v5 31.05 dB (+0.06 over e1v4). On the library's real
  footage e2v5 closes most of e2v4's gap to V1 on obs1 13100 (34.00 vs 32.98 dB; V1 34.80) and
  gives up 0.13 dB on battlefield; e1v5 is the best of all on battlefield (36.66) and the worst
  on obs1 13100 (32.09). `Why` appended and not renumbered: the numbers are packed into
  `g_uLearned`, and the library keeps its enum values stable. The six values need three bits,
  so the net field is now bits 0-2 and the other three switches moved up (see Host plumbing).
  A profile that saved an earlier net keeps it (the serializer writes every key); only a
  profile with no `learned_net` key gets e2v5.
- **Motion blur accepts it too** (`motion_blur.motion_version` = `learned`). It has **no
  net / precision of its own**: the two rows above are Frame generation's and the blur
  borrows them. `Why:` one interpolator serves both features, and two more rows for a
  3-way experiment was not worth it; the help text says so.
- **Host plumbing** (`FrameGenHost.{h,cpp}`): `fghost::Motion::Learned`,
  `fghost::LearnedNet` (the library's six values), `Config::learnedNet` / `learnedHalf` /
  `learnedZeroTest` / `learnedStaticGuard`. The main packed config word is full, so motion
  stays at its bits 28-29 (bits 30-31 are unused again) and the four Learned settings have
  their own word, `g_uLearned` (net bits 0-3, precision 4, zero test 5, guard 6 since round 4b; like the
  crosshair box's `g_uUiBox`). `Why` a word of its own and not the blur word's spare bits:
  they are Frame generation's settings, and the blur word is kept apart so a blur edit never
  touches Frame generation. The blur word's motion is bits 15-16 and its quality bit 17
  (in-memory only, nothing persists these words). The library-config key (`uLibraryConfig`)
  includes the motion bits and the five Learned bits (at bit 56), so a change reaches
  `setSettings()` (runtime, no GPU wait) without a reconfigure.
- **flowScale 2 is forced** for Learned, exactly as for V2 (`EnsureReady()`): the nets were
  trained at 2 and `settingsError()` refuses anything else. So **Quality's Performance is
  ignored while Learned runs** (V1's flowScale 4 returns when the version goes back to V1,
  which drops the estimate anyway). `ResolveMotion()` judges the value that will be applied
  (flowScale 2), not the Performance one.
- **Refusal falls back to V1**, like V2: `ResolveMotion()` asks `settingsError()`; a reason
  (the ml shaders or `ml_weights.h` not built in -- `kMlNotBuilt`; no-Vulkan-feature cases)
  goes to the log (`Learned motion refused, running V1: ...`, once per change) and
  `RenderStatus::motionNote`, which the Status lines print as ` · V1: <reason>`.
  `Why this matters:` without the ml shaders and the generated `ml_weights.h` the library
  still compiles (it probes them with `__has_include`) and **silently** leaves Learned out;
  the only symptom is that V1 runs. Build checks: see below and `ML-FG-TESTING.md`.
- **Weights are compiled in** (`ml_weights.h`, generated by the library's
  `gpu/ml/embed_weights.py` from `gpu/ml/{e2v3,e1v3,e2v4,e1v4,e2v5,e1v5}/{manifest.json,weights.bin}`,
  in that order = the `LearnedNet` values; the script also verifies the sha256 and the
  shapes). No runtime file, nothing extra to install. They
  reach the GPU through `vkCmdUpdateBuffer` inside the **first Learned estimate's command
  buffer**; the host submits every command buffer `RecordBaseLayer()` records into (`pCmd`
  is submitted unconditionally at the end, with no early return after `Cmd()`), so the upload
  cannot be dropped. Switching to Learned creates 76 pipelines at once on the render thread
  (one short hitch; 56 in round 2, 34 in round 1).
- **Duplicate frames are the library's job.** The net invents motion on identical frames, so
  every `recordEstimate` first runs `ml_dup` / `ml_reduce` (mean |a - c| over pixels and R, G,
  B, threshold 0.6 levels): on a duplicate every net pass exits and the field is zero. It is
  unconditional in the Learned estimate path (`mlEstimate()`); the host does nothing.
  **Caveat:** the test is whole-frame, so a small moving cursor on an otherwise still
  screen counts as a duplicate and its motion is not interpolated.
- **Cost** (library figures, RX 7900 XTX, 1080p 2x, estimate + synth per real pair): V1
  0.30-0.33 ms, e2v4 (and e2v3, e2v5) 1.44-1.48 (half 1.09-1.13), e1v4 (and e1v3, e1v5) 0.62
  (half 0.49-0.51; round 3 re-measured e2v5 1.35-1.66 and e1v5 0.53-0.54); the zero test adds ~0.005 ms, the static guard ~0.045 ms per generated frame.
  Round 1's figures were ~10 % lower on a quieter machine; 1440p e2v3 2.59 (half 1.62), not
  re-measured. Memory on top of V1: e2v4/e2v3/e2v5 100 MB (half 50) + 11 MB state at 1080p,
  e1v4/e1v3/e1v5 58 MB (half 29); the zero test and guard need none. The memory grows to the
  largest net used and stays until teardown (`releaseUnusedMotion()` is not called, as for
  V2).
- **Quality**: with the zero test (the default) e2v4 / e1v4 match V1 on the library's
  obs2 clip, beat it by ~0.2 dB on battlefield and sit ~2 dB below it on obs1 13100 (the one
  regression against e2v3 on FGR's clips). **Known: static HUD text** is much better than in
  round 1 (the net has no zero-motion prior of its own; the zero test supplies one) but not
  yet as clean as V1: glyph edges on a money counter stay notched. The static guard does not
  change that where the field around the HUD moves more than ~1 px. UI protection works as
  with V1 but Crosshair V2 leaves more pixels around the crosshair with Learned (the library's
  known issue (a)).
- Shaders: eight `ml_*` compute shaders in `src/meson.build`'s `framegen_shaders` (`ml_dup`,
  `ml_reduce`, `ml_pyr`, `ml_match`, `ml_conv`, `ml_out`, and since round 2 `ml_zero` and
  `ml_guard`; `ml_common.glsl` is their include, picked up by the depfile), `ml_dup` and
  `ml_guard` also with `rgb10` and `f16` variants (the two Learned passes that read colour),
  `ml_planes` (round 4, also with `rgb10` / `f16` variants) and `ml_feat` (round 4), and the `framegen_ml_weights` custom target for `ml_weights.h` (eight nets). The library
  probes `ml_conv`, `ml_zero`, `ml_guard` and `ml_weights.h` (plus each format's `ml_dup` /
  `ml_guard` variants) with `__has_include`: **any one missing and Learned silently
  disappears**. Touch `FrameGenLib.cpp` when this set changes (stale incremental builds).

### Learned round 4 and 4b: colour and features nets

(2026-10-28, FrameGen `981c844`, library's local-only `ml-fg-testing`.) Two **architecture
variants** of the estimator, opt-in in the Learned net row after the luma nets. The **colour**
variant (25 inputs: the first conv also sees two chroma-mismatch and nine chroma-cost
channels; needs `ml_planes`) and the **learned-features** variant (27 inputs: a small shared
conv extractor on RGB, four feature maps matched like the luma; needs `ml_feat`). Not a
weight swap: the first conv's input contract changes. Round 4 shipped them as 600-step
stand-ins (`c2smoke`, `c2feat`); **round 4b replaced the stand-ins with the four real nets**:

| key | `LearnedNet` | GUI label | kind |
|---|---|---|---|
| `g2v2` | `G2v2 = 6` | g2v2 Colour (games) | colour, trained on games |
| `g2v3` | `G2v3 = 7` | g2v3 Features (games) | learned features, games |
| `m2v2` | `M2v2 = 8` | m2v2 Colour (allround) | colour, allrounder |
| `m2v3` | `M2v3 = 9` | m2v3 Features (allround) | features, allrounder |

The default stays `e2v5` and Motion version stays V1. Cost at 1080p 2x on Vulkan (fp32 /
half): e2v5 1.31 / 1.02 ms, g2v2 and m2v2 1.74 / 1.25, g2v3 and m2v3 1.89 / 1.40. The library's
spot check has m2v3 best even on game clips. The zero test, static guard, half precision and
duplicate skip work per net as before. **Known limitation:** Crosshair V2's fill patches the
luma only, not the chroma / RGB planes these variants also read.
**Migration:** a saved `"c2smoke"` / `"c2feat"` loads as `g2v2` / `g2v3` (a plain rename in
the `learned_net` parse in `ConfigManager.cpp`; the next save writes the new name, no schema
bump). `Why:` the stand-ins were never meant to outlive the real nets, and a profile that
saved one should keep the same variant (colour / features) instead of falling to e2v5.
**Four-bit net field:** ten nets do not fit the old three bits (eight values), so the Learned
word `g_uLearned` is now net bits 0-3, precision 4, zero test 5, static guard 6 (mask `0x7F`
in `uLibraryConfig` at bits 56-62; the box tenths at 48-55 are untouched). `Why:` that mask is
the live-change key for `setSettings()`: a bit left out would make switching to that net live
silently do nothing (round 3 hit this class of bug).

## History

- **Learned motion** (terminology entry, moved here 2026-11-07): **Learned motion** (Learned; since 2026-10-28 also four opt-in colour / learned-features nets, **g2v2** / **g2v3** (trained on games) and **m2v2** / **m2v3** (allround); saved `c2smoke` / `c2feat` load as g2v2 / g2v3) — the FrameGen library's third motion version, `MotionVersion::Learned` (2026-10-26, experimental and opt-in, branch `ml-fg-testing`; row **Motion version** > **Learned** (`framegen.motion_version` = `learned`), with the rows **Learned net** (`e2v5` default since round 3, 2026-10-27; `e1v5` the small cheap net; `e2v4` / `e1v4` round 2's and `e2v3` / `e1v3` round 1's, kept for comparing; a profile that saved an earlier net keeps it), **Learned precision** (Full / Half), **Zero-motion test** (default on) and **Static guard** (default off); Motion blur's own Motion version accepts it too and borrows Frame generation's four Learned rows): **V1 with its motion search replaced by a small learned neural network** (from the user's separate ml-fg project). Everything after the motion estimate (synthesis, fallbacks, UI protection, blur) is V1's. The **zero-motion test** treats a pixel as still wherever keeping it still fits the two real frames as well as the net's motion does, which fixes most of the static-HUD warp the net alone causes (~0.006 ms at 1080p); the **static guard** shows a plain blend of the two real frames where both are locally identical and the motion is small (~0.05 ms per generated frame). Needs the library's flowScale 2, so Quality's Performance is ignored while it runs; costs several times V1's GPU time (e1v5 about 2x, e2v5 about 4.5x at 1080p); skips two identical frames. Where the library refuses it V1 runs and the Status line says why. Unrelated to **Motion version 2** (the "morphing" interpolator). See `superdoc/features/frame-generation.md`'s "Learned motion" section and `ML-FG-TESTING.md`.
