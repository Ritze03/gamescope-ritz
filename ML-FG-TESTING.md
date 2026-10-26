# ML-FG-TESTING: Learned motion in gamescope-ritz

Branch `ml-fg-testing` (local only, based on master `c52a245`; not pushed, not merged).
It integrates frame-gen-ritz's (FGR) opt-in `MotionVersion::Learned` (V1 with its motion
search replaced by a small learned conv net). V1 and V2 stay the defaults; with Learned off
nothing changes (V1 output is byte-identical per the library's own checks).

Round 1 pinned FGR `fd5c163` (nets `e2v3` / `e1v3`). **Round 2 pins FGR `6133a4a`**: new
nets `e2v4` (the new default) and `e1v4` (the small, cheap one), ml-fg's zero-motion test
(on by default) and its static guard (off by default). See "Round 2" below.

## Round 2 (FGR `6133a4a`)

- **Nets**: `Learned net` now has `e2v4` (default; e2v3 fine-tuned by ml-fg, same cost),
  `e1v4` (e1v3 fine-tuned; the recommended cheap fallback, about 2.4x cheaper than e2v4 at
  1080p) and round 1's `e2v3` / `e1v3`, kept for comparing.
- **Zero-motion test** (default **on**): per flow pixel, the vector becomes 0 where keeping
  the pixel still fits the two frames as well as the net's motion. It fixes much of round 1's
  warped static HUD (a "$69" money counter went from an unreadable blob to readable, menu text
  is close to V1). Cost ~0.005 ms per pair at 1080p, no memory.
- **Static guard** (default **off**, as the user wanted it optional): a full-resolution pass
  per generated frame that shows the plain time mix of the two real frames where both are
  locally identical and the motion is small. Cost ~0.045 ms per generated frame at 1080p, no
  memory. It does **not** repair a HUD on fast-moving scenery (the library's own finding); the
  zero test is what helps the HUD. It gives +1.3 dB on a duplicate-frame pair.
- **How to switch each** (overlay: Frame generation, with Motion version = Learned; or the
  profile JSON, section `framegen`):

  | what | overlay row | config key | values | default |
  |---|---|---|---|---|
  | net | Learned net | `framegen.learned_net` | `e2v4`, `e1v4`, `e2v3`, `e1v3` | `e2v4` |
  | precision | Learned precision | `framegen.learned_half` | `false` (Full) / `true` (Half) | `false` |
  | zero-motion test | Zero-motion test | `framegen.learned_zero_test` | `true` / `false` | `true` |
  | static guard | Static guard | `framegen.learned_static_guard` | `true` / `false` | `false` |

  Example: `"framegen": { "motion_version": "learned", "learned_net": "e1v4",
  "learned_static_guard": true }`. All four rows are greyed out unless Learned is what runs.
  Motion blur's Learned borrows all four from Frame generation's rows.
- **Packing** (`FrameGenHost.cpp`): the main packed config word is full (bits 0-29 used,
  28-29 = motion), so round 1's net (bit 30) and precision (bit 31) **moved** into a word of
  their own, `g_uLearned`: net bits 0-1, half bit 2, zero test bit 3, guard bit 4. The main
  word now packs exactly as before for V1 (bits 30-31 unused again). The five bits enter the
  library-config key at bit 56, next to the motion bits, so a change reaches `setSettings()`
  (runtime, no GPU wait) without a reconfigure.
- **Build**: `src/meson.build` adds `ml_zero` and `ml_guard` (and `ml_guard` as `rgb10` /
  `f16` variants), and the weights target is now a loop over `e2v3, e1v3, e2v4, e1v4` (the
  `LearnedNet` order). 56 pipelines are created when Learned is first used (34 before).
- **Profile caveat (no migration)**: the config serializer writes **every** key into the
  profile on each save. So any profile saved with round 1's build already carries
  `"learned_net": "e2v3"` and **keeps e2v3**; it does not pick up the new e2v4 default. The
  new keys are absent there and load as zero test on / guard off. To move such a profile to
  e2v4 pick it once in the Learned net row (or edit the JSON). Tested in
  `framegen: a profile saved with round 1's learned_net keeps it, ...`.

## What was done (round 1; round 2's changes are above)

- **Submodule bump**: `subprojects/FrameGen` -> FGR `fd5c163` (FGR's local-only
  `ml-fg-testing` branch); round 2: `6133a4a`.
- **Build** (`src/meson.build`; round 1, see Round 2 for the additions): the six `ml_*.comp` shaders (`ml_dup`, `ml_reduce`,
  `ml_pyr`, `ml_match`, `ml_conv`, `ml_out`), `ml_dup` also as `rgb10` / `f16` variants,
  and a `framegen_ml_weights` custom target that runs the library's
  `gpu/ml/embed_weights.py` to make `ml_weights.h` (both nets compiled in, no runtime file).
  `src/FrameGen/FrameGenLib.cpp` comment touched to force the one recompile.
- **Host** (`src/FrameGen/FrameGenHost.{h,cpp}`): `Motion::Learned`, `LearnedNet`,
  `Config::learnedNet` / `learnedHalf` (round 1 packed bits 28-29 motion, 30 net, 31 half; net and
  half have since moved, see Round 2), flowScale forced to 2 for Learned (Quality's Performance is ignored then),
  `ResolveMotion()` generalised: a refused Learned runs V1 and the library's reason shows
  in the Status line (` · V1: <reason>`) and the log (`Learned motion refused, running V1:`).
- **Config**: `framegen.motion_version` accepts `"learned"`; new `framegen.learned_net`
  (round 1: `"e2v3"` default / `"e1v3"`; now four values, default `"e2v4"`) and `framegen.learned_half` (default `false`);
  `motion_blur.motion_version` accepts `"learned"` too. Additive, schema stays 5.
- **Overlay**: Frame generation > Motion version has a third choice, **Learned**; rows
  **Learned net** and **Learned precision** (Full / Half) below it, greyed out unless Learned
  is what runs. Motion blur's Motion version also offers Learned; it borrows Frame
  generation's net and precision (a deliberate simplification, said in the help text).
- Docs: `superdoc/features/frame-generation.md` ("Learned motion"), `motion-blur.md`,
  `build-and-tooling.md`, `superdoc/meta/TERMINOLOGY.md` ("Learned motion"), `CHANGELOG.md`
  (0.14.3). Config test added to `tests/test_config.cpp`.

## How to build

`6133a4a` (and round 1's `fd5c163`) is **local-only**: it is not on GitHub. A fresh clone / worktree needs the commit
fetched from the local FGR checkout before `git submodule update` can check it out:

```sh
git -C subprojects/FrameGen fetch /home/mo/Schinken/Programming/GitHubProjects/frame-gen-ritz ml-fg-testing
git submodule update --init subprojects/FrameGen
```

(The main checkout's submodule store already has the objects of both rounds, fetched for this
purpose, so `git submodule update` works offline there too.) The build itself does not need the network
for FrameGen. Then, as always for agents, `scripts/gentle-build.sh` (-> `build-agent/`; add
`--test` for the unit tests).

## How to run it for a game test

- The gentle build is already `release -O2`, no LTO. Binary:
  `/home/mo/Schinken/Programming/GitHubProjects/gamescope-ritz/build-ml-fg-testing/build-agent/src/gamescope`.
  `/usr/bin/gamescope-ritz` is a symlink to the *main checkout's* `build-release/` binary, so it does
  **not** run this branch. Launch the build-tree binary directly, e.g.
  `build-agent/src/gamescope <your usual flags> -- %command%`.
- **WSI layer**: the build-tree binary bakes in the install prefix `/usr/local`
  (`/usr/local/share/vulkan/implicit_layer.d/VkLayer_RITZ_gamescope_wsi.x86_64.json`). That
  manifest is already installed on this machine (from 3 Oct), so the binary finds and uses it
  without installing anything (this branch does not touch `layer/` or the protocol). If you
  prefer a fully matching, LTO-optimised binary, run `scripts/build-gamescope-ritz.sh` *in
  this worktree* (it builds this worktree's own `build-release/`; it is heavy, so it was not
  run) and point the launch at that binary. Nothing was installed.
- **Enable it**: overlay > Frame generation > Motion version > **Learned** (then Learned net,
  Learned precision, Zero-motion test, Static guard; see the table under Round 2). Frame
  generation must be on, and the game must be below the refresh rate for it to generate. Or in
  the profile JSON:
  `"framegen": { "motion_version": "learned", "learned_net": "e2v4", "learned_half": false,
  "learned_zero_test": true, "learned_static_guard": false }` (`"learned_net": "e1v4"` and
  `"learned_half": true` are the cheaper settings). Motion blur: its own Motion version row
  (with "Use Frame generation's settings" off) or `motion_blur.motion_version: "learned"`.
- If the Status line shows ` · V1: ...` the library refused Learned and V1 is running; the
  text is the reason.

## What to watch for

- **GPU cost** (library figures, 1080p 2x on an RX 7900 XTX, estimate + synth per real pair,
  ms): V1 0.30-0.33, e2v4 1.44-1.48 (Half 1.09-1.13), e1v4 0.62 (Half 0.49-0.51); the zero
  test adds ~0.005, the static guard ~0.045 per generated frame. (Round 1 measured e2v3 1.30
  on a quieter machine; e2v4 costs what e2v3 does.) 1440p e2v3 ~2.6 ms (half ~1.6), not
  re-measured. At high game fps (many estimates per second) this adds up; try e1v4 / Half if
  the Status line shows the cost guard biting. Extra GPU memory: ~110 MB (e2v4 fp32, 1080p);
  e1v4 ~69 MB; the zero test and guard need none.
- **Static HUD** (library known issues (f), (g)): with the zero test the HUD is much better
  than in round 1 but not yet as clean as V1: glyph edges on a money counter stay notched with
  a few stray pixels (27.2 dB vs V1 31.2 dB on that crop). The static guard does not change it
  where the field around the HUD moves more than ~1 px. Watch money counters / menu titles.
- **obs1 13100 regression** (library known issue (h)): e2v4 and e1v4 are ~2 dB below V1 on
  that one pair (about 1.3 dB below e2v3 with the zero test); on the others they match or beat
  V1. Net choice is worth A/B testing on the user's own games.
- **Duplicate skip** (known issue (f)): two near-identical frames (mean |a-c| <= 0.6 levels
  over the whole frame) skip the net and are treated as still. A small moving cursor on an
  otherwise still screen therefore counts as a duplicate and its motion is not interpolated.
- **Quality's Performance is ignored** while Learned runs (the nets need flowScale 2).
- Crosshair V2 UI protection leaves more pixels around the crosshair with Learned than with
  V1 (library known issue (a)).
- Switching to Learned the first time builds 56 pipelines at once: one short hitch.
- If the Status line shows ` · V1: ...` Learned silently vanished or was refused. With the
  shader or weights list incomplete the library compiles and just leaves Learned out
  (`kMlNotBuilt`); round 2 needs `ml_zero`, `ml_guard` (+ rgb10/f16 variants) and all four nets.
- Switching versions shows real frames for an instant (estimate dropped), same as V1 <-> V2.

## Findings from reading the code

- **Command buffers are never discarded.** `RecordBaseLayer()` makes one lazily created
  `pCmd` (`Cmd()` lambda), records the observe, estimate and synths into it, and submits it
  at the end whenever it exists; there is no early return between the first `Cmd()` and the
  submit, and a failed record still leaves the buffer submitted. So the weights'
  `vkCmdUpdateBuffer` in the first Learned estimate's command buffer always executes.
- **Duplicate skip is unconditional in the library.** `mlEstimate()` always runs `ml_dup` +
  `ml_reduce` first; on a duplicate every later pass exits and the field is zero. The host
  needs nothing.
- **Silent-failure guard**: without the ml `_spv.h` headers or `ml_weights.h` the library
  still compiles and just refuses Learned (`kMlNotBuilt`). That is covered by the checks below
  and, at runtime, by the Status line / log (V1 runs, with the reason).

## Checks run

Round 2:

- `scripts/gentle-build.sh --test` (build-agent/): build OK, **All tests passed (19446784
  assertions in 682 test cases)** (round 1: 19446763 / 681). The learned config case now covers
  all four nets, the zero test and guard, invalid-string fallback; a second new case covers a
  round-1 profile keeping `e2v3`. Only warnings are pre-existing ones.
- Compiled in (`build-agent/src/`): `ml_zero_spv.h`, `ml_guard_spv.h`, `ml_guard_rgb10_spv.h`,
  `ml_guard_f16_spv.h` (plus round 1's `ml_dup*`, `ml_reduce`, `ml_pyr`, `ml_match`, `ml_conv`,
  `ml_out`), and `ml_weights.h` (3.4 MB) holding `e2v3`, `e1v3`, `e2v4` and `e1v4` (each with its
  `kMlWidth_` / `kMlTable_` / `kMlWeights_`).
- Runtime (same rig as round 1: nested gamescope under a private headless sway, vkcube held
  at 30 fps by MangoHud, a 1920x1080 client at `-r 120`, fixed 2x, throwaway profile in a
  temporary `XDG_CONFIG_HOME`, audio capture off, the other installed Vulkan frame layer
  disabled with its own env var, nothing in `~/.config/gamescope-ritz` touched, `log_framegen
  debug`), about 25 s per run:
  - `learned` e2v4 + zero test: "frame generation ready: 1920x1080, flowScale 2", generated
    at ~2.0-2.2x, **no refusal** in the log and no ` · V1:` note.
  - `learned` e2v4 with the zero test off: the same, no refusal.
  - `learned` e1v4 + static guard on: the same, no refusal.
  - `learned` e2v4 + static guard on: the same, no refusal.
  - Estimate times (the GPU was shared with other jobs, so the readings are noisy and only
    the ordering is meaningful): V1 ~0.4-0.75 ms; e1v4 ~0.6-1.1; e2v4 ~1.4-2.8 (a few
    readings up to ~6 while the GPU clocked up). The library's quiet-machine figures are in
    the cost bullet above.
  - Not exercised: a live switch between nets from the overlay (the library handles it as a
    runtime `setSettings()`; the host's key change is the same path as a quality switch), a
    real game, HDR / 10-bit content, any visual comparison of the output.

Round 1 (kept for reference):

- `scripts/gentle-build.sh --test`: 19446763 assertions in 681 test cases, all passed.
- Compiled in: the `ml_*` headers and `ml_weights.h` with both round-1 nets.
- Runtime, vkcube at 30 fps: `learned` (e2v3, fp32) generated at 2.01x, estimate 0.68 ms vs
  0.30 ms for V1 (500x500 client); `learned` + e1v3 + half + `quality: performance` + Motion
  blur with its own `learned` generated at 2.01x with 4 blur samples, estimate 0.41 ms.

## Known issues

The library's round-2 list applies (see FGR's `ML-FG-TESTING.md`). Round 1's static-HUD warp
(h) is **largely fixed** by the zero test and the old (h) is now (g)/(f) there: the residue is
notched glyph edges, and the guard does not fix a HUD on fast-moving scenery. Still open:

- (a) UI-protection quality tests fail for Learned in the library's synthetic suite
  (Crosshair V2 leaves more pixels around the crosshair than with V1);
- (e) the whole-frame duplicate test also holds a small moving cursor on an otherwise still
  screen (not interpolated);
- (h) obs1 13100: e2v4 / e1v4 are ~2 dB below V1 on that pair;
- by design the static guard is applied once to a Motion blur average, not per sample;
- Quality's Performance is ignored while Learned runs (the nets need flowScale 2).

Not tested here: a real game, NV12 / P010 (not built into this fork anyway).
