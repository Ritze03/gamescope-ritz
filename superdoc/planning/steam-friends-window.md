# Steam's Friends window inside gamescope-ritz — feasibility

**Status: research, 2026-09-08. Nothing in `src/` was changed.** The request was to
show Steam's standalone Friends window (tray icon → Friends) over the game on
`Ctrl+Shift+Tab`. This page records what was measured on this machine, what each
candidate approach actually did when tried, and what should be built if anything.

**The one-line answer.** The real Friends window **cannot** be shown inside
gamescope-ritz on this setup — it belongs to the host's Xwayland and cannot be moved,
captured usefully, or made clickable from inside a nested compositor. What *can* be
built, and was proven working end to end, is a **fullscreen interactive overlay hosting
Steam's own web chat**, using X properties steamcompmgr already reads — no compositor
change beyond the hotkey. Whether that is worth having is a judgement call the
["Recommendation"](#recommendation) section puts plainly.

---

## 1. Where the Friends window actually lives

Measured, not assumed.

| Question | Answer | How it was established |
| --- | --- | --- |
| Where does Steam run? | On the **host** Hyprland session, started 2026-09-08 00:45 | `ps` + `/proc/2017811/environ` |
| Which display server owns it? | Hyprland's **Xwayland `:1`** | `DISPLAY=:1` in Steam's environ; `hyprctl clients` reports the window with `"xwayland": true` |
| What kind of window is the Friends list? | An **X11 toplevel** owned by `steamwebhelper` (pid 2018065), `class` `steam`, title `Friends List`, mapped at the host's full size | opened it with `steam steam://open/friends`, then `hyprctl clients -j` and `xprop` on `:1` |
| Is Steam ever launched *inside* gamescope in the user's workflow? | **No.** | `extensions/gamescope-ritz.json`'s `CommandSyntax` is `gamescope-ritz {OPTIONS} --`, i.e. gamescope wraps **the game**, per game. Steam launches Ritz, not the other way round: `localconfig.vdf` shows `LaunchOptions` `ritz %command%` on every configured title. |

So the shape is: **host Hyprland → host Xwayland `:1` → Steam client and its Friends
window**, and separately **host Hyprland → gamescope-ritz (Wayland backend) → its own
Xwayland `:2` → the game**. Two different X servers. A window on one is invisible to
the other, and no protocol on this host lets one compositor embed another's surface —
`zxdg_importer_v2` is advertised, but xdg-foreign only expresses *parent* relationships,
never embedding.

> `Why:` this matters more than it looks. Every "just show the window" idea dies on this
> one fact, so it is stated first rather than rediscovered per approach.

---

## 2. Approach 1 — Steam's own in-game overlay (Shift+Tab)

Steam's overlay already contains a Friends list and chat, is fully interactive, and
costs nothing to build. It has to be ruled in or out before anything else is worth
doing.

**What was found.** The user has used it: `localconfig.vdf` carries
`OverlaySavedDataV2_<appid>_windows` entries with a `FriendsList` panel for a dozen
titles, CS2 among them. The overlay works in gamescope in general — it is Valve's own
path and the Steam Deck's whole model, since it draws inside the *game's* frames rather
than as a window.

**But it is switched off for exactly the games that use gamescope here.** Ritz's `Misc`
module maps `clear_ld_preload` to `LD_PRELOAD=""`
(`ritz/resources/extensions/default/misc.json`), which drops `gameoverlayrenderer.so`
and with it the overlay. Cross-referencing `~/.config/ritz`:

| appid | title | gamescope-ritz | `clear_ld_preload` |
| --- | --- | --- | --- |
| 730 | Counter-Strike 2 | **on** | **true** — no Steam overlay |
| 240 | Counter-Strike Source | **on** | **true** — no Steam overlay |
| *(preset)* `Default.json` | — | — | **true**, so every game on that preset |

Those are the only two titles with gamescope enabled at all. Both have the Steam overlay
disabled by the user's own config.

**Second, weaker objection.** Because the overlay renders inside the game's own frames,
it goes through `lsfg-vk` frame generation with the game — the same reason this fork
grew its own compositor-drawn crosshair (see `../features/crosshair.md`). Chat text
being interpolated is less harmful than a smeared crosshair, but it is the same defect.

**Verdict.** Not a wasted-effort dismissal, but not a free win either. If the user is
happy to clear `clear_ld_preload` for their gamescope titles, **Shift+Tab gives them a
better Friends list than anything this page proposes** — native, logged in, with
invites and voice. That question should be put to them before any code is written.
Everything below assumes the answer is "no, I want it off".

---

## 3. Approach 2 — open the Friends window on gamescope's `DISPLAY`

**Test.** A gamescope-ritz instance was started nested in the host session
(`--backend wayland`), which stood up its own Xwayland on `:2`. Then:

```
DISPLAY=:2 WAYLAND_DISPLAY=gamescope-0 /usr/bin/steam steam://open/friends
```

**Result.** Nothing appeared on `:2` — the only windows there stayed the compositor's
own (`steamcompmgr`) and the test client. The already-running Steam client swallowed the
URL through `~/.steam/steam.pipe` and handled it on the display *it* started on.

That is the expected design: `steam://` URLs are forwarded to the running client, and
that client's X connection was made once, at start, to `:1`. `steamwebhelper` is one
long-lived CEF process; it does not open a second X connection per window.

**The only way this approach works** is if Steam itself is started inside gamescope —
the Steam Deck's `gamescope -- steam` session model. Then the Friends window is a native
window on gamescope's own Xwayland and everything (paint, input, focus) is free. It is
worth saying out loud as an option, because it is the *only* configuration in which the
literal request is satisfiable. It also means restructuring the user's whole workflow:
one gamescope session owning the desktop instead of one per game, and Ritz's per-game
`gamescope-ritz {OPTIONS} --` wrapper would have to go. That is a much larger change
than the feature justifies.

**Verdict: not possible without restarting Steam inside gamescope.**

---

## 4. Approach 3 — capture the host window and composite it

### 4a. The capture half — possible, with a caveat

The host advertises everything needed (enumerated by talking raw Wayland to
`wayland-1`):

- `ext_foreign_toplevel_list_v1`, `zwlr_foreign_toplevel_manager_v1 v3` — enumerate toplevels
- `ext_foreign_toplevel_image_capture_source_manager_v1` + `ext_image_copy_capture_manager_v1` — the **upstream** per-toplevel capture path
- `hyprland_toplevel_export_manager_v1 v2`, `zwlr_screencopy_manager_v1 v3` — the wlroots/Hyprland ones

gamescope binds **none** of these today — `WaylandBackend.cpp`'s registry handler has no
screencopy of any kind — so this would be new client-side protocol code in the backend.

Capture of an **XWayland** toplevel does work: a probe kitty forced onto the host's X11
(`linux_display_server=x11`) captured with real content via `wayshot --toplevel`. The
Steam Friends window itself captured as **pure black** in every attempt, but that was
self-inflicted (an `xdotool windowclose` earlier in the session left the CEF view a
zombie), so it is not evidence against the protocol. It *is* a live risk worth naming:
a CEF window that the host has occluded or unmapped can stop committing frames, and a
capture of it goes stale or black. Nothing in the capture protocols obliges the source
to keep painting.

### 4b. The input half — dead end

A captured image is a picture. To make it a Friends list you must route clicks and keys
back to the real window on the host, and **no protocol on this host can do that.**

- `zwlr_virtual_pointer_manager_v1` and `zwp_virtual_keyboard_manager_v1` inject into
  the host **seat**, in global compositor coordinates. They move the user's real cursor
  and deliver to whatever window is topmost at that point. There is no "send this event
  to *that* toplevel" request anywhere in the set.
- While gamescope is fullscreen over the Friends window, "topmost at that point" is
  gamescope. The events would come straight back to the compositor that sent them.
- The obvious dodge — park the Friends window on a second monitor or another workspace
  and aim the virtual pointer there — needs a multi-monitor host, visibly drags the real
  cursor off the game's screen, moves host keyboard focus away from gamescope, and is
  impossible in embedded (DRM) mode. It is not shippable.
- X11's own `XSendEvent` would target a specific window without moving the pointer, but
  Chromium/CEF ignores synthetic events (`send_event = True`), which is what the Friends
  window is.

**Verdict: view-only.** A friends list you cannot click or type in is not worth the
protocol code, the capture-staleness risk, or the maintenance. Rejected.

---

## 5. Approach 4 — an interactive overlay client on gamescope's own Xwayland

This is the one that works, and it needs almost nothing from gamescope.

### The primitive, verified

steamcompmgr already promotes any X11 window on its own Xwayland to a fullscreen
interactive overlay when it carries the right properties — this is the mechanism Steam
uses on the Deck, and it is not restricted to Steam:

| property | value | effect | code |
| --- | --- | --- | --- |
| `STEAM_OVERLAY` | `1` | the window becomes `ctx->focus.overlayWindow`, painted on top of the game | `src/steamcompmgr.cpp:4433` |
| `STEAM_INPUT_FOCUS` | `1` | **mouse and keyboard** both go to it | `src/steamcompmgr.cpp:4458`, `:6272` |
| `STEAM_INPUT_FOCUS` | `2` | mouse to the overlay, keyboard stays with the game (the Deck's QAM behaviour) | `src/steamcompmgr.cpp:4516`, `:5080` |
| `_NET_WM_WINDOW_OPACITY` | `0` / `0xFFFFFFFF` | hide / show | `src/steamcompmgr.cpp:2482` |

(`GAMESCOPE_EXTERNAL_OVERLAY` is the *paint-only* sibling — `DetermineAndApplyFocus`
never routes input to it. It is the wrong one for this.)

### What was actually run

Full script and screenshots:
`build-release/verify-shots/steam-friends-2026-09-08/` (`poc-steam-chat-overlay.sh`,
`README.md`). In outline:

1. `gamescope --backend wayland -w 1280 -h 720 -W 1280 -H 720 -- glxgears`, Xwayland on `:2`.
2. `DISPLAY=:2 chromium --ozone-platform=x11 --app=https://steamcommunity.com/chat`
   into an isolated `--user-data-dir`.
3. `xprop` set `STEAM_OVERLAY=1`, `STEAM_INPUT_FOCUS=1`, opacity max on that window.

Results, each with a screenshot:

- **Composites fullscreen over the game**, correctly letterboxed by gamescope's scaler
  (`01-small.png`).
- **Mouse works.** A real click (via `ydotool`, through the host seat, into gamescope)
  at the page's "Reject All" button dismissed the cookie banner; gamescope's own cursor
  is visible at the click point (`s-05.png`).
- **Keyboard works.** Clicking the account-name field and typing put the text in it
  (`s-06.png`).
- **Toggle works.** Opacity `0` shows the game through again (`s-07-overlay-hidden.png`),
  back to max restores the overlay (`s-08-overlay-shown-again.png`).
- **One trap, measured.** With opacity `0` but `STEAM_INPUT_FOCUS` still `1`, the
  *invisible* overlay kept swallowing every keystroke (`s-09.png`). Clearing
  `STEAM_INPUT_FOCUS` to `0` as well hands input back to the game (`s-10.png`). The hide
  path must clear **both**. `Why:` `DetermineAndApplyFocus`'s `if (w->isOverlay &&
  w->inputFocusMode) inputFocus = w;` is not conditional on opacity.

### What the overlay can contain

The Friends *window* cannot be moved here (§1, §3). So the content has to be something
that can be re-created on gamescope's display. The realistic candidate is **Steam's own
web chat, `https://steamcommunity.com/chat`**, in a browser in `--app` mode: it is
Valve's, it has the friends list, presence and text chat, and it is a real interactive
page, not a picture.

Honest limitations:

- **A separate login.** The browser profile is not the Steam client's session. The user
  must sign in once (Steam Guard included) with "Remember me". Easiest done on the host
  desktop against the same profile directory, before ever opening it in-game.
- **No voice, no game invites, no rich presence actions** — it is the web client, not the
  overlay.
- **Weight.** A resident Chromium is hundreds of MB and its own GPU work while the game
  runs. Mitigation: spawn it lazily on the first `Ctrl+Shift+Tab` and keep it after.
- **Notifications don't reach gamescope.** A new message will not raise this fork's own
  toast; the user only sees it after opening the overlay.

---

## Recommendation

**Tell the user the honest shape before building anything**, because two of the three
things they might have wanted are impossible and the third has a real cost:

1. **The literal request — Steam's own Friends window, in gamescope — is not achievable**
   with Steam running on the host. The only configuration where it is, is running Steam
   itself inside gamescope, which means giving up Ritz's per-game wrapper.
2. **If they are willing to stop clearing `LD_PRELOAD` for their gamescope titles, they
   already have a better Friends list than this feature can be** — Steam's own Shift+Tab
   overlay, with voice and invites. This should be checked first; it is a config toggle,
   not a commit.
3. **If they still want it**, the overlay-hosted web chat is genuinely good: proven
   interactive, hotkey-toggleable, ~250 lines, and it composites *after* frame
   generation rather than being interpolated with the game.

**Cost of (3)**: one small helper + one keybind action + one settings row, three commits.
**Cost of doing nothing**: zero, and (2) may well cover it.

Recommend: **ask first, build (3) only if the answer to (2) is no.**

---

## Implementation sketch (only if approach 4 is chosen)

One commit per phase. Nothing here needs a change to the compositing pipeline.

### Phase 1 — the helper and the toggle, hotkey-less

New `src/Overlay/CompanionWindow.{h,cpp}` (name provisional), owning:

- **Spawn.** Fork a configured command on the *first* show, with `DISPLAY` set to
  gamescope's own Xwayland (`wlserver_get_nested_display_name()` / the existing
  `steamcompmgr` Xwayland ctx), and a `GAMESCOPE_RITZ_COMPANION=1` marker property the
  wrapper sets so the window can be recognised without guessing at `WM_CLASS`.
  Default command: `chromium --ozone-platform=x11 --user-data-dir=<config>/companion
  --app=https://steamcommunity.com/chat`, overridable in settings (Firefox users exist).
