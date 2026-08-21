# gamescope-ritz Overlay — Master Design Spec

*Written 2026-08-21. Synthesizes nine feasibility-scout reports plus the settled
decisions in [`DECISIONS.md`](./DECISIONS.md) into one implementer-facing spec.
Where a decision overrides a scout's recommendation, that scout's dissent is noted
inline in one line — it is not re-litigated.*

*Every `path:symbol` claim in the Architecture and per-feature sections was
grep-verified against this repo's `master` (`fcc1341`) while writing this document;
a handful of scout-reported line numbers had drifted by the time of writing and are
corrected here (noted where it happened).*

---

## Overview

gamescope-ritz is gaining a toggleable, in-process ImGui settings overlay: press a
hotkey, the game freezes taking input, and a dark-themed panel fades in on top of the
composited frame offering five things — live ReShade-style color/sharpening effects,
an FPS counter, gamescope's own upscale filter/scaler/sharpness controls, the hosted
game's system audio volume, and a global/profile/per-game config system that
remembers all of the above per title. Close the panel (same hotkey) and the game gets
its input back. Everything renders through gamescope's existing Vulkan compositor —
there is no second process, no second window, and no protocol round-trip for the
common case; the overlay is a new consumer of Vulkan infrastructure gamescope's
`CVulkanDevice` already provisions but has never used (a general graphics+compute
queue, dynamic rendering). The genuinely new engineering is not drawing the panel —
it's capturing all keyboard/mouse input into it and handing it back cleanly, because
no existing gamescope mechanism does anything like that today.

---

## Architecture

### Where the overlay renders

The overlay is one more `Layer_t` in the same `FrameInfo_t` stack every backend
already composites, following the existing precedent for a hand-built layer with no
backing client window: the "HACK HACK HACK" blank-texture layer built inline in
`paint_all()` when no Steam overlay client is present but a placeholder is still
needed to avoid a present stutter (`src/steamcompmgr.cpp:2792`-`~2820`, fills
`scale`/`offset`/`opacity`/`zpos`/`eAlphaBlendingMode`/`colorspace`/`tex` and
`frameInfo.layers.push()`s it). `k_nMaxLayers` is 6; the layer-order convention
already reserves a slot pattern ending in "Primary Overlay (Steam Overlay)" /
"Cursor" (`src/rendervulkan.hpp:26`-`38`) — the settings overlay sits at or above
that zpos, and `nReservedLayers` budgeting (`src/steamcompmgr.cpp:2718`-`2725`)
already exists to guarantee overlay/cursor layers aren't starved by decoration
windows; a new reserved slot follows the same pattern.

This means the overlay is **backend-agnostic by construction**: DRM, SDL, Wayland,
and Headless all funnel through the same `paint_all()` (`src/steamcompmgr.cpp:2564`,
called at `:9488`) → `vulkan_composite()` (`src/rendervulkan.cpp:4030`) →
`IBackendConnector::Present()` path — every backend's `UsesVulkanSwapchain()` flag
only changes how the *finished, already-composited* frame reaches the screen, not
whether the layer stack got assembled. **OpenVR is explicitly out of scope** (see
Decisions) so its divergent "own `COpenVRConnector`" design question, flagged by the
ImGui scout as a real fork-in-the-road, does not need resolving for this roadmap.

### Which thread draws it

**The steamcompmgr thread, inside (or immediately before) `paint_all()`.** Per
`superdoc/architecture/overview.md`'s verified threading model, that thread already
owns the vblank-paced repaint cadence (`GetVBlankTimer().ProcessVBlank()`, `:8882`)
and is the only thread that builds `FrameInfo_t` and calls `vulkan_composite()`.
Calling `ImGui::NewFrame()`/`Render()` from the main thread (`wlserver_run()`,
`src/wlserver.cpp:2254`) would require a second cross-thread handoff of the finished
ImGui texture into a frame already being assembled on steamcompmgr — recreating the
exact "two threads cooperating on one shared frame" hazard the architecture doc
identifies as the reason the render loop already lives on one thread, for no
benefit. Concrete call site: a new
`if ( g_bSettingsOverlayVisible ) { ImGui::NewFrame(); ...; ImGui::Render(); }`
block belongs right before the layer-stack assembly in `paint_all()`, pushing the
result as a `Layer_t` the same way the HACK-HACK-HACK layer does.

### Vulkan plumbing

`CVulkanDevice` already exposes three of the five prerequisites, unused by anything
today (confirmed: grepping for `generalQueue()`/`generalCommandPool()` outside
`rendervulkan.{cpp,hpp}` returns nothing):

- **A graphics+compute queue** — `generalQueue()` / `generalQueueFamily()` /
  `generalCommandPool()` (`src/rendervulkan.hpp:840`, `:844`, `:842`), selected
  separately from the compute-only `m_queue` whenever the driver offers distinct
  families (`src/rendervulkan.cpp:362`-`396`). ImGui's own Vulkan backend
  (`imgui_impl_vulkan.h`) issues `vkCmdDraw*` graphics-pipeline commands and its
  `ImGui_ImplVulkanH_SelectQueueFamilyIndex()` helper requires
  `VK_QUEUE_GRAPHICS_BIT` — it **cannot** run on gamescope's compute-only queue.
  `generalQueue()` is the only viable choice, and it's already there.
- **Dynamic rendering feature bit** — `VkPhysicalDeviceVulkan13Features::dynamicRendering
  = VK_TRUE` (`src/rendervulkan.cpp:621`), confirmed. ImGui's Vulkan backend can
  render via dynamic rendering with no `VkRenderPass`/framebuffer object at all.
  **Caveat, unresolved, first item in Risks:** `grep -n "DYNAMIC_RENDERING"
  src/rendervulkan.cpp` matches only the feature-struct field name — gamescope
  requests the Vulkan 1.3 *feature bit* but never adds
  `VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME` to `enabledExtensions` anywhere. ImGui's
  own backend header documents needing the **extension string**, "even for Vulkan
  1.3." Whether a 1.3-core feature bit alone satisfies ImGui's own init-time check is
  unverified — this is the first thing to check empirically, before writing any
  overlay-render code, not something to discover mid-implementation.
- **Texture usage flags** — `CVulkanTexture::createFlags` (`src/rendervulkan.hpp:131`-
  `157`) already has `bColorAttachment`, mapping to
  `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` (`src/rendervulkan.cpp:2048`), plus
  `bSampled`. An ImGui offscreen render target is exactly `{bColorAttachment,
  bSampled}` — no new usage flag needed.

**What's genuinely new:**

- A `VkDescriptorPool` for ImGui's own descriptor sets. Can be auto-created by
  setting `InitInfo.DescriptorPoolSize > 0` (a documented ImGui backend
  convenience) rather than hand-rolled — gamescope's existing descriptor pool
  (`src/rendervulkan.cpp:910`-`925`) is sized for the compute-composite pipeline's
  own layout and is not reusable as-is; give ImGui its own.
- A command-buffer/submission path against `generalQueue()`/`generalCommandPool()`.
  `CVulkanCmdBuffer`'s constructor already takes `queue`/`queueFamily` as
  parameters, so the type supports this — no call site constructs one against the
  general queue anywhere today. A thin wrapper analogous to the existing
  `commandBuffer()`/`submit()` is small new work.
- **Cross-queue synchronization** between "ImGui finished drawing on the general
  queue" and "compute composite wants to sample that texture." `CTimeline`
  (`src/Timeline.h:25`, `ToVkSemaphore()` at `:48`) is DRM-syncobj-backed and
  convertible to a Vulkan timeline semaphore — the natural mechanism to reuse. No
  code in this repo demonstrates a same-frame compute-queue-waits-on-general-queue
  pattern today; this would be the first. See Risks.

