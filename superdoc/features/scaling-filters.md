# Scaling & Sharpening Filters — how the base layer gets resized

When the running application's buffer doesn't match the output resolution, gamescope
has to scale it. Which algorithm does that scaling — and how the aspect ratio is fit
to the screen — are two independent choices (`--filter` and `--scaler`), both resolved
into flags on the `FrameInfo_t` that [compositing-vulkan.md](compositing-vulkan.md)
dispatches.

## How it works

- **Filter** (the resampling algorithm) is `GamescopeUpscaleFilter`
  (`src/main.hpp:40`): `LINEAR`, `NEAREST`, `FSR`, `NIS`, `PIXEL`. Parsed from the
  `--filter`/`-F` CLI flag by `parse_upscaler_filter()` (`src/main.cpp:425`) into
  `g_wantedUpscaleFilter` (`src/main.hpp:69`); the live value used by the compositor
  is `g_upscaleFilter` (`src/main.hpp:67`).
- **Scaler** (how the content rectangle is fit into the output) is a separate enum,
  `GamescopeUpscaleScaler` (`src/main.hpp:58`): `AUTO`, `INTEGER`, `FIT`, `FILL`,
  `STRETCH`. Parsed from `--scaler`/`-S` by `parse_upscaler_scaler()`
  (`src/main.cpp:407`). The actual output scale factor for a given source size is
  computed by `calc_scale_factor_scaler()` (`src/steamcompmgr.cpp:1729`):
  `STRETCH` scales X/Y independently to fill the output; `FIT`/`AUTO` scale
  uniformly to the smaller axis (`AUTO` additionally clamps to `g_flMaxWindowScale`);
  `FILL` scales uniformly to the larger axis (cropping); `INTEGER` floors the
  uniform scale to a whole number once it exceeds 1.0, so pixel-art content scales in
  exact multiples instead of a fractional ratio.
- **FSR** (`GamescopeUpscaleFilter::FSR`) is AMD FidelityFX Super Resolution 1, run as
  two compute passes when `frameInfo->useFSRLayer0` is set (decided in
  `steamcompmgr.cpp`'s per-frame paint, the "just draw focused window as normal" branch
  around `src/steamcompmgr.cpp:2989` — see "FSR/NIS at native resolution" below for
  exactly when that's true): an EASU upscale pass (`src/shaders/cs_easu.comp`, pipeline
  `SHADER_TYPE_EASU`) into a scratch image, then an RCAS sharpen pass
  (`src/shaders/cs_composite_rcas.comp`, `SHADER_TYPE_RCAS`) that also does the final
  layer composite in the same dispatch (`src/rendervulkan.cpp`, around `:6029`). Both
  passes `#include "ffx_fsr1.h"` (`src/shaders/ffx_fsr1.h`), AMD's vendored reference
  implementation — gamescope does not reimplement the EASU/RCAS math itself.
  RCAS sharpness is driven by `g_upscaleFilterSharpness / 10.0f`.
- **NIS** (`GamescopeUpscaleFilter::NIS`) is Nvidia Image Scaling, run when
  `frameInfo->useNISLayer0` is set (same gate as FSR, see below): one compute pass
  (`src/shaders/cs_nis.comp`, `SHADER_TYPE_NIS`) upscales into a scratch image using
  precomputed coefficient textures (`g_output.nisScalerImage`/`nisUsmImage`), then a
  plain BLIT pass composites that scratch image as if it were a screen-size layer
  (`src/rendervulkan.cpp`, around `:6062`). Sharpness is inverted from the same
  `g_upscaleFilterSharpness` value: `(20 - g_upscaleFilterSharpness) / 20.0f`.
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
  `DoesHardwareSupportUpscaleFilter()` (`src/main.hpp:51`) returns true only for
  `LINEAR`. `commit_t::ShouldPreemptivelyUpscale()` (`src/commit.cpp:133`) uses that
  to decide whether a FIFO (vsync-paced) commit should be pre-upscaled via shader
  ahead of time rather than relying on the hardware scaler at present time.
  *Why gate on FIFO:* pre-emptive upscaling costs a shader pass per commit, so it's
  only worth paying for content that's actually going to be shown at a steady cadence,
  not e.g. an unthrottled high-fps commit stream.
