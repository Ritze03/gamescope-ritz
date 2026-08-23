# Feasibility — E3 "The Bench" against stock (non-docking) Dear ImGui 1.92.9b

Honest assessment. Where this direction is weak or speculative, it says so, and §5 lists
every ambitious thing in it with a red/amber/green grade so it is judged now rather than
discovered during implementation.

Verified against the repo: `subprojects/imgui.wrap` pins **stock v1.92.9b**
(`f1cc2ae`, non-docking, with the note *"docking is deliberately not used here"*);
`src/SettingsOverlay.cpp` initialises **`ImGui_ImplVulkan`** and records the overlay's
draw data into its own `s_pOverlayTexture` via `CmdBeginRendering`, which is then handed
to the composite as a layer (`layer->tex = s_pOverlayTexture`). Those two facts decide
most of what follows.

---

## 1. Two edge-pinned slabs

```cpp
const Layout L = Shell::ComputeLayout( ImGui::GetIO().DisplaySize, palette::DisplayScale() );

constexpr ImGuiWindowFlags kSlab =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
    | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
    | ImGuiWindowFlags_NoNavInputs;                       // see section 2

ImGui::SetNextWindowPos ( L.spine.Min );
ImGui::SetNextWindowSize( L.spine.GetSize() );
ImGui::Begin( "##spine", nullptr, kSlab );  DrawSpine();  ImGui::End();

if ( L.step != Step::ScopeHidden )
{
    ImGui::SetNextWindowPos ( L.scope.Min );
    ImGui::SetNextWindowSize( L.scope.GetSize() );
    ImGui::Begin( "##scope", nullptr, kSlab );  DrawScope();  ImGui::End();
}
```

**Risk: none material.** Two `Begin()` calls with `NoMove | NoResize | NoSavedSettings` is
the oldest pattern in ImGui. Both rects come from one pure function of
`(DisplaySize, display_scale, ui.gutter, ui.scope)`, which is unit-testable without a
renderer.

**The obvious objection — "you deleted six floating windows and added two back" — and the
answer.** A floating window is defined by having a position that can *differ*: from run to
run, from user to user, from a bug. These have no position at all; they have a computed
rect. Everything that generated this repo's window bugs is absent:

| Past bug | Cause | Present here? |
|---|---|---|
| #33 collapse | native collapse wired to a title bar that no longer exists | No title bar, no collapse |
| #34 measured-and-grown sizing | a layout value feeding back into the thing that produced it | Rects are computed from the surface, never from content |
| #42 hand-rolled drag | `NoMove` blocks `StartMouseMovingWindow()` | No drag |
| tiling / stacking / "buried panel" | independent placement + z-order | Two rects that cannot overlap except in ladder step 2, where the overlap is the design |

The only thing genuinely re-introduced is a **second `Begin()`'s worth of focus routing**,
which §2 addresses.

`ImGuiStyleVar_WindowPadding` and `ItemSpacing` are zeroed once in `Shell::Draw()`, and
the slab chrome (background, border, Ledger, focus glow) is drawn manually onto each
window's draw list — as today.

---

## 2. Input: drop ImGui nav entirely. This is the biggest simplification in the proposal.

Direction E's feasibility doc called *"accept ImGui's nav wholesale"* its single most
important implementation decision, and then listed three focus scopes and cross-region
`←/→` nudging as its worst weakness. **Dropping gamepad support (2026-08-23) makes the
opposite choice cleanly correct.**

E3 sets `ImGuiWindowFlags_NoNavInputs` on both slabs and owns selection itself:

```cpp
// Shell.cpp
std::string m_sSelected;       // a STABLE entry id -- "display.sharpness"
Region      m_eFocus;          // Spine | Scope
```

What is kept from ImGui, unchanged, because it is correct and free:

- **`ItemAdd()` + `ItemSize()`** per row — clipping, hover rects, the item-status stack.
- **`ButtonBehavior()`** for every clickable region — hover, hold, `ActiveId`, click
  semantics, and correct behaviour when the mouse leaves the window mid-press.
- **`SliderBehavior()`** is *not* used; the shipped `widgets::SliderControl()` already
  hand-rolls its drag against `ActiveId` and is the measured, signed-off geometry. It
  keeps doing that, with the lane rect passed in.
- **`InputText()`** for the one Text control, with `SetKeyboardFocusHere()` on `Enter` and
  a documented seam where ImGui owns the keyboard until `Enter`/`Esc`.
- `SetScrollHereY( 0.5f )` when selection moves outside the visible band.

