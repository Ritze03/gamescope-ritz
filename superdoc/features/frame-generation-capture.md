# Frame generation: the capture (`ritz_fg_capture`)

A troubleshooting aid, not a feature of the settings UI: it records a burst of generated frames
with everything needed to replay them through the FrameGen library offline. Part of
[Frame generation](frame-generation.md); the code is `src/FrameGen/FrameGenCapture.{h,cpp}` (no
Vulkan, unit tested) and `FrameGenCaptureSession.cpp` (GPU copies, writer thread, the console
command), wired in at `RecordBaseLayer()` in `FrameGenHost.cpp` and `FrameGen_PrePaint()` in
`steamcompmgr.cpp`.

## What it is for

A defect of generated frames (for example single generated frames going dark) that the library
cannot reproduce offline needs the exact inputs of the frames around it: the same pair of real
frames, the same `t`, the same settings. Nothing else in the host records those. Off unless
triggered; no overlay row, no profile setting, no keybind (the keybind system has UI rows and docs
a temporary debugging aid should not touch).

## Triggering it

```
gamescopectl ritz_fg_capture            # start now
gamescopectl ritz_fg_capture <seconds>  # start that many seconds later (0..600)
```

The delay is for typing it in a terminal, going back into the game and reproducing the defect.
A second call while a capture is armed, running or still writing is refused with a log line.
The command runs on the console thread and only sets atomics (state and the start time in the
monotonic clock the paint path uses); everything else happens on the paint thread.

`gamescopectl` connects to the Wayland socket named by `GAMESCOPE_WAYLAND_DISPLAY`, default
`gamescope-0`, in `XDG_RUNTIME_DIR`. A gamescope started from the desktop (for example by a game
launcher) opens `gamescope-0`, or `gamescope-1`... when that name is taken (the first free slot); a terminal
on the same desktop session reaches it without any setup, and with two instances it needs
`GAMESCOPE_WAYLAND_DISPLAY=gamescope-1 gamescopectl ritz_fg_capture`. The variable is only
exported to the game's own environment, so a terminal does not have it set.

## What a capture records

A burst of the next **N = 40** (`kFrames`) generated frames after the start. A "generated frame" is
a composite that shows a synth output: a fresh synth, or a cached output slot being repainted
(`cached=1`, same `outId`, no synth that composite). For each one:

1. the generated frame exactly as the library returned it (the output slot's bytes, before
   effects, FSR, overlays and colour management);
2. the two real frames of its pair, as the host's ring copies of them; each real frame is stored
   **once** per capture (many generated frames share a pair), keyed by commit id;
3. one `frame` line in `capture.txt`.

Plus, once: the capture header, every field of the library's `framegen::Settings` in effect, and
the pacer's last ~200 decisions before the start.

If the frame generation is off (no live renderer), an armed capture waits for it up to 10 s after
its start time, logs "capture not started" and writes nothing. If the renderer is live but the
pacer only shows real frames (pass-through), the capture starts and, when no generated frame
comes for 10 s (`kTimeoutNs`), ends with `reason=timeout` and the header, settings and decisions
only.

## Early preparation

The expensive part (free-space check, allocating the staging, creating the directory) is not done at the start time: with
`ritz_fg_capture <seconds>` it runs on the first composite after arming at which the renderer is live (`prepare_ms` is
logged as "capture prepared: ... starts in N s" and written to `capture.txt`), and the start itself only flips state.
`Why:` at 1440x1080 the staging is about 0.5 GB and its allocation takes about 40 ms; a stall that long (and a longer
one on a busy machine) landing at the start of a burst that lasts 0.15 s at 270 fps would make the pacer drop its history
("Frame gap") and show real frames while it warms up, so the burst would record the recovery from the capture's own stall
instead of the steady state the user was reproducing. The immediate form prepares and starts in the same composite
(and logs its preparation time). If the game's size or format changes between preparation and start, the prepared staging
is dropped (its empty directory removed) and prepared again; when frame generation is torn down while armed nothing is
started (no live renderer) and the prepared staging is freed by the existing 10 s "not started" timeout, or compared with
the new size when the renderer is live again. A change of settings between preparation and start is harmless: the settings
dump is taken at the start.

## Where the files land

