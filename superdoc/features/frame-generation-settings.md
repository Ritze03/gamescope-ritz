# Frame generation: settings and status

Part of [Frame generation](frame-generation.md). The settings area, config keys, the status line and logging.

## Settings and status

Area `image.framegen` ("Frame generation"), per profile (game profiles inherit), no
keybind, config section `framegen` (schema stays 5; additive).

| Key | Values | Meaning |
| --- | --- | --- |
| `framegen.enabled` | `false` (default) / `true` | the master switch, the row "Frame generation" (split out of `mode` 2026-10-04) |
| `framegen.mode` | `fixed` / `target` (default) | which kind of multiplier, the row "Multiplier": 2x..8x / Target fps. Never `off` any more |
| `framegen.multiplier` | 2..8 (default 2) | fixed mode's multiplier; normalised to 2..8 on load (below 2 -> 2, above 8 -> 8) |
| `framegen.target_fps` | 0, 30..1000 | target mode: 0 = the display's refresh; below 30 clamps to 30, above 1000 to 1000 |
| `framegen.priority` | `low_latency` / `smoothness` (default) | what pacing trades under jittery frame times |
| `framegen.pause_at_refresh` | `true` (default) / `false` | stop generating once the game reaches the refresh (on), or keep generating above it (off); additive, an older config loads `true` |
| `framegen.pause_when_cursor_visible` | `false` (default) / `true` | the row "Pause while cursor visible": real frames pass through while the game's cursor shows (see [its section](frame-generation-pacing.md#pause-while-cursor-visible-framegenpause_when_cursor_visible)); additive, absent loads `false`, schema stays 5 |
| `framegen.gpu_limit` | `false` (default) / `true` | the row "Limit to GPU speed": the cost guard (see "Cost guard" in the pacing section) on (lower the output rate, then pass through) or off (always generate at the chosen rate); additive, absent loads `false`, schema stays 5 |
| `framegen.quality` | `quality` / `performance` | the one Quality row for [both motion versions](frame-generation-motion-v2.md#motion-version-2). On V2 it picks V2's Quality / Performance mode. On V1: Performance = flow scale 4, sub-pixel off: roughly 35-50% cheaper, can miss thin fast detail. Help quotes the measured GPU ms per game frame on the development GPU (estimate + generated frames; Quality / Performance): 1080p 0.35 / 0.42 / 0.49 vs 0.19 / 0.26 / 0.31 at 2x / 3x / 4x, 1440p 0.50 / 0.62 / 0.71 vs 0.25 / 0.36 / 0.46; PSNR at 2x 29.15 vs 28.33 dB. Asked for 2026-10-04: *"actually show how much faster / slower quality to performance mode roughly is"* |
| `framegen.safety` | `off` / `very_low` / `low` / `default` (default since 2026-10-28) / `high` | trust ramp (40,68) / (16,56) / (12,40) / (8,28) in 8-bit levels on the 7x7-averaged mismatch of the two warped frames (below `trustLow` interpolated, above `trustHigh` the nearer real frame, blended between); the help states each level's range plus the 15% whole-frame and scene-cut fallbacks. `off` disables every fallback: trust (254,255), `globalFallback` 1.0, `sceneCutSad` 255 -- never the real frame, not on fast flicks or scene cuts (smoothest, visible smearing and blended cuts). The others keep the library's 0.15 / 30. `very_low` (2026-10-28, packed as safety value 4 with its third bit in bit 30 of the main config word) `Why:` ml-fg measured the ramp with Learned e2v5 + zero test at x2: (12,40) gave 0.16% ghost pixels and 1.3% whole-frame fallbacks; (40,68) gave +0.28 dB on game clips and +0.30 dB on video clips, 0.28% ghost pixels and ~0 fallbacks. A later ramp is smoother and holds the real frame far less often, but ghosts more where the motion is wrong |
| `framegen.hud_protection` | `off` / `normal` / `strong` (default) | zero-vector bonus 0 / 1 / 2.5: a soft "prefer still" bias for see-through HUD. It no longer gates anything else (until 2026-10-04 it also switched the crosshair protection on) |
| `framegen.ui_protection` | `off` / `crosshair` (default) / `crosshair_v2` / `whole_screen` | the library's [UI protection](frame-generation-ui-protection.md#ui-protection); additive (schema stays 5), an older config loads `crosshair`, an unknown string keeps it (`crosshair_v2` added 2026-10-05) |
| `framegen.motion_version` | `v1` (default) / `v2` / `learned` | the row "Motion version": the library's [Motion version 2](frame-generation-motion-v2.md#motion-version-2) on (any built format, capable GPU) or the original V1; additive, absent loads `v1` (2026-10-22); `learned` = [Learned motion](frame-generation-learned.md#learned-motion) (2026-10-26) |
| `framegen.learned_net` | `e2v5` (default) / `e1v5` / `e2v4` / `e1v4` / `e2v3` / `e1v3` / `g2v2` / `g2v3` / `m2v2` / `m2v3` (round 4b) | the row "Learned net", used only while Learned runs; additive, an unknown string keeps `e2v5` (`e2v5` / `e1v5` and the new default are round 3, 2026-10-27; round 2 defaulted to `e2v4`, round 1 to `e2v3`, and since saving writes every key, a profile saved by an earlier build keeps its explicit net) |
| `framegen.learned_half` | `false` (default) / `true` | the row "Learned precision" (Full / Half), used only while Learned runs; additive (2026-10-26) |
| `framegen.learned_zero_test` | `true` (default) / `false` | the row "Zero-motion test", used only while Learned runs; additive (2026-10-26, round 2) |
| `framegen.learned_static_guard` | `false` (default) / `true` | the row "Static guard", used only while Learned runs; additive (2026-10-26, round 2) |
| `framegen.ui_box_height` | float percent of the game's height, `2.5` (default), clamped to 0.5-10 on load | the row "Crosshair box size" ([box size](frame-generation-ui-protection.md#crosshair-v2-and-the-box-size)); additive (schema stays 5), absent loads 2.5; written rounded to 0.1 |

**Migration** (on load; save writes `enabled`, `mode`, `multiplier`, `target_fps` and never
`"off"` again): `mode: "off"` -> `enabled = false`, `mode = "fixed"`, the stored
multiplier kept (normalised to 2..8); `mode: "fixed"` / `"target"` with no `enabled` ->
`enabled = true` (a `fixed` with multiplier below 2 stays disabled, as it used to load as
off); no `mode` and no `enabled` (written before Target fps) derives from the old
`multiplier`: 0 -> disabled, 2..8 -> enabled + `fixed`, so an old 3x profile stays 3x. An
explicit `enabled` always wins. Unknown strings keep the default. Since the mode default
became `target` (below): a section with **neither** `mode` nor `multiplier` keeps the
compiled-in `target`; only a legacy section that carries a `multiplier` (with no or an
unknown `mode`) loads as `fixed`, so what an existing user configured is unchanged.

**Defaults are the user's tuned values (2026-10-04).** Why: the user tuned Frame generation,
Motion blur and Lag spike buffer by hand in their `Test` profile and chose those as the
out-of-the-box values; the master switches stay off (*"All of them are off by default, but
the settings themselves are correct."*). Changed from before: `mode` `fixed` -> `target`,
`priority` `low_latency` -> `smoothness`, `safety` `default` -> `off`, `hud_protection`
`normal` -> `strong`; everything else already matched. Kept in step in `ConfigSchema.h`,
`fghost::Config` (also `multiplier` 0 -> 2) and each row's `.Default()` in
`PanelFrameGen.cpp`; Motion blur and Lag spike buffer already matched in all three places.

**Artifact safety default changed back to `default` (2026-10-28).** By user decision, to
match browser-frame-gen. `Why:` ml-fg measured that Off has no whole-frame or scene-cut
fallback, while Default (ramp 12,40) holds the real frame on bad motion. Changed in the same
three places (`ConfigSchema.h`, `fghost::Config`, the row's `.Default()` and its unknown-string
fallback). Saving writes every `framegen` key, so a profile saved earlier already stores its
`safety` explicitly and keeps it (a child profile only stores differences, and a missing key
inherits the parent's value); only a profile with no `safety` key loads the new default.
No migration.

**Layout** (2026-11-07): two columns (`a.Columns( 2 )`, every group pinned with `GroupColumn()`).
Column 0: group "Frame generation" (Frame generation, Multiplier, Target fps, Priority, Motion
version, Quality), then "Artifact protection" (Artifact safety, Static HUD protection, UI protection,
Crosshair box size). Column 1: "Learned motion" (Learned net, Learned precision, Zero-motion test,
Static guard), "Pausing and limits" (Pause at refresh rate, Pause while cursor visible, Limit to
GPU speed), "Status". Priority and Quality are `.Segmented()` buttons (as is Motion blur's Measured
against and Quality), because the user wanted them "like Motion version"; a row that cannot fit
falls back to a dropdown. `Why:` the user arranged this on a visual workbench.

Rows, in the order they appear (column 0, then column 1). **Frame generation** group: **Frame
generation** (a switch, `framegen.enabled`; help *Generates extra frames between the game's own
frames. Adds delay, so it is not suited to twitch shooters.*), **Multiplier** (2x / 3x / 4x / 5x /
6x / 7x / 8x / Target fps; it sets `mode`, and `multiplier` for a fixed choice, and is keyed to
`framegen.mode` -- the Shell's overridden-dot follows one key per row), **Target fps** (a slider, 0
shown as "Display refresh", disabled unless Multiplier is Target fps; a drag into 1..29 snaps to 0
or 30), **Priority** (Low latency / Smoothness), **Motion version** (V1 / V2 / Learned), **Quality**.
**Artifact protection** group: **Artifact safety** (Off / Very low / Low / Default / High),
**Static HUD protection**, **UI protection** (Off / Crosshair / Crosshair V2 / Whole screen),
**Crosshair box size** (a slider, `2.5% · 24 px`, disabled while UI protection is Off). Column 1,
**Learned motion** group: **Learned net**, **Learned precision**, **Zero-motion test**, **Static
guard**. **Pausing and limits** group: **Pause at refresh rate** (a switch; help: stops at the
refresh rate or the Target fps, whichever is lower), **Pause while cursor visible**, **Limit to GPU
speed**. **Status** group: the status line. Multiplier and the rows after it are disabled while the
switch is off. The rail summary reads "off", "N×" or "target N". Priority help: *Low latency adds the least
delay, but motion can stutter when the game's frame times jitter. Smoothness spaces the
frames perfectly evenly and adds about one game frame of delay.*

`Why two rows:` the user, after testing the single combined choice: *"There should be a
single toggle to enable and disable framegen in general, and then below that should just
be the multiplier. It shouldn't be combined into one."* Internally `fghost::Config` gained
`bool enabled` (packed word bit 26, `Enabled()`/the gate follow it) and `Mode::Off` stays
in the enum only because `pacing.h`'s `Mode` mirrors it (`steamcompmgr.cpp` static_asserts
the two match); the config never produces it.

`Why:` the library's expert dials (searchPenalty, smoothBonus, sceneCutSad, globalFallback)
are deliberately not exposed -- only the library's defaults were visually reviewed, and raw
sliders would invite settings nobody has looked at.

**Status line** (always one line):

- generating, target: `60->280 · target 280 (×4.6) · +13 ms` (game fps -> presented fps,
  the target, the effective multiplier to one decimal, the delay in whole ms);
- generating, fixed: `60->120 · 2× · +8 ms`; when the display cannot show N x the game the
  effective multiplier follows: `60->280 · 8× (×4.6) · +13 ms`;
- the FG / UI GPU times are **not** on the line any more (`Facts` rows have no details pane);
  they remain in the `framegen` debug log and `RenderStatus::lastPairGpuMs` / `lastUiMs`;
- while generating with V2 wanted but refused, ` · V1: <the library's reason>` is appended (see [Motion version 2](frame-generation-motion-v2.md#motion-version-2); the Motion blur Status line does the same for the blur);
- otherwise the reason (renderer's first, then pacing's), `Off`, or `Waiting for frames`.

It writes `->` and 1/2 in help text because the overlay font atlas is Latin-1 only (no
U+2192 or the vulgar-fraction glyphs); `·` and `×` are Latin-1 and render. `Why:` the line
was trimmed 2026-10-04 because the user found it overloaded: *"The Status line is pretty
overloaded at the moment. Reduce it to (example values): 60->280 * target 280 (x4.6) * +13ms"*.
**Status
semantics:** `presentedFps` counts output frames actually presented (generated frames plus
the real frames pacing decided to show) and **not** repaints of an already-shown output
(cursor, overlay) -- it used to be a paint count; `activeN` = the effective multiplier
rounded, at least 2 while generating and 0 when passing through, but that is the pacer's instantaneous
plan (it reads 2 at an effective 0.9x too); the steady "visibly generating" signal is
`generating` / `steadyMultiplier` (library PR #15, gate removed by PR #16), stamped with `publishedNs`, which the
HUD's *FPS shown* Both separator uses (see [fps-display](fps-display.md)); `chosenN` = the fixed N, 0 in target mode; `delayMs` = D.
A vblank that holds a real frame while the next one is still to come re-presents it and is
not counted, so under Low latency with a saturated display the distinct-frame rate is a
little below the refresh (Smoothness fills it).

### Logging

`framegen` (renderer) lines come from `FrameGenHost.cpp`; `framegen_pacing` (pacing/status)
lines from `steamcompmgr.cpp`:

- info, on every change of mode / multiplier / target / priority, of generating vs passing
  through, of the pass reason or of the renderer's reason: `frame generation: <mode>,
  <priority>, game .. fps, presented .. fps (x, generating|passing through), delay .. ms,
  pacing: "<reason>" (n), renderer: <text>`;
- the status lines also carry the motion blur part (`motion blur off` / `motion blur 4
  samples, 50% shown frame, gaussian, window 8.3 ms`) and are keyed on blur's settings and
  whether it is applied too;
- info on `frame generation: on, <mode>, <priority>, motion blur on|off` at start and
  `frame generation and motion blur: off` when both are off;
- info (since 2026-10-06; was debug), at most every 5 s while on: the same numbers (`frame generation status: ...`).
- The framegen status lines end with `draw mean/max` (wake to end of Present over the last 60 paints), `fg gpu: estimate/synth/pair/ui` (GPU timestamp queries, `n/a` if unsupported) and `missed K/N (>lead), L/N (>refresh)` (paints in that window that ended after their wake-up lead's vblank, and paints longer than one whole refresh; the second is comparable across builds, lead kinds and VRR states, the first is not), to tell raw FG GPU cost from queue-wait behind the game. The 5 s periodic line is info level too (2026-10-06), so a steady-state shortfall (e.g. 257-279 presented against a 280 target while still "generating") is visible; the longer state-change line is unchanged. The lead reads `(measured, max draw ...)` (non-VRR only), `(vrr)` or `(default)`.

Not keyed on the rounded multiplier: a game at a fractional ratio would flap between two
values and spam the log.
