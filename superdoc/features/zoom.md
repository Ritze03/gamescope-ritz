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
> Brightness, Bloom, Brightness Map, …) run as a pre-pass **inside**
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

## Threading

`Zoom_OnChord()` and `Zoom_MouseScale()` run on the wlserver thread and touch
atomics only; the settings they need (enabled, mode, factor, mouse_scale) are
mirrored into atomics by the steamcompmgr thread whenever the config cache is
(re)loaded. Everything else is the steamcompmgr thread, like Crosshair.cpp.

## Verified

- `tests/test_keybinds.cpp` — mouse-button terms parse and format; the zoom
  fires as a subset, is not swallowed on a button, reports its release, does
  not re-fire while down, is swallowed on a real key, is a plain press on a
  modifier, loses to an exact chord, and is dropped by a focus boundary.
- `tests/test_config.cpp` — every `zoom` field round-trips; absent means
  defaults.
- `tests/test_overlay_ui.cpp` — the area has an icon and sits in MISC.
- `build-release/verify-shots/zoom-2026-09-14/` — headless captures (private
  sway, `effects_scene_client`, `wlserver_debug_mouse_button "273 1"`):
  circle/hold on the `colors` bands shows the bands at exactly 2× inside a
  360 px ring with the ring drawn, and before/released captures are
  byte-identical; rectangle/toggle on `texdark` stays zoomed across the
  release and clears on the second press.
