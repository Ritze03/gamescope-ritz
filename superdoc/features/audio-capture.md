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
dynamics (compressor/limiter) come in Phase B.

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
  `CopyPlaneSafe()` — the **DSP hook point** for Phase B.
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
reads as on. Read at startup and pushed live by `ritz_apply_config_live()` →
`Capture::SetWanted()`. The Mixer's "Capture game audio" switch row is `audio.capture`
(see audio-mixer.md).

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
- The sink's volume and mute are not persisted: every session starts at 100% / unmuted
  (a fresh sink is created each session; its `node.name` carries the pid). The
  guessing path never persisted either, but WirePlumber restored the game's own stream
  volume per app, which still applies to the stream inside the sink.

## Live checks

Silent-only, `pw-cat --volume 0.01 /dev/zero`, our sink at 0.01 first, a few seconds
each: the sink appears (`wpctl status`, `pactl list short sinks`); a tagged client is
linked into it (`pw-link -l`); our playback is linked to where the client was headed
(`easyeffects_sink` while EasyEffects processes all outputs, else the default device);
after exit
and after `kill -9` nothing of ours remains in `wpctl status`.
