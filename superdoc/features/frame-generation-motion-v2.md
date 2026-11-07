# Frame generation: Motion version 2

Part of [Frame generation](frame-generation.md). The library's "morphing" second motion algorithm.

## Motion version 2

(2026-10-22, the library's **`framegen-v2` branch at a5a5991**, not its `main` -- the submodule
is pinned there for testing; `main` stays untouched until the user judges V2.) The library has a
second motion algorithm next to the original (V1, unchanged): V2 is a "morphing"
interpolator. It moves the picture along a smoothed motion field and fills gaps with stretched
content; there is **no per-pixel hold** (no trust ramp -- only the whole-frame scene-cut /
flick fallbacks show the nearer real frame), so individual frames can look distorted but the
motion reads better. **Same latency as V1** (a synth reads only the current pair).

- **Rows**: **Motion version** (V1 default / V2, `framegen.motion_version` `v1` / `v2`) and
  **Quality** (`framegen.quality`, below), enabled only while the Frame generation switch is on.
  Per profile, additive (schema stays 5): absent loads the default, an unknown string keeps it.
  `Settings::v2StaticMask` stays at the library default (on) and is not exposed: it keeps HUD,
  text and semi-transparent panels / blended crosshairs in place without needing UI protection,
  in both modes.
- **Motion blur uses the same interpolator** ([motion-blur.md](motion-blur.md)):
  it follows these two settings by default (its "Use Frame generation's settings" switch, on)
  and runs with them when Frame generation is off. With that switch off and the blur on, the
  blur's own pair (`motion_blur.motion_version` / `motion_blur.quality`) replaces these two for
  the whole picture. The merge is host-side, in `RecordBaseLayer()` (it rewrites the quality
  and motion bits of the packed config word before the library-config key is built, so
  `EnsureReady()` just sees the effective values). `Why no second interpolator:` every shown
  frame is one `recordSynthBlur()` call (the blur averages the sub-frames of the very synth
  that makes the frame), so one output cannot be V2 for the generated frame and V1 for its
  blur; two instances would only double the motion estimate for nothing. The blur's choice
  therefore also decides the frames Frame generation makes while the blur is on.
- **One Quality for both versions** (2026-10-22). `framegen.quality` drives V1's preset
  (Performance = flowScale 4, sub-pixel off) **and** V2's mode (`framegen::V2Mode`; Quality ->
  Quality, Performance -> Performance); the earlier separate `framegen.v2_mode` / "V2 mode" row
  is gone (never released: no migration, an old key left in a profile file is ignored on load).
  `Why:` the user, seeing two identical selectors: *"Right now we have quality with the options
  quality and performance and v2 mode with quality and performance. This could realistically be
  a single selector for just quality and performance, and just pass it the same way for v1 and
  v2 since both of them have the same options."* Library numbers (the development GPU, 1080p 2x, ms per
  real pair): V1 0.30, V2 Quality 0.24, V2 Performance 0.14. V2 Quality adds a per-pixel
  edge-aware field and a coverage pass (which frame still shows each spot); Performance is the
  grid morph only.
- **No hitch on a Quality change while V2 runs.** A V1 quality change is `reconfigure()` (a GPU
  wait; the estimate is dropped), a V2 mode change is a plain `setSettings()` that keeps the
  estimate. So while V2 runs, `EnsureReady()` ignores V1's quality: V2 always estimates at the
  library's own flowScale 2 / sub-pixel (the settings its numbers are measured at), and the
  Quality row is only V2's mode, free. V1's flowScale 4 is applied when the version goes back
  to V1, and a V1 Performance -> V2 switch applies flowScale 2; both are version switches, which
  drop the estimate anyway, so the one `reconfigure()` wait costs nothing extra. At start V2 is
  not switched on in `init()` but by the same change block on the first frame
  (`uAppliedConfig` left unequal on purpose), so the pinning is in one place.
