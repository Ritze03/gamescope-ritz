# Overlay presentation architecture

> ## RESOLVED — 2026-08-21 (later same day). The "unexplained" fullscreen VRR
> ## artifacting from the "CONCURRENT fix did not cure it" section is an SDL-backend
> ## bug. Read this before relying on that section's "unexplained" framing or the
> ## stacked-VRR hypothesis.
>
> Confirmed on real hardware: `DISABLE_LSFG=1 ./build/src/gamescope --backend sdl -f
> -- vkcube` flickers badly; the identical binary with **no `--backend` flag** (auto-
> selecting Wayland — `auto_select_backend()` in `src/main.cpp`) is completely clean.
> The user's packaged upstream 3.16.24 flickers under SDL too — matching exactly what
> the "Stock-vs-ours comparison" below found ("indistinguishable... consistent with
> the tearing is not our defect") and explaining it fully: **that whole comparison
> ran `--backend sdl` for both binaries** (see the command line quoted there), so
> "indistinguishable" was correctly observed but for the wrong reason — not "both are
> equally fine", but "both hit the same upstream SDL-backend bug". Consequences for
> the rest of this doc:
>
> - The **"stacked adaptive-sync layers" hypothesis is superseded/ruled out**: Hyprland's
>   `vrr = 3` engages the host VRR layer for *any* fullscreen game/video-content
>   window regardless of which gamescope backend is running underneath, yet only the
>   SDL backend flickers — so double VRR stacking alone is not sufficient to cause
>   the symptom. Don't chase "set DP-1's `vrr` to `0`" as the next test; the SDL vs.
>   no-`--backend` A/B already is the decisive test, and it's done.
> - **Section 0's queue-family `CONCURRENT` fix and the wait-stage-mask sync-validation
>   fix remain correct, worthwhile fixes on their own merits** — they just were never
>   going to cure this symptom, because the symptom lives in the SDL backend, not in
>   our overlay texture production. No further action needed on either.
> - **Defect A ("the artifacting") is explained; revisit-trigger #2 in the final
>   section ("if the stacked-VRR hypothesis is ruled out") is now moot** — the
>   injected-texture-path rewrite decision below stands, on firmer footing than
>   before.
> - Draft upstream report: `superdoc/planning/upstream-sdl-backend-flicker-report.md`.
>
> The `Status:` line below and everything through "the CONCURRENT fix did not cure
> the artifacting" is kept for the record (Section 0's Vulkan spec-violation fix,
> the sync-validation findings, and the ImGui context hardening are all real,
> independent fixes worth keeping) — just read it with the above in mind.

Status: decided, **with Section 0's root-cause claim retracted** — see
"2026-08-21: the CONCURRENT fix did not cure the artifacting" at the end of this
file before relying on anything in Section 0.

This document answers three questions the user asked, in the order that makes
the answers make sense:

1. How does the Steam overlay actually draw, and how does it get input?
2. Should we draw the overlay from a Vulkan WSI layer, like MangoHud?
3. Should we become a real surface presented like `GAMESCOPE_EXTERNAL_OVERLAY`?

The short version: **the artifacting was not caused by our presentation
architecture.** It was a Vulkan queue-family ownership violation in how we
*produce* the overlay texture. That reframes all three questions, so the root
cause comes first.

---

## 0. Root cause of the artifacting

`CVulkanDevice` owns two queues (`src/rendervulkan.hpp:919-930`):

- `m_queue` / `m_queueFamily` — the **compute-only** family, used by
  `vulkan_composite()`.
- `m_generalQueue` / `m_generalQueueFamily` — the **graphics** family, used by
  `CVulkanDevice::generalCommandBuffer()`.

On this machine those are genuinely different families. Verified at runtime,
not inferred:

```
vulkan: selecting physical device 'AMD Radeon RX 7900 XTX (RADV NAVI31)':
        queue family 1 (general queue family 0)
```

Both overlay textures — `SettingsOverlay.cpp`'s and `Overlay/FpsDisplay.cpp`'s
independent ImGui render targets — are:

- **written** on queue family 0, by the ImGui render pass in `RenderAndSubmit()`
- **sampled** on queue family 1, by the compute shader in `vulkan_composite()`

and were created `VK_SHARING_MODE_EXCLUSIVE`, because that is the only mode
`CVulkanTexture::BInit()` had (`src/rendervulkan.cpp`, the `imageInfo`
initialiser).

Per Vulkan 1.3 §7.7.4, an `EXCLUSIVE` resource accessed by a second queue
family **without an explicit queue family ownership transfer has undefined
contents**. There was no such transfer. The one barrier in `RenderAndSubmit()`
passes `VK_QUEUE_FAMILY_IGNORED` on both sides and runs once, at texture
creation.

