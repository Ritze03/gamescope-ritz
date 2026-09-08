# Steam chat companion overlay

**2026-09-08.** `Ctrl+Shift+C` opens Steam's web chat over the game, in a
browser gamescope launches on its **own** Xwayland and then promotes to a
fullscreen, clickable, typeable overlay. Press it again and it goes away and the
game has the keyboard back.

> **The chord moved, later the same day.** This used to be `Ctrl+Shift+Tab`.
> That is Steam's own friends-list muscle memory, and this fork now *has* a
> friends list you can actually join people from —
> **[steam-friends.md](steam-friends.md)** — so the chord went to the feature
> that can use it. This one is `Ctrl+Shift+C`, for Chat. Both are editable
> under **Setup > Keybinds**; swapping them back means moving `friends` off
> `Ctrl+Shift+Tab` first, because the conflict rule refuses two actions on one
> chord.

> **If what you want is to JOIN a friend, this is the wrong page.** The web
> chat client has no join button and never will — see
> [steam-friends.md](steam-friends.md), which reads the running Steam client
> directly and can. This page is for *talking*.

Code: `src/SteamCompanion.{h,cpp}` (the runtime),
`src/SteamCompanionCmd.h` (the pure rules), `src/Keybinds.cpp`'s `companion`
action, `src/Overlay/PanelSystem.cpp`'s `system.companion` area,
`src/steamcompmgr.cpp` (the per-frame tick, the teardown call and one guard in
`handle_desktop_window`). Tests: `tests/test_steam_companion.cpp`. Captures:
`build-release/verify-shots/steam-companion-2026-09-08/`.

---

## Read this first: what it is not

**It is not Steam's real Friends window, and it cannot be.** Steam runs on the
host session with its own X server; gamescope has a separate one. The window
cannot be moved across, and while capturing a picture of it is possible, no
protocol on this host can route a click or a keystroke back into it — so a
captured Friends list would be a photograph. That was measured, not assumed;
[`../planning/steam-friends-window.md`](../planning/steam-friends-window.md)
records every approach that was tried and what each one actually did.

So what you get is **Steam's own web chat in a browser**, which is a real,
interactive page, hosted as an overlay. The honest limitations, all of them:

| | |
|---|---|
| **A separate sign-in** | The browser profile is not the Steam client's session. You sign in once, with Steam Guard, and tick *Remember me*. Easiest done on your desktop against the same profile folder (`~/.config/gamescope-ritz/companion-browser`) before you ever open it in a game. |
| **No voice** | It is the web client. There is no voice chat in it. |
| **No game invites** | Same reason. You can talk; you cannot invite or join from here. **[The friends list](steam-friends.md) can join** — it does not go through a browser at all. |
| **No message notifications** | A new message does not raise a toast in gamescope. You find out when you open it. |
| **It is a browser** | A few hundred MB of RSS and some GPU work while your game runs, from the first press until gamescope exits. |

**Steam's own overlay would give you all three of the missing things** — voice,
invites and notifications, natively. It is switched off for the games that use
this fork only because the Ritz launcher's `clear_ld_preload` module sets
`LD_PRELOAD=""`, which drops `gameoverlayrenderer.so`. If you are willing to
stop clearing it for your gamescope titles, `Shift+Tab` is a better friends
list than this feature can be. That trade is stated here, in the feature's own
documentation, because it is the first thing a user should be told.

`Why build it anyway:` because the user asked for it having already made that
choice, and because this overlay composites *after* an external
frame-generation layer (`lsfg-vk`) rather than being interpolated with the
game's frames — the same reason this fork grew its own crosshair
([crosshair.md](crosshair.md)).

---

## How it works

### The mechanism, which is almost entirely already there

`steamcompmgr` promotes any X11 window **on its own Xwayland** to a fullscreen
interactive overlay from three properties it already reads — the mechanism
Steam uses on the Deck, not restricted to Steam:

| property | value | effect |
|---|---|---|
| `STEAM_OVERLAY` | `1` | the window becomes `ctx->focus.overlayWindow`, painted on top of the game |
| `STEAM_INPUT_FOCUS` | `1` | mouse **and** keyboard go to it |
| `_NET_WM_WINDOW_OPACITY` | `0` / `0xFFFFFFFF` | hidden / shown |

