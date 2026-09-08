# Steam friends you can join

**2026-09-08.** `Ctrl+Shift+Tab` opens a list of the friends who are in a game
right now, read straight out of the Steam client already running on this
machine. A friend in a lobby you can join is marked **[Join]**; clicking them
(or pressing Enter) hands the running Steam client a `steam://joinlobby/…`
URL and it moves you in.

**No Web API key, no browser, no second Steam client, no second sign-in, no app
id and no `SteamAPI_Init` anywhere.**

Code: `src/SteamFriends.{h,cpp}` (the Steam calls and the poller),
`src/SteamFriendsCmd.h` (the pure rules — the joinability predicate, the URL
builder, the status wording, the manifest readers),
`src/Overlay/PanelFriends.{h,cpp}` (the `system.friends` area),
`src/Keybinds.cpp`'s `friends` action, `src/wlserver.cpp`'s dispatch,
`src/Overlay/UI/Shell.cpp`'s `RequestArea`/`AreaActive` and the per-frame
`PanelFriends_Tick()`.
Tests: `tests/test_steam_friends.cpp`, `tests/steamclient_stub.cpp`.
Live check: `tests/steam_friends_live_probe.cpp`.
Captures: `build-release/verify-shots/steam-friends-phase345-2026-09-08/`
(phase 3–5) and `…/steam-friends-phase12-2026-09-08/` (the read path).
The investigation that settled the whole design:
[`../planning/steam-friends-join.md`](../planning/steam-friends-join.md).

---

## What is proven, and what is not

This is the first thing to read, because one part of this feature has never
been observed working and the panel is built to fail honestly when it does not.

| | status |
|---|---|
| **Loading the Steam client's own `steamclient.so` with no app id** | **Proven live** on this machine. Every symbol resolves, `Steam_CreateSteamPipe` + `Steam_ConnectToGlobalUser` attach to the already-signed-in user, and nothing registers a game. |
| **Reading the friends list** | **Proven live.** 75 friends on the first run, 28 after the signed-in account changed; persona names all printable. |
| **Reading what each friend is playing** | **Proven live.** App ids came back as real Steam apps (730, 252950, 2483190, 2357570, 736220), CGameID type 0 for every one — so `m_gameID`'s offset in `FriendGameInfo_t` is anchored correctly. |
| **The corrected vtable offsets** | **Measured, not quoted.** The published `ISteamFriends` order every write-up repeats is **wrong by one slot** against the real client; `GetFriendCount` is slot 2 and `GetFriendByIndex` is slot 3. See [§6e](../planning/steam-friends-join.md#6e-correction-the-published-vtable-order-is-wrong-by-one-slot). |
| **`m_steamIDLobby`'s offset** | **NOT PROVEN.** Nobody in the friends list was in a joinable lobby at any moment it was sampled, so the field has only ever read as zero — which is also exactly what a wrong offset would look like. |
| **Firing a join** | **Not run against a live client.** The URL builder is unit-tested, and the whole spawn path (build, PATH lookup, fork, argv) is proven end to end against a `steam` shim that records what it was handed. No `steam://joinlobby` URL has ever reached a real Steam. |

### What the user has to run to close it

Out of a match, with **a friend who is actually in a joinable lobby**:

```
build-release/tests/steam_friends_live_probe --diagnose
```

Expect `RESULT m_steamIDLobby: N non-zero, 0 outside the chat band -> PASS`
with N at least 1, and `RESULT joinable rows` above 0 from the plain
`steam_friends_live_probe`. It prints **no names, no SteamIDs and no lobby
ids** — counts, app ids and range verdicts only — and is strictly read-only.

Watch for `outside the chat band` being non-zero: that means `m_steamIDLobby`
is being read from the wrong offset, and a join URL built from it would carry
a wrong number.

Then, once, by hand, with that friend still in that lobby **and in the game
you are already running**:

```
steam "steam://joinlobby/<appid>/<lobbyid>/<their steamid64>"
```

Expect the running game to move you into the lobby with no relaunch. If it
relaunches instead, the "different game" case in this doc needs rethinking.

---

## Why the list shows every friend in a game, not only the joinable ones

The implementation sketch said *"one line per joinable friend"*. **The shipped
panel lists every friend who is in a game and marks which of them can be
joined.** That is a deliberate departure, and the reason is the unproven offset
above.

A wrong `m_steamIDLobby` offset reads as **zero**, which is byte-for-byte
identical to *"this friend is not in a lobby you can join"*. So a
joinable-only list would be **indistinguishable from a completely broken
one**: the user would open the panel, see an empty box, and have no way at all
to tell whether their friends are simply all busy or the feature does not work
on their client.

Listing everyone in a game degrades honestly. Both worlds show the same thing —
your friends, and what they are playing — and the only difference is whether
any row carries a **[Join]** mark. Nothing is hidden, nothing is guessed, and
on the day a real lobby id appears the marks light up with no other change.

The same reasoning is why `LooksLikeLobbyId()` in `SteamFriends.cpp` only
**counts** out-of-band lobby ids into a debug line and never **filters** on
them: an unverified band check would silently hide exactly the friends this
feature exists to show.

---

## The area

`system.friends`, "Friends" in the rail, in the same group as the HUD, the
Mixer and the Crosshair — the things you reach for *during* a match rather
than the pages you set up once.

| row | kind | what it does |
|---|---|---|
| **Friends** (`friends.list`) | list + verbs | One line per friend in a game. Click a row or press Enter to act on it. |
| — verb **Join** | | Joins the selected row. Dimmed, with a reason, when the selection cannot be joined. |
| — verb **Refresh** | | Polls Steam now instead of waiting for the next few seconds to elapse. |
| **Status** (`friends.status`) | read-only | Why the list is the length it is, plus the app id this session is running under and the standing note about the unproven lobby offset. |

There are **no settings** here — nothing on this page writes a config file, so
the area deliberately carries no layer badge. The one thing you *can* configure
is the chord, under **Setup > Keybinds**.

### A row

```
[Join]  atze                            Counter-Strike 2
        Blendgranate       Rust · not in a lobby you can join
```

- The **label** is the persona name, and it is never sacrificed to fit — that
  priority is the list atom's own (`Controls.h`'s `LayoutListBoxItem`).
