# Frame generation: Learned motion

Part of [Frame generation](frame-generation.md). The opt-in learned-network motion estimator.

## Learned motion

(2026-10-26, experimental, **opt-in**, branch `ml-fg-testing`; see `ML-FG-TESTING.md` in the
repo root for how to build and test it.) The library's `MotionVersion::Learned`: **V1 with its
motion search replaced by a small learned conv net** (a 5-level coarse-to-fine net on the
flow-resolution luma). It writes what V1's search writes (the per-pixel half-vector field and
the scene-cut SAD), so V1's synthesis, trust ramp, fallbacks, motion blur and UI protection
are untouched. Defaults are unchanged: `v1`; once Learned is chosen: full precision,
zero-motion test on, static guard off.

- **One network** (since 2026-11-25). The library keeps only its default net, `e2v5`
  (`framegen::LearnedNet::E2v5` = 4); there is no choice. The host sets
  `s.learnedNet = framegen::LearnedNet::E2v5` in `FrameGenHost.cpp` for **every** Motion
  version, because the library refuses any other value under any version
  (`validate` / `settingsError`).
  `Why:` the owner: "For learned, lets only keep the default network, just to reduce the
  footprint of the library". The library went from 122 to 26 pipelines on the first switch
  to Learned and from 3.2 MB to 0.4 MB of weights.
  **A stored `framegen.learned_net` is ignored** (any value: a name, a retired name, a number)
  and is not written by the next save; no warning. The loader reads keys by name, so nothing
  complains about the leftover key.
