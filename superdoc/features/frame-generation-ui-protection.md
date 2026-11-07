# Frame generation: UI protection

Part of [Frame generation](frame-generation.md). Keeping the game's still HUD and crosshair exact: detectors, Crosshair V2, the box and its preview.

## UI protection

The **UI protection** row (`framegen.ui_protection`: Off / **Crosshair** (default) /
**Crosshair V2** / Whole screen) keeps a still crosshair, or with Whole screen every solid
still HUD element, pixel-exact on generated frames. It is **independent of Static HUD
protection**. Two more things sit with it: [Crosshair V2](#crosshair-v2-and-the-box-size)
and the **Crosshair box size** slider, with a [live preview](#the-box-preview) in the Inspector.

**It lives in the FrameGen library, not in this repository.** The library work is
[Ritze03/frame-gen-ritz PR #1](https://github.com/Ritze03/frame-gen-ritz/pull/1) (branch
`ui-protection`, merged; pinned here at FrameGen main `b699301`, which includes PR #1, UI protection); the host only maps the setting and feeds frames
in. The first version of this feature was host code (three shaders of our own, ROI-sized
textures per ring slot). `Why` it moved: the user, on where it belongs: *"It should really
be part of the library ... especially since the repository will be public in the end ... so
if anyone actually decides to use it, he can just implement it easily."* A host-side
crosshair hack that every user of the library would have to rewrite is the opposite of that.
The old `cs_fg_crosshair_*.comp` / `cs_fg_inpaint.comp` shaders, their `SHADER_TYPE_FG_*`
registrations, `CVulkanCmdBuffer::uploadConstantsRaw` and the per-slot patch textures were
deleted; the library's six shaders (`ui_detect`, `ui_box_update`, `ui_box_mask`, `ui_mask`,
`ui_inpaint`, `ui_patch`, plus the include `ui_common.glsl`) are compiled by
`src/meson.build` like its other eleven.

`Why` a mask beats Static HUD protection's `hudBonus`: `hudBonus` only biases the block
motion search towards a zero vector, and a static crosshair shares its block with the
moving background behind it, so the block still gets one vector (the background's drags the
crosshair, or the crosshair's freezes the background) and the output is an interpolation,
never exact. The mask instead *removes* the still pixels from what the estimate and warp
see (the library's clean copy has them inpainted from their surroundings) and pastes the
**exact real pixels** back over each generated frame. That is why the old text of the
Static HUD row ("keeps a still crosshair sharp") was wrong, and the row now only describes
the soft bias, which still matters for see-through HUD the mask cannot cover.

The user's original request (after testing in a real game, "looks REALLY good"): *"One
thing that many frame gens get wrong is the crosshair [...] We won't optimize for like weird
big crosshairs, but like the middle one to maybe three percent of the screen [...] It should
analyze multiple frames and can easily tell if the crosshair isn't moving. And then it should
detect it, that it is the crosshair. Then the image that is being used to generate frames
should have this crosshair removed and at a later point patched on again. This of course
doesn't work as well if there's transparency, but it should work for most games."*

**What the library does** (full contract in `subprojects/FrameGen/gpu/framegen.h` and
`INTEGRATION.md`): a pixel is *still* if no channel moved more than `uiStillThreshold`
(3.5 levels) between two consecutive real frames, and is protected after `uiStillFrames` (8)
consecutive still frames. Crosshair mode watches a centred square of side
`round(H * uiBoxHeightFrac)` (default 0.025, i.e. 36 px at 2560x1440; until 2026-10-05 it was
`round(sqrt(0.03 * w * h))`, 332 px there -- see [the box size](#crosshair-v2-and-the-box-size));
Whole screen watches every pixel in cells and protects the still ones, giving up on a frame
where more than `uiMaxStillFraction` (0.75) of it is still (a paused game, a loading
screen). Per observed frame the library keeps its own **clean copy** with the protected
pixels inpainted (reach `uiInpaintReach` 12 px); estimate and synth run on the clean copies,
and every synth ends with `out = mix(out, real pixel, mask)`, reading the real pixels from
the `curr` view it is given. All of these are runtime `Settings` fields; the host sets only
`uiProtection` and `uiBoxHeightFrac` (the slider) and leaves the rest at the library's defaults.

**Host wiring** (`FrameGenHost.cpp`):

- `recordObserve(cb, ringSlotView)` is called **once per new real frame**, in the command
  buffer that copied it into the ring, right after the ring copy and its barrier, **also
  for frames that are only passed through** (the stillness counters compare consecutive real
  frames; a gap costs only the exact "8 in a row"). While the pacer is **inert** the renderer
  tracks instead (`recordTrack`, see below). Off: the call is skipped entirely.
- The **ring holds the real, un-inpainted frame**: `recordSynth` must get the real `curr`
  to paste from. The library keeps `uiHistory` (default `kUiFrames` = 3) clean copies, LRU by
  view; the ring re-observes exactly one slot per new frame and `uiHistory` follows the ring's
  depth, so the library holds exactly the ring's frames and a pair whose frames are both in the ring is always protected. A pair
  with an unobserved view (protection just switched on) simply runs unprotected.
- If `recordSynth` returns false with UI protection on (an observed frame of the pair was
  evicted or re-observed after the estimate, or a descriptor ring is full), the real frame
  is shown for that slot and the estimate is dropped: the same path as a failed record but
  **without** the "A generation step failed" status reason and without disabling FG. It
  logs once under `framegen`. It should not occur with the 3-slot ring: the pacer never
  needs a frame that has left the ring (`bEstimateValid` is cleared the moment the pair's
  frames are overwritten, before the next observe).
- `setSettings()` is a runtime change (no GPU wait): `ToLibrarySettings()` maps the config
  to `uiProtection`, and the packed config word carries it at bits 24-25 so a change goes
  through the same applied-preset check as Quality/Artifact safety/Static HUD protection.
  If the library refuses (`false`), the old settings stay and it logs once.
- The library has a **third descriptor ring**, `kUiSets = 64` (separate from the two of
  32). Per composite the host records one observe (1 set Crosshair, 2 Whole screen) plus
  one patch set per synth, far below it (`kMaxSynthsPerPair` is 24).

**Tracking while passing real frames through (the inert state, 2026-10-04).** When the
pacer is inert (the game reaches the refresh / target with Pause at refresh rate on, the
cost guard, ...) the renderer does no ring copy and no observe, so the library's stillness
counters used to stop at the last frame before the stretch. They survive that gap, but then
compare the frame before it with the one after, so UI that **changed meanwhile** (a
recoloured crosshair, a scope, a HUD update) restarted cold at generation's resume and could
shift for `uiStillFrames` real frames. The user: *"make sure that it still keeps track of
like static UI elements, like the crosshair, even when it's temporarily deactivated because
the FPS is high enough ... so it doesn't start shifting around once it starts generating
frames again."* `RecordBaseLayer()`'s inert branch now calls `TrackWhileInert()`: **one
`recordTrack()` (library PR #9) per NEW real frame**, in a command buffer of its own that is
built and submitted only for such a frame (never an empty one per composite). Rules, pinned
by the pure `ShouldTrackUi()` / `LayerFitsHost()` (`FrameGenFormat.h`, `tests/test_framegen_format.cpp`):
UI protection on; the Interpolator live; the layer still the format plan / size / colourspace
it was created for (the D4 gates: an unsupported layer is not tracked); and a commit id the
library was not already handed (`Host_t::ulUiFedId`, set by an observe **and** a track), so
a repaint of the same frame (cursor, overlay) or the frame observed on the way into
pass-through is never counted twice. A frame is tracked **or** observed, never both; on
resume the next real frames go through `recordObserve` as always (ring copy and clean copy
return), and the first pair is (first observed frame, the one after it). The game's own texture is
sampled directly (its raw UNORM view, GENERAL layout after `prepareSrcImage` +
`insertBarrier`): no ring copy, and the values equal what the ring would hold (the copy is
exact; the library reads rgb only). **Cost: ~0.003 ms of GPU in Crosshair mode, ~0.02-0.03
ms in Whole screen**, plus one small command buffer per real frame on the CPU, only while
inert. `Why` no lazy Interpolator init for tracking: pacing drives the renderer (and so
`EnsureReady()`) through its warm-up before it can ever say "game too fast", so a not-live
host while inert means generation never started and there is nothing to keep warm; an init
only for tracking would buy a pipeline-build stall. `Why` nothing else had to change: the
audit of what could reset the counters found only legitimate triggers: `DropFrames()`
forgets the host's ring and cached outputs, never the library's state; the inert branch
returns before `EnsureReady()`, so no `resize()` / `reconfigure()` / `setSettings()` runs
during the stretch; at resume `setSettings()` (UI-history follow, `FollowDepth`, and the
preset change at `EnsureReady`) only zeroes the box counters when UI protection itself goes
Off or comes On (`uiAdopted()`), and `Teardown()` (the format class / FG-off) is a true
reset by nature. Known edges: a UI protection **change made while inert** (Off -> On, a
different mode) is only applied at resume (`EnsureReady` is not run), so that stretch tracks
with the old setting and the counters start cold at the change, as before; a resume on the
very frame last tracked observes it again (one still-step of over-count, harmless).
The debug log (`framegen` scope) counts tracked frames ("UI protection: tracking while
passing through") and notes the resume ("observing again after N tracked real frames").

**Timing.** The library adds `ui_detect` and `ui_inpaint` (per observe) and `ui_patch`
(per synth) to its profile, and an observe opens a profile that the next estimate
continues. `HarvestProfile()` therefore classifies stamps by name: the observe's two
passes are added to the **estimate's** time (per real frame, like the estimate) and
`ui_patch` to each **synth's**, so the cost guard (and the debug log's FG time) include
them; the UI share alone is published as `RenderStatus::lastUiMs` (one observe plus one
patch) (the Status line no longer shows either figure). Idle gaps are the hazard: a delta whose previous stamp was recorded in an
earlier command buffer contains up to a frame of nothing, so it is dropped (a `ui_detect`
that does not directly follow the profile's start stamp; `luma` behind an observe unless the
estimate was recorded in the observe's own command buffer; the first stamp of a synth that
does not directly follow the estimate) -- an under-count of 0.01-0.05 ms in place of a
16 ms over-count. The host reads the previous profile **before** recording an observe (it
resets the query pool), never in the same composite after it (that would wait on a
submission that does not exist yet).

**Cost** (library figures, 7900 XTX, 1440p, moving scene, marginal over Off): Crosshair
+0.03 ms, Whole screen +0.05 ms; the worst case (Whole screen, 70% still) about 0.25 ms in
total. **Memory**: three clean copies of the frame (44 MB at 1440p) in both modes, about
15 MB more for Whole screen. The memory stays with the library until the interpolator is
destroyed or resized (switching the row Off stops the work, not the allocation).

**BGRA validation warning (known, upstream).** The ring and every output are
`B8G8R8A8_UNORM`; the library's `ui_detect.comp`, `ui_inpaint.comp` and `ui_patch.comp` declare
their storage images as `rgba8` on those B8G8R8A8 views (emitted only while UI protection
is on), which the validation layer reports as
`Undefined-Value-StorageImage-FormatMismatch-ImageView`. It works on RADV (gamescope itself
does the same on BGRA views); it is not fixed here because the library is not edited from
this repository. Expect that line in a validation-layer run.

**Limits**: UI that is see-through (its pixels change with the background, so it is never
still), UI that changes (hit markers, an animated ammo counter, a spreading crosshair),
off-centre reticles in Crosshair mode. A still camera masks the background too, which is
harmless (restored with identical pixels) and leaves the mask the moment anything moves.
With Whole screen on, **Static HUD protection can usually be lowered**: the solid HUD no
longer needs the bias. **Off** for racing games or anything without a fixed HUD (the
detector would spend its time on nothing, or on a still dashboard it should not freeze).
The fork's own Crosshair (`system.crosshair`) is drawn after FG and unaffected either way.

### Crosshair V2 and the box size

**Crosshair V2** (`ui_protection = "crosshair_v2"`, library PR #14, `UiProtection::CrosshairV2`)
is a second crosshair mode next to the original, which stays selectable as **Crosshair** so the
two can be compared in the GUI. The user, on why both: *"Then do a V2 for now (in the GUI, so i
can compare them). The V1 (current) does result in artifacting."* V1 hides the crosshair by
inpainting it and pastes it back over a clean copy, which on a moving background leaves white
specks and moving patterns inside the box. V2 has **no inpaint and no clean copy**: a pixel is
protected only if it stays still **and** stands out from what is behind it
(`uiContrastThreshold` 20), it leaves the mask only after `uiLeaveFrames` (4) consecutive changed
frames and re-enters after `uiReentryFrames` (2), and a protected pixel is pasted back only if it did
not change in the pair being synthesised -- it never pastes over moving content. Outside the pasted
pixels its output is bit-identical to Off. `uiEstimateFill` (hide the UI from the motion estimate)
stays **false**: it measured as a wash. The host does nothing different for it: `recordObserve` /
`recordTrack` are called exactly as for V1 (V2's track is a full detect pass, 0.02 ms). Switching
between V2 and another mode forgets the observed frames, so one pair goes unprotected; harmless.
`Why` the numbers (library README, footage of 12 segments / 1173 frames): artifact pixels per frame
(worse than Off by more than 24 levels) **V2 0.012**, V1 4.44 at a 27 px box and **20.9 at the old
249 px box**; white specks 0.009 against 1.36; mask flips per frame 2.0 against 16.7; recall of the
crosshair after 8 frames 0.977 against 0.917; V2's observe costs 0.022 ms at 1440p against V1's
0.066 ms. Limits: opaque, static crosshairs only -- a semi-transparent one or one that changes shape
stays unprotected while it changes, by design; a *thick* crosshair on a hard-edged moving background
interpolates better around it with V1 (which inpaints it out of the estimate). Whole screen stays
V1-style (no V2 variant exists yet). The enum value order is the library's (Off 0, Crosshair 1,
Whole screen 2, CrosshairV2 3) and `fghost::UiProt` matches it, so `PanelFrameGen.cpp`'s `kUiKeys`
is indexed by it; the GUI's *display* order (Off / Crosshair / Crosshair V2 / Whole screen) is only
the order of `kUiOptions`. Pinned: `tests/test_framegen_format.cpp` asserts the values.

**Crosshair box size** (`framegen.ui_box_height`, a float **percent of the game's height**,
default 2.5, 0.5-10, step 0.1; row text `2.5% · 24 px`, the pixel figure being the library's own
`Interpolator::boxSize()` for the game's frame). The side is `clamp(round(H * pct), 8, min(W, H))`:
24 px at 960 rows, 27 at 1080, 36 at 1440, 54 at 2160. It applies to Crosshair, Crosshair V2 and the
box part of Whole screen. `Why` a percentage of the **height**, not of the area: the old default
was 3% of the frame's *area*, which is 192 px on the user's 1280x960 CS2 (249 px at 1080p, 332 at
1440p) -- far more than a crosshair, and everything in the box beyond it is background that gets
pasted from the real frame, which is where V1's specks came from (20.9 artifact pixels per frame
against 4.44 at 27 px). A height fraction is also what a human can judge ("a bit bigger than the
crosshair") and keeps the box square and independent of the aspect ratio. The smaller default also
removes most of V1's specks on its own. `Why` a slider and not a fixed number: crosshairs differ
(a dot, a thin cross, a big ring), and the preview below lets the user size it exactly. Per profile, like the other rows; the host takes it in **tenths of a percent** in its
own atomic word (`g_uUiBox`; the main config word is full), and a change is a runtime `setSettings`,
no wait. A game whose frame size changes needs nothing: the library recomputes the box from the
fraction.

**The doubled crosshair (library PR #20, 2026-10-06).** V2 left the *static* crosshair in the
frames the colour warp samples, so any warp tap that landed on it copied it to
`crosshair +/- motion*t`: a second, fainter crosshair offset along the camera pan. The patch
only pastes the real crosshair at its true spot and could not hide that copy. The fix
(`gpu/shaders/ui_reject.glsl`, included by the existing shaders; no host API change, no new
shader file, `src/meson.build` untouched): a warp tap whose bilinear footprint touches the
frame's own V2 mask uses the other frame's sample instead. CS2-like test scenes: ghost pixels
23-28 -> 0-1, crosshair still exact, cost <= ~0.01 ms per pair; `uiEstimateFill` stays off.
`Why:` the user's report: *"V2 has major artifacting ... (doubled crosshair)"*. PR #14's tests
compared V2 against Off, which has the very same ghosts, so they could not see it; the new
tests count ghost pixels directly.

**The box margin (library PR #18, 2026-10-06).** The box is where UI is **detected**; what it
protects reaches past it. The library grows the box by `Interpolator::boxMargin()` -- **2 px for
Crosshair (V1), 1 px for Crosshair V2, 0 for Off / Whole screen** -- clamped to the frame, into a
*work rectangle* (`Interpolator::workRect()`; the box itself is `boxRect()`). The mask dilation, the
inpaint source and the paste-back use the work rectangle, so a box that just fits the crosshair
also protects its anti-aliased rim; detection still stays inside the box. The host asks the
library for both rectangles with the live mode (`BoxRects()` in `FrameGenHost.cpp`), so the
preview cannot disagree with it. `Why:` the user set the box to 1.1% (11 px at 960 high) because
the help text said *"Make it just big enough for the whole crosshair"* and *"Small is better"*;
their CS2 crosshair (about 8x7 game px) then smeared under frame generation: *"my crosshair isn't
protected"*, and *"with motion blur enabled, it also started smearing the crosshair even more"*.
The rim fell outside the box. Library tests: 9 px boxes went from 3 / 1 / 6 crosshair pixels wrong
to 0, a 34 px box on a 35 px test crosshair from 2-10 wrong to 0. The help text no longer says
everything inside the box is pasted (false for V1, which pastes only still pixels plus a rim, and
for V2, which pastes only confirmed unchanged pixels): it says the box is where the crosshair is
looked for, should cover its solid pixels, the soft rim may stick out, and extra room is harmless.

### The box preview

The two UI-protection rows (`framegen.ui_protection`, `framegen.ui_box_height`) declare
`.Preview( Entry::PreviewKind::UiBox )`, so while either is the selected row the Shell's
**Inspector** shows, above VALUES, the game's pixels under the box: a crop of the frame's centre
(the box plus its own size again around it, at least 48 px), enlarged by a whole number with
**nearest** replication so each game pixel is a crisp block, everything outside the **protected area** (the work rectangle, see the
box margin above) dimmed, the box outlined (a dark halo under an accent line) and the work
rectangle outlined fainter (a thin translucent accent line, drawn only when the margin is at
least a fraction of a screen pixel, so not for Off / Whole screen), and `Box 24 × 24 px · protected 28 × 28 px` /
`Game frame 1280 × 960` under it (just `Box 24 × 24 px` when there is no margin; `· picture
reduced` joins the second line when the crop is reduced). The crop is at least the work rectangle
plus 16 px, so the margin is always inside the picture. The user's request: *"a slider, so the user can adjust it for himself, and then in the
right inspector rail we can show an image of the area, basically, that's underneath the current UI,
so he can see if the whole crosshair fits in, and then the user can configure it, like, really
precise."* It follows the **selected** row like the Adaptive Brightness strip does (a click on the row
selects it); there is no hover-only preview.

**How the picture gets its pixels (no GPU wait, no new threads):**

- `fghost::BoxPreviewWanted()` is called by `UiBoxPreview_Draw()` every frame the block is on screen;
  it lapses by itself 400 ms after the last call, so closing the Inspector, selecting another row or
  closing the overlay stops all capture work with no hook. `Why` a lapse and not a visibility flag:
  the block has more ways to leave the screen than the overlay has of closing (the same argument
  `EffectPreview.cpp` makes for its strip).
- While wanted, the renderer records the crop **into the command buffer it already submits for a new
  real frame** (`BoxPreviewRecord()` in `FrameGenHost.cpp`, from the ring-copy branch and from
  `TrackWhileInert()` for the pass-through case): one `cs_fg_crop.comp` dispatch (at most 128 x 128
  threads) on **layer 0 itself**, i.e. the real game frame before any effect, the same pixels the
  library protects, then a copy into a host-mappable 128 x 128 staging image. At most 10 captures a
  second and one in flight. Cost: a dispatch of a few thousand threads plus a 64 KB copy per capture,
  only while the preview is on screen.
- The pixels are read back on a **later composite**, once `g_device.completedSeqNo()` shows the
  submission has retired (`BoxPreviewPoll()`), never by waiting. This differs from the Adaptive
  Brightness strip (`vulkan_effects_preview_*`), which arms one composite and waits once; that
  mechanism lives inside the effects pre-pass, which does not run when no native effect is on, so it
  could not serve a row of this area. `cs_fg_crop.comp` is new (`SHADER_TYPE_FG_CROP`, precompiled).
- **Format handling:** the shader fetches the game's raw UNORM view like every layer-0 pass. 8-bit and
  10-bit values are already display-encoded and are only clamped; an fp16 **scRGB** (linear) game is
  sRGB-encoded for display, and anything above 1.0 clips; HDR10 (PQ) is shown as encoded, so it looks
  flat. A crop wider than 128 px (a big box on a high-resolution game) is reduced by a whole factor with
  a box mean, so it never aliases; the caption then says *(picture reduced)*. YCbCr / unsupported
  formats are refused by the renderer as they are for generation, so there is no picture.
- The overlay side (`src/Overlay/UiBoxPreview.{h,cpp}`) owns one fixed 192 x 192 `ImTextureData`
  registered with ImGui's context (`ImGui::RegisterUserTexture`, the same user-texture path
  `EffectPreview.cpp` uses, uploaded by the existing `ImGui_ImplVulkan` backend), enlarges the capture
  by the largest whole factor that fits and uploads only that sub-rectangle.
- **Placeholders:** with Frame generation, Motion blur and the Lag spike buffer all off the renderer is
  not running (the rows are disabled then anyway): *"Turn Frame generation on to see what is under
  the box."* With the renderer on but no capture in the last two seconds (no game frame, or the game
  is paused): *"Waiting for a game frame."* With UI protection **Off** the picture still shows, and
  the caption says protection is off. The box is the centred one (`uiBoxOffsetX/Y` stay 0).

The Registry got one new generic hook for this: `Entry::ValueText( fn )`, a value-text override for a
row whose number alone says too little (used by the box-size row for `2.5% · 24 px`); it replaces
`FormatDeclValue()`'s "value + unit" in every host that draws the value (sheet row, Inspector row),
so they cannot disagree.

## History

- **Crosshair V2** (terminology entry, moved here 2026-11-07): **Crosshair V2** / **Crosshair box** — Frame generation's UI protection has a **box**: the centred square it watches and pastes back exactly (`framegen.ui_box_height`, the *Crosshair box size* slider, 0.5-10% of the game's *height*, default 2.5%, shown as `2.5% · 24 px`; the Inspector previews the game pixels under it, `src/Overlay/UiBoxPreview.cpp`). **Crosshair** is the original (V1) detector, which inpaints the crosshair out of the estimate; **Crosshair V2** (2026-10-05, the library's `UiProtection::CrosshairV2`) only protects pixels that stay still and stand out, never pastes over moving content and has no fill-in step, so the specks and moving patterns V1 leaves are gone. See `superdoc/features/frame-generation.md`'s "Crosshair V2 and the box size".
