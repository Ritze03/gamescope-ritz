# Frame generation: backends, host frames and direct scanout

Part of [Frame generation](frame-generation.md). Which backends can show generated frames, why the nested host loses frames, and the direct-scanout fix.

## Backends

Generated frames must go through the composite, so full composite is forced while FG is on
(or still holds resources, see above). DRM also disables partial composite (and therefore
direct scanout): a scanned-out buffer never passes `vulkan_composite()`, and a partial
composite has already dropped the base layer. Wayland and OpenVR force full composite; SDL
always composites. See the backend pages.


### Frames the host shows (nested Wayland, 2026-11-04)

Under nested Wayland the status line ends with `host: sent 311/s shown 217/s discarded 94/s
zero-copy 0/s no free image 0/s` (`src/FrameGen/HostShown.h`, counted in the Wayland backend's
`wp_presentation` feedback handlers; one feedback object per commit of the toplevel surface,
so `sent` = commits, `shown` = `presented`, `discarded` = replaced by a newer commit before the
host drew, `zero-copy` = shown frames the host scanned out directly, `no free image` = paints
dropped because the host held every output image; other backends print nothing). `Why:` the "presented 311 fps" figure counts
what gamescope *sent*, and it misled us: the screen showed far fewer of those frames, and
nothing in the log said so.

**Cause of the "only ~170-240 of 311 shown" report: the host cannot draw while the game's
GPU job runs** (mechanism below, in [why the host loses frames](#why-the-host-loses-frames)).
First step of that finding, that it is not gamescope's commits: setup: Minecraft, Default profile (Target = refresh, Smoothness, Learned motion),
nested on a high-refresh monitor (<connector>) under a recent Hyprland (the release current at the time; later releases may differ); the counter checked per commit
(`sent` == feedback requests == `presented` + `discarded` in a per-event trace, every run).
Falsification: a 100-line plain Wayland shm client (no gamescope, no frame generation) that
commits at 311/s with the same feedback counting, as a window next to the game on the same
monitor, gets the same number gamescope does in the same 40 s:

| host state, 311 commits/s sent | gamescope shown | plain client shown | GPU busy |
| --- | --- | --- | --- |
| no game running (idle GPU), window | -- | 279-281 (31 discarded) | idle |
| no game running, fullscreen, VRR on | -- | 280 (31 discarded) | idle |
| game + FG, window, no VRR, limit 120 | 213, 211, 156 | 215, 214, 156 | 83, 84, 96% |
| same, uncapped | 168, 153 | 172, 152 | 99, 100% |
| same, limit 30 | 225, 204 | 226, 204 | 70% |
| game + FG, fullscreen, VRR on, limit 120 / 30 / uncapped | 217, 244, 244 / 243 / 143, 227 | (not possible, the game covers it) | 78 / 60 / 100% |

So gamescope's commits were evenly spaced (window, limit 120: 51 % of commit-to-commit gaps
0.5-0.9 refresh, 42 % 0.9-1.1, 6 % 1.1-1.5, 0.1 % under 0.5; the busiest runs had 4-6 % under
0.5) and every one was answered; what
changes is how many vblanks the host actually draws. With the game running, 46 % of the
shown frames' timestamps were two refreshes apart (window, limit 120, 191 shown) instead of
one; fullscreen with VRR lost less (8-11 % two apart, 244 shown). The host's rate fell with
the GPU's load in every pair of runs (busy 60-70 % -> 204-243, 78-84 % -> 211-244, 96-100 %
-> 143-168), and a client that has nothing to do with gamescope loses the same frames.
That was a correlation with average busy; the same day's tests below replaced it with the
mechanism, which goes by the length of one GPU job and not by busy. A discarded commit is a
frame the screen never showed; with 311 sent against a 280 Hz refresh at least ~31 must be
discarded even on an idle host.

