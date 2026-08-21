# PipeWire Loudness Control — Planning Scout

Scope: can the planned ImGui overlay show and control the **system-level PipeWire volume
of the game process gamescope is hosting** (not an internal gain gamescope applies to a
stream it owns)? This is a planning document only — no code, no prototypes, no system
audio state was changed while researching it.

## Verdict

**Awkward, not genuinely risky — but the crux question (node identification) has no
100%-reliable answer, only a good-enough heuristic, and that has to be accepted up
front rather than discovered late.** Gamescope's existing PipeWire code
(`src/pipewire.cpp`) is 100% video — it opens one outbound stream and has never
enumerated a node, so audio control is new surface area, not an extension. The
mechanical parts (reading/writing a node's volume, the linear→cubic UI curve, the
build/dependency story) are well-trodden PipeWire ground with clear APIs. The one
genuinely uncertain part is mapping "gamescope's child process tree" onto "a PipeWire
stream node," because a Steam/Proton launch commonly goes through `pressure-vessel`/
`bwrap` sandboxing, which can put the real game process in a different PID namespace
than the PID gamescope itself sees — silently breaking naive PID matching for exactly
the platform this feature is presumably most wanted on. Recommended path: shell out to
`wpctl` for v1 (it already implements PID-based node matching as a first-class flag,
which removes most of the "how do I even call this API" work), and treat the
namespace-mismatch failure mode as an explicitly accepted, user-visible "not detected"
state rather than something to solve perfectly before shipping.

## 1. What PipeWire integration exists today, and is it reusable for audio?

`init_pipewire()` (`src/pipewire.hpp:58`, implemented `src/pipewire.cpp:671`) does, in
order: `pw_context_new` (`src/pipewire.cpp:688`) → `pw_context_connect`
(`:694`) → `pw_stream_new` with `PW_KEY_MEDIA_CLASS = "Video/Source"`
(`:700`-`704`) → `pw_stream_connect(..., PW_DIRECTION_OUTPUT, ...)` (`:723`). That's the
entire API surface. I grepped the file for `pw_registry` and got **zero hits** — this
module has never enumerated a global, bound a proxy, or read/written a `SPA_PARAM_Props`
on anything; it is purely a one-way video-frame producer talking to its own private
`pw_core`.

Reusable: the *pattern*, not the *state*. `pipewire_state` is a private file-scope
singleton (`src/pipewire.cpp:19`) built for exactly one stream; there's no clean seam to
attach a registry listener to the same `pw_core` without coupling audio-control's
lifetime to the video-capture feature's (which the `pipewire` build option already
somewhat does — see §4). The thread/loop scaffolding is the reusable part: a dedicated
`std::thread` named `"gamescope-pw"` (`src/pipewire.cpp:746`) running a private blocking
`poll()` over the PipeWire loop fd plus a nudge pipe (`:632`), with buffers/commands
handed across via `std::atomic<T*>` (`out_buffer`/`in_buffer`, `:22`-`:24`) and a
`nudge_pipewire()` wake call. Recommendation: a **second, separate `pw_context`/`pw_core`
connection** for audio (its own thread or a `pw_thread_loop`, PipeWire's own built-in
equivalent of the hand-rolled poll loop gamescope already uses) rather than repurposing
the video stream's core — same library, same build gate, independent lifetime.

## 2. The hard identification problem

**What the live system actually shows** (`pw-dump`, read-only, checked on this machine):
`Stream/Output/Audio` nodes for real applications carry `application.process.id` and
`application.process.binary` — e.g. a live Floorp (browser) stream reported
`application.process.id: 546835`, `application.process.binary: floorp`,
`application.name: Floorp`, matching that browser's actual host PID. But this is **not
universal**: a `cava` (Stream/Input/Audio) node on the same dump had `None` for all three
properties — clients populate them voluntarily (via their PipeWire/PulseAudio-compat
client library), and one can turn up empty. `wpctl status` on this machine also confirms
the three-tier structure that matters here: **Devices** (hardware cards) → **Sinks/
Sources** (whole-output-device volume — the wrong target) → **Streams** (per-app
volume — the *right* target, this is the "current process" the user means).

