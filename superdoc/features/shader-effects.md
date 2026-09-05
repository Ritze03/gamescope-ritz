# Shaders settings area — Vibrancy, Shadow Control, Pre-Sharpen, Adaptive Brightness

The overlay's **Shaders** area (`image.shaders`, `src/Overlay/PanelShaders.cpp`) exposes
four independent effects, all implemented as gated passes inside one combined ReShade
effect file, `reshade/Shaders/gamescope-ritz.fx`. This page covers what each effect does
and, in depth, the two settings added/changed 2026-09-04 (planning batch
`requests-2026-09-04.md`, items #2 and #3): **Vibrancy**'s range and **Shadow Control**, a
new effect.

**Titles vs. identifiers.** The three multi-word switches were retitled 2026-09-05:
"Shadow lift" → **Shadow Control**, "Adaptive brightness" → **Adaptive Brightness**,
"Pre-sharpen" → **Pre-Sharpen**. `Why only the titles moved:` an entry id is what the
command palette's saved entries and every cross-reference resolve against, and a config
key is what a user's `global.json` already contains — renaming either would break
existing configs and palette state for a purely cosmetic change. So the id
`image.shaders.shadow_lift`, the config struct `ReshadeShadowLiftSettings`, and the
uniform names `shadow_lift_enabled` / `shadow_lift_strength` all deliberately keep the
old spelling. Expect the code and this page to say "shadow lift" where it means the
identifier and "Shadow Control" where it means the label.

This is the settings-panel/effect-content layer. For how the underlying compile/upload/
uniform-push mechanism works (the generic `gamescope_reshade` protocol, pipeline caching,
etc.), see [reshade-effects](reshade-effects.md) — that page is the mechanism, this one is
what's actually loaded through it.

## Why one combined `.fx` file, not one per effect

`gamescope-ritz.fx`'s own header comment (and `superdoc/planning/DECISIONS.md` #13)
explains this in full: gamescope's ReShade manager recompiles the *entire* pipeline —
full parse + SPIR-V codegen + Vulkan pipeline build — inline on the render thread
whenever the active effect changes. Four separate techniques a user "switches between"
would mean every checkbox toggle pays that cost. Instead there is one always-loaded
technique with four independently-gated passes/branches; toggling an effect is a single
uniform write. `PanelShaders.cpp`'s `EnsureEffectLoaded()` loads the whole file once, the
first time *any* of the four is turned on, and never unloads it again for the session.

## SDR only (v1)

Every effect on this panel works directly on the base layer's already gamma/sRGB-encoded
0..1 RGB, with no HDR-aware colour handling. `PanelShaders.cpp`'s `EffectsUsable()` /
`IsBaseLayerSdr()` gate disables the whole panel (with a stated reason,
`kSdrOnly`) whenever the focused app is presenting HDR or scRGB content — see
`superdoc/planning/DECISIONS.md` #15 for why this was scoped out of v1, and
`reshade-shaders.md` Q6 for what correct HDR-space math here would require. Every effect
below, Shadow Control included, inherits this gate; none of them needs to branch on
`BUFFER_COLOR_SPACE` itself as a result.

## The four effects

### Vibrancy (`image.shaders.vibrancy`)

Adaptive saturation: boosts saturation more on already-dull pixels than on already-vivid
ones (so vivid colours don't clip as fast toward the boost end), with an optional mask
that dampens the effect on skin-tone-like hues.

**Config**: `ReshadeVibrancySettings` (`src/Config/ConfigSchema.h`) —
`enabled` (bool), `strength` (float), `protect_skin_tones` (bool, default true).

**`strength` — a true saturation multiplier, 0.0..3.0, neutral at 1.0** (changed
2026-09-04, request #2, from an additive -1.0..+1.0 boost with 0.0 neutral):

- **0.0** — full greyscale.
- **1.0** — the image is unchanged. This is the default.
- **3.0** — maximum boost.

Shader math (`ApplyVibrancy()` in `gamescope-ritz.fx`), built from two pieces that *add*
rather than an if/else, so there is exactly one return and no branch-dependent code path
to keep in sync:

```
mix   = min(VibrancyStrength, 1.0)
boost = max(VibrancyStrength - 1.0, 0.0) * (1.0 - saturation) * skin_protect
output = lerp(luma, color, mix + boost)
```

`mix` alone reaches the two endpoints (0.0 = full grey, 1.0 = unchanged) with a plain
uniform blend toward luma — deliberately carrying none of the adaptive/skin-tone shaping,
because at the 0.0 end *every* pixel must land on exactly the same grey target; a
per-pixel exception there would mean 0.0 isn't really full greyscale for skin-toned or
already-saturated pixels. `boost` is zero at and below neutral and picks up this effect's
original adaptive shape above it, reparameterized off the new 1.0 neutral point instead of
the old 0.0 one.

`Why 0.0..3.0 with neutral at 1.0, not a boost-only 0.0..N range:` the user explicitly
chose to keep desaturation reachable rather than clamp the slider to boost-only — see
`requests-2026-09-04.md` item #2's "Decided" note.

#### Migration: an existing config's old value

The old scale's neutral (0.0, additive) does not mean the same thing as the new scale's
0.0 (full greyscale, multiplicative) — reread blind, every config that never touched
Vibrancy would suddenly open in black and white. This needed handling explicitly rather
than shipped as a silent reinterpretation, so this fork's config schema-migration
scaffold (`src/Config/ConfigManager.cpp`'s `ParseConfigFile`, built but never
exercised until now — see its own comment) got its first real use:

- `kCurrentSchemaVersion` bumped 1 → 2 (`src/Config/ConfigSchema.h`).
- `Migrate_1_to_2()` (`src/Config/ConfigManager.cpp`) runs once, on load, for any file
  whose `schema_version` is below 2 (including a file with no `schema_version` key at
  all — everything predates the rename). It transforms `reshade.vibrancy.strength` in
  place: `new = clamp(old + 1.0, 0.0, 3.0)`.

`Why +1.0 and not something more elaborate:` +1.0 is exactly the constant that carries
old-neutral onto new-neutral, so an untouched config (by far the common case) lands back
on 1.0 — no greyscale surprise, the actual risk this migration exists to prevent. A
config that *did* customize the old value keeps the same displacement from neutral
instead of being reset to a single fallback value:

| Old value (additive, 0.0 neutral) | New value (multiplier, 1.0 neutral) |
| --- | --- |
| `-1.0` (old min: full desaturate) | `0.0` (full greyscale) |
| `0.0` (old neutral, untouched) | `1.0` (new neutral) — **the case that mattered** |
| `+1.0` (old max boost) | `2.0` (a real boost, short of the new 3.0 ceiling) |

The old max mapping to 2.0 rather than the new 3.0 ceiling is not an exact fidelity
match — the effect's shape itself changed too (see the math above), so exact
pixel-for-pixel equivalence was never really on the table — but it is monotonic, sane,
and, most importantly, gets the untouched-default case exactly right.

A config saved fresh under the new build round-trips its `strength` unmigrated (it
already carries `schema_version: 2`, so `Migrate_1_to_2` is a no-op for it) — see
`tests/test_config.cpp`'s `"a config saved under the current schema round-trips vibrancy
strength unmigrated"`.

### Shadow Control (`image.shaders.shadow_lift`)

New effect, added 2026-09-04 (request #3: *"a darkness booster for dark games"*).
Brightens dark areas so detail in dark games becomes visible, while leaving bright areas
essentially alone. **Not** a global brightness control (that would also wash out
highlights) and **not** shadow-crushing (the opposite of what was asked).

**Config**: `ReshadeShadowLiftSettings` (`src/Config/ConfigSchema.h`) — `enabled` (bool,
default false), `strength` (float, 0.0..1.0, default 0.0/neutral). Purely additive new
struct/keys, so an existing config — which has neither key, the setting being brand new —
simply resolves to these defaults. No migration needed for this one, unlike Vibrancy
above.

**The curve** (`ApplyShadowLift()` in `gamescope-ritz.fx`): a gamma curve on the low end.

```
exponent = 1.0 - 0.5 * strength   // 1.0 (identity) down to 0.5 (sqrt) at strength=1.0
output   = pow(saturate(color), exponent)
```

`Why a gamma curve:` 0.0 and 1.0 are fixed points of *any* power curve (`0^g = 0`,
`1^g = 1` for any `g > 0`), so black and white never move — only the shape between them
does — which is exactly the "leave highlights alone" requirement, guaranteed
mathematically rather than tuned into place. The curve concentrates its effect at the low
end because a power function's derivative is largest near 0 for `g < 1`: at full
strength, `0.1 → 0.316` (+0.216) while `0.9 → 0.949` (+0.049) — an order of magnitude
smaller boost near white. The exponent range (1.0 down to 0.5, not further) was picked as
the same shape a "raise gamma to ~2.0" boost applies — a familiar, well-understood amount
rather than an arbitrary one.

**Where in the pipeline, and which colour space:** folded into the *same* pixel-shader
pass as Vibrancy (`PS_Vibrancy` in `gamescope-ritz.fx`) rather than given its own
render-target pass. `Why:` both effects are cheap, purely per-pixel operations on one
texture sample with no neighbour reads (unlike Pre-Sharpen's 4-tap blur) and no
cross-frame state (unlike Adaptive Brightness's persistent luminance history) — there is
nothing a separate pass would buy here beyond a redundant full-screen-triangle draw and an
extra named texture. It runs on the base layer's encoded 0..1 sRGB-ish colour, the same
space every other effect on this panel operates in (see "SDR only" above) — there is no
linear-light or PQ value that can ever reach this `pow()` call, since the SDR-only gate
keeps HDR/scRGB content out of the whole panel, not just this one effect.

**Order relative to Vibrancy:** lift runs first, then Vibrancy. `Why:` this is a
tone/exposure-style adjustment, and lift-before-saturate is the conventional order for
that family of operations (the classic lift/gamma/gain grading chain runs tone shaping
before vibrance/saturation). It also means Vibrancy's grey target (`luma`) is computed
from the already-lifted colour, so a lifted pixel's desaturated version doesn't drift back
toward a stale, pre-lift grey.

**Bounds under HDR:** guaranteed safe by construction, not just by the panel's SDR gate.
`pow()` of a `saturate()`-clamped 0..1 input to a positive exponent (`0.5..1.0` here) is
mathematically bounded to 0..1 — it cannot go negative and cannot overshoot 1 — so even if
the SDR gate were ever bypassed, this specific operation could not itself produce an
out-of-range value. The pass's own closing `saturate()` (shared with Vibrancy, in
`PS_Vibrancy`) is a second, independent backstop.

### Pre-Sharpen and Adaptive Brightness

Unchanged by this batch of work. See `gamescope-ritz.fx`'s header comment and
`PanelShaders.cpp`'s own comments for their design (Pre-Sharpen: a 4-tap unsharp mask
pre-upscale; Adaptive Brightness: an auto-exposure EMA against a self-sampled 1×1
luminance history texture, `superdoc/planning/DECISIONS.md` #14).

## When an effect appears to do nothing

The `.fx` is compiled at runtime from a file on disk while the panel that drives it is
compiled into the binary, so the two ship separately and can drift — and an older copy of
`gamescope-ritz.fx` under the legacy `~/.local/share/gamescope/reshade/Shaders/` tree
wins the search over a newer one in the `gamescope-ritz` tree. When that happens the
panel writes uniforms the loaded shader never declared, they are silently dropped, and
the controls move with nothing changing on screen.

The Shaders area's **Diagnostics** group answers this directly: **loaded from** names the
file that actually compiled, and **uniforms** says whether everything the panel writes is
recognised by it (and lists what is not). See
[reshade-effects](reshade-effects.md#where-effect-files-are-searched-and-how-a-stale-copy-shadows-a-new-one)
for the search order, the mechanism, and the matching log lines.

## The settings-panel budget

Each switch row may own at most six `Param`s before `Registry.cpp` aborts registration
(the row must be promoted to a category instead) — see `PanelShaders.cpp`'s own "THE SIX
BUDGET" comment. Current counts: Vibrancy 2, Pre-Sharpen 1, Adaptive Brightness 6 (zero
headroom), **Shadow Control 1**.

## Related links

- [reshade-effects](reshade-effects.md) — the generic compile/upload/uniform-push
  mechanism these effects run through.
- `superdoc/planning/reshade-shaders.md` — the M9 Adaptive Brightness design spike this
  combined-effect file grew out of, including the HDR-space math question (Q6) still open
  for a future milestone.
- `superdoc/planning/DECISIONS.md` #13 (combined-file design), #14 (Adaptive Brightness),
  #15 (SDR-only v1 scope).
- `superdoc/planning/requests-2026-09-04.md` items #2 and #3 — the requests this page's
  Vibrancy-migration and Shadow-Control sections document.
