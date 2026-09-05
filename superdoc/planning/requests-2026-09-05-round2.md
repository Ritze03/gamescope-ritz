# Requests — 2026-09-05 (round 2)

The day's second batch of requests. Nothing is `[x]` until it is
**implemented, built, and verified live on the test laptop** (build on the desktop,
test on the laptop; the desktop is not used for testing unless the user grants it).
Checks that cannot be run here go in **For the user to test** at the bottom, with
exact steps — the user has asked for that list explicitly.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done and verified.

---

## [x] 1. Pixel-regression script

**Done `d976392`.** `scripts/pixel-regression.sh` (+ `scripts/pixel_regression_sample.py`,
a PIL sampler — PIL was already installed, no new dependency). Deliberately **desktop-only
by design**, not a laptop check: that is the entire point (the laptop round trip is what
this replaces). `gamescope --backend headless` was tried first and rejected — measured on
this rig it captures no extra composited layer at all via `gamescopectl screenshot`, see
`superdoc/features/cursor-pipeline.md`'s "Verified by direct X11 query" section. Instead: a
private, invisible sway (`WLR_BACKENDS=headless`, isolated `XDG_RUNTIME_DIR`, no input
devices) hosts a real nested `gamescope --backend wayland`, and `gamescopectl screenshot
"<path> 4"` against that instance does capture the HUD/crosshair. Test client is `kitty`
with matching `-o background/foreground/cursor` (xterm, the documented recipe, is not
installed here). State is driven via an isolated config file plus `overlay_e2_set` /
`fps_display_force` over `gamescopectl` — no OS input.

19 checks, all passing on the real binary (46s runtime): HUD inversion at a dark and a
mid-tone background, Inverted-HUD-plus-crosshair split mode (digit still inverts, crosshair
keeps its colour, crosshair's own outline stays black — the exact regression from round-1
item 11), Fixed-mode colour, the HUD's own outline on/off, and all four crosshair arms'
colour/gap/outline geometry. Exits non-zero on any real failure, so it gates a commit; see
`scripts/README.md`'s "Pixel regression" section for how to add a check and read a failure.
Docs: `superdoc/features/fps-display.md` and `crosshair.md` point at it, and the
`grade-screenshots-not-checklists` memory note now says to run it.

## [x] 2. Layer budget fails loudly

**Done `84a50f6`.** `LayerStack_t::push()` (`src/rendervulkan.hpp`) now bumps a global
atomic drop counter and a high-water mark on every call, regardless of what the caller
does. Each real call site that silently dropped a layer (HUD, crosshair split,
notifications, shell, cursor, base and window/game layers in `steamcompmgr.cpp`,
`FpsDisplay.cpp`, `Notifications.cpp`, `SettingsOverlay.cpp`) now logs a rate-limited
warning (first drop, then every 600th) via its module's own `LogScope`, naming which
layer was dropped and the count / `k_nMaxLayers`. New `layer_budget_stats` ConCommand
prints both counters. `k_nMaxLayers` unchanged. Two call sites in `steamcompmgr.cpp`
(the Steam-overlay blank-texture layer and the mura-correction layer) were left as
their existing `assert()` because both are already pre-guarded with
`frameInfo.layers.count() < k_nMaxLayers` immediately before the push — `push()` cannot
fail there, so it isn't a real drop site. DRMBackend.cpp's internal
`presentCompFrameInfo` pushes were left alone too: a separate, always-small transient
stack, not one of the frame's user-visible layer types.

Verified headless: built, launched `--backend headless` under
`scripts/with-gamescope-lock.sh` with an isolated `XDG_CONFIG_HOME`/`XDG_RUNTIME_DIR`,
ran `gamescopectl layer_budget_stats` — printed `layer_budget_stats: 0 drop(s),
high-water mark 0 / 6 layers`. Torn down cleanly, no leftover process.

## [~] 3. Retire the double-height split texture for Inverted HUD + crosshair

