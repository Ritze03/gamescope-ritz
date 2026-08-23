# Feasibility — A3 "BLOOM" against immediate-mode ImGui

Target: Dear ImGui **1.92.9b, stock non-docking branch** — confirmed as the pinned
revision in `subprojects/imgui.wrap` (`f1cc2ae`, tag v1.92.9b). Everything below was
checked against that tree, not from memory. (`src/reshade/deps/imgui/` is a *separate*
vendored 1.90.4 for ReShade and is not the overlay's ImGui — worth knowing before someone
greps the wrong header.)

**Verdict.** The navigation model is *cheaper* than A's, which was already cheaper than
what ships. Two ideas carry real risk: **Peek** (§4.1) and the **`HudGhost` preview**
(§4.2). Both have designed-in fallbacks that leave the rest of the design intact. One
idea is a genuine one-frame-latency nuisance that has to be tested rather than reasoned
about: **the bloom's hit ordering** (§3.1).

---

## 1. Cheaper than A

BLOOM deletes mechanisms A still had to build. Each of these is work that does not happen:

| A had to build | BLOOM |
|---|---|
| a sub-tab bar per screen | a group heading (`AddText` + `AddLine`) and a rail row |
| a three-level depth stack with push/pop state | one level; no stack |
| a breadcrumb, depth pips, back semantics | none |
| stage push/pop transition (A §2.3 — and A already conceded it had to fake it) | none |
| **Sheets**: a modal, a scrim, focus trapping, a slide-in, a "sheets never stack" rule | none — the bloom *is* the sheet, in place |
| a lazily-built Find index with the admission that unvisited screens aren't findable | the registry is complete at startup; nothing is lazy |
| distinct hover-vs-focus treatments (because a pad and a mouse could both be live) | one state; gamepad is out of scope |
| a toggle-switch control kind (theme, hit-test, animation, disabled variant) | deleted; a bool is a two-cell `Choice` |
| 21 registered screens | 6 sections of declarations |

Net: **five fewer mechanisms and one fewer control kind**, before any of BLOOM's own
additions are counted.

---

## 2. Easy — stock ImGui, or a straight `ImDrawList` paint

| Thing | Why |
|---|---|
| **One fixed panel** | `SetNextWindowPos/Size` + `NoDecoration\|NoMove\|NoResize\|NoSavedSettings`. Deletes `Chrome.cpp`'s hand-rolled title-bar drag, collapse, close, Z-order and tiled defaults. |
| **Header / rail / legend bands** | fixed-height bands in one window, manual cursor placement. `Chrome.cpp` already paints harder chrome than this. |
| **The rest row** | `ItemSize`/`ItemAdd`/`ButtonBehavior` over the full row rect, then two `AddText` and one `AddLine`. This is exactly what `Widgets.cpp`'s `Toggle()` already does; the item just becomes the row. |
| **Clipping the reel** | `ImGuiListClipper` with a **uniform** row height, guaranteed forever by §3.1's overlay decision. `IncludeItemByIndex()` (present in 1.92.9b) pins the focused row so a bloomed entry is never clipped away mid-drag. |
| **Segmented bank / option list / chips / stepper / action** | `ButtonBehavior` per cell + `AddRectFilled`/`AddText`. All exist today in `Widgets.cpp` in some form. |
| **The slider instrument** | `SliderBehavior()` on the track rect, drawn by hand. It is today's `SliderControl()` with a *bigger* rect, which is strictly easier. The `GrabMinSize`-vs-drawn-handle invariant from `slider-widget-spec.md` §3 carries over unchanged. |
| **The bloom drawn above the rows** | `ImDrawListSplitter` — present, stock, and exactly the primitive for "emit these commands into a later channel". Two channels. No second window, no `SetNextWindowPos` gymnastics, no Z-order bookkeeping. |
| **Rail collapse** | one `Approach()`ed float driving the label column's width, with the label alpha ramped by the same factor. The icon gutter is a constant, so nothing can shift (SPEC §1.5). |
| **The 5 animation floats** | one `Approach()` helper against `io.DeltaTime`. Nothing retained, nothing ID-keyed. |
| **Command palette** | `OpenPopup` + `BeginPopupModal` with `NoTitleBar\|NoResize\|NoMove\|NoBackground`, painted by us. Stock modal gives focus trapping and Esc for free — the same recipe A used for its sheets, needed exactly once instead of five times. |
| **Fuzzy match over ~60 entries** | subsequence scorer, recomputed only on query change. Microseconds. |
| **The contrast guard** | two `std::clamp` expressions in `Theme.cpp` (`API.md` §4.1). |
| **`ui.contrast` inspector** | the composited panel colour is known (game average × darkening × alpha); the ratios are arithmetic. The mockup ships a working implementation of exactly this. |
| **0.5×–2.0× scaling** | everything derives from `u` and `base`; `Metrics` centralises what the current code repeats per file. Strictly less code. |

