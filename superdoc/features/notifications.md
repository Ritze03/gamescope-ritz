# Notifications (toasts)

Self-dismissing on-screen messages the rest of the codebase fires via
`gamescope::Notifications::Show()` -- a profile applied, an override toggled, the
"Test notification" action. Code: `src/Overlay/Notifications.{h,cpp}`. Settings rows
(placement, mute, test) are registered from the same file into the Appearance area
(`RegisterRows()`); the scale/opacity sliders live in `PanelConfig.cpp`'s General tab
and write straight into `Notifications::g_LiveTheme`.

## Shape

The toast layer follows `FpsDisplay.cpp`'s pattern exactly, as a second independent
instance: its own ImGui context, its own full-output BGRA8 offscreen texture, a
general-queue submission fenced by its own timeline semaphore, and its own `Layer_t`
pushed from `AddLayer()` (called once per `paint_all()`, between the HUD and the Shell
-- `g_zposNotifications`). Toasts render every composited frame while queued, whether or
not the Shell is open. See `Notifications.h`'s header comment for the *why* behind the
independent context, and `superdoc/planning/DECISIONS.md` #25 for the
global-placement / per-game-mute split.

- Queue: at most 8 toasts (oldest dropped), 4 drawn at once, text truncated at 240
  chars with `U+2026`. Placement is one of nine anchors, always global; `muted` is
  per-game-eligible like any other setting.
- Font: `Style::Label` at `RasterSize(14 * notification_scale)` -- one (face, size)
  pair, returned by `ToastFont()` / `ToastFontSize()` so the draw and the warm-up
  below cannot disagree.

## The lazy-init hazard (requests-2026-09-05 item 6)

Everything the toast needs used to be created on the frame the first toast was drawn:
the ImGui context and font parse, `ImGui_ImplVulkan_Init()`'s shader modules and
`vkCreateGraphicsPipelines` (no pipeline cache), the offscreen texture, and two blocking
JSON reads in `EnsureConfigLoaded()`. The dominant cost, and the one that *recurs*, is
the font atlas: this ImGui build bakes glyphs **lazily**, per (font, integer pixel size),
on the first draw that needs them. Baking flags the atlas texture `WantCreate` /
`WantUpdates`, and `ImGui_ImplVulkan_RenderDrawData()` services that flag with a staging
upload followed by **`vkQueueWaitIdle()` on the general queue** (`imgui_impl_vulkan.cpp`'s
`UpdateTexture()`, its own "FIXME-OPT"). That queue is shared with the HUD's and Shell's
submissions, so the render thread stalls for a full drain -- on the first toast, and
again on every later toast whose text uses a glyph nobody has drawn yet.

Issue #30 had looked at exactly this and left it lazy on the theory that "nothing calls
`Show()` at startup, so first use is genuine first use". The user's report is why that
was wrong: *"When the first notification is being shown, there is always a small but
visible lag spike ... I would rather have a ghost lag at launch than a lag while being
mid-game."* Genuine first use **is** mid-game -- the first toast usually follows a
settings edit.

## The warm-up

`Notifications::WarmUp()` (idempotent, one attempt) is called from
`SettingsOverlay_AddLayer()` on the first frame with a known output size, **before** the
startup announcement's timer is armed and regardless of whether the announcement is
enabled. In order:

1. `EnsureConfigLoaded()` -- the two JSON reads, off the first toast's frame.
2. `EnsureImguiInit()` -- context, font parse, pipeline.
3. `EnsureTexture(g_nOutputWidth, g_nOutputHeight)` -- the offscreen target.
4. Two hidden frames: `NewFrame` -> `fonts::WarmGlyphs()` draws `U+0020..U+00FF` plus
   `U+2026` (the fallback glyph) with `ToastFont()` at `ToastFontSize()`, wrapped to the
   display width -> `Render` -> `RenderAndSubmit()`. The first frame does the atlas
   texture create/upload and its `vkQueueWaitIdle` once, at launch, with every glyph a
   later toast can use already resident (so the atlas never grows mid-game either). Two
   frames because the backend rotates through `ImageCount` (2) vertex/index buffer
   slots and allocates each on first use; the glyph string is longer than any toast, so
   those buffers never hit the resize path later.
5. **No `Layer_t` is pushed.** `AddLayer()` still early-outs on an empty queue, nothing
   composites the warm-up's pixels, and `s_bHasPendingWaitPoint` is cleared so
   `WaitForRender()` stays a no-op. Pushing a layer would make `layers.count() > 1` and
   force full composition every frame, defeating direct scanout for the whole session.
   The general-queue work is still fenced: the next real `RenderAndSubmit()`'s
   `DrainPrevSubmission()` waits on it before clearing the texture.

`WarmUp()` logs one info line with its wall time. If it never runs (output size unknown,
Vulkan init failed), every guard is still lazy and the old behaviour is the fallback.

**Why the Shell block, not `steamcompmgr.cpp`:** `SettingsOverlay_AddLayer()` already
owned issue #30's warm-up (the announcement racing its own setup) and runs on the same
thread one call before the announcement's first visible frame; extending it kept the
launch sequence in one place. That block now also runs a hidden glyph pass for the
Shell's own context -- every (face, size) the kit and `DrawStartupAnnounce()` draw with,
after applying the real `display_scale` (`palette::EnsureThemeLoaded()` ->
`RebuildAll()` -> `Pump/ApplyPendingRebuild()`), because baking against the 1.0x
bootstrap atlas would be wiped by that rebuild on first open. A side effect worth
knowing: the announcement now draws with the configured accent hue instead of the
compiled default.

**`display_scale` rebuilds re-lazy everything.** `fonts::RebuildAll()` (Palette.cpp,
PanelConfig.cpp's slider release) calls `ClearFonts()` on every context, which drops
every bake. The Shell's and Notifications' glyphs are lazy again from that point until
first use; the warm-up is **not** re-run on rebuild. Deliberate: a rebuild is a
user-initiated Appearance edit with the Shell open, so the one-time re-bake lands on an
interaction the user is already watching, not mid-game. Re-running the glyph pass from
`ApplyPendingRebuild()` is the obvious follow-up if that ever reads as a hitch.

**HUD seam.** `FpsDisplay.cpp`'s context is still fully lazy (init, texture and Hero-size
glyphs on its first enabled frame). The call site carries
`// TODO(agent owning FpsDisplay.cpp): FpsDisplay_WarmUp() here`; the intended shape is
identical to `Notifications::WarmUp()`.

## Verifying

`notifications_time_render` (ConVar, default off) logs the render-thread wall time of
every frame that actually draws a toast, plus the queue depth and whether the warm-up
ran. With it on: fire a toast right after a settings change (or `notify_test`) and read
the first frame's time against the following ones -- the first should be within noise of
steady state, not a multi-millisecond outlier. `overlay_e2_trace on` before the first
toast, then `dump`, confirms the composite-forced state does not flip on the warm-up
frame (no toast queued, no layer pushed).
