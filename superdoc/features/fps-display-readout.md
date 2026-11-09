# FPS display: readout behaviour

FPS shown, update modes, Hide-if-above hysteresis, the removed backdrop, outline, lag-spike detection, pinned digit width and warm-up. Part of [fps-display](fps-display.md); sections below are moved verbatim from that page.

## FPS shown (2026-10-04)

Labels (2026-11-08): **Game**, **Frame gen**, **Game + FG** (were Game / Output /
Both); stored values unchanged. Why: "Output" was unclear to the user. Sections
below still use the old names in the user's quotes.

`fps_display.fps_shown` (row `hud.fps_shown`, a Choice: **Game** / **Frame gen** /
**Game + FG**, stored as `"game"` / `"output"` / `"both"`, default **Game**, per
profile like its siblings, additive — schema stays 5). Same Show-HUD-only gate
as its siblings, never gated on frame generation being on. It replaced the
morning's `count_generated_frames` switch. The user, first: *"for the HUD, make
sure that there's an option to count fake frames as real frames, and then it
should just, in the bottom right, say the new number ... But still only a single
number, just what's actually being outputted."* Then: *"For the HUD, add toggle,
to show the original and the framegen FPS. <OriginalFPS>-><FrameGenFPS>
40->280"* (the `->` was later shortened to a single `>`, 2026-10-04: `40 > 280`)

- **Game**: the game's real frame rate (the commit counter); exactly the HUD as
  it was before this option.
- **Frame gen** (stored `"output"`): what `count_generated_frames=true` did. The pacer's `presentedFps`
  while Frame generation is *generating* (the latched signal below), the game's own
  rate otherwise: a 340 fps game passed through on a 280 Hz display reads 340, not
  about 280.
- **Game + FG** (stored `"both"`): `<game> > <output>` (e.g. `120 > 280`) while Frame generation is
  generating, `<game> - <output>` (e.g. `340 - 340`) while the Frame generation
  **switch** is on but it passes the game through. With the switch **off** it is
  the lone game number, exactly like Game mode, even when Motion blur or the Lag
  spike buffer drive the pacer. `fpsmath::PlanShown( switchOn, latchedGenerating )`
  decides both the pair and the output number's source. The output number in a
  pair is never a second measurement of the commit counter (that would read
  `120 - 119` from a different window phase): without a generating latch it *is*
  the game number.