- The **tag** is `[Join]` on a row that can be joined, and nothing otherwise.
- The **secondary** is the game, and — on a row that cannot be joined — the
  quiet reason, one of four: *not in a lobby you can join*, *not a Steam game*
  (a mod, a shortcut or a non-Steam game they added), *no game to join*, *no
  Steam ID*.

Clicking a row that cannot be joined selects it and toasts the same reason. It
is never an error and never does nothing silently.

### Where the game's name comes from

`ISteamFriends` hands back an **app id and no name**. The name is read out of
Steam's own `appmanifest_<appid>.acf` — the file the client writes for every
**installed** game — found under `~/.steam/steam/steamapps` plus every root
`libraryfolders.vdf` lists. A game this machine does not have falls back to
`App 730`, which is short and never wrong.

`Why that is enough rather than a half-measure:` the case this feature exists
for is joining a friend in the game you are already in, and a game you can join
is a game you have installed. A web lookup would need a network call, a key or
both to say the same thing.

### The empty states

Each is **text inside the list**, never blank space, and each says a different
true thing:

| situation | what the list says |
|---|---|
| no Steam installed here | *Steam isn't installed here.* |
| a library that is not Steam's | *the Steam client library isn't the one we know.* |
| a client whose interfaces this build does not know | *this Steam client's interfaces are newer than this build knows.* |
| Steam installed but not running | *Steam isn't running.* |
| running but signed out | *Steam is signed out.* |
| the friends interface is not laid out as expected | *this Steam client's friends interface isn't laid out the way this build expects.* |
| nobody playing anything | *Nobody's in a game right now.* |
| before the first poll finishes | *Asking Steam…* |

The one state that is **not** empty is "friends in a game, none joinable" — the
rows are all there, unmarked, and the status line reads *5 friends in a game,
none you can join.*

---

## Joining

`steam://joinlobby/<appid>/<lobbyid>/<steamid64>`, handed to the running client
by `exec`ing `steam <url>`, which forwards it over `~/.steam/steam.pipe`.
`joinlobby` is a registered command in Steam's own URL table inside
`steamui.so`, sitting beside `rungame` and `connect` — not a guess.

