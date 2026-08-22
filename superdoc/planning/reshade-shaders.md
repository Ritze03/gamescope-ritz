# ReShade Shader Integration — planning notes for a live ImGui overlay

Scope: can gamescope-ritz's existing `ReshadeEffectPipeline`/`ReshadeEffectManager`
(`src/reshade_effect_manager.{hpp,cpp}`) drive three ImGui-adjustable effects —
**Adaptive Brightness**, **Vibrancy**, **Sharpness** — with live sliders? Planning only;
no code was written or changed.

## Verdict

**Per-frame parameter tweaks are cheap and already work today — but only for uniforms
the shader author tags with a `source` annotation, which is a gamescope-specific
extension, not something a stock ReShade community shader gets for free.** Such a
uniform is read out of a mutex-guarded runtime map on every frame
(`RuntimeUniform::update`, `src/reshade_effect_manager.cpp:541`), so an ImGui slider
writing into that map (via `reshade_effect_manager_set_uniform_variable`,
`:1938`) costs nothing beyond a `memcpy` — no shader recompile, no pipeline rebuild.
Uniforms *without* a `source` annotation (i.e. plain ReShade `ui_type`/`ui_min`/`ui_max`
sliders as authored for upstream ReShade) fall through to `DataUniform` (`:196`,`:607`)
and just re-copy their FX-file default/initializer every frame forever — they are
**inert** to any external control gamescope offers. This is externally corroborated:
a maintainer comment on `ValveSoftware/gamescope#962` states uniforms "will just use
their initializer values" unless driven through the `gamescope_reshade` protocol.

What is **not** cheap: switching *which* effect/technique is active, or changing the
base layer's resolution/format/colorspace, both force a full synchronous FX
preprocess+parse+SPIRV-codegen+Vulkan-pipeline build inline on the steamcompmgr
render thread (`ReshadeEffectManager::pipeline` → `ReshadeEffectPipeline::init`,
`:915`). That will visibly hitch the vblank-paced loop — see Q2 below.

**Adaptive Brightness is architecturally feasible but is the one item I'd prototype
before committing to it** — the plumbing for a multi-pass technique with a persistent,
frame-to-frame render-target texture appears to exist (compute *and* graphics passes,
named render targets, `LOAD_OP_LOAD` by default), but I could not find a shipped
example effect in this repo that actually exercises that path, so the "does last
frame's data really survive" claim rests on reading Vulkan barrier code, not on a
working reference. See Q4 and the Adaptive Brightness risk note.

## Q1 — Does live parameter update work without a recompile?

Yes, via a dedicated mechanism, but it's opt-in per uniform:

- `createReshadeUniforms()` (`src/reshade_effect_manager.cpp:615`) scans every
  `module.uniforms` and dispatches on its `source` annotation string. Built-ins:
  `frametime`, `framecount`, `gamescope_refresh_mhz`, `date`, `timer`, `pingpong`,
  `random`, `bufready_depth`, `gamescope_always_paint` (a flag, not a value). `key`,
  `mousebutton`, `mousedelta` exist as classes but are **TODO stubs that always return
  zero/false** (`KeyUniform::update` `:436`, `MouseButtonUniform::update` `:450`,
  `MouseDeltaUniform::update` `:485`) — not usable inputs today. `mousepoint` is real
  (`MousePointUniform::update` `:464`, reads `steamcompmgr_get_current_cursor()`).
- Any **other non-empty `source` string** becomes a `RuntimeUniform` (`:181`,`:509`),
  keyed by that string in a global map `g_runtimeUniforms`
  (`std::unordered_map<std::string, uint8_t*>`, `:30`, guarded by
  `g_runtimeUniformsMutex` `:31`). `reshade_effect_manager_set_uniform_variable(key, value)`
  (`:1938`) writes into that map and calls `force_repaint()`; `RuntimeUniform::update`
  (`:541`) reads it back every frame under the same lock and `memcpy`s into the mapped
  UBO, falling back to the uniform's `defaultValue` annotation if nothing was ever set.
  This is a genuinely live, per-frame, thread-safe path — the crux mechanism the three
  effects should use for every adjustable parameter.
