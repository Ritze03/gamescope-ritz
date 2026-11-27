# Steam overlay and the dummy window

Steam's in-game overlay (Shift+Tab: friends, chat, browser, achievements) is two pieces
Steam loads into every process of a launch. gamescope-ritz is part of Steam's `%command%`,
so it and everything it starts inherit both. This page is about who carries them. Step I1
of the Steam-overlay work (2026-11-27): a **dummy window** behind a **test-only switch**.
With the switch off nothing changes (last section).

## The two pieces

- `gameoverlayrenderer.so`, through `LD_PRELOAD` (Steam passes a 32-bit and a 64-bit path,
  the dynamic linker skips the wrong one). It interposes Xlib's event and window functions
  (`XOpenDisplay`, `XCreateWindow`, `XNextEvent`, `XPending`, `XGrabKey`, ...), GL
  (`glXSwapBuffers`, `egl*`) and, from xcb, only `xcb_create_window`. It exports plain
  functions such as `IsOverlayEnabled()` and `BOverlayNeedsPresent()`. **There is no export
  that says "the overlay is open".** It detects the hotkey from key events in the Xlib event
  stream of the hooked process.
- The Vulkan implicit layer `VK_LAYER_VALVE_steam_overlay_64`: on with env
  `ENABLE_VK_LAYER_VALVE_steam_overlay_1=1`, off with `DISABLE_VK_LAYER_VALVE_steam_overlay_1=1`
  (the loader lets DISABLE win).

The overlay draws only where a process presents (Vulkan present or GL swap); its UI is a
separate process, `gameoverlayui`, rendering into shared textures.

## What the dummy window is, and why

The **dummy window** (`src/Apps/gamescopesteamoverlay.cpp`, executable
`gamescopesteamoverlay`) is a tiny helper process
gamescope-ritz starts inside its own Xwayland. It is one full-screen, see-through X window
that presents with Vulkan. It carries the overlay; the game and gamescope-ritz carry
neither piece. gamescope composites the window above the game as its own layer.

