# Keybinds

**2026-09-08.** This fork's own compositor hotkeys are the user's to change:
one chord per action, stored in `global.json` and edited from the settings
shell's **Keybinds** area (`setup.keybinds`). Three actions at the rework; a
fourth (`friends`) landed the same day with the friends-you-can-join list. A
fifth (`companion`), for a Steam chat overlay that landed alongside it, was
removed 2026-09-09 — see [History: `companion`](#history-companion) below.

Code: `src/Keybinds.{h,cpp}` (grammar, store, gesture engine, capture),
`src/Overlay/PanelKeybinds.cpp` (the rows), `src/wlserver.cpp`'s
`wlserver_check_ritz_keybinds()` (what an action *does*),
`src/Overlay/UI/Controls.cpp`'s `controls::Chord()` (the capture chip).
Tests: `tests/test_keybinds.cpp`. Live proof:
`build-release/verify-shots/keybinds-2026-09-08/` (33 checks on a real
compositor).

## What was bound before this, and what still is

The inventory, taken from the code rather than from memory, at the commit this
work started from (`a4ff805`):

| Chord | What it did | Where | Whose |
|---|---|---|---|
| Right Shift (tap) | toggle the settings shell | `wlserver.cpp:468` `wlserver_check_shell_shortcuts` | **this fork** (D22) |
| Left Ctrl + Right Shift | the launcher, and close it again | same function | **this fork** (D22 / D25 / issue #88) |
| Ctrl+Shift+O | toggle the settings shell | `wlserver.cpp:345` `wlserver_check_settings_overlay_toggle` | **this fork** (M2, DECISIONS #6) |
| whatever a client registered | `gamescope_action_binding` triggers (Steam's own bindings) | `wlserver.cpp:3496`, `WaylandServer/GamescopeActionBinding.h` | upstream |
| Ctrl+Alt+F1..F12 (`XF86Switch_VT_*`) | VT switch | `wlserver.cpp:1131` | upstream |
| Volume up/down, Power | routed to the root server, never to the game | `wlserver.cpp:1142` | upstream |

**Only the first three are editable.** `Why:` the other three are not this
fork's to change. The client binding table belongs to whatever registered it
(Steam), and rebinding it here would break the Steam button on a Deck with no
way for the user to connect the two facts. VT switching and the volume/power
keys are the kernel's and the session's respectively; a compositor that let a
settings screen capture them could take away the user's way out of a wedged
session.

Also **not** bound here: everything the shell does with a key *while it is
open* (Esc, `Ctrl+K` for the palette, Tab region cycling, `Ctrl+D` reset).
Those are ImGui-level, handled in `Overlay/UI/Shell.cpp` downstream of the
hotkey layer, and are keys inside an application rather than keys the
compositor takes from the game. Making them configurable is a separate,
larger job with a different risk profile.

## The actions

| id | Title | Default |
|---|---|---|
| `shell` | Open settings | `RShift` |
| `shell_alt` | Open settings (alternate) | `Ctrl+Shift+O` |
| `launcher` | Open launcher | `LCtrl+RShift` |
| `friends` | Open friends list | `Ctrl+Shift+Tab` |

`friends` was added 2026-09-08 with the join list
([steam-friends.md](steam-friends.md)).

**`Ctrl+Shift+Tab` belongs to `friends`.** That chord is Steam's own overlay
chord for the friends list, so the muscle memory is already right — and for
part of 2026-09-08 it was held by a `companion` action (a browser that
explicitly *could not* join anybody — see [History: `companion`](#history-companion)
below) before that action gave it up. Using it here is not a conflict with
Steam: gamescope swallows the key before anything downstream sees it, so
neither the game nor Steam's own overlay can be reached by it. The chord is
not reserved or special — it is a row like the other three, and rebinding it
means moving whatever else uses it first, because the conflict rule refuses
two actions on one chord.

`Why the friends binding is a toggle rather than an opener:` pressed again
while the friends area is the one on screen, it closes the overlay instead of
re-selecting what is already selected. A key that can only open would leave the
user hunting for a second key to get their game back. `wlserver.cpp` asks
`ui::shell::AreaActive( "system.friends" )` to tell the two cases apart.

`Why two shell actions rather than one action with two chords:` the settings
grammar is one row per value. A list-valued row would need a row per element
and an answer to "which of my two shell chords is this row" — a worse question
than "what is my alternate shell chord". One chord per action also makes the
conflict rule a plain comparison rather than a set intersection.

## History: `companion`

From 2026-09-08 to 2026-09-09 there was a fifth action, `companion` (Open
Steam chat, default `Ctrl+Shift+C`), for a browser window gamescope launched
on its own Xwayland pointed at Steam's web chat. It briefly held
`Ctrl+Shift+Tab` before giving that chord up to `friends` the same day, since
`friends` is the feature that can actually use Steam's own friends-list
muscle memory — the companion could talk but, unlike the friends list, could
never join anybody. It was removed entirely on 2026-09-09 once the friends
list proved it could join a friend for real; see `CHANGELOG.md`'s 2026-09-09
entry and [steam-friends.md](steam-friends.md)'s *"History: the browser
companion it replaced"*. A `global.json` left over from before the removal may
still carry `overlay.keybinds.companion`: it is simply ignored on load (the
action no longer exists to look it up) and dropped from the file the next time
settings are saved.

## The chord grammar

A chord is an unordered **set of terms** matched against the set of keysyms
currently held — the same model gamescope's own external binding table uses
(`Keybind_t`). Terms are `+`-separated; case and whitespace are free; the
stored and displayed form is canonical (modifiers first in Ctrl, Alt, Shift,
Super order, then the remaining keys).

- **Either-side modifiers**: `Ctrl`, `Alt`, `Shift`, `Super` — either hand.
- **Sided modifiers**: `LCtrl`, `RCtrl`, `LShift`, `RShift`, `LAlt`, `RAlt`,
  `LSuper`, `RSuper` — that hand only.
- **Anything else**: an xkb keysym name (`O`, `F5`, `Tab`, `Escape`), matched
  case-insensitively and normalised the way the hotkey layer normalises a real
  key event (upper-cased, `Meta`→`Super`, `ISO_Left_Tab`→`Tab`, …).

`Why both sided and either-side terms, when one kind would be simpler:` because
the three existing bindings need both, and collapsing them would silently
change two of them. `Ctrl+Shift+O` has always worked with whichever Ctrl and
whichever Shift the user reaches for; the shell's own chord is specifically the
**right** Shift and must never fire on the left one, which games bind to walk
or crouch and which is therefore held and released constantly.

A chord must match the **whole** held set, never a prefix of it. That single
rule is what stops `Ctrl+Shift+O` being eaten by the `LCtrl+RShift` its own
gesture starts with — the historical bug this system was rebuilt around.

Refused at parse time, each with a sentence the UI shows: an empty chord, a
dangling or doubled `+`, a name xkb does not know, more than five keys, and
**overlapping terms** (`Ctrl+LCtrl`) — one physical key cannot satisfy two
terms, so such a chord could never fire and accepting it would mint a dead
binding.

## Two firing rules, decided by the chord itself

Not a flag anyone sets. `Keybinds.cpp`'s `ProcessKey()`:

- A chord of **only modifiers** (`RShift`, `LCtrl+RShift`) is a **tap**. It
  fires on a **release**, and only if the largest set held during the gesture
  was exactly the chord — i.e. nothing else was pressed while it was down. It
  is **never swallowed**.

  `Why a tap:` a modifier that fires on its own press cannot be used as a
  modifier any more — Right Shift + C would open the overlay every time.
  `Why never swallowed:` swallowing a modifier's release after its press was
  delivered leaves the game holding a Shift that is physically up, which is
  worse than any binding is worth.

- **Any other chord** (`Ctrl+Shift+O`) fires on the **press** that completes it
  and **is** swallowed, so the letter never reaches the game. Its own release
  is swallowed too, tracked per key so the modifiers around it are unaffected.

### The peak set, and what it replaced

The old code armed a gesture on Right Shift's press, upgraded it when Left Ctrl
arrived, and disarmed it on any other key: three rules, each of which had to be
remembered at every new exit. That binding broke four times, and each break was
one of those rules being forgotten or being applied at a moment when the
gesture was not yet decidable.

The engine now records one thing — the **largest held set seen since this
gesture began** — and says the same thing structurally:

- `Ctrl+Shift+O` peaks at three keys, so no two-key tap can match it: the tap
  cannot eat the longer gesture, and nothing has to be disarmed by hand;
- a tap fires at most once per gesture, because firing clears the peak;
- the peak clears when the last key comes up.

`Why this is worth the rewrite:` "the chord already fired, so do not also fire
the tap" is a rule somebody has to remember. "The peak was three keys and the
chord is two" is arithmetic.

## Storage

`global.json`, `overlay.keybinds` — an object of action id → chord string:

```json
{ "overlay": { "keybinds": { "shell_alt": "Ctrl+Shift+P" } } }
```

**Only actions that differ from their default appear.** A fresh config carries
no keybind keys at all and behaves exactly as every build before this one; a
row cleared (or `ritz_keybinds_reset`) removes its key again. That is the same
"unset means follow the built-in" shape `fade_ms` and `cursor_outline_color`
use, expressed as absence-from-a-map because the set of actions is open: an
action added later needs no schema change, and an entry naming an action this
build does not have is simply never looked up.

**Global, never per profile.** This is the `overlay` section, which
[`profiles.md`](profiles.md) keeps in `global.json` precisely because it is
about the player's own screen and hands rather than about a game. Two extra
reasons apply specifically here:

- a per-profile keybind would mean the chord that opens the settings **changes
  when you launch a different game** — the one control you need in order to fix
  it would move on its own; and
- profiles are switched *from* the settings shell, so a keybind stored in a
  profile could be changed by an action taken through the very UI it opens.

A stored chord that will not parse, or that collides with an already-resolved
one, falls back to that action's default with a warning in the log
(`ApplyFromConfig`). `Why repair rather than obey:` only hand-editing can
produce such a file, and the failure it produces — two actions on one chord,
one of them the way into the settings — is exactly the one the user cannot fix
from inside the UI.

## Conflicts

No two actions may hold the same chord, and nothing may hold the reserved one.
Checked in `SetChord()`, so the refusal a click gets and the refusal
`ritz_keybind` gets are the same sentence from the same check:

```
Already used by "Open settings (alternate)".
That chord is reserved as the way back into the settings if a binding goes wrong (Ctrl+Alt+Shift+O).
"Wumpus" is not a key I know.
```

A refused set changes nothing: the store, the file and the row all stay as they
were. In the UI the refusal arrives as a toast; from the console it is an error
line.

## Getting back in — two ways, on purpose

The one genuinely dangerous edit here is a shell chord the user cannot type any
more, because at that point the settings UI, and with it the way to fix the
binding, is gone. So there are two independent recoveries, neither of which
needs the other:

1. **`Ctrl+Alt+Shift+O` always opens the settings.** It is not an action, it
   cannot be rebound, and it is refused as a value for any action. It is shown
   in the Keybinds area's own "If you get stuck" group, as a read-only row —
   read-only *because* a row that could be edited could be edited into
   uselessness, which is the failure it exists to prevent.
2. **`gamescopectl ritz_keybinds_reset`** puts every action back on its
   default, from outside the session, with no keyboard chord involved at all.
   `ritz_keybinds` lists what is bound; `ritz_keybind <action> <chord>` sets
   one.

`Why both:` the reserved chord is useless to someone who cannot remember it,
and the console command is useless to someone in a fullscreen game with no
second machine. Each covers the other's gap, and neither costs anything.

## The editing control

A chord row is a `Kind::Text` row (a plain string binding) carrying the
presentation flag `Entry::Chord()` — exactly the shape `Entry::Dropdown()`
already had. The shell draws it as a **capture chip** instead of an input
field: clicking it arms a capture, the chip reads `Press a chord (Esc
cancels)`, and the next chord you press becomes the value.

`Why capture and not a text field:` the grammar is writable and the console can
write it — that is what `ritz_keybind` and `overlay_e2_set` are for, and it is
how the tests drive one. But asking a player to type a keysym name in order to
change a keyboard shortcut is the wrong question: they know the chord as a
thing their hands do. So the field listens, and the typed form stays as the
scriptable and documented fallback rather than as the interface.

`Why a presentation flag and not a new Kind:` the row stays an ordinary string
row everywhere that matters — the palette, `overlay_e2_set`/`get`, the
Inspector, reset-to-default and persistence all keep reading it unchanged. Only
the row painter's choice of atom changes. A new Kind would have meant a new
branch in every one of those.

While a capture is armed, `ProcessKey()` **swallows every key event** and
matches nothing. That is what makes "the chord being captured cannot itself
trigger an action" true structurally rather than by a list of exceptions —
including the reserved chord, and including the very binding being rebound.
Only a release whose press was swallowed is swallowed in turn, so a modifier
already held when the chip was clicked keeps its release and the game cannot be
left holding it down.

- **Esc cancels** (a capture whose whole gesture is exactly Escape).
- The capture is armed on the steamcompmgr thread (the click), resolved on the
  wlserver thread (the keys), and **committed** back on the steamcompmgr thread
  by `PumpCapture()`, because committing means writing `global.json` and
  `config::` is single-threaded. The pump runs from the row getters, which run
  every frame the area is on screen — and the area is necessarily on screen,
  because arming requires clicking one of its own rows.

### Capture generalises modifiers, except in a tap

A captured chord that contains a real key stores its modifiers as
**either-side**: pressing Left Ctrl + Left Shift + P stores `Ctrl+Shift+P`,
which then also works with the right-hand keys. A captured chord that is
**only** modifiers keeps its sides: tapping Right Shift stores `RShift`, not
`Shift`.

`Why the exception:` a modifier *in* a chord is a modifier, and either hand is
fine. A modifier that *is* the chord is a specific key, and which hand matters
enormously — a lone `Shift` tap would fire every time the player crouches.

## Live application

A rebind takes effect on the next key event, with no restart. Three paths keep
that true:

- `SetChord()` updates the live table **and then** persists, so the chord is
  already in force when the write is queued;
- `main.cpp`'s `ritz_apply_config_live()` calls
  `PanelKeybinds_SeedFromConfig()` **above** its `bStartup` early-out, so a
  saved rebind is in force from the first key event of the process rather than
  from the first time the shell is drawn — the shell being the thing the
  binding opens;
- `PanelKeybinds_RegisterArea()` seeds once more when the registry is built,
  for a harness that never runs the startup apply.

This follows `PanelDisplay.cpp`'s `ApplyEdit` ordering rule (config first, then
live state) in the one direction that matters here: the live table is the
authority the hotkey path reads, and a config reload writes the same value into
it rather than an older one.

## Console surface

```
gamescopectl ritz_keybinds                            # list, with the reserved chord
gamescopectl ritz_keybind "shell_alt Ctrl+Shift+P"    # set one (empty chord = default)
gamescopectl ritz_keybinds_reset                      # all back to defaults
gamescopectl overlay_e2_set "keybinds.shell RShift"   # the row's own binding
gamescopectl wlserver_debug_key "29 1 42 1 24 1 24 0 42 0 29 0"   # actually press Ctrl+Shift+O
```

`wlserver_debug_key` is the only way to exercise a hotkey from a script:
`overlay_e2_key` appends to the overlay's own input queue, which is downstream
of `wlserver_process_hotkeys()` and therefore cannot see a binding at all. See
that ConCommand's own comment in `wlserver.cpp`.

## What is verified, and where

- `tests/test_keybinds.cpp` — the grammar (round trip, canonicalisation,
  refusals), sided vs either-side matching, the tap rule, defaults-when-unset,
  conflict refusal, and the gesture engine driven exactly as wlserver drives
  it, including the historical "longer chord eaten by its own prefix" case and
  every capture path.
- `build-release/verify-shots/keybinds-2026-09-08/` — `verify_keybinds.py`,
  33 checks on a real gamescope against a private headless sway and an isolated
  `XDG_CONFIG_HOME`: the defaults fire, a rebind takes effect live and the old
  chord dies, conflicts are refused, the reserved chord recovers a deliberately
  broken shell binding, the value survives a restart, it lands in `global.json`
  and no profile file is touched, and a capture driven by a real click plus a
  real chord lands in the row (with screenshots).
- `scripts/settings-audit.sh` reports the three chord rows as NOT COVERED by
  name, pointing at the two above. `Why not covered:` the audit's generic
  round trip needs "a different valid value", and for a chord that is neither
  a step nor a next option — every string that is not a chord is refused by
  design.
