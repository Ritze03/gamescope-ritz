# Input Emulation — libei virtual input server + libinput device stealing

Gamescope can act as an [libei](https://gitlab.freedesktop.org/libinput/libei) input
emulation server (letting clients like Steam Input inject synthetic mouse/keyboard/gamepad
events) and, separately, can grab raw device input directly via `libinput` for contexts
(e.g. VR) that have no normal seat/session to draw from.

## How it works

- `gamescope::GamescopeInputServer` (`src/InputEmulation.h:gamescope::GamescopeInputServer`) wraps a libei server socket.
  `Init` (`src/InputEmulation.cpp:GamescopeInputServer::Init`) creates the `eis` context and binds it to a socket
  path; `OnPollIn` (`src/InputEmulation.cpp:GamescopeInputServer::OnPollIn`) dispatches libei events — client
  connect/disconnect, seat bind — and on `EIS_EVENT_SEAT_BIND` creates a virtual
  "Gamescope Virtual Input" device exposing pointer, absolute-pointer, button, scroll and
  keyboard capabilities (same `OnPollIn`, the `EIS_EVENT_SEAT_BIND` case).
- Built only when compiled with libeis support: the implementation file is wrapped in
  `#if HAVE_LIBEIS` (top of `src/InputEmulation.cpp`), gated by the Meson feature flag
  `input_emulation` (`meson_options.txt:input_emulation`, description "Support for XTest/Input Emulation
  with libei").
- `gamescope::CLibInputHandler` (`src/LibInputHandler.h:gamescope::CLibInputHandler`) is a separate, independent
  path: it opens `udev` + `libinput` directly (`Init`, `src/LibInputHandler.cpp:CLibInputHandler::Init`) and
  assigns `seat0` so Gamescope can read raw mouse/keyboard/scroll events without a normal
  desktop session. `OnPollIn` (`src/LibInputHandler.cpp:CLibInputHandler::OnPollIn`) translates libinput pointer
  motion/button/scroll and keyboard events into `wlserver_mousemotion`/`wlserver_mousebutton`/
  `wlserver_mousewheel`/`wlserver_key` calls, taking `wlserver_lock()`/`wlserver_unlock()`
  around each. The header comment explains the intent: "Handles libinput in contexts where
  we don't have a session and can't use the wlroots libinput stuff... eg. in VR where we
  want global access to the m + kb without doing any seat dance."
  *Why:* `CLibInputHandler` is currently defined but not instantiated anywhere in this
  codebase (verified — no call site outside `LibInputHandler.cpp`/`.h`; `src/Backends/OpenVRBackend.cpp` only includes the header); it exists as an
  unwired building block for a VR-style global-input path rather than an active feature.
- Both classes derive from `IWaitable` (`src/waitable.h:IWaitable`), exposing `GetFD()`/`OnPollIn()`
  so they plug into Gamescope's epoll-based waiter machinery rather than blocking directly
  on a `poll()`/`read()` call themselves.

## Threading model (verified)

`GamescopeInputServer` is *not* run on its own OS thread doing a blocking read loop — it is
registered as an `IWaitable` with `gamescope::CAsyncWaiter g_LibEisWaiter( "gamescope-eis" )`
(`src/wlserver.cpp:g_LibEisWaiter`, registered with `g_LibEisWaiter.AddWaitable(...)` in `wlserver_init`).
`CAsyncWaiter` (`src/waitable.h:CAsyncWaiter`) does own a dedicated `std::thread` (`src/waitable.h:CAsyncWaiter::m_Thread`, started in its constructor), but that thread just runs a private epoll
loop that calls each registered waitable's `OnPollIn()` when its fd becomes readable — so
from the input-emulation code's point of view it *is* event-poll-driven (`IWaitable`), and
that poll loop happens to live on a small dedicated thread per `CAsyncWaiter` instance
(named `"gamescope-eis"` here) rather than sharing either of Gamescope's main
(`"gamescope-wl"`, Wayland-protocol dispatch) or steamcompmgr (`"gamescope-xwm"`,
render/present) threads.
`CLibInputHandler` is also `IWaitable`-shaped and intended to be driven the same way, but
since it has no current call site there is no live waiter to confirm which one it would be
added to.

## Using it

Enable at build time with `-Dinput_emulation=enabled` (Meson feature, `meson_options.txt:input_emulation`).
At runtime the libei server listens on a socket path passed to
`GamescopeInputServer::Init(pszSocketPath)`; clients speaking the libei protocol connect to
inject pointer/keyboard events as if from real hardware.

A separate, related mechanism is Gamescope's Wayland action-binding protocol for hotkeys —
see the example client `src/Apps/gamescope_hotkey_example.cpp`, which binds key combinations
via `gamescope_action_binding_manager` (a different protocol from libei input emulation, but
the nearest real example client shipped in-tree for input-adjacent client code).

## Options

| Config key | Default | Meaning |
| --- | --- | --- |
| `input_emulation` (Meson feature) | auto | Builds the libei-based `GamescopeInputServer` support (`meson_options.txt:input_emulation`). |

## Related links

- [steamcompmgr-focus](steamcompmgr-focus.md) — keyboard focus routing that emulated input events ultimately land on.
- [wayland-protocols](wayland-protocols.md) — the action-binding protocol used by the hotkey example client.
