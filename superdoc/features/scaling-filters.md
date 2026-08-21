# Scaling & Sharpening Filters — how the base layer gets resized

When the running application's buffer doesn't match the output resolution, gamescope
has to scale it. Which algorithm does that scaling — and how the aspect ratio is fit
to the screen — are two independent choices (`--filter` and `--scaler`), both resolved
into flags on the `FrameInfo_t` that [compositing-vulkan.md](compositing-vulkan.md)
dispatches.

## How it works

- **Filter** (the resampling algorithm) is `GamescopeUpscaleFilter`
  (`src/main.hpp:35`): `LINEAR`, `NEAREST`, `FSR`, `NIS`, `PIXEL`. Parsed from the
  `--filter`/`-F` CLI flag by `parse_upscaler_filter()` (`src/main.cpp:409`) into
  `g_wantedUpscaleFilter` (`src/main.hpp:64`); the live value used by the compositor
  is `g_upscaleFilter` (`src/main.hpp:62`).
- **Scaler** (how the content rectangle is fit into the output) is a separate enum,
  `GamescopeUpscaleScaler` (`src/main.hpp:53`): `AUTO`, `INTEGER`, `FIT`, `FILL`,
  `STRETCH`. Parsed from `--scaler`/`-S` by `parse_upscaler_scaler()`
  (`src/main.cpp:391`). The actual output scale factor for a given source size is
  computed by `calc_scale_factor_scaler()` (`src/steamcompmgr.cpp:1527`):
  `STRETCH` scales X/Y independently to fill the output; `FIT`/`AUTO` scale
  uniformly to the smaller axis (`AUTO` additionally clamps to `g_flMaxWindowScale`);
  `FILL` scales uniformly to the larger axis (cropping); `INTEGER` floors the
  uniform scale to a whole number once it exceeds 1.0, so pixel-art content scales in
  exact multiples instead of a fractional ratio.
- **FSR** (`GamescopeUpscaleFilter::FSR`) is AMD FidelityFX Super Resolution 1, run as
  two compute passes when `frameInfo->useFSRLayer0` is set (decided in
  `src/steamcompmgr.cpp:2689`, true only when the filter is FSR *and* the layer
  actually needs scaling): an EASU upscale pass (`src/shaders/cs_easu.comp`, pipeline
  `SHADER_TYPE_EASU`) into a scratch image, then an RCAS sharpen pass
  (`src/shaders/cs_composite_rcas.comp`, `SHADER_TYPE_RCAS`) that also does the final
  layer composite in the same dispatch (`src/rendervulkan.cpp:4080`-`4109`). Both
  passes `#include "ffx_fsr1.h"` (`src/shaders/ffx_fsr1.h`), AMD's vendored reference
  implementation — gamescope does not reimplement the EASU/RCAS math itself.
  RCAS sharpness is driven by `g_upscaleFilterSharpness / 10.0f`
  (`src/rendervulkan.cpp:4109`).
- **NIS** (`GamescopeUpscaleFilter::NIS`) is Nvidia Image Scaling, run when
  `frameInfo->useNISLayer0` is set (`src/steamcompmgr.cpp:2690`): one compute pass
  (`src/shaders/cs_nis.comp`, `SHADER_TYPE_NIS`) upscales into a scratch image using
  precomputed coefficient textures (`g_output.nisScalerImage`/`nisUsmImage`,
  `src/rendervulkan.cpp:4131`,`4134`), then a plain BLIT pass composites that scratch
  image as if it were a screen-size layer (`src/rendervulkan.cpp:4149`). Sharpness is
  inverted from the same `g_upscaleFilterSharpness` value:
  `(20 - g_upscaleFilterSharpness) / 20.0f` (`src/rendervulkan.cpp:4123`).
- **NEAREST / LINEAR / PIXEL** need no extra compute pass — they're expressed purely
  as a sampler choice on the plain BLIT path. `bind_all_layers()`
  (`src/rendervulkan.cpp:3949`) sets nearest-neighbour sampling when
  `Layer_t::filter == NEAREST`, when the layer is already screen-sized
  (`isScreenSize()`), or when it's `LINEAR` but the colorspace won't convert to linear
  automatically. `PIXEL` is resolved earlier, per-layer, in
  `src/steamcompmgr.cpp:2248`-`2252`: if the scale ratio on both axes is an exact
  integer, `PIXEL` downgrades itself to `NEAREST` (no benefit to a smarter filter when
  every source pixel maps to a whole block of output pixels); otherwise it currently
  falls through to whatever `Layer_t::filter` already was. *Why PIXEL exists
  separately from NEAREST:* it's meant to read as "sharp/pixel-perfect if possible",
  future room for a smarter non-blurry filter for the non-integer case, without
  callers having to know which case they're in.
- Hardware overlay planes only support bilinear scaling, not FSR/NIS/nearest —
  `DoesHardwareSupportUpscaleFilter()` (`src/main.hpp:46`) returns true only for
  `LINEAR`. `commit_t::ShouldPreemptivelyUpscale()` (`src/commit.cpp:133`) uses that
  to decide whether a FIFO (vsync-paced) commit should be pre-upscaled via shader
  ahead of time rather than relying on the hardware scaler at present time.
  *Why gate on FIFO:* pre-emptive upscaling costs a shader pass per commit, so it's
  only worth paying for content that's actually going to be shown at a steady cadence,
  not e.g. an unthrottled high-fps commit stream.
- Sharpness is a single shared control, `g_upscaleFilterSharpness`
  (`src/main.hpp:66`, default `2`, `src/main.cpp:318`), 0–20, set via `--sharpness` or
  the `GAMESCOPE_SHARPNESS` X11 property (clamped in `src/steamcompmgr.cpp:6578`); FSR
  and NIS each remap it into their own native sharpness range as noted above.

## Using it

- `--filter <linear|nearest|fsr|nis|pixel>` (`-F`) selects the resampling algorithm.
- `--scaler <auto|integer|fit|fill|stretch>` (`-S`) selects how content is fit to the
  output rectangle.
- `--sharpness <0-20>` sets FSR/NIS sharpening strength; ignored by `linear`/`nearest`/
  `pixel`.
- At runtime, the same choices are exposed as X11 root-window properties gamescope
  polls (e.g. the sharpness property handled at `src/steamcompmgr.cpp:6578`) — see
  [scripting-convars.md](scripting-convars.md) for the general convar/property
  mechanism these ride on.

## Options

| Config key | Default | Meaning |
| --- | --- | --- |
| `--filter` | `linear` (`GamescopeUpscaleFilter::LINEAR`, `src/main.cpp:316`) | Resampling algorithm: `linear`, `nearest`, `fsr`, `nis`, `pixel`. |
| `--scaler` | `auto` (`GamescopeUpscaleScaler::AUTO`, `src/main.cpp:314`) | Content-fit mode: `auto`, `integer`, `fit`, `fill`, `stretch`. |
| `--sharpness` | `2` (`src/main.cpp:318`) | 0–20, FSR RCAS / NIS sharpen strength. |

## Related links

- [compositing-vulkan.md](compositing-vulkan.md) — the dispatch loop these filters plug
  into, and how `FrameInfo_t` carries the resolved filter per layer.
- [hdr-color-management.md](hdr-color-management.md) — colorspace/EOTF handling that
  runs alongside these same compute passes.
- [scripting-convars.md](scripting-convars.md) — the general mechanism for the
  runtime-settable equivalents of these CLI flags.