Worse, gamescope *does* have ownership-transfer machinery —
`CVulkanCmdBuffer::insertBarrier()` tracks an owner in `image->queueFamily` and
emits `srcQueueFamilyIndex`/`dstQueueFamilyIndex` transfers — but the overlay
bypasses it entirely. `insertBarrier()` sees `queueFamily ==
VK_QUEUE_FAMILY_IGNORED` on first sight, assigns it `m_queueFamily` (1,
compute), and from then on emits `src == dst == 1` — a no-op. The texture is
permanently recorded as compute-owned while the graphics queue writes it every
frame.

**A timeline semaphore does not fix this.** `SettingsOverlay_WaitForRender()`
gives execution and memory dependency; ownership transfer is a separate,
additional requirement. That is why the existing semaphore did not help, and
why the reverted double-buffering attempt (`e171f72`) did not either — it
changed *when* the texture was written, not *whether the write was legal*.

### Why this produces exactly the reported symptom

- **Whole screen, not just the overlay.** Both overlay layers are full-screen,
  alpha-blended `FrameInfo_t` layers. Undefined contents means undefined
  *alpha* across all 1920x1080, so the garbage composites over the entire game
  image rather than staying inside the panel.
- **Worse with non-Linear filters.** Linear is hardware-scaled, so
  `commit_t::ShouldPreemptivelyUpscale()` returns false. Every other filter
  makes it true, which adds a **second `vulkan_composite()` per frame**
  (`src/steamcompmgr.cpp:7766-7800`) — a second cross-family read of the same
  illegally-shared image each frame. "Linear sometimes flickers, the others are
  really bad" is exactly the shape this predicts.
- **Only fullscreen, only VRR, only at 280 Hz.** These raise the transition
  rate and remove the idle gaps that would otherwise let caches and DCC
  metadata settle incidentally. The bug is timing-sensitive undefined
  behaviour, not a deterministic fault — which is precisely why a previous
  attempt testing in a small window at fixed refresh saw nothing.
- **The unexplained GPUVM fault** during the double-buffering experiment is
  consistent with this, and was never actually attributable to double
  buffering.

On RADV/Navi31 the concrete mechanism is DCC (colour compression) metadata: the
graphics queue writes the image compressed, and with no release barrier the
metadata is never resolved for the compute family, which then samples an image
whose compression state it disagrees with.

### Why upstream never hits it

Upstream gamescope (cloned and read, not assumed) has the same two queues but
**no `generalCommandBuffer()`** — that factory is our fork's addition. Upstream
has exactly one general-queue user, ReShade, and it handles the cross-family
problem by brute force:

```cpp
// upstream src/reshade_effect_manager.cpp:1185-1186, 1210-1211
device->submitInternal(&*m_cmdBuffer);
device->waitIdle(false);
```

plus `g_device.wait(seq)` on the composite side. A full device drain between
the graphics write and the compute read makes the ownership question moot.

So **there is no proven Valve pattern for doing this asynchronously to copy.**
Our overlay is the first thing in this codebase to hand general-queue-rendered
content to the compute queue without a full stall — deliberately, because a
`waitIdle()` per frame at 280 Hz would serialise CPU and GPU and destroy frame
pacing. Copying upstream's pattern here would be a correctness fix that trades
one visible defect for another.

### The fix shipped

1. New `CVulkanTexture::createFlags::bGeneralQueueShared`. When set *and* the
   two families actually differ, the image is created
   `VK_SHARING_MODE_CONCURRENT` over `{compute family, general family}`. Both
   overlay textures set it. `insertBarrier()` emits `VK_QUEUE_FAMILY_IGNORED`
   for concurrent images, since a concurrent image has no owner to transfer and
   emitting a transfer for one is invalid usage.

   Chosen over hand-matched release/acquire barrier pairs because the number of
   composite reads per frame is **variable** — one normally, two under
   preemptive upscale, zero when the connector is not visible, plus any
   screenshot composite. Matching pairs across that is exactly the kind of
   bookkeeping that produced the double-buffering failure. The cost is losing
   DCC on two UI textures, which is noise.

2. **Issue #22 closed properly**, in both overlays. A second timeline semaphore
   (`s_pReadDoneSemaphore`) carries the return signal: every
   `vulkan_composite()` submission that could sample the texture signals a
   fresh point, and the next `RenderAndSubmit()` makes its general-queue
   submission **wait** on the last such point before its
   `VK_ATTACHMENT_LOAD_OP_CLEAR`. This closes the write-after-read window
   rather than widening it, needs no second texture, and costs no CPU stall.

   Deadlock safety is structural: a point is only ever waited on after
   `SettingsOverlay_CommitReads()` / `FpsDisplay_CommitReads()` promotes it,
   and those are called from `vulkan_composite()` *after* `g_device.submit()`.
   A point belonging to a command buffer that was never submitted is simply
   never promoted, so the general queue can never block on a signal that will
   not come.

