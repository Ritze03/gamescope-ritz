# ML-FG-TESTING: Learned motion in gamescope-ritz

Branch `ml-fg-testing` (local only, based on master `c52a245`; not pushed, not merged).
It integrates frame-gen-ritz's (FGR) opt-in `MotionVersion::Learned` (V1 with its motion
search replaced by a small learned conv net, ml-fg's `e2v3` / `e1v3`). V1 and V2 stay the
defaults; with Learned off nothing changes (V1 output is byte-identical per the library's
own checks).

## What was done

- **Submodule bump**: `subprojects/FrameGen` -> FGR `fd5c163` (FGR's local-only
  `ml-fg-testing` branch).
- **Build** (`src/meson.build`): the six `ml_*.comp` shaders (`ml_dup`, `ml_reduce`,
  `ml_pyr`, `ml_match`, `ml_conv`, `ml_out`), `ml_dup` also as `rgb10` / `f16` variants,
  and a `framegen_ml_weights` custom target that runs the library's
  `gpu/ml/embed_weights.py` to make `ml_weights.h` (both nets compiled in, no runtime file).
  `src/FrameGen/FrameGenLib.cpp` comment touched to force the one recompile.
- **Host** (`src/FrameGen/FrameGenHost.{h,cpp}`): `Motion::Learned`, `LearnedNet`,
  `Config::learnedNet` / `learnedHalf` (packed bits 28-29 motion, 30 net, 31 half; V1 = all
  zero), flowScale forced to 2 for Learned (Quality's Performance is ignored then),
  `ResolveMotion()` generalised: a refused Learned runs V1 and the library's reason shows
  in the Status line (` · V1: <reason>`) and the log (`Learned motion refused, running V1:`).
- **Config**: `framegen.motion_version` accepts `"learned"`; new `framegen.learned_net`
  (`"e2v3"` default / `"e1v3"`) and `framegen.learned_half` (default `false`);
  `motion_blur.motion_version` accepts `"learned"` too. Additive, schema stays 5.
- **Overlay**: Frame generation > Motion version has a third choice, **Learned**; rows
  **Learned net** and **Learned precision** (Full / Half) below it, greyed out unless Learned
  is what runs. Motion blur's Motion version also offers Learned; it borrows Frame
  generation's net and precision (a deliberate simplification, said in the help text).
- Docs: `superdoc/features/frame-generation.md` ("Learned motion"), `motion-blur.md`,
  `build-and-tooling.md`, `superdoc/meta/TERMINOLOGY.md` ("Learned motion"), `CHANGELOG.md`
  (0.14.3). Config test added to `tests/test_config.cpp`.

## How to build

`fd5c163` is **local-only**: it is not on GitHub. A fresh clone / worktree needs the commit
fetched from the local FGR checkout before `git submodule update` can check it out:

```sh
git -C subprojects/FrameGen fetch /home/mo/Schinken/Programming/GitHubProjects/frame-gen-ritz ml-fg-testing
git submodule update --init subprojects/FrameGen
```

(The main checkout's submodule store already has the objects, fetched for this purpose, so
`git submodule update` works offline there too.) The build itself does not need the network
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
- **Enable it**: overlay > Frame generation > Motion version > **Learned** (then Learned net
  / Learned precision). Frame generation must be on, and the game must be below the
  refresh rate for it to generate. Or in the profile JSON:
  `"framegen": { "motion_version": "learned", "learned_net": "e2v3", "learned_half": false }`
  (`"learned_net": "e1v3"` and `"learned_half": true` are the cheaper settings). Motion blur:
  its own Motion version row (with "Use Frame generation's settings" off) or
  `motion_blur.motion_version: "learned"`.
- If the Status line shows ` · V1: ...` the library refused Learned and V1 is running; the
  text is the reason.

## What to watch for

- **GPU cost**: 1080p 2x on an RX 7900 XTX: V1 0.3 ms, e2v3 ~1.3 ms (half ~1.0), e1v3 ~0.5
  ms; 1440p e2v3 ~2.6 ms (half ~1.6). At high game fps (many estimates per second) this adds
  up; try e1v3 / Half if the Status line shows the cost guard biting. Extra GPU memory:
  ~110 MB (e2v3 fp32, 1080p).
- **Static HUD warp** (library known issue (h)): the net has no zero-motion prior, so static
  HUD text over moving scenery gets dragged with the scenery (V1's HUD protection is not
  applied to the net). Watch money counters / menu titles.
- **Duplicate skip** (known issue (f)): two near-identical frames (mean |a-c| <= 0.6 levels
  over the whole frame) skip the net and are treated as still. A small moving cursor on an
  otherwise still screen therefore counts as a duplicate and its motion is not interpolated.
- **Quality's Performance is ignored** while Learned runs (the nets need flowScale 2).
- Crosshair V2 UI protection leaves more pixels around the crosshair with Learned than with
  V1 (library known issue (a)).
- Switching to Learned the first time builds 34 pipelines at once: one short hitch.
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

- `scripts/gentle-build.sh --test` (build-agent/): build OK, **All tests passed (19446763
  assertions in 681 test cases)**, including the new
  `framegen: learned motion, net and precision round-trip, ...` case (15 assertions).
  Only warnings are pre-existing ones.
- Compiled in: `build-agent/src/` contains `ml_dup_spv.h`, `ml_dup_rgb10_spv.h`,
  `ml_dup_f16_spv.h`, `ml_reduce_spv.h`, `ml_pyr_spv.h`, `ml_match_spv.h`, `ml_conv_spv.h`,
  `ml_out_spv.h` and `ml_weights.h`.
- Runtime (nested gamescope under a private headless sway; vkcube held at 30 fps by
  MangoHud, `-r 120`, fixed 2x, throwaway profiles in a temporary `XDG_CONFIG_HOME`, audio
  capture off, the other installed Vulkan frame layer disabled with its own env var; no audio
  played, nothing in `~/.config/gamescope-ritz` touched):
  - `motion_version: "learned"` (e2v3, fp32): generated at 2.01x, no refusal in the log, no
    ` · V1:` note; GPU estimate **0.68 ms** vs **0.30 ms** for the same run with `"v1"`
    (500x500 client), which only fits Learned running.
  - `learned` + `e1v3` + half + `quality: performance` + Motion blur on with its own
    `learned` (1080p output): generated at 2.01x with 4 blur samples, no refusal; estimate
    0.41 ms.
  - Not exercised: a live switch V1 <-> Learned from the overlay (same `EnsureReady()` change
    block as V1 <-> V2), a real game, HDR / 10-bit content.

## Known issues

The library's list applies (see FGR's `ML-FG-TESTING.md`): (a) UI-protection quality
thresholds, (f) whole-frame duplicate test, (h) static HUD warp. Not tested here: a real
game, NV12 / P010 (not built into this fork anyway).