**Every byte of that URL is `std::to_string()` over an integer.** The persona
name — the one field a remote person controls — has no path into the URL at
all, so a friend who renames themselves `x";rm -rf ~;"` changes it by exactly
nothing. That is a property of the types, not of an escaping routine somebody
could forget, and `tests/test_steam_friends.cpp` pins it by building the same
URL twice with two hostile names and requiring the bytes to be identical.

### The confirmation, and exactly when it appears

| the friend is in | what happens |
|---|---|
| **the game you are already running** (their app id == `config::SessionAppId()`) | It fires straight away, no dialog. Steam relaunches nothing: the running game receives the join through its own `GameLobbyJoinRequested` callback and moves you. Nothing about gamescope, Ritz or the wrapper is involved. |
| **a different game** | A dialog first: *"Join Team Fortress 2? This leaves Counter-Strike 2 and starts Team Fortress 2."* Steam launches that game over this one — which is exactly what accepting an invite in Steam's own overlay does today, so this is not making anything worse; it is just worth stopping to ask about. |
| **not a Steam game at all**, or gamescope was not launched by Steam | Every join is treated as a different game and gets the dialog. |

### A click only records; `Tick()` acts

The list's setter is reachable two ways: a click or Enter on the draw thread,
and `overlay_e2_set friends.list <n>` on the **console** thread. Forking a
process and opening a modal are both illegal on the latter. So the setter only
records a pending join, and `PanelFriends_Tick()` — called once per frame from
the shell's own `Draw()` — is the single place a join is ever fired.

The pending join stores the **SteamID it was aimed at**, not just the index.
The poller can replace the list between the click and the tick, and joining
whoever happens to land on index 2 afterwards is exactly the bug an index-only
handoff produces. A row that is gone by the time the tick runs toasts *"That
lobby is gone."*

---

## Nothing on the frame path ever waits for Steam

`Snapshot()` is a round trip to another process over `~/.steam/steam.pipe`. A
Steam that is swapping, starting up or wedged can make that take seconds, and
doing it on the steamcompmgr thread — which **is** the frame path — would drop
frames in a running game.

So:

- **One worker thread owns every Steam call.** It polls every 3 s and publishes
  a `View` under its own mutex.
- **The panel calls `CurrentView()`**, which copies that published `View` and
  returns. It never takes the lock `Snapshot()` holds for the whole round trip.
  The two locks being different is what makes "the panel blocks on Steam"
  unrepresentable rather than merely avoided.
- **It only runs while somebody is looking.** `CurrentView()` records the ask;
  the worker keeps polling while asks arrive and then sleeps on a condition
  variable. A build whose friends panel is never opened and whose hotkey is
  never pressed **starts no thread and dlopens nothing** — `steamclient.so` is
  46 MB and loading it starts threads inside our process.
- **The panel keeps no cache of its own.** Its getters run on the draw thread
  *and* on the console thread, and a `std::vector<Friend>` in a static would be
  written by one while the other read it. Copying a few dozen small rows a
  handful of times a frame is cheaper than that race.

### How that was proven

`tests/steamclient_stub.cpp` grew a **`slow` mode** that sleeps 1.5 s inside
the snapshot. Two measurements, both in
`build-release/verify-shots/steam-friends-phase345-2026-09-08/`:

- **A unit test** measures the worst `CurrentView()` call while such a poll is
  in flight: **under 15 ms**, against the ~1 500 000 µs a blocking
  implementation would have measured.
- **The live harness** runs the same six seconds twice, with a Steam that
  answers instantly and with one that takes 1.5 s a call: **12 frames each**,
  worst compositor round trip **3 ms vs 2 ms**. For contrast, `friends_dump` —
  which calls `Snapshot()` straight from the console thread on purpose — does
  take the full 1.5 s, which is what proves the stub really was slow.

`Shutdown()` (from `steamcompmgr_exit()`) **joins** rather than detaches. A
detached worker would outlive the statics it publishes into, which is a crash
at exit rather than a clean one. The cost is that a Steam that has stopped
answering can delay gamescope's exit by up to one round trip — an exit, never
a frame.

---

## The keybind