3. **Resize lifetime hazard fixed.** `EnsureTexture()` dropped the old texture
   on output-resolution change while a general-queue submission could still be
   rendering into it. Unlike the compute side — whose
   `CVulkanCmdBuffer::m_textureRefs` holds its own `Rc<>` on everything it
   sampled — the general-queue submission names the texture only as a raw
   `VkImageView` in its `VkRenderingAttachmentInfo` and keeps nothing alive.
   Both files now `DrainPrevSubmission()` before replacing. Resizes are rare,
   so a stall there is the right trade.

---

## 1. How does the Steam overlay actually draw?

Two different mechanisms get conflated. Both are real; only one applies on
gamescope.

### Steam's classic desktop overlay

`gameoverlayrenderer.so`, `LD_PRELOAD`ed into the game, hooks the graphics API
and draws into the game's own swapchain image before present. This is the
MangoHud/vkBasalt approach, and it is what the WSI-layer proposal describes.

### Steam on gamescope (the Deck, the QAM) — what actually happens

Steam's UI is a **separate client window that gamescope composites as a
layer**. Confirmed in both our fork and upstream:

- `src/steamcompmgr.cpp:1124-1125` defines two distinct atoms:
  `STEAM_OVERLAY` (`isOverlay`) and `GAMESCOPE_EXTERNAL_OVERLAY`
  (`isExternalOverlay`).
- `w->isOverlay` is read from an X11 property on an Xwayland client window
  (`src/steamcompmgr.cpp:5185`), so it is by construction another process's
  window.
- It is composited through the ordinary `paint_window()` path
  (`src/steamcompmgr.cpp:2490-2492`), producing an ordinary `FrameInfo_t`
  layer.
- `FrameInfo_t::blurLayer0` exists to blur the game behind it
  (`src/steamcompmgr.cpp:2868`).

**This is the significant finding, and it argues against the WSI rewrite.**
Valve's own overlay on gamescope uses the same composited-layer architecture
ours does. The approach is proven in exactly the VRR/fullscreen conditions
where ours was failing. Our implementation was the thing that was wrong.

### How Steam's overlay receives input — and why `GAMESCOPE_EXTERNAL_OVERLAY` is the wrong thing to copy

This was the pivotal question, and the code settles it. The two atoms are
**not** interchangeable:

- **`STEAM_OVERLAY` + `STEAM_INPUT_FOCUS`** is what the QAM uses. When
  `overlayWindow->inputFocusMode` is set, gamescope hands it both pointer and
  keyboard focus:

  ```cpp
  // src/steamcompmgr.cpp:4580-4585
  if ( gamescope::VirtualConnectorIsSingleOutput() &&
       pFocus->overlayWindow && pFocus->overlayWindow->inputFocusMode )
  {
      pFocus->inputFocusWindow = pFocus->overlayWindow;
      pFocus->keyboardFocusWindow = pFocus->overlayWindow;
  }
  ```

- **`GAMESCOPE_EXTERNAL_OVERLAY`** has **no input path at all**. It is
  excluded from focus candidacy (`src/steamcompmgr.cpp:1078`) and is never
  assigned to `inputFocusWindow` or `keyboardFocusWindow` anywhere in the file.
  In the entire presentation path it does exactly **one** thing:

  ```cpp
  // src/steamcompmgr.cpp:2251-2254
  if ( w->isExternalOverlay )
      layer->zpos = g_zposExternalOverlay;
  ```

So the instruction "copy `GAMESCOPE_EXTERNAL_OVERLAY` exactly" rests on a
misidentification: that atom is not what the QAM uses, and copying it would
produce an overlay that **cannot receive input**, destroying M2 for no gain.

And there is nothing to copy regardless. `isExternalOverlay` sets a zpos. It
uses the *same* `FrameInfo_t` layer mechanism our overlay already uses. There
is no distinct "external overlay presentation path" — we were already on it.
The only real difference was texture production, which is what Section 0 fixed.

---

## 2. Should we draw from a Vulkan WSI layer, like MangoHud?

**No.** Recommendation: reject.

It was proposed on the theory that "an extra layer changes gamescope's
presentation path under VRR". That theory is now falsified — the cause was a
spec violation in texture production, fixed in ~40 lines. The WSI layer would
have fixed the artifacting only as an incidental side effect of changing how
the texture is produced, at enormous cost.

The costs, all real:

- The overlay would live in the **game's process**. Config, `Audio::GetState()`,
  the whole `src/Config/` layer, the ConVar surface and M2's input capture
  would all need IPC across a process boundary.
- It would render at the **game's** resolution and be upscaled with it, making
  the blurriness the user already complained about strictly worse.
- It would appear inside **screenshots and streams** as part of the game image.
- It would not work at all for a client that does not use the WSI layer.
- Valve does not do this on gamescope, which is the strongest evidence
  available that it is not the right architecture here.

