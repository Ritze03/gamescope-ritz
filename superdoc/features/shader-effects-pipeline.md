# Shader effects: the pre-pass, uniforms and panel budget

The native pre-pass: where it runs, encoded space, the uniform block, backends, the effect list, the settings-panel budget and diagnostics. Part of [shader-effects](shader-effects.md); sections below are moved verbatim from that page.

## Preview (split screen) (`image.shaders.preview_split`, new 2026-09-14)

A Switch row at the very TOP of the area, above the Effects band — the user's own
request, verbatim: *"Create a feature for the shaders itself, that's called something
like preview mode. It should only apply the shader to the right half of the screen. If
it's simpler for the implementation, it would be fine if the whole image is processed
and only the displaying part differs, so it's mainly for testing and seeing how much of
an effect you can actually achieve."* That second sentence is exactly what shipped: the
pre-pass (measure, Bloom, V2's guided filter, everything) still runs over the **whole**
frame unchanged, and `cs_effects_layer0.comp`'s final store — the very last thing the
shader does, after every effect above it — overwrites only the LEFT half's pixels with
the raw input texel (the slot-0 sample before `grade()`, same gamma-encoded space) when
the switch is on. The right half is whatever the effects below already produce; nothing
about them changes. No marker line is drawn at the split boundary.

`Why display-only, not a second dispatch over half the frame:` the simpler shape the
user explicitly sanctioned, and it has a real property the cheaper one would not: an
adaptive effect's statistics (Adaptive Brightness/Gamma/V2's smoothed percentiles and
content anchor) come from taps over the WHOLE frame, so turning the split on or off
never moves what the picture is adapting to — the right half looks exactly like the
effect running normally, at any moment, because that is what it is.

`Cost when nothing else is on:` the switch is a bare bool on `NativeEffectsState_t`,
deliberately kept OUT of `AnyEnabled()` (`rendervulkan.hpp`) — it must never be what
makes the pre-pass run. With every other effect off, the pass does not dispatch at all
and both halves are the same raw frame (raw|raw), at zero extra cost.

`Not included:` a user's own ReShade `.fx` file (the separate, runtime-compiled loader —
see [reshade-effects](reshade-effects.md)) is unaffected; the split only touches this
native pre-pass's own final store.

## Where frame generation runs relative to the effects

[Frame generation](frame-generation.md) substitutes the raw layer 0 *before* the effects
pre-pass and ReShade, so every effect processes generated frames exactly like real ones.
The adaptive effects therefore measure generated frames too, which is harmless (they
smooth over many frames anyway).

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
cs_effects_measure ──► g_output.effectsHistory  (8×1, persistent; one 16×16 workgroup,
   │                    ▲   reads last frame's value, writes this frame's)
   │                    │
   │  [Bloom, if on: cs_effects_bloom_down ─► bloomA, then blurh A─►B, blurv B─►A,
   │   all at W/8 × H/8 -- see the Bloom section]
   ▼                    │
cs_effects_layer0  ──►  g_output.effectsOutput  (pooled, source res, 8×8 groups)
   │  a private copy of the FrameInfo_t gets layers[0].tex = effectsOutput;
   │  the caller's struct is never written
   ▼
