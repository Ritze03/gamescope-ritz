# Frame limiter

The Display > **Frame limiter** area (`display.frame_limiter`, `src/Overlay/PanelDisplay.cpp`, its
`display.fps_limit` stepper) caps the *game's* frame rate. The compositor side lives in
`src/FpsLimitApply.h` (what is pushed and when) and `g_uLimiterPaceFps`; for Vulkan games the pacing
itself happens in the WSI layer through `src/LimiterPace.h` ([vk-wsi-layer.md](vk-wsi-layer.md)).

## Frame generation and the Frame limiter

The Frame limiter caps the *game*; [frame generation](frame-generation.md) multiplies the
capped rate on screen, and never caps the game itself. refresh/N is the natural pairing
(144 Hz, limit 48, 3x). Frame generation reads the refresh the vblank timer ticks at: the
nested refresh if set, else the output refresh, and steps N down when the game is too fast
for it.

## The saved limit is applied at launch and on a profile switch (2026-11-04)

Nothing used to apply `gamescope.fps_limit` outside the stepper's own edit: the startup config
apply returns before the compositor exists, and the panel's reload push skipped it on purpose. So
the stepper showed the saved 120 while the game ran uncapped until the stepper was touched. Now
`PanelDisplay_ApplyConfigFpsLimit()` (`PanelDisplay.cpp`) runs once in `steamcompmgr_main()` as soon
as the Xwayland servers exist, and again from `main.cpp`'s live-apply hook on a profile switch (0
included, so a switch to a profile with no limit uncaps). It uses the stepper's own setter (the
`g_nCombinedAppRefreshCycleOverride` path, plus the `GAMESCOPE_FPS_LIMIT` property at startup), and
`update_runtime_info()` puts the rate in the limiter file, which the layer reads per present, so
the game may start before or after.

- `Why` only a change of the configured value is pushed (`fpslimit::NeedsPush()`,
  `src/FpsLimitApply.h`): the hook fires on every config change, and a limit set live meanwhile
  (the `gamescope_control` protocol, `debug_set_fps_limit`, the stepper) must survive an unrelated
  reload.
- An explicit `--framerate-limit` wins over the profile (`Opt::FramerateLimit`), and is now applied
  through the same setter: the bare variable it used to set was zeroed by the first frame's
  `update_app_target_refresh_cycle()`.

## The limiter holds every Vulkan game (2026-11-03)

The compositor only holds commits flagged FIFO (`wlserver_surface_is_fifo()` from the
`set_present_mode` the layer sends), and the layer always forces MAILBOX at the driver. The layer
used to report the app's own mode unless the app was "frame-limiter aware" (DXVK >= 2.3 /
vkd3d >= 2.12), so a game presenting MAILBOX / IMMEDIATE (vsync off: CS2, Forza under
vkd3d-proton, Minecraft's LWJGL Vulkan) was never held. While the limiter is active the layer now
reports FIFO to the compositor for every app (`layer/VkLayer_FROG_gamescope_wsi.cpp`,
`QueuePresentKHR`); aware apps still also see FIFO-only mode lists and get `OUT_OF_DATE` once on a
limiter flip, unchanged. `Why:` tearing is moot when the user asked for a cap, and the report is
only the compositor's pacing hint, not what the driver presents. Needs the layer's
`GAMESCOPE_LIMITER_FILE` (set by `main.cpp`) as before. Since 2026-11-04 this applies only while the
limiter is on the vblank grid; see the next section.

## The limiter paces Vulkan games in the layer (2026-11-04)

The compositor's limiter works at its paints: without VRR it releases every Nth vblank (an integer
divisor, so 120 on 280 Hz is really 140); with VRR a time schedule is only looked at when something
wakes the loop. A Vulkan game is now paced by the WSI layer instead: `update_runtime_info()` writes
a second word to the limiter file, the rate to pace to (`g_uLimiterPaceFps`, set in the main loop),
and after each present the layer sleeps to an absolute deadline
(`limiterpace::NextDeadline()`, `src/LimiterPace.h`, `clock_nanosleep`) and reports MAILBOX so the
compositor holds nothing. No per-frame traffic, no protocol: it reaches X11 bypass, X11 non-bypass
and native Wayland swapchains alike.

- **Who paces** (`limiterpace::LayerPaces()`): the layer, unless VRR is off, none of frame
  generation / motion blur / the lag spike buffer is on, and the limit divides the refresh. `Why`
  that case stays on the vblank grid: it is exact there and locked to the display, where a timer
  would beat against the refresh (60 on 59.94 Hz drops a frame every 16 s). A game without our
  layer (GL, plain X11) never reads the word and is limited as before, frame callbacks included.
- **Where it sleeps**: after the present, before the game's next frame, where Minecraft's own
  limiter sleeps, so input is read after the wait. Sleeping before the driver's present was tried:
  no better on output, and it delays a finished frame by the whole sleep.
- **Why not at the paints.** Traced with the paint times beside the game's acquire and present
  times: under the old limiter the game sat blocked in `vkAcquireNextImageKHR` for 6.0 ms per
  frame, and came back 0.01 ms (median) after a paint's wake-up, because that is when a held commit
  is let go and its buffer freed. With about 3 ms of CPU to record its frame it then presented
  right before the next paint, 883 of 3000 paints fell within 1 ms after a present, and those
  waited 5.4 ms on average, 7.4 at most, for the frame that had just been queued. The game's limiter
  and the layer's put the frame at whatever phase the timer gives, so it hits fewer paints, and a
  paint that is hit waits only for what is left of the burst. That is the gap between the 6.5 and
  3.5 ms maxima under the two limiters. It stopped mattering much once the wait itself was fixed,
  see [frame-generation](frame-generation-pacing.md#paints-behind-the-games-gpu-burst-2026-11-04), but the
  held game stays wrong: output locked at 2.00x, and a limit that is not the number asked for.
- `Why` a word in the limiter file and not the frame gate the Hold game switch used: the layer
  already reads that file at every present; the gate needs compositor traffic per frame, and a timer
  on the compositor's loop to make it exact.

Measured (Minecraft, 280 Hz, VRR on, Target = refresh, Smoothness, HUD fix in, same session, two
25 s runs each; the worktree build with the limiter path switched live):

| Limit | Old limiter | Layer |
| --- | --- | --- |
| 90 | game 89, output 121-164 | game 90, output 311 |
| 120 | game 118, output 243-281 | game 120.5, output 311 |
| 160 | game 157, output 310 | game 160, output 311 |

Before the HUD fix, and for comparison Minecraft's own limit of 120 in the same scene: old limiter
game 113-117, output 220-240; layer game 121, output 299-300; Minecraft's own game 119, output
300-301, `other` paints missing 35-41% in both. With VRR off the old limiter gave output 184-195
and the layer 302.

**Not verified live:** a game that does not bypass Xwayland (frame callbacks are still gated there,
so with VRR it may run a little under the limit); DXVK / vkd3d-proton games (the limiter-aware
path), Minecraft being the only game tested. With frame generation off the rate was checked
headless only (vkcube at 280 Hz, frames over time: limit 120 gave 119.5 on X11 bypass with FIFO,
119.8 with MAILBOX, 118.6 on native Wayland; limit 140, on the grid, 139.9; at 60 Hz, limit 60,
60.1), not by eye on a display.

## Related

- [frame-generation.md](frame-generation.md), [frame-generation-limits.md](frame-generation-limits.md) — the capped rate frame generation multiplies.
- [vk-wsi-layer.md](vk-wsi-layer.md) — the layer half of the pacing.
- [resolution-and-refresh.md](resolution-and-refresh.md) — where this page was split from.
