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
cs_effects_measure ──► g_output.effectsHistory  (4×1, persistent; one 16×16 workgroup,
   │                    ▲   reads last frame's value, writes this frame's)
   ▼                    │
cs_effects_layer0  ──►  g_output.effectsOutput  (pooled, source res, 8×8 groups)
   │  a private copy of the FrameInfo_t gets layers[0].tex = effectsOutput;
   │  the caller's struct is never written
   ▼
FSR / NIS / blur / blit as before
```

Both dispatches share one `uploadConstants<EffectsPushData_t>()` (same `effects_t` block,
same values); the measure dispatch runs whenever the pre-pass runs at all — see
[Adaptive Brightness](#adaptive-brightness-imageshadersadaptive_brightness) below.

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

**Exactly once per composite — and the caller's struct is never written.**
`vulkan_composite()` is called more than once on the *same* `FrameInfo_t`: the backend's
`Present()` composites `paint_all()`'s frameInfo (SDL/Wayland/OpenVR pass the pointer
through, `steamcompmgr.cpp` `paint_all()` → `Present()`; DRM composites a copy,
`DRMBackend.cpp`'s `compositeFrameInfo`), and `gamescopectl screenshot` types 3 and 4
(`full_composition`, `screen_buffer`) then composite that very struct again into a
mappable texture (`steamcompmgr.cpp`'s screenshot block, right after `Present()`). Type 1
and 2 captures go through `vulkan_screenshot()` instead, which runs no effect pass at all
— see the follow-up note below.

The rule is about the **texture** in layer 0, never about which call this is: *grade it
unless it already carries the effects*. Two textures do — the pre-emptively upscaled one
(`steamcompmgr.cpp` `paint_window_commit()` marks the struct it puts it in with
`bBaseLayerEffectsApplied`; the passes ran at source resolution while building it) and
the pooled `g_output.effectsOutput` itself (an identity check no caller's struct can
trigger any more, kept because it is free). Everything else is raw and gets the pass. To
make that hold, `vulkan_composite()` takes a `const FrameInfo_t *` and **never writes
it**: once ReShade or the native pass has run, the function continues on a private copy
of the struct whose layer 0 is the processed texture (`SubstituteLayer0` in
`rendervulkan.cpp`), so the caller's layer 0 stays raw. Every composite of a frame then
starts from the same raw source and runs the passes fresh — the present composite, DRM's
copy, and a screenshot composite each apply them once, and the screenshot is pixel-
identical to the presented frame (same source, same shaders, same uniforms). The cost is
one extra sub-millisecond pass on a screenshot frame only. `steamcompmgr` is the flag's
only writer.

`Why not the flag-based version (2026-09-05, removed the same day):` the first native
port set `bBaseLayerEffectsApplied` on the struct **after** the pass ran and swapped
`layers[0].tex` for `effectsOutput` in place, so the second call would skip. That made
every later composite depend on the *first* call's side effects — which struct it ran on
(DRM's copy or `paint_all()`'s own), whether the flag travelled with the texture it
described, and whether the pooled texture layer 0 now pointed at still held *this* frame.
Measured live on the laptop: under the `.fx` (no flag) a `gamescopectl screenshot "<p>
4"` showed the effects **twice** (gain²) against `grim`'s once; with the flag it showed
them **not at all** (Vibrancy 3×: `(199,24,0)` = raw in the screenshot vs `(254,2,0)` on
screen; Shadow Control 1.0: `(0,196,48)` vs `(0,225,112)`). A "skip me next time" marker
on a shared, mutable struct is the wrong tool for "apply once": whether it is *right* is a
question about another call's history. Do not reintroduce it — the const signature is
there to make the in-place swap a compile error.