FSR / NIS / blur / blit as before
```

Both dispatches share one `uploadConstants<EffectsPushData_t>()` (same `effects_t` block,
same values); the measure dispatch runs whenever the pre-pass runs at all — see
[Adaptive Brightness](shader-effects-adaptive-brightness.md#adaptive-brightness-imageshadersadaptive_brightness) below.

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
per-tap `grade()` (Shadow Control + Saturation + Vibrancy) and the history pack/unpack
helpers, so the measure pass grades its taps with exactly the code the per-pixel pass
grades its pixels.

| Field | Meaning |
| --- | --- |
| `uint u_flags` | bits: `1<<0` Shadow Control, `1<<1` Saturation, `1<<2` Saturation's protect skin, `1<<3` Pre-Sharpen, `1<<4` Adaptive Brightness, `1<<5` Adaptive Brightness's **Dynamic** mode (else Whole image), `1<<6` Vibrancy (added 2026-09-08), `1<<7` Adaptive Gamma (added 2026-09-08), `1<<8` Bloom (added 2026-09-08 — gates only the composite; the three dispatches that build the glow are simply not recorded when it is clear); `1<<9` was the experimental Brightness Map effect (removed 2026-09-14, left unused rather than reassigned; reused by Adaptive Brightness V2), `1<<10` Preview split, `1<<11` Color Boost (added 2026-10-29), `1<<31` reset history (the history texture was created this frame, or the pre-pass is resuming after a frame in which it did not run — see "Resets on resume" below) |
| `float u_saturation` | 0..3, 1 neutral (renamed from `u_vibrancy` 2026-09-08 — same meaning, see below) |
| `float u_vibrancy` | 0..3, 0 neutral (added 2026-09-08 — the new effect's own strength; unrelated to the field above despite the name) |
| `float u_shadowLift` | 0..1, 0 neutral |
| `float u_colorBoost` | 0..3, 0 neutral (added 2026-10-29; **appended LAST** in both the GLSL block and `EffectsPushData_t`, masked to 0 by the host while the effect is off). The curve exponent is `1 + u_colorBoost` |
| `uint u_rcasCon` | `floatBitsToUint(con.x)` for RCAS, 0 when sharpen is off |
| `float u_abTarget, u_abMin, u_abMax, u_abStrength` | Adaptive Brightness's own parameters, straight from config (both modes read all of them — see the Dynamic section for what each means there) |
| `float u_adaptUp, u_adaptDown` | The EMA's two time constants, seconds — **the ACTIVE adaptive effect's**, not necessarily Adaptive Brightness's (renamed from `u_abUp`/`u_abDown` 2026-09-09, when Adaptive Gamma got its own pair). The two effects cannot both be on, so one EMA serves both and the host picks; clamped to the panel's own 0.1 s floor, and floored again by `ema_alpha()` |
| `float u_abDt` | seconds since the previous effects dispatch, host-measured and clamped (see Adaptive Brightness) |
| `float u_abLocal` | Local adaptation, 0..1. **Masked to 0 by the host in Whole image mode** (`EffectsPushData_t`'s constructor) rather than in the shader, so the uniform says exactly what the frame did — which is what `effects_ab_log` prints |
| `float u_bloomThreshold, u_bloomIntensity, u_bloomRadius` | Bloom's three parameters (added 2026-09-08). Threshold and Radius are read by the two dispatches that build the glow, Intensity only by the composite; all three are masked to their neutral values (1.0 / 0.0 / 0.0) when the effect is off, for the same "the uniform says what the frame did" reason as `u_abLocal` |
| `float u_agTarget, u_agMaxLift, u_agMaxDarken, u_agStrength, u_agLocal` | Adaptive Gamma's five *curve* parameters (added 2026-09-08; its two speeds reach the shader through `u_adaptUp`/`u_adaptDown` above instead, because the EMA is shared). **All masked to their neutral values by the host whenever Adaptive Brightness is also on**, for the same "the uniform says what the frame did" reason as `u_abLocal` — see [the exclusion](shader-effects-adaptive-gamma.md#adaptive-gamma-vs-adaptive-brightness-mutually-exclusive) |

Host state is `g_nativeEffects` (`NativeEffectsState_t`, `src/rendervulkan.hpp`): a plain
struct written by `PanelShaders.cpp` and by `main.cpp`'s startup config apply, read by
`vulkan_composite()` and by the three backends' "needs full composite" decision — all on
the steamcompmgr thread, same discipline as `g_upscaleFilterSharpness`. `Why the startup
apply:` under E2 nothing in `PanelShaders.cpp` runs per frame, so without it saved effects
would only switch on the first time the Shaders area was drawn.

`NativeEffectsState_t::AnyEnabled()` counts every effect switch (Color Boost included), both adaptive effects
and Bloom included, so any one of them forces the full composite the
pre-pass needs. `NeedsStatistics()` is the narrower question — "does anything read the
measure pass's history" — and is true for Adaptive Brightness and Adaptive Gamma alone.
**Bloom moves the first and not the second**, deliberately: its
bright pass gates on each pixel's own luma against a fixed threshold, never on the frame's
statistics, so it neither needs the history warm nor has any reason to keep the measure
dispatch alive.

## Backends

Every backend already forced a full composite for `!g_reshade_effect.empty()`
(`DRMBackend.cpp`, `WaylandBackend.cpp`, `OpenVRBackend.cpp`). Each now also ORs in
`vulkan_native_effects_active()`; without that, direct scanout would skip the pre-pass and
the effects would silently vanish whenever the base layer could be scanned out directly.

## The seven effects (plus Color Boost, 2026-10-29)

(The heading's "seven" is the historical count the section grew from; the per-tap order
below now has an extra colour step, Color Boost, right after Vibrancy.)

Shadow Control, Saturation and Pre-Sharpen's maths is ported 1:1 from the retired `.fx`;
Vibrancy, Adaptive Gamma and Bloom are new (2026-09-08). Applied **per tap, in this
order**: Shadow Control → Saturation → Vibrancy → Color Boost → (Pre-Sharpen) → Bloom (the glow, from
its own three dispatches) → Adaptive Brightness (Whole image's gain, or Dynamic's curve)
→ Adaptive Gamma (one exponent) → `saturate`. The last
two can never both run — see
[Adaptive Gamma vs Adaptive Brightness](shader-effects-adaptive-gamma.md#adaptive-gamma-vs-adaptive-brightness-mutually-exclusive)
— so the order between them is a formality the shader states rather than a behaviour. `Why Vibrancy right after Saturation:` both are colour-
intensity effects and belong next to each other in the pipeline the same way they sit
next to each other in the panel (see the Vibrancy section's own note on Effects-band
ordering); Vibrancy reads the *already-saturated* colour Saturation just produced, the
conventional "adjust overall saturation first, then add a further boost on top" grading
order.

## The settings-panel budget

Each row may own at most **ten** `Param`s before `Registry.cpp` aborts registration —
raised from six 2026-09-06 (request #17, see the
[Adaptive Brightness budget decision](shader-effects-adaptive-brightness.md#the-budget-decision-seven-params-not-six-2026-09-0607)
for the evidence and the why), from seven 2026-09-07 (Local adaptation, below), and from
eight 2026-09-14 (Adaptive Brightness V2's own darkening — Max darken, Darken — see
[the row-budget note](shader-effects-darkening.md) in that effect's
own section). See `PanelShaders.cpp`'s "THE SIX BUDGET" comment and `Registry.cpp`'s
`kParamBudget`. Counts: Saturation 2, Vibrancy 1 (new 2026-09-08), Color Boost 1 (new 2026-10-29), Pre-Sharpen 1, Bloom 3
(new 2026-09-08), Adaptive Brightness 8 (zero headroom of the OLD 8-ceiling; two spare
now), Adaptive Gamma 7 (5 on 2026-09-08, plus its own two adaptation speeds 2026-09-09),
Adaptive Brightness V2 **10** (zero headroom, raised the same day it hit the old ceiling
— see [its own page](shader-effects-adaptive-brightness-v2.md)), Shadow Control
1. **The budget was not raised again** for Adaptive Gamma, for Bloom, or for Adaptive
Gamma's speeds, and did not need to be — see each effect's own section for why five and three params are their honest
counts rather than a squeeze.

**The second raise, 7 → 8 (2026-09-07), and the debt it books.** The note left after the
first raise said the next param was the signal to **promote** Adaptive Brightness to its own
rail category, not to raise the number again. Local adaptation is that param, and the number
was raised anyway — knowingly, not by oversight. The reasoning, so the next author can
disagree with it on the record: the promotion is still the right end state, but it moves the
effect out of "Shaders" where every capture, keyword and doc link currently points at it, and
it is a shell-layout change whose risk has nothing to do with the tone curve this request was
about — landing both together would make one hard-to-judge diff out of two easy ones. The
measured cost of the raise itself is one more Inspector row: verified by capture at 2560×1440
(`build-release/verify-shots/adaptive-local-2026-09-07/inspector-8-params.png`) with the
header correctly reading **"PARAMETERS 8 of 8"** and well over half the panel empty below the
rows; `test_overlay_shell.cpp` still shows the same row set scrolling at 2.0×, as it did at
six and at seven. **The promotion is now owed work** (`requests-2026-09-08.md`), and Adaptive
Brightness is the only row in the whole registry above two params — this constant exists for
it alone, which is exactly why promoting it, rather than raising it a third time, is what
happens next.

**The third raise, 8 → 10 (2026-09-14), and why it is not the same debt.** The warning
just above is specifically about Adaptive Brightness raising this constant a third time
for its OWN growth — it still stands, and Adaptive Brightness has not asked for a ninth
param. Adaptive Brightness V2's darkening feature (see that section) needed two more
params on a DIFFERENT row that had just reached the old ceiling the same day Stage 3
did. Checked, not assumed: `test_overlay_shell.cpp`'s "an entry at the six-param budget
overflows the drawer at 2.0x" reads `ui::ParamBudget()` live rather than a literal, so it
keeps proving the Inspector's Configure body scrolls past whatever the budget is, at 10
with no test edit needed — a taller row costs a scrollbar, not a layout break, at every
scale already pinned by that test. Raising was still weighed against folding an existing
V2 param (the way Adaptive Brightness's own history has always preferred); see
[the row-budget note](shader-effects-darkening.md) for why no fold
was honest this time either.

## Diagnostics

The **Pipeline** Facts row keeps "base layer" (the SDR gate) and says the effects are built
into the binary. The "effect file" / "compiled" / "loaded from" / "uniforms" rows added
2026-09-05 for the stale-file case were removed the same day along with the failure they
diagnosed.

**`effects_ab_log [frames] [x y]`** (ConCommand, `rendervulkan.cpp`, 2026-09-07): prints
Adaptive Brightness's state for the next N composites, one line each on `console_log`
(so `gamescopectl effects_ab_log 300` shows it live, and it lands in gamescope's log):

```
ab_log n=<i> t=<ms> dt=<ms> <off|whole|dynamic|gamma> raw mean=… p2=… p50=… p98=…
       smooth mean=… p2=… p50=… p98=… gain=… gamma=… px(<x>,<y>)=<r>,<g>,<b>
       local=… lmin=… lmax=… lprobe=… gainlo=… gainhi=… bind=<n> (<text>)
       tau=<up>/<down>
