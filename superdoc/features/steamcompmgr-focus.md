# steamcompmgr Focus — window and input focus arbitration

Decides, every reroll, which window is *the* focused window per output connector, which
window gets X11 keyboard focus, which gets the mouse, and which override/underlay/
decoration windows get composited on top of it. This is the fork's most heavily-patched
subsystem: five of the last ten commits on this codebase are focus-arbitration edge-case
fixes here or in the OpenVR backend that feeds it (see [backend-openvr](backend-openvr.md)).

## How it works

### The focus data model

- `focus_t` (`src/xwayland_ctx.hpp:27`) is the per-Xwayland-server focus snapshot:
  `focusWindow`, `inputFocusWindow`, `overrideWindow`, `overrideUnderlayWindow`,
  `overrideWindowMouse`, `decorationWindows`, plus a `ulCurrentFocusSerial` dirty-flag
  checked by `IsDirty()`.
- `global_focus_t : focus_t` (`src/steamcompmgr.cpp:846`) adds the cross-server view:
  `keyboardFocusWindow`, `fadeWindow`, `cursor`, and `ulVirtualFocusKey` — the connector
  this snapshot belongs to.
- `g_VirtualConnectorFocuses` (`src/steamcompmgr.cpp:870`) is
  `unordered_map<VirtualConnectorKey_t, global_focus_t>` — **one focus snapshot per
  output connector**, not one global snapshot. `GetCurrentFocus()` (`:871`) and
  `GetCurrentMouseFocus()` (`:882`) look up the map by the backend's current
  connector / current mouse connector respectively; `GetCurrentMouseFocus()` falls back
  to `GetCurrentFocus()` when there's no distinct mouse connector. *Why a map keyed by
  connector at all:* in VR (see [backend-openvr](backend-openvr.md)) keyboard and mouse
  focus can legitimately sit on different overlay connectors at once, and multiple
  physical outputs can each be showing a different app — a single global snapshot
  can't represent that.
- `focus_info` (`src/steamcompmgr.cpp:932`, a `ConCommand` — see
  [scripting-convars](scripting-convars.md) for the ConVar/ConCommand mechanism) sets
  `g_bPendingFocusInfo`, consumed on the steamcompmgr thread (`:9316`) to call
  `DumpFocusInfo()` (`:4376`). It prints the global focus window/input/keyboard/override
  windows, every connector's focus next to which one is current, the
  `gamescopeFocusedAppAtom`/`gamescopeFocusedWindowAtom` X properties Steam reads back,
  and — per Xwayland server — the *real* X11 keyboard focus (`XGetInputFocus`) next to
  the window gamescope *wanted* it on. *Why print both:* only the current connector's
  focus is published to Steam, so a correct focus that never made it into the
  properties, or a desired focus that X11 silently didn't honour, is invisible from the
  properties alone.

### The reroll pipeline

`determine_and_apply_focus` (`src/steamcompmgr.cpp:4434`) is the per-connector entry
point, called once per `global_focus_t` in `g_VirtualConnectorFocuses` whenever
`IsDirty()` is true. Per pass it:

1. Resets the snapshot but carries `focusWindow`, `ulVirtualFocusKey`, and
   `pVirtualConnector` forward from the previous pass.
2. Calls `xwayland_ctx_t::DetermineAndApplyFocus` (`src/xwayland_ctx.hpp:105`) for every
   Xwayland server whose local `focus.IsDirty()` is set, and
   `steamcompmgr_xdg_determine_and_apply_focus` (`:4356`) for the XDG-native path — both
   funnel into `pick_primary_focus_and_override` (`:3739`), which picks `focusWindow`
   and `overrideWindow` from the connector's possible-focus-window list according to the
   active `VirtualConnectorStrategy` (see [backend-openvr](backend-openvr.md) for the
   VR-specific strategies).
