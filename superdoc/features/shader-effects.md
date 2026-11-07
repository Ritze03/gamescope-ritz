# Shaders settings area — Saturation, Vibrancy, Color Boost, Shadow Control, Pre-Sharpen, Bloom, Adaptive Brightness, Adaptive Gamma, Adaptive Brightness V2

The overlay's **Shaders** area (`image.shaders`, `src/Overlay/PanelShaders.cpp`) exposes
nine effects — six independent (**Color Boost** new 2026-10-29), plus **Adaptive Brightness**, **Adaptive Gamma** (new
2026-09-08) and **Adaptive Brightness V2** (new 2026-09-14), which are mutually exclusive
with EACH OTHER and with nothing else — see
[Adaptive Brightness V2](shader-effects-adaptive-brightness-v2.md#adaptive-brightness-v2-imageshadersadaptive_brightness_v2--new-2026-09-14)
for why a third choice was added ALONGSIDE the first two rather than replacing them
(the user's own decision, quoted there). Since 2026-09-05 they are **one native compute pre-pass compiled
into the binary at build time** — `src/shaders/cs_effects_layer0.comp`, dispatched from
`vulkan_composite()` (`src/rendervulkan.cpp`) on the base/game layer at source resolution
before any scaling. **Bloom** (new 2026-09-08) is the one effect that needs more than
that one dispatch — it is spatial, so it brings three of its own; see
[Bloom](shader-effects-bloom.md#bloom-imageshadersbloom--new-2026-09-08). A second spatial effect, the
experimental **Brightness Map**, lived here 2026-09-09 to 2026-09-14; see its
[History](shader-effects-bloom.md#brightness-map--history-2026-09-09-to-2026-09-14) entry below. They used to be
gated passes
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


## Pages

This page is the hub; the detail lives in these sub-pages (sections moved verbatim, 2026-11-07 superdoc repair).

| Page | Covers |
|---|---|
| [shader-effects-pipeline.md](shader-effects-pipeline.md) | The native pre-pass: where it runs, encoded space, the uniform block, backends, the effect list, the settings-panel budget and diagnostics. |
| [shader-effects-color.md](shader-effects-color.md) | Shadow Control, Saturation, Vibrancy, Color Boost and Pre-Sharpen. |
| [shader-effects-bloom.md](shader-effects-bloom.md) | Bloom (the spatial effect with its own dispatches) and the removed Brightness Map experiment. |
| [shader-effects-adaptive-brightness.md](shader-effects-adaptive-brightness.md) | Adaptive Brightness, part 1: the budget decision, statistics, Whole image and Dynamic modes, gain limits and the ceiling fix. |
| [shader-effects-adaptive-brightness-local.md](shader-effects-adaptive-brightness-local.md) | Adaptive Brightness, part 2: Local adaptation, the Inspector preview, the pulse and the history texture. Continues [part 1](shader-effects-adaptive-brightness.md). |
| [shader-effects-adaptive-gamma.md](shader-effects-adaptive-gamma.md) | Adaptive Gamma and the per-effect dark-scene floor shared by Adaptive Brightness and Adaptive Gamma. |
| [shader-effects-adaptive-brightness-v2.md](shader-effects-adaptive-brightness-v2.md) | Adaptive Brightness V2: toe-gamma curve, content anchor, guided-filter stages. |
| [shader-effects-darkening.md](shader-effects-darkening.md) | The two-sided Darkening curve of [Adaptive Brightness V2](shader-effects-adaptive-brightness-v2.md) and its 2026-09-15 redesign. |

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
  frame-to-frame stability under a pan. A block measuring the now-removed Brightness Map
  effect (`models`/`modelsinv` scenes, `bmap-*` checks) ran here 2026-09-09 to 2026-09-14
  on its own gamescope instance with its own scene ring, for a reason worth keeping in
  mind for the next block that needs new scenes: several checks in this script navigate
  by "advance N to wrap round to `dark`", so the shared ring's *length* is arithmetic
  they depend on — appending two scenes to it turned 28 passing checks red in one run
  before that block was isolated onto its own instance.
- `superdoc/planning/requests-2026-09-08.md` item 6 — the pulse: measured, found, fixed;
  and the Local adaptation item — the split-scene, halo and gain-sweep evidence.
