# Input › General

`input.general` — the INPUT rail group's own General area (added 2026-09-27,
`src/Overlay/PanelInput.{h,cpp}`). Two switches, both about gamescope taking
exclusive hold of an input device away from the host desktop while running
nested (SDL or Wayland backend):

- **Force grab cursor** — moved here from `display.general` (`PanelDisplay.cpp`).
- **Force grab keyboard** — new.

The user's own words, verbatim (the request this area was built from):

> Input:
>   - General
>     - Force grab cursor (Remove from Display>General)
>     - Force grab keyboard
>   - Autoclicker
>   - Null binds

## Force grab cursor

Unchanged in every way except its location. Config field `gamescope.force_grab_cursor`
(`src/Config/ConfigSchema.h`), row id `input.force_grab_cursor` (renamed from
`display.force_grab_cursor` — nothing outside the Shell hardcodes the old id;
`scripts/settings-audit.sh`/`settings_audit.py` enumerate rows live via
`overlay_e2_dump_keys`, never a fixed list, so the rename needed no script change).
Live on every backend that has a host to grab the cursor from, through
`steamcompmgr_set_force_relative_mouse()` — issue #68's fix, unchanged by the move: this
routes through the one entry point `ShouldDrawCursor()`/the pointer-lock code actually
reads, never writes `g_bForceRelativeMouse` directly (a direct write has no live effect;
see `steamcompmgr.hpp`'s own comment on that function). See
[cursor-pipeline.md](cursor-pipeline.md) for what "grabbed" changes about which cursor is
drawn.

`Why moved:` the user asked for an INPUT rail group to hold "things that intercept
input" (see `superdoc/meta/TERMINOLOGY.md`'s Rail group entry) separately from DISPLAY's
own quick toggles (VRR, tearing, force-maximize) — this was the one setting in
`display.general` that fit input better than display.

## Force grab keyboard

New. Config field `gamescope.force_grab_keyboard`, row id `input.force_grab_keyboard`,
default off. Mirrors upstream's `-g`/`--grab` CLI flag's own runtime effect on
`main.hpp`'s `g_bGrabbed` — the same relationship Force grab cursor has to
`g_bForceRelativeMouse`.

### What it does, and how much of it is live, per backend

**Live on both nested backends as of 2026-09-27.** Before that date, Wayland only ever
changed the window-title suffix — see "History" below.

| Backend | Startup (`-g`, or the config field) | Toggling the switch while running |
|---|---|---|
| SDL (nested) | `SDL_WINDOW_KEYBOARD_GRABBED` set at window creation, `CSDLConnector::Init()` (`SDLBackend.cpp`) | **Live** — `SDLBackend_SetKeyboardGrabbed()` calls `SDL_SetWindowKeyboardGrab()` |
| Wayland (nested) | A real `zwp_keyboard_shortcuts_inhibit_v1` created for the connector's own surface + the host seat, `CWaylandConnector::Init()` (`WaylandBackend.cpp`) | **Live** — `WaylandBackend_SetKeyboardGrabbed()` calls `CWaylandBackend::SetKeyboardGrabbed()`, which creates/destroys the inhibitor |
| DRM / OpenVR / Headless (embedded) | `g_bGrabbed` has no consumer at all — there is no host to grab from | n/a |

**SDL, live path.** `SDLBackend.cpp` gained a new `SDLCustomEvents` member,
`GAMESCOPE_SDL_EVENT_KEYBOARD_GRAB`, mirroring the existing
`GAMESCOPE_SDL_EVENT_GRAB` (the *mouse* relative-mode grab
`CSDLConnector::SetRelativeMouseMode()` already drives — a different flag, force grab
*cursor*, not this row). `SDL_SetWindowKeyboardGrab()` must run on the SDL thread (the
same rule `RequestOutputSize()`'s own comment states for `SDL_SetWindowSize()`), so
`CSDLBackend::SetKeyboardGrabbed()` parks the requested value in an atomic
(`m_bKeyboardGrabbed`) and crosses over as that event; the event handler calls
`SDL_SetWindowKeyboardGrab()` and re-pushes `GAMESCOPE_SDL_EVENT_TITLE` so the
`"(grabbed)"` suffix follows. `PanelInput.cpp` reaches this through a free function,
`SDLBackend_SetKeyboardGrabbed( bool )`, declared ad hoc (no shared header — the same
convention `PanelDisplay.cpp` already uses for e.g. `set_color_sdr_gamut_wideness()`)
since `CSDLBackend` is a type local to `SDLBackend.cpp`'s own translation unit; it is a
no-op on every other backend (`dynamic_cast<CSDLBackend *>( GetBackend() )` fails).

**SDL, startup path.** `CSDLConnector::Init()` reads
`config::ResolvedSettings().gamescope.force_grab_keyboard` directly and, if `-g`/`--grab`
didn't already set `g_bGrabbed` true, seeds it from there before computing the window's
initial flags — the same "config seeds, an explicit CLI flag still wins" ordering
`force_grab_cursor`'s own startup seed uses in `main.cpp`'s
`apply_ritz_config_to_startup_state()`, just done locally in the backend.

**Wayland: implemented via `zwp_keyboard_shortcuts_inhibit_manager_v1`
(2026-09-27).** `protocol/meson.build` adds
`unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml` to its
protocol list, the same way `pointer-constraints`/`relative-pointer` are already wired
(see [backend-wayland.md](backend-wayland.md)). `CWaylandBackend::Wayland_Registry_Global()`
binds the manager as an **optional** global — a host that never advertises it is not a
hard `Init()` failure the way `m_pPointerConstraints` etc. are, it just means the switch
can never do anything live (`GetKeyboardGrabStatus()` reports `Unsupported`).

- **Object model.** `CWaylandBackend` owns `m_pKeyboardShortcutsInhibitManager` (the
  bound global) and `m_pKeyboardShortcutsInhibitor` / `m_pKeyboardShortcutsInhibitedSurface`
  (the current inhibitor, if any, and the surface it was created for) — the same shape
  `m_pLockedPointer`/`m_pLockedSurface` already use for the pointer-lock case just above
  it in the file. `CWaylandBackend::SetKeyboardGrabbed( wl_surface *, bool )` destroys any
  existing inhibitor first (the protocol raises an `already_inhibited` error if
  `inhibit_shortcuts` is called twice for the same surface+seat without a `destroy`
  between), then creates a new one via
  `zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts( manager, surface, seat )`
  when turning on. `CWaylandConnector::SetKeyboardGrabbed( bool )` — a plain method, not
  an `INestedHints` override, since that interface has no keyboard-grab hook — delegates
  to it with `m_Planes[0].GetSurface()`, the connector's own toplevel surface, the same
  way `CWaylandConnector::SetRelativeMouseMode()` delegates to the backend's pointer-lock
  equivalent.
- **Thread/lifecycle.** Runs on whatever thread calls it — the same "steamcompmgr thread"
  `SetTitle()`/`RequestOutputSize()` already call straight into libdecor from. This file's
  only *separate* dispatch thread is `CWaylandInputThread` (relative-pointer motion), which
  owns none of the state this feature touches (`m_pSeat`, the inhibit manager and the
  connector's surface are all main-connection state). `CWaylandConnector::Init()` seeds
  `g_bGrabbed` from config the same "config seeds, CLI wins" way SDL's `Init()` does, then
  calls `SetKeyboardGrabbed( true )` if it ended up set — so `-g`/`--grab` grabs at startup
  on Wayland now, not only on SDL.
- **`active`/`inactive` events.** `zwp_keyboard_shortcuts_inhibitor_v1`'s two events say
  whether the compositor is *actually* honouring the inhibitor right now — a user can
  disable it live via whatever mechanism the host offers (see the Hyprland section below).
  Both are logged (`xdg_log.infof`, category `xdg_backend`) and folded into
  `CWaylandBackend::GetKeyboardGrabStatus()`, an enum
  (`NotRequested` / `Unsupported` / `Requested` / `Active` / `Inactive`) read by the
  Diagnostics fact below through `WaylandBackend_GetKeyboardGrabStatus()` (a plain
  `const char *`, same ad hoc-free-function convention as `SDLBackend_SetKeyboardGrabbed`
  — a scoped enum crossing two independently-compiled ad hoc declarations would be its own
  hazard for no benefit over a string the row prints verbatim).
- **`"(grabbed)"` title suffix.** Unchanged in *when* it appears (still `g_bGrabbed`,
  reflecting the *request*), but as of this date it now also actually means something on
  Wayland: the suffix and the real inhibitor track the same flag, so seeing "(grabbed)" now
  corresponds to a real `inhibit_shortcuts` request having been made, not merely a label.

**The row's own Diagnostics fact** (`input.backend_grab_support`) reads
`GetBackend()->GetCurrentConnector()->GetName()` (`"SDLWindow"` vs `"Wayland"`) and:
- **SDL:** `"live on this backend (SDL)"`.
- **Wayland:** `"on this backend (Wayland): "` + one of `WaylandBackend_GetKeyboardGrabStatus()`'s
  strings — `"not requested"`, `"unsupported by this compositor (no keyboard-shortcuts-inhibit
  protocol)"`, `"requested; waiting for compositor confirmation"`, `"live (compositor
  honouring it)"`, or `"requested; compositor declined"` (an `inactive` event arrived).
- **DRM / OpenVR / Headless:** `"not applicable on this backend (embedded, no host to grab
  from)"`.

Never "applies at next launch" any more on a backend where the grab is actually live —
that phrasing only ever applied to Wayland before this row had a real implementation
there, and it overclaimed.

### Safety: this cannot lock you out

A host-level keyboard grab (SDL's `SDL_WINDOW_KEYBOARD_GRABBED`, or Wayland's
`zwp_keyboard_shortcuts_inhibit_v1`) stops the **host compositor's own** global shortcuts
(Alt+Tab, a Meta-key launcher, a compositor-bound screenshot key, ...) from firing while
gamescope has focus. Per the Wayland protocol's own doc comment, it changes **nothing**
about which `wl_keyboard` events gamescope's own process receives — gamescope reads every
key event itself first, forwards to wlserver, and wlserver's own keybind matching
(`RShift` → Open settings, the reserved `Ctrl+Alt+Shift+O`) runs upstream of anything the
game, the host compositor, or the grab could intercept. So the Shell, and the reserved
chord, are reachable at all times regardless of this switch, on both backends.

Two independent escapes exist if a grab ever feels stuck, one per backend, because only
the side holding the inhibitor can release it:

- **SDL:** **Super+G** (`LGUI+G` — `SDLBackend.cpp`'s `SDL_KEYUP` handler, `KEY_G`),
  **not Ctrl+G**; the row's help text says so explicitly, since Ctrl+G is the
  commonly-assumed default from other software and would be the wrong thing to type here.
- **Wayland:** the **host compositor's own** mechanism, since only it can lift an
  inhibitor it granted — gamescope has no protocol request to force this itself.
  **This user runs Hyprland** (`gamescope-ritz --expose-wayland -f -- …`, no `--backend`,
  so the Wayland backend auto-selects), and Hyprland honours
  `zwp_keyboard_shortcuts_inhibit_v1` by default. Hyprland's own escape is a **bind flag**:
  a `bind` in `hyprland.conf` marked with the **`p`** flag — per the [Hyprland Binds
  wiki page](https://wiki.hypr.land/0.42.0/Configuring/Binds/), `p` "bypasses app's
  requests to inhibit keybinds" — still fires while an inhibitor is active, so a bind like
  `bindp = SUPER, Escape, exec, hyprctl ...` (or any bind the user pre-flags this way)
  reaches Hyprland even with the grab on. This is a **compositor-side** escape the user
  sets up in their own Hyprland config, not something gamescope-ritz can add for them —
  gamescope-ritz's own contribution to "cannot lock you out" is that its OWN hotkeys never
  depend on the host at all (previous paragraph), and the switch's help text points at the
  `p` flag as Hyprland's own way out if a grab ever feels stuck.
- The switch defaults to **off** on both backends, and a fresh empty profile (`ritz_profile
  <name>` with no prior file) never has it set.

### History: how this got here

Until 2026-09-27, Wayland had no real implementation: `WaylandBackend.cpp` never bound
`zwp_keyboard_shortcuts_inhibit_manager_v1`, `g_bGrabbed` only ever reached
`CWaylandConnector::SetTitle()`'s `"(grabbed)"` suffix, and the Diagnostics fact said
"applies at next launch on this backend" for Wayland — which overclaimed, since it never
grabbed at all, on any launch. `Why implemented then and not before:` this is the backend
the user actually runs day to day (`--expose-wayland -f`, no `--backend`, so
`auto_select_backend()` picks Wayland whenever `$WAYLAND_DISPLAY` is set — see
[backend-wayland.md](backend-wayland.md#using-it)), so "Force grab keyboard" doing
nothing live for them specifically was the actual gap, not a hypothetical one.

### Fixed alongside this: profile-switch live-apply gap, and a first-load stomp

`force_grab_cursor` was already re-applied centrally on every profile switch by
`main.cpp`'s `ritz_apply_config_live()`, independent of which Shell area is currently open
(see [resolution-and-refresh.md](resolution-and-refresh.md)). `force_grab_keyboard` now
gets the same treatment: `ritz_apply_config_live()` sets `g_bGrabbed` and calls **both**
`SDLBackend_SetKeyboardGrabbed()` and `WaylandBackend_SetKeyboardGrabbed()` (each a no-op
on the backend that isn't running) whenever it runs with `bStartup == false` — i.e. on
every profile switch, from any Shell area, matching `force_grab_cursor`'s own shape
exactly.

Wiring that up **removed** a redundant, and actively harmful, compensating push that used
to live in `PanelInput.cpp`'s own `EnsureConfigLoaded()`: before this change, that
function pushed `g_bGrabbed = s_Settings.gamescope.force_grab_keyboard` (plus both backend
calls) on *every* reload — including its very first call ever, which fires the moment
*anything* first touches the E2 registry (the Shell's first draw, or a single
`gamescopectl overlay_e2_get`/`overlay_e2_dump_keys` against **any** row, not just this
one). That first-load push unconditionally overwrote `g_bGrabbed` with whatever the
**on-disk** config said, silently undoing an explicit `-g`/`--grab` CLI override the
backend had already correctly applied at startup. Measured live on the Wayland backend:
`-g` created and activated a real inhibitor at startup, and the very first
`overlay_e2_get` of the session (for an unrelated row, `hud.enabled`) destroyed it again a
moment later. Now that `main.cpp` owns re-applying this centrally and correctly on every
*real* profile switch, the redundant push was pure duplication that was also wrong on the
very first load — removed rather than special-cased, per the fix in `PanelInput.cpp`.
`force_grab_cursor`'s own equivalent push in the same function was left alone (out of this
change's scope; it predates this feature and has no CLI-flag equivalent to stomp the same
way).

## See also

- [Terminology's Rail group entry](../meta/TERMINOLOGY.md) — where INPUT sits among the
  Shell's six rail groups.
- [resolution-and-refresh.md](resolution-and-refresh.md) — Force grab cursor's own
  live-apply path, `ritz_apply_config_live()`.
- [cursor-pipeline.md](cursor-pipeline.md) — what "grabbed" changes about which cursor is
  drawn.
- [keybinds.md](keybinds.md) — the reserved chord and the Shell's own hotkeys, unaffected
  by either switch.
