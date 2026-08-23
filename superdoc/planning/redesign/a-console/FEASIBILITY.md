# Feasibility — Direction A "Console" in immediate-mode ImGui

Dear ImGui **1.92.9b, stock non-docking branch**. Honest assessment: what is easy, what
needs `ImDrawList` work, what fights immediate mode, and what it costs to get there from
today's `src/Overlay/`.

Verdict up front: **this direction is structurally *easier* to implement than what ships
today**, because it deletes the two things ImGui is worst at (multi-window management,
per-window focus/Z-order) and leans on the two things it is best at (a single window with
stock nav, and hand-painted rows on one draw list). Three items are genuinely hard, and
one of them (System Monitor's live preview) is the only thing on the list I would not
promise before spiking it.

---

## 1. Easy — stock ImGui does this, or it's a straight `ImDrawList` paint

| Thing | Why it's easy |
|---|---|
| **One window, fixed pos/size** | `SetNextWindowPos/Size` + `NoDecoration|NoMove|NoResize|NoSavedSettings`. This removes today's entire hand-rolled title-bar drag (`Chrome.cpp`'s per-frame `SetWindowPos` delta), collapse toggle, close glyphs, tiled default positions and focus/Z bookkeeping. |
| **Rail, header, legend** | fixed-height bands inside one window; `PushClipRect` + manual cursor placement. Nothing novel — `Chrome.cpp` already paints harder chrome than this. |
| **The Row frame** | `ItemSize`/`ItemAdd`/`ButtonBehavior` over the full row rect, then paint. This is the *exact* pattern `Widgets.cpp`'s `Toggle()`/`Checkbox()` already use; the only change is that the item is the whole row instead of the control. Making the row the item is a simplification, not a complication. |
| **Switch / segmented / chips / readout / action / drill** | pure `AddRectFilled` + `AddText`. All exist today in some form. |
| **Compact slider** | `SliderBehavior()` on the track rect, drawn manually — this is `SliderControl()` today with a narrower rect and the marks removed. The `GrabMinSize`-vs-drawn-handle invariant (`slider-widget-spec.md` §3) carries over verbatim and must be preserved. |
| **Group heading + rule** | one `AddText` + one `AddLine`. |
| **Scrolling stage** | one `BeginChild` with `ImGuiChildFlags_None` — the only child in the design. Stock scroll, stock `SetScrollHereY` for nav-follow. |
| **LOG virtualisation** | `ImGuiListClipper` is stock and handles 100k lines. `LogCapture` already stores lines as a vector. |
| **Sub-tab bar** | it's a segmented control at a fixed position. Not `BeginTabBar` — which is good, because `BeginTabBar` is exactly the widget whose look we can't control. |
| **Font atlas** | 6 roles (≈8 baked faces counting the two Mono weights) vs today's 10. Deferred-rebuild machinery (`fonts::RebuildAll` + `ApplyPendingRebuild`, issue #51) is unchanged and already works. |
| **The 4 animations** | 5 floats total, lerped against `io.DeltaTime` with one `Approach()`. No retained state, no per-widget storage, no ID-keyed animation table. This is the cheapest possible answer and it is deliberate. |
| **OKLCH accent flow** | unchanged from `Palette.cpp`. `state/danger` adds ~10 lines. |
| **0.5×–2.0× scaling** | everything derives from `u` and `base`; the current code already multiplies its constants by `DisplayScale()` — the kit centralises that into one `Metrics` struct instead of repeating it per file. Strictly less code. |

---

## 2. Medium — needs care, has a known recipe

### 2.1 ← / → adjusts the focused control without activating it

ImGui's nav model is: `Left/Right` moves focus *between* items on the same row, and only
becomes a value tweak once an item is *activated* (A/Enter, `ImGuiItemFlags_Inputable`).
The console wants Left/Right to adjust the focused row's control directly.

**Why it works here:** there is exactly one item per row, so ImGui's own horizontal nav has
nothing to move to and the keys are free. The recipe (`API.md` §4.4) reads
`GetKeyPressedAmount()` for the four keys with repeat, gated on `IsItemFocused()`.