`VkLayer_FROG_gamescope_wsi` remains the right place for swapchain redirection.
It is not the right place for the overlay.

---

## 3. Should the overlay become a real surface?

**Not now.** Recommendation: keep the in-process `FrameInfo_t` layer.

This was worth taking seriously — gamescope hosts its own Wayland server
(`src/wlserver.cpp`), so an internally-owned `wl_surface` is possible in-process
with no IPC, and it would bring `blurLayer0` along. But:

- The presentation benefit is **zero**. A surface-backed overlay would arrive
  as a client commit and be composited as a `FrameInfo_t` layer — the same
  layer path we already use. The only thing it would change is texture
  production, and Section 0 already fixed that directly and provably.
- The input mechanism it would inherit requires being an X11/Xwayland client
  window with `STEAM_OVERLAY` + `STEAM_INPUT_FOCUS`. `isOverlay` is read via
  `get_prop(ctx, w->xwayland().id, ...)` — gamescope would have to become a
  client of its own embedded server, with all the wlserver-lock and
  thread-ordering hazards that implies.
- It would replace M2's input capture wholesale — the most expensive thing
  built so far — to reach the same pixels.

**`blurLayer0` does not require this.** It is a `FrameInfo_t` flag
(`src/steamcompmgr.cpp:2868`), settable from our existing layer without any
surface. If the backdrop blur is wanted, that is a small, separate change; see
the `experimental/blurred-background` branch.

Revisit only if we later need the overlay to be a real focus target for
reasons other than presentation.

---

## Secondary finding, not fixed

`CWaylandFb::m_bCompositorAcquired` (`src/Backends/WaylandBackend.cpp:946-971`)
is a **bool, not a refcount**. If the same buffer is presented twice before the
host compositor releases it, `OnCompositorAcquire()` no-ops the second time,
and the second `wl_buffer.release` falls into the `else` branch:

```
xdg_backend: Compositor released us but we were not acquired. Oh no.
```

Observed 3x during fullscreen mode transitions and teardown; **0 occurrences in
30 s of steady-state fullscreen VRR playback**, so it is not the artifacting
cause. It is still a genuine buffer-lifetime tracking bug, and under VRR
solitary/direct-scanout a wrong refcount could in principle let gamescope reuse
a buffer the display is still scanning out. Related: `g_output.nOutImage`
advances `(n + 1) % 3` unconditionally (`src/rendervulkan.cpp:4343`) without
consulting host release state.

This is upstream code. Left alone deliberately: it could not be observed
failing in steady state, and changing refcounting in the nested backend without
a reproducible failure risks introducing something worse. Documented for
follow-up.

---

## What was verified, and what was not

Verified:

- Queue families genuinely differ on this GPU (family 1 vs 0), from gamescope's
  own startup log on the user's hardware.
- The exact failing configuration was reached — fullscreen 1920x1080 on **DP-1**
  (`fullscreen=2`, `monitor=0`), with `vrr=True` and Hyprland in **solitary**
  mode, confirmed via `hyprctl monitors`/`clients` while gamescope was running.
  This is the configuration a previous attempt failed to reach.
- Stable across `linear`, `fsr`, `nis`, `nearest` in that configuration, with
  the FPS HUD enabled: no hang, no `VK_ERROR_DEVICE_LOST`, no GPUVM fault in
  the kernel log, no deadlock from the new GPU-side wait.
- `meson test`: 63/63.

**Not** verified:

- **Whether the artifacting is visually gone.** This was diagnosed and fixed
  from an agent session with no eyes on the display. gamescope's screenshot
  path reads back a coherent GPU buffer, so scanout-level corruption is
  structurally invisible to it — it could not have been captured even in
  principle.
- Vulkan validation layers are **not installed** on this system and installing
  system-wide packages was out of scope, so there is no validation-layer or
  sync-validation confirmation of either the original violation or its absence
  now. Installing `vulkan-validation-layers` and re-running with
  `VK_LAYER_KHRONOS_validation` (sync validation enabled) is the single test
  that would settle it independently.

The root cause is a spec violation that is certain from reading the code and
confirmed against this GPU's actual queue-family configuration. That it
*explains* every reported symptom is strong but circumstantial. The user's own
eyes on DP-1 remain the confirming test.

> **Superseded 2026-08-21.** The user has now run that confirming test and the
> artifacting is still present. The claim that this spec violation explained the
> symptom is retracted — see the next section.

---

## 2026-08-21: the CONCURRENT fix did not cure the artifacting

Hardware retest by the user, on the build that already carries the
`bGeneralQueueShared` / `VK_SHARING_MODE_CONCURRENT` fix from Section 0:

> Major tearing with fullscreen enabled, independent of FPS Overlay, Overlay
> open/closed or the Filter.

