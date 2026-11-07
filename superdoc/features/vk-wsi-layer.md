# Vulkan WSI Bypass Layer — `VK_LAYER_RITZ_gamescope_wsi`

An implicit Vulkan layer that intercepts a game's swapchain creation and redirects it to render straight into gamescope-owned Wayland buffers, skipping XWayland's compositing/blit path entirely. This fork builds and installs its **own** layer under its own name (see [Layer identity](#layer-identity-and-why-it-is-not-upstreams-name)); upstream's is `VK_LAYER_FROG_gamescope_wsi`. It exists so games (most of which are Win32/Wine titles running under Xwayland) can present at low latency without an extra composite hop through the X server.

## How it works

- The layer is **not** linked into the `gamescope` binary. It is built as its own shared object, `layer/meson.build:gamescope_wsi_layer` (`shared_library( layer_file + '_' + cpu_family, ...)`, i.e. `libVkLayer_RITZ_gamescope_wsi_<family>.so`), and installed as a Vulkan **implicit layer** manifest `VkLayer_RITZ_gamescope_wsi.<family>.json` under `vulkan/implicit_layer.d` (generated from the template `layer/VkLayer_FROG_gamescope_wsi.json.in`, whose `@layer_name@`, `@layer_file@`, `@enable_env@` and `@disable_env@` placeholders `layer/meson.build` fills in). Only the *source* file keeps upstream's `VkLayer_FROG_gamescope_wsi` name. Games pick it up the normal Vulkan loader way — no gamescope-side wiring needed. *Why a separate .so instead of code in the compositor:* it runs inside the **game's** process, hooking the game's own Vulkan calls; it has nothing to do with gamescope's own rendering.
- Build-gated by `meson_options.txt:enable_gamescope_wsi_layer` (default on).
- The manifest template (`layer/VkLayer_FROG_gamescope_wsi.json.in`, keys `enable_environment` / `disable_environment`) makes it an opt-in layer: it only activates when `ENABLE_GAMESCOPE_RITZ_WSI=1` is set in the game's environment, and `DISABLE_GAMESCOPE_RITZ_WSI=1` forces it off. Both are resolved by the Vulkan loader from the manifest; the layer's own code never reads them. `src/steamcompmgr.cpp:LaunchNestedChildren` sets the enable variable for the compositor's children (see [Layer identity](#layer-identity-and-why-it-is-not-upstreams-name)). Even when active, `layer/VkLayer_FROG_gamescope_wsi.cpp:isRunningUnderGamescope` gates all behavior on `GAMESCOPE_WAYLAND_DISPLAY` being set and matching `WAYLAND_DISPLAY` — so the layer is a no-op passthrough for any Vulkan app not actually running inside a gamescope session (including gamescope's own child compositor use of Vulkan, filtered out by `layer/VkLayer_FROG_gamescope_wsi.cpp:isAppInfoGamescope`).
- **Interception mechanism:** the layer is generated from [vkroots](https://github.com/Joshua-Ashton/vkroots)-style override classes that vkroots dispatches to automatically for any matching entry point — `GamescopeWSILayer::VkInstanceOverrides` and `GamescopeWSILayer::VkDeviceOverrides` in `layer/VkLayer_FROG_gamescope_wsi.cpp`. There's no manual `vkGetInstanceProcAddr` trampoline table to maintain by hand.
  - `VkInstanceOverrides::CreateInstance` force-enables `VK_KHR_wayland_surface` and `VK_KHR_xcb_surface`, then opens a second Wayland connection straight to gamescope's own socket (`gamescopeWaylandSocket()` reads `GAMESCOPE_WAYLAND_DISPLAY`) and binds `gamescope_swapchain_factory_v2` off it (`GamescopeWaylandObjects`). *Why a second connection:* the game may already be talking to a different Wayland server (nested nested case, or none at all if it's an X11-only app) — the layer needs its own direct line to gamescope regardless of how the game itself connects.
  - `CreateXcbSurfaceKHR` / `CreateXlibSurfaceKHR` redirect X11 surface creation through `CreateGamescopeSurface`, which creates **two** underlying Vulkan surfaces per game window: a `wl_surface`-backed `VkSurfaceKHR` (the bypass path) and a real `VkXcbSurfaceCreateInfoKHR`-backed fallback surface tied to the actual X11 window. Both are cached in a `GamescopeSurface` map keyed by the surface handle the game sees.
  - `VkDeviceOverrides::CreateSwapchainKHR` is the core of the bypass: for each swapchain create call it decides `GamescopeSurfaceData::canBypassXWayland()` per-surface, then either points the real driver's `vkCreateSwapchainKHR` at the Wayland surface (bypass) or the XCB fallback surface (composited normally through XWayland). Either way the app only ever sees one `VkSwapchainKHR` handle; which underlying surface backs it is invisible to the caller. It also forces `presentMode` to `VK_PRESENT_MODE_MAILBOX_KHR` and `imageColorSpace` to `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` before calling the real driver, and re-implements the app's requested present mode/FIFO behavior itself over the `gamescope_swapchain` protocol instead (`gamescope_swapchain_set_present_mode`, called from `VkDeviceOverrides::QueuePresentKHR`). *Why:* gamescope needs uniform control over pacing/present-mode across all its client swapchains rather than trusting each driver's interpretation of FIFO/MAILBOX.
  - `GamescopeSurfaceData::canBypassXWayland()` is the eligibility gate: native Wayland clients always bypass; X11 clients only bypass if the window isn't obscured by any real child window bigger than 1x1 (composite redirect / GDI-blit dummy windows disqualify it) and isn't a Wine "offscreen presentation" window (`_WINE_ALLOW_FLIP=0`, or parented under a 1×1 override-redirect dummy — unless the format is HDR, which always forces bypass because the SDR blit path can't carry HDR). A per-client override, `GamescopeLayerClient::Flag::ForceBypass`, is read back from the X11 property `GAMESCOPE_LAYER_CLIENT_FLAGS` (read in `CreateGamescopeSurface`, flags defined in `src/layer_defines.h`) for games/tools that need to force the decision.
  - Once bypassing, the swapchain talks to gamescope over `protocol/gamescope-swapchain.xml` — `gamescope_swapchain_factory_v2` creates a `gamescope_swapchain` object per Vulkan swapchain, which then carries feedback (`swapchain_feedback`), present-time hints (`set_present_time`), past-presentation timing/refresh-cycle events used for frame pacing, and HDR metadata (`set_hdr_metadata`). See [wayland-protocols.md](wayland-protocols.md) for the protocol's full request/event surface.
- The result: for the common case (a fullscreen, unobscured game window), gamescope's compositor imports the game's Wayland buffers directly as a plane/layer, with no XWayland composite step and no extra copy — the entire point of "XWayland bypass".

## Layer identity, and why it is not upstream's name

| | This fork | Upstream / distro package |
| --- | --- | --- |
| Layer name | `VK_LAYER_RITZ_gamescope_wsi_<family>` | `VK_LAYER_FROG_gamescope_wsi_<family>` |
| Library | `libVkLayer_RITZ_gamescope_wsi_<family>.so` | `libVkLayer_FROG_gamescope_wsi_<family>.so` |
| Enable / disable | `ENABLE_GAMESCOPE_RITZ_WSI` / `DISABLE_GAMESCOPE_RITZ_WSI` | `ENABLE_GAMESCOPE_WSI` / `DISABLE_GAMESCOPE_WSI` |

*Why:* two manifests claiming one layer name collide, and the Vulkan loader resolves that by search order, so whichever gamescope ran got whichever layer was found first. A compositor of this fork run against an older distro layer lost every Vulkan client at once: `swapchain_feedback` gained a seventh argument upstream, the old layer sent six, and the compositor dropped the connection as a protocol error. With distinct names, file names and variables both layers can sit in the search path without shadowing each other, and the compositor enables only its own for its children, leaving the distro layer dormant for this fork and untouched for the packaged gamescope (`layer/meson.build` header comment). The compositor falls back to upstream's `ENABLE_GAMESCOPE_WSI` only when this fork's manifest is not on disk or `GAMESCOPE_RITZ_USE_SYSTEM_WSI=1` is set; the install, the fallback and its rationale are in [build-and-tooling.md](build-and-tooling.md).

## Frame-limiter pacing in the layer

The layer also paces Vulkan games for the compositor's frame limiter (commit `d83864c`): `gamescopeLimiterInfo` reads the limiter file named by `GAMESCOPE_LIMITER_FILE` (word 0 limiter on, word 1 the rate to pace to), and in `VkDeviceOverrides::QueuePresentKHR` the layer sleeps to an absolute deadline from `limiterpace::NextDeadline` (`src/LimiterPace.h`) after the present while reporting MAILBOX. Who paces, where it sleeps and why are documented in [resolution-and-refresh.md](resolution-and-refresh.md) (section "The limiter paces Vulkan games in the layer").

## Using it

Nothing to configure from a user's perspective under Steam/gamescope's normal launch path — the environment variables (`ENABLE_GAMESCOPE_RITZ_WSI`, `GAMESCOPE_WAYLAND_DISPLAY`) are set for the game by the compositor itself (`src/steamcompmgr.cpp:LaunchNestedChildren`). To debug or force behavior for a specific game:

- Set `DISABLE_GAMESCOPE_RITZ_WSI=1` in the game's environment to fall back to plain XWayland compositing (useful to rule out a bypass-related bug).
- Set the X11 window property `GAMESCOPE_LAYER_CLIENT_FLAGS` (bitmask of `GamescopeLayerClient::Flag` in `src/layer_defines.h`) to force `ForceBypass`, disable HDR, or opt into other per-client behaviors.
- Build-time debug tracing of the bypass decision is behind `GAMESCOPE_WSI_BYPASS_DEBUG` (compile define; see the `#if GAMESCOPE_WSI_BYPASS_DEBUG` blocks around `canBypassXWayland()`).
- Runtime `fprintf(stderr, "[Gamescope WSI] ...")` logging throughout the layer (swapchain creation, format negotiation, surface state dumps) is always on and is the primary way to see what the layer decided for a given game window.

## Options

| Env var / property | Meaning |
| --- | --- |
| `ENABLE_GAMESCOPE_RITZ_WSI=1` | Required for the implicit layer to activate at all (manifest `enable_environment`, set by `layer/meson.build:layer_enable`). |
| `DISABLE_GAMESCOPE_RITZ_WSI=1` | Forces the layer off even if enabled (manifest `disable_environment`, `layer/meson.build:layer_disable`). |
| `GAMESCOPE_RITZ_USE_SYSTEM_WSI=1` | Read by the compositor, not the layer: skip this fork's layer and enable the distro's instead (see below). |
| `GAMESCOPE_LIMITER_FILE` | Path of the compositor's limiter file, read per present by the layer (`gamescopeLimiterInfo`); see the pacing section below. |
| `GAMESCOPE_WAYLAND_DISPLAY` | gamescope's own Wayland socket name; also gates `isRunningUnderGamescope()`. |
| `GAMESCOPE_LAYER_CLIENT_FLAGS` (X11 window property, uint32) | Per-client override of `GamescopeLayerClient::Flag` bits (`DisableHDR`, `ForceBypass`, `FrameLimiterAware`, `NoSuboptimal`, `ForceSwapchainExtent`) — `src/layer_defines.h`. |
| `SteamAppId` | Read by `clientAppId()` (`layer/VkLayer_FROG_gamescope_wsi.cpp:clientAppId`) to identify the app for default flag heuristics and stats. |

## Related links

- [wayland-protocols.md](wayland-protocols.md) — the `gamescope-swapchain` protocol this layer speaks to the compositor.
- [compositing-vulkan.md](compositing-vulkan.md) — how gamescope's compositor consumes the buffers this layer hands it.
- [hdr-color-management.md](hdr-color-management.md) — the HDR colorspace/metadata handling referenced above.
- [build-and-tooling.md](build-and-tooling.md) — the `enable_gamescope_wsi_layer` build option and packaging.