**Residual risk (low–medium):** ImGui may still consume the arrow keys for `NavMoveRequest`
in some frames (e.g. when the focused item is at the edge of a scroll region and ImGui
tries a "wrap" move). Mitigation: set `ImGuiItemFlags_NoNav` off but clear the horizontal
move request in the same frame we consumed it (`ImGui::NavMoveRequestCancel()`), and QA it
specifically at the top/bottom of a scrolled list. Budget half a day; it is one helper, not
a per-widget problem.

### 2.2 The Sheet

Do **not** hand-roll a focus-trapping overlay. Hand-rolled nav trapping in ImGui means
poking `g.NavWindow`, which is internal and version-fragile.

**Recipe:** `ImGui::OpenPopup()` + `ImGui::BeginPopupModal()` with
`NoTitleBar|NoResize|NoMove|NoBackground`, positioned by `SetNextWindowPos` to the stage's
right edge, painted entirely by us. Stock modal gives focus trapping, Esc-to-close and the
dimming pass for free. The slide-in is `SetNextWindowPos` with an `Approach()`ed x.

Cost: low. Risk: low. The only compromise is that a modal blocks the console's own hotkeys
for its duration, which is correct behaviour anyway.

### 2.3 Stage push/pop transition

A true two-page cross-slide needs *both* screens' `pfnDraw()` running in the same frame.
That is possible (`PushID` keeps their IDs disjoint) but doubles the draw work during the
transition and risks double-firing any side effect a screen's draw performs.

**Decision: don't.** During a push, draw only the incoming screen, offset by 24u and ramped
from alpha 0.4 → 1.0 over ~120ms. The outgoing screen simply stops. This reads as motion,
costs one float, and cannot double-fire anything. Flagged so nobody later "fixes" it into a
real cross-fade without understanding what they're buying.

### 2.4 Inline text edit in the control column

`InputText` with `ImGuiInputTextFlags_AutoSelectAll`, sized to the control column, painted
with our own frame (`PushStyleColor(FrameBg, transparent)` + our `AddRectFilled` behind).
Activating from nav needs `SetKeyboardFocusHere()` on the frame the row is activated —
standard, but the IME path in gamescope (`input-method-ime.md`) is worth one check.

Cost: low–medium. There are only two text fields in the whole product (profile name,
stream filter).

### 2.5 The Find index

The clean design ("run every screen once in a suppressed probe pass to harvest labels")
**does not work**, because screen draw functions are not side-effect-free today —
`PanelShaders`' draw calls `EnsureEffectLoaded()`, `PanelAudio`'s enumerates PipeWire
nodes, and several read live compositor state. A probe pass would trigger all of it.

**What to do instead:** index lazily. `EndRow()` records `{screenId, label}` into a
persistent map the first time each row is actually drawn. Find searches that map plus a
static table of screen titles. Practical effect: a setting on a screen you have never
opened this session isn't findable by its label until you visit that section once —
acceptable, and honest. If a full index is wanted later, the fix is to make screen draws
pure (move the `Ensure*` calls into a per-screen `pfnEnter` hook), which is a worthwhile
refactor on its own but is not a prerequisite.

### 2.6 Log Find highlighting

Substring highlight inside a monospace line means splitting the line into up-to-3 `AddText`
runs and painting an accent rect behind the match. With Geist Mono the x-position of the
match is `index × advance`, so it's arithmetic, not measurement. Easy in the mono case;
would be nasty in the proportional case — which is why the LOG stays Mono.

---

## 3. The fine print on typography

The claim "6 roles" hides a detail: **ImGui bakes one atlas entry per (face, size) pair**,
and Geist Mono 500 and 600 are different faces. Real bake count:

`Display` (Mono600 24) · `Title` (Mono600 11.7) · `Body` (Sans400 15) · `Value` (Mono500
14.4) · `Value-small` (Mono500 13.5) · `Meta` (Sans400 12) · `Micro` (Mono400 10.5) ·
`Seg-active` (Mono600 13.5) = **8 entries**, down from today's 10. Modest win, not a
dramatic one. Worth stating so nobody expects the atlas to halve.

The `Value`/`Value-small` split exists because a 44px row cannot carry the same numeral
size as a hero readout. If that split is unacceptable, collapse to one `Value` and accept
slightly large numerals in dense rows — a real trade, not a bug.

---

## 4. Hard — real ImDrawList work

### 4.1 Row-level clipping with an animated stage offset

