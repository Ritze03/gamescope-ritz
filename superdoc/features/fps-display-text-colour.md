# FPS display: text colour (Fixed vs. Inverted)

Fixed vs. Inverted text colour: history, root cause, what is not inverted, and the pixel verification recipe. Part of [fps-display](fps-display.md); sections below are moved verbatim from that page.

## Text colour: Fixed vs. Inverted

**Fixed** — the UI's own accent colour (`Palette.h`'s `kAccentValue`,
overridable by the "Number colour" row, `color_fps` — see "Number colour and
Text opacity" above; unexposed in the UI until 2026-09-07, though the field
and the render-side read of it existed since issue #29). On a detected lag
spike, the resolved colour is inverted
(`1 - r, 1 - g, 1 - b`) for the hold window — literally "just invert the
text colour", per the user's own spec.

**Inverted** — REDESIGNED 2026-09-27: a **single sample** of the game's own
colour, taken once per frame at the centre of the readout's own box and
inverted, used to colour the *whole* digit. From Phase 3 (2026-09-03) until
this redesign it was instead a **true per-pixel invert**, each glyph pixel
inverting the real composited colour immediately underneath it. See
[Root cause: why it "just stayed white"](#root-cause-why-it-just-stayed-white)
below for why this changed, and
[Verifying Inverted mode](#verifying-inverted-mode-pixel-recipe) for measured
values. Implemented one layer down in the Vulkan compute-composite shader
rather than in this file's own ImGui draw pass, same as before:

- This file just draws the digits as **plain opaque magenta**, `(255, 0,
  255)`, and hands the HUD's single layer a new blend mode,
  `rendervulkan.hpp`'s `ALPHA_BLENDING_MODE_INVERT` (`FpsDisplay_AddLayer()`).
  Opaque, not `text_opacity`-scaled — a partial alpha here would only dilute
  the invert. Magenta specifically: its **zero green channel** is the marker
  the shader uses to tell the digits apart from the outline and the
  crosshair, see
  [What Inverted mode does *not* invert](#what-inverted-mode-does-not-invert)
  (unchanged by this redesign). `FpsDisplay_AddLayer()` also computes the
  readout box's own centre (`DrawReadout()`'s return value, in output pixel
  space) and stores it in `FrameInfo_t::invertSamplePos`, alongside setting
  `bNeedsDestinationBlend` — a name left over from the old mechanism (see
  that field's own comment in `rendervulkan.hpp`); its job, forcing the
  frame through the full compute-composite path, is unchanged, since a
  plain compute dispatch over layer 0's own sampler still only happens
  there.
- The actual sample-and-invert happens in `src/shaders/alphamode.h`'s
  `g_hudInvertColor` — a plain global, set ONCE per invocation near the top
  of each composite `.comp` file's `main()` (before the layer loop that
  calls `BlendLayer()`), from a **single extra texel fetch** of layer 0's
  own sampler at `u_invertSamplePos` (the box centre, in the SAME uniform
  block as `u_scale`/`u_offset`/`u_alphaMode` — `blit_push_data.h`, plus a
  `u_hasInvertSample` flag so a frame with no Inverted-mode HUD layer pays
  nothing extra), then `InvertEncodedSample(clamp(sample, 0, 1))` — see the
  next bullet for what that function does and why. `BlendLayer()`'s
  `alpha_mode_invert` branch then just reads that one value for every digit
  pixel it colours, weighted by the digit coverage `d` the marker recovers
  (below): `outputValue.rgb = bg * (1 - layerAlpha) + g_hudInvertColor * d`.
  One function, one edit, but the SAMPLING differs per composite path,
  because each one reads layer 0 differently — see each `.comp` file's own
  comment next to where it sets `g_hudInvertColor`: the plain blit path
  reuses the exact same `sampleLayer(0, uv)` helper the real per-pixel read
  already uses (so it inherits layer 0's colourspace/colour-management/
  offset-scale handling for free, just at a different `uv`); both blur
  passes take one plain tap of the pre-blurred proxy texture instead of
  re-running the blur at the fixed point; RCAS mirrors its own non-RCAS
  layer-0 block (degamma, CTM, colour management) with a plain
  `texelFetch()` in place of the sharpen filter. Fully-transparent
  HUD-texture pixels (`layerAlpha == 0`) pass the background through
  completely unchanged, as before.
- **No contrast guard, on purpose — and inverted in ENCODED space, not
  linear (corrected 2026-09-27, same day).** The old per-pixel invert had
  a perceptual floor (2026-09-05, kept below in
  [History: the retired contrast guard](#history-the-retired-contrast-guard)
  for the record) that pushed a too-close inversion away from the
  background; this redesign has none, on the user's own ask: *"It should
  capture a single pixel of the color below it and just invert it and use
  that color ... it's more like an OLED thingy."* The FIRST cut of this
  redesign kept the pre-existing blend's **linear-light** invert
  (`BlendLayer()` runs after `apply_layer_color_mgmt()`, in linear-light
  blend space) with the guard simply removed — which measured correctly
  as "an honest invert" but turned out to be the wrong honest invert for
  what the user meant: a linear invert of a DARK-to-MID scene (most of
  CS2, most games) lands very close to WHITE (encoded 51 → linear invert
  → encoded ~251; encoded 148 → ~218), reproducing the exact "it just
  stays white" complaint the whole redesign exists to fix, just for a
  different reason than the per-pixel invert's own bugs. "Just invert it"
  in the user's own, everyday sense means inverting the pixel **as seen on
  screen** — plain `255 − x` on each encoded channel — which requires
  encoding the linear sample first, inverting THAT, then decoding back to
  linear for `BlendLayer()`'s own blend space (`alphamode.h`'s
  `InvertEncodedSample()`): encoded 51 → 204, encoded 148 → 107, a
  per-channel colour (64,128,192) → (191,127,63) — exact `255 − x` per
  channel for SDR output, measured and matched exactly (see
  [Verifying Inverted mode](#verifying-inverted-mode-pixel-recipe)). Under
  HDR/PQ the sampled colour is clamped to `[0, 1]` before this whole
  encode/invert/decode round-trip, same reasoning as before: an
  out-of-range HDR value could otherwise hand either transfer-function
  call a negative or wildly out-of-range result.
- **What it inverts against**: no longer "whatever has already been folded
  into `outputValue`" (the old per-pixel invert's own destination read) —
  the single sample is layer 0 alone (the game, plus the Shaders effects
  pre-pass and Zoom when either is active, since both substitute their
  output for layer 0 before the composite shaders run — see
  [compositing-vulkan.md](compositing-vulkan.md)). It does not see the
  cursor, mura correction, other overlay layers, or the Shell, all of
  which push *after* the HUD (see
  [compositing-vulkan.md](compositing-vulkan.md#layer-order-zpos)'s
  push-order note) — narrower than the old per-pixel invert, which (being
  a read of the accumulated destination) picked up the cursor and mura
  correction too. Not expected to matter in practice: none of those
  layers are likely to sit under the FPS readout's own corner.

### History: the retired contrast guard

Kept for the record; none of this applies since the 2026-09-27 redesign
above, which has no contrast guard at all.

A literal invert can land too close to the background to read. The
2026-09-05 guard judged "too close" in **encoded (sRGB) space**: it encoded
both the background and its true invert (`linearToSrgb`), took each one's
Rec.709 luma on the encoded channels (`0.2126/0.7152/0.0722`, i.e. Y'), and
required `|Y'(inverted) − Y'(bg)| ≥ 0.40` (≈100/255). Where the true invert
already cleared that, it was left exactly as it was; where it did not, the
inverted colour was moved uniformly in every channel to sit exactly 0.40
from the background's Y' on the side *away* from the background — below it
when the background was brighter than perceptual mid grey (`Y' > 0.5`),
above it otherwise. 0.40 was not arbitrary: at `Y'(bg) = 0.5` (encoded 128)
the true invert's Y' is ~0.896, a gap of ~0.40, i.e. precisely what a true
invert gives at perceptual mid grey.

> **Why perceptual, and why not the linear rule it replaced.** The guard
> was originally (2026-09-03) computed in *linear* light:
> `|(1 − bgLuma) − bgLuma| ≥ kMinLumaSeparation`, floor 0.25 then raised
> the same day to 0.40 from a single on-screen check at encoded 188 (188
> inverted to a marginal grey-on-grey 138 at 0.25, to a readable 91 at
> 0.40). That rule only engaged for **linear** luma in `(0.30, 0.70)` —
> encoded ~149–225 — and the linear invert of the mid-tones that dominate
> real game scenes landed just outside it: encoded 148 is linear 0.30, its
> true invert linear 0.70 encodes to ~218, and the guard, seeing a linear
> gap of 0.40, did nothing. The result over encoded ~120–190 was a faint
> light grey at ~215–225 that reads as "white text that ignores the
> background" — reported 2026-09-05 as "the colour inversion doesn't work
> anymore". A bisect found no regression (identical pixels at `0ff8a55`
> and HEAD); the defect had been there since the 0.40 floor. Linear-light
> separation is simply not what the eye judges.

### Root cause: why it "just stayed white"

The user's report, verbatim: *"for the HUD, the inverted color mode for the
FPS display doesn't work at the moment, it just stays white. It should
capture a single pixel of the color below it and just invert it and use
that color so it constantly changes. it's more like an OLED thingy."* Told
that the code already did a per-pixel invert, the user's own answer was
still *"Single pixel. Nothing except for shaders is on, but turning them
off doesnt fix it."*

Code review (2026-09-27) of the old per-pixel path found its maths intact
and unchanged since the 2026-09-09 fix (`git log` on `alphamode.h` and
`FpsDisplay.cpp` shows nothing touched either file between then and this
redesign), and every backend correctly forced the frame through the full
compute-composite path whenever the HUD's texture was present (none of
them have a backend `Fb` to scan out directly, so `bNeedsFullComposite`
always ends up true). Two things WOULD independently have produced exactly
"stays white, regardless of the game or of the Shaders toggle," though
neither could be confirmed as the one the user actually hit without
reproducing their live CS2 + Hyprland + nested-Wayland session, which the
verification rules for this task ruled out disturbing:

1. **A genuine ordering bug in `cs_composite_blur_cond.comp`** (the
   `BLUR_MODE_COND` composite path — Steam's own overlay/QAM blur, `g_BlurMode`
   in `steamcompmgr.cpp`, distinct from the Shell's own background blur,
   which uses the *other* blur shader and orders layer 0 first correctly).
   That shader blends every layer at or above `c_blur_layer_count` — where
   the HUD always sits — in a loop that runs **before** layer 0 is folded
   into `outputValue` at all (folded in afterwards, in one of two branches
   depending on how much the overlay layers already cover a given pixel).
   The old per-pixel invert read `outputValue` at exactly that point, saw
   pure black (nothing had been blended in yet), and inverted black to
   white — independent of the game's actual colour and of any Shaders
   setting, since this path is selected by the blur mode alone. Not
   reproduced live; found by reading the shader's control flow.
2. **A locally-uniform patch of game content under the readout.** Even
   with the old maths working exactly as designed, a per-pixel invert of a
   background that is itself uniform under the whole readout box (a dark
   HUD corner, a loading-screen letterbox bar, a black scoreboard panel —
   plausible at a typical top-left or top-right FPS anchor) produces a
   uniform inverted colour by construction. That is not a bug in the old
   mechanism, but it looks exactly like "stays the same colour" to a
   player whose corner of the screen happens to be visually static.
3. **A genuine bug in the FIRST cut of this very redesign** (found and
   fixed the same day, before the user ever saw it, via this doc's own
   measured table): the first cut kept the old mechanism's **linear-light**
   invert, just with the destination-read replaced by the single sample.
   Measured live, that made the symptom WORSE, not better — a linear
   invert of a dark-to-mid scene (most of CS2, most games) lands very
   close to white (encoded 51 → ~251, encoded 148 → ~218), which is the
   "always white" complaint all over again, from a third, unrelated cause.
   Inverting in **encoded** space instead (see the "No contrast guard"
   bullet above) gives the plain `255 − x` per channel a player actually
   means by "invert the pixel". This is the version shipped.

This redesign (single sample of the box centre, inverted in encoded space)
structurally rules out causes (1) and (2): it never reads the composited
destination at all, so cause (1) cannot reproduce through it, and by
sampling once and inverting rather than inverting many independent
per-pixel local backgrounds, it changes exactly when the ONE sampled point
changes — which is also just a more literal reading of what the user
actually asked for. Cause (3) was this redesign's own, and is fixed by the
encoded-space correction above. See
[Verifying Inverted mode](#verifying-inverted-mode-pixel-recipe) for the
measurements confirming the shipped behaviour tracks the sampled
background, not a fixed colour, and matches plain `255 − bg`.

**Inverted mode has no lag-spike indication (2026-09-09).** It can't
"invert" already-inverted text to signal a spike — doing that would show
nothing against itself — so its reaction used to be a muted warning red
tinted into the backdrop, and only when one was actually drawn. With the
backdrop gone there is nothing left to tint, so a spike does nothing in
this mode; "Lag spike detection" is a Fixed-mode feature now, which its
help text says. That is the second thing removing the backdrop cost (see
[Backdrop: removed](#backdrop-removed)), and the same trade the setting
already made at opacity 0 since 2026-09-03.

### What Inverted mode does *not* invert

Only the **digits' fill**. The outline stays black and is not inverted
(it was, until 2026-09-03), and neither is the
[crosshair](crosshair.md), in whatever colour and opacity it was given.
(The backdrop was a third such element until it was removed, 2026-09-09.)

All of it lives in **one** composite layer, and the shader tells the
digits apart from everything else by a **marker in the texel** (2026-09-06):

- The digits' fill is drawn pure opaque **magenta**, `(255, 0, 255)`, and
  everything that may end up *under a digit's antialiased edge* is pure
  **black**: the outline stamps and the cleared texture. Removing the
  backdrop (2026-09-09) took one such element away — it was drawn pure
  black in this mode, with a green-free spike tint, precisely to satisfy
  this rule — so it can only have made the marker safer, never weaker:
  the set of things a digit's edge can mix with shrank, and everything
  left in it still has `G == 0`. Re-measured after the removal; see
  [Verifying Inverted mode](#verifying-inverted-mode-pixel-recipe).
- ImGui's straight-alpha blend over black or clear therefore leaves a digit
  texel as `d · (1, 0, 1) + (a − d) · black` with **G exactly 0**, where
  `d` is the digit's own coverage and `a` the texel's alpha. So in
  `alphamode.h`'s invert branch: `G == 0` ⇒ digit; the encoded R *is* `d`;
  the pixel becomes `bg · (1 − a) + inverted · d`, the black share
  contributing nothing. That reconstructs the layering **exactly** —
  an edge over the outline is a clean ramp from black to the inverted
  colour (measured: `0, 22, 45, 46` across a stroke over encoded 148 with
  a 2 px outline, back when a 50 % backdrop sat under it too), a full digit texel is exactly the
  inverted colour, and an outline-only texel is exactly the coverage
  blend of black.
- `G > 0` ⇒ not a digit; the texel blends **bit-for-bit as
  `alpha_mode_coverage`** would. That is the crosshair (and, until
  2026-09-09, the backdrop's 12 %-white border). A crosshair colour with no green at all
  (pure red, blue, magenta, black) is nudged from `G = 0` to `G = 1` while
  it shares an Inverted layer (`CrosshairFrame::bReserveInvertMarker`,
  `Crosshair.cpp`'s `ReserveInvertMarker()`) — one count, not visible —
  and every other colour is used exactly as configured.
- **16 bits per channel for that one pairing.** The nudge is stored
  premultiplied, and `1/255 × opacity` rounds back to 0 in an 8-bit
  texture below 50 % opacity — a faint red crosshair would invert. So
  `ResolveTextureFormat()` makes the HUD texture `R16G16B16A16_UNORM`
  exactly when the readout is Inverted **and** the crosshair is on (the
  marker keeps `G ≥ 1/514 × opacity` there, i.e. down to ~0.4 % opacity,
  below which the crosshair adds under half a count anyway), and
  `B8G8R8A8_UNORM` in every other configuration, which therefore pays
  nothing. ImGui's pipeline names its attachment format, so `EnsureTexture()`
  rebuilds it (`ImGui_ImplVulkan_CreateMainPipeline`) on a switch, after
  the previous submission has drained. The bytes in that configuration
  equal what the retired double-height 8-bit texture used.

> **Why a marker, and why this one.** The selector before it (2026-09-03 to
> 2026-09-06) was the texel's own *brightness*: `smoothstep(0.25, 0.80,
> luma)` on white digits, with the backdrop and outline dark. It could
> not tell a white digit from a white crosshair, so the crosshair had to
> leave the layer — the "split mode" below — and that cost a layer slot.
> Any per-texel encoding has to survive ImGui's over-blend, which keeps
> `rgb ≤ a` and mixes the digit's edge with whatever is under it; a
> brightness or ratio test therefore cannot separate a digit's edge over
> the black outline from a grey crosshair of that same value (both are
> `(v, v, v, 1)`), and an 8-bit colour restriction of ±2 counts on the
> crosshair drowns in premultiplied quantisation below ~50 % opacity.
> What *does* survive mixing with black is a **zero channel**: `0 · cov +
> 0 · (1 − cov)` is 0. Hence "G == 0 marks a digit", black for everything
> under a digit, `G ≥ 1` for everything else, and 16 bits so `G ≥ 1`
> stays non-zero after premultiplication. Magenta rather than red so the
> marker reads as a marker (the same `(1, 0, 1)` `composite.h` uses for
> its plane-border debug view) if the texture is ever dumped; the shader
> reads only R and G.
>
> **Why one layer and not two.** This was briefly split across two
> layers — a normally blended backdrop + outline layer with an
> invert-blended glyph-only layer above it — and that **broke Inverted
> mode outright** the same day (2026-09-03): with `backdrop_opacity` at
> its 0.5 default, the lower layer painted a dark box over the game
> exactly where the digits were about to land, so the upper layer
> inverted *the HUD's own backdrop* instead of the game and the digits
> came out a constant near-white. The general rule the split violated:
> **a blend that reads the destination cannot be split across two layers
> that overlap.** Whatever the lower one paints becomes the "background"
> the upper one inverts.

**Known limitation — the readout drawn *on top of* the crosshair.** The
marker only survives mixing with *black*. If the readout is anchored so
its digits overlap the crosshair (anchor `center`, small margins — the
crosshair sits at the game rect's centre), a digit's antialiased edge
pixels that land on a green/coloured arm pick up that arm's `G > 0`,
are classified "not a digit", and composite as their own magenta-tinted
colour: a few magenta fringe pixels along the digit's edge where it
crosses the arm (measured 2026-09-06 on encoded 148 with the default green
crosshair: 35 such pixels in the glyph pair,
`build-release/verify-shots/split-retire/zoom/overlap-mid.png`). The
digit's interior over the arm still takes `g_hudInvertColor` correctly; the crosshair everywhere
else is untouched. The split mode rendered this configuration cleanly
(separate layers), so this is the trade for the layer slot — accepted
because an FPS number placed on the crosshair is not a configuration
anyone runs on purpose, and every other anchor never overlaps it.
Drawing the crosshair *after* the readout in this mode would move the
fringe to a translucent arm over a digit instead; left as is.

**Layer budget.** The HUD is **one layer in every configuration**, so it
has no special layer-budget behaviour: `k_nMaxLayers` / `VKR_MAX_LAYERS`
are both 6, `LayerStack_t::push()` returns `nullptr` when the frame is
full (and, since 2026-09-06, counts the drop for `layer_budget_stats`),
and the HUD then draws nothing that frame with a rate-limited warning.
`paint_all()` reserves slots for the cursor and the overlay planes but
**not** for the HUD, the toasts or the Shell, so a busy frame (base +
override + external overlay + Steam overlay + cursor + mura is already
six) can lose them.

**The crosshair shares this layer (2026-09-05).** The
[crosshair](crosshair.md) is drawn into this same ImGui frame and texture,
before the readout, and rides the same `Layer_t` — the layer budget above
is exactly why it has no layer of its own. Two consequences for this file:
`FpsDisplay_AddLayer()` builds the layer when the readout **or** the
crosshair is on (and still nothing at all when neither is), and the 500 ms
repaint-timer flag is recomputed (`UpdateTimerFlag()`) from that same
predicate. See
[crosshair.md](crosshair.md#where-it-is-drawn-the-huds-layer).

> **The retired split mode (2026-09-05 → 2026-09-06).** With Inverted text
> colour *and* the crosshair on, the brightness selector of the time would
> have inverted a bright crosshair, so the texture was rendered at twice
> the output height — readout top, crosshair bottom — and two `Layer_t`s
> sampled the two halves (INVERT then COVERAGE, the second's `offset.y`
> the output height). That was one of the six layer slots spent on a HUD:
> measured with both on from startup, the frame's high-water mark was 5
> layers on the split build and is 4 now (`scripts/pixel-regression.sh`'s
> `layer-budget` check pins the 4). The marker above made it unnecessary.

**Interaction with the outline.** None, now. The outline is black, the
selector leaves black alone, and every digit pixel takes the single sampled
colour (`g_hudInvertColor`) regardless of how thick the outline is.

### Verifying Inverted mode (pixel recipe)

**Re-verified 2026-09-27 for the single-sample, encoded-space redesign**
(headless: a private, invisible sway hosting a nested `--backend wayland`
gamescope, no laptop round trip — the same recipe
`scripts/pixel-regression.sh` automates, see that script's own header for
why this is how pixel truth is captured in this sandbox). Config:
`fps_display.color_mode = "inverted"`, `outline_strength 0`, `font_size
48`, anchor `top-left`, margin 24, `fps_display_force 60`, crosshair off.
A flat-colour `kitty` client (`-o background=X -o foreground=X -o
cursor=X`, matching `pixel-regression.sh`'s own substitute for `xterm`,
which is not installed on this sandbox) stands in for the game, one solid
colour at a time:

```
gamescope --backend wayland -w 1280 -h 720 -W 1280 -H 720 \
    --scaler auto --force-windows-fullscreen -- \
    kitty -c NONE -o background='#949494' -o foreground='#949494' \
        -o cursor='#949494' sleep 600
gamescopectl screenshot "/path/shot.png 4"      # one quoted argument
```

then sample the digit core (the most common non-background colour inside
the digit box) and compare it to the box centre's own background colour,
inverted in **ENCODED** space — plain `255 − bg` per channel for SDR (see
the "No contrast guard ... encoded, not linear" bullet above for why this
replaced a linear-light invert within the same day). Three backgrounds,
captured under `build-release/verify-shots/hud-invert-2026-09-27/`:

| background (encoded) | digit core (measured) | expected (`255 − bg`) |
|---|---|---|
| `(51,51,51)` `#333333` | `(204,204,204)`, n=440 | `(204,204,204)` |
| `(148,148,148)` `#949494` | `(107,107,107)`, n=450 | `(107,107,107)` |
| `(64,128,192)` `#4080C0` | `(191,127,63)`, n=439 | `(191,127,63)` |

All three landed **exactly** on plain `255 − bg` (tol ±3; no slack was
needed in practice). Four things this confirms at once: the digit is no
longer stuck white regardless of background (the reported bug); the
retired contrast guard is genuinely gone (`148` gives `107`, not the old
guard's pushed `46`, and not even the redesign's own short-lived
linear-invert `218` — see
[History: the retired contrast guard](#history-the-retired-contrast-guard)
and [Root cause](#root-cause-why-it-just-stayed-white)'s cause 3); the
per-channel colour case (`#4080C0`) shows the sample preserves hue, not
just luma; and every dark-to-mid background now reads as a genuinely
different, non-white colour, the specific thing a linear invert got wrong
for real gameplay. A **second background swap mid-session** (`#333333` →
`#949494` → `#4080C0`, one nested gamescope instance per colour) is the
"it constantly changes" half of the user's ask: the digit colour tracked
each new background's inversion across every relaunch. `n=` is the pixel
count that matched the fill colour exactly inside the digit's own box (out
of ~600 box pixels); the rest are antialiased edge pixels blending between
that one fill colour and the background, consistent with ONE sampled
colour for the whole digit rather than a spatially-varying per-pixel
invert (which a flat single-colour test client cannot itself distinguish
from single-sample, since the local background is uniform everywhere under
the box either way — the distinction is structural, see
[Root cause](#root-cause-why-it-just-stayed-white), not something a flat
test can show on its own).

Fixed mode, sampled the same way over `#333333` with `color_fps = 0x40C0FF`:
measured `(64, 192, 255)` against the configured `(64, 192, 255)` exactly —
unaffected by this redesign, as expected (Fixed mode never touches
`alphamode.h`'s invert branch).

**`scripts/pixel-regression.sh`'s own Inverted-mode checks** (`inversion`,
`inversion-midtone`, `inversion-crosshair`, `inversion-crosshair-alpha`)
were updated the same day for this redesign: `EXP_DARK_*` 51→204 (was the
retired guard's 251), `EXP_MID_*` 107 (was 46), `MID_TOL` loosened only to
4 (was 20 — that slack was specific to the retired guard's push maths,
which no longer runs), and the old `MIN_ENCODED_SEPARATION` floor is now
`DIGIT_MIN_GAP=0` (a documented no-op — asserting a minimum gap would be
re-imposing a guard that no longer exists). Re-run headlessly (this
task's own rules: private sway, isolated `XDG_*` dirs, this session's own
gamescope socket, `nice -n 19 ionice -c 3`) via `--only <check>` per
check: `inversion`, `inversion-midtone`, `inversion-crosshair-digit`,
`inversion-crosshair-alpha-digit`, `fixed-color` and `layer-budget` all
PASS with the numbers above. One unrelated, pre-existing failure surfaced
incidentally: `inversion-crosshair-outline-black` (the crosshair's OWN
outline colour/position check, nothing to do with the HUD digit's invert
colour) — `git diff` between this redesign's two commits and the base
they started from shows `Crosshair.cpp`, `Crosshair.h` and this script's
crosshair constants byte-identical throughout, so this predates and is
unrelated to this change; not investigated further here (crosshair
geometry is outside this task's scope), flagged for a separate look.