`GAMESCOPE_EXTERNAL_OVERLAY` is the paint-only sibling — `DetermineAndApplyFocus`
never routes input to it, so it is the wrong one for this.

So the whole feature is: launch a browser with `DISPLAY` set to gamescope's own
Xwayland, find its window, set those properties. No change to the compositing
pipeline, no new Wayland protocol, no capture code.

### The lifecycle

- **Launched on demand, never resident.** The first press of the chord spawns
  the browser. Later presses only move two X properties.
- **A press while it is starting means "never mind".** The plan is computed
  from *is a browser running*, not *is its window up*, so a second press during
  the wait hides it — the window comes up hidden instead of jumping into your
  face three seconds later. A third press shows it instantly.
- **Spawned at most once**, by construction rather than by a lock: `PlanToggle`
  cannot answer `Launch` while a child is alive, and a `waitpid(WNOHANG)` every
  tick is what keeps "alive" true.
- **A browser that died while hidden relaunches on the next press**, for that
  same reason.
- **It never outlives gamescope.** Three independent guarantees, because a
  browser window left behind on a display that no longer exists is the worst
  failure this feature could have: the child is put in **its own process group**
  and `steamcompmgr_exit()` `killpg`s it (SIGTERM, one second, then SIGKILL);
  it sets **`PR_SET_PDEATHSIG`**, so a gamescope that is killed outright still
  takes it down; and `main.cpp`'s existing `KillAllChildren()` covers the rest.
- **Nothing blocks the compositor.** The hotkey path bumps one atomic and
  nudges steamcompmgr. The tick does one `waitpid(WNOHANG)`, one walk of the
  window list and at most three `XChangeProperty` calls.

### The trap — the one thing this feature is really built around

An overlay left at **opacity 0 that still carries `STEAM_INPUT_FOCUS=1` is
invisible and keeps swallowing every keystroke.** The game looks focused and
answers nothing. The cause is one line in `DetermineAndApplyFocus()`:

```c++
if ( w->isOverlay && w->inputFocusMode ) inputFocus = w;
```

which is not conditional on opacity — and should not be; an overlay that wants
input while translucent is legitimate. So **hiding must clear both properties**,
and the fix lives on this side.

It is pinned three ways, deliberately at three different levels:

1. **At compile time.** `SteamCompanionCmd.h` expresses the pair as one
   function, `PropsFor(bool)`, and `static_assert`s that the hidden value has
   `uInputFocus == 0`. A build that reintroduces the bug does not compile.
2. **At test time.** `tests/test_steam_companion.cpp` asserts the same thing by
   name, so an ordinary test run says *what* is wrong, not only *where*.
3. **At run time.** `WatchForSwallowedInput()` checks, every tick, whether the
   companion window is hidden while `inputFocusMode` is still non-zero. Two
   seconds of that logs a loud named error and raises a toast — this is the
   layer that catches a regression in the *plumbing* (a path that sets the
   opacity and never flushes the focus) rather than in the intent.

### Finding the window