- **Version switches at runtime** through `Interpolator::setSettings()` (or `reconfigure()` when
  the flowScale has to change, above). A **version** switch drops the estimate: `EnsureReady()`
  sees the library's `settings().motionVersion` change, clears `bEstimateValid` and the cached
  outputs, so the next paint records a fresh estimate with the new version (until then the real
  frame is shown; the library's `recordSynth*` would return false). The first switch to a version
  builds its pipelines on the spot. The config word `g_uConfig` carries the version at bit 28
  and the applied-config key includes it.
- **Every format V1 runs (since library a5a5991, 2026-10-22), plus a subgroup requirement, with
  V1 as the fallback.** Until a5a5991 V2 refused any `Format` but `Rgba8`; now it supports Rgba8,
  Rgb10, RgbaF16 (encoded and linear), and the library's Nv12 / P010, in both modes, with the
  static mask, motion blur and every UI protection mode. (This build offers Rgba8, Rgb10 and
  RgbaF16: Nv12 / P010 are deliberately not built, see "HDR and 10-bit games".) What still makes
  the library refuse V2 is a GPU without compute subgroups (basic / vote / arithmetic / shuffle /
  clustered, at least 16 lanes), a format whose four V2 synthesis variants are not built in, or a
  size beyond a V2 buffer limit. `InitInfo::getProps2` is
  `g_device.vk.GetPhysicalDeviceProperties2`, which lets the library check the device (and read its
  size limits). `Why a fallback and not "unavailable":` the library's `init()` refuses V2
  outright on such a device, which would have killed frame generation just because the profile
  says V2. So `init()` always starts V1; `ResolveMotion()` then asks
  `Interpolator::settingsError( want )` (format variants + device rule + the held size under V2)
  and, if it names a reason, runs V1 instead. The reason is a library string literal: it goes to the log
  (`Motion version 2 refused, running V1: ...`, once per change) and to
  `RenderStatus::motionNote`, which the Frame generation Status line appends while generating and
  the Motion blur Status line appends while the blur is applied (` · V1: <reason>`, also with
  Frame generation off).
  A format change re-inits, so an HDR game and an SDR game each get their own answer.
- **Sizes** (library `Interpolator::sizeError( w, h )`): 16 px up to the device's limits (image
  dimension, 2^30 pixels, `maxStorageBufferRange`, workgroup count). The host's own gate for the
  16 px floor stays (`Unavailable::TooSmall`, before any library call); the one place the library's
  reason text is used is the log line when a resolution change makes `resize()` refuse
  (`could not resize to WxH: <reason>`). `init()` already logs `initError()`, which carries the
  same reason.
- **Unused-version memory.** `releaseUnusedMotion()` is deliberately not called: it needs the
  retire rule (a GPU wait) and there is no natural spot (the ring-shrink wait is rare and
  unrelated). The unused version's pipelines / buffers stay until teardown (`// ponytail:` in
  `EnsureReady()`).
- **UI protection is unchanged**, `Crosshair V2` included (the library says V2 motion works with
  every `uiProtection` mode and with `recordSynthBlur`, so motion blur works too). V1 output
  is byte-identical (the default is V1).
- Shaders: 13 plain `v2_*` compute shaders in `src/meson.build`'s `framegen_shaders`
  (`v2_medhist`, `v2_search`, `v2_clean`, `v2_morph`, `v2_repick`, `v2_qfield`, `v2_qown`,
  `v2_qcov`, `v2_qmorph`, `v2_static`, `v2_enclose`, `v2_morph_s`, `v2_qmorph_s`: the Rgba8 set)
  and, since a5a5991, four per-format variants each for `rgb10` and `f16` -- `v2_morph`,
  `v2_morph_s`, `v2_qmorph`, `v2_qmorph_s` (-DFG_FORMAT_<FMT>=1), in the same list as V1's 12
  colour shaders (16 per format). They are the only V2 passes that read or write colour; the
  other nine read the luma and are one shader for every format. Without a format's four, the
  library refuses V2 there with a reason and V1 still works (so a stale incremental build dir
  shows up as the Status line's ` · V1: ...`; `FrameGenLib.cpp` is the file to touch).

## History

- **Motion version 2** (terminology entry, moved here 2026-11-07): **Motion version 2** (V2) — the FrameGen library's second motion algorithm (2026-10-22, library branch `framegen-v2`; row **Motion version** V1 / V2 (`framegen.motion_version`); the **Quality** row, `framegen.quality`, also picks V2's Quality / Performance mode — there is no separate V2 mode setting, 2026-10-22. Motion blur has its own pair and a "Use Frame generation's settings" switch (default on, `motion_blur.same_as_framegen`) that follows Frame generation's; both features share ONE interpolator, so with the switch off the blur's own pair applies to the whole picture while the blur is on): a "morphing" interpolator with no per-pixel hold. Works in every game format this build runs (8-bit, 10-bit, HDR, fp16; library commit a5a5991) on a GPU with the needed subgroup operations; where the library refuses it (e.g. no subgroups) V1 runs and the Status line gives the library's reason. Unrelated to **Crosshair V2** (see [frame-generation-ui-protection.md](frame-generation-ui-protection.md)) and to **Adaptive Brightness V2** (see [shader-effects-adaptive-brightness-v2.md](shader-effects-adaptive-brightness-v2.md)).
