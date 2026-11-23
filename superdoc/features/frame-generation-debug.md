# Frame generation: Debug options

Part of [Frame generation](frame-generation.md). Test views and readouts for judging what frame generation does. `Why:` the user asked for them on 2026-11-23 to see what frame generation does (filtered UI, only the generated frames, where motion was found, a stats overlay).

## The switch

The row **Debug options** (`framegen.debug`, a switch, default off, never greyed, in group "Frame generation" beside **Expert settings**) shows the group **Debug**, the last group of the area (after "Motion search"). While the switch is **off, everything in the group is off**: `config::FrameGenEffective()` (`ConfigManager.cpp`) hands the host `debug_only_generated = false` and `debug_stats = false`, whatever is stored; the stored values are kept and act again when the switch goes on. Persisted per profile, like Expert settings (there is no session-only mechanism). The rows are shown with `Entry::ShownWhen(DebugOn)`, so they are also absent from keyboard navigation and the command palette while the switch is off, and greyed with "frame generation is off" while frame generation is off.

| Key | Default | Row |
|---|---|---|
| `framegen.debug` | false | "Debug options" (master switch) |
| `framegen.debug_only_generated` | false | "Only generated frames" |
| `framegen.debug_stats` | false | "Stats overlay" |

Additive (2026-11-23), schema stays 5; absent or wrong-typed loads false.

**The path to the renderer.** The packed host `Config` word is full, so the debug values travel in their own atomic, `fghost::DebugConfig` (`FrameGenHost.h`; `SetDebugConfig()` / `GetDebugConfig()`), packed by `PackDebug()` in `src/FrameGen/FrameGenDebug.h` (bit 0 only generated, bit 1 stats, bits 2-5 `view`, reserved for the later debug views, 0 = off, unused so far). `PanelFrameGen.cpp`'s `PushToRenderer()` pushes the EFFECTIVE values, on every edit, load and profile switch.

## Only generated frames

Shows only generated images; the game's own frames are skipped on screen. With Priority Low latency at fixed 2x the picture then updates at half the rate (a generated image, the same one again, a generated image, ...); at 3x it is gen, gen, held. With a Target above what the game delivers a held image can repeat several ticks. While nothing is generated (pass-through, inert, frame generation off) the game's frames show, so the picture never freezes.

**Design** (`RecordBaseLayer()`, `FrameGenHost.cpp`, at the very end, after the stale-guard block and before the submit). The pacer is not touched and the real tick is NOT skipped: its paint still does the ring copy, the observe and the pair bookkeeping that later pairs and UI protection need. Only the image that is returned changes:

1. A tick that returned a generated output (`!IsRealTick`) stamps its pool slot as the "last generated" (`Host_t::nLastGenSlot`, `ulLastGenOutId`, `ulLastGenNs`) and bumps that slot's `ulUse`.
2. A **real tick** (`IsRealTick(prevId, t1)`: no pair, or `t1 >= 1`, which is how a Motion-blurred real frame looks) shows that slot's texture instead, if `ShouldHoldGenerated()` (`FrameGenDebug.h`) agrees: option on, the interpolator live, the slot still valid with the same `outId`, and the last generated image not older than `kHoldMaxNs` (100 ms) and the pacer's own `GetPacingStatus().generating` set (a status not older than 500 ms). It bumps `ulUse` again.

`Why` in `RecordBaseLayer()` and not in the pacer: the pacer's decision drives the ring copy and the observe; dropping the real tick there would starve the ring and the UI-protection history. `Why` the `ulUse` bump: the output pool has three slots and `PickOutSlot()` is LRU; a held image must stay the most recently used so the next synth does not overwrite an image a composite still reads. `Why` the 100 ms cap and the `generating` check: a stalled generator, a scene cut or a stretch where the cost guard passes frames through must not keep an old generated image on screen; the pacer flag ends the hold at once (it is republished about every 250 ms, so it can lag), the cap bounds that lag (100 ms is a 10 fps game's frame; slower games show the real frames). The hold is skipped when the interpolator is not live (`H.bLive`) and for inert or pass-through requests, which return before the hold code. `DropFrames()` / `InvalidateOutputs()` clear slot validity, so a reset or a size change drops the hold.

**When it has an effect: Priority matters.** A "real tick" exists only where the pacer shows the game's own frame (no pair, `t = 1`). With Priority **Low latency** a fixed 2x alternates generated, real, generated, real, so Only generated frames shows each generated image twice and the shown rate stays 2x the game's. With Priority **Smoothness** (the default) the pacer already plays every shown frame as a synthesised image between the pair (phases such as 0.4 and 0.9), real frames are shown only in pass-through stretches, and the option changes almost nothing there. Smoke at fixed 2x with a 30 fps client, Low latency: real and generated ticks about 1:1, every real tick held (counter lines `100 real ticks shown ... (of 101 real, 100 generated ticks)`), shown rate 60 fps with the option on and off.

**Known side effect:** the judder meter's content time still assumes the real frame was shown, so judder numbers are wrong while the option is on. Motion blur together with Only generated frames (a blurred real frame counts as a real tick) is designed for but was not run.

## Stats overlay

The row only switches it (`debug_stats`; `fghost::GetDebugConfig().stats`, wired to `fgstats::SetEnabled()` in `PushToRenderer()`). It is drawn by `src/Overlay/FgStats.{h,cpp}`; see the section below.

Rows of text in the top left of the picture: line 1 the motion version (V3 Hybrid with its variant, `A-A(off)` when the blend is selected but not running), the quality and the format tag; line 2 game fps -> shown fps, the effective multiplier (or "passing through") and the mode (fixed Nx / target fps / display refresh); line 3 the added delay in ms and the frames per second the host discarded (nested Wayland only, else `-`); line 4 the library's GPU times (estimate, synth, pair, UI protection, ms); line 5 the UI protection mode (Whole screen V2 by that name); line 6 `not generating: <reason>`, only when there is one. With frame generation off it is the single line `frame generation off`; a missing number prints `-`, never 0. The text is refreshed at most 4 times a second. Formatting is pure (`FormatLines`) and unit-tested.

`Why` it shares the HUD's layer: it draws into the same draw list and texture as the FPS readout and the crosshair, so it costs no extra layer slot (6 at most; another one forces a full composite), and frame generation never sees that layer. It is NOT part of the HUD (the Profiler was removed at the user's request on 2026-09-03; the HUD stays a plain FPS integer): own files, no HUD setting. A top-left HUD readout pushes it below the readout. `Why` `fghostshown::TakeRates()` is not used: its function-static baselines belong to the status line; the overlay keeps its own previous raw counters. `Why` the draw-time mean and max are missing: `CVBlankTimer::TakeStats()` resets what it reads. It is not in `gamescopectl screenshot` (screenshots exclude the HUD layer).

Test aids: the ConVar `ritz_fg_stats 1` shows it whatever the switch says; `gamescopectl log_fgstats debug` logs the text when it changes. Not covered so far: the HUD's Inverted colour mode together with the block, and repaint while the game is idle with only the overlay on.

## Planned

A **Debug view** Choice (UI highlighted / UI removed / Motion map) needs a library debug view and comes with a later library pin. `DebugConfig::view` is reserved for it; no rows or keys exist for it yet.