While the stage slides, rows must clip to the stage rect, not the window rect. That means
`PushClipRect` around the stage content and drawing at an offset cursor. ImGui handles this
fine, but `ItemAdd` hit-testing uses the *unclipped* rect unless clipped explicitly —
`ImGui::ItemAdd()` with the clip rect pushed does the right thing, but scroll +
translation + clip interacting correctly needs testing at both scale extremes. Not novel;
just fiddly.

### 4.2 Painting the accent tick with a glow

Two expanded `AddRectFilled` passes behind the tick (the recipe already used for the slider
handle glow). Cheap. The only trap: the tick animates between rows *within a clipped scroll
region*, so its y must be in the same coordinate space as the scroll — store it in scroll
space, not screen space, or it drifts when the list scrolls.

### 4.3 The rail's collapse animation and text fade

Rail labels must fade out as the rail narrows, not clip mid-glyph. Solution: alpha-ramp the
label colour by the same animated factor and stop drawing under 0.05. Trivial, but has to
be remembered.

---

## 5. Genuinely risky — the System Monitor live preview

This is the single item I would spike before committing.

**What the design asks for:** a Well showing the actual FPS HUD, at its real pixel size,
composited over a patch of the actual frame, with the 3×3 anchor grid over it, updating
live as the user drags Font size or toggles the Backdrop.

**Three problems:**

1. **The HUD lives in a different ImGui context.** `FpsDisplay.cpp` owns its own context
   (per its file comment, and `Fonts.h` confirms atlases are per-context). You cannot draw
   context A's widgets into context B's draw list. To preview it in the console you must
   refactor the module draw functions from "draw into my context at my anchor" to
   "`Draw(ImDrawList*, ImVec2 origin, float flScale)`", with the fonts resolved from the
   *calling* context. That is a real refactor of the biggest file in the overlay
   (`FpsDisplay.cpp`, 2469 lines) — mechanical, but broad, and it must not regress the
   real HUD. **Estimate: 1–1.5 sessions on its own.** Font sizing across contexts is the
   sharp edge: the HUD's `font_size` is in HUD-context pixels, and previewing it in the
   console context means either baking those sizes into the console's atlas too (atlas
   growth, and it changes when the user drags the slider — a rebuild per drag frame, which
   is exactly what issue #51 forbids) or accepting a scaled approximation. **Recommended
   compromise: draw the preview at the console atlas's nearest available size and label the
   Well "approximate scale".** Honest, cheap, and it still shows placement, backdrop,
   colours and which modules are on — which is 90% of what the preview is for.

2. **"A patch of the actual frame" is probably not available.** The console sits on top of
   a compositor blur+darken pass applied to the whole game layer
   (`FrameInfo_t::blurRadius`, the darkening CTM). Punching an unblurred rectangular hole
   in that pass would need the blur/darkening to accept a rect exclusion, which it does not
   today. **Fallback (what the mockup shows): a representative static backdrop inside the
   Well.** It demonstrates contrast and placement; it is not literally this frame. If a
   rect-exclusion ever lands in the blur pass, upgrade it then.

3. **Cost of getting it wrong:** a preview that disagrees with the real HUD is worse than
   no preview. Whatever compromise is chosen must be *labelled* in the Well header
   ("approximate scale") rather than implied to be exact.

**If the spike says no:** the Well degrades to a schematic preview — the anchor grid over a
neutral backdrop with a to-scale *outline* box showing the HUD's measured footprint (the
measure functions already exist: `MeasureFpsModule()` and siblings). That is still strictly
better than today's floating-window-with-no-preview, and it is a two-hour job. **Design
this fallback in from the start; treat the live preview as an upgrade.**

---

## 6. What fights immediate mode (flagged explicitly)

| Item | The fight | Resolution |
|---|---|---|
| Cross-fading two stages | needs both screens drawn per frame; side effects double-fire | don't do it — offset+alpha the incoming page only (§2.3) |
| A search index over screens never drawn | immediate mode has no retained widget tree to walk | lazy index; accept "visit once to index" (§2.5) |
| Focus trapping for the sheet | requires internal nav state if hand-rolled | use a stock modal (§2.2) |
| Per-widget animation | would need an ID-keyed persistent store | design says no — 5 global floats, nothing per-widget |
| Cross-context HUD preview | atlases and draw lists are per-context | refactor the module draws to take a draw list, or degrade to schematic (§5) |
| "Reset to default" (Y) | needs to know each row's default | comes from `Config/ConfigSchema.h`'s defaults, not from the UI — pass the default through `Opt` or look it up by field path. Small design task, not an ImGui problem. |
| `Choice()` picking segmented-vs-picker by measuring | measurement happens at draw time, so a font-atlas rebuild frame can flip the decision mid-drag | measure against the *previous* frame's atlas and hysteresis the decision (only flip when it's off by >10%) — or simply let the caller's option count decide (≤5 short labels = segmented). Prefer the latter for predictability. |