**Section 0's root-cause claim is therefore retracted.** The
`EXCLUSIVE`-without-ownership-transfer finding was a real Vulkan spec violation
and the fix is kept on its own merits, but it did **not** cause the symptom the
user reports. The artifacting is **unexplained**. Its per-symptom explanations
("worse with non-Linear filters", "only fullscreen, only VRR") are now
contradicted by the user's observation that the symptom is independent of
overlay state and filter.

### Stock-vs-ours comparison (the experiment that partitions the problem)

Both binaries run identically: `--backend sdl -f -W 1920 -H 1080 -r 280
--adaptive-sync -- vkcube`, then moved to DP-1 and fullscreened, with the
fullscreen state **asserted** (`fullscreen=2`) rather than assumed.

| | stock `/usr/bin/gamescope` 3.16.24 | ours (`4f027b6`) |
|---|---|---|
| fullscreen on DP-1 | `fullscreen=2` | `fullscreen=2` |
| DP-1 `vrr` | `true` | `true` |
| DP-1 `solitary` (direct scanout) | our window | our window |
| DP-1 `activelyTearing` | `false` | `false` |
| refresh cycle reported | 3.57 ms | 3.57 ms |
| GPUVM / device-loss markers | 0 | 0 |
| abort on SIGTERM shutdown | **yes** (`Aborted`, core dumped) | intermittently yes |

On every host-observable axis the two are **indistinguishable**. That is
consistent with "the tearing is not our defect", but it is not proof: tearing is
a scanout-level visual artifact and no instrument available here can see it.
**The user's own A/B on DP-1 is still the confirming test.**

The shutdown abort is **upstream behaviour**, reproduced on stock. Our overlay
work did not introduce it.

### Environmental finding: two stacked adaptive-sync layers, always on

`~/.config/hypr/subcfgs/monitors.lua` sets **`vrr = 3`** on DP-1. Hyprland's
mode 3 means "VRR on for a fullscreen window whose content type is game/video".
Measured consequence:

- gamescope's window reports `contentType: game`.
- Running gamescope fullscreen on DP-1 **with `--adaptive-sync` omitted
  entirely** still drives DP-1 to `vrr=true`.

So the host VRR layer engages **unconditionally** whenever gamescope is
fullscreen on DP-1 — regardless of gamescope's own adaptive-sync setting, and
regardless of the overlay's "VRR / Adaptive Sync" toggle. Whenever gamescope's
own adaptive sync is also on, two adaptive-sync layers are stacked on a 280 Hz
panel. This is true for stock gamescope too.

**Why this matters:** it explains the one thing the overlay hypothesis never
could — why the symptom is independent of the overlay, the FPS HUD and the
filter. It is also the only mechanism found so far that is gated on *fullscreen*
specifically, which is the user's stated trigger.

**Highest-value next test, and it is one line:** set DP-1's `vrr` to `0` in that
file, reload Hyprland, and see whether the artifacting stops.

`cv_tearing_enabled` is **not** the mechanism: it defaults false, and `bTearing`
additionally requires `GetBackend()->SupportsTearing()`, which the Wayland
backend hard-codes to `false` (`src/Backends/WaylandBackend.cpp`).

### What sync validation found

`vulkan-validation-layers` is **not** installed system-wide. The Arch package is
unpacked into the session scratchpad and pointed at with `VK_LAYER_PATH` +
`LD_LIBRARY_PATH` per run, so nothing outside the scratchpad changed.

1. **`VUID-vkQueueSubmit-pWaitDstStageMask-00066` — fixed.**
   `CVulkanDevice::submitInternal()` named `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`
   as a wait stage for *every* external dependency, including on the
   **compute-only** queue `vulkan_composite()` submits to — a graphics-only stage
   on a queue family that does not support it. Upstream carries the same
   unconditional line, but it is latent there; our overlay attaches an external
   dependency to the composite submission on **every frame**, so the violation
   fired continuously. Now masked to the target queue family's capabilities.
   Verified: 10 occurrences before, 0 after.

2. **`VUID-vkCmdDraw-None-09600` — open, not fixed.** An ImGui draw binds
   descriptors declaring `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` while the
   images are actually in `VK_IMAGE_LAYOUT_GENERAL` (4 distinct images, one
   command buffer). Left alone deliberately: `GENERAL` is a valid layout to
   sample from and RADV will not fault on it, the mechanism is not understood
   yet, and the standing rule is not to ship an unreproduced fix.

Remaining validation output is pre-existing upstream noise from gamescope's
dmabuf import path and its YCbCr composite descriptor set.

### Defect B (GPU fault on resize) did NOT reproduce

Driving real host-window resizes with both ImGui contexts live (FPS HUD on via
config, settings overlay on via the `settings_overlay_visible` convar — both
confirmed rendering in a captured screenshot, not assumed):

- SDL backend: **300** resizes, **305** swapchain recreations — no fault.
- Wayland backend: **200** resizes — no fault.
- Under sync validation: no sync hazard reported on either overlay texture.