- Sharpness is a single shared control, `g_upscaleFilterSharpness`
  (`src/main.hpp`, default `2`, `src/main.cpp:334`), 0–20, set via `--sharpness`/
  `--fsr-sharpness` (aliases) or the `GAMESCOPE_SHARPNESS`/`GAMESCOPE_FSR_SHARPNESS` X11
  properties (handled in `src/steamcompmgr.cpp`, around `:7110`); FSR and NIS each
  remap it into their own native sharpness range as noted above.
- **Changing the filter KEEPS the sharpness value** (reversed 2026-09-22; see below).

### Switching filters keeps the sharpness value

There has only ever been ONE stored sharpness — one global
(`g_upscaleFilterSharpness`), one config key (`gamescope.sharpness`). Until
2026-09-22, `PanelDisplay.cpp`'s `SetFilter()` reset the UI slider to 0 % on every
actual filter change (the user, 2026-08-24: *"Combine them, so it is just the
sharpness (when switching between filters, it resets to 0%)"*) — the reasoning at
the time being that "80% of RCAS and 80% of NIS are not the same amount of
sharpening", so carrying a percentage across a filter switch silently applied a
strength the user never chose for that pass.

The user reversed that 2026-09-22: *"combine the sharpness value for both, so
switching in between filters and especially FSR and NIST doesn't set it to zero
again... And when selecting another filter, the slider should keep its value, but
it should just be grayed out... to indicate that it doesn't have an effect for that
filter mode."* `SetFilter()` no longer touches the sharpness value at all — it only
writes the selected filter. The row (`display.filter.sharpness`,
`RegisterUpscaling()` in `PanelDisplay.cpp`) already read the live
`g_upscaleFilterSharpness` unconditionally and was already greyed (not hidden or
zeroed) via `.DisabledUnless( SharpnessApplies, ... )` whenever the selected filter
has no sharpening pass (Linear/Nearest/Pixel) — that half of "combine + keep +
grey out" needed no code change, only the reset needed removing.

### FSR/NIS apply at native resolution too

**Since 2026-09-22, FSR and NIS also run at exact native resolution (`scale == 1`
on both axes), not just when actually upscaling.** The user: *"make sure that
FreeSync and NIST upscaling works even at the native resolution, so it behaves more
like a filter instead of an actual upscaler."* ("FreeSync and NIST" is
speech-to-text for FSR and NIS.)

The gate lives in `steamcompmgr.cpp`'s per-frame paint, in the "just draw focused
window as normal" branch (around `src/steamcompmgr.cpp:2989`):

```cpp
bool bFilterPassApplies = frameInfo.layers.get( 0 ).scale.x <= 1.001f && frameInfo.layers.get( 0 ).scale.y <= 1.001f;
frameInfo.useFSRLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::FSR && bFilterPassApplies;
frameInfo.useNISLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::NIS && bFilterPassApplies;
```