**The matching primitive gamescope already has**: `Process::GetChildPids(pid_t nPid)`
(`src/Utils/Process.cpp:65`) walks `/proc/*/stat` reading each process's parent field and
returns direct children of `nPid`; `Process::KillAllChildren`
(`src/Utils/Process.cpp:113`) recurses it into `KillProcessTree`
(`src/Utils/Process.cpp:103`) to reach the *entire* descendant tree — this is precisely
the "walk gamescope's owned process tree" primitive the crux question asks for, and it
already exists and is exercised today (by the reaper's cleanup path,
`src/Apps/gamescopereaper.cpp`, which becomes a subreaper via `Process::BecomeSubreaper`
so orphaned grandchildren — a spawned Proton/Wine process's own children — reparent to
it instead of PID 1, keeping them inside the tree this walk can find). The intended
usage: start the walk from the PID gamescope's reaper wraps (`Process::SpawnProcess`
inside `GamescopeReaperProcess`, `src/Apps/gamescopereaper.cpp`), recurse to a
descendant-PID set at match time (not cached — this is a discovery-time query, not a
hot-path one), and intersect that set against the `application.process.id` values seen
in a PipeWire registry snapshot (or `pw-dump`/`wpctl` output, see §6).

**wpctl already builds exactly this primitive in, on its side**: `wpctl set-volume ID
VOL[%] --pid` / `wpctl set-mute ID 1|0|toggle --pid` — "Treat ID as a process ID and
affect all nodes associated with it" (verified against the WirePlumber `wpctl(1)` man
page, https://man.archlinux.org/man/extra/wireplumber/wpctl.1.en, checked 2026-08-21).
So the "given a PID, find/control its audio node(s)" half of the problem is already
solved by the tool if we shell out — gamescope's own job reduces to "which PID(s), among
my descendants, actually have one," which still has to be answered by our own
tree-walk + intersection (wpctl's `--pid` matches one exact PID's nodes; it does not
walk a process tree itself).

