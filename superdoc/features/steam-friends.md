# Steam friends you can join

**2026-09-08, extended 2026-09-09.** `Ctrl+Shift+Tab` opens a list of the friends who are in a game
right now, read straight out of the Steam client already running on this
machine. A friend in a lobby you can join is marked **[Join]**; clicking them
(or pressing Enter) hands the running Steam client a `steam://joinlobby/…`
URL and it moves you in.

**No Web API key, no browser, no second Steam client, no second sign-in, no app
id and no `SteamAPI_Init` anywhere.**

Code: `src/SteamFriends.{h,cpp}` (the Steam calls, the poller and the name
lookup), `src/SteamFriendsCmd.h` (the pure rules — the joinability predicate,
the URL builder, the status wording, the manifest readers, the row order),
`src/SteamAppNames.h` (the game-name cache, the endpoint and the fetch),
`src/Overlay/PanelFriends.{h,cpp}` (the `system.friends` area),
`src/Keybinds.cpp`'s `friends` action, `src/wlserver.cpp`'s dispatch,
`src/Overlay/UI/Shell.cpp`'s `RequestArea`/`AreaActive` and the per-frame
`PanelFriends_Tick()`.
Tests: `tests/test_steam_friends.cpp`, `tests/steamclient_stub.cpp`.
Live check: `tests/steam_friends_live_probe.cpp`.
Captures: `build-release/verify-shots/steam-friends-phase345-2026-09-08/`
(phase 3–5), `…/steam-friends-phase12-2026-09-08/` (the read path) and
`…/friends-names-invites-2026-09-09/` (the game-name lookup, the row order and
the invite measurement).
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
| **Looking a game's name up online** | **Proven live**, 2026-09-09. Two of the seven friends in a game were in Rocket League (252950) and ULTRAKILL (1229490), neither installed here; both were bare `App <id>` before and both came back named, in **one** request, and were in the cache file afterwards. A second run made **no** request at all. |
| **Reading a pending received invite** | **Established impossible** on this route, with the measurement in [Received invites](#received-invites). Not "unbuilt" — asked, measured, and answered no. |

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
| **Friends** (`friends.list`) | list + verbs | One line per friend in a game, joinable first. Click a row or press Enter to act on it. |
| — verb **Join** | | Joins the selected row. Dimmed, with a reason, when the selection cannot be joined. |
| — verb **Refresh** | | Polls Steam now instead of waiting for the next few seconds to elapse. |
| **Look up game names online** (`overlay.friends_lookup_names`) | switch | Whether an app id the local files cannot name is looked up against Steam's public list. See [Where the game's name comes from](#where-the-games-name-comes-from). |
| **Status** (`friends.status`) | read-only | Why the list is the length it is, the app id this session is running under, the standing note about the unproven lobby offset, and why invites are not here. |

The area carries the **`global only`** badge, like Appearance, Cursor and
Keybinds: its one setting writes `global.json` whatever profile the session is
editing, because *"may this machine reach the network"* is a fact about the
machine and not about the game. (Until 2026-09-09 the area had no settings at
all and deliberately carried no badge.) The other thing you can configure is
the chord, under **Setup > Keybinds**.

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

`ISteamFriends` hands back an **app id and no name**, so the name is looked up
in two places, in this order, and the second is only ever asked what the first
could not answer.

**1. Steam's own `appmanifest_<appid>.acf`** — the file the client writes for
every **installed** game — under `~/.steam/steam/steamapps` plus every root
`libraryfolders.vdf` lists. Free, offline, and authoritative about what this
machine has.

**2. Steam's keyless public endpoint**, for the ids the manifests cannot answer
(2026-09-09). Until then those rows read `App 252490`, which was true but
useless: measured live on this machine, three of the seven friends in a game
were in Rocket League and ULTRAKILL, neither installed here, so nearly half the
list was bare numbers.

`Why the original "the manifest is enough" reasoning was wrong:` it argued that
a game you can join is a game you have installed. That is true of the **join**
and false of the **list** — the panel deliberately shows every friend in a
game, joinable or not (see the section above), so most rows are about games you
do *not* own.

#### Which endpoint, and why

Measured from this machine on 2026-09-09:

| candidate | verdict |
|---|---|
| `api.steampowered.com/ISteamApps/GetAppList/v2` (and `/v0002`) | **HTTP 404** — `Method 'GetAppList' not found in interface 'ISteamApps'`. The "download the whole list once" option is not a size trade-off any more; the endpoint is **gone**. |
| `store.steampowered.com/api/appdetails?appids=<id>` | 36 501 bytes for **one** app; 15 231 with `filters=basic`, which is still the whole store description. One request **per app**, on the rate-limited endpoint. (`filters=name` is not a thing — it answers `{"success":true,"data":[]}`.) |
| **`api.steampowered.com/ICommunityService/GetApps/v1/`** | **chosen.** Keyless, **batched**, and **711 bytes for four apps**. |

So one request per poll rather than one per app, ~50× smaller than the only
alternative that still exists, and appdetails' rate limit never comes into
play. An id Steam does not know comes back as `{"appid":N}` with no name —
a clean, authoritative *"there is no name for this"*, which is cached so the
id is never asked about again.

#### Exactly what leaves the machine

**A list of app ids. Nothing else.** `SteamAppNames.h`'s `BuildAppNamesUrl()`
builds the whole query string out of `std::to_string()` over integers, so there
is no string input for a SteamID, a persona name, a lobby id or an account name
to travel in — the same property `BuildJoinUrl()` has, and for the same reason.
The unit test walks every byte after the `?` and requires it to be a digit or
punctuation this code wrote.

Plus what any HTTP request unavoidably carries: **this machine's IP address**,
and **curl's own version string** as the User-Agent. **No cookie** — the fetch
passes no jar, and `-q` stops curl reading `~/.curlrc`, so nothing the user
configured can attach an identity to it. The request cannot be tied to a Steam
login.

#### The switch, and why it defaults on

**Friends → "Look up game names online"** (`overlay.friends_lookup_names`, in
`global.json`). Off, no fetch is ever spawned and an unknown game stays
`App 252490`.

`Why on by default,` stated with its counter-argument because the opposite is
defensible. **For:** the request contains nothing about the user; it happens
only while the friends list is actually being looked at (the poller sleeps
otherwise) and only for ids the disk could not answer, so an idle compositor
and a user who never opens the panel make **zero** requests; and defaulting it
off would ship the exact `App 252490` this exists to fix, behind a switch
nobody knows to look for. **Against:** a compositor talking to the internet is
a new class of behaviour, and consent is normally opt-in. The tie is broken by
what is actually at stake — an app id is not a fact about a person — and by the
switch being one row away, in the same area, with its Help line naming what
leaves the machine.

#### The cache

| | |
|---|---|
| **where** | `$XDG_CACHE_HOME/gamescope-ritz/appnames.json`, else `~/.cache/gamescope-ritz/appnames.json`. **Never the config directory** — game names are not a setting, nothing here is the user's choice, and deleting the file must cost nothing but a few hundred bytes of traffic. |
| **what** | `{"version":1,"apps":{"730":{"name":"Counter-Strike 2","seen":1788906836}}}`. An **empty name** is a real entry: *Steam was asked and has no name for this id*. |
| **bound** | 512 entries, enforced on **read and on every write**. Past it, the least recently *seen* entries go — an LRU over "when was a friend last playing this", so the games your friends actually play stay. Eviction ties break on the app id, so the survivors are a function of the data and not of the machine. |
| **corrupt** | Truncated, half-written, hand-edited, a wrong type in one entry, a future `version`, a megabyte of zeroes — **all parse as empty**, which is indistinguishable from a fresh machine. A cache is a thing the program must work without. |
| **written** | Through a temporary and renamed, so a crash or a full disk leaves the *old* cache rather than half of a new one. |
| **failure** | A timeout, no network or an HTTP error is **never written**. It is not an answer about the app, and storing it would bake a temporary outage into a permanent wrong label. A failed fetch backs the *whole* lookup off for five minutes — failures are network-wide, not per-app. |
| **silence** | An id that was asked about, in a request that *succeeded*, and that the answer did not mention at all is not asked about again **this session** — and not cached either. Steam saying nothing is not an answer worth keeping forever, but re-asking every three seconds for as long as the panel is open would be a request loop. |

#### It cannot stall a frame

The fetch runs on the **poller thread**, after the view has already been
published — so the rows are on screen (as `App <id>`) before the network is
touched at all — and never inside `Snapshot()`'s lock. It is bounded twice:
curl's own `--max-time`, and a wait loop that kills the child if it outlives
it, because "somebody else enforces the timeout" is only true while that
somebody is alive. `Shutdown()` cuts it short, so a wedged endpoint cannot
delay gamescope's exit either.

Measured, against a `curl` that sits there for five seconds: worst
`CurrentView()` **under 15 ms** — the same threshold, and the same shape of
test, as the slow-Steam case below. Measured again against the live client with
a blackholed endpoint: every row still drew, the installed game still got its
name from the manifest, the two unknown ones read `App <id>`, and nothing was
written to the cache.

### The order the rows are drawn in

Requested 2026-09-09: *"Joinable players should be sorted towards the top. And
topmost should be invites."* Three bands, in that order, alphabetical inside a
band:

1. **invites** — see [Received invites](#received-invites) below: **nothing
   produces one**, so this band is always empty today. The rule is written down
   in full anyway (`SteamFriendsCmd.h`'s `FriendGroup`), and the panel draws no
   invite row and offers no Accept/Deny, because there is nothing true to draw.
2. **joinable** — the `[Join]` rows.
3. **everyone else in a game.**

The tiebreak inside a band is the persona name folded to lower case (ASCII
only — a locale-aware fold would make the order depend on the user's locale),
then the **SteamID**.

`Why the SteamID is in the comparator at all:` personas are not unique, and a
friend can rename themselves into somebody else's name on purpose. Without a
final tiebreak `std::sort` would be free to swap two same-named rows on every
poll, and the list would flicker between two orders while nothing about it had
changed. With it, the order is a function of the data, so an unchanged friends
list draws identically forever — pinned by a test that sorts the same rows from
three different starting permutations and requires three identical results.

The sort lives in **`Snapshot()`**, not in the panel, so the panel,
`friends_dump` and `friends_join <n>`'s indices all address one list in one
order.

#### The selection follows the person, not the row number

The panel stores the **SteamID of the selected friend** and looks its index up
each frame; `ActivateRow()` records a SteamID, and the list's getter answers
with wherever that person is now.

`Why that is not a detail:` the list sorts itself and the poller replaces it
every three seconds. The moment one friend joins a lobby they jump to the
joinable band and every row below them moves. With an index-based selection the
outline would land on whoever slid into that slot — under a user who had not
touched anything — and the **Join** verb would be aimed at them. That is the
classic bug in a list that reorders itself, and storing the person instead of
the row number is what makes it unrepresentable. A selection that genuinely
stops existing (they quit, they went offline) reads as *nothing selected*,
which is a different and honest state.

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

## Received invites

**Asked 2026-09-09: "make sure that received invites can be clicked/joined to.
Once invited, there should be an accept/deny button."**

**Verdict: a pending received invite cannot be observed through any path this
design can safely take, so no invite row and no Accept/Deny were built.** This
section is the evidence, because "we did not build it" is only an acceptable
answer with the measurement attached.

### What was established

**1. Steamworks has no call that enumerates a pending received invite.**
Nothing in `ISteamFriends` answers *"who has invited me and not been answered
yet"*. The only invite-shaped surfaces in the whole interface are three
**callbacks**:

| callback | id | when it fires |
|---|---|---|
| `GameLobbyJoinRequested_t` | 333 | the user has **already accepted** an invite, in Steam's own UI or overlay |
| `GameRichPresenceJoinRequested_t` | 337 | same, by the rich-presence route |
| `GameConnectedFriendChatMsg_t` | 343 | a friend chat message, **only** after `SetListenForFriendsMessages(true)` |

All three describe an invite the user has **finished with**, not one waiting for
an answer. By the time 333 fires, the Accept this panel would have offered has
already been pressed somewhere else.

**2. Callbacks are not the blocker — they reach us fine.** This was the open
question, because our route deliberately has no app id and no `SteamAPI_Init`,
and the callback pump is usually described as needing both. It does not:
`steamclient.so` exports `Steam_BGetCallback` and `Steam_FreeLastCallback` as
**flat C symbols with published signatures**, so pumping needs no vtable slot
and breaks no rule from [§6e](../planning/steam-friends-join.md#6e-correction-the-published-vtable-order-is-wrong-by-one-slot).
Measured on a pipe with no app id, against the live client
(`steam_friends_live_probe --callbacks`):

```
RESULT callbacks delivered: 16512          (a 300 s pump)
  callback id 304   x499  payload 12 bytes  (PersonaStateChange_t)
  callback id 336   x368  payload 12 bytes  (FriendRichPresenceUpdate_t)
  …plus other interfaces' internal ids
RESULT callbacks reach a no-app-id pipe: YES
RESULT friends-range (300..399) callbacks seen: YES
RESULT invite-shaped callbacks (333/337/343) seen: NO
```

Both friends-range ids arrive with **exactly** their published payload sizes
(12 bytes each), which is what makes "these really are Steamworks callback ids"
a measurement rather than a guess. Callback queues are **per pipe**, so this
cannot take anything from the running game's own pipe.

**3. `GetFriendRichPresence(steamID, "connect")` was considered and rejected,
twice over.** It would not answer the question — a `connect` string says *this
friend advertises a joinable session*, which is what `m_steamIDLobby` already
tells us, and says nothing about whether they invited **you**. And it is not
reachable safely: applying §6e's measured −1 shift puts it at slot 44, whose
neighbours under the plausible layouts include `ClearRichPresence` — a **write
on the user's live account**. There is no argument shape that is read-only
under every candidate identity, which is precisely the case §6e's rule forbids.

**4. Not tested with a real invite, and that is stated rather than hidden.**
Testing one means asking `Blendgranate` or `atze` to send it, and asking them
means sending a Steam message — which is an `ISteamFriends` **write** at an
unmeasured slot, the same wall as point 3. So the finding rests on points 1–3,
not on having watched an invite arrive and be missed.

### What would have to change

- **Chat-carried invites:** `SetListenForFriendsMessages(true)` plus
  `GetFriendMessage()`, i.e. two more vtable slots measured by a method that
  does not call an unmeasured slot. Even then a chat invite is a
  `k_EChatEntryTypeInviteGame` **message**, not a queryable pending list, so
  "Deny" would still only be able to hide a row.
- **Or** the flat `libsteam_api.so` route with an app id — which
  [§6a](../planning/steam-friends-join.md#6a-two-routes-and-the-safer-one-needs-no-app-id-at-all)
  rejected for the whole design, and which still would not enumerate pending
  invites.
- **Or** Steam's Web API, which needs a key — rejected on the user's own
  instruction, and correctly.

### What the panel says instead

The Status area carries the sentence in the product, not only here: *"invites —
not shown; Steam has no way to tell this list about an invite you have been
sent; accept those in Steam itself."* A row that never appears would be worse
than a sentence that explains why.

The ordering rule still names invites as the top band
([above](#the-order-the-rows-are-drawn-in)), and a test pins **both** halves:
that the band sorts first, and that the shipping read path never puts a row in
it.

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
