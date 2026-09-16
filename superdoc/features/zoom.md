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
| | Zoom key | — | Read-only: the `zoom` action's chord, `RMB` by default. Rebound under **Keybinds**, like every other hotkey ([keybinds.md](keybinds.md)). |
| | Activation | `mode` | `"hold"` (zoomed while the chord is down) or `"toggle"` (press in, press out). Int-backed Choice like `crosshair.hide_mode`: `overlay_e2_set zoom.mode 1` is Toggle. |
| | Zoom level | `factor` | 1.5–5.0, step 0.1, default 2.0. |
| | Match mouse speed | `mouse_scale` | Multiplies relative mouse motion by `1 / factor` while zoomed. See below. |
| | Keep the button from the game | `consume_button` | Default off. Swallows the zoom chord's own mouse button, press and release, instead of forwarding it. See below. |
| | Scroll to change zoom level | `scroll_adjust` | Default off. While zoomed, the wheel steps `factor` by 0.25 instead of reaching the game. See below. |
| | Fade duration | `fade_ms` | 0–2000 ms, step 10, default **200**. `ZeroMeans("Instant")`. One duration for the whole staged reveal — see below. |
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
`Zoom.h`'s `kZoomFadeSplit` (0.5):

| Progress | What moves |
| --- | --- |
| `0 → 0.5` | The shape's opacity, 0 → 1, **at its final size**. The outline and the picture inside it fade up together; the magnification is pinned at exactly 1.0×, so what appears is the projector drawn over unmagnified content. |
| `0.5 → 1` | The magnification, `mix(1.0, factor, t)`, *inside* the already-solid shape. |

Releasing the chord runs the same progress back down, so the zoom ramps out
first and the shape fades away second — the reveal backwards, not a separate
animation.

`Why one Param and not three:` the two phases are one reveal the user times as
a whole. A second slider would only let them disagree, and a split of the one
duration expresses the staging without a knob to get wrong. `kZoomFadeSplit` is
an even 0.5 so neither phase reads as the fast one — both are plain linear
ramps of the same length.

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
