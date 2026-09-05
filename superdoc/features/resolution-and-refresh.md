# Resolution and refresh at runtime (nested mode)

The Display > **Resolution** area of the settings overlay (`src/Overlay/PanelDisplay.cpp`,
`RegisterResolution()`) changes three things while gamescope and the game keep running.
Tracker: `../planning/requests-2026-09-05.md` item 7. Phase A was live behaviour with no
persistence; **Phase B**, below, persists game resolution and refresh across a restart.

## The three things, and which are live

| | CLI | What it is | Live? | How |
|---|---|---|---|---|
| **Game resolution** | `-w/-h` | The RandR screen Xwayland reports to the game; gamescope scales it to the window. | Yes | `steamcompmgr_set_nested_mode()` |
| **Refresh** | `-r` | The paced (fake-vblank) rate and the mode's advertised Hz. `0` = follow the host. | Yes | same call; `g_nNestedRefresh` |
| **Window size** | `-W/-H` | The size the host compositor gives gamescope's window. | **Request only** | `INestedHints::RequestOutputSize()` |

Nothing restarts: not gamescope, not Xwayland, not the game.

### Game resolution and refresh — `steamcompmgr_set_nested_mode(w, h, refresh_mHz)`

`src/steamcompmgr.cpp` (declared in `steamcompmgr.hpp`). Steamcompmgr thread only — the
Shell's setters already run there. It writes `g_nNestedWidth/Height/Refresh`, tells the
connector's `INestedHints` via `OnNestedRefreshChanged()` (see the SDL gotcha), then under
`wlserver_lock()` calls the **existing** `wlserver_set_xwayland_server_mode(idx, w, h, mHz)`
(`src/wlserver.cpp`) — the exact path the Steam Deck's `GAMESCOPE_XWAYLAND_MODE_CONTROL`
root atom uses. *Why a new function and not the atom:* the atom handler deliberately does
not touch `g_nNestedWidth/Height` (Steam owns those on the Deck); the fork's UI must, because
cursor-scale ratios, the layer-shell configure size and the area's own read-back come from
them.

The chain, verified in code:

1. `wlr_output_state_set_custom_mode()` + commit on the headless output backing Xwayland.
2. Xwayland updates its RandR screen — `xrandr` inside gamescope (`DISPLAY=:N`) shows the new
   `WxH@Hz` — and the X server sends a `ConfigureNotify` on the **root** window.
3. `configure_win()` (`steamcompmgr.cpp`, the `ce->window == ctx->root` branch) stores
   `root_width/height`, re-arms placement, `MakeFocusDirty()`.
4. `determine_and_apply_focus()` force-resizes the focused **fullscreen** game window to
   `root_width x root_height` (the `win_has_game_id()` branch).

Refresh needs no chain: `vblankmanager.cpp` re-reads `g_nNestedRefresh` every cycle and the
per-frame body recomputes `g_SteamCompMgrAppRefreshCycle` from it. The advertised mode
refresh is `g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh`, as wlserver's startup
path does — a 0 Hz mode would be invalid.

With `--xwayland-count > 1` only servers `#1..` get the new mode; `#0` is Steam's and the
per-frame output-changed block keeps it at the output size.

### Window size — `INestedHints::RequestOutputSize(w, h)` (`src/backend.h`)

Default no-op (OpenVR, whose "window" is HMD-sized). Physical pixels, matching what
`g_nOutputWidth/Height` read back as.

- **SDL** (`src/Backends/SDLBackend.cpp`): parks the size in two atomics and pushes
  `GAMESCOPE_SDL_EVENT_RESIZE`; the SDL thread leaves `FULLSCREEN_DESKTOP` (SDL ignores
  `SDL_SetWindowSize()` while it is set), converts pixels to points using the current
  pts/pixels ratio (HiDPI), and calls `SDL_SetWindowSize()`. The grant arrives as
  `SDL_WINDOWEVENT_SIZE_CHANGED` and lands in `g_nOutputWidth/Height` the normal way.
- **Wayland** (`src/Backends/WaylandBackend.cpp`): unsets fullscreen
  (`SetFullscreen(false)` + `UpdateFullscreenState()`), writes `g_nOutputWidth/Height`, and
  marks plane 0 `RequestDecorCommit()` so the next `Commit()` sends `libdecor_state_new(w,h)`
  — the same thing a host-initiated `LibDecor_Frame_Configure()` already does, with our
  number first. A floating window keeps it; a tiled host answers with its own configure,
  which overwrites `g_nOutputWidth/Height` again. Scale: physical in, logical on the wire
  via `CommitLibDecor()`.

