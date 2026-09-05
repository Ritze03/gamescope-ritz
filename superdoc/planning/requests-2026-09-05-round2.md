# Requests — 2026-09-05 (round 2)

The day's second batch of requests. Nothing is `[x]` until it is
**implemented, built, and verified live on the test laptop** (build on the desktop,
test on the laptop; the desktop is not used for testing unless the user grants it).
Checks that cannot be run here go in **For the user to test** at the bottom, with
exact steps — the user has asked for that list explicitly.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done and verified.

---

## [ ] 1. Pixel-regression script

A headless capture-and-sample script under `scripts/`: launches gamescope headless,
captures a screenshot, samples known pixels for HUD inversion, crosshair colour, and
outline, and fails on a threshold. Intended to catch regressions like item 11 in the
round-1 tracker automatically instead of relying on a manual bisect.

## [ ] 2. Layer budget fails loudly

`LayerStack_t::push()` (`src/rendervulkan.hpp` ~line 433) returns `nullptr` when
`m_nCount >= k_nMaxLayers`, and callers silently drop the layer. Log each dropped
push (rate-limited, naming the layer and current count / `k_nMaxLayers`) via the
module's existing logger, add a global atomic drop counter, and expose it via a
ConVar/ConCommand. Do not raise `k_nMaxLayers`.

## [ ] 3. Retire the double-height split texture for Inverted HUD + crosshair

Draw the crosshair as an unmarked region of the single HUD layer instead of a
separate double-height split texture, freeing one layer slot.

## [ ] 4. Profiles and per-game config: new concept

A new, easy and extensible concept for profiles and per-game config, including
loading a named profile from the command line at launch (`--profile <name>`).
Concept doc first, implementation after the user approves.

`--profile` flag implemented against the current model once the concept fixes its
semantics.

## [ ] 5. Brainstorm of further filters and features

A prioritised planning doc brainstorming further native-effect filters and other
features.

## [ ] 6. FPS HUD digit alignment to anchor edge

Align the FPS HUD digits to the anchor's screen edge so the readout doesn't shift
when the value's digit width changes.

---

## For the user to test (cannot be verified here)

(empty — nothing completed yet this round)
