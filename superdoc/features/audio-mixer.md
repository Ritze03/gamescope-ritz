# Mixer (`audio.mixer`)

Volume for the game gamescope is hosting, plus per-app volume for everything else
playing. UI: `src/Overlay/PanelAudio.{h,cpp}` (registers as the `audio.mixer` area, rail
label "Mixer"). Logic: `src/Audio/Capture.{h,cpp}` (game audio capture, see
[audio-capture.md](audio-capture.md)) and `src/Audio/Volume.{h,cpp}` (the wpctl-based
stream guessing and per-stream volume).

**Two paths drive "Game volume"** (Phase A, D5/D13 of the game-audio-effects plan):

- **Capture** — while `Audio::Capture::Active()`, the row is
  `Capture::SetVolume()/GetVolume()/SetMute()/GetMute()` on the private sink the game
  plays into: one control for everything the game plays, no guessing. These are
  non-blocking and safe per frame. It needs no wpctl.
- **Fallback** — capture off, or it failed: the original stream guessing, exactly as
  before capture existed — a slider and mute toggle over whichever stream detection
  resolves as "the game" (`Audio::GetState()`/`Audio::RequestVolume()`/`RequestMute()`/
  `SetManualSelection()`; the four detection strategies — manual, PID-tree,
  process-name, newest-since-launch — are in `Volume.h`'s top comment, and why v1 shells
  out to `wpctl` rather than talking to PipeWire directly). The row appears only once a
  stream is detected.