`Layer_t::scale` is texture size / destination size — `< 1` is an upscale, `== 1`
is native, `> 1` is a downscale (`rendervulkan.hpp`'s `Layer_t`). Before this
change the condition required `scale < 0.999f` strictly, so at `1:1` neither pass
ran and the plain BLIT path (no sharpening at all) silently took over instead.

**Downscaling (`scale > 1`) stays excluded**, deliberately: EASU's documented range
is 1x–4x *up*sampling only (`src/shaders/ffx_fsr1.h`), and NIS's own
`NVScalerUpdateConfig()` rejects `kScaleX`/`kScaleY > 1`
(`src/shaders/NVIDIAImageScaling/NIS/NIS_Config.h`) — neither filter is designed to
run backwards, so a downscaled layer keeps falling through to the plain BLIT path.

**What happens at exactly `1:1`:** `Layer_t::integerWidth()`/`integerHeight()`
equal the input texture size, so EASU dispatches input→input — a near-identity
resample — and RCAS still applies its sharpen kernel on top at the configured
strength. NIS at `scale == 1.0` passes its own bounds check the same way, and its
upscale pass becomes a near-identity resample feeding its own (still active) sharpen
term. Net effect: with FSR or NIS selected and the game already at the output's
native resolution, the picture is no longer bit-identical to Linear — it's sharpened
by the same RCAS/NIS strength the Sharpness slider drives when actually upscaling.

**Consumers double-checked for a `scale == 1` FSR/NIS-active frame:**
- `update_tmp_images()`'s scratch-image sizing (`rendervulkan.cpp`) already sizes off
  `integerWidth()/integerHeight()`, which just equals the input size at `1:1` — no
  special case needed.
- The DRM, OpenVR and Wayland backends' direct-scanout / plane-passthrough decisions
  (`bNeedsFullComposite |= pFrameInfo->useFSRLayer0/useNISLayer0` in
  `DRMBackend.cpp`, `OpenVRBackend.cpp`, `WaylandBackend.cpp`) now also force full
  compositing at native resolution whenever FSR/NIS is selected, where before a
  native-res frame with FSR/NIS selected could still take the cheaper scanout path
  (the filter was a no-op then, so skipping composition was invisible). This is the
  intended cost of the sharpen pass actually running — there is no way to sharpen a
  frame without compositing it.
- `g_bFSRActive` (drives `mangoapp`'s `fsrUpscale` flag and the
  `GAMESCOPE_FSR_ACTIVE` root-window property, both read from `frameInfo.useFSRLayer0`
  each frame) now correctly reports FSR as active at native resolution too, which is
  the accurate answer now that the pass genuinely runs.
- The pre-emptive FIFO upscale path (`steamcompmgr.cpp`, `bPreemptiveUpscale`) already
  set `useFSRLayer0`/`useNISLayer0` from the selected filter alone, with no scale
  gate at all — this change brings the main per-frame path in line with that existing
  precedent rather than introducing a new shape.
- The overlay's own blur-behind pass (`SettingsOverlay.cpp`) and the `g_BlurMode`
  path (`steamcompmgr.cpp`) both already clear both flags unconditionally whenever
  they need the base layer for their own effect instead — unaffected by this change.

## Using it

- `--filter <linear|nearest|fsr|nis|pixel>` (`-F`) selects the resampling algorithm.
- `--scaler <auto|integer|fit|fill|stretch>` (`-S`) selects how content is fit to the
  output rectangle.
- `--sharpness <0-20>` sets FSR/NIS sharpening strength; ignored by `linear`/`nearest`/
  `pixel`.
- At runtime, the same choices are exposed as X11 root-window properties gamescope
  polls (e.g. the sharpness properties handled around `src/steamcompmgr.cpp:7110`) —
  see [scripting-convars.md](scripting-convars.md) for the general convar/property
  mechanism these ride on.

## Options

| Config key | Default | Meaning |
| --- | --- | --- |
| `--filter` | `linear` (`GamescopeUpscaleFilter::LINEAR`, `src/main.cpp:332`) | Resampling algorithm: `linear`, `nearest`, `fsr`, `nis`, `pixel`. |
| `--scaler` | `auto` (`GamescopeUpscaleScaler::AUTO`, `src/main.cpp:333`) | Content-fit mode: `auto`, `integer`, `fit`, `fill`, `stretch`. |
| `--sharpness` | `2` (`src/main.cpp:334`) | 0–20, FSR RCAS / NIS sharpen strength. Aliased as `--fsr-sharpness`. |

## Related links

- [compositing-vulkan.md](compositing-vulkan.md) — the dispatch loop these filters plug
  into, and how `FrameInfo_t` carries the resolved filter per layer.
- [hdr-color-management.md](hdr-color-management.md) — colorspace/EOTF handling that
  runs alongside these same compute passes.
- [scripting-convars.md](scripting-convars.md) — the general mechanism for the
  runtime-settable equivalents of these CLI flags.
