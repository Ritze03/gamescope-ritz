# Joining friends from inside the game — feasibility

**2026-09-08, research only. Nothing under `src/` was changed.**
Sibling of [`steam-friends-window.md`](steam-friends-window.md), which settled *"can we
show Steam's Friends window"* (no) and shipped a Steam chat companion instead — later
removed, 2026-09-09, once this page's friends list proved it could do the one thing the
companion never could: actually join somebody (see
[`../features/steam-friends.md`](../features/steam-friends.md)'s *"History: the browser
companion it replaced"*). This page is what happened when the question was re-asked four
times in one session and the target moved each time, ending somewhere much better than
where it started.

Everything here was measured on the user's own machine **while they were in a CS2 match**,
so every test is headless (private `sway` on the wlroots headless backend, isolated
`XDG_RUNTIME_DIR`, per `scripts/pixel-regression.sh`'s recipe) and nothing was ever mapped
on their desktop, no input was injected into their seat, and their Steam and their game
were never touched. Scripts, screenshots and raw output:
`build-release/verify-shots/steam-child-session-2026-09-08/`.

---

## The one-line answer

**Build a native "friends you can join" list in the overlay.** Read the friends list and
each friend's *joinable lobby id* out of the already-running Steam client by `dlopen`ing
the client's own `steamclient.so`; act on a selection by handing the running client a
`steam://joinlobby/...` URL. **No API key, no browser, no second Steam client, no child
session, no capture protocol, no second login, and — on the route recommended below — no
app id and no `SteamAPI_Init` anywhere.**

Every one of the four approaches this session started with is worse than that, including
the one that already ships.

---

## How the question moved

| # | The user asked | The honest answer |
|---|---|---|
| 1 | *"Can the Friends window run in a child session we stream and remote-control?"* | The remoting stack costs two packages this machine does not have, and the idea is circular: see [§2](#2-the-child-session-proposal-and-why-it-is-circular). |
| 2 | *"Can it just take a single window? Spawn it from the tray headlessly?"* | No. An X11 window belongs to the server its client connected to; the tray only messages the running client, which opens the window on **its** display. [§1](#1-moving-an-existing-window-closed-for-good) closes this for good. |
| 3 | *"The important thing is joining friends — any way is fine."* | This is the question with a good answer. [§5](#5-the-approach-that-actually-wins-a-native-join-list). |
| 4 | *"No API keys. Explore what the default Steam `.so` offers."* | Correct instinct, and it makes the answer *better*, not worse — the local library gives the one field the web API was wanted for. [§6](#6-the-local-library-what-it-offers-and-what-it-costs). |

---

## What is installed on this machine

Checked by absolute path across `/usr/bin`, `/usr/local/bin`, `~/.local/bin` and both
flatpak export dirs.

| present | absent |
|---|---|
| `sway`, `Xwayland`, `chromium`, `firefox`, `xdotool`, `ydotool`, `grim`, `slurp`, `wayshot`, `wf-recorder`, `kitty`, `steam`, `gamescope` (stock 3.16.25), `swaymsg`, `python3` + `websockets`, `node`, FreeRDP 3 (`xfreerdp3`, `wlfreerdp3`, `freerdp-shadow-cli3`) | **`xpra`**, **`cage`**, **`weston`**, **`wayvnc`**, **`waypipe`**, **`Xephyr`**, **`Xvfb`**, any VNC server or viewer (`x11vnc`, `Xvnc`, `vncviewer`, `wlvncc`, `neatvnc`), `wtype`, `labwc` |

So of the "software for this" the user had in mind, **none of the obvious candidates is
installed**. `xpra` (the closest fit — you start apps inside its X11 session and clients
attach with full input) and a `wayvnc` + VNC-viewer pair are each a **new package this
report does not install**; they are listed as prerequisites, not installed, per the task's
rules.

---

## 1. Moving an existing window — closed for good

The user's *"can it just take a single window?"* deserves a flat answer rather than an
inherited one.

**It cannot, and the reason is structural, not a missing feature.**

- An X11 window is an object **inside one X server**. Its window id is only meaningful on
  the connection that created it. `XReparentWindow` takes two window ids **from the same
  `Display*`**; there is no call in the protocol that takes a window from server A and a
  parent from server B, because there is no way to name both at once.
- The two servers here are real and separate, and that is measurable on the user's live
  session right now: their Steam runs with `DISPLAY=:1` (Hyprland's Xwayland) and their
  CS2, inside gamescope, runs with `DISPLAY=:2` (gamescope's own Xwayland). Read straight
  out of `/proc/<pid>/environ` for pids `2017811` and `1798270`.
- **The tray item does not open a window; it sends a message.** "Friends" in the tray, and
  `steam://open/friends` from a shell, both reach the running client through
  `~/.steam/steam.pipe`, and *that client* then opens the window on the display **it**
  connected to at startup. `steamwebhelper` is one long-lived CEF process with one X
  connection; it does not open a second one per window.
- That was already tested, in the earlier study's §3: `DISPLAY=:2
  WAYLAND_DISPLAY=gamescope-0 steam steam://open/friends` produced **nothing** on `:2`;
  the URL was swallowed by the host client and handled on `:1`.
- Steam's own binary states the forwarding in as many words. From
  `~/.steam/steam/ubuntu12_32/steam`:
  `"Steam is already running, exiting (command line was forwarded)."`

`Why this is worth writing down twice:` "just move the window" is the first idea everyone
has, it is cheap to *sound* possible, and the cost of re-testing it is an hour. It is
closed.

---

## 2. The child-session proposal, and why it is circular

The proposal was: run the Friends window in a child Wayland/X11 session we own, stream it
into gamescope, and drive it with input we synthesise into *that* session's seat. The
input objection that killed window capture genuinely does disappear — you own that seat.

**But the idea eats itself.** A child session can only show you an application it can
*start*. Steam's Friends window cannot be started into any session other than the one the
running client already owns (§1). So the child session would have to run **its own Steam
client** — and if a second Steam client is acceptable at all, you do not need a child
session, because **gamescope's own Xwayland is already a child session we own**, and the
shipped companion already knows how to promote a window on it to a fullscreen interactive
overlay. The child session plus a streaming stack is strictly *more* machinery for
strictly *less* than running the thing directly on `:2`.

Two things were still worth measuring, because they matter for anything nested.

### 2a. What a game launched inside a child session actually renders into

`probe-nesting.sh`, two private headless sways: **S1** stands in for the child session,
**S2** for the host.

| shot | what it shows |
|---|---|
| `01-child-session-has-the-game.png` | a gamescope started **inside S1** rendering its client — centre pixel `rgb(16,80,192)`, the client's own colour |
| `02-host-session-is-empty.png` | S2 at the same moment — `rgb(0,0,0)`, nothing |
| `03-host-session-after-explicit-env.png` | the *same client command*, with `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY` pointed at S2 — `rgb(192,80,16)`, it lands in S2 |

So: **a game launched from a Steam living in a child session renders into that child
session by default**, purely by environment inheritance, and it is escapable only by
explicitly overriding the environment per launched process. One nuance worth recording:
both sways named their socket `wayland-1`; only `XDG_RUNTIME_DIR` separated them. A
"clear `WAYLAND_DISPLAY`" style fix would therefore silently reconnect to the *wrong*
compositor.

**But — and this is the good news — none of that applies to the shape the user actually
has.** Their launch path is Steam → Ritz → `gamescope-ritz {OPTIONS} --` → game
(`extensions/gamescope-ritz.json:830`), one wrapper *per game*, and the host Steam is what
launches it. A second Steam used only for chat never launches anything, so their games
keep launching from the host client and never touch any child session. The
virtual-display worry is real in general and **absent in this specific design**.

### 2b. Would a second Steam client work, and what would it cost?

Not tested live, deliberately — testing it means logging an account in, and they were in a
match. What is established:

- **Same `HOME` is impossible.** The pipe is `$HOME/.steam/steam.pipe` (present, a real
  FIFO). A second `steam` finds it and forwards its command line, as its own error string
  above says.
- **A different `HOME` is a different Steam *installation*.** It bootstraps and downloads
  the client from scratch (hundreds of MB — while they are in an online match), has none of
  their games, and needs a **fresh login with Steam Guard**, because the machine-identifying
  files (`registry.vdf`, the `ssfn*` tokens) live in that home and would be absent. It is a
  new device from Steam's point of view.
- **The flags exist.** `-silent` (start minimised to tray) is in the client binary's flag
  table, alongside `-nofriendsui`, `-noverifyfiles`, `-login`, `-dev`, `-forcesteamupdate`,
  `-steamos`. **`-no-browser` is *not* in that table, and would be wrong anyway** — the
  Friends UI *is* the CEF browser, so disabling it disables the thing we want.
- **Concurrent clients on one account: unverified, and the risk is asymmetric.** Steam does
  permit an account to be signed in on several devices, with game-*play* exclusive to one.
  A chat-only second client should therefore coexist. But "should" is doing real work in
  that sentence, and the failure mode is being dropped out of a live match — which is worse
  than never having the feature. **This must not be tested for them.** If they want to know,
  the exact steps are in [§8](#8-steps-only-the-user-can-run).
- **The costs are not small.** A second Steam client is another ~700 MB–1.5 GB of RAM and
  six-plus `steamwebhelper` processes running *while a game runs*, versus the single
  chromium the shipped companion costs. And accepting a game invite in that client would
  try to launch the game from an installation that has no games in it.

**Verdict: rejected.** More machinery than the shipped companion, for the same separate
login, plus a risk to their match.

---

## 3. Steam's CEF remote debugging — mechanically proven, then killed by one measurement

The earlier study never considered this. It deserved to be, and it very nearly worked.

### What is true

- **Steam really does have the switch.** `.cef-enable-remote-debugging` is a literal string
  in `~/.steam/steam/ubuntu12_32/steamui.so`, sitting in a run of client setting names. The
  marker file does **not** exist on this machine, and nothing is listening on 8080, so the
  endpoint is **off** right now.
- **`steamwebhelper` takes the flag.** `remote-debugging-port` is a string in
  `~/.steam/steam/ubuntu12_64/steamwebhelper`.
- **Steam's CEF is new enough to have everything needed.** `libcef.so` is
  **CEF / Chrome 126.0.6478.183** and exports the whole surface:
  `Page.startScreencast`, `Page.stopScreencast`, `Page.screencastFrame`,
  `Page.screencastFrameAck`, `Page.screencastVisibilityChanged`,
  `Input.dispatchMouseEvent`, `Input.dispatchKeyEvent`, `Input.insertText`,
  `Runtime.evaluate`, `Target.attachToTarget`, `Target.getTargets`.

### What was proven headlessly

`probe-cdp.sh` + `cdp_client.py`, with a plain chromium standing in for `steamwebhelper`
(Steam could not be restarted with the flag while they were gaming). A page with state
"only this browser has" runs in a private headless sway; a **separate process that owns
nothing** connects to `127.0.0.1:<port>` and:

- **renders it** — `Page.startScreencast` delivers PNG frames (`10-cdp-screencast-before.png`);
- **drives it** — a synthesised `Input.dispatchMouseEvent` press/release at the input box's
  measured centre, then `Input.insertText` and a real `Input.dispatchKeyEvent`, put
  `"typed over CDP!"` into the field, read back through the page itself
  (`11-cdp-screencast-after.png`). `RESULT control: PASS`.

No Wayland protocol, no seat, no capture extension, no window ownership. **The rendering
and input problem that killed §4 of the earlier study is genuinely solved by CDP.**

### The measurement that kills it

The Friends window would be sitting on the host desktop *underneath a fullscreen
gamescope*. So: does the stream survive that?

| state of the source window | frames received |
|---|---|
| visible | **yes** (`RESULT render: PASS`; every frame of the run arrived here) |
| moved to another workspace (surface **unmapped**) — `13-source-window-is-off-screen.png` is solid black | **0** |
| **mapped, but fully covered by a fullscreen window** — `15-source-window-is-covered.png` is solid black, and `14a-restored-before-covering.png` proves it was visible immediately before | **0** |

The last row is exactly the shape of "a fullscreen gamescope over the host's Friends
window", and it produced **no frames at all** — a page-background change made during that
window never arrived. The compositor stops sending frame callbacks to a fully-occluded
surface, the renderer stops producing, and `Page.startScreencast` has nothing to forward.

`Why this matters more than it looks:` it is the *same* staleness hazard the earlier study
flagged for the capture protocols (§4a's "nothing obliges the source to keep painting"),
and it turns out to bite CDP identically. The mechanism works; the scenario does not.

**Getting round it** would mean keeping the Friends window visible somewhere — a second
monitor, or a child session whose compositor always composites it (which is a real,
non-silly argument *for* §2's child session, and the only one found). Both re-import all
of §2's costs. And it still needs Steam restarted with a debugging endpoint open on
localhost for the life of the session, which is a standing "anything on this machine can
drive your Steam UI" hole.

**Verdict: rejected**, with the mechanism recorded because it is genuinely useful and was
genuinely proven.

---

## 4. What the shipped companion is actually worth

At `3be018b` the Steam chat companion works: `Ctrl+Shift+Tab`, a browser on gamescope's
own Xwayland promoted to a fullscreen clickable overlay, verified end to end. Its flaws,
from its own docs: a **separate sign-in**, **no voice**, **no game invites**, **no
notifications**, and a resident browser costing a few hundred MB while the game runs.

Note which of those the user's real goal collides with: **"join a friend" is precisely the
thing the web chat cannot do.** The web client has no join button. So the shipped feature
does not solve the problem the user actually has, and never could.

---

## 5. The approach that actually wins — a native join list

Once the goal is *"see which friends are in a joinable game, and join one, without leaving
the game"*, the window was never the point. Split it into three parts and each has a cheap,
local answer.

### Data — where "joinable" lives

**`ISteamFriends::GetFriendGamePlayed`.** It fills a `FriendGameInfo_t` carrying the
friend's `m_gameID` **and `m_steamIDLobby`, which is non-zero exactly when they are in a
lobby you can join.** That is the entire feature in one field, obtained locally from the
already-logged-in client. Details and cost in [§6](#6-the-local-library-what-it-offers-and-what-it-costs).

Sources that were considered and rejected:

| source | verdict |
|---|---|
| **Steam Web API** (`GetFriendList` + `GetPlayerSummaries`, which does return `lobbysteamid`) | **Rejected on the user's instruction, and they are right.** A Web API key is not read-only: it can act on trades and inventory. Measured here: both endpoints return **HTTP 400** without a key, so there is no key-less variant. |
| **Steam Community XML** (`/profiles/<id>/?xml=1`) | Key-less, and useless. Measured: it returns `onlineState`, `stateMessage`, `privacyState`, avatars — **and no lobby id, ever**. `/friends/?xml=1` now answers **HTTP 302**. It cannot tell you what is joinable. |
| **Local Steam files** | `~/.steam/steam/config/loginusers.vdf` gives the logged-in **SteamID64** and account name — genuinely useful, and it means the user never has to type their own id anywhere. Presence and lobbies are **not** on disk; `~/.steam/steam/friends/` is only `.res` layout files for the old friends UI. |

### UI — a list in our own overlay

The Shell already has a list control, a rail, settings areas and a keybind system
(`src/Overlay/UI/`, `PanelConfig.cpp`'s Profiles list is the closest existing shape). A
native list of *friends currently in a joinable lobby* removes the browser, the separate
login, the memory cost and the "no notifications" complaint in one stroke, and matches the
user's recorded preference for **list + verbs** over stacked rows.

### Action — hand the running client a URL

`steam://joinlobby/<appid>/<lobbyid>/<steamid>`, delivered by `exec`ing
`steam <url>`, which forwards over `steam.pipe` to the running client — the very
forwarding that was a *limitation* in §1 is the *mechanism* here.

Evidence the form is real, from `~/.steam/steam/ubuntu12_32/steamui.so`'s URL-command
table: `joinlobby` is a registered command, sitting in the same table as `rungame`,
`rungameid`, `openurl`, `connect`, dispatched by `CSteamURLController::ExecuteSteamURL`.
**The string immediately after `joinlobby` in the binary is `+connect_lobby %llu`** — the
launch argument Steam appends when it acts on a join. There is also a `CJoinGameController`
with a `LaunchGame()` path. (`steam://friends/joingame/...` was looked for and is **not**
in that table; `steam://friends/...` only carries the four `status/*` commands.)

**What happens when it fires while a game is running** — the consequence to think through:

- **Same game (the common case: they are in CS2, the friend is in CS2).** Steam does not
  relaunch anything. The already-running game gets the join delivered through its own
  Steamworks callback (`GameLobbyJoinRequested`) and moves the player. **Nothing about
  gamescope, Ritz or the wrapper is involved, and the overlay simply closes.** This is the
  case worth optimising for.
- **Different game.** Steam launches it: Ritz applies that game's extension config and
  spawns a *second* `gamescope-ritz` over the first. That is messy — but it is exactly what
  happens today if they accept an invite in Steam's own overlay, so the feature is not
  making anything worse. Worth a confirm step in the UI ("Join *Rust*? This will start
  another game") rather than special-casing.
- **Not a Steam app at all** (no app id): the feature is unavailable and says so.

---

## 6. The local library — what it offers, and what it costs

### 6a. Two routes, and the safer one needs no app id at all

| | route A — `libsteam_api.so`, flat C API | route B — the client's own `steamclient.so` |
|---|---|---|
| what you bind to | `SteamAPI_ISteamFriends_GetFriendCount`, `..._GetFriendByIndex`, `..._GetFriendPersonaName`, `..._GetFriendPersonaState`, `..._GetFriendGamePlayed`, `..._GetFriendRichPresence`, `..._ActivateGameOverlayToUser` — **all verified exported** (`nm -D`) on all three copies found on this machine | `CreateInterface`, `Steam_CreateSteamPipe`, `Steam_ConnectToGlobalUser`, `Steam_BConnected`, `Steam_BLoggedOn`, `Steam_BReleaseSteamPipe`, `Steam_ReleaseUser` — **all verified exported**, and all **resolved in-process** by the probe below |
| where the file is | **nowhere reliable.** It is a per-*game* redistributable: found only inside individual games' folders (`.../Soulstone Survivors/.../libsteam_api.so`, `.../Pic-Me!/`, `.../Proton 7.0/dist/lib64/`). **The Steam client ships no copy** — `~/.steam/steam/{linux64,ubuntu12_64}` has none | **part of the client**: `~/.steam/steam/linux64/steamclient.so`, 46 MB, dated with the client, plus a `linux32` sibling |
| needs an app id? | **yes** — `SteamAPI_Init` refuses without one | **no. None, anywhere.** `Steam_ConnectToGlobalUser` attaches to the client's already-logged-in user |
| ABI you declare | pleasant flat C functions | C++ vtables, declared by hand against a **versioned** interface string |

**Route B is the recommendation**, for the reason the user cares about most: with no app
id supplied anywhere, **nothing registers a game**, so the "your friends see you playing
something you are not" problem cannot occur, and the "second `SteamAPI_Init` for an app id
already in use" question never has to be answered. It is also the only route whose library
is guaranteed to be present.

Its cost is honest and should be stated: `CreateInterface` + `ConnectToGlobalUser` +
`GetISteamFriends` is **the undocumented low-level entry** (it is what `SteamAPI_Init`
does internally). We declare the leading vtable entries ourselves. What makes that
tolerable rather than reckless is that the interfaces are **explicitly versioned** — this
client exports `SteamClient018`…`SteamClient023` and `SteamFriends016/017/018` side by
side, which is precisely so that a caller pinning `"SteamFriends017"` keeps the layout it
compiled against. Ask for a list of versions newest-first and use the first that answers.

**No Valve SDK headers are vendored either way** — the flat symbols and the handful of
vtable slots are declared locally, so no licensing question enters the repo.

### 6b. The app-id route, if route B is ever rejected

The user's proposal — *"use the current game's AppID; if nothing is provided, it isn't a
Steam game"* — is correct and needs no new plumbing:

- **The compositor already resolves it, once, in one place.** `config::SessionAppId()`
  (`src/Config/ConfigManager.cpp:2083`) memoises `config::ResolveAppId()`
  (`src/Config/AppId.cpp:51`), which reads `GS_RITZ_APPID`, then `STEAM_COMPAT_APP_ID`,
  then `SteamAppId` (rejecting a literal `0`), then `STEAM_COMPAT_DATA_PATH`'s basename,
  and returns `std::nullopt` otherwise. **Reuse that; do not add a second way to learn it.**
- **It really is populated.** Read live from their running session, `/proc/1797955/environ`
  (the `gamescope-ritz` running their CS2): `SteamAppId=730`, `SteamGameId=730`,
  `STEAM_COMPAT_APP_ID=730`, `SteamOverlayGameId=730`, `SteamEnv=1`, `SteamClientLaunch=1`.
- **`steam_appid.txt` is not needed.** `libsteam_api.so` looks at the environment first
  (`SteamAppId`/`SteamGameId` are both strings in it; the file is only the fallback its own
  error message names: *"Either launch the game from Steam, or put the file
  steam_appid.txt…"*). So a compositor that does not own its working directory is fine.
- **`nullopt` → the feature is simply unavailable.** No fallback, no Spacewar, no 480.

**The second-`SteamAPI_Init` question, answered as honestly as it can be without touching
their session.** Their game has already initialised for app 730; ours would be a second
process doing so concurrently.

- *Permitted?* In practice yes — launcher-plus-game pairs do it routinely, and
  `SteamAPI_Init` is a connection to the client, not a lock.
- *Could our exit end their play session?* **Almost certainly not, and their own command
  line is the evidence.** Steam tracks an app session through the process tree it launched:
  `/proc/1797955/cmdline` reads `gamescope-ritz … steam-launch-wrapper -- … reaper
  SteamLaunch AppId=730 -- …`. The **reaper** is what tells Steam the app started and
  stopped, and it is a *child* of `gamescope-ritz`. Our SteamAPI usage would add no new
  coupling that gamescope's own lifetime does not already have — if gamescope exits, the
  session ends regardless, which is true today.
- *Achievements, playtime, cloud saves, the overlay?* Nothing in this design writes: no
  stats calls, no `StoreStats`, no cloud, and the in-game overlay is already off for these
  titles (`LD_PRELOAD=` empty, confirmed again in their live environ).
- **But this is reasoning, not a measurement**, and the failure it is ruling out is
  expensive. Which is the real argument for route B: **route B does not need this
  paragraph to be true.**

### 6c. Proof of concept — what was actually run

`steamclient_probe.c` (+ the built `steamclient_probe`), deliberately split in two:

- **`--symbols` — run, on the user's machine, while they were in a match.** It `dlopen`s
  `~/.steam/steam/linux64/steamclient.so` and `dlsym`s every entry point the friends path
  needs. It creates no pipe, opens no socket and sends the running client nothing.
  Result, in `results-steamclient.txt`:

  ```
  RESULT dlopen: PASS (/home/mo/.steam/steam/linux64/steamclient.so)
    CreateInterface              resolved
    Steam_CreateSteamPipe        resolved
    Steam_ConnectToGlobalUser    resolved
    Steam_BConnected             resolved
    Steam_BLoggedOn              resolved
    Steam_BReleaseSteamPipe      resolved
    Steam_ReleaseUser            resolved
  RESULT symbols: PASS
  RESULT connect: SKIPPED (run with --connect; it talks to the live client)
  ```

- **`--connect` — written, compiled, deliberately not run.** It creates the pipe, connects
  to the global user, asks for `ISteamFriends` and enumerates friends with
  `GetFriendGamePlayed`, printing **counts and app ids only — never names or SteamIDs**,
  because the output is committed to this repo. It is one flag away and is
  [§8](#8-steps-only-the-user-can-run)'s first step.

### 6e. Correction — the published vtable order is wrong by one slot

**Added 2026-09-08, when phases 1 and 2 were built and run against the live
client with the user's permission. This supersedes the layout in §6c's probe.**

`steamclient_probe.c` declared `ISteamFriends` from the order every write-up
repeats — `GetPersonaName`, `SetPersonaName`, `GetPersonaState`,
`GetFriendCount`, `GetFriendByIndex`, … — and phase 1 was built on the same
declaration. **Against the real client it is off by one**, and the failure mode
is the dangerous kind: not a crash, but a friends list that is silently and
permanently **empty**, which reads exactly like *"nobody is in a joinable
game"*. The first live run reported `RESULT friend count: 0` and looked like a
correct answer.

Measured on `SteamFriends018` with `tests/steam_friends_live_probe --diagnose`,
by calling raw vtable slots in shapes that are read-only under *both* candidate
layouts:

| call | result |
|---|---|
| slot 2 as `GetFriendCount(flags)` | **varies with the flag** — 75 for Immediate, 140 for `0xFFFF`, 1 for FriendshipRequested |
| slot 3 as `GetFriendCount(flags)` | 0 for every flag |
| slot 3 as `GetFriendByIndex(0, Immediate)` | an id inside the individual-account SteamID64 band |
| slot 4 as `GetFriendByIndex(0, Immediate)` | not a SteamID at all |

So `GetFriendCount` is **slot 2**, `GetFriendByIndex` is **slot 3**, and
everything after them shifts down by one. With that corrected the read works:
75 friends, 5 in a game, app ids `730`, `252950`, `2483190`, `2357570` — all
real Steam apps with CGameID type 0 — and every persona name printable.

Three consequences worth carrying forward:

- **`src/SteamFriends.cpp` declares the measurement, not the documentation**, and
  says so at length. **Slots 0 and 1 are opaque `void*` and must never be
  called**: under the published order slot 1 is `SetPersonaName`, a *write* on
  the user's live account, and "call it and see" is not available for a setter.
- **The version string is no longer the only guard.** `Snapshot()` range-checks
  the first SteamID it gets back; a client whose layout moves again produces one
  named log line and an empty list rather than nonsense. `tests/test_steam_friends.cpp`
  has a case for it.
- **A stub cannot catch this class of bug.** The unit tests all passed against
  `tests/steamclient_stub.cpp` while the layout was wrong, because a fake built
  from the same declaration agrees with itself. Layout is the live probe's job;
  control flow is the stub's. Keep both.

`Still unproven, and phase 3 was designed around it:` **`m_steamIDLobby`'s
offset in `FriendGameInfo_t`.** Nobody in
the friends list was in a joinable lobby at any moment sampled, and a wrong
offset reads as zero just like an absent lobby does. `m_gameID`'s offset in the
same struct *is* confirmed. The code deliberately does **not** filter on a
lobby-id range — an unverified band check would silently hide exactly the
friends this feature exists to show — and counts out-of-band ids into its debug
line instead. To close it, run the probe while a friend is actually in a
joinable lobby; that is now the first item of [§8](#8-steps-only-the-user-can-run).

### 6d. Robustness — the four failure modes, and the rule

The compositor must never block or crash on any of these, and every one of them is a
`nullptr` or a zero on a path that already has to handle it:

| situation | behaviour |
|---|---|
| **Steam not running** | `dlopen` may still succeed; `Steam_CreateSteamPipe` returns 0. Feature shows "Steam isn't running". |
| **Library missing** (no Steam installed, or a flatpak Steam whose files are elsewhere) | `dlopen` fails, once, at first use. Feature shows "Steam not found". Never fatal. |
| **User offline / not logged in** | `Steam_ConnectToGlobalUser` returns 0, or `Steam_BLoggedOn` is false. Feature shows "signed out". |
| **Friend in a non-joinable game** | `GetFriendGamePlayed` fills the struct with `m_steamIDLobby == 0`. That friend is listed as in-game but **not** offered as joinable — the list's whole filter is this one field. |
| **A newer client changed the vtable** | The interface *version string* is the guard: try `SteamFriends018/017/016` in order and use the first that answers; if none does, the feature disables itself with a named log line. |

And the threading rule, which is not optional: **`dlopen`, the pipe, and every friends call
happen off the compositor's frame path.** Do this on the steamcompmgr thread at a low poll
rate (a few seconds; presence does not change faster than a human reads), or on a worker,
and never inside a paint. The shipped companion's own contract — *"nothing blocks the
compositor"* — is the precedent to copy.

---

## 7. The four options, ranked

| | separate login | notifications | **can join** | resident cost | new deps | risk to their session |
|---|---|---|---|---|---|---|
| **Native join list (§5, route B)** | **none** | possible later (poll the same data) | **yes** | a `dlopen`, a few `struct`s | **none** | none — no app id, no init, read-only calls |
| Native join list, app-id route (§6b) | none | same | yes | one `SteamAPI_Init` | none | small but *unmeasured* second-init question |
| Shipped browser companion (`3be018b`) | **yes** | no | **no** | a resident chromium | a browser | none |
| Second Steam client / child session (§2) | **yes** | yes | in theory | ~1 GB + 6 processes, plus a remoting stack | `xpra` **or** `wayvnc`+viewer | **could drop them out of a match** |
| CEF remote debugging (§3) | no | yes | yes | a decoder + CDP client | none | needs Steam restarted with a debug port open |

**Recommendation: build §5 on route B.**

**And say plainly what that means for what already ships.** The shipped companion solves a
different problem (chat) and cannot solve this one (joining). If the join list lands and
the user finds they only ever wanted to join, the companion is a resident browser earning
nothing and **should be reduced or removed rather than maintained** — that call is the
user's, and it should be put to them once the list works, not before.

---

## 7b. Implementation sketch — one commit per phase

Nothing here touches the compositing pipeline, and every phase is reviewable on its own.
Where the shipped companion already solved a problem, **reuse it rather than re-solve it**:
`SteamCompanionCmd.h`'s split-a-command-line-and-substitute is exactly what firing
`steam <url>` needs, and `SteamCompanion.cpp`'s fork-into-its-own-process-group +
`PR_SET_PDEATHSIG` + resolve-`argv[0]`-through-`PATH`-**before**-forking is exactly the
spawn contract a URL fire wants.

**Phase 1 — `src/SteamFriends.{h,cpp}`, headless, no UI.**
`dlopen` the client library (route B), resolve the seven symbols, pin the interface
version newest-first, and expose one call: `std::vector<JoinableFriend> Snapshot()`
returning `{ persona, appid, lobbyid, steamid }` for friends whose `m_steamIDLobby` is
non-zero. Everything fails soft to an empty vector plus one named log line. Split the pure
half (version-string order, the "joinable" predicate, the URL builder) into
`src/SteamFriendsCmd.h` so `tests/test_steam_friends.cpp` runs it with no Steam — the same
shape as `SteamCompanionCmd.h`, for the same reason.
Reviewable through a `friends_dump` ConCommand; no hotkey, no panel.

**Phase 2 — the join action.** One function that builds
`steam://joinlobby/<appid>/<lobbyid>/<steamid>` and `execvp`s `steam` with it, using
`SteamCompanion`'s existing spawn discipline. Guard: refuse a lobby id of 0, and never
build the URL by string-concatenating anything a friend controls (a persona name never
enters it).

**Phases 1 and 2 landed 2026-09-08** — `src/SteamFriends.{h,cpp}`,
`src/SteamFriendsCmd.h`, `tests/test_steam_friends.cpp`,
`tests/steamclient_stub.cpp`, `tests/steam_friends_live_probe.cpp`, and the
`friends_dump` / `friends_join` ConCommands. Evidence:
`build-release/verify-shots/steam-friends-phase12-2026-09-08/results.txt`.
No `CHANGELOG.md` entry: nothing is user-visible until the UI exists, so the
repo's rule ("purely internal work gets no entry") applies. **Read
[§6e](#6e-correction-the-published-vtable-order-is-wrong-by-one-slot) before
touching any of it.**

**Phase 3 — the list in the Shell.** A `system.friends` area with the existing list
control: one line per joinable friend (`persona` — `game`), Enter/click joins, with a
confirm step **only** when the target app id differs from `config::SessionAppId()`.
Poll `Snapshot()` a few seconds apart, off the frame path. Empty states are text, not
blank space: "Steam isn't running", "signed out", "nobody's in a joinable game".

**Phase 4 — the keybind.** Add `Friends` to `keybinds::Action` and its row to `kActions`
in `Keybinds.cpp`; the settings row appears from that table automatically. **Do not invent
a chord in code** — the keybinds rework made chords data. Whether this hangs off the
existing `companion` action or gets its own is a one-line decision to put to the user when
Phase 3 is visible, not before.

**Phase 5 — docs.** `superdoc/features/steam-friends.md`, a line in `superdoc/README.md`,
a `CHANGELOG.md` bullet under **Added**, and — if the user then wants the browser companion
gone — a separate commit for that, never folded into this one.

**Phases 3, 4 and 5 landed 2026-09-08** — `src/Overlay/PanelFriends.{h,cpp}`, the
poller in `src/SteamFriends.{h,cpp}`, `ui::shell::RequestArea()`/`AreaActive()`,
the `friends` keybind action, and
[`../features/steam-friends.md`](../features/steam-friends.md). Evidence:
`build-release/verify-shots/steam-friends-phase345-2026-09-08/results.txt`
(43 checks, 0 failed). **Three things went differently from the sketch above,
and each is recorded where it was decided:**

- **The list shows EVERY friend in a game, not only the joinable ones.** The
  sketch's filter would make an empty panel and a broken read
  indistinguishable, because a wrong `m_steamIDLobby` offset — the one thing
  [§6e](#6e-correction-the-published-vtable-order-is-wrong-by-one-slot) says
  is still unproven — reads as zero exactly like "not in a lobby". Rows that
  cannot be joined carry the reason instead. See the feature doc's *"Why the
  list shows every friend in a game"*.
- **The chord question in phase 4 was decided rather than deferred.**
  `friends` takes `Ctrl+Shift+Tab` (Steam's own friends-list muscle memory,
  and the chord the user asked for), and `companion` — a browser that
  explicitly cannot join anybody ([§4](#4-what-the-shipped-companion-is-actually-worth))
  — moved to `Ctrl+Shift+C`.
- **The poll is a worker thread, not a slow tick on steamcompmgr.** §6d's
  threading rule said "on the steamcompmgr thread at a low poll rate, or on a
  worker"; the first of those is the frame path, so it had to be the second.
  The panel reads a published `View` under a different mutex from the one
  `Snapshot()` holds, which is what makes "the panel blocks on Steam"
  unrepresentable. Measured against a stub that sleeps 1.5 s per call: under
  15 ms worst case in a unit test, and 12 frames in six seconds either way in
  the live harness.

**Deliberately not in the sketch:** no Web API key anywhere, no bundled Steamworks SDK
headers, no `SteamAPI_Init` (route B), no browser, no capture protocol, no second Steam
client.

---

## 8. Steps only the user can run

Nothing here was run for them. Each is safe *when not in a match*.

1. **The one thing phases 1 and 2 could not prove: a non-zero lobby id.**
   Everything else in the read path is now confirmed live ([§6e](#6e-correction-the-published-vtable-order-is-wrong-by-one-slot));
   this needs a friend who is *actually in a joinable lobby* at the moment you run it,
   which never happened while it was being built.
   ```
   build-release/tests/steam_friends_live_probe --diagnose
   ```
   Expect `RESULT m_steamIDLobby: N non-zero, 0 outside the chat band -> PASS` with N
   at least 1, and `RESULT joinable rows` above 0 from the plain
   `steam_friends_live_probe`. It prints **no names, no SteamIDs and no lobby ids** —
   counts, app ids and range verdicts only, and it is strictly read-only.
   Watch for: `outside the chat band` being non-zero, which would mean
   `m_steamIDLobby` is being read from the wrong offset and the join URL would carry
   a wrong number.
   (The study's own `steamclient_probe --connect` still exists but declares the
   **superseded** vtable order, so it reports a friend count of 0 and should not be
   trusted; §6e is why.)
2. **The join URL, once, by hand** — with a friend in a joinable lobby of the game already
   running:
   ```
   steam "steam://joinlobby/<appid>/<lobbyid>/<their steamid64>"
   ```
   (`<lobbyid>` from step 1.) Expect: the **running** game moves them into the lobby, with
   no relaunch and no second gamescope. Watch for: whether it relaunches instead — that
   would change the design of the "different game" case.
3. **Only if the second-Steam idea is ever revisited** (§2b) — and this one carries the
   match risk, so it is last and optional: with a private `HOME`, `steam -silent`, log in,
   then watch whether the host client stays signed in and whether a game still launches
   from it.

---

## Reproducing everything on this page

`build-release/verify-shots/steam-child-session-2026-09-08/` — `README.md` there lists
every command. That directory lives under the build tree and a clean rebuild wipes it.

| file | what it establishes |
|---|---|
| `probe-nesting.sh` → `results-nesting.txt`, `01`–`03*.png` | §2a: a nested session keeps its own game; env inheritance is what decides, and it is escapable per process |
| `probe-cdp.sh` + `cdp_client.py` → `results-cdp.txt`, `10`–`15*.png` | §3: CDP renders and drives a browser you do not own — and stops dead the moment its window is unmapped **or fully covered** |
| `steamclient_probe.c` → `results-steamclient.txt` | §6c: the client's own `steamclient.so` loads and every symbol the friends path needs resolves, with no app id and nothing sent to the live client. **Its `--connect` half declares the superseded vtable order — see [§6e](#6e-correction-the-published-vtable-order-is-wrong-by-one-slot).** |
| `../steam-friends-phase12-2026-09-08/results.txt` | phases 1 and 2: the off-by-one correction, the live read after it, every fail-soft path, and the gate results |
