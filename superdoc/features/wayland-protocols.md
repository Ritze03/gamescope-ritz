# Custom Wayland Protocol Extensions

gamescope is a Wayland compositor, so most of its external surface is Wayland protocol, not a library API. Alongside the standard/upstream protocols it implements (`xdg-shell`, `linux-dmabuf`, etc. — listed in `protocol/meson.build`), it defines a set of **private, gamescope-specific extensions** under `protocol/gamescope-*.xml`. "Private" here is a real Wayland convention, not just a comment: each carries `<description summary="gamescope-specific ... protocol">This is a private Gamescope protocol. Regular Wayland clients must not use it.</description>`, meaning no ABI stability is promised and only gamescope's own client tooling is meant to bind them. This page indexes what each one exposes and who talks to it; deep implementation detail for the subsystem behind a protocol lives on that subsystem's own page (linked per-section below) rather than duplicated here.

All protocols listed here are wired into the build via `protocol/meson.build:13` (the `protocols` list), which generates both client- and server-side headers/sources (`protocols_client_src`, `protocols_server_src`) for every entry.

## Protocol → consumer map

| Protocol | Server-side implementer | Who binds it as a client |
| --- | --- | --- |
| `gamescope-control.xml` | `src/wlserver.cpp:1331 gamescope_control_impl` | `gamescopectl`, the Steam client — see [control-ipc.md](control-ipc.md) |
| `gamescope-swapchain.xml` | gamescope's compositor (buffer/present path) | the Vulkan WSI bypass layer, per-game — see [vk-wsi-layer.md](vk-wsi-layer.md) |
| `gamescope-xwayland.xml` | `src/wlserver.cpp` | superseded, see below |
| `gamescope-pipewire.xml` | `src/wlserver.cpp` (`gamescope_pipewire_interface`) | PipeWire-consuming screen capture clients — see [screen-capture-pipewire.md](screen-capture-pipewire.md) |
| `gamescope-input-method.xml` | `src/ime.cpp` | on-screen keyboard / IME clients — see [input-method-ime.md](input-method-ime.md) |
| `gamescope-action-binding.xml` | `src/WaylandServer/GamescopeActionBinding.h` | hotkey/action clients, e.g. `src/Apps/gamescope_hotkey_example.cpp` — see [input-emulation.md](input-emulation.md) |
| `gamescope-reshade.xml` | `src/WaylandServer/Reshade.h` | reshade effect-controlling clients — see [reshade-effects.md](reshade-effects.md) |
| `gamescope-private.xml` | `src/wlserver.cpp` | `gamescopectl` (convar execution) — see [control-ipc.md](control-ipc.md) and [scripting-convars.md](scripting-convars.md) |
| `color-management-v1.xml` | *(consumed, not served)* | gamescope itself, as a client of a host compositor — see below |
| `frog-color-management-v1.xml` | *(consumed, not served)* | gamescope itself, as a client of a host compositor — see below |

*Why the last two are inverted:* every other protocol here is one gamescope **serves** to its own client applications (games, IME clients, `gamescopectl`). `color-management-v1` and `frog-color-management-v1` are the opposite direction — gamescope's own [Wayland backend](backend-wayland.md) *binds* these off the **host** compositor's registry (`src/Backends/WaylandBackend.cpp:2540`, `frog_color_management_factory_v1_interface`) when gamescope itself runs nested inside another Wayland session, so it can negotiate its own output's color/HDR metadata with whatever's hosting it.

## Per-protocol summaries

### `gamescope-control.xml` — `gamescope_control` (v6)
General out-of-band control channel: feature discovery (`feature_support` event, an enum of optional gamescope capabilities), active-display info/refresh-rate list, target-refresh-cycle requests, screenshot capture, display sleep/wake, setting a color "look" (a 3D LUT), and app performance-stat queries. This is the surface documented in full on [control-ipc.md](control-ipc.md) — that page covers each request's exact semantics and the `gamescopectl`/Steam-client usage pattern.

