# Crosshair

A crosshair gamescope draws over the game: four arms, an optional centre
dot, an optional outline, an optional auto-hide while the right mouse
button is held, and an optional per-axis stretch to match a stretched
game. `src/Overlay/Crosshair.{h,cpp}` (config, settings area, right-click
state, the draw), `src/Overlay/CrosshairMath.h` (the geometry and the hide
animation, pure and unit-tested in `tests/test_crosshair.cpp`), config in
`config::CrosshairSettings` (`src/Config/ConfigSchema.h`, JSON key
`crosshair`), settings area `system.crosshair` ("Crosshair" in the rail,
right after the HUD). Default **off**.

> **Why (2026-09-05):** the user runs frame generation externally —
> `lsfg-vk` as a Vulkan layer inside the game's own process. It
> interpolates the game's own frame, so anything drawn *inside* it — above
> all the in-game crosshair — smears between real frames. A crosshair
> gamescope draws is composited **after** interpolation, over the finished
> frame, and cannot smear. See
> [fidelityfx-opticalflow-framegen.md §5.3](../planning/fidelityfx-opticalflow-framegen.md)
> for the layer analysis that makes the compositor the right place for it.

## The settings, top to bottom

`system.crosshair`'s row order matches this list (`Crosshair_RegisterArea()`).
Every row except the master switch is greyed with a reason while the
crosshair is off; each element's own rows are additionally greyed while
that element is off ("the dot is off", and so on).

| Group | Row | Config field | Notes |
| --- | --- | --- | --- |
| Crosshair | Show crosshair | `enabled` | Master switch. Default off. |
| Line | Show lines | `line_enabled` | The four arms. |
| | Length | `line_length` | px, 1–64. Each arm's own length. |
| | Width | `line_width` | px, 1–16. **1 is exactly one pixel** — see 1px mode. |
| | Gap | `line_gap` | px, 0–64, from the centre column/row's *edge* to the arm. 0 joins the arms into a solid plus. |
| | Colour | `line_color` | `0xRRGGBB`, the shared RGB colour picker (`CompositeKind::Color`, as `PanelCursor.cpp` uses). |
| | Opacity | `line_opacity` | 0–1. The user's word is "transparency"; the row is labelled Opacity because a slider whose 0 means invisible reads backwards under the other name. `transparency` is a search keyword. |
| Dot | Show dot | `dot_enabled` | |
| | Size | `dot_size` | px, 1–16. Always a **square** — see geometry. |
| | Colour / Opacity | `dot_color`, `dot_opacity` | as for the line |
| Outline | Show outline | `outline_enabled` | |
| | Width | `outline_width` | px, 1–8 |
| | Opacity / Colour | `outline_opacity`, `outline_color` | |
| Auto-hide | Hide while holding right-click | `hide_on_right_click` | |
| | Hide mode | `hide_mode` | Choice. **Stored** in config as a stable string key (`"fade"` / `"focus"` / `"shrink"`, `CrosshairSettings::hide_mode`); the **row is int-backed** like every registry Choice, so `overlay_e2_set crosshair.hide_mode N` takes the option index -- `0` fade, `1` focus, `2` shrink -- and a word is parsed as 0 (fade). `Crosshair.cpp`'s `HideModeToInt()`/`HideModeFromInt()` are the two-way map. |
| | Time to hide | `hide_time_ms` | ms, 0–2000; 0 hides at once |
| Scaling | Apply scaling | `apply_scaling` | see Geometry |

Pixel sizes are **ints**, not floats: the whole point of the 1px mode is
that "1" is exactly one pixel, so a fractional size has no meaning here.
Every setter persists through `config::EnqueueRoutedWrite()` (per-game
snapshot when one is active, `global.json` otherwise — a normal per-layer
section, copied by `ApplyProfile()` like `fps_display`) and calls
`force_repaint()`, so an edit shows on the next frame even with an idle
game.

## Where it is drawn: the HUD's layer

The crosshair has **no layer of its own**. `FpsDisplay_AddLayer()`
(`src/Overlay/FpsDisplay.cpp`) draws it into the FPS HUD's ImGui frame, into
the background draw list, before the readout, and the HUD's single
`Layer_t` carries both.