---

## 3. Medium — has a recipe, needs QA

### 3.1 The bloom's hit ordering — the nuisance

**What the design needs:** the bloom draws over rows; only its *instrument* takes the
pointer; the rows underneath keep deciding what is hovered (SPEC §3.2 — this is what
kills accordion oscillation).

**How ImGui does it:** hit-testing is by submission order within a window; a later item
overlapping an earlier one takes `HoveredId`, but **only if the earlier item opted in**
via `SetNextItemAllowOverlap()`. So: `SetNextItemAllowOverlap()` on every rest row, then
submit the instrument after the row loop. Draw order is handled separately by the
splitter. The two orderings are independent, which is exactly what we want.

**The catch, stated in ImGui's own header:** *"Require previous frame HoveredId to match
before being usable."* That means one frame of latency at the moment the pointer crosses
into the instrument's rect. At 165 Hz nobody notices; at 30 Hz in a heavy scene it could
read as a missed click on the very first press. **Mitigation:** the instrument's rect is
static for the whole time an entry is bloomed, so the "previous frame" condition is
satisfied from the second frame of the bloom onward — and the bloom animates in over
~4 frames anyway. **Budget half a day and QA it at a capped 30 fps specifically.**

**Residual risk: low-medium.** If it turns out ugly, the fallback is to hit-test the row
list ourselves against the pointer y (we already own the row rects) and submit rows as
`InvisibleButton`s only where the bloom does not cover them. That is ~20 lines and removes
the dependency on `AllowOverlap` entirely.

### 3.2 `←→` adjusts the bloomed row without activating it

Same fight A identified, same recipe (`API.md` §4.4), and easier here: with the bloom
being the only horizontally-interested item in the window, ImGui's own horizontal nav has
nothing to move to. `NavMoveRequestCancel()` is confirmed present in `imgui_internal.h`
(line 3545). QA specifically at the top and bottom of a scrolled reel, where ImGui tries a
wrap move.

**Risk: low.**

### 3.3 The big value's font size

The bloom's value is `1.85 × base` and the rest row's is `0.90 × base`. Animating between
them would be pretty; 1.92 makes it *possible* (`PushFont(font, size)` with an arbitrary
size — confirmed at `imgui.h:523`) but each distinct size is a glyph-cache entry, so a
smooth lerp during a bloom transition churns the atlas.

**Decision: do not animate the size.** The value snaps to its bloomed size on the frame
the bloom opens. This is consistent with SPEC §3.4's "values never animate" and costs
nothing. Flagged so nobody later "improves" it into a per-frame rebake.