**Honest verdict — not 100% reliable.** PID matching against `/proc` (gamescope's view)
against `application.process.id` (the PipeWire client's own view of its PID) only agrees
when both are in the same PID namespace. Steam's real Proton launches on Linux commonly
run through `pressure-vessel`/`bwrap` sandboxing (Steam Linux Runtime "soldier"/"sniper"
containers) — general web research on `pressure-vessel`/`bwrap` container behavior
surfaced a `--share-pid` control knob on the `pressure-vessel-unruntime` tooling
(https://github.com/ValveSoftware/steam-runtime, checked 2026-08-21) implying PID
namespace sharing is *configurable*, not something I can confirm is always on or always
off for a given install without a live Proton launch to test against (which this
read-only planning pass did not do — no game was launched). Flag this as **the single
biggest unresolved risk**, not a confidently-solved detail: if the container does not
share the host PID namespace, `application.process.id` inside PipeWire will not appear
in gamescope's own `/proc`-rooted descendant walk at all, and PID matching silently
finds nothing.

**Fallback strategy when PID matching yields zero or ambiguous results:**
- Zero matches (also the normal *transient* case — many games open their audio device
  lazily, well after the process itself has started): don't error, show "audio: not yet
  detected," and re-attempt on every PipeWire registry `global`/`global_remove` event
  (or, if shelling out, on a slow poll) rather than once at startup.
- Multiple matches (a launcher plus the actual game both emit audio, or a companion
  voice-chat process is a tracked child too): either present a small picker or apply the
  volume/mute to every matched node at once — this is a product decision, not a purely
  technical one (see Open Questions).
- PID matching finds nothing even though a game is clearly making sound (the
  namespace-mismatch case): a secondary heuristic — match `application.process.binary`
  against the launched executable's basename, or against known Proton/Wine process names
  (`wine64-preloader`, `wineserver`) — can be tried, but it's a heuristic, not a
  guarantee, and should be documented to the user as best-effort. The last-resort fallback
  is a manual "which stream is this" picker — the same thing a human already does today in
  `pavucontrol`/`helvum` when guessing which "Wine" entry is their game, so this isn't a
  regression versus current desktop tooling, just not an invisible one-click experience.

## 3. Read/write mechanism

Confirmed from the local system's installed `spa/param/props.h`
(`/usr/include/spa-0.2/spa/param/props.h:66`,`:70`, not a repo file, but the header the
build already compiles against once `pipewire` is enabled — `pkg-config --cflags
libpipewire-0.3` shows `-I/usr/include/spa-0.2` is pulled in transitively, no separate
`libspa` link needed):
- `SPA_PROP_channelVolumes` — "a volume array, one (linear) volume per channel... 0.0 is
  silence, 1.0 is without attenuation. This is the **effective** volume that is applied."
  This is the field to read/write for "this stream's volume."
- `SPA_PROP_softVolumes` exists separately as the software-only component (when part of
  the volume is applied in hardware); for an application *stream* node (not a hardware
  sink), volume is applied in software, so `channelVolumes` is the field that matters.
- `SPA_PROP_mute` — plain bool.

Mechanically (general PipeWire API knowledge, **not verifiable against source present in
this repo** — no PipeWire library source tree is checked out here, only its headers —
cross-checked against `docs.pipewire.org`, https://docs.pipewire.org/group__pw__registry.html,
checked 2026-08-21): a client creates a `pw_registry` off its `pw_core`
(`pw_core_get_registry`), receives a `global` event per existing object including every
node, `pw_registry_bind()`s a proxy to the node(s) of interest, and can then read current
params via the node's own param-changed events and write new ones via `pw_node_set_param`
/ the stream-control equivalent, building a `spa_pod` of type `SPA_TYPE_OBJECT_Props`, id
`SPA_PARAM_Props`, containing an updated `channelVolumes` array. This confirms gamescope's
existing zero use of `pw_registry` (§1) is a real gap, not an oversight to route around.

**Persistence / what "system level" means here.** WirePlumber ships a documented
`node.stream.restore-props` setting: "WirePlumber stores stream parameters such as
volume and mute status for **each client (i.e. application) stream**... will restore the
previously stored stream parameters when the stream is activated"
(https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/settings.html,
checked 2026-08-21). This is what makes a volume change made via `wpctl`/`pavucontrol`
feel "system level" the way the user's request implies: it's stored by WirePlumber, not
by the app that changed it, and any tool reading PipeWire sees the same value. Two
caveats I could not verify against this specific system's WirePlumber config (only the
setting's existence in general docs, not this machine's actual restore-keying behavior
for a Proton process specifically): (a) exactly what identity key WirePlumber restores
by (the docs describe it as per-client/per-application but don't spell out whether that
survives a Proton relaunch producing a fresh, differently-propertied stream each time),
and (b) whether that restore fires *before* our overlay would want to show a value. Flag
as open — see §7 and Open Questions.

**Curve.** `channelVolumes` is raw linear amplitude per the header comment above. Human
loudness perception is not linear in amplitude, so UI sliders in this ecosystem use a
cubic mapping instead: WirePlumber's own `device.routes.default-sink-volume` setting
documents its default explicitly as `"0.4 ^ 3 (40% on the cubic scale)"`
(same settings page as above, checked 2026-08-21) — i.e. the convention is
`linear = (display_fraction)^3`, `display_fraction = linear^(1/3)`. **Recommendation: the
overlay's slider should present a 0–100% (optionally up to 150% "boost," matching
`pavucontrol`/`wpctl` convention) value and convert to/from the raw linear float with a
cubic curve**, not a 1:1 linear mapping, so the control feels consistent with the rest of
the user's PipeWire-based desktop tools rather than feeling "wrong" at the low end.

**Stream node vs sink/device — confirmed via live `wpctl status`** (read-only, this
machine): the tool itself separates `Devices` → `Sinks`/`Sources` → `Streams` into
distinct sections; setting a Sink's volume changes the whole system's output level
(everything routed through that device), which is explicitly *not* what was asked for.
The target must be the Stream node matched in §2, never the Sink/Device.

## 4. Dependency and build impact

- Already linked: `pipewire_dep = dependency('libpipewire-0.3', required:
  get_option('pipewire'))` (`meson.build:45`), plus `librt_dep` gated on the same option
  (`meson.build:46`). `libspa-0.2` headers ride along for free via `libpipewire-0.3`'s
  pkg-config Cflags (verified: `pkg-config --cflags libpipewire-0.3` on this machine
  emits `-I/usr/include/spa-0.2`) — no separate Meson dependency needed for SPA.
- The option is `feature`-typed: `option('pipewire', type: 'feature', description:
  'Screen capture via PipeWire')` (`meson_options.txt:1`), default `auto`, gated at
  compile time via `-DHAVE_PIPEWIRE=@0@` (`meson.build:74`) and consumed with `#if
  HAVE_PIPEWIRE` guards at six sites across `src/main.cpp`, `src/wlserver.cpp`, and
  `src/steamcompmgr.cpp` (grepped).
- **Recommendation: reuse this same option** for audio control rather than adding a new
  one — it's the same library, and it keeps one flag instead of two for users to reason
  about. The overlay's audio panel should compile out / gray out under `#if
  !HAVE_PIPEWIRE`, matching the existing guard pattern.
- Note (not acted on — planning only): `meson_options.txt:1`'s description string
  ("Screen capture via PipeWire") becomes stale/misleading once the same flag also gates
  audio control; worth a one-line wording update whenever this is actually implemented.

## 5. Threading

Two established cross-thread patterns already exist in this codebase (per
`superdoc/architecture/overview.md`'s verified Threading model section):

- **Lock + dirty-flag + nudge**, used for the main/steamcompmgr thread pair sharing one
  tightly-coupled frame/focus model (`wlserver_lock()`/`wlserver_unlock()`,
  `MakeFocusDirty()`, `nudge_steamcompmgr()`, `src/steamcompmgr.cpp:831`,`:7537`).
- **Dedicated thread + atomic-pointer mailbox + nudge pipe**, used for PipeWire video,
  because PipeWire's `pw_loop` expects to own its iterate cycle and must not be paced by
  gamescope's own loops (`out_buffer`/`in_buffer` atomics, `nudge_pipewire()`,
  `src/pipewire.cpp`).

PipeWire's client API is loop-affine — registry/proxy calls (bind, set_param, and
reading param-changed callbacks) are meant to run on the thread iterating that `pw_core`'s
loop, the same constraint gamescope's existing video code already respects by giving
PipeWire its own private thread rather than folding it into the shared epoll waiter. Audio
control should follow the **same second pattern**, not the lock/dirty-flag one — it's a
different subsystem talking to a different external loop, not shared frame/focus state.

Where does the UI slider itself live? **Unverified/inferred, flagged explicitly**: no
ImGui integration exists anywhere in `src/` yet (grepped, zero hits) — this whole overlay
is still pre-implementation. `superdoc/planning/ui-design-guide.md` describes it as drawn
over the composited frame, and compositing (`paint_all()`/`vulkan_composite()`) runs on
the **steamcompmgr thread**, not the main `wlserver_run()` thread — so the slider's
draw+interact code most likely runs there. Treat this as an assumption to confirm once
the ImGui host thread is actually decided, not a settled fact.

**Recommended concrete pattern** (a direct extension of the proven video-buffer
handoff, not a new design):
1. New dedicated PipeWire-audio thread (or a `pw_thread_loop` — PipeWire's own built-in
   helper for exactly this, arguably cleaner to start fresh with than hand-rolling the
   poll()+nudge-pipe idiom `pipewire.cpp` uses, since `pw_thread_loop_lock`/`unlock`
   gives a direct call-in mechanism).
2. UI thread (steamcompmgr) writes a *desired* volume/mute into a small atomic/locked
   "pending command" slot — the same shape as the existing `in_buffer`/`out_buffer`
   atomic-pointer pattern — and nudges the audio thread.
3. The audio thread, and *only* the audio thread, touches the `pw_core`/registry/node
   proxies: it applies the pending command via the real `SPA_PARAM_Props` write, then
   publishes a "current volume/mute/detected-node-count" snapshot back through a second
   atomic/locked slot.
4. The UI thread reads that snapshot each frame to render the slider position, mute
   state, and "detected N stream(s)"/"not detected" status — mirroring exactly how
   `out_buffer` already flows composited-adjacent state back from the PipeWire thread for
   display today.

This mailbox-in / mailbox-out shape is worth standardizing on for any future
PipeWire-node-control feature the overlay adds, rather than reinventing per-feature.

## 6. Alternatives

| Approach | Cost | Benefit |
| --- | --- | --- |
| Direct `pw_registry`/`pw_node` API (§1–§3, §5) | New code: registry listener, global/global_remove handling, proxy binding, `spa_pod` building for both read and write, plus the full loop-affinity threading design in §5. Zero existing in-repo code to build from (confirmed: no `pw_registry` usage anywhere today). | Live push updates when volume changes externally (e.g. user adjusts it in `pavucontrol` while the overlay is open) come for free via param-changed events; no subprocess spawn per interaction; full control over matching logic. |
| WirePlumber's own API | WirePlumber's externally-facing surface (beyond its internal Lua scripting) is the same PipeWire protocol — no separate, simpler IPC was found for third-party callers in the docs consulted (`pipewire.pages.freedesktop.org/wireplumber`, checked 2026-08-21; no WirePlumber D-Bus service was visible in this machine's live `pw-dump` client list either, though that's a soft negative, not a definitive one). | None over the direct API — same work, extra layer. Not recommended. |
| Shell out to `wpctl`/`pactl` (both confirmed installed: `/usr/bin/wpctl`, `/usr/bin/pactl`) | Reading current volume back means parsing `wpctl status`/`pw-dump` JSON periodically rather than getting a push event (`pw-dump`'s JSON was easy to filter with a one-line script during this investigation — not fragile, just poll-based). Spawning a subprocess per slider-drag tick is wasteful if done naively (debounce before sending). Adds a **runtime** dependency (`wpctl`, the WirePlumber-flavored tool, not PulseAudio's) that isn't build-time checked the way `libpipewire-0.3` is — a missing binary becomes a silent-failure mode this codebase doesn't otherwise have in the audio path. | Genuinely the least code: `wpctl` already implements the exact "given a PID, find and control its node(s)" primitive via `--pid` (§2), no threading-affinity problem (each call is a short subprocess via gamescope's existing `Process::SpawnProcess`), no new registry-listener bookkeeping. |

**Recommendation: shell out to `wpctl` for v1.** Given there is zero existing
registry/audio code in this codebase to extend (§1), and the loop-affinity threading
work in §5 is real, non-trivial new surface area, the shell-out buys a working feature
fast at the cost of (a) a soft runtime dependency the build doesn't currently guard
against, and (b) losing free push-notification of externally-made volume changes (a
polling timer on `wpctl status`/`pw-dump` covers that adequately for an overlay that
isn't open 24/7). If tighter integration or guaranteed availability later matters more
than v1 speed, do the direct API instead — but pick one; don't half-build the direct API
"for later" alongside a shell-out v1.

## 7. What the UI should expose

- **Volume**: 0–100% (optionally to 150% "boost," matching `pavucontrol`/`wpctl`
  convention) on a **cubic** display curve (§3) mapped to PipeWire's linear
  `channelVolumes`.
- **Mute**: boolean toggle → `SPA_PROP_mute` / `wpctl set-mute --pid`.
- **Per-channel vs single fader**: `channelVolumes` is an array (could differ per
  channel), but recommend a single fader that sets every channel to the same value —
  "this game's loudness" isn't a balance control, and per-channel exposure adds UI
  surface a v1 doesn't need.
- **Match-count status**: a small "audio: N stream(s) detected" / "not detected yet"
  readout (§2/§5's snapshot slot) so the control is never silently wrong when the
  heuristic finds zero or several nodes.
- **Persistence — flag as a real conflict, not a settled schema field**: WirePlumber's
  own `node.stream.restore-props` (§3) already tries to remember per-application stream
  volume. If gamescope *also* persists a remembered volume in its own config and
  re-applies it on next launch, the two can disagree — WirePlumber's restore could fire
  first (or later) than gamescope's own re-apply, and whichever writes last "wins"
  invisibly to the user. **Recommendation for v1: don't add a separate gamescope-side
  persisted volume field; let WirePlumber's own restore be the single source of truth**,
  and treat the overlay's slider purely as a live control surface. Revisit only if that
  proves unreliable in practice for this fork's launch pattern (Proton streams may not
  present a stable enough identity across relaunches for WirePlumber's own restore to
  reliably recognize "the same app" either — genuinely unverified, see Open Questions).

## Risks

1. **Node identification is not guaranteed to work on the platform's most common
   real-world launch path.** Steam Linux Runtime's `pressure-vessel`/`bwrap` sandboxing
   can put the actual Proton/game process in a PID namespace gamescope's own `/proc` walk
   doesn't see, silently breaking PID-based matching. This could not be tested in this
   read-only planning pass (no game was launched). Must be verified against a real
   sandboxed Proton launch before committing to the design in §2.
2. **Zero existing precedent in this codebase.** `pw_registry` is used nowhere today —
   everything in §1/§2/§3/§5 is new surface area, not a small extension of proven code,
   regardless of which approach (direct API vs shell-out) is chosen.
3. **Loop-affinity bugs** (calling PipeWire proxy methods off the thread iterating that
   `pw_core`'s loop) are a well-known class of PipeWire integration bug; the mailbox
   pattern in §5 needs to be enforced from the first commit, not retrofitted after a bug
   report.
4. **WirePlumber's restore-keying identity for a Proton stream is unverified.** The
   *existence* of `node.stream.restore-props` is documented; whether it reliably
   recognizes "the same game" across relaunches (given Proton/Wine's stream properties
   may not be stable across runs) was not confirmed against source or a live multi-launch
   test.
5. **Shell-out approach adds an unguarded runtime dependency** (`wpctl`) that has no
   equivalent today — the build's only PipeWire dependency check is build-time
   (`libpipewire-0.3`), not runtime-binary presence.
6. **Feature-flag conflation**: recommending reuse of the single `pipewire` Meson option
   for two conceptually different features (video capture, audio control) is efficient
   but removes the ability to build one without the other.

## Open questions for the user

1. **Direct PipeWire API vs shell out to `wpctl`?** Recommended: shell out for v1 (§6).
   This trades guaranteed-availability and live external-change push updates for much
   less new code — confirm that trade is acceptable, or that the direct API is worth
   building instead.
2. **When node matching finds zero or multiple candidate streams, what should the UI
   do** — a picker, "apply to all matched," or just report "not detected" and stop?
   (§2, §7) This is a product call, not a technical one.
3. **Should gamescope persist its own remembered per-game volume, or defer entirely to
   WirePlumber's `node.stream.restore-props`?** (§7) Recommended: defer, to avoid the
   two systems silently disagreeing — confirm that's acceptable even if WirePlumber's
   restore turns out to feel unreliable for Proton-launched games specifically.
4. **Is it acceptable to ship with an honest "not detected" fallback for
   `bwrap`-sandboxed Proton launches** (Risk 1) if that turns out to be the common case
   on the primary target platform, or is reliable detection there a hard requirement
   before this feature ships at all?
5. **Volume ceiling** — cap the slider at 100%, or allow boost past unity (125%/150%,
   matching `wpctl`/`pavucontrol` convention)?
6. **Does this block on the overlay's general settings-persistence design being decided
   first?** This document assumes "no gamescope-side persistence" (§7/Q3), which sidesteps
   needing a config schema slot for this feature specifically — confirm that's the
   intended relationship rather than this feature needing its own storage format decided
   in parallel.

---

*Sources consulted online (dates checked 2026-08-21), separate from what was verified
against this repo's source or this machine's live, read-only state: `wpctl(1)` man page
(https://man.archlinux.org/man/extra/wireplumber/wpctl.1.en); WirePlumber "Well-known
settings" (https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/settings.html);
PipeWire Registry docs (https://docs.pipewire.org/group__pw__registry.html); Steam
Runtime / pressure-vessel repo (https://github.com/ValveSoftware/steam-runtime).*
