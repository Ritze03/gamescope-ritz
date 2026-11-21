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
        flowScale=<n> n=40
```
(one line; `fallback_v1=1` when the library refused the wanted motion version and runs V1;
`library_commit` is the submodule's HEAD at configure time.)

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
      out_slot= prev_slot= curr_slot= out_file= prev_file= curr_file=
```
`t0`/`t1` are the arguments of `recordSynthBlur` (equal when blur is off). The arrival times are
the host's `OnArrival` timestamps of the pair's two real frames (ns, monotonic, same clock as
`now_ns` and the decisions), `-1` when the id is no longer in the host's small table (the last 16
ids). `estimate=recorded` means `recordEstimate` was recorded in this very composite and
`estimate_ret` is its return value; `reused` means an earlier composite's estimate served this
output (`na`). `synth_ret` is `recordSynthBlur`'s return (a frame is only captured when it
succeeded, so 1). Slots are the host's output slot (0..2) and ring slots.

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
allocated when the capture starts and freed when its writer is done. About 0.5 GB for a 1440x1080
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

## Why

`Why:` the user's defect is a single generated frame going dark at one moment of a real game; the
library maintainers cannot see it without the exact inputs. Everything is recorded at the host
because only the host knows which real frames the ring held and which `t` the pacer asked for.
The copies ride in the synth's command buffer instead of reading the slots later because the slots
are recycled within three frames. The trigger is a console command with an optional delay (not a
keybind) so a terminal can arm it while the user returns to the game.
