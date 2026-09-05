# LSFG-VK In-Tree Port — Planning Scout

**Status: PLANNING ONLY.** No code, no prototypes, nothing installed, nothing cloned.
This document is the deliverable. It re-examines the question answered in the sibling
doc [`lsfg-vk-integration.md`](lsfg-vk-integration.md) under a **different framing**:
not "load LSFG-VK as a Vulkan layer into gamescope's process" (foreclosed there — no
swapchain to hook on DRM/Wayland/Headless/OpenVR), but "port LSFG-VK's *frame-generation
functionality itself* into gamescope, discarding the layer machinery entirely." Do not
re-litigate the layer-hooking conclusion; it stands.

## Verdict

**Feasible-with-caveats.** The single reason this isn't a clean "yes": **the licence
forecloses a literal in-tree port (vendoring LSFG-VK's GPLv3 source into gamescope-ritz's
own BSD-2-Clause binary) unless gamescope-ritz accepts relicensing that binary to
GPLv3.** That is a project-policy decision, not an engineering one — see the Licence
gate below. Set that aside, though, and the technical picture is **much more favorable
than the sibling doc's brief dismissal of "option (c)" suggested**: as of the current
`develop` branch, LSFG-VK's own maintainer has already done most of the decoupling work
the user is proposing — the frame-generation engine was refactored out of the
Vulkan-layer into a standalone library (`lsfg-vk-backend` + `lsfg-vk-common`) driven
entirely by **dma-buf file descriptors and a timeline semaphore**, with zero swapchain
dependency, and it is already proven to run outside any Vulkan layer by LSFG-VK's own
`lsfg-vk-cli debug` tool. Every Vulkan extension/feature that engine requires is already
enabled by gamescope's own `CVulkanDevice`. **The remaining hard problem is not the
algorithm or the license mechanics in isolation — it's presentation cadence**: making
gamescope's vblank-paced, single-present-per-interval loop show extra frames at the
right times without wrecking latency or fighting VRR, a problem LSFG-VK's own mature
in-process implementation still hasn't fully solved either.

## Licence gate

**LSFG-VK is GPLv3** (`LICENSE.md`, full GPLv3 text; every source file carries
`SPDX-License-Identifier: GPL-3.0-or-later`; GitHub's own detection reports
`GPL-3.0`). **gamescope-ritz is BSD-2-Clause** (`/home/mo/GitHubProjects/gamescope-ritz/LICENSE`,
confirmed: "BSD 2-Clause License, Copyright (c) 2013-2022 Valve Corporation..."; a
bundled OpenColorIO LUT file carries BSD-3-Clause, immaterial here).