- **Rows** (Frame generation area): **Motion version** has a choice **Learned**
  (`framegen.motion_version` = `learned`). Three more rows, shown only with Expert settings on
  and Learned the effective Motion version (`ShowLearnedRows()`, see
  [Expert settings](frame-generation-settings.md#expert-settings)), and greyed out while
  neither Frame generation nor Motion blur is on (`LearnedRowsUsable()`): **Learned
  precision** (`framegen.learned_half`, Full default / Half), **Zero-motion test**
  (`framegen.learned_zero_test`, default on) and **Static guard**
  (`framegen.learned_static_guard`, default off). Additive config, schema stays 5; unknown
  strings keep the default. `Why` the guard is a separate opt-in switch: the user wanted it
  optional (it costs a full-resolution pass per generated frame and, per the library's own
  measurements, it does not repair a HUD on fast-moving scenery -- the zero test does that).
- **Zero-motion test** (`Settings::learnedZeroTest`): the vector becomes 0 where keeping the
  pixel still fits as well as the net's motion; it fixes much of the static-HUD warp the net
  alone causes and costs ~0.006 ms per pair and no memory. **Static guard**
  (`Settings::learnedStaticGuard`) is a pass over each finished generated frame that shows the
  plain time mix `(1 - t) A + t C` where both real frames are locally identical and the motion
  is small, ~0.05 ms per generated frame at 1080p. Both, and the precision, are runtime
  (`setSettings()`, no GPU wait, no allocation).
- **Motion blur accepts it too** (`motion_blur.motion_version` = `learned`). It has **no
  precision / test / guard rows of its own**: the rows above are Frame generation's and the
  blur borrows them. `Why:` one interpolator serves both features, and more rows for a 3-way
  experiment was not worth it; the help text says so.
- **Host plumbing** (`FrameGenHost.{h,cpp}`): `fghost::Motion::Learned`,
  `Config::learnedHalf` / `learnedZeroTest` / `learnedStaticGuard`. The main packed config
  word is full, so motion stays at its bits 28-29 and the Learned settings have their own word,
  `g_uLearned` (precision bit 4, zero test 5, guard 6, mask `0x7F`; **bits 0-3 are free**, they
  held the net choice until 2026-11-25 and are written as 0 -- the other fields did not move,
  because the mask is referenced by the library-config key; like the crosshair box's
  `g_uUiBox`). `Why` a word of its own and not the blur word's spare bits: they are Frame
  generation's settings, and the blur word is kept apart so a blur edit never touches Frame
  generation. The library-config key (`uLibraryConfig`) includes the motion bits and the
  Learned bits (at bit 56), so a change reaches `setSettings()` without a reconfigure; a
  bit left out of that mask would make a live switch silently do nothing.
- **flowScale 2 is forced** for Learned, exactly as for V2 (`EnsureReady()`): the net was
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
  the only symptom is that V1 runs.
- **Weights are compiled in** (`ml_weights.h`, generated by the library's
  `gpu/ml/embed_weights.py` from `gpu/ml/e2v5/{manifest.json,weights.bin}`; the script also
  verifies the sha256 and the shapes, and refuses any other net). No runtime file, nothing
  extra to install. They reach the GPU through `vkCmdUpdateBuffer` inside the **first Learned
  estimate's command buffer**; the host submits every command buffer `RecordBaseLayer()`
  records into, so the upload cannot be dropped. Switching to Learned creates 26 pipelines at
  once on the render thread (one short hitch).
- **Duplicate frames are the library's job.** The net invents motion on identical frames, so
  every `recordEstimate` first runs `ml_dup` / `ml_reduce` (mean |a - c| over pixels and R, G,
  B, threshold 0.6 levels): on a duplicate every net pass exits and the field is zero. It is
  unconditional in the Learned estimate path (`mlEstimate()`); the host does nothing.
  **Caveat:** the test is whole-frame, so a small moving cursor on an otherwise still
  screen counts as a duplicate and its motion is not interpolated.
- **Cost** (library figures, a development GPU, 1080p 2x, estimate + synth per real pair): V1
  0.30-0.33 ms, Learned 1.3-1.5 ms (half precision 1.0-1.1); the zero test adds ~0.005 ms,
  the static guard ~0.045 ms per generated frame. Memory on top of V1: ~100 MB (half 50) +
  11 MB state at 1080p; the zero test and guard need none. The memory stays until teardown
  (`releaseUnusedMotion()` is not called, as for V2).
- **Quality**: with the zero test (the default) it matches V1 on some clips and sits behind it
  on others. **Known: static HUD text** is much better than without the zero test but not yet
  as clean as V1: glyph edges on a counter stay notched. The static guard does not change that
  where the field around the HUD moves more than ~1 px. UI protection works as with V1 but
  Crosshair V2 leaves more pixels around the crosshair with Learned (the library's known
  issue (a)).
- Shaders: eight `ml_*` compute shaders in `src/meson.build`'s `framegen_shaders` (`ml_dup`,
  `ml_reduce`, `ml_pyr`, `ml_match`, `ml_conv`, `ml_out`, `ml_zero`, `ml_guard`;
  `ml_common.glsl` is their include, picked up by the depfile), `ml_dup` and `ml_guard` also
  with `rgb10` and `f16` variants (the two Learned passes that read colour), and the
  `framegen_ml_weights` custom target for `ml_weights.h` (one net). The library probes
  `ml_conv`, `ml_zero`, `ml_guard` and `ml_weights.h` (plus each format's `ml_dup` /
  `ml_guard` variants) with `__has_include`: **any one missing and Learned silently
  disappears**. Touch `FrameGenLib.cpp` when this set changes (stale incremental builds).

## History

- **Learned motion** (glossary detail): the FrameGen library's third motion version, `MotionVersion::Learned` (2026-10-26, experimental and opt-in).
  - Rows: **Motion version** > **Learned**, with **Learned precision** (Full / Half), **Zero-motion test** (default on) and **Static guard** (default off). Motion blur's own Motion version accepts it too and borrows Frame generation's Learned rows.
  - What it is: **V1 with its motion search replaced by a small learned neural network** (from the user's separate ml-fg project). Everything after the motion estimate (synthesis, fallbacks, UI protection, blur) is V1's.
  - Cost and limits: needs the library's flowScale 2, so Quality's Performance is ignored while it runs; costs several times V1's GPU time (about 4.5x at 1080p); skips two identical frames. Where the library refuses it V1 runs and the Status line says why.
  - **Nets, history:** between 2026-10-26 and 2026-11-25 there was a **Learned net** row with ten networks (two earlier rounds' nets and their small variants, the default `e2v5` and its small variant, and four colour / learned-features nets in two training sets). All but `e2v5` were removed on 2026-11-25 (library `678f9cc`), the row and `framegen.learned_net` with them, at the owner's request (see the `Why:` above). A stored value is ignored. The library proved `e2v5` frames byte-identical before and after, and the other motion versions too. The comparison tables of the removed nets are gone with them.
  - Unrelated to **Motion version 2** (the "morphing" interpolator).
  - See `superdoc/features/frame-generation.md`'s "Learned motion" section and `ML-FG-TESTING.md`.
