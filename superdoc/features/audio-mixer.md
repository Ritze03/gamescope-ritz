# Mixer (`audio.mixer`)

Per-app PipeWire volume for the game gamescope is hosting: a slider and mute
toggle over whichever stream detection resolves as "the game", a manual
picker to correct a wrong guess, one slider per other live stream, and a
Diagnostics group that explains every way detection can come up short. UI:
`src/Overlay/PanelAudio.{h,cpp}` (registers as the `audio.mixer` area,
rail label "Mixer"). Logic: `src/Audio/Volume.{h,cpp}` — the panel only
ever reads `Audio::GetState()`/`Audio::GetAvailableStreams()` and calls
`Audio::RequestVolume()`/`Audio::RequestMute()`/`Audio::SetManualSelection()`;
the wpctl shell-outs, the PID/name/recency matching and the volume curve all
live there. See that header's own top comment for the four detection
strategies (manual, PID-tree, process-name, newest-since-launch) and why v1
shells out to `wpctl` instead of talking to PipeWire directly.

Not persisted: WirePlumber already remembers per-application stream volume
itself, so this panel deliberately keeps no copy of its own — it is a live
control surface only. What IS persisted, per game, is the manual stream
*selection* (`config::AudioSettings::manual_node_binary`, via
`config::GameEntry(appId).audio_node`) — a different thing from a volume
value, since WirePlumber has no concept of "which stream did the user mean."

## Rows

- **`audio.stream`** ("Stream", a `Choice`) — "Automatic" (index 0, the
  default) lets detection pick the stream; every other option is one live
  stream, offered by identity (`application.process.binary`, falling back
  to `application.name`) so the pick survives a relaunch. A stream that
  reported neither is not offerable (it has no identity to remember) and is
  instead counted in the Diagnostics "not pinnable" fact. A manual pick
  whose stream isn't currently live is still shown, tagged "(not
  streaming)", rather than silently reverting to "Automatic".
- **`audio.stream.volume`** ("Game volume", a `Slider`, 0–150%) — drawn only
  once `Audio::VolumeState::bDetected` is true. Moves every node behind
  `VolumeState::vecMatchedNodeIds` together (`Audio::RequestVolume()`, not
  the per-node call), which is what makes this row different from the
  per-stream rows below when a game owns several nodes sharing one winning
  identity. Carries a `mute` param. **Dynamic title** — see its own section
  below.
- One `audio.node.<nodeId>` slider per other live stream (the "Streams"
  group) — issue #36. The row the Stream picker/detection resolved as
  primary is skipped here (it's the row above); every other live
  Stream/Output/Audio node gets its own independent slider + mute, titled
  with `StreamName()` (see below). Binds straight to `RequestVolumeForNode`/
  `RequestMuteForNode`, by node id, so a positional row never silently
  retargets when the stream above it ends.
- **Diagnostics** (`audio.server`, a read-only `Facts` row) — wpctl
  present/missing, the detection method in play, matched node ids, total
  stream count (with the tied-candidates case reported, never silently
  resolved), the manual override (and whether it's currently stale), and
  the "not pinnable" count. Every status the legacy pre-E2 panel printed as
  prose lands here as a live fact instead of dead prose.

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
failed). `s_PendingPrimary` covers the primary/"Game volume" row (which has
no stable single node id from the UI's POV); `s_PendingByNode` covers every
other stream row, keyed by node id.
