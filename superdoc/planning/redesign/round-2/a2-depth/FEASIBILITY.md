# Feasibility — A2 "Console + Ledge" in immediate-mode ImGui

Dear ImGui **1.92.9b, stock non-docking branch**. Manual layout, `ImDrawList`, animation
via `io.DeltaTime`. This file assesses only the **deltas from `a-console/FEASIBILITY.md`** —
everything that document called easy, medium or hard is unchanged unless listed here.

**Verdict up front:** the Ledge is the cheapest structural change in any of the three
directions. It is *one more fixed-height band inside the window we were already drawing*,
plus one `BeginChild` and one float of animation. Two of A's four named risks are
**eliminated** by this refinement rather than mitigated, and the gamepad drop removes a
third. The one genuinely new risk is small and has a designed-in fallback.

---

## 1. What gets easier

### 1.1 The Ledge itself is a band, not a region

A already draws a fixed-height legend band at the bottom of the same `ImGui::Begin()`
window (`a-console/API.md` §4.3, `DrawLegend()`). The Ledge occupies the same rect. The
resting state is:

```cpp
ImDrawList *dl = ImGui::GetWindowDrawList();
dl->AddRectFilled( tl, br, Theme::Elev1() );
dl->AddLine     ( tl, ImVec2( br.x, tl.y ), Theme::Hair() );
dl->AddText     ( … , Theme::TextMeta(), szHelpClipped );
```

Three draw calls plus the right-hand Mono strip. This is strictly *less* work than the
legend it replaces, which drew five key chips with borders.

The raised state adds one `BeginChild` for scroll (the second and last child in the whole
design — the stage is the first), and inside it calls the **same** `Rows::Draw()` the stage
uses. No new renderer, no new grammar, no new theme tokens.

**Height animation:** one float, `Approach()`ed at 16/s, same helper as A's four motions.
The grid is `header / stage / ledge` with the stage taking the remainder, so growing the
Ledge shrinks the stage automatically — no relayout code, and no case where the console
changes size.

**Cost: low. Risk: low.** Estimated 180–260 lines for `Ledge.cpp` including the section
card and the log-line decode.

### 1.2 Help sub-lines are gone → uniform row height → `ImGuiListClipper` works

A's `Opt::pszHelp` made rows grow to content (`a-console/SPEC.md` §2.1: *"With a help
sub-line they grow to the content"*). Variable row height means no clipper, which means
every row on a screen is laid out every frame. Moving help to the Ledge makes **every row
exactly 11u**, so the stage can clip like the LOG does. Not a performance need at 21
screens, but it removes the one place the taxonomy and the renderer disagreed.

### 1.3 The wide-vs-narrow control column is deleted

`a-console/FEASIBILITY.md` §6 flagged a real hazard: `Choice()` picked segmented-vs-picker
and narrow-vs-wide by *measuring* option labels at draw time, so a font-atlas rebuild frame
could flip the decision mid-drag. The fix it proposed was hysteresis.

**A2 does not need the fix, because it does not make the measurement.** One control column
width (SPEC §2.2); segmented cells are `flex:1` inside it; the segmented-vs-picker choice
is `options.size() <= 5 && every label <= 10 chars`, evaluated once at *registration*, not
per frame. A whole class of frame-dependent layout is gone.

### 1.4 Gamepad is dropped → A's risk #4 is closed, and §2.1 gets simpler

A's `NavStep()` (`a-console/API.md` §4.4) read four keys — two gamepad, two keyboard — and
A's `FEASIBILITY` §2.1 rated the fight against ImGui's `NavMoveRequest` as low-medium risk,
with a "budget half a day" note. With gamepad gone the helper halves:

```cpp
int NavStep()
{
    if ( !ImGui::IsItemFocused() ) return 0;
    const int n = ImGui::GetKeyPressedAmount( ImGuiKey_RightArrow, 0.35f, 0.06f )
                - ImGui::GetKeyPressedAmount( ImGuiKey_LeftArrow,  0.35f, 0.06f );
    if ( n ) ImGui::NavMoveRequestCancel();
    return n;
}
```

The residual risk (ImGui consuming arrows for a wrap move at a scroll edge) is unchanged
and still needs QA at the top and bottom of a scrolled list, but the surface is smaller and
`io.ConfigNavMoveSetMousePos = false` plus `NavMoveRequestCancel()` is the whole recipe.

A's risk register item #4 was *"confirm gamescope actually routes a gamepad into the
overlay's ImGui context — a 30-minute answer that shapes everything else."* That question
is now moot: gamescope has no gamepad input path (libinput ignores gamepads), and the
design no longer wants one.