Vendoring follows the repo's own established pattern: `subprojects/stb.wrap` +
`subprojects/packagefiles/stb/meson.build` is a live exact-match precedent for a
library with no native Meson support (`glm` is the same pattern, both consumed via
`subproject('stb')`/`subproject('glm')` at `meson.build:55`/`:53`). A
`subprojects/imgui.wrap` pinned to a tag, with a hand-written
`packagefiles/imgui/meson.build` exposing a `static_library` for `imgui.cpp`,
`imgui_draw.cpp`, `imgui_widgets.cpp`, `imgui_tables.cpp`, and
`backends/imgui_impl_vulkan.cpp`, follows suit. **Use stock upstream ImGui, not the
`docking` branch** — multi-viewport needs the platform backend to create real OS
windows, which has no meaning for gamescope (see UI structure); docking's
tile/snap-into-tabs behavior doesn't match the design guide's free-floating,
draggable-by-titlebar window model either, which plain `ImGui::Begin` +
`SetNextWindowPos` already provides natively.

### Input capture and release

**This is new territory — no existing gamescope mechanism does anything like it.**
Two separate problems:

1. **The toggle itself** is cheap and has a direct precedent: `wlserver_handle_key`
   (`src/wlserver.cpp:300`) already special-cases global hotkeys (volume keys,
   VT-switch) *before* the normal seat-forward path, via a `forbidden_key` check
   (`:319`-`:323`) that redirects the event and explicitly returns so it never
   reaches the focused client. A settings-overlay toggle is the same shape: check a
   keysym combo early in this function, flip an atomic
   (`g_bSettingsOverlayVisible`), and return. Pair it with a `ConVar<bool>`/
   `ConCommand` (`src/convar.h:127`) so `gamescopectl`/Lua can also drive it —
   `cc_focus_info` (`src/steamcompmgr.cpp:932`) is the established template for a
   command callback that only flips an atomic, with the state change happening on
   the thread that owns the data.
2. **Full input capture for the duration the overlay is up is genuinely unsolved.**
   Every key/pointer listener (`wlserver_handle_key` and the pointer
   motion/button/axis handlers, `src/wlserver.cpp:358`-`396`) currently always ends
   by forwarding to `wlr_seat_keyboard_notify_key`/the pointer equivalents — there
   is no "consume into an in-process UI instead of the seat" branch anywhere. The
   plan: gate those forward calls on `g_bSettingsOverlayVisible`; when set, feed raw
   key/mouse state into `ImGuiIO` directly (ImGui's own Wayland/GLFW backend doesn't
   apply — there's no `wl_surface` for the overlay to own; a hand-written `ImGuiIO`
   feed from wlroots' raw event structs is the correct shape, structurally similar
   to how the dead-code `CLibInputHandler`, `src/LibInputHandler.h:11`, already
   translates raw libinput events into `wlserver_key`/`wlserver_mousemotion`
   calls — same translation target class, different destination). Restoring normal
   forwarding on toggle-off is symmetric. **No crash/edge-case history exists here
   to lean on** — budget real testing time, especially for what happens to a game's
   held-key state when capture cuts it off mid-press (a key-up the game never
   receives). See Risks and Build order M2.

### How the overlay's texture reaches the composite

Straightforward once the above exists: ImGui renders into an offscreen
`CVulkanTexture` (render-target + sampled, per above) on the general queue; a
`CTimeline`-backed semaphore signals when that draw is done; `paint_all()` pushes a
`Layer_t` referencing that texture; `vulkan_composite()`'s compute-queue submission
waits on the same timeline point before sampling it — no different in kind from any
other layer's texture, just a new producer of one.

---

## Per-feature sections

### 1. ImGui overlay (shell)

**What it does:** a toggleable panel that fades in/out, takes all keyboard/mouse
input while open, and hosts the other four features' UI plus config management.