- A uniform with **no `source` annotation at all** becomes `DataUniform` (`:196`) —
  `update()` just re-copies the FX initializer value (`copy<uint32_t>(mappedBuffer,
  nullptr)`, `:609`), unconditionally, every frame. **This is the important gotcha**:
  the standard ReShade authoring convention (`< ui_type = "slider"; ui_min = 0; ui_max
  = 1; >` with no `source`) does nothing here — gamescope has no code path that reads
  `ui_min`/`ui_max`/`ui_type` at all (not referenced anywhere in
  `reshade_effect_manager.cpp`). Any shader meant to be ImGui-controlled must be
  authored (or retrofitted) with `source = "some_name"` annotations, one per exposed
  parameter, and the overlay must call `reshade_effect_manager_set_uniform_variable`
  (in-process) or the `gamescope_reshade.set_uniform_variable` Wayland request
  (out-of-process) with that same name.
- Wire format: `gamescope_reshade.set_uniform_variable` takes `key: string`,
  `value: array` (`protocol/gamescope-reshade.xml:52`) — the handler
  (`CReshadeManager::SetUniformVariable`, `src/WaylandServer/Reshade.h:35`) copies the
  raw bytes and hands them to the manager function verbatim; `RuntimeUniform::update`
  reinterprets those bytes per the uniform's declared FX type (float/bool/int/uint,
  1–4 components) — so the byte layout the overlay writes must match the shader's
  declared uniform type exactly, there's no type-checking at the protocol boundary.

## Q2 — Selection/loading lifecycle, and is any of it blocking on the render thread?

Two independent knobs, plumbed through different mechanisms, that both funnel into one
global `std::string g_reshade_effect` (`src/steamcompmgr.cpp:174`):

- **CLI/startup**: `--reshade-effect <path>` sets `g_reshade_effect` directly at parse
  time (`src/steamcompmgr.cpp:8742`); `--reshade-technique-idx <n>` sets
  `g_reshade_technique_idx` (`:8744`).
- **Runtime, via the `gamescope_reshade` Wayland protocol**
  (`protocol/gamescope-reshade.xml`, impl `src/WaylandServer/Reshade.h`): `set_effect`
  → `reshade_effect_manager_set_effect(path, callback)` (`:1951`) only stores the path
  and callback in globals (`g_reshadeEffectPath`, `g_effectReadyCallback`) — **no
  compile happens yet**. `enable_effect` → `reshade_effect_manager_enable_effect()`
  (`:1959`) → `gamescope_set_reshade_effect()` (`src/steamcompmgr.cpp:5990`), which
  sets an **X11 root-window property** (`gamescopeReshadeEffect`) rather than touching
  `g_reshade_effect` directly. The steamcompmgr thread's X11 event loop picks up the
  property-change and only then sets `g_reshade_effect = path`
  (`src/steamcompmgr.cpp:7048`). This X11-property hop is almost certainly what the
  existing `reshade-effects.md` doc means by "asynchronously loads" — it decouples the
  protocol call from the render loop by one event-loop round trip, but it is **not** a
  background compile thread.
- **The actual compile is synchronous and on the render thread.** Every call to
  `vulkan_composite()` (`src/rendervulkan.cpp:4030`) builds a `ReshadeEffectKey{path,
  bufferWidth, bufferHeight, bufferColorSpace, bufferFormat, techniqueIdx}` from the
  base layer (`:4041`-`:4049`) and calls `g_reshadeManager.pipeline(key)`
  (`:4051`). `ReshadeEffectManager::pipeline()` (`:1921`) does a single-entry cache
  check (`m_lastKey == key`) and, on any mismatch, calls `pipeline->init(m_device,
  key)` (`:1929`) **inline, on whichever thread called `vulkan_composite()`** — which,
  per `superdoc/architecture/overview.md`, is the **steamcompmgr thread**, the same
  thread that paces the vblank loop and presents frames. `init()` (`:915`) does FX
  preprocessing/`#include` resolution, full parsing, SPIRV codegen, and Vulkan
  buffer/texture/sampler/pipeline-layout/pipeline creation — real, possibly-slow work,
  all blocking that thread. **Flag: this will visibly stutter** on (a) first enabling
  an effect, (b) switching effects or technique index, and (c) — easy to miss — any
  change to the base layer's width/height/format/colorspace, since those are part of
  the cache key. A window resize (nested/windowed backends) or the running game
  changing its internal render resolution will silently force a full recompile on the
  next frame.