### 1.5 Sub-tabs as an underline run

Cheaper than the pill buttons: one `AddText` per tab plus one `AddRectFilled` for the
sliding underline, versus a fill + border + text per tab. The slide is one more float in
`ConsoleAnim` (six total for the whole UI, up from five). No `BeginTabBar` anywhere, which
was already the plan.

### 1.6 The rail icon-shift fix is arithmetic

`rail_collapsed_w = 2 × (stage_pad_x + icon_w/2)` — both terms are already in `Metrics`.
The icon is drawn at `x = stage_pad_x` in the expanded state and at
`x = (rail_w - icon_w)/2` in the collapsed one, and the derivation makes those equal. The
label and count fade by alpha against the same animated factor and stop drawing under 0.05
(A's `FEASIBILITY` §4.3, now mandatory rather than a note). A one-line assert in
`ui.audit` compares the painted centre in both states.

**Cost: an hour. Risk: none.** It is a constant chosen by equation instead of by eye.

---

## 2. What is new and needs care

### 2.1 Focus must be known before the Ledge draws

The Ledge describes the focused row, but immediate mode has no "what is focused" query
after the fact. Solution (API §3.1): `EndRow()` writes `Ledge().pPendingSubject = &entry`
when `rf.bFocused`, the stage draws before the Ledge, done. One pointer, written at most
once per frame, cleared at frame top.

**The trap:** if the focused row is scrolled out of view, `ItemAdd` culls it and `EndRow`
never runs, so the Ledge would go blank exactly when you scrolled — the worst possible
moment. Two fixes, use both:

1. Nav-follow already keeps the focused row on screen (`SetScrollHereY` when focus moves),
   so the culled case only arises from *mouse wheel* scrolling with focus elsewhere.
2. Fall back to the last non-null subject rather than to null. The Ledge is allowed to
   describe a row you scrolled past; it is not allowed to be empty. `pPendingSubject` is
   only cleared when focus genuinely moves to the rail, a sheet, or the palette.

**Cost: low. Risk: low, but it is the bug that will ship if this paragraph is not read.**

### 2.2 Nested focus scope: `Ctrl+↓` into the Ledge

Focus moving from a stage row into a Ledge row, with `Esc` returning it, is two focus
scopes in one window. Do **not** poke `g.NavWindow` — internal and version-fragile
(A's `FEASIBILITY` §2.2 makes the same call about the sheet).

**Recipe:** keep a single `bFocusIn` flag in `LedgeState` and gate the two row loops on it.
When `bFocusIn` is true the stage's rows are drawn with `ImGuiItemFlags_NoNav` pushed and
the Ledge's rows without it; when false, the reverse. ImGui's nav then only ever sees one
navigable set, so `↑↓` inside the Ledge and `↑` off the top (which clears `bFocusIn`) are
ordinary nav within one scope that happens to change membership between frames. The
originating row still paints its dimmed "parent" tick because painting is independent of
navigability.

**Cost: low–medium. Risk: low.** The one thing to QA: pressing `↑` off the top of the
Ledge in the same frame the flag flips must not double-move focus. Consume the key.

### 2.3 The raised Ledge is a second scroll region

`BeginChild` inside a window that already contains a `BeginChild` (the stage) is fine in
stock ImGui — they are siblings, not nested. The only interaction is that mouse-wheel
routing goes to whichever child is hovered, which is the correct behaviour and comes free.

### 2.4 `Detail()` text wrapping and measurement

The paragraph wraps to the Ledge's width. `ImGui::TextWrapped` inside the child is stock,
but the design wants inline emphasis (`*aces*` rendered in `text/label` inside a
`text/meta` run). ImGui has no rich text.

**Decision: do not build a markup renderer.** Split the string on `*` at load time into
runs, and draw runs with `ImDrawList::AddText` advancing the cursor by
`CalcTextSize(run).x`, wrapping at the last space that fits. That is ~40 lines and it only
has to handle one toggle (emphasis on/off), no nesting, no links inside prose — links are
`Related()` chips, which are separate widgets.

**Cost: low–medium (~half a day). Risk: low.** Fallback if it is not worth it: strip the
`*` markers and draw one flat `TextWrapped`. The prose still reads; it just loses the
emphasis on option names. Ship the fallback first if the schedule is tight.

### 2.5 The palette's query line

Unchanged from B's assessment: hand-rolled, reading `io.InputQueueCharacters`, which
wlserver already fills with layout-correct UTF-8 via `xkb_state_key_get_utf8()`. Do not use
`InputText` for it — the query line has no frame, no selection, and needs to coexist with
`↑↓` and `←→` being routed to the result list rather than to the caret.

**Cost: low.** ~120 lines including the fuzzy scorer and match-highlight positions.

### 2.6 The palette's index is complete, unlike A's Find

A's Find had to index lazily (`a-console/FEASIBILITY.md` §2.5): screen draw functions are
not side-effect-free (`PanelShaders` calls `EnsureEffectLoaded()`, `PanelAudio` enumerates
PipeWire), so a probe pass would trigger all of it, and a setting on a screen you had never
visited was not findable.

**The registry removes that problem entirely.** Registration happens once at startup and
touches no draw function; the palette indexes declarations, not renders. `Ctrl+K → "toe
strength"` works on a cold start. This is the single biggest functional argument for
adopting B's registry, and it is worth more than the palette itself.

The cost is real and should be stated: **registration must not depend on runtime state.**
`AvailableWhen()` predicates are evaluated per frame (fine), but a screen that today builds
its row list from a live PipeWire enumeration has to be restructured into a static
declaration plus a dynamic list *inside* a row. `PanelAudio.cpp`'s stream picker is the one
real case, and it becomes a `Drill` to a sheet whose contents are enumerated at open time —
which is what A's design already wanted.

---

## 3. Contrast enforcement (`ui.contrast`)

New, and cheap enough to be worth building rather than trusting review. The
implementation that actually works in immediate mode:

- `Theme::TextColor( Role, Surface )` is the only way a text colour is obtained.
- In debug builds it composites the role's `#EFF5FB @ alpha` over the surface's known
  effective colour, computes the WCAG ratio, and compares against the role's floor
  (4.5 for body roles, 3.0 for `faint` and UI boundaries).
- The "surface's known effective colour" is not sampled from the framebuffer — it is
  *computed* from the same chain the spec uses (compositor scrim → base → elevation lift),
  with the game assumed at maximum luminance. That is deliberately the worst case, so a
  pass is a pass on every game.

~60 lines, one ConVar, and it catches the entire class of bug that produced the user's
"some text is hard to read". `Palette.h` issue #62 was that bug reported by a human, twice.

**One honest limitation:** it validates roles, not the *compositor settings*. A user who
drags `Darkening` down to 0.2 makes the effective base much brighter and can push
`text/meta` under 4.5:1 themselves. Options: clamp `Darkening`'s lower bound to 0.55 (the
measured point where meta stops clearing 4.5:1), or warn in the Ledge when it is below
that. **Recommend the warning, not the clamp** — it is the user's screen, and the Ledge is
exactly the right place to say so.

---

## 4. What still fights immediate mode

| Item | The fight | Resolution |
|---|---|---|
| Knowing what has focus before drawing the Ledge | no retained tree to query | `EndRow` writes a pointer; stage draws first (§2.1) |
| Two focus scopes in one window | hand-rolled trapping needs internal nav state | one flag + `ImGuiItemFlags_NoNav` on the inactive set (§2.2) |
| Rich text in `Detail()` | ImGui has no markup | 40-line two-state run splitter, or drop emphasis (§2.4) |
| Cross-fading two stages | both screens drawn per frame; side effects double-fire | unchanged from A — don't; offset + alpha the incoming page only |
| Per-widget animation | needs an ID-keyed store | unchanged — **six** global floats for the whole UI |
| Registration must be static | today's panels build rows from live state | one real call site (`PanelAudio`), becomes a Drill + sheet (§2.6) |
| Cross-context HUD preview | atlases and draw lists are per-context | **unchanged from A, and still the only genuinely risky item** — see §5 |

---

## 5. The one carried-over risk: the System Monitor live preview

Nothing in this refinement changes `a-console/FEASIBILITY.md` §5. The HUD lives in its own
ImGui context (`FpsDisplay.cpp`, 2469 lines); previewing it in the console context needs
those module draws refactored to `Draw( ImDrawList*, ImVec2 origin, float flScale )`, and
the font-size question has no clean answer because the HUD's `font_size` is in
HUD-context pixels.

The recommendation stands and is reinforced: **build the schematic fallback first** — the
3×3 anchor grid over a representative backdrop with a to-scale outline box from the
existing `MeasureFpsModule()`, labelled *"approximate scale"* in the Well header. Two
hours, strictly better than today's no-preview, and it de-risks the whole System Monitor
port. The live preview is an upgrade, spiked separately, not a prerequisite.

The mockup shows the approximate-scale version, labelled as such, for exactly this reason.

---

## 6. Migration cost

Baseline: `a-console/FEASIBILITY.md` §7 estimated the kit at 1,800–2,300 lines and a net
**−1,400 to −1,800** lines across `src/Overlay/` once the port completes. A2's deltas:

| Component | vs A | Lines |
|---|---|---|
| `Ledge.cpp` (band, raised body, section card, log decode) | **new** | +180 … +260 |
| `Registry.{h,cpp}` (entries, binds, asserts, `SelfTest`) | **new** (adopted from B) | +320 … +420 |
| `Palette.cpp` (query line, fuzzy scorer, hit rows) | **new** | +180 … +240 |
| `Theme` contrast check | new | +60 |
| `Rows.cpp` — help sub-line measurement and growth | **deleted** | −60 |
| `Rows.cpp` — wide/narrow control-column measurement + hysteresis | **deleted** | −70 |
| `Console.cpp` — the static legend | **replaced by the Ledge** | −80 |
| `Console.cpp` — gamepad key paths across nav, rows and the legend | **deleted** | −120 … −160 |
| Screen files — `.Help()`/`.Detail()` strings | new prose, no logic | +250 … +400 (strings) |
| **Net vs A** | | **+660 … +1,010**, roughly half of it prose |

Against today's 10,384-line `src/Overlay/`, the port still lands **net negative** (about
−500 to −1,000 lines) while adding a registry, a palette and a documented help channel that
do not exist today at all.

