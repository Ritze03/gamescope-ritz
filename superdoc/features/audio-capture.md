# Game audio capture

Phase A of the game-audio plan (branch `audio-effects`, 2026-10-07): gamescope-ritz
creates **one private PipeWire sink per session** and moves every audio stream of the
game it hosts into it, so "which stream is the game?" is no longer a guess. Code:
`src/Audio/Capture.{h,cpp}` (everything), a call in `LaunchNestedChildren()`
(`src/steamcompmgr.cpp`), the live switch in `ritz_apply_config_live()` and teardown in
`main()` (`src/main.cpp`), the config key `audio.capture` (`src/Config/`). Tests:
`tests/test_audio_capture.cpp`.

This is the **capture** half. The Mixer's *Game volume* row drives this sink's volume
(`Capture::SetVolume/SetMute`) while capture is `Active()`, and the stream guesser
(`Volume.{h,cpp}`) is only the fallback — see [audio-mixer.md](audio-mixer.md);
the compressor/limiter that processes it (Phase B, 2026-10-21) is ["Dynamics" below](#dynamics-the-dsp-stage).

## The pieces

**The sink — a loopback pair** (two `pw_stream`s, the pattern of PipeWire's
`module-loopback`, not a `pw_filter`):

- a **capture** stream announced as `media.class = Audio/Sink`, `node.name =
  gamescope-ritz.<pid>`, description "gamescope-ritz (game)". Games play into it.
- a **playback** stream (`gamescope-ritz.<pid>.out`) to wherever the game's audio was
  headed before we moved it (the **original route**, below), else the system's
  **default output**. With no route recorded it carries no target, so it follows the
  default when that changes: WirePlumber re-links a target-less stream.
  The capture stream's process callback only *triggers* the playback side
  (`pw_stream_trigger_process`), so both run in the same graph cycle and the pair adds
  no extra quantum. The playback callback copies the newest captured buffer through
  `CopyPlaneSafe()`, then runs the Dynamics stage over all planes (the **DSP hook
  point**, `RunDynamics()`).
- Format F32P, rate unset (the pair follows the graph; seen 44.1 kHz/512 and 48 kHz/1024),
  channel layout = the default output's `audio.position` read once at creation
  (fallback FL,FR).
- both: shared `node.group` + `node.link-group`, `state.restore-props=false`,
  `state.restore-target=false`, our markers `gamescope.ritz.session=<pid>` and
  `gamescope.ritz.pid=<pid>`; the latency params are mirrored between the two streams
  like `module-loopback`'s `update_latency`.

`Why a loopback pair, one node:` no second module or null sink to manage, and the samples
pass through our own code (effects now, sound-direction analysis later).

`Why node.link-group:` it stops WirePlumber linking our playback into our own sink.

`Why the playback has no node.dont-move:` it used to (2026-10-07, the plan's D8:
"processed audio always goes to the default output"), so WirePlumber ignored every
metadata target and linked it to the default device, which **bypassed EasyEffects** (the
user runs it with "process all outputs", which writes a target into the `default`
metadata for *every* output stream). The user replaced that decision the same day:
*"the audio should go to the previous default (before us overwriting it). So if
easyeffects process all audio streams is set, respect it. But do it by reading the audio
out before setting it (or using what gamescope got in the first place)"*. **This replaces
D8.** See "The original route" below.

`Why state.restore-*=false:` WirePlumber's state-stream otherwise restores a remembered
volume/target for our nodes.

`Why the id is the pid:` it needs no sanitising (the app id is an opaque string) and
two sessions never collide.

**The tag.** `ApplyGameEnvironment()` (called from `LaunchNestedChildren` right before
the game is spawned, **only when there is a game to host**) merges
`gamescope.ritz.session=<pid>` and `state.restore-target=false` into the environment the
game inherits:

| var | form | merge |
|---|---|---|
| `PIPEWIRE_PROPS` | SPA-JSON object | unset → `{ gamescope.ritz.session = N state.restore-target = false }`; an existing `{ … }` gets ours inserted before the `}`; a bare `k = v` list is wrapped; already carrying our key → unchanged |
| `PULSE_PROP` | space-separated `k=v` | ours appended |
| `PULSE_SINK` | name | set to our sink **only** when capture is on and the launcher has not set one |

Never overwrites what the launcher set (D12). The pure merge functions are in
`Capture.h` (`Env::MergePipewireProps`, `Env::AppendPulseProp`) and unit-tested. The tag
is injected **always** (it is inert), so capture can be switched on live mid-session.

`Why tagging and not target pinning:` the user's own `client.conf.d` rule strips
`target.object` for CS2 and `pw-cat`, which defeats a plain `PIPEWIRE_NODE`/target pin;
the rule exists on purpose and is not touched.

`Why state.restore-target=false on the game's streams:` otherwise WirePlumber persists
*our* move as the game's remembered target, which would break the user's CS2 →
EasyEffects routing outside gamescope.

`Why PULSE_SINK is only a fallback:` the tag-and-move route works regardless; PULSE_SINK
costs the launcher's own choice if it had one (it has a `sink` field).

**The mover.** A registry listener binds every `Stream/Output/Audio` node (the registry
`global` event carries only a *subset* of props, never custom ones, so the bind + the
node `info` event is the only way to read `gamescope.ritz.session`) and, for a node whose
tag equals ours, writes
`pw_metadata_set_property(default, <stream id>, "target.object", "Spa:Id", "<sink serial>")`.
Our own playback (it carries `gamescope.ritz.pid`) is never moved.

`Why target.object with the object.serial, not target.node:` `target.node` loses to a
writer that sets `target.object` (EasyEffects).

`Why it re-asserts:` EasyEffects races us — it writes its own `target.object` for the
same stream. The mover listens to the metadata's `property` events and puts ours back
whenever a tagged stream's value differs, capped at 10 per stream so two writers can
never fight forever (live it converged in 1).

Switching capture **off** live clears every metadata entry we wrote (the game streams'
targets **and our playback node's**) and destroys the pair; tagged streams then fall
back to wherever WirePlumber routes them (the default sink, or EasyEffects under the
user's rules). Switching it on again creates a *new* sink node, reads the original route
again and moves the streams back.

## The original route (replaces D8)

Our playback goes where the game's audio would have gone without us. Pure decisions are
in `Capture.h`'s `Route::` (unit-tested), the PipeWire half in `Capture.cpp`.

1. **Remembering it.** Right before the mover's first write for a tagged stream
   (`ObserveOriginalRoute()`), where the stream was headed is read, first hit wins:
   (a) its `target.object` / `target.node` in the `default` metadata (the registry tracks
   every node's entries from the metadata's own property events, so a value written
   before we looked is there); (b) its own node props `target.object` / `node.target`;
   (c) the sink it is currently linked to (tracked from registry `Link` globals); (d)
   nothing, meaning the default output. A value that is **ours** is skipped, never
   recorded, and does not end the search. A link to the **current default sink** is not
   a route (see `Why` below). Also, when the re-assert loop sees a **foreign** write
   for a tagged stream (EasyEffects racing us), that value is the route the game would
   have had: it is recorded, the **latest foreign write wins**.
2. **The form.** The route is a `node.name` (`easyeffects_sink`), resolved from a serial
   or id through the registry's sinks (`Route::ResolveRouteValue()`; only a *known*
   Audio/Sink resolves, so a stale name or `-1` records nothing). Serials and ids change
   if EasyEffects restarts; the name does not.
3. **Pointing the playback there** (`ApplyPlaybackRoute()`). With a route: `target.object`
   for our playback node in the `default` metadata = the name, as `Spa:String` (the
   spike found a name and a serial `Spa:Id` both work). Without: nothing is written, so
   WirePlumber links us to the default output and follows default changes, as before.
   Never our own sink (`node.link-group` already stops WirePlumber's loop; the code
   guards it too: `Route::PlaybackTargetFor()`).
4. **EasyEffects also writes a target for our playback** (it is an output stream). When
   the route is EasyEffects that agrees with ours. When the route is another sink, ours
   is put back, capped at 10 like the game streams'
   (`Route::ShouldReassertPlayback()`). With **no route** recorded we do not fight at all.
5. **The route's node goes away** (EasyEffects quits): the route and our playback's
   metadata entry are cleared, so we fall back to the default output. If the sink
   returns, EasyEffects' next write for a game stream records it again.
6. **Diagnostics**: `Capture::PlaybackRoute()` (non-blocking) gives the name or `""`;
   the Mixer shows `capture plays to: easyeffects_sink (game's original route)` or
   `default output`.

`Why a link to the default sink is not a route:` by the time we first see a stream
WirePlumber has usually linked it to the default sink. Recording that would pin our
playback to that device by name, and it would stop following a later change of the
default, which "no route" does. (a) and (b) are explicit targets and count even if they
name the default.

`Why a name, as Spa:String:` it survives EasyEffects restarting; a serial does not.

`ponytail:` one route per session. Several tagged streams with different original routes
share the most recently recorded one; honouring each would need one playback stream per
route. An unresolvable foreign value on our playback is not fought either.

**Lifetime (D11).** `Start()` is called before the game is spawned and waits up to
1.5 s for the sink, so it exists when the game's first stream appears; `Shutdown()` runs
in `main()` after the children are reaped. There is **no stale-sink scan**.
`Why no scan:` PipeWire removes a dead client's nodes itself — verified: after
`kill -9` of the owning process no node of ours is left in `wpctl status` — so a leftover
from a crashed session cannot exist and a scan would be dead code. (The one residue is a
`target.object` metadata entry pointing at the dead sink for a stream that outlives it;
that stream is re-linked to the default and the entry goes when the stream does.)

**Volume and mute** act on the capture stream's node (`pw_stream_set_control`:
`SPA_PROP_channelVolumes` / `SPA_PROP_mute`), so the Mixer, `wpctl` and pavucontrol see
one value, and an external change comes back into `GetVolume()`/`GetMute()`.
`Why the wish is kept apart from what the sink reports:` the setters record a *wish*
that is applied the moment a sink is ready, **before any sample flows**, and survives a
live off/on (a recreated sink starts at unity). The units are the Mixer's display curve
(`DisplayToLinearVolume`: `channelVolumes = display^3`, the pavucontrol curve).
**Verified 2026-10-07** (wpctl 0.5.18, read-only against live nodes): `wpctl
get-volume` reports the cube root of `channelVolumes` (0.38742 → `0.73`, 0.511977 →
`0.80`, 0.713888 → `0.89`), i.e. wpctl speaks the same cubic scale as the display
fraction, so `SetVolume(0.5)` writes `channelVolumes 0.125` and `wpctl get-volume`
reports `0.50` — the two agree. `Volume.cpp` used to treat wpctl as raw linear and got
this wrong on its own (fallback) path; fixed, see audio-mixer.md's "wpctl scale".

## The playback copy is safe by construction

The playback callback runs on the realtime thread straight into the speakers, so it
never forwards a buffer as found. Every plane goes through `CopyPlaneSafe()`
(`Capture.h`, pure, unit-tested):

- the source range is clamped to its `maxsize` (an offset past it reads as nothing);
- no source, a chunk flagged `EMPTY`/`CORRUPTED`, or a null data pointer is **silence**;
- a short source is zero-padded to the wanted size; the wanted size is the graph's
  `requested` frames, clamped to the destination's `maxsize` and to whole floats;
- the destination chunk's `offset`, `size`, `stride` (4) and `flags` are always set;
- the last step clamps every sample to [-1, 1] and maps NaN/inf to 0 (tested on the bit
  pattern: the project builds with `-ffast-math`, which may remove a float `f == f`).

`Why:` the first spike's copy forwarded whatever the capture side held, and during
testing a "silent" stream came out at full volume. The likely mechanism is a capture
buffer with nothing valid (no writers, or a chunk flagged empty) being copied
verbatim; the old loop also took its size from the *minimum* over the input planes
(`UINT32_MAX` when there were none) and dereferenced `data` unchecked.

## Dynamics: the DSP stage

Phase B (2026-10-21). `src/Audio/Dynamics.{h,cpp}` is a stereo-linked compressor
followed by a 1.5 ms look-ahead peak limiter, pure C++ with no PipeWire in it (unit-tested
offline, `tests/test_audio_dynamics.cpp`). The playback callback's order per cycle is:
`CopyPlaneSafe()` per plane (copy + clamp) → `Dynamics::Processor::Process()` over **all**
planes in place → `ClampPlaneSafe()` again over each → `DirectionHook()`. The second clamp
means whatever the DSP does, what reaches the speakers is within [-1, 1] and finite, so the
"safe by construction" guarantee below holds with the stage in the path.

User-facing: presets Off / Even out / Night / Limiter only plus six Advanced values, per
profile, in the Mixer (`audio.dynamics.*`, see [audio-mixer.md](audio-mixer.md)). Default
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

## Failure behaviour

Everything degrades to "as before": no PipeWire (`pw_context_connect` fails), a stream
error, a lost connection, or no `default` metadata (WirePlumber not running) leaves
`Active()` false, records a one-line `StatusText()` for the Mixer's Diagnostics, and
the stream guesser keeps working. No reconnect (`ponytail:` a PipeWire restart takes
the game's own streams with it; capture stays off until gamescope restarts). Compiled
without libpipewire, every function is a no-op and `StatusText()` says so.

## Config

`audio.capture` — per profile, default **on** (`config::AudioSettings`, JSON
`"audio": { "capture": true }`). Additive, no schema bump: a profile without the object
reads as on. Read at startup and on a profile switch by `ritz_apply_config_live()` →
`Capture::SetWanted()`, and pushed live by the Mixer switch's own setter (a routed write
does not bump the config generation, so the hook does not run for it; fixed 2026-10-22). The Mixer's "Capture game audio" switch row is `audio.capture`
(see audio-mixer.md).

`audio.dynamics.*` — per profile, default preset **off** (`config::AudioDynamicsSettings`,
JSON `"audio": { "dynamics": { "preset", "threshold_db", "ratio", "attack_ms",
"release_ms", "makeup_db", "ceiling_db" } }`). Additive, no schema bump. Clamped on
load to the ranges `Dynamics.cpp` enforces (−80..0 dB, 1..20, 0.1..500 ms, 1..5000 ms,
±24 dB, −24..0 dB). `AudioDynamicsParams()` (`PanelAudio.h`) turns it into the
`Dynamics::Params`: a named preset runs the library's own values, `custom` runs the six
stored ones. Applied at startup and on a profile
switch by `ritz_apply_config_live()` → `Capture::SetDynamics()`, and on every in-session
edit by the Mixer rows themselves: `AudioDynamicsPickPreset()` / `AudioDynamicsSetValue()`
(`PanelAudio.h`) write the stored settings and call `SetDynamics()` through a callback.
`Why:` a routed write does not bump the config generation, so the live-apply hook never
runs for it; until 2026-10-22 the rows only saved and a preset did nothing until a restart
or profile switch (the user's "the compressor doesn't work in CS2"). The callback is what
`tests/test_audio_dynamics.cpp` drives. At ratio 1 the compressor is off and only the
limiter (ceiling) acts, so Threshold/Attack/Release do nothing there (the Ratio help says
so); "Limiter only" loads ratio 1.

## Known limits (`ponytail:`)

- The channel layout is read once when the pair is created; a later change of the default
  output's layout (stereo → 5.1) is followed only after capture is switched off and on.
- If an outer gamescope-ritz already tagged the environment, an inner one leaves the outer
  tag standing (the game is captured by the outermost).
- A game started by something *other* than gamescope's child tree is never tagged.
- The Mixer's stream list would show our playback node (a Stream/Output/Audio node like
  the game's); `Volume.cpp`'s poll drops our nodes at the source (by `PlaybackNodeId()` /
  `SinkNodeId()` and by the `gamescope-ritz.` `node.name` prefix, since the playback node
  can reach the registry before its id is published), so they are never listed or guessed.
- The sink's **volume is saved per game** (`games.<key>.game_volume` in `global.json`,
  2026-10-10), not per profile; **mute is not saved** (every session starts unmuted). A
  fresh sink is created each session (its `node.name` carries the pid) and carries
  `state.restore-props=false`, so WirePlumber remembers nothing for it; the saved value
  is the sink's volume *wish*. `LaunchNestedChildren` (`steamcompmgr.cpp`) tells config
  the launched command (`config::NoteLaunchCommand(argv[0])`), asks
  `config::SessionGameVolumeKey()` for the key and calls
  `Capture::SetVolume(config::GetGameVolume(key))` right before `Capture::Start()`;
  `SetVolume` only records the wish before `Start()` and applies it at connect / PAUSED /
  STREAMING, so it is in place before the first sample. **The key**
  (`config::ChooseGameVolumeKey()`, `AppId.h`, pure and unit-tested): the resolved app
  id when there is one, else the basename of the primary child's executable, both
  through `SanitizeAppId()`; with neither (no child, no id) nothing is saved or restored
  and the slider is session-only. Only a Mixer slider move saves (`config::SetGameVolume`,
  the debounced `global.json` write); external changes mirrored into `GetVolume()` are
  not persisted and applying never saves, so there is no feedback loop. The value is a
  display fraction clamped to 0..1.5 on load and write; absent = 1.0, and unity writes
  nothing. `Why:` the user, 2026-10-10: *"We need some way of saving the game volume"*,
  then, correcting a first per-profile cut: *"Even if two games use the same profile, it
  has to somehow differentiate between them"* -- hence per game. Mute stays session-only
  on purpose: a game that starts muted next launch would look broken. Stored in the
  existing `games` map (next to `selected` / `audio_node`) because nothing there treats
  an entry's presence as "this is a game": profile assignment looks up only
  `SessionAppId()` (the fallback key is never one), the Friends gate asks
  `ResolveAppId()`, and an entry with an empty `selected` means "never chose". Weakness:
  a command like `wine game.exe` or `env ... game` keys on `wine` / `env`, shared by every
  such game -- set `RITZ_GS_APPID` to separate them. The guessing path is unchanged
  (WirePlumber restores the game's own stream volume per app there, and the saved value
  is not applied to a guessed stream).

## Live checks

Silent-only, `pw-cat --volume 0.01 /dev/zero`, our sink at 0.01 first, a few seconds
each: the sink appears (`wpctl status`, `pactl list short sinks`); a tagged client is
linked into it (`pw-link -l`); our playback is linked to where the client was headed
(`easyeffects_sink` while EasyEffects processes all outputs, else the default device);
after exit
and after `kill -9` nothing of ours remains in `wpctl status`.
