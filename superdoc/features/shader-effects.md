# Shaders settings area — Vibrancy, Shadow Control, Pre-Sharpen, Adaptive Brightness

The overlay's **Shaders** area (`image.shaders`, `src/Overlay/PanelShaders.cpp`) exposes
four independent effects. Since 2026-09-05 they are **one native compute pre-pass compiled
into the binary at build time** — `src/shaders/cs_effects_layer0.comp`, dispatched from
`vulkan_composite()` (`src/rendervulkan.cpp`) on the base/game layer at source resolution
before any scaling. They used to be gated passes inside a runtime-compiled ReShade file,
`reshade/Shaders/gamescope-ritz.fx`; that file, and the install step that copied it, are
gone (`superdoc/planning/DECISIONS.md` #27). The ReShade loader itself stays, for users'
own `.fx` files — see [reshade-effects](reshade-effects.md), which is now third-party-only.

**Titles vs. identifiers.** The three multi-word switches were retitled 2026-09-05:
"Shadow lift" → **Shadow Control**, "Adaptive brightness" → **Adaptive Brightness**,
"Pre-sharpen" → **Pre-Sharpen**. `Why only the titles moved:` an entry id is what the
command palette's saved entries and every cross-reference resolve against, and a config
key is what a user's `global.json` already contains — renaming either would break
existing configs and palette state for a purely cosmetic change. So the id
`image.shaders.shadow_lift` and the config struct `ReshadeShadowLiftSettings` keep the
old spelling. Expect the code and this page to say "shadow lift" where it means the
identifier and "Shadow Control" where it means the label.

## Why a native pre-pass, not the `.fx` (2026-09-05)

The `.fx` was compiled at runtime from whichever copy won a four-directory search
(`reshade-effects.md`). A stale copy under the legacy `~/.local/share/gamescope/reshade`
tree silently no-op'd Shadow Control and Adaptive Brightness for the user: the panel
pushed uniforms the compiled module never declared, they were dropped by name, and a
green build proved nothing because the shader was not part of the build. Diagnostics
rows were added to *detect* that; the user decided to *remove the possibility* instead.

Now `src/meson.build`'s `glsl_generator` compiles `cs_effects_layer0.comp` to
`cs_effects_layer0.h` (SPIR-V as a C array) alongside every other compute shader. A GLSL
error **fails the build** — verified by deliberately breaking the shader: `glslang`
reports the error and `ninja` stops. The panel and the shader ship in one binary and
cannot drift; there is nothing on disk to shadow.

## Where it runs, and why not inside the composite shaders

`vulkan_composite()` records the pre-pass on the same compute command buffer, right after
the ReShade block and before the FSR/NIS/blur/blit branch chooses a path:

```
layer0.tex (game, source res)
   │  [ReShade, if a user .fx is set]           -- unchanged, general queue + CPU wait
   ▼
cs_effects_layer0  ──►  g_output.effectsOutput  (pooled, source res, 8×8 groups)
   │  frameInfo->layers[0].tex = effectsOutput; bBaseLayerEffectsApplied = true
   ▼
FSR / NIS / blur / blit as before
```

`Why not fold the maths into cs_composite_*.comp:` layer 0 is read four different ways
there (`composite.h`, `cs_composite_rcas.comp`, `blur.h`, `cs_composite_blur*.comp`), so
it would be four copies of the maths; Pre-Sharpen needs its neighbours at **source**
resolution before scaling, which the composite shaders never see; and the composite chain
has already degamma'd by the time it has a colour, while these effects were authored for
encoded values (next section). A pre-pass that swaps its output into layer 0 is exactly
how the ReShade slot already worked, so every downstream path consumes it unchanged.

`Why the same command buffer:` the ReShade block runs on the general queue and then
`g_device.wait()`s — a per-frame CPU stall. The native pass uses `bindTarget` /
`uploadConstants` / `dispatch` like EASU→RCAS does, so the barriers are inserted by
`dispatch()` and nothing waits on the CPU.

**Never twice per frame.** `vulkan_composite()` is called more than once on the *same*
`FrameInfo_t`: the backend's `Present()` composites `paint_all()`'s frameInfo in place
(SDL/Wayland/OpenVR pass the pointer through; DRM composites a copy), and
`gamescopectl screenshot` then re-composites that struct into a mappable texture. Under
the `.fx` this double-applied — measured live as gain² in a `gamescopectl` screenshot vs
gain in `grim` of the same frame. Two gates now stop it: `bBaseLayerEffectsApplied`
(renamed from `bBaseLayerReshaded`) is set on the frameInfo the moment either the native
pass or ReShade runs, as well as by steamcompmgr for the pre-emptive-upscale texture; and
the native pass also refuses when layer 0 already *is* `g_output.effectsOutput`.

## Encoded space, on purpose

The ReShade loader bound the input through `view(false)` = `m_srgbView` — the raw UNORM
view — so the `.fx` always read gamma-encoded 0..1 code values. The native pass matches
it: `bindTexture(0, layer0.tex); setTextureSrgb(0, true)` selects the same raw view (as
`bind_all_layers()` does for SRGB layers), and the storage target is written through its
UNORM view, so encoded goes in and encoded comes out. The composite shaders degamma
afterwards exactly as before.

**Naming trap** (see `rendervulkan.cpp`'s own TODO on it): `m_srgbView` means *values
still sRGB-encoded* (the UNORM format); `m_linearView` is the `_SRGB` Vulkan format that
the hardware linearises on read. `setTextureSrgb(slot, true)` therefore means "give me
the encoded values".

**SDR only (v1)**, `superdoc/planning/DECISIONS.md` #15: the host skips the dispatch for
HDR (scRGB / HDR10 PQ) and passthru layers and for YCbCr, and the panel greys every switch
with the `kSdrOnly` reason via `g_eLastBaseLayerColorspace` / `IsBaseLayerSdr()`. No
linear-light or PQ value can reach the `pow()`.

**Output format.** The pooled `effectsOutput` uses the input's own DRM format when the
device supports it as an optimal-tiled storage+sampled image (so a 10-bit game stays
10-bit), else `ARGB8888` — 10-bit storage images are optional in Vulkan (see
`vulkan_get_rgb10_capture_format()`'s note on NVIDIA). Checked once per format.

## The uniform block and the seams left for Adaptive Brightness

`EffectsPushData_t` (`src/rendervulkan.cpp`, inside the `#pragma pack(push,1)` region,
beside `EasuPushData_t`) mirrors the shader's `effects_t` block field-for-field:

| Field | Meaning |
| --- | --- |
| `uint u_flags` | bits: `1<<0` Shadow Control, `1<<1` Vibrancy, `1<<2` protect skin, `1<<3` Pre-Sharpen, `1<<4` Adaptive Brightness *(reserved)*, `1<<31` reset history *(reserved)* |
| `float u_vibrancy` | 0..3, 1 neutral |
| `float u_shadowLift` | 0..1, 0 neutral |
| `uint u_rcasCon` | `floatBitsToUint(con.x)` for RCAS, 0 when sharpen is off |
| `float u_abTarget, u_abUp, u_abDown, u_abMin, u_abMax, u_abStrength, u_abDt` | **reserved** for Adaptive Brightness; already filled from config, consumed by nothing yet |

Host state is `g_nativeEffects` (`NativeEffectsState_t`, `src/rendervulkan.hpp`): a plain
struct written by `PanelShaders.cpp` and by `main.cpp`'s startup config apply, read by
`vulkan_composite()` and by the three backends' "needs full composite" decision — all on
the steamcompmgr thread, same discipline as `g_upscaleFilterSharpness`. `Why the startup
apply:` under E2 nothing in `PanelShaders.cpp` runs per frame, so without it saved effects
would only switch on the first time the Shaders area was drawn.

Adaptive Brightness's rows, config, and values are intact and plumbed into the reserved
fields; the shader multiplies by a constant `1.0` at the clearly marked seam, so the effect
has **no visible result** until its history/measure half is rebuilt. The seams: sampler
slot `VKR_EFFECTS_HISTORY_SLOT` (= 1, `descriptor_set_constants.h`) for the 1×1 adapted-
luminance texel; the reserved `u_ab*` fields and the two reserved flag bits; the second
`EffectsPushData_t` constructor argument (`flAbDtSeconds`, `0.0f` today); and
`NativeEffectsState_t::AnyEnabled()`, which deliberately does not count
`bAdaptiveBrightness` yet so an inert switch does not force a full composite.

## Backends

Every backend already forced a full composite for `!g_reshade_effect.empty()`
(`DRMBackend.cpp`, `WaylandBackend.cpp`, `OpenVRBackend.cpp`). Each now also ORs in
`vulkan_native_effects_active()`; without that, direct scanout would skip the pre-pass and
the effects would silently vanish whenever the base layer could be scanned out directly.

## The four effects

The maths is ported 1:1 from the retired `.fx`, applied **per tap, in this order**:
Shadow Control → Vibrancy → (Pre-Sharpen) → × Adaptive Brightness gain → `saturate`.

### Shadow Control (`image.shaders.shadow_lift`)

Added 2026-09-04 (request #3: *"a darkness booster for dark games"*). Brightens dark
areas so detail becomes visible while leaving bright areas essentially alone — **not** a
global brightness control and **not** shadow-crushing.

**Config**: `ReshadeShadowLiftSettings` — `enabled` (default false), `strength` (0.0..1.0,
default 0.0/neutral). Purely additive keys; no migration.

```
exponent = 1.0 - 0.5 * strength   // 1.0 (identity) down to 0.5 (sqrt) at strength=1.0
output   = pow(saturate(color), exponent)
```

`Why a gamma curve:` 0.0 and 1.0 are fixed points of *any* power curve, so black and white
never move — the "leave highlights alone" requirement guaranteed mathematically. The
curve's effect concentrates at the low end (`0.1 → 0.316` vs `0.9 → 0.949` at full
strength). The exponent floor of 0.5 is the same shape a "raise gamma to ~2.0" boost
applies. `Why first:` a tone/exposure adjustment; lift-before-saturate is the conventional
grading order, and it means Vibrancy's grey target is computed from the lifted colour.

### Vibrancy (`image.shaders.vibrancy`)

Adaptive saturation with an optional skin-tone damper.

**Config**: `ReshadeVibrancySettings` — `enabled`, `strength` (float),
`protect_skin_tones` (default true).

**`strength` — a true saturation multiplier, 0.0..3.0, neutral at 1.0** (changed
2026-09-04, request #2, from an additive -1.0..+1.0 boost):

```
mix   = min(strength, 1.0)
boost = max(strength - 1.0, 0.0) * (1.0 - saturation) * skin_protect
output = lerp(luma, color, mix + boost)
```

`mix` alone reaches both endpoints (0.0 = full grey, 1.0 = unchanged) with a plain blend
toward luma, carrying none of the adaptive shaping — at 0.0 *every* pixel must land on the
same grey. `boost` is zero at and below neutral and picks up the adaptive shape above it.
`Why 0.0..3.0 with neutral at 1.0:` the user chose to keep desaturation reachable
(`requests-2026-09-04.md` #2).

#### Migration: an existing config's old value

`kCurrentSchemaVersion` 1 → 2; `Migrate_1_to_2()` (`src/Config/ConfigManager.cpp`)
transforms `reshade.vibrancy.strength` once, on load: `new = clamp(old + 1.0, 0.0, 3.0)`.
`Why +1.0:` it carries old-neutral (0.0) onto new-neutral (1.0), so an untouched config
does not open in black and white; a customised value keeps its displacement from neutral.
A config saved under schema 2 round-trips unmigrated — `tests/test_config.cpp`.

### Pre-Sharpen (`image.shaders.presharpen`) — now RCAS

**Config**: `ReshadePreSharpenSettings` — `enabled`, `strength` (`optional<float>`,
0.0..2.0, default 0.5).

The `.fx` used a plain 4-tap unsharp mask. The native pass **reuses FSR1's RCAS**
(`src/shaders/ffx_fsr1.h`, `FSR_RCAS_F`) instead: the same 5-tap cross, but clip-aware
(its lobe is limited so a sharpened value cannot overshoot the local min/max ring) and
normalised, so it does not ring on hard edges at high strength. `FsrRcasLoadF(p)` is
defined as a clamped `texelFetch` on slot 0 **with Shadow Control and Vibrancy applied to
each tap**, so the sharpen sees the graded image exactly as the `.fx`'s pass order did;
when sharpen is off the centre tap is used directly. `FsrRcasInputF` is empty, as in
`cs_composite_rcas.comp` — input is already encoded.

**Slider → `con.x` mapping**, so today's slider feel is preserved: RCAS scales its lobe by
`con.x ∈ 0..1` (`FsrRcasCon()` derives it as `exp2(-stops)`). With `k` the slider value,

```
con.x = clamp( k / (0.75 * (1 + k)), 0, 1 )      // 0→0 (off), 0.5→0.444, 1→0.667, 2→0.889
```

monotonic and saturating, so the top of the slider is "as sharp as RCAS goes" rather than
a cliff. Passed as float bits (`u_rcasCon`), like `RcasPushData_t::u_c1`.

### Adaptive Brightness (`image.shaders.adaptive_brightness`)

Six params (the settings budget exactly — see below). Rows and config unchanged; **no
visible result in this build** — see the seams section above. Its `.Default()` values in
the panel and `ConfigSchema.h`'s compiled-in defaults currently disagree; that is left to
the Adaptive Brightness rebuild.

## The settings-panel budget

Each switch row may own at most six `Param`s before `Registry.cpp` aborts registration —
see `PanelShaders.cpp`'s "THE SIX BUDGET" comment. Counts: Vibrancy 2, Pre-Sharpen 1,
Adaptive Brightness 6 (zero headroom), Shadow Control 1.

## Diagnostics

The **Pipeline** Facts row keeps "base layer" (the SDR gate) and says the effects are built
into the binary. The "effect file" / "compiled" / "loaded from" / "uniforms" rows added
2026-09-05 for the stale-file case were removed the same day along with the failure they
diagnosed.

## Related links

- [reshade-effects](reshade-effects.md) — the ReShade loader, now for third-party `.fx`
  files only.
- [compositing-vulkan](compositing-vulkan.md) — where the pre-pass sits in
  `vulkan_composite()`.
- [scaling-filters](scaling-filters.md) — the built-in FSR/NIS path whose RCAS this reuses.
- `superdoc/planning/DECISIONS.md` #12 (two sharpen controls), #15 (SDR-only), #27 (the
  native port; #13/#14 superseded).
- `superdoc/planning/requests-2026-09-04.md` items #2 and #3 — Vibrancy's range and
  Shadow Control.
