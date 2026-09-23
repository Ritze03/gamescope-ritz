# Zoom

A magnified copy of the middle of the game, drawn while a key or mouse button
is held (or toggled), cut to a circle, rectangle or square, with a 1 px black
outline. `src/Overlay/Zoom.{h,cpp}` (config, settings area, the active flag),
`src/shaders/cs_zoom.comp` + the zoom block in `vulkan_composite()`
(`src/rendervulkan.cpp`; the picture), `keybinds::Action::Zoom`
(`src/Keybinds.cpp`; the chord). Config `config::ZoomSettings`
(`src/Config/ConfigSchema.h`, JSON key `zoom`, a normal per-profile section
like `crosshair`), settings area `system.zoom` ("Zoom", in the rail's MISC
group after Crosshair). Default **off**.

> **Why (2026-09-14):** the user's request, near verbatim: *"Something like a
> zoom bind. So I can set it up to either add a zoom when I hold my right mouse
> button or any other key. So it should be like our configurable keybinds. And
> then I should be able to choose how big the projection is, the shape, so a
> circle, rectangle, or square. For the sizing, you can just use percentages,
> but hide them behind 0 to 1 values, floats. And make me able to select
> whether I want it on holding the key or pressing the key, like toggle or
> hold. ... it should have some kind of outline. For now a basic black single
> pixel outline would be fine. And it shouldn't be affected by the scaling, so
> it should draw as an overlay ... after the game, the shaders, and so on."*
> Later the same day: a zoom-level slider from 1.5 to 5.0, and an option that
> divides the mouse speed by that level using gamescope's own
> `--mouse-sensitivity` path.

## The settings