Things that are **not** the cause (each checked): gamescope waits for no frame callback
(the backend never calls `wl_surface_frame`) and commits at once; the 311/s commits are
evenly spaced, not paired inside one host vblank (the `<0.5 refresh` bucket held 0.1-6 %);
the host reported 280 Hz and the true VRR state was sampled each run (VRR only while
fullscreen: Hyprland `vrr = 3`); no compiler or other heavy job of ours ran (load 3-6).
Not measured: fullscreen **without** VRR (the harness refused the only live way to switch
VRR off, `hyprctl eval`; Hyprland's `keyword` is a no-op under its Lua config), so that cell
of the matrix is empty.

#### Why the host loses frames

Hyprland composites on the GPU's graphics queue, the queue the game renders on, and that
queue does not preempt a running job (RDNA 3). Hyprland's own work is tiny (7-23 ms of
graphics engine time per second, `drm-engine-gfx` in `/proc/<pid>/fdinfo`), but each frame it
draws has to wait for the game's job that is running, and with no finished draw it has
nothing to flip at that vblank. So what costs frames is **how long one game job is, not how
busy the GPU is on average**, and on a saturated queue the host gets about one draw per game
job. `Why this matters:` frame generation runs on the compute queue, so gamescope keeps
producing 311 frames per second, and they then die in the host. Uncapped, **shown is about
the game's frame rate**: game 152 -> shown 157, 150 -> 157, 120 -> 124; the earlier runs above
agree (228 -> 227, 178 -> 168, 158 -> 153). **Proven 2026-11-05**: with Hyprland's draw taken
out of the path and nothing else changed, the same load shows 272-278 (see
[direct scanout](#direct-scanout-the-fix), below). The wait itself was then **observed**
(2026-11-07, scheduler trace, see [GPU clock at light load](#gpu-clock-at-light-load-2026-11-07)):
Hyprland's draws sit behind the game's job on the one graphics ring.

Synthetic reproduction, no game and no gamescope (`dc`: a dmabuf client committing 311/s,
fullscreen on the 280 Hz screen, VRR on, signalled buffers; `burst`: a headless Vulkan
client running one job of L ms on the graphics queue R times a second):

| one GPU job, 30 per second | 4 ms | 6 ms | 8 ms | 12 ms | 16 ms |
| --- | --- | --- | --- | --- | --- |
| shown per second | 279 | 270 | 253-264 | 211-212 | 180-182 |
| GPU busy | 33% | 41% | 44% | 52% | 60% |

The same job split into eight 2 ms jobs (16 ms total, 30 per second, GPU as busy) shows 262,
and 270 at 60 per second with the GPU 90% busy. The real game fits the same curve. A tiny job
on the graphics queue (`burst lat`, submit to signal) sees what a compositor draw sees:

| Minecraft, fullscreen, VRR, 311/s sent | game fps | shown | tiny job wait p50 / p99 / max |
| --- | --- | --- | --- |
| limit 30 | 30 | 206-208 | 0.14 / 13.8 / 14.5 ms |
| limit 120 | 119-122 | 230-233 | 4.5 / 6.4 / 9.7 ms |
| uncapped | 150-153 | 156-159 | 3.5-3.8 / 6.4-7.3 / 9.8 ms |

(Two launches, unchanged build and host: 7 runs each at limit 120 and limit 30, 5 uncapped. An
eighth limit 120 run, the first of its session, read 130 at 97% busy and did not come back; its
cause is not known.) Limit 30 waits for one ~14 ms job 30 times a second, between the synthetic
12 ms (212) and 16 ms (182) rows, which is why it shows only ~207 at 60% busy. That job is
about 2x its uncapped length (13 ms against 6.3 ms by the 2026-11-07 trace); the cause is the
GPU clock, see [GPU clock at light load](#gpu-clock-at-light-load-2026-11-07). One game frame is one `vkQueueSubmit` of 2 submit infos and 3 command buffers
(counted in the WSI layer: 30, 120 and 147 calls per second at those three rates), and nearly
all of it sits in one command buffer: splitting the three into separate submits changed
nothing. In one heavier scene, limits 40 / 60 / 80 / 100 showed 152 / 175 / 194 / 198 and
uncapped (game 120) 124, so a limit near the GPU's edge shows the most, and past it shown
collapses to the game's rate.

Tried, and it does not help (each interleaved or repeated in one session unless noted):

| Change | Shown at limit 120 / 30 / uncapped | Note |
| --- | --- | --- |
| none | 232 / 207 / 158 | |
| `render:new_render_scheduling` | 233 / 204 / 157 | in its second repeat `presented` fell to **0** for about a minute, every commit discarded, until it was switched off (the screen itself was not watched); synthetic 16 ms job 104 vs 180. Do not use |
| game queues at LOW global priority (from the WSI layer, separate launch) | 231 / 206 / 157 | game fps unchanged, tiny job wait unchanged. Lowering works (split job: p99 19 -> 4.5 ms) but cannot stop a running job, and Hyprland already passes queued work (next paragraph) |
| game's command buffers split into separate submits (WSI layer, separate launch) | 224 / 206 / 158 | one command buffer holds the work |
| commits paced by `wl_surface_frame` | synthetic only: 8 / 12 / 16 ms 265 / 211 / 184 | commit phase is not the cause, candidate closed |

A tearing test proved nothing: with an `immediate` rule the game's `activelyTearing` stayed
false in every sample and shown did not move, and the synthetic client with the `async` hint
stayed at 280, so nothing tore. Hyprland's draw would still queue behind the game's job.

Hyprland's draws do pass queued jobs: with the split job at 60 per second a normal-priority
tiny job waited 13 ms while Hyprland still showed 270. So its context behaves as high
priority (aquamarine logs "Got a high priority context"; the kernel side was not readable),
and a priority change can only ever help between jobs. A job cannot be stopped, and a layer
cannot cut a command buffer without wrapping every `vkCmd*` and re-binding state, so nothing
contained on gamescope's side shortens the game's job. `Why:` the fix has to take the
host's draw out of the path.

#### GPU clock at light load (2026-11-07)

Measured with the kernel's `gpu_scheduler` trace events (`drm_sched_job_queue/run/done`,
5-second captures; Minecraft, RX 7900 XTX, 280 Hz, Frame generation target = refresh, 311/s
sent, `direct_scanout` 0). Only one graphics ring exists (`gfx_0.0.0`); Hyprland and the game
share it, and gamescope runs on compute rings (`comp_1.*`). At limit 30 the game's job is 13.7 ms
typical (30/s) and Hyprland's is 0.15 ms typical (293/s). Of 1327 Hyprland draws, 264 took
longer than one refresh (3.57 ms), up to 15.9 ms, and nearly all of the overlapping time was the
game's job. In every trace zero Hyprland jobs ran and finished inside a game job, so there is no
preemption in practice, and Hyprland submits nothing new while a draw is stuck. Job times are
derived from completion order on the ring, not from hardware timestamps.

Length of the game's job on the graphics ring:

| Setup | Job |
| --- | --- |
| Frame generation on, blur on, limit 30 | 13.7 ms |
| Frame generation on, blur off, limit 30 | 13.0-13.2 ms |
| Frame generation off, blur on, limit 30 | 12.5 ms |
| Frame generation off, blur on, game's own 30 fps, gamescope's limiter off | 12.0 ms |
| Frame generation on, blur on, uncapped (153 fps) | 6.3 ms |
| Frame generation on, blur off, limit 120, performance level high | 6.8 ms |

Frame generation adds about 1 ms; neither it nor gamescope's limiter makes the job long. What
does is the clock (limit 30, Frame generation on, blur off):

| GPU clock setting | Core clock | Shown |
| --- | --- | --- |
| normal (CoreCtrl Advanced, range 500-2970 MHz) | about 1284 MHz | 187-214 |
| minimum clock raised to 2825 MHz (`OD_SCLK`) | average 1745 MHz, samples 384-2180 | 211-236 |
| `power_dpm_force_performance_level` = high | about 1960 MHz steady | 264-267 for over two minutes |
| back to normal, same session | not sampled | 213-214, job 13.0 ms |

Uncapped the GPU runs at 2709 MHz and 398 W (at the 402 W limit) with a 6.3 ms job: clock ratio
2.11x, job ratio 2.10x. The high-level job length at limit 30 was not traced; 264-267 shown fits
the synthetic 8 ms row (253-264). Why high settles at about 1960 MHz rather than near the maximum
is not known.

`Why:` the GPU firmware lowers the clock while the graphics queue is mostly idle, which roughly
doubles the length of each game job, and each job blocks Hyprland's draws for longer. A game
at its own frame rate does not need those draws, so frame generation is what exposes it; the
display path treats generated and native frames alike (with Frame generation off about 20% of
Hyprland's draws still took longer than a refresh). Raising the minimum clock in `OD_SCLK` does
not hold at light load, so testing that way can look like "clocks do not matter".

Limits: at limit 120 the performance level high shows 230-233, the same as normal (230-233),
with a 6.8 ms job and the graphics ring 83% busy; the clocks are already up there. Uncapped is
not helped either (GPU at about 2700 MHz and its power limit; shown stays near the game's
rate). So high helps when the GPU has spare capacity and is downclocking, and direct scanout
remains the only change that reaches about 280 in every case.

Workaround, no gamescope code: a CoreCtrl per-app profile with Performance mode Fixed > High
while the game runs. The sysfs file is root-only, so gamescope cannot set it itself.

Tooling warning: never read `/sys/kernel/debug/dri/*/*` in bulk as root (no `grep` or `cat` over
the folder). Some files act when read: on 2026-11-07 a `grep` across it hit `amdgpu_evict_vram`,
which evicted all VRAM, crashed gamescope and Minecraft with GPU page faults and a compute ring
reset, and then looped in the kernel, unkillable, pinning the GPU at 100% for about ten minutes.
Safe trace method, it does not touch the GPU: as root enable
`/sys/kernel/tracing/events/gpu_scheduler/enable` for a few seconds, read
`/sys/kernel/tracing/trace`, then turn it off.

#### Direct scanout: the fix

With `render:direct_scanout = 2` Hyprland hands gamescope's buffer to the display and draws
nothing, so no job of its own waits behind the game. Until 2026-11-05 that could not happen for
gamescope: KMS refused its output images, which also explains the user's `settings.lua` comment
"Breaks FSR in Gamescope". The refusal, the fix and the buffer count are in
[backend-wayland.md](backend-wayland.md#direct-scanout-under-a-host). The setting itself is the
user's: Hyprland's config still has it off, and it should stay off for now: scanout on can crash
the GPU (root cause unknown, 2026-11-06, see backend-wayland.md's known issue).

Synthetic (`dc` + `burst`, as above, scanout on, `zc` = zero-copy every shown frame): **280 at
every job length**, 8, 12, 16 and 24 ms at 30 per second, and 5 and 7 ms at 120 per second with
the GPU 84% busy. With the draw in the path the same jobs showed 253-264, 211, 181, and 240.

Minecraft, fullscreen, VRR, frame generation on (Default profile), Hyprland's option switched
live by a temporary config file, the two states interleaved inside each launch:

| shown per second | limit 120 | limit 30 | uncapped |
| --- | --- | --- | --- |
| before, no capture, `direct_scanout` 0 | 232, 231, 129* | 206, 206, 170* | 162, 161, 128* |
| before, `direct_scanout` 2 (scanout never engaged) | 147, 130, 84* | 204, 171, 117* | 88, 74, 73* |
| after, `direct_scanout` 0 | 221, 127*, 227, 183+, 130* | 196, 165*, 201, 151+, 162* | 131*, 127*, 158, 160+, 127* |
| after, `direct_scanout` 2 | 275, 276, 277, 277, 272 | 277, 278, 278, 268, 276 | 272*, 272, 261, 268*, 272 |

`*` the scene had turned heavier (uncapped game about 120 instead of 153; it drifts on its own
with the player idle). `+` the first run after a launch, still settling. Scanout does not care:
272 with the game at 120 or at 153. In every run of the last row all shown frames were zero-copy
(`zero-copy` = `shown`), `directScanoutTo` was set in all 8 samples and `directScanoutBlockedBy`
was empty, and uncapped game fps was what it was without scanout. Shown no longer follows the
game's frame rate. Frame generation was on throughout, so the uncapped column is the case that
matters: game 121-154, shown 261-272.

The capture was not part of this: `simple-screensharing` running gave 230-233 / 206-208 /
156-159, closed gave the first row, within noise across two sessions, with `RECORD` gone and no
video stream left in `pw-dump`. A capture does block scanout, though: `RECORD` is global, on
every monitor while any capture runs.
