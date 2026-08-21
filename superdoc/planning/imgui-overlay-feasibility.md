# ImGui Settings Overlay — Feasibility

## Verdict

**Awkward but tractable — not hard.** The compositor already has three of the
five hard prerequisites sitting unused: a graphics-capable "general" queue +
command pool alongside the compute queue (`CVulkanDevice::generalQueue()`,
`src/rendervulkan.hpp:840`/`842`/`844`), `VK_KHR_dynamic_rendering` requested
at device-creation time (`src/rendervulkan.cpp:621`), and a `createFlags`
struct on `CVulkanTexture` that already supports
`VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` (`src/rendervulkan.hpp:144`/
`src/rendervulkan.cpp:2048`). None of that is wired to anything today — it's
latent capacity, not a finished path — but it means ImGui's Vulkan backend
doesn't need a new render pass or a second logical device, only a new
consumer of infrastructure that already exists. The genuinely hard part is
**not** rendering ImGui, it's **input**: gamescope has no concept of "steal
all keyboard+mouse from the focused game window into an in-process UI
without a `wl_surface`" — that has to be built from primitives that don't
quite fit (see Q4). The toggle hotkey is easy (Q5) because there's a direct,
already-used precedent for a global-hotkey bypass outside the normal focus
pipeline.

## 1. Where does the overlay get rendered?

Evaluated against `paint_all()` (`src/steamcompmgr.cpp:2564`) and
`vulkan_composite()` (`src/rendervulkan.cpp:4030`):

- **(a) An additional `FrameInfo_t` layer, recommended.** `paint_all()`
  already has a precedent for constructing a raw `Layer_t` by hand with no
  backing client window: the "HACK HACK HACK" blank-texture overlay layer at
  `src/steamcompmgr.cpp:2745`-`2775`, built when the Steam overlay client
  isn't present but a placeholder layer is still needed to avoid a present
  stutter. It manually fills `scale`, `offset`, `opacity`, `zpos`,
  `eAlphaBlendingMode`, `colorspace`, and `tex`, then `frameInfo.layers.push()`s
  it — exactly the shape an ImGui layer would take. `k_nMaxLayers` is 6
  (`src/rendervulkan.hpp:38`) and the layer-order comment at
  `src/rendervulkan.hpp:26`-`38` already reserves a slot pattern ending in
  "Primary Overlay (Steam Overlay)" / "Cursor" — an ImGui settings layer
  would sit at or above that zpos. `nReservedLayers` budgeting
  (`src/steamcompmgr.cpp:2718`-`2725`) already exists to guarantee the
  overlay/notification/cursor layers aren't starved by decoration windows; a
  new reserved slot for the settings overlay follows the same pattern.
- **(b) A separate Vulkan render pass before `Present()`.** Not needed as a
  *distinct* pass — ImGui's own draw pass (via dynamic rendering into an
  offscreen `CVulkanTexture`) happens **before** `vulkan_composite()`, on a
  different queue, and the *result* is consumed as a normal sampled texture
  by (a). So this isn't really a competing option, it's the mechanism that
  produces the texture (a) composites — see Q2/Q3.