| Group | Row | Config field | Notes |
| --- | --- | --- | --- |
| Zoom | Enable zoom | `enabled` | Master switch, default off. The chord does nothing while this is off. |
| | Zoom key | — | The `zoom` action's chord, `RMB` by default, as a capture chip (row id `zoom.bind`). Editable here since 2026-09-22; the same chord as Keybinds' **In-game hotkeys** row, so it is stored in `global.json` and shared by every profile ([keybinds.md](keybinds.md#two-groups-and-a-copy-in-the-features-own-area-2026-09-22)). |
| | Activation | `mode` | `"hold"` (zoomed while the chord is down) or `"toggle"` (press in, press out). Int-backed Choice like `crosshair.hide_mode`: `overlay_e2_set zoom.mode 1` is Toggle. |
| | Zoom level | `factor` | 1.5–5.0, step 0.1, default 2.0. |
| | Match mouse speed | `mouse_scale` | Multiplies relative mouse motion by `1 / factor` while zoomed. See below. |
| | Keep the button from the game | `consume_button` | Default off. Swallows the zoom chord's own mouse button, press and release, instead of forwarding it. See below. |
| | Scroll to change zoom level | `scroll_adjust` | Default off. While zoomed, the wheel steps `factor` by 0.25 instead of reaching the game. See below. |
| | Fade duration | `fade_ms` | 0–2000 ms, step 10, default **200**. `ZeroMeans("Instant")`. One duration for the whole staged reveal — see below. |
| | Sharpen | `sharpen` | 0–1, step 0.05, default **0** (off). Sharpens the projector's own picture only. See "Sharpen" below. |
| Projection | Shape | `shape` | `"circle"`, `"rectangle"`, `"square"`. |
| | Size | `size` | Circle diameter / square side, as a fraction (0.05–1) of the game's on-screen **height**. Greyed for a rectangle. |
| | Width / Height | `width`, `height` | The rectangle, as fractions of the on-screen width and height. Greyed unless the shape is a rectangle. |

Fractions, not pixels, on purpose: the user asked for "percentages hidden
behind 0 to 1 floats", and a fraction of the game's on-screen rect means the
same projection at every resolution. There is no outline row yet — the ring is
1 px black, hard-coded in the composite (`uOutlinePx = 1`); "for now a basic
black single pixel outline would be fine".

## Where the picture is made: inside the composite, as layer 1

The zoom is **not** drawn by paint_all() into an overlay texture the way the HUD
and crosshair are. `Zoom_FillRequest()` (called from `paint_all()` right after
`FpsDisplay_AddLayer()`) only fills `FrameInfo_t::zoom` — active, shape,
fractions, factor — and `vulkan_composite()` builds the layer itself.

> **Why there and nowhere else.** Two of the user's requirements pin the
> place. *"After the game, the shaders"*: the bundled effects (Adaptive
> Brightness, Bloom, …) run as a pre-pass **inside**
> `vulkan_composite()` on a private copy of the frame, and the graded base
> layer exists only there — a magnifier sampling the base in `paint_all()`
> would show the raw game. *"Not affected by the scaling"*: the FSR/NIS/blit
> pass that upscales the game to the output runs after that point, so building
> the projection before it, at output resolution, from the source texels,
> means the zoomed picture is never run through the upscaler — it is a
> bilinear magnification of the graded source, no more.
>
> **Why a layer of its own and not a drawing into the HUD's texture.** The
> HUD's ImGui frame is rendered by the ImGui Vulkan backend into its own
> texture *before* the composite runs, so it cannot see the composite's
> post-effects base. A
> `Layer_t` in the composite's own stack does, and the composite already knows
> how to blend a premultiplied RGBA layer with a colorspace — so the shape mask
> and the outline live in the layer's alpha, and nothing in the composite
> shaders had to learn a new per-layer clip.

The block (rendervulkan.cpp, "THE ZOOM"), after the effects pre-pass and
before the FSR/NIS branches:

1. Skip unless the base is SDR, non-YCbCr, and there is a free layer slot
   (`k_nMaxLayers` is 6; a full frame logs a rate-limited line and draws
   without the zoom rather than without something else).
2. Size the projection in output pixels from the fractions and layer 0's
   on-screen rect (`tex.size / scale`), clamped to `[4, output]`. A circle's
   texture is square by construction (`Zoom_FillRequest()` converts the
   height fraction into a width fraction using the on-screen aspect).
3. `cs_zoom.comp`, one dispatch over the projection: for each pixel, sample
   layer 0 bilinearly at `centre + d * (scale / factor)` source texels —
   i.e. `factor` output pixels per source pixel at the game's own scale —
   mask by a signed distance to the shape, and write premultiplied RGBA:
   picture inside, black for the 1 px ring just inside the edge, transparent
   outside. Encoded in, encoded out, exactly as the effects pass: slot 0 on
   the raw view, written through the UNORM view of an ABGR8888 texture
   (`g_output.zoomOutput`, pooled, re-created only when the size changes).
4. Insert it as **layer 1** of the composite's private copy (every layer above
   shifts up one), centred on layer 0's on-screen rect at whole pixels so the
   composite copies it texel for texel, with layer 0's colorspace so it is
   decoded the same way. Above the game, beneath every overlay `paint_all()`
   pushed — Steam overlay, cursor, HUD and crosshair, toasts, Shell.

`Why it forces a full composite.` `Zoom_FillRequest()` sets
`bNeedsDestinationBlend` while zoomed, the same flag the HUD's Inverted mode
uses for the same reason: DRM's partial-composition shortcut and the nested
backends' direct scanout both take the base layer *out* of the composite, and
the zoom samples the base. The cost — a full compute composite instead of a
plane — is paid only on zoomed frames.

Because the request lives in the caller's `FrameInfo_t` and the composite never
writes that struct back, a `gamescopectl screenshot` re-composite of the same
frame builds the same zoom and is pixel-identical to what was presented.

## The chord: a held action with mouse buttons

The zoom is `keybinds::Action::Zoom` (`zoom`, "Zoom", default `RMB`), which
added two things to the keybind engine ([keybinds.md](keybinds.md) has the
rules in full):

- **Mouse buttons as chord terms** — `LMB`, `RMB`, `MMB`, `Mouse4`, `Mouse5`,
  carried as `XKB_KEY_Pointer_Button1..5` so the held set stays one set.
  `wlserver_dispatch_mouse_button()` feeds a button going *to the game* (and,
  during a rebind capture, one going to the overlay) into the engine with the
  keyboard ledger plus the buttons down; the keyboard path's set carries no
  buttons, so a Right Shift tap still opens the shell while the player is
  aiming. The engine's swallow verdict is ignored for a button: the game gets
  its right-click exactly as before.
- **The held-action rule** (`ActionInfo::bHeld`): the chord fires on the
  press that completes it *as a subset* of the held keys (RMB while W is held
  still zooms — a gameplay key is pressed mid-movement), only on one of its
  own keys, only when not already down (key repeat), and the engine reports
  the release that breaks it (`KeyResult::bReleased`). A modifier-only chord
  is a plain press here, never a tap. Exact chords are checked first, so an
  exact `Ctrl+Shift+C` on another action wins over a zoom on `C`.

`Zoom_OnChord(true/false)` then applies hold or toggle: hold follows the
chord; toggle flips on the press and ignores the release. A focus boundary
(`wlserver_clear_pressed_hotkeys()`) releases a held zoom, because the release
that would have ended it went somewhere else.

**A held chord is broken only by the release of one of its own keys** — see
[keybinds.md](keybinds.md)'s "Held actions and mouse buttons" for the rule and
its `Why:`. `Why (2026-09-14):` the user's report, verbatim: *"Pressing keys
closes the zoom overlay. It should stay open while the right mouse button is
pressed, no matter what."* Releasing an ordinary gameplay key (`W`, `Shift`,
…) while `RMB` was held for the zoom used to end it, because the engine's
release check couldn't tell "an unrelated key came up" from "the chord's own
key came up" once the chord was a mouse button the keyboard path's held set
never carries.

`Why RMB by default:` the zoom is an aim-down-sights stand-in, and the right
button is where shooters put that. With the master switch off by default the
binding is inert until the user opts in.

## Match mouse speed

While zoomed, the picture moves `factor` times as far per mouse count, so
with `mouse_scale` on `wlserver_mousemotion()` multiplies the relative delta by
`Zoom_MouseScale()` = `1 / factor`, right after — on top of — gamescope's own
`g_mouseSensitivity` (`--mouse-sensitivity`). Applied at that one site so it
covers every relative-motion source the sensitivity option covers and nothing
else; absolute (windowed) motion is untouched.

`Why the RAMPED factor, not the configured one (2026-09-16):` with the fade on
there are frames where the shape is on screen but the magnification is still
1.0×, and more where it is part-way up. Dividing the mouse by the full factor
there would slow the aim before anything had been magnified, so
`Zoom_MouseScale()` divides by `s_flLiveFactor` — the magnification
`Zoom_FillRequest()` actually put on screen this frame — which is exactly the
configured factor once the ramp has finished, and exactly 1.0 when nothing is
zoomed.

## Keep the button from the game (`consume_button`)

> **Why (2026-09-14):** the user's request, verbatim: *"Add a switch, to
> consume the right mouse click, so the game never sees it."*