**Effort:**

| Phase | Sessions |
|---|---|
| Registry + binds + asserts + `SelfTest` (before any UI) | 1 |
| Kit core: Metrics, Theme (+ contrast check), Rows through Switch/Slider/Readout | 1.5 |
| **Ledge: resting band, raised body, section card** | **0.75** |
| Shaders pilot end-to-end — proves the row grid *and* the Ledge on a real screen | 0.5 |
| Remaining row kinds + Sheet + `Expert()` hosting | 1 |
| Palette (query line, scorer, hit rows, `GoTo` into an expert) | 0.75 |
| Gamescope (4 screens) + Config (3 screens) | 1.5 |
| Audio + first Well, Log + first Wide + log-line decode | 1 |
| System Monitor (schematic Well) | 1 |
| Polish, 0.5×/2.0× QA, contrast pass, delete `Chrome.cpp`'s window code | 1 |
| **Total** | **~10 sessions** for a complete, shippable console |
| System Monitor live preview (`FpsDisplay` draw-list refactor) | +1–1.5, *optional, spike first* |

That is ~1.5 sessions more than A's estimate, and it buys the registry, the palette, and
the depth channel. The registry is front-loaded deliberately: doing it *before* the first
screen is ported means no screen is ever written twice.

**Risk register, ranked:**