`${XDG_CACHE_HOME:-$HOME/.cache}/gamescope-ritz/fg-capture/<YYYYMMDD-HHMMSS>/` (created with
parents; a `-1`, `-2`... suffix when the directory name is taken). The full path is logged at info
level when the capture starts and again, with the counts, when it has finished writing.

Before anything is allocated or created, free space on that filesystem is checked with `statvfs`:
`(N + N + 2) x width x height x bytes-per-pixel + 64 MiB` (`RequiredBytes()`); if short the capture
is refused with a log line and nothing is written. If a write fails mid-way the capture stops,
writes the `end` line with `reason=disk` where it can, and logs it.

## File formats (what a replay tool reads)

Raw image files, the image bytes as they are in the image, **tightly packed rows**
(`width x bytes-per-pixel` per row, no row padding), no header, no conversion:

- `NNN_out.bgra` -- generated frame number NNN (3 digits from `000`);
- `id<commit id, decimal>_real.bgra` -- a real frame.

The extension is fixed; the pixel format is the one in the `capture` line (`vk_format`,
`format_name`, `bytes_per_pixel`). For an ordinary 8-bit game that is `B8G8R8A8_UNORM`, 4 bytes per
pixel: B, G, R, A in memory. 10-bit and fp16 games are stored in their own format (4 and 8 bytes).
Every image file is exactly `width x height x bytes_per_pixel` bytes.

`capture.txt`: UTF-8, one record per line, ending with a newline; every line is a kind word, then
`key=value` tokens separated by single spaces (no value contains a space). Order: one `capture`
line, the `setting` lines, the `decision` lines (oldest first), one `frame` line per captured
frame in `seq` order, one `end` line. A `frame` line is written only after its three files are
complete.

```
capture v=1 start_wall=<local time, %Y-%m-%dT%H:%M:%S%z> start_mono_ns=<n> width=<w> height=<h>
        vk_format=<VkFormat number> format_name=<name> bytes_per_pixel=<n> pitch=tight
        library_commit=<short hash or unknown> ritz_version=<About version> ritz_commit=<hash>
        ritz_dirty=<0|1> motion_wanted=<V1|V2|Learned|V3> motion_effective=<...> fallback_v1=<0|1>
        flowScale=<n> n=40 prepare_ms=<float> prepared_early=<0|1>
```
(one line; `fallback_v1=1` when the library refused the wanted motion version and runs V1;
`library_commit` is the submodule's HEAD at configure time; `prepare_ms` is how long the space check, the staging allocation and the directory creation took, `prepared_early=1` when that was done when the capture was armed, before the start time.)

```
setting <name>=<value> [text=<enum name>]     # one per field of framegen::Settings (44 today)
```
Floats are printed with 9 significant digits (exact round trip), bools as 0/1, enums as their
integer plus `text=` (`uiProtection`, `blurWeights`, `motionVersion`, `v2Mode`, `v3Guard`,
`learnedNet`). A static_assert on the struct's size in `FrameGenCapture.cpp` fails the build when a
library bump adds a field, so the dump cannot silently fall behind.

```
decision n=<counter> ns=<monotonic ns> newestId= prevId= currId= outId= showId= t= t0= t1=
         inert= skip= reset= repaintNext= newOutput= discontinuity= generate= reason=<n> reason_text=<name>
```
One per pacer `Decision` (every paint that ran the pacer), from an always-on ring of 256, the last
200 written. `generate=1` means a pair is named, `t < 1` and the renderer is neither inert nor
skipped. `discontinuity=1` is a paint with no decision at all (layer 0 was not a plain game frame;
the renderer stays inert). **`reason` is the reason of the pacer's last pacing status**, published
about every 250 ms, not of this decision: the `Decision` struct carries none. Values are
`framegen::pacing::Reason` (0 Normal, 1 Off, 2 WarmingUp, 3 CostGuard, 4 GameTooFast, 5 GameStalled,
6 RendererUnavailable, 7 WaitingForDecoder).

```
frame seq= outId= prevId= currId= newestId= t0= t1= arrival_prev_ns= arrival_curr_ns= now_ns=
      width= height= estimate=<recorded|reused> estimate_ret=<1|0|na> synth_ret=<0|1> cached=<0|1>
      near=<0|1> out_slot= prev_slot= curr_slot= out_file= prev_file= curr_file=
```
`near=1` (2026-11-21) is the over-the-cap repaint: past `kMaxSynthsPerPair` synths of one pair the
host shows the most recently used output slot again (`pNear` in `RecordBaseLayer()`); its `outId`,
pair and `t0`/`t1` are that output's own (the output slots remember them), not the request's. It
counts toward the 40.
`t0`/`t1` are the arguments of `recordSynthBlur` (equal when blur is off). The arrival times are
the host's `OnArrival` timestamps of the pair's two real frames (ns, monotonic, same clock as
`now_ns` and the decisions), `-1` when the id is no longer in the host's small table (the last 16
ids). `estimate=recorded` means `recordEstimate` was recorded in this very composite and
`estimate_ret` is its return value; `reused` means an earlier composite's estimate served this
output (`na`). `synth_ret` is `recordSynthBlur`'s return (a frame is only captured when it
succeeded, so 1). Slots are the host's output slot (0..2) and ring slots.

