# Motion blur (`image.motionblur`)

Blends several in-between frames into each frame that is shown, like a camera shutter.
It is the library's **interpolation-based motion blur**: the same optical-flow estimate
frame generation uses, sampled at several instants inside a short window and averaged --
the technique of video tools such as Blur and smoothie-rs, done live in the compositor.
Added 2026-10-04 (frame-gen-ritz `2eca330`, its PR #2 for the blur and PR #3 for the
pacer that plans it). Off by default, per profile, rail group **MOTION** (second area, below
[Frame generation](frame-generation.md)). Works **with or without** Frame generation.

The user's idea, quoted by the library: *"interpolated motion blur, which people basically
achieve by, for example, interpolating a 60 FPS video to 240, and then blending the frames
together to get like really accurate motion blur."*

Code map:

| What | Where |
| --- | --- |
| The blur primitive (`recordSynthBlur`, `Settings::blurSamples/blurWeights`, 3 shaders) | `subprojects/FrameGen/gpu/framegen.{h,cpp}`, `shaders/blur_*.comp` |
| The window planner (`BlurWindow`, `Inputs::blur*`, `Decision::t0/t1`) | `subprojects/FrameGen/gpu/pacing.h` |
| Renderer host: the one renderer call, ring, cost harvest | `src/FrameGen/FrameGenHost.{h,cpp}` (`fghost::BlurConfig`, `SetBlurConfig`, `Active()`) |
| Pacing glue (feeds the pacer, applies the decision) | `src/steamcompmgr.cpp` (`FrameGen_PrePaint`) |
| Settings area | `src/Overlay/PanelMotionBlur.{h,cpp}` |
| Config | `src/Config/ConfigSchema.h` (`MotionBlurSettings`), `ConfigManager.cpp` (`motion_blur`) |
| Tests | `tests/test_config.cpp` (the `motion_blur` keys); the planner is tested in the library |

## The architecture rule, and the composition model

The user: *"you're basically only building the GUI in this chat and most of the stuff
should go into the frame gen itself"*, and about the features: *"all of them are kind of
separate, but they can all be turned on at the same time, and they actually act nicely
together"*. So gamescope has **no blur logic of its own**: it maps five settings onto the
library, feeds the library's pacer, and applies what it decides.

Three features share **one content timeline**, each owning **one dimension** of it
(`gpu/pacing.h`, "THREE FEATURES, ONE CONTENT TIMELINE"):

| Feature | Owns | In gamescope |
| --- | --- | --- |
| Lag-spike buffer | how far behind real time we show | the Lag spike buffer switch -> `Inputs::lagBuffer` ([lag-spike-buffer](lag-spike-buffer.md)); `extraDelayNs` stays 0 |
| Frame generation | which instants get an output frame (the cadence) | the Frame generation switch -> `Inputs::frameGen` |
| Motion blur | what each output shows over time (the shutter window `[t0, t1]`) | the Motion blur switch -> `Inputs::blur*` |

Every combination falls out of the one planner: FG off + blur on = one blurred output per
real frame ("real-rate" planning, game rate in, game rate out, no delay); FG on + blur on =
every generated output is blurred over its own window; both off = pass-through, renderer
inert. gamescope runs the pacer whenever **any of the three** is on (`fghost::Active()`), applies the
`Decision` with the one renderer rule (see [frame-generation](frame-generation.md)'s API
note): `prevId != 0` -> `recordSynthBlur( prev, curr, out, t0, t1 )` (a plain synth when
`t0 == t1`, a blurred real frame when the window ends at 1); `prevId == 0` -> show the real
frame `showId`.

`Why` the window **trails** (always ends at the instant the output shows): it needs no
future frame, so **blur adds no delay**. A centred window would need the next real pair for
the output that is the real frame itself.

## Settings