### `gamescope-swapchain.xml` — `gamescope_swapchain_factory_v2` / `gamescope_swapchain` (v1)
Per-swapchain companion protocol used by the [Vulkan WSI bypass layer](vk-wsi-layer.md): `gamescope_swapchain_factory_v2.create_swapchain` mints one `gamescope_swapchain` object per Vulkan swapchain on a given `wl_surface`. That object carries `swapchain_feedback` (image count/format/colorspace/alpha/transform), `set_present_mode`, `set_present_time` (desired presentation time for a commit), `set_hdr_metadata` (HDR10 static metadata), and events for `past_present_timing`, `refresh_cycle`, and `retired`. This is the protocol XWayland-bypass presentation is built on; see [vk-wsi-layer.md](vk-wsi-layer.md) for the interception mechanism that drives it and [compositing-vulkan.md](compositing-vulkan.md) for how the compositor consumes the resulting buffers.

### `gamescope-xwayland.xml` — `gamescope_xwayland` (v1)
A single request, `override_window_content`, that lets an X11 client rebind which `wl_surface` XWayland associates with a given X11 window for buffer submission (input routing is untouched). *Why it still exists:* its own doc comment says it **"has been superceded by the 'gamescope-swapchain' protocol"** — it's legacy surface, kept for compatibility rather than actively designed against. Prefer `gamescope-swapchain.xml` for anything new.

### `gamescope-pipewire.xml` — `gamescope_pipewire` (v1)
Advertises a PipeWire stream node (`stream_node` event) that a client can connect to for screen capture, so a client doesn't need to hard-code discovery. Full capture pipeline detail: [screen-capture-pipewire.md](screen-capture-pipewire.md).

### `gamescope-input-method.xml` — `gamescope_input_method_manager` / `gamescope_input_method` (v3)
Lets a client register as gamescope's input method (e.g. an on-screen/virtual keyboard): text commit, string composition, action requests, and — since version 3 — direct pointer control requests (`pointer_motion`, `pointer_warp`, `pointer_wheel`, `pointer_button`). Implementation and usage: [input-method-ime.md](input-method-ime.md).

### `gamescope-action-binding.xml` — `gamescope_action_binding_manager` / `gamescope_action_binding` (v1)
Lets a client register a named, armable action bound to keyboard triggers (`create_action_binding`, `add_keyboard_trigger`, `arm`/`disarm`, `triggered` event) — the mechanism behind configurable hotkeys. A minimal reference client lives at `src/Apps/gamescope_hotkey_example.cpp`. Deeper coverage: [input-emulation.md](input-emulation.md).

### `gamescope-reshade.xml` — `gamescope_reshade` (v1)
Lets a client point gamescope at a ReShade `.fx` effect file, enable/disable it, and set uniform variables at runtime (`set_effect`, `enable_effect`, `disable_effect`, `set_uniform_variable`, with an `effect_ready` event once compiled). Full behavior: [reshade-effects.md](reshade-effects.md).

### `gamescope-private.xml` — `gamescope_private` (v1)
Deliberately unstable ("versioned with gamescope", no ABI guarantee) debug channel: a single `execute` request taking a convar name/value pair, a `log` event streaming gamescope's console output back to the client, and `command_executed` as an ack. This is what `gamescopectl`'s actual command-execution path uses (as opposed to `gamescope_control`, which it only uses for read-only info/feature queries) — see [control-ipc.md](control-ipc.md) and [scripting-convars.md](scripting-convars.md) for the convar system it drives.

### `color-management-v1.xml` / `frog-color-management-v1.xml`
Upstream-style (`wp_color_manager_v1`, …) and Frog-authored (`frog_color_management_factory_v1`, `frog_color_managed_surface`) color-management protocols. gamescope binds these as a **client** of its host compositor when [nested](backend-wayland.md), and also underpins its own [HDR/color-management](hdr-color-management.md) pipeline — that page owns the color-space/EOTF semantics; this entry only notes the protocol surface exists and where gamescope binds it (`src/Backends/WaylandBackend.cpp:2540`).

## Related links

- [control-ipc.md](control-ipc.md) — the `gamescope_control`/`gamescope_private` external-control surface in full.
- [vk-wsi-layer.md](vk-wsi-layer.md) — the `gamescope-swapchain` client (the Vulkan WSI bypass layer).
- [screen-capture-pipewire.md](screen-capture-pipewire.md), [input-method-ime.md](input-method-ime.md), [input-emulation.md](input-emulation.md), [reshade-effects.md](reshade-effects.md), [hdr-color-management.md](hdr-color-management.md) — subsystem pages behind their respective protocols.
- [backend-wayland.md](backend-wayland.md) — the nested-compositor backend that consumes `color-management-v1`/`frog-color-management-v1` as a client.
