# Frame generation: stale real frames (the guard at the ring copy)

Part of [Frame generation](frame-generation.md). Code: `StaleGuard()` / `StaleProbe()` in
`src/FrameGen/FrameGenHost.cpp` (called at the ring copy in `RecordBaseLayer()`), the verdict in
`src/FrameGen/StaleFrame.h` (pure, unit-tested in `tests/test_framegen_stale.cpp`), the probe shader
`src/shaders/cs_fg_stale_probe.comp`. Added 2026-11-21.

## What a stale frame is

A **stale real frame** is a new commit of the game (a frame id the host has not seen) whose buffer
does not hold a new picture but the picture of an EARLIER real frame. The game rotates a few
swapchain buffers; when it hands one over again before its new rendering has landed in it (or
without drawing into it at all), the buffer still holds what it held the last time round: the frame
two or three arrivals before.

Without frame generation that is one frame shown out of order. With frame generation it is worse:
the frame becomes one half of a pair, and a pair of two pictures that do not follow each other
gives a motion estimate that is nonsense, so one whole generated frame comes out garbled.

**Evidence (2026-11-21).** A capture of a game showed, after a hitch, three consecutive real frames
handed to the library byte-identical to the real frame three arrivals earlier each (ids N, N+1, N+2
equal to N-3, N-2, N-1), while a new frame arrived with every paint. The FrameGen maintainer
reproduced the user's garbled-frame look only by feeding the library two frames that do not
correspond. A read of the host's ring bookkeeping found it correct: the ring copy is always
recorded and submitted, and its only skip test is "same id as the newest". So the game's buffer of
commit N itself still held frame N-3 when the copy ran.

**What is not known**: which of two causes it is. Either the commit was marked ready before the
game's rendering into the buffer had landed (a client or driver fence race; the commit path is
upstream's, unchanged), or the game really presented a buffer it had not drawn into again. Both put
the same stale picture into the host. The guard does not depend on which. The diagnostic second
probe (below) is there to tell them apart on the machine where it happens.

## The probe

Before a new real frame N is copied into the ring, a probe compares a fixed grid of
**64 x 64 texels** of the game's buffer (layer 0) with **every frame the ring holds**, up to three:
the newest (N-1), 2 back and 3 back. One compute dispatch (`cs_fg_stale_probe.comp`) binds layer 0
in sampler slot 0 and the ring frames in slots 1-3 (an unused slot gets layer 0 again and is
ignored), all through the raw UNORM view, and writes a 64 x 64 rgba8 mask: r/g/b = 1 where the texel
equals ring frame 1/2/3. The mask is copied into a mappable image and the CPU counts the equal
texels per ring frame. The comparison is exact `==` on rgb: the ring copy stores exactly the values
it fetches, so a frame the copy took unchanged compares equal texel for texel.

`Why:` exact equality and not a tolerance, because the stale frames seen were byte-identical over
the whole frame, and any tolerance would call a slow scene "unchanged". `Why:` 64 x 64 texels, not
the whole frame: 4096 exact compares of a picture that is either identical or a different frame
decide it; a moving game differs on many of them.

## The verdict (`stale::Judge`)

- **Stale** = identical (every probed texel) to at least one OLDER ring frame (2 or 3 back) AND not
  identical to the newest (1 back). It is logged with how many frames back it matched.
- **Repeat** = identical to the newest. Not stale: a still picture (identical to all three) or a
  plain repeat of the frame before shows the same either way. Used exactly as before, only counted.
- **Fresh** = identical to none: a normal new frame.
- **No reference** = the ring holds no frame yet (start, or after the ring was dropped): no verdict,
  copied as usual.

`Why identical to an older AND different from the newest`: an older picture coming back after a newer
one was shown is a step back in time; a pair built on it is two frames that do not correspond. The
probe compares with every ring frame, not only the slot the copy would overwrite (frame 3 back):
a 2-buffer swapchain, or a client that re-presents, gives a frame identical to 2 back, which is just
as much a step back. It also keeps a RUN of stale frames caught: the ring is not advanced by a
refused frame, so the second of three stale frames (the picture of 2 back by then) is found by the
compare with 2 back, not with the slot to be overwritten.

## Synchronous

The probe runs in its own command buffer, **submitted and waited for** before the ring copy is
recorded. `Why:` a synth of the very paint that copies N may already use N as `curr` (pacing names
the newest frame), so the verdict must be known before N is copied or used; read one paint late,
the first stale frame of a run would already be in a pair. It runs once per NEW game frame, not per
paint; a repaint of an already-judged frame does nothing.

Cost, measured headless (nested Wayland backend, 500 x 500 game window, 30 fps game, 120 Hz):
see the numbers in the next section. On the nested Wayland backend each composite is already waited
for before the next paint, so the queue is idle when the probe is submitted and the wait is the probe
itself. On a backend that does not wait for its composites (DRM), the wait also covers whatever is
still in flight on the queue.