**Follow-up (not done):** `vulkan_screenshot()` — screenshot types 1 (`base_plane_only`,
the default, and the type Steam's own F12 uses via the X11 property) and 2
(`all_real_layers`), plus the PipeWire capture — is a plain blit and has never run either
effect pass, so those captures show the raw game. Making it call the same pre-pass helper
would give every capture path the presented look; it is a separate decision because it
also changes what a PipeWire stream carries.

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

## The uniform block

`EffectsPushData_t` (`src/rendervulkan.cpp`, inside the `#pragma pack(push,1)` region,
beside `EasuPushData_t`) mirrors the `effects_t` block in `src/shaders/effects_common.h`
field-for-field. That header is shared by both shaders and also holds the flag bits, the
per-tap `grade()` (Shadow Control + Vibrancy) and the history pack/unpack helpers, so the
measure pass grades its taps with exactly the code the per-pixel pass grades its pixels.

| Field | Meaning |
| --- | --- |
| `uint u_flags` | bits: `1<<0` Shadow Control, `1<<1` Vibrancy, `1<<2` protect skin, `1<<3` Pre-Sharpen, `1<<4` Adaptive Brightness, `1<<5` Adaptive Brightness's **Dynamic** mode (else Whole image), `1<<31` reset history (the history texture was created this frame, or the pre-pass is resuming after a frame in which it did not run — see "Resets on resume" below) |
| `float u_vibrancy` | 0..3, 1 neutral |
| `float u_shadowLift` | 0..1, 0 neutral |
| `uint u_rcasCon` | `floatBitsToUint(con.x)` for RCAS, 0 when sharpen is off |
| `float u_abTarget, u_abUp, u_abDown, u_abMin, u_abMax, u_abStrength` | Adaptive Brightness's six parameters, straight from config (both modes read all six — see the Dynamic section for what each means there) |
| `float u_abDt` | seconds since the previous effects dispatch, host-measured and clamped (see Adaptive Brightness) |

Host state is `g_nativeEffects` (`NativeEffectsState_t`, `src/rendervulkan.hpp`): a plain
struct written by `PanelShaders.cpp` and by `main.cpp`'s startup config apply, read by
`vulkan_composite()` and by the three backends' "needs full composite" decision — all on
the steamcompmgr thread, same discipline as `g_upscaleFilterSharpness`. `Why the startup
apply:` under E2 nothing in `PanelShaders.cpp` runs per frame, so without it saved effects
would only switch on the first time the Shaders area was drawn.

`NativeEffectsState_t::AnyEnabled()` counts all four switches, Adaptive Brightness
included, so any one of them forces the full composite the pre-pass needs.

## Backends

Every backend already forced a full composite for `!g_reshade_effect.empty()`
(`DRMBackend.cpp`, `WaylandBackend.cpp`, `OpenVRBackend.cpp`). Each now also ORs in
`vulkan_native_effects_active()`; without that, direct scanout would skip the pre-pass and
the effects would silently vanish whenever the base layer could be scanned out directly.

## The four effects

The maths is ported 1:1 from the retired `.fx`, applied **per tap, in this order**:
Shadow Control → Vibrancy → (Pre-Sharpen) → Adaptive Brightness (Whole image's gain, or
Dynamic's curve) → `saturate`.

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

**One row, two modes** (request #16, 2026-09-06). The row is a three-way **Choice** —
`Off` | `Whole image` | `Dynamic` — in its own **Adaptive Brightness** band below the
Effects band, with the same six params behind it: `strength`, `target`, `up_speed`,
`down_speed`, `min_gain`, `max_gain`. **Config**: `ReshadeAdaptiveBrightnessSettings` —
`enabled` (bool, kept) plus `mode` (`"whole_image"` | `"dynamic"`, default
`whole_image`, an unknown value on disk resolves to it) and the six floats, defaults
1.0 / 0.5 / 1.0 s / 1.0 s / 0.5 / 2.0. `Off` leaves `mode` alone, so switching back
lands on the mode the user had. The panel's `.Default()`s read that struct
(`config::ReshadeAdaptiveBrightnessSettings{}.field`, the `PanelCursor.cpp` pattern)
instead of repeating literals. `Why:` the two had drifted (panel said 1.5 s / 2.5 s / 0.8 /
1.6), so "reset to default" landed on values no fresh install ever had.

`Why a three-way Choice and not a Mode param:` the Six Budget (`PanelShaders.cpp`'s
header comment, SPEC §5.2 clause 3) — the row already owned six params, and a seventh is
a registration abort whose remedy is a whole new rail area for one effect. On/off and
mode are one decision ("which adaptation, if any"), so they became one control. The id
`image.shaders.adaptive_brightness` and every config key are unchanged;
`overlay_e2_set "image.shaders.adaptive_brightness 1"` means what the old switch's "on"
meant (Whole image is the original behaviour). `Why its own band:` the Effects band's
`n / m` corner count is computed from **Switch** rows only (`Shell.cpp`'s
`DrawGroupBand`), so a Choice row among the switches would make "3 / 3" sit under four
visible rows. The rail Summary still says "N of 4 effects on".

#### The statistics (both modes)

Two dispatches per frame. **The measure pass** (`cs_effects_measure.comp`,
`SHADER_TYPE_EFFECTS_MEASURE`) takes a **64×64 grid of taps** over the whole image (16×16
threads, 4×4 per thread, 4096 taps), each run through `grade()` so it measures the
Shadow-Control/Vibrancy-graded image the `.fx` measured (its `PreSharpenOut` texture) —
sharpening is not applied to the taps; it does not move the statistics. From those taps it
derives four numbers on the **encoded** Rec.601 luma (`(.299, .587, .114)`, as the `.fx`
computed it):

| History texel | Statistic | Read by |
| --- | --- | --- |
| `HISTORY_MEAN` (0) | arithmetic mean (shared-memory tree reduction, as before) | Whole image |
| `HISTORY_P2` (1) | 2nd percentile — the shadows | Dynamic (shadow cap) |
| `HISTORY_P50` (2) | median — the mid-tone anchor | Dynamic (gamma) |
| `HISTORY_P98` (3) | 98th percentile — the highlights | Dynamic (levels gain) |

The percentiles come from a **64-bin histogram in shared memory** (one `atomicAdd` per
tap, then invocation 0 walks the bins), **interpolated inside the bin** the target count
falls in — so a percentile is continuous rather than stepping by 4 code values as a
scene drifts across a bin edge. `Why 64 bins:` 4096 taps over 64 bins is 64 per bin on a
flat image; finer bins would be mostly empty and the interpolation already recovers
sub-bin precision. `Why median, not the mean, for Dynamic:` a few bright windows in a dark
room must not read as "the room is lit" — the `.fx`'s 25 fixed taps could be swung by one
highlight, the mean by a few percent of the image, the median by neither.

**Each statistic is smoothed with the `.fx`'s EMA**, unchanged, per statistic:

```
tau      = measured > adapted ? up_speed : down_speed
alpha    = clamp(1 - exp(-dt / max(tau, 0.001)), 0, 1)
adapted' = mix(adapted, measured, alpha)          // written to its history texel
```

`up_speed` is the time constant while a statistic **rises** (the scene got brighter; the
picture will be dimmed), `down_speed` while it **falls** (the scene got darker; the picture
will be lifted). The panel now says exactly that — "Adapt to brighter" / "Adapt to darker",
retitled 2026-09-06 from "Brighten speed" / "Darken speed", whose help text described the
opposite direction to the one the code used. Defaults are symmetric (1 s / 1 s); the eye
adapts to brightness faster than to darkness, and a game wants the blown-out case fixed
fast too, so a user who wants asymmetry sets `up_speed` shorter. Every statistic is
measured and smoothed **whatever the mode**, and the measure pass never looks at the
Adaptive Brightness flags, so a mode switch needs no re-convergence.

#### Whole image (`mode: whole_image`) — the original behaviour

```
// cs_effects_layer0.comp, per pixel:
gain = clamp(target / max(adapted_mean, 0.001), min_gain, max_gain)
out  = mix(c, c * gain, strength)
```

One gain for the frame from the smoothed mean, ported 1:1 from the `.fx`'s
`PS_AdaptiveBrightnessApply`. Its known weakness is the reason Dynamic exists: in a dark
scene the gain hits `max_gain` and every value above `1 / max_gain` **clips** — the
"blown-out parts" the user asked to be rid of (measured below).

#### Dynamic (`mode: dynamic`) — levels + gamma + shoulder

The curve lives in **`src/shaders/effects_curve.h`**, a header of pure scalar functions
that the GLSL pass and the C++ unit tests (`tests/test_effects_curve.cpp`) compile from the
same text, so the properties below are asserted on the code the GPU runs. Encoded space
in, encoded out, per channel, from the smoothed `p2 / p50 / p98`:

```
1. GAIN      G = clamp(0.9 / p98, min_gain, max_gain)            // levels: p98 -> WHITE
2. GAMMA     g = ln(target) / ln(p50 * G), clamped to [0.5, 1.5]   // median -> target
             if g > 1: g = min(g, ln(p2 * min_gain) / ln(p2 * G))  // shadow cap
3. SHOULDER  y = (x * G)^g;  top = G^g
             if top > 1 and y > 0.7:  u = y - 0.7;  y = 0.7 + u / (1 + u / c)
             with c = (top - 0.7)(0.3) / (top - 1), so x = 1 lands exactly on 1.0
out = mix(c, curve(c), strength)
```

- **Levels**: the smoothed 98th percentile is pulled toward `WHITE = 0.9`, as far as the
  user's gain bounds allow — a dark scene gets `max_gain`, a bright one a touch under 1.
  `Why no black-point subtraction, though "levels" usually has one:` pulling p2 toward black
  crushes the deepest shadows, which is the opposite of what a dark map needs, and capped
  small enough to be harmless on a mid scene it is also too small to do anything. Every
  step passes through (0, 0), so black stays black without a floor being needed.
- **Gamma**: the exponent that lands the smoothed **median** on `target` after the gain,
  bounded to `[0.5, 1.5]`. `Why 0.5:` a sqrt lift — the same floor Shadow Control uses —
  and with `max_gain` 2 it takes a 5..20-code scene to the 50..100 range. `Why 1.5:` a
  darkening gamma crushes shadows by nature; beyond 1.5 the shadow cap is all that keeps
  detail. **The shadow cap** is `min_gain`'s Dynamic meaning: a darkening gamma may not
  push the smoothed 2nd percentile below `p2 × min_gain` — "how dark may it go", applied
  to the shadows. `max_gain` bounds the levels gain only; the gamma lift on top is what
  lets a *super* dark map get further than Whole image's 2× ever could.
- **Shoulder**: only when the curve's own top (`G^g`, its value at x = 1) exceeds 1.0 —
  i.e. only when something *would* clip. Reinhard-shaped on the excess above the knee
  `0.7`, with `c` chosen so x = 1 lands exactly on 1.0: slope 1 at the knee (no visible
  break), monotonic for any overshoot, and it degenerates to the identity as the top → 1,
  so a scene that needs no compression gets none. `Why not a fixed asymptote below 1:` a mid
  scene's whites would dim for nothing; adaptive knee-to-top mapping keeps near-identity
  scenes near-identical.
- **Strength** blends the curve with the identity; at 0 the picture is untouched (tested).
- `Why encoded space, not linear light:` the pre-pass runs in encoded space on purpose (see
  "Encoded space, on purpose" above); the statistics, the sibling effects and `target`
  (an encoded mean target in Whole image mode) all live there; and the gain/gamma bounds are
  perceptual numbers — a 2× gain in encoded terms is a 2× perceptual step, which is what a
  user-facing "Max gain" should mean. The hard properties (monotonic, bounded, 0 → 0) hold
  in either space.

**Properties asserted on the CPU** (`[effects_curve]`, 11 cases, 1.26 M assertions over
seven scenes × five targets × nine gain-bound pairs): output in `[0, 1]` and finite;
monotonic in the input; `0 → 0`; `x = 1 → exactly 1` whenever the shoulder is active;
identity at strength 0; the mid reference scene is the identity; the gamma clamps and the
shadow cap hold; the shoulder is C0/C1-continuous at the knee. Plus the config round-trip
of `mode` and its unknown-value fallback.

#### Measured (desktop, headless, `scripts/effects-regression.sh`, 2026-09-06)

The recipe is `pixel-regression.sh`'s (private headless sway, nested `gamescope --backend
wayland`, `gamescopectl screenshot "<path> 4"`), with `tests/effects_scene_client.c` as
the game: an SDL2 window painting three synthetic scenes from flat regions at known pixel
positions — **dark** (bands 5/8/12/16/20, a pure-black corner, six 240 squares ≈ 1 % of the
image), **bright** (bands 200/215/230/245/255, six 30-valued rectangles ≈ 4 %, so p2 *is*
the shadows) and **mid** (26/77/128/179/230) — advanced with `SIGUSR1`. Every number is the
mean grey of a region's interior, all six Adaptive Brightness params at their defaults,
strength 1.0. Captures: `build-release/verify-shots/adaptive-2026-09-06/`.

| Scene / region (input) | Off | Whole image | **Dynamic** | Dynamic must |
| --- | --- | --- | --- | --- |
| dark: darkest band (5) | 5 | 10 | **50** | be readable: ≥ 30 |
| dark: bands 8 / 12 / 16 / 20 | 8 / 12 / 16 / 20 | 16 / 24 / 32 / 40 | **64 / 78 / 90 / 101** | keep their order |
| dark: 240 highlights | 240 | **255 — clipped** | **253** | stay < 255, above every band |
| dark: pure black | 0 | 0 | **0** | stay ≤ 2 |
| bright: bands 200 / 215 / 230 | 200 / 215 / 230 | 115 / 124 / 132 | **165 / 180 / 196** | keep their order |
| bright: 245 band (p98 region) | 245 | 141 | **213** | come down below 235 |
| bright: white (255) | 255 | 146 | **224** | — |
| bright: 30 shadows (p2) | 30 | 17 | **15** | not crushed: ≥ 8, and ≥ 30 × min_gain 0.5 |
| mid: 26 / 77 / 128 / 179 / 230 | identical | 26 / 77 / 128 / 178 / 229 | **25 / 75 / 126 / 177 / 228** | near-identity: worst ≤ 6 (measured 2) |

Whole image on the dark scene is the blown-out case the user described: the gain hits
`max_gain` 2 and the 240 highlights go to 255 with everything above 128 clipping, while the
darkest band only reaches 10. Dynamic takes that band to 50 and keeps the highlights at
253, two counts below white. On the bright scene Whole image dims everything to ~0.57×
(shadows 30 → 17); Dynamic keeps more of the picture (median 230 → 196) and the shadow cap
holds at exactly `30 × min_gain` = 15. The bright captures under Whole image are not
asserted — they are recorded as INFO lines to show the difference.

**Temporal** (dark → bright under Dynamic, `tau` 1 s; the "requested at" times are
measured from the `SIGUSR1` to the screenshot request, the screenshot itself adds a
frame): the 230 band read **252** at 0.37 s, **245** at 1.29 s, **205** at 3.36 s, and
**196** settled — monotonic, no overshoot, within 12 counts of settled at 3 s. The first
second looks slow because while the smoothed gain is still above 1 the shoulder pins the
bright bands near white; once the gain drops under 1 the curve releases them. No
oscillation was seen in any capture.

**Per-frame cost.** The measure pass is still one 16×16 workgroup over 4096 taps; the
histogram adds one shared-memory `atomicAdd` per tap and a 64-iteration walk on one
invocation. The per-pixel pass, in Dynamic mode, adds two `log`s per invocation (gain and
gamma are frame constants) and three `pow`s plus three divides per pixel. Neither is
measurable against the ~sub-millisecond pre-pass on this desktop's GPU; there is no
GPU-timestamp instrumentation in `vulkan_composite()` to give a finer number.

**Resets on resume, does not track while off.** The measure pass runs whenever the
pre-pass runs at all (any of the four switches on) and never looks at the Adaptive
Brightness flag itself; only the per-pixel gain is gated. But when Adaptive Brightness is
the *only* switch on and it is turned off, `NativeEffectsState_t::AnyEnabled()` goes
false and the whole pre-pass — measure dispatch included — stops running, so the
history freezes at its last values instead of continuing to track the scene.

`vulkan_composite()` handles this by remembering, across calls, whether the measure
dispatch ran the *previous* time this code path was reached
(`s_bEffectsPassRanLastTime`). Whenever it resumes after not having run — the switch
flipped back on, or content came back to SDR RGB after a stretch of HDR/passthru or
YCbCr frames that skipped the pass — the pre-pass sets `kResetHistory` for that frame
exactly as it does for a freshly-created history, so the very first re-enabled frame
writes the current measurement straight into the history instead of blending with the
stale one. Re-enabling on a changed scene is therefore instant and correct, with no
clipped first frame and no re-convergence ramp.

`Why not track while off (the `.fx` did):` the alternative — running the measure dispatch
even with every switch off, so the history stays warm — would require
`vulkan_native_effects_active()` (`AnyEnabled()`) to report "active" purely to keep a
disabled effect's history fresh. The backends OR that into `bNeedsFullComposite`
(`WaylandBackend.cpp`, `DRMBackend.cpp`, `OpenVRBackend.cpp`), which defeats DRM direct
scanout for a feature the user has switched off. Reset-on-resume gets the same "instant"
result — no stale-gain slam, no ramp — for zero cost while off, at the price of one
`kResetHistory` frame instead of a warm history; the two are indistinguishable to the
user (both an immediately-correct gain on re-enable), so nothing is lost.

When *no* effect is on and the pre-pass never runs, the history simply holds its last
value until the pass resumes and resets it.

**`dt`** is host-side: `vulkan_composite()` keeps the `get_time_in_nanos()` of the previous
effects dispatch (a function-local static) and passes the difference in seconds as
`u_abDt`, **clamped to 0.25 s**. `Why wall time and not a per-frame constant:` the pass
runs once per *composite*, and a `gamescopectl screenshot` frame composites twice (see
"Exactly once per composite" above), so the measure dispatch runs again a fraction of a
millisecond after the presented one. With wall-time `dt` that second step is worth exactly
that fraction, and the next frame's step is shorter by the same amount — the EMA
integrates elapsed time, so adaptation over any interval is unchanged and the screenshot's
Adaptive Brightness gain is within a sub-millisecond step of the screen's. `Why the
clamp:` a hitch, a pause, or the first frame after a long idle would otherwise slam the
EMA to the new measurement in one step; a quarter-second nudge keeps the transition
smooth. The first dispatch passes `0`.

#### The history texture — persistence as a contract, not luck

`g_output.effectsHistory` is a **4×1 `ABGR8888` storage+sampled texture** (one texel per
statistic — `HISTORY_MEAN`, `_P2`, `_P50`, `_P98`; `kEffectsHistoryTexels` on the host
mirrors the shader's `HISTORY_COUNT`), created once by `update_effects_history()` and kept
for the life of the output (it does not depend on the game's size, and its contents *are*
the effect's state — so, unlike the `.fx`, a resolution change does not reset it). It was
1×1 until Dynamic mode (2026-09-06) needed the percentiles.

**Storage format — RGBA8 bytes, not R32F.** Each float is spread bit-for-bit over the four
8-bit channels of its texel (`history_pack` = `unpackUnorm4x8(floatBitsToUint(v))`, `history_unpack` =
`uintBitsToFloat(packUnorm4x8(t))`, `effects_common.h`). `Why:` `dst` in `descriptor_set.h`
is declared `rgba8` and `CVulkanCmdBuffer::dispatch()` binds one RGB target there; an
`r32f` second-target path through the shared descriptor set, for one texel used by one
pass, was judged more plumbing than four exact byte lanes. `Why it is lossless:`
`unpackUnorm4x8` yields `b/255`; UNORM8 storage rounds `b/255·255` back to `b` (float→UNORM
conversion is round-to-nearest by spec); the fetch returns `b/255` again and `packUnorm4x8`
rounds it back to `b`. Both sides go through the same raw UNORM view, so the format's channel
order cancels. `Why not plain 8-bit:` an EMA step of `alpha·(measured − adapted)` at
`alpha ≈ 0.016` (60 fps, τ = 1 s) is below `1/255` whenever the gap is under 0.25, so the
value would freeze short of its target.

**Discard-safe binding.** `CVulkanCmdBuffer::prepareDestImage()` marks a target it sees for
the first time in a command buffer `discarded`, and `insertBarrier()` then emits
`oldLayout = UNDEFINED`, which *permits the driver to drop the contents*. The `.fx` version
relied on that never actually happening (DECISIONS.md #14: "confirmed empirically on
RADV") — luck, not contract. The native pass binds the history as a **source first**: in the
measure dispatch it is sampler slot `VKR_EFFECTS_HISTORY_SLOT` (= 1) *and* the storage
target, and `dispatch()` runs `prepareSrcImage()` (tracks it with `discarded = false`) before
`prepareDestImage()` (returns early for a tracked image). So the barrier for it is either
nothing (steady state) or `GENERAL → GENERAL`; never `UNDEFINED`. `Why self-sampling is
safe:` only invocation 0 touches the texels, and its fetches precede its stores in program
order; the layout is `GENERAL` for both bindings. The `.fx` did the same.

The one legitimate `UNDEFINED` is the creation frame: the fresh `VkImage` really is in
`UNDEFINED` layout, so `vulkan_composite()` calls `discardImage()` on it *before* the
dispatch (its `emplace` wins, so the later `prepareSrcImage` no-ops), the barrier is
`UNDEFINED → GENERAL`, and `kResetHistory` (`1<<31`) tells the shader to write `measured`
directly rather than blend with undefined bits. The shader additionally snaps a NaN/inf
history to the measurement and clamps to 0..1, so the value can never poison later frames.

**Barrier sequence** (the full write-up is the comment above the measure dispatch in
`rendervulkan.cpp`): (1) measure dispatch — history tracked as source, no `UNDEFINED`;
(2) `dispatch()` ends with `markDirty(history)`; (3) the per-pixel dispatch binds it as a
source again, `insertBarrier()` sees `dirty` and emits `SHADER_WRITE → SHADER_READ`,
`GENERAL → GENERAL`, `ALL_COMMANDS → ALL_COMMANDS` — the read-after-write between the two
dispatches; (4) across frames, that same barrier made frame N's write available to all
later reads on the queue, and the write-after-read (frame N+1's measure overwriting what
frame N's per-pixel pass read) is ordered because `insertBarrier()` records a
`vkCmdPipelineBarrier ALL_COMMANDS → ALL_COMMANDS` unconditionally, even with zero image
barriers, and pipeline barriers order against all earlier work on the queue across command
buffers. Every `vulkan_composite()` runs on the compute queue; nothing else touches the
history.

**No lag, by ordering.** Measure runs before apply on the same command buffer, so the gain a
frame receives is computed from that frame's own measurement (as the `.fx`'s pass order
did). If the history allocation ever fails, the pre-pass still runs with the Adaptive
Brightness flag masked off and the measure dispatch skipped.

After the per-pixel dispatch slot 1 is unbound, so the FSR/NIS/blit dispatches that follow
(which bind layers `0..n-1`) do not carry a stray history descriptor on single-layer frames.

## The settings-panel budget

Each row may own at most six `Param`s before `Registry.cpp` aborts registration —
see `PanelShaders.cpp`'s "THE SIX BUDGET" comment. Counts: Vibrancy 2, Pre-Sharpen 1,
Adaptive Brightness 6 (zero headroom — which is why its mode is the row's own three-way
value rather than a seventh param, see its section), Shadow Control 1.

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
  Shadow Control; `requests-2026-09-06.md` item #16 — Adaptive Brightness's modes.
- `scripts/effects-regression.sh` — the headless measurement gate for both modes.
