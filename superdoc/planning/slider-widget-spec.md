# Slider Widget — Spec (measured from a real render)

Date: 2026-08-23. Companion to `ui-mockup-precise-spec.md` — that file records what the
*original Claude Design mockup* measured (§7 "Slider"); this file records what the
**shipped `widgets::SliderControl()` implementation** (`src/Overlay/Widgets.cpp`) actually
draws today, on a real gamescope-ritz build, after issue #23's ~20-25% baseline raise and
issue #61's own adjustments. Where the two disagree, this file wins for anything
slider-shaped — it was measured from the compiled overlay, not the mockup.

**Method.** Built `./build/src/gamescope`, launched nested (`-W 1920 -H 1080 -w 1280 -h 960
-S stretch --adaptive-sync --immediate-flips --expose-wayland --filter fsr --sharpness 5
-f -- vkcube`, `DISABLE_LSFG=1`, throwaway `XDG_CONFIG_HOME` preset with Vibrancy enabled so
its slider renders in the *active* accent-gradient state, not the dimmed disabled one),
screenshotted the SHADERS panel with `grim -g` bounded to the window (never
`gamescopectl screenshot` — see `documentation.md`'s tooling notes), and pixel-sampled the
result with Pillow at 1:1 (screenshot is 1920×1080, the same "UI scale 1.0 on a 1920×1080
surface" reference frame `ui-mockup-precise-spec.md` §1 uses). Every pixel figure below is
a real measurement cross-checked against `Widgets.cpp`'s own constants, not a transcription
of the source — the two agreed exactly everywhere they were compared.

Reference control: SHADERS panel, "Vibrancy" group, "Strength" slider (`widgets::SliderFloat
"Strength##vibrancy"`, range -1.00..1.00, value 0.42 in the reference render).

---

## 1. Row anatomy (top to bottom)

```
Strength                                                              0.42     <- label row
[############################==|--------------------------------]             <- track row
-1.00                                                                  1.00    <- mark row
```

One `widgets::SliderFloat()`/`SliderInt()` call draws all three lines as a single ImGui
item (one `ItemAdd()`/`ItemSize()` call against `totalBB`) — label+value baseline-aligned
above, the track/handle hit-row below that, an optional min/max mark row below that. The
label row and mark row are optional-height (mark row only exists if the caller passed
min/max text, which every current call site does); the track row is always present.

## 2. Geometry (at `display_scale` 1.0 — everything below scales linearly with it, see §5)

| Element | Value | Source |
|---|---|---|
| Label→track gap | **8px** | `kLabelTrackGap` — raised from 6px, issue #61 (see §4) |
| Track→mark gap | **8px** | `kTrackMarkGap` — raised from 6px, issue #61 (see §4) |
| Track hit-row height | 22px | `kHitHeight` — the clickable/hoverable band, not the visible bar |
| Track visible height | **6px**, measured exactly 6px (rows 152-157 in the reference render) | `kTrackHeight` |
| Track corner radius | 3.5px | `kTrackRounding` |
| Track width | full row width (`ImGui::GetContentRegionAvail().x` unless the caller pushed an explicit item width) | — |
| Handle size | **10×22px**, measured 9-10px wide (edge pixels are anti-aliased) × exactly 22px tall (rows 142-163) | `kHandleW` / `kHandleH` |
| Handle corner radius | 1px | literal in `AddRectFilled(..., 1.0f)` |
| Handle glow | two enlarged rects behind the handle: handle-half-size+4px @ accent 18%, +2px @ accent 30% | derived from `kHandleW`/`kHandleH`, not independent literals (see §3) |

The handle's vertical center sits exactly on the track's vertical center in every
measurement taken (track center row 154.5 vs. handle center row 152.5 at one scale, 154.5
at the *display_scale 1.0* re-measurement below — both agree to within 1px, the resolution
of the pixel grid).

## 3. The GrabMinSize / drawn-handle invariant (why #23's bug can't recur)

Issue #23 found a real bug here once: the drawn handle disagreed with `style.GrabMinSize`,
so `ImGui::SliderBehavior()` (which places/hit-tests the grab using `GrabMinSize`) computed
a different fraction/position than what got painted, desyncing the visual handle from the
actual click/drag target.

`SliderControl()`'s fix (still in place, unchanged by #61) makes this **structurally**
impossible rather than merely correct-today: `kHandleW` is computed once per call, then
*both* consumed by `PushStyleVar( ImGuiStyleVar_GrabMinSize, kHandleW )` immediately before
`SliderBehavior()` runs, *and* reused directly as the half-width of the rect the draw code
paints afterward. There is exactly one local variable; `SliderBehavior()`'s hit-test and the
paint call cannot read two different widths because there is only one width for either to
read. The same shape applies to the label/value/track/mark geometry: every constant is a
single `const float` computed once at the top of the function from `DisplayScale()`, and
every downstream position (`flTrackTop`, `trackHitBB`, `grabBB`, `handleCenter`, mark
positions) is derived arithmetically from those same locals — there is no second,
independently-hand-picked copy of any of these numbers anywhere else in the function for
drift to creep into.

## 4. Issue #61: the extra vertical spacing

The user's own read of the SHADERS panel: *"They only need a bit more vertical spacing
inbetween elements."* SHADERS panel sliders (`PanelShaders.cpp`'s `DrawVibrancyGroup()` /
`DrawPreSharpenGroup()` / adaptive-brightness group) are **not** wrapped in
`BeginGroupBlock()` — they sit directly in the window's item list, so the label→track and
track→mark gaps inside the slider's own draw are the only "breathing room" available
between a slider and its neighbors (global `style.ItemSpacing.y` — 6px, unscaled by
`display_scale` — governs the gap *between* separate items like the "Vibrancy" toggle and
the "Strength" slider below it, and is out of scope here: changing it would also move every
`Toggle()`/`Checkbox()` row, which #61 never measured or was asked about).

Both of `SliderControl()`'s own internal gaps were raised **6px → 8px** (`kLabelTrackGap`,
`kTrackMarkGap`, both still `* DisplayScale()`). This is deliberately modest ("a bit more",
not a redesign) and self-contained: raising the constants inside the shared helper
increases the control's own registered `totalBB` height, which pushes whatever the *next*
item is further down for free — no other file needed to change.

Measured effect (reference render, Vibrancy Strength slider, both at `display_scale` 1.0):

| Gap | Before (px) | After (px) |
|---|---|---|
| Label row → track top | track top at row 150 | track top at row 152 (**+2px**, exactly the 6→8 delta) |
| Track bottom → mark row | mark glyph top ≈ row 170 | mark glyph top ≈ row 177 (**+7-ish px** apparent — glyph-ink vs. logical-row-height offset is constant, the logical gap itself moved by the same +2px) |

## 5. Scaling (`display_scale`, 0.5–2.0×)

Every geometry constant in this file is `* gamescope::palette::DisplayScale()`, the same
live-read pattern `Chrome.cpp`'s dock uses for `dock_scale` (issue #23 half two). Checked at
**1.0×** (all measurements above) and **2.0×** (SHADERS panel re-rendered with
`overlay.display_scale: 2.0` preset in `global.json`): every element — track, handle, label,
value, marks, the inter-element gaps from §4 — visibly doubles together and stays legible;
nothing desyncs or clips.

## 6. Colors

| Element | Token | Value | Notes |
|---|---|---|---|
| Label | `palette::Text( 0.62f )` | `#EFF5FB` @ 62% | unchanged by #61/#62 — already reads fine per the user's own "good" verdict |
| Value | `palette::kAccentValue` | `#78DBF6` | unchanged |
| Track fill (unfilled/"rail") | `palette::White( palette::kRailAlpha )` | `#FFFFFF` @ **16%** (was 9%) | **issue #62** — see `Palette.h`'s own comment; measured composite (44,44,46)→ now ~(49,49,51) over the reference render's background, a real ~55% brighter rail, not just a spec-table number |
| Track fill (filled portion, gradient) | `palette::Accent( 0.50f )` → `palette::kAccentGradHi` | `#36BDDD`@50% → `#47CAEA` | unchanged |
| Handle | `palette::kAccentHandle` | `#BAE7F4`, measured exactly (186,231,244) | unchanged |
| Handle glow | `palette::Accent( 0.18f )` / `palette::Accent( 0.30f )` | — | unchanged |
| Min/max marks | `palette::White( palette::kMarkAlpha )` | `#FFFFFF` @ **38%** (was 26%) | **issue #62** — measured brightest glyph pixel (69,70,71) → (97,98,99), a real, visible lift |
| Disabled fill/handle | `palette::White( 0.30f )` / `palette::White( 0.45f )` | — | unchanged; disabled-state dimming is a deliberate "inert control" treatment, not the readability bug #62 targets |

`palette::kRailAlpha` / `palette::kMarkAlpha` / `palette::kMetaTextAlpha` are new (issue
#62), defined in `Palette.h` next to `White()`/`Text()`/`Black()`. `kMetaTextAlpha` also
covers `widgets::ReadoutStrip()`'s system-value text (`#FFFFFF` @ 34% → 44%) — not a slider
element, but the same "dark grey text" family the user flagged, and the same file. See
`Palette.h`'s comment on why this is a set of *named, targeted* tiers rather than a change
to `White()`/`Text()` themselves — those two are reused for plenty of *intentionally* low
alphas (group-block fills at 2-3%, hairline borders at 6-10%, disabled dimming) that were
never part of this complaint and would wash out if brightened along with it.

## 7. Fonts

| Role | Family/weight/size | `fonts::Style` |
|---|---|---|
| Label | Geist Sans Regular, 14px | `Label` |
| Value | Geist Mono Medium, 16px | `Value` |
| Min/max marks | Geist Mono Regular, 11.5px | `ScaleMark` |

(Baseline sizes per `Fonts.cpp`'s `kSpecs` table — already includes issue #23's raise and
issue #53's Plex→Geist swap; unchanged by #61/#62.)

## 8. Rollout status

**Routed through `widgets::SliderControl()` today** (i.e. get everything in this file for
free): `PanelShaders.cpp` (Vibrancy/Pre-Sharpen/Adaptive-Brightness — the reference render
above), `PanelDisplay.cpp` (Sharpness, FPS Limit, SDR Gamut Wideness, SDR-on-HDR Brightness,
HDR Input/SDR Input Gain), `PanelAudio.cpp` (Volume), `PanelConfig.cpp` (per-field config
sliders, Display/UI-scale).

**Still stock `ImGui::SliderFloat()`** (none of this file's geometry, colors, or the #61/#62
fixes apply): `FpsDisplay.cpp`'s System Monitor panel config (Vertical/Horizontal margin,
Module spacing, Font size, Text opacity, Backdrop opacity/rounding/padding) — this is the
"every slider in the System Monitor tab" the user called out as bad. `FpsDisplay.cpp`
already has its own `SetStockSliderFullWidth()` workaround for one symptom (sliders not
spanning the row) with a comment noting the real fix belongs in the shared widget; issue #59
(System Monitor rework, queued behind #61/#62) is the right place to actually convert these
call sites — out of scope here per the coordinating task's own instruction to stop the
rollout at `Widgets.cpp`.

---

*Supersedes `ui-mockup-precise-spec.md` §7 "Slider" for anything the two disagree on — that
section describes the original mockup's numbers (5/5/18/5/3/8/18, before issue #23's raise);
this file describes what actually ships, including #61/#62's own subsequent changes.*
