# Lag spike buffer (`image.lagbuffer`)

Detects the game's frame-time spikes and runs the picture slightly **behind** real time,
so a repeat of a recent spike is bridged with **generated frames** instead of freezing.
Added 2026-10-04 (frame-gen-ritz PR #4, merged; gamescope pins FrameGen main `2991d45`).
Off by default, per profile, rail group **MOTION** (third area, below
[Motion blur](motion-blur.md)). Works **with or without** Frame generation and combines
with Motion blur.

The user's request: *"some games on Linux are sometimes unplayable because of lag spikes
... in case one frame is missing, it can just fully generate between the lag spike ...
mainly usable for controller games since it will add ... delay. But it could just turn a
game from unplayable into a playable one."*

## The architecture rule

*"you're basically only building the GUI in this chat and most of the stuff should go into
the frame gen itself"*. The detection, the target, the ramp, the gap fills and the history
depth are all the library's (`subprojects/FrameGen/gpu/pacing.h`, `Pacer::Inputs::lagBuffer`);
`INTEGRATION.md`'s "Lag-spike buffer" and "Deep history" sections are authoritative and **this
page does not restate them** beyond a summary. gamescope maps four settings onto
`LagBufferSettings`, feeds the pacer, keeps the host's frame ring and the library's UI history
following the depth the pacer asks for, and prints the pacer's `Report`.

Code map:

| What | Where |
| --- | --- |
| Spike detection, target, ramp, fills, `HistoryDepth()` | `subprojects/FrameGen/gpu/pacing.h` |
| `Settings::uiHistory`, `releaseUnusedUi()` | `subprojects/FrameGen/gpu/framegen.{h,cpp}` |
| Renderer host: `LagBufferConfig`, gate, ring that follows the depth, `uiHistory` sync | `src/FrameGen/FrameGenHost.{h,cpp}` (`fghost::SetLagBufferConfig`, `FollowDepth`, `ResizeRing`) |
| Glue: `Inputs::lagBuffer`, the status line | `src/steamcompmgr.cpp` (`FrameGen_PrePaint`, `FrameGen_ToStatus`, `FrameGen_LogStatus`) |
| Settings area | `src/Overlay/PanelLagBuffer.{h,cpp}` |
| Config | `src/Config/ConfigSchema.h` (`LagBufferSettings`), `ConfigManager.cpp` (`lag_buffer`) |
| Tests | `tests/test_config.cpp` (the `lag_buffer` keys), `tests/test_overlay_ui.cpp` (rail order, icon census, accordion fit) |

## Why a separate tab, independent of frame generation

The user **rejected a magic "1x fill gaps only" multiplier value** in Frame generation. So:
**frame generation off** = real frames are shown, and generated frames appear only inside
gaps; **frame generation on** = gaps are filled as part of the normal pacing; and it
combines with blur. The library's pacer owns the buffer's dimension of the content timeline
(how far behind real time we show) and composes it with the other two, exactly like the other
features ([frame-generation](frame-generation.md)'s "How the three combine").

## The model, briefly (the user's own words, as recorded by the library)

- *"the starters that I'm talking about should be like 1, 2, maybe 3 missing frames, so it
  shouldn't go that high."* -- the default cap, 50 ms, is about 3 frames at 60 fps.
- *"it should take mostly the most recent lag spikes into account. So only like the newest
  biggest lag spike in the last 5 minutes actually affects ... and everything else is just
  smoothed out."* -- the newest big spike in the look-back window dominates, older ones fade
  with a cosine weight and are never hard-dropped inside the window; default look-back 5 min.
- *"single, like huge lag spikes, for example, when you're compiling and drops down to like 10
  FPS, it shouldn't build a 100 millisecond buffer. That would be stupid."* -- a spike larger
  than **Max buffer** is an **outlier**: counted, never sized for.
- *"the user should also be able to set a buffer to something insane like 250 milliseconds,
  but ... default max should be maybe like 50 milliseconds."* -- the sliders go to 250 ms,
  the default is 50.
- *"The amount of frames should be dynamically be determined by the buffer size."* -- see
  "The host ring follows the depth" below.

In short: a spike is an arrival interval above 1.75x the smoothed interval, its size is the
missing time; `target = clamp(1.10 x newest weighted spike + 1 ms, 0, Max buffer)`; the live
buffer ramps toward it at 3 % of real time (content plays at 0.97x while it grows, 1.03x
while it shrinks, so it never jumps); **the first spike of a session still freezes**, because
the buffer reacts. The test mode pins the target at 0 or at Max buffer through the same ramp.

## Settings

Area `image.lagbuffer` ("Lag spike buffer"), per profile (game profiles inherit), no keybind,
config section `lag_buffer` (schema stays 5; additive; absent loads the defaults; clamped on
load).

| Key | Values | Meaning |
| --- | --- | --- |
| `lag_buffer.enabled` | `false` (default) / `true` | the row "Lag spike buffer" (switch) |
| `lag_buffer.lookback_min` | 1..10 (default 5) | the row "Look-back", minutes (`lookbackSec` = 60 x this; the library takes up to 600 s) |
| `lag_buffer.max_ms` | 0..250 (default 50) | the row "Max buffer", ms: the cap on the buffer and the spike/outlier line |
| `lag_buffer.test_mode` | `off` (default) / `min` / `max` | the row "Test mode": Off / Force minimum / Force maximum (`LagTestMode`); an unknown string keeps `off` |

Rows, in order: **Lag spike buffer** (switch), **Look-back** (slider, step 1, `min`), **Max
buffer** (slider, step 5, `ms`), **Test mode** (choice), **Status**. The other rows are
disabled while the switch is off. The rail summary reads `off` or `max N ms`. The help texts
are plain ASCII (the overlay font atlas is Latin-1; `->`, never an arrow glyph) and say: it
detects frame-time spikes and runs the picture slightly behind so a repeat of a recent spike is
bridged instead of freezing; the newest big spike in the look-back sizes it and older ones
fade; spikes above Max buffer (shader compiles) are ignored; the first spike of a session still
freezes; it adds its buffer as input delay, so it is meant for controller and slower games;
audio is not delayed, so above about 50-80 ms lip-sync drifts; memory is about one frame copy
per 16 ms of buffer at 60 fps; test mode forces the minimum or maximum so you can feel both
ends.

**Status line** (always one line): `Off`; the renderer's reason while it refuses (YCbCr, an unsupported format, ...);
`Waiting for frames` before the first publish; otherwise `buffer 32 ms · 4 frames · last spike
28 ms`, with `no spikes yet` in place of the last part, ` · ignored 1 outlier (>max)` when
spikes above Max buffer are in the window, and ` · test: min` / ` · test: max` while a test mode
pins the target. "frames" is the real frames the host keeps (`Report::historyFrames`): what the
memory scales with. Log: `FrameGen_LogStatus` adds a `lag buffer ...` part (delay, target, max,
look-back, test mode, last spike, spikes in window, outliers, frames kept) to the one
`framegen_pacing` info line, written on change of the target, last spike, outliers, frame count,
settings or whether the live buffer has reached its target (not on every ramp step), and to the
periodic debug line.

## gamescope wiring

- **Gate.** `fghost::Active()` is now frame generation **OR** motion blur **OR** the lag spike
  buffer (`kLagEnabledBit` in its own atomic word, `g_uLag`; `UpdateGate()`). Everything keyed on
  it follows: the backends full-composite (no direct scanout), no pre-emptive upscale, the pacer
  is driven, `RecordBaseLayer` tears down only when all three are off. `Why` a third word: the
  frame-generation word is full and the blur word was split off for the same reason. `Enabled()`
  stays frame generation only (the HUD's generated-frame count).
- **Pacer inputs.** `FrameGen_PrePaint` sets `in.frameGen`, `in.blur*` and `in.lagBuffer`
  (`enabled`, `lookbackSec` = minutes x 60, `maxBufferMs`, `testMode`) each from its **own**
  config. `OnArrival` is called for every real frame of the focused window while the pacer is
  active, so spike detection also runs while the buffer is 0 and the plan is a pass-through
  (the library's host duty (1)).
- **The host ring follows `HistoryDepth()`** (`fghost::FollowDepth`, once per active paint,
  `FrameRequest::historyDepth`; never sized from Max buffer). **Grow**: at once, every paint
  the depth rises (the pacer asks ahead of the ramp; up to `kHistoryMax` = 65), allocation only,
  no GPU wait; the new empty slots are inserted at the **oldest** end (`ResizeRing` moves the Rc
  pointers in age order, so the depth is gained immediately instead of after a wrap-around). A
  failed grow is not fatal (a pair or delayed frame missing from the ring shows the newest real
  frame; logged once, not retried until the depth drops). **Shrink**: only after the wanted depth
  has stayed at least 2 below the ring for 4 s (the library's own depth has already dwelt 2 s);
  then one deliberate `g_device.waitIdle()` ("WAITIDLE 4 of 4"), the oldest slots are dropped,
  and `Interpolator::releaseUnusedUi()` frees the library's unused clean copies at the same
  point. Why a wait and a dwell: freeing a slot a composite in flight may still read needs the
  GPU idle, and a wobbling depth must not stall repeatedly. Teardown (all three off) and a
  resolution change free everything anyway.
- **`uiHistory` follows too.** With UI protection on, `Settings::uiHistory` = the depth
  (clamped 3..65), set through `setSettings` before this paint's observe; growth allocates
  without a wait and returns false (nothing changed) on failure, which is logged and not retried
  until the depth drops; lowering frees nothing. With protection off it stays at the default 3.
  `ToLibrarySettings()` carries the current value so a preset change does not reset it.
- **Library PR #5/#6 absorbed**: `fghost::PassReason::WaitingForDecoder` mirrors
  `fgpacing::Reason::WaitingForDecoder` (text "Waiting for the next video frame"; never occurs
  for gamescope's arrival mode, static_asserted); `OnFrame` and the format variants (Rgb10,
  RgbaF16, Nv12, P010) are used for Rgba8, Rgb10 and RgbaF16 since 2026-10-04 (HDR frame generation, see
  [frame-generation](frame-generation.md#hdr-and-10-bit-games)); Nv12 / P010 are not built.

## Costs and limits

- **Latency**: the live buffer in ms is added as input delay, by design: meant for controller and
  slower games. At most Max buffer.
- **Audio** is **not** delayed. Up to about 50 ms nobody sees the drift; above roughly 50-80 ms
  lip-sync becomes visible (Force maximum at 250 ms is for feeling the buffer).
- **Memory** per buffered frame: the host keeps `HistoryDepth()` real frames (4 B/px at 8-bit
  RGBA and 10-bit, 8 B/px at fp16 / scRGB: 4.9 MB at 1280x960, 14.7 MB at 1440p) and, with UI protection, one clean copy per frame
  (4 B/px, WholeScreen +1 B/px of mask). About one frame per 16 ms of buffer at 60 fps; 250 ms
  at 60 fps = 17 frames. It scales with the *current* buffer, so it is only paid while it is up.
- **Composite**: like the other two features, an enabled lag spike buffer forces a full composite
  (no direct scanout) for as long as it is on, even while its delay is 0.
- **GPU**: the fills are ordinary synths; no work when the buffer is 0.
- **Known, library-side (measured 2026-10-04, headless sway, nested `-r 120`, vkcube held at 30
  fps)**: with frame generation **off** and the live buffer above 0, the pacer's real-rate plan
  repeats the last output at every vblank until the next real frame is due (`PaintRealRate`'s
  `Repeat()` never sets `Decision::skip`), so the compositor composites at the refresh rate
  (about 123 surface commits/s) for a 30 fps game; with the buffer at 0, with blur only, or with
  frame generation on (cadence skips) it is 30 / 30 / 60. Output is unchanged, only the GPU
  work. The fix belongs in the library (`d.skip = bSkippable && newestId == m_Last.newestId`
  in that branch, so a new real frame is still copied into the ring).
- **Known, library-side**: before the first game interval is measured `HistoryDepth()` is
  computed against a tiny default interval, so a forced maximum asks for a ring of about 27 on
  the very first paints (100 ms) and settles to 5-6 after the 2 s + 4 s dwells (the smoke log
  shows `frame ring: shrunk from 27 to 6 frames`). A buffer sized from spikes starts at 0 and does
  not do this.
- Limits are the library's: RGB only (8-bit, 10-bit, fp16; SDR or HDR); the first spike freezes; a game-rate change looks
  like a few spikes for about 4 frames.

## Related

[frame-generation](frame-generation.md) · [motion-blur](motion-blur.md) ·
[build-and-tooling](build-and-tooling.md)