Off by default. When on, and the zoom's own chord is a mouse button (`RMB` by
default; the switch text does not say "right" because the chord is whatever
the user has bound), that button's press and release never reach the seat at
all -- `wlserver_dispatch_mouse_button()`'s game branch runs the zoom's hotkey
check **before** `wlr_seat_pointer_notify_button()`, and if the press
completed the zoom's chord with the switch on, the notify is skipped and the
button is tracked in its own set (`s_setSwallowedButtons`, not
`s_setMouseButtonsForwardedToGame`) so the matching release is skipped too.
`Zoom_ConsumesButton()` is the read: true only when the zoom is enabled AND
this switch is on.

Applies to **toggle mode** the same way as hold: both the zoom-in press and
the zoom-out press (and each one's release) are swallowed, because each is a
fresh completion of the chord.

`Why the crosshair's auto-hide is skipped, not delayed, on a swallowed press:`
`Crosshair_NotifyRightButton( true )` exists to hide the crosshair while the
game is aiming down sights on a real click -- there is no ADS to hide it for
when the game never receives the click at all, so that call is skipped
entirely on the swallowed path rather than reordered around it.

**What this does NOT touch:** a keyboard-key chord (`F`) is already kept from
the game by the keybind engine's own swallow rule regardless of this switch,
and a modifier-only chord (`Alt`) is never swallowed by design (Keybinds.h:
"a mouse button is never swallowed [by the engine]... a modifier keeps its
day job"). This switch only ever changes what happens to a *mouse button*,
which is the one case the engine itself always leaves alone. The Switch row's
help text says all three cases so the behaviour is legible from the setting
alone.

## Scroll to change zoom level (`scroll_adjust`)

> **Why (2026-09-14):** the user's request, verbatim: *"Add another switch
> for on demand zoom level adjustment. If enabled, the user should be able to
> scroll while zoomed, to change how big the zoom actually is."*

Off by default. When on, and the zoom is active, the mouse wheel steps
`factor` by **0.25 per notch** (clamped 1.5..5.0) instead of reaching the
game -- both `wlserver_mousewheel()` (the SDL/nested-Wayland/IME/
InputEmulation path) and `wlserver_handle_pointer_axis()` (the raw libinput/
DRM listener, which bypasses that function) gate on `Zoom_IsActive() &&
Zoom_ScrollAdjustEnabled()` on their **game** branch and drop the event
entirely rather than forwarding it -- dropping the whole event, not just the
vertical component, so a horizontal-scroll binding (e.g. a weapon cycle)
cannot fire while zoomed either. One notch is `1.0` in the units these
functions already use (`flY`/120 upstream, or `delta_discrete` /
`WLR_POINTER_AXIS_DISCRETE_STEP`); a device with no discrete report falls
back to the sign of the continuous delta as one notch.

`Why the picture reacts on the very next frame, and where the setting
persists:` `Zoom_OnScroll()` runs on the **wlserver thread** and only ever
touches atomics -- `config::` is documented single-threaded (Keybinds.h's
threading note) and the wlserver thread is not that thread. So a scroll
notch steps the LIVE `s_flFactor` atomic (via the header-only
`Zoom_StepFactor()` helper) and sets `s_bFactorDirty`; `Zoom_FillRequest()`
(steamcompmgr thread, called unconditionally every frame from `paint_all()`)
flushes that flag into `s_Settings.zoom.factor` and calls
`PersistAndRepaint()` at most once per frame no matter how many notches
arrived since the last one, coalescing a fast scroll into one config write.
The composite's `req.flFactor` and `Zoom_MouseScale()` both read the live
atomic directly rather than the persisted copy, so the magnification and the
mouse-speed divisor change immediately; the **Zoom level** slider reads
`s_Settings.zoom.factor` like every other row, so it shows the new value once
the same-frame flush has run.

## Fade in / fade out (`fade_ms`)

> **Why (2026-09-16):** the user's request, verbatim: *"For the Zoom feature,
> add a fade in/fade out. It should fade in the projector outline and then zoom
> inside of that projector gradually. It is important, that this zoom in is
> configurable through the UI (see the crosshair auto-hide feature)."*

The reveal is **two staged phases, not one crossfade**, and that is the whole
point of the request: the user wants to see *where* the projection is before
its contents start moving. So one progress float drives both, split at
`Zoom.h`'s `kZoomFadeSplit` (0.1 since 2026-09-23; was an even 0.5 before):

| Progress | What moves |
| --- | --- |
| `0 → 0.1` | The shape's opacity, 0 → 1, **at its final size**. The outline and the picture inside it fade up together; the magnification is pinned at exactly 1.0×, so what appears is the projector drawn over unmagnified content. |
| `0.1 → 1` | The magnification, `mix(1.0, factor, t)`, *inside* the already-solid shape. |

Releasing the chord runs the same progress back down, so the zoom ramps out
first and the shape fades away second — the reveal backwards, not a separate
animation.

`Why one Param and not three:` the two phases are one reveal the user times as
a whole. A second slider would only let them disagree, and a split of the one
duration expresses the staging without a knob to get wrong.

> **Why 0.1 and not an even split (2026-09-23):** the user's words, verbatim:
> *"The outline of the projector should fade in in the first 10% of the set
> time and the other 90% should be used for increasing the zoom inside of the
> projector."* The original 2026-09-16 design used an even 0.5 split "so
> neither phase reads as the fast one" — that reasoning still holds for *why
> the split is a single named constant and not two independent durations*,
> but the user decided the outline itself should read as near-instant and
> the magnification should get almost the whole budget. At the default
> `fade_ms` of 200 the outline now appears in **20 ms** and the zoom-in takes
> the remaining **180 ms**, versus 100/100 before.

`Why 1.0× is invisible:` at factor 1.0 `cs_zoom.comp` samples
`u_srcPerDst = base.scale`, i.e. one source texel per projection pixel when the
game is rendered at output resolution. The projection is placed at **whole**
pixels — `floor(centre - uW*0.5)`, so the composite copies it texel for texel
rather than resampling it — and the shader is handed that same floored origin
back as its fetch centre
(`u_srcCenter = (dstX + uW*0.5 + base.offset) * base.scale`, `rendervulkan.cpp`).
Projection pixel `p` therefore lands on `dstX + p + 0.5`: the **centre** of the
very texel it is drawn over, **whatever the projection's size** — the identity no
longer depends on the size's parity, which is what the rest of this section is
about.

> The one remaining precondition is `base.scale == 1`. A game rendered *below*
> output resolution samples at `scale/factor` source texels per projection pixel,
> so factor 1.0 is not a texel centre — but that frame's base is being resampled
> by the upscaler underneath the shape anyway, so there is no sharp original to
> be identical *to*. Inherent to magnifying a scaled source, not a defect.

> Measured (2026-09-16, headless captures diffed against a zoom-off frame). In
> every case the *only* pixels that differ during phase 1 are the ring's own:
> 360 px square on 1280×720 — 0 delta over all 128 164 interior pixels; 361 px
> on 1280×720 — 0 over 128 881; 77 px on 1366×768 (Size 0.10) — 0 over 5 625;
> 38 px on 1366×768 (Size 0.05, the slider minimum) — 0 over 1 296. Each at
> four alphas spanning phase 1.
>
> **Do not "simplify" the fetch centre back to `texW*0.5`.** That was the
> original code, and it is right only when `texW - uW` and `texH - uH` are both
> **even**: the half-texel disagreement between a centre computed from the base's
> exact middle and a layer placed at a floored origin puts the fetch on a texel
> *boundary* for an odd difference, and bilinear then returns the average of two
> neighbours. Measured before the fix: 361 px on 1280×720 softened 15 196–15 461
> of its 128 881 interior pixels (up to 85/255), and 77 px on 1366×768 — Size
> 0.10, one plain slider step — softened 725 of 5 625 (up to 65/255). Reachable
> sizes are all even at 720p/1080p/1440p, so this only ever showed on bases that
> are not a multiple of 40 (768, 1366, …). The fade is what made it *visible*,
> by being the first thing to ever put a 1.0× frame on screen; the geometry
> quirk predated it.
>
> Deriving the centre from the floored origin was chosen over rounding the
> projection to an even size (which would move every size slider's result by a
> pixel) and over dropping the `floor` (which would make the composite resample
> the projection layer itself, blurring it at *every* magnification). It is one
> expression, changes nothing when the difference is already even, and above
> 1.0× it moves the aim by at most half a source texel — onto a texel centre
> rather than a boundary, so the magnified picture is if anything sharper.

`Why the shader was not touched:` the fade is the zoom layer's own `opacity`.
`Zoom_FillRequest()` writes `FrameInfo_t::Zoom_t::flAlpha`,
`vulkan_composite()` copies it into the layer's `opacity`, and
`alphamode.h`'s premultiplied `BlendLayer()` already does
`layerColor * opacity + dst * (1 - a*opacity)` — the correct fade of the whole
projection, outline included, against the untouched frame beneath. The 1 px
ring keeps its **geometry** at every alpha and only loses opacity, which is
what a fading outline should do; scaling the shape instead would have made the
ring sub-pixel and blurred it.

`Why real elapsed time:` `Zoom_FillRequest()` advances the progress by
`get_time_in_nanos()` deltas — the compositor's own monotonic clock, the same
one everything else here is timed against — divided by `fade_ms`, not by a
per-frame constant. The compositor runs at whatever rate the game gives it, so
a per-frame step would make the fade twice as fast at 120 fps as at 60.
`Zoom_AdvanceFade()` is a plain integrator, as `crosshair::AdvanceHide()` is
and for the same reason: the state is the progress itself, so a **re-press
mid-fade-out reverses from where it got to** instead of snapping to 0 and
restarting.

`fade_ms = 0` is instant in both directions — the progress is forced straight
to 1 or 0 — which is byte-for-byte the pre-2026-09-16 behaviour.

Two smaller consequences worth knowing:

- While the progress is still moving, `Zoom_FillRequest()` calls
  `force_repaint()`, exactly as `Crosshair_Draw()` does for its hide animation:
  a paused game commits no frames of its own, and without this the fade would
  stall halfway. It is called **only** while moving — parked at 0 or at 1 it is
  not, so a static screen costs nothing.
- The last-advanced timestamp is **cleared whenever the progress parks**, at 0
  *or* at 1, so the frame that starts moving again measures a zero delta rather
  than however long the game happened to sit idle beforehand. Both ends need it
  for the same reason: `force_repaint()` stops at both, so both can be followed
  by an arbitrarily long gap. `Why (QC, 2026-09-16):` clearing it only at 0
  left the release after an idle spell at full zoom measuring the whole gap in
  one delta, which snapped the fade-out out of existence instead of playing it.

## Sharpen (`sharpen`)

> **Why (2026-09-22):** the user, verbatim: *"For the zoom feature, add an
> option to add a sharpening filter on top of the projector area only, so the
> zoom content doesn't look as blurry as it does right now."*

The projector's blur is `cs_zoom.comp`'s one hardware-bilinear fetch per output
pixel (`textureLod(s_samplers[0], uv, 0)`): at 1.5–5x magnification that
interpolation is the softness. `sharpen` (0–1, default **0**, off) applies a
**contrast-clamped unsharp mask** to the projector's own picture to counter
it.

**History — first cut used FSR1 RCAS, replaced the same day.** The first
version reused the Shaders area's Pre-Sharpen operator (FSR1 RCAS,
`cs_effects_layer0.comp`), with its 5-tap cross spaced in *projector-pixel*
space (one MAGNIFIED pixel apart, via a `d -> uv` remap of `FsrRcasLoadF`).
A QC pass measured that raising edge-gradient energy inside the projector by
only **+4.3%** at the top of the slider — invisible at normal viewing size,
"just as blurry as the user's complaint describes" at 1.0. Two design faults,
diagnosed and fixed the same day:

1. **Wrong spatial scale.** The blur being corrected is bilinear
   interpolation *between source texels*, which sit `factor` projector pixels
   apart (3px at 3x). A 1-projector-pixel-spaced tap sees almost no gradient
   inside a 3px-wide interpolation ramp — it was measuring the wrong scale
   entirely.
2. **RCAS is a touch-up, not a de-blur.** Its negative lobe is capped at
   `FSR_RCAS_LIMIT` (0.1875) times `con.x`, and the old `con.x` mapping
   topped out at 0.667 (short of RCAS's own 1.0 ceiling) — so the *effective*
   lobe was capped at ≤0.125 regardless of the slider. RCAS is designed as a
   mild post-upscale polish, not something that can meaningfully de-blur a 3x
   magnification.

**The operator (`cs_zoom.comp`).** Taps are spaced **one SOURCE texel**
apart — `uv ± 1/textureSize` per axis, the scale the blur actually lives at,
not the projector-pixel scale the first cut used:

```
c              = bilinear(uv)
n,s,e,w,
ne,nw,se,sw    = bilinear(uv ± 1 source texel, all 8 directions — the full 3x3)
blur           = mean(n,s,e,w,ne,nw,se,sw)
out            = c + amount * (c - blur)
lo             = min(c, n,s,e,w,ne,nw,se,sw)   -- per channel
hi             = max(c, n,s,e,w,ne,nw,se,sw)   -- per channel
out            = clamp(out, lo, hi)
```

**The per-channel clamp to the local 3x3-source-texel neighbourhood's own
min/max is what makes a large `amount` safe.** The result can never leave the
range of values already present in that neighbourhood, so there is no
overshoot — no halo, no new dark or bright fringe — no matter how large
`amount` gets; it only makes edges steeper, never wrong. This is a stronger,
more direct guarantee than RCAS's own clip limiting, and it is why the new
operator can run at an `amount` far past anything RCAS's `con.x` could
safely reach.

**Why the FULL 3x3 (8 taps: cross + diagonals), not just the 4-tap cross.**
Measured directly (`amount` swept 3 → 2000 with the clamp active, so the
result sits at its true asymptote — the ceiling any *finite* `amount` can
approach, since the clamp bounds the result regardless of how large `amount`
gets): a 4-tap cross plateaus at +19.7% / +22.7% (midtone / darklight test
clients, below), short of target. Adding the four diagonal taps — the full
3x3 the brief's own halo-check wording describes — raised the ceiling to
+24.1% / +29.0%. Widening the tap radius further (2, 3, 4 source texels,
same sweep) did **not** raise the ceiling any further; it plateaued at the
same figures and fell at radius 4. The content's own local contrast at this
magnification, not the window size past one texel, is what bounds the
result — this is a hard ceiling of the "genuinely zero-overshoot" family of
operators on this content, not a tuning miss.

**Applied before the shape's `fill`/alpha**, exactly as the first cut was —
only the picture inside the ring is ever fed through the unsharp mask, the
ring's own antialiasing and the outline are provably untouched (their `rgb`
is multiplied by `fill == 0` regardless of what upstream sharpening computed
for them). `sharpen == 0` (`u_sharpenAmount == 0.0`, a plain float — no
bit-cast trick needed since nothing here packs it for FFX's uint-SIMD
constants any more) branches around the whole block entirely, so the shader
is byte-for-byte identical to before this feature existed at the default —
measured, not just argued (below).

**The amount mapping is NOT linear** (retuned a second time, same day, once
a linear map was shipped and found to leave the slider nearly dead — see
below). `Zoom_SharpenAmount()` (`Overlay/Zoom.h`, the one place this
mapping lives) inverts a fitted curve of the operator's own gain instead:

```
amount(k) = (kZoomSharpenMaxAmount * kZoomSharpenHalfK * k)
          / (kZoomSharpenMaxAmount * (1 - k) + kZoomSharpenHalfK)

kZoomSharpenMaxAmount = 32.0   -- amount at k = 1.0
kZoomSharpenHalfK     = 1.39   -- Michaelis-Menten half-saturation amount
```

**Why not linear.** The first cut used a plain `amount = kMaxAmount *
sharpen` with `kMaxAmount = 64` — chosen (correctly) to sit near the
operator's own asymptote, but that revealed a UX problem the +30% target
had been hiding: because the clamped-unsharp-mask's gain saturates fast
(see "Why the FULL 3x3" above), `sharpen 0.5` (`amount = 32`) already
reached **97%** of what `sharpen 1.0` (`amount = 64`) did. The bottom half
of the slider did almost nothing; the whole 0..1 range should be doing
something the user can feel.

**How the curve was found.** Mean gradient-energy gain vs raw `amount`,
swept and measured directly (default test client, `zoom.sharpen` forced to
`1.0` with `kZoomSharpenMaxAmount` temporarily overridden to each value):

| amount | 0.5 | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 2000 (asymptote) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gain | 7.9% | 11.2% | 14.4% | 17.7% | 20.7% | 22.7% | 23.7% | 24.3% | 25.1% |

A Michaelis-Menten curve, `gain(a) = Ginf·a/(a+K)`, fits this table well
with `Ginf = 25.12` (the measured asymptote) and `K = 1.39` (least-squares
over the table, `Ginf` held fixed) — `kZoomSharpenHalfK` above is that `K`.
`kZoomSharpenMaxAmount = 32` is the **smallest** amount in the table
reaching **~95% of `Ginf`** (23.726 / 25.120 = 94.5%; `64` only adds
another 2.4 percentage points for double the amount — wasted headroom at
the top of the slider), per the instruction to pick the smallest amount
that gets there rather than defaulting to a bigger, safer-feeling number.

`amount(k)` is the inversion of that fitted curve, rescaled so that
`gain(amount(k))` tracks `k · gain(kZoomSharpenMaxAmount)` — i.e. the
SLIDER, not the raw amount, grows roughly linearly in its own effect. The
formula is exact at both ends **by construction**, independent of the
curve fit's own error: `amount(0) = 0`, `amount(1) = kZoomSharpenMaxAmount`
always. `amount(0.25) ≈ 0.438`, `amount(0.5) ≈ 1.279`, `amount(0.75) ≈
3.553`.

Unit-tested (`tests/test_config.cpp`, `"Zoom_SharpenAmount: 0 at k=0,
kZoomSharpenMaxAmount at k=1, monotonic, clamped"`): the two exact
endpoints, the three intermediate values above (independently computed),
monotonicity, and clamping outside `[0, 1]`.

**The fade-identity invariant governs it too**, unchanged from the first
cut. The fade's first phase (see above) is a *measured* guarantee that the
projector is byte-identical to the unzoomed frame while the magnification is
pinned at exactly 1.0x; sharpening during that phase would break it. So the
MAPPED amount is scaled by `Zoom_SharpenRamp()` (`Overlay/Zoom.h`, next to
`Zoom_SharpenAmount()` and the fade helpers) — `clamp((curFactor - 1) /
(targetFactor - 1), 0, 1)`, guarded for a target at or below 1.0 — computed
in `Zoom_FillRequest()` from the SAME ramped `flFactor` the picture's own
magnification uses:

```
req.flSharpen = Zoom_SharpenAmount(clamp(z.sharpen, 0, 1)) * Zoom_SharpenRamp(req.flFactor, targetFactor);
```

`FrameInfo_t::Zoom_t::flSharpen` now carries the already-mapped,
already-ramped **amount** (shader units, not the 0..1 slider value) into
`vulkan_composite()`'s zoom block, which passes it straight to
`ZoomPushData_t`'s constructor — no mapping or ramping logic lives in
`rendervulkan.cpp` any more; `ZoomPushData_t` just forwards the float it is
given.

> **Measured (2026-09-22 retune, headless captures, `--backend wayland`,
> isolated `XDG_CONFIG_HOME`, `screenshot <path> 3`, RMB via
> `wlserver_debug_mouse_button "273 1"`, `zoom.fade 0`, factor 3.0, circle,
> size 0.5):** mean gradient magnitude (central-difference on luma) inside
> the projector mask, three test clients — kitty's own default colours
> printing this repo's README (the same client the +4.3% baseline used),
> and two mid-tone clients chosen so clipping can't hide a halo
> (`#707070`/`#e0e0e0` and `#d8d8d8`/`#202020`), through the REAL
> `zoom.sharpen` slider (not a raw `amount` override):
>
> | Client | sharpen 0 | sharpen 0.5 | sharpen 1.0 |
> | --- | --- | --- | --- |
> | default (README text) | 8.783 | 9.866 (+12.3%) | 10.866 (**+23.7%**) |
> | midtone (#707070/#e0e0e0) | 4.360 | 4.891 (+12.2%) | 5.358 (**+22.9%**) |
> | darklight (#d8d8d8/#202020) | 5.901 | 6.809 (+15.4%) | 7.531 (**+27.6%**) |
>
> `sharpen 1.0`'s numbers match the first retune's measurement exactly
> (`amount(1.0) = kZoomSharpenMaxAmount = 32` by construction, the same
> amount that build used). `sharpen 0.5` now lands at roughly **half** of
> `sharpen 1.0`'s gain, not 97% of it as the interim linear map (`amount =
> 64·sharpen`) had it — the fix the remapping above set out to make. The
> default client's full curve, all four reported slider positions:
>
> | k | 0.25 | 0.5 | 0.75 | 1.0 |
> | --- | --- | --- | --- | --- |
> | gain | +7.3% (31% of max) | +12.3% (52% of max) | +17.2% (72% of max) | +23.7% (100%) |
>
> A Sobel cross-check on the default client at `sharpen 1.0` agrees in
> direction (+21.2%). **The target of +30% was not reached** by any client
> — see "Why the FULL 3x3" above for the measured ceiling and why a
> genuinely zero-overshoot operator cannot cross it on this content; this
> is still a ~5.5–6.6x improvement over the RCAS-era +4.3%, now spread
> across the whole slider instead of bunched at the top. **Zero halo
> violations** on all three clients (comparing `sharpen 1.0` against a
> per-channel 3x3-source-texel min/max filter of that same client's own
> `sharpen 0` capture — the brief's own halo-check method — with a 1-level
> tolerance for 8-bit rounding): 0 of 92,944–94,548 masked pixels violate
> it, any client. **Containment**: 0 of 921,600 pixels differ between
> `sharpen 0` and `sharpen 1` outside the circle's own radius, for all
> three clients — the projector's centre was measured from the
> unzoomed-vs-`sharpen 0` diff each time, not assumed. **The 1px black
> outline is unchanged**: its fully-opaque pure-black locus (16 pixels per
> mid-tone client, at pixel-quantised resolution around a ~360px circle) is
> bit-identical between `sharpen 0` and `sharpen 1` — also provable
> algebraically, since `rgb` there is multiplied by `fill == 0` regardless
> of any upstream sharpening. **`sharpen 0` is byte-identical to a capture
> built from master `d259cae`** (the pre-retune, RCAS-era commit) for both
> mid-tone clients: 0 of 921,600 pixels differ, max delta 0 — unaffected by
> this second retune, since `amount(k=0) = 0` exactly regardless of the
> mapping curve, so the off-path never changed shape.
> `build-release/verify-shots/zoom-sharpen-v2-2026-09-22/` has the first
> retune's captures (`shots/`, `README.txt`) and this remapping's
> (`v3/README.txt`, plus `sweep/` for the raw amount- and k-sweep
> captures behind the tables above); the RCAS-era measurements are
> preserved in `build-release/verify-shots/zoom-sharpen-2026-09-22/`.

**Third retune (2026-09-22): the ceiling was lowered by visual QC, not
gradient energy — gradient energy was measuring the wrong thing.** A
grader looking at real text inside the projector at `sharpen 1.0`
(`amount = kZoomSharpenMaxAmount = 32`) found the top of the range
destructive: `e`'s aperture gouged to black, `4`/`2`'s diagonals
fragmented, and curves stair-stepped at the 3px magnification period. The
per-channel min/max clamp (see "Why the FULL 3x3" above) stops the
operator *overshooting* past its 3x3-source-texel neighbourhood, but it
does not stop it *collapsing onto* that neighbourhood's own extremes — a
mid-grey stroke pixel surrounded by darker background can be pulled all
the way down to the neighbourhood minimum, which raises gradient energy
(a bigger local difference) while visibly destroying the glyph. That is
exactly why the metric that picked `32` as "94.5% of asymptotic gain"
approved of a range a human grader rejected: it rewards binarising an
antialiased edge as if it were legitimate sharpening. By the old
`sharpen 0.75` (`amount ≈ 3.55`) the gouging was already objectionable;
`sharpen 0.4–0.5` under the OLD ceiling (`amount ≈ 1.0–1.3`) read as
clean and clearly crisper than off.

`kZoomSharpenMaxAmount` was cut from `32.0` to **`1.3`** — exactly the old
`amount(k=0.5)`, the grader's clean point — reusing the same
Michaelis-Menten formula and `kZoomSharpenHalfK = 1.39` unchanged. This
keeps the slider-evenness property exactly (`gain(amount(k)) = k ·
gain(kZoomSharpenMaxAmount)` by construction, independent of the
ceiling's value) and `amount(0) = 0` exactly (`sharpen 0` stays
byte-identical — the shader still branches out on `amount == 0`). Since
`1.3 < kZoomSharpenHalfK`, the new curve sits in the fitted gain curve's
near-linear low end rather than its saturating top, so `amount(k)` itself
is now close to linear in `k` — a coincidence of where the new ceiling
lands, not a change of approach:

| k | 0.25 | 0.5 | 0.75 | 1.0 |
| --- | --- | --- | --- | --- |
| `amount(k)` | 0.191 | 0.443 | 0.790 | 1.3 |
| gain (from the MM fit, not re-measured) | +3.0% | +6.1% | +9.1% | +12.1% |

The gain row is computed from the same fitted curve as the table above
(`Ginf = 25.12`, `K = 1.39`), not a fresh gradient-energy sweep — this
retune's own evidence is the visual QC grade, not a metric, per the
finding above that the metric itself was misleading at the top of the
old range. Evidence: `build-release/verify-shots/zoom-sharpen-v2-2026-09-22/v4/`
(crops and a `sweep-stack.png`-style vertical stack for the default
client at k = 0/0.25/0.5/0.75/1.0, plus k = 0/1.0 for both mid-tone
clients; `README.txt` has the numbers). `sharpen 0` remains
byte-identical to every earlier build (`amount(0) = 0` regardless of the
ceiling), and 0 pixels differ outside the projector between `sharpen 0`
and `sharpen 1` under the new ceiling either.

**If a stronger ceiling is wanted later**, the QC finding points at a
different fix than raising this number again: tighten the *clamp*
itself — lerp its min/max bounds toward the original pixel as `amount`
grows, so the operator can no longer collapse a stroke onto its
neighbourhood's extreme value — rather than trading visible strength for
less gouging by picking a number partway up the same curve.

## Threading

`Zoom_OnChord()`, `Zoom_MouseScale()`, `Zoom_ConsumesButton()`,
`Zoom_IsActive()`, `Zoom_ScrollAdjustEnabled()` and `Zoom_OnScroll()` all run
on the wlserver thread and touch atomics only; the settings they need
(enabled, mode, factor, mouse_scale, consume_button, scroll_adjust) are
mirrored into atomics by the steamcompmgr thread whenever the config cache is
(re)loaded. `Zoom_OnScroll()` is the one exception that writes an atomic the
steamcompmgr thread later reads back (`s_flFactor`, `s_bFactorDirty`) rather
than only reading mirrored ones -- see "Scroll to change zoom level" above
for why the persist itself has to happen on the other thread. Everything else
is the steamcompmgr thread, like Crosshair.cpp.

The fade's own state (`s_flFadeProgress`, `s_ulLastFadeNs`) is **steamcompmgr
thread only** — it is read and advanced in `Zoom_FillRequest()` and nowhere
else. The one value it publishes across the boundary is `s_flLiveFactor`, the
magnification actually on screen this frame, which `Zoom_MouseScale()` reads on
the wlserver thread (see "Match mouse speed" above).

## Verified

- `tests/test_keybinds.cpp` — mouse-button terms parse and format; the zoom
  fires as a subset, is not swallowed on a button, reports its release, does
  not re-fire while down, is swallowed on a real key, is a plain press on a
  modifier, loses to an exact chord, and is dropped by a focus boundary. A
  regression case models wlserver's two real held-set paths separately (a
  mouse event's set is keyboard-ledger ∪ buttons-down, a keyboard event's is
  the keyboard ledger alone) and proves an unrelated key's press and release
  no longer end a mouse-button-held zoom (2026-09-14).
- `tests/test_config.cpp` — every `zoom` field round-trips; absent means
  defaults.
- `tests/test_overlay_ui.cpp` — the area has an icon and sits in MISC.
- `tests/test_config.cpp` — `zoom.fade_ms` round-trips with the rest of the
  section (2026-09-16); a file without the key takes the 200 ms default, so no
  schema bump or migration was needed.
- Headless capture run 2026-09-16 (`--backend headless`, `vkcube`, isolated
  `XDG_CONFIG_HOME`, `screenshot <path> 3` for a full composition, RMB driven
  by `wlserver_debug_mouse_button "273 1"`): at `fade_ms 1000`, `factor 4`,
  `size 0.5`, the ring's darkness against its surroundings climbs monotonically
  from ~0 to full over the first ~500 ms while the content discontinuity across
  the ring stays at its unzoomed baseline, and only then does the
  discontinuity jump — the staged order, at the split. Release reverses it:
  the discontinuity returns to baseline by ~650 ms and the ring is gone by
  ~900 ms, ~1000 ms for the whole cycle. At `fade_ms 0` the first captured
  frame after the press is already fully zoomed and the first after the
  release already clear.
- `build-release/verify-shots/zoom-2026-09-14/` — headless captures (private
  sway, `effects_scene_client`, `wlserver_debug_mouse_button "273 1"`):
  circle/hold on the `colors` bands shows the bands at exactly 2× inside a
  360 px ring with the ring drawn, and before/released captures are
  byte-identical; rectangle/toggle on `texdark` stays zoomed across the
  release and clears on the second press. `keypress-while-held-*.png` — RMB
  held, then a `W` tap and a `LShift` tap injected with `wlserver_debug_key`
  in between: the zoom circle is present and byte-identical across the held
  and both tap frames, and gone (byte-identical to the pre-zoom frame) only
  after the RMB release.
- `tests/test_config.cpp` — `Zoom_StepFactor()`'s clamp-and-step arithmetic
  (0.25 per notch, 1.5..5.0 both ends), with no Zoom.cpp/compositor link.
- `consume-*.png` and `scroll-*.png` in the same directory (2026-09-14): with
  `consume_button` on, RMB still zooms (the circle appears) and the
  `log_binding` debug channel logs "button 273 swallowed ... never reached
  the seat" for the press -- the release is silent because it is never
  forwarded either, by the same tracking-set path. With `scroll_adjust` on,
  RMB held at the default 2.0× and two `wlserver_debug_mouse_wheel "-1 -1"`
  notches (scroll up) produce a visibly larger circle and
  `overlay_e2_get zoom.factor` reads `2.5`.
- `tests/test_config.cpp` — `zoom.sharpen` round-trips with the rest of the
  section; `Zoom_SharpenRamp()`'s arithmetic (0 at factor 1.0, 1 at the
  target, linear between, clamped past both ends, safe for a target at or
  below 1.0) with no Zoom.cpp/compositor link (2026-09-22);
  `Zoom_SharpenAmount()`'s curve (exact at both ends by construction,
  monotonic, matches three independently-computed intermediate values,
  clamped past both ends) with the same no-Zoom.cpp-link property
  (2026-09-22, the slider remapping).
- `build-release/verify-shots/zoom-sharpen-v2-2026-09-22/` — see the
  "Sharpen" section above for the numbers: `sharpen 0` byte-identical to
  master `d259cae` (the pre-retune build), every differing pixel between
  `sharpen 0` and `sharpen 1` inside the projector's own radius, zero halo
  violations against a 3x3-source-texel min/max filter, and the unchanged
  1px outline. `shots/` and `README.txt` are the first retune (the linear
  map); `sweep/` and `v3/README.txt` are the slider remapping's amount- and
  k-sweeps and its own re-confirmation captures (default, midtone,
  darklight, all three checked); `v4/README.txt` is the ceiling-lowering
  retune (ceiling `32 -> 1.3`) with the default client's full k-sweep and
  its vertical stack image, plus k=0/1.0 re-confirmation for both mid-tone
  clients. The RCAS-era measurements (superseded) are preserved in
  `build-release/verify-shots/zoom-sharpen-2026-09-22/`.