```
repeat kind=<repeat|static> id=<new> same_as=<old>     (zero or more)
repeat_check reals=<checked> repeat=<n> static=<n>
```
Written before `timing` (2026-11-21): the repeat test (see "The repeat test" below) over the stored
real frames in the order they were first needed. A `repeat` line means a real frame's image equals
an earlier one's although a different frame came between: a ring slot carried a new commit id over
an old image.

```
timing first_frame_ns= last_frame_ns= burst_ns= max_decision_gap_ns= gap_after_n= last_decision_to_start_ns=
```
Written once, just before `end`. `first_frame_ns` / `last_frame_ns` are `now_ns` of the first and last captured `frame`
(`burst_ns` their difference, 0 with no frame). `max_decision_gap_ns` is the largest gap between two consecutive `decision`
times of the lines above (`gap_after_n` the counter `n` of the decision that ended the gap), and
`last_decision_to_start_ns` the gap from the last of them to `start_mono_ns`: together they show at a glance whether a stall
preceded the burst (the pacer drops its history over about 100 ms).

```
end frames=<n> reals=<m> dropped=<k> reason=<complete|timeout|disk|settings|resize|format|teardown|shutdown>
```
`dropped` counts generated frames that could not be recorded completely (no partial record is ever
written) plus recorded ones the writer could not finish. `settings`: a setting of frame generation,
motion blur or the lag buffer changed during the burst, so the records no longer share one
settings dump; `resize` / `format` / `teardown`: the renderer was rebuilt; `shutdown`: the process
was exiting.

## The correctness rule