The resize fault is **not reproduced on this machine by this method**, and no fix
for it is claimed. Two prior passes shipped unreproduced fixes for this and both
were wrong; this pass does not add a third.

A silent trap worth recording, because it invalidated the first attempt:
**Hyprland >= 0.56 dispatches are Lua-only.** `hyprctl dispatch
resizewindowpixel ...` (and every classic string dispatch, including through the
raw IPC socket) is rejected with "expected a dispatcher" *while hyprctl still
exits 0*. The first 40-resize run therefore resized nothing and "passed"
vacuously. Working form:

```
hyprctl dispatch 'hl.dsp.window.resize({ window = "address:0x...", x = W, y = H, exact = true })'
```

and the window must be floated first. `scripts/overlay-test-harness.sh` had the
same dead syntax in its `--fullscreen` reinforcement — fixed in this pass, so
`--fullscreen` now actually reaches `fullscreen=2` on the target output and says
so, instead of silently testing a tiled window.

### Texture-lifecycle audit: the Rc<> claim checks out

Section 0's resize-safety argument depends on a compute submission holding a
live reference to the overlay texture. Verified against `src/rc.h`: a *public*
`Rc<T>` bumps the private (lifetime) count on its 0→1 transition, so
`CVulkanCmdBuffer::m_textureRefs` (a `std::vector<Rc<CVulkanTexture>>`) does keep
the image alive. `EnsureTexture()`'s comment is correct as written.

### ImGui context handling — hardened

Two independent ImGui contexts live in this process, each with its own
`ImGui_ImplVulkan` backend data, font atlas, descriptor pool and per-frame
vertex/index buffers. `Overlay/FpsDisplay.cpp` saved/set/restored the
globally-current context around its pass; `SettingsOverlay.cpp` **never called
`SetCurrentContext` at all**, relying on `CreateContext()` leaving its context
current forever and on FpsDisplay's discipline to put it back. That held only
because of the exact init order — an unenforced global-state coupling between two
files. `SettingsOverlay.cpp` now owns an explicit `s_pImguiContext` and uses an
RAII `ScopedImguiContext` guard across its whole pass (covering `DrainInputQueue()`,
which writes `ImGuiIO`). Its init guard is also keyed on its own context handle
rather than "is any context current", which previously aborted init whenever the
*other* file's context merely happened to be current.

### Fork-vs-upstream divergence: there is none in the base

Checked, not assumed: our `master` base is **exactly** upstream `master`
(`fcc1341`), confirmed with `git merge-base --is-ancestor`. Every commit above it
is additive overlay work. There are therefore **no upstream fixes we are missing**
in the render/present paths, and the premise that "our fork has drifted" is
false. The one place we now diverge inside upstream code is `submitInternal()`'s
wait-stage mask, which diverges *toward* spec compliance.

Consequences, recorded so they are not re-litigated:

- There are **no upstream fixes to pull in**. If a bug reproduces on stock
  `/usr/bin/gamescope`, it is present in current upstream too — the stock
  comparison above is a test of upstream HEAD, not of an older baseline.
- There is **no separate Steam Deck implementation to copy**. The Deck runs this
  code. `GAMESCOPE_EXTERNAL_OVERLAY` really is just a zpos assignment, and at the
  compositing layer "copy how the Deck does it" is already satisfied.

### Decision: keep the injected-texture path for now — but this is the real divergence

The one thing we genuinely do that the Deck does not is **produce the overlay's
pixels**. Steam's overlay is a separate Wayland client committing real buffers
through gamescope's ordinary client-commit path (`src/commit.cpp`, dmabuf import,
`CVulkanTexture` from a dmabuf) — a path exercised for every game every frame,
and therefore extremely well tested for exactly the three things we keep finding
bugs in: queue-family ownership, resize, and lifetime. Ours renders ImGui into an
internally-created `CVulkanTexture` and injects it straight into `FrameInfo_t`,
on a path nothing else in the codebase uses.

That is a genuinely strong prior for rewriting. **The evidence gathered in this
pass does not support acting on it yet:**

- **Defect A does not point here.** The symptom is independent of the overlay
  being open, of the FPS HUD, and of the filter — i.e. independent of whether the
  injected texture is being produced or composited at all. And stock gamescope,
  which has no injected texture, is indistinguishable from ours on every measured
  axis in the failing configuration. A buffer-production rewrite cannot fix a
  symptom that is present with the producer switched off.
- **Defect B does not point here either — yet.** 500 real resizes across two
  backends, with both contexts live and sync validation on, produced no fault and
  no sync hazard on either overlay texture. There is currently no reproduction to
  attribute to the injected path.
- The overlay-attributable defect sync validation *did* find (the wait-stage mask)
  is in shared submit code and would have existed identically for a
  client-committed buffer.

