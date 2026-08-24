# SDL Backend — gamescope as a window on a desktop session

The SDL2 backend runs gamescope nested inside an existing desktop session (X11 or SDL's
own Wayland driver) as an ordinary resizable window, rather than gamescope owning the
display outright. It's the fallback "nested mode" backend when Wayland-native nesting
(see [backend-wayland.md](backend-wayland.md)) isn't available or isn't chosen, and the
one most useful for desktop development/testing of gamescope itself.

## How it works

- Implemented in `src/Backends/SDLBackend.cpp`: `src/Backends/SDLBackend.cpp:132
  CSDLBackend` (a `CBaseBackend`) owns one `src/Backends/SDLBackend.cpp:60 CSDLConnector`,
  which doubles as the `INestedHints` implementation
  (`src/Backends/SDLBackend.cpp:60 CSDLConnector : public CBaseBackendConnector, public INestedHints`) —
  cursor image, window title/icon, relative-mouse mode, and clipboard/selection all route
  through `INestedHints` (`src/backend.h:254`) back into SDL window/clipboard calls.
  *Why:* nested backends need a way for `steamcompmgr` to push "OS-level" window hints
  (title, cursor shape, clipboard contents) down to the host window manager, which
  `IBackendConnector` alone doesn't model — `GetNestedHints()` is the hook
  (`src/backend.h:211`).
- **Settings-overlay cursor** — `INestedHints::PresentOverlayCursor( bool ) -> bool`, driven
  every frame from `paint_all()`. While the overlay owns the pointer, SDL shows
  `SDL_GetDefaultCursor()` (the *system* cursor) rather than the game's cursor image, and
  returns whether that cursor is actually on screen; the overlay turns ImGui's own software
  cursor off for exactly as long as the answer is true. *Why the system cursor and not the
  game's image:* a game's cursor can be a crosshair or fully blank, neither of which is usable
  overlay chrome. *Why the return value matters:* under a pointer grab
  (`SDL_SetRelativeMouseMode`) the OS cursor is hidden, so the answer is false and ImGui's
  cursor stays — that fallback is what keeps **exactly one cursor visible, never zero**
  (`#69`, revised by D29). The shared rule lives in `src/CursorPolicy.h`.
- SDL runs on its own dedicated thread: `m_SDLThread` is started in the
  `CSDLBackend` constructor (`src/Backends/SDLBackend.cpp:398`) and joined on
  destruction by pushing an `SDL_QUIT` event (`src/Backends/SDLBackend.cpp:402`).
  `Init()` (`src/Backends/SDLBackend.cpp:409`) blocks on
  `m_eSDLInit.wait(SDLInitState::SDLInit_Waiting)` until that thread finishes its own
  setup and flips `m_eSDLInit` to `Success`/`Failure`. *Why:* SDL's event pump and window
  APIs are expected to run from a single, consistent thread, but `IBackend::Init()` is
  called from gamescope's main thread — the dedicated thread plus atomic handoff is how
  the two are reconciled.
- Presentation is a real Vulkan swapchain, not a dmabuf hand-off:
  `CSDLBackend::UsesVulkanSwapchain()` returns `true`
  (`src/Backends/SDLBackend.cpp:497`), the window surface is created with
  `SDL_Vulkan_CreateSurface` (`src/Backends/SDLBackend.cpp:268`), and
  `CSDLConnector::Present` calls `vulkan_composite` then `vulkan_present_to_window`
  (`src/Backends/SDLBackend.cpp:342`). This is the key architectural contrast with the
  Wayland backend, which imports client buffers straight into the compositor via
  `zwp_linux_dmabuf_v1` and never touches a swapchain
  (see [backend-wayland.md](backend-wayland.md#how-it-works)).
- `ImportDmabufToBackend` (`src/Backends/SDLBackend.cpp:460`) is a stub returning a bare
  `CBaseBackendFb` — SDL has no scanout object of its own to import into (the
  composited result reaches the screen only via the swapchain present, not by handing
  a client dmabuf onward), so `UsesModifiers()` is `false` and
  `GetSupportedModifiers()` is always empty (`src/Backends/SDLBackend.cpp:465`, `:469`).
- `SupportsPlaneHardwareCursor()` is `false` (`src/Backends/SDLBackend.cpp:486`) — cursor
  rendering goes through the `INestedHints` cursor-image path instead of a hardware
  overlay plane, since there's no real display controller underneath to own a cursor
  plane. `SupportsTearing()` is also `false` (`src/Backends/SDLBackend.cpp:493`) — a
  desktop-hosted window can't do a tearing scanout the way a backend owning the CRTC can
  (contrast [backend-drm.md](backend-drm.md#how-it-works)'s `SupportsTearing()`, which is
  conditional on `g_bSupportsAsyncFlips`).
- `IsSessionBased()` returns `false` (`src/Backends/SDLBackend.cpp:502`) — SDL doesn't
  need the logind/DRM-master session handling that `CDRMBackend` requires.

## Using it

- Select explicitly with `--backend sdl`, gated at compile time behind
  `meson_options.txt:4 sdl2_backend` and `#if HAVE_SDL2` in
  `src/main.cpp:435-438 parse_backend_name` / `src/main.cpp:983-987`.
  `src/main.cpp:453 auto_select_backend` falls back to `SDL` when `$DISPLAY` is set but
  `$WAYLAND_DISPLAY` isn't (i.e. a plain X11 session) — see
  [backend-wayland.md](backend-wayland.md#using-it) for the Wayland-preferred case.
- Window behavior CLI flags (parsed in `src/main.cpp`, consumed in
  `CSDLConnector::Init`, `src/Backends/SDLBackend.cpp:226`):
  - `-b, --borderless` → `g_bBorderlessOutputWindow`, adds `SDL_WINDOW_BORDERLESS`
    (`src/Backends/SDLBackend.cpp:248`).
  - `-f, --fullscreen` → `g_bFullscreen`, adds `SDL_WINDOW_FULLSCREEN_DESKTOP`
    (`src/Backends/SDLBackend.cpp:251`); also toggled live at runtime with a key binding
    that calls `SDL_SetWindowFullscreen` (`src/Backends/SDLBackend.cpp:767`).
  - `-g, --grab` → `g_bGrabbed`, adds `SDL_WINDOW_KEYBOARD_GRABBED`
    (`src/Backends/SDLBackend.cpp:254`); also toggled live
    (`src/Backends/SDLBackend.cpp:794`).
  - `--display-index` → `g_nNestedDisplayIndex`, picks which physical monitor the SDL
    window opens on (`src/Backends/SDLBackend.cpp:259-260`).
  - `-W`/`-H`/`-r` (nested width/height/refresh) size the window itself, same as every
    nested backend — defaults to 720p @ 60 Hz, 16:9, if unset
    (`src/Backends/SDLBackend.cpp:232-244`).
- On the auto-select Wayland path, if the Wayland backend's `Init()` fails (e.g. no
  Wayland compositor actually reachable despite `$WAYLAND_DISPLAY` being set), gamescope
  falls back to constructing `CSDLBackend` right there in the same switch case
  (`src/main.cpp:997-1002`) — SDL is effectively the safety net under the Wayland
  backend, not just an independent selectable mode.

## Related links

- [backend-wayland.md](backend-wayland.md) — the other desktop-nested backend; SDL is
  the fallback for it both at auto-select time and at init-failure time.
- [backend-drm.md](backend-drm.md) — the embedded/owns-the-display counterpart; contrast
  hardware cursor planes, tearing, and session handling.
- [../meta/TERMINOLOGY.md](../meta/TERMINOLOGY.md) — "nested mode" vs "embedded mode".