| Measured (2026-11-21, headless, 500 x 500 game, Motion version 3) | Value |
| --- | --- |
| probe, CPU wall clock from recording to the counted result | mean 0.17-0.18 ms, max 0.9-1.6 ms |
| per second at 30 game fps | 30 probes, about 5 ms of render-thread time |
| presented rate with the probe on vs off | 2.02-2.03x the game's rate both ways |

The probe's GPU work is tiny (4096 texels, one 64 x 64 mask); the time is the submit, the wait and
the wake-up. It does not grow with the game's resolution.

## What happens to a stale frame

- **Not copied into the ring.** The ring keeps its three good frames and their ids; the guard
  remembers the refused id, so a repaint while it is still the newest commit does not probe again.
- **Never paired.** A pair is found by id (`FindSlot`); the refused id is in no slot, so any pacer
  Decision naming it as `prev` or `curr` takes the existing "pair not found: show a real frame"
  path. The estimate stays valid for a pair of good ring frames (nothing overwrote them), and no
  cached output can reference the refused id.
- **What is shown instead.** Layer 0 is the stale buffer itself (a step back in time), so while the
  newest commit is a refused one, every paint that would show the real frame (no pair, a pair not
  found, a refused synth, a `showId` of the newest or of the refused frame, the pair span guard's
  real frame) shows the **newest good ring frame** (N-1) instead of layer 0. A pair of two older good
  ring frames (Smoothness plays a little behind) is still generated as usual.
- **Several in a row** (three in the field capture): each is refused the same way; the picture holds
  the last good frame for those few paints (a freeze, like the hitch it follows), then the first fresh
  frame is shown as the real frame (its pair names the refused frame before it, so it is not
  interpolated), and generation resumes with the next fresh pair.
- Not covered: while pacing is inert or passing real frames through, the host copies nothing and the
  probe does not run; layer 0 is shown, as without frame generation.

## Console commands, log lines

- `ritz_fg_stale_check` (default 1): 0 = no probe, the old behaviour, to compare.
- `ritz_fg_stale_test <n>` (default 0 = off): n > 0 judges every n-th new real frame stale on purpose
  (no probe for that frame), so the fail-safe path can be exercised where real stale frames never
  come. `1` refuses every new frame: the picture freezes on the last good frame until it is set back.
- `ritz_fg_stale_stats`: prints the counters: stale (forced), plain repeats, probes, the probe's mean
  and max time (submit + wait + count, CPU wall clock).
- Log (scope `framegen`, info, at most one line a second; refused frames in between are counted in
  the next line):
  `frame generation: game frame <id> has the content of the frame <k> before it (frame <id>); not used (<n> so far, <m> not logged since the last line; <r> plain repeats)`,
  and for the forced test `... marked stale by ritz_fg_stale_test; not used (...)`.
- Repeats: at most one line a minute, `frame generation: <n> new game frame(s) were plain repeats of
  the frame before (<total> so far; used as usual)`, so a journal shows how often a "new" frame
  repeats the one before while the game moves.

## The diagnostic second look (which cause is it?)

To tell the two possible causes apart where stale frames happen, every frame that was copied into the
ring gets a second probe of the game's buffer against that very ring copy, recorded as the LAST
dispatch of the host's command buffer of the same paint (after the estimate and the synths), with a
second image set, and read one or more paints later once that submission has retired (a counter
read, never a wait). Gamescope still holds that buffer then (it is the newest commit), so a
well-behaved game cannot write into it. If any probed texel differs, the buffer changed after the
copy took it: the commit was handed over before its rendering had landed (the fence race), and the
log says so, at most once a second:
`frame generation: game buffer of frame <id> changed after it was taken (<d> of 4096 probed texels differ; <n> so far, <m> not logged since the last line)`.

Reading the log after a stale frame:
- stale frames and "changed after it was taken" lines: the fence race (the rendering landed late);
- stale frames and no such line: the game presented a buffer it had not drawn into again.

`Why` in the same command buffer and not after the composite: the host has no hook after the
backend's wait, and its own command buffer already runs after the copy on the same queue; the gap is
the estimate and synth time. It runs only with `ritz_fg_stale_check` on. Not yet seen to fire: no
stale frame has been produced headless (see below).

## Headless testing

The smoke test (a nested gamescope in a private headless sway running a 30 fps rotating cube) shows:
no stale frame and no repeat over minutes of normal running, every captured real frame different;
with `ritz_fg_stale_test 5` every fifth frame is refused, logged, never appears in any captured pair,
and the capture checker still passes; with `ritz_fg_stale_test 1` the picture holds the last good
frame on every paint and generation resumes when it is set back to 0. Real stale frames could not
be provoked there (stopping and continuing the game or the nested compositor for tens of
milliseconds did not produce one), so the forced test is the proof of the fail-safe path; the guard
waits for the machine where they happen.