`Ctrl+Shift+Tab`, the `friends` action in `src/Keybinds.cpp`'s `kActions`
table. That is Steam's own friends-list muscle memory, and this is a friends
list, so it is the right chord for it. It is not a conflict with Steam's
overlay: gamescope swallows the key, so neither the game nor Steam's overlay
(which this fork's users have switched off anyway) ever sees it.

**The Steam chat companion moved off that chord to `Ctrl+Shift+C`** on the same
day. The companion is a browser that explicitly *cannot join anybody* — the one
thing the user actually wanted — so it was holding the binding for the feature
that could not deliver it. Anyone who prefers the old arrangement can set it
back under **Setup > Keybinds**; the conflict rule will make them move
`friends` off `Ctrl+Shift+Tab` first, which is the correct order.

The binding is a **toggle on its own area**: pressed again while the friends
area is the one on screen, it closes the overlay rather than re-selecting what
is already selected and leaving no way back out on the same key. That needs
`ui::shell::RequestArea()` (an atomic consumed by `Draw()` on the thread that
owns the selection) and `AreaActive()` (republished every frame) — deliberately
not `overlay_e2_select`, which assigns the selection straight from the console
thread and is fine only for a debug surface driven by a script.

**The key path makes no Steam call of any kind.** It sets one atomic; the first
Steam call of the whole feature still happens later, on the poller's thread.

---

## Privacy

Names appear on screen because that **is** the feature. They stay out of
everything else:

- **Nothing in `src/SteamFriends.cpp` or `src/Overlay/PanelFriends.cpp` logs,
  toasts or writes a persona name, a SteamID or a lobby id.** The log lines
  carry counts, indices, app ids and interface version strings.
- `friends_dump` prints **counts and app ids by index**, plus which rows are
  joinable and why the others are not. `friends_join <n>` takes that index, so
  a user never has to see or retype an id to use the feature from the console.
- The live probe prints range **verdicts** about ids, never the ids.
- The verification harness asserts it: no fake persona name and no SteamID64
  appears in any gamescope log it produced.

**One documented exception.** `overlay_e2_get friends.list` is the registry's
generic "what is this row's value" debug command, and a list row's value is its
selected item's label — which here is a person's name. That is a debug command
you run yourself, echoing something already on your screen; the product never
performs it on its own. It is named here so nobody is surprised to find a name
in a log they made that way.

---

## Its relationship to the browser companion

[`steam-companion.md`](steam-companion.md) is the other half of "gamescope
talking to Steam", and the two do genuinely different things:

| | friends list (this page) | [chat companion](steam-companion.md) |
|---|---|---|
| **can join a friend** | **yes** — the whole point | **no**, and never could: the web client has no join button |
| separate sign-in | none | yes |
| resident cost | a `dlopen` and one sleeping thread | a browser, a few hundred MB, for the life of the session |
| chat | no | yes |
| voice, invites, notifications | no | no |
| chord | `Ctrl+Shift+Tab` | `Ctrl+Shift+C` |

If it turns out you only ever wanted to join, the companion is a resident
browser earning nothing and should be **reduced or removed rather than
maintained**. That call is the user's, and it should be put to them once this
list has been seen working against a real lobby — not before.

Steam's own overlay would still beat both at voice, invites and notifications;
it is off for these games only because Ritz's `clear_ld_preload` module empties
`LD_PRELOAD`. That trade is stated in the companion's own page and has not
changed.

---

## Failure modes, and what each one does

Every one of these is a `nullptr` or a zero on a path that already has to
handle it. Nothing here throws, blocks or is fatal.

| situation | behaviour |
|---|---|
| Steam not installed, or somewhere we do not look | `dlopen` fails once, at first use. One log line, and the list says so. |
| a library loaded that is not Steam's | Six fingerprint symbols are checked before anything is cast to a vtable. One log line. |
| Steam not running | `Steam_CreateSteamPipe` returns 0. |
| signed out | `Steam_ConnectToGlobalUser` returns 0. |
| a client whose interface versions we do not know | The version lists in `SteamFriendsCmd.h` are **closed**: an unknown client disables the feature rather than guessing a layout. |
| a client whose vtable moved again | `Snapshot()` range-checks the first SteamID it gets back. A number that is not a SteamID abandons the whole read with a named line, rather than handing `GetFriendGamePlayed` a garbage id. |
| `steam` not on PATH | The join refuses with one sentence. |

Each failure logs its reason **exactly once** per distinct reason, and re-logs
if the situation changes (Steam started, then signed out) — the panel polls
every few seconds, and a line per poll is a log nobody can read.