- **Show / hide.** On the steamcompmgr thread, walk `ctx->list` for the marked window and
  set, via the existing `XChangeProperty` helpers:
  - show — `STEAM_OVERLAY=1`, `STEAM_INPUT_FOCUS=1`, `_NET_WM_WINDOW_OPACITY=0xFFFFFFFF`
  - hide — `STEAM_INPUT_FOCUS=0`, `_NET_WM_WINDOW_OPACITY=0` (**both**, per §5's trap)
- **Teardown.** Kill the child in `steamcompmgr_exit`'s reap path like any other child.

Testable with the ConCommand alone (`companion_toggle`), no hotkey yet — which is also
how it should be reviewed.

### Phase 2 — the `Ctrl+Shift+Tab` binding

The keybinds rework (`src/Keybinds.{h,cpp}`, `src/Overlay/PanelKeybinds.cpp`, another
worker's, in the tree as of 2026-09-08) makes chords data in `global.json`, so **do not
hardcode a chord**. The whole binding is:

1. Add `Companion` to `keybinds::Action` (before `Count`).
2. Add its row to `kActions` in `Keybinds.cpp`:
   `{ "companion", "Open Steam chat", "Ctrl+Shift+Tab", "<help text>" }`.
   `Ctrl+Shift+Tab` parses under the existing grammar — `Ctrl` and `Shift` are
   either-side pairs, `Tab` a plain key — and is not modifier-only, so it fires on
   press and is swallowed.
3. `wlserver.cpp`'s hotkey handler already switches on the fired action; add the case
   that calls the Phase-1 toggle. (That file is another worker's this session — this
   phase lands after theirs.)
4. The Keybinds settings row appears automatically from `kActions`; nothing else in the
   panel needs touching.

The chord clashes with nothing this fork binds (`RShift`, `LCtrl+RShift`, `Ctrl+Shift+O`,
reserved `Ctrl+Alt+Shift+O`), and the store refuses duplicates anyway. It *is* Steam's
own overlay chord for the friends list, which is a feature, not a conflict: gamescope
swallows the key, so the game and the Steam overlay never see it.

### Phase 3 — the settings area and the docs

- A `system.companion` area: enable toggle, command string, and a "sign in" note
  explaining the one-time login. Global, not per profile — same reasoning as every other
  `overlay.*` field (which browser you use is a fact about the machine, not the game).
- `superdoc/features/companion-window.md`, an entry in `superdoc/README.md`, and a
  `CHANGELOG.md` bullet under **Added**.

### What is deliberately not in the sketch

- No capture protocol in `WaylandBackend.cpp` (§4 — input makes it pointless).
- No bundled browser, no embedded web view. The helper is *any* command; shipping a
  Chromium dependency for a chat window would be absurd.
- No attempt to relocate Steam's real Friends window (§2 — impossible).

---

## Reproducing any of this

Everything above is re-runnable from
`build-release/verify-shots/steam-friends-2026-09-08/` (that directory lives under the
build tree and a clean rebuild wipes it; a copy is in the session scratchpad). The
protocol enumeration used a ~30-line raw-Wayland registry dump rather than
`wayland-info`, which is not installed here.
