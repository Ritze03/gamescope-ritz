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
4. Two hidden frames (`RunHiddenFrame(true)`): `NewFrame` -> `fonts::WarmGlyphs()` draws
   `U+0020..U+00FF` plus `U+2026` (the fallback glyph) with `ToastFont()` at
   `ToastFontSize()`, wrapped to the display width -> `Render` -> `RenderAndSubmit()`.
   The first frame does the atlas texture create/upload and its `vkQueueWaitIdle` once,
   at launch, with every glyph a later toast can use already resident (so the atlas
   never grows mid-game either). Two frames because the backend rotates through
   `ImageCount` (2) vertex/index buffer slots and allocates each on first use; the glyph
   string is longer than any toast, so those buffers never hit the resize path later.
5. **No `Layer_t` is pushed.** `AddLayer()` still early-outs on an empty queue, nothing
   composites the warm-up's pixels, and `s_bHasPendingWaitPoint` is cleared so
   `WaitForRender()` stays a no-op. Pushing a layer would make `layers.count() > 1` and
   force full composition every frame, defeating direct scanout for the whole session.
   The general-queue work is still fenced: the next real `RenderAndSubmit()`'s
   `DrainPrevSubmission()` waits on it before clearing the texture.

`WarmUp()` logs one info line with its wall time and the texture size. If it never runs
(output size unknown, Vulkan init failed), every guard is still lazy and the old
behaviour is the fallback.

### The residual cost: the texture is keyed on the output size

With all of the above in place a fresh launch still measured the first toast frame at
3-5 ms against a 0.1-1 ms steady state (three trials, `notifications_time_render 1`,
text entirely inside the baked range). Everything `AddLayer()` does inside that timer
runs identically every frame -- the glyphs, the bake lookup, the buffers, the command
buffer, the submit -- except `EnsureTexture(g_nOutputWidth, g_nOutputHeight)`. The
output size can change **after** the warm-up: in nested mode the SDL backend rewrites
`g_nOutputWidth/Height` from its event thread on `SDL_WINDOWEVENT_SIZE_CHANGED`, and a
tiling compositor resizes the window right after it maps, i.e. after the first sized
frame the warm-up keys off. The first toast then re-creates the full-output BGRA8
texture (`vkCreateImage` + `vkAllocateMemory`, ~8 MB at 1080p), re-arms the
`UNDEFINED->GENERAL` barrier, and its `vkQueueSubmit` faults the fresh backing pages
in -- once, on the render thread, inside the timed window.

`ReWarmTextureIfResized()` closes it: on every frame with no toast queued, if the output
size differs from the texture's and has held that value for 500 ms
(`kResizeSettleNanos`), the texture is re-created and one glyph-less hidden frame is
run, then the wait point is cleared exactly as in `WarmUp()`. It is settle-gated so an
interactive window resize (a new size per frame) does not allocate a texture per frame,
and wall-clock rather than frame-counted so it means the same thing at 40 Hz and
144 Hz. The cost lands once, half a second after the resize ends, on a frame with nothing
on screen. A toast that is already up during a resize takes `AddLayer()`'s existing lazy
path (re-created on its next frame) and the settle clock is reset by the size match.
Logged as `re-warmed the offscreen texture at WxH after a resize`. **Not mirrored in
the HUD**: an enabled HUD draws every frame and so re-creates on the resize frame
itself; a disabled HUD has no texture. The Shell re-creates lazily on its next drawn
frame, which is a user opening it.

### The composite pipeline (outside the timer)

`CVulkanDevice::compileAllPipelines()` precompiles every (layer count, ycbcr, blur)
combination on a background thread, but its `PipelineInfo_t` keys default the trailing
members: `colorspaceMask = 0`, `outputEOTF = 0`, `itmEnable = false`. A real composite
keys on `frameInfo->colorspaceMask()`, and any sRGB layer makes that non-zero -- so with
a game on screen the precompiled set is never hit, and every new (layer count,
colourspace) pair compiles a compute pipeline synchronously in `pipeline()`, on the
render thread, on the first frame that needs it. The first toast is such a frame
whenever it brings a layer count the session has not composited yet (HUD on: game + HUD
+ toast = 3), and the compile happens in `vulkan_composite()` after `AddLayer()` returns,
so `notifications_time_render` never sees it. Upstream behaviour, not this fork's.

The launch block now calls `vulkan_warm_overlay_composite_pipelines(pFrameInfo, 2)`
(`rendervulkan.cpp`) after the three warm-ups: it pushes one, then two, screen-size sRGB
layers onto a **copy** of the launch frame and asks `pipeline()` for the key the final
pass would use each time (RCAS under FSR, BLUR/BLUR_COND if `blurLayer0` is already set,
BLIT otherwise -- NIS included). Synchronous, deliberately: it is launch time, the user
asked for launch cost over mid-game cost, and with the startup announcement enabled the
+1 key was already being compiled synchronously on that very frame by the
announcement's own composite. The frame is not modified; nothing is pushed onto it.

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

**HUD.** `FpsDisplay_WarmUp()` is the third leg of the same launch block, same shape as
`Notifications::WarmUp()` (init, texture, hidden glyph frames, no `Layer_t`). A HUD that
is off at launch has nothing to warm and stays lazy until it is switched on; that first
enabled frame pays the one-time cost, by design ("nothing enabled -> nothing allocated").

## Verifying

`notifications_time_render` (ConVar, default off) logs the render-thread wall time of
every frame that actually draws a toast, plus the queue depth, whether the warm-up ran,
the texture size, and `recreated=` -- whether that frame had to (re)create the texture.
With it on: fire a toast right after a settings change (or `notify_test`) and read the
first frame's time against the following ones -- the first should be within ~1.5x of
steady state, not a multi-millisecond outlier. A first-toast line with `recreated=1`
means the output size changed after launch and the settle re-warm did not get its
500 ms (or the toast came first); `recreated=0` with a spike points elsewhere.

Two log lines say what the warm-up did and did not do, and they are the check that the
hidden passes push nothing (there is no other: `overlay_e2_trace`'s `TraceFrame()` is
called only from `Shell.cpp` and never observes the notification layer):

- `notifications: warm-up done in N ms (... texture WxH, layers pushed: 0)` -- the
  literal is a statement of the contract; `WarmUp()` has no `FrameInfo_t` to push to.
- `settings_overlay: launch warm-up done in N ms (... composite pipelines for A and B
  layers in M ms; layers pushed: 0)` -- the count is measured: `pFrameInfo->layers.count()`
  after the whole block minus before it. Anything but `0` there is a bug.