3. `pick_decoration_windows` (`:3671`) collects same-app, same-pid helper windows (e.g.
   Xalia's highlight overlay) that aren't the override window, are viewable and
   on-screen, to paint above the override.
4. `carry_override_underlay` (`:3706`) keeps the *previous* override window painted
   beneath a newly-arrived one, as `overrideUnderlayWindow`, while it's still the same
   pid and a good override candidate. *Why:* only one override paints per frame, so
   when a dialog's own popup (a combo box list, a nested warning box) grabs the
   override slot, the dialog underneath would otherwise blink out until the popup
   closes — this was breaking the Warframe launcher's settings dialog and dropping it
   from Steam's per-app PipeWire recording stream too.
5. Publishes the result back to Steam as X properties (`gamescopeFocusedAppAtom` /
   `gamescopeFocusedWindowAtom`) via `get_prop`/`XChangeProperty` on the root window.

### X11 keyboard focus: the toplevel-vs-subwindow problem

`xwayland_ctx_t::DetermineAndApplyFocus` (defined near `src/steamcompmgr.cpp:4173`-`:4219`)
is where the subtlety lives. Steam's CEF browser can focus a *subwindow* of its own
toplevel (e.g. a dropdown), and gamescope must not yank X11 focus back to the toplevel
on the next no-op reroll:

```cpp
if ( keyboardFocusWindow && ctx->currentKeyboardFocusWindow &&
     find_win( ctx, ctx->currentKeyboardFocusWindow ) == keyboardFocusWin )
    keyboardFocusWindow = ctx->currentKeyboardFocusWindow;   // keep the preserved subwindow
```

- *Why (`0f8dc34`, "re-apply keyboard focus to the preserved subwindow"):* the code
  already tracked the preserved subwindow so a no-op reroll wouldn't disturb it, but the
  actual `XSetInputFocus` call still targeted the toplevel unconditionally. When the
  apply path re-fired after CEF focused its browser subwindow, X11 focus snapped back to
  the toplevel and Steam's focus ring silently died until something else refocused it.
  The fix targets `keyboardFocusWindow` (the preserved subwindow when there is one)
  instead of the toplevel, and picks the revert mode to match:
  `RevertToParent` when targeting a subwindow, `RevertToNone` only when the target
  genuinely is the toplevel — so a destroyed subwindow reverts to its parent instead of
  stranding focus on `None`.
- *Why (`396794a`, "reclaim keyboard focus when it lands on None"):* an X11 client can
  drop keyboard focus to `None` directly, and with `RevertToNone` any revert (e.g. the
  subwindow being destroyed under the old code) also lands on `None`. The `FocusOut`
  handler (`src/steamcompmgr.cpp:7934`, in `xwayland_ctx_t::Dispatch()`) only corrected
  focus when it moved to a *different tracked window*; its guard compared
  `w->xwayland().id == ctx->currentKeyboardFocusWindow`, which never matched once focus
  was preserved on a subwindow (the tracked window is the *subwindow* id, but `w` here
  is looked up from the event's own window). The fix (`src/steamcompmgr.cpp:7940`)
  matches the stored window directly
  (`ev.xfocus.window == ctx->currentKeyboardFocusWindow`) as well as by toplevel, tracks
  focus moves that stay within the same toplevel, and — the actual bug fix — explicitly
  reclaims focus (`bSetFocus = true`) when the new real focus is `None`. Without this,
  focus dropped to `None` was never noticed and nothing ever took it back.
- The `bSetFocus` path at the bottom of `Dispatch()` (`src/steamcompmgr.cpp:8044`-`:8046`)
  applies the same toplevel-vs-subwindow revert-mode logic as the main apply path, via
  `find_win( ctx, ctx->currentKeyboardFocusWindow, false )` (the `false` disables
  recursing into children, so it only matches an exact toplevel — see `find_win`,
  `src/steamcompmgr.cpp:1338`) to decide `RevertToNone` vs `RevertToParent`.

### Two crash/undefined-behaviour fixes in the property path

- *Why (`1efc919`, "do not read zero-element properties in `get_prop`"):* `get_prop`
  (`src/steamcompmgr.cpp:3342`) calls `XGetWindowProperty`, which — as this fork
  discovered — hands back a non-null allocation even when the property holds **zero**
  elements (e.g. the focused-app atom when nothing is focused). The old code
  unconditionally `memcpy`'d `sizeof(unsigned int)` bytes out of that allocation
  regardless of `n` (the returned element count), reading uninitialized memory whenever
  focus was empty. The fix checks `n < 1` up front and returns the caller's default
  with `*found = false` instead of touching the buffer.
- *Why (`1f0321c`, "cope with a missing input focus window"):* `determine_and_apply_focus`
  publishes the focused Steam appID by reading `pFocus->inputFocusWindow->appID`, but a
  `global_focus_t` can validly hold a `focusWindow` while `inputFocusWindow` is null
  (nothing has claimed input focus yet) — that dereference crashed steamcompmgr outright.
  The fix guards the read: `focusedAppId` only gets set `if ( pFocus->inputFocusWindow )`,
  otherwise it stays zero and the atom is published with no elements — which is exactly
  the case the `get_prop` fix above makes safe to read back. These two fixes are a pair:
  the crash fix stops writing garbage, the `get_prop` fix stops a *different* caller from
  reading garbage that a zero-element write can produce.

### Override-window edge cases (context, not this run's headline fixes)

- `overrideWindowMouse` used to be assigned only while empty (`fa3cd15c`), so it latched
  the first popup an app opened and ignored every one after — fixed by assigning it
  unconditionally on every reroll so the mouse override always tracks the *current*
  override window, not the first one seen.
- Interactive override painting is clamped to the output origin (composited position
  only, not the X11 geometry) because some app dropdowns (WebView2-based launchers)
  position slightly outside the output; moving the actual X11 window instead makes some
  hosts close it outright.
- `log_focus`-scoped logging (`focus_log`, `src/steamcompmgr.cpp:126`) reports override
  and underlay slot changes with the window's override-redirect flag and pid, since
  keyboard focus no longer visibly follows helper windows — without it, override fights
  like the one `carry_override_underlay` fixes are invisible in the journal.

## Using it

- `focus_info` — dump the full focus state described above; run it from the in-app
  console (see [scripting-convars](scripting-convars.md)) or via `--convar-json` at
  startup.
- `log_focus` debug logging — enable to see override/underlay slot changes and
  `determine_and_apply_focus` reroll traces as they happen.
- steamcompmgr owns all of this state on its **own thread**, spawned at
  `src/main.cpp:1101` (`std::thread steamCompMgrThread( steamCompMgrThreadRun, argc,
  argv )`, named `"gamescope-xwm"`) — and that thread, not the main thread, is the one
  that owns the vblank-paced render/present loop (`paint_all()`,
  `src/steamcompmgr.cpp:2564`) alongside the X11 window management described above; the
  main thread only runs `wlserver_run()`'s Wayland-protocol dispatch loop after startup.
  See [architecture overview](../architecture/overview.md#threading-model) for the full
  threading picture. Backends signal a focus change across that boundary with `MakeFocusDirty()`
  (`src/steamcompmgr.cpp:831`, an atomic serial bump) followed by `nudge_steamcompmgr()`
  (`:7537`), rather than message-passing; the reroll itself only runs on the
  steamcompmgr thread on its next pass through `determine_and_apply_focus`.

## Related links

- [backend-openvr](backend-openvr.md) — the SteamVR-side focus grant and take-on-visible
  paths that feed `g_VirtualConnectorFocuses` in VR.
- [scripting-convars](scripting-convars.md) — the `ConVar`/`ConCommand` mechanism behind
  `focus_info` and `log_focus`.
- [Architecture overview](../architecture/overview.md) — process/threading model.
- [Terminology](../meta/TERMINOLOGY.md) — override window, underlay window, connector,
  `VirtualConnectorKey_t`, Xwayland ctx, ConVar/ConCommand.