- The `effect_ready` Wayland event (client-facing "your effect is compiled and live"
  signal) fires from inside `ReshadeEffectPipeline::update()` (`:1672`-`:1677`), which
  runs on the steamcompmgr thread as part of `execute()` — so the callback that ends
  up calling `gamescope_reshade_send_effect_ready()` (a `wl_resource` send) also
  executes off the main/Wayland thread. I did not find a `wlserver_lock()` in that call
  chain; whether this send is currently safe is a genuine open item, not something I
  could confirm or rule out from reading alone (see Risks).
- **Cross-thread safety note**: `g_runtimeUniforms` is mutex-guarded (see Q1), but
  `g_reshadeEffectPath` / `g_effectReadyCallback` (globals in
  `reshade_effect_manager.cpp`) and `g_reshade_effect` (`steamcompmgr.cpp:174`) are
  plain globals written from the main thread (protocol dispatch, X11 property handler)
  and read from the steamcompmgr thread with no lock I could find. Today this only
  changes rarely (effect switches); an ImGui overlay that toggles effects more often
  raises the odds of hitting this in practice.

## Q3 — Where does the ReShade pass run relative to scaling and HDR?

Confirmed order in `vulkan_composite()` (`src/rendervulkan.cpp:4030`):

1. **ReShade** (`:4037`-`:4059`) — runs first, and **only on `frameInfo->layers.get(0)`**
   (the base/game layer), replacing its texture in place
   (`pipeline->execute(frameInfo->layers.get(0).tex, &frameInfo->layers.get(0).tex)`,
   `:4056`). It never sees overlay/cursor/decoration layers, and it runs on the raw
   app-committed pixels for that layer, in that layer's own
   `GamescopeAppTextureColorspace` (linear/sRGB/scRGB/HDR10_PQ) — **before** any
   gamescope color-management transform.
2. **FSR/NIS/blit scaling** (`:4080` onward, per
   `superdoc/features/scaling-filters.md`) — still base-layer-only, upscales/sharpens
   *after* ReShade has already run.
3. **Compositing all layers + HDR color management** — `bindColorMgmtLuts()`
   (`:4077`-`:4078`) binds the shaper/3D LUTs consumed during the blit/FSR/NIS
   dispatch and blend (`bind_all_layers()`, `src/rendervulkan.cpp:3949`); this is where
   the base layer (now post-ReShade, post-upscale) is finally blended with cursor/
   overlay layers and run through gamescope's SDR↔HDR tonemap.

So the fixed order is **ReShade → upscale/sharpen → composite+HDR tonemap**, and
ReShade cannot see or affect any layer but the base one. This is architecturally the
*right* place for an exposure/vibrancy adjustment (content-adaptive, before
display-referred tonemapping), but it means ReShade-based sharpening runs on the
**pre-upscale, source-resolution** image — a materially different result from
gamescope's existing RCAS/NIS sharpen, which is explicitly a **post-upscale** pass at
output resolution (see Q on Sharpness in Per-effect design, and
`superdoc/features/scaling-filters.md:26`-`:44`).

Also worth noting: ReShade submits its command buffer on `device->generalQueue()` /
`device->generalCommandPool()` (`src/reshade_effect_manager.cpp:1002`,`:1015`), **not**
the dedicated async-compute queue the rest of the composite path prefers
(`superdoc/features/compositing-vulkan.md`, queue selection at
`src/rendervulkan.cpp:365`; queue members `CVulkanDevice::m_generalQueue` /
`m_generalCommandPool`, `src/rendervulkan.hpp:898`,`:905`). A community comment on
`ValveSoftware/gamescope#958` makes the same observation independently: enabling any
ReShade effect adds latency because that work competes with the game's own
graphics-queue work rather than running alongside it on the dedicated compute queue —
this is a property of *having any ReShade effect active at all*, not specific to which
of the three effects is chosen.

