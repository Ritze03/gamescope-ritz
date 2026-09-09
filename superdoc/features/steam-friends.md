# Steam friends you can join

**2026-09-08, narrowed 2026-09-09.** `Ctrl+Shift+Tab` opens a list of the friends who are
**in the game you are running right now and in a lobby you can join**, read
straight out of the Steam client already running on this machine. Click one (or
press Enter) and gamescope hands the running Steam client a
`steam://joinlobby/…` URL; it moves you in without relaunching anything.

**No Web API key, no browser, no second Steam client, no second sign-in, no app
id handed to Steam, no `SteamAPI_Init` — and, since the 2026-09-09 narrowing, no
network at all. This compositor now makes no outbound request of any kind.**

Code: `src/SteamFriends.{h,cpp}` (the Steam calls and the poller),
`src/SteamFriendsCmd.h` (the pure rules — the two predicates, the URL builder,
the status wording, the row order), `src/Overlay/PanelFriends.{h,cpp}` (the
`system.friends` area), `src/Keybinds.cpp`'s `friends` action,
`src/wlserver.cpp`'s dispatch, `src/Overlay/UI/Shell.cpp`'s
`RequestArea`/`AreaActive` and the per-frame `PanelFriends_Tick()`.
Tests: `tests/test_steam_friends.cpp`, `tests/steamclient_stub.cpp`.
Live check: `tests/steam_friends_live_probe.cpp`.
Captures: `build-release/verify-shots/friends-simplify-2026-09-09/` (the
narrowed list, every empty state and the hidden area),
`…/steam-friends-phase345-2026-09-08/` (phase 3–5) and
`…/steam-friends-phase12-2026-09-08/` (the read path).
The investigation that settled the whole design:
[`../planning/steam-friends-join.md`](../planning/steam-friends-join.md).

---

## What the list contains, and what it deliberately does not

**One row per friend who is in *this* game and is joinable.** Nothing else.
Asked for on 2026-09-09, in these words:

> *"lets simplify it to only showing friends, which are playing the same game as
> the running one. So there is no unneeded stuff in the UI. … Just show joinable
> friends in the UI. Also dont show the 'Friends' menu at all, if it isnt a
> steam game"*

Three things follow from it, and each removed code rather than adding it:

- **A friend in a different game is not a row.** Joining them would close the
  game you are in and launch theirs — which the panel used to offer behind a
  confirmation dialog. The dialog is gone with the rows it existed for.
- **A friend in this game who is not joinable is not a row either** — but they
  are still *counted*, in the Status line. See the next section, which is the
  one part of the old design this narrowing had to preserve on purpose.
- **The whole area is hidden when this is not a Steam game.** Not a disabled
  rail entry: no rail entry, no palette rows, no reachable sheet.

`Why the row itself is just a name and a [Join]:` every row is by construction
the game already running, so a game column would print the same words down the
whole list. That was the "unneeded stuff" the request names.

---

## Why the Status row carries two counts

This is the one piece of the previous design that survives the narrowing, and
it is load-bearing.

