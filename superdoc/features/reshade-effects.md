# ReShade Effects — runtime post-process shader pipeline

A runtime pipeline that compiles and runs [ReShade](https://reshade.me/)-style `.fx`
shader effects against the composited frame, for visual post-processing (e.g. color
grading, sharpening, CRT-style filters) chosen by the user at runtime.

This is a **different feature from FSR/NIS upscaling** — those are the *scaling* filters
covered in [scaling-filters](scaling-filters.md); ReShade effects are arbitrary
user-authored shader passes injected into the composite step, not Gamescope's own
upscale/sharpen implementations. *Why the split:* scaling filters are a fixed, built-in
part of the composite path with a small tuning surface (ratio/sharpness), while ReShade
effects are an open-ended plugin surface (arbitrary compiled shader code) with its own
resource/pipeline lifecycle — different enough in shape to be a separate page.

## How it works

- `ReshadeEffectPipeline` (`src/reshade_effect_manager.hpp:49`) owns one compiled effect:
  its Vulkan pipelines, textures, samplers, uniform buffer, descriptor sets, and the
  parsed `reshadefx::module`. `init(CVulkanDevice*, const ReshadeEffectKey&)`
  (declared `:55`) builds all of that for a specific `ReshadeEffectKey` — effect path,
  target buffer size/colorspace/format, and technique index — so a pipeline is keyed to
  both the shader file and the exact framebuffer it will run against.
- `ReshadeEffectManager` (`src/reshade_effect_manager.hpp:91`) caches a single "last"
  compiled pipeline (`m_lastKey` / `m_lastPipeline`); `init(CVulkanDevice*)`
  (declared `:96`, implemented `src/reshade_effect_manager.cpp:1910`) stores the device,
  and `pipeline(const ReshadeEffectKey&)` (`:98`) returns the cached pipeline if the key
  matches or rebuilds it otherwise. *Why:* effects are swapped rarely relative to frames
  rendered, so a one-entry cache avoids recompiling the shader module every frame while
  keeping the manager itself trivial — no need for a full pipeline cache/LRU.
- Free functions bridge the manager to the rest of Gamescope:
  `reshade_effect_manager_set_uniform_variable(key, value)` pushes a uniform value by
  name; `reshade_effect_manager_set_effect(path, callback)`
  (`src/reshade_effect_manager.hpp:110`, implemented
  `src/reshade_effect_manager.cpp:1951`) asynchronously loads/compiles a new effect file
  and invokes `callback` when done; `reshade_effect_manager_enable_effect()` /
  `reshade_effect_manager_disable_effect()` toggle whether the active effect is applied
  during composite.
- The `gamescope_reshade` Wayland protocol (`protocol/gamescope-reshade.xml`) exposes this
  to clients — a client sets/enables/disables an effect and pushes uniform values over the
  protocol rather than Gamescope reading files off disk itself at the client's request.

## Using it

A client (or Gamescope's own tooling) binds the `gamescope_reshade` protocol global, sends
the path to an `.fx` effect file to compile, waits for the compiled-effect callback/event,
enables it, and optionally pushes uniform variable values to drive runtime parameters
(e.g. a strength slider) each frame.

## Options

| Config key | Default | Meaning |
| --- | --- | --- |
| `ReshadeEffectFlag::AlwaysScanout` (`src/reshade_effect_manager.hpp:45`) | off | Per-pipeline flag bit; effect always contributes to the scanout-eligible output rather than only an intermediate buffer. |

## Related links

- [scaling-filters](scaling-filters.md) — the separate, built-in FSR/NIS upscale/sharpen path this feature is not part of.
- [compositing-vulkan](compositing-vulkan.md) — the Vulkan composite path ReShade pipelines execute within.
- [wayland-protocols](wayland-protocols.md) — conventions for Gamescope's custom protocols, including `gamescope-reshade.xml`.
- [shader-effects](shader-effects.md) — the settings-panel/effect-content layer built on top of this mechanism (Vibrancy, Shadow Lift, Pre-Sharpen, Adaptive Brightness, `reshade/Shaders/gamescope-ritz.fx`).