## Q4 — Do the three shaders exist, or must they be authored?

**None exist in this repo.** `src/reshade` is a git submodule
(`.gitmodules`: `path = src/reshade`, `url =
https://github.com/Joshua-Ashton/reshade`) that vendors only the `reshadefx`
parser/codegen library (`effect_parser.hpp`, `effect_codegen.hpp`,
`effect_preprocessor.hpp`, included from `reshade_effect_manager.cpp:11`-`:13`) — it
is not checked out in this working tree and, even checked out, is a compiler library,
not a shader collection. `ReshadeEffectPipeline::init()` loads FX source from
`$prefix/share/gamescope-ritz/reshade/Shaders/<key.path>` (local-usr then global-usr,
falling back to the plain unnamespaced `$prefix/share/gamescope/reshade/Shaders/<key.path>`
at each scope for a user's pre-existing shader library, `:939`-`:975`) — that install path
doesn't exist anywhere in this tree either. All
three effects must be authored (or ported from the community `crosire/reshade-shaders`
library and adapted, since stock `ui_*`-only uniforms are inert here per Q1).

The subset of ReShade FX gamescope actually implements, verified against
`reshade_effect_manager.cpp`:

- **Supported**: multiple passes per technique, both **compute** (`pass.cs_entry_point`,
  dispatch at `:1799`-`:1806`) and **graphics/pixel** passes (dynamic rendering via
  `vkCmdBeginRendering`, `:1826`-`:1863`) in the same technique; named render targets
  looked up by `unique_name` (`findTexture`, `:1891`); up to 8 simultaneous color
  attachments per pass; blend state (`ConvertReshadeBlendOp`/`ConvertReshadeBlendFactor`,
  `:764`-`:793`, wired at `:1475`-`:1482`); per-pass `clear_render_targets` controlling
  `VK_ATTACHMENT_LOAD_OP_CLEAR` vs `..._LOAD` (`:1843`); samplers with
  min/max LOD (`:1249`-`:1250`); a handful of built-in dynamic uniform sources (Q1).
  A `AlwaysScanout`/`gamescope_always_paint` flag exists as a gamescope-specific
  annotation (`:677`-`:680`, `src/reshade_effect_manager.hpp:45`).
- **Explicitly stubbed / not real**: depth-buffer access always reports
  `hasDepth = false` (`DepthUniform::update`, `:499`-`:502`) — any effect that branches
  on `bufready_depth` will always see "no depth," so depth-aware effects are not
  viable. Keyboard/mouse-button/mouse-delta uniforms are TODO stubs (Q1) — an effect
  cannot react to input today.
- **Stencil ops are compiled out** (`#if 0` block, `:730`-`:762`) — stencil-testing FX
  passes are not supported.
- **Uncertain / not verified**: the `pooled` texture-sharing annotation (a real
  upstream ReShade FX feature — per ReShade's own docs, it lets multiple effects share
  GPU memory for same-size/format textures) — I did not find handling of it in
  `reshade_effect_manager.cpp`; since only one effect is ever active at a time here
  (Q-Risks), it may simply be irrelevant rather than unsupported, but I can't confirm
  either way from source alone.

**Per-effect authoring assessment**:

- **Vibrancy**: single technique, single pixel pass, no textures beyond the input —
  well within the supported subset. Low risk.
- **Sharpness (if built as ReShade rather than reusing the existing control — see Q on
  Sharpness below)**: single pixel or compute pass, standard unsharp-mask math. Low
  risk technically, but see the strong recommendation against building it at all.
- **Adaptive Brightness**: needs a multi-pass technique — one or more downsample/
  reduction passes to measure scene luminance (compute passes, which are supported),
  plus a persistent 1×1-or-small "history" render target that is read *and* written
  across frames to implement temporal smoothing (an exponential moving average toward
  the current frame's measured luminance), plus a final pass that applies the
  resulting gain to the image. The **plumbing appears present**: `m_textures`/`m_rt`
  are owned by the `ReshadeEffectPipeline` object, which is cached whole by
  `ReshadeEffectManager` and only rebuilt when the `ReshadeEffectKey` changes
  (Q2) — so as long as the effect/technique/buffer-dims stay stable, the *same*
  `VkImage` objects persist frame to frame, and a render target whose pass doesn't set
  `clear_render_targets` uses `VK_ATTACHMENT_LOAD_OP_LOAD` (`:1843`), i.e. keeps its
  prior contents. The one caveat: at the top of every `execute()` call, **every**
  storage/render-target texture and `m_rt` get `m_cmdBuffer->discardImage()`
  (`:1766`-`:1776`) before the pass loop runs. I read `discardImage()`
  (`src/rendervulkan.cpp:1874`) as only setting a per-command-buffer bookkeeping flag
  used to pick `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED` for that image's first barrier
  in *that* submission (a Vulkan layout-transition hint, not an explicit clear/zero) —
  which should be compatible with content surviving across frames since the memory
  itself isn't touched — **but I did not find a shipped effect in this repo that
  actually exercises persistent-texture history to confirm this empirically**, and
  Vulkan's spec leaves content on an `UNDEFINED`-sourced transition
  implementation-defined in the sense that no guarantee is made either way — drivers
  commonly preserve it, but "commonly" isn't "guaranteed." **Recommend building a
  minimal throwaway prototype effect** (one pass that increments a value stored in a
  persistent 1×1 texture and displays it as a color) before committing engineering
  time to the full adaptive-brightness pipeline, to settle this empirically on target
  hardware/drivers rather than trust this read of the barrier code.

  **Update (2026-08-21):** that spike ran — persistence confirmed empirically on
  real hardware (RADV/AMD 7900 XTX), not just inferred from the barrier code, plus
  two unrelated findings (a zero-uniform-buffer crash in `ReshadeEffectPipeline::init()`,
  and a same-execute() recompile hazard if the implicit-output pass isn't last).
  Full method and evidence in `superdoc/planning/DECISIONS.md` #14's update block.
  Adaptive Brightness is now implemented on an experimental branch
  (`reshade/Shaders/gamescope-ritz.fx`, `src/Overlay/PanelShaders.cpp`).

  **Update (2026-08-22):** landed on master. Re-validated against a real `mpv`
  client playing a genuinely time-varying (looping dark/bright) video through the
  full pipeline, not just `vkcube` — luminance range compressed roughly 3× with
  the effect on vs off. Full method/numbers in `DECISIONS.md` #14's second update
  block. One gotcha worth keeping here: `gamescopectl screenshot`'s default
  screenshot type (`base_plane_only`) bypasses `vulkan_composite()` — and
  therefore ReShade — entirely; measuring ReShade's effect on a screenshot
  requires explicitly passing type `3` (`full_composition`) as the command's
  second argument.

## Q5 — Parameters to expose per effect

See **Per-effect design** below for the concrete list; summarized here for reference:
Adaptive Brightness needs target luminance, adaptation speed (possibly separate
up/down), min/max gain clamps, and an overall mix/strength; Vibrancy needs a single
signed strength (and optionally a skin-tone-protection toggle, a common vibrance
refinement); Sharpness — see the strong recommendation to not build a separate control
at all.

## Q6 — HDR interaction

- ReShade sees `frameInfo->layers.get(0)` **before** gamescope's own color management
  runs (Q3), and the shader is compiled with the layer's actual colorspace baked in as
  a preprocessor macro: `BUFFER_COLOR_SPACE` (`src/reshade_effect_manager.cpp:934`,
  built from `ConvertToReshadeColorSpace(key.bufferColorSpace)`, `:692`-`:706`) and
  `BUFFER_COLOR_BIT_DEPTH` (`:935`). A layer's colorspace is
  `GamescopeAppTextureColorspace` — `LINEAR`/`SRGB` (SDR), `SCRGB` (extended-range
  linear, values can exceed 1.0 and dip below 0 near wide-gamut edges), or `HDR10_PQ`
  (PQ-encoded, perceptually nonlinear, codeword 0–1 maps to roughly 0–10,000 nits) —
  see `superdoc/features/hdr-color-management.md`.
- **Danger**: a naive vibrancy (HSV-style saturation boost assuming clamped 0–1 RGB) or
  adaptive-brightness (linear average assuming display-referred SDR luma) written
  without checking `BUFFER_COLOR_SPACE` will silently produce wrong results on
  scRGB/HDR10 content — over-saturating/clipping on scRGB highlights, or measuring
  "luminance" directly on PQ codewords (which is not luminance at all without going
  through the PQ EOTF first). Gamescope's own `pq_to_nits()`/`nits_to_pq()`
  (`src/color_helpers.h:53`,`:69`) are C++ host-side helpers **not available inside the
  FX shader** — if HDR support is wanted, the PQ math has to be reimplemented in FX
  source by hand.
- **Recommendation**: gate all three effects to SDR-only for v1 — branch on
  `BUFFER_COLOR_SPACE` (or simpler, have the ImGui overlay just not enable the effect
  when the focused app's layer isn't `LINEAR`/`SRGB`) — until HDR-correct math is
  written and tested. This is a scope/safety flag, not a hard architectural blocker.
- One structural plus: because ReShade runs *before* gamescope's SDR↔HDR tonemap
  (Q3), an SDR-space Adaptive Brightness gain is applied to source pixels ahead of
  display-referred tonemapping — the architecturally correct order for an
  exposure-style adjustment, once/if HDR support is added properly.

## Per-effect design

### Vibrancy
- **Pipeline placement**: single-pass ReShade technique, base layer, pre-upscale (Q3).
- **Parameters**: `Vibrance` — signed strength, range **-1.0 to +1.0**, default **0.0**
  (no change); optionally `ProtectSkinTones` — bool toggle, default **true**, a common
  refinement in reference vibrance shaders (e.g. ReShade's own `Vibrance.fx`) to avoid
  oversaturating faces.
- **Risk**: low. Standard single-pass color-grade math, well within the supported FX
  subset (Q4). Main real risk is the HDR-space one (Q6) — vibrance math must clamp or
  branch correctly on scRGB.

### Sharpness — **flag: likely redundant with the existing RCAS/NIS control**
- Gamescope already has a live, working sharpen knob:
  `g_upscaleFilterSharpness` (`src/main.hpp:66`, 0–20, default 2), set via `--sharpness`
  or the `GAMESCOPE_SHARPNESS` X11 property polled at `src/steamcompmgr.cpp:6578`
  (`superdoc/features/scaling-filters.md`). It runs **after** upscale, at output
  resolution, as part of the same RCAS (`cs_composite_rcas.comp`) or NIS
  (`cs_nis.comp`) dispatch — and only has an effect when `--filter fsr` or `--filter
  nis` is selected; it's a no-op for `linear`/`nearest`/`pixel`.
- A ReShade "Sharpness" effect, by contrast, would run **before** upscale, at
  source resolution (Q3) — a materially different visual result (amplifies
  source-resolution detail/noise that then gets blurred by the scaler, rather than
  sharpening the final displayed image), and would double-apply (compounding halos)
  whenever the user also has `--filter fsr`/`nis` selected with its own sharpness > 0.
- **Recommendation**: do not build a separate ReShade sharpness effect. Expose the
  *existing* `g_upscaleFilterSharpness` control directly as the ImGui slider (0–20,
  wired the same way the `GAMESCOPE_SHARPNESS` property already is). The one case
  where a distinct pre-upscale ReShade sharpen might still be wanted is if the product
  goal is "sharpen even when filter=linear/nearest, where RCAS/NIS never runs at all" —
  that's a real but different feature; flagged as Open Question 1.

### Adaptive Brightness — highest risk of the three
- **Pipeline placement**: multi-pass technique, base layer, pre-upscale, pre-HDR-tonemap
  (Q3, Q6) — measures and adjusts the *source* image before gamescope's own tonemap
  sees it, which is the right order.
- **Structure** (standard technique per external research — see Sources): one or more
  compute passes reduce the base layer to an average-luminance value (a mip-chain-style
  iterative downsample, or a luminance histogram for more accurate exposure behavior —
  histograms resist over/under-weighting by outlier bright/dark pixels but cost more
  passes/complexity; see Bruop/alextardif sources below); a persistent small texture
  stores an exponentially-smoothed "adapted luminance" that blends toward the current
  frame's measurement over time (not snapping instantly) — this is the part needing
  the frame-to-frame texture persistence flagged as uncertain in Q4; a final pass
  computes a gain from `target / adapted_luminance` (clamped) and applies it to the
  image.
- **Parameters**:
  - `TargetLuminance` — float, range **0.1–0.9** (normalized), default **0.5**.
  - `AdaptationSpeed` — float, "seconds to reach ~63% of the target" (a time constant),
    range **0.1–5.0**, default **1.0**. Consider splitting into `AdaptUpSpeed` /
    `AdaptDownSpeed` (brightening vs darkening) since real eye-adaptation shaders
    commonly adapt faster one direction than the other (matches the "eye dazzled
    coming out of a tunnel" asymmetry noted in the sources below).
  - `MinGain` / `MaxGain` — float, range **0.5–1.0** / **1.0–2.0**, defaults **0.5** /
    **2.0** — hard clamps so the effect can't crush blacks or blow out highlights
    without bound.
  - `Strength` — float **0.0–1.0**, default **1.0** — a dry/wet mix against the
    unmodified image, useful for the ImGui overlay to fade the effect in/out without
    fully disabling it (which would tear down the pipeline/lose adaptation state,
    Q2).
- **Risk**: the effect concept and math are standard and well-documented externally
  (see Sources), but this repo's ability to actually persist a texture across frames
  inside one technique is inferred from code, not demonstrated by a shipped example —
  build the throwaway persistence prototype from Q4 before committing to this design.
  Also inherits the resize-resets-history problem (Risks, below).

## Risks

- **Blocking compile on the render thread** (Q2) on effect/technique switch and on any
  base-layer resolution/format/colorspace change — will stutter the vblank-paced loop;
  an ImGui overlay that lets users freely toggle effects needs to either accept
  occasional hitches, debounce toggles, or the manager needs a non-blocking/background
  compile path before this is safe to expose casually.
- **Resize silently resets Adaptive Brightness's adaptation state** — because
  `ReshadeEffectKey` includes buffer width/height, any resolution change (windowed
  resize, or a game changing its internal render resolution) creates a new
  `ReshadeEffectPipeline`, which destroys the old persistent luminance texture and
  starts adaptation over from scratch (as well as paying the blocking-compile cost
  above). Worth deciding whether this is acceptable (Open Question 3).
- **Only one effect active at a time.** `ReshadeEffectManager` caches exactly one
  pipeline (`m_lastKey`/`m_lastPipeline`, `src/reshade_effect_manager.hpp:101`-`:102`);
  a maintainer discussion on `ValveSoftware/gamescope#962` confirms multi-effect
  support is a still-open feature request upstream, not something this fork has either.
  If the product intent is three independently toggleable ImGui checkboxes, they must
  either be combined into one `.fx` file with one technique (simpler, works today) or
  the manager needs extending to chain multiple pipelines (bigger change).
- **General queue contention** (Q3) — any active ReShade effect competes with the
  game's own GPU work on the general graphics+compute queue rather than running
  alongside it on gamescope's dedicated async-compute queue; a fixed cost of the
  feature existing, independent of which effect.
- **Possible cross-thread races** on `g_reshadeEffectPath`/`g_effectReadyCallback`/
  `g_reshade_effect`, unlike the mutex-protected `g_runtimeUniforms` path (Q2) —
  worth an audit before an overlay drives effect switches more frequently than the
  current CLI/protocol-only usage pattern does.
- **No shader assets ship in-repo**, and the install path
  (`share/gamescope-ritz/reshade/Shaders/`) doesn't exist in this tree (Q4) — packaging/
  install wiring for wherever the three new `.fx` files live is itself unscoped work.
  (Superseded by M6: `reshade/Shaders/gamescope-ritz.fx` now ships in-repo and
  `default_extras_install.sh` installs it to that path.)
- **Stock community ReShade shaders are inert here** (Q1) — reusing an existing
  community effect as a shortcut requires retrofitting `source` annotations onto every
  parameter meant to be ImGui-controlled; it is not a drop-in.
- **HDR correctness** (Q6) — flagged above; silently wrong (not crashing) on
  scRGB/HDR10_PQ content if shipped without color-space-aware math or an SDR-only gate.

## Open questions for the user

1. Drop "Sharpness" from the ReShade three and just surface the existing
   `g_upscaleFilterSharpness` (0–20, RCAS/NIS) control in the ImGui overlay instead —
   or is a genuinely separate pre-upscale ReShade sharpen wanted (e.g. specifically to
   cover `--filter linear/nearest`, where RCAS/NIS sharpening never runs)?
2. Only one ReShade effect can be active at a time today (Risks) — should Adaptive
   Brightness + Vibrancy (+ Sharpness, if kept) ship as one combined multi-technique
   `.fx` file with a single "which technique" selector, or is extending
   `ReshadeEffectManager` to run multiple chained pipelines in scope?
3. Given a resize forces both a render-thread hitch and a reset of Adaptive
   Brightness's adaptation state (Risks), is an occasional visible pop/re-adapt on
   resize acceptable for v1, or does that block shipping the feature until the manager
   supports non-blocking recompiles?
4. Should all three effects be hard-gated to SDR content (bypassed on scRGB/HDR10_PQ
   base layers) for v1, or is correct HDR-space math in scope from the start (Q6)?
5. Does the ImGui overlay act as an actual `gamescope_reshade` Wayland client (protocol
   round-trip, same path as any external tool) or call `reshade_effect_manager_*`
   in-process (bypassing the protocol since it lives in the same binary)? This decides
   which thread the overlay's calls land on and what locking discipline it owes
   `g_reshade_effect`/`g_reshadeEffectPath` (Risks).
6. For Adaptive Brightness, is a cheap average-luminance downsample acceptable, or is
   the more accurate (and more passes/complexity) histogram approach wanted (Q on
   Adaptive Brightness design, sources below)?
7. Should Adaptive Brightness react only to the base/game layer (all it can
   structurally see today, Q3), or is reacting to the final composited frame
   (including overlays/cursor, post-HDR-tonemap) an actual requirement — which would
   need the effect moved to a different point in the pipeline than where ReShade runs
   today?

## Sources checked (external, dated 2026-08-21)

- [ValveSoftware/gamescope#958 "Reshade support"](https://github.com/ValveSoftware/gamescope/issues/958) —
  confirms general/graphics-queue latency cost and that uniforms need the
  gamescope-reshade Wayland protocol or they "just use their initializer values."
- [ValveSoftware/gamescope#962 "Support multiple reshade shaders"](https://github.com/ValveSoftware/gamescope/issues/962) —
  confirms only one effect is active at a time today; multi-effect is an open request,
  not implemented anywhere.
- [crosire/reshade-shaders REFERENCE.md](https://github.com/crosire/reshade-shaders/blob/slim/REFERENCE.md) —
  background on the `pooled` texture annotation (not found handled in this repo's code, Q4).
- [Bruno Opsenica, "Automatic Exposure Using a Luminance Histogram"](https://bruop.github.io/exposure/) —
  histogram-vs-downsample tradeoff used in the Adaptive Brightness design section.
- [Alex Tardif, "Adaptive Exposure from Luminance Histograms"](https://alextardif.com/HistogramLuminance.html) —
  same, plus temporal-adaptation curve shape.
- [MJP, "Average luminance calculation using a compute shader"](https://therealmjp.github.io/posts/average-luminance-compute-shader/) —
  confirms the mip-chain/iterative-downsample compute approach is standard and viable
  on the compute-pass support this repo's FX subset already has (Q4).
