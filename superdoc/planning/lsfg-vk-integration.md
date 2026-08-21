# LSFG-VK Integration — Planning Scout

**Status: PLANNING ONLY.** No code, no prototypes, nothing installed. This document is
the deliverable.

## Verdict

**Not feasible as literally described ("hook gamescope instead of the game") on four
of gamescope's five backends — feasible only in the narrow, currently-unsupported SDL
windowed-nested case, and even there it buys little.** LSFG-VK hooks
`vkCreateSwapchainKHR` / `vkQueuePresentKHR` on a real `VkSwapchainKHR`. Gamescope's
compositor loop never creates one, except on the SDL backend
(`src/Backends/SDLBackend.cpp:497-500`) — every other backend (DRM, Wayland, Headless,
OpenVR) reports `UsesVulkanSwapchain() == false` and presents by DRM atomic KMS commit
or dmabuf hand-off instead (`src/backend.h:365`; grep-verified per-backend below). DRM
is gamescope's flagship "embedded mode" backend, so the mechanically simplest and
highest-value target is exactly the one LSFG-VK cannot attach to.

The workable path is not "hook the compositor" — it's **the cheap, already-proven
option**: keep LSFG-VK where it already runs today (the game's process, on its real
swapchain), and have gamescope only provide the config UI, env-var plumbing, and
`Lossless.dll` path field, restarting the game session with the layer enabled. That is
option (a) in Q5, and it's the only option with real-world precedent — LSFG-VK's own
docs already document running underneath gamescope this way, workarounds and all.

## 1. What is LSFG-VK, mechanically?

