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

`src/steamcompmgr.cpp` (declared in `steamcompmgr.hpp`). It writes
`g_nNestedWidth/Height/Refresh`, tells the connector's `INestedHints` via
`OnNestedRefreshChanged()` (see the SDL gotcha), then — with the wlserver lock held — calls
the **existing** `wlserver_set_xwayland_server_mode(idx, w, h, mHz)` (`src/wlserver.cpp`),
the exact path the Steam Deck's `GAMESCOPE_XWAYLAND_MODE_CONTROL` root atom uses.

*Two callers, opposite lock states.* The Shell's setters run on the steamcompmgr thread
**without** the lock; `gamescopectl overlay_e2_set display.resolution.aspect N` (or one of the
`preset_*` rows) reaches the same setter from `gamescope_private_execute()` (`src/wlserver.cpp`), which wlserver dispatches
**with** the lock already held. The lock is a plain non-recursive mutex, so the first
version — an unconditional `wlserver_lock()` — deadlocked the console path: the command never
returned and every later `gamescopectl` command queued behind it (laptop, 2026-09-05). The
function now takes the lock only if `!wlserver_is_lock_held()`, the same pattern
`wlserver_debug_key` / `wlserver_debug_mouse_button` use. The keyboard path (Left/Right on
the Choice row) reaches it too: `AdjustValue()` (`Registry.cpp`) writes the next option
through `Binding().Set()`, which is `SetAspectChoice()` / `SetSizeChoice()` → `ApplyNestedMode()`. *Why a new function and not the atom:* the atom handler deliberately does
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

### The pointer follows the change