The area never shows the requested size as fact. The `display.output_size` Choice reads back
`g_nOutputWidth/Height`: a preset shows as selected only while the window really is that
size, so a refused request falls back to **Follow window** and the Facts row shows the
host's answer. *Why:* a control that displays what was asked for while the window shows
something else is the "renders but does nothing" defect class (#25/#68) again.

## Honest limits (the help text says these too)

Not achievable at runtime in nested mode, and not promised anywhere in the UI or here:

- **Forcing a tiling host to honour a window size.** Best effort; the host decides.
- **Changing the host monitor's refresh.** A refresh above the host's rate means frames are
  paced faster than the screen can show them.
- **Guaranteeing a running game adopts the new mode.** Fullscreen windows are resized within
  a frame or two; windowed games with size hints keep their size; games that read the mode
  list once list the new mode only after a restart. Wording used: *"most games switch
  instantly; a few only list it after a restart."*
- `ui::Applies::NeedsRestart` exists in `Registry.h` but nothing renders it yet, so the
  caveat is a Facts line ("takes effect") and help text. Swap to the badge when the shell
  grows one.

## The SDL static gotcha

`SDLBackend.cpp` keeps `g_nOldNestedRefresh`, the focused refresh that `FOCUS_GAINED`
writes back over `g_nNestedRefresh` after `FOCUS_LOST` swapped in
`g_nNestedUnfocusedRefresh`. Set at SDL init only, it would silently revert a runtime refresh
change on the next focus regain. Fix: `INestedHints::OnNestedRefreshChanged(mHz)` (default
no-op) — SDL's implementation rewrites the static, now a `std::atomic<int>` because the SDL
thread reads it while the steamcompmgr thread writes it.

## UI rows (`display.resolution`, gated `AvailableWhen(GetNestedHints() != nullptr)`)

| id | kind | notes |
|---|---|---|
| `display.resolution.preset` | Choice | Native (window size at the moment of the pick), 3840x2160, 2560x1440, 1920x1080, 1600x900, 1280x720, Custom |
| `display.resolution.width` / `.height` | Stepper | 320–7680, step 8, `DisabledUnless` Custom; `width.lock_aspect` Param holds the ratio captured when the lock engaged (no drift) |
| `display.refresh` | Choice | Follow host, 60, 90, 120, 144, 165, 240, Custom |
| `display.refresh.custom` | Stepper | 24–500 Hz, `DisabledUnless` Custom |
| `display.output_size` | Choice | Follow window, 1920x1080, 2560x1440, 3840x2160, Custom → `RequestOutputSize()` |
| `display.output_size.width` / `.height` | Stepper | 320–7680, step 8, `DisabledUnless` Custom |
| `display.resolution_facts` | Facts | `Game sees WxH @ R Hz · window WxH · host R Hz`, from the game Xwayland root's `root_width/height`, `g_nNestedRefresh` (host when 0), `g_nOutputWidth/Height`, `g_nOutputRefresh`; plus "takes effect" and "applied via" lines |

"Game sees" reads the Xwayland root, not `g_nNestedWidth/Height`, because Steam's atom path
changes the former without the latter and the row exists to show the truth. Labels use a
plain `x`, not `×` — the overlay font is not known to carry that glyph.

Embedded (DRM) is a different feature for a later phase — `GetModes()` plus the
dynamic-refresh atom — not a disabled copy of this area.

## Phase B — persistence

Game resolution and refresh survive a restart; the window size deliberately does not (host
window rules are the right tool for that).

- **Write-back**: `ApplyNestedMode()` in `PanelDisplay.cpp` — the single write point for all
  three live values — also writes `GamescopeSettings::nested_width/height/refresh_hz` into the
  routed config (`config::EnqueueRoutedWrite()`) every time it runs, `0` meaning "as launched".
  Native resolution and Follow-host refresh both write `0`, not the live pixel size / Hz at the
  moment of the pick — `s_nResolutionChoice == kPresetNative` is what tells "Native" apart from
  a Custom pick that happens to match the output size; Follow-host already arrives as `nRefreshmHz
  == 0` from `SetRefreshChoice()`, no extra check needed.
- **Startup apply**: `main.cpp`'s `apply_ritz_config_to_startup_state()` sets
  `g_nNestedWidth/Height` when both `nested_width` and `nested_height` are nonzero, and
  `g_nNestedRefresh` (mHz — converted from the schema's Hz via `ConvertHztomHz()`) when
  `nested_refresh_hz` is nonzero. This runs before the getopt loop in `main()`, so an explicit
  CLI `-w`/`-h`/`-r` overwrites it unconditionally and always wins — the ordering is not
  incidental, it is why this function is called where it is.

## Related

- [backend-sdl.md](backend-sdl.md), [backend-wayland.md](backend-wayland.md) — the two
  `RequestOutputSize()` implementations.
- [steamcompmgr-focus.md](steamcompmgr-focus.md) — the force-resize the mode change relies on.
- [../planning/requests-2026-09-05.md](../planning/requests-2026-09-05.md) — item 7 scouting.