The five type roles bake ~7 faces (Sans 400, Sans 500, Mono 400, Mono 500, Mono 600 at two
sizes, Mono 600 large). Down from today's ten. `fonts::RebuildAll` + `ApplyPendingRebuild`
(issue #51) is unchanged and already works.

### 3.4 Preview strips

Seven of the eight are pure `ImDrawList` and cheap:

| Preview | Implementation | Cost |
|---|---|---|
| `LuminanceRamp` | ~24 `AddRectFilledMultiColor` quads through the actual curve | trivial |
| `Saturation` | same, through the actual gain in OKLCH | trivial |
| `TextSample` | `PushFont(font, s.v)` and one `AddText` — this is the one place 1.92's dynamic sizing genuinely earns its keep, because the HUD's font size is a live slider | one cache entry per distinct size the user lands on; bounded by the 8–48 px integer range = 41 entries worst case. Acceptable; measure. |
| `FrametimeRuler` | 40 `AddRectFilled` from the frametime ring buffer that already exists in `Metrics/SystemStats.h` | trivial |
| `AudioMeters` | 48 `AddRectFilled` from the existing peak values | trivial |
| `AccentSweep` | 24 quads across the OKLCH hue circle | trivial |
| `Sharpen` | **needs a texture.** See below. | medium |
| `HudGhost` | **the hard one.** See §4.2. | risky |

**`Sharpen` honestly:** a true before/after needs a real image patch put through the real
RCAS pass. That means a small offscreen render target and a compute dispatch per frame the
preview is visible — buildable (the shader exists) but it is a rendering feature, not a UI
feature. **Ship the cheap version first:** a procedurally-drawn high-frequency test patch
(`ImDrawList` line hatch + a gradient) with the *approximation* of the kernel applied by
choosing between three pre-drawn hatch densities. It demonstrates *what sharpening does*
and *which direction the slider goes*, and the Well header says so. Upgrade to a real
patch later if anyone asks. Same honesty rule A applied to its HUD preview: **label the
approximation rather than imply it is exact.**

### 3.5 Inline text edit

`InputText` with `AutoSelectAll`, sized to the bloom's field rect, `FrameBg` pushed
transparent and our own rect painted behind. `SetKeyboardFocusHere()` on activation.
There are two text fields in the whole product. The IME path (`input-method-ime.md`) is
worth one check.

**Risk: low.**

---

## 4. The two ambitious things that might not survive the renderer

### 4.1 PEEK — reaching into the compositor

**What it needs:** while a control is held, ramp `FrameInfo_t::blurRadius` and the base
layer's darkening CTM toward zero, and ramp the overlay's own alpha down.

**The good news, verified in the tree.** `SettingsOverlay.cpp` already does exactly this
shape. `SettingsOverlay_AddLayer()` computes, per frame:

```cpp
const int nOverlayBlurRadius = std::clamp(
    (int)std::lround( k_nMaxOverlayBlurRadius * g_BackgroundLiveTheme.flBlur * s_flCurrentAlpha ),
    0, k_nMaxOverlayBlurRadius );
...
const float flDarkenStrength = std::clamp(
    g_BackgroundLiveTheme.flDarkening * s_flCurrentAlpha, 0.0f, 1.0f );
```

Both are already **animated per frame** by `s_flCurrentAlpha` (the fade-in ramp), and both
already have a "0 means don't request the pass at all" branch. Peek is literally one more
multiplicand — `* (1.0f - flPeek)` — in each expression. **That part is two lines.**

**Four honest problems:**

1. **The FSR/NIS pop at the bottom of the ramp.** The blur request forcibly clears
   `useFSRLayer0` / `useNISLayer0`, because `vulkan_composite()` picks exactly one of
   {FSR, NIS, blur, blit} per call. When the peek ramp drives `nOverlayBlurRadius` to 0,
   the blur branch stops being taken and **FSR comes back on**, changing the game layer's
   sharpness in a single frame, mid-ramp. On a Steam Deck running FSR that is a visible
   pop at exactly the moment the user is judging sharpness — i.e. the worst possible
   moment. **Mitigation:** hold `useFSRLayer0 = false` for the whole duration of a peek
   (including at radius 0) so the filter does not change under the user; the game is
   bilinear-upscaled for the ~200 ms of the peek. That is a *different* small visual
   change, and it is the honest one to make, because it is constant across the peek rather
   than snapping mid-ramp. **This needs an eyeball, not an argument.**

2. **Blur radius is an integer 0..11.** The ramp is 12 steps, not continuous. Precedent
   exists (`s_flCurrentAlpha` already ramps it the same way), so it is presumably
   acceptable, but a fast peek ramp is a harsher test than a fade-in.

3. **The darkening CTM blob.** `GetDarkeningCtmBlob()` caches and rebuilds when strength
   moves by more than 1/512 — on the DRM backend that is a real
   `drmModeCreatePropertyBlob()` ioctl plus a free. A ~110 ms ramp at 165 Hz is ~18
   rebuilds per peek edge, ×2 edges. Small, and only at the edges (mid-drag, peek is
   pinned at 1.0 and strength is 0, which takes the "no ctm at all" branch), but it is
   ioctl traffic driven by pointer input, which deserves a note. **If it measures badly:
   quantise the peek ramp to 8 steps.** Also inherits the existing caveat that darkening
   silently does nothing when `layers[0]` already carries a CTM (HDR wide-gamut, mura) —
   in that case peek only lifts the blur, which is still most of the effect.

4. **The game has to still be rendering.** Peek is worthless if the game pauses on focus
   loss. Many do not; some do. This does not break anything — the peek just shows a
   frozen frame — but it means the feature's value varies per title, which should be said
   out loud rather than discovered.

**Verdict:** Peek is the boldest idea here and it is **the cheapest bold idea in the
document** — two multiplicands in code that already exists. Problem 1 is the one that
could sink it. **Spike it in an afternoon behind `overlay.peek 1`, default off, and judge
it on a Deck with FSR on.** If it loses, the rest of the design is untouched: peek is five
lines in the shell and nothing else depends on it.

### 4.2 `HudGhost` — the same wall A hit, but smaller

A's `FEASIBILITY.md` §5 is right and applies here: `FpsDisplay.cpp` owns its own ImGui
context, atlases are per-context, and previewing the real HUD means refactoring 2,469
lines of module draws into `Draw(ImDrawList*, ImVec2 origin, float flScale)`.

**BLOOM needs less of it than A did.** A's design put a live HUD preview in a permanent
Well on the System Monitor screen — a first-class, always-visible piece of the layout. In
BLOOM the ghost is a *preview inside one bloom*, on nine of ~60 entries. So:

- **Ship the schematic ghost from day one.** A rounded plate at the measured footprint
  (`MeasureFpsModule()` and siblings already exist), with the real backdrop opacity,
  padding, rounding and text opacity applied, and placeholder module text drawn in the
  console's own Mono at the nearest available size. That demonstrates placement, plate
  geometry, margins and which modules are on — which is what all nine of those settings
  actually adjust.
- **Label it.** The mockup says *"ghost drawn at approximate scale"* in the bloom. Keep
  that string. A preview that quietly disagrees with the real HUD is worse than no
  preview.
- **The real-HUD refactor is an upgrade, not a prerequisite**, and if Peek ships, it is
  also much less necessary: with peek, adjusting HUD placement shows you *the actual HUD
  on the actual game*, live, because the real HUD is still being composited. **Peek makes
  the hardest preview in the product optional.** That is the strongest argument for
  spending the risk budget on Peek rather than on the `FpsDisplay` refactor.

**"A patch of the actual frame" is still not available** and BLOOM does not ask for it —
punching an unblurred rect out of the compositor blur pass is not something the pass
supports. Peek achieves the same goal by lifting the blur globally, which the pass *does*
support.

---

## 5. What fights immediate mode — flagged explicitly

| Item | The fight | Resolution |
|---|---|---|
| A bloom that expands the list | variable row height breaks `ImGuiListClipper` | it doesn't expand — it overlays (SPEC §3.1). The design constraint and the implementation constraint agree, which is why this is the right shape. |
| Draw-above but hit-below | ImGui couples draw order and hit order by default | `ImDrawListSplitter` for draw, `SetNextItemAllowOverlap` for hit. One frame of latency (§3.1). |
| A palette that adjusts an entry that isn't being drawn | immediate mode has no retained widget tree | the registry is a declaration store, not a widget tree. This is precisely why the API is declarative (`API.md` §1). |
| `EnabledWhen` predicates that read other sections | an imperative kit only knows the current screen | same answer; predicates are `std::function` evaluated at draw time against globals/ConVars, exactly as today's `if (!s.enabled)` does. |
| Per-widget animation | would need an ID-keyed persistent store | five global floats, nothing per-widget. |
| Continuous font-size lerp | 1.92 allows it; the glyph cache pays for it | don't (§3.3). |
| Choice → segmented vs option list | measuring at draw time can flip the decision on an atlas-rebuild frame | **deterministic rule, not measurement**: `≤5 options AND every label ≤10 chars` → segmented bank; otherwise option list. Decided once at registration, cached on the `Entry`. No hysteresis needed because nothing is measured. |
| `Preview` needing arbitrary drawing | a `std::function<void(ImDrawList*)>` would let every caller invent its own look | closed enum (`API.md` §2). This is the main anti-sprawl device and it is deliberately inconvenient. |
| Reset-to-default | needs each entry's default | `.Default()` is required at registration and asserts if missing. Solved by the registry, not by the UI. |

---

## 6. Migration cost

Current `src/Overlay/`: **10,384 lines**.

**New code — the kit:**

| File | Est. lines |
|---|---|
| `Registry.{h,cpp}` + `Bind.h` (declarations, builders, asserts, SelfTest) | 450–600 |
| `Overlay.cpp` (panel, halo, header, rail + TOC + scroll-spy, reel, legend, peek, nav) | 650–800 |
| `Bloom.cpp` (nine instruments + the rest row + hold-to-confirm) | 550–700 |
| `Preview.cpp` (eight strips) | 300–400 |
| `Palette.cpp` + `Match.cpp` | 250–320 |
| `Theme.{h,cpp}` (roles, guard, `Paint*`, `Approach`) | 260–340 |
| `Metrics.h` | 90 |
| **Kit total** | **~2,550–3,250** |

Larger than A's estimated kit (~1,800–2,300) by roughly the size of `Registry.cpp` and
`Preview.cpp` — the two things A did not have and that buy the palette, the always-complete
search index, the previews, `gamescopectl` addressability and reset-for-free.

**Converted sections** (now declaration files, not draw code):

| Today | Lines | After | Notes |
|---|---|---|---|
| `PanelShaders.cpp` | 367 | ~110 | 11 declarations; the shader plumbing stays |
| `PanelDisplay.cpp` | 778 | ~200 | 15 declarations |
| `PanelAudio.cpp` | 431 | ~150 | PipeWire logic untouched |
| `PanelConfig.cpp` | 914 | ~230 | profiles become long `Choice`; delete becomes `.Danger()` |
| `PanelLog.cpp` | 185 | ~30 | one `Stream()` declaration; gains find/levels/follow |
| `FpsDisplay.cpp` config UI | ~700 of 2469 | ~180 | the HUD renderer itself untouched |
| `Chrome.cpp` | 1627 | ~300 | **~1,330 deleted** — windows, title bars, drag, collapse, dock, tiling, focus. Icons survive. |
| `Widgets.cpp` | 942 | ~0 | absorbed into `Bloom.cpp`/`Theme.cpp` |
| `Fonts.cpp` | 460 | ~380 | 10 roles → ~7 bakes |
| `Palette.cpp` (colour) | 127 | ~150 | + `state/danger`, + the contrast guard |
| `Notifications.cpp` | 787 | unchanged | outside the overlay |
| `LogCapture.cpp` | 244 | unchanged | |

**Net: roughly −900 to −1,400 lines** once the port completes, *including* the kit — a
smaller saving than A claims, because BLOOM buys the palette and the previews with real
code instead of getting them free. That trade should be made consciously.

**Effort:**

| Phase | Sessions |
|---|---|
| Registry + Theme + Metrics + rest rows + reel + rail (no instruments) | 1.5 |
| Shaders pilot end-to-end (proves the row grammar + two bloom kinds) | 0.5 |
| Remaining instruments + hold-to-confirm | 1.5 |
| Command palette | 0.5 |
| Display + Config + Audio + Log conversions | 1.5 |
| Previews (six cheap ones) | 0.5 |
| Monitor + schematic `HudGhost` | 1 |
| **Peek spike** (behind a ConVar; judge on a Deck with FSR on) | 0.5 |
| 0.5×/2.0× QA, 30 fps hit-ordering QA, contrast audit, delete `Chrome.cpp` windows | 1 |
| **Total** | **~8.5**, of which ~7 gets a complete shippable overlay without Peek or previews |

---

## 7. Risk register, ranked

1. **Peek's FSR/NIS pop (§4.1 problem 1).** The only thing here that could make a feature
   feel *broken* rather than merely absent. Cheap to spike, cheap to abandon: peek is five
   lines in the shell and nothing depends on it.
2. **Bloom hit ordering at low frame rates (§3.1).** Needs measurement at 30 fps, not
   reasoning. Has a 20-line fallback that removes the dependency entirely.
3. **`HudGhost` fidelity (§4.2).** Mitigated by shipping the schematic version first and
   labelling it. Made largely moot if Peek ships.
4. **The `Sharpen` preview being an approximation (§3.4).** Must be labelled. The failure
   mode is a preview that lies, which is worse than none.
5. **Registry line count (§6).** BLOOM buys more and therefore saves less. If the only
   goal were "delete `Chrome.cpp`", A is the smaller change. If the goal is "one call, and
   the palette, and previews, and `gamescopectl ui set`", this is what it costs.
6. **Deleting the toggle switch (SPEC §2.4).** Not a technical risk — a taste risk, and
   the loudest thing in the design. It is one `Bloom.cpp` case to add back if the user
   hates it, and the API does not change (`Bool()` would simply render differently).
7. **Everything else** — normal work.

---

## 8. The honest summary of what is ambitious

Listed plainly, so it is judged now rather than discovered later:

- **Peek** lifts the compositor's blur and darkening while a control is held. Two lines in
  existing per-frame code; one visible risk (FSR pop). **Spike first.**
- **The bloom overlays instead of reflowing**, which requires splitting draw order from hit
  order. Standard ImGui primitives; one frame of latency to QA.
- **The contrast guard** ties panel alpha to compositor darkening. Trivial code; it changes
  what a user's Transparency slider means, which is a product decision as much as a
  technical one.
- **No text token has alpha.** Free to implement, but it means "make this dimmer" is a new
  colour, not a multiply — a discipline the team has to keep.
- **The toggle switch is deleted.** Free to implement, easy to revert, loudest to look at.
- **`Preview` is a closed enum.** Free, but it means the first feature that wants a bespoke
  preview must change the kit rather than route around it. That is the point, and it will
  feel like friction at least once.
- **`TextSample` uses 1.92's dynamic font sizing** for the HUD font-size preview. Confirmed
  present; bounded to 41 cache entries; measure it anyway.