The absolute-pointer mapping (force-grab off) is rebuilt from the painted base layer every
frame, so it follows the resized window on its own; what did not follow was the client's
pointer, which kept the old game-space position until the next host motion. Since item 10
(2026-09-05) `update_touch_scaling()` re-syncs it from the last host sample the moment the
mapping changes. Details, including the force-grab toggle and the X-side limit for windows
larger than the new root: [cursor-pipeline.md](cursor-pipeline.md), "The absolute pointer under
a stretched resolution".

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
| `display.resolution.aspect` | Choice | Native (window size at the moment of the pick), 16:9, 4:3, 16:10, 21:9, Custom — see [Two rows](#two-rows-aspect-then-size) |
| `display.resolution.preset_16_9` | Choice | 3840x2160, 2560x1440, 1920x1080, 1600x900, 1280x720, Custom; `DisabledUnless` Aspect is 16:9 |
| `display.resolution.preset_4_3` | Choice | 2880x2160, 1920x1440, 1440x1080, 1280x960, Custom; `DisabledUnless` Aspect is 4:3 |
| `display.resolution.preset_16_10` | Choice | 3840x2400, 2560x1600, 1920x1200, 1680x1050, 1440x900, 1280x800, Custom; `DisabledUnless` Aspect is 16:10 |
| `display.resolution.preset_21_9` | Choice | 5120x2160, 3440x1440, 2560x1080, Custom; `DisabledUnless` Aspect is 21:9 |
| `display.resolution.width` / `.height` | Stepper | 320–7680, step 8, `DisabledUnless` Custom (as the aspect, or within a shape's list); `width.lock_aspect` Param holds the ratio captured when the lock engaged (no drift) |
| `display.refresh` | Choice | Follow host, 60, 90, 120, 144, 165, 240, Custom |
| `display.refresh.custom` | Stepper | 24–500 Hz, `DisabledUnless` Custom |
| `display.output_size` | Choice | Follow window, 1920x1080, 2560x1440, 3840x2160, Custom → `RequestOutputSize()` |
| `display.output_size.width` / `.height` | Stepper | 320–7680, step 8, `DisabledUnless` Custom |
| `display.resolution_facts` | Facts | `Game sees WxH @ R Hz · window WxH · host R Hz`, from the game Xwayland root's `root_width/height`, `g_nNestedRefresh` (host when 0), `g_nOutputWidth/Height`, `g_nOutputRefresh`; plus "takes effect" and "applied via" lines |

### Two rows: aspect, then size

Tracker item 13 (2026-09-05). The user picks the **shape** first (Aspect row), then a
**size** from that shape's list; Custom stays. The four lists are exactly the user's.

*Why four size rows and not one whose list changes.* The Registry copies a Choice's
options at registration (`Entry::m_Options`, filled by `Area::Choice()`) and has no
per-entry visibility gate — `AvailableWhen` exists only on an Area. So "the list follows
the aspect" is one Choice per shape, each `DisabledUnless` its shape is the live one. All
five rows are dropdowns (labels over 8 characters, or more than 5 options, auto-downgrade
from segmented), so the three greyed rows are compact and each carries its reason
("pick 4:3 in Aspect above to use this list"). Every size row — live or greyed — shows the
live mode's entry when it is on that list and Custom otherwise; a row shows what *is*, never
a suggestion of what a pick would do.

*Why every list ends in Custom.* A live mode with the shape but on no list (a launch-time
`-w 1600 -h 1200`) has to be reflectable as "4:3 + Custom"; a dropdown with no matching
value draws an empty label, which is the "shows nothing" defect class again.

*Picking a shape applies nothing — the shape you pick stays selected until you choose a
size.* The Aspect row only decides which size list is enabled; the game's mode changes when
a **size** is picked (or a Custom stepper moves). Picking 4:3 over a live 1920x1080 changes
nothing on screen, `xrandr` still reports 1920x1080, the 4:3 row is enabled reading
"Custom" (no 4:3 entry is live) with the steppers at 1920x1080, and only picking 1440x1080
applies it. **Why:** changing the game's resolution as a side effect of browsing a list is
exactly the kind of surprise the user asked to have removed from the profile UI. Native and
Custom are sizes in their own right rather than shapes with a list, so they keep applying
as before (Native the window size; Custom the live size, a no-op on screen).

*Custom seeding.* Picking Custom — as the aspect, or inside a shape's list — seeds the
steppers from the live mode (which is the last preset applied) and re-captures the lock
ratio, so "16:10, then Custom" starts at a 16:10 size with the lock holding 16:10. Picking
Custom therefore changes nothing until a stepper moves. A stepper move under "4:3 + Custom"
stays recorded as 4:3 (the lock holds the ratio); under the Custom shape it stays Custom.

### The reflection rule (`CurrentAspect()` / `CurrentSizeChoice()`)

The rows read the live `g_nNestedWidth/Height`, never a stored pick, so a fresh open and a
change from outside (Steam's atom, a host resize) both show the truth:

1. An explicit pick is trusted **while the live mode is still the one it was made against**
   (`s_nPickWidth/Height`, captured at pick time — after the apply for a size pick, so it is
   recorded against the size it applied). This is what keeps a browsed shape selected, and
   what tells "Native" from "16:9 + 1920 x 1080" in a 1920x1080 window. Picking a size sets
   the pick to that size's shape. The trust ends the moment the mode changes for any other
   reason — a CLI/config apply, a per-game switch, Steam's atom.
2. Otherwise the live size is classified on its own: on any shape's list → that shape + that
   entry; equal to the window → Native; within 3 % of a shape's nominal ratio
   (`kAspectTolerance`; 21:9 is nominally 64:27 = 2.37 so the 2.37–2.39 panel sizes and true
   2.33 all qualify) → that shape + Custom; anything else → Custom.

So a persisted `nested_width/height` of 1280x960 reopens as Aspect 4:3, size 1280x960, with
no pick stored anywhere. Applying and persisting are unchanged: every path ends in
`ApplyNestedMode()` (Phase B below); `nested_*` write `0` for Native as before, keyed on
`s_nAspectChoice == kAspectNative`.

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
  moment of the pick — `s_nAspectChoice == kAspectNative` is what tells "Native" apart from
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