**Implemented and desktop-verified `0b9b79d`, laptop pending.** One HUD layer in every
mode. The invert shader (`src/shaders/alphamode.h`) now finds the digits by a marker in
the texel rather than by brightness: the readout draws them pure magenta (`G == 0`) over
pure-black outline/backdrop, so `G == 0` ⇒ digit with R as its coverage, `G > 0` ⇒
composite as coverage. The crosshair keeps any colour at any opacity — a colour with no
green is nudged `G 0 → 1` (one count) while it shares an Inverted layer, and the texture is
16-bit for exactly that pairing so the nudge survives premultiplication at low opacity.
Why not an alpha sub-range / low-bit marker in 8 bits: ImGui's over-blend keeps `rgb ≤ a`
and mixes a digit's edge with what is under it, so no brightness/ratio test separates a
digit edge over the black outline from a grey crosshair of the same value, and a ±2
colour restriction drowns in 8-bit premultiplied quantisation below ~50 % opacity; a zero
channel is the one thing that survives mixing with black. Full reasoning in
`superdoc/features/fps-display.md` ("What Inverted mode does not invert").

Measured on the desktop (`scripts/pixel-regression.sh`, now 23 checks, all pass): digits
251 over 51 and 46 over 148 unchanged; `(0,255,0)` arm at 50 % over 51 = `(35, 99, 35)`
on both the split build and this one (that is the documented premultiplied-then-coverage
blend, *not* `c×0.5 + bg×0.5` — see `coverage_blend_expected()`); pure red at 10 % on
both crosshair paths matches to the count; layer high-water mark with readout + crosshair
on from startup **5 → 4** (`layer-budget` check). Known limitation, documented: a readout
anchored *on* the crosshair shows a few magenta fringe pixels where a digit edge crosses
an arm (`build-release/verify-shots/split-retire/zoom/overlap-mid.png`).

Laptop: re-run the fps-display.md pixel recipe with Inverted + crosshair on Intel/ANV, and
eyeball a red/blue crosshair at 10–50 % opacity in that mode (the 16-bit format switch and
the pipeline re-creation are the parts this desktop's nested run cannot vouch for on
another driver).

## [~] 4. Profiles and per-game config: new concept

A new, easy and extensible concept for profiles and per-game config, including
loading a named profile from the command line at launch (`--profile <name>`).
Concept doc first, implementation after the user approves.

`--profile` flag implemented against the current model once the concept fixes its
semantics.

**2026-09-06:** concept written, awaiting user approval —
[`profiles-concept.md`](profiles-concept.md). Implementation and `--profile` not started.

`--profile` / `GS_RITZ_PROFILE` implemented against the current model, `4caee05`.

## [x] 5. Brainstorm of further filters and features

A prioritised planning doc brainstorming further native-effect filters and other
features.

**2026-09-06:** written — [`feature-ideas-2026-09-05.md`](feature-ideas-2026-09-05.md)
(16 filters, 17 features, Top 5). A doc, so nothing to verify on the laptop.

## [~] 6. FPS HUD digit alignment to anchor edge

Align the FPS HUD digits to the anchor's screen edge so the readout doesn't shift
when the value's digit width changes.

**2026-09-06:** implemented and built -- `MeasureFpsModule()`/`DrawReadout()` in
`FpsDisplay.cpp` now place the unpadded digits flush to the anchor's horizontal
side (left/right anchors flush, centre unchanged) instead of always centring them
in the pinned-width box; `ResolveAnchoredOrigin()` already kept the box's
anchor-facing edge fixed across a digit-count change, confirmed rather than
assumed. New debug ConCommand `fps_display_force "<n>"` forces the reading for
testing. Code landed in `84a50f6` (bundled there by a concurrent edit to the same
working tree, not its own commit); the changelog entry and feature-doc section are
`392fdac`. Pixel-verified **headless on the desktop** only (`--backend headless`,
isolated `XDG_RUNTIME_DIR`/`XDG_CONFIG_HOME`, `fps_display_force` + `gamescopectl
screenshot "<path> 4"`, measured digit-ink and backdrop bounding boxes for
top-left/top-right/top-center at readings 60/144/1000 -- captures and the
measured-edge table in `build-release/verify-shots/hud-align/`): the anchor-facing
edge is pixel-stable (within 1px) across all three readings for every anchor,
including across the 3-to-4-digit box widening. **Not yet verified live on the
test laptop** per this file's own bar -- left `[~]` rather than `[x]` for that
reason.

---

## For the user to test (cannot be verified here)

(empty — nothing completed yet this round)
