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

**This is not symmetric with Force grab cursor, and the row's own help text says so.**

| Backend | Startup (`-g`, or the config field) | Toggling the switch while running |
|---|---|---|
| SDL (nested) | `SDL_WINDOW_KEYBOARD_GRABBED` set at window creation, `CSDLConnector::Init()` (`SDLBackend.cpp`) | **Live** — `SDLBackend_SetKeyboardGrabbed()` calls `SDL_SetWindowKeyboardGrab()` |
| Wayland (nested) | `g_bGrabbed` only ever changes the `"(grabbed)"` window-title suffix (`CWaylandConnector::SetTitle()`) | Same — title only, no host-shortcut inhibition |
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
`apply_ritz_config_to_startup_state()`, just done locally in the backend instead of
centrally, since `main.cpp` is outside this feature's own scope of ownership (see "Known
follow-up" below for what that means).

**Wayland: not implemented, title only.** `WaylandBackend.cpp` never binds
`zwp_keyboard_shortcuts_inhibit_manager_v1` (or any other protocol) — there is no
registry entry for it in `CWaylandBackend`'s `wl_registry_bind()` calls. `g_bGrabbed`
only reaches `CWaylandConnector::SetTitle()`'s `"(grabbed)"` suffix. This was already
true of upstream's `-g` flag on the Wayland backend before this feature existed; this
row does not regress anything, it just doesn't add real grab behaviour there either.
Wiring the real protocol needs a client-protocol XML added to `protocol/meson.build`
(wayland-protocols ships
`unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml` already,
so the XML exists on the build machine — only the meson wiring and the
bind/create-inhibitor/destroy-inhibitor calls are missing) — outside this change's own
file scope, so it is not done here.

**The row's own Diagnostics fact** (`input.backend_grab_support`) reads
`GetBackend()->GetCurrentConnector()->GetName()` (`"SDLWindow"` vs `"Wayland"`, the two
nested connectors' own names) and prints "live on this backend (SDL)" or "applies at
next launch on this backend" — so the Shell tells the truth about what the switch is
about to do, rather than implying parity with Force grab cursor.

### Safety: this cannot lock you out

`SDL_WINDOW_KEYBOARD_GRABBED` stops the **host desktop's own** global shortcuts (Alt+Tab,
a Meta-key launcher, a compositor-bound screenshot key, ...) from firing while gamescope
has focus. It does **not** touch which events reach gamescope's own SDL event loop —
gamescope reads every key event itself first, forwards to wlserver, and wlserver's own
keybind matching (`RShift` → Open settings, the reserved `Ctrl+Alt+Shift+O`) runs
upstream of anything the game or the grab could intercept. So the Shell, and the reserved
chord, are reachable at all times regardless of this switch. If a grab ever feels stuck,
the SDL backend's existing release valve is **Super+G** (`LGUI+G` — `SDLBackend.cpp`'s
`SDL_KEYUP` handler, `KEY_G`), **not Ctrl+G**; the row's help text says so explicitly,
since Ctrl+G is the commonly-assumed default from other software and would be the wrong
thing to type here.

### Known follow-up: profile-switch live-apply has a gap

`force_grab_cursor` is re-applied centrally on every profile switch by `main.cpp`'s
`ritz_apply_config_live()`, independent of which Shell area is currently open (see
[resolution-and-refresh.md](resolution-and-refresh.md)). `force_grab_keyboard` has no
such central hook — `main.cpp` is outside this feature's own file scope — so a profile
switch that changes it only takes effect once `input.general` itself has drawn and its
own `EnsureConfigLoaded()` re-pushes it, the same staleness class issues #25/#68 were
before `PanelDisplay.cpp` grew `PushCachedSettingsToLiveState()` for its own fields. The
natural fix is adding this field beside `force_grab_cursor` in `ritz_apply_config_live()`;
left as a follow-up rather than done here.

## See also

- [Terminology's Rail group entry](../meta/TERMINOLOGY.md) — where INPUT sits among the
  Shell's six rail groups.
- [resolution-and-refresh.md](resolution-and-refresh.md) — Force grab cursor's own
  live-apply path, `ritz_apply_config_live()`.
- [cursor-pipeline.md](cursor-pipeline.md) — what "grabbed" changes about which cursor is
  drawn.
- [keybinds.md](keybinds.md) — the reserved chord and the Shell's own hotkeys, unaffected
  by either switch.
