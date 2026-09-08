# Shaders settings area — Saturation, Vibrancy, Shadow Control, Pre-Sharpen, Bloom, Adaptive Brightness, Adaptive Gamma

The overlay's **Shaders** area (`image.shaders`, `src/Overlay/PanelShaders.cpp`) exposes
seven effects — six independent, plus **Adaptive Gamma** (new 2026-09-08), which is
mutually exclusive with Adaptive Brightness and with nothing else. Since 2026-09-05 they are **one native compute pre-pass compiled
into the binary at build time** — `src/shaders/cs_effects_layer0.comp`, dispatched from
`vulkan_composite()` (`src/rendervulkan.cpp`) on the base/game layer at source resolution
before any scaling. **Bloom** (new 2026-09-08) is the one effect that needs more than
that one dispatch — it is spatial, so it brings three of its own; see
[its section](#bloom-imageshadersbloom--new-2026-09-08). They used to be gated passes
inside a runtime-compiled ReShade file, `reshade/Shaders/gamescope-ritz.fx`; that file,
and the install step that copied it, are gone (`superdoc/planning/DECISIONS.md` #27).
The ReShade loader itself stays, for users' own `.fx` files — see
[reshade-effects](reshade-effects.md), which is now third-party-only.

**Titles vs. identifiers.** The three multi-word switches were retitled 2026-09-05:
"Shadow lift" → **Shadow Control**, "Adaptive brightness" → **Adaptive Brightness**,
"Pre-sharpen" → **Pre-Sharpen**. `Why only the titles moved:` an entry id is what the
command palette's saved entries and every cross-reference resolve against, and a config
key is what a user's `global.json` already contains — renaming either would break
existing configs and palette state for a purely cosmetic change. So the id
`image.shaders.shadow_lift` and the config struct `ReshadeShadowLiftSettings` keep the
old spelling. Expect the code and this page to say "shadow lift" where it means the
identifier and "Shadow Control" where it means the label.

**The Saturation / Vibrancy split (2026-09-08).** This one *did* move the id and the
config key, deliberately — see that section below for why a rename this time was
worth breaking the usual rule. What used to be the only "Vibrancy" effect is now
**Saturation** (`image.shaders.saturation`, `ReshadeSaturationSettings`), and
**Vibrancy** (`image.shaders.vibrancy`, `ReshadeVibrancySettings`) names a brand new,
second effect built beside it. Expect an old commit, an old capture, or an old test
name in this doc's history to say "Vibrancy" and mean what is now Saturation.

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
per-tap `grade()` (Shadow Control + Saturation + Vibrancy) and the history pack/unpack
helpers, so the measure pass grades its taps with exactly the code the per-pixel pass
grades its pixels.

| Field | Meaning |
| --- | --- |
| `uint u_flags` | bits: `1<<0` Shadow Control, `1<<1` Saturation, `1<<2` Saturation's protect skin, `1<<3` Pre-Sharpen, `1<<4` Adaptive Brightness, `1<<5` Adaptive Brightness's **Dynamic** mode (else Whole image), `1<<6` Vibrancy (added 2026-09-08), `1<<7` Adaptive Gamma (added 2026-09-08), `1<<8` Bloom (added 2026-09-08 — gates only the composite; the three dispatches that build the glow are simply not recorded when it is clear), `1<<31` reset history (the history texture was created this frame, or the pre-pass is resuming after a frame in which it did not run — see "Resets on resume" below) |
| `float u_saturation` | 0..3, 1 neutral (renamed from `u_vibrancy` 2026-09-08 — same meaning, see below) |
| `float u_vibrancy` | 0..2, 0 neutral (added 2026-09-08 — the new effect's own strength; unrelated to the field above despite the name) |
| `float u_shadowLift` | 0..1, 0 neutral |
| `uint u_rcasCon` | `floatBitsToUint(con.x)` for RCAS, 0 when sharpen is off |
| `float u_abTarget, u_abUp, u_abDown, u_abMin, u_abMax, u_abStrength` | Adaptive Brightness's six original parameters, straight from config (both modes read all six — see the Dynamic section for what each means there) |
| `float u_abDt` | seconds since the previous effects dispatch, host-measured and clamped (see Adaptive Brightness) |
| `float u_abLocal` | Local adaptation, 0..1. **Masked to 0 by the host in Whole image mode** (`EffectsPushData_t`'s constructor) rather than in the shader, so the uniform says exactly what the frame did — which is what `effects_ab_log` prints |
| `float u_bloomThreshold, u_bloomIntensity, u_bloomRadius` | Bloom's three parameters (added 2026-09-08). Threshold and Radius are read by the two dispatches that build the glow, Intensity only by the composite; all three are masked to their neutral values (1.0 / 0.0 / 0.0) when the effect is off, for the same "the uniform says what the frame did" reason as `u_abLocal` |
| `float u_agTarget, u_agMaxLift, u_agMaxDarken, u_agStrength, u_agLocal` | Adaptive Gamma's five parameters (added 2026-09-08). **All masked to their neutral values by the host whenever Adaptive Brightness is also on**, for the same "the uniform says what the frame did" reason as `u_abLocal` — see [the exclusion](#adaptive-gamma-vs-adaptive-brightness-mutually-exclusive) |

Host state is `g_nativeEffects` (`NativeEffectsState_t`, `src/rendervulkan.hpp`): a plain
struct written by `PanelShaders.cpp` and by `main.cpp`'s startup config apply, read by
`vulkan_composite()` and by the three backends' "needs full composite" decision — all on
the steamcompmgr thread, same discipline as `g_upscaleFilterSharpness`. `Why the startup
apply:` under E2 nothing in `PanelShaders.cpp` runs per frame, so without it saved effects
would only switch on the first time the Shaders area was drawn.

`NativeEffectsState_t::AnyEnabled()` counts all seven switches, both adaptive effects
and Bloom included, so any one of them forces the full composite the pre-pass needs.
`NeedsStatistics()` is the narrower question — "does anything read the measure pass's
history" — and is true for Adaptive Brightness and Adaptive Gamma alone. **Bloom moves
the first and not the second**, deliberately: its bright pass gates on each pixel's own
luma against a fixed threshold, never on the frame's statistics, so it neither needs the
history warm nor has any reason to keep the measure dispatch alive.

## Backends

Every backend already forced a full composite for `!g_reshade_effect.empty()`
(`DRMBackend.cpp`, `WaylandBackend.cpp`, `OpenVRBackend.cpp`). Each now also ORs in
`vulkan_native_effects_active()`; without that, direct scanout would skip the pre-pass and
the effects would silently vanish whenever the base layer could be scanned out directly.

## The seven effects

Shadow Control, Saturation and Pre-Sharpen's maths is ported 1:1 from the retired `.fx`;
Vibrancy, Adaptive Gamma and Bloom are new (2026-09-08). Applied **per tap, in this
order**: Shadow Control → Saturation → Vibrancy → (Pre-Sharpen) → Bloom (the glow, from
its own three dispatches) → Adaptive Brightness (Whole image's gain, or Dynamic's curve)
→ Adaptive Gamma (one exponent) → `saturate`. The last
two can never both run — see
[Adaptive Gamma vs Adaptive Brightness](#adaptive-gamma-vs-adaptive-brightness-mutually-exclusive)
— so the order between them is a formality the shader states rather than a behaviour. `Why Vibrancy right after Saturation:` both are colour-
intensity effects and belong next to each other in the pipeline the same way they sit
next to each other in the panel (see the Vibrancy section's own note on Effects-band
ordering); Vibrancy reads the *already-saturated* colour Saturation just produced, the
conventional "adjust overall saturation first, then add a further boost on top" grading
order.

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

### Saturation (`image.shaders.saturation`) — renamed from "Vibrancy" 2026-09-08

Adaptive saturation with an optional skin-tone damper. **Renamed from "Vibrancy"
2026-09-08** — the user's own observation: this effect is a flat multiplier, the same
relative boost applied to every pixel regardless of how saturated it already is, which
is what an iPhone "Saturation" slider does, not that app's "Vibrancy". The maths below
is **byte-for-byte unchanged** by the rename — only the id, the config key and the
label moved. See [the Vibrancy section](#vibrancy-imageshadersvibrancy--new-2026-09-08)
below for the new effect that took the freed-up "Vibrancy" name, and
["Verified: the rename changed nothing"](#verified-the-rename-changed-nothing-2026-09-08)
for the measurement that backs "byte-for-byte" up.

**Config**: `ReshadeSaturationSettings` — `enabled`, `strength` (float),
`protect_skin_tones` (default true). (`ReshadeVibrancySettings` is now a *different*
struct — the new effect's, below.)

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
(`requests-2026-09-04.md` #2). Note that `boost`'s own `(1.0 - saturation)` term means
that *above* neutral (`strength` > 1), this effect already leans muted-first — an
already-saturated pixel picks up less of the extra boost than a duller one does; only
the `mix`-only regime (`strength` <= 1) is a genuinely flat, saturation-independent
multiplier. See the measured ratio table below.

#### Migration: an existing config's old value, twice

`kCurrentSchemaVersion` 1 → 2; `Migrate_1_to_2()` (`src/Config/ConfigManager.cpp`)
transforms `reshade.vibrancy.strength` once, on load: `new = clamp(old + 1.0, 0.0, 3.0)`.
`Why +1.0:` it carries old-neutral (0.0) onto new-neutral (1.0), so an untouched config
does not open in black and white; a customised value keeps its displacement from neutral.
A config saved under schema 2 round-trips unmigrated — `tests/test_config.cpp`.

`kCurrentSchemaVersion` 3 → 4 (2026-09-08): `Migrate_3_to_4()` renames the JSON object
`reshade.vibrancy` to `reshade.saturation` in place, once, on load — a pure key rename,
no value transform (the 1→2 step already handled the value's meaning; this step only
runs *after* it, so an ancient schema-0/1 file gets both in the right order). `Why not
just leave the old key readable too:` the new "Vibrancy" effect below needed the
`reshade.vibrancy` key for itself, so an old file's `reshade.vibrancy` object had to
stop meaning the old effect **before** it could start meaning the new one, or an old
config would silently load as the wrong effect (a saturation multiplier reread as a
punch-boost strength — wrong shape, wrong scale, no crash to notice it by). The general
policy this follows: **migrate on load, not "just ignore the old key"** — an old file's
saved value is real user data (their `strength`, their `protect_skin_tones`), and
dropping it silently would be a bigger surprise than a one-line rename. Saving after
load naturally rewrites the file under the new key (`SectionsToJson()` only ever emits
the current field names), so an old profile self-heals the first time anything saves it
— it does not need to be touched by hand. `tests/test_config.cpp`'s "a schema-3
profile's vibrancy key renames to saturation on load and rewrites on save" pins this;
so does the fact that this session's own real `~/.config/gamescope-ritz/profiles/*.json`
files were rewritten this way (by hand, with the user's explicit permission, since only
the key needed to change and a full save-through-the-app round trip risked touching
unrelated formatting) — see the CHANGELOG.md Info entry and this doc's own history.

### Vibrancy (`image.shaders.vibrancy`) — NEW 2026-09-08

The user's request, verbatim: *"Our current 'Vibrancy' behaves like 'Saturation' when
editing images on an iphone. The color highlighter should make already punchier colors
even more punchy. So it should behave more like the 'Vibrancy' option on an iphone."*
Built exactly as described: this effect's boost **rises with a pixel's own existing
saturation** — a near-neutral pixel is left close to untouched, and an already-punchy
pixel is pushed further.

**Discrepancy, flagged rather than silently resolved:** on Apple's own Photos app, the
control actually named *Vibrance* conventionally does the **opposite** of what is built
here — it boosts *muted* colours more and *protects* already-saturated ones (skin tones
in particular), which is precisely the shape [Saturation](#saturation-imageshaderssaturation--renamed-from-vibrancy-2026-09-08)'s
`boost` term already had, above neutral, *before* this rename. So the user's stated
definition of "Vibrancy" is the inverse of the word's usual photo-editing meaning. This
doc builds exactly what was asked — more boost the more saturated a colour already is —
and says so plainly rather than "correcting" it to match Apple's convention. If the
direction ever feels backwards in practice, that is the thing to revisit; the maths
below is not a misunderstanding, it is the literal spec.

**Config**: `ReshadeVibrancySettings` — `enabled` (default false), `strength` (float,
0.0..2.0, default 0.0/neutral). One param — no skin-tone toggle: skin tones sit at a
moderate, not extreme, saturation, so this effect already gives them a moderate rather
than maximal boost on its own; adding a second toggle to suppress an effect this mild on
skin was not worth a second control (see the panel's own comment for the same point).

```
sat  = max(c.r, c.g, c.b) - min(c.r, c.g, c.b)   // 0..1, the pixel's own chroma
gain = 1.0 + strength * sat                       // >= 1.0 always
out  = clamp(luma + (c - luma) * gain, 0.0, 1.0)
```

`Why this shape:` `gain` is `1.0` (identity) at `sat = 0` regardless of `strength` — a
grey pixel has `c == luma` exactly, so it is an *exact* no-op there, not an
approximation, at every strength. `gain` grows linearly with the pixel's own saturation,
so the punchiest colours (`sat` near 1) get pushed by up to `1 + strength`, while a
barely-tinted colour (`sat` near 0) gets almost no push — "already punchier colors even
more punchy" stated as a formula. `Why linear in `sat`, not `sat²` or a curve:` this is
the cheapest shape that satisfies the request (one multiply, one madd inside a function
already computing `luma` and `sat` for other reasons — see `effects_common.h`'s
`grade()`), it is monotonic by construction, and the measured table below shows it
already produces a clearly increasing boost across four bands without needing a steeper
curve. `Why `1.0 + strength * sat` and not, say, `strength ^ sat`:` the additive-gain
form keeps `strength = 0` an exact identity at every saturation (multiplying by `0^sat`
would not), which is what "0 is off" has to mean for a switch's paired param.
`Why the final clamp, and why it can slightly desaturate the punchiest colours:` the
same reason every other effect in `grade()` ends on `clamp(..., 0.0, 1.0)` — `c - luma`
scaled by a `gain` > 1 can overshoot 0 or 1 on a channel that started near the gamut
edge, and clamping is what "never wraps hue" means in practice: the *direction* away
from `luma` is preserved exactly (each channel's sign never flips), only the *magnitude*
is capped. A pixel that is already at the sRGB gamut boundary for its hue (one channel
at 0, one at 255 — `sat = 1.0` exactly) is *already as saturated as an 8-bit encoding can
represent*, so no `strength` can push its measured chroma any further; this is not a bug,
it is the ceiling every colour effect in this pipeline runs into eventually.

**Why `0.0..2.0`, not `0.0..3.0` like Saturation's range:** at `strength = 2.0` the most
saturated pixels get triple their original chroma before clamping (`gain = 3`), already
enough headroom to push a moderately-saturated colour hard; `strength` is additive on top
of the always-present `1.0`, unlike Saturation's `0.0..3.0` multiplier which has to reach
all the way down to `0.0` (full grey) as one of its endpoints. `0.0` here is simply "off",
not a second special value to reach.

**Where it sits in the Effects band, and why:** registered immediately after Saturation
(`PanelShaders.cpp`), so the two colour-intensity effects read together — a user
comparing "the old slider" against "the new one" finds them adjacent rather than
scattered among Shadow Control / Pre-Sharpen / Adaptive Brightness. The Effects band went
from 4 switch rows to **5**; this row's own Inspector column has **1** param (`strength`),
nowhere near `kParamBudget`'s 8 (Adaptive Brightness still owns that ceiling, unchanged).

#### Verified: the rename changed nothing, and the new effect works as designed (2026-09-08)

Measured with `scripts/effects-regression.sh`'s `colors` scene (added for this change:
`tests/effects_scene_client.c`'s `kColorBands`, five horizontal RGB bands instead of the
grey levels every other scene there uses — grey has `sat = 0`, so it is an exact no-op
for both colour effects and could never have tested either one). Every scene capture
sampled and checked against the closed-form formulas above by
`scripts/effects_regression_sample.py`'s `colorcheck`/`colorshape` subcommands — not eyeballed:

| band (input RGB, `sat`) | off | Saturation 1.0 (unchanged) | Saturation 2.0 | Vibrancy 0.5 | Vibrancy 1.0 |
| --- | --- | --- | --- | --- | --- |
| 0: (128,128,128), 0.000 | (128,128,128) | (128,128,128) | (128,128,128) | (128,128,128) | (128,128,128) |
| 1: (148,134,120), 0.110 | (148,134,120) | (148,134,120) | (158,132,105) | (149,134,119) | (149,134,118) |
| 2: (178,140,92), 0.337 | (178,140,92) | (178,140,92) | (199,136,56) | (183,139,83) | (189,138,74) |
| 3: (214,118,54), 0.627 | (214,118,54) | (214,118,54) | (242,110,22) | (237,111,27) | (255,105,0) |
| 4: (255,60,0), 1.000 | (255,60,0) | (255,60,0) | (255,60,0) | (255,34,0) | (255,9,0) |

Every one of those 25 cells matched its formula's prediction to within capture rounding
(worst deviation 0.5 of 255 counts, `colors-saturation-*`/`colors-vibrancy-*`, all PASS)
— **Saturation at 1.0 reproduces the input exactly** (the identity case, proving the
rename moved nothing), and the pure-grey band (0) is untouched by either effect at every
strength tested, including Vibrancy at 2.0 (not shown above) — confirming the "grey stays
grey" invariant both formulas share.

**The headline shape check** (`colors-shape`, PASS) makes the qualitative difference a
number rather than an impression: captured saturation ÷ input saturation, per band —

| | band 1 | band 2 | band 3 | band 4 |
| --- | --- | --- | --- | --- |
| Saturation @ 0.5 (pure `mix`, no `boost`) | 0.50 | 0.50 | 0.50 | 0.50 |
| Vibrancy @ 1.0 | 1.11 | 1.34 | 1.59 | 1.00 |

Saturation's ratio is **exactly flat** (spread 0.00) — the same relative change for
every pixel regardless of its own saturation, which is the literal meaning of "behaves
like Saturation." Vibrancy's ratio **strictly increases** across bands 1-3 — the more
saturated the input, the larger the relative boost — before band 4 lands back at 1.00,
which is the gamut-clamp ceiling described above, not a break in the trend: band 4's
input, `(255, 60, 0)`, already has a channel at each end of the 0..255 range, so its
chroma cannot be measured any higher than 255 no matter how large `gain` gets. (Note the
comparison deliberately uses Saturation at `0.5`, its pure-multiplier `mix`-only regime,
rather than a strength above neutral — see the formula section above for why `boost`
would make even the *old, renamed* effect's own ratio non-flat there, which would have
muddied exactly the comparison this check exists to make.)

Captures: `build-release/verify-shots/vibrancy-split-2026-09-08/effects-regression-captures/10-colors-*.png`;
full numeric results: that directory's `results.txt`.

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

### Bloom (`image.shaders.bloom`) — NEW 2026-09-08

The user's request, verbatim: *"Add a bloom shader for more casual games"*. A glow
around bright areas — aimed at looking good rather than at competitive clarity, so it
ships **off** and its help text says what it is for.

**Config**: `ReshadeBloomSettings` (`ConfigSchema.h`) — `enabled` (false), `threshold`
(0.75), `intensity` (0.8), `radius` (0.5). Purely additive keys, so
`kCurrentSchemaVersion` stays **4** and there is no migration: an old profile has none
of them and takes these compiled-in defaults, exactly the shape
[Shadow Control](#shadow-control-imageshadersshadow_lift) and
[Adaptive Gamma](#adaptive-gamma-imageshadersadaptive_gamma--new-2026-09-08) were added
in.

**This is the first effect here that is not a per-pixel function.** Every other one is
`f(pixel)`; bloom needs to know what is *near* a pixel, which is why it is the only one
that adds dispatches and memory rather than a flag bit and a uniform.

#### The pipeline: three extra dispatches, one ping-pong pair

```
layer0.tex (game, source res)
   │
   ├─► cs_effects_bloom_down.comp   ─► bloomA   (W/8 × H/8)
   │      grade() + bright pass per tap, 8×8 box mean per output texel
   │
   ├─► cs_effects_bloom_blurh.comp  bloomA ─► bloomB   (17-tap Gaussian, x)
   ├─► cs_effects_bloom_blurv.comp  bloomB ─► bloomA   (17-tap Gaussian, y)
   │
   ▼
cs_effects_layer0.comp  ── samples bloomA bilinearly, composites (below)
```

- **Dispatches added: 3.** They are recorded only on frames Bloom is on; `bBloom` gates
  the recording itself, not just a uniform.
- **Memory added: two `ABGR8888` textures at ⌈W/8⌉ × ⌈H/8⌉.** At 1920×1080 that is
  240×135×4 = 130 KB each, **259 KB** for the pair; at 2560×1440, 461 KB. Pooled like
  `effectsOutput`: re-created only when the base layer's source size changes, never
  freed.
- **Work added, in taps.** The bright pass reads every source pixel exactly once (an 8×8
  box, 64 `texelFetch`es per glow texel), so it roughly doubles the pre-pass's
  full-resolution texture reads. The two blur passes are 17 taps each over 1/64 of the
  frame's pixels — together about **0.53 taps per source pixel**. The composite adds
  four cached fetches of a tiny texture and three `pow`s per pixel.
- **`Why 1/8 and not 1/2 or 1/4:`** the blur is what costs, and it costs per glow texel,
  so each doubling of the reduction takes three quarters off both blur passes. A glow has
  no high frequencies to lose, which is what makes it the one effect here that can be
  computed small. 8 is the largest reduction that still leaves the falloff smooth: the
  buffer's texels are already an 8-pixel box, the smallest σ the slider can ask for is
  one texel, and the bilinear upsample interpolates between values that differ by less
  than the eye resolves.
- **`Why the bright pass still reads every source pixel:`** a sparse sampling would make
  a small bright object appear and disappear as the grid slid over it under camera
  motion — the shimmer the threshold's own shape exists to avoid, reintroduced at a
  different scale.
- **`Why separable, and why two shaders instead of one with a direction uniform:`** a
  17×17 two-dimensional kernel is 289 taps per texel against 34 for two passes. The
  direction cannot be a uniform because `effects_t` is uploaded **once** per frame and
  read by every dispatch of the pre-pass; making it a uniform would mean three uploads a
  frame and a field whose value differs between dispatches documented to share one. Two
  three-line files including `effects_bloom_blur.h` is the shape
  `cs_composite_blur.comp` / `cs_gaussian_blur_horizontal.comp` already uses.
- **`Why 8-bit glow buffers:`** `dst` in `descriptor_set.h` is declared `rgba8` and
  `dispatch()` binds one RGB target, the same constraint the history texture works
  under. What it costs: a contribution below 1/255 quantises to zero — in practice a
  single isolated bright pixel inside an 8×8 block, whose glow would be invisible anyway.
  Above that, the bilinear upsample interpolates *between* quantised texels, so the
  falloff the user sees is continuous rather than stepped and there is no banding to
  trade against.

**No GPU-timestamp instrumentation exists in `vulkan_composite()`**, so there is no
measured microsecond figure for any of this and none is claimed — the same statement
Local adaptation's cost section makes. What can be said from the tap counts is that the
added work is one extra full-resolution read of the base layer plus about half a tap per
source pixel, on a pre-pass that is already sub-millisecond on this desktop's GPU — so
the figure below is an **estimate from the tap counts, not a measurement**, and is
labelled as one rather than invented. Nothing in the headless gate's wall-clock timing
can substitute for it: that run is dominated by its own sleeps.

#### Where it sits in the order, and why

**After Pre-Sharpen, before both adaptive effects**:

> Shadow Control → Saturation → Vibrancy → (Pre-Sharpen) → **Bloom** → Adaptive
> Brightness *or* Adaptive Gamma → `saturate`

- **After `grade()`**, so the glow is built from the same graded colour the picture
  carries — a saturation-boosted red lamp glows the boosted red. Gating on the *ungraded*
  luma would also make the Threshold slider mean a different brightness than the one on
  screen, because Shadow Control moves luma.
- **After Pre-Sharpen**, but the glow's own source is *not* sharpened: a 5-tap cross is
  invisible after an 8× box reduction, which is the same argument
  `cs_effects_preview.comp` and the measure pass already make for skipping it.
- **Before the adaptive block**, for two reasons that are about this pipeline rather than
  about the textbook. First, **there is no feedback**: `cs_effects_measure.comp` reads the
  *raw* base layer and grades its taps with `grade()` alone, so the statistics never see
  the glow and a bloomed frame can never turn its own bloom down. Second, the adaptive
  operators then treat the bloomed picture as *the* picture — the glow scales with the
  exposure they choose instead of floating on top at a fixed level, and Dynamic's shoulder
  rolls the added highlights off for free.
- **On the textbook.** Bloom conventionally happens before a tone curve, and it does here:
  the tone curve is the adaptive block. The other half of the convention — "in linear
  HDR" — this pipeline does not have, by DECISIONS.md #15 and the encoded-space decision,
  and that is exactly what shapes the bright pass below. There is nothing above 1.0 to
  use as a natural "this is emissive" line.

#### The bright pass: why the threshold is not a gate

In an HDR renderer a bloom threshold has an unambiguous meaning — only values above 1.0
emit, and an ordinary lit surface never reaches one. Here every pixel is already inside
[0, 1], so a plain "above this, glow fully" gate makes a bright wall emit exactly as hard
as a lamp, and a scene that is mostly bright turns into uniform haze. Arithmetically, at
the shipped threshold of 0.75, a 200-code surface contributes **2 %** of its colour under
the weight below and would contribute **100 %** under a gate whose knee ended beneath it.
The measured consequence of the shipped weight is in the tables further down: the `bright`
scene's 30-code shadow rectangles read **48** at the defaults rather than being washed most
of the way up to the bands around them.

```
x = clamp( (luma - threshold) / max(1 - threshold, 0.001), 0, 1 )
w = x * x
contribution = colour * w            // per tap, before the 8x8 box mean
```

- `w` is **zero and flat at the threshold**. That is where the shimmer question is
  decided: a pixel wandering across the boundary under a pan must not change its
  contribution abruptly. A hard gate makes that first step 1.0 and a linear ramp makes it
  `1/(255·headroom)`; the square makes it at most a **twentieth** of the linear ramp's
  anywhere in range. That factor is the anti-shimmer margin and is what
  `tests/test_effects_curve.cpp` asserts, rather than an absolute number that would drift
  with the threshold.
- `w` still **rises right up to white** — no plateau. A smoothstep would be flat at both
  ends, which would make a 250-code pixel and a 255-code one emit identically and throw
  away the "brighter things glow more" the effect is about.
- **Gate per tap, average after.** Gating the block *mean* would make a single 250-code
  lamp inside an otherwise black 8×8 block fall under any useful threshold and vanish,
  while a uniformly mid-grey block sailed over it.
- **At threshold 1.0 nothing glows, at any luma** — the right meaning for the top of that
  slider, and what the guarded divide produces rather than a NaN.

#### The composite: how it is kept from clipping

Adding light to bright pixels is the one operation in this pipeline that naturally blows
highlights out. It is prevented by the *shape of the operator*, not by a clamp afterwards:

```
out = 1 - (1 - base) * (1 - glow)^intensity          // effects_curve.h's bloom_apply
```

- At `intensity = 1` this is exactly the familiar screen, `base + glow(1 - base)`.
- For `base, glow ∈ [0, 1]` and any `intensity ≥ 0` the result is in `[base, 1]`, and it
  is **strictly** below 1.0 wherever `base` was — so a bright source below 255 stays
  below 255, at every setting of every knob.
- It is **linear in intensity where the glow is faint** — `(1-g)^k ≈ 1 - kg` — so on a
  dark field with a small light, the regime the effect actually lives in, the slider
  behaves like a plain gain. It saturates instead of clipping only where the glow is
  strong.
- `intensity = 0` is an exact identity for every glow, and `glow = 0` an exact identity
  for every intensity.

**`Why not the obvious screen(base, glow × intensity)` — it clips.** That was built
first. Above intensity 1 the product runs past 1.0, the clamp that has to follow pins it
there, and every neighbourhood whose blurred glow exceeds `1/intensity` goes to pure
white. Quantified from the shipped form's own captures rather than from a second gate run
— the blurred glow at each sampled region is recovered by inverting `bloom_apply` on the
measured output, then both composites are evaluated on it:

| `bright` scene region | recovered glow | shipped `(1-g)^k` | `screen(base, g×k)` |
| --- | --- | --- | --- |
| 245 band, Intensity 2.0 | 0.684 | **254** | **255 — clipped** |
| 200 band, Threshold 0 + Intensity 2.0 | 0.484 | 240 | 253 |
| 215 band, Threshold 0 + Intensity 2.0 | 0.606 | 249 | **255 — clipped** |

So on the brightest scene the multiply form merges **four** of the five bands onto white
where the shipped form merges two, and it does so one whole slider position earlier.
Moving the intensity inside the exponent removes the clamp entirely.

`tests/test_effects_curve.cpp` asserts the property exhaustively over all 65,536 8-bit
`(base, glow)` pairs at six points on the Intensity slider — in range, never darkening,
monotone in the glow, in the base and in the intensity, and strictly below white wherever
the picture was. The gate measures the same thing on real captures
(`bloom-noclip-*`, plus Adaptive Gamma's stricter `noclip-*-bloom-default`).

#### The params, and the row count

Three: **Threshold** (0..1, 0.75), **Intensity** (0..2, 0.8), **Radius** (0..1, 0.5) —
"what glows", "how much", "how far". Against `kParamBudget`'s 8 that is comfortable, and
deliberately so; the two candidates for a fourth were weighed and rejected:

- **A knee / falloff shape.** There is no separate knee to expose — the contribution is
  already a smooth function of how far above the threshold a pixel is — and the only
  thing a control there could do is make it *harder*, which is the setting that shimmers.
- **A glow colour / tint.** The glow is built from `grade()`'s own output, so it already
  carries the picture's colour; a tint would be a second, contradicting answer to a
  question Saturation and Vibrancy already own.

Radius maps to the separable blur's σ as `1 + 2·radius` **glow-buffer texels**, i.e.
**8..24 source pixels**, so a nominal 3σ reach of 24..72 pixels. Measured on the
reference source below — where the glow's own amplitude is only about 15 counts, so it
disappears into the field well before 3σ — the reach is **15 / 28 / 43 px** at Radius
0 / 0.5 / 1. `Why the floor is 1 and not 0:` the buffer is an 8×8 box average, so its
own texels are already an 8-pixel-wide feature; a σ below one texel would leave that box
structure visible as blocking after the bilinear upsample instead of a smooth falloff.

**The Effects band went from 6 switch rows to 7** (Saturation, Vibrancy, Pre-Sharpen,
Bloom, Shadow Control, Adaptive Brightness, Adaptive Gamma), and the area's summary now
reads `n of 7 effects on`. Bloom is registered immediately after Pre-Sharpen because
those two are the only **spatial** effects in the band — the only ones that read a
pixel's neighbours — and because the pipeline runs Pre-Sharpen and then Bloom, so the
pair reads in pipeline order in the panel too. The budget was **not** raised.

#### The Inspector's before/after preview: deliberately not extended

Bloom's row declares **no** `Preview`, so the strip is simply not drawn for it — the
cleanest possible degradation, since there is no placeholder to get wrong. `Why not, when
extending it looked cheap:`

1. **`EffectPreviewMath.h`'s whole contract is per-pixel.** `ApplyPixel()` is
   `(u, v, stats, params) → rgb`, and `Compose()`'s 256-entry LUT fast path — asserted
   equal to `ApplyPixel()` for every byte value in `test_overlay_ui.cpp` — exists because
   the adaptive effects are a pure function of one byte whenever the gain is a frame
   constant. A spatial operator breaks that at the root, not at the edges.
2. **The capture cannot say how big the glow should be.** The strip is a fixed 256×144
   downscale of a frame of *unknown* source size, and Radius is defined in source pixels.
   A CPU mirror would have to guess the reduction factor and would draw a glow of the
   wrong radius. That is precisely the "a preview that lies is worse than no preview"
   line the Adaptive Brightness strip's own design took.
3. **The capture pass is armed by the adaptive effects.** `cs_effects_preview.comp` runs
   only when `NeedsStatistics()` is true and applies `grade()` alone; making it serve
   Bloom would mean a second capture path, a second arming rule and a CPU blur — a
   feature of its own, not an extension.

Bloom is also the effect that needs the strip least: unlike a tone curve, its result is
visible on the game the moment the switch is flipped, at the size and radius it will
actually have.

#### Measured (desktop, headless, `scripts/effects-regression.sh`, 2026-09-08)

Same recipe as everything above (private headless sway, nested `gamescope --backend
wayland`, `gamescopectl screenshot "<path> 4"`, `tests/effects_scene_client.c` as the
game, 1280×720). Bloom alone, every other effect off, defaults unless stated. Captures:
`build-release/verify-shots/bloom-2026-09-08/`.

The line profiles are measured on the existing **`haloinv`** scene — a flat 220 box,
320×320, centred on a flat 15 field — which was built for the Local-adaptation halo
checks and happens to be exactly what a bloom test wants: a distinct bright source on a
dark field with a hard, straight edge at a known x. The profile walks out from that edge
along the box's centre line; `bloom-off-flat` is the control that says the field really
is flat before any of it means anything (measured: spread **0.0 counts**, 15 at every
distance from 4 px to 460 px).

##### The three sliders, as three different line profiles

The field value at each distance in source pixels out from the box's right edge. "Extent"
is where the profile falls back to within 2 counts of the far field, linearly
interpolated. Everything else at the shipped defaults.

**Radius** (`bloom-radius`, PASS) — the glow must reach **further**:

| Radius | d4 | d8 | d16 | d32 | d64 | d128 | far (d460) | extent |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.0 (σ 8 src px) | 26.1 | 21.5 | 16.1 | 15.0 | 15.0 | 15.0 | 15.0 | **15 px** |
| 0.5 (σ 16, default) | 28.9 | 25.6 | 20.5 | 16.0 | 15.0 | 15.0 | 15.0 | **28 px** |
| 1.0 (σ 24 src px) | 29.6 | 27.5 | 23.6 | 18.0 | 15.0 | 15.0 | 15.0 | **43 px** |

The reach nearly triples while the amplitude beside the source barely moves (+11.1 →
+13.9 → +14.6) — which is exactly what a wider Gaussian does to the same amount of light,
and is why Radius is checked on the extent and Intensity on the amplitude. Every profile
is **monotone decreasing** out from the edge: no ring, no overshoot.

**Intensity** (`bloom-intensity`, PASS) — the glow must get **brighter**:

| Intensity | d4 | d8 | d16 | d32 | far | amplitude at d4 |
| --- | --- | --- | --- | --- | --- | --- |
| 0.4 | 22.0 | 20.4 | 17.8 | 15.6 | 15.0 | **+7.0** |
| 0.8 (default) | 28.9 | 25.6 | 20.5 | 16.0 | 15.0 | **+13.9** |
| 1.6 | 41.6 | 35.8 | 26.4 | 17.4 | 15.0 | **+26.6** |

Doubling the slider almost exactly doubles the added light (+7.0 → +13.9 → +26.6), which
is the "linear where the glow is faint" property of the composite showing up as a
measurement rather than as an argument.

**Threshold** (`bloom-threshold`, PASS) — raising it must make **less** glow. The box is
220 (0.863 encoded), so 0.5 is well below its emission and 0.85 almost above it:

| Threshold | d4 | d8 | d16 | d32 | far | amplitude at d4 |
| --- | --- | --- | --- | --- | --- | --- |
| 0.5 | 50.5 | 42.5 | 29.4 | 16.9 | 15.0 | **+35.5** |
| 0.7 | 34.9 | 30.5 | 23.2 | 16.5 | 15.0 | **+19.9** |
| 0.85 | 15.9 | 15.4 | 15.0 | 15.0 | 15.0 | **+0.9** |

At 0.85 the source has essentially stopped emitting — the slider does turn the glow off
from above, which is the property that makes "what glows" a real control rather than a
brightness knob by another name.

##### Nothing away from a source moves

`bloom-unchanged-dark` (PASS) compares the `dark` scene with Bloom off against the same
scene with it on at the defaults. The bands are sampled at x 1150..1270 and the nearest
240-code highlight ends at x = 1070 — 80 source pixels away, five σ at the default
Radius:

| region | off | on | change |
| --- | --- | --- | --- |
| bands 5 / 8 / 12 / 16 / 20 | 5 / 8 / 12 / 16 / 20 | 5 / 8 / 12 / 16 / 20 | **0.00** |
| pure black corner | 0 | 0 | **0.00** |

Worst change anywhere away from a source: **0.00 counts**. Dark and mid-tone regions are
untouched, measured rather than asserted.

##### No clipping

`bloom-noclip-*` (all PASS) — the bands stay strictly ordered and nothing whose input was
below white comes out on white. `noclip-*-bloom-default` runs Adaptive Gamma's stricter
"and at least 2 counts apart" check on top, at the shipped defaults, and also passes.

| scene / setting | bands | the other region |
| --- | --- | --- |
| dark, off | 5 / 8 / 12 / 16 / 20 | 240 highlights: 240 |
| dark, defaults | 5 / 8 / 12 / 16 / 20 | **243.3** |
| dark, Intensity 2.0 | 5 / 8 / 12 / 16 / 20 | **246.9** |
| bright, off | 200 / 215 / 230 / 245 / 255 | 30 shadows: 30 |
| bright, defaults | **201.1 / 219.1 / 237.1 / 251.0 / 255.0** | **48.4** |
| bright, Intensity 2.0 | **202.3 / 224.2 / 244.1 / 254.0 / 255.0** | **72.7** |

The 240-code highlights on a dark field are the case a naive additive bloom destroys:
here they come out at 243 and, at the very top of the Intensity slider, at 247 — still
eight counts below white and clearly distinguishable from it. On the bright scene the
245 band lands on 251 at the defaults and 254 at Intensity 2.0: the gap to white narrows,
but nothing crosses.

**And exactly what the extreme costs, reported rather than hidden.** With Threshold at
0.0 — *every* pixel emitting, which is the setting a user reaches for when they want the
whole picture to glow — and Intensity at 2.0:

| scene | bands | the other region |
| --- | --- | --- |
| dark | 5 / 8 / 12 / 16 / 20 (**unchanged**) | 240 highlights: **249.7** |
| bright | 240.3 / 248.8 / 253.1 / **255.0** / 255.0 | 30 shadows: **116.8** |

On the dark scene nothing is lost: a band sitting at 0.02 of full scale contributes its
square, which is nothing, so the bands do not move and the highlights stop at 250. On the
**bright** scene the 245 band does reach 255 — `bloom_apply` is still strictly below 1.0
there, but by less than half of one 8-bit code, so the capture rounds it onto white. That
is the honest limit of the effect: *at the very top of two sliders at once, on a frame
that is already almost all white, the two brightest bands merge.* It is a setting the user has to go out of their way to
reach, it is not the default, and it is the only combination in the whole matrix that
does it — which is why it is an INFO line in the gate rather than a threshold picked so
that it passes. The 30-code shadows going to 117 in that state is the same statement from
the other side: a strong bloom on a bright frame is veiling glare, and veiling glare is
what bloom *is*.

##### Stability under a pan: no shimmer

The threshold question, as a number. `bloom-stability-static` (PASS) holds `texdark`
perfectly still for 300 composites with Bloom on and reads the graded output pixel out of
the `effects_ab_log` readback: **peak-to-peak exactly 0**, and the raw measurement
peak-to-peak exactly 0 too. The three extra dispatches are a deterministic function of
the frame, to the code value.

`bloom-stability-pan` (PASS) is the one that matters. Six captures of the same `texdark`
scene panning at 3 px/frame with `--periodic` — so the frame's *light population* is
identical every frame and only the layout moves — with the effect off and then on:

| | frame means | peak-to-peak |
| --- | --- | --- |
| Bloom off | 25.73 25.73 25.73 25.75 25.74 25.72 | **0.024 counts** |
| Bloom on (defaults) | 26.49 26.49 26.60 26.49 26.49 26.48 | **0.114 counts** |

**The frame-to-frame variation with Bloom on is 0.114 counts of 255** — roughly a ninth
of one code — against 0.024 with it off. So the effect does add a little frame-to-frame
movement, as any operator that reads a moving source must, and the amount is about a
tenth of a code on a frame whose mean is 26: far below what a human can see, and below
the rounding of the 8-bit capture it was measured in. The quadratic weight is why: a
pixel arriving at the threshold contributes a second-order amount, so the population
crossing the cut in any one frame moves the total by almost nothing. Bloom also adds a
constant **+0.76 counts** to this scene's frame mean, which is the glow itself and is
supposed to be there.

`Why the frame mean and not a probe pixel:` a probe pixel on a panning scene moves with
the texture and would measure the pan, not the effect (the Adaptive Gamma pan check reads
147 counts of probe-pixel movement for exactly that reason). The whole-frame mean is the
one quantity `--periodic` holds constant by construction, which is what makes a residual
in it attributable to the operator.

**What was NOT measured:** a real game frame. Everything here is the synthetic client, at
1280×720, on this desktop's GPU. The shape of the operator is pinned by the unit tests and
its behaviour on flat and textured synthetic scenes by the gate; how it *looks* on a real
game with real lights is a judgement only the user can make.

##### The settings audit

`scripts/settings-audit.sh` covers all four new rows (the switch and its three params) in
all three routing situations: **412 settings audited (138 unique ids across 3 situations,
412 rows), 312 passed, 0 failed, 100 not covered** — up from 400 / 134 / 300 before this
change, i.e. the twelve new rows all pass. Unlike Adaptive Gamma, Bloom needed **no**
change to the audit itself and no `SIBLING_KEYS` entry: its four writes each touch
exactly their own declared key and nothing else, because it has no exclusion rule with
any other effect and nothing about it is derived from another row's state.

##### Looked at, not just measured

Captures in `build-release/verify-shots/bloom-2026-09-08/`, graded by eye rather than by
checklist:

- **`17-haloinv-bloom-intensity-1.6.png`** — the clearest single answer. A soft,
  symmetric halo around the bright box, brightest right at the edge and fading smoothly
  outward with no ring and no blockiness, while the box's own interior stays where it
  was. This looks like bloom, not like a blur bug.
- **`17-haloinv-bloom-off.png`** vs the radius trio — off is a hard-edged white box on
  black; 0.0 is a tight rim; 1.0 is a wide, even haze. The difference between the three
  is legible at a glance, which is the test of whether the slider is worth having.
- **`16-texdark-bloom-off-1.png`** vs **`16-texdark-bloom-on-1.png`** — the closest thing
  here to a game frame: a dark textured field with scattered lights. Off, the lights are
  hard squares; on, each one has its own halo and the scene reads as if the lights are
  actually emitting. This is the capture that says the effect does what the request asked
  for.
- **`19-bright-bloom-worstcase.png`** — the unflattering one, and it is in the directory
  on purpose: threshold 0 and Intensity 2.0 on the brightest scene is a hazy, washed-out
  picture where the top two bands have merged. Nobody would ship that, and nothing in the
  defaults goes near it.


### Adaptive Brightness (`image.shaders.adaptive_brightness`)

**A plain switch, with Mode as a Param** (request #17, 2026-09-07,
`requests-2026-09-07.md` item 7: *"the adaptive brightness mode selector should be
inside of the inspector rail. In the main view, it should still only be a switch."*
This supersedes the previous day's three-way-Choice shape — see the history note
below). The sheet row is back to a plain **Switch**, on/off, in the shared **Effects**
`GroupCount` band alongside Vibrancy, Pre-Sharpen and Shadow Control (the pre-request-16
shape, restored). The mode lives in the Inspector's **Configure** page as the row's own
first `Param` — a two-way Choice, `Whole image` | `Dynamic` — ahead of the same six
existing params: `strength`, `target`, `up_speed`, `down_speed`, `min_gain`, `max_gain` —
plus, from 2026-09-07, an eighth, `local_strength`
([Local adaptation](#local-adaptation-local_strength-2026-09-07--one-curve-per-neighbourhood)).
**Config**: `ReshadeAdaptiveBrightnessSettings` — `enabled` (bool) and `mode`
(`"whole_image"` | `"dynamic"`, default `whole_image`, an unknown value on disk resolves
to it) are independent fields, written independently: the Switch's setter only ever
touches `enabled`, the Mode param's setter only ever touches `mode`, so switching the
effect off and back on leaves the mode exactly where the user left it. The panel's
`.Default()`s still read `ReshadeAdaptiveBrightnessSettings{}` (the `PanelCursor.cpp`
pattern) rather than repeating literals.

#### The budget decision: seven params, not six (2026-09-06/07)

Putting the mode back where a knob belongs (the Inspector's params column) made it a
genuine **seventh** `Param` on a row that already had six — a real registration abort
(SPEC §5.2 clause 3, `Registry.cpp`'s `kParamBudget`), not a styling choice. Two ways out
were weighed, honestly, against the existing six:

- **Merge or relocate one of the six.** No candidate survived scrutiny. `strength` and
  `target` are the effect's core dial and target and apply to both modes; `min_gain` and
  `max_gain` are independently meaningful bounds (a floor and a ceiling are not one
  number); `up_speed`/`down_speed` were the closest candidate — one "Adaptation speed"
  in place of two — but they are a **deliberate, documented asymmetry** ("Adapt to
  brighter" vs "Adapt to darker", retitled 2026-09-06 specifically because the old
  labels described the wrong direction — see [above](#the-statistics-both-modes)), not
  two names for one idea. Collapsing them would be a real capability loss (no more
  "react fast to a sudden bright flash, ease into darkness"), not a tidy-up. No merge
  was honest.
- **Raise the shared budget.** `Registry.cpp`'s `kParamBudget` went `6 → 7`, with its
  own comment carrying the same reasoning. This is a **one-time, evidenced exception**,
  not a standing invitation — the law's name and enum (`Law::SixBudget`) are unchanged,
  and the next param added anywhere is still a signal to promote, not to raise the
  number again.

Verified by capture before committing to it, not just argued: at 2560×1440 (2x-ish
shell width) the Inspector's Configure page renders all 7 rows —  Mode, Strength,
Target brightness, Adapt to brighter, Adapt to darker, Min gain, Max gain — with well
over half the panel's height still empty below them
(`build-release/verify-shots/requests-07-item2-7/03-shaders-ab-selected-whole.png`). No
crowding at this width; a narrow drawer at high UI scale (D13.4's 2.0x case,
`tests/test_overlay_shell.cpp`) still needs to scroll, exactly as it already did at six.

**Follow-up fixed 2026-09-07 (requests-2026-09-07.md item 12):** `Shell.cpp`'s Inspector
header used to hardcode the denominator — `snprintf(..., "PARAMETERS   %d of 6", ...)`
— so a capture of this very row showed the header reading **"PARAMETERS 7 of 6"**,
literally wrong once the ceiling moved to 7. `Registry.h`/`.cpp` gained `ParamBudget()`
(reads `kParamBudget` outside its former anonymous namespace) and `Shell.cpp` now builds
the header via `controls::ParametersHeaderText( nCount, nBudget )` (`Controls.h`/`.cpp`,
pure formatting, pinned in `test_overlay_ui.cpp`) instead of a second hand-copied
literal — the header and the registry constant cannot silently disagree again.
Verified: selecting this row now reads **"PARAMETERS 7 of 7"**
(`build-release/verify-shots/accent-header-2026-09-07/02-adaptive-brightness-inspector.png`).

`Why the declaration order matters elsewhere too:` groups in a sheet area are packed
into columns by a greedy shortest-column algorithm over declaration order, not by an
explicit column API (`Registry.h`; see `crosshair.md`'s note on the same mechanism) —
unrelated to the budget, but the same "the shell has no per-group/per-param placement
API, only ordering" shape shows up twice this cycle.

The id `image.shaders.adaptive_brightness` and every config key from the Choice-row era
are unchanged; `overlay_e2_set image.shaders.adaptive_brightness 1` still means "on",
and the mode is now set the same way any Param is —
`overlay_e2_set image.shaders.adaptive_brightness.mode 1` for Dynamic (`Registry::FindParam()`
resolves a Param id exactly like an Entry id).

<details>
<summary>History: the three-way Choice (2026-09-06, superseded the next day)</summary>

Request #16 first asked only for a mode, and the row briefly became a three-way
**Choice** — `Off` | `Whole image` | `Dynamic` — in its own **Adaptive Brightness** band
below Effects, to stay under the six-param budget without a seventh param: on/off and
mode were treated as one decision ("which adaptation, if any"). It needed its own band
because the Effects band's `n / m` count is computed from **Switch** rows only
(`Shell.cpp`'s `DrawGroupBand`), and a Choice row among the switches would have made the
count read one short. Request #17 the next day asked for the opposite layout, which is
what the rest of this section describes.

</details>

#### The statistics (both modes)

Two dispatches per frame. **The measure pass** (`cs_effects_measure.comp`,
`SHADER_TYPE_EFFECTS_MEASURE`) takes a **128×128 grid of taps** over the whole image (16×16
threads, 8×8 per thread, 16384 taps — 64×64 / 4096 until 2026-09-07, see
[the pulse](#the-pulse-rank-cuts-on-a-bimodal-histogram-2026-09-07) below), each run
through `grade()` so it measures the Shadow-Control/Vibrancy-graded image the `.fx`
measured (its `PreSharpenOut` texture) — sharpening is not applied to the taps; it does
not move the statistics. From those taps it derives four numbers on the **encoded**
Rec.601 luma (`(.299, .587, .114)`, as the `.fx` computed it):

| History texel | Statistic | Read by |
| --- | --- | --- |
| `HISTORY_MEAN` (0) | arithmetic mean (shared-memory tree reduction, as before) | Whole image |
| `HISTORY_P2` (1) | mean of the taps ranked 1 %..3 % — the shadows | Dynamic (shadow cap) |
| `HISTORY_P50` (2) | mean of the taps ranked 25 %..75 % — the mid-tone anchor | Dynamic (gamma) |
| `HISTORY_P98` (3) | mean of the taps ranked 97 %..99 % — the highlights | Dynamic (levels gain) |
| `HISTORY_RAW` + 0..3 (4..7) | this frame's unsmoothed measurement of the same four | nothing — the `effects_ab_log` readback only |

The three ranked statistics are **rank-window means** — the mean luma of the taps whose
rank, sorted from black, falls in a window centred on the named percentile — computed
from a **64-bin histogram in shared memory** of counts *and* value sums (two
`atomicAdd`s per tap, then invocation 0 walks the bins once). The part of a bin that
falls inside the window contributes that many taps at the bin's own mean value, so on a
dense histogram this *is* the interpolated percentile at the window's centre: the flat
reference scenes below measure identically to the rank-cut percentiles they replaced, to
the code value. Where the histogram has an empty stretch the two differ, and that is the
point — a rank *cut* jumps across the gap on a single tap, a window *mean* moves by
`gap / window taps` (see the pulse section). `Why 64 bins:` 16384 taps over 64 bins is
256 per bin on a flat image; finer bins would be mostly empty and the in-bin mean already
recovers sub-bin precision. `Why the median (well, its window), not the mean, for
Dynamic:` a few bright windows in a dark room must not read as "the room is lit" — the
`.fx`'s 25 fixed taps could be swung by one highlight, the mean by a few percent of the
image, the 25..75 % window by neither (anything in the top quarter is outside it).

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
   gmin = clamp(1 / max_gain, 0.25, 1.0)     // the gamma bounds are the user's own
   gmax = max(min(1.5, 1 / min_gain), 1.0)   //   gain bounds -- see "The ceiling" below
1. GAIN      G = clamp(max(0.9 / p98,             // levels: p98 -> WHITE
                           target^(1/gmin) / p50), //   ... or what Target needs
                       min_gain, max_gain)
2. GAMMA     g = ln(target) / ln(p50 * G), clamped to [gmin, gmax]  // median -> target
             if g > 1: g = min(g, ln(p2 * min_gain) / ln(p2 * G))   // shadow cap
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
- **Target's own demand on the gain** (2026-09-08): `target^(1/gmin) / p50` — the exposure
  the gain must supply so that the gamma, *at its floor*, can still land the median on the
  target. The **larger** of it and the white point wins, so the white point is a floor on
  the gain (a bright scene is never dimmed past putting p98 at 0.9) and Target keeps moving
  the picture once the gamma floor is reached instead of going flat. See
  [The ceiling](#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08).
- **Gamma**: the exponent that lands the smoothed **median** on `target` after the gain,
  bounded to `[gmin, gmax]`, both derived from the user's own gain bounds. `Why
  gmin = 1/max_gain:` Max gain then means "how bright may it go" in *both* channels — the
  exact mirror of `min_gain`, which already means "how dark may it go" through the shadow
  cap — and `max_gain` 2.0 reproduces the historical fixed floor 0.5 exactly, which is why
  every pre-2026-09-08 measured number is still reproducible at that setting. It is clamped
  to `[0.25, 1.0]`: below 0.25 a single code of near-black lands above 90 and sensor noise
  is all you see; 1.0 is "no lift at all", so `max_gain` 1.0 really does not brighten.
  `Why gmax = min(1.5, 1/min_gain):` a darkening gamma crushes shadows by nature and beyond
  1.5 the shadow cap is all that keeps detail; the `1/min_gain` half is the mirror again
  (`min_gain` 1.0 = "do not darken") and is slack at every shipped default (1/0.3 = 3.33),
  so it changes no measured number. **The shadow cap** is `min_gain`'s Dynamic meaning: a
  darkening gamma may not push the smoothed 2nd percentile below `p2 × min_gain` — "how
  dark may it go", applied to the shadows.

#### Why min_gain 0.3, max_gain 4.0 (2026-09-07 request: *"make min gain 0.3, max gain
4.0"*)

Ranges widened from 0.5..1.0 / 1.0..2.0 to **0.3..1.0 / 1.0..4.0**, and the schema
**defaults moved to the new extremes** — `min_gain` 0.5 → 0.3, `max_gain` 2.0 → 4.0
(`ConfigSchema.h`) — the plain reading of "make min gain 0.3, max gain 4.0". An existing
config keeps whatever value it already stored; only a config that never set these keys
(or a fresh install) sees the new numbers. `GAMMA_MIN`, `GAMMA_MAX`, `KNEE` and `WHITE`
(`effects_curve.h`) are deliberately **unchanged** by this request — a separate
highlight-rolloff change touching those four is being designed against the user
separately, and keeping them fixed here lets the two be judged independently.

Two consequences checked by measurement rather than assumed:

- **The shadow cap loosens.** `min_gain` is also "how dark may the 2nd percentile go"
  inside the gamma shadow cap (above). On the bright reference scene (p2=30, p50=225,
  p98=250, target 0.5, gain 0.918 unaffected since it is not at either bound): at
  `min_gain` 0.5 the cap itself was the binding constraint and held the shadows at
  exactly `p2 × 0.5` = **15**; at `min_gain` 0.3 the cap's own ceiling (`ln(p2 × 0.3) /
  ln(p2 × gain)` ≈ 1.503) now exceeds `GAMMA_MAX` 1.5, so **`GAMMA_MAX` becomes the
  binding constraint instead of the cap**, and the shadows land at **9** — measured
  identically off the GPU (`scripts/effects-regression.sh`'s `bright-dynamic`: rect
  9.0) and from the pure curve function (`tests/test_effects_curve.cpp`'s dedicated
  "min_gain 0.3 loosens the shadow cap" case). The cap's own invariant (never below
  `p2 × min_gain` = 9.0 exactly) still holds — 9.0..9.05 is within float rounding of
  it — so nothing is mathematically broken, but the **margin above "crushed" is real
  and roughly halved** (15 vs a floor of "distinguishable from black" versus 9 vs the
  same floor). Visually (`build-release/verify-shots/adaptive-gain-2026-09-07/`,
  bright scene, min_gain 0.5 vs 0.3, both at `max_gain` 4.0): the 30-value shadow
  rectangles read as very dark grey in both captures, not literally crushed to black,
  but the 0.3 capture is visibly the darker of the two — the loosened cap is a real,
  if secondary, effect of this request and not just a formula footnote.
- **The dark scene's shadows lift much further, without crushing anything.** In the
  dark reference scene the darkening branch of the gamma never engages (the scene needs
  a *lift*, not a darken), so the shadow cap is irrelevant there; the whole change is
  driven by `max_gain` alone. See the re-measured table below.
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

**Properties asserted on the CPU** (`[effects_curve]`, 25 cases, 7.94 M assertions over
seven scenes × five targets × twenty gain-bound pairs — widened 2026-09-07 from nine
pairs / 1.26 M assertions to cover the full 0.3..1.0 / 1.0..4.0 panel ranges, gain 4.0
included at every gamma in range): output in
`[0, 1]` and finite; monotonic in the input; `0 → 0`; `x = 1 → exactly 1` whenever the
shoulder is active; identity at strength 0; the mid reference scene is the identity; the
gamma clamps and the shadow cap hold; the shoulder is C0/C1-continuous at the knee; the
shadow cap at min_gain 0.3 is measurably looser than at 0.5 yet still holds its own
invariant (2026-09-07 case). Plus, from 2026-09-08: the gamma bounds come from the gain
bounds and can never invert; `max_gain` 2.0 reproduces the pre-2026-09-08 curve exactly;
`max_gain` 1.0 brightens **nothing**, at any target, on any scene; Target moves a realistic
dark frame monotonically and by more than 20 counts from 0.5 to 0.7; Max gain moves it by
more than 15 counts per step and monotonically over the whole panel range; and
`ab_dyn_binding()` returns exactly one in-range code with a non-empty wording for every
combination. Plus the config round-trip of `mode` and its unknown-value fallback.

#### Measured (desktop, headless, `scripts/effects-regression.sh`, re-measured 2026-09-07
at the new min_gain 0.3 / max_gain 4.0 defaults; superseded numbers at the old 0.5 / 2.0
defaults are kept alongside for comparison)

The recipe is `pixel-regression.sh`'s (private headless sway, nested `gamescope --backend
wayland`, `gamescopectl screenshot "<path> 4"`), with `tests/effects_scene_client.c` as
the game: an SDL2 window painting three synthetic scenes from flat regions at known pixel
positions — **dark** (bands 5/8/12/16/20, a pure-black corner, six 240 squares ≈ 1 % of the
image), **bright** (bands 200/215/230/245/255, six 30-valued rectangles ≈ 4 %, so p2 *is*
the shadows) and **mid** (26/77/128/179/230) — advanced with `SIGUSR1`. Every number is the
mean grey of a region's interior, all six Adaptive Brightness params at their defaults,
strength 1.0. Captures: `build-release/verify-shots/effects-regression/20260907-055139/`
and, for a direct old-vs-new side-by-side at fixed scenes,
`build-release/verify-shots/adaptive-gain-2026-09-07/`.

> **2026-09-08 note.** The `Dynamic (new)` column below is the **2026-09-07** curve. The
> ceiling fix ([The ceiling](#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08))
> changed exactly one row of it — the dark scene's bands, which now read
> **88 / 107 / 127 / 143 / 157** with the 240 highlights and pure black unchanged at 254
> and 0. Every other row here was re-measured after the fix and is bit-identical.

| Scene / region (input) | Off | Whole image (new) | **Dynamic (new)** | Dynamic (old 0.5/2.0) | Dynamic must |
| --- | --- | --- | --- | --- | --- |
| dark: darkest band (5) | 5 | 20 | **71** → 88 (2026-09-08) | 50 | be readable: ≥ 30 |
| dark: bands 8 / 12 / 16 / 20 | 8/12/16/20 | 32/48/64/80 | **90 / 111 / 128 / 143** → 107 / 127 / 143 / 157 | 64/78/90/101 | keep their order |
| dark: 240 highlights | 240 | **255 — clipped** | **254** | 253 | stay < 255, above every band |
| dark: pure black | 0 | 0 | **0** | 0 | stay ≤ 2 |
| bright: bands 200 / 215 / 230 | 200/215/230 | 115/124/132 | **152 / 169 / 187** | 165/180/196 | keep their order |
| bright: 245 band (p98 region) | 245 | 141 | **206** | 213 | come down below 235 |
| bright: white (255) | 255 | 146 | **218** | 224 | — |
| bright: 30 shadows (p2) | 30 | 17 | **9** | 15 | not crushed: ≥ 8, and ≥ 30 × min_gain (0.3 → 9.0) |
| mid: 26 / 77 / 128 / 179 / 230 | identical | 26/77/128/178/229 | **25 / 75 / 126 / 177 / 228** | 25/75/126/177/228 | near-identity: worst ≤ 6 (measured 2) |

Widening `max_gain` to 4.0 roughly doubles how far Dynamic lifts a dark scene: the
darkest band goes from 50 to **71** (a √2× step, since the curve's gamma floor stays at
0.5 and the gain doubled), and every darker band lifts proportionally more — genuinely
more readable shadow detail, still monotonic, still nowhere near clipping (254, two
counts under white, essentially unchanged from the old 253). Whole image on the dark
scene is still the blown-out case the user described, more so now: the gain hits the new
`max_gain` 4 and the 240 highlights still go to 255 with far more of the image clipping
above it, while the darkest band only reaches 20 (was 10 at the old `max_gain` 2). This is
exactly the contrast Dynamic exists to fix, sharper than before.

On the **bright** scene the honest, less flattering half of this change: `max_gain` 4.0
does not touch the bright scene at all (its gain, 0.918, sits well inside both the old and
new bounds), but `min_gain` 0.3 pulls every highlight number **down** relative to the old
0.5 default — the 245-band figure that "comes down below 235" now reads **206** instead of
213, and white itself lands at **218** instead of 224. That is *more* compression of the
highlights, not less: the user's standing complaint is "some things are still overblown",
and on this scene alone the new defaults compress the top end **harder**, not softer — a
real trade-off of the wider range, not a pure win. It happens because the same gamma that
governs the highlight fall-off is now clamped to `GAMMA_MAX` 1.5 (unchanged, per the
task's constraint) instead of the old, tighter shadow cap — see "Why min_gain 0.3, max_gain
4.0" above for the mechanism. The shadow cap itself is why: at `min_gain` 0.5 the cap held
gamma to 1.273; at `min_gain` 0.3 the cap's own ceiling (≈1.503) now exceeds `GAMMA_MAX`
1.5, so gamma rides the fixed 1.5 ceiling instead — a bigger darkening exponent, which is
what drags every bright-scene number down together. Whole image is unaffected on this
scene either way (INFO only, not asserted) since its one linear gain, 0.567, is inside
both ranges. `Why the 2nd-percentile shadows are the sharpest change (15 → 9):` the shadow
cap is `min_gain`'s own knob for that value specifically — see above.

**Temporal** (dark → bright under Dynamic, `tau` 1 s; the "requested at" times are
measured from the `SIGUSR1` to the screenshot request, the screenshot itself adds a
frame): the middle (230) band read **254** at 0.37 s, **245** at 1.28 s, **198** at 3.35 s,
and **187** settled — monotonic, no overshoot, within 12 counts of settled at 3 s (down
from 252/245/205/196 at the old 0.5/2.0 defaults — the settled value is lower for the same
"compresses the highlights harder" reason as the table above). The first second looks slow
because while the smoothed gain is still above 1 the shoulder pins the bright bands near
white; once the gain drops under 1 the curve releases them. No oscillation was seen in any
capture. Per frame (`transition-gain`, the `effects_ab_log` trace the script takes across
the same switch): the gain goes 4.000 → 0.905 monotonically, never below its settled value,
within 5 % of it at **1.73 s**; the 230-band pixel goes 254 → 188 monotonically, within 5 %
at 3.9 s (the shoulder holds it near white while the gain is still above 1). These are the
EMA's numbers and the 2026-09-07 estimator change did not touch the EMA, so they are the
same before and after that fix.

#### Does Dynamic actually work above `max_gain` 2.0? (2026-09-07 — measured, yes, **on this scene only** — superseded 2026-09-08)

> **Superseded.** Everything below is still true *of the flat dark band chart*, and that
> is the problem: the answer generalised from a scene whose statistics are nothing like a
> game's. See [The ceiling](#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08)
> immediately after it. The numbers in the table below are the pre-2026-09-08 curve's and
> are kept as the "before" side of that measurement.


Asked directly (*"Does the adaptive brightness even work above 2.0 gain? Check that."*),
and answered by sweeping the knob on the dark reference scene rather than by reading the
code. Dynamic, everything else at the schema defaults, Local adaptation forced to 0 so
this is the global curve alone; the applied gain is `effects_ab_log`'s, the band values
are the capture's
(`build-release/verify-shots/adaptive-local-2026-09-07/dark-maxgain-{1.5,2,3,4}.png`):

| `max_gain` | applied gain | gamma | dark bands 5 / 8 / 12 / 16 / 20 | 240 lights |
| --- | --- | --- | --- | --- |
| 1.5 | **1.500** | 0.500 | 44 / 55 / 68 / 78 / 87 | 252 |
| 2.0 | **2.000** | 0.500 | 50 / 64 / 78 / 90 / 101 | 253 |
| 3.0 | **3.000** | 0.500 | 62 / 78 / 96 / 111 / 124 | 254 |
| 4.0 | **4.000** | 0.500 | 71 / 90 / 111 / 128 / 143 | 254 |

**The gain reaches its bound exactly, at every setting, and the picture keeps getting
brighter past 2.0** — nothing on the path clamps earlier. Checked one by one: `ab_dyn_gain`
clamps to `[min_gain, max_gain]` and lands *on* `max_gain` here (a 20-code p98 wants
`0.9 / 0.078` = 11.5, far above any bound); the gamma clamps do not touch the gain; the
shadow cap only exists for a *darkening* gamma (`g > 1`) and this scene's gamma is 0.5, its
opposite bound; the shoulder compresses only above the 0.7 knee, and the 240 lights land at
252–254 rather than clipping; `EffectsPushData_t` passes `flAbMaxGain` through untouched;
and the panel's own `Range( 1.0f, 4.0f )` is the only ceiling in the system, at 4.0.

**But the returns are square-root, and that is worth knowing.** On any scene dark enough to
want maximum lift, the gamma is pinned at `GAMMA_MIN` 0.5, so the curve is `(x·G)^0.5` and
doubling `G` multiplies the output by only **√2**: 50 → 71 for a 2× gain step, not 50 → 100.
That is the honest shape of "does it work above 2" — it works, and each further stop of Max
gain buys 41 % rather than 100 %, because the *gamma floor is already saturated*, not because
anything clamps the gain. Raising `GAMMA_MIN` would be the lever there, and it is deliberately
not touched (see the constant's note in `effects_curve.h`).

#### The ceiling: Target brightness and Max gain went inert (2026-09-08)

> *"It still feels like anything above target brightness 0.5 and max gain 2.0 does
> anything at all"* — read with the missing negation: raising either appears to do
> nothing. Reported straight after the table above had "proved" Max gain works.

Both halves were true, for two independent reasons, and the section above is exactly how
the second one stayed hidden: **a flat band chart flatters this operator.**

##### What was measured, before anything was changed

`effects_curve.h` was evaluated on the CPU — the same header text the GPU compiles — over
seven scenes × the whole 0.1..0.9 Target range × Max gain 1.5/2/3/4, with a mirror of
`cs_effects_measure.comp`'s tap grid, 64-bin histogram and rank-window means producing each
scene's statistics. The harness reproduces the GPU's dark-scene capture **code for code**
(71/90/111/128/143), which is what makes the rest of its numbers usable. Beside the four
synthetic scenes it was run on two **real photographs** resized to 1280×720 — a night-city
frame and a moonlit one — because a continuous histogram is the thing the band charts do
not have; no real game frame existed under `build-release/verify-shots/` to use instead.

| scene (measured p50 / p98) | `0.9/p98` — all Max gain ever saw | Target inert above | which clamp binds there |
| --- | --- | --- | --- |
| dark band chart (0.047 / 0.153) | **5.90** | 0.436 | gamma at its 0.5 floor |
| textured dark, 2 % lights (0.075 / 0.318) | **2.83** | 0.461 | gamma floor |
| night-city photograph (0.141 / 0.378) | **2.38** | 0.581 | gamma floor |
| moonlit photograph (0.076 / 0.440) | **2.05** | 0.394 | gamma floor |
| half-dark/half-bright (0.449 / 1.000) | 0.90 | 0.635 | gamma floor |
| mid (0.502 / 0.902) | 1.00 | 0.708 | gamma floor |
| bright (0.898 / 1.000) | 0.90 | 0.899 (and inert *below* 0.726) | shadow cap / `GAMMA_MAX` |

- **Target** reached the picture *only* through `g = ln(target) / ln(p50·G)`, clamped to a
  fixed `[0.5, 1.5]`. Solving `g = 0.5` gives `target = sqrt(p50·G)` — every target above
  that produces the identical clamped gamma. The shipped default 0.5 already sat on the
  dead side of that threshold on three of the four dark scenes.
- **Max gain** only ever fed `clamp(0.9/p98, min, max)`. A realistic frame has enough
  bright content that the demand is ~2, so **above it the clamp never binds and the slider
  is an exact no-op** — not diminishing returns, zero. The band chart's p98 of 0.15 asks
  for 5.9, which is why every setting up to 4.0 bit there and the 2026-09-07 table read as
  a clean pass.

Confirmed on the GPU with the *old* binary, `effects-regression.sh` on the textured dark
scene, frame mean: Target 0.3 / 0.5 / 0.7 read **83.0 / 112.1 / 112.1** (the last step
`+0.0`), and Max gain 3 vs 4 read **112.06 vs 112.06** — byte-identical.

##### The fix, and why this shape and not the obvious ones

1. **`gamma_min = clamp(1 / max_gain, 0.25, 1.0)`** replaces the fixed 0.5. Max gain now
   governs "how bright may it go" in both channels — the exact mirror of `min_gain` through
   the shadow cap — and this is what makes Max gain move a *realistic* frame, whose
   white-point demand saturates around 2. On the moonlit frame a 5-code input reads
   24 / 50 / 77 / 77 at Max gain 1.5 / 2 / 3 / 4 where it read **51 at every one of them**
   before. `Why 2.0 is the anchor:` `1/2.0` is exactly the old floor, so every measured
   number taken before this change is still reproducible at that setting — asserted in
   `tests/test_effects_curve.cpp`. `Why the 0.25 floor:` at 0.25 with a 4× gain, one code
   of near-black lands at 90; lower than that and shadow noise is the whole picture.
   `Why the 1.0 ceiling:` it removes a real lie — at Max gain 1.0, "do not brighten", the
   old gamma still lifted a 20-code band to **71**.
2. **The gain also carries `target^(1/gamma_min) / p50`**, the smallest exposure that
   leaves the target reachable at the gamma floor, and the larger of it and the white point
   wins. Past the floor the gain takes over, so Target keeps moving the picture.
   `Why not the simpler target / p50:` that is a *bigger* gain, and moving lift out of the
   toe into a linear gain darkens the shadows at the same median — measured, it dropped the
   half-dark/half-bright scene's dark half from 12/17/23/28/34 to **6/9/13/18/22**. The
   smallest-sufficient form leaves that scene bit-identical.
3. **`ab_dyn_binding()`** — see [Which limit is binding](#which-limit-is-binding-2026-09-08)
   below. Rejected alternatives: *lowering `GAMMA_MIN` alone* fixes neither half (Max gain
   stays inert wherever the gamma is free, and a fourth-root floor for everyone is milky);
   *making Target a direct exposure* costs the bright scene its highlights (245-band
   206 → ~142, the blown-out look Dynamic exists to avoid) and the split scene its shadow
   lift; *a new control* was refused on the parameter budget (`kParamBudget` is at 8 and
   already owes a rail-area split) and would not have been honest anyway — the sliders that
   say they do this had to start doing it.

##### After: what each slider is worth now (GPU, `effects-regression.sh`, frame mean)

| sweep, textured dark scene | before | after |
| --- | --- | --- |
| Target 0.3 → 0.5 → 0.7 (Max gain 4) | 83.0 → 112.1 → **112.1** | 82.8 → 129.4 → **175.4** |
| Max gain 1.5 → 2 → 3 (Target 0.5) | 90.0 → 103.1 → 112.1 | 65.7 → 103.0 → 129.4 |
| Max gain 3 → 4 (Target 0.5) | 112.06 → 112.06 | 129.39 → 129.39 (**still equal** — see below) |

And the reference scenes at the shipped defaults, off the same two runs:

| scene | before | after |
| --- | --- | --- |
| dark: bands 5/8/12/16/20 | 71 / 90 / 111 / 128 / 143 | **88 / 107 / 127 / 143 / 157** |
| dark: 240 highlights, pure black | 254, 0 | 254, 0 — unchanged |
| bright: 200/215/230/245/255, 30-shadows | 152/169/187/205/218, 9 | **identical** |
| mid: 26/77/128/179/230 | 26/77/128/178/229 | **identical** |
| halfsplit at Local 0 and 50 % | 12/17/23/28/34 · 195/207/217/228/235 | **identical** |
| halo amplitude, halobox / haloinv at 50 % and 100 % | +8 / +15, −7 / −14 | **identical** |
| still-frame peak-to-peak at Local 0 / 50 / 100 | 0 | **0** |
| panning gain spread (300 frames) | 0.0324 | 0.0191 |

The dark scene is the only thing that moved, and it moved brighter: at Max gain 4 the
gamma the median asks for (0.417) is no longer clamped up to 0.5.

##### The residual ceiling, stated plainly

**Once the mid-tones are on Target, Max gain is no longer a brightness control.** The
median is pinned by construction, so a higher Max gain only shifts the same mid-tone level
out of the toe and into the linear gain: the deepest shadows come out slightly darker and
everything above the mid-tones slightly brighter (dark chart at target 0.5, inputs 5 / 20:
Max gain 3 → 93 / 152, Max gain 4 → 84 / 157). That is why Max gain 3 and 4 still produce
the same textured-dark picture at the default target, and why the readout below names
**Target brightness** in that state — it is the control that can still move the picture.
Target itself is limited by the user's own Max gain: on the flat dark chart at Max gain 4
it stops mattering above 0.70 (up from 0.436), on the moonlit frame above 0.75.

##### Which limit is binding (2026-09-08)

The most expensive part of this was never the ceiling — it was that **a clamped slider
looks exactly like a working one**, so a session went into dragging controls that could
not respond. `ab_dyn_binding()` classifies, from the same three statistics and the same
bounds the curve uses, which constraint is currently stopping the picture, and
`ab_binding_text()` is the single wording of the six codes:

| code | shown as |
| --- | --- |
| `AB_BIND_NONE` | `none -- the mid-tones are on Target brightness` |
| `AB_BIND_GAIN_MAX` | `gain is at Max gain` |
| `AB_BIND_GAIN_MIN` | `gain is at Min gain` |
| `AB_BIND_LIFT` | `Max gain -- Target brightness does no more here` |
| `AB_BIND_DARKEN` | `the darkening limit -- Target brightness does no more here` |
| `AB_BIND_SHADOW` | `Min gain, holding the shadows up` |

It appears twice, from one function, so a trace and the panel cannot disagree: appended to
every `effects_ab_log` line as `bind=<n> (<text>)`, and as a third `LIVE` line —
`adaptive limit` — on the Shaders area's **Pipeline** facts row
(`AbPreview_BindingLine()`, which classifies the frame the Inspector's before/after preview
already captures and arms that capture itself, so the row fills in on its own).
`Why the frame's global curve and not the pixel's:` Local adaptation redistributes gain
*inside* these same bounds and never widens them, so the frame's answer is the one that
tells the user which knob to reach for. In Whole image mode the row names that mode's own
single gain bound instead of pretending the Dynamic classifier applies. Captured at three
settings in `build-release/verify-shots/adaptive-ceiling-2026-09-08/`.

#### Local adaptation (`local_strength`, 2026-09-07) — one curve per neighbourhood

`requests-2026-09-08.md`: *"we need to somehow make it more aggressive/adapt better. It
doesn't really seem like it can handle a lot of different brightness differences on the
screen."*

**The diagnosis, confirmed by capture before anything was built.** Everything above is a
*global* curve: one gain, one gamma, one shoulder for the whole frame. On a frame that is
half dark interior and half bright sky there is no such curve — lifting the interior blows
the sky, protecting the sky leaves the interior black. Measured on the new `halfsplit` scene
(left half the dark scene's 5 / 8 / 12 / 16 / 20 bands, right half the bright scene's
200 / 215 / 230 / 245 / 255): with Dynamic on and the global curve alone, the frame's p98
comes from the bright half, the gain settles at **0.90**, and the dark half reads
**12 / 17 / 23 / 28 / 34** — barely distinguishable from the effect being off
(`split-off.png` vs `split-local-0.png` in the captures directory are nearly the same
picture). More aggression in a global curve cannot fix that; it only moves which half is
wrong.

**The operator.** Three pieces, all inside the two dispatches that already exist:

1. **A 16×16 map of local mean luminance**, written by `cs_effects_measure.comp` into rows
   1..16 of the same history texture (which grew from 8×1 to 16×17; row 0 is still the
   statistics). *It costs no taps at all*: the measure pass is 16×16 threads and thread
   (tx, ty) already owns exactly the 8×8 block of the 128×128 tap grid covering image cell
   (tx, ty), so the per-thread sum it computes for the mean's tree reduction **is** that
   cell's mean. `Why the arithmetic mean of the encoded luma and not a log mean:` the pass
   works in encoded space on purpose and an encoded value is already perceptually spaced —
   a log of it would apply a second perceptual curve — and `HISTORY_MEAN` is the arithmetic
   mean of the same quantity, so local ÷ global is a ratio of like for like.
2. **Smoothing, spatial then temporal.** Spatially, a separable 5-tap binomial blur over
   that 16×16 grid in shared memory, run at strides 4, 2, 1, 1 (the à-trous trick: a
   stride-*k* pass costs the same five taps but carries variance *k*²), total variance 22,
   i.e. **σ ≈ 4.7 of the 16 cells** — getting on for a third of the frame. Temporally, the
   **same EMA with the same asymmetric `up_speed`/`down_speed`** every other statistic gets,
   per cell.
3. **A per-pixel shift of the frame's own histogram.** The apply pass samples the map
   bilinearly (by hand, on the unpacked floats — the texels are float bits spread over four
   UNORM8 lanes, so hardware filtering would interpolate the *bytes*), forms
   `r = clamp(local / global_mean, 0.25, 4)`, blends it as `r_eff = r^strength`, and feeds
   `p98·r_eff`, `p50·r_eff`, `p2·r_eff` into the **same** `ab_dyn_gain` / `ab_dyn_gamma` /
   `ab_dyn_curve` as before. Nothing about the curve changed.

```
r      = clamp(local_mean / mean, 0.25, 4.0)      // effects_curve.h, ab_local_ratio
r_eff  = r ^ local_strength                       //                  ab_local_shift
gain   = ab_dyn_gain (p98 * r_eff, p50 * r_eff, target, min_gain, max_gain)
gamma  = ab_dyn_gamma(p2 * r_eff, p50 * r_eff, gain, target, min_gain, max_gain)
```

- `Why shift the global histogram rather than measure a local one:` a 16×16 cell holds one
  number, not a histogram. "This corner has the frame's shape at a lower level" is the
  cheapest assumption available and it is *exactly right* when the frame is uniform.
- **Strength 0 is the old behaviour, exactly.** `r_eff` is then the literal 1.0 and
  `p98 * 1.0` is exact for every finite float, so the same bits reach the same functions.
  The apply pass's `if (u_abLocal > 0.0)` skips only the four map fetches; it is not what
  makes the identity hold. Pinned in `tests/test_effects_curve.cpp`.
- **The user's bounds are never widened.** `ab_dyn_gain` still clamps to
  `[min_gain, max_gain]` and `ab_dyn_gamma` to `[gmin, gmax]` *after* the shift, so a
  locally-adapted pixel can never leave the range the user set — local adaptation
  redistributes gain **inside** those bounds. Asserted over every scene × bound × ratio ×
  strength combination on the CPU.
- `Why r^strength and not mix(1, r, strength):` the ratio divides into the white point, so
  the gain goes as 1/r and a linear blend gives a wildly uneven slider. Measured on the
  split scene (r at its 0.25 clamp), strengths 0 / 25 / 50 / 75 / 100 %: linear gave gains
  **0.90 / 1.11 / 1.44 / 2.06 / 3.60** — three quarters of the effect in the last quarter
  of the travel — and `r^s` gives **0.90 / 1.27 / 1.80 / 2.55 / 3.60**, every step a
  constant 1.41× (half a stop). Both are exactly 1 at 0 and exactly *r* at 1; only the
  middle differs.

##### Halo control: the make-or-break, and what the radius actually buys

A local tone operator's signature artefact is a bright rim around a dark object on a bright
field. Two scenes were added to measure it rather than argue about it: **`halobox`** (a flat
200 field with one flat 10 box, 320×320 — a quarter of the frame across) and **`haloinv`**
(the inverse, a 220 box on a 15 field), sampled along a line straight out from the box's
right edge. This is a deliberately adversarial case: a hard, straight, high-contrast edge on
a perfectly flat field is the worst thing a local operator can be shown.

**There is no ring, at any strength, by construction and by measurement.** The map is a
non-negative, symmetric, unimodal blur of the image, and such a kernel maps a step to a
*monotone* ramp — it cannot overshoot. Every profile below is monotone out from the edge;
`effects-regression.sh` asserts that at every strength, precisely so that a later
"improvement" (a sharper map, an edge-aware filter) that reintroduces a rim fails loudly.

What *does* exist is a broad gradient, and its amplitude is what the radius buys.
`halobox`, at Local adaptation 100 %, field value at 12 px from the box edge vs 460 px away:

| blur σ (cells) | beside the box | far field | amplitude | what the capture shows |
| --- | --- | --- | --- | --- |
| 1.4 (first attempt) | 246 | 189 | **+57** | an unmistakable white glow ringing the box |
| 2.2 | 243 | 190 | +53 | barely better — see below |
| **4.7 (shipped)** | **219** | **204** | **+15** | a faint frame-wide shading; no glow |
| 4.7, at the 50 % default | 216 | 208 | **+8** | not visible on the flat field |

`haloinv` mirrors it: −56 codes at σ 1.4, **−14** at σ 4.7 / 100 %, **−7** at the default.

`Why widening from 1.4 to 2.2 did almost nothing, and 4.7 did everything:` right *at* a step
edge a blurred step always sits halfway between the two sides, whatever the σ — widening the
kernel widens the ramp without lowering it. What changes the amplitude is making the kernel
much **larger than the object**, so the object contributes its share of the kernel's *area*
instead of half a step. A 4-cell box inside a σ-4.7 kernel is a small fraction of it.

`Why that costs the feature almost nothing:` the case this exists for is *big*. A
half-dark/half-bright frame is an 8-cell feature, and a Gaussian this wide still passes
~90 % of an 8-cell step while passing ~30 % of a 4-cell box. Measured, on the split scene at
100 %: widening 1.4 → 4.7 cost the dark half's lift **34 → 28 codes** (a fifth) and removed
**three quarters** of the halo. The coarse 16×16 grid and this blur are the same decision
twice — the operator is deliberately incapable of resolving an object's outline, and just
capable of resolving which half of the screen you are in. That asymmetry *is* the design,
and it is why the slider's full 0..100 % range is shippable rather than needing a cap.

##### Measured: the split scene, off vs on

`scripts/effects-regression.sh`'s `halfsplit`, Dynamic, defaults, strength 1.0. "Local 0" is
the global curve — the same numbers this feature exists to fix. Captures:
`build-release/verify-shots/adaptive-local-2026-09-07/split-local-{0,50,100}.png`.

| Local adaptation | dark half 5 / 8 / 12 / 16 / 20 | bright half 200 / 215 / 230 / 245 / 255 | bright half's spread |
| --- | --- | --- | --- |
| effect off | 5 / 8 / 12 / 16 / 20 | 200 / 215 / 230 / 245 / 255 | 55 |
| 0 % (global curve) | 12 / 17 / 23 / 28 / 34 | 195 / 207 / 217 / 228 / 235 | 40 |
| 25 % | 14 / 20 / 27 / 34 / 39 | 182 / 191 / 199 / 207 / 212 | 30 |
| **50 % (default)** | **18 / 25 / 33 / 39 / 46** | **169 / 176 / 182 / 187 / 191** | **22** |
| 75 % | 22 / 31 / 39 / 47 / 53 | 157 / 163 / 166 / 170 / 172 | 15 |
| 100 % | 28 / 37 / 47 / 55 / 62 | 146 / 150 / 152 / 154 / 155 | 8 |

Both halves are serviceable from 50 % on: the dark half's darkest band goes from 12 (a value
that reads as black) to 18, and its top band from 34 to 46, while the bright half comes down
from "near white" to a light grey that still has 22 codes of internal separation across its
five bands. The `effects_ab_log` readback on the same frame shows what the operator is
actually doing: the smoothed map spans **24.7 .. 228.1 codes** and the per-pixel gain it
produces spans **3.600 .. 0.476** — a 7.6× range across one frame, entirely inside the user's
own `[0.3, 4.0]`.

**`Why the default is 50 % and not 100 %`**, decided from those captures and not from taste:
the second failure mode of any local operator is the opposite of a halo — pushing every
neighbourhood toward its own target flattens the contrast *inside* each one, and the last
column shows exactly that. At 100 % the bright half's five bands collapse to 8 codes apart
and the picture reads flat and grey; at 50 % they keep 22, more than half of what the global
curve left them, while the dark half is already visibly readable. 50 % is also where the
halo amplitude on the adversarial scene (+8 / −7 codes) is below what shows on a flat field.
100 % remains available and remains ring-free; it is a stronger look, not a broken one.
`effects-regression.sh` guards the collapse directly ("the bright half keeps ≥ 6 counts of
internal contrast").

##### Stability: it does not reintroduce the pulse

The [2026-09-07 pulse fix](#the-pulse-rank-cuts-on-a-bimodal-histogram-2026-09-07) set the
standard — a still frame must produce a *constant* output, peak-to-peak zero — and the local
map is held to it, at three strengths, by `stability-static` in the gate:

| Local adaptation | raw p98 p2p | smoothed p98 p2p | gain p2p | output pixel p2p |
| --- | --- | --- | --- | --- |
| 0 % | 0 | 0.038 codes | 0.00093 | **0** |
| 50 % | 0 | 0.000 codes | 0.000010 | **0** |
| 100 % | 0 | 0.000 codes | 0.000000 | **0** |

Under a **pan** (the same `--periodic` scene, whose *global* statistics are constant by
construction) the probe cell's gain does move: p2p **0.009 / 0.072 / 0.103** at 0 / 50 /
100 % over 300 frames. That is reported, not asserted, and it is the operator working
rather than a pulse: `--periodic` fixes the frame's histogram, not its *layout*, so a cell
genuinely sees different content as the texture scrolls past and its gain is supposed to
follow. The movement is a ~5 % drift over five seconds with the EMA's own time constant, not
a frame-to-frame flicker — and the still-frame column above is exactly zero, which is the
test that would catch a real oscillation.

##### Cost, stated plainly

- **Measure pass**: zero extra taps and zero extra texture fetches — the cell means are the
  per-thread sums it already computes. Added: four blur passes (each two half-passes of five
  shared-memory reads per thread, 16 barriers in total) and one `imageStore` per thread.
- **Apply pass**, and only when Local adaptation > 0: **four extra `texelFetch`es** of a
  16×17 texture per pixel, plus three `mix`es and one `pow`. The two `log`s were already
  per-pixel (the gain and gamma were never frame constants here), so the curve maths costs
  nothing new — it just gets different inputs.
- **Memory**: the history texture 8×1 → 16×17 `ABGR8888` (32 bytes → 1088), and the
  `effects_ab_log` staging copy the same.
- **There is still no GPU-timestamp instrumentation in `vulkan_composite()`**, so there is
  no measured microsecond figure for any of this, and none is claimed. What can be said is
  that the whole pre-pass is sub-millisecond on this desktop's GPU and that the added work
  is a few hundred shared-memory reads in one workgroup plus four cached fetches per pixel.

##### The white point (0.9) and the knee (0.7), reconsidered

The standing *"some things are still overblown"* complaint pointed at these two constants,
and they were re-examined against the new measurements. **Local adaptation makes them
materially less critical, and neither was changed.** The reason the bright reference scene
compresses hard is not the white point — it is the *global* gamma riding `GAMMA_MAX` 1.5
(see "Why min_gain 0.3, max_gain 4.0" above), and a local operator attacks that at its
source: a bright region now gets its own `r > 1`, its own lower gain and its own gamma
instead of one compromise for the frame. On the split scene the bright half's five bands
keep 22 codes of separation at the default where the global curve left them 40 and a
*stronger* global compression would have left them fewer.

Where a highlight control would still help is the case local adaptation cannot reach: a
frame that is uniformly bright, where every neighbourhood has the same level and `r ≡ 1` by
definition, so the local path is the global path. **A "Highlight headroom" control (the knee,
or the white point, exposed) is still worth having and is recommended — but not built here**,
and deliberately not built in the same change as the local operator, so the two can be judged
against each other rather than as one indivisible "it looks different now".

#### The Inspector's before/after preview (2026-09-07)

`requests-2026-09-08.md`: *"You could maybe add a small comparison picture to the
inspector rail, that is split in half (left/right) with one being the original image
(captured frame, when the UI was opened) and the one modified by the adaptive
brightness."*

Selecting the **Adaptive Brightness** row now puts a small 16:9 picture in the
Inspector's CONFIGURE page, above the VALUES block: **one frozen frame from the game,
split down the middle — the left half as the picture reaches Adaptive Brightness, the
right half with the effect applied at whatever the sliders currently say.** Moving a
slider re-grades the frozen frame, so a setting can be judged immediately instead of by
hunting for a scene that shows it. It is a **read**: nothing in it touches the
compositing path.

##### Where the pixels come from

Two things are captured, **on one composite, together**:

1. **The picture.** `src/shaders/cs_effects_preview.comp` — a new 256×144 compute
   dispatch — box-averages the base layer down to that size and runs
   `effects_common.h`'s **`grade()`** on every tap, so what it writes is *exactly the
   image `cs_effects_layer0.comp`'s Adaptive Brightness block is about to receive*:
   Shadow Control and Vibrancy already applied, in encoded space, on the same slot-0
   raw-UNORM binding the other two dispatches use. Pre-Sharpen is deliberately not
   applied (a 5-tap cross at source resolution is invisible after a ~7× downscale, and
   it does not move the statistics either).
2. **The statistics.** The `effectsHistory` texture `cs_effects_measure.comp` wrote for
   *that same frame* — mean, p2, p50, p98 and Local adaptation's 16×16 map — copied out
   beside the pixels.

`Why capture the statistics rather than re-estimate them on the CPU:` this is the whole
reason the right half agrees with a screenshot to the code value. The measure pass's
estimator is a 128×128 tap grid, a 64-bin histogram and rank-window means, followed by
an EMA (see [The statistics](#the-statistics-both-modes)); a CPU re-implementation of
all that would be a second definition to keep in step, and would differ from the shader
by whatever the two implementations disagreed about. Copying the history is exact and
free. It also satisfies "the stats come from the captured frame itself": both halves of
the capture are from one composite and are frozen together, so the preview never drifts
with what the live game is doing behind the UI.

The apply half is `src/Overlay/EffectPreviewMath.h`, which runs
**`effects_curve.h` — the same text the GPU compiles** — over the captured pixels: the
same `ab_local_shift` / `ab_dyn_gain` / `ab_dyn_gamma` / `ab_dyn_curve`, in the same
order, with the same clamps, plus a line-for-line port of `ab_local_sample()`'s
hand-rolled bilinear.

##### The refresh rule

- **A frame is captured when the strip appears on screen**, and only then. "Appears"
  is defined as *not drawn for 250 ms* — which covers the overlay being opened,
  another row or rail area being selected, the DETAILS page, the Inspector being
  hidden, and the strip being scrolled out of view. One rule for all of them, and it
  is also what makes a stale frame from an earlier session structurally impossible:
  nothing older than a quarter-second-before-it-appeared can be displayed.
- **The right half is re-graded when a parameter changes**, or when a new frame
  arrives. Nothing else redraws it; a still overlay costs nothing.
- **The capture is armed only while the effect's own switch is on**, so the preview
  never asks the compositor for work on a frame the pre-pass would not otherwise run.

##### Degradation

Every not-ready case draws a bordered box with one muted sentence in it, never a black
rectangle and never a stale picture. Ordered by what the user can act on:

| Case | What is shown |
| --- | --- |
| Base layer is not SDR RGB | "Preview unavailable: these effects only run on an SDR picture." |
| Adaptive Brightness switched off | "Turn Adaptive Brightness on to preview it." |
| Armed, no frame yet (no game, HDR frames, allocation failed) | "Open the overlay over a game to preview." |

`Why not-SDR outranks switched-off:` the switch is greyed out under the `kSdrOnly`
reason in that state, so "turn it on" would be advice the user cannot take. A texture
allocation failure disarms rather than retrying every composite, and a resolution
change cannot break anything: the capture is a fixed 256×144 downscale, so the strip's
size never depends on the game's.

##### Measured: the right half is the effect, not an impression

`build-release/verify-shots/ab-preview-2026-09-07/`, headless, `pixel-regression.sh`'s
recipe, `tests/effects_scene_client.c` as the game, Adaptive Brightness alone (the other
three off), Dynamic, schema defaults. The strip is sampled in the capture and compared
against a `gamescopectl screenshot "<p> 4"` of the *same scene with the effect on and
the overlay closed*:

| Scene | Preview, left half (before) | Preview, right half (after) | The real effect, same scene | Error |
| --- | --- | --- | --- | --- |
| `dark`, bands 5 / 8 / 12 / 16 / 20 | 5 / 8 / 12 / 16 / 20 | **71 / 90 / 111 / 128 / 143** | 71 / 90 / 111 / 128 / 143 | **0 counts** |
| `halfsplit`, bright half 200…255 | (its own dark half) | **196 / 207 / 218 / 229 / 236** | 196 / 207 / 218 / 229 / 236 | **0 counts** |
| `texdark` (a textured frame), mean | 24.9 | **124.1** | 121.1 | 3.0 counts |

The two flat scenes are exact to the code value. `texdark`'s 2-count difference is the
one honest inexactness and it is inherent to previewing a *downscaled* frame: the strip
grades the box mean of each 7×5 source block, while the real frame grades each source
pixel and is then averaged by the measurement — `curve(mean(x))` against `mean(curve(x))`
on a non-linear curve. It is a property of the thumbnail, not of the maths, and at 3
counts on the hardest content the test client has it is below what a user can see.

Two more behaviours measured in the same run, both from the strip itself:

- **Strength → 0 makes the right half byte-identical to the left** (5/8/12/16/20 on both
  sides) — the slider-change proof, and the identity property asserted on the CPU as
  well.
- **Local adaptation 0 → 50 % moves the right half on `halfsplit`** (196/207/218/229/236
  → 173/181/187/192/195) and **does not move it at all on `dark`** — correct in both
  cases: a uniform frame has `r ≡ 1` by construction, so there is nothing local to
  adapt to.

##### Cost, stated plainly

- **The capture**: one 256×144 dispatch (36,864 invocations, 16 texelFetches each), two
  small image copies, and one `g_device.wait()` — the same one-GPU-frame stall
  `effects_ab_log` takes, paid **once per time the strip appears**, not per frame. Every
  other frame the pipeline is never bound.
- **The re-grade**, measured on this desktop (`-O2`, 256×144, the right half only —
  18,432 pixels):

  | Case | Per update |
  | --- | --- |
  | Whole image | **32 µs** |
  | Dynamic, Local adaptation 0 | **47 µs** |
  | Dynamic, Local adaptation > 0 | **2.1 ms** |

  The first two are cheap because the effect is then a pure function of one byte — gain
  and gamma are frame constants — so `Compose()` evaluates the curve **256 times** into
  a table instead of 55,296 times. The table is built by calling the *same*
  `ApplyPixel()`, and `test_overlay_ui.cpp` asserts the two paths are equal for every
  byte value in both modes, so it is a speed-up and not an approximation.

  With Local adaptation on, every pixel has its own gain and the table cannot apply, so
  it is ~2.1 ms — six transcendentals per pixel, run on the steamcompmgr thread inside
  the overlay's own draw. **That is the honest worst case**, and it is bounded: it
  happens only on frames the overlay is *already* redrawing, only when a value actually
  changed (a still overlay recomposes nothing), and only in Dynamic mode above 0 %
  Local adaptation. No throttle was added on purpose — throttling risks the strip
  showing the *previous* slider position if the overlay then stops redrawing, and a
  preview that lies is worse than a 2 ms frame. If it ever needs to be faster, the
  table generalises by quantising the local ratio, at the price of the exactness the
  table has today.
- **Memory**: a 256×144 storage texture plus two host-mappable staging copies (147 KB +
  147 KB + 1 KB), created on the first capture and kept.

##### What it deliberately does not show

- **Only Adaptive Brightness.** Shadow Control and Vibrancy are baked into *both* halves
  by the capture's `grade()` call, so the strip isolates this effect — which is what a
  before/after for this row should do.
- **The same frame on both sides, not two crops.** The divider is the midpoint of one
  picture, so a feature straddling it shows its own before and after touching each
  other. On a scene that is itself spatially split (`halfsplit`), that means each side
  of the strip shows a different part of the frame — inherent to the shape the request
  asked for.
- **Not 16:9 content.** The capture is a fixed 256×144, so a 16:10 or ultrawide frame is
  stretched to the strip's shape rather than letterboxed. Tone, which is what the strip
  is for, is unaffected.

##### The size, and what it costs the params

The block is the Inspector's content width capped at **240 logical px** (a 135-px
picture plus a 14-px label line and a 4-px gap: 153 px total). `Why capped, and why
there:` the strip is drawn *above* the params so it is on screen while they are reached
for, which means every pixel of it pushes them down — and at 1280×720 this row's eight
params already filled the Inspector body exactly. Measured at that size: 320 px wide
(the first size tried, measured during this task and not kept) left three params visible;
240 leaves four, and is still legible enough to judge tone on.

**The honest limitation**: at 1280×720 the CONFIGURE page scrolls, and scrolling down to
Min gain or Local adaptation takes the strip off the top of the page with it — so the
lowest params cannot be dragged while watching the preview. At 2560×1440 everything is
on screen together. Pinning the strip above the Inspector's scrolling body would fix it
and is the obvious follow-up; it was not done here because it is a change to
`DrawInspector()`'s region split, with nothing to do with this feature's own risk.

#### The pulse: rank cuts on a bimodal histogram (2026-09-07)

`requests-2026-09-08.md` item 6: *"For the dynamic mode, sometimes the whole image
pulsates for some reason."* Found by measurement, not argument — the `effects_ab_log`
readback (below) was built for it and is what the numbers here come from
(`build-release/verify-shots/adaptive-pulse-2026-09-07/`, `results.txt` has every run).

**What it was not.** A completely still frame (`tests/effects_scene_client.c`'s `texdark`,
no motion) logged for 400 composites at both `max_gain` 2 and 4: raw and smoothed
statistics, gain, gamma and the output pixel **identical every frame** — peak-to-peak
zero, to the last float bit. That rules out the feedback-loop hypothesis outright (the
measure pass reads `layer0.tex`, the game's own buffer, and the pre-pass block is guarded
against ever seeing `effectsOutput` or a pre-upscaled texture; the log confirms the loop
is open), and with it any limit cycle of the EMA, the gain/gamma coupling, the shoulder
or the shadow cap switching: nothing internal moves without the input moving.

**What it was.** A percentile is a rank *cut*: the value of the k-th brightest tap. On a
histogram with an empty stretch — a dark scene with a few lights, a half-sky/half-ground
view: precisely the scenes Dynamic exists for — the cut sits in the gap, and **one tap**
moving from one mode to the other flips it from the top of the lower mode to the bottom
of the upper one. Under camera motion the tap grid lands on different pixels every frame,
so the count straddling the cut jitters by ±√(N·p·q) ≈ 9 taps and the cut telegraphs
between the two modes at random. The EMA cannot hide a square wave of that size; it turns
it into a slow wander, which the gain (`0.9 / p98`) and the gamma (from `p50`) turn into
the picture breathing. Measured on `texdark` with **2 %** light cells (so the 98th
percentile sits in the gap) panning at 3 px/frame, 340 frames:

| | raw p98 p2p | smoothed p98 p2p | gain p2p @ max 2 | gain p2p @ max 4 |
| --- | --- | --- | --- | --- |
| rank cut, 64×64 taps (before) | **107 codes** (0.217 ↔ 0.636) | 16.8 codes | 0 (pinned at 2.0) | **0.81** (3.19..4.00, 25 %) |
| rank-window mean, 64×64 | 46 codes | 9.6 codes | 0 (pinned) | 0.22 (2.16..2.38) |
| rank-window mean, 128×128 (after) | 36 codes | 17.6 codes* | 0 (pinned) | 0.45* |
| … same, `--periodic` (true statistics constant) | 11 codes | **0.6 codes** | — | **0.013** (2.24..2.25, 0.6 %) |

And the same cliff on the **median**: `texsplit` 50/50 panning, rank cut: raw p50 p2p
**98 codes** (0.214 ↔ 0.599), smoothed 33 codes, **gamma 0.86..1.18** with the gain not
moving at all — the midtones alone breathing between a lift and a darkening. After: raw
7.5 codes, smoothed 2.5, gamma p2p 0.02; periodic: smoothed 0.15 codes, gamma p2p 0.0013.

\* The non-periodic pan is not a fair "noise" number: the texture scrolling in at the
frame edge changes the scene's true light population, which the effect is *supposed* to
follow, and the window mean now follows it continuously instead of in jumps. The
`--periodic` row (the same texture repeating every frame width, so a pan changes nothing
but where the taps land) is the sampling noise alone — and that is what the tap count
buys: 64×64 → 128×128 halves the raw spread (26.8 → 11.3 codes, the expected 1/√4) and
the smoothed gain's (0.025 → 0.013). At `max_gain` 2 the dark scene sits on the clamp and
the pulse was invisible; **at 4.0 it is worse than what the user saw**, since a 25 %
swing at gain 4 is the whole picture moving a stop — the widening landed the same day, so
the report predates it and the fix was measured at both.

**The fix**, in `cs_effects_measure.comp` only: the three percentiles became the
rank-window means described above (`histogram_window_mean()`, from a histogram of counts
and fixed-point value sums), and the tap grid went 64×64 → 128×128 (four times the
fetches in the same single workgroup; still nowhere near a millisecond). The EMA, its
asymmetric time constants, `dt`, the curve and the history contract are untouched, so
**adaptation is exactly as fast as before** — the trade-off chosen is *no* added lag: the
residual (a ≤ 1 % gain wander with the EMA's own correlation time on the pathological
scene, zero on a still one) was judged below notice, while a deadband or a longer smoothing
would have been visible as steps or sluggishness on every real transition. `Why not more
taps still:` on spatially coherent content the taps are not independent samples (the
16-px cells of the test texture already show 128×128 not gaining the full 2× on the
non-periodic pan), so past this point taps buy little; the estimator's continuity is what
mattered. `Why the same numbers on the reference scenes:` see the statistics section —
the flat scenes have no gap, so `effects-regression.sh`'s dark/bright/mid tables above
are unchanged to the code value by this fix.

**Guarded by** three new `effects-regression.sh` checks, all on the per-frame readback:
`transition-gain` (monotone, no overshoot, settling time reported), `stability-pan` (the
2 %-lights scene, periodic, panning: gain p2p ≤ 0.06, smoothed p98 ≤ 3 codes, raw p98
≤ 40 codes — the rank cut read 0.81 / 16 / 107) and `stability-static` (the same scene
held still via `SIGUSR2`: raw p98 p2p **exactly 0**, output pixel exactly 0, smoothed p98
< 0.1 code and gain < 0.002 — the EMA's last fraction of a percent of convergence after
the pan stops). Measured on the final run: 0.025 / 1.16 / 11.3 and 0 / 0 / 0.013 / 0.0003.

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

`g_output.effectsHistory` is a **16×17 `ABGR8888` storage+sampled texture**.
**Row 0** is one texel per smoothed statistic — `HISTORY_MEAN`, `_P2`, `_P50`, `_P98` —
plus, from `HISTORY_RAW` = 4, the same four unsmoothed, for the readback only.
**Rows 1..16** are Local adaptation's 16×16 map of smoothed local mean luminances
(`AB_LOCAL_ROW`, `AB_LOCAL_GRID`); `kEffectsHistoryWidth`/`Height` on the host mirror the
shader's `HISTORY_TEX_W`/`_H`. It is created once by `update_effects_history()` and kept
for the life of the output (it does not depend on the game's size, and its contents *are*
the effect's state — so, unlike the `.fx`, a resolution change does not reset it). It was
1×1 until Dynamic mode (2026-09-06) needed the percentiles, 4×1 until the pulse
investigation (2026-09-07) wanted measured next to smoothed, and 8×1 until Local adaptation
the same day. `Why the map rides in the same texture:` `dst` in `descriptor_set.h` is a
single `rgba8` target and `dispatch()` binds one, so a second storage image would mean
teaching the shared descriptor set a second target for the sake of one pass — where one
extra sampler-slot-free texture row costs nothing and keeps both dispatches on one bind.

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
safe:` row 0's texels are touched by invocation 0 alone, and its fetches precede its stores
in program order; every *other* invocation touches exactly one local-map texel, its own,
and reads it before writing it, so no two invocations address the same texel either way.
The layout is `GENERAL` for both bindings. The `.fx` did the same.

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

### Adaptive Gamma (`image.shaders.adaptive_gamma`) — NEW 2026-09-08

The user's request, verbatim: *"Make something similar, but make it gamma based. Call it
adaptive gamma."* — "similar" meaning
[Adaptive Brightness](#adaptive-brightness-imageshadersadaptive_brightness) above.

Built as the **exponent-only** operator that description implies: the same measured
statistics, and the whole effect is *one exponent*. No levels gain, no white point, no
shoulder, no shadow cap. It is not a cheaper Adaptive Brightness — it is a different
thing that a gain cannot be, for a reason that is arithmetic rather than taste (below).

**Config**: `ReshadeAdaptiveGammaSettings` (`ConfigSchema.h`) — `enabled` (false),
`target_luminance` (0.5), `max_lift` (4.0), `max_darken` (1.5), `strength` (1.0),
`local_strength` (0.0). Purely additive keys, so `kCurrentSchemaVersion` stays **4** and
there is no migration: an old profile has none of them and takes these compiled-in
defaults, exactly the shape [Shadow Control](#shadow-control-imageshadersshadow_lift) was
added in.

#### The formula

`src/shaders/effects_curve.h`'s `ag_*` block — the same header the GLSL pass and
`tests/test_effects_curve.cpp` both compile, so the properties below are asserted on the
code the GPU runs. Encoded space in, encoded out, per channel:

```
gmin = clamp(1 / max_lift,  0.25, 1.0)          // the user's own bounds
gmax = clamp(max_darken,    1.0,  4.0)
r    = ab_local_shift(local_mean, mean, local_strength)   // exactly 1.0 when off
g    = clamp( ln(target) / ln(p50 * r), gmin, gmax )
out  = mix(x, x^g, strength)
```

`g` is the exponent that lands the smoothed **median** on Target. Median, not mean, for
the reason Dynamic mode gives: a few bright windows in a dark room must not read as "the
room is lit".

#### Why an exponent alone is a different effect

For `x` in `[0, 1]` and any `g > 0`, `x^g` is again in `[0, 1]`, with **0 and 1 as exact
fixed points** and strictly increasing in between. Three consequences, and each is why a
piece of Adaptive Brightness's machinery is *absent* here rather than merely omitted:

- **It cannot clip, at any setting** — so it needs no shoulder, no knee and no white
  point. Adaptive Brightness needs all three because its levels gain multiplies and can
  push a value past 1.0; there is nothing here to protect the highlights *from*. Measured:
  on the dark reference scene the 240 highlights read **251** at the defaults and never go
  above 251 at any setting tried, while Whole-image mode drives that same region to a
  **clipped 255**. A near-white value stays near-white *and still distinct from white*.
- **It changes contrast, not exposure.** A gain moves every value by the same factor; an
  exponent moves the mid-tones a lot and the two ends not at all. So the picture's black
  and white points are untouched by construction and only the shape between them adapts.
  That is a different look — flatter blacks stay black, which on a dark game is either
  exactly what you want (no milky lifted blacks) or not enough (the deepest shadows are
  never raised off the floor). The doc's job is to say which it is, not to pretend the
  trade does not exist.
- **It is cheaper**: one `log` and one `pow` per channel, no gain, no shoulder branch, and
  with Local adaptation off the exponent is a frame constant.

#### The bounds are the user's own — and why that is the whole point

Target reaches the picture **only** through this exponent. So whatever clamps the exponent
also decides where Target stops working — and the most expensive lesson of 2026-09-08 was
that [a clamped slider looks exactly like a working one](#the-ceiling-target-brightness-and-max-gain-went-inert-2026-09-08).
With a fixed `[0.5, 1.5]` — the constants Adaptive Brightness used to carry — Target on
this row would go inert above `sqrt(p50)` with nothing on screen saying so and no control
to reach past it. That is the exact bug, rebuilt.

So the two bounds are **params**, `Max lift` and `Max darken`, and the header's hard
clamps are the panel's own `Range()` ends (`AG_LIFT_MAX` / `AG_DARKEN_MAX`, both 4.0) —
asserted equal in `test_effects_curve.cpp`, so a slider always reaches the bound it says
it does and there is no second, hidden ceiling underneath it.

- **Max lift** `L` sets the exponent **floor** to `1 / L`. `1.0` means "do not brighten",
  exactly (pinned by test at every target on every scene). The default **4.0** gives a
  floor of 0.25 — the same floor `AB_DYN_GAMMA_MIN` allows Adaptive Brightness at
  `max_gain` 4.0, one shared constant with one justification: below a fourth-root lift a
  single code of near-black lands above 90 and sensor noise is the whole picture.
- **Max darken** `D` **is** the exponent ceiling. `1.0` means "do not darken", exactly.
  The default is **1.5**, not 4.0: a darkening exponent crushes shadows by nature and this
  operator has no shadow cap to hold them up (that is Adaptive Brightness's `min_gain`,
  which has no counterpart here), so the shipped default stops where Adaptive Brightness's
  own `GAMMA_MAX` stops. Measured cost of going past it, bright reference scene: at
  `max_darken` 1.5 the 30-value shadows read **10**; at 3.0 they read **0**. Ordering is
  kept either way (the gate asserts it), but detail is genuinely gone — available on
  purpose, not a default.

**The residual ceiling, stated plainly.** Because the exponent is the only mechanism, the
highest target it can reach on a frame is **`p50 ^ (1 / max_lift)`**. On the textured dark
scene (`p50` = 0.076) with Max lift at the top of its slider that is **0.52**, so Target
above about 0.5 does nothing there — measured: 0.2 / 0.35 / 0.5 move the frame mean
**55.1 → 91.3 → 128.0**, and 0.7 produces the same picture as 0.5 with the readout saying
`Max lift -- Target brightness does no more here`. Adaptive Brightness gets further on the
same frame because its gain carries what the exponent cannot; that is the honest price of
"only an exponent", and it is *named* rather than silent.

#### Which limit is binding

`ag_binding()` / `ag_binding_text()`, the same two-part pattern
[`ab_dyn_binding()`](#which-limit-is-binding-2026-09-08) uses — codes in the shared header
because it compiles as GLSL too, wording on the C++ side — so the panel and the
`effects_ab_log` trace print one string and cannot drift:

| code | shown as |
| --- | --- |
| `AG_BIND_NONE` | `none -- the mid-tones are on Target brightness` |
| `AG_BIND_LIFT` | `Max lift -- Target brightness does no more here` |
| `AG_BIND_DARKEN` | `Max darken -- Target brightness does no more here` |
| `AG_BIND_STRENGTH` | `Strength is 0 -- nothing is applied` |

Every control this row owns can be inert, and each of those states has a code: Target when
the exponent is clamped (LIFT / DARKEN); Max lift and Max darken when the target is
already reached (NONE — "nothing is stopping the picture", so raising the limit that is
not binding buys nothing); Strength at 0, which makes *all* of them inert and is the one
case a limit code would blame the wrong control for. `Why a fourth code rather than
reusing Adaptive Brightness's six:` the wordings name controls, and this row has no "Max
gain" or "Min gain" — pointing a user at a slider that is not on the page is precisely the
near-miss the readout exists to prevent. `test_effects_curve.cpp` asserts the equivalence
the readout claims: whenever it says a limit binds, a further step of Target really does
change nothing, and whenever it says NONE, Target really does still move the picture.

The line appears in both places Adaptive Brightness's does — appended to every
`effects_ab_log` line as `bind=<n> (<text>)`, and as the Shaders area's **Pipeline** facts
row `adaptive limit`. That row now serves **both** adaptive effects and never has to
choose between them, because they cannot both be on.

#### Local adaptation — included, and it is the same operator

`local_strength` reuses [`ab_local_shift()`](#local-adaptation-local_strength-2026-09-07--one-curve-per-neighbourhood)
verbatim: the measure pass's 16×16 map, the `r = clamp(local/mean, 0.25, 4)` deviation
clamp, the `r^strength` geometric blend, the hand-rolled bilinear sample. Only what the
shifted median feeds changes — `ag_gamma(p50 * r_eff, …)` instead of the gain/gamma pair.

`Why include it at all, on an effect whose selling point is simplicity:` the map costs the
measure pass **nothing** (it is the per-thread tap sums it already computes), the apply
pass pays four cached `texelFetch`es *only above 0*, and without it a half-dark /
half-bright frame is unfixable by any single exponent — the same failure that made local
adaptation necessary for Adaptive Brightness. Reusing the existing operator means one
definition of "how bright is this part of the frame", one halo argument, and one set of
measurements; inventing a second would be two things to keep true.

`Why the default is 0.0, where Adaptive Brightness defaults to 0.5:` this effect's whole
identity is the cheap, purely global exponent, so that is what it should be out of the
box — and it makes the shipped default exactly the operator the user asked for, "only an
exponent", with no per-pixel work at all. Local adaptation is available and opted into.
The consequence, stated rather than buried: **on a mixed-brightness frame Adaptive Gamma
does nothing about the mix until you raise this slider**, and on an evenly-lit frame the
slider does nothing at all (every cell's ratio is 1 by construction — a no-op that is
correct, not broken, and is why it is not classified as a "binding limit" above).

Bounded exactly as Adaptive Brightness's is: `ag_gamma` clamps *after* the shift, so a
locally-adapted pixel can never leave `[1/max_lift, max_darken]` — local adaptation
redistributes inside the user's bounds and never widens them. Asserted over every
scene × bound × ratio × strength combination on the CPU.

#### Adaptive Gamma vs Adaptive Brightness: mutually exclusive

**Decided and enforced, not left undefined.** Both effects aim the frame's mid-tones at a
Target, and both read the **same** statistics — the measure pass grades its taps but knows
nothing about either effect, so its `p50` is always the *pre-effect* median. Run together,
the second operator fits its curve to a median the first has already moved and the
correction is applied **twice**. On the textured dark scene that is not "both, a bit": the
frame Adaptive Brightness settles at a mean of ~129 would be taken to about **212** by
Adaptive Gamma's exponent (0.269) on top of it — a washed-out picture no setting of either
effect fixes.

So turning either switch on turns the other off (`PanelShaders.cpp`'s
`SetAdaptiveBrightnessEnabled` / `SetAdaptiveGammaEnabled`), and `EffectsPushData_t` drops
Adaptive Gamma's flag and neutralises its uniforms whenever Adaptive Brightness is also
on, so a **hand-edited config** with both cannot produce the double correction either
(Adaptive Brightness wins: it is the older, richer effect and the one every existing
config, capture and doc points at).

`Why a radio and not a greyed-out switch:` greying Adaptive Gamma out while Adaptive
Brightness is on leaves a config that somehow has both **stuck** — neither row toggleable
— and greying only one of the pair is an asymmetry a user cannot infer. Turning one on
turning the other off is a familiar interaction and it is *visible*: both switches sit in
the same Effects band, so the other one goes dark in the same frame. `Why not compose them
and document the result:` the composed result is not a look anybody would choose, and
"both on does something surprising" is exactly the class of undefined behaviour this
project keeps having to go back and fix.

Measured (`effects-regression.sh`'s `ag-exclusive`, driven through `overlay_e2_set`, i.e.
through the same setter a click uses): with Adaptive Gamma on, turning Adaptive Brightness
on leaves `image.shaders.adaptive_gamma` reading **off**; turning Adaptive Gamma back on
leaves `image.shaders.adaptive_brightness` reading **off**. The three captures either side
of those flips read frame means **128.2 | 129.1 | 128.3** — one effect's picture or the
other's, and back again, never the ~212 a compounded frame would be.

#### The Inspector's before/after preview

The row declares the **same** `PreviewKind::AdaptiveBrightness` strip, deliberately: the
two effects are mutually exclusive, so only one of them can ever be the thing being
previewed, and `EffectPreview.cpp` picks whichever is on. That made extending it cheap —
`EffectPreviewMath.h`'s `Params` gained `bGamma`, `flMaxLift` and `flMaxDarken` and
`ApplyPixel()` gained one branch that calls `ag_gamma` / `ag_curve`; nothing in
`Shell.cpp`, `Registry.h` or the capture pass changed, and the 256-entry LUT fast path
applies unchanged (Adaptive Gamma's exponent is a frame constant whenever Local adaptation
is off, so `IsUniform()` is true and a re-grade is ~45 µs rather than ~2 ms). The
placeholder sentence for "switched off" now names both effects.

#### Measured (desktop, headless, `scripts/effects-regression.sh`, 2026-09-08)

Same recipe as everything above (private headless sway, nested `gamescope --backend
wayland`, `gamescopectl screenshot "<path> 4"`, `tests/effects_scene_client.c` as the
game). Adaptive Gamma alone, every other effect off, defaults unless stated. Captures:
`build-release/verify-shots/adaptive-gamma-2026-09-08/`.

| dark scene / region (input) | Off | Whole image | Dynamic | **Adaptive Gamma** |
| --- | --- | --- | --- | --- |
| bands 5 / 8 / 12 / 16 / 20 | 5/8/12/16/20 | 20/32/48/64/80 | 88/107/127/143/157 | **95 / 107 / 119 / 128 / 135** |
| 240 highlights | 240 | **255 — clipped** | 254 | **251** |
| pure black | 0 | 0 | 0 | **0** |

The dark scene is where the difference in *shape* is visible rather than argued: Adaptive
Gamma's five bands span 40 codes where Dynamic's span 69 — the exponent flattens the very
bottom of the range as it lifts it, because a toe that steep has less slope left over —
while the highlights come out two counts lower than Dynamic's and nine below white. Whole
image is the blown-out case both exist to avoid.

| setting (dark scene) | bands | 240 highlights |
| --- | --- | --- |
| defaults (target 0.5, lift 4.0, darken 1.5, strength 1.0, local 0) | 95 / 107 / 119 / 128 / 135 | 251 |
| Target 0.3 | 53 / 64 / 75 / 84 / 92 | 249 |
| Target 0.9 | 95 / 107 / 119 / 128 / 135 (**identical** — `Max lift` binds) | 251 |
| Strength 0.5 | 50 / 58 / 65 / 72 / 77 | 246 |
| Max lift 1.5 | 19 / 25 / 33 / 40 / 47 | 245 |
| Max darken 4.0 | 95 / 107 / 119 / 128 / 135 (identical — slack on a dark scene) | 251 |
| Local adaptation 1.0 | 95 / 107 / 119 / 128 / 135 (identical — this frame is evenly lit) | 251 |

| sweep | frame mean |
| --- | --- |
| Max lift 1.5 → 2 → 3 → 4 (dark chart, where it binds at every setting) | 34.7 → 55.7 → 90.9 → **116.9** |
| Target 0.2 → 0.35 → 0.5 (textured dark, continuous histogram) | 55.1 → 91.3 → **128.0** |
| Strength 0 → 0.5 → 1.0 (textured dark) | 25.7 → 77.0 → **128.4** |
| Max darken 1.0 → 1.5 → 2 → 3 (bright scene, where Target is pinned) | 221.2 → 209.3 → 199.1 → **181.5** |

| bright / mid scene | Off | **Adaptive Gamma** |
| --- | --- | --- |
| bright: bands 200 / 215 / 230 / 245 / 255 | 200/215/230/245/255 | **177 / 197 / 218 / 240 / 255** |
| bright: 30 shadows | 30 | **10** (and **0** at Max darken 3.0) |
| mid: 26 / 77 / 128 / 179 / 230 | identical | **25 / 76 / 127 / 178 / 230** (near-identity, worst 1) |

**No clipping, proved rather than asserted.** `effects-regression.sh`'s `noclip-*` checks
sample seven settings on the dark scene and two on the bright one and require, every time:
each band at least 2 counts above the one below it (nothing compressed into its
neighbour), the 240 highlights below 254 *and* above every band, pure black ≤ 2, and — on
the bright scene, whose top band already **is** 255 — the 245 band strictly below it. All
PASS. Note the one place the property genuinely does not extend: the **shadow** side has no
cap, so a high `Max darken` can take a deep shadow to 0 (the bright scene's 30-rectangles
at `max_darken` 3.0). Ordering survives; detail does not. That is a documented trade of
having no `min_gain`, not a clip, and the gate asserts the ordering rather than pretending
the darkening is free.

**Stability, to the standard the [2026-09-07 pulse fix](#the-pulse-rank-cuts-on-a-bimodal-histogram-2026-09-07)
set.** `ag-stability-static` / `ag-stability-pan`, 300 frames each off the same
`effects_ab_log` readback, watching the **exponent** (this effect's entire state) where the
Adaptive Brightness checks watch the gain:

| | raw p98 p2p | smoothed p98 p2p | exponent p2p | output pixel p2p |
| --- | --- | --- | --- | --- |
| still frame, Local 0 % | **0** | 0.015 codes | 0.00006 | **0** |
| still frame, Local 100 % | **0** | 0.000 codes | 0.000000 | **0** |
| panning (`--periodic`), Local 0 % | 11.3 codes | 0.59 codes | 0.00009 | 147 (the probe pixel moves with the texture) |

A still frame is exactly constant — peak-to-peak zero on the raw measurement and on the
output pixel — and the panning case's exponent moves by nine hundred-thousandths, three
orders of magnitude below what the pulse looked like. Nothing here is new machinery: the
estimator and the EMA are shared with Adaptive Brightness, which is why a regression in
either fails both effects' checks at once.

**The settings audit.** `scripts/settings-audit.sh` covers all six new rows in all
three routing situations (400 settings, 300 passed, 0 failed, 100 not covered). It took
one change to the audit itself, recorded here rather than buried: the first run reported
three FAILs, one per situation, all of them the same thing — setting
`image.shaders.adaptive_gamma` on also writes `reshade.adaptive_brightness.enabled`,
which the audit's "nothing else moved" rule correctly flags as a collateral write. That
write **is** the exclusion working, so it was declared in `scripts/settings_audit.py`'s
existing `SIBLING_KEYS` table ("a row whose own write legitimately moves a second key" —
the mechanism the Resolution rows' locked aspect ratio already uses), in both directions.
`Why declare rather than redesign:` the alternative that avoids the cross-write is greying
the switch, which is the shape rejected above; and an audit that reports the same
intentional write as a bug on every future run trains people to ignore it.

**Row count and the parameter budget.** The Effects band went from 5 switch rows to
**6**. Adaptive Gamma owns **5** params (Strength, Target brightness, Max lift, Max
darken, Local adaptation) against `kParamBudget`'s 8 — comfortably inside it, and
deliberately: it has fewer knobs because it has fewer mechanisms (no gain to bound, no
shadow cap, no mode). Adaptive Brightness is still the only row in the registry above two
params, and the budget was **not** raised for this change. The Effects band's row count
went from 6 to **7** the same day, when [Bloom](#bloom-imageshadersbloom--new-2026-09-08)
landed after it.

## The settings-panel budget

Each row may own at most **eight** `Param`s before `Registry.cpp` aborts registration —
raised from six 2026-09-06 (request #17, see the
[Adaptive Brightness budget decision](#the-budget-decision-seven-params-not-six-2026-0607)
for the evidence and the why) and from seven 2026-09-07 (Local adaptation, below). See
`PanelShaders.cpp`'s "THE SIX BUDGET" comment and `Registry.cpp`'s `kParamBudget`. Counts:
Saturation 2, Vibrancy 1 (new 2026-09-08), Pre-Sharpen 1, Bloom 3 (new 2026-09-08),
Adaptive Brightness 8 (zero headroom), Adaptive Gamma 5 (new 2026-09-08), Shadow Control
1. **The budget was not raised again** for Adaptive Gamma or for Bloom and did not need
to be — see each effect's own section for why five and three params are their honest
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

## Diagnostics

The **Pipeline** Facts row keeps "base layer" (the SDR gate) and says the effects are built
into the binary. The "effect file" / "compiled" / "loaded from" / "uniforms" rows added
2026-09-05 for the stale-file case were removed the same day along with the failure they
diagnosed.

**`effects_ab_log [frames] [x y]`** (ConCommand, `rendervulkan.cpp`, 2026-09-07): prints
Adaptive Brightness's state for the next N composites, one line each on `console_log`
(so `gamescopectl effects_ab_log 300` shows it live, and it lands in gamescope's log):

```
ab_log n=<i> t=<ms> dt=<ms> <off|whole|dynamic> raw mean=… p2=… p50=… p98=…
       smooth mean=… p2=… p50=… p98=… gain=… gamma=… px(<x>,<y>)=<r>,<g>,<b>
       local=… lmin=… lmax=… lprobe=… gainlo=… gainhi=…
```

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

## Related links

- [reshade-effects](reshade-effects.md) — the ReShade loader, now for third-party `.fx`
  files only.
- [compositing-vulkan](compositing-vulkan.md) — where the pre-pass sits in
  `vulkan_composite()`.
- [scaling-filters](scaling-filters.md) — the built-in FSR/NIS path whose RCAS this reuses.
- `superdoc/planning/DECISIONS.md` #12 (two sharpen controls), #15 (SDR-only), #27 (the
  native port; #13/#14 superseded).
- `superdoc/planning/requests-2026-09-04.md` items #2 and #3 — Vibrancy's range and
  Shadow Control; `requests-2026-09-06.md` item #16 — Adaptive Brightness's modes;
  `requests-2026-09-07.md` item 7 — the mode moved into the Inspector and the Six
  Budget raised to 7.
- `scripts/effects-regression.sh` — the headless measurement gate for Adaptive
  Brightness's both modes, and (2026-09-08) the `colors` scene pinning Saturation and
  Vibrancy against their closed-form formulas plus the `ag-*` / `noclip-*` checks pinning
  Adaptive Gamma's sliders, its no-clipping property, its stability and the exclusion
  with Adaptive Brightness, plus the `bloom-*` checks pinning Bloom's three sliders to
  three different statements about a line profile, its own no-clip property and its
  frame-to-frame stability under a pan.
- `superdoc/planning/requests-2026-09-08.md` item 6 — the pulse: measured, found, fixed;
  and the Local adaptation item — the split-scene, halo and gain-sweep evidence.