- **BSD → GPL is a one-way street.** BSD-2-Clause is permissive and GPL-compatible in
  the sense that BSD code may be folded into a GPLv3 work — but not the reverse. If
  LSFG-VK's GPLv3 source is copied into gamescope-ritz's own translation units and
  compiled into the same binary (a literal "clone the repo and implement its
  functionality into gamescope" as the user described), the result is a **combined
  work**, and GPLv3 requires the *entire* combined work, as distributed, to comply with
  GPLv3 — source availability, no additional restrictions, etc. Concretely: **shipping
  a gamescope-ritz binary containing vendored LSFG-VK source means gamescope-ritz itself
  must be redistributed under GPLv3 terms**, not BSD-2-Clause. That is a real
  relicensing decision affecting the whole project, not a footnote.
- **This is a policy call the maintainers must make, not something this scout can
  resolve.** Per this task's own instruction, a licence that forces relicensing means:
  say so plainly, and do not recommend the literal in-tree/statically-linked port.
  **This document does not recommend vendoring LSFG-VK source into gamescope-ritz's
  binary.**
- **There is one architecturally-clean way to get the practical outcome the user wants
  without relicensing**, and it happens to line up naturally with how LSFG-VK's own
  code is now built: run `lsfg-vk-backend` as a **separate, optionally-installed GPLv3
  component** — a small subprocess or a `dlopen()`-loaded shared object that the *user*
  builds/installs themselves (gamescope-ritz never bundles, links, or distributes it,
  exactly as it never bundles `Lossless.dll` today) — and have gamescope talk to it
  purely over the dma-buf-fd + timeline-semaphore protocol its own CLI tool already
  demonstrates (`lsfg-vk-backend/include/lsfg-vk-backend/lsfgvk.hpp`, see Q2/Q3 below).
  This keeps gamescope-ritz's own source and binary 100% BSD-2-Clause; the GPLv3 code
  never enters its build. Whether "talks to an optional external process the user
  installs" satisfies what the user meant by "implement its functionality into
  gamescope" is exactly the trade-off to put in front of them (see Open questions). It
  is **not** the same thing as vendoring source, and it is the only path this scout
  found that clears the licence gate while still landing inside gamescope's own
  composite loop rather than the game's process.
- **`Lossless.dll` re-confirmed:** still never executed as Windows code.
  `lsfg-vk-backend/src/extraction/dll_reader.cpp` and `shader_registry.cpp` parse it as
  a data container and extract embedded shaders at runtime. It remains a proprietary,
  Steam-distributed blob (`store.steampowered.com/app/993090/Lossless_Scaling/`) that
  the *user* must own and point at by path — gamescope-ritz must never download, cache,
  or bundle it, under any of the architectures discussed here.

Sources: [LICENSE.md](https://github.com/PancakeTAS/lsfg-vk/blob/develop/LICENSE.md)
(checked 2026-08-21, `develop` branch), GitHub repo metadata (`gh api
repos/PancakeTAS/lsfg-vk`, `license.spdx_id: GPL-3.0`, checked 2026-08-21),
`/home/mo/GitHubProjects/gamescope-ritz/LICENSE` (read directly, checked 2026-08-21).

## 1. Separating layer plumbing from the algorithm

**LSFG-VK has already done this split itself**, and it's more thorough than the sibling
doc's snapshot of the old layer-only architecture found. Current `develop`-branch
repository layout (`gh api repos/PancakeTAS/lsfg-vk/git/trees/develop?recursive=1`,
checked 2026-08-21):

| Module | Size (source) | What it is |
|---|---|---|
| `lsfg-vk-layer/` | ~44 KB (`entrypoint.cpp`, `instance.cpp/hpp`, `swapchain.cpp/hpp`) | **Pure Vulkan-layer boilerplate.** Entry-point interception (`vkCreateInstance`/`vkCreateDevice`/`vkCreateSwapchainKHR`/`vkQueuePresentKHR`/`vkDestroySwapchainKHR`), layer manifest, per-app profile matching. Nothing here does frame generation. |
| `lsfg-vk-backend/` | ~93 KB (`lsfgvk.cpp`, `extraction/dll_reader.cpp` + `shader_registry.cpp`, `shaderchains/*.cpp` — `alpha0/1`, `beta0/1`, `gamma0/1`, `delta0/1`, `generate`, `mipmaps` — `helpers/managed_shader.cpp`) | **The actual algorithm.** DLL shader extraction, the motion-estimation/warping/interpolation compute-shader chain, dispatch orchestration. Public API: `lsfg-vk-backend/include/lsfg-vk-backend/lsfgvk.hpp`. |
| `lsfg-vk-common/` | ~116 KB, excl. a 486 KB vendored `toml.hpp` | Generic Vulkan RAII wrappers (buffer/image/command_buffer/descriptor_set/fence/semaphore/shader/timeline_semaphore) + TOML config parsing. Reusable infrastructure the backend depends on; not layer-specific, but also not something a gamescope port needs verbatim — gamescope's own `CVulkanDevice`/`CVulkanTexture` (`src/rendervulkan.cpp`) already cover the same ground and a port would more naturally adapt the backend to gamescope's existing wrappers than import this module's ~80 KB of `.cpp`. |
| `lsfg-vk-cli/` | ~23 KB | Standalone CLI (`validate`/`benchmark`/`debug` subcommands) that calls `lsfg-vk-backend` **directly, with no Vulkan layer involved at all.** |
| `lsfg-vk-ui/` | Qt/QML | Config UI. Not relevant to a port (gamescope has its own config UI). |

**Rough proportion: the actual frame-gen algorithm (`lsfg-vk-backend`) is roughly
2x the size of the layer glue it depends on, and — critically — is already consumed
completely independently of that glue** by `lsfg-vk-cli`. This is the crux finding:
the user's premise ("if the algorithm is a clean compute pipeline behind a thin layer
wrapper, the port is plausible") is **empirically true of the current codebase, not
hypothetical** — LSFG-VK's own maintainer already ships a swapchain-free consumer of the
exact same backend a gamescope port would use.

Sources: [repo tree, `develop`](https://github.com/PancakeTAS/lsfg-vk/tree/develop)
(checked 2026-08-21), [`lsfg-vk-cli/src/main.cpp`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/lsfg-vk-cli/src/main.cpp),
[`lsfg-vk-cli/src/tools/debug.cpp`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/lsfg-vk-cli/src/tools/debug.cpp),
[`lsfg-vk-cli/src/tools/benchmark.cpp`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/lsfg-vk-cli/src/tools/benchmark.cpp).

## 2. What the algorithm needs as input/output

From the public API (`lsfg-vk-backend/include/lsfg-vk-backend/lsfgvk.hpp`, checked
2026-08-21) and its two working consumers (`benchmark.cpp`, `debug.cpp`):

- **`Instance(devicePicker, shaderDllPath, allowLowPrecision)`** — constructs its
  **own private `VkInstance`/`VkDevice`** internally (see `lsfg-vk-common/src/vulkan/vulkan.cpp`),
  it does not take an externally-supplied device. A gamescope integration would run this
  as a second, independent Vulkan device inside (or beside) gamescope's process, not a
  literal function call into `CVulkanDevice`.
- **`openContext(sourceFds, destFds, syncFd, width, height, hdr, flow, perf)`** —
  `sourceFds` is a `pair<int,int>` of **dma-buf file descriptors** for two alternating
  source frames; `destFds` is a `vector<int>` of dma-buf fds for the `multiplier - 1`
  generated output frames; `syncFd` is an **exported `VkSemaphore` (timeline) fd**. No
  motion vectors are supplied by the caller — the model derives motion itself from the
  two source images (confirmed by `shaderchains/` containing its own optical-flow-style
  passes, e.g. `delta0`/`delta1`/`gamma0`/`gamma1`).
- **Formats:** `VK_FORMAT_R8G8B8A8_UNORM` for SDR, `VK_FORMAT_R16G16B16A16_SFLOAT` for
  HDR (doc comment on `openContext`) — both formats gamescope's own compositor already
  produces/consumes.
- **Sync protocol:** application signals the timeline semaphore by 1 when a new source
  frame is ready; the library signals it back once per generated frame. This is a
  producer/consumer handoff over one shared semaphore, not a bespoke IPC layer.
- **Vulkan requirements** (`lsfg-vk-common/src/vulkan/vulkan.cpp`, `createLogicalDevice`/`checkFP16`):
  API version 1.2, device extensions `VK_KHR_EXTERNAL_MEMORY_FD`,
  `VK_KHR_EXTERNAL_SEMAPHORE_FD`, `VK_KHR_TIMELINE_SEMAPHORE`; `VkPhysicalDeviceVulkan12Features.timelineSemaphore = true`
  (required); `.shaderFloat16` (optional, gated by `allow_fp16`). No subgroup-op
  extension is requested at device-creation time. Resolution/multiplier: no hard
  resolution constraint found; `flow` is documented (`docs/Configuration.md`) as a
  0.25–1.0 scale factor, `multiplier` an integer ≥ 2 limited in practice by driver
  swapchain/descriptor-pool budgets (`lsfg-vk-backend/src/helpers/limits.cpp` sizes its
  descriptor pool proportionally to the multiplier).
- **Match against gamescope's `CVulkanDevice` (`src/rendervulkan.cpp`, grep-verified
  2026-08-21):**

  | LSFG-VK needs | gamescope already has |
  |---|---|
  | Vulkan 1.2+ | `src/rendervulkan.cpp:351` requires ≥1.2; gamescope's own instance targets `VK_API_VERSION_1_3` (`:3535`) |
  | `VK_KHR_EXTERNAL_MEMORY_FD` | enabled, `src/rendervulkan.cpp:575` |
  | `VK_KHR_EXTERNAL_SEMAPHORE_FD` | enabled, `src/rendervulkan.cpp:578` |
  | `timelineSemaphore` feature | enabled `VK_TRUE`, `src/rendervulkan.cpp:658` |
  | `shaderFloat16` (optional) | already probed/tracked as `m_bSupportsFp16`, `src/rendervulkan.cpp:517-526` |
  | dma-buf-backed image import/export | gamescope's entire buffer-sharing model is dma-buf based: `VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME` enabled (`:576`), `CVulkanTexture::BInit(...wlr_dmabuf_attributes*...)` imports (`:2026`), `GetMemoryFdKHR` exports a texture as a dma-buf fd (`:2422`), `vulkan_create_texture_from_dmabuf` (`:3584`) round-trips it |

  **Every Vulkan feature/extension LSFG-VK's backend requires is already enabled in
  gamescope's own device setup, with no additions needed.** This is an unusually clean
  match — better than a generic "yes, Vulkan 1.2 GPUs mostly support this" answer;
  gamescope enables these specific bits today, for its own dma-buf compositing, for
  reasons independent of frame generation.

Sources: [`lsfgvk.hpp`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/lsfg-vk-backend/include/lsfg-vk-backend/lsfgvk.hpp),
[`vulkan.cpp`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/lsfg-vk-common/src/vulkan/vulkan.cpp),
[`limits.cpp`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/lsfg-vk-backend/src/helpers/limits.cpp),
[`docs/Configuration.md`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/docs/Configuration.md)
(all checked 2026-08-21); gamescope citations grep-verified against this repo,
2026-08-21.

## 3. Where it would sit in gamescope's frame pipeline

Composite/present today: `paint_all()` (`src/steamcompmgr.cpp:2564`, called from the
steamcompmgr thread's vblank loop at `:9488`) → `vulkan_composite()`
(`src/rendervulkan.cpp:4030`) → `IBackendConnector::Present()` (`src/backend.h:205`),
vblank-paced by `gamescope::CVBlankTimer` (`src/vblankmanager.hpp:27,42-43`).

- **Retaining the previous composited frame is already cheap.** Gamescope does not
  composite into a single throwaway image; `g_output.outputImages[3]`
  (`src/rendervulkan.cpp:577`, populated `:3314-3331`) is a small retained ring buffer
  indexed by `nOutImage`, and `vulkan_composite()`'s own signature already takes
  `pOutputOverride`/`pPipewireTexture` parameters for handing the composited image
  elsewhere (`:4030`) — gamescope already exports its composited output as a texture for
  Pipewire screen capture today, which is functionally the same "grab the composited
  frame as a separate dma-buf-exportable texture" step a frame-gen hookup needs. Holding
  a reference to "the previous composite" is not new plumbing; it's closer to indexing
  one further into a ring gamescope already maintains.
- **Overlay placement matters and is unresolved by the algorithm itself.** Gamescope
  composites the Steam overlay / on-screen notification layer *into* the same output
  image before presenting (`nReservedLayers`, `src/steamcompmgr.cpp:2461`;
  `g_zposOverlay`, `:2232`/`:2803`). Frame generation must run on the **pre-overlay**
  composite (the game content only) and have the overlay/ImGui/FPS-counter layer
  composited back in *after* interpolation, per real and generated frame alike —
  otherwise static UI elements get smeared/ghosted by a motion-interpolation pass that
  has no business touching them (this is the same artifact class the sibling doc's Q6
  flagged for the layer approach; it applies identically here since it's a property of
  "interpolate the final image," not of how the interpolator is invoked). This is a
  pipeline-ordering requirement, not a blocker — but the overlay-composite step would
  need to move to *after* the frame-gen stage rather than being baked into the one
  composite gamescope does today.

Sources: grep-verified against this repo, 2026-08-21 (paths/line numbers above).

## 4. The presentation-cadence problem

This is the part that deserves the same "say so plainly" treatment as the licence gate.

- **Gamescope presents exactly once per vblank interval it chooses to repaint.**
  `CVBlankTimer` fires, `ProcessVBlank()` decides whether to call `paint_all()`, which
  does one `vulkan_composite()` and one `Present()`. There is no concept today of
  "present N times within one refresh interval" on any backend — DRM presents via one
  `drmModeAtomicCommit` per commit (`src/Backends/DRMBackend.cpp:2245`,`:4111`, per the
  sibling doc), which is coarser and less flexible than a windowed swapchain's present
  queue. Making room for interpolated frames means the vblank-timer/paint_all loop needs
  to become a scheduler capable of firing multiple composite+present cycles inside one
  refresh interval (for a fixed-refresh target) or coordinating with VRR's variable
  interval (see below) — a genuine change to the render-loop's shape, not an additive
  hook alongside it.
- **Interpolation is lagging by construction.** Producing a frame between real frame A
  and real frame B requires already holding B — so B's presentation must be delayed
  until after the interpolated frame(s) between A and B have shown. This adds at least
  one real frame's worth of latency before *any* generated frame reaches the screen, on
  top of whatever latency the interpolation compute work itself costs. Gamescope's
  existing vblank-synchronous, single-hop pacing is deliberately tight specifically to
  minimize input latency for gaming (Steam Deck, handhelds); this is a direct trade
  against the reason that pacing model exists, not a side effect that can be optimized
  away.
- **VRR interaction is a real, not hypothetical, conflict.** `cv_adaptive_sync`
  (`src/steamcompmgr.cpp:295`) lets gamescope vary the commit interval per real frame's
  readiness. Interleaving generated frames at sub-interval cadence needs either (a) a
  fixed effective refresh rate of `real_fps × multiplier` within VRR's supported range —
  which fights the entire point of VRR (matching cadence to whatever the game actually
  produces) — or (b) forcing fixed-rate presentation while frame-gen is active, which is
  exactly the workaround LSFG-VK's own docs already recommend today
  (`ENABLE_GAMESCOPE_WSI=0`, "pacing_mode = none forces V-Sync", noted in the sibling
  doc's Q4). **LSFG-VK's own upstream pacing model is admittedly primitive** —
  `docs/Configuration.md`'s Pacing Modes section states plainly "there are no other
  pacing modes yet" — so building correct multi-present-per-interval, VRR-aware pacing
  *inside* gamescope's loop is not a case of "reuse LSFG-VK's solved pacing logic," it is
  **new engineering gamescope would have to invent, that doesn't exist anywhere in the
  ecosystem yet, in-process or in-compositor.**
- **The FPS limiter (`g_nSteamCompMgrTargetFPS`, `src/steamcompmgr.cpp:960`, gating
  logic `:6124-6187`) would also need to become multiplier-aware** — today it targets a
  cadence for real frames; with frame-gen active it would need to target
  `real_fps = display_fps / multiplier` and let interpolation fill the rest, which is a
  change to what the limiter is even computing, not just a new setting.

**This — not the algorithm, not (given the separate-process architecture) the licence —
is where the genuine, unresolved engineering risk concentrates.** It is real, it is
non-trivial, and there is no existing reference implementation (upstream LSFG-VK
included) that has solved it well. Say this as clearly as the licence gate: **if the
project isn't prepared to design new vblank/VRR/FPS-limiter-aware scheduling, the
"algorithm fits" finding above does not by itself make this shippable.**

Sources: grep-verified against this repo, 2026-08-21;
[`docs/Configuration.md`](https://github.com/PancakeTAS/lsfg-vk/blob/develop/docs/Configuration.md)
Pacing Modes section, checked 2026-08-21; sibling doc `lsfg-vk-integration.md` Q3/Q4
(cadence and pacing-conflict findings reused, not re-derived).

## 5. Effort estimate and staged path

**Cheap kill-test — and upstream has essentially already run it for you.**
`lsfg-vk-cli debug <folder-of-frames>` (`lsfg-vk-cli/src/tools/debug.cpp`) already
dispatches the real shader chain on a sequence of static images completely offline —
own Vulkan instance, dma-buf-fd-imported images, no Vulkan layer, no swapchain, no
pacing. **Milestone 0, effort: an afternoon:** build `lsfg-vk-cli` standalone (per its
own `docs/Building-From-Source.md`, entirely outside gamescope-ritz's tree — this
doesn't touch or link against gamescope), feed it two screenshots of an actual
gamescope-composited frame pair, and inspect the interpolated output for quality and
artifacts around the overlay/notification layer. This alone tells you whether the
model's output is good enough to bother with the rest, before any gamescope code is
touched. **It is the single highest-value, lowest-cost step and should happen before
anything else in this plan.**

- **Milestone 1 (days): dma-buf handoff prototype, off to the side of gamescope's own
  build/binary.** A throwaway harness (not merged, not shipped) that uses gamescope's
  *existing* dma-buf export path (the same `GetMemoryFdKHR` machinery that already
  backs Pipewire capture, `src/rendervulkan.cpp:2422`) to grab two real composited
  frames, hands their fds to a locally-built `lsfg-vk-backend` via `openContext`, and
  confirms the fd/format/semaphore contract actually round-trips with gamescope's real
  textures (not just LSFG-VK's own synthetic DDS test images). Still zero pacing
  changes, zero linkage into gamescope-ritz's own binary.
- **Milestone 2 (1-2 weeks): wire it into `paint_all()` without changing presentation.**
  Call the backend once per composite, on the composited-but-pre-overlay image, generate
  the interpolated frame(s), but **discard or only log them** rather than presenting —
  proves the integration is correct and fits gamescope's real per-frame time budget
  inside the actual compositor process, without touching `CVBlankTimer` or any present
  path at all.
- **Milestone 3 (weeks to a couple months, genuinely uncertain): the pacing
  rearchitecture from Q5.** Multi-present-per-interval scheduling, VRR-awareness,
  FPS-limiter integration, re-validated per backend (DRM/Wayland/Headless/OpenVR each
  present differently). This is the R&D-grade part; nothing upstream or in this research
  de-risks it, and it's the one milestone that could still fail outright (e.g. discover
  DRM atomic-commit timing genuinely can't be subdivided finely enough on some hardware).
- **Resolve the licence/architecture question before Milestone 2**, since it determines
  *how* Milestone 2+ calls the backend — an in-process call (only viable if the
  relicensing decision is made) versus an IPC/dlopen boundary to a separately-installed
  component (viable without relicensing, per the Licence gate). Building Milestone 1/2
  against the wrong assumption means redoing the integration boundary later.

**Honest total:** a go/no-go on quality (Milestone 0) costs essentially nothing. A
correctness-proven, still-not-presenting prototype (Milestones 1-2) is genuinely a
matter of days-to-weeks given how much of the hard decoupling work upstream has already
done. A shippable, pacing-correct feature (Milestone 3) is a multi-week-to-few-month
effort with real technical uncertainty — closer to the sibling doc's "R&D-grade"
characterization of option (c), but now scoped to one specific hard problem
(presentation cadence) rather than the whole port being an unknown.

## 6. Compare against the fallback (option (a))

The sibling doc's option (a) — gamescope as config/launch UI only, LSFG-VK stays in the
game's own process on its own real swapchain — remains the **lower-risk, faster-to-ship**
choice: no licence entanglement (gamescope-ritz code never touches LSFG-VK source), no
pacing rearchitecture (the game's own present loop paces itself, same as it does for
every other Vulkan game today), and it's the configuration LSFG-VK's own docs already
describe workarounds for. This scout's findings **raise, but don't erase, the gap**
between the two options: the in-tree port is now a well-scoped, evidence-backed
engineering project rather than a research-grade unknown, but it still costs real weeks
of vblank/VRR-aware scheduling work and either a relicensing decision or an
IPC/dlopen architecture, for a benefit (frame-gen "just working" without per-game
Vulkan-layer setup) that option (a) mostly already delivers with a settings panel.

**Recommendation: ship (a) first regardless.** It is not exclusive with a later in-tree
port — (a) can ship now, and if the project later decides the licence trade-off and the
pacing R&D are worth it, the port can replace it without (a) having been wasted work
(the config UI, `Lossless.dll` path field, and multiplier/flow-scale/performance-mode
settings from the sibling doc's Q7 are needed either way). Pursue the in-tree port only
if (a)'s ceiling (per-game Vulkan-layer quirks, no control over pacing when it's the
game's own present loop) turns out to matter in practice, and only after Milestone 0
above confirms the model's output quality is worth the Milestone 3 investment.

## Risks

- **Licence risk (see gate above):** the literal in-tree port, as the user described it,
  forces a GPLv3 relicense of the distributed gamescope-ritz binary unless the
  separate-process/dlopen architecture is used instead — and that architecture choice
  must be made deliberately, not discovered mid-implementation.
- **Pacing risk (Q5):** the hardest unsolved problem in this whole proposal, with no
  existing reference solution anywhere, including in LSFG-VK's own mature in-process
  implementation. Treat Milestone 3 as the one step that could still kill the project
  outright.
- **Upstream churn:** `develop`'s current backend/common/layer split is itself fairly
  recent (a "v2" restructuring per the sibling doc's note on LSFG-VK's README banner);
  the fd-based `lsfgvk.hpp` API is a moving target and should be re-verified immediately
  before any real implementation work, not assumed stable from this snapshot.
- **Overlay/UI ghosting (Q3):** unresolved by the algorithm itself — requires the
  overlay composite step to move to *after* frame generation in the pipeline, a
  restructuring of `paint_all()`'s current single-pass composite order.
- **Latency regression** *(corrected 2026-09-05 — see [`fidelityfx-opticalflow-framegen.md`](fidelityfx-opticalflow-framegen.md) §5: with the target pinned to the display refresh and the FPS-limiter vblank divisor holding the game at refresh/2, 2x frame gen is one composite + one present per vblank, and the added latency is one refresh interval plus compute, not a full game frame)*: frame generation adds at least one real frame of delay by
  construction, independent of how well pacing is engineered — a real user-facing cost
  on latency-sensitive targets (Steam Deck, OpenVR) that no amount of implementation
  quality removes.
- **Dual-GPU / GPU-selection:** LSFG-VK's `Instance` picks its own physical device via a
  caller-supplied `DevicePicker`; it must be steered to match whichever GPU gamescope
  itself is compositing on (dual-GPU setups are explicitly unsupported upstream, per the
  sibling doc's Q7 finding — unchanged here).

## Open questions for the user

1. **Given the licence gate, which shape of "in-tree" is actually wanted:** vendoring
   GPLv3 source into gamescope-ritz's own binary (forces a GPLv3 relicense of the whole
   distributed binary), or a separately-installed GPLv3 component gamescope talks to
   over dma-buf fds at runtime (keeps gamescope-ritz BSD-2-Clause, but is closer in
   spirit to option (a) than to a true merge)? This decision gates how Milestone 2+ is
   even built.
2. Is the project willing to accept relicensing gamescope-ritz's distributed binary to
   GPLv3 if the separate-process path is rejected as insufficiently "in-tree"? This is
   the one question that, answered "no," ends the literal in-tree port regardless of
   how favorable the technical findings above are.
3. Given Q5's finding that pacing is genuinely unsolved even in LSFG-VK's own mature
   in-process form — is there appetite for the multi-week-to-month scheduling R&D in
   Milestone 3, or should this effort stop at Milestone 0/1 (a quality/feasibility
   check) and revert to shipping option (a)?
4. Should Milestone 0 (the offline quality check using `lsfg-vk-cli debug` on real
   gamescope screenshots) happen before this scout's findings are acted on at all, given
   it's nearly free and would directly inform whether Milestones 1-3 are worth
   scheduling?
5. If frame generation ships inside gamescope's own composite loop, should it be
   restricted to VRR-disabled / fixed-refresh sessions only (sidestepping the VRR
   scheduling problem in Q5 entirely, at the cost of not supporting the project's VRR
   users), or is full VRR interop a hard requirement for launch?

---
*Checked 2026-08-21 against gamescope-ritz `master` (commit `fcc1341` and earlier) and
LSFG-VK `develop` branch (tree listing and file contents fetched live via GitHub API,
2026-08-21 — no specific pinned commit hash was available from this fetch method; treat
line-number citations as approximate if LSFG-VK moves before this is acted on, and
re-fetch before implementation).*