1. **System Monitor live preview** — carried over from A, unchanged, mitigated by a
   designed-in schematic fallback (§5).
2. **`.Help()` / `.Detail()` are a writing task, not a coding one.** ~90 settings × two
   strings. The assert makes it non-optional, which is correct, but it will feel like drag
   during the port. Mitigation: `.Help()` alone satisfies the assert; `.Detail()` can land
   per-screen afterwards, and its absence degrades gracefully (the raised Ledge shows the
   facts and the sentence). Do not let this block a screen from shipping.
3. **`←/→` adjust vs ImGui nav** — smaller than in A (gamepad gone), still needs QA at
   scroll edges (§1.4).
4. **Focus subject going null on wheel-scroll** (§2.1) — one line, but it must be the
   *right* line.
5. **Everything else** — normal work.

---

## 7. What would make this fail

Stated so it can be checked against later, rather than rediscovered:

- **If `.Detail()` gets written as marketing prose.** The raised Ledge is worth having only
  if the paragraph says something the label does not. "Controls the sharpness of the image"
  under a row labelled *Sharpness* is worse than nothing, because it teaches the user that
  raising the Ledge is not worth doing. The mockup's strings are the intended register:
  what it actually does, what the failure mode looks like, and when to reach for the other
  setting instead.
- **If `Expert()` becomes a dumping ground.** Six per host is the cap in `SelfTest()`. Any
  screen that wants more is telling you it should have been two screens.
- **If the resting line grows to two lines.** It will be tempting. It is the whole
  mechanism: one line is a status bar and people ignore it comfortably; two lines is a
  panel and the console stops being calm. The cap belongs in the renderer, not in review.