By **process group**, never by `WM_CLASS` or title. The child is put in its own
group, every descendant inherits it, and `steamcompmgr_win_t::pid` is the real
client pid from the XRes extension — so any window any part of the browser opens
is recognisable. `Why not the class:` a class name is a fact about one browser
(chromium derives it from the URL's host; firefox says `Navigator`), so matching
on it would quietly stop working the moment the user changed the command, which
is a setting this feature deliberately offers.

The window is then resized to the root, and `handle_desktop_window()` skips it
so its "pull a desktop window back to its requested size" rule cannot fight
that.

---

## Settings

**Settings → System → Steam chat** (`system.companion`), and the hotkey under
**Setup → Keybinds** (`companion`, default `Ctrl+Shift+C` —
[keybinds.md](keybinds.md)).

| row | key | default |
|---|---|---|
| Steam chat overlay | `overlay.companion_enabled` | on |
| Status | *(read-only)* | — |
| Browser command | `overlay.companion_command` | `chromium --ozone-platform=x11 --user-data-dir={profile} --no-first-run --no-default-browser-check --app={url}` |
| Page | `overlay.companion_url` | `https://steamcommunity.com/chat` |

All three are **global** (`global.json`'s `overlay` section), like every other
field in `OverlaySettings`. `Why:` which browser exists is a fact about the
machine, not about the game — a per-profile browser command would mean "chat
works in CS2 and does nothing in Rust" with no visible cause. The enable switch
goes with them for the reason the keybinds map is global: the chord that opens
it is one setting for the whole install, so whether it opens anything must be
too.

`Why the switch defaults to on:` the chord is bound by default and is swallowed
whatever the switch says — the hotkey layer fires before any of this is
consulted. Defaulting to off would make `Ctrl+Shift+C` a key that is taken
from the game *and* does nothing. Off still answers the press, with a toast
naming this switch and the Keybinds area, rather than silence.

### The command string

Split like a command line — whitespace separates, `'...'` is literal, `"..."`
honours `\"` and `\\`, a backslash escapes outside quotes — and handed to
`execvp` directly. **There is no shell**: no globbing, no variables, no `;` or
`|`. An unterminated quote is refused with a reason.

Two placeholders are substituted **into the already-split arguments**:

- `{url}` — the Page row. A command with no `{url}` gets the page appended as a
  final argument, so `firefox --kiosk` does the obvious thing.
- `{profile}` — `~/.config/gamescope-ritz/companion-browser`.

`Why substitution happens after splitting, always:` a URL that contains a space,
a quote or a semicolon then lands inside **one** argument and can never become a
second one. The URL row cannot add a flag to your browser, whatever you type in
it. `tests/test_steam_companion.cpp` pins that with the obvious attempts.

`Why {profile} is not decoration:` without a private `--user-data-dir`, a second
chromium hands its URL to the one already running on your desktop over its
singleton socket and exits. Nothing would ever appear inside gamescope and the
failure would look like "the hotkey does nothing". That folder is also where the
one-time Steam login is remembered.

### When the browser is missing

`argv[0]` is resolved through `PATH` **before** forking, so a missing program
becomes a toast naming what to install and where to change it — never a silent
no-op and never a crash. `Why before the fork:` `execvp` failing in the child is
a message nobody sees.

---

## First press, and the delay

A cold browser takes seconds to put a window up. The first press therefore
raises one self-dismissing toast, **"Opening Steam chat…"**; show and hide are
instant and say nothing.

`Why a toast and not nothing:` a hotkey that changes nothing on screen for three
seconds reads as a hotkey that did not work, and the natural response is to
press it again — which on a toggle means *hide*. The toast costs nothing, uses
machinery already on screen for every other confirmation in this fork, and turns
a dead wait into one the user understands.

---

## What was verified, and how

`build-release/verify-shots/steam-companion-2026-09-08/` — a private, invisible
sway hosting a real nested gamescope, every key and click driven through
gamescope's *own* seat (`wlserver_debug_key`,
`wlserver_debug_absolute_motion`, `wlserver_debug_mouse_button`), a throwaway
config directory, and the "game" a terminal writing what it receives to a file.
`verify-steam-companion.sh` re-runs the whole thing.

| shot | claim |
|---|---|
| `00-game-only` | the game alone |
| `01-overlay-shown` | the chord opens it fullscreen over the running client |
| `02-after-click` | a real click lands in the page |
| `03-typed` | typing lands in the page |
| `04-hidden` | hiding restores the game — **byte-identical to `00`** |
| `05-keys-reach-the-game` | the terminal shows `aaa` (before) and `ccc` (after hiding) and **not** `bbb` (typed while the overlay was up): the trap is handled, and the keys did not leak either way |
| `06-reshown` | re-shown with the same browser process, page state (`hellobbb`) intact |
| `07a` / `07b` | killed while hidden, relaunched cleanly on the next press |
| `08-missing-browser` | a nonexistent command produces a clear toast and spawns nothing |
| `10-real-steam-chat` | the shipped default against the real `steamcommunity.com/chat` — which is also the honest picture of the separate sign-in |

No companion process survived gamescope exiting (10 processes before, 0 after).
