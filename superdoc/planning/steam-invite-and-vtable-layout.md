# Inviting a friend, and the vtable layout that decides whether we can

**2026-09-09, research only. Nothing under `src/` was changed.**
Sibling of [`steam-friends-join.md`](steam-friends-join.md), whose
[§6e](steam-friends-join.md#6e-correction-the-published-vtable-order-is-wrong-by-one-slot)
this page **refines and partly corrects**. Read this before acting on §6e's
"one slot earlier" rule.

Asked: *"Can we add an option, to invite someone to the game?"* — the outbound
direction, as opposed to the join that already ships. Asked alongside it:
*"Is there a detection for invites?"* — the inbound direction, which
[`../features/steam-friends.md`](../features/steam-friends.md)'s
*Received invites* section had already answered no.

Probes, scripts and raw output:
`build-release/verify-shots/steam-invite-2026-09-09/`.

---

## The one-line answer

**The slot problem is solved, and it was solved by measurement rather than by
inference.** `ISteamFriends::InviteUserToGame` is **slot 47** of
`SteamFriends018`, established four independent ways without calling anything,
and a compositor launched by Steam already carries app id 730 on its Steam pipe.
**One read, which only the user can take, stands between this and a working
Invite verb**: whether `GetFriendGamePlayed(self)` hands back our own lobby id
while a game is running. The **receive** direction remains a no, and now for a
sharper reason than before.

---

## 1. The mechanism

```
bool ISteamFriends::InviteUserToGame( CSteamID steamIDFriend,
                                      const char *pchConnectString );
```

It is the right call. Steam shows the invitee *"X invited you to play Y"*; on
accept, if their game is running they get `GameRichPresenceJoinRequested_t`
carrying `pchConnectString`, and if it is not, the string is appended to the
launch command line.

The two things that decide whether it is *useful* are the app it names and the
string it carries. Both are answered below ([§4](#4-the-app-id-and-a-correction-to-route-bs-headline-claim),
[§5](#5-the-connect-string)).

**The alternative, `ISteamMatchmaking::InviteUserToLobby( lobby, invitee )`
(`SteamMatchMaking009`, slot 16), is worse for us** and was not pursued: it
requires being a member of the lobby, and our pipe is not in one — the game's
own process is. It is recorded here only so the next person does not re-derive
it.

---

## 2. The slot problem, and how it was closed

§6e established, by calling slots against the live client, that `GetFriendCount`
is slot 2 rather than the published 3. It could not establish *why*, so it could
only offer a **uniform −1 shift** as a guess, and correctly forbade acting on
that guess for a write. Four sources now name every slot outright.

### Source 1 — Valve's own Proton bridge

Proton's `lsteamclient.so` is Valve-authored glue that calls **the same Linux
`steamclient.so` interfaces this fork binds to**. Every method gets a wrapper
symbol `ISteamFriends_SteamFriends018_<Method>`, whose body is literally

```
mov (%rdi),%rax          ; the interface's vtable
jmp/call *0xNN(%rax)     ; the slot, in bytes
```

so `0xNN / 8` **is** the slot index, stated by Valve's compiler for that exact
version string. `extract-vtable-slots.py` disassembles every wrapper and prints
the map (`slots-SteamFriends018-proton10.txt`).

Seven independent builds of that bridge agree exactly
(`cross-build-agreement.txt`):

| build | methods | GetPersonaName | GetPersonaState | GetFriendCount | GetFriendByIndex | GetFriendGamePlayed | **InviteUserToGame** | SetListenForFriendsMessages |
|---|---|---|---|---|---|---|---|---|
| Proton 9.0 (Beta) | 78 | 0 | 1 | 2 | 3 | 7 | **47** | 62 |
| Proton 10.0 | 78 | 0 | 1 | 2 | 3 | 7 | **47** | 62 |
| Proton 11.0 | 78 | 0 | 1 | 2 | 3 | 7 | **47** | 62 |
| Proton - Experimental | 78 | 0 | 1 | 2 | 3 | 7 | **47** | 62 |
| Proton Hotfix | 78 | 0 | 1 | 2 | 3 | 7 | **47** | 62 |
| GE-Proton10-28 | 78 | 0 | 1 | 2 | 3 | 7 | **47** | 62 |
| proton-cachyos-11.0 | 78 | 0 | 1 | 2 | 3 | 7 | **47** | 62 |

(Proton 7.0 and GE-Proton9-20 are blank because they **predate
`SteamFriends018`** — they stop at 017. That is a fact about their age, not a
disagreement.)

### Source 2 — the SDK redistributable inside the user's own CS2

`…/Counter-Strike Global Offensive/game/bin/linuxsteamrt64/libsteam_api.so`
binds `SteamFriends018` and exports **78** flat `SteamAPI_ISteamFriends_*`
wrappers, each a one-instruction tail jump through the vtable. Extracted with
`extract-flat-slots.py` (`slots-flat-cs2-libsteam_api.txt`):

```
SteamAPI_ISteamFriends_InviteUserToGame:
    mov    (%rdi),%rax
    jmp    *0x178(%rax)        ; 0x178 / 8 = slot 47
```

**Every one of the 69 slots this file could decode is identical to Proton's
map.** (The other nine return a `CSteamID` or a struct by value and use a
prologue plus a `call`, which the decoder skips; they are not in dispute.)
Note also: **there is no `SteamAPI_ISteamFriends_SetPersonaName` at all.**

### Source 3 — the live client's vtable *lengths*

`vtable_read_probe.cpp` reads the vtable pointer arrays out of the running
client's memory and walks each until a pointer stops being a function inside
`steamclient.so`. **It calls no slot.** Every address is checked against
`/proc/self/maps` first, so it cannot fault. `results-version-lengths.txt`:

| version | live vtable | Proton's method count | agree |
|---|---|---|---|
| SteamFriends013 | 63 | 63 | yes |
| SteamFriends014 | 64 | 64 | yes |
| SteamFriends015 | 73 | 72 | off by one — see below |
| SteamFriends016 | 73 | (not in the bridge) | — |
| **SteamFriends017** | **80** | **80** | **yes** |
| **SteamFriends018** | **78** | **78** | **yes** |

`Why the walk can be one long:` the word after a vtable is sometimes another
function pointer in the same library, and nothing in the bytes distinguishes
"one past the end" from "one more slot". That makes a length a *corroboration*,
not a proof — which is why source 4 exists.

### Source 4 — the live client's own thunks, cross-aligned between two versions

Every entry in the client's `SteamFriends0NN` vtable is a forwarding thunk onto
**one shared implementation object**:

```
mov 0x8(%rdi),%rdi          ; hop to the inner implementation object
[ small argument fixups ]
mov (%rdi),%rax             ; its vtable
jmp *0xNN(%rax)             ; slot NN/8 of the INNER vtable
```

The inner slot number is therefore a **version-independent identity** for a
method. `decode-thunks.py` reads those bytes for all 78 slots of
`SteamFriends018` and all 80 of `SteamFriends017`, aligns the two tables **by
inner slot**, and then asks Proton what it calls each side:

```
RESULT cross-version name agreement (018 vs 017, aligned by inner slot):
  78 slots agree, 0 disagree, 0 thunks undecodable
```

and, for the slot this whole page is about, the two thunks are byte-identical
apart from a RIP-relative displacement:

```
018 slot 47  +0x0124c420  48 8b 7f 08 49 89 f1 48 89 d1 48 8b 07 4c 8b 80 b8 06 00 00 …
017 slot 49  +0x0124b940  48 8b 7f 08 49 89 f1 48 89 d1 48 8b 07 4c 8b 80 b8 06 00 00 …
                                                          ^^^^^^^^^^^^^^^^^^^ 0x6b8/8 = inner slot 215
```

Proton independently names 018's slot 47 and 017's slot 49 both
`InviteUserToGame`. **That is the identification closed end to end, from the
running client's own bytes, with nothing called.**
(`results-thunk-identity.txt`, `thunk-bytes-rich-presence-and-invite.txt`.)

---

## 3. The correction to §6e: the shift is **not** uniform

§6e's five live-called anchors were all right. Its *explanation* was not, and the
difference matters for exactly the case it was warning about.

**`SteamFriends018` is `SteamFriends017` with two methods removed**, not with
everything moved down one:

| removed in 018 | its 017 slot |
|---|---|
| `SetPersonaName` | 1 |
| `GetUserRestrictions` | 42 |

So, measured over all 78 slots (`018 slot − 017 slot`):

| shift | how many slots |
|---|---|
| 0 | 1 (`GetPersonaName`) |
| **−1** | 40 |
| **−2** | 37 |

**Applying §6e's −1 uniformly to `InviteUserToGame` gives slot 48, which is
`GetCoplayFriendCount` — the wrong function.** The doc's caution was justified
by its own arithmetic, and the answer to the brief's question *"does the uniform
one-slot shift hold across the whole vtable?"* is a flat **no: it holds for 40
slots and then becomes −2 for the remaining 37.** A derived index was never
defensible; a named one is.

Two further consequences worth carrying:

- **Slot 1 is `GetPersonaState`, a read — not a setter.**
  `src/SteamFriends.cpp` calls it `_Slot1_MAY_BE_A_SETTER` and forbids touching
  it. That caution cost nothing and can stay, but the comment now has an answer:
  `SetPersonaName` does not exist in `SteamFriends018` at all, which is the
  whole reason §6e saw a shift.

- **There is a latent bug in `kFriendsVersions`, and the existing tripwire
  catches it.** `SteamFriendsCmd.h` lists `018, 017, 016` and
  `src/SteamFriends.cpp` declares **018's** layout for all three. On a client
  that offered only 017, slot 6 would be `GetFriendPersonaState` — an `int`
  returned into a `const char *` and then dereferenced, i.e. a segfault in the
  compositor. It cannot get that far: `Snapshot()` calls slot 3 first, which
  under 017 is `GetFriendCount`, whose small integer fails
  `LooksLikeIndividualSteamId()` and abandons the read with `Reason::WrongShape`.
  So the shipping code is safe **because of the tripwire, not because the version
  list is right**. If anyone ever removes that check, remove 017 and 016 from the
  list in the same commit — or give them their own layout, which is now cheap to
  write down.

---

## 4. The app id, and a correction to route B's headline claim

`ISteamFriends::InviteUserToGame` names **the caller's app**. Route B was chosen
precisely because it supplies no app id, so the obvious expectation was that an
invite from this compositor would name nothing and do nothing.

**Measured, and it is the opposite of what the docs claim.**
`ISteamUtils::GetAppID()` (slot 9 — same four sources, neighbours
`GetCurrentBatteryPower` and `SetOverlayNotificationPosition`, both harmless),
called on a pipe made exactly the way `src/SteamFriends.cpp` makes one
(`results-invite-probe-readonly.txt`):

| run | environment | `GetAppID()` |
|---|---|---|
| 1 | a plain shell | **0** |
| 2 | `SteamAppId=730 SteamGameId=730 STEAM_COMPAT_APP_ID=730` | **730** |
| 3 | a plain shell again | **0** |

**`steamclient.so` reads `SteamAppId` out of the process environment when the
pipe is created.** And gamescope-ritz is launched *by Steam*, with exactly that
variable set — [`steam-friends-join.md` §6b](steam-friends-join.md#6b-the-app-id-route-if-route-b-is-ever-rejected)
measured it live on the user's own session: `SteamAppId=730`, `SteamGameId=730`,
`STEAM_COMPAT_APP_ID=730`.

So:

- **The claim "no app id anywhere" in
  [§6a](steam-friends-join.md#6a-two-routes-and-the-safer-one-needs-no-app-id-at-all)
  and in `features/steam-friends.md` is true of the *code* and false of the
  *process*.** The running compositor's Steam pipe already reports app 730. It
  should be reworded rather than deleted: what route B actually buys is that
  **we never call `SteamAPI_Init`** — no callback registration, no stats
  context, no second initialisation for an app id already in use — and the app
  id the pipe does carry is the app the user is genuinely playing, arriving from
  the environment Steam itself set. Nothing registers a *play session*; that is
  the reaper's job, not a pipe's.
- **For the invite, this is the good news.** An invite sent from inside
  gamescope-ritz would name app 730 — the game the user is in — with no change to
  the design and no `SteamAPI_Init`.

`Why this was worth measuring rather than reasoning about:` the reasoning would
have concluded "app 0, therefore the feature is impossible", and that would have
been wrong.

---

## 5. The connect string

**For CS2 the join unit is a lobby, and the connect string Steam itself uses is
`+connect_lobby <lobby id>`.** Two pieces of evidence, both from files:

- `steamui.so`'s URL-command table has `+connect_lobby %llu` as the literal
  **immediately after** `joinlobby` — the argument Steam appends when it acts on
  a join (already recorded in
  [§5](steam-friends-join.md#5-the-approach-that-actually-wins-a-native-join-list)).
- CS2's own `libclient.so` contains `connect_lobby`, `steam_display`,
  `steam_player_group`, `steam_player_group_size`, `game:state`,
  `CMsgGCHInviteUserToLobby`, `k_EGCMsgInviteUserToLobby` and
  `CCallbackInternal_OnGameRichPresenceJoinRequested`. It binds
  `SteamFriends017`, `SteamMatchMaking009`, `SteamUser023`, `SteamUtils010`.
  (`cs2-connect-string-evidence.txt`, `…-2.txt`.)

So CS2 both **publishes** a lobby and **handles** a rich-presence join request.
An invite carrying `+connect_lobby <our lobby id>` is the correct outbound
analogue of the `steam://joinlobby` the panel already fires inbound.

**The one thing missing is our own lobby id, and reading it is the open
measurement.** `GetFriendGamePlayed( ourOwnSteamID, &info )` is a read at slot 7
— the slot the shipping code already calls — and it returns the same struct for
ourselves that it returns for a friend. Run with nothing playing it answers
honestly:

```
RESULT GetFriendGamePlayed(self): false, app 0, CGameID type 0, own lobby id zero
```

which is correct (nothing was running) and settles nothing. **Run while CS2 is
up it either hands back app 730 and a non-zero lobby — in which case the invite
is buildable today — or it returns false for ourselves, in which case there is
no route to our own lobby id and an invite can only name the game, not the
lobby.** That is [§8](#8-what-only-the-user-can-run)'s first item.

`Reading a friend's "connect" rich-presence key was considered and is not
needed:` `GetFriendRichPresence` is slot 43 (measured, inner slot 93), so it is
now reachable — but it answers a question about *them*, and an invite needs a
string about *us*. `features/steam-friends.md`'s *Received invites* §3 rejected
that call on the grounds that its neighbours might be `ClearRichPresence`; the
measurement now says slot 42 **is** `ClearRichPresence` and slot 43 **is**
`GetFriendRichPresence`, so the reasoning was right about the neighbourhood and
the arithmetic was one out. It stays rejected, on the "wrong question" ground
alone.

---

## 6. Is there a non-vtable route? No.

The join direction had one (`steam://joinlobby`), so this was looked for hard.

| candidate | verdict |
|---|---|
| a `steam://…invite…` URL | **Does not exist.** `steam://[a-z/]*invite` matches **zero** strings in `steamui.so`, `ubuntu12_32/steam`, `ubuntu12_64/steamwebhelper` **and** `linux64/steamclient.so`. The URL-command table around `joinlobby` was dumped in full (`joinlobby`, `+connect_lobby`, `remoteplay`, `connect`, `takesurvey`, `startvrdashboard`, `controllerconfig`, `setcustomartwork`, `friends`, `bigpicture`, `settings`, `openurl_external`, `AddNonSteamGame`, the `devkit-1` family …) and carries no invite verb. The `Invite*` strings that *do* exist are chat-room-group, family-group and broadcast protobufs — not game invites. |
| a flat exported C function | **None.** `nm -D` on `steamclient.so` gives 1076 defined dynamic symbols; the `Steam_*` family is the 35 pipe/user/callback entry points the fork already knows, and **no `SteamAPI_ISteamFriends_*` is exported**. The 562 mangled C++ exports are all `pcrecpp`, `tinyxml2` and `protobuf`. |
| symbols or debug info naming the methods | **None reachable.** The library is "not stripped" only in the sense that it keeps a `.symtab` of 1553 entries, all of them C library imports and third-party code. Its `.gnu_debuglink` names `steamclient.so.dbg`, **which Valve does not ship** — it is not on this machine and not in the depot. |
| `ActivateGameOverlayInviteDialog` (slot 32) | Would open Steam's own invite dialog — but it is still a vtable call, and this fork's users run with `LD_PRELOAD` emptied by Ritz's `clear_ld_preload` module, so **there is no overlay for it to open**. |

**So every route to an invite is a vtable call.** That is why §2 had to be done
properly rather than worked around.

---

## 7. The receive direction — *"is there a detection for invites?"*

The earlier answer (`features/steam-friends.md`, *Received invites*) was: no,
because the only invite-shaped surfaces are three callbacks that fire *after*
acceptance, and the one that could carry a pending invite —
`GameConnectedFriendChatMsg_t` (343) — needs `SetListenForFriendsMessages`, a
**write at an unmeasured slot**. A 300 s pump on a no-app-id pipe saw 16 512
callbacks, including 304 and 336 at their exact published payload sizes, and
**zero** of 333 / 337 / 343.

**What changes: the slot wall is gone.** `SetListenForFriendsMessages` is slot
**62** and `GetFriendMessage` is slot **64** (`ReplyToFriendMessage` 63), each
established by the same four sources and each confirmed by the live thunk
alignment (inner slots 79 and 81). Anything blocked on *"we cannot safely reach
that method"* is now unblocked.

**What does not change: the answer.** Three things stand in the way, and none of
them is about slots.

1. **No route observes an invite without a write.** A directed invite adds
   nothing readable. It does not alter the sender's rich presence — they were
   already advertising a joinable lobby *before* they invited anyone, which is
   the state `m_steamIDLobby` already reports and the panel already draws as
   `[Join]`. `FriendRichPresenceUpdate_t` (336) does arrive on our pipe, so if
   an invite touched rich presence we would already see it; the 300 s pump says
   it does not. **Checked, and negative.**
2. **Even with listening on, an invite is a message, not a queryable pending
   list.** It arrives as a chat entry of type `k_EChatEntryTypeInviteGame` and
   is read out of a callback. There is no *"what am I holding unanswered"* call
   at any slot in the interface — the full 78-method map in
   `slots-SteamFriends018-proton10.txt` is now on the record and contains no
   such method. So **Deny could only hide a row**, and a restart would lose the
   state entirely.
3. **The row would add almost nothing.** Accepting a game invite *is*
   `steam://joinlobby` — which is exactly what the panel's existing `[Join]` row
   already does for that same friend. An invite row would be a second, more
   fragile path to a button that is already on screen.

And it would cost something real: `SetListenForFriendsMessages(true)` makes
Steam deliver **every friend chat message** to our pipe, so the compositor would
start receiving the user's private conversations in order to notice the one
message in a thousand that is an invite. That is a privacy escalation this
feature's own Privacy section would have to argue for, and it cannot be argued
for by a row that duplicates an existing one.

**Verdict on receive: still no, and now for product reasons rather than
technical ones.** The panel keeps no invite row. What would change it is a
Steamworks call that enumerates pending invites, and the complete interface map
now proves there is none.

**Do send and receive stand or fall together?** No, and it is worth saying
plainly: they shared *one* blocker (an unmeasured slot) and that blocker is gone
for both. Past it they diverge — **send** has a real remaining question with a
real answer coming ([§8](#8-what-only-the-user-can-run)), and **receive** has a
settled negative. Fixing one does not fix the other.

---

## 8. What only the user can run

Everything below is safe **out of a match**, prints no ids and no names, and is
one command each.

**1. The measurement the Invite verb is waiting on. Run it while CS2 (or Rust)
is actually running.**

```
build-release/verify-shots/steam-invite-2026-09-09/invite_probe
```

Read the last three lines. Expect the interlock to say `PASS`, and then:

- `GetFriendGamePlayed(self): true, app 730, …, own lobby id PRESENT and in the
  chat band` → **the invite is buildable**; §9's sketch is correct as written.
- `… false …` → the client does not report *us* through the friends interface,
  and an invite can name the game but not the lobby. Say so and §9 shrinks to a
  plain "invite to this game" with no lobby.
- `GetAppID() on our pipe: 730` → confirms §4 in the real launch environment
  (the probe run by hand from a shell will say 0; that is expected and correct).

**2. The one thing that cannot be established without sending an invite.** This
was **not run** — the session's permission system refused the call, which is the
right default for a write that reaches another person's Steam client, and it was
not worked around. `Blendgranate` **was** re-verified as present in the current
friends list first (`atze` is **not** in it any more), so the target is
available whenever the user wants this run:

```
build-release/verify-shots/steam-invite-2026-09-09/invite_probe \
    --invite Blendgranate --connect '+connect_lobby <your lobby id from step 1>'
```

It refuses unless the layout interlock passes **and** the persona resolves to
exactly **one** friend, and it calls slot 47 exactly once. Read
`RESULT InviteUserToGame returned:` — `TRUE` means the client accepted it, and
Blendgranate's Steam should show an invite to CS2.

`Why the risk of that one call is small, stated so it can be disagreed with:`
the slot is named by four independent sources with 78/78 internal agreement, the
probe re-derives it from the live process's own bytes before calling, and slot
47's **neighbours are harmless** — 46 is `RequestFriendRichPresence` and 48 is
`GetCoplayFriendCount`, both reads. That is a materially different neighbourhood
from slot 43's, whose neighbours 41 and 42 are `SetRichPresence` and
`ClearRichPresence`.

**3. Still open from the join work, unchanged:** a friend actually in a joinable
lobby, for `steam_friends_live_probe --diagnose`. See
[`steam-friends-join.md` §8](steam-friends-join.md#8-steps-only-the-user-can-run).

---

## 9. If step 1 comes back positive — the implementation sketch

Two commits, no new files beyond a test, and nothing on the frame path.

**Commit 1 — the call, in `src/SteamFriends.{h,cpp}` and `SteamFriendsCmd.h`.**

- Extend `SteamFriendsVTable` from 8 entries to 48. Slots 8..46 stay **opaque
  `void *`**, named after what the map says they are, so the compiler cannot
  call them by accident — the same discipline slot 1 already gets. Only slot 47
  becomes a function pointer:
  `bool ( *InviteUserToGame )( ISteamFriends *, uint64_t, const char * );`
  With a comment pointing here, because a 48-entry hand-declared vtable is
  exactly the thing the next reader will distrust, and the four sources are the
  answer.
- **Keep the length interlock, in the shipping code.** Before the first invite,
  walk the vtable the way `vtable_read_probe` does and require **78**. A client
  whose `SteamFriends018` is not 78 long disables the Invite verb with one named
  log line and leaves Join working. This is cheap, it is the check that would
  have caught §6e a day earlier, and it is the difference between "we measured
  it once" and "we check it every time".
- `std::optional<uint64_t> OwnLobbyId()` — `GetFriendGamePlayed(self)` at the
  existing slot 7, with the own SteamID read from
  `~/.steam/steam/config/loginusers.vdf` (newest `Timestamp`; this client writes
  no `MostRecent` key, which the probe found the hard way). No new interface, no
  new slot.
- `bool Invite( const Friend &, std::string *psError )`, on the poller thread,
  never from a paint. `BuildConnectString( uint64_t lobbyId )` goes in
  `SteamFriendsCmd.h` beside `BuildJoinUrl()` and is **`std::to_string` over an
  integer**, so a persona name has no path into it — the same property, pinned
  the same way, by the same test file.
- Refuse when: the interlock fails, our own lobby id is zero, the target has no
  SteamID, or the target's app id differs from ours (inviting somebody to a game
  you are not in is not a thing this button should do).

**Commit 2 — the verb.** An **Invite** button in `PanelFriends.cpp`, beside
Join. Same shape as Join: the click only records a pending action and
`PanelFriends_Tick()` performs it, because the setter is reachable from the
console thread too. Dimmed with a reason when we have no lobby of our own
(*"you're not in a lobby to invite anyone to"*), toasts *"Invited <them>."* on
success. The Status row gains one sentence about invites, replacing the current
one that says they are impossible.

**Deliberately not in the sketch:** no `SteamAPI_Init`, no callback pump, no
`SetListenForFriendsMessages`, no invite row, no Accept/Deny — see
[§7](#7-the-receive-direction--is-there-a-detection-for-invites).

---

## Reproducing everything on this page

`build-release/verify-shots/steam-invite-2026-09-09/`. That directory lives
under the build tree and a clean rebuild wipes it; the two probe sources and the
three scripts are the parts worth keeping.

| file | what it establishes |
|---|---|
| `extract-vtable-slots.py` → `slots-SteamFriends0{15,17,18}-proton10.txt`, `slots-SteamMatchMaking009-…`, `slots-SteamUtils010-…` | §2 source 1: the full slot map, out of Valve's own Proton bridge, by disassembly |
| `cross-build.sh` → `cross-build-agreement.txt` | §2 source 1: seven independent Proton builds agreeing |
| `extract-flat-slots.py` → `slots-flat-cs2-libsteam_api.txt` | §2 source 2: CS2's own shipped Valve SDK, 69/69 identical |
| `vtable_read_probe.cpp` → `results-vtable-read.txt`, `slot-offsets-018-vs-017.txt`, `slot-offsets-017-vs-018.txt` | §2 source 3: the live vtables, read out of memory with **no slot called** |
| `version-lengths.sh` → `results-version-lengths.txt` | §2 source 3: live length vs Proton's count, six versions |
| `decode-thunks.py` → `results-thunk-identity.txt`, `thunk-bytes-rich-presence-and-invite.txt` | §2 source 4: 78/78 cross-version agreement from the client's own thunk bytes |
| `invite_probe.cpp` → `results-invite-probe-readonly.txt` | §4 and §5: the in-process interlock, `GetAppID()` 0-vs-730, and `GetFriendGamePlayed(self)`. Its `--invite` path is §8 step 2 and **was not run** |
| `cs2-connect-string-evidence.txt`, `…-2.txt` | §5: what CS2 publishes and which interfaces it binds |