**Hooks:** `paint_all()` (new layer push, see Architecture); `wlserver_handle_key`
(toggle + capture, see Architecture); a new `ConCommand`
(`cc_toggle_settings_overlay`, following `cc_focus_info`'s template).

**Existing vs. new:** the render-target texture creation, general-queue submission,
`Layer_t` injection, and toggle hotkey are all small extensions of existing
mechanisms. Full input capture/release (§ above) is wholly new. Fade in/out is also
wholly new — ImGui itself has no built-in tweening (immediate-mode UI); this needs a
hand-driven alpha lerp across N frames after toggle, gated on
`g_bSettingsOverlayVisible`'s edge transition, applied as the layer's `opacity`.

**Settings:**

| Setting | Type | Range | Default | Notes |
|---|---|---|---|---|
| `overlay.hotkey` | keysym combo | — | `Ctrl+Shift+O` (decision — deliberately avoids colliding with the Steam Deck's existing `Super+U/I/O` filter-cycling bindings, which the design guide's `SHIFT+TAB` placeholder never considered) | v1: hardcoded; user-configurability is a stretch goal, not required for v1 |
| `overlay.fade_ms` | int (ms) | 0–1000 | TBD | design guide specifies zero motion timing; pick a value, don't invent false precision — flag as a design decision, not an engineering one |

**Risks:** input capture ballooning past estimate (top risk, see Risks section);
the `VK_KHR_dynamic_rendering` extension-string gap (see Architecture); cross-queue
sync correctness (see Architecture).

### 2. ReShade effects — Vibrancy, Sharpness, and (later) Adaptive Brightness

**What it does:** live-adjustable color/sharpening effects running on the base
(game) layer, before upscaling and before gamescope's HDR tonemap. **Ship order
(decision): Vibrancy and Sharpness first; Adaptive Brightness is deferred to a
later, separate milestone (M9), not on the critical path.** The combined-`.fx`
design below is deliberately built so a third effect (Adaptive Brightness, when its
milestone starts) slots in as one more gated pass without restructuring the file
or the manager integration — see "one combined `.fx` file" below.

**Hooks:** `vulkan_composite()` runs ReShade first (`src/rendervulkan.cpp:4037`-
`4059`), only on `frameInfo->layers.get(0)`, replacing its texture in place — before
FSR/NIS scaling and before compositing/HDR color management. Per-frame *parameter*
updates are cheap and already work today, but **only for uniforms tagged with a
`source` annotation** — a gamescope-specific extension, not standard ReShade
authoring. Such a uniform lives in a mutex-guarded map
(`g_runtimeUniforms`/`g_runtimeUniformsMutex`, `src/reshade_effect_manager.cpp:30`-
`31`), read every frame by `RuntimeUniform::update()` (`:541`) and written via
`reshade_effect_manager_set_uniform_variable(key, value)` (`:1938`) — a plain
`memcpy`, no recompile. A uniform **without** a `source` annotation falls through to
`DataUniform` (`:196`), which just re-copies the shader's own FX-file default every
frame forever — it is inert to any UI control. **Every parameter this feature
exposes must be authored with a `source` annotation**, or it silently does nothing.

**What forces a recompile (avoid triggering this from casual UI interaction):**
switching *which* technique is active, or any change to the base layer's
width/height/format/colorspace (all part of `ReshadeEffectKey`), forces a full
synchronous FX preprocess+parse+SPIRV-codegen+pipeline build **inline on the
steamcompmgr render thread**, inside `ReshadeEffectManager::pipeline()`
(`:1921`-`1929`, confirmed: `m_lastKey` cache check, calls `pipeline->init()` on a
mismatch). This visibly stutters the vblank loop. A window resize or the game
changing its internal render resolution silently triggers this too, since those are
part of the cache key.

**Only one pipeline is cached at a time** (`m_lastKey`/`m_lastPipeline`,
`src/reshade_effect_manager.hpp:101`-`102`) — multi-effect chaining is an open
upstream feature request (`ValveSoftware/gamescope#962`), not something this fork
has. **Decision, with an explicit reason worth preserving so nobody "simplifies"
it back into separate effects later:** ship every effect as **one combined `.fx`
file, one always-loaded technique, sequential passes**, each independently
toggleable via its own `source`-tagged on/off uniform (0 = no-op), rather than as
separate techniques a user "switches between." The reason is not stylistic — it's
the recompile cost documented above. `ReshadeEffectManager::pipeline()` treats a
technique switch exactly like an effect switch: any change to the active
`ReshadeEffectKey` (which includes `techniqueIdx`) forces the full synchronous FX
parse + SPIR-V codegen + Vulkan pipeline build inline on the steamcompmgr render
thread. If Vibrancy and Sharpness were two techniques in the same file (or two
separate files), toggling one on/off from the overlay would hitch the vblank loop
on every single toggle — the exact opposite of what "live-adjustable" is supposed
to mean. Gating each effect behind its own uniform inside one always-loaded
technique means toggling it costs a `memcpy` into `g_runtimeUniforms`
(`RuntimeUniform::update`, `:541`), not a recompile. **Build the file's pass
ordering and uniform namespacing so a third gated pass (Adaptive Brightness, M9)
can be appended without touching the existing Vibrancy/Sharpness passes or their
uniform names** — this is why Adaptive Brightness's parameters are still specified
in full below and reserved in the config schema now, even though M9 doesn't start
until after Vibrancy/Sharpness ship.

**Both sharpness paths exist (decision, overrides scout recommendation):** the
scout recommended dropping a ReShade sharpness effect entirely in favor of exposing
the existing `g_upscaleFilterSharpness` control, since a pre-upscale ReShade sharpen
produces a materially different result and can double-apply with FSR/NIS's own
post-upscale RCAS/NIS sharpen. The decision keeps both because gamescope's own
sharpening misbehaves on some games. **The UI must label these distinctly** —
"Upscale Sharpness" (post-upscale, only affects FSR/NIS, existing control, see
Feature 4) vs. "Pre-Sharpen" (this ReShade pass, pre-upscale, works regardless of
filter — including `LINEAR`/`NEAREST`/`PIXEL`, where upscale sharpness is a no-op).

**Settings (all as `source`-tagged uniforms in the combined `.fx`; ship order per
the decision above — Vibrancy and Sharpness first, Adaptive Brightness in M9):**

| Setting | Type | Range | Default | Notes |
|---|---|---|---|---|
| `reshade.vibrancy.enabled` | bool | — | false | ships in M6 |
| `reshade.vibrancy.strength` | float | -1.0–+1.0 | 0.0 | signed, ships in M6 |
| `reshade.vibrancy.protect_skin_tones` | bool | — | true | ships in M6 |
| `reshade.pre_sharpen.enabled` | bool | — | false | ships in M6 |
| `reshade.pre_sharpen.strength` | float | TBD | TBD | unsharp-mask amount; no existing reference value in this codebase — pick during implementation, mark TBD in schema until then; ships in M6 |
| `reshade.adaptive_brightness.enabled` | bool | — | false | **deferred to M9** — reserved in schema now, not built until then |
| `reshade.adaptive_brightness.target_luminance` | float | 0.1–0.9 | 0.5 | normalized, M9 |
| `reshade.adaptive_brightness.adapt_up_speed` | float | 0.1–5.0 | 1.0 | seconds to ~63% of target, brightening, M9 |
| `reshade.adaptive_brightness.adapt_down_speed` | float | 0.1–5.0 | 1.0 | same, darkening, M9 |
| `reshade.adaptive_brightness.min_gain` | float | 0.5–1.0 | 0.5 | hard clamp, M9 |
| `reshade.adaptive_brightness.max_gain` | float | 1.0–2.0 | 2.0 | hard clamp, M9 |
| `reshade.adaptive_brightness.strength` | float | 0.0–1.0 | 1.0 | dry/wet mix; use this to fade the effect, not the enabled flag, so adaptation state isn't lost, M9 |

**HDR gate — deliberate v1 limitation, not an oversight (decision).** All effects
are gated to SDR content for v1: when the focused layer's colorspace isn't
`LINEAR`/`SRGB` (i.e. HDR is active — scRGB or HDR10_PQ), **the effect controls in
the Shaders panel grey out with an explanation** ("Effects are SDR-only; disable
HDR or wait for HDR-correct color math") rather than silently applying effects
whose math assumes clamped 0–1 SDR RGB. This is the same "disabled/inactive" visual
treatment the design guide already defines for other inert controls (34% opacity,
desaturated fill) — reuse it here rather than inventing a new state. The underlying
reason: naive vibrancy/brightness math produces silently wrong results (over-
saturating/clipping on scRGB highlights, treating PQ codewords as if they were
linear luminance) on HDR content, and gamescope's host-side PQ helpers
(`src/color_helpers.h:53`,`:69`) aren't available inside FX shader source at all —
correct HDR-space math is real, unscoped future work, not a bug in this v1.

**Risks:** blocking recompile on resolution/format change resets Adaptive
Brightness's persistent adaptation state (below); general-queue contention with the
game's own GPU work (ReShade submits on `generalQueue()`, not the dedicated
async-compute queue the rest of composite prefers); possible unsynchronized
cross-thread writes to `g_reshadeEffectPath`/`g_reshade_effect` (unlike the
mutex-guarded uniform map) if the overlay switches effects more often than today's
CLI/protocol-only usage pattern.

**Adaptive Brightness specifically (M9, deferred) — build the prototype first, and
only once M9 actually starts.** The plumbing for a persistent frame-to-frame
render target *appears* to exist (a render target whose pass doesn't set
`clear_render_targets` uses `VK_ATTACHMENT_LOAD_OP_LOAD`, `:1843`) — but every
storage/render-target texture gets `discardImage()`'d at the top of every
`execute()` call (`:1766`-`1776`), which sets a layout-transition hint, not a
guaranteed content-preserving one per the Vulkan spec. **No shipped effect in this
repo exercises this path.** Do not write the full Adaptive Brightness pipeline
before confirming this empirically — see Build order M9a. Because Adaptive
Brightness is off the critical path, this uncertainty does not block Vibrancy or
Sharpness shipping in M6.

### 3. FPS display

**What it does:** an always-drawn (independent of the settings panel's open/closed
state) frame-rate readout with configurable font size, backdrop, and blend mode.

**Hooks:** a second, independently-gated ImGui draw call inside the same
`paint_all()`-adjacent block as the settings panel, but on its **own** visibility
flag — it must render every composited frame regardless of whether the settings
panel is open, so it cannot share the panel's toggle gate.

**Data source — reuse mangoapp's math, not mangoapp's rendering (decision).**
mangoapp (`src/mangoapp.cpp`) is a one-way SysV message-queue *producer*; the actual
HUD rendering is done by a fully separate, external MangoHud process gamescope has
zero rendering control over (no font size, no backdrop, no blend mode reachable from
this codebase) — reusing it would mean abandoning the font/backdrop/blend
requirements outright. The **headline number is the game's own frame rate**
(decision): `frametime_ns = lastCommit->present_time - w->last_commit_present_time`
(`src/steamcompmgr.cpp:7466`, inside `handle_presented_for_window`, steamcompmgr
thread) — the same computation mangoapp's `app_frametime_ns` field mirrors. Read
this value directly in-process (or have that call site also stash it somewhere the
ImGui pass reads); do not round-trip through the one-shot
`request_app_performance_stats` Wayland protocol
(`protocol/gamescope-control.xml:135`-`143`), which exists for genuinely external
clients, not in-process code.

**Settings:**

| Field | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `fps_display.enabled` | bool | – | false | independent of settings-panel visibility |
| `fps_display.font_size` | float (px) | 10–48 | 18 | |
| `fps_display.backdrop_enabled` | bool | – | true | |
| `fps_display.backdrop_opacity` | float | 0.0–1.0 | 0.5 | |
| `fps_display.backdrop_rounding` | float (px) | 0–16 | 4 | |
| `fps_display.backdrop_padding` | float (px) | 0–24 | 6 | |
| `fps_display.blend_mode` | enum | `alpha` \| `additive` | `alpha` | additive + backdrop rect interact oddly (the backdrop itself would glow) — UI should auto-disable/warn, not silently combine them |
| `fps_display.text_opacity` | float | 0.0–1.0 | 1.0 | independent of backdrop |

**Risks:** none load-bearing beyond the general ImGui-render-pipeline risks already
covered — this is the cheapest of the five features once the overlay shell exists,
since it needs no interactive input at all to draw (only to configure).

### 4. Live gamescope options — filter, scaler, sharpness

**What it does:** exposes gamescope's own upscale filter (linear/nearest/FSR/
NIS/pixel), scaler (auto/integer/fit/fill/stretch), and sharpness as live overlay
controls.