What is replaced: `RenderNavCursor()`, because a nav cursor drawn by ImGui would disagree
with `m_sSelected`. The focus ring is one `AddRect` at `accent@100%` (9.35:1), drawn by
`Row::Begin()` from `m_sSelected`.

**Why this is less code, not more.** ImGui nav would have to be fought in four places:
`Tab` (ImGui uses it for next-item; we need it for region switching), `←/→` (ImGui uses
them for nav within a window; we need them to adjust values with one keypress), the
accordion (a closed category's rows are not in the item list, so nav would skip
inconsistently), and cross-window movement (`Ctrl+Tab` is ImGui's, not ours). Owning a
single string id and a `Region` enum is roughly 120 lines and has no fight in it.

**The honest cost:** hit-testing and hover come from ImGui but *keyboard* focus does not,
so the two can disagree if a future contributor calls a stock ImGui widget inside a slab.
`ui_lint` reports any `ImGui::` call outside `Controls.cpp` for exactly this reason.

**Input capture over a game** is unchanged: the overlay already takes input when open.
`Ctrl+K`, `Ctrl+I`, `Ctrl+D` and **`Alt`** must be consumed and not forwarded. `Alt` is
the one to watch — it is a common game modifier, and holding it while the overlay is open
must not reach the game. The existing capture path already swallows everything while the
overlay is open, so this is a non-issue *as long as peek stays bound to a key rather than
to a "hold while the overlay is closed" gesture*, which it does.

---

## 3. The accordion, scrolling, and the clipper

**One `BeginChild` per slab.** The Spine's body is a single scrolling child; the Scope's
is another. `PushClipRect` is needed only for ladder step 2, where the Scope paints over
the Spine's child.

**Row counts.** With one category open, the Spine draws at most ~24 rows plus 10 collapsed
category headers plus 4 section headers — under 40 items, a few hundred `ImDrawList`
primitives. No clipper needed, and this is *strictly cheaper than E's sheet*, which drew
a whole category's rows in a taller region with a rail beside it.

**The Log** is the one long list (up to ~40 000 lines). `ImGuiListClipper` requires a
uniform item height; the Log's `Raw` rows are all 20 base, so the clipper is exact and the
whole buffer costs one screenful. The severity minimap on the scrollbar is drawn from a
pre-computed index maintained when `LogCapture` appends — it does not iterate the buffer
per frame.

**Mixed heights.** A sheet with both 42 and 70 rows breaks a naïve clipper. Two
mitigations, in order: (a) an open category is short enough not to need one; (b) if one
ever isn't, the Spine already knows every row's height because it assigned it, so a
prefix-sum rebuilt only when the row count changes makes an exact clipper possible.
Deferred until measured; not a blocker.

**IDs.** `PushID( entry.Id() )` — the *stable string id*, never the loop index. A category
opening or closing changes the row list, and index-derived ids would make ImGui think a
slider being dragged became a different widget mid-drag. This also removes a real current
bug class: `"Strength##vibrancy"`, `"Strength##presharpen"`,
`"Strength##adaptivebrightness"` are manual disambiguations that stable ids delete.

**No splitter, deliberately.** A splitter is ~30 lines to write and hard to keep correct:
a persisted float that must be reinterpreted when `display_scale` changes (store base or
physical? get it wrong and 2.0× users find their Scope 4px wide), clamping that must
interact with the ladder, a drag that must not be captured by the row underneath, and a
mid-frame resize that invalidates clipper assumptions. Instead:
`ui.scope ∈ { hidden, docked, wide }` and `ui.gutter ∈ { on, off }`, both persisted, both
base-unit, both screenshot-testable. This is a real capability loss — nobody gets a
600px Scope for one session — and it buys the removal of the highest-risk interaction in
the design.

---

## 4. The width budget

Base units: spine 552, scope 432, margin 56, lane 190. Surface 1920×1080.
Everything is `× display_scale`.

| Scale | Spine px | Scope px | Margins px | Gutter px | Ladder step | Spine content, **base** |
|---|---|---|---|---|---|---|
| **0.5×** | 276 | 216 | 56 | 1372 | **−1 · wide Scope** | 502 |
| **0.75×** | 414 | 324 | 84 | 1098 | 0 · open | 502 |
| **1.0×** | 552 | 432 | 112 | 824 | 0 · open | 502 |
| **1.25×** | 690 | 540 | 140 | 550 | 0 · open | 502 |
| **1.5×** | 828 | 648 | 168 | 276 | **1 · docked** | 502 |
| **1.75×** | 966 | 756 | 196 | — | **2 · overlay** | 502 |
| **2.0×** | 1104 | 864 | 224 | — | **2 · overlay** | 502 |

**The right-hand column is the point.** The Spine's content width in base units is
**502 at every scale**, because the slabs are edge-pinned and fixed-base rather than
proportional. There is no reflow: the same 296-base label column and the same 190-base
lane at 0.5× and at 2.0×. No two-column mode, no three-column mode, no "at 1.25× rows go
two-line". Compare Direction E's table, where the sheet's base width swung between 703 and
1732 and drove a five-step column ladder that had to be designed, implemented and
screenshot-tested at each step.

**What this costs at the extremes, honestly:**

- **2.0× kills the thesis.** The Spine alone is 1104px of a 1920 screen; with the Scope
  overlaid, the design is a conventional single panel and the "keep the centre clear"
  idea is gone. It still *works* — every setting is reachable and legible — but the thing
  that makes E3 different is a 1.0×–1.5× feature. Someone running at 2.0× is running at
  2.0× because they need large type, and they will get a large, ordinary, correct
  settings panel. That is the right degradation, but it is a degradation.
- **0.5× looks lost.** A 276px slab on a 1920 screen is a stamp. Ladder step −1 widens
  the Scope to 560 base so the probes are worth looking at, and that is all the answer
  there is. Direction E flagged 0.5× as its own under-discussed risk for the same reason;
  neither direction solves it, and it is worth a real screenshot check before shipping.

---

## 5. The ambitious list — graded

Everything in this proposal that is not "draw a rectangle", with an honest grade.

### 🟢 GREEN — pure `ImDrawList`, no new plumbing

| Thing | Notes |
|---|---|
| **The Lane Law** | A struct with three `ImRect`s. The cheapest idea in the document and the one that most directly answers the user. |
| **The accordion Spine** | One `int m_openCategory`, one loop. |
| **The Ledger** | `n_changed × AddRectFilled` + a hover test. Under 30 lines. |
| **The Selector, both densities** | `CalcTextSize()` per option once per frame, then the existing `widgets::SegmentedControl()` painting. The compressed form is the same painter with different cell widths. |
| **The Stage composite** | `AddRectFilled` for the miniature, `InvisibleButton` for the drag, nine 9px `ButtonBehavior` targets, and one call to the *existing* `FpsDisplay::Geometry()` for the block. No new data, no new pass. **This is the showcase and it is also one of the cheapest things here.** |
| **Trace probe** | `FpsDisplay.cpp` already keeps a 240-frame history and already draws a graph. The probe draws the same buffer bigger, plus one `AddLine` for the cap. Near-zero marginal cost. |
| **Meter probe** | `PanelAudio.cpp` already has peak data. Same story. |
| **Curve probe** | ~100 `AddLine` segments of a closed-form PQ curve. Static per frame. |
| **Swatches probe** | 6 × `palette::OklchToImU32`. Already exists for the hue slider's preview. |
| **Delta probe** | Numbers and three rects, from `Binding` alone. |
| **Peek** | One `ui::Anim` lerp applied to both slabs' alpha and the veil's. |
| **Command palette** | ~150 registered entries, a fuzzy scorer, recomputed only when the query changes — Direction B measured this at <50 µs. |

### 🟡 AMBER — real work, no unknowns

| Thing | Notes |
|---|---|
| **Owning selection instead of ImGui nav** | ~120 lines, plus the `InputText` seam. Low risk but it is new code where E planned to use stock behaviour. |
| **Two slabs' focus routing** | `Tab` must move `m_eFocus`, and a click in either slab must set it. Straightforward; just needs to actually be done. |
| **The clamp on `ui.darken`** | The helper must refuse a darkening value that would put `TextMeta` under 4.5:1 over white. Needs the contrast function in `Theme.cpp` — 20 lines — and a reason string. It is a *user-facing slider being overruled*, which needs a good message, not just a clamp. |
| **`ui_lint`'s source greps** | The float-literal and forbidden-call checks are not runtime checks; they want a small script in `meson.build` or a pre-commit hook, not a ConCommand. Say so rather than pretending `ui_lint` can do it. |

### 🔴 RED — the Frame probe. The one thing that might not survive contact.

The wipe that shows a real region of the frame processed two ways is the most interesting
thing in this proposal and the least certain.

**What is genuinely available.** `SettingsOverlay.cpp` initialises `ImGui_ImplVulkan`, so
`ImGui_ImplVulkan_AddTexture( sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL )`
returns a `VkDescriptorSet` usable as an `ImTextureID` with `ImDrawList::AddImage()`. The
game's frame is a `CVulkanTexture` with an existing image view. The overlay's ImGui pass
runs *before* the composite (it produces a layer that the composite then consumes), so
reading the game image there is ordering-safe.

**The three concrete problems.**

1. **Descriptor lifetime.** `ImGui_ImplVulkan_AddTexture` allocates from the backend's
   descriptor pool. The game's texture rotates every frame, so a naïve
   add-per-frame exhausts the pool within seconds. This needs a small cache keyed on
   `VkImageView`, with `ImGui_ImplVulkan_RemoveTexture` on eviction, and correct
   deferred destruction against in-flight submissions — the same hazard the file already
   documents around `s_pOverlayTexture` ("*never gets an explicit … while that submission
   is still in flight*"). This is the part most likely to produce a validation-layer
   nightmare.
2. **Layout and barriers.** Client textures are not always in
   `SHADER_READ_ONLY_OPTIMAL`. A transition + barrier must be inserted before
   `CmdBeginRendering` in the overlay pass, and undone. Doable; needs care on every
   backend path (DRM, SDL, OpenVR, Headless) and on the wsi-layer'd path where the image
   is owned by the game's swapchain.
3. **"After" is a lie unless it is real.** The honest version of the probe is not "linear
   vs FSR"; it is *"the raw frame vs what is actually on screen"*, because gamescope has
   the first for free and the second only if it retains a copy of the composite output.
   A true A/B of two *different* filter settings requires running the upscale pipeline
   twice at preview resolution.

**Staged plan, so this can be landed or cut without touching the design.**

| Tier | What it shows | Cost | Confidence |
|---|---|---|---|
| **0** | Nothing — every Frame probe falls back to **Delta** | zero | shipped |
| **1** | The raw frame region at 1:1 vs magnified, with `VK_FILTER_NEAREST` and `VK_FILTER_LINEAR` samplers on the two halves | one cached descriptor pair + a barrier | high — genuinely demonstrates nearest vs linear, honestly labelled |
| **2** | A real A/B: a 256×144 offscreen target, one extra dispatch of the existing FSR/NIS/sharpen shaders at preview scale per half | a small render target, a per-frame dispatch, ~2–3 days | medium |
| **3** | Frame history, before/after across time | needs retained composite output | **cut** |

**Recommendation: build tier 1, design for tier 2, ship tier 0 if either slips.** The API
is deliberately shaped so this is a one-line change per call site (`.Probe(...)` removed)
with no structural consequence — see `API.md` §4. Six of the seven probe kinds are green;
the direction does not depend on the red one.

### Other ImGui-specific notes

**Backdrop blur.** Unchanged by this proposal — the blur is gamescope's own composite pass
behind the overlay layer, not ImGui. Two rects to blur behind instead of six, and no
overlapping-blur double-darkening. The **per-region veil gradient** (strong under the
slabs, weak across the middle) is one `AddRectFilledMultiColor` inside the overlay
texture, which is free, but note that it darkens the *overlay texture*, not the game — so
the "uncovered centre" is uncovered because nothing is drawn there, and the gradient only
softens the transition. Correct, and cheaper than it looks.

**Font atlas.** Six roles instead of ten, re-baked on `display_scale` change via the
existing non-idempotent `fonts::Load()` path (#38). Smaller atlas, same mechanism.

**Hairlines at 0.5×.** `1 × 0.5 = 0.5px` aliases away. Rule:
`ImMax( 1.0f, ImFloor( 1.0f * scale ) )` in one helper, `ui::Hairline()`, and no call site
computes it.

**`ImDrawList` budget at 1.0×.** Spine ~40 items × ~12 primitives ≈ 500; Scope ~200 plus
the probe (the Trace probe is the heaviest at 240 `AddRectFilled`); Ledger ~8. Under 1000
per frame, against the low thousands the current six-window layout already produces.

---

## 6. What this direction is worst at — candidly

1. **The accordion hides its siblings' contents.** You cannot compare Upscaling's rows
   against HDR's rows without collapsing one. Direction E's rail + sheet could not do that
   either, but E's list-and-inspector pattern *could* (both shader effects on screen, one
   click apart). E3 gives that up for one fewer focus scope. `Ctrl+K` and the Ledger
   soften it; they do not remove it.
2. **A long category scrolls a long way.** Monitor has 17 rows across five groups. Open,
   it pushes SETUP well below the fold. This is the direct price of merging rail and
   sheet, and it is worst on exactly the category that most needs the Scope.
3. **The design's best idea is its most expensive one.** The Frame probe is what makes
   "the Scope is an instrument" more than a slogan for the settings that most deserve it
   (Filter, Sharpness, the shaders). If it lands at tier 0, those five settings get a
   number line, and the direction is meaningfully less special than it reads.
4. **Two slabs is a harder sell than one.** It is defensible (§1) and it is not what the
   past bugs came from, but "we are adding a second window" will and should attract
   scrutiny, and a reviewer who does not accept the position/rect distinction will read
   this as a regression.
5. **2.0× is an ordinary settings panel** (§4). Everything works; nothing is special.
6. **The Ledger is decorative until you have changed something.** On a fresh install it is
   six pixels of empty column. That is correct — a monochrome screen means everything is
   at default — but it means the first-run screenshot does not show what it is for.
7. **More code than E's first landing.** A Spine, a Scope, a Ledger, a probe layer, a
   ladder, a palette, a lint pass and a selection model is the largest first PR of any
   direction proposed. §8's sequencing exists to make that survivable, not to hide it.

---

## 7. What it is best at

Stated once, for symmetry with §6, because a feasibility document that only lists risks
is not honest either.

- **The four complaints are answered structurally, not stylistically.** One boolean
  control (the checkbox is deleted, not re-motivated). One choice control (density
  measured, not chosen). One lane with one right edge (no alignment parameter exists).
  Composites promoted into the depth region (the user's own prescription, applied to a
  control).
- **The layout never reflows.** 502 base units of content at every scale (§4). There is
  one ladder with three steps instead of E's five, and no column mode at all.
- **Dropping gamepad is converted into a real simplification** (§2) rather than just
  removing a section.
- **The one control the user singled out as bad becomes the one they will show people.**
  The Stage is green-graded, reuses `FpsDisplay::Geometry()`, replaces three rows with
  one, deletes a second drifting call site, and previews itself live over the game.

---

## 8. Migration cost

**Estimate: ≈ 2.5 weeks of focused work, landable as seven PRs, net roughly LOC-neutral.**

| PR | Work | New | Deleted |
|---|---|---|---|
| 1 | `UI/Shell` — two slabs, `ComputeLayout`, the ladder, the Spine accordion, section/category headers, the selection model, peek, `Escape()`. Every existing panel hosted verbatim as an escaped category. **Ships as a working overlay on day one.** | ~750 | 0 |
| 2 | `UI/Row` + `UI/Controls` — the Lane Law, the eleven control kinds, `Bind`/`Cfg`, the Selector's measurement, `ui_lint`, `ui_snapshot`. Ports `Widgets.cpp`'s painters behind rect-taking signatures. **Deletes `widgets::Checkbox`** (issue #60: zero callers). | ~1300 | ~180 |
| 3 | `UI/Scope` + `UI/Probe` — the Scope shell, the Facts grid from `Binding`, expert blocks, the category card, and the five green probes (Trace, Curve, Meter, Swatches, Delta). | ~600 | 0 |
| 4 | Convert Display (3 categories), Shaders, Audio. The Ledger. The `Ctrl+K` palette. | ~700 | ~1300 |
| 5 | Convert Monitor — **including the Stage composite**, which deletes the 3×3 grid + two margin sliders in two files — plus Profiles, Per-Game, Appearance. | ~800 | ~1900 |
| 6 | Convert LOG (raw category, filter bar, severity minimap, line Scope). | ~300 | ~185 |
| 7 | Delete `Chrome.cpp`'s dock / window / title-bar / tiling / drag / collapse machinery; delete `Escape()`. **Frame probe tier 1** behind a convar, measured on real hardware. | ~450 | ~1000 |
| | **Total** | **≈ 4900** | **≈ 4565** |

The estimate's real content is the sequencing. **PR 1 is the one that matters:** because
`Escape()` hosts unmigrated panel bodies unchanged, the shell ships and can be lived with
*before* a single panel is rewritten. If the user opens PR 1 over a game and finds two
slabs worse than one, the cost sunk is a week, not the redesign. Every later PR is
independently revertible, and PR 7's Frame probe is behind a convar precisely because it
is the red item.

Two things are cheaper than they look: `Widgets.cpp` moves almost intact (the measured
slider spec, already signed off, is preserved to the pixel), and four tab bars plus six
floating windows are *deleted*, not ported. Two things are more expensive than they look:
the Frame probe (§5), and Monitor — folding six tabs into one accordion category with a
Stage composite is an information-architecture rewrite, not a re-layout.