`Why` (switch off, the user's review of the first Both): *"When the actual switch
is off, it should only show the single number."* The `120 - 120` pair this
replaces was the previous rule (a pair whenever the pacer was not driven); with
the switch off there is nothing to compare, so the lone number is right, and the
box is pinned to the game-only string so it does not keep a wide empty half.

`Why` (output number): *"It should then still show, what is actually being
rendered (games 340 in this case)"*. The first Both took `presentedFps` whenever
the pacer was driven, which in pass-through is about min(game, refresh), so it
under-reported a game faster than the display. Now `presentedFps` is the output
number only while generating.

`Why:` the user, on the HUD flapping in Both with Pause at refresh rate off:
*"it sometimes shows the generated frames and sometimes it just switches off for
no apparent reason ... it's really annoying that it switches back and forth. And
instead of, if it's not generating, instead of hiding one of the values, just
switch there, like bigger than symbol, the arrow, to just a simple dash so it
doesn't move around as much in the bottom right of the corner of the screen."*

**What "generating" means** (`UpdateAndGetDisplayFps()`, pure parts in
`fpsmath`: `GeneratingSignal`, `GeneratingLatch`, `BothSeparator`):

1. **The library's signal**, `PacingStatus::generating` (frame-gen-ritz
   `Pacer::Report::generating`, PR #15, simplified by PR #16): the pacer is in the
   Generate state AND a generated frame (an output with `t < 1` made from a pair)
   was presented within the last 1 s. **There is no multiplier gate any more**: PR
   #15 also required outputs per real frame >= 1.2 (>= 1.1 to stay on), so a
   steady 1.15x showed `-`; PR #16 removed those constants and `steadyMultiplier`
   is informational only. The user: *"If its generating any frames, it should show
   the '>'. Otherwise that differentiation is useless."* The host-side latch below
   is unchanged.
   It replaced `activeN >= 2`: `activeN` is the pacer's *instantaneous* plan,
   clamped to at least 2, so an effective 0.89x pass counted as generating.
2. **Staleness**: the status is stamped `publishedNs` by `PublishPacingStatus()`
   and one older than 1 s (`kPacingStaleNs`) counts as not generating. `Why:` it
   is republished only about every 250 ms while the pacer is driven, and not at
   all while paints are skipped or the pacer is idle, so the last value can
   outlive the truth.
3. **A host-side latch**, `s_GeneratingLatch`: `>` comes back immediately, but
   the separator changes to `-` only after the signal has been false for 1.5 s
   (`kGeneratingLatchNs`). A safety net on top of (1): even a signal that does
   dip never flickers the separator faster than that.

**The output number follows the separator's latch.** The source is
`s_GeneratingLatch` (the same state that draws `>`) AND the switch being on: latched
generating means `presentedFps` (the output frames actually handed to present),
anything else means the commit counter. `Why:` number and separator change
together, so the HUD does not show `>` with the game's own rate or `-` with a
generated one. A latch still lingering after the switch went off does not count.

**Three windows, none ever restarted by a flip.** `UpdateAndGetDisplayFps()` runs
`s_GameWindows` (commit counter) and `s_PacedWindows` (the pacer's `presentedFps`)
every call, each with its own Smoothing/Immediate state, and picks one for the
output number. The status is published about every 250 ms and is already a rate,
so the paced windows average the per-paint samples instead of differencing a
counter; with no fresh status (stale, pacer not driven) they are fed the game's
own rate instead, so they are always warm. `Why:` `RateWindows::Step` restarts a
window when its source flips, which would show 0 for a moment on the very frame the
separator changes; keeping both running and only picking one means a flip is a
switch between two ready numbers. It also means the game number never jumps or
restarts when generation starts or stops, and changing "FPS shown" never shows a
stale value.

**Layout.** `MeasureFpsModule( nFps, nOut, bGenerating, ... )` (`nOut < 0` = a lone
number, which is Game, Output, and Both with the switch off; its pinned field is the
game-only zero run) builds the string
`"%d > %d"` or `"%d - %d"` (`fpsmath::BothSeparator`; plain ASCII — the overlay
font atlas is Latin-1 only, an arrow glyph would be a missing glyph). The
pinned box is `<game zeros> X <output zeros>`, each side padded to its own >= 3
cells like a lone number, so the box only widens when a side gains a digit. X is
whichever of `>` / `-` measures **wider** in the font, in both states, so a flip
of the separator never resizes or moves the box (the text is flush to the
anchored side, so at the bottom-right the output number stays put and only the
game number shifts by the glyph width difference). The box is placed by the same `ResolveAnchoredOrigin()` and
the digits hug the anchored side via `flTextOffsetX` as before, so at a right
anchor the string grows leftwards from the margin and cannot clip. The margin
and ink-bearing correction is measured off the game number's zero run in every
mode (`szInkRef`), not off the `>`, so Game mode is byte-identical and the `>`
glyph cannot shift the vertical correction. The outline and Inverted modes
need no change: the outline is drawn from the same string, and Inverted samples
one pixel at the box centre (now the middle of the wider string). The digits are
monospaced, so their widths are stable.

**Unchanged:** lag-spike detection and the frametime history keep running on
real frames. Hide above X compares against the game number in Both (and in
Game), and against the output number in Output. A forced reading
(`fps_display_force`) always draws the single number.

**Legacy key.** A profile with `count_generated_frames: true` and no
`fps_shown` loads as `"output"`; the old key is read for that, never written
again (`fps_shown` is written instead), the same stop-writing precedent as other
superseded HUD keys.

## Update modes

Two ways to turn the commit count into the number on screen. Both are kept
live in `UpdateAndGetDisplayFps()` regardless of which is selected (the
bookkeeping is two timestamps and two counts), so switching modes never
shows a stale value:

- **Smoothing** (default) — the user's own spec, verbatim: every
  **1000 ms** the window rolls over and `target = Δcount / Δt` is taken;
  the shown value then **glides from what was on screen to `target` over
  300 ms** with `smoothstep`, and **holds for the remaining 700 ms**.
  `DrawReadout()`'s `lround()` walks the digits through every intermediate
  integer during the move. State: `s_flShownFrom`, `s_flShownTo`,
  `s_ulGlideStartNs`; `fpsmath::GlideValue()` / `GlideMoving()` are the
  pure functions. Until the first window completes it shows the Immediate
  value, so the first second is a real reading rather than a made-up 60.
- **Immediate** — the count over the last **100 ms**, republished each time
  that window rolls over. Jittery by design; reacts to a rate change within
  ~100 ms.

**"Update every second" is gone** (2026-09-05): Smoothing now *is* a
one-second windowed rate, with the glide on top, so the old mode had
nothing left to offer. A stored `"per_second"` (or any unrecognised value)
loads as Smoothing — `fpsmath::UpdateModeToInt()`'s fallback rule, the same
legacy-value idiom `ParsePlacement()` uses — so existing configs are not
rewritten and do not break. Covered by `test_config.cpp`'s "legacy
per_second loads as Smoothing".

### Repaint cadence for the glide

A 300 ms movement needs frames. A game at ≥ 60 fps already paints every
vblank, so nothing extra happens in the normal case. For an **idle**
client (paused, alt-tabbed) the HUD's repaint-timer thread
(`EnsureRepaintTimerThread()`) is what drives paints, and since 2026-09-05
it is **adaptive**: it wakes every ~16 ms but only calls `force_repaint()`
every 500 ms — *unless* `s_bGliding` is set, in which case every wake
requests a paint, so the glide gets ~18 frames instead of one.
`s_bGliding` is set/cleared by `UpdateAndGetDisplayFps()` itself (true
while `GlideMoving()` and Smoothing is the selected mode), i.e. by the
paints the thread provokes, and the paint that sees the glide reach its
target clears it.

> **Why off-thread, and why not the lag-spike hold's mechanism:** the
> lesson recorded at the top of `FpsDisplay.cpp` applies — a gate
> evaluated only inside `paint_all()` cannot manufacture repaints on a
> clock nothing is driving. And the lag-spike hold does *not* carry its own
> 700 ms either (an earlier assumption): it relies on the game still
> committing. So the glide's clock lives in the timer thread. A 16 ms wake
> that does an atomic load is cheap; the thing rationed is
> `force_repaint()`, which costs a composite. With an idle client the
> 1-second window itself is only checked at each paint, so the hold can
> run up to 500 ms long there — accepted; the glide itself is still smooth.

## Hide if FPS above X — with hysteresis

Modeled as a `Switch` (`hud.hide_above`) with a threshold `Param`
(`hud.hide_above.fps`) — the same `.Param()` idiom `hud.anchor` already
uses for its margins.

A plain `hidden = fps > X` flips every frame the reading sits on either
side of X, which at a stable frame rate is real visible flicker (ordinary
float jitter crosses an exact threshold constantly). `DrawReadout()`
instead runs a one-sided Schmitt trigger:

- While **shown**, only hides once fps climbs **5 fps past** X.
- While **hidden**, only reappears once fps drops back **to or below** X
  itself.

So "hide above X" still means what it says (X is the line that matters
going from shown to hidden), while the reverse crossing needs 5 fps of
margin — enough that ordinary frame-to-frame variance right at the line
can't retrigger it. The 5 fps band is a fixed constant in
`FpsDisplay.cpp`, not a setting — the user asked for hysteresis, not a
second number to tune.

## Backdrop: removed (2026-09-09)

The HUD used to draw a plain unrounded rectangle behind the number, sized
to the text plus a fixed 6 px padding, at a configurable opacity where 0
meant "no backdrop". The user asked for it to go, so it is gone outright —
the drawing (`DrawModuleBackdrop()`), the "Backdrop opacity" row, and both
schema fields (`backdrop_opacity`, `backdrop_padding`). What opacity 0
already did is now the only behaviour: the HUD draws the digits and their
outline and nothing else.

**A saved backdrop setting no longer does anything.** A profile written by
an older build still carries `"backdrop_opacity"` and `"backdrop_padding"`;
neither is read any more, the file loads unchanged, and both keys disappear
the first time anything writes that profile — the same "just stop reading
an old key" precedent `friends_lookup_names`, `backdrop_enabled`,
`backdrop_rounding` and `shadow_strength` set. Pinned by `test_config.cpp`'s
"a config carrying the removed backdrop keys loads and drops them", which
asserts both halves: the live keys in that same file still load, and the
removed ones are absent after the next write.

> **Why `backdrop_padding` went too, rather than staying at 6 px.** With
> nothing drawn behind the text it had no job left — it existed to give
> the rectangle breathing room around the digits. It was also a pure no-op
> on placement, provably: the readout's box grew by `2 × padding` and the
> draw origin moved in by `padding`, so on a centred axis the two cancel
> (`(display − content − 2p)/2 + p == (display − content)/2`) and on a
> hugging axis `EdgeShift()` cancelled it again by construction. Removing
> it moved no pixel — the margin matrix below measures identically with and
> without it.

> **What the removal cost.** Two things, both accepted. Inverted mode has
> **no lag-spike indication at all** now: it cannot invert an
> already-inverted digit against itself, so its spike reaction was a red
> tint *on the backdrop* — see the Inverted section below. And the number
> has nothing but its outline to separate it from bright, busy content;
> "Outline size" is the control that job belongs to now.

## Outline

`outline_strength` (row "Outline size", 0–4 **pixels**, step 0.25,
default 0 = off). Its name kept the `_strength` suffix from the few hours
it spent as a 0–1 opacity on 2026-09-03; a config written in that window
still loads, and its 0–1 value is simply read as a thin (sub-pixel to
1px) outline. It replaced the earlier drop shadow the same day at the
user's request, and the old config key `shadow_strength` is no longer
read at all (an old file falls back to the default).

The geometry: the digits stamped again in solid black on **concentric
rings** around the real text position, the fill then drawn on top. Ring
spacing is capped at 1px (`ceil(radius)` rings), and each ring's stamps
are spaced at most ~0.75px apart along it (`ceil(2πr / 0.75)`, clamped to
8–48 per ring). A 1px outline is therefore a single 8-stamp ring — as
cheap and as crisp as the four-offset version it replaced — while a 4px
outline is four rings of up to ~34 stamps, ~90 text draws of at most
three glyphs.

> **Why rings and not four axis-aligned offsets:** four offsets only look
> solid while the radius is about a pixel. Past that the diagonals open
> up and the outline reads as a cross, not a stroke. The 1px ring spacing
> and the ~0.75px stamp spacing are what keep a 4px outline continuous.
>
> **Why a size and not an opacity:** the user asked for a *size* ("the
> max outline size should be 4.0"). It is also the only version that can
> work — stacking translucent offset copies saturates the alpha byte long
> before the ring closes, and an outline that fades as it grows reads as
> blur, which is exactly what the drop shadow was rejected for.
>
> **Why 4px is the ceiling:** the user asked for exactly that ("the max
> outline size should be 4.0"). It used to be justified by
> `backdrop_padding`'s 6px — the outline stayed inside the backdrop box at
> any setting — but with the backdrop gone (2026-09-09) there is no box to
> stay inside, and the range needs no such justification: `EdgeShift()`
> takes the outline's own geometric reach, so a thicker outline pulls the
> digits *in* rather than pushing past the margin. Verified at outline 2 at
> every corner in the margin matrix below.

> **Why each stamp's offset is rounded to a whole pixel (2026-09-04
> fix):** the stamp angles are a full, evenly-spaced sweep, so the *ideal*
> (unrounded) stamp cloud is provably centred on the text position — but
> Dear ImGui's `ImFont::RenderText()` independently floors every
> `AddText()` call's position to a whole pixel before drawing it
> (`imgui_draw.cpp`'s "Align to be pixel perfect"), and flooring does not
> distribute over a fractional offset: `floor(textPos + r)` is not always
> `floor(textPos) + r`. That let individual stamps floor to a different
> pixel than the fill did, reading as the outline sitting up-left of the
> digits — worse at a bigger radius (more, farther-flung stamps) and a
> bigger font (the same misalignment is a larger fraction of a thinner
> stroke). Rounding the stamp's *offset* from the text position to a
> whole pixel first makes its own floor an exact no-op relative to the
> fill's, for any text position, font size or radius.

> **Sub-pixel radius (below 1px) is alpha, not smaller geometry
> (2026-09-04 fix, part 2):** the whole-pixel rounding above fixes the
> lean, but it also means every stamp on a below-1px ring rounds to
> `(0, 0)` — right on top of the fill — so radii below 0.5 drew no
> outline at all, a regression from the pre-rounding version's faint
> sub-pixel outline. There is no such thing as a smaller-than-1px stroke
> on a whole-pixel grid, so a sub-pixel radius is rendered as what it
> physically is: a faint 1px outline. Below radius 1, the code stamps the
> same whole-pixel radius-1 ring the solid path uses at
> `outline_strength` 1, and scales that ring's **alpha** by the radius
> instead of shrinking its geometry — alpha 64 at 0.25, 128 at 0.5, 191
> at 0.75. At radius 1.0 the alpha is exactly 255, bit-for-bit the solid
> path's own colour, so the transition across 1px is continuous; radii at
> or above 1 take the untouched solid-black geometry unchanged. Safe
> under Inverted mode's marker (`alphamode.h`'s `alpha_mode_invert`) for
> the same reason plain black already was: black at any alpha has
> `R == G == 0`, so it is never read as digit coverage and never un-marks
> a digit edge drawn over it — no separate handling needed for that mode.

## Lag-spike detection

Switchable (`lag_detection_enabled`, row "Lag spike detection", default
**on** so an existing config keeps today's behaviour). With it **off**
there is no spike reaction of any kind: Fixed mode never flips the
number's colour — which since 2026-09-09 is the whole of the reaction, the
Inverted-mode backdrop tint having gone with the backdrop. Nothing else in
the tab depends on it, so no row greys out when it is off.

> **Why the detector keeps running while the switch is off:** the
> frametime history is a handful of floats per frame, later work wants it
> anyway, and keeping it warm means switching detection back on reacts to
> the very next spike instead of waiting for a window of samples to
> refill.

The per-frame frametime ring buffer (`s_flFrametimeHistoryMs`, 240
samples, kept since Phase 1 unused until now) feeds a fixed heuristic —
**no user-facing threshold**, deliberately: the user asked for the
feature, not a second slider to tune it with.

A frame counts as a spike (`ComputeIsSpike()`) when it is **both**:

- at least **1.75x** the median of the last 30 prior samples
  (`kSpikeFactor`), and
- at least **4ms** slower in absolute terms (`kSpikeMinDeltaMs`).

Relative alone would trip constantly on a very fast, very stable game (a
240fps game's own frame-to-frame jitter is easily +75% of its ~4ms
median without anything being wrong); absolute alone would never trip on
a game already running slow, where every frame is "big" in milliseconds
but nothing has changed. The **median** (not the mean) is the baseline so
one prior spike doesn't drag it down enough to mask the next.

Detected state holds visible for **700ms** (`kSpikeHoldNs`) after the
triggering frame — a single bad frame lasts a fraction of a millisecond
on screen otherwise, which isn't "perceptible", it's a flicker the eye
filters out.

## No layout jitter: pinned digit width — and no 999 ceiling

The box is sized off a run of `'0'` cells **as long as the current digit
count, never fewer than 3** (`fpsmath::PinnedDigitCount()`:
`max(3, digits(fps))`, capped at 7), measured in Geist Mono, which is
genuinely monospaced — so `'0'` measures the same as any digit and a
fixed cell count is tabular by construction. 0–999 share one 3-cell box
that never resizes as the number goes from 1 to 2 to 3 digits; a
four-digit rate widens it to 4 cells, a five-digit one to 5.

**The `std::clamp(nFps, 0, 999)` is gone (2026-09-05).** It turned the
mis-sampled rate the counting rewrite fixed into a plausible-looking fake
(the "999 FPS in Rust" report), and it turned a genuine 1200 fps — an
uncapped mailbox-mode client on a 60 Hz nested output really does this —
into 999 too. Only the floor at 0 remains.

> **Why the pin follows the number rather than fixing 4 or 5 cells:** a
> fixed 5-digit pin would make the normal 2–3-digit readout sit in a box
> twice too wide. **Why the floor is 3 and not the exact count:** a
> 2-digit reading would otherwise shrink the box every time the game
> dipped below 100, which is the jitter the pin exists to prevent. The
> growth to 4+ cells is therefore the one deliberate resize, and it only
> happens for a reading that needs it.

What's actually *drawn* is the plain unpadded digits (`"%d"`), not the
padded string — drawing a padded string put the leading blank glyph's
advance inside the text draw, which left a visible empty gutter on the
left of a two-digit number and shoved the digits against the box's right
edge (fixed 2026-09-03). `MeasureFpsModule()` measures both the pinned
field and the plain digits and derives `flTextOffsetX`, the width
difference (or half of it for a centre anchor), added to the text origin
so the digits sit at the side of the pinned-width box that faces the
anchor. The outline and the digits themselves both draw at that same
offset origin, so they track together.

**Digits hug the anchor's side, not always the box's centre (2026-09-06
fix).** `flTextOffsetX` used to always be half the pinned-vs-unpadded
width gap, i.e. always centred, regardless of anchor — so with a
right-hand anchor a reading like `60` (2 digits in the pinned 3-cell box)
sat centred rather than flush with the screen edge, and visibly drifted
sideways as the digit count changed (e.g. `60` → `144`). `MeasureFpsModule()`
now takes the anchor's horizontal side (`ParsePlacement()`'s `nHoriz`) and
places the digits flush left, flush right, or centred to match — left and
right anchors flush, centre anchor unchanged. The box itself (size and
`ResolveAnchoredOrigin()`'s placement of it) is untouched by this; only
where the digits sit inside it changed. `fps_display_force "<n>"` (a debug
ConCommand, `-1` or no argument releases it) forces the displayed reading
to an exact integer, bypassing smoothing, for pixel-measuring this without
waiting on a real frame rate — see the untracked `hud-align` capture run
for the measured before/after edge positions.

## Warm-up: `FpsDisplay_WarmUp()`

ImGui 1.92 bakes glyphs lazily, per (font, size), and
`ImGui_ImplVulkan_UpdateTexture()` then does a blocking `vkQueueWaitIdle()`
on the first frame that shows a never-before-drawn glyph — the same first-
frame hitch the Shell's startup warm-up (`SettingsOverlay.cpp`, issue #30)
and `Notifications::WarmUp()` exist to pre-pay. `FpsDisplay_WarmUp()`
(2026-09-05) is the HUD's version, meant to be called from the Shell's
startup warm-up block. Its steps, exactly:

1. `EnsureConfigLoaded()`; return if the HUD is **off** (an off HUD
   allocates nothing — this file's standing guarantee; enabling it later
   pays the one-time cost on that first frame, as before) or the output
   size is not yet known (try again next call).
2. `EnsureImguiInit()` → `EnsureTexture(output w, h)`, with the same
   ImGui-context save/restore `FpsDisplay_AddLayer()` uses.
3. One hidden frame: `NewFrame`, `AddText("0123456789")` in the **Hero**
   face at the configured `font_size` — the readout draws digits and
   nothing else, and the fill and every outline stamp use that same face
   and size; the crosshair draws rects only and has no glyphs to warm.
   Opaque colour on purpose: `AddText()` early-outs on alpha 0 and would
   bake nothing.
4. `ImGui::Render()` + `RenderAndSubmit()` — the atlas upload, and its
   `vkQueueWaitIdle`, happen here.
5. **Stop. No `layers.push()`.** Nothing reaches the screen; the next real
   frame's `LOAD_OP_CLEAR` wipes the texture. This is the whole contract: a
   pushed layer would composite the warm-up frame, and a second layer on
   the stack every frame forces the full composite path for every frame
   after.

One-shot once init has been attempted. A later font-size change bakes the
new size on its first frame; that one hitch (on a slider release, not at
game start) is accepted.