So rewriting now would be a large, risky change justified by a prior rather than
by evidence — the same mistake as the two unreproduced resize fixes.

**Revisit immediately if any of these become true**, at which point the rewrite is
the right answer and should be taken:

1. Defect B reproduces and the fault is attributable to `EnsureTexture()`,
   `s_pOverlayTexture`'s lifetime, or the general-queue submission.
2. The artifacting is shown to track overlay *visibility* after the stacked-VRR
   hypothesis above is ruled out.
3. Sync validation reports a hazard on an overlay texture that cannot be fixed
   without hand-rolling the ownership/lifetime bookkeeping the client-commit path
   already does correctly.

## 2026-08-22: frame-rate-dependent double-click, root-caused and fixed

Unrelated to the artifacting investigation above -- a separate report: the max
delay between clicks for a title-bar double-click (roll shade in/out,
`Overlay/Chrome.cpp`'s drag zone, `ImGui::IsMouseDoubleClicked`) tracked
framerate. `IsMouseDoubleClicked` and `flDeltaTime` (real `get_time_in_nanos()`
deltas, clamped, fed to `io.DeltaTime` every frame) were both already correct
and were not the cause.

**Root cause**: `DrainInputQueue()` (`src/SettingsOverlay.cpp`) batches every
event queued since the last frame and hands the whole batch to ImGui in one
pass, right before that frame's single `ImGui::NewFrame()` call. ImGui's own
input-event trickling (`io.ConfigInputTrickleEventQueue`, on by default,
never touched by this codebase) applies only **one** same-button transition
per `NewFrame()` call and defers the rest to later calls -- so a fast
double-click's down/up/down/up, once backlogged into one drain (routine at
low framerate, or on any single slow frame), gets spread across several
*frames*. Each transition only becomes a "click" (and gets timestamped with
ImGui's internal `g.Time`, which advances by the real `DeltaTime` of whichever
`NewFrame()` call is doing the advancing) once its own later frame runs -- so
the gap between the two registered clicks tracks real frame *period* × frame
*count*, not the real time the clicks were actually apart. Confirmed by a
standalone instrumentation harness linking this repo's vendored
`subprojects/imgui` directly (not the gamescope binary): a realistic 150ms
double-click (comfortably under the 300ms default `MouseDoubleClickTime`)
registered correctly for simulated framerates down to ~8fps and silently
stopped registering below that -- with the simulated *real* click timing held
fixed the whole time. That is the reported symptom, reproduced in isolation.

**A tempting non-fix, measured and rejected**: setting
`io.ConfigInputTrickleEventQueue = false` does NOT fix this -- it makes ImGui
apply every event in a batch but keep only the *net* button-state change for
that frame, so a whole press+release landing in one drain (the common case at
low fps, since a real click's own down-to-up gap is often shorter than one
frame period there) collapses to **no click detected at all**. Measured
across the same framerate sweep: this "fix" broke even isolated single clicks
at framerates below ~60fps. Worse than the bug it was meant to cure -- do not
reach for this switch here.

**Actual fix**: `DrainInputQueue()` now gives every `MouseButton` event in a
batch, other than the batch's last event, its own correctly-timed "micro"
`NewFrame()`/`EndFrame()` cycle (no windows opened, nothing rendered) with
`DeltaTime` set to the real wall-clock gap since ImGui's clock was last
advanced -- using a new `ulTimestampNanos` field on the queued-event struct,
stamped from `get_time_in_nanos()` at `QueueEvent()` time (the producer side,
so as close to the real libinput event as this cross-thread handoff gets).
This makes `g.Time` track real elapsed time between transitions instead of
render-frame count, for exactly the transitions that would otherwise be
trickle-delayed within the same drain. Every other event kind (motion/wheel/
key) is untouched -- applied without a pump of its own, same as before -- and
the batch's last event is left pending for the caller's real, visible
`NewFrame()` (`SettingsOverlay_AddLayer()`, right after `DrainInputQueue()`
returns), so real render cadence and its `DeltaTime` are unaffected. Re-ran
the same instrumentation harness with this scheme: the 150ms double-click now
registers at every framerate down to 2fps; an isolated single click still
registers as exactly one click at every framerate; two genuinely separate
clicks 500ms apart never falsely merge into a double-click at any framerate.
Verified `ninja -C build` and `meson test -C build` (63/63) after the change;
M2's release-safety backstop (`io.ClearInputKeys()`/`ClearInputMouse()` on a
capturing-to-not-capturing edge) is untouched -- it still runs once, after
the whole batch, unaffected by the added micro-pumps.

## 2026-09-02: overlay text "doubling", root-caused and fixed

Reported as: every glyph in the overlay carries a ghost of itself offset by
about a pixel. It survived six investigations. The cause was **not** in the
compositor at all.

### Cause

