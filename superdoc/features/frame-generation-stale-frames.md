# Frame generation: stale real frames (the guard at the ring copy)

Part of [Frame generation](frame-generation.md). Code: `StaleGuard()` / `StaleProbe()` in
`src/FrameGen/FrameGenHost.cpp` (called at the ring copy in `RecordBaseLayer()`), the verdict in
`src/FrameGen/StaleFrame.h` (pure, unit-tested in `tests/test_framegen_stale.cpp`), the probe shader
`src/shaders/cs_fg_stale_probe.comp`. Added 2026-11-21. The cause it was built for, a game buffer handed
back too early, is fixed in `update_wayland_res()` (`src/steamcompmgr.cpp`): see "The early hand-back".

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

**The cause, found the same day: the early hand-back** (next section). The game got a buffer back
while gamescope still held it, and drew a LATER frame into it. The "stale" frame is the legitimate
one: the frame copied three arrivals earlier already held its picture (a motion estimate of the field
capture shows ring frame N-3 one step of motion ahead of where it should be, and ring frame N stepping
back by two). The first two readings this page started from (a fence race on the acquire side, or a
game presenting a buffer it had not drawn into) were wrong.

## The early hand-back (the cause; fixed 2026-11-21)

Code: `update_wayland_res()` and `ReleaseParkedReleasePoints()` in `src/steamcompmgr.cpp`; the commit's
sync facts on `commit_t` (`src/commit.h`).

- Every `wl_surface.commit` reaches `update_wayland_res()`, also one that attaches **no new buffer**
  (upstream: "committing without buffer state is valid and commits the same buffer again",
  `xwayland_surface_commit()`). Some clients send such commits (a frame-callback-only commit).
- The explicit-sync surface state is never cleared after a commit (`LinuxDrmSyncobj.h`: the
  `Extract*TimelinePoint()` calls are `const`). So that commit gets NEW acquire and release point
  objects with the SAME timeline and value as the commit that attached the buffer.
- A release point object signals its point when its last owner drops it (`~CTimelinePoint`). The new
  object either dies with a discarded commit, or replaces the backend fb's own object in
  `SetReleasePoint()`, which destroys the original. Either way the buffer's release point fires at
  once, while gamescope still holds that buffer as the newest commit and may not have read it yet.
- A game that renders ahead (waiting for a free buffer) draws its next frame into it at once. A
  frame taken from that buffer afterwards holds a FUTURE picture ("changed after it was taken", nearly
  every texel), the copy of an earlier id holds the picture of a later one, and the later commit then
  looks identical to it (a "plain repeat", or "has the content of the frame k before it" when the game
  is k frames ahead).

**The fix**: a commit whose buffer AND release point (timeline and value) equal those of a commit still
queued is a re-commit. Its duplicate release point is **parked** with the buffer's texture (a private
reference) and neither handed to the fb nor dropped; the fb keeps its own. A parked point is destroyed
(signalled) once nothing uses the buffer any more (`CVulkanTexture::IsInUse()`), swept before every
commit is taken and once per main-loop iteration, so it always fires before the client can be given a
newer release point of that buffer. `Why` park instead of building a disarmable point: no change to
`Timeline.h`, and the extra signal of the same value is what upstream already does when the fb's own
object fires.

Not known yet: which commits the user's game sends without a new buffer, and why they showed up only
while frame generation presented more frames than the refresh setting. The counters below say how many
there are in each mode.

### Measured headless (2026-11-21, nested Wayland backend, vkcube through the WSI layer)

No headless client sends commits without a new buffer (vkcube through the layer, vkcube and glxgears
through Xwayland: 0), and none produced a stale frame, also with 60-120 compositor stalls of 20-50 ms
at Target = display refresh. So the commit such a client sends is injected (`ritz_debug_recommit n`,
below): after every 3rd commit, Fixed 2x, 120 Hz, an uncapped vkcube (always waiting for a buffer):

| Run (about 12 s of injection) | Result |
| --- | --- |
| upstream behaviour (`ritz_recommit_keep_release 0`, `ritz_fg_handback_check 0`) | 503 re-commits, **503 plain repeats** (the later frame equal to the copy of the earlier one) |
| upstream behaviour, hand-back check on, 130 fps game, Target = refresh, 240 Hz | 520 re-commits, 516 new frames found handed back before taken, 895 held paints |
| the fix (default) | 505 re-commits, all kept, 0 still held at the end, 0 repeats, 0 stale, 0 handed back |

A game that renders one frame ahead shows the hand-back as plain repeats; the field game, three ahead,
as "the frame 3 before it".

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