`m_steamIDLobby`'s offset inside `FriendGameInfo_t` is **still unproven**
([§6e](../planning/steam-friends-join.md#6e-correction-the-published-vtable-order-is-wrong-by-one-slot)):
nobody in the friends list has been in a joinable lobby at any moment the read
path was measured, so the field has only ever read as **zero** — and a wrong
offset reads as zero too. A joinable-only *list* is therefore, on its own,
**indistinguishable from a broken read**: an empty box either way.

Before the narrowing that was solved by listing everyone in a game and marking
the joinable ones. That is exactly the clutter the user asked to remove, so the
diagnosability moved into **one line of text**:

```
3 friends in this game, 0 you can join.
```

- **`0 friends in this game`** — nobody else is here. Nothing is claimed about
  the lobby read at all.
- **`3 friends in this game, 0 you can join`** — the friends read works, the
  game match works, and *only* the lobby field is coming back empty. That is
  either the honest answer (nobody is in a lobby) or the unproven offset, and
  the user can now tell it apart from every other empty state.
- **`3 friends in this game, 2 you can join`** — the day the offset is
  confirmed, this is what it looks like.

It costs no list clutter and no extra call: the count falls out of the same
walk that builds the rows.

---

## What is proven, and what is not

| | status |
|---|---|
| **Loading the Steam client's own `steamclient.so` without `SteamAPI_Init`** | **Proven live** on this machine. Every symbol resolves, `Steam_CreateSteamPipe` + `Steam_ConnectToGlobalUser` attach to the already-signed-in user, and nothing registers a game. *This row used to say "with no app id"; measured 2026-09-09, `steamclient.so` reads `SteamAppId` out of the environment at pipe creation, and Steam sets it — so a compositor Steam launched reports app 730 on its pipe. What we never do is `SteamAPI_Init`. See [`../planning/steam-invite-and-vtable-layout.md`](../planning/steam-invite-and-vtable-layout.md) §4.* |
| **Reading the friends list** | **Proven live.** 75 friends on the first run, 28 after the signed-in account changed; persona names all printable. |
| **Reading what each friend is playing** | **Proven live.** App ids came back as real Steam apps (730, 252950, 2483190, 2357570, 736220), CGameID type 0 for every one — so `m_gameID`'s offset in `FriendGameInfo_t` is anchored correctly, which is what the same-game filter rests on. |
| **The corrected vtable offsets** | **Measured, not quoted.** The published `ISteamFriends` order every write-up repeats is **wrong by one slot** against the real client; `GetFriendCount` is slot 2 and `GetFriendByIndex` is slot 3. See [§6e](../planning/steam-friends-join.md#6e-correction-the-published-vtable-order-is-wrong-by-one-slot). |
| **`m_steamIDLobby`'s offset** | **NOT PROVEN.** Nobody in the friends list was in a joinable lobby at any moment it was sampled, so the field has only ever read as zero — which is also exactly what a wrong offset would look like. The Status row above is what keeps that distinguishable. |
| **Firing a join** | **Not run against a live client.** The URL builder is unit-tested, and the whole spawn path (build, PATH lookup, fork, argv) is proven end to end against a `steam` shim that records what it was handed. No `steam://joinlobby` URL has ever reached a real Steam. |
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

Expect the running game to move you into the lobby with no relaunch.

---

## The area

`system.friends`, "Friends" in the rail, in the same group as the HUD, the
Mixer and the Crosshair — the things you reach for *during* a match rather
than the pages you set up once.

| row | kind | what it does |
|---|---|---|
| **Friends** (`friends.list`) | list + verbs | One line per joinable friend in this game. Click a row or press Enter to join. |
| — verb **Join** | | Joins the selected row. Dimmed, with a reason, when nothing is selected or the list is empty. |
| — verb **Refresh** | | Polls Steam now instead of waiting for the next few seconds to elapse. |
| **Status** (`friends.status`) | read-only | The two counts above, the app id this session is running under, the standing note about the unproven lobby offset, and why invites are not here. |

The area carries **no settings at all**, so it carries no profile badge either.
It has nothing to store: the rows come from Steam and the only thing you can
configure about the feature is the chord, under **Setup > Keybinds**.

### A row

```
[Join]  atze
[Join]  Blendgranate
```

The **label** is the persona name, and it is never sacrificed to fit — that
priority is the list atom's own (`Controls.h`'s `LayoutListBoxItem`). The
**tag** is `[Join]`, on every row, because every row is joinable. There is no
secondary line.

### Hidden when this is not a Steam game

`PanelFriends.cpp` declares
`a.AvailableWhen([]{ return steamfriends::SessionAppId() != 0; })`, using the
registry's existing area-level gate — the same one `system.display`'s
nested-only rows use. `Registry::RailAreas()`, `CommandPalette.cpp`'s two index
walks and `Shell.cpp`'s `SelectedArea()` all skip an area that is not
`Available()`, so one predicate removes the rail entry, the palette rows and
the sheet together.

`Why hidden rather than disabled:` with no app id every row would have to match
an id that does not exist, so the page could only ever be empty. A disabled
entry is a promise that something is there; there is nothing there.

The app id is `config::SessionAppId()`, resolved once at startup and **copied
into `SteamFriends.cpp` as an integer** by `PanelFriends_SeedFromConfig()`
(called from `main.cpp`). `Why a copy and not the getter:` `SessionAppId()`
resolves lazily into a plain static with no lock, and the poller runs on its
own thread — so calling it there would race the draw thread's first call. One
seeded integer keeps the worker out of the config layer entirely, and makes it
impossible for the panel and the poller to disagree about which game this is.

### The order the rows are drawn in

Requested 2026-09-09: *"Joinable players should be sorted towards the top. And
topmost should be invites."* Two bands survive the narrowing — everything in
the list is joinable now, so the third band has nothing to hold:

1. **invites** — see [Received invites](#received-invites) below: **nothing
   produces one**, so this band is always empty today. The rule is kept in full
   anyway (`SteamFriendsCmd.h`'s `FriendGroup`) because it is the user's stated
   order and costs ten lines; the panel draws no invite row and offers no
   Accept/Deny, because there is nothing true to draw.
2. **joinable** — every row there is.

The tiebreak inside a band is the persona name folded to lower case (ASCII
only — a locale-aware fold would make the order depend on the user's locale),
then the **SteamID**.

`Why the SteamID is in the comparator at all:` personas are not unique, and a
friend can rename themselves into somebody else's name on purpose. Without a
final tiebreak `std::sort` would be free to swap two same-named rows on every
poll, and the list would flicker between two orders while nothing about it had
changed. With it, the order is a function of the data, so an unchanged list
draws identically forever — pinned by a test that sorts the same rows from
three different starting permutations and requires three identical results.

The sort lives in **`Snapshot()`**, not in the panel, so the panel,
`friends_dump` and `friends_join <n>`'s indices all address one list in one
order.

#### The selection follows the person, not the row number

The panel stores the **SteamID of the selected friend** and looks its index up
each frame; `ActivateRow()` records a SteamID, and the list's getter answers
with wherever that person is now.

`Why that is not a detail:` the list sorts itself and the poller replaces it
every three seconds. The moment one friend leaves their lobby they drop out and
every row below them moves. With an index-based selection the outline would
land on whoever slid into that slot — under a user who had not touched anything
— and the **Join** verb would be aimed at them. That is the classic bug in a
list that reorders itself, and storing the person instead of the row number is
what makes it unrepresentable. A selection that genuinely stops existing reads
as *nothing selected*, which is a different and honest state.

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
| no Steam app id (console only — the area is hidden) | *this isn't a Steam game, so there's nobody here to join.* |
| nobody else in this game | *nobody else is in this game right now.* |
| friends here, none joinable | *3 friends in this game, 0 you can join.* |
| before the first poll finishes | *Asking Steam…* |

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

**There is no confirmation dialog, and there is nothing left for one to ask.**
It existed for the "this friend is in a different game, so Steam closes yours
and starts theirs" case; that row cannot exist now. Joining somebody in the
game you are already in relaunches nothing — the running game receives the join
through its own `GameLobbyJoinRequested` callback and moves you — so a dialog
would be a speed bump on the only path there is.

### A click only records; `Tick()` acts

The list's setter is reachable two ways: a click or Enter on the draw thread,
and `overlay_e2_set friends.list <n>` on the **console** thread. Forking a
process is illegal on the latter. So the setter only records a pending join,
and `PanelFriends_Tick()` — called once per frame from the shell's own
`Draw()` — is the single place a join is ever fired.

The pending join stores the **SteamID it was aimed at**, not just the index.
The poller can replace the list between the click and the tick, and joining
whoever happens to land on index 2 afterwards is exactly the bug an index-only
handoff produces. A row that is gone by the time the tick runs toasts *"That
lobby is gone."*

---

## Nothing leaves this machine

The 2026-09-09 narrowing deleted the game-name lookup — the only outbound
network request this compositor ever made — because every row is now the game
the user is already in, so there was nothing left to look a name up *for*.

Gone with it: `src/SteamAppNames.h` in full, the `curl` invocation and its
argv builder, the `ICommunityService/GetApps` endpoint, the on-disk cache under
`$XDG_CACHE_HOME/gamescope-ritz/appnames.json`, the five-minute failure
backoff, the local `appmanifest_<id>.acf` and `libraryfolders.vdf` readers, and
the **`overlay.friends_lookup_names`** setting with its row in this area.

So the plain statement, which is worth having: **gamescope-ritz opens no
socket. There is no code path in the compositor that makes an HTTP request, and
no setting that could enable one.** The only processes it ever spawns are
`steam <url>` for a join and the game itself.

A stale `"friends_lookup_names": true` left in somebody's `global.json` from a
build before this one is **ignored**: `ConfigManager.cpp` reads named keys and
never rejects a file for carrying one it does not know, so the config loads
unchanged and the key is dropped the next time that file is rewritten. This is
verified rather than assumed — see the capture set's `stale-key.txt`.

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

> **Updated 2026-09-09.** The whole `SteamFriends018` vtable has since been
> named outright — see
> [`../planning/steam-invite-and-vtable-layout.md`](../planning/steam-invite-and-vtable-layout.md).
> `GetFriendRichPresence` is **slot 43**, not 44, and slot 42 really is
> `ClearRichPresence`, so the instinct above was right about the neighbourhood
> and one out on the arithmetic. It stays rejected on point 3's *first* ground
> alone: it answers a question about **them**, and an invite is a fact about
> **us**. The same page shows `SetListenForFriendsMessages` (slot 62) and
> `GetFriendMessage` (slot 64) are now reachable too — and that the answer in
> this section is still no, because an invite is a chat *message* rather than a
> queryable pending list, because Accepting one is the `steam://joinlobby` this
> panel's `[Join]` row already fires, and because listening would put every
> private friend message through the compositor to catch it.

**4. Not tested with a real invite, and that is stated rather than hidden.**
Testing one means asking a friend to send it, and asking them means sending a
Steam message — which is an `ISteamFriends` **write** at an unmeasured slot,
the same wall as point 3. So the finding rests on points 1–3, not on having
watched an invite arrive and be missed.

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
  written by one while the other read it. Copying a few small rows a handful of
  times a frame is cheaper than that race.

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
day, because it was a browser that explicitly *could not join anybody* — the
one thing the user actually wanted — so it was holding the binding for the
feature that could not deliver it. It was removed entirely on 2026-09-09 once
this list proved it could join for real; see
[History: the browser companion it replaced](#history-the-browser-companion-it-replaced)
below.

The binding is a **toggle on its own area**: pressed again while the friends
area is the one on screen, it closes the overlay rather than re-selecting what
is already selected and leaving no way back out on the same key. That needs
`ui::shell::RequestArea()` (an atomic consumed by `Draw()` on the thread that
owns the selection) and `AreaActive()` (republished every frame) — deliberately
not `overlay_e2_select`, which assigns the selection straight from the console
thread and is fine only for a debug surface driven by a script.

**The key path makes no Steam call of any kind.** It sets one atomic; the first
Steam call of the whole feature still happens later, on the poller's thread.

### What it does when the area is hidden

**Nothing on screen, and one `friends` log line.** `wlserver.cpp` checks
`steamfriends::SessionAppId()` before it touches anything and returns —
*before* `SetVisible()` and *before* `RequestArea()` — so the shell's state is
bit-for-bit what it was: a closed shell stays closed, an open shell stays on
whatever page it was on. There is no half-open state because there is no state
change at all. `Shell.cpp` also declines a request for an unavailable area, so
even a `RequestArea("system.friends")` from somewhere else cannot land the user
on a hidden page (or, worse, silently redirect them to Display, which is what
`SelectedArea()`'s fallback would otherwise do).

`Why silence and not a toast,` because the alternative was seriously
considered:

- **The feature is comprehensively absent, not merely unavailable.** There is
  no rail entry, no palette row and no sheet. A key that opens something which
  is not in the UI at all doing nothing is *consistent* with the rest of the
  UI, not a contradiction of it.
- **A toast would have to be fired from the wrong thread.**
  `Notifications::Show()` documents itself as steamcompmgr-thread-only and
  keeps no lock; its `std::deque` is drained by `AddLayer()` on the render
  thread. The keybind runs on the **wlserver** thread. Buying a data race in
  the compositor to explain a hidden feature is a bad trade, and the plumbing
  that would avoid the race (a third atomic consumed inside `paint_all()`) is
  more machinery than the message is worth.
- **It is not silent to somebody who looks.** The chord's own Help line under
  **Setup > Keybinds** says it only works in a Steam game, and the press is
  recorded once in the `friends` log scope.

---

## Privacy

Names appear on screen because that **is** the feature. They stay out of
everything else:

- **Nothing in `src/SteamFriends.cpp` or `src/Overlay/PanelFriends.cpp` logs,
  toasts or writes a persona name, a SteamID or a lobby id.** The log lines
  carry counts, indices, app ids and interface version strings.
- `friends_dump` prints **counts and app ids by index**. `friends_join <n>`
  takes that index, so a user never has to see or retype an id to use the
  feature from the console.
- The live probe prints range **verdicts** about ids, never the ids.
- The verification harness asserts it: no fake persona name and no SteamID64
  appears in any gamescope log it produced.
- **Nothing leaves the machine at all** — see
  [Nothing leaves this machine](#nothing-leaves-this-machine). The one outbound
  request this fork ever had was deleted on 2026-09-09.

**One documented exception.** `overlay_e2_get friends.list` is the registry's
generic "what is this row's value" debug command, and a list row's value is its
selected item's label — which here is a person's name. That is a debug command
you run yourself, echoing something already on your screen; the product never
performs it on its own. It is named here so nobody is surprised to find a name
in a log they made that way.

---

## History: the browser companion it replaced

For part of 2026-09-08 this fork also shipped a Steam chat companion: a
browser window on gamescope's own Xwayland, pointed at Steam's web chat and
promoted to a fullscreen overlay the same way this list's own window is. It
could chat but, unlike this list, could never join a friend — the web client
has no join button. Once this list proved it could actually join somebody, the
companion was earning nothing a browser tab on the host couldn't already do,
and it was removed on 2026-09-09 (see `CHANGELOG.md`). Its measurements and
the reasoning behind the design live on in
[`../planning/steam-friends-join.md`](../planning/steam-friends-join.md) and
[`../planning/steam-friends-window.md`](../planning/steam-friends-window.md).

Steam's own overlay would still beat this list at voice, invites and
notifications; it is off for these games only because Ritz's
`clear_ld_preload` module empties `LD_PRELOAD`. That trade has not changed.

---

## Failure modes, and what each one does

Every one of these is a `nullptr` or a zero on a path that already has to
handle it. Nothing here throws, blocks or is fatal.

| situation | behaviour |
|---|---|
| not a Steam game (no app id) | The area is not offered at all, and the keybind does nothing. `friends_dump` on the console says so in one sentence. |
| Steam not installed, or somewhere we do not look | `dlopen` fails once, at first use. One log line, and the list says so. |
| a library loaded that is not Steam's | Six fingerprint symbols are checked before anything is cast to a vtable. One log line. |
| Steam not running | `Steam_CreateSteamPipe` returns 0. |
| signed out | `Steam_ConnectToGlobalUser` returns 0. |
| a client whose interface versions we do not know | The version lists in `SteamFriendsCmd.h` are **closed**: an unknown client disables the feature rather than guessing a layout. |
| a client whose vtable moved again | `Snapshot()` range-checks the first SteamID it gets back. A number that is not a SteamID abandons the whole read with a named line, rather than handing `GetFriendGamePlayed` a garbage id. |
| `steam` not on PATH | The join refuses with one sentence. |
| a stale `friends_lookup_names` in `global.json` | Ignored; the config loads unchanged. |

Each failure logs its reason **exactly once** per distinct reason, and re-logs
if the situation changes (Steam started, then signed out) — the panel polls
every few seconds, and a line per poll is a log nobody can read.