*Why not the game's process:* the overlay then lags (steam-for-linux#11446: lag spikes after
25-40 minutes; Valve fixed it for Vulkan presenters in 2025, still reported for GL ones) and
cannot draw at all when the game does not present through something it hooks.
*Why not gamescope-ritz's process:* upstream gamescope hit lag spikes with the overlay in its
own process (upstream commit `e399345`, gamescope#163); that is why the preload is dropped on
re-exec. *Why Vulkan:* the open lag bug is on the GL path. *Why a helper process, not a late
load:* the preload is fixed at process start; a late `dlopen` cannot hook calls that are
already bound, and injecting into a running game is what anti-cheat treats as tampering.

Supporting evidence that a helper can carry it: `steamcompmgr.cpp` strips the overlay from
mangoapp ("would latch onto mangoapp's own swapchain as if it were the game").

**The window must be plain Xlib** (`XOpenDisplay`, `XCreateWindow`, `XPending`/`XNextEvent`),
not xcb and not SDL, or the overlay's hooks never see its window or its input events.

## Environment per process

The route is on only on Wayland, SDL and Headless, and only with the switch set. DRM and
OpenVR stay as "switch off" even with it set.

| process | switch off | switch on |
|---|---|---|
| gamescope-ritz | Wayland/DRM/OpenVR/Headless: re-exec without the preload; SDL: keeps it. Vulkan layer: untouched | re-exec without the preload on every route backend (SDL too); `DISABLE_VK_LAYER_VALVE_steam_overlay_1=1`, `ENABLE_..` unset, set right after the re-exec point, before any Vulkan instance |
| game, mangoapp, other children | non-SDL backends: preload restored from the stash (the game draws the overlay itself); SDL: gamescope-ritz keeps and draws it, so it is removed for the game; layer untouched | no overlay entry in `LD_PRELOAD` (other entries kept); layer off as above; the stash variable never reaches them |
| dummy window's `gamescopereaper` | does not exist | the game's environment: no overlay preload, layer off, plus two private variables `GS_RITZ_STEAM_OVERLAY_HANDOVER=1` and `GS_RITZ_STEAM_OVERLAY_PRELOAD` (the overlay entries) that load nothing |
| dummy window (after its hand-over re-exec) | does not exist | `LD_PRELOAD` = the overlay entries from the stash (both paths, passed through unchanged), `ENABLE_VK_LAYER_VALVE_steam_overlay_1=1`, `DISABLE_..` unset, private variables removed |

**Only the dummy window carries the overlay, not the reaper.** The helper runs under
`gamescopereaper`; if the reaper had the overlay in its environment, the real library would
load into the reaper too and register with Steam as a second hooked process of the game.
So gamescope-ritz hands the values over in the private variables and the helper, at startup,
moves them into `LD_PRELOAD` / the layer pair and `execv`s itself once. (Alternative not
taken: starting the helper without the reaper needs its own death-signal, shutdown and
no-respawn handling; the hand-over is smaller.) Checked in `/proc/<pid>/environ` of the
game, its reaper, the helper's reaper and the helper.

If neither `LD_PRELOAD` nor the stash holds an overlay entry (a launcher wrapper clears
`LD_PRELOAD`), the dummy window gets a 64-bit `gameoverlayrenderer.so` found in the Steam
install: `<steam root>/steamrt64/` (current clients) or `<steam root>/ubuntu12_64/`
(older), roots tried in order `$STEAM_COMPAT_CLIENT_INSTALL_PATH`, `$HOME/.steam/steam`,
`$HOME/.steam/root`, `$HOME/.local/share/Steam`. Not found: the dummy window still starts
and hosts nothing (it logs that).

Code: pure functions in `src/Utils/Process.{h,cpp}` (`ComputeGameOverlayEnv`,
`ComputeDummyOverlayEnv`, `FindSteamOverlayRenderer`, `StripOverlayFromPreloadList`), thin
environment wrappers (`ApplySteamOverlayEnv`, `TakeStashedOverlayPreload`), tests in
`tests/test_process.cpp` (`[steam-overlay]`). `main()` (`src/main.cpp`, at the re-exec
point) decides the route once; `StartSteamOverlayDummy()` / `StopSteamOverlayDummy()`
(`src/steamcompmgr.cpp`) start and stop the helper. Start runs at the end of
`LaunchNestedChildren`; stop has no caller yet (shutdown's `KillAllChildren` already takes
the helper down, and the reaper's death signal covers a crash). The helper is started like
mangoapp, through `gamescopereaper` without `--respawn` (a helper that exits at once must
not spin).

**Finding the helper** (`FindSteamOverlayHelper()`): first `gamescopesteamoverlay` in the
directory of the running executable (`readlink /proc/self/exe`, so the symlink
`$HOME/.local/bin/gamescope-ritz` resolves to `<clone>/build/`, and a build tree resolves
to `build-agent/src/`), spawned by that absolute path; else the bare name, searched in
`PATH`. Missing in both places: one warning, no dummy window, the session carries on. The
start line logs the path used. `install.sh` builds the helper with the compositor and
copies it to `<clone>/build/gamescopesteamoverlay` (temp file, then `mv -f`, like the
compositor), so no link in `$HOME/.local/bin` is needed. *Why next to the executable and
not `PATH`:* the installer links only the compositor, and the distribution's own
`gamescopereaper` on `PATH` knows nothing of this fork's helper. `--remove` leaves
`build/` alone (it only deletes links), so the copy goes with the clone.

## The test switch and every knob

The switch is deliberately temporary: step I3 replaces it with a config key. (`GS_RITZ_STEAM_OVERLAY_IDLE_CLOSE_MS` existed until 2026-11-27 and is gone: the real log proved `BOverlayNeedsPresent()` cannot tell open from closed.) The question
"is the route on?" lives in one function, `Process::SteamOverlayDummyRequested()`.

| env | default | meaning |
|---|---|---|
| `GS_RITZ_STEAM_OVERLAY_DUMMY` | off | `1` turns the route on (gamescope-ritz) |
| `GS_RITZ_STEAM_OVERLAY_LOG` | stderr | file the dummy window appends its timestamped log to |
| `GS_RITZ_STEAM_OVERLAY_IDLE_FPS` | `1` | presents per second while the overlay needs none |
| `GS_RITZ_STEAM_OVERLAY_HOTKEY` | Steam's own | e.g. `Ctrl+Shift+Tab`; else `InGameOverlayShortcutKey` from the newest `localconfig.vdf`, else Shift+Tab |
| `GS_RITZ_STEAM_OVERLAY_NO_CALLBACK` | off | `1`: never load `steamclient.so`; open/closed is the plain hotkey toggle (for comparing) |
| `GS_RITZ_STEAMCLIENT` | search `<steam root>/linux64/steamclient.so` | full path of the `steamclient.so` the helper loads for the open/closed callback (same override as the main program) |
| `GS_RITZ_STEAM_OVERLAY_TEST_RECT` | off | `1`: one opaque orange rectangle in the top-left quarter |

## How the window works

- One 32-bit ARGB window covering the root window, follows root size changes. Properties
  `STEAM_OVERLAY=1` (gamescope composites it above the game and never treats it as the game
  or focus window, `DetermineAndApplyFocus`) and `STEAM_INPUT_FOCUS=0`; opacity is left at
  the default (opaque), because notifications (achievement toasts) show with the overlay
  closed.
- Vulkan swapchain: `B8G8R8A8_UNORM`, FIFO, composite alpha PRE_MULTIPLIED (Mesa offers it
  for a 32-bit-visual window), cleared to RGBA 0,0,0,0 each present.
  **Measured (headless, RADV): the game shows through everywhere nothing is drawn, both with
  and without this fork's WSI layer.** The dummy window therefore does not get the WSI layer
  (plain Xwayland is the one path). **No alpha, no window:** if the surface does not offer
  PRE_MULTIPLIED, the helper logs an error and exits instead of presenting (an opaque window
  would cover the game); it is not respawned, so that means no overlay. A failed swapchain
  recreate (resize) is logged and stops presenting.
- Presents only when needed: `BOverlayNeedsPresent()` (found with `dlsym(RTLD_DEFAULT, ..)`,
  there only when the overlay library is preloaded) is polled every 16 ms and a present
  follows when it is true; plus an idle heartbeat (`IDLE_FPS`, default 1/s) so the overlay
  can attach and its buffers get serviced. Without the library: heartbeat only, polling
  stops (one wake-up per heartbeat).
- Idle cost measured headless with a stand-in library: 1 present/s, ~63 wake-ups/s (the
  16 ms poll), CPU below the 1% tick resolution, ~44 MB RSS.
- Test drawing: `TEST_RECT=1` clears the top-left quarter to (255, 76, 0), opaque
  (`vkCmdClearAttachments`).
- Exits when the X connection goes away (IO error handler) or on SIGTERM.

### Hotkey

The game has the keyboard, so the dummy window would never see Steam's hotkey. It takes a
passive `XGrabKey` on the **root** window for the hotkey, with the NumLock and CapsLock
modifier variants, so the press is delivered to the dummy window's own Xlib event stream,
where the overlay's hooks look. Source order: env, then `InGameOverlayShortcutKey` in the
newest `<steam root>/userdata/<id>/config/localconfig.vdf` (a tab-separated line such as
`Ctrl<TAB>Shift<TAB>KEY_TAB`), then Shift+Tab. Tokens it maps: `Ctrl`, `Shift`, `Alt`,
`Super`, `KEY_` names via X keysym names (letters, digits, `Tab`, `F1`..`F24`, `Space`,
`Home`, ...); anything else is logged and falls back. **Settled in a headless run:** with a
client holding the keyboard, the grabbed press reaches the dummy window (`hotkey press
delivered`). **Unproven against real Steam:** that the overlay's hooks accept a press that
arrives as a grab event (window = root) and open the overlay. A side effect to know: the
grabbed combination no longer reaches the game at all while the dummy window runs.

### Open and closed, and the input hand-over

There is no "overlay is open" export, so the helper has to learn it from Steam. The one
function `DecideOpen()` holds the whole rule (every transition and every ignored press is
logged). Opening sets `STEAM_INPUT_FOCUS=1` on the window (gamescope then routes mouse and
keyboard to it), closing sets it back to 0. It must be cleared, not just the window hidden:
a window with it set takes input even at opacity 0 (`planning/steam-friends-window.md`).

**Authority rule.** Steam posts the Steamworks callback `GameOverlayActivated_t` (callback id
**331** = friends callbacks base 300 + 31, first payload byte `m_bActive`, 1 = opened,
0 = closed) whenever the overlay opens or closes, by whatever means (hotkey, Esc, its close
button, Steam opening it itself).

- Until the first 331 of the session has arrived, the **hotkey toggles** (press: closed to
  open, next press: open to closed). Ignored, with one log line, when there is nothing to
  give input to: no overlay library loaded, or `IsOverlayEnabled()` returns false.
- From the first 331 on, **callbacks are the only thing that changes the state**; hotkey
  presses are only logged ("hotkey press ignored, callbacks decide").
- So a failed connection (no library, missing symbol, pipe 0, user 0: each logged once)
  degrades to the plain hotkey toggle, and the helper never exits because of Steam.

**The callback pipe.** After startup, with an overlay library loaded, the helper `dlopen`s
`steamclient.so` (`RTLD_LAZY | RTLD_LOCAL`; path `GS_RITZ_STEAMCLIENT`, else
`<steam root>/linux64/` over the same roots as the hotkey lookup), requires the symbols
`Steam_CreateSteamPipe`, `Steam_ConnectToGlobalUser`, `Steam_BGetCallback`,
`Steam_FreeLastCallback`, `Steam_ReleaseUser`, `Steam_BReleaseSteamPipe` (flat C exports,
same signatures and `CallbackMsg_t` layout as `tests/steam_friends_live_probe.cpp`), creates
its own pipe, connects the global user and drains `Steam_BGetCallback` /
`Steam_FreeLastCallback` in the existing 16 ms main-loop wake-up, in the helper's own
thread (no new one). The queues are per pipe, so this cannot take a callback from the game's
pipe (`steam-friends.md`, "Callbacks are not the blocker"). The pipe and user are released
on exit; the library is never `dlclose()`d (it starts threads). Without an overlay library
there is nothing to give input to, so no pipe is created. `GS_RITZ_STEAM_OVERLAY_NO_CALLBACK=1`
skips all of it.

**Why the hotkey toggle cannot work** (first real Steam test, Unity game from Steam, 2026-11-27):

- The hotkey (`Ctrl+Shift+Tab` from Steam's config, grabbed as keycode 23, modifiers 0x5 plus
  the NumLock/CapsLock variants) opened the overlay: `hotkey press delivered` then
  `open/closed: hotkey -> OPEN`, `STEAM_INPUT_FOCUS -> 1`; mouse, clicks and keys reached the
  helper for about 40 s.
- That was the only hotkey press the helper ever saw. While the overlay is open its hooks
  swallow key presses (per-second counts show `KeyRelease` without `KeyPress`), so the press
  that closes it never arrives. The helper stayed "open", `STEAM_INPUT_FOCUS` stayed 1, and
  after the overlay was closed (presents dropped from ~130/s to 1 to 2/s) mouse and keyboard
  still went to the helper and the game got nothing: "it works, but it freezes the game".
- `BOverlayNeedsPresent()` is no substitute: it was true on nearly every 16 ms poll for the
  first 146 s with the overlay **closed** (about 58 presents/s the whole time), stayed busy
  while open, and only dropped to one every ~2 s after the first open/close cycle. It says "a
  frame is wanted", not "the overlay is open". (Its transition lines are throttled to those
  that follow >= 0.5 s of stability, the per-second line carries the count of true polls.)

**What that first real test established:** the real overlay attaches to the helper
(`IsOverlayEnabled -> 1` 0.8 s after start, swapchain 1920x1080 format 44 alpha mode
PRE_MULTIPLIED), the hotkey read from Steam's config and grabbed with a passive `XGrabKey`
does open it, and the see-through window works on RADV. **Not yet known:** whether 331 is
delivered to the helper's own pipe (look for `callback 331` in the log), and what the
`FocusIn`/`FocusOut` bursts while open are (they are now logged with mode and detail).

**Log lines for the next real test.** Per second: `presents=N needsTrue=N events: ... callbacks: ID=N ...`
(counts of every callback id seen on the pipe). `callbacks: connected (...)` or one failure
line at start. `callback 331 GameOverlayActivated_t: 8 bytes [..] -> active=0|1`, then
`first GameOverlayActivated_t, callbacks are now the only authority`, then
`callback -> OPEN|CLOSED`. `a hotkey press was not followed by a 331 within 1 s` means the
callback is not reaching this pipe (the hotkey path stays in charge until a first 331).
`FocusIn|FocusOut window .. mode NotifyNormal|NotifyGrab|NotifyUngrab|NotifyWhileGrabbed
detail ..` (one line each); `KeyPress reached the helper while open: keycode N state 0x..`
(at most 20 per second) shows which presses the overlay lets through.

**Settled headless** (stand-ins: a `gameoverlayrenderer.so` whose `BOverlayNeedsPresent()`
follows a flag file, and a `steamclient.so` whose `Steam_BGetCallback` delivers a 331 when a
flag file changes, plus noise ids 304 and 336; input through gamescope's own seat with
`gamescopectl wlserver_debug_key`):
hotkey opens and sets `STEAM_INPUT_FOCUS=1`; then a 331 with active=1 switches the authority;
a 331 with active=0 and **no** second hotkey closes it and clears the focus, and input sent
afterwards no longer reaches the helper; a 331 alone (no hotkey ever) opens and closes;
after the switch a hotkey press alone changes nothing; `NO_CALLBACK=1` never maps
`steamclient.so`; a missing library path and a library without the symbols each give one
log line and keep the hotkey toggle; without an overlay library the hotkey is ignored
and no `steamclient.so` is loaded.
**Careful with `gamescopectl` in tests:** it reads `GAMESCOPE_WAYLAND_DISPLAY` (default
`gamescope-0`), not `WAYLAND_DISPLAY`, so an unset variable talks to whatever session owns
`gamescope-0`.
A hotkey press seen while open produced `FocusOut ... NotifyGrab` and
`FocusOut/FocusIn ... NotifyUngrab` on the dummy window: those are the passive grab
activating, not focus changes by gamescope.

### Open: the game may pause while the overlay has input

The user's "freezes the game" may also be the game pausing itself: Unity pauses on focus loss
unless "run in background" is set. With Steam's own in-game overlay the game keeps its focus.
What gamescope does when the dummy window has `STEAM_INPUT_FOCUS=1` (read from the code, not
yet observed on a real game):

- `xwayland_ctx_t::DetermineAndApplyFocus` (`steamcompmgr.cpp`, ~4578) picks the overlay
  window (`isOverlay`, `inputFocusMode`) as `inputFocus` (~4619); `keyboardFocusWin = inputFocus`
  (~4673), and only `inputFocusMode == 2` swaps it back to `focus.focusWindow` (~4677).
- With mode 1 it then calls `XSetInputFocus(ctx->dpy, <overlay window>, ...)` (~4764): the
  game's window, on the **same** Xwayland server, receives `FocusOut` with mode `NotifyNormal`.
  It also writes `_NET_ACTIVE_WINDOW` = the overlay window (~4767-4769), hides the cursor
  (~4788) and, in the global pass, `wlserver_keyboardfocus(<overlay surface>)` (~5271), so the
  Wayland keyboard leaves the game's surface. Not done: no unmap, no `WM_STATE` change
  (`set_wm_state` only runs for the *focus* window, ~4658). So the game observes a plain
  focus loss, which is what makes a "pause on focus loss" game pause.
- Mode 2 (`STEAM_INPUT_FOCUS=2`, `planning/steam-friends-window.md`) keeps the keyboard on the
  game (~4677, ~5248, ~6441): mouse to the overlay, `XSetInputFocus` stays on the game.
- Upstream's Deck setup avoids it by construction: with `--xwayland-count 2`, the Steam UI
  window lives on one Xwayland server and the game on another (`get_index() != 0` gives the
  `SteamControlled` strategy, ~4629); `XSetInputFocus` is per server connection, so the game's
  server never sees its window lose X focus. (The Wayland keyboard still moves between the
  two Xwayland surfaces; whether that makes the game's Xwayland emit a focus loss is
  unmeasured.) The Deck's quick-access menu uses mode 2 for the same reason, and Steam's
  overlay proper runs inside the game process.

Ways to keep the game believing it is focused, with the cost of each:

- **(a) Dummy window on a second Xwayland server.** Needs the helper to start on another
  `DISPLAY`, and gamescope-ritz to run two servers (`--xwayland-count 2`, game placement on
  the second: reshapes launch, focus selection and `DISPLAY` handling for everything). The
  passive `XGrabKey` only works on the game's own server, so the hotkey would have to be
  injected by gamescope itself (`wlserver_process_hotkeys`) into the helper's stream. Largest
  change, most risk; the Wayland keyboard-leave question remains.
- **(b) Route at the Wayland level, X focus stays on the game, the helper takes an active
  grab.** The helper holds `XGrabKeyboard` / `XGrabPointer` on its window while open. X then
  delivers all key and pointer events to it, and the game sees at most `FocusOut` with mode
  `NotifyGrab` (SDL ignores those; other toolkits unknown, Unity unmeasured), never a
  `NotifyNormal` loss. The cost is in the helper alone (grab on open, ungrab on close, handle
  a refused grab), gamescope untouched if combined with (c). Risk: the overlay's own hooks may
  grab as well, and the game still gets the `NotifyGrab` events.
- **(c) Simplest, found while reading: use `STEAM_INPUT_FOCUS=2` instead of 1, plus (b)'s
  `XGrabKeyboard`.** Mode 2 already means "pointer to the overlay, `XSetInputFocus` stays on
  the game" (~4677), so no gamescope change: the helper writes 2 instead of 1 and, while
  open, grabs the keyboard (the Wayland keyboard stays on the game's surface, but an X grab
  inside Xwayland redirects the events to the grabber). One-line property change, a dozen
  lines of grab code, `XUngrabKeyboard` on close.
- **Recommendation:** (c), measured first with a log-only run (set 2 and watch whether the
  helper still receives key events with and without the grab, and whether the game's window
  logs a focus loss), then keep (a) in reserve. Do not implement (a) before (c) is shown to
  fail on a real game.

## Layering (read from the paint code, nothing changed)

`STEAM_OVERLAY` windows wider than the root (or with input focus) become
`focus.overlayWindow`; `paint_all` pushes them as their own layer at `g_zposOverlay` (3):
above the game, the override and the external overlay (0 to 2), below the cursor (4), the
FPS HUD (6), toasts (7) and the settings overlay. Frame generation only substitutes layer 0
(`rendervulkan.cpp`, "FRAME GENERATION ... runs FIRST, on the RAW layer 0"), so the dummy
window's layer is never smeared. The alpha mode for overlay windows is premultiplied
(`cv_overlay_unmultiplied_alpha` is only set on some DRM paths).

**Cost of the always-visible window (measured by reading the code at this commit; the
planned cure is not implemented).** While the overlay window has non-zero opacity,
`steamcompmgr.cpp` `bHasOverlay` (~11679) is true. That forces `FlipType::Normal` (no
tearing, ~11739-11740), skips frame generation's async-flip path (~11727), and its tear
present / the auto-V-sync async path (~11793-11808, ~11881, ~11890). Each heartbeat commit
also forces a compose (`hasRepaintNonBasePlane`, ~9020-9038). An extra full-screen layer is
composited. The dummy window is always mapped and opaque, so all of this holds for the whole
session, and the first real test runs that way on purpose, to keep variables out of it.
**Planned cure for the next step:** the helper writes `_NET_WM_WINDOW_OPACITY = 0` while
idle and closed and restores it when `BOverlayNeedsPresent()` is true or the overlay is open
(every gate tests `->opacity`; input focus does not depend on opacity; toasts need the
`BOverlayNeedsPresent()` branch). Line numbers will drift; search the symbols.

## Testing without Steam

Never against the real client. A stand-in `gameoverlayrenderer.so` exporting
`BOverlayNeedsPresent`/`IsOverlayEnabled` (few lines of C, true while a flag file exists) as
`LD_PRELOAD`, a throwaway `HOME` (so the install lookup and the Vulkan layer search cannot
find the real Steam) and an isolated `XDG_CONFIG_HOME` with `audio.capture` off:

```sh
GS_RITZ_STEAM_OVERLAY_DUMMY=1 GS_RITZ_STEAM_OVERLAY_TEST_RECT=1 \
  gamescope --backend headless -w 1280 -h 720 -W 1280 -H 720 -- vkcube
gamescopectl screenshot "<path>.png 4"      # type 4 = screen_buffer
```

(`gamescopesteamoverlay` and `gamescopereaper` must be on `PATH`; a system-wide implicit
frame-generation Vulkan layer may need its own disable variable for the test client.) Result
(2026-11-27): with `TEST_RECT`, pixels in the top-left quarter are (255, 76, 0) and the cube
shows through elsewhere; without it the picture is byte-identical to a run with the switch
off.

## What launch handling does with the switch off

Steam preloads its overlay into gamescope-ritz, but only the SDL backend can draw it.
`main()` resolves the backend (`Auto` via `auto_select_backend()`); on anything but SDL it
calls `Process::RestartWithoutSteamOverlay(argv)`: it stashes the original `LD_PRELOAD` in
`GAMESCOPE_STEAM_OVERLAY_LD_PRELOAD` (which doubles as the guard against exec'ing twice),
removes the overlay entries and re-execs `/proc/self/exe`. In `LaunchNestedChildren`,
`RestoreSteamOverlayPreload()` puts the stash back into `LD_PRELOAD` for the game, and if
there is no stash and the backend presents with a Vulkan swapchain (SDL) the overlay is
removed from `LD_PRELOAD` instead (the game must not draw it twice). mangoapp always has it
removed. The Vulkan layer variables are untouched, so whatever Steam set stays.

## Related

- [process-management](process-management.md) - the reaper the helper runs under.
- [steam-friends](steam-friends.md), `planning/steam-friends-window.md` - the
  `STEAM_OVERLAY` / `STEAM_INPUT_FOCUS` window mechanism this builds on.
- [unit-tests](unit-tests.md) - the `[steam-overlay]` cases.
