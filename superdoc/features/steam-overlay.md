# Steam overlay and the dummy window

Steam's in-game overlay (Shift+Tab: friends, chat, browser, achievements) is two pieces
Steam loads into every process of a launch. gamescope-ritz is part of Steam's `%command%`,
so it and everything it starts inherit both. This page is about who carries them: a
**dummy window** behind the global switch **Steam overlay control** (default Off), with a
second switch **Enable Steam Overlay** that starts and stops the window live (see "The two
switches"). With control off nothing changes (last section), but the in-game overlay then takes no
input under gamescope-ritz (see "Known limits").

This replaces the fork's own Friends list (`system.friends`, removed 2026-11-27 at the
user's request: "It isnt needed anymore, and its broken anyways"), whose default chord
`Ctrl+Shift+Tab` was also the user's Steam overlay hotkey.

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

The route is on only on Wayland, SDL and Headless, and only with Steam overlay control on at
launch. DRM and OpenVR stay as "control off" even with it on.

| process | control off | control on |
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
`LaunchNestedChildren`; stop runs from the same reconcile step (shutdown's `KillAllChildren` takes the
helper down too, and the reaper's death signal covers a crash). The helper is started like
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

## The two switches

Both are keys of `global.json`'s `overlay` section, **global only** (a machine-wide setup; the
Steam launch option and the Steam install are per machine). They are never in a profile file
and never part of a settings export (`overlay.*` is excluded there). Model and setters:
`src/Overlay/SteamOverlay.{h,cpp}` (UI-free; the settings tab binds to it).

| key | default | meaning | when it applies |
|---|---|---|---|
| `overlay.steam_overlay_control` ("Steam overlay control") | Off | Off: the game carries the overlay exactly as before (everything in the "control off" column), but it takes no input, see "Known limits". On: the dummy-window route | **next launch** |
| `overlay.steam_overlay_enabled` ("Enable Steam Overlay") | On | with control on at launch: whether the dummy window runs. With control off it is stored and does nothing | **live** |

*Why control is read at launch:* the overlay library is loaded into a process when it starts, so
what the game and gamescope-ritz get in their environment is fixed then. *Why enabled can be
live:* with control on the game never has the overlay, only the helper does, so stopping or
starting the helper is all it takes; the game is unaffected either way. The UI shows
"Restart to apply" while `SteamOverlay_Control() != SteamOverlay_ControlAtLaunch()` (the same
shape as Compatibility's "Compress output images").

**Reading the key at launch.** `main()` asks `SteamOverlay_ControlAtLaunch()` at the re-exec
point, before Vulkan and before the normal config load. The first call reads `global.json`
lazily (`config::LoadGlobal()`, the same early read `Diagnostics.cpp` does for the
compatibility switches) and latches the value; that is what the route runs on for the rest of
the process, on both sides of the re-exec. Nothing in start-up is reordered.

**Env override.** `GS_RITZ_STEAM_OVERLAY_DUMMY` is kept as an override of control for one
launch: `1` forces it on, `0` forces it off, unset or empty = the key
(`Process::SteamOverlayDummyOverride()`, `steamoverlay::ResolveControl()`). *Why:* an existing
Steam launch option uses it, and it is the test hook. `SteamOverlay_ControlAtLaunch()` reports
the effective value.

**Live start/stop.** `SteamOverlay_SetEnabled()` stores the value, writes `global.json` and
`nudge_steamcompmgr()`s. The steamcompmgr thread (the only one that forks, kills and touches
the helper's pid) runs `ReconcileSteamOverlayDummy()` every loop pass and compares
`steamoverlay::HelperShouldRun( controlAtLaunch, enabled )` with what it last applied: on a
change it calls `StartSteamOverlayDummy()` or `StopSteamOverlayDummy()`. Edge-triggered on
purpose: a helper that exits on its own is **not** restarted (no restart loop); flipping Off
then On starts a fresh one. `LaunchNestedChildren` makes the first call, so Enabled Off at
launch means no helper (the game still runs, its environment is already stripped). *Why no
request mechanism of its own:* this is the cursor-policy shape (a value read on the
steamcompmgr thread each pass, see `g_bCursorFallbackPolicyDirty`), minus the flag because the
stored value is the flag. Without a tab: `gamescopectl ritz_steam_overlay_enabled 0|1` (a
ConCommand in `SteamOverlay.cpp`; with no argument it just reports).

**Stop.** `StopSteamOverlayDummy()` clears the pid, sends SIGTERM to the reaper (which forwards
it to the helper and exits) and hands the pid to a detached thread that `waitpid()`s it, so no
zombie is left and the main loop never blocks. The helper handles SIGTERM, leaves its main
loop and closes its X connection; the server releases its keyboard grab and drops the window,
so input returns to the game, also while the overlay is open.

## The Steam Overlay tab (2026-11-27)

Area `setup.steam_overlay` ("Steam Overlay"), the one area of the **MISC** rail group;
`src/Overlay/PanelSteamOverlay.{h,cpp}`, registered from `Shell.cpp`. `Why` MISC: the user, after
it first landed in SETTINGS: *"Put it into the "Misc" tab, instead of "Settings""* (the group,
removed earlier that day with the Friends list, was restored for it; the id is unchanged). It holds only these two
switches: no overlay-control rows were built (see "Controlling the overlay (not built)").

| row | binds | applies | shown |
|---|---|---|---|
| Steam overlay control | `SteamOverlay_Control()` / `SteamOverlay_SetControl()`, key `overlay.steam_overlay_control`, default Off | next launch; status "Restart to apply" (`Entry::StatusWhen`, orange) while `Control() != ControlAtLaunch()`, the Compatibility rows' mechanism | always |
| Enable Steam Overlay | `SteamOverlay_Enabled()` / `SteamOverlay_SetEnabled()`, key `overlay.steam_overlay_enabled`, default On | live | only while the **stored** control value is On (`Entry::ShownWhen`) |

Both rows carry `.Key( "overlay.…" )` like the Compatibility switches, so they are global: no
per-profile dot, no "Reset to inherited", not in a settings export. A `ShownWhen` row is hidden
from the Shell and also leaves the command palette while hidden (the palette indexes the shown
set). The row appears the moment control is switched On, even though the dummy window only runs
after the next launch: with control Off at launch "Enable Steam Overlay" is stored and does
nothing, which the control row's status already explains.

Why: the user's words: *"Lets call the main toggle 'Steam overlay control'. This enables
general steam overlay handling, so current setups stay unaffected. If that is enabled, the
toggle for enabling the steam overlay should be shown. Steam overlay control should be off by
default."* and, once control is On, "Enable Steam Overlay" defaults to On.

## Controlling the overlay (not built)

The user decided the tab holds only the two switches. What was found about controlling the
overlay from the dummy window, for whoever picks it up:

- **What `gameoverlayrenderer.so` exports** and could be called from the dummy window:
  `IsOverlayEnabled`, `BOverlayNeedsPresent`, `SetNotificationPosition`,
  `SetNotificationInset`. None says "the overlay is open"; `BOverlayNeedsPresent` only says it
  has something to draw.
- **Open or closed** comes from Steam callback 331 (`GameOverlayActivated_t`, see "Open and
  closed"), not from the library.
- **Opening it from outside** would go through the Steamworks friends interface
  (`ActivateGameOverlay` and its variants, `ISteamUtils::SetOverlayNotificationPosition` /
  `SetOverlayNotificationInset` for the corner) or by sending the hotkey; `steam://open/...`
  URLs were not checked for whether they open the in-game overlay or the desktop client.
- **Every such call needs the dummy window to exist first**: the library is only loaded in
  that process, and the calls act on the calling process's game. That is why none was built
  now; the user said *"i will then test it myself"* and decides afterwards what to add.

## Known limits

**A game that asks whether the overlay is enabled gets "no" (2026-11-27).** With control On
the overlay library is deliberately not loaded into the game, so
`ISteamUtils::IsOverlayEnabled()` answers false there. Seen by the user: a game's in-game
store refused a purchase, saying it "requires the Steam Community Overlay" and that the
in-game Steam Community should be enabled.

*Why "no" (static finding; `steamclient.so` was inspected on disk, never loaded):* it contains
the names `gameoverlayrenderer.so`, `IsOverlayEnabled`, `BOverlayNeedsPresent`,
`SetNotificationPosition`, `SetNotificationInset` and imports `dlopen`/`dlsym`, i.e. it looks
those functions up in the overlay library **inside the calling process**. The exact lookup (by
library name or among loaded objects) was not traced. The dummy window's process has the
library; the game's does not.

**The way out today:** put `GS_RITZ_STEAM_OVERLAY_DUMMY=0` into that game's Steam launch
option. That switches control off for the launch (see "Env override"), so the game gets the
normal in-game overlay and its store works.

**Not built: a stand-in library.** A small library preloaded into the game under the overlay
library's name, answering "enabled", would probably satisfy the check. Unproven: whether the
lookup accepts it, and whether Steam would then open the purchase page in the dummy window's
overlay (it may address the overlay by the game's process). `Why` it was not built: it would
put a custom library into games, anti-cheat-protected ones included, and change what the
Steam API reports there. The user: *"I dont want custom libraries, because of anti cheats."*
and, of the feature as it is: *"I guess it already does its job then."*

**The normal in-game overlay takes no input under gamescope-ritz (2026-11-27, not fixed).**
With control Off (Steam's library and Vulkan layer inside the game) the overlay opens, is
drawn and its hotkey works, but mouse clicks and typed text do not reach it. Seen
(user-observed) in a native Vulkan game and in a Proton game; the same native game without
gamescope-ritz works.

*Evidence (measured).* The overlay library's own log (`STEAM_OVERLAY_LOGGING=1` plus
`STEAM_OVERLAY_LOGGING_FLUSH=1`; `gameoverlayrenderer.<pid>.log` in the system temp
directory) shows in a working run, right after "Enabling overlay": "First swap of overlay
on <display> <window>", "using display ... window ... key input win ...", "Game process
locale ...". Under gamescope-ritz these lines never appear: the overlay never associates a
presented frame with the game's X window, so it sets up no keyboard or mouse input. With
the fork's WSI layer off for the game (`DISABLE_GAMESCOPE_RITZ_WSI=1`) the lines appear and
input works (user-confirmed).

*Cause, as far as established (read statically from Steam's layer, never loaded).* The
fork's WSI layer replaces the game's X11 Vulkan surface with a Wayland surface on
gamescope's own socket (it calls down `vkCreateWaylandSurfaceKHR`, with a fallback
`vkCreateXcbSurfaceKHR`; the swapchain lives on the Wayland surface). Steam's layer
remembers the X window of a surface only when it sees the X11 surface creation for the
surface the swapchain is built on. A Steam layer **below** the WSI layer sees only the
Wayland surface and skips its input set-up while still drawing.

*Not settled: whether it is only a layer-order problem.* Measured with stand-in layers: the
Vulkan loader does not sort manifests, the first-found implicit layer is closest to the
application, and within one directory the order is `readdir` order. Read from its source:
the Steam Runtime container (pressure-vessel) enumerates host layers along the search path
and sorts file names byte-wise per directory, so with both manifests in the user's data dir
the fork's (`VkLayer_RITZ...`) comes before Steam's (`steamoverlay...`), i.e. the fork's
layer above Steam's. But forcing the distribution's layer from `/usr/share` with
`GAMESCOPE_RITZ_USE_SYSTEM_WSI=1` did not help either (user test), which order alone does
not predict. The real layer order of a real launch was never captured (the
`VK_LOADER_DEBUG=layer` log is missing).

*A fix candidate that was not built:* the manifest in a directory of its own
(`<data>/gamescope-ritz/vulkan/implicit_layer.d/`) and the compositor appending
`<data>/gamescope-ritz` to the end of `XDG_DATA_DIRS` for the game, so the loader and
pressure-vessel find the layer last. Verified only on the bare loader with stand-ins, not
inside the container, and useless if the overlay fails with any gamescope WSI layer
regardless of order.

**Decision: not fixed.** `Why:` the user: *"Lets scrap the compatibility part.
gamescope-ritz only offers the DUMMY window steam overlay."* The supported usable Steam
overlay under gamescope-ritz is the dummy window (control On). Per-game way out for the
in-game overlay anyway: `DISABLE_GAMESCOPE_RITZ_WSI=1` in the launch options; the cost is
that the game presents through Xwayland instead of the layer's direct path, so everything
that depends on the layer is off for it (see the WSI layer entry in `build-and-tooling.md`).

*Not gamescope-ritz defects, found on the way.* A Proton game launched with
`--expose-wayland` and Proton's Wayland driver is a Wayland-native client, and Steam's
overlay library has no Wayland keyboard input (read statically). The suspicion that the
fork's pointer-lock gate in `wlserver_mousewarp` blocks overlay clicks was refuted:
`wlserver_pointer_stats` showed no lock and nothing suppressed.

## Environment switches and every knob

(`GS_RITZ_STEAM_OVERLAY_IDLE_CLOSE_MS` existed until 2026-11-27 and is gone: the real log proved
`BOverlayNeedsPresent()` cannot tell open from closed.)

| env | default | meaning |
|---|---|---|
| `GS_RITZ_STEAM_OVERLAY_DUMMY` | unset (the key decides) | override of Steam overlay control for this launch: `1` on, `0` off |
| `GS_RITZ_STEAM_OVERLAY_LOG` | stderr | file the dummy window appends its timestamped log to |
| `GS_RITZ_STEAM_OVERLAY_IDLE_FPS` | `1` | presents per second while the overlay needs none |
| `GS_RITZ_STEAM_OVERLAY_HOTKEY` | Steam's own | e.g. `Ctrl+Shift+Tab`; else `InGameOverlayShortcutKey` from the newest `localconfig.vdf`, else Shift+Tab |
| `GS_RITZ_STEAM_OVERLAY_NO_CALLBACK` | off | `1`: never load `steamclient.so`; open/closed is the plain hotkey toggle (for comparing) |
| `GS_RITZ_STEAM_OVERLAY_INPUT` | `grab` | how the helper takes input while the overlay is open: `grab` (`STEAM_INPUT_FOCUS=2` plus an active keyboard grab, the game keeps its X focus) or `focus` (`STEAM_INPUT_FOCUS=1`, the game loses X focus); see "Input modes" |
| `GS_RITZ_STEAMCLIENT` | search `<steam root>/linux64/steamclient.so` | full path of the `steamclient.so` the helper loads for the open/closed callback (same override as the main program) |
| `GS_RITZ_STEAM_OVERLAY_TEST_RECT` | off | `1`: one opaque orange rectangle in the top-left quarter; counts as "something to show", so the window stays opaque the whole time |
| `GS_RITZ_STEAM_OVERLAY_ACTIVE_POLL_MS` | `1` | main-loop wait in ms while the overlay is active and a frame is pending (4 ms, or this if larger, when none is); `16` reproduces the old ~60 fps limit; see "Overlay frame rate" |
| `GS_RITZ_STEAM_OVERLAY_MIRROR_KEYS` | on | `0`: no key mirroring (behaviour as before it existed: no root property selection, no foreign-window selection); see "Key mirroring" |

## How the window works

- One 32-bit ARGB window covering the root window, follows root size changes. Properties
  `STEAM_OVERLAY=1` (gamescope composites it above the game and never treats it as the game
  or focus window, `DetermineAndApplyFocus`) and `STEAM_INPUT_FOCUS=0`;
  `_NET_WM_WINDOW_OPACITY` follows the "Window opacity" rule below (0 while nothing shows).
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
  (`vkCmdClearAttachments`); the window starts and stays opaque then.

### Window opacity (2026-11-27)

The window writes `_NET_WM_WINDOW_OPACITY` (CARDINAL 32) itself: **0 while it has nothing to
show, `0xffffffff` while it has.** It starts at 0, set before the window is mapped
(gamescope reads the property at map), so the game is never held back at launch.
"Has something to show" = the input is held (open, confirmed or provisional), or the test
rectangle is on, or the overlay asks for frames *continuously* (a notification toast):
`BOverlayNeedsPresent()` true on at least 4 polls within 250 ms. The idle pattern (one
needs-present about every 2 s) does not count, or tearing would switch off and on every 2 s.
It is lowered 500 ms after the last moment any of that held (the closing animation is still
presented). Constants `k_nBusyPolls`, `k_flBusyWindow`, `k_flLowerAfter`
(`OpacityPolicy_t`). Raised before the input is taken and before the first visible frame. Each
change logs `window opacity -> opaque (open | test rectangle | overlay wants frames)` or
`-> 0 (nothing to show)`. A toast loses its first ~50 ms of fade-in; accepted.

*Why:* `steamcompmgr.cpp` `bHasOverlay` ("HACK: Disable tearing if we have an overlay", reads
`overlayWindow->opacity`) forces sync flips, switches off frame generation's above-refresh
pacing and counts for Auto V-Sync. The always-opaque window made that true for the whole
session: with tearing or frame generation above the refresh in use, the game would have been
held to the refresh for no reason. (This was first read as the user's "capped to 60 fps"
report; the user then clarified that the *game* is fine and the *overlay* is capped, which is
"Overlay frame rate" below.) What gamescope does with an
opacity-0 overlay window (read from the code): the property is read at map and on
`PropertyNotify` (`handle_property_notify`), the window is still picked as `overlayWindow`
(`opacity >= maxOpacity` with 0 >= 0) and still takes `STEAM_INPUT_FOCUS` (focus ignores
opacity), but `paint_all` skips it (`overlay->opacity` test) and the capture path too, so no
layer is composited; its commits no longer force a repaint (`w->opacity != TRANSLUCENT`
guard); a live change sets `hasRepaintNonBasePlane` when a game is focused. Whether the cap
really lifts on a tearing-capable host is **unproven** (headless has no tearing).
- Exits when the X connection goes away (IO error handler) or on SIGTERM.

### Overlay frame rate (2026-11-27)

The user: "Not the game is capped, thats fine. The overlay is capped" (to 60 fps). The log
showed `presents=58 needsTrue=58` per second while open or while a toast animated, and
`BOverlayNeedsPresent` flipping true again within 2 ms of every present: the overlay always
had the next frame ready and the only limit was the helper's own loop, one present per 16 ms
poll turn. **Rule:** while *active* (input held, or the overlay wants frames continuously, the
same state that raises the opacity) the loop waits 1 ms when a frame is pending and 4 ms
otherwise (`g_nActivePendingMs`, `g_nActiveIdleMs`; knob `GS_RITZ_STEAM_OVERLAY_ACTIVE_POLL_MS`
sets the first, the second is `max(it, 4)`), and **the FIFO swapchain does the pacing**: with
a frame ready every turn, the next acquire blocks until the compositor has released an image,
so presents follow gamescope's refresh. Idle and closed it stays the 16 ms poll, one present
when asked. Measured headless (stand-in library reporting needs-present continuously, RADV):
`-r 60` gave 60 presents/s, `-r 144` gave 144 (16 ms wait at `-r 144`: 90 to 94), about 2
wake-ups per present, helper CPU about 1%; idle unchanged (1 present/s). So no time cap and
no refresh lookup was needed. The compositor side: a commit on the overlay window sets
`hasRepaintNonBasePlane` (`steamcompmgr.cpp`, commit handling for `overlayWindow` when
`opacity != TRANSLUCENT`, ~9013-9016) and the paint scheduler paints on it (~11838), so the
overlay animates over an idle or paused game by itself. Note that an opacity-0 window's commits
do not (that is why raising the opacity first matters). Whether it really reaches the user's
refresh rate is for the user to read: the `presents=N` of the per-second line while open
should be about the screen's refresh rate.

### Hotkey

The game has the keyboard, so the dummy window would never see Steam's hotkey. It takes a
passive `XGrabKey` on the **root** window for the hotkey, with the NumLock and CapsLock
modifier variants, so the press is delivered to the dummy window's own Xlib event stream,
where the overlay's hooks look. A `BadAccess` on the grab is retried from the main loop every
250 ms for 5 s (logs a `WARNING ... retrying` once, then `hotkey grab succeeded after N
retries`, or the `ERROR: XGrabKey failed` line after the last try; every variant is ungrabbed
before a retry). `Why:` a quick Off-then-On starts the new helper while the old one, only sent
SIGTERM, may still hold the grab; without the retry the hotkey stayed dead until the next
toggle. Source order: env, then `InGameOverlayShortcutKey` in the
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
logged). Opening hands the input to the window (how: "Input modes" below), closing gives it
back and sets `STEAM_INPUT_FOCUS` to 0. It must be cleared, not just the window hidden:
a window with it set takes input even at opacity 0 ("The window mechanism" below).

**Authority rule.** Steam posts the Steamworks callback `GameOverlayActivated_t` (callback id
**331** = friends callbacks base 300 + 31, first payload byte `m_bActive`, 1 = opened,
0 = closed) whenever the overlay opens or closes, by whatever means (hotkey, Esc, its close
button, Steam opening it itself).

- A 331 always decides: active=1 is OPEN (it confirms a provisional take-over), active=0 is
  CLOSED.
- **Callbacks connected: provisional input.** A hotkey press that *reaches* the helper means
  the overlay library did not recognise it (it swallows the ones it does; in the second real
  test not one recognised press ever arrived after the first 331). While CLOSED such a press
  takes the input **provisionally** (grab or focus per input mode, exactly as OPEN) and starts
  a 3 s deadline (`k_flProvisionalSecs`, no knob). A 331 active=1 before it confirms; if none
  comes the helper gives the input back (ungrab first, then `STEAM_INPUT_FOCUS` 0) and logs
  `provisional input released: no GameOverlayActivated_t within 3 s`. A press while
  provisional releases at once. A press while OPEN is only logged. This holds before and
  after the first 331. *Why provisional and not "do nothing":* in both launches of the second
  test the overlay opened exactly when Ctrl, Shift and Tab then reached it through the active
  grab, so taking the input is the working fallback if key mirroring does not fix the first
  press; *why a deadline:* the first version left the input on the helper from the press to
  the 331 (about 2 s) with no overlay, and would have kept it forever had the overlay not
  opened (the user: "the input is on the steam overlay, but it isnt open").
- **No callbacks** (no library, missing symbol, pipe 0, user 0, `NO_CALLBACK=1`; each logged
  once): the plain hotkey toggle (press: closed to open, next press: open to closed; ignored
  with one log line when no overlay library is loaded or `IsOverlayEnabled()` is false). The
  helper never exits because of Steam.

**The callback pipe.** After startup, with an overlay library loaded, the helper `dlopen`s
`steamclient.so` (`RTLD_LAZY | RTLD_LOCAL`; path `GS_RITZ_STEAMCLIENT`, else
`<steam root>/linux64/` over the same roots as the hotkey lookup), requires the symbols
`Steam_CreateSteamPipe`, `Steam_ConnectToGlobalUser`, `Steam_BGetCallback`,
`Steam_FreeLastCallback`, `Steam_ReleaseUser`, `Steam_BReleaseSteamPipe` (flat C exports,
signatures and `CallbackMsg_t` layout under "Callback facts" below), creates
its own pipe, connects the global user and drains `Steam_BGetCallback` /
`Steam_FreeLastCallback` in the existing 16 ms main-loop wake-up, in the helper's own
thread (no new one). The queues are per pipe, so this cannot take a callback from the game's
pipe ("Callback facts" below). The pipe and user are released
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
detail ..` (one line each); the per-second `events: KeyPress=N KeyRelease=N` counts show how
many presses reached the helper's own window (the overlay lets them through while open).
The log never records which keys were pressed, only how many (the one exception is the
configured hotkey's own line `hotkey press delivered`). `Why:` the overlay is typed into
(Steam chat, search) and this log is the file a user hands over with a bug report, so a
per-key line would be a key log of private typing.

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

### Key mirroring (2026-11-27)

**What.** While closed the helper selects `KeyPress|KeyRelease` on the window that has the X
input focus (the game's; gamescope sets it with `XSetInputFocus`, `steamcompmgr.cpp`
~4764, and writes `_NET_ACTIVE_WINDOW` on the root right after). X delivers a copy of every
key event of the game to the helper's normal event loop, so the overlay library's Xlib hooks
see them; the game still gets its own. No grab, no synthetic event, no added latency. The
helper itself ignores them (event window neither its own nor the root) and only counts them:
per-second line `mirrored: KeyPress=N KeyRelease=N`, never a keycode (that would be a key log
of the user's typing). Target changes log `key mirroring: now on window 0x...`.

**How the target is tracked.** `PropertyChangeMask` on the root, re-query `XGetInputFocus` on
a `_NET_ACTIVE_WINDOW` change, plus a once-per-second re-query as a safety net (and for a game
window that does not exist yet at start). On a change the old window is deselected and the
new one selected; never None, PointerRoot, the root or the helper's own window (the old
target then stays). `XSelectInput` on a foreign window goes through **libX11's own function**
(same `dlopen`/`dlsym` as the grab; the overlay's hook may do anything with a foreign
window) and selects only the two key masks (a second client selecting `ButtonPress` gets
`BadAccess`). An X error handler swallows `BadWindow`/`BadMatch`/`BadAccess` from
`X_ChangeWindowAttributes` and only when the error's resource is a foreign window (never the
helper's own window or the root, so a real failure of its own `XSelectInput` still logs as an
`X error` line), logged once per window, so a destroyed game window cannot kill the helper. The passive hotkey grab on the root stays (it keeps the hotkey from the game).

**Why** (second real test, hotkey `Ctrl+Shift+Tab`): the first press of each launch reached
the helper through the passive grab but the overlay did not open; it opened only when the
next press arrived as Ctrl, Shift, Tab through the active grab. Every later press was
swallowed by the overlay's hook before the helper saw it. **Hypothesis (unproven; only real
Steam can show it):** the library keeps its own key state from the events in its hooks and
ignores the event's `state` field; while closed the helper only ever got the Tab press, so
it never saw Ctrl and Shift go down. (An alternative that fits too: it ignores the hotkey
until its window has had any keyboard or focus activity. Mirroring covers both.)

**Known limit (unproven).** X stops propagating a key event at the first window where some
client selected it. A game that selected key events only on an *ancestor* of the focus window
would lose them to our selection. In gamescope's Xwayland a client toplevel's only ancestor
is the root, so it should not happen; `GS_RITZ_STEAM_OVERLAY_MIRROR_KEYS=0` is the way out.

### Input modes: `grab` (default) and `focus`

**The problem.** The user's first real test said "it works, but it freezes the game". Besides
the stuck-open state (fixed by the callback above), a game that pauses on focus loss (Unity
unless "run in background" is set) pauses for as long as the overlay is open, because of what
gamescope does with `STEAM_INPUT_FOCUS=1` (read from `steamcompmgr.cpp`):

- `xwayland_ctx_t::DetermineAndApplyFocus` (~4578) picks the overlay window (`isOverlay`,
  `inputFocusMode`) as `inputFocus` (~4619); `keyboardFocusWin = inputFocus` (~4673), and
  only `inputFocusMode == 2` swaps it back to `focus.focusWindow` (~4677).
- With mode 1 it calls `XSetInputFocus(ctx->dpy, <overlay window>, ...)` (~4764): the game's
  window, on the **same** Xwayland server, receives `FocusOut` with mode `NotifyNormal`. It
  also writes `_NET_ACTIVE_WINDOW` = the overlay window (~4767-4769), hides the cursor (~4788)
  and, in the global pass, `wlserver_keyboardfocus(<overlay surface>)` (~5271), so the
  Wayland keyboard leaves the game's surface. No unmap and no `WM_STATE` change
  (`set_wm_state` only runs for the *focus* window, ~4658).
- Steam's own in-game overlay never takes focus from the game; upstream's Deck setup avoids
  it by construction (`--xwayland-count 2`: Steam's UI and the game on different Xwayland
  servers, `get_index() != 0` gives the `SteamControlled` strategy ~4629, and
  `XSetInputFocus` is per server connection).

**`GS_RITZ_STEAM_OVERLAY_INPUT=grab` (default).** On open the helper takes an active
`XGrabKeyboard` on its window (`owner_events` False, `GrabModeAsync` both) and writes
`STEAM_INPUT_FOCUS=2`. Mode 2 means "pointer to the overlay, keyboard stays with the game"
(the Deck's quick-access menu uses it; `steamcompmgr.cpp` ~4677, ~5248-5253, ~6441): no
`XSetInputFocus` on the helper, no Wayland keyboard move, so the game keeps its X focus.
The keys reach the helper because X delivers them to the grabbing client. On close it
ungrabs, then writes `STEAM_INPUT_FOCUS=0`; it also ungrabs on exit, and X drops the grab
with the window. Every grab logs its return code (`GrabSuccess`, `AlreadyGrabbed`, ...); a
failed grab logs it and falls back to `STEAM_INPUT_FOCUS=1` for that open, so the overlay
is never left without keys.

**`focus`** is the old behaviour (`STEAM_INPUT_FOCUS=1`, no grab), kept so one test session
can compare.

**Why the real libX11 functions.** The overlay library exports `XGrabKeyboard` and
`XUngrabKeyboard` itself (it tracks and suspends the game's own grabs while its UI is up).
A plain call from the helper would go through that hook and might be recorded and deferred
instead of executed. The helper therefore resolves both from libX11's own handle
(`dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD)` + `dlsym`, which does not see the
preloaded interposer) and logs which path it uses (`keyboard grab will use: ...`); if that
lookup fails it falls back to the plain call.

**The passive hotkey grab** is unchanged. Its activation already produces `FocusOut ...
NotifyGrab` and `FocusOut`/`FocusIn ... NotifyUngrab` on the dummy window (seen headless
without any helper keyboard grab), so such lines are not gamescope focus changes. The game's own window sees the same pair at each hotkey press (`FocusOut NotifyGrab`, then `FocusIn NotifyUngrab`), in either mode, since the grab is on the root window. Every
`FocusIn`/`FocusOut` log line ends with `(own keyboard grab: yes|no)`.

**Headless proof** (stand-in overlay library and `steamclient.so`; the "game" is a small Xlib
window printing the focus and key events it receives; open and close driven only by the
stub's 331):

| | `grab` | `focus` |
|---|---|---|
| game on open | `FocusOut mode NotifyGrab` only | `FocusOut mode NotifyNormal` |
| game on close | `FocusIn mode NotifyUngrab` | `FocusIn mode NotifyNormal` |
| keys sent while open | reach the helper, none reach the game | reach the helper, none reach the game |
| pointer motion while open | reaches the helper, not the game | reaches the helper, not the game |
| keys after close | reach the game again, not the helper | the same |

The `focus` column confirms the diagnosis (the game sees a plain focus loss); `grab` shows
no `NotifyNormal` event at all. Without the stand-in overlay library, `grab` mode ignores
the hotkey exactly as before and takes no grab.

**Proven by the second real test (2026-11-27):** 331 is delivered to the helper's own pipe;
the overlay accepts keys delivered through the helper's active grab (it opened and closed
about 30 times, every cycle driven by a 331); input returns to the game on every close;
notifications (toasts) do not take input.

**Unproven until the next real test:**
- that key mirroring makes the very first hotkey press open the overlay (the hypothesis
  above), and that the provisional-input fallback works against the real library;
- that the opacity-0 window lets a tearing or frame-generation game run above the refresh
  again, and that no toast or first open frame is visibly lost;
- that the overlay itself now moves at the screen's refresh (`presents=N` while open);
- what to read in the log: first press: `key mirroring: now on window ...`, per-second
  `mirrored: KeyPress=N`, then `callback 331 ... active=1` right after the press (worked), or
  `input taken PROVISIONALLY` then `provisional input released: no GameOverlayActivated_t
  within 3 s` (fallback, nothing opened); opacity lines `window opacity -> opaque (...)` /
  `-> 0 (nothing to show)`;
- whether a Unity game treats `FocusOut mode NotifyGrab` as a focus loss (SDL ignores it);
- pointer behaviour for a game holding a pointer lock: the cursor constraint is applied to
  the **keyboard** focus surface (`wlserver_keyboardfocus` with `bConstrain`,
  `wlserver.cpp` ~3633), so under mode 2 the game's lock stays active while the pointer
  focus moves to the overlay window. The cursor image comes from the same Xwayland context
  (`steamcompmgr.cpp` ~5226), and the cursor is hidden on the focus change (~4788). Whether
  the overlay's mouse is usable then is unmeasured; `focus` mode is the comparison.

**Reserve: (a) the dummy window on a second Xwayland server.** Needs the helper on another
`DISPLAY` and gamescope-ritz running two servers (`--xwayland-count 2`, the game on the second:
reshapes launch, focus selection and `DISPLAY` handling for everything). The passive
`XGrabKey` only works on the game's own server, so gamescope would have to inject the hotkey
(`wlserver_process_hotkeys`) into the helper's stream. Largest change, most risk; whether the
Wayland keyboard moving between two Xwayland surfaces makes the game's Xwayland emit a focus
loss is unmeasured. Only worth doing if `grab` fails on a real game.

## Layering (read from the paint code, nothing changed)

`STEAM_OVERLAY` windows wider than the root (or with input focus) become
`focus.overlayWindow`; `paint_all` pushes them as their own layer at `g_zposOverlay` (3):
above the game, the override and the external overlay (0 to 2), below the cursor (4), the
FPS HUD (6), toasts (7) and the settings overlay. Frame generation only substitutes layer 0
(`rendervulkan.cpp`, "FRAME GENERATION ... runs FIRST, on the RAW layer 0"), so the dummy
window's layer is never smeared. The alpha mode for overlay windows is premultiplied
(`cv_overlay_unmultiplied_alpha` is only set on some DRM paths).

**Cost of a visible window (read from the code; the cure is "Window opacity" above, since
2026-11-27, so this holds only while the window is opaque).** While the overlay window has non-zero opacity,
`steamcompmgr.cpp` `bHasOverlay` (~11679) is true. That forces `FlipType::Normal` (no
tearing, ~11739-11740), skips frame generation's async-flip path (~11727), and its tear
present / the auto-V-sync async path (~11793-11808, ~11881, ~11890). Each heartbeat commit
also forces a compose (`hasRepaintNonBasePlane`, ~9020-9038). An extra full-screen layer is
composited. Until 2026-11-27 the window was always opaque, so all of this held for the whole
session. Line numbers will drift; search the symbols.

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
shows through elsewhere; without it the picture is byte-identical to a run with control
off.

## What launch handling does with control off

Steam preloads its overlay into gamescope-ritz, but only the SDL backend can draw it.
`main()` resolves the backend (`Auto` via `auto_select_backend()`); on anything but SDL it
calls `Process::RestartWithoutSteamOverlay(argv)`: it stashes the original `LD_PRELOAD` in
`GAMESCOPE_STEAM_OVERLAY_LD_PRELOAD` (which doubles as the guard against exec'ing twice),
removes the overlay entries and re-execs `/proc/self/exe`. In `LaunchNestedChildren`,
`RestoreSteamOverlayPreload()` puts the stash back into `LD_PRELOAD` for the game, and if
there is no stash and the backend presents with a Vulkan swapchain (SDL) the overlay is
removed from `LD_PRELOAD` instead (the game must not draw it twice). mangoapp always has it
removed. The Vulkan layer variables are untouched, so whatever Steam set stays.

## Callback facts

Measured against a live Steam client for the (since removed) Friends list, kept here because
the helper's callback pipe relies on them:

- `steamclient.so` exports `Steam_BGetCallback` and `Steam_FreeLastCallback` as **flat C
  symbols with published signatures**, so pumping callbacks needs no interface vtable slot,
  no app id and no `SteamAPI_Init`. The six functions the helper uses:

  ```
  int32_t Steam_CreateSteamPipe();
  int32_t Steam_ConnectToGlobalUser( int32_t hPipe );
  bool    Steam_BGetCallback( int32_t hPipe, CallbackMsg_t *pMsg );
  void    Steam_FreeLastCallback( int32_t hPipe );
  void    Steam_ReleaseUser( int32_t hPipe, int32_t hUser );
  bool    Steam_BReleaseSteamPipe( int32_t hPipe );

  struct CallbackMsg_t { int32_t m_hSteamUser; int m_iCallback; uint8_t *m_pubParam; int m_cubParam; };
  ```

- Callback queues are **per pipe**: a pump on our own pipe cannot take anything from the
  game's.
- A 300 s pump on a pipe with no app id delivered 16512 callbacks, among them ids 304
  (`PersonaStateChange_t`, 499 times) and 336 (`FriendRichPresenceUpdate_t`, 368 times), both
  at exactly their published payload size (12 bytes), which is what shows they really are
  Steamworks callback ids.

## The window mechanism

gamescope already promotes an X11 window on its own Xwayland to a fullscreen interactive
overlay when it carries the right properties (the way Steam does on the Deck; not
restricted to Steam):

| property | value | effect |
|---|---|---|
| `STEAM_OVERLAY` | `1` | the window becomes `ctx->focus.overlayWindow`, painted on top of the game |
| `STEAM_INPUT_FOCUS` | `1` | mouse and keyboard both go to it |
| `STEAM_INPUT_FOCUS` | `2` | mouse to the overlay, keyboard stays with the game |
| `_NET_WM_WINDOW_OPACITY` | `0` / `0xFFFFFFFF` | hide / show |

(`GAMESCOPE_EXTERNAL_OVERLAY` is the paint-only sibling; input is never routed to it.)
Proved in 2026-09 with a browser window given these properties: it composited fullscreen over
a nested game, mouse and keyboard worked, opacity 0 showed the game again. **The trap:** with
opacity 0 but `STEAM_INPUT_FOCUS` still 1 the *invisible* window kept swallowing every
keystroke, because `DetermineAndApplyFocus` does not look at opacity. The hide path must
clear `STEAM_INPUT_FOCUS` as well.

## A launcher wrapper that clears `LD_PRELOAD`

Steam's overlay reaches a game only through `LD_PRELOAD` (`gameoverlayrenderer.so`). A
launcher wrapper that sets `LD_PRELOAD=""` (a "clear LD_PRELOAD" option) therefore turns the
Steam overlay off for the game, whatever gamescope-ritz does. The dummy window is the way
around it: it carries the overlay itself (the stash and fallback lookup above).

## Related

- [process-management](process-management.md) - the reaper the helper runs under.
- [unit-tests](unit-tests.md) - the `[steam-overlay]` cases.