> **Why:** `k_nMaxLayers` / `VKR_MAX_LAYERS` is 6, a busy frame already
> fills it (base + override + external overlay + Steam overlay + cursor +
> mura), and `LayerStack_t::push()` fails **silently** when full — see
> [compositing-vulkan.md](compositing-vulkan.md#layer-order-zpos). The HUD's
> layer already spans the whole output and sits above the game and the
> cursor, below the toasts and the Shell, which is exactly where a
> crosshair belongs.

Consequences for the HUD's own logic, all in `FpsDisplay.cpp`:

- The layer exists when the readout **or** the crosshair is on, and not
  at all when neither is (`bReadout || bCrosshair`), so a run with both
  off still never creates an ImGui context, a texture or a layer.
- The 500 ms repaint-timer thread's enable flag (`s_bHudEnabledForTimer`)
  is recomputed by `UpdateTimerFlag()` as that same predicate, so "no
  layer" and "no keepalive repaints" cannot disagree. A static crosshair
  needs no keepalive of its own; counting it costs two idle repaints a
  second while it is on.
- **Split mode.** The readout's *Inverted* text colour puts the layer in
  `ALPHA_BLENDING_MODE_INVERT`, whose shader (`src/shaders/alphamode.h`)
  decides **by brightness** which texels invert the game — the contract the
  readout keeps by drawing its digits pure white and everything else dark
  ([fps-display.md](fps-display.md#text-colour-fixed-vs-inverted)). A
  bright user-chosen crosshair colour would trip that selector and invert
  the game under it instead of showing its colour. So when Inverted mode
  and the crosshair are **both** on, the HUD texture is rendered at twice
  the output's height — readout in the top half, crosshair in the bottom
  half — and two `Layer_t`s sample the two halves of the one texture
  (`composite.h`'s `sampleLayerEx`: `texcoord = (pixel + offset) * scale`,
  so the second layer's `offset.y = output height`), the first INVERT, the
  second COVERAGE. Same context, same render pass, same submission. This is
  the **one** case the HUD pushes a second layer; if that push fails the
  crosshair sits the frame out and the readout is unaffected. Every other
  combination is one output-sized texture and one layer, as before.

## Geometry

All of it is `crosshair::Build()` in `CrosshairMath.h`; the tests in
`tests/test_crosshair.cpp` pin the exact pixels.

**Centre.** The centre of **layer 0's on-screen rect**, not the output's,
so a letterboxed or offset game still gets the crosshair on the game.
`paint_all()` has already pushed the base plane as layer 0 when the HUD
runs, and `Layer_t::offset`/`scale` *are* the sampling mapping, so the
base's on-screen rect is `[-offset, -offset + tex.size / scale)` in output
pixels (`ResolveCrosshairFrame()`). With no base plane this frame it falls
back to the output centre.

**Apply Scaling off:** every size is an output pixel and the crosshair is
drawn square whatever the game's aspect. **On:** sizes are *game* pixels,
multiplied per axis by "output pixels per game pixel" = layer 0's
on-screen size divided by the game's own committed buffer size — so a 4:3
image stretched to 16:9 gets a horizontally stretched crosshair, exactly
like a stretched in-game one. The game's buffer size comes from
`g_uBaseLayerSourceWidth/Height` (`steamcompmgr.hpp`), published by
`paint_window_commit()` from the raw commit rather than read off layer 0's
texture, because that texture may already be gamescope's pre-emptively
upscaled copy (`ShouldPreemptivelyUpscale()`) whose size says nothing
about the game's.

**1px mode.** Every primitive — arm, dot, outline — is an axis-aligned
`AddRectFilled` on **whole-pixel** coordinates, drawn with
`ImDrawListFlags_AntiAliasedFill` (and `…Lines`) cleared for exactly those
draws and restored afterwards, so width 1 is one solid pixel with no
half-alpha neighbours. Sizes snap with `lround` after scaling (so under
Apply Scaling a 1px game-space line becomes `round(scale)` output pixels,
still whole); a thickness never snaps below 1; an arm whose length snaps
below 1 is omitted. The centre snaps by parity (`detail::SnapCenter`): an
odd thickness centres on a pixel, an even one on a pixel edge, so every
element is exactly its thickness and mirror-symmetric about the snapped
centre. The **gap is measured from the centre column/row's own edge**, so
at gap 0 the arms touch the centre square; that square then joins the arms
so a closed gap reads as one continuous plus, not two arms with a pixel
missing.

**The dot is always a square**, at every size. *Why:* a circle cannot be
pixel-exact at small sizes, the outline/union arithmetic below works
uniformly on rects, and the small square is the conventional shape; a
round dot was not worth a second code path.

**Outline.** `outline_width` px around every arm and the dot, computed as
`expand(all fills) − union(all fills)` — the stroke sits **strictly
outside** the fill and is never drawn underneath it, so a translucent line
shows the game through it, not the outline. *Why the union/difference
decomposition (`crosshair::Decompose`):* with a small gap the arms'
outlines overlap each other and the dot's, and at gap 0 the arms overlap
outright; drawing overlapping rects at a partial alpha leaves visibly
darker squares where two meet. Each element's rects are rebuilt as
non-overlapping bands instead, so every pixel of an element is painted
exactly once. Draw order: outline, arms, dot (the dot's colour wins where
it overlaps an arm).

## Auto-hide while holding right-click

`f = clamp((now − pressNs) / hide_time_ms, 0, 1)`; `hide_time_ms ≤ 0`
means `f = 1` at once (`crosshair::HideProgress`). Multipliers
(`crosshair::EvaluateHide`), applied before `Build()`:

| Mode | `f < 0.5` | `f ≥ 0.5` |
| --- | --- | --- |
| **Fade out** | `alpha = 1 − f` | (same, continuous) |
| **Focus** | `gap = 1 − 2f`, alpha 1 | gap 0, `alpha = 2 − 2f` |
| **Shrink** | `gap = 1 − 2f` | gap 0, `length = 2 − 2f`, **dot size × the same** |

`alpha` scales every element's opacity, outline included. In **Shrink**
the dot shrinks with the arms over the second half — a dot left behind
would defeat the point of hiding (the in-game scope has its own reticle).
At `f = 1` nothing is drawn.

**Release restores instantly** — there is deliberately no reverse
animation. *Why:* the moment the player comes off the sights they want the
crosshair back to re-acquire; an ease-in there is the one place a delay is
actually felt.

**Repaints.** `Crosshair_Draw()` returns "still animating" (`f < 1`), and
`FpsDisplay_AddLayer()` then calls `force_repaint()` for one more frame —
per frame, the way the HUD's lag-spike hold does, but at frame rate. Idle
or fully hidden, it asks for nothing. The press and release themselves
call `force_repaint()` too, so the animation starts (and the crosshair
comes back) on an idle game without waiting for it to commit.

### The right-click hook and its gating

`wlserver_dispatch_mouse_button()` (`src/wlserver.cpp`) is the single
fork between "the Shell/Launcher is capturing input" and "this goes to
the game". On the **game** branch only, after the seat notify, a
`BTN_RIGHT` press calls `Crosshair_NotifyRightButton(true)` and the
matching release `…(false)`. The event itself is neither consumed nor
altered — the game gets it exactly as before. Because releases are paired
to the press's destination by the existing tracking set
(`s_setMouseButtonsForwardedToGame`), a right button pressed in the game
and released after the Shell opened still restores the crosshair, and a
click captured by the Shell never starts a hide. The hook runs on the
wlserver thread and touches one atomic ("held since <ns>", 0 when not
held; only the first press starts the clock) plus `force_repaint()`; it
never reads the config cache. The render side reads the atomic.

## Verification

Nothing here needs OS-level input injection (banned — see
`AUTONOMOUS-DECISIONS.md` D4): `wlserver_debug_mouse_button` (a sibling of
`wlserver_debug_key` / `wlserver_debug_mouse_motion`, `src/wlserver.cpp`)
enters at `wlserver_mousebutton()`, the exact function every real backend
uses, so it reaches the capture gate, the game's seat and this hook like a
real click. Through `gamescopectl` the arguments are **one quoted
argument** (`273` is `BTN_RIGHT`):

```
gamescopectl wlserver_debug_mouse_button "273 1"     # press: hide starts
sleep 0.1
gamescopectl wlserver_debug_mouse_button "273 0"     # release: instant restore
```

Live checklist (the lead runs this on the laptop; the geometry tests cover
the arithmetic):

- Centre on the game rect: a letterboxed client (`-w 1280 -h 720 -W 1920
  -H 1080`, or any non-matching size) gets the crosshair on the *game*, not
  the output centre.
- 1px: width 1 is exactly one pixel, no anti-aliasing fringe, in a zoomed
  capture; with Apply Scaling on and a scaled game it is `round(scale)`
  pixels, still crisp.
- Apply Scaling: a 4:3 game on a 16:9 output with `--scaler stretch` (or
  any per-axis stretch) gives a horizontally stretched crosshair; off keeps
  it square.
- Hide modes at 50 % / 100 % of Time to hide: Fade half-transparent /
  gone; Focus gap closed at full opacity / gone; Shrink gap closed at full
  length / gone (dot included).
- Release: instant, no fade-in.
- No hide while the Shell is open (the right-click goes to the Shell, not
  the game); a press made in the game and released after opening the Shell
  still restores.
- Colours and opacity per element; the outline sits outside the fill and a
  translucent line shows the game, not black, through it.
- Inverted HUD text colour + crosshair: the digits still invert the game
  and the crosshair keeps its own colour (split mode).

## Known limitation (pre-existing, shared with the HUD)

The ImGui Vulkan backend blends `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` onto a
texture cleared to `(0,0,0,0)`, i.e. the HUD texture ends up
**premultiplied**, while the layer composites as
`ALPHA_BLENDING_MODE_COVERAGE` (straight alpha). A pixel at opacity `a`
therefore lands at `c·a·a + bg·(1−a)`, slightly darker and thinner than
`a` says. This is the HUD's existing behaviour (its backdrop at 0.5 has
always been affected the same way); the crosshair's opacity sliders
inherit it. Switching the layer to `ALPHA_BLENDING_MODE_PREMULTIPLIED`
would fix both but changes the HUD's look, so it is left for a deliberate
HUD-level decision.
