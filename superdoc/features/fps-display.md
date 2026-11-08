# FPS display (the HUD)

One integer, drawn well, over the game. `src/Overlay/FpsDisplay.cpp` /
`FpsDisplay.h`; config lives in `config::FpsDisplaySettings`
(`src/Config/ConfigSchema.h`); settings area `system.hud`
(`FpsDisplay_RegisterArea()`).

This is the **HUD** (see `superdoc/meta/TERMINOLOGY.md`) — distinct from the
**Shell**/**Click-UI** settings surface that hosts its settings tab, and from
the unrelated **Launcher**. It has its own visibility flag and its own
render pipeline (own ImGui context, offscreen texture, timeline semaphore),
entirely independent of the settings panel, so it keeps drawing over the
game while the panel is closed.

**The layer's texture is redrawn only when it changed** (2026-11-04). `FpsDisplay_AddLayer()` still
runs ImGui every paint but hashes the draw data and skips the general-queue draw when it matches
what the texture holds. `Why:` the composite waits for that draw, and the general queue is the one
the game renders on, so a draw per paint put every paint behind the game's frame. See
[frame-generation](frame-generation-pacing.md#paints-behind-the-games-gpu-burst-2026-11-04).

## History: two phases, both 2026-09-03

**Phase 1** stripped what used to be a small performance profiler (CPU/GPU
load, a frametime graph, a percentile row, Now Playing — see
`superdoc/meta/TERMINOLOGY.md`'s "profiler" entry) down to a single FPS
integer, positioned by a 9-point anchor plus pixel margins, at a
user-chosen font size. Those three settings (Placement, Font size) are
unchanged since Phase 1.

**Phase 2** (this page) added everything else, following the user's own
spec verbatim: hide-above-X, update modes (three then, two since 2026-09-05),
a plain backdrop (removed again 2026-09-09 — see [Backdrop: removed](fps-display-readout.md#backdrop-removed-2026-09-09)),
a two-way text-colour choice with lag-spike reactions, and a black outline
sized in pixels (a drop shadow until 2026-09-03, when the outline
replaced it).
The user's own framing: *"it is nice, instead of feature bloat"* — one
number, drawn well, not a second profiler.

## The settings, top to bottom

`system.hud` is a two-column area (`Columns(2)`, every group pinned with `GroupColumn`),
laid out as the user's workbench mock-up (2026-11-08); `FpsDisplay_RegisterArea()`:

| Column | Group | Rows |
| --- | --- | --- |
| 0 | HUD | Show HUD (`enabled`; master switch, not gated by itself, SPEC §3.13) |
| 0 | Placement | Placement (`anchor`, 3x3 grid), Horizontal margin (`hud.margin_x`), Vertical margin (`hud.margin_y`); sliders 0-128 px step 4 |
| 0 | Visibility | Hide above X (`hide_above_enabled`), Threshold (`hud.hide_above_fps`, 30-300 fps step 5, enabled only while Hide above X is on), Lag spike detection (`lag_detection_enabled`, default on) |
| 0 | Readout | FPS shown (`fps_shown`: Game / Frame gen / Game + FG), Update mode (`update_mode`: Smoothing / Immediate) |
| 1 | Appearance | Font size, Outline size (0-4 px, 0 = off), Text colour (`color_mode`: Fixed / Inverted), Number colour (`color_fps`, Fixed only; its "Custom colour" stays a Param), Text opacity (Fixed only) |

Every row is gated `DisabledUnless(MonitorOn, "the HUD is off")` except the
master switch itself (plus the Fixed-only and Threshold rules above).

Why: the user likes the workbench layout (related rows grouped, Appearance alone
in the second column). The margins and the threshold used to be `.Param()`s of
Placement / Hide above X; they are sheet rows now so they read as settings, not
fine print. Their ids changed from `hud.anchor.margin_x` / `hud.anchor.margin_y` /
`hud.hide_above.fps` to `hud.margin_x` / `hud.margin_y` / `hud.hide_above_fps`;
the config keys are unchanged, so no saved value moves.

### Number colour and Text opacity (2026-09-07)

Both fields existed in the schema and were already read by the renderer
(`color_fps` since issue #29, `text_opacity` since Phase 2) but had no row
of their own — a static cross-check
caught this. They now sit right below "Text colour", in the shape their
siblings use:

- **Number colour** (`hud.color_fps`) is a `Composite(Color)` row with the
  same custom/accent-follow idiom as `PanelCursor.cpp`'s
  `cursor.outline_color`: the band shows and edits the UI's own accent
  colour whenever `color_fps` is unset, and a `custom` `.Param()` switch
  toggles between `std::nullopt` (follow the accent) and a captured literal
  — sharing the row's own `Key()` so a stored `null` reads as "the switch's
  own write", not a stale copy contradicting the band. `PackColorRgb()`
  (`FpsDisplay.cpp`, mirrors `PanelCursor.cpp`'s own `PackRgb()`) packs the
  accent's `ImU32` down to the same `0xRRGGBB` shape `UnpackColorRgb()`
  already read.
- **Text opacity** (`hud.text_opacity`) is a plain 0–1 `Slider`, the same
  shape as the crosshair's own opacity sliders.

**Both are gated to Fixed mode only**, sharing one predicate
(`FixedColorApplies` in `FpsDisplay.cpp`, `MonitorOn && color_mode !=
"inverted"`) and one disabled reason ("text colour is set to Inverted").
Neither has any effect in Inverted mode: that mode draws the digits as an
opaque magenta *marker* the compositor's shader replaces per-pixel (see
"Text colour: Fixed vs. Inverted" below) — an explicit colour override
would never be seen, and a partial alpha would only dilute the invert
(already documented, before this pass, in that section's "Opaque, not
`text_opacity`-scaled" note). Rather than silently doing nothing, both rows
grey out the moment Inverted is selected, the way every other conditionally-
irrelevant row in this codebase does.

Rendered proof (2026-09-07):
against a flat `#202020` background, `color_fps` unset measured the digit
fill at `(120, 219, 246)` (the UI accent); setting it to `0xFF3B30` over the
real `overlay_e2_set` path measured `(255, 70, 48)`, matching within
antialiasing tolerance. With that colour held fixed, `text_opacity 1.0`
measured the same `(255, 70, 48)` (fully opaque) and `text_opacity 0.4`
measured `(70, 30, 28)` — markedly pulled toward the background, proving
the slider reaches the render. (The exact blend arithmetic isn't
hand-verified against a formula here — like the Inverted-mode contrast
guard above, this compositor blends in linear light and re-encodes, which a
naive sRGB lerp doesn't reproduce — but the two opacities produce two
clearly different, correctly-ordered results.)

## What is measured — and why it counts instead of timing

The number is the **commit rate of the focused window**: how many frames
the game handed the compositor per second. `commit_t::Signal()`
(`src/commit.cpp`) increments `g_ulAppCommitCount` once per focused-window
commit (the same `m_bMangoNudge` gate that feeds mangoapp), and
`UpdateAndGetDisplayFps()` reads that counter at paint time and computes
`Δcount × 1e9 / Δns` over a window of its own. The pure arithmetic lives in
`FpsDisplay.h`'s `gamescope::fpsmath` and is unit-tested
(`tests/test_fps_counter.cpp`).

> **Why counting replaced timing (2026-09-05, the "999 FPS" bug).** Until
> then the rate came from the per-commit *frametime*
> (`g_ulLastAppFrametimeNs`, `now − last`), of which the HUD consumed at
> most one new sample per paint, clamped to `[0.1, 2000] ms`. steamcompmgr
> drains every finished commit in one loop, and when two or more land in a
> batch — the external layer presenting a real frame and a generated one back to
> back guarantees it, and any game briefly out-running the compositor can
> do it — the *last* `Signal()` in the batch measures `≈ 0 ms`, clamped to
> 0.1 ms, i.e. 10 000 fps. Because the HUD only ever saw that last write,
> every mode sat on the old 999 clamp, in Rust for one. A count is immune:
> two commits in a batch are two commits, whenever they arrived.
>
> The frametime is kept for the **lag-spike detector only** (below). Note
> that batching fools it too — a batch of two writes a ~0 ms sample and the
> next real frame then measures about twice the median. Accepted for now:
> it only reacts to that pattern under frame generation, where the
> frametime is already not the game's own.

**Inherent limit to tell the user:** with the external layer at ×2 the counter shows
the **generated** rate. The compositor only sees post-frame-gen presents —
the same thing MangoHud shows when loaded after the layer — so the
pre-frame-gen rate is not observable from here.


## Pages

This page is the hub; the detail lives in these sub-pages (sections moved verbatim, 2026-11-07 superdoc repair).

| Page | Covers |
|---|---|
| [fps-display-readout.md](fps-display-readout.md) | FPS shown, update modes, Hide-if-above hysteresis, the removed backdrop, outline, lag-spike detection, pinned digit width and warm-up. |
| [fps-display-text-colour.md](fps-display-text-colour.md) | Fixed vs. Inverted text colour: history, root cause, what is not inverted, and the pixel verification recipe. |
| [fps-display-margin.md](fps-display-margin.md) | Margin: the measured-to-first-visible-row rule, the bottom-anchor nudge and the worst-case digit correction. |

## History

- **HUD** (glossary detail): **HUD** — the on-screen readout drawn over the game: a single FPS integer (`src/Overlay/FpsDisplay.cpp`'s `DrawReadout()`), positioned by a 9-point anchor plus pixel margins. Its settings-panel area is `system.hud` ("HUD" in the rail), renamed from `system.monitor`/"Monitor" (HUD layouts Phase 1, 2026-09-02) because "Monitor" read as ambiguous against this project's own "Overlay" naming, which **Shell**/**Click-UI** (see [overlay-shell.md](overlay-shell.md)) already claims. Distinct from both the **Shell** (the settings surface that hosts the HUD's own settings tab) and the **Launcher** (an unrelated command-palette feature). Used to carry a small **profiler** (see the **Profiler** bullet below) and a named per-module layout system; both were removed 2026-09-03 as a deliberate scope reduction, and the current minimal `system.hud` area (Show HUD / anchor / margins / font size) is a placeholder a later phase rebuilds properly. Its "FPS shown" choice (2026-10-04; Game by default) picks the game's own frame rate, the output rate actually sent to the display (frames made by Frame generation included; "Frame gen"), or "Game + FG", drawn as two numbers while the Frame generation switch is on, `120 > 280` (game > output) while it is generating and `340 - 340` (game - output) while it passes frames through, and as the lone game number with the switch off (the output number is the game's own rate except while generating); it replaced the short-lived "Count generated frames" switch. Since 2026-09-27, the Inverted text-colour mode samples one pixel of the game underneath the number each frame and shows its plain inverse, so the readout keeps changing with the scene instead of staying white — per the user's "capture a single pixel of the color below it and just invert it".
- **Profiler** (glossary detail): **Profiler** — the user's own term for the perf-stats modules the **HUD** used to carry on top of the plain FPS number: CPU load, GPU load, a frametime readout, a frametime graph, a 1%/0.1%-low percentile row, and a Now Playing (MPRIS) module. Removed 2026-09-03 at the user's explicit request as feature bloat — the HUD is a single FPS integer now. The underlying frame-rate counting/timing and lag-spike detection (the raw per-frame frametime history, `FpsDisplay.cpp`'s `s_flFrametimeHistoryMs`) were kept, since a later phase builds on them; the CPU/GPU/media sampling (`src/Metrics/SystemStats.{h,cpp}`, the only consumer of which was the profiler) was deleted along with it.