The three images of one record must belong together and to the `frame` line. An output slot is
reused after three synths, a ring slot after three real frames, so **nothing is ever read back
from a live slot later**. Instead, in the same command buffer as the synth (right after it) the
host records copies of the output slot, and of each real frame of the pair this capture has not
stored yet, into staging images owned by the capture (`CVulkanCmdBuffer::copyImage`, which does the
layout handling and barriers; the ring and output textures carry `TRANSFER_SRC` for this). A cached
output being repainted is copied the same way. The staging images are host-visible linear images
of the same format (the screenshot path's mechanism): N for outputs, N + 2 for real frames,
allocated when the capture is prepared (see "Early preparation") and freed when its writer is done. About 0.5 GB for a 1440x1080
8-bit burst. If the staging runs out, a copy cannot be recorded, or a cached frame's pair is gone
from the ring and was never stored, the frame is counted in `dropped` and skipped entirely.

After the composite that carries the copies is submitted, its records go to the writer thread with
the submission's sequence number. The writer waits for the timeline counter (`completedSeqNo()`,
the box preview's pattern, never a device wait), then writes the files row by row from the mapped
staging (dropping the row pitch) and then the `frame` line. Texture reference counts are only touched
on the paint thread; the writer sees raw pointers that stay valid until the session is freed.

## Cost

Always on, with no capture: one small POD pushed into a fixed ring per pacer decision (a mutex held
for a copy of about 80 bytes; the console thread never takes it), one table write per real frame's
arrival, and one relaxed atomic load per composite. No allocation. Extra, once for everyone: the
ring and output textures were created with `TRANSFER_SRC` usage. During a burst: the staging
allocation at the start, a texture-to-texture copy or two per generated frame, a stutter is
acceptable. The renderer entry point is also called while a capture is armed, so a capture that
cannot start can time out.

## Ending early, and limits

Ends cleanly (staging freed, thread joined, what was recorded still written) on a changed setting,
a resize, a format change, frame generation being torn down, a disk error, a timeout or process
exit. One capture at a time. Only a frame in the host's ring can be copied: the pair of a repainted
cached output is normally still there. The decisions' `reason` is up to 250 ms old (see above).
The capture does not record the host's later stages (effects, scaling, overlays).

## Watch mode (`ritz_fg_capture watch`, 2026-11-21)

```
gamescopectl ritz_fg_capture "watch [seconds] [sensitivity]"
```
`gamescopectl` sends everything after the command name as ONE argument string, which the server
splits on spaces, so the words after `ritz_fg_capture` must be quoted together (an unquoted
`watch 30` reaches the server as `watch` alone and runs the default). `seconds` 1..600, default 60;
`sensitivity` a number >= 0, default 1, that divides the pixel trigger's threshold; `0` makes every
measured frame trigger (to test the dump path live). Starts at once (staging prepared on the next
live composite, as the burst does); refused while any capture is armed, running or writing.

A burst catches the next 40 generated frames, about 0.14 s at 300 presented fps, so an
intermittent defect needs luck. A watch observes **every** composite that shows (or tries to show)
a generated frame for the whole time and saves the frames around a suspicious one by itself.

**Telemetry.** One `rec` record per such composite, kept in memory (reserved for
`kWatchRecordsPerSecond` = 500 per second; more are counted as `dropped_records`) and written to
`watch.txt` at the end: fresh synths, cached repaints, the over-the-cap repaint (`near=1`) and
refused estimates / synths (no image).

**Host-state flags** (`flags=`, `-` for none; each kind counted in the summary): `gap` (`currId !=
prevId + 1`), `back` (`currId` or `outId` smaller than in the previous record), `near` (the
over-the-cap repaint), `stale` (the estimate is reused although a ring slot of its pair was written
since it was recorded: the host stamps each ring slot on every write, moves the stamps with the
textures in `ResizeRing()`, and the estimate remembers its two stamps), `refused` (`recordEstimate`
or the synth returned false), `pairmismatch` (the host's estimate pair is not the requested pair, a
cached output was generated from another pair than the one now requested under its `outId`, or an
id of the pair is held by two ring slots), `repeat` / `static` (the repeat test, below). The first
three and `static` can be routine; `stale`, `refused`, `pairmismatch` and `repeat` should never
happen and trigger a dump. Detection only: the host's behaviour is unchanged.

**The pixel trigger ("coarse error").** Per measured frame the output and the plain blend
`(1 - t) * prev + t * curr` of its pair (`t` the middle of `t0`..`t1`) are reduced to 16x16 block
means per colour channel; `coarse` is the mean absolute difference of the two, `pair` the same
measure between prev and curr. It flags when `coarse > (6 + 1.5 * pair) / sensitivity`. `Why:` a
correct frame differs from the blend only by local motion, which block means mostly average away,
while a frame built from a wrong estimate moves whole blocks of wrong content; the allowance grows
with `pair` because more motion between the reals makes a correct frame legitimately farther from
the blend. With full sampling the largest `coarse` of a good frame in a real capture with motion
was 8.5 at `pair` 14.6 (ratio 0.41). The watch estimates each block mean from every 4th pixel in x
and y (16 samples per block, `kMetricStep`), which reads about 1.5-3.5 higher on both measures on
fine game texture (sampling noise); on that capture the sparse ratio stayed at or below 0.33. The
means of a real frame are computed once and reused by every output of it. Channel values are on a
0..255 scale for every format (10-bit / 4, fp16 x 255). The arithmetic is in `FrameGenCapture.cpp`
(`ComputeBlockMeans`, `BlendDiff`, `BlockDiff`, `Threshold`), unit tested.

**The repeat test.** A real frame seen for the first time is compared with the last 16 real frames
(`RepeatDetector`): equal sparse block means AND an equal hash of three full rows (1/4, 1/2, 3/4
height) count as the same image. When a different frame lies between the two (A B A) it is a
`repeat` (dump trigger; the trigger line names both ids); when every frame between is the same too
(A A) it is `static`, a counter only. A truly still picture repeats too, which is why a repeat
needs a different frame between; a picture that alternates between two exact images (a blinking
cursor over a frozen scene) would still be reported as a repeat.

**Rolling window and dumps.** Every recorded composite is also copied into the staging images as
in a burst, but cyclically (`RollingWindow`: 40 outputs, 42 real frames). A real frame's staging
image is reused only when no output still in the window refers to it, and no record leaves the
window before the analysis thread has measured it. When a dump trigger fires, about 12 more
records are kept (`kWatchAfterTrigger`), then the window freezes and is written as a normal v=1
capture folder (`dump-1/`, `dump-2/`, ...; at most `kWatchMaxDumps` = 3 per watch) that the replay
and check tools read as-is, with one extra record kind after the decisions, before the `frame` lines:
```
trigger kind=<pixel|stale|refused|pairmismatch|repeat> record=<watch record index> seq=<frame seq in this dump, -1 if none>
        coarse= pair= threshold=   [id=<new> same_as=<old>   for repeat]
```
Its `end` reason is `trigger_<kind>`; the decisions are those at the freeze. While a dump is
written, telemetry continues but frames are not imaged or measured (`not_measured_dumping`). A
frame whose reals are gone from the ring, or that would push an unmeasured record out of the
window, is skipped whole (`images_skipped`).

**Files.** `${XDG_CACHE_HOME:-$HOME/.cache}/gamescope-ritz/fg-capture/<YYYYMMDD-HHMMSS>-watch/` with
`watch.txt` and the `dump-N/` folders. `watch.txt`:
```
watch v=1 start_wall= start_mono_ns= width= height= vk_format= format_name= bytes_per_pixel= library_commit=
      ritz_version= ritz_commit= ritz_dirty= motion_wanted= motion_effective= fallback_v1= flowScale= seconds=
      sensitivity= threshold=(6+1.5*pair)/sensitivity block=16 sample_step=4 max_dumps=3 window=40 after_trigger=12
rec i= ns= outId= prevId= currId= newestId= t0= t1= arrival_prev_ns= arrival_curr_ns= span_ms=
    estimate=<recorded|reused> estimate_ret=<1|0|na> synth_ret= cached= near= out_slot= prev_slot= curr_slot=
    coarse=<x|na> pair=<x|na> flags=<a,b|->
cost analysed= analysis_avg_ms= analysis_max_ms= images_skipped= pixel_triggers=
summary records= measured= not_measured= not_measured_dumping= dropped_records= dumps= triggers=
        max_ratio= max_record= max_outId= max_prevId= max_currId= max_coarse= max_pair=
        flag_<kind>=... repeat= static= span_known= span_median_ms= span_max_ms= span_max_record=
        span_max_prevId= span_max_currId= span_gt2= span_gt3= span_gt5= span_gt8= reason=
```
`span_ms` is the pair's arrival span (curr minus prev; `na` when an arrival is unknown). The
summary is the point of the mode: `max_ratio` is the largest `coarse / threshold` at sensitivity 1
with that record's ids, so a watch with no trigger still says how close the library's output ever
came to looking structurally wrong; `span_gtK` counts distinct pairs whose span exceeded K times
the median span (a pair spanning a stall). `reason` is `watch_done` (ran its time) or one of the
burst's end reasons. A watch has no inactivity timeout: a paused game is no reason to stop.

**Cost.** Text: one ~400-byte line per record in memory. Pixels: a copy or two per generated frame
(as a burst), and on the writer thread about 0.06 ms of analysis per frame at 500x500 and about
0.2 ms of block means per 1440x1080 image (the staging is `HOST_CACHED`). Free space for three
dumps plus the text is checked first. The same staging as a burst (about 0.5 GB at 1440x1080).

## Why

`Why:` the user's defect is a single generated frame going dark at one moment of a real game; the
library maintainers cannot see it without the exact inputs. Everything is recorded at the host
because only the host knows which real frames the ring held and which `t` the pacer asked for.
The copies ride in the synth's command buffer instead of reading the slots later because the slots
are recycled within three frames. The trigger is a console command with an optional delay (not a
keybind) so a terminal can arm it while the user returns to the game.
