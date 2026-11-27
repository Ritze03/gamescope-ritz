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
`gamescopesteamoverlay`, installed beside `gamescopereaper`) is a tiny helper process
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
not spin), found through `PATH`.

## The test switch and every knob

The switch is deliberately temporary: step I3 replaces it with a config key. The question
"is the route on?" lives in one function, `Process::SteamOverlayDummyRequested()`.

| env | default | meaning |
|---|---|---|
| `GS_RITZ_STEAM_OVERLAY_DUMMY` | off | `1` turns the route on (gamescope-ritz) |
| `GS_RITZ_STEAM_OVERLAY_LOG` | stderr | file the dummy window appends its timestamped log to |
| `GS_RITZ_STEAM_OVERLAY_IDLE_FPS` | `1` | presents per second while the overlay needs none |
| `GS_RITZ_STEAM_OVERLAY_HOTKEY` | Steam's own | e.g. `Ctrl+Shift+Tab`; else `InGameOverlayShortcutKey` from the newest `localconfig.vdf`, else Shift+Tab |
| `GS_RITZ_STEAM_OVERLAY_IDLE_CLOSE_MS` | `0` (off) | a positive value closes the overlay state after `BOverlayNeedsPresent()` stayed false that long; see "Open and closed" |
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

There is no "overlay is open" export, so `DecideOpen()` in the helper is a plain hotkey
toggle (one small clearly marked function; every transition and ignored press is logged):

- hotkey press, closed to open: set `STEAM_INPUT_FOCUS=1` on the window (gamescope then
  routes mouse and keyboard to it). **Ignored, with one log line, when there is nothing to
  give input to:** no overlay library loaded (`BOverlayNeedsPresent` did not resolve), or
  `IsOverlayEnabled()` resolved and returns false;
- hotkey press, open to closed: set it back to 0 (always honoured);
- optional: with `IDLE_CLOSE_MS` > 0, `BOverlayNeedsPresent()` false that long closes it.
  Off by default: a static overlay may legitimately not ask for presents.

**Accepted limit of the spike:** the overlay is assumed to be opened and closed only with
the hotkey. Closing it with Esc or its own button leaves input on the dummy window until the
hotkey is pressed again, and from then on the helper's toggle and Steam's are inverted until
the helper is restarted. The cure is the `GameOverlayActivated_t` callback below. The first
real test is meant to show how `BOverlayNeedsPresent()` and `IsOverlayEnabled()` behave while
the overlay is open, static, closed and showing a toast (both are logged on every change,
with per-second event and present counts). `STEAM_INPUT_FOCUS` must be cleared again, not
just the window hidden: a window with it set takes input even at opacity 0
(`planning/steam-friends-window.md`). Also assumed: events the overlay consumes are swallowed
by its `XNextEvent` hook. **Settled headless** (a stand-in library whose
`BOverlayNeedsPresent()` follows a flag file; keys and pointer motion sent through
gamescope's own seat with `gamescopectl wlserver_debug_key` / `wlserver_debug_mouse_motion`):
while open, `KeyPress`/`MotionNotify` reach the dummy window; after the second hotkey
press they no longer do (they go back to the game); with the flag dropped for 3.5 s while
open it stays open (no auto-close at the default); without any library the hotkey is logged
as ignored and `STEAM_INPUT_FOCUS` never becomes 1.

### The upgrade path: `GameOverlayActivated_t`

The official signal is Steam's `GameOverlayActivated_t` callback (callback id 331, first
byte `m_bActive`). It would take a Steam pipe from `steamclient.so` (`CreateSteamPipe`,
`ConnectToGlobalUser`), then polling `Steam_BGetCallback` / `Steam_FreeLastCallback` from a
thread of the dummy window, the way `src/SteamFriends.cpp` already talks to
`steamclient.so`. That replaces `DecideOpen()` with a true signal. Not built: it needs the
real client to test and this step may not touch one.

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
