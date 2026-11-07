# Diagnostics

The Shell's **Diagnostics** area (`system.diagnostics`, OTHER rail group, last) checks setup
outside gamescope that costs frames without printing an error, says what is wrong and what to
set, and has a **Recheck all** button. **Since 2026-11-06 nothing warns** (see
[Warnings are switched off](#warnings-are-switched-off-2026-11-06)). Code: `src/Overlay/Diagnostics.{h,cpp}` (runtime),
`src/Overlay/DiagnosticsLogic.h` (pure: parsing and state decisions,
`tests/test_diagnostics.cpp`), `src/Overlay/PanelDiagnostics.cpp` (area). One check today.

## Warnings are switched off (2026-11-06)

`WarnsAbout()` in `DiagnosticsLogic.h` returns false for every state, so there is no startup
toast and no yellow rail dot. `Why:` on 2026-11-06, gamescope nested on Hyprland with direct
scanout on and a game running crashed the GPU 4 times in about 285 fullscreen entries (a gfxhub
write page fault from gamescope, then a cs2 gfx ring timeout, a failed ring reset and a full GPU
reset that killed the desktop). With `direct_scanout = 0` there were 0 crashes in 183 entries.
Giving the output image the screen's own format, so no format-change modeset happens, did not
stop it. **The root cause is unknown** and still being investigated. Telling users to turn the
option on is therefore harmful. The user: *"Yeah, turn off the warning for now."*

The toast, the dot (`Area::WarnWhen`, `Diagnostics_HasWarning()`), `AnyWarns()` and the check
itself are all kept. **To restore:** make `WarnsAbout()` `return e == ScanoutState::Off;` again,
restore the fix text in `PanelDiagnostics.cpp` (set `render:direct_scanout = 2`) and the toast
text in `Diagnostics.cpp`, and flip the assertions in `tests/test_diagnostics.cpp`. Do that only
once the crash is fixed. The area now says: Off is the safe setting (costs some generated
frames), and On carries a caution that it can currently crash the GPU. Both are plain text, not
a warning.

## Hyprland direct scanout

Why it matters (when it works): under nested Wayland on Hyprland the host's own draw waits behind the game's GPU
job, so frames Frame generation makes are lost (shown about 160 uncapped). With
`render:direct_scanout = 2` and the output images of [backend-wayland.md](backend-wayland.md#direct-scanout-under-a-host)
the host puts gamescope's buffer on a plane and shown reaches 261-272. See
[frame-generation.md](frame-generation-host-scanout.md#why-the-host-loses-frames).

States (`ScanoutState`):

| State | When | Warns |
| --- | --- | --- |
| Not applicable | the backend cannot be scanned out by a host (`HostMayScanOutOutputImages()` false: DRM, headless, OpenVR, **SDL**) | no |
| Hyprland not detected | `HYPRLAND_INSTANCE_SIGNATURE` was not set when gamescope started | no |
| On | `render:direct_scanout` is 1 or 2 | no |
| Off | it is 0 | no (until 2026-11-06 **yes**; see above) |
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

### Warning (switched off 2026-11-06)

The text below describes the machinery, which is intact but never fires while `WarnsAbout()`
is false.

The check starts on the first paint with a sized output (`Diagnostics_Tick()`, called every frame
from `SettingsOverlay_AddLayer()`). When it finishes with **Off**, `Tick()` raises one warning
toast (`Notifications::Show()` is paint-thread only, so the worker only sets a flag):
`Why one per session:` a repeating toast under a game is worse than the loss, and the Diagnostics
row stays as the record. There is no "don't show again" setting; the notification system has none
(it has a global mute, which also silences this). A manual **Recheck all** never toasts. The row
and the fix text stay until a recheck reads On.

### The area

Sheet rows: **Direct scanout** (status; Details: the value read and the reason) and **Recheck
all** (disabled while a run is going). The docs button is **not a sheet row**: it is the
Inspector-only verb `Entry::InspectorVerb()` of the Direct scanout row ("Hyprland render options",
chip **Open**, `https://wiki.hypr.land/configuring/core/config-options/#render`). It is drawn
under the title on that row's Inspector page (Details, which a Facts row opens on, and Configure)
and on the area's **Overview**, the page the area opens on, so it needs no row selected first.
Keyboard: Enter/Space on the focused Inspector row fires it (Overview: the area's first shown
verb). Shown while the check applies (state On, Off or Could not check); hidden when Pending,
Not applicable or Hyprland not detected. It is not a palette row (like every read-only row);
searching "diagnostics", "hyprland", "wiki" or "docs" finds the area, whose Overview carries it.
`Why:` the user, on the first version with the button in the sheet: *"It should only show this
button on the inspector rail on the right side"*. Below the rows, when Off: leave it off for now, what that costs (some generated
frames), and that scanout on can crash the GPU when entering or leaving fullscreen while a game
runs (until 2026-11-06 this was the fix `render:direct_scanout = 2` with config snippets). When
On: a short caution of the same crash. For the other states the area shows the one-sentence
reason.

The link runs `xdg-open <url>` the same way (fixed argv, no shell, the host environment above,
detached in its own session). Gamescope is a subreaper, so the child is reaped with a
non-blocking `waitpid` on later ticks. Opening a browser over a fullscreen game may take focus
from it; nothing opens by itself.

### The warning dot on the rail (switched off 2026-11-06)

Nothing warns now, so the dot is never shown; the machinery stays. While any diagnostic warns, the Shell's rail shows a plain filled yellow circle (`glyph::WarnDot()`,
`glyph::kWarnDotColor`, antialiased, no "!") on the **OTHER** group heading and on the
**Diagnostics** row. `Why:` the first cut was a drawn warning triangle with a "!", which looked
improvised; the user: *"The badge looks really bad. It doesnt look proper. Just make it a yellow
circle instead of the fake warning symbol."* The colour is a literal yellow, not `Role::Warn`
(orange).
`Diagnostics_HasWarning()` is derived from the published results (`AnyWarns()` over them, in
`DiagnosticsLogic.h`), not a separate flag, so a future check counts as soon as its result is in
the published range, and it clears when a recheck finds the problem fixed. Only **Off** warns:
"Could not check" does not, the same rule as the toast. It is wired through the generic
`Area::WarnWhen(fn)`; the rail asks it each frame it is drawn (one short mutex-guarded struct
copy, no child process, no repaint request: the worker already calls `force_repaint()` when a
result lands).

How it looks per rail state (`Shell.cpp`, `DrawRail()`):

| Rail state | Heading (OTHER) | Diagnostics row |
| --- | --- | --- |
| Full rail, group closed | dot right after the name, left of the chevron | not drawn (group closed, the heading carries it) |
| Full rail, group open | same | dot right after the row title |
| Icon-only rail | small badge on the icon's top-right | small badge on the icon's top-right |

The dot is drawn as a filled circle, not a font character: the bundled faces bake only Basic
Latin and Latin-1 (`Fonts.cpp`), so a glyph such as U+26A0 would be a box. It is placed after the name and clamped before the
chevron, so heading width and the accordion animation are unchanged. Not on the HUD or the
launcher. `Why:` the user: *"if something is active in the diagnostics, it should show a warning
sign on the OTHER category and the Diagnostics tab name, to tell the user, to look there."*

### Adding a check

`kChecks[]` in `Diagnostics.cpp` is a plain array of functions from `Facts` to a result; the
runner and Recheck go through it. Nothing else exists (no plugin interface) because one check
does not need it.

### Test hook

`GS_RITZ_DIAGNOSTICS_ASSUME_WAYLAND=1` makes the check treat the backend as applicable, so a
headless instance can run the whole path (spawn, timeout, toast, row) against a fake `hyprctl`
put early on `PATH`. It exists because the real condition needs a host compositor; it changes
nothing else.

## History

- **Diagnostics** (terminology entry, moved here 2026-11-07): **Diagnostics** — the Shell's `system.diagnostics` area, last in the **OTHER** rail group (2026-11-05; `src/Overlay/PanelDiagnostics.cpp`, `Diagnostics.{h,cpp}`, pure half `DiagnosticsLogic.h`): checks of the setup *around* gamescope that cost the user frames without any error, each with a plain state and what to change, plus a **Recheck all** button. One check today: whether Hyprland has `render:direct_scanout` on, read with `hyprctl` off the paint thread and only when gamescope runs nested through the Wayland backend (anywhere else it says "Not applicable" and never warns). Its warnings (a once-per-session toast, and a yellow dot on the OTHER heading and the Diagnostics row of the rail, both added 2026-11-05) are **switched off since 2026-11-06** because direct scanout on can crash the GPU (`WarnsAbout()` returns false; the machinery is kept); the docs link is an Inspector-only button, not a sheet row. The user's own wording: *"Under the 'OTHER' category, add the tab 'Diagnostics'."* Distinct from the **Log** (what gamescope printed) and from the `Diagnostics` *group* inside the Log area (its buffer facts).