---

## 7. Migration cost from today's code

Current `src/Overlay/`: **10,384 lines**.

**New code — the kit:**

| File | Est. lines |
|---|---|
| `Console/Console.cpp` (shell, header, rail, stage, legend, nav, anim) | 700–850 |
| `Console/Rows.cpp` (12 kinds + RowFrame + LogView) | 600–750 |
| `Console/Sheet.cpp` | 180–250 |
| `Console/Theme.h/.cpp` (roles + `Paint*`) | 250–350 |
| `Console/Metrics.h` | 80 |
| **Kit total** | **~1,800–2,300** |

**Converted screens:**

| Today | Lines | After | Notes |
|---|---|---|---|
| `PanelShaders.cpp` | 367 | ~150 | pure settings; the shader plumbing stays |
| `PanelDisplay.cpp` | 778 | ~280 | 4 screens; the gamescope-state plumbing stays |
| `PanelAudio.cpp` | 431 | ~220 | PipeWire logic untouched, UI shrinks |
| `PanelConfig.cpp` | 914 | ~380 | profiles/copy/delete become Drill+Sheet |
| `PanelLog.cpp` | 185 | ~120 | gains Find/levels/follow |
| `FpsDisplay.cpp` config UI | ~700 of 2469 | ~300 | the HUD renderer itself is untouched |
| `Chrome.cpp` | 1627 | ~320 | **~1,300 lines deleted** — windows, title bars, drag, collapse, dock, tiling, focus. Icons (~300) survive. |
| `Widgets.cpp` | 942 | ~0 | absorbed into `Rows.cpp`/`Theme.cpp`; `SliderBehavior` discipline carried over |
| `Fonts.cpp` | 460 | ~400 | 10 roles → 8 bakes |
| `Palette.cpp` | 127 | ~140 | + `state/danger` |
| `Notifications.cpp` | 787 | unchanged | outside the console |
| `LogCapture.cpp` | 244 | unchanged | |

**Net:** roughly **−1,400 to −1,800 lines** of overlay code once the port completes,
*including* the new kit. The saving is almost entirely `Chrome.cpp` plus the per-panel
layout boilerplate.

**Effort, realistically:**

| Phase | Sessions |
|---|---|
| Kit core + Theme + Metrics + Rows (through `Switch`/`Slider`/`Readout`) | 1.5 |
| Shaders pilot end-to-end (proves the row grid on a real screen) | 0.5 |
| Remaining row kinds + Sheet + Find | 1 |
| Gamescope (4 screens) + Config (3 screens) | 1.5 |
| Audio + first Well, Log + first Wide | 1 |
| System Monitor (schematic Well) | 1 |
| System Monitor live preview (the `FpsDisplay` draw-list refactor) | 1–1.5 *(optional; spike first)* |
| Polish, 0.5×/2.0× QA, gamepad QA, delete `Chrome.cpp`'s window code | 1 |
| **Total** | **~8–9 sessions**, of which ~7 gets a complete, shippable console without the live preview |

**Risk register, ranked:**

1. **System Monitor live preview** — the only item that might not be achievable as designed
   (§5). Mitigated by a designed-in fallback.
2. **← / → adjust vs ImGui nav** (§2.1) — likely fine, needs edge-case QA.
3. **`FpsDisplay.cpp` refactor blast radius** — only if the live preview is attempted.
4. **Gamepad plumbing** — worth confirming *before* anything else that gamescope actually
   routes a gamepad into the overlay's ImGui context today. The whole direction is designed
   for a controller; if only keyboard+mouse reach the overlay, the design still works
   (every gamepad path has a keyboard twin) but the primary justification weakens. **Check
   this first — it's a 30-minute answer that shapes the priority of everything else.**
5. **Everything else** — normal work.
