# Settings-Overlay Feature Ideas — Practicality Research

Scope: three feature ideas for the settings overlay — Steam friends list with
join-game, Spotify/playerctl integration, system load display. **Research only, no
implementation, no code written outside this file.** Claims verified against this
checkout are marked *(verified)*; everything else is external research, marked with a
source URL and the date it was checked (2026-08-21).

## Verdict summary

| Idea | Effort | Recommendation |
|---|---|---|
| 1. Steam friends + join | Small (steam:// launcher) to unbuildable (real friends list) | **Skip** the friends list; the "join a specific lobby link" case (not a browsable list) is the only cheap, honest slice, and it is thin enough it may not be worth a UI |
| 2. Spotify/playerctl | Small–medium | **Build a reduced version**: shell out to `playerctl`, background-thread + snapshot, no direct D-Bus linking |
| 3. System load display | Small–medium | **Build a reduced version**: 3–5 sysfs numbers in the existing style, not a MangoHud clone |

All three are display widgets that want the same infrastructure the FPS HUD already
built — see §4.

---

## 1. Steam friends list with join-game

### What a non-Steam process can actually see

**Steamworks SDK (`ISteamFriends`)** is the interface with `GetFriendCount` /
`GetFriendByIndex` / rich presence and `ActivateGameOverlayToUser("jointrade"/"friends")`
etc. It requires `SteamAPI_Init()` to succeed, which requires Steam to hand the
calling process a validated app id — normally by launching the process itself, or via
a `steam_appid.txt` next to the binary during development. gamescope is a compositor
Steam does not launch as a game and has no app id of its own *(external, not
re-verifiable from this repo)*. Source: Steamworks docs, "Steamworks API Overview" —
https://partner.steamgames.com/doc/sdk/api (checked 2026-08-21). **This path is closed
to gamescope as an architectural fact, not a missing-feature gap** — there is no
"gamescope's own app id" to register that fixes it, because gamescope isn't a game.

**Steam Web API (`ISteamUser/GetFriendList`)** works over plain HTTPS with a
per-developer API key, no app id needed. But it only bypasses privacy settings for the
*key owner's own* Steam ID — for anyone else's friends list it returns HTTP 401 unless
that list is public, and gamescope would need to store a durable, revocable Web API key
tied to one specific Steam account. That is a secret with real blast radius sitting in
a compositor's config, which is qualitatively different from the wpctl/playerctl
subprocess calls elsewhere in this codebase (no credential involved). Source: Steamworks
Web API docs — https://partner.steamgames.com/doc/webapi/isteamuser — and community
confirmation that friends-only lists 401 for third-party keys, checked 2026-08-21.
Also: this API gives you *a* friends list and rich-presence strings, not a "join"
action — joining still routes through `steam://` (below).

**Local Steam client files**: `~/.local/share/Steam/userdata/<id>/config/localconfig.vdf`
on Linux is reported to cache friend nicknames in plain VDB/VDF text, readable without
any API *(external claim, unverified against a live Steam install in this sandbox —
Steam is not installed here)*. This is explicitly a **name cache**, not a live presence
feed: whether someone is online, in what game, and their current lobby id are
transient state Steam pushes over its own private protocol and there is no evidence
this file is kept current for that. Treating it as a friends-*list* source is
plausible; treating it as a source of "what are they playing right now, can I join" is
not supported by anything found. Source: forensic/community write-ups on
`localconfig.vdf`, checked 2026-08-21 — no Valve documentation exists for this file, it
is reverse-engineered community knowledge and could change or vanish across Steam
client updates without notice.

### "Join game" via `steam://`

`steam://joinlobby/<appid>/<lobbyid>/<inviter-steamid>` is a real, documented-by-community
protocol handler Steam's client registers with the desktop (xdg mime handler on Linux),
e.g. `steam://joinlobby/871200/109775241003709890/76561198017227219`. Firing it (e.g.
`xdg-open "steam://joinlobby/…"` or Linux's raw exec of `steam` with a `steam://` arg)
just hands off to the already-running Steam client, which does its own auth, ownership
check, and game launch. *(external, checked 2026-08-21 —*
https://partner.steamgames.com/doc/features/multiplayer/matchmaking,
https://steamcommunity.com/sharedfiles/filedetails/?id=3130451258*)*. **This part is
cheap** — a shell-out identical in spirit to how the audio feature shells out to `wpctl`
*(verified: `src/Audio/Volume.cpp` uses `popen`/subprocess against `wpctl`, no library
link)*. Grepping this repo for `steam://` or `xdg-open` turns up **no existing
precedent** *(verified: zero hits under `src/`)* — this would be new surface, though
architecturally trivial (spawn + argv, no parsing, no long-running process).

The catch: firing a `steam://joinlobby/...` URL requires *having a lobby id in hand
already*. Nothing above legitimately supplies one — the Web API's friend list gives
presence text (often a human-readable "In Lobby" string), not a structured, joinable
lobby id, and the SDK path that would (`ISteamFriends::GetFriendGamePlayed`,
`GetFriendRichPresence("connect")`) is exactly the interface closed off by the missing
app id. So the "right click a friend, Join Game" UX the user described is not
actually reachable this way: you'd have a name, maybe an app id they're playing, but
not a lobby id to hand to `steam://joinlobby`. What *is* reachable cheaply is "user
pastes/receives a `steam://` link from elsewhere and gamescope's overlay has a button
that fires it" — a link launcher, not a friends browser.

### Verdict

Skip the friends-list-with-context-menu feature as described — the piece that makes it
useful (seeing what a friend is playing and getting a joinable id for it) needs either
an app id gamescope cannot obtain, or a durable Steam Web API credential with real
privacy/secret-storage cost for a partial result (names and coarse presence text, not
reliably a lobby id). The one cheap, honest slice — firing an existing `steam://`
link — is real but is a link-launcher, not "show friends, right-click, join," and may
not be worth a dedicated UI on its own. **Biggest blocker: gamescope has no Steam app
id and cannot get one, which closes the one interface (`ISteamFriends`) that actually
returns a joinable lobby id.**

---

## 2. Spotify / playerctl (MPRIS) integration

### Mechanism

MPRIS (Media Player Remote Interfacing Specification) is the standard D-Bus interface
media players (Spotify, VLC, browsers, etc.) implement; `playerctl` is a thin CLI/lib
wrapper over it. *(external, well-established fact, not separately cited.)*

**On this machine**: `playerctl` v2.4.1 and a working D-Bus stack (`dbus-broker`,
`dbus-send`) are installed *(verified via `which playerctl` / `pacman -Q`)*. So a
shell-out story is viable here today without adding anything to the target system for
testing purposes — though it's still an *added runtime dependency* for anyone who
doesn't have it, same as `wpctl`/WirePlumber already is for audio.

**In this repo**: `meson.build`'s `dependency()` calls are `libpipewire-0.3`, `hwdata`,
`x11`, `wayland-client`, `vulkan`, `openvr` *(verified, grepped the full file)* — **no
`dbus-1`, `glib-2.0`, `gio-2.0`, or `sdbus-cpp`/`sdbus-c++` dependency anywhere**, and
no such subproject under `subprojects/` *(verified: `subprojects/` holds Catch2, glm,
imgui, libdisplay-info, libliftoff, nlohmann_json, openvr, stb, wlroots — no D-Bus
library)*. Linking a D-Bus client directly is therefore a new build dependency, not
something already half-present transitively.

### What's genuinely available vs. not

Via MPRIS/playerctl: track title/artist/album, play/pause/next/previous/stop,
position/seek, volume (player-local, separate from the PipeWire stream volume this repo
already controls), playback status, and album art — but only as a **URL**
(`mpris:artUrl`, often a local file:// path Spotify caches, or a Spotify CDN https URL
for the desktop client depending on version) — fetching/decoding/rendering that image
inside the overlay is real extra work (a texture upload path), not "read a field."
Not available at all: anything Spotify-account-specific that isn't playback state —
playlists, search, recommendations — none of that is MPRIS's job; it is strictly "what
is the local media player doing right now."

### Threading

*(verified)* `src/Audio/Volume.h`'s header comment states the exact constraint this
idea shares: "the /proc walk ... happen[s] on a dedicated background thread; nothing
here ever blocks the caller on a subprocess spawn," backed by a `PollThreadMain` on
`std::thread`, `std::mutex`-guarded state (`g_StateMutex`, `g_PendingMutex`), and a
`VolumeState` struct explicitly documented as "cheap to copy, safe to read every frame."
This is the identical shape a playerctl integration needs: D-Bus (or `playerctl`
subprocess calls) on a background thread, a small POD snapshot struct
(`NowPlayingState { title, artist, playing, positionMs, artUrl, playerAvailable }`)
protected by a mutex or atomics, read once per overlay frame from steamcompmgr's
vblank-paced thread. This pattern transfers directly; no new synchronization design is
needed.

### Shell out vs. link D-Bus directly

Following the `wpctl` precedent *(verified in `src/Audio/Volume.h`'s own header
comment and DECISIONS.md references to #22/#23)*: shelling to `playerctl metadata
--format ...` and `playerctl play-pause` etc. avoids the new `libdbus`/`sdbus-cpp`
build dependency entirely, costs one process spawn per poll (same cost class as the
existing `wpctl get-volume` calls), and gets play/pause/next/prev/track-metadata for
free. The cost: polling cadence is coarser than a D-Bus signal subscription (must poll
rather than being pushed a "track changed" event), and album art needs a second step
(playerctl returns the URL, something still has to fetch/decode it). A direct D-Bus
signal subscription would be more "real" (push-based, sub-second update) but is the
kind of investment this project has repeatedly decided isn't worth it for a v1 (same
tradeoff already made for `wpctl` over raw PipeWire, and for hand-drawn HUD glyphs over
an SVG library).

### Verdict

Build a reduced version: `playerctl` subprocess polling on a background thread
(mirroring `Volume.cpp`'s pattern almost exactly), title/artist/play-pause/skip in the
overlay, album art deferred to a later pass (it's the one piece needing new texture
plumbing). **Main risk: none of this exists if the user's session has no MPRIS player
running** — that's a graceful "nothing playing" state, not a hard failure, same shape
as `bWpctlAvailable` in `VolumeState`. **Biggest blocker: none structural** — this is
the most tractable of the three ideas and the closest match to an already-proven
in-repo pattern.

---

## 3. System load display

### What's cheap vs. what needs a library

All of the following are plain-text reads under `/proc` or `/sys`, no library needed,
same cost class as the sysfs/proc reads gamescope's own `get_appid_from_pid()`
already does against `/proc/<pid>/stat` *(verified, `src/steamcompmgr.cpp:5329`)*:

- **CPU load**: `/proc/stat` (per-core jiffies, need two samples + a delta) or
  `/proc/loadavg` (1/5/15 min load average, one read, no delta math).
- **RAM**: `/proc/meminfo` (`MemTotal`, `MemAvailable`).
- **GPU utilisation, VRAM, clocks, temps, power**: AMD's `amdgpu` sysfs nodes.
  *(verified on this machine)*, under `/sys/class/drm/card1/device/`:
  `gpu_busy_percent` (0–100 int), `mem_info_vram_used` / `mem_info_vram_total` (bytes),
  `mem_info_gtt_used/total`, `pp_dpm_sclk`/`pp_dpm_mclk` (clock states), plus a
  `hwmon` subdirectory. The system `hwmon2` is named `amdgpu` *(verified via
  `/sys/class/hwmon/hwmon2/name`)* and `sensors` on this box reports `edge`/`junction`/
  `mem` temperatures, `PPT` power draw (67.00 W of a 402 W cap at idle), fan RPM, and
  `vddgfx` — all backed by plain numeric files under that hwmon node (`tempN_input`,
  `power1_average`, `fanN_input`), no parsing beyond `atoi`/dividing by 1000.
- **Per-process/game GPU usage** specifically (as opposed to whole-GPU) is **not**
  cheaply available — `amdgpu`'s sysfs is whole-device, not per-PID; MangoHud itself
  gets close only via `fdinfo` (`/proc/<pid>/fdinfo/<fd>` DRM client stats), which is a
  materially bigger and fragile read.

This machine is confirmed AMD RX 7900 XTX-class *(verified: `lspci -nn` reports
`Navi 31 [Radeon RX 7900 XT/7900 XTX/7900 GRE/7900M]`, PCI id `1002:744c`)*, so the
`amdgpu` sysfs surface above is directly applicable, not a generic "should work on
AMD" guess — it was read live off this box.

### Polling cost

None of these reads are expensive per-call (small sysfs files, no locking contention
expected), but "continuously while the HUD is visible" still means picking a cadence —
once or twice a second is standard for this kind of display (matches typical MangoHud
defaults) and keeps the read cost negligible against a vblank-paced render loop;
polling every frame would be wasteful and unnecessary since none of these numbers are
meaningful at sub-100ms resolution anyway. Same threading shape as §2 applies: a
background thread (or even just a "poll once per N overlay frames" on the UI thread
itself, since these are non-blocking local file reads, not subprocess spawns or network
calls) feeding a small cheap-to-copy snapshot struct.

### Does this just duplicate MangoHud?

Yes, functionally — **MangoHud already reads every one of the sources above** and
gamescope already has a live integration point for exactly this class of data via
`init_mangoapp()`/`mangoapp_update()` (`src/mangoapp.cpp`, a SysV message queue pushing
frametime/latency/appid/etc. to a listening mangoapp process) *(verified, read the
file)*. That is precisely the same tension the project already resolved once: the FPS
HUD itself was rebuilt in-overlay (`src/Overlay/FpsDisplay.cpp`,
`gamescope::FpsDisplay_DrawSettingsPanel()` wired into `SettingsOverlay.cpp`) *(verified)*
specifically **because mangoapp/MangoHud cannot be styled to match this overlay** — it's
a separate always-on-top overlay window with its own look, not embeddable inside
`gamescope::chrome`'s panel system. The same argument transfers directly: a built-in
system-load panel isn't adding data the user couldn't already get from MangoHud, it's
adding **consistent styling and a single settings surface** instead of a second,
differently-skinned overlay running alongside gamescope's own. That's a real but modest
value-add — worth noting plainly rather than oversold.

### Verdict

Build a reduced version: CPU load average, RAM used/total, GPU busy %, VRAM used/total,
one or two temperatures, all from the sysfs/proc sources above, polled once or twice a
second, styled to match the existing panel system (`Overlay/Widgets.h`,
`Overlay/Palette.h`). Skip trying to match MangoHud's full breadth (per-process GPU
stats, historical graphs, the fdinfo path) — that's real added complexity for data most
users already have another way to see. **Main risk: none technical** — all the data
sources are confirmed present and cheap on this exact machine; the only judgment call
is how much of MangoHud's surface to replicate before it stops being "a light system
panel" and starts being "a second MangoHud living inside gamescope." **Biggest
blocker: none** — this is a scope decision, not a feasibility question.

---

## 4. Shared infrastructure across ideas

Ideas 2 and 3 both want the same thing: a small, always-visible (or togglable) panel
showing a handful of live numbers/text, fed by a background-thread-plus-snapshot
pattern that already has one working instance in this codebase
(`src/Audio/Volume.cpp`/`.h`) and a panel-hosting mechanism already built for the FPS
HUD (`gamescope::chrome::BeginPanelWindow`, `PanelId::Fps`, wired in
`SettingsOverlay.cpp` — see `DrawFpsHudPanel()`). A "now playing" widget and a
"system load" widget are naturally two more panels of the same shape sitting next to
`PanelAudio`, `PanelDisplay`, `PanelShaders`, `PanelConfig` *(verified, all present
under `src/Overlay/`)*, not two separate pieces of new plumbing. Idea 1's one cheap
slice (a `steam://` link launcher) doesn't share this shape — it's a fire-and-forget
action, not a live-updating display — and would be a much smaller, different kind of
addition if pursued at all.

## Build-first recommendation

Build **playerctl/MPRIS (§2)** first: it is the most tractable, has a proven in-repo
threading pattern to copy nearly verbatim, needs no new build dependency if shelling
out, and `playerctl` is already present on this development machine to test against.
System load (§3) is a close second and shares the same panel infrastructure once
built. Steam friends/join (§1) is the one to be honest about: the interesting,
Steamworks-quality version is blocked by gamescope structurally lacking an app id, and
the cheap fallback (fire a `steam://` link) is real but thin enough that it may not
warrant a dedicated feature versus, at most, a single "open Steam link" utility.