`Overlay/Fonts.cpp`'s `ImFontConfig` never set `PixelSnapH`, which ImGui
defaults to **false**. `ImFont::RenderText()` already snaps the text *origin*
to whole pixels ("Align to be pixel perfect"), so the first glyph of a string
is always grid-aligned -- but the per-glyph *advance* is only rounded when the
font asks for it (`imgui_draw.cpp`, `ImFontAtlasBakedAddFontGlyph`: `if
(src->PixelSnapH) advance_x = IM_ROUND(advance_x);`). With it false, advances
stay fractional, x accumulates a sub-pixel error per character, and every
glyph after the first is placed off the pixel grid. The atlas is sampled
bilinearly, so an off-grid glyph is resampled and a 1px stem lands half in one
column and half in the next -- read as a bright stroke with a mid-grey ghost
beside it, worsening along a string.

Fix: `cfg.PixelSnapH = true` in `Load()`. Measuring and drawing stay
consistent because `CalcTextSizeA()` reads the same baked advances the draw
uses, so `DrawText()`'s ellipsis/alignment arithmetic is unaffected.

Measured on the label `Adaptive sync (VRR)` (nested-sway Wayland backend, 1:1,
1920x1080), counting ink pixels in the text band: ghost-to-stroke ratio
1.26 -> 1.04. Individual stems that were split ~50/50 across two columns
(`95 87`, `97 98`) became single crisp columns (`168 17`, `133 63`).

### What this was NOT -- do not re-derive these

Measured, not assumed, on the Wayland backend at 1:1 with `vkgears`:

- **The composite is pixel-exact.** The settings-overlay layer reaches
  `FrameInfo_t` exactly ONCE, at `scale 1.0,1.0` and `offset 0.0,0.0`
  (`zpos=6`). `BlitPushData_t` feeds the shader `offsetPixelCenter()`
  (`offset + 0.5/scale`), so `cs_composite_blit.comp`'s `uv = vec2(coord)`
  lands on exact texel centres. `isScreenSize()` is true for this layer, so
  it binds a NEAREST sampler anyway.
- **The Wayland presentation is pixel-exact.** A `grim` capture of the host
  output is byte-identical to the composited buffer at every probed edge. No
  fractional viewport scale, no second surface, no doubled present.
- **The UI's own 1px rules were always crisp** -- single columns end to end
  (`0 0 0 0 0 0 27 5 5 5`, divider `29x7, 66, 34x8`, top border one row). That
  is the tell that separated this from a compositing bug: ImGui draws rects
  from the atlas's white pixel, where filtering cannot matter, so only glyphs
  sample real texels and only glyphs showed it. It is also why the scaler
  filter, RCAS sharpness and the backdrop blur made no difference.
- **Not the host window being non-1:1.** Shrinking gamescope's host window to
  1900x1060 keeps borders crisp -- it re-renders at the window size rather
  than letting the host rescale a 1920x1080 buffer.

### How to capture the overlay -- this is what blocked six rounds

`gamescopectl screenshot <path>` **cannot** show the overlay, and this is not
a compositor limitation. Two things stack:

1. The default screenshot type is `base_plane_only` (value **1**), which
   truncates every layer with `zpos >= g_zposExternalOverlay` (2). The
   settings overlay is `g_zposSettingsOverlay` = **6**, so it is always cut.
2. `gamescopectl` collapses everything after the command name into ONE
   argument, so `gamescopectl screenshot /tmp/a.png 4` arrives as a single
   path field and the type silently stays at the default.

Both together mean the obvious invocation can never work. The working form
quotes path and type as one argument, and uses type **4** (`screen_buffer`,
"the buffer displayed on-screen - 1:1", which truncates nothing and reuses the
same `vulkan_composite()` call the present path uses):

```
gamescopectl screenshot "/tmp/shot.png 4"
```

This is the general rule for every multi-argument console command, and the
per-command help text says so -- it is easy to miss for `screenshot`, whose
help does not repeat the warning.

### Safe harness for reproducing this class of bug

Never point `grim` at a real output. Run gamescope's **Wayland backend**
inside your own headless sway instead, which exercises the identical backend
path with nothing on the user's screen:

```
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 XDG_RUNTIME_DIR=/tmp/gsr-ovl \
  sway -c <cfg with: output HEADLESS-1 resolution 1920x1080, borders/gaps 0>
# then, against that display:
WAYLAND_DISPLAY=wayland-1 gamescope --backend wayland ... -- vkgears
WAYLAND_DISPLAY=wayland-1 grim -o HEADLESS-1 out.png
```

`grim` there can only ever see the headless output: the private
`XDG_RUNTIME_DIR` contains only that sway and its gamescope, so the user's
compositor is not reachable from it. Note the runtime dir must be a SHORT
path -- a unix socket path is capped at 108 bytes, and the usual scratchpad
directory overflows it.
