# Audio Dynamics: the DSP stage

Part of [Game audio capture](audio-capture.md); split out of it on 2026-11-07, text moved verbatim
(one claim corrected: the load clamps live in the config loader, not in `Dynamics.cpp`).


Phase B (2026-10-21). `src/Audio/Dynamics.{h,cpp}` is a stereo-linked compressor
followed by a 1.5 ms look-ahead peak limiter, pure C++ with no PipeWire in it (unit-tested
offline, `tests/test_audio_dynamics.cpp`). The playback callback's order per cycle is:
`CopyPlaneSafe()` per plane (copy + clamp) → `Dynamics::Processor::Process()` over **all**
planes in place → `ClampPlaneSafe()` again over each → `DirectionHook()`. The second clamp
means whatever the DSP does, what reaches the speakers is within [-1, 1] and finite, so the
"safe by construction" guarantee ([audio-capture.md](audio-capture.md#the-playback-copy-is-safe-by-construction)) holds with the stage in the path.

User-facing: presets Off / Even out / Night / Limiter only plus six Advanced values, per
profile, in the Effects area (`audio.effects`; rows `audio.dynamics.*`, see
[audio-mixer.md](audio-mixer.md)). Default
**Off**: Off is a bit-exact zero-delay pass-through.

**Threading — the one real hazard.** `Process()` runs on PipeWire's **data-loop thread**
(the playback stream is `PW_STREAM_FLAG_RT_PROCESS`); `Prepare()` (allocates, must never
overlap `Process()`) is called from the playback stream's `param_changed(Format)` event,
which PipeWire delivers on the **libpipewire main-loop thread** (the thread loop) — two
different threads, so they can overlap when the graph's rate changes. The guard is one
`std::atomic<bool> s_bDynBusy`: the realtime side only *try*-locks it (`exchange`) and skips
the DSP for that single block if `Prepare()` holds it — it never waits, allocates or
logs; `Prepare()` spins with `yield()` (fine off the realtime thread: a `Process()` block
takes microseconds). The prepared channel count is stored inside the same guard and the
realtime side compares it with the output buffer's plane count under the lock, so a
mismatch (different plane count, a missing plane, planes of unequal length, more than 8)
**skips the DSP** and the audio passes as copied, rather than overrunning. `SetParams()`
needs no guard (relaxed atomics, lands at the next block). The processor is a file-static,
not a member of the pair, so params set at startup (before the pair exists) and across a
live capture off/on are kept. `ponytail:` a stream pause/resume does not clear the delay
line, so up to 1.5 ms of stale audio plays at a resume.

**Latency ≤ 2 ms.** The only delay is the limiter's look-ahead, `ceil(rate * 1.5 ms)`
frames (67 at 44.1 kHz, 72 at 48 kHz); zero while Off. `Why:` the user is latency-sensitive
(CS2) — plan D14 capped the added delay at 2 ms. The Mixer's Diagnostics shows the figure.

**Click-free toggling.** A toggle is a crossfade, not a cut. Turning **on**: the delay
line starts empty, so the processed signal is silent for its first 1.5 ms; the output
stays **dry** until the line is full, then ramps (linear, 8 ms, equal-gain because dry and
processed are the same signal 1.5 ms apart) to the processed signal. Turning **off**:
the processor keeps running while it ramps back to dry over 8 ms, then stops and costs
nothing. A processor that has never run since `Prepare()` (stream start) goes straight to
fully on. The make-up gain glides to its target (10 ms one-pole in dB) so a preset switch is
not a step in level. `Why:` the first cut started from silence and cut from delayed to live,
a tick per toggle, and the user plays a game where a tick at the wrong moment is noise.
Consequence: during a fade the output is a blend with the *unprocessed* signal, so the
limiter ceiling is guaranteed only once fully on (and from frame 0 for a never-run
processor). `ponytail:` the ceiling itself is not smoothed.

**The direction hook (plan D3).** `DirectionHook(planes, positions, channels, frames)` in
`Capture.cpp` is a deliberate **no-op**, called once per block after the DSP and the last
clamp with the finished planar samples, the SPA channel positions (`FL`, `FR`, ...) read
from the negotiated format, and the frame count. A later sound-direction display reads
its input there; nothing is behind it. It runs on the realtime thread, so it must not
allocate, lock or log.

`Why own DSP and not a plugin (LV2/EasyEffects) (D6):` no dependency to install or break,
full control of the presets and the metering, and direct access to the samples for the
direction display. `Why a single gain for all channels:` per-channel gains would shift the
stereo image whenever one side is louder; the direction of a footstep is the information.

## Config

`audio.dynamics.*` — per profile, default preset **off** (`config::AudioDynamicsSettings`,
JSON `"audio": { "dynamics": { "preset", "threshold_db", "ratio", "attack_ms",
"release_ms", "makeup_db", "ceiling_db" } }`). Additive, no schema bump. Clamped on
load, by the loader in `src/Config/ConfigManager.cpp`, to these ranges (−80..0 dB, 1..20,
0.1..500 ms, 1..5000 ms, ±24 dB, −24..0 dB). `AudioDynamicsParams()` (`PanelAudio.h`) turns it into the
`Dynamics::Params`: a named preset runs the library's own values, `custom` runs the six
stored ones. Applied at startup and on a profile
switch by `ritz_apply_config_live()` → `Capture::SetDynamics()`, and on every in-session
edit by the Effects rows themselves: `AudioDynamicsPickPreset()` / `AudioDynamicsSetValue()`
(`PanelAudio.h`) write the stored settings and call `SetDynamics()` through a callback.
`Why:` a routed write does not bump the config generation, so the live-apply hook never
runs for it; until 2026-10-22 the rows only saved and a preset did nothing until a restart
or profile switch (the user's "the compressor doesn't work in CS2"). The callback is what
`tests/test_audio_dynamics.cpp` drives. At ratio 1 the compressor is off and only the
limiter (ceiling) acts, so Threshold/Attack/Release do nothing there (the Ratio help says
so); "Limiter only" loads ratio 1.

## Related

- [audio-capture.md](audio-capture.md) — the capture sink whose playback callback runs this stage.
- [audio-mixer.md](audio-mixer.md) — the Mixer / Effects areas that hold the rows.

## History

- **Dynamics** (glossary detail): **Dynamics** — the Mixer's compressor/limiter on the captured game audio (2026-10-21; `src/Audio/Dynamics.{h,cpp}`, settings groups "Dynamics" and "Dynamics: Advanced" in the **Effects** area (`audio.effects`, AUDIO rail group; inside the `audio.mixer` area until 2026-10-22), config `audio.dynamics.*`, per profile, default Off). Four **presets** — Off, Even out (quiet footsteps up next to loud gunfire), Night (a hard squeeze), Limiter only (just stops peaks) — plus **Advanced** sliders (threshold, ratio, attack, release, make-up gain, ceiling); moving a slider makes the preset **Custom**. One gain for all channels (the stereo image, i.e. the direction of a sound, is kept) and a 1.5 ms look-ahead limiter, so it adds at most 2 ms of delay and none while Off. It processes only what the **Capture sink** carries, so it does nothing while capture is off. Not a **Reshade effect** and not EasyEffects.