LSFG-VK ([PancakeTAS/lsfg-vk](https://github.com/PancakeTAS/lsfg-vk), GPLv3, checked
2026-08-21 at commit `8b0da266`, default branch `develop`) is a Vulkan **device/instance
layer** (`VK_LAYER_LSFGVK_frame_generation`) split into a thin layer shim
(`lsfg-vk-layer/`) and a backend (`lsfg-vk-backend/`) that runs the actual frame-gen
model.

- **Hooked entry points** (`lsfg-vk-layer/src/entrypoint.cpp:470-475`, negotiated via
  the standard `vk_layer.h` `pfnGetPhysicalDeviceProcAddr`/layer-chaining protocol —
  structurally the same pattern gamescope's own WSI layer uses via vkroots):
  `vkCreateInstance`, `vkCreateDevice`, `vkCreateSwapchainKHR`, `vkQueuePresentKHR`,
  `vkDestroySwapchainKHR`. That's the whole hook surface — it does nothing to a game
  that never calls these five functions on a real swapchain.
- **What it needs from the swapchain:** `layer::Root::createSwapchainContext()` /
  `layer::Swapchain` (`lsfg-vk-layer/src/instance.hpp`,
  `lsfg-vk-layer/src/swapchain.hpp`) is keyed on a live `VkSwapchainKHR` handle and
  stores its images, format, colorspace, extent and present mode. `present()`
  intercepts a real `vkQueuePresentKHR(queue, swapchain, imageIdx, ...)` call, runs the
  frame-gen model against the acquired swapchain images, and presents the extra
  generated frames itself by calling the driver's present additional times (per the
  multiplier) before returning. **It does not operate on an arbitrary rendered image —
  it needs an actual swapchain object with acquire/present semantics.**
- **Queue:** no explicit graphics-queue requirement was found in the backend source;
  it does image blits/compute dispatches and syncs via `VK_KHR_timeline_semaphore` +
  `VK_KHR_external_semaphore(_fd)`/`VK_KHR_external_memory(_fd)`, which it force-adds
  in `lsfg-vk-layer/src/instance.cpp:90-121`. It rides whichever queue the swapchain's
  present call uses — a graphics-capable present queue in the normal case, since that's
  what `vkQueuePresentKHR` requires by the Vulkan spec.
- **`Lossless.dll`:** never executed as a Windows binary. `lsfg-vk-backend/src/extraction/
  dll_reader.cpp` + `shader_registry.cpp` **parse the DLL as a data file** and extract
  the embedded frame-gen model shaders out of it directly on Linux — the DLL is a
  shader/model blob container, not code that runs. Users must own a legitimate
  Lossless Scaling license (Steam, ~$7) and point LSFG-VK at their own copy
  (`docs/Configuration.md`: global `dll` option / `LSFGVK_DLL_PATH` env var).
- **Licensing/distribution:** LSFG-VK itself is GPLv3
  (`LICENSE.md`). `Lossless.dll` is proprietary, third-party, Steam-distributed —
  **gamescope-ritz must never bundle, download, or embed it**, must only accept a
  user-supplied filesystem path, and any UI copy should say so explicitly (mirrors
  LSFG-VK's own README: "make sure you have Lossless Scaling downloaded on Steam"
  before installing).

Sources: [README](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/README.md),
[docs/Configuration.md](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/docs/Configuration.md),
[docs/Troubleshooting.md](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/docs/Troubleshooting.md),
[lsfg-vk-layer/src/entrypoint.cpp](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/lsfg-vk-layer/src/entrypoint.cpp),
[lsfg-vk-layer/src/instance.cpp](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/lsfg-vk-layer/src/instance.cpp),
[lsfg-vk-layer/src/instance.hpp](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/lsfg-vk-layer/src/instance.hpp),
[lsfg-vk-layer/src/swapchain.hpp](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/lsfg-vk-layer/src/swapchain.hpp),
[lsfg-vk-layer/VkLayer_LSFGVK_frame_generation.json.in](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/lsfg-vk-layer/VkLayer_LSFGVK_frame_generation.json.in),
[LICENSE.md](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/LICENSE.md),
[Lossless Scaling on Steam](https://store.steampowered.com/app/993090/Lossless_Scaling/).

## 2. Can it be loaded into gamescope's process at all?

Mechanically yes — `VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation` (or the layer
manifest's implicit `GLOBAL` activation, gated only by `DISABLE_LSFGVK`;
`lsfg-vk-layer/VkLayer_LSFGVK_frame_generation.json.in`) would insert it into
gamescope's own Vulkan instance/device the same way any layer attaches to any Vulkan
app. **But whether it does anything productive there depends entirely on whether
gamescope creates a `VkSwapchainKHR` gamescope itself presents through — and on four of
five backends it does not:**

| Backend | `UsesVulkanSwapchain()` | Verified at |
|---|---|---|
| DRM (embedded/"real" mode) | **false** | `src/Backends/DRMBackend.cpp:3997-3999` |
| Wayland (nested) | **false** | `src/Backends/WaylandBackend.cpp:2296-2299` |
| Headless | **false** | `src/Backends/HeadlessBackend.cpp:219-222` |
| OpenVR | **false** | `src/Backends/OpenVRBackend.cpp:795-798` |
| SDL (nested) | **true** | `src/Backends/SDLBackend.cpp:497-500` |

The pure-virtual contract is `src/backend.h:365`. DRM presents via
`drmModeAtomicCommit` directly (`src/Backends/DRMBackend.cpp:2245 CDRMConnector::Present`,
`:4111`) — literal atomic KMS scanout, no swapchain object exists to hook. Wayland
backend hands dmabufs straight to the host compositor. So: **LSFG-VK's entry-point set
(`vkCreateSwapchainKHR`/`vkQueuePresentKHR`) is simply never called by gamescope's own
compositing code on DRM, Wayland, Headless, or OpenVR** — the layer would attach, sit
idle, and do nothing, because gamescope never triggers its hooks. Only the SDL backend
(nested-in-a-desktop-window mode) creates and presents through a real swapchain, so
that is the only backend where "load LSFG-VK into gamescope's process" is even
mechanically possible — and SDL is the least-used, least-relevant-to-Steam-Deck/
handheld backend in this project.

Gamescope's own compositing is also async-compute, not a graphics render loop
(`vulkan_composite()` prefers a compute-only queue family,
`src/rendervulkan.cpp:355-393`, falls back to a combined graphics+compute "general"
queue only when no compute-only family exists or a real surface needs present support)
— this reinforces rather than causes the blocker: even where a swapchain exists (SDL),
gamescope's dispatch model has no "render, then hand to LSFG-VK's present hook, then
LSFG-VK generates and presents extra frames" render loop shape to intercept, because
gamescope's own present call is a single `Present()` per real frame, not a
frame-generation-aware loop.

## 3. Where would generated frames go in gamescope's pipeline?

They wouldn't, on DRM (the case that matters). Presentation is vblank-paced by
`gamescope::CVBlankTimer` (`src/vblankmanager.hpp:27`), consumed by the steamcompmgr
thread's loop (`ProcessVBlank()`, `src/steamcompmgr.cpp:8882`), which decides once per
vblank whether to call `paint_all()` (`src/steamcompmgr.cpp:2564`) →
`vulkan_composite()` (`src/rendervulkan.cpp:4030`) → `IBackendConnector::Present()`
(`src/backend.h:205`) → one atomic KMS commit. There is exactly one composite+present
per vblank interval today; frame generation needs *N* presents per real frame at
sub-vblank cadence between the real ones. Making that happen in-tree would require:
rearchitecting the vblank-timer-driven single-shot repaint into a scheduler that can
fire multiple presents per interval, running the frame-gen model itself on gamescope's
compositor image, and re-deriving DRM atomic-commit timings for the interpolated
frames (DRM KMS scanout timing is coarser and less flexible than a windowed swapchain's
present queue). This is a genuine rearchitecture of the render loop described in
[compositing-vulkan.md](../features/compositing-vulkan.md) and
[architecture/overview.md](../architecture/overview.md#threading-model), not an
additive hook. **Latency:** frame generation is inherently a "hold frame N, wait for
frame N+1, interpolate, then release both" technique — it adds at least one real
frame's worth of delay before *any* generated frame reaches the screen. Gamescope's
existing pacing is deliberately tight (vblank-synchronous, single hop) specifically to
minimize latency for gaming; grafting interpolation onto that hot path works directly
against the reason `CVBlankTimer`/async-compute exist.

## 4. Layer-ordering hazard

Gamescope's own WSI bypass layer, `VK_LAYER_FROG_gamescope_wsi`
(`layer/VkLayer_FROG_gamescope_wsi.cpp`, see
[vk-wsi-layer.md](../features/vk-wsi-layer.md)), is an **opt-in `enable_environment`**
layer (only activates when `ENABLE_GAMESCOPE_WSI=1` is set in the *game's*
environment) and is a no-op passthrough for anything that isn't a Vulkan app actually
running inside a gamescope session
(`isRunningUnderGamescope()`, `layer/VkLayer_FROG_gamescope_wsi.cpp:105`) — including
gamescope's own child compositor use of Vulkan, explicitly filtered by
`isAppInfoGamescope` (`:98`). LSFG-VK's manifest, by contrast, is `type: GLOBAL` with
only a `disable_environment` gate (`DISABLE_LSFGVK`,
`VkLayer_LSFGVK_frame_generation.json.in`) — it is implicit-active-by-default for
*every* Vulkan process on the system unless disabled, and its own `active_in`
profile-matching (by executable name) decides per-process whether it actually does
anything. **This is already a real, documented interaction today, not a hypothetical:**
LSFG-VK's own Quirks/Troubleshooting pages tell Steam Deck / gamescope users to pass
`ENABLE_GAMESCOPE_WSI=0 %command%` (disabling gamescope's WSI bypass in the *game's*
process) because gamescope's compositor "does not respect the V-Sync setting" LSFG-VK's
`pacing_mode = none` depends on, causing frame skipping — i.e. gamescope's compositor
being in the presentation path (even without LSFG-VK loaded into gamescope itself)
already interferes with LSFG-VK's pacing model. If a user enables LSFG-VK system-wide
(implicit layer, no `DISABLE_LSFGVK`) while also running gamescope, LSFG-VK's hooks
land in **both** the game's process (where it can work, on a real swapchain, subject to
the above) **and** gamescope's own process (where, per Q2, it attaches but finds no
swapchain to act on except under SDL) — no crash expected, but wasted attach overhead
and profile-matching confusion in the non-SDL case, plus the documented WSI-bypass
pacing conflict in the case where it does something.

Sources: [Quirks wiki](https://github.com/PancakeTAS/lsfg-vk/wiki/Quirks),
[docs/Configuration.md](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/docs/Configuration.md)
("Pacing Modes" section), `superdoc/features/vk-wsi-layer.md`.

## 5. Realistic integration options, ranked

**(a) Env-var launch integration — gamescope UI/config only, no engine changes.**
Gamescope's General settings gains the `Lossless.dll` path, enable toggle, multiplier,
etc.; on apply, gamescope writes/updates LSFG-VK's own `~/.config/lsfg-vk/conf.toml`
(or sets `LSFGVK_*` env vars for the child game process) and the *game* launches with
`ENABLE_GAMESCOPE_WSI` set appropriately per the Quirks-page guidance, and restarts the
session if config changed while running. **Effort: low** (a settings panel + a TOML
writer + documented env-var plumbing, no gamescope engine touched).
**Invasiveness: none** — zero changes to `rendervulkan.cpp`/`steamcompmgr.cpp`.
**Likelihood of working: high**, because this is exactly the LSFG-VK-under-gamescope
setup its own docs already describe workarounds for — it's proven, if fiddly, to work
today by hand. **What it breaks:** nothing in gamescope; inherits all of LSFG-VK's
existing per-game compatibility caveats (Vulkan-only, 64-bit only, VRR conflicts,
tearing-control conflicts) unchanged. **This does not "hook gamescope instead of the
game"** — it's LSFG-VK exactly where it runs today, with gamescope only as the
configuration surface. Given Q1-Q4's findings, **this is the only option with a real
chance of working across all backends, and should be the actual first (and possibly
only) version.**

**(b) Gamescope explicitly enables the layer at its own instance creation.**
Only possible where `UsesVulkanSwapchain()` is true — SDL backend alone (Q2). Even
there it buys nothing over (a): the game would still need its own path to LSFG-VK for
DRM/Wayland/Headless/OpenVR sessions anyway, so gamescope would end up shipping and
maintaining two different attach paths for one feature. **Effort: medium**
(instance-creation-time layer enablement, config plumbing into gamescope's own Vulkan
device setup in `rendervulkan.cpp`). **Invasiveness: medium** (touches gamescope's own
Vulkan bring-up). **Likelihood of working: only on SDL**, i.e., not the backend anyone
running gamescope in "real" (DRM/embedded) mode cares about. **What it breaks:**
gamescope's own composite dispatch assumes a single present per vblank (Q3) — even on
SDL, LSFG-VK's extra presents would fight `CVBlankTimer`'s pacing assumptions.
**Not recommended.**

**(c) Deep in-tree integration — call frame-gen directly in the composite path.**
Reimplement (or link) the frame-gen model invocation inside `vulkan_composite()` or the
`paint_all()`/`CVBlankTimer` loop itself, producing interpolated frames as new
`FrameInfo_t` composites on gamescope's own schedule, independent of LSFG-VK's
swapchain-hook architecture entirely. **Effort: very high** — this is not "integrate
LSFG-VK," it's "build a bespoke frame-generation pipeline inside gamescope, informed by
LSFG-VK's shader-extraction technique," touching the vblank timer, the render loop,
DRM atomic-commit timing, and every backend's present path (Q3). **Invasiveness:
maximal.** **Likelihood of working: unknown/research-grade** — no evidence anywhere
that a compute-only, KMS-atomic-commit compositor has been made to run this kind of
model inline; would need real prototyping to know if the model even runs fast enough
inside the vblank budget. **What it breaks:** everything downstream of
`vulkan_composite()`, and would need re-validating against all five backends
individually. **Not a "first version" — only worth considering if (a) is judged
insufficient and there's appetite for a multi-month R&D effort.**

## 6. Does this actually deliver the stated goal?

The user's stated rationale is **compatibility** — hooking the compositor instead of
each game so per-game Vulkan-layer-injection quirks don't matter. That rationale
does not survive the mechanics: **LSFG-VK's compatibility problems are not primarily
about which process it's injected into — they're about swapchain/present-mode
semantics** (needs a real `VkSwapchainKHR`, fights VRR, fights tearing-control, needs a
Vulkan game in the first place, doesn't work with OpenGL without Zink). Moving the
attach point from "the game" to "gamescope" does not fix any of those; per Q2 it mostly
just removes the attach point entirely on gamescope's primary (DRM) backend. If
anything, compositing-then-generating (were it possible, per option (c)) would
introduce a **new** compatibility problem that per-game injection doesn't have:
gamescope composites the Steam overlay and gamescope's own on-screen notification layer
into the *same* frame the game occupies before presenting — verified at
`src/steamcompmgr.cpp:2461` (`nReservedLayers` reserved for `pFocus->overlayWindow`)
and `:2473-2476` (`paint_window(pFocus->overlayWindow, ...)`, `zpos = g_zposOverlay` at
`:2230-2236`). A frame generator running after this composite step would interpolate
motion *into* overlay/notification pixels that never actually moved between real
frames — a visible smearing/ghosting artifact on exactly the UI elements a user is
least likely to tolerate looking wrong. Per-game (in-process) LSFG-VK never sees this,
because it hooks before any compositor overlay exists. **Net assessment: the
compatibility argument for hooking gamescope instead of the game does not hold up —
the actual attach point (a real swapchain) is the same requirement either way, and
gamescope's own compositing introduces a new artifact class that per-game injection
avoids.**

## 7. What must the General settings UI expose?

Grounded in `docs/Configuration.md`'s actual global/profile schema (not invented):

- **`Lossless.dll` path** (global `dll` option / `LSFGVK_DLL_PATH`) — file picker,
  explicit "not bundled, point at your own Steam-purchased copy" copy in the UI.
- **Enable/disable** — per-profile activation is really "is this game's executable
  name in `active_in`"; a simple per-session toggle in gamescope maps to writing/
  removing a profile entry (or setting `DISABLE_LSFGVK`).
- **Multiplier** (`multiplier`, default `2`) — LSFG-VK's real range is not fixed at
  2/3/4x by the code; it's an integer multiplier limited in practice by the driver's
  max swapchain image count (`docs/Troubleshooting.md`/community docs note e.g.
  multiplier 4 alone can consume most of an 8-image swapchain budget) — the UI should
  clamp to whatever the actual queried swapchain image count allows, not hardcode 2/3/4.
- **Flow Scale** (`flow_scale`, default `1.0`) — motion-vector resolution scale,
  lower = faster/lower quality.
- **Performance Mode** (`performance_mode`, default `false`) — lighter model variant.
- **Allow half-precision** (`allow_fp16`, global, default `true`) — big AMD win, can
  regress older NVIDIA (GTX 1000-series and older); worth a GPU-vendor-aware default
  or at least a warning, not a silent blanket default.
- **GPU selection** (`gpu`) — required to be the *same* GPU the game uses; **dual-GPU
  is explicitly unsupported** upstream. On a single-GPU handheld (the likely primary
  target) this can probably be omitted/auto-filled; expose it for desktop multi-GPU
  users.
- **Pacing Mode** (`pacing`) — today only `none` exists upstream (forces V-Sync, "might
  require workarounds on some compositors" — i.e. gamescope). Surface it as a field now
  so the schema doesn't need to change when upstream adds more modes, but don't imply
  choice exists yet.

Source: [docs/Configuration.md](https://github.com/PancakeTAS/lsfg-vk/blob/8b0da266/docs/Configuration.md).

## Risks

- **Primary architectural risk:** the entire premise ("hook gamescope, not the game")
  is foreclosed on DRM/Wayland/Headless/OpenVR by `UsesVulkanSwapchain() == false`
  (Q2). Any UI built around "gamescope intercepts frame-gen" language will describe a
  capability that doesn't exist on the backend most users run.
- **Upstream churn:** LSFG-VK is pre-1.0-feeling and actively developed (v2.0 in
  progress per its own README banner as of the commit checked); config schema
  (`conf.toml` keys, env var names) may change before any gamescope integration ships.
  Treat the Q7 field list as a snapshot, not a stable contract.
- **License/distribution risk:** any code path that downloads, caches, or bundles
  `Lossless.dll` (even "for convenience") would violate its proprietary licensing and
  must be avoided; the UI must only ever accept a user-supplied local path.
- **Latency risk:** frame generation adds a frame of delay by construction (Q3);
  gamescope is used heavily for latency-sensitive gaming (Steam Deck, VR via OpenVR
  backend). Even the low-risk option (a) inherits this from LSFG-VK itself — it is not
  gamescope-specific, but it is a real user-facing regression to flag in any UI.
- **Support-burden risk:** option (a) still means gamescope's UI is now responsible for
  correctly wiring a fast-moving third-party project's env vars/config file, with the
  documented gamescope-specific pacing workaround (`ENABLE_GAMESCOPE_WSI=0`) as a
  required detail to get right, not optional polish.

## Open questions for the user

1. **Given Q2/Q6, "hook gamescope instead of the game" is not achievable on the DRM
   backend and does not actually improve compatibility over per-game injection — is
   option (a) (gamescope as config/launch UI only, LSFG-VK still runs in the game's
   process) an acceptable reframing of the goal, or does the feature only matter if it
   genuinely runs inside gamescope's own process?**
2. Is the **SDL (nested) backend** — the only backend where loading LSFG-VK into
   gamescope's own process is even mechanically possible — a use case worth building
   option (b) for, given it's not the primary Steam Deck/embedded-mode target?
3. Should the General settings UI ship now with only what LSFG-VK v2.0 actually
   supports today (Q7), knowing the schema will likely need a follow-up pass as
   upstream evolves — or wait for LSFG-VK to stabilize past its README's own "upcoming
   2.0" caveat first?
4. Is a per-game `Lossless.dll` license/ownership disclaimer in the UI (confirming the
   user owns Lossless Scaling on Steam) sufficient, or does legal/distribution policy
   need a firmer gate (e.g. refusing to enable the feature until the configured path is
   verified to actually contain a plausible `Lossless.dll`)?
5. Given the documented `ENABLE_GAMESCOPE_WSI=0` conflict between gamescope's own WSI
   bypass layer and LSFG-VK's pacing model (Q4) — should enabling LSFG-VK for a game
   automatically force-disable gamescope's WSI bypass for that game, trading away the
   XWayland-bypass latency win to make LSFG-VK's pacing work?
6. Is any appetite at all for option (c) (true in-tree frame generation), given it's
   R&D-grade and unproven even in principle for a compute-only, KMS-atomic-commit
   compositor — or should this scout's finding ("not worth pursuing now") close that
   door for the foreseeable roadmap?

---
*Checked 2026-08-21 against gamescope-ritz `master` (commit `fcc1341` and earlier) and
LSFG-VK `develop` (commit `8b0da266`). LSFG-VK moves fast — re-verify swapchain/queue
assumptions against its current source before implementing option (a) or (b).*