Area `image.motionblur` ("Motion blur"), per profile (game profiles inherit), no keybind,
config section `motion_blur` (schema stays 5; additive; absent loads the defaults, which are the user's hand-tuned Test-profile values, 2026-10-04: the defaults below are those, and switches stay off).


| Key | Values | Meaning |
| --- | --- | --- |
| `motion_blur.enabled` | `false` (default) / `true` | the row "Motion blur" (switch) |
| `motion_blur.samples` | 2..8 (default 4) | the row "Samples": sub-frames averaged per shown frame; **every whole number 2..8** (the user asked for it explicitly, not just 2/4/8); clamped on load (the library takes up to 16, the UI offers 8) |
| `motion_blur.amount` | 0..100 (default 50) | the row "Blur amount": the shutter in percent of one interval (`Inputs::blurAmount` = `amount / 100`); 0 is no blur; clamped on load |
| `motion_blur.relative` | `shown` (default) / `game` | the row "Measured against": **Shown frame** = the shutter is a fraction of one *shown* frame's interval (subtle; stays short as FG shows more frames); **Game frame** = a fraction of one *game* frame's interval (the film look: longer, heavier, needs more samples) = `BlurRelative::ShownFrame` / `GameFrame` |
| `motion_blur.same_as_framegen` | `true` (default) / `false` | the switch "Use Frame generation's settings": on = the blur follows `framegen.motion_version` / `framegen.quality`; additive, absent loads `true` (2026-10-22) |
| `motion_blur.motion_version` | `v1` (default) / `v2` / `learned` | the row "Motion version": the blur's own version, used only while the switch is off (see "The interpolator's version and quality"); additive, an unknown string keeps `v1`. `learned` (2026-10-26) borrows Frame generation's `framegen.learned_net` / `framegen.learned_half` -- the blur has no net or precision rows of its own ([Learned motion](frame-generation.md#learned-motion)) |
| `motion_blur.quality` | `quality` (default) / `performance` | the row "Quality": the blur's own, used only while the switch is off; on V2 it is V2's mode; additive |
| `motion_blur.weights` | `even` / `gaussian` (default) | the row "Weighting": `BlurWeights::Even` (a box shutter, hard-edged smear like a camera) / `Gaussian` (the ends carry 13.5% of the centre's weight: softer, effectively shorter) |

Rows, in order: **Motion blur** (switch), **Samples** (slider 2..8 step 1), **Blur amount**
(slider 0..100 %), **Measured against**, **Weighting**, then (group "Interpolation") **Use Frame
generation's settings** (switch, default on), **Quality** and **Motion version**, and **Status**.
The other rows are disabled while the Motion blur switch is off; Quality and Motion version are
also disabled while "Use Frame generation's settings" is on (greyed out, showing Frame
generation's values). The rail summary reads `off` or `N samples`. The help
texts are plain ASCII (the overlay font atlas is Latin-1; `->` and no arrows) and carry the
essentials: it blends in-between frames like a camera shutter, works with or without Frame
generation, never waits for a future frame so it adds no delay, costs about 0.03 ms per
sample at 1280x960 and 0.1 ms at 1440p per shown frame, Shown frame is subtle and Game frame
is the film look, UI protection keeps the crosshair sharp inside the blur, and it reduces
clarity so it is not for competitive play.

**The interpolator's version and quality** (2026-10-22). Blur and Frame generation run on ONE
`framegen::Interpolator`, whose Motion version (V1 / V2) and Quality are Frame generation's
`framegen.motion_version` / `framegen.quality` ([frame-generation.md](frame-generation.md#motion-version-2)).
This area has the switch **Use Frame generation's settings** (`motion_blur.same_as_framegen`,
default **on**) and the blur's own **Quality** (`motion_blur.quality`) and **Motion version**
(`motion_blur.motion_version`) rows:

- Switch **on**: the two rows are greyed out and *display Frame generation's current values*
  (read from the renderer's live config, `fghost::GetConfig()`, so this panel never keeps a copy
  of the `framegen` section). The blur runs on Frame generation's pair, also with Frame
  generation off (`EnsureReady()` reads the packed config whatever the Frame generation switch
  says).
- Switch **off**: the blur's own pair is pushed with the blur config (`BlurConfig::useOwnInterp`,
  `motion`, `quality`, bits 14-16 of the blur word) and, while the blur is on, **replaces**
  Frame generation's pair for the interpolator (`RecordBaseLayer()` rewrites the quality and
  motion bits of the packed config before the library-config key is built). Selecting V2 here
  takes effect at once; a V2 the library refuses (a GPU without the subgroup
  operations, a size beyond its limits) falls back to V1 and the Status line below ends with ` · V1: <reason>`.
- `Why no second interpolator for the blur:` every shown frame IS a blurred synth: one
  `recordSynthBlur()` per output, whose sub-frames are what the blur averages. One output cannot
  be V2 for the generated frame and V1 for its blur, so a second instance would only double the
  motion estimate (~0.15-0.3 ms at 1080p) for nothing. The blur's own choice therefore applies to
  the whole picture while the blur is on, Frame generation's frames included (Frame generation's
  own two settings are then not used), at no extra GPU cost. Only in blur-only mode does it
  differ visibly from "follow Frame generation".
- The user, asking for it: *"For motion blur also add an option that is like uh use same
  settings as frame gen and the user can just tick that and then it just reuses the same
  setting and by default it should be on. It should just disable the other two, like gray them
  out."* and, on what unticking does: *"It should really just copy over the settings
  essentially. Like if basically like when I have frame gen on v2 quality, then it should just
  set v2 and quality for motion blur as well."* And before that, why the version is selectable
  here at all: *"Make sure that I can also select the v2 for the motion blur"*. They also
  wanted V1 for the blur next to V2 frame generation (*"When it's turned off, I should be able
  to use V1 for motion blur, for example."*), which the single-synth design delivers only as
  "the blur's pair wins while the blur is on".
- `Why own config keys rather than the framegen ones:` the config merge is per *section*; own
  keys in the `motion_blur` section keep one writer per section. (The settings audit sets the
  two rows with the switch turned off first, a `GATES` entry: with it on they show Frame
  generation's value and a write would not read back.)

The **UI protection** setting lives in the Frame generation area and applies to the blur too,
even with Frame generation itself off. `Why:` UI protection pastes the real pixels of a still
crosshair / HUD *after* the average (the library's `ui_patch`), so the crosshair stays exact
while the world around it is blurred.

**Status line** (always one line): `4 samples · 1.8 ms window` while applied (the window is
the last output's shutter length in ms of content time, the pacer's `Report::blurWindowMs`;
` · V1: <reason>` is appended when Motion version V2 was asked for and refused); `Off`; `Blur amount is 0: no blur`; otherwise the reason (the renderer's first -- YCbCr, an
unsupported pixel format, ... -- then pacing's: warming up, a frame gap, the GPU too slow); `Waiting for
frames` before the first publish. Pacing's reason `Off` (what it reports for a blur-only
plan with frame generation off) is deliberately not shown as a reason.

## Sample positions (library PR #10, 2026-10-04)

The `N` samples sit at the **midpoints of `N` equal slices** of the shutter window
(`t_k = t0 + (t1 - t0)(k + 0.5)/N`, a box filter), not at both ends of it. `Why`: with
both-ends sampling a real frame at a shared end was counted in two neighbouring outputs
whenever windows tile (100% blur), double-weighting it. Every sample is now a synth (plus one
for windows ending at 1); `t0 == t1` / blur off is unchanged. No API change: the host only moved
the FrameGen submodule to frame-gen-ritz `0f8c58e`.

## Costs and limits

Library measurements (RX 7900 XTX, per sample): about **0.030 ms at 1280x960**, 0.052 ms at
1080p, 0.105 ms at 1440p, per *shown* frame; a sample costs about as much as a generated
frame. Scratch memory is about 3 B per pixel per sample at flow scale 2 (N = 8: 29 MB at
1280x960, 88 MB at 1440p), allocated by the first blurred call and only growing. Blur only
with 4 samples and a window ending on the real frame costs about 0.29 ms per real frame at
1280x960 Quality (3 interpolated samples: the newest sample *is* the real frame).

The pacer's **cost guard** ("Limit to GPU speed" in Frame generation, off by default)
budgets the estimate plus `blurSamples` synths per output; the renderer's published synth
time is per *sample* for blurred outputs so that product is right.

Limits: blur averages interpolation errors, it does not hide them -- an artefact in a
sub-frame is spread over the window, softer but wider. It reduces clarity in motion by
design, so competitive players will often want it off or a short shutter. The library's usual
limits apply per sub-frame: RGB only, 8-bit, 10-bit or fp16, SDR or HDR (YCbCr is refused and the status
says why; see [frame-generation](frame-generation.md#hdr-and-10-bit-games)), very fast motion, repetitive structures, scene cuts and flicks present the
nearer real frame. With 2 samples a fast object shows two ghost copies rather than a
ramp -- use more samples for a longer smear.

## Related

[frame-generation](frame-generation.md) · [lag-spike-buffer](lag-spike-buffer.md) · [compositing-vulkan](compositing-vulkan.md)