**This needs almost no new plumbing — it is already a fully live, runtime-mutable
subsystem, and the mechanism is the one the Steam client itself uses.** Every value
has a `g_wanted*` variable external input writes and a plain `g_*` variable
`paint_all()` reads each frame, reconciled once per frame on the steamcompmgr thread
(`g_upscaleScaler = g_wantedUpscaleScaler; g_upscaleFilter = g_wantedUpscaleFilter;`,
`src/steamcompmgr.cpp:9330`-`9331`; skipped in favor of a hardcoded FIT/LINEAR
override while the Steam window itself is focused, `:9320`-`9325` — surface this
override in the UI rather than showing a lying "Filter: FSR" while it's active).
Live writers already exist as X11 root-window properties, polled by the
steamcompmgr thread's own `PropertyNotify` handler:
`GAMESCOPE_NEW_SCALING_FILTER`/`GAMESCOPE_NEW_SCALING_SCALER` (atoms interned at
`src/steamcompmgr.cpp:8275`-`8276`) and `GAMESCOPE_FSR_SHARPNESS`/
`GAMESCOPE_SHARPNESS` (atoms at `:8242`-`8243`). **The overlay's job is to write
these same properties** (`XChangeProperty`), not invent a fourth code path — this is
the identical mechanism the Steam client already drives these options with in
production (external corroboration: `Super+I`/`Super+O` sharpness hotkeys on Steam
Deck use the same live state).

**Thread-safety note:** `g_upscaleFilter`/`g_wantedUpscaleFilter`/
`g_upscaleFilterSharpness` are plain globals, not atomics — safe today only because
every writer runs on the steamcompmgr thread. Since the overlay itself renders on
that thread (Architecture), writing these directly (or through the X11-property
path) from the overlay's own UI callback is safe with no new synchronization,
**provided the ImGui callback that handles the slider drag runs on steamcompmgr**,
which it does by construction here.

**Sharpness is one auto-corrected slider, not a raw 0–20 passthrough (decision).**
The same `g_upscaleFilterSharpness` int is remapped per filter and the two
directions **invert**: FSR (RCAS) maps `raw/10.0` → `0.0–2.0`, higher raw = sharper
(`src/rendervulkan.cpp:4109`); NIS maps `(20-raw)/20.0` → `1.0–0.0`, higher raw =
**less** sharp (`:4123`). The UI slider must present a single "sharper →" direction
regardless of which filter is active, remapping to the correct raw value on write —
upstream Valve looked at unifying this exact inconsistency and closed it as not
planned (`ValveSoftware/gamescope#515`), so this is a UI-layer fix owned entirely by
this project, not something a future upstream merge resolves underneath it.
`PIXEL` self-downgrades to `NEAREST` on an exact-integer scale ratio
(`src/steamcompmgr.cpp:2248`-`2252`) — UI copy should say "sharp when the scale
factor is a whole number," not promise a distinct look always.

**Settings:**

| Knob | Type | Default | Applies to |
|---|---|---|---|
| `gamescope.filter` | enum: `LINEAR`\|`NEAREST`\|`FSR`\|`NIS`\|`PIXEL` | `LINEAR` | all layers |
| `gamescope.scaler` | enum: `AUTO`\|`INTEGER`\|`FIT`\|`FILL`\|`STRETCH` | `AUTO` | content-fit |
| `gamescope.sharpness` | int, one auto-corrected slider | 2 (raw) | `FSR`/`NIS` only, no-op otherwise |
| `gamescope.vrr_enabled` | bool → `cv_adaptive_sync` | false | live, `src/steamcompmgr.cpp:295` |
| `gamescope.hdr_enabled` | bool → `cv_hdr_enabled` | false | live, `:461`, confirmed live via `GAMESCOPE_DISPLAY_HDR_ENABLED` property at `:6713` |
| `gamescope.tearing_enabled` | bool → `cv_tearing_enabled` | false | live, `:455`, confirmed live via `GAMESCOPE_ALLOW_TEARING` property at `:6671` (scout flagged this as unconfirmed; verified here) |

**Risks:** none beyond the sharpness-direction UI trap above and the Steam-focus
override; this is the cheapest feature in the whole roadmap to implement.

### 5. PipeWire volume

**What it does:** shows and controls the system-level PipeWire volume of the hosted
game's process (a Stream node, not a Sink/Device — the whole-output-level control is
explicitly the wrong target).

**Hooks:** none in-process today — `init_pipewire()` (`src/pipewire.cpp:671`) is
100% video, has never called `pw_registry`, and shares nothing reusable for audio
beyond its thread/loop *pattern*.

**v1 mechanism (decision): shell out to `wpctl`.** `wpctl set-volume ID VOL[%]
--pid` / `wpctl set-mute ID 1|0|toggle --pid` already implements exactly the
"given a PID, find and control its node(s)" primitive gamescope would otherwise need
to build against the raw PipeWire registry/proxy API — that's the crux reason v1
shells out rather than embedding a second `pw_core` connection.

**The hard part is finding the PID(s), not calling `wpctl`.** `wpctl --pid` matches
one exact PID's nodes; it does not walk a process tree. Gamescope already has the
tree-walk primitive: `Process::GetChildPids(pid_t nPid)` (`src/Utils/Process.cpp:65`)
walks `/proc/*/stat` parent links; start from the PID the reaper wraps
(`GamescopeReaperProcess`, `src/Apps/gamescopereaper.cpp`), recurse to the full
descendant set, and try each against `wpctl status`/`pw-dump` output looking for a
`Stream/Output/Audio` node whose `application.process.id` is in that set. **This is
not guaranteed to work on the platform's most common real launch path** — Steam
Linux Runtime's `pressure-vessel`/`bwrap` sandboxing can put the real Proton process
in a PID namespace gamescope's own `/proc` walk never sees, silently breaking PID
matching. This could not be verified empirically by any scout (no game was launched
during scouting) and must be checked against a real sandboxed Proton launch early in
implementation, not assumed benign.