- **(c) Reuse the "external overlay" window machinery.** Rejected as the
  primary path, though it's informative. Gamescope already composites a
  privileged overlay layer sourced from an *external client window*: any
  X11 window with the `GAMESCOPE_EXTERNAL_OVERLAY` property
  (`src/steamcompmgr.cpp:1122`, `EXTERNAL_OVERLAY_PROP`) is tracked as
  `pFocus->externalOverlayWindow` and painted via `paint_window()` at
  `src/steamcompmgr.cpp:2765`-`2775`, unscaled, with input-focus wiring
  already in place (`if ( externalOverlay == pFocus->inputFocusWindow ... )`
  at `:2772`). This is the mechanism mangoapp/MangoHud-style overlays use.
  It would work for an ImGui overlay shipped as a **separate process** that
  connects to gamescope's Wayland socket like any other client — but that
  means a second binary, IPC to actually change gamescope's own ConVars,
  and no direct access to `CVulkanDevice`'s internal state (queues,
  textures) the way an in-process overlay gets for free. Worth flagging to
  the user as a real alternative architecture (see Open Questions), but not
  what this doc assumes as the default.
  *External research, checked 2026-08-21:* mangoapp (the companion process
  MangoHud uses under gamescope) is the concrete real-world example of this
  path, and its own source clarifies it's a **transparent GLFW/OpenGL
  window**, not a Wayland-protocol client speaking gamescope's own
  IPC — it registers itself with the `GAMESCOPE_EXTERNAL_OVERLAY` X11
  property (`src/app/main.cpp:247`-`259` in
  [flightlessmango/MangoHud](https://github.com/flightlessmango/MangoHud/blob/main/src/app/main.cpp)),
  and separately exchanges frame-timing/control data with gamescope over a
  **SysV IPC message queue** (`mangoapp_proto.h`, per
  [MangoHud's own gamescope-integration writeup](https://deepwiki.com/flightlessmango/MangoHud/5.1-gamescope-integration)),
  not the `gamescope-control`/`gamescope-private` Wayland protocols this
  repo's own [wayland-protocols.md](../features/wayland-protocols.md)
  documents. That's a second viable IPC shape for option (c) beyond "just
  another Wayland client" if this path is chosen. Critically — and this is
  the part I verified against **our own source**, not the external
  writeup — mangoapp's own code also sets a `GAMESCOPE_NO_FOCUS` X11
  property on itself (`src/app/main.cpp:381`-`384`) to mark itself
  non-interactive, but `grep -rn "NO_FOCUS" src/` in this repo finds **no
  reader for that property at all** — gamescope-ritz never looks for it.
  Mangoapp is safely non-interactive here for a different, structural
  reason: `xwayland_ctx_t::GetPossibleFocusWindows`
  (`src/steamcompmgr.cpp:3956`-`3987`) unconditionally excludes any window
  with `isExternalOverlay` set from `vecPossibleFocusWindows` at all
  (`:3961`, "Always skip system tray icons and overlays") — before
  `pick_primary_focus_and_override` (`:3739`) ever runs. So the existing
  external-overlay path isn't merely "usually non-interactive," it's
  **structurally excluded from ever becoming focusable** by this fork's own
  candidate-collection code. That means option (c) is proven prior art for
  *rendering* an overlay this way, but zero prior art for *making one
  interactive* — an interactive external-overlay window would need this
  exclusion actively carved around (e.g. a new window flag distinct from
  `isExternalOverlay` that's eligible for focus), which is new work either
  way, same as Q1a.
- **(d) Nothing else in the code suggests a fourth path.** There is no
  generic "post-composite UI layer" hook; ReShade is the closest analogous
  "inject a pass into the pipeline" precedent (`src/rendervulkan.cpp:4037`-
  `4059`, before the main composite dispatch, operating on the base layer's
  texture in place) but it's a full-frame post-process effect, not an
  independently-positioned/sized UI layer — its plumbing isn't reusable for
  ImGui, only its existence as precedent that "inject before the main
  dispatch" is an accepted pattern in this codebase.

## 2. Vulkan plumbing for ImGui

*External research, checked 2026-08-21:* before relying on the queue
inference below, I checked ImGui's own backend header/source directly
([`imgui_impl_vulkan.h`](https://github.com/ocornut/imgui/blob/master/backends/imgui_impl_vulkan.h)
and the [Vulkan backend overview](https://deepwiki.com/ocornut/imgui/3.3.1-vulkan-backend)).
This confirms, from the primary source rather than inference: ImGui's
Vulkan backend issues `vkCmdDraw*`/graphics-pipeline commands and its own
`ImGui_ImplVulkanH_SelectQueueFamilyIndex()` helper explicitly looks for
`VK_QUEUE_GRAPHICS_BIT` — **it cannot run on a compute-only queue at all.**
That settles the async-compute concern this doc opens with: gamescope's
compute-only `m_queue` is a non-starter for ImGui, and `generalQueue()`
(graphics+compute) is not just a plausible choice, it's the *only* viable
one already present in `CVulkanDevice`. The header also documents two
details worth carrying into implementation: (1) `DescriptorPool` can be
left `VK_NULL_HANDLE` and a pool auto-created by setting
`InitInfo.DescriptorPoolSize > 0` instead of hand-rolling one (a November
2024 convenience addition per the header comments) — softens the
"descriptor pool has to be added" point below to "can be trivial, not just
possible"; (2) dynamic-rendering mode requires **explicitly enabling the
`VK_KHR_dynamic_rendering` extension string, "even for Vulkan 1.3."** That
second point sharpens the version-gap caveat already flagged below: I
independently confirmed by `grep`ing `src/rendervulkan.cpp` that gamescope
only ever sets the *feature bit*
(`VkPhysicalDeviceVulkan13Features::dynamicRendering = VK_TRUE`,
`:621`) and never adds `VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME` (or any
`DYNAMIC_RENDERING` string) to `enabledExtensions` anywhere in that file —
`grep -n "DYNAMIC_RENDERING" src/rendervulkan.cpp` matches only the feature
struct's field name. Per ImGui's own guidance this may not be sufficient
for its backend even though Vulkan 1.3 makes dynamic rendering core; this
is a concrete thing to verify empirically (does the extension need adding
explicitly here, or does ImGui's check tolerate a 1.3 core feature with no
extension string) rather than assume either way — I did not find a
definitive answer to *that* specific question in the sources checked.

`CVulkanDevice` (`src/rendervulkan.hpp:810`) already exposes, unused by
anything today:

- `generalQueue()` / `generalQueueFamily()` / `generalCommandPool()`
  (`:840`, `:844`, `:842`) — a **graphics+compute** queue family, selected
  separately from the compute-only `m_queue` whenever the driver offers
  distinct families (`src/rendervulkan.cpp:362`-`396`; on Intel and when
  `GAMESCOPE_FORCE_GENERAL_QUEUE` is set, `m_queueFamily` and
  `m_generalQueueFamily` collapse to the same family, `:406`-`412`). This is
  confirmed unused elsewhere: `grep` for `generalQueue()`/
  `generalCommandPool()` outside `rendervulkan.cpp`/`.hpp` returns nothing.
  This answers the async-compute concern directly: **yes, a graphics queue
  is available** — it's just never been used for anything but being created.
- Dynamic rendering is requested at device-creation time:
  `VkPhysicalDeviceVulkan13Features::dynamicRendering = VK_TRUE`
  (`src/rendervulkan.cpp:621`), chained via `VkPhysicalDeviceFeatures2`. No
  render pass / framebuffer object plumbing needs to be added — ImGui's
  Vulkan backend supports rendering via
  `VK_KHR_dynamic_rendering` directly (`ImGui_ImplVulkan_InitInfo::UseDynamicRendering`),
  which sidesteps needing a `VkRenderPass` at all.
  *Caveat, flagged as uncertain:* `selectPhysDev()` only rejects physical
  devices below `VK_API_VERSION_1_2` (`src/rendervulkan.cpp:350`), so a
  1.2-only device could reach `createDevice()` and get handed a
  `VkPhysicalDeviceVulkan13Features` struct chained onto instance/device
  creation regardless — I did not verify whether the driver silently
  ignores an unsupported `sType` here or this is a pre-existing latent bug;
  either way it's not something an ImGui feature should need to touch, but
  it means "does dynamic rendering actually work everywhere this runs"
  isn't fully nailed down by this scout pass. See the external-research note
  immediately below for the second half of this caveat: even where dynamic
  rendering does work, ImGui's own docs say its backend wants the
  `VK_KHR_dynamic_rendering` extension string enabled explicitly, which
  gamescope's device creation does not currently do.
- `CVulkanTexture::createFlags` (`src/rendervulkan.hpp:131`-`157`) already
  has `bColorAttachment`, which maps to `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`
  (`src/rendervulkan.cpp:2048`), plus `bSampled` for
  `VK_IMAGE_USAGE_SAMPLED_BIT`. An ImGui target texture — render-target
  now, sampled input to `vulkan_composite()` later — is exactly
  `{bColorAttachment, bSampled}` and needs **no new usage flag**.
- **What's missing / has to be added:**
  - A `VkDescriptorPool` for ImGui's own descriptor sets
    (combined-image-samplers for its font atlas + any textures) — though
    per the external research above, this may not need to be hand-built:
    ImGui's backend can create its own pool given
    `InitInfo.DescriptorPoolSize > 0`. Either way, the existing
    `descriptorSet()` (`src/rendervulkan.hpp:824`-`829`) cycles a fixed pool
    sized for the compute-composite pipeline's own layout
    (`m_descriptorSetLayout`, `:901`) — not general-purpose, not reusable
    as-is.
  - A command-buffer/submission path on the general queue. `commandBuffer()`
    / `submit()` (`:818`-`819`) are hard-wired to `m_commandPool`/`m_queue`
    (the compute queue) via `CVulkanCmdBuffer`'s constructor
    (`src/rendervulkan.cpp:1536`-`1537`, which takes `queue`/`queueFamily`
    as parameters — so the *type* already supports a general-queue command
    buffer, but no call site constructs one against `generalQueue()`
    anywhere today). A thin wrapper analogous to `commandBuffer()` that
    pulls from `m_generalCommandPool`/`m_generalQueue` is new but small.
  - Cross-queue sync between "ImGui finished drawing on the general queue"
    and "compute composite wants to sample that texture." `CTimeline`
    (`src/Timeline.h:25`) already exists for exactly this class of problem
    (DRM-syncobj-backed, convertible to a Vulkan timeline semaphore via
    `ToVkSemaphore()`) and is the natural mechanism to reuse rather than
    inventing a second one.
  - `ImGui_ImplVulkan_InitInfo` also wants a `VkPipelineCache` (optional,
    can be `VK_NULL_HANDLE`) and a `MinImageCount`/`ImageCount` — trivial
    for an offscreen single-buffered target, not a swapchain-count
    negotiation.
- **Docking/viewports (external research, checked 2026-08-21):** ImGui
  separates the two. **Docking** — arranging panels within one image — is a
  pure per-context feature with no platform dependency; it works unmodified
  against a single offscreen render target, so if the settings UI wants
  dockable panels (tabs, resizable sub-windows) inside itself, that's free.
  **Multi-viewport** — letting ImGui panels become separate real OS/desktop
  windows the user can drag outside the main image — requires the platform
  backend to be able to create new native windows and graphics contexts
  per [ocornut/imgui's own Multi-Viewports wiki](https://github.com/ocornut/imgui/wiki/Multi-Viewports),
  which has no meaning for a single offscreen texture composited as one
  `Layer_t` with no windowing backend behind it at all. **Recommendation:**
  plan for docking within the single overlay surface if the UI needs it;
  treat multi-viewport as out of scope unless the architecture changes to
  Q1's option (c) (a real windowed client process), which is the only
  variant of this design that has actual OS windows to put a dragged-out
  panel into.

## 3. Which thread drives the UI?

**The steamcompmgr thread, inside (or immediately before) `paint_all()`**
(`src/steamcompmgr.cpp:2564`, called at `:9488`). Per the architecture
overview's threading model, that thread is the one that (a) owns the
vblank-paced repaint cadence — `GetVBlankTimer().ProcessVBlank()`
(`:8882`) — that ImGui's `NewFrame()`/`Render()` cadence should track, and
(b) is the only thread that touches `FrameInfo_t` construction and calls
`vulkan_composite()`. Calling `ImGui::NewFrame()` anywhere else (e.g. the
main thread's `wlserver_run()`, `src/wlserver.cpp:2254`) would require a
second cross-thread handoff of the finished ImGui draw texture into a frame
already being assembled on the steamcompmgr thread — solvable, but it
recreates exactly the "two threads cooperating on one shared frame" hazard
the architecture doc calls out as the reason the render loop already lives
where it does, for no benefit (input still has to reach the overlay through
wlserver's listeners either way — see Q4). The concrete call site: a new
`if ( g_bSettingsOverlayVisible ) { ImGui::NewFrame(); ...; ImGui::Render(); }`
block belongs right before the layer-stack assembly in `paint_all()`, with
the resulting texture pushed as a `Layer_t` the same way the blank-texture
hack at `:2745` does.

## 4. Input routing — capturing keyboard/mouse on toggle

This is the part with no ready-made mechanism; the existing pieces solve
adjacent but different problems:

- **`gamescope-action-binding.xml` / `CGamescopeActionBinding`**
  (`src/WaylandServer/GamescopeActionBinding.h:95`) is *only* reachable by
  an external Wayland client binding `gamescope_action_binding_manager` and
  creating a binding object over the protocol
  (`s_Bindings.push_back(this)` happens in the resource's own constructor,
  `:103`) — there is no in-process C++ API to register a binding without
  also being a Wayland client of gamescope's own server. Not directly
  reusable for driving an in-process overlay's own hotkey.
- **What *is* directly reusable: the global-hotkey bypass pattern in
  `wlserver_handle_key`** (`src/wlserver.cpp:298`-`345`). Volume keys and
  VT-switch keysyms are already special-cased *before* the normal
  `wlserver_process_hotkeys()` / seat-forward path, redirecting the event to
  a specific surface and explicitly `return`ing so it never reaches the
  focused client (`:315`-`336`, the `forbidden_key` block). This is the
  exact shape a settings-overlay toggle needs: check a specific keysym
  combo early in `wlserver_handle_key`, flip an atomic
  (`g_bSettingsOverlayVisible` or similar) instead of forwarding, and
  `return`. `wlserver_process_hotkeys()` (`:2406`) itself is a second,
  perfectly good option if the toggle is expressed as a proper
  `CGamescopeActionBinding`-style multi-key combo tracked via
  `wlserver.mapPressedHotkeyKeys` — but that machinery is presently coupled
  to protocol-registered bindings only, so extending it to accept an
  internal, non-protocol binding is itself a small piece of new plumbing,
  not something that exists today.
- **What has to be built: full input capture for the duration the overlay
  is up**, not just the toggle keypress. Every key/pointer listener
  (`wlserver_handle_key`, `wlserver_handle_pointer_motion`/`_button`/`_axis`,
  `src/wlserver.cpp:358`-`396`) currently always ends by calling
  `wlr_seat_keyboard_notify_key` / the pointer equivalents, forwarding to
  whatever `wl_surface` currently has focus. There is no existing "consume
  this event into an in-process UI queue instead of the seat" branch. The
  natural extension: gate those forward calls on `g_bSettingsOverlayVisible`
  and, when set, feed raw key/mouse state into `ImGuiIO` directly instead
  (ImGui's own Wayland/GLFW backend isn't usable here since there's no
  `wl_surface` for the overlay to own — a hand-written `ImGuiIO` feed from
  wlroots' raw event structs is the correct shape, analogous to how
  `CLibInputHandler` — dead code today, `src/LibInputHandler.h:11` — already
  translates raw libinput events into `wlserver_key`/`wlserver_mousemotion`
  calls; here it's the same translation target but into `ImGuiIO` instead).
  Suppressing forwarding to the *game* while still letting the overlay work
  is a straightforward early-return; restoring normal forwarding on
  toggle-off is symmetric. No crash/edge-case history here to lean on (unlike
  the five recent focus-arbitration fixes covered in
  [steamcompmgr-focus.md](../features/steamcompmgr-focus.md)) — this is new
  territory for the codebase, so budget real testing time for it, especially
  around what happens to a game's own held-key state when input capture cuts
  it off mid-press (a key-up the game never receives).
- **OpenVR is a partial exception worth flagging.** `COpenVRBackend`
  (`src/Backends/OpenVRBackend.cpp:441`) already tracks keyboard/mouse
  focus as separate atomics per connector
  (`m_pKeyboardFocusConnector`/`m_pMouseFocusConnector`, `:1488`-`1489`,
  see [backend-openvr.md](../features/backend-openvr.md)) and SteamVR
  itself already arbitrates which overlay plane gets input focus. A
  settings overlay implemented as its own `COpenVRConnector` could piggyback
  on that existing focus-taking machinery (`UpdateVisibility`,
  `:1842`) rather than needing the raw-event-interception approach above —
  but that only helps the OpenVR backend specifically, not DRM/SDL/Wayland/
  Headless, so it can't be the *only* input path if the overlay needs to
  work everywhere.

## 5. The toggle itself

**Recommend a `ConVar<bool>`/`ConCommand` (`src/convar.h`) driving the
`wlserver_handle_key` early-intercept pattern from Q4**, not the action-
binding protocol. Reasoning:

- `ConVar`/`ConCommand` is the established cross-thread-safe pattern for
  exactly this shape of problem — `cc_focus_info`
  (`src/steamcompmgr.cpp:932`) is the documented precedent: a command
  callback (invokable from any thread, including via `gamescopectl`/Lua)
  that only flips an atomic flag, with the actual state change happening on
  the thread that owns the data (see
  [scripting-convars.md](../features/scripting-convars.md)). A
  `cc_toggle_settings_overlay` command following that exact template gives
  scripting/`gamescopectl`/Lua control "for free" alongside the hotkey.
- The hotkey itself is cheapest wired directly into `wlserver_handle_key`'s
  existing special-case block (Q4) rather than through
  `gamescope_action_binding_manager`, because the action-binding protocol's
  only registration path is a Wayland client binding the protocol — making
  gamescope's *own* overlay register itself that way means either faking an
  internal Wayland client connection to its own server, or exposing a
  non-protocol internal-registration API on `CGamescopeActionBinding` that
  doesn't exist today. Both are more machinery than a single keysym check
  next to the existing `forbidden_key` block.
- `src/Apps/gamescope_hotkey_example.cpp` is real but answers a different
  question: it demonstrates how an **external** client binds a hotkey
  through the protocol (relevant if the overlay is ever built as the
  separate-process architecture from Q1's option (c)), not how gamescope's
  own in-process code would react to one.

## 6. Backend coverage

The recommended design (Q1a: a `Layer_t` textured by an offscreen ImGui
render, composited by the same `vulkan_composite()` every backend already
calls through `paint_all()`) is **backend-agnostic by construction** — DRM,
SDL, Wayland, and Headless all funnel through the same
`paint_all()` → `vulkan_composite()` → `IBackendConnector::Present()` path
confirmed in [architecture/overview.md](../architecture/overview.md); every
backend implements `UsesVulkanSwapchain()`
(`src/Backends/DRMBackend.cpp:3997`, `SDLBackend.cpp:497`,
`WaylandBackend.cpp:2296`, `HeadlessBackend.cpp:219`,
`OpenVRBackend.cpp:795`) but that flag only decides how `Present()` gets the
*finished, already-composited* frame to the screen — it doesn't change
whether the layer stack got assembled and composited, which is where the
overlay lives. Headless still composites (only presentation is a no-op,
[backend-headless.md](../features/backend-headless.md)), so the overlay
would render into the (unviewable) headless output too — harmless but worth
noting as a "why bother" for that specific backend.

**OpenVR is where this needs real design thought, not just plumbing.** It's
this fork's active area, and an in-VR settings overlay is plausible and
arguably higher-value than on a flat 2D backend, but:
- The recommended Q1a path composites into whatever `FrameInfo_t` becomes
  the connector's output — for OpenVR that means the settings UI would be
  baked into one specific SteamVR overlay plane's texture at a fixed
  position, not floating as its own independently-movable/scalable SteamVR
  overlay the way a native VR UI usually would.
- A more VR-native alternative — giving the settings overlay its own
  `COpenVRConnector` (own SteamVR overlay handle, own focus via
  `UpdateVisibility`, `src/Backends/OpenVRBackend.cpp:1842`) — is
  architecturally closer to how mangoapp/external overlays already work
  (Q1c) than to Q1a, and would need its *own* input-focus story reusing the
  `m_pKeyboardFocusConnector`/`m_pMouseFocusConnector` machinery instead of
  Q4's raw-interception approach. This is a real fork-in-the-road: **Q1a's
  answer for flat backends and OpenVR's best answer may not be the same
  architecture**, and this scout pass did not find enough in the existing
  code to declare one design that's clean on both. Flagged for the user in
  Open Questions.
  *External research, checked 2026-08-21, weak confidence:*
  [wlx-overlay-s](https://github.com/galister/wlx-overlay-s) is the closest
  real-world prior art for "an own-overlay-plane, Vulkan-rendered,
  interactive VR settings panel" — it's a standalone Vulkan app presenting
  its own SteamVR/OpenXR overlay(s) with laser-pointer-driven interaction.
  That corroborates the general shape of the "own `COpenVRConnector`"
  alternative sketched above. I could **not** get a documented answer on
  its exact input-routing mechanism or which Vulkan queue it renders
  with — the available secondary source ([DeepWiki summary](https://deepwiki.com/galister/wlx-overlay-s))
  states input-handling and rendering details live in source pages it
  didn't surface content for, and I did not pull the raw source to verify
  further given this doc's scope. Treat this as "a comparable project
  exists and solves the same class of problem," not as a validated design
  to copy.

## 7. Dependency vendoring

**Recommend `subprojects/` with a `wrap-git` + hand-written
`subprojects/packagefiles/imgui/meson.build` overlay** — this is a live,
exact-match precedent already in the repo, not a guess:
`subprojects/stb.wrap` (`[wrap-git]`, pins a revision of
`github.com/nothings/stb`, `patch_directory = stb`) pairs with
`subprojects/packagefiles/stb/meson.build`, a hand-authored build
definition for a library that has no native Meson support upstream —
exactly ImGui's situation (upstream `ocornut/imgui` ships no `meson.build`).
`glm` follows the identical pattern (`subprojects/glm.wrap` +
`subprojects/packagefiles/glm/meson.build`). The top-level `meson.build`
consumes these via `subproject('stb')`/`subproject('glm')`
(`meson.build:55`/`53`). A `subprojects/imgui.wrap` pinned to a specific
ImGui tag/commit, with a `packagefiles/imgui/meson.build` exposing a
`static_library` target for `imgui.cpp`, `imgui_draw.cpp`,
`imgui_widgets.cpp`, `imgui_tables.cpp`, and
`backends/imgui_impl_vulkan.cpp`, follows the repo's own convention
directly. A system dependency (`dependency('imgui')`) is not viable — no
distro ships a stable pkg-config for ImGui since it's typically
vendored/statically-linked by consumers. CMake-subproject (the pattern used
for `openvr`, `meson.build:65`, via `cmake.subproject`) is unnecessary
extra machinery ImGui doesn't need since a plain Meson overlay (the
stb/glm pattern) is sufficient for a source-only library.

## Risks

- **Input capture is genuinely new territory.** Unlike every other topic in
  this doc, there is no existing gamescope mechanism — dead code or
  otherwise — that "steals all input from the focused window into a
  non-`wl_surface` sink." This is the single most likely place for the
  implementation to balloon past its estimate, especially getting key-repeat,
  modifier state, and "what does the game see when a key held before
  toggle-on is released after toggle-off" right.
- **Cross-queue synchronization is easy to get subtly wrong.** ImGui drawing
  on the general (graphics) queue and `vulkan_composite()` sampling the
  result on the compute queue is a textbook data race if the handoff isn't
  fenced correctly every single frame. `CTimeline` is the right primitive to
  reuse, but nothing in the current codebase demonstrates a same-frame
  compute-queue-waits-on-general-queue pattern to copy from — this would be
  the first.
- **The OpenVR fork-in-the-road (Q6) risks two divergent implementations.**
  If the "flat backend" design (bake into an existing `FrameInfo_t` layer)
  and the "VR-native" design (own `COpenVRConnector`, own focus grant) both
  get built to give each backend a good experience, that's meaningfully more
  surface area than a single settings overlay implementation, on the fork
  whose own most-active subsystem (steamcompmgr-focus, five of the last ten
  commits) is already focus arbitration — a new focus-taking path adds to
  exactly the area that's already fragile.
- **The `VkPhysicalDeviceVulkan13Features` / device-version gap noted in
  Q2 is unverified beyond "the code looks inconsistent."** If it turns out
  to matter (a driver actually rejects device creation, or silently doesn't
  get dynamic rendering), that's a pre-existing bug an ImGui overlay would
  be the first feature to trip over, not something this feature introduces
  — but it should be checked, not assumed benign.

## Open questions for the user

1. **Scope for v1: flat backends only, or OpenVR too?** Given Q6's finding
   that the clean designs diverge, is a first version allowed to skip
   OpenVR (settings overlay simply doesn't render/toggle there), or does it
   need to work — even partially — on day one?
2. **In-process ImGui layer (Q1a, this doc's default) vs. a separate
   overlay process using the existing external-overlay window mechanism
   (Q1c)?** The latter avoids all of Q2's Vulkan-plumbing work and Q4's
   raw-input-interception work (an external client gets normal Wayland
   keyboard/pointer focus, and gamescope only needs to route focus to it
   like it already does for `externalOverlayWindow`), at the cost of IPC to
   actually mutate gamescope's own ConVars/state from outside the process,
   and losing direct access to `CVulkanDevice` internals. This is close to
   a coin-flip architecturally and materially changes the rest of the
   design — worth deciding before any code is written.
3. **What does the settings overlay actually need to configure?** This
   scout didn't assess scope/complexity of the UI itself, only the
   rendering/input mechanics — knowing whether it's "toggle five ConVars"
   or "a full tabbed settings app with live previews" changes whether the
   single-offscreen-texture approach in Q2 is even sized right.
4. **Is a toggle-time input handoff (game loses all input while the overlay
   is up) acceptable, or does the overlay need to coexist with the game
   still receiving some input** (e.g. a game continuing to render/simulate
   behind a semi-transparent settings panel)? This changes Q4's design
   significantly — full capture-and-suppress is much simpler than any kind
   of input splitting.
5. **Should the toggle hotkey be user-configurable**, and if so, should it
   go through the same `ConVar` the rest of the codebase uses for
   configuration, or is a `--settings-overlay-hotkey` CLI flag (matching
   the OpenVR backend's own CLI-flag-heavy configuration style,
   [backend-openvr.md](../features/backend-openvr.md)) preferred?
6. **Priority of the `VkPhysicalDeviceVulkan13Features`/
   `VK_KHR_dynamic_rendering`-extension-string gap flagged in Q2/Risks** —
   worth a dedicated empirical check (does ImGui's backend actually need the
   extension string added, or does it tolerate the 1.3 core feature alone)
   before overlay work starts, or acceptable to proceed and treat it as a
   pre-existing risk the overlay merely inherits?

## Sources

Everything above is `path:symbol`-cited against this repo's own source
except the passages explicitly marked "External research" (with a checked
date), which draw on the following. All checked 2026-08-21; where a claim
from these sources is repeated in the body above, it's re-verified against
this repo's own source wherever that was possible (called out inline) and
kept visibly separate where it wasn't.

- [ocornut/imgui — `backends/imgui_impl_vulkan.h`](https://github.com/ocornut/imgui/blob/master/backends/imgui_impl_vulkan.h) —
  primary source for `ImGui_ImplVulkan_InitInfo` fields, the graphics-queue
  requirement, descriptor-pool auto-creation, and the dynamic-rendering
  extension-string caveat (Q2).
- [DeepWiki — ocornut/imgui Vulkan backend](https://deepwiki.com/ocornut/imgui/3.3.1-vulkan-backend) —
  secondary corroboration of the graphics-queue requirement (Q2).
- [ocornut/imgui wiki — Multi-Viewports](https://github.com/ocornut/imgui/wiki/Multi-Viewports) —
  primary source distinguishing docking (single-context, no platform
  dependency) from multi-viewport (requires platform-window creation) (Q2).
- [flightlessmango/MangoHud — `src/app/main.cpp`](https://github.com/flightlessmango/MangoHud/blob/main/src/app/main.cpp) —
  primary source for mangoapp's `GAMESCOPE_EXTERNAL_OVERLAY`/
  `GAMESCOPE_NO_FOCUS` X11 properties (Q1c); the claim that this repo never
  reads `GAMESCOPE_NO_FOCUS`, and the structural focus-candidate exclusion
  that explains why mangoapp is safely non-interactive here anyway, are
  both independently re-verified against `src/steamcompmgr.cpp` in this
  repo, not taken from any external source.
- [DeepWiki — MangoHud gamescope integration](https://deepwiki.com/flightlessmango/MangoHud/5.1-gamescope-integration) —
  secondary source for the SysV-IPC-message-queue channel between mangoapp
  and gamescope (Q1c); not independently verified against gamescope-ritz's
  own source since that channel is implemented on MangoHud's side, outside
  this repo.
- [galister/wlx-overlay-s](https://github.com/galister/wlx-overlay-s) and its
  [DeepWiki summary](https://deepwiki.com/galister/wlx-overlay-s) — weak-confidence
  prior-art pointer for an interactive VR overlay architecture (Q6); explicitly
  flagged in the body as not yielding a confirmed input-routing or queue-type
  answer.

---
*Scouted: 2026-08-21.*