```

`tau` (2026-09-09) is the EMA's two time constants **as the frame actually used them** —
the active effect's, which since Adaptive Gamma got [its own pair](shader-effects-adaptive-gamma.md#adaptation-speed-2026-09-09--adaptive-gammas-own-pair)
is no longer always Adaptive Brightness's. Appended at the end, so a parser written
against an older line still matches.

`raw` is this frame's measurement, `smooth` the history the pixel pass read, `gain` and
`gamma` are recomputed on the host with the same `effects_curve.h` the shader compiles,
and `px` is one pixel of the graded output (`effectsOutput`) before scaling — the probe
defaults to the centre of the game image. The `local…` fields (2026-09-07) are Local
adaptation's: the strength in force, the darkest and brightest cell of the smoothed 16×16
map, the cell the probe pixel falls in, and the two **extreme per-pixel gains** the map
produces this frame (`gainlo` from `lmin`, `gainhi` from `lmax`) — which is how "the
operator is spreading the gain 3.60 … 0.476 across this frame" becomes a number instead of
an impression. `gain`/`gamma` are the probe cell's. `How:` the pre-pass block copies the history
and the probe pixel into two host-mappable staging textures (`effectsDebugHistory`,
`effectsDebugPixel`, created on first use) and `vulkan_composite()` waits for that
submit right after it, so every logged composite stalls the render thread by one GPU
frame — which is why it is a bounded count, not a switch; `frames` defaults to 1. The
history's state is on the GPU and used to be unobservable except through screenshots at
a few frames per second; this is what made the pulse measurable rather than argued
about. `scripts/effects-regression.sh` drives it for its per-frame checks
(`effects_regression_sample.py ablog`).

## History

- **Preview split** (terminology entry, moved here 2026-11-07): **Preview split** — the **Shaders** area's `image.shaders.preview_split` switch (added 2026-09-14, top row, above the Effects band), the user's own testing aid: *"It should only apply the shader to the right half of the screen ... so you can judge what an effect really does."* Display-only: every effect still processes the WHOLE frame (an adaptive effect's statistics are never disturbed), and `cs_effects_layer0.comp`'s final store overwrites only the left half with the raw input texel. Deliberately excluded from `NativeEffectsState_t::AnyEnabled()`, so with every other effect off it shows raw|raw at zero cost rather than forcing the pre-pass on by itself. Does not apply to a user's own ReShade `.fx` file.
