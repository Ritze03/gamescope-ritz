# Diagnostics

The Shell's **Diagnostics** area (`system.diagnostics`, OTHER rail group, last) checks setup
outside gamescope that costs frames without printing an error, says what is wrong and what to
set, and has a **Recheck all** button. Code: `src/Overlay/Diagnostics.{h,cpp}` (runtime),
`src/Overlay/DiagnosticsLogic.h` (pure: parsing and state decisions,
`tests/test_diagnostics.cpp`), `src/Overlay/PanelDiagnostics.cpp` (area). One check today.

## Hyprland direct scanout

Why it matters: under nested Wayland on Hyprland the host's own draw waits behind the game's GPU
job, so frames Frame generation makes are lost (shown about 160 uncapped). With
`render:direct_scanout = 2` and the output images of [backend-wayland.md](backend-wayland.md#direct-scanout-under-a-host)
the host puts gamescope's buffer on a plane and shown reaches 261-272. See
[frame-generation.md](frame-generation.md#why-the-host-loses-frames).

States (`ScanoutState`):

| State | When | Warns |
| --- | --- | --- |
| Not applicable | the backend cannot be scanned out by a host (`HostMayScanOutOutputImages()` false: DRM, headless, OpenVR, **SDL**) | no |
| Hyprland not detected | `HYPRLAND_INSTANCE_SIGNATURE` was not set when gamescope started | no |
| On | `render:direct_scanout` is 1 or 2 | no |
| Off | it is 0 | **yes** |
| Could not check | no `hyprctl`, no answer in 1 s, non-zero exit, no such option, unparsable output, a value other than 0/1/2 | no |

The backend decides before Hyprland does, so headless under a Hyprland session says "Not
applicable". `Why SDL is "not applicable" and not checked:` the SDL backend presents through a
window swapchain the host composites (it round-trips composited frames, see
[backend-sdl.md](backend-sdl.md)), and only the Wayland backend allocates output images a host
can scan out; telling an SDL user to turn the option on would promise a gain that does not come.
`Why "could not check" never warns as if off:` a missing `hyprctl` or an unknown option is not
evidence that scanout is off, and a false alarm under a game is worse than a silent miss.

### How it is read

`hyprctl -j getoption render:direct_scanout` prints `{"option": "render:direct_scanout", "int": 1,
"set": true }`; an unknown option prints the bare text `no such option` with exit 0. (`hyprctl
dispatch` cannot read options.) Only `"int"` is read. The call is `posix_spawnp` with a fixed
argv, no shell, stdin and stderr from `/dev/null`, every fd above 2 closed, its own session,
killed after 1 s. It runs on a **worker thread** that publishes one struct under a mutex
(`Diagnostics_View()` copies it): the compositor paints at 280 Hz and a blocking read on that
path would stall it.

`Why the environment is snapshotted at the top of main():` hyprctl needs
`HYPRLAND_INSTANCE_SIGNATURE` and `XDG_RUNTIME_DIR`, which gamescope never touches, but gamescope
does `unsetenv("WAYLAND_DISPLAY")` and sets `DISPLAY`, `XDG_*` and more for its own game.
`Diagnostics_CaptureHostEnv()` keeps the launch environment, and both hyprctl and the browser run
with it. For the browser it also drops what only makes sense for the game: `LD_PRELOAD`,
`LD_LIBRARY_PATH`, `ENABLE_GAMESCOPE*`, `GAMESCOPE_*`, `SDL_VIDEODRIVER`, `STEAM_GAMESCOPE_*`,
`PRESSURE_VESSEL*`, and so on (`DropForBrowser()`).

### Warning

The check starts on the first paint with a sized output (`Diagnostics_Tick()`, called every frame
from `SettingsOverlay_AddLayer()`). When it finishes with **Off**, `Tick()` raises one warning
toast (`Notifications::Show()` is paint-thread only, so the worker only sets a flag):
`Why one per session:` a repeating toast under a game is worse than the loss, and the Diagnostics
row stays as the record. There is no "don't show again" setting; the notification system has none
(it has a global mute, which also silences this). A manual **Recheck all** never toasts. The row
and the fix text stay until a recheck reads On.

### The area

Rows: **Direct scanout** (status; Details: the value read and the reason), **Recheck all**
(disabled while a run is going), **Hyprland render options** (opens
`https://wiki.hypr.land/configuring/core/config-options/#render`). Below them, when Off, the
fix: set `render:direct_scanout = 2` (games only; 1 is every fullscreen window) and reload, in
both forms Hyprland configs come in (Lua `render = { direct_scanout = 2 }`, hyprlang
`render { direct_scanout = 2 }`), and that screen capture turns scanout off while it runs. For the
other states the area shows the one-sentence reason.

The link runs `xdg-open <url>` the same way (fixed argv, no shell, the host environment above,
detached in its own session). Gamescope is a subreaper, so the child is reaped with a
non-blocking `waitpid` on later ticks. Opening a browser over a fullscreen game may take focus
from it; nothing opens by itself.

### Adding a check

`kChecks[]` in `Diagnostics.cpp` is a plain array of functions from `Facts` to a result; the
runner and Recheck go through it. Nothing else exists (no plugin interface) because one check
does not need it.

### Test hook

`GS_RITZ_DIAGNOSTICS_ASSUME_WAYLAND=1` makes the check treat the backend as applicable, so a
headless instance can run the whole path (spawn, timeout, toast, row) against a fake `hyprctl`
put early on `PATH`. It exists because the real condition needs a host compositor; it changes
nothing else.