| Measured (2026-11-21, headless, Motion version 3, fixed 2x) | Value |
| --- | --- |
| probe, CPU wall clock from recording to the counted result (500 x 500 game at 30 fps, 120 Hz) | mean 0.17-0.18 ms, max 0.9-1.9 ms |
| the same with the game at 120 fps, 240 Hz | mean 0.165 ms, max 0.74 ms (120 probes a second, about 20 ms of render-thread time) |
| a still 640 x 480 picture presented at about 1000 fps | mean 0.06 ms |
| presented rate, game at 120 fps, 240 Hz, probe on / off / on again | 266.7 / 266.2 / 266.7 fps (2.22x each) |

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

## The commit facts and the hand-back check (2026-11-21)

When a new frame is first taken (the probe), `FrameGen_CommitFacts()` (`steamcompmgr.cpp`) reports what
the compositor knows about its commit (`stale::CommitFacts`, `StaleFrame.h`): explicit or implicit
sync, acquire point already signalled when the commit was received ("ready at commit") or waited for,
a re-commit without a new buffer, whether its **release point has already been signalled** (a
`drmSyncobjQuery` of the release timeline against the commit's point: the game has the buffer back),
the buffer's implicit write fence (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`, read flag, polled without waiting;
with explicit sync the driver may attach none, so "signalled" says little there), the times received ->
ready -> newest done commit -> probe, and the texture's and fb's reference counts.

- **Hand-back check** (`ritz_fg_handback_check`, default 1): a new frame whose release point has
  already fired is not used, like a stale frame (the newest good ring frame is held). `Why`: under the
  early hand-back the frame to distrust is the one whose buffer the game has back, not the later frame
  that then looks stale. With the fix it should never fire; it stays as the backstop for any other
  path that hands a buffer back early.
- Log, at most one line a second: `frame generation: game frame <id> had already been handed back to
  the game when it was taken (its buffer may hold a later frame); not used (<n> so far, <m> not logged
  since the last line; <facts>)`.
- The stale line and the "changed after it was taken" line end with the facts too (`commit: explicit
  sync, waited for, new buffer, released to the game no, write fence signalled, received->ready ...
  ms, ready->newest ... ms, ->probe ... ms, refs t/f`); the second adds `released to the game by now
  YES|no|commit gone` (queried when the second look is read).
- Compositor (scope `xwm`, at most one line per 5 s): `a commit without a new buffer re-committed the
  buffer of commit <id> (release point <p>): its release point is kept until the buffer is no longer
  used (<n> so far, <m> since the last line)`.
- `ritz_fg_stale_stats` prints a second line: `handed back before taken <n> (not used); stale with:
  released, re-commit, ready at commit, explicit sync, write fence pending; changed after taken with:
  released, re-commit, write fence pending; re-commits without a new buffer <n> (release kept <k>,
  still held <h>); new frames taken with explicit sync, ready at commit, re-commit`.
- `ritz_recommit_keep_release` (default 1; 0 = the upstream behaviour, to compare) and
  `ritz_debug_recommit <n>` (test: after every n-th explicit-sync commit, inject the commit a client
  sends without a new buffer) live in `steamcompmgr.cpp`.

Reading a field run: re-commits > 0 with the fix on and 0 handed back / 0 stale = this was the cause;
handed back > 0 with re-commits kept = another early path (then the facts' times and refs say where).

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

Reading the log after a stale frame: the field log had both, and they turned out to be the early
hand-back (above): the game drawing a later frame into a buffer gamescope still held. "Released to the
game by now YES" on such a line says exactly that.

`Why` in the same command buffer and not after the composite: the host has no hook after the
backend's wait, and its own command buffer already runs after the copy on the same queue; the gap is
the estimate and synth time. It runs only with `ritz_fg_stale_check` on. Not yet seen to fire: no
stale frame has been produced headless (see below).

## Headless testing

The smoke test (a nested gamescope in a private headless sway running a 30 fps rotating cube) shows:
- normal running: no stale frame and no repeat, every captured real frame different, the capture
  checker passes, the second look never fires;
- `ritz_fg_stale_test 5`: every fifth frame is refused and logged, never appears in any captured
  pair (the ring's ids skip exactly those), the capture checker still passes, two held paints per
  refused frame; `ritz_fg_stale_test 1`: every paint holds the last good ring frame, and generation
  resumes at 2x when it is set back to 0;
- a still picture (a lossless video of one image, presented through Xwayland at about 1000 fps):
  over 41000 new frames, every one a plain repeat, none stale.

Real stale frames could not be provoked there: stopping and continuing the game (Vulkan client and
an Xwayland OpenGL client) for 15-40 ms, 150 times, and the nested compositor for 30-50 ms, 100
times, produced none, neither in the guard's counter nor as repeated real frames in a capture with
the guard off. The forced test is the proof of the fail-safe path. The cause itself is reproduced with
`ritz_debug_recommit` (see "The early hand-back").