**The control hides itself when the node can't be identified (decision).** No
picker, no "apply to all" — when zero matches are found, show "audio: not detected"
and keep polling (registry events if the direct API is ever built later, a slow
`wpctl status` poll for the shell-out v1); don't error or block. **No gamescope-side
volume persistence** — WirePlumber's own `node.stream.restore-props` setting already
remembers per-application stream volume; adding a second, gamescope-side persisted
value risks the two silently disagreeing about which wrote last. The overlay's
slider is a pure live control surface with no config-file backing for its value
(the panel-open/mute-UI-state may still be a UI preference, but the volume number
itself is not written to gamescope-ritz's config).

**Curve:** `channelVolumes` is raw linear amplitude; UI convention in this ecosystem
(matching `pavucontrol`/`wpctl`) is a cubic display curve —
`linear = (display_fraction)^3`. Present 0–100% (optionally to 150% "boost").

**Settings:**

| Field | Type | Range | Default | Notes |
|---|---|---|---|---|
| (runtime only, not persisted) volume | float, cubic-mapped | 0–100% (150% optional boost) | — | single fader, all channels set equal — not a balance control |
| (runtime only) mute | bool | – | – | → `SPA_PROP_mute` / `wpctl set-mute --pid` |

**Risks:** node-identification failure on `bwrap`-sandboxed Proton launches (top
risk for this feature, unverified — flag as an accepted, user-visible "not
detected" fallback, not a blocker); `wpctl` is an unguarded **runtime** dependency
(the build's only PipeWire check is build-time, against `libpipewire-0.3` — a
missing `wpctl` binary is a new silent-failure mode this codebase doesn't otherwise
have in the audio path); subprocess-per-slider-tick would be wasteful if not
debounced.

### 6. Config system — global / profile / per-game layering

**What it does:** a JSON config directory at `~/.config/gamescope-ritz/` with three
layers — global defaults, named profiles, and per-game overrides — resolved once at
startup (and once on profile-apply) into the in-memory struct the other four
features read.

**App id resolution order (decision, corrects the original brief's "existing env var
named `AppId`" — no such literal env var exists anywhere, confirmed by grep and by
external documentary evidence):**

1. `GS_RITZ_APPID` — new, gamescope-ritz-specific, always wins when set.
2. `STEAM_COMPAT_APP_ID` — Proton titles; the more reliable of Steam's two vars per
   documentary evidence (steamtinkerlaunch's own `initAID()` treats it as primary).
3. `SteamAppId`, **but only if it parses as a nonzero integer** — native Linux
   titles; a literal `"0"` must be treated as absent, matching steamtinkerlaunch's
   own guard.
4. `STEAM_COMPAT_DATA_PATH` basename, if numeric — covers the Proton-but-
   `SteamAppId=0` case.
5. None found → **global config only.** No `games/<AppId>.json` is looked up, read,
   or created. The per-game UI tab is shown in a disabled/informational state
   ("No game identity detected for this session"); the "Override Global Config"
   control is disabled, not hidden, since there's no file path to write it to.

All four env vars are read **once, at the very top of `main()`**
(`src/main.cpp:723`), before `wlserver_init()`, backend selection, or the
steamcompmgr thread spawn (`:1101`) — this is as early as architecturally possible
and is fundamentally different from (and much earlier than) the existing per-window
`get_appid_from_pid()` scrape (`src/steamcompmgr.cpp:5285`, called from
`wlserver.cpp:1951` and `steamcompmgr.cpp:5465`, both well after startup, both
per-window not per-process). **A late-arriving Steam-discovered per-window app id
does not trigger a config reload in v1** — `GS_RITZ_APPID`/Steam-env-var resolution
at startup is the only identity source for config purposes.

**Relationship to `ConVar<T>` — hybrid, not replace (decision-compatible synthesis
of the config-system scout's recommendation):** `ConVar<T>` (`src/convar.h:127`) has
no persistence and no thread-safety of its own (`m_Value` is a plain field). Where a
setting already maps to a real `ConVar` (VRR, HDR, tearing), config load/profile-
apply calls `cv.SetValue(json_value)` — cheap, matches how
`gamescope_private.execute` already does it. Where a setting is a plain global
(filter/scaler/sharpness), config writes through the **existing X11-property
mechanism** (Feature 4), not by reaching into `g_wanted*` directly. Everything with
no existing backing variable at all (ReShade parameter values, FPS-display styling)
is owned entirely by the config system's own struct, read directly by its consumer.

**"Override Global Config" takes a full snapshot (decision — overrides the config
scout's recommendation of per-key fallback).** The scout argued per-key fallback
better matches "let me tweak a couple things for this game" UX; the decision is
explicit that the per-game file captures **everything** at the moment it's turned
on, and later global changes don't reach that game. Concretely: flipping the
checkbox on writes a `games/<AppId>.json` containing the game's **fully resolved
effective settings at that instant** (global, or global-then-profile if one was
applied) — not a sparse delta. Default off; if a user never enables it, no
`games/<AppId>.json` is ever created. Resolution order becomes strictly two-level,
not three: `games/<AppId>.json` (if `override_global: true`) **or** `global.json` —
never both merged key-by-key.

**Applying a profile copies values once (decision — overrides the config scout's
"live reference" recommendation).** "Apply profile FPS" copies the profile's values
into the per-game file at that moment; the per-game file has no memory of which
profile it came from, and editing the profile later does not retroactively affect
games that already applied it. This simplifies resolution back to two levels (no
profile-reference hop) and removes the config scout's flagged "stale profile value"
risk entirely, at the cost of profiles not being a live, ongoing relationship.

**File lifecycle (scout recommendation, unmodified by any decision):** debounced/
batched writes (dirty flag on edit, flush after a few hundred ms of inactivity, and
always on overlay-close/app-exit) — not every slider tick, not only on an explicit
save. Atomic writes via temp-file-then-`rename()` in the same directory (`rename()`
is atomic on the same Linux filesystem). Malformed/hand-edited JSON: fail loudly for
that one layer, fall back one level (per-game parse failure → behave as if
`override_global` were off for the session; global parse failure → hardcoded
defaults), and surface a visible UI warning — never crash gamescope or silently
discard edits. Disk I/O must never happen inline on the steamcompmgr thread (a
`fsync()`/`rename()` stall shows up as a dropped/late frame) — queue the write and
flush from the otherwise-idle main thread or a small one-shot worker thread.
Profile names are sanitized to `[A-Za-z0-9 _-]`, non-empty, not `.`/`..`, before
becoming a path component — user input directly becomes a filename.
`profiles/<ProfileName>.json` (the `.json` extension, correcting an inconsistency
the config scout flagged against the original brief's extensionless
`profiles/<ProfileName>`).

**Risks:** everything downstream of the app-id order above assumes the documentary
evidence (steamtinkerlaunch's/ScopeBuddy's source, both read directly, not just
their docs) actually matches Steam's real behavior when gamescope itself sits in
`gamescope -- %command%` — no scout could launch a real game during this planning
pass to confirm directly; the one-line live-verification command in
`appid-detection.md` §2 should be run the first time any implementer has a game
running, before trusting this order in production.

---

## Config schema

`~/.config/gamescope-ritz/global.json`:

```jsonc
{
  "schema_version": 1,
  "gamescope": {
    "filter": "LINEAR",          // enum: LINEAR | NEAREST | FSR | NIS | PIXEL — src/main.hpp:35-42
    "scaler": "AUTO",            // enum: AUTO | INTEGER | FIT | FILL | STRETCH — src/main.hpp:53-59
    "sharpness": 2,               // int 0..20, raw g_upscaleFilterSharpness value (UI remaps per filter, see Feature 4)
    "vrr_enabled": false,         // bool -> cv_adaptive_sync
    "hdr_enabled": false,         // bool -> cv_hdr_enabled
    "tearing_enabled": false      // bool -> cv_tearing_enabled
  },
  "fps_display": {
    "enabled": false,
    "font_size": 18.0,
    "backdrop_enabled": true,
    "backdrop_opacity": 0.5,
    "backdrop_rounding": 4.0,
    "backdrop_padding": 6.0,
    "blend_mode": "alpha",        // enum: alpha | additive
    "text_opacity": 1.0
  },
  "reshade": {
    "vibrancy": {                       // ships in M6
      "enabled": false,
      "strength": 0.0,                 // float -1.0..+1.0
      "protect_skin_tones": true
    },
    "pre_sharpen": {                    // ships in M6
      "enabled": false,
      "strength": null                 // TBD: range/default not yet set, see Feature 2
    },
    "adaptive_brightness": {            // deferred to M9 (decision) - reserved here now so the
                                          // schema version doesn't need a breaking bump when it lands
      "enabled": false,
      "target_luminance": 0.5,        // float 0.1-0.9
      "adapt_up_speed": 1.0,          // float 0.1-5.0, seconds to ~63% of target
      "adapt_down_speed": 1.0,        // float 0.1-5.0
      "min_gain": 0.5,                // float 0.5-1.0
      "max_gain": 2.0,                // float 1.0-2.0
      "strength": 1.0                 // float 0.0-1.0, dry/wet mix
    }
  },
  "overlay": {
    "fade_ms": null                     // TBD: motion timing not specified by the design guide, see Feature 1
  }
  // No "audio" block: volume is a live control surface only, never persisted here
  // (see Feature 5). No "general"/lossless_dll_path block: was LSFG-VK-only, and
  // LSFG-VK is cut from this roadmap (see Deferred).
}
```

`~/.config/gamescope-ritz/profiles/<ProfileName>.json`:

```jsonc
{
  "schema_version": 1,
  "name": "FPS",                // redundant with filename by design: keeps a copied/renamed
                                  // file self-describing and gives the UI a display name
                                  // independent of filesystem-safe naming
  "gamescope": { /* same shape as global.json */ },
  "fps_display": { /* ... */ },
  "reshade": { /* ... */ }
  // No "audio", no "overlay.fade_ms" (process-level UI preference, not a per-game/profile concern)
}
```

`~/.config/gamescope-ritz/games/<AppId>.json` (only ever created once "Override
Global Config" is turned on for that game — see Feature 6):

```jsonc
{
  "schema_version": 1,
  "override_global": true,       // mirrors the UI checkbox; while true, this file is fully
                                  // authoritative for this game and global.json is not consulted
  "gamescope": { /* full snapshot at the moment override was enabled, or last edited */ },
  "fps_display": { /* ... */ },
  "reshade": { /* ... */ }
}
```

### Schema migrations

`schema_version` is a flat integer, bumped on any breaking rename/removal/type
change (additive-only changes don't strictly need a bump, but bumping anyway is
free and keeps history legible). On load, if `schema_version` is older than
current, run an ordered chain of migration functions
(`migrate_1_to_2(json&)`, ...), each doing one reshape step, then re-save. If
`schema_version` is *newer* than current (a config from a future build opened by an
older one), **fail loudly rather than guessing** — do not silently drop unknown
fields, since that risks quietly discarding settings on a downgrade. A file missing
`schema_version` entirely is treated as version 0 and run through the full
migration chain (or rejected per the malformed-file policy above) — this specific
edge case is a judgment call left to the implementer, not resolved here.

---

## UI structure

Panel/tab layout, mapped onto the design language in `ui-design-guide.md` (dark-only,
IBM Plex Sans/Mono, flat/hairline/square-corner chrome, cyan accent, free-floating
draggable windows rather than a docked tab strip — the mockup's own layout is
multiple small floating windows plus a bottom dock, not one monolithic tabbed panel,
and that's what stock ImGui without the docking branch naturally supports).
**Dark theme only (decision, settled — the design guide's own open question 2 is
resolved): no light theme.** The handoff has no light-palette source material at
all — inventing one would mean designing colors with nothing to check them against —
so the overlay ships with the single dark palette the handoff actually specifies,
with no runtime theme toggle.

- **Bottom dock** (per design guide's "Dock" component, centered, 38px from bottom
  edge, 54×54px square icon buttons, 5px gaps) — one button per floating window
  below, toggling its visibility. Persisted per-game (position/open-state), same as
  any other windowed-UI-position preference.
- **Gamescope panel** — filter segmented control, scaler segmented control,
  sharpness slider (single, auto-corrected, re-labels per filter), VRR/HDR/tearing
  toggles. Cheapest panel to build; build it first once input capture exists (see
  Build order M3).
- **Shaders panel** — Vibrancy group (strength slider, protect-skin-tones
  checkbox) and Pre-Sharpen group (enabled toggle, strength slider) ship in M6;
  an Adaptive Brightness group (enabled toggle, target luminance slider, up/down
  adaptation-speed sliders, min/max gain sliders, strength slider) is appended in
  M9 without restructuring the panel, per Feature 2's ship-order decision. Each
  group gets the design guide's "active group = 2px accent left-edge stripe"
  affordance. The whole panel greys out with an explanation when HDR is active
  (Feature 2's SDR-only gate — a deliberate v1 limitation, not a bug). Two distinct
  sharpness controls live in two different panels (Gamescope panel's "Upscale
  Sharpness," Shaders panel's "Pre-Sharpen") — label both explicitly to avoid the
  ambiguity flagged in Feature 2.
- **FPS HUD panel** — enabled toggle, font-size slider, backdrop group (enabled/
  opacity/rounding/padding), blend-mode segmented control (alpha/additive), text-
  opacity slider. The HUD's own always-on readout is a *separate* draw pass from
  this config panel (Feature 3) — this panel only edits its settings.
- **Audio panel** — single volume fader (cubic curve, 0–100%, optional 150% boost),
  mute toggle, a small status line ("audio: 1 stream detected" / "audio: not
  detected"). Entire panel greys out / shows the "not detected" state per Feature
  5's decision, rather than presenting a picker.
- **Config/Profiles panel** — not designed in the source mockup (flagged there as
  "not yet designed"), built fresh using the flat-hairline-box language for its two
  new control types the mockup never shows: a text input (profile name — reuse the
  numeric-readout flat-box treatment: `rgba(255,255,255,.05)` fill, 1px
  `rgba(255,255,255,.08)` border, Mono value text) and a dropdown/combo (profile
  picker — build fresh from the segmented-control's flat/hairline/accent-active
  language per the design guide's own explicit gap flag; no true combo box exists in
  the mockup to copy). Contains: global-vs-per-game indicator, "Override Global
  Config" checkbox (disabled when no app id is resolved, per Feature 6), profile
  picker + "Apply" button (one-time copy, not live — Feature 6), "Save as new
  profile" text input.

**Icon list to author as SVG** (per decision: icons generated as SVG, following the
design guide's rules — geometric line-drawn glyphs, 1–1.5px stroke, no rounded
corners, 16–20px grid, monochrome, outline-first with small solid-fill accents,
explicitly **not** copied from the mockup's own placeholders):

1. Settings (gear) — dock entry point / overlay identity.
2. Display/scaling (monitor or scan-lines glyph) — Gamescope panel.
3. Shaders/color grading (half-filled circle, the mockup's reusable
   brightness/contrast metaphor) — Shaders panel.
4. Performance/FPS (bar-chart glyph, present in the mockup) — FPS HUD panel.
5. Audio (speaker + waveform, present in the mockup) — Audio panel.
6. Profiles/game-config (three horizontal lines with tick marks, present in the
   mockup, reads as a config/preset list) — Config/Profiles panel.
7. Reset/restore (circular-arrow, **not present in the mockup** — needs a fresh
   glyph in the same stroke weight).
8. Close (×) — present in the mockup, window chrome.
9. Collapse/minimize (–) — present in the mockup, window chrome.
10. Dock overflow/more (**not present in the mockup** — needs a fresh glyph, only
    needed if the dock ever exceeds its fixed button count; may be deferred if six
    panel icons fit without overflow).

---

## Build order

Foundation first: **the config system (M0) and the overlay shell (M1+M2) are what
everything else plugs into** — no feature milestone below depends on anything later
in this list. Live filter/scaler/sharpness (M3) needs almost no new plumbing and
should be the first *feature* built once input works, precisely because it validates
the whole input→ImGui→X11-property→live-effect loop cheaply before the harder
features (ReShade, audio) build on top of it. Every milestone's acceptance criterion
is phrased as a concrete `vkcube` run.

- **M0 — Config system foundation.** *Size: M.* JSON load/save (`nlohmann::json`
  vendored via the `stb`/`glm` wrap pattern), schema v1, atomic writes, migration
  scaffolding, app-id resolution order (Feature 6), global/profile/per-game file
  layout — no UI yet, driven by a temporary CLI flag or `gamescopectl` debug command
  that dumps the resolved effective config to stderr. **No dependency on the
  overlay** — buildable and testable in complete isolation.
  **Acceptance (vkcube):** hand-write a `global.json` with
  `gamescope.filter: "FSR"`, `gamescope.sharpness: 12`; launch
  `gamescope-ritz -- vkcube`; confirm (a) the debug dump shows the resolved values,
  and (b) gamescope actually applies them at startup (visually, FSR upscaling
  active — run at a non-native output size so the effect is visible, e.g.
  `-w 640 -h 480 -W 1920 -H 1080`). Then set `GS_RITZ_APPID=1` and hand-write
  `games/1.json` with `override_global: true` and a different filter; relaunch with
  the same env var and confirm the per-game value wins; unset it and confirm
  `global.json`'s value returns.

- **M1 — ImGui render shell.** *Size: M.* General-queue command buffer/pool,
  descriptor pool, offscreen render target, `Layer_t` injection into `paint_all()`,
  the `VK_KHR_dynamic_rendering` extension-string question (Architecture) resolved
  empirically, the toggle `ConVar`/hotkey wired via `wlserver_handle_key`'s
  `forbidden_key` pattern, fade in/out. **No input capture yet** — the panel is
  visible but not interactive; this milestone proves the render path in isolation
  from the harder input problem.
  **Acceptance (vkcube):** `gamescope-ritz -- vkcube`; press the toggle hotkey;
  confirm `ImGui::ShowDemoWindow()` (a placeholder, not real UI) fades in over the
  spinning cube; press again, confirm it fades out; confirm vkcube keeps rendering
  underneath throughout (proves the layer composites correctly, doesn't stall the
  vblank loop).

- **M2 — Input capture and release.** *Size: L (highest-uncertainty milestone in
  the whole roadmap).* Gate every key/pointer forward call in
  `wlserver_handle_key`/pointer handlers on `g_bSettingsOverlayVisible`; build the
  `ImGuiIO` feed from raw wlroots event structs; handle held-key-across-toggle
  release correctly.
  **Acceptance (vkcube):** with the overlay open (M1's demo window), drag the demo
  window by its titlebar and click its buttons — confirms the overlay owns the
  pointer. Add a temporary debug counter that logs forwarded key/pointer events to
  vkcube's focused window; confirm it stays at zero while the overlay is open and
  resumes immediately on close. Hold a key down, toggle the overlay open then closed
  while still holding it, release it, and confirm no stuck-key state (check via the
  same debug counter, or vkcube's own lack of crash/hang as a coarse signal).

- **M3 — Live gamescope options tab.** *Size: S.* Filter/scaler segmented controls,
  the auto-corrected sharpness slider, VRR/HDR/tearing toggles, writing through the
  existing X11-property mechanism. Depends on M1+M2 only (config wiring optional at
  this stage — can hardcode defaults and layer in M0 later).
  **Acceptance (vkcube):** `gamescope-ritz -w 640 -h 480 -W 1920 -H 1080 --backend
  sdl -- vkcube`; open the overlay, switch Filter to FSR, drag the sharpness slider
  and visually confirm the upscaled cube sharpens; switch Filter to NIS and confirm
  the slider's displayed direction still reads "sharper →" despite the underlying
  raw value now needing to move the opposite way (proves the remap, not just the
  write).

- **M4 — FPS display.** *Size: S.* The always-on readout (Feature 3), independent
  visibility flag, font/backdrop/blend draw code. Depends on M1 only (no input
  needed to draw; its *settings* UI can piggyback on M3's panel pattern once M2
  exists, but the readout itself doesn't need to wait).
  **Acceptance (vkcube):** `gamescope-ritz -- vkcube`; confirm the FPS counter is
  visible in the corner with the settings panel closed, showing a plausible number
  close to vkcube's own reported frame rate; toggle the settings panel open/closed
  and confirm the FPS readout's own visibility is unaffected either way.

- **M5 — PipeWire volume.** *Size: M, plus an unresolvable-without-a-real-launch
  risk.* `wpctl` shell-out, `Process::GetChildPids`-based PID matching, the mailbox
  thread pattern, cubic curve, "not detected" fallback UI. Depends on M2 for the
  interactive fader.
  **Acceptance (vkcube):** vkcube itself is silent, so pair it with an audio-
  producing sibling to actually exercise PID matching:
  `gamescope-ritz -- bash -c 'vkcube & speaker-test -t sine -f 440 -c 1; wait'`.
  Open the overlay's Audio panel, confirm it detects the `speaker-test` stream
  (or, if run under a Steam/Proton launch, confirms/refutes the `bwrap` PID-
  namespace risk directly — this is the concrete moment to retire that risk, see
  Risks), drag the volume slider and confirm `wpctl status`/`pw-dump` from a
  separate terminal shows the corresponding node's volume changing. Kill
  `speaker-test`, confirm the panel falls back to "not detected" without erroring.

- **M6 — Combined ReShade effect (Vibrancy + Sharpness) + Shaders panel.**
  *Size: L.* The real `gamescope-ritz.fx` — Vibrancy and Pre-Sharpen as sequential
  gated passes in one always-loaded technique, per Feature 2's decision — all
  parameters as `source`-tagged uniforms, the Shaders panel UI, SDR-only gating
  (controls grey out with an explanation when HDR is active). **Adaptive Brightness
  is explicitly not part of this milestone** (decision) — the file's pass ordering
  and uniform namespacing must leave room for it to be appended later (M9) without
  restructuring what M6 ships. Depends on M2 (interactive sliders) only.
  **Acceptance (vkcube):** `gamescope-ritz -- vkcube`; open the Shaders panel;
  toggle Vibrancy on and drag its strength slider, visually confirm the cube's
  color saturation changes; toggle Pre-Sharpen with Filter set to `LINEAR` (where
  Upscale Sharpness, M3, is a documented no-op) and confirm it still visibly
  sharpens — this is the one case that justifies keeping both sharpness paths at
  all; toggle each effect on/off repeatedly and confirm no recompile stutter on any
  toggle (only a uniform write, per Feature 2's decision) — this is the empirical
  proof the "one combined always-loaded `.fx`" design achieves what it was chosen
  for. Force HDR on (`gamescope.hdr_enabled`) and confirm the Shaders panel greys
  out with the SDR-only explanation rather than applying visibly wrong color math.

- **M7 — Full config integration.** *Size: M.* Wire M0's config system to M3/M4/M6's
  live settings so panel edits actually persist (debounced writes), the "Override
  Global Config" full-snapshot checkbox, one-time-copy profile apply, the Config/
  Profiles panel UI. Depends on M0, M2, and at least M3 existing to have something
  real to snapshot (doesn't strictly need M4/M5/M6, but is more meaningfully
  testable once they exist).
  **Acceptance (vkcube):** with `GS_RITZ_APPID=1` set, `gamescope-ritz -- vkcube`;
  change the filter in the Gamescope panel, enable "Override Global Config," confirm
  `games/1.json` is written containing a full snapshot (not just the filter key);
  create a profile, apply it, confirm the per-game file's values update to match,
  then edit the profile file directly on disk and relaunch — confirm the game's
  already-applied values do **not** change (proves one-time-copy, not live
  reference, per the decision).

- **M8 — Visual polish pass.** *Size: L (per the design guide's own risk ranking —
  nearly every control in this design deviates from stock ImGui's default
  rendering).* Custom `ImDrawList` widget draw code (rectangular slider handles,
  square toggle knobs, filled-square checkboxes), IBM Plex Sans/Mono font atlas,
  tracked-text letter-spacing helper, window chrome (accent border/glow on focus,
  status dots), dock chrome, icon SVGs from the UI structure list, flat semi-opaque
  fallback in place of true backdrop blur (dropped per decision). Depends on every
  panel above existing functionally; this milestone only changes how they look.
  **Acceptance (vkcube):** side-by-side visual comparison against
  `ui-design-guide.md`'s described component styling (color tokens, spacing,
  slider/toggle/checkbox geometry) with the overlay open over a running `vkcube`;
  no functional regression versus M1–M7's behavior.

- **M9a — Adaptive Brightness persistence spike.** *Size: S. Deferred (decision) —
  not on the critical path; nothing above depends on it, and it can be pulled
  forward and run any time after M2 if someone wants to retire the risk early, but
  is sequenced last here for clarity since Adaptive Brightness itself ships last.*
  A minimal throwaway `.fx`: one pass that increments a value in a persistent 1×1
  texture and displays it as a background tint. Not gated on the overlay at all —
  can be driven by the existing `--reshade-effect`/`--reshade-technique-idx` CLI
  flags, no ImGui needed.
  **Acceptance (vkcube):** `gamescope-ritz --reshade-effect <spike.fx> -- vkcube`;
  confirm the background tint visibly drifts/cycles across frames rather than
  resetting every frame — this is the empirical answer to whether `discardImage()`
  actually preserves render-target content across frames on the target driver
  (Architecture/Feature 2 risk). If it resets every frame, the full Adaptive
  Brightness design in Feature 2 needs rethinking before M9b starts — this
  milestone exists specifically to catch that before real engineering time is
  spent on a feature that was already deferred once.

- **M9b — Adaptive Brightness, appended to the combined effect.** *Size: M.*
  Appends the Adaptive Brightness passes to the `gamescope-ritz.fx` M6 already
  shipped (per Feature 2's decision, the file was built to allow exactly this
  without restructuring Vibrancy/Sharpness), plus its group in the Shaders panel.
  Depends on M6 (the file to append to) and M9a (persistence confirmed).
  **Acceptance (vkcube):** `gamescope-ritz -- vkcube`; open the Shaders panel,
  toggle Adaptive Brightness on, confirm no recompile stutter on toggle (same
  uniform-gated-pass proof as M6, now for the third pass) and confirm Vibrancy/
  Sharpness (M6) still work unchanged with Adaptive Brightness enabled alongside
  them, proving the passes compose rather than conflict.

---

## Risks

Ranked by how much they could cost if wrong, with the cheapest retiring experiment
noted for each:

1. **Input capture (M2) is genuinely new territory with no adaptable precedent in
   this codebase.** Every other topic in this roadmap extends something that
   already exists in some form; full keyboard/mouse capture-and-restore does not.
   Highest risk of ballooning past its size estimate — especially key-repeat,
   modifier state, and what a game sees when a key held before toggle-on is
   released after toggle-off. **Retiring experiment:** M2's own acceptance test
   (the zero-forwarded-events debug counter) is the cheapest way to find out early
   whether the design is even directionally right, before building any real UI on
   top of it.

2. **Cross-queue synchronization (ImGui on `generalQueue()`, compositing on the
   dedicated compute queue) has no same-frame precedent in this codebase to copy
   from.** `CTimeline` is the right primitive, but a fencing bug here is a textbook
   data race, not a crash that reliably reproduces. **Retiring experiment:** M1's
   acceptance criterion (vkcube keeps rendering correctly under the overlay across
   many toggle cycles) is a cheap first signal; a longer soak test (leave the
   overlay toggling on a timer for an hour against a driver with strict validation
   layers enabled) is the real retiring test before M1 is called done.

3. **Adaptive Brightness's persistent-texture assumption is unverified by any
   shipped effect in this repo.** `discardImage()`'s Vulkan-spec-implementation-
   defined behavior on an `UNDEFINED`-sourced transition could mean content does
   not actually survive frame-to-frame the way the design assumes. Lower urgency
   than the other risks here since Adaptive Brightness itself is deferred to M9
   (decision) and nothing else is blocked on it. **Retiring experiment: M9a,
   already built into the build order as its own milestone before any real
   engineering time goes into the full effect** — cheapest possible way to answer
   the question, and can be run any time after M2 if someone wants the answer
   before M9 formally starts.

4. **The `VK_KHR_dynamic_rendering` extension-string gap.** Gamescope requests the
   Vulkan 1.3 *feature bit* but never adds the extension string to
   `enabledExtensions`; ImGui's own backend docs say the string is required "even
   for Vulkan 1.3." Unverified whether this actually blocks anything on real
   drivers. **Retiring experiment:** the first few minutes of M1 — attempt ImGui
   Vulkan-backend init against gamescope's existing device creation as-is; if it
   fails, add the extension string explicitly (`src/rendervulkan.cpp`'s device
   creation, alongside the existing feature-bit chain) and retry. Cheap either way,
   just needs to happen before the rest of M1 is built on top of an unverified
   assumption.

5. **PipeWire node identification may not work at all on `bwrap`-sandboxed Proton
   launches** — the platform's most common real launch path. No scout could test
   this directly (no game was launched during scouting). **Retiring experiment:**
   M5's acceptance test, run once against a real Steam/Proton title rather than
   only the `speaker-test` stand-in — this is explicitly called out in M5's
   acceptance criteria as the moment to resolve this risk, not a separate task.

6. **ReShade's blocking, synchronous, render-thread compile on effect/technique
   switch or base-layer resize.** Combining all three effects into one technique
   (Feature 2's design) removes the *toggle-driven* recompile risk, but a window
   resize or the game changing its internal render resolution still forces one —
   this will visibly stutter the vblank loop and, for Adaptive Brightness
   specifically, silently resets its adaptation state. **Mitigation, not a full
   fix:** accept the stutter/reset for v1 (it's an existing property of
   `ReshadeEffectManager`, not something this feature introduces) and document it
   in UI copy near the Adaptive Brightness controls rather than pretending it won't
   happen.

7. **Cross-thread unsynchronized writes** to `g_reshadeEffectPath`/
   `g_reshade_effect` (unlike the mutex-guarded runtime-uniform map) and to plain
   globals like `g_wantedUpscaleFilter` if the overlay's input handling ever ends
   up running off the steamcompmgr thread contrary to the Architecture section's
   design. **Mitigation:** enforce "the overlay's UI callbacks run on steamcompmgr"
   as an invariant from M1 onward, not something to retrofit later.

---

## Deferred

- **LSFG-VK** — cut from this roadmap (decision). The research is preserved in
  [`lsfg-vk-integration.md`](./lsfg-vk-integration.md) (why hooking gamescope's own
  swapchain doesn't work on 4 of 5 backends) and
  [`lsfg-in-tree-port.md`](./lsfg-in-tree-port.md) (the GPLv3-vs-BSD-2-Clause
  licensing gate that forecloses a literal in-tree port, plus the more favorable
  "separate optional GPLv3 component talking dma-buf-fd + timeline-semaphore"
  alternative that document develops) — read either if this feature is revisited.
- **OpenVR** — out of scope (decision: flat backends only). The ImGui feasibility
  scout's Q6 finding that OpenVR needs a structurally different design (its own
  `COpenVRConnector`/focus-taking path, not the `Layer_t`-in-`FrameInfo_t` approach
  this spec builds on) is preserved in
  [`imgui-overlay-feasibility.md`](./imgui-overlay-feasibility.md) §6, should a VR
  settings overlay ever become a real, separately-scoped project.