`Why:` with capture on the guess is gone from the normal path (it kept picking the wrong
app, and a sandboxed game's PID never lined up), while keeping it as the fallback means a
machine where the sink cannot be made (no PipeWire socket, no WirePlumber metadata) still
has today's Mixer. The two never both drive a row: the id `audio.stream.volume` is
declared once, by whichever path is current, and `AudioGeneration()` mixes
`Capture::Active()` into the row-set hash so the area rebuilds when it flips.

Persistence: the Mixer keeps no volume value of its own. On the fallback path WirePlumber
already remembers per-application stream volume; the capture sink is created fresh each
session, so **its volume and mute start at 100% / unmuted every session** (a deliberate
non-feature, per the plan: persistence was not part of this step). What IS persisted, per
game, is the manual stream *selection* (`config::GameEntry(appId).audio_node`) — a
different thing from a volume value, since WirePlumber has no concept of "which stream
did the user mean." The capture switch itself (`audio.capture`) is per profile.

**Hotkey** (2026-09-27) — the `mixer` **keybind action**, default
`Ctrl+Shift+M`, opens the settings shell directly on this area (or switches
it here if the shell is already open elsewhere), and closes the shell again
if it is already showing Mixer. Global, editable from Setup > Keybinds
alongside every other action — see
[keybinds.md](keybinds.md#the-actions).

## Rows

- **`audio.capture`** ("Capture game audio", a `Switch`, default on) — the per-profile
  config key itself (`config::AudioSettings::capture`, JSON `audio.capture`), declared in
  the "Game audio" group at the top. Writing it goes through `EnqueueRoutedWrite()`; the
  live apply hook (`ritz_apply_config_live()` → `Capture::SetWanted()`) creates or
  destroys the sink on the spot. Its help says what D8 says: while on, the game's audio
  goes straight to the system default output, so it **bypasses EasyEffects** (and any
  other per-app routing) unless EasyEffects is the default output.
- **`audio.stream`** ("Stream", a `Choice`; group "Game stream", retitled "Game stream
  (used if capture is off)" while capture is active) — only the fallback path uses it;
  its help says so. "Automatic" (index 0, the
  default) lets detection pick the stream; every other option is one live
  stream, offered by identity (`application.process.binary`, falling back
  to `application.name`) so the pick survives a relaunch. A stream that
  reported neither is not offerable (it has no identity to remember) and is
  instead counted in the Diagnostics "not pinnable" fact. A manual pick
  whose stream isn't currently live is still shown, tagged "(not
  streaming)", rather than silently reverting to "Automatic".
- **`audio.stream.volume`** ("Game volume", a `Slider`, 0–150%, carries a `mute` param;
  built by `EmitGameVolumeRow()` for both paths) — **capture path:** drawn whenever
  capture is `Active()`, titled plain "Game volume", bound to `Capture::*` through its own
  optimistic-pending slot (`s_PendingCapture`). **Fallback path:** drawn only
  once `Audio::VolumeState::bDetected` is true. Moves every node behind
  `VolumeState::vecMatchedNodeIds` together (`Audio::RequestVolume()`, not
  the per-node call), which is what makes this row different from the
  per-stream rows below when a game owns several nodes sharing one winning
  identity. **Dynamic title** (fallback path only) — see its own section below.
- One `audio.node.<nodeId>` slider per other live stream (the "Streams"
  group) — issue #36. On the fallback path the row the Stream picker/detection resolved
  as primary is skipped here (it's the row above); on the capture path Game volume is
  the sink, not a stream, so nothing is skipped (the game's own streams keep a per-app
  row). Every other live
  Stream/Output/Audio node gets its own independent slider + mute, titled
  with `StreamName()` (see below). Binds straight to `RequestVolumeForNode`/
  `RequestMuteForNode`, by node id, so a positional row never silently
  retargets when the stream above it ends.
- **Diagnostics** (`audio.server`, a read-only `Facts` row) — **capture** (`Capture::
  StatusText()` plus the live stream count while Active, e.g. `Active
  (gamescope-ritz.1234), 2 streams`, or `Off (switched off in this profile)` /
  `Unavailable: …` / `Error: …` — why capture is not in use), **game volume drives**
  (`the capture sink (gamescope-ritz.1234)`, `a guessed stream (<method>)`, or `nothing`),
  then wpctl present/missing, the detection method in play, matched node ids, total
  stream count (with the tied-candidates case reported, never silently resolved), the
  manual override (and whether it's currently stale), and the "not pinnable" count. The
  collapsed summary is prefixed `capture on` / `capture not active` and the area's rail
  summary reads `game audio captured · N streams` while capture is active. Every status
  the legacy pre-E2 panel printed as prose lands here as a live fact instead of dead
  prose.

## Our own nodes are never listed or guessed

Capture's playback stream (`gamescope-ritz.<pid>.out`) is a Stream/Output/Audio node like
the game's, and as the newest stream it is exactly what the "newest since launch" tier
would pick. `Volume.cpp`'s poll drops our nodes right after `wpctl inspect`, before they
reach the candidate list, `SelectCandidate()` or the stream count: by the ids Capture
reports (`PlaybackNodeId()`, `SinkNodeId()`, pushed in by the panel every frame through
`Audio::SetOwnNodeIds()` so `Volume.cpp` does not link `Capture.cpp` — the unit tests link
`Volume.cpp` alone) **and** by `Parse::IsOwnCaptureNodeName()`
(the `gamescope-ritz.` `node.name` prefix) — the name check because the playback node can
reach the registry before Capture has published its id. `Why:` filtering once at the
source means the Streams rows, the Stream picker, the totals and the guessing all agree.

## wpctl scale (fixed 2026-10-07)

`wpctl get-volume`/`set-volume` speak **the cubic scale**, the same as the Mixer's display
fraction (`channelVolumes = value^3`). `Volume.cpp` used to treat wpctl as raw linear: it
sent `display^3` to `set-volume` (wpctl cubed it again, so a 50% slider wrote
`channelVolumes` ≈ 0.002, about −54 dB) and ran `cbrt()` over the `get-volume` reading (a
stream at 80% displayed as 93%). Checked read-only on wpctl 0.5.18 against live nodes:
`channelVolumes` 0.38742 / 0.511977 / 0.713888 read back as `Volume: 0.73 / 0.80 / 0.89`.
Now the display fraction goes to wpctl unchanged and the reading is used as-is; only
code that writes `channelVolumes` itself (`Capture.cpp`) applies `DisplayToLinearVolume()`.
So the slider means the same thing on both paths, and the per-app and fallback Game
volume rows are now louder, for a given slider value, than they were (they were wrongly
near-silent below 100%). Pinned in `tests/test_audio_volume.cpp`.

## Stream naming (`StreamName()`)

A stream's display name is `StreamCandidate::sLabel`, assembled once in
`Volume.cpp`'s poll thread with issue #63's precedence: `application.name`,
falling back to `media.name`, falling back to whatever `wpctl status` itself
printed for the node. This is not re-derived anywhere else in the panel —
every row that names a stream (the Stream picker's option text, the
per-stream "Streams" rows, and the dynamic "Game volume" title below) reads
this one field, so they always agree.

## The "Game volume" row's dynamic title

Added 2026-09-27. The user's own words: *"if the window doesnt have a fixed
audio stream, show the audio stream name instead of 'Game volume'. Just so
the user can quickly confirm, that it was detected correctly."*

- **While Automatic** (`s_sManualNode` empty) **and detection found a
  usable name**, the row's title is that name — the identical string
  `StreamName()` would show in the Stream picker for that node — instead of
  the literal "Game volume". A glance at the row now confirms detection
  landed on the right app.
- **While a stream is pinned by hand**, the title stays plain "Game
  volume" — the user already knows which stream it is, having picked it
  from the list themselves.
- **No usable name** (nothing detected, or the primary matched node
  reported neither `application.name` nor `media.name`) falls back to
  plain "Game volume" too.
- **Several matched nodes** (`VolumeState::vecMatchedNodeIds.size() > 1` —
  one game legitimately owns more than one Stream/Output/Audio node under
  the same winning identity, e.g. a stereo pair opened as separate nodes)
  — the title names the **lowest node id** among them (`PrimaryMatchedStream()`
  in `PanelAudio.cpp`: `s_vecAreaStreams` is already sorted ascending by
  node id, so "the first match found" is deterministic and stable across a
  poll tick, unlike the push order `SelectCandidate()` builds
  `vecMatchedNodeIds` in, which isn't sorted). A `"  (+N)"` suffix is
  appended for the other `N` matched nodes, so the row is honest that more
  than one stream moves together rather than silently naming the group
  after only one of them. In practice this rarely matters: nodes sharing
  one winning identity are (almost by construction) the same application
  under a different node, so their names usually agree anyway.

**The row's id never changes** — always `audio.stream.volume` — only its
*title* does; the settings audit, the command palette's identity for the
row, and anything keying off the id are unaffected.

**Palette findability**: the command palette's search blob is built from a
row's *current* title plus its id and keywords
(`CommandPalette.cpp`'s `HayForEntry()`). Since this row's title is no
longer always the literal string "Game volume", the row's `Keywords()` now
explicitly includes the literal phrase `"game volume"` (not just the two
words separately) so a search for "game volume" still finds the row
regardless of what it's currently titled.

**Implementation**: the actual decision is a pure function,
`gamescope::AudioMixerPrimaryRowLabel(bManualStreamPicked,
sPrimaryStreamName, nOtherMatchedNodes)`, declared **inline in
`PanelAudio.h`** (not declared-there/defined-in-the-.cpp) specifically so
`tests/test_audio_volume.cpp` can exercise it directly with no live
PipeWire state, no config, no ImGui and no `ui::Registry` — and without
adding anything to `tests/meson.build`. `PanelAudio.cpp`'s
`PrimaryStreamRowLabel()` threads the live state (`s_sManualNode`,
`PrimaryMatchedStream()`, `VolumeState::vecMatchedNodeIds`) into it. No
persistent/static string storage is needed for the label itself:
`ui::Entry::Emit()` copies a row's `pszTitle` into its own `std::string`
(`Registry.h`'s `m_sTitle`) immediately, so the label only has to live for
the duration of the `.Slider()` call — unlike the Stream picker's own
`s_dqOptionText` deque, which DOES have to keep its C strings alive across
the frame because `ui::Option` holds a bare `const char *`.

The row set (which streams exist at all) is discovered per frame via
`ui::Area::Rebuilds()` (`AudioGeneration()` / `BuildAudioArea()`) — the
generation hash already mixes every matched node id and every candidate's
`sLabel`, so a change to who's detected, how many nodes are matched, or
what any of them is named triggers a rebuild on its own; nothing extra was
needed to make the dynamic title's dependencies participate in that hash.

## Volume jump-back fix (optimistic pending state)

`Audio::GetState()`/`GetAvailableStreams()` are snapshots of the background
poll thread's last 750ms-cadence tick. Between a slider calling
`RequestVolume*()` and the next tick landing, a naive read of live state
would show the pre-change value and the slider would visibly snap back,
then jump forward once the poll thread caught up (issue #36). Fixed with
UI-side optimistic local state (`PendingRowState`,
`ResolveDisplayVolume()`/`ResolveDisplayMute()`): the row remembers what it
just requested and keeps showing that until either the poll thread agrees
or a 1s timeout elapses (a safety net against a wpctl call that silently
failed). `s_PendingPrimary` covers the fallback-path "Game volume" row (which has
no stable single node id from the UI's POV); `s_PendingCapture` the capture-path one; `s_PendingByNode` covers every
other stream row, keyed by node id.
