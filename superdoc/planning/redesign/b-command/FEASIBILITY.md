# Feasibility — direction B against immediate-mode ImGui 1.92.9b

Honest assessment. Grounded in this tree: `src/Overlay/*`, `src/SettingsOverlay.cpp`,
`src/wlserver.cpp`, stock non-docking Dear ImGui 1.92.9b. Two things in this direction are
load-bearing and therefore get the most scrutiny: **text input** (§2) and **gamepad** (§3).

---

## 1. Summary table

| Area | Verdict | Notes |
|---|---|---|
| One window instead of six | **Easier than today** | deletes the whole multi-window Z/focus/position problem |
| Uniform row grid | **Easy** | `ItemAdd`/`ItemSize` on a fixed BB; this is ImGui's happy path |
| `ImGuiListClipper` over 147 rows | **Easy** | uniform height is a taxonomy guarantee, which is exactly what the clipper wants |
| Fuzzy match, 150 entries per keystroke | **Trivial** | < 50 µs; cached per query anyway |
| Match-character highlighting | **Moderate** | needs per-glyph `CalcTextSize` walk + `AddText` runs; ~60 lines, done once in `RowRender` |
| Mini-track in a result row | **Easy** | it is `SliderBehavior()` on a smaller BB; the `GrabMinSize` invariant from `slider-widget-spec.md` §3 applies unchanged |
| Row expand on selection | **Moderate** | breaks clipper uniformity; solved by reserving the delta (see §4.2) |
| Query line | **Moderate — hand-rolled, not `InputText`** | see §2. ~120 lines |
| Stream pane (LOG) | **Easy** | clipper over `LogCapture::Snapshot()`; already how `PanelLog.cpp` works |
| Gauge pane | **Easy** | `ImDrawList` cards; the drawing already exists in `FpsDisplay.cpp` |
| Chip stack in the query line | **Easy** | `ButtonBehavior` on measured text boxes |
| Selection cursor animation | **Easy** | one float lerped against `io.DeltaTime`, drawn as a rect before rows |
| Quick Wheel (radial) | **Hard-ish, self-contained** | pure `ImDrawList` in a second, borderless window; ~350 lines |
| Letter wheel | **Moderate** | 27 `ButtonBehavior` cells in a 3×3×3 layout; ~150 lines |
| Gamepad input plumbing | **Net-new, non-trivial** | nothing exists today; see §3 |
| Clipboard (`^C`) | **Broken today, must be fixed** | see §5.3 |

---

## 2. `InputText`: what it actually does, and why the query line will not use it

The query line is the single most important widget in this direction, so this needs to be right
rather than optimistic.

### 2.1 What ImGui's `InputText` really gives you

- **One active instance at a time.** State lives in `ImGuiContext::InputTextState`, keyed by the
  widget ID. Fine for us — we only ever have one.
- **It takes ownership of keys while active.** `InputTextEx()` calls
  `SetActiveIdUsingAllKeyboardKeys()` (key-ownership routing in 1.90+), which is what stops
  keyboard *nav* from stealing arrows/Enter/Escape/Tab from a focused field. That is correct
  behaviour for a form field and directly hostile to a palette, where those keys mean
  move/adjust/open/scope/unwind.
- **Up/Down have a supported escape hatch.** `ImGuiInputTextFlags_CallbackHistory` fires the
  user callback with `EventKey == ImGuiKey_UpArrow / DownArrow` — this is exactly the mechanism
  ImGui's own console demo uses, and it works. **Up/Down are not the problem.**
- **Left/Right have no escape hatch.** There is no `CallbackNav`, no per-key flag, no
  "don't consume horizontal movement" option. They always drive the stb_textedit caret. The only
  workarounds are (a) `CallbackAlways` + detecting and rewriting `data->CursorPos`, which fights
  the widget and cannot distinguish a keyboard caret move from a mouse click, or (b) taking key
  ownership back with `SetKeyOwner()` before the call, which is undocumented territory that
  changes between releases.
- **Escape's default is wrong for us.** Esc on an active `InputText` *reverts the buffer to its
  value at activation* and deactivates. We want Esc to mean clear → pop chip → close.
  `ImGuiInputTextFlags_EscapeClearsAll` changes it to "clear", which is closer but still not the
  three-stage unwind, and it still deactivates.
- **Tab is consumed** for focus movement unless `AllowTabInput`, and `↹` is our scope key.
- **Selection machinery we do not want.** Shift+arrows, double-click word select, Ctrl+A,
  drag-select, undo/redo stack — all present, all stealing keys, none useful for a 20-character
  query.
- **IME: it buys us nothing here.** `InputText` positions the IME candidate window through
  `io.SetPlatformImeDataFn`. Our overlay's ImGui context has **no platform backend** — 
  `SettingsOverlay.cpp` hand-feeds `ImGuiIO` from a queue and only uses
  `ImGui_ImplVulkan` for rendering. `SetPlatformImeDataFn` is null. gamescope's own IME
  (`src/ime.cpp`) drives the Wayland text-input protocol *for game clients*, not for this ImGui
  context. So there is no IME either way; `InputText` does not change that.

### 2.2 Decision

> **The query line is hand-rolled. `ImGui::InputText` is used in exactly one place: the Text
> pane (profile naming), where full editing is genuinely wanted and no key conflicts exist.**

This is a smaller decision than it sounds, because our requirements are narrow: a single-line
buffer with append / backspace / word-delete / clear, a blinking caret, no selection, no undo.
The input source already exists and is already correct:

```cpp
// wlserver.cpp -> SettingsOverlay_QueueKeyEvent(keycode, pressed, sUtf8Text)
//   sUtf8Text resolved by xkb_state_key_get_utf8() against the real keyboard
// SettingsOverlay.cpp:1421 -> io.AddInputCharactersUTF8( sUtf8Text.c_str() )
```

So `io.InputQueueCharacters` is already filled with **layout-correct** characters every frame.
The query line reads that vector, appends printable code points, and handles Backspace/Ctrl+W
via `IsKeyPressed()`. Roughly:

```cpp
void Console::DrawQueryLine()
{
    ImGuiIO &io = ImGui::GetIO();
    for ( ImWchar c : io.InputQueueCharacters )
        if ( c >= 0x20 && c != 0x7F )
            AppendUtf8( m_sQuery, c );
    io.InputQueueCharacters.resize( 0 );          // we are the sole consumer

    if ( ImGui::IsKeyPressed( ImGuiKey_Backspace, true ) )
        io.KeyCtrl ? DeleteWordBack( m_sQuery ) : PopUtf8( m_sQuery );
    // arrows / Enter / Tab / Escape are read normally, by the console, unowned.
    ...
}
```

**Cost:** ~120 lines including caret blink, UTF-8 append/pop, and the chip stack's backspace
interaction. **Benefit:** every key means exactly what the design says it means, with no
ownership fights and no version-fragile workarounds.

### 2.3 Honest limits of the hand-rolled line

- **No text selection, no mouse caret placement, no drag-select.** Acceptable for a query;
  documented.
- **No compose/dead-key sequences.** `xkb_state_key_get_utf8()` resolves single keypresses, not
  `xkb_compose_state` sequences. On a layout where `é` is one key it works; where it is
  `´` + `e` it does not. **This is a pre-existing limitation** — the profile-name `InputText`
  today has exactly the same one, because the limit is in the producer, not the consumer. Worth
  a future `xkb_compose_state` addition in `wlserver_dispatch_key()`, which would fix both.
- **No CJK/IME.** Same as today. A search-first UI in a non-Latin locale falls back to browse,
  which is a complete path — this is precisely why browse is not allowed to be a second-class
  citizen in this design.
- **Paste (`^V`)** needs `io.GetClipboardTextFn`, which is stubbed (§5.3). Low priority for a
  query line.

---

## 3. Gamepad — the honest assessment

### 3.1 Starting point: there is nothing

- `ImGuiConfigFlags_NavEnableGamepad` is **never set** anywhere in the tree
  (`SettingsOverlay.cpp` sets only `NavEnableKeyboard`, and only when
  `cv_settings_overlay_keyboard_nav` is on).
- `ImGuiBackendFlags_HasGamepad` is never set.
- `SettingsOverlay.h`'s producer API accepts keyboard, mouse motion, mouse buttons and wheel.
  **There is no pad event kind at all.**
- gamescope has no evdev/libinput joystick reader. The only `evdev` references are keycode
  conventions and `LibInputHandler` (keyboard/pointer on the DRM backend).

**Therefore gamepad support is net-new work under every one of the five directions.** This
direction does not inherit a deficit; it does, however, *depend* on the work more visibly, so it
must be scoped rather than assumed.

### 3.2 What the plumbing costs

1. **A pad reader.** `libevdev` (or raw `/dev/input/event*` + `EVIOCGBIT`) enumerating devices
   with `BTN_SOUTH`/`ABS_X`, hot-plug via the existing udev monitor gamescope already runs for
   the DRM backend. ~250 lines. Steam Deck and any XInput-class pad are covered by the standard
   `BTN_*`/`ABS_*` mapping; exotic pads are not, and that is acceptable.
2. **A new producer entry point**, `SettingsOverlay_QueuePadEvent( uCode, flValue )`, mirroring
   the existing key/mouse queue exactly. ~40 lines, same pattern, same thread discipline.
3. **Consumer mapping.** Two choices, and this direction takes the second:
   - Set `NavEnableGamepad` + `HasGamepad` and feed `io.AddKeyEvent(ImGuiKey_GamepadDpadUp, …)`
     etc., letting ImGui's own nav drive. **Rejected**: ImGui nav is a generic
     "move focus between items" model. Our `←→ = adjust the selected value` semantic is not what
     it does — nav's Left/Right moves focus horizontally, and adjusting a slider requires
     entering it first (`ImGuiNavInput_Activate`, then tweak mode). That is exactly the modal
     behaviour §3.2 of `SPEC.md` promises to eliminate.
   - **Map pad events to our own five verbs directly**, in `Console`, and leave ImGui nav off.
     The console already owns its selection index; a pad event is just another way to change it.
     ~80 lines. This is strictly simpler *because* there is one uniform row list — there is no
     spatial nav problem to solve, no "which widget is to the right of this one".
4. **Blocking the pad from the game while the overlay is open.** Same shape as the existing
   `SettingsOverlay_IsCapturingInput()` gate in `wlserver.cpp`, but for the pad device. This is
   the one genuinely fiddly bit: gamescope does not currently forward pad input at all (the game
   opens `/dev/input` itself, or gets it via Steam Input), so "capturing" a pad may mean grabbing
   the evdev device with `EVIOCGRAB` while the overlay is open, which is heavier-handed than the
   keyboard path. **Risk, flagged.** Mitigation: the Quick Wheel (`L2`, §3.1 of `SPEC.md`)
   deliberately does *not* capture — it reads the pad passively and lets the game keep receiving
   it, which is why the game keeps running under it. Only Browse posture grabs.

### 3.3 Is the resulting gamepad experience actually good?

Assessing my own direction critically:

**Good:**
- The Quick Wheel is genuinely better than anything the current dock offers. Eight settings,
  no typing, ~1 s, game still running. This is the interaction a Steam Deck user actually wants,
  and it exists *because* the registry knows what a "pinnable, steppable value" is.
- `←→` adjusting in place with no enter/leave mode is a real ergonomic win, and it is only
  possible because the taxonomy forces every type to define a signed step. A free-form panel
  design cannot promise this.
- Browse posture is a strictly better list than today's six panels, because the ordering,
  grouping and geometry are generated rather than hand-built.

**Weak, and I will not pretend otherwise:**
- **The letter wheel is a compromise.** ~0.6 s/character is fine for 2–3 characters and tedious
  beyond that. The p90 ≤ 3 characters claim is defensible *and enforced* by
  `Registry::SelfTest()`, but it is a claim about the current 147 entries; a future area with
  many similar names would degrade it. The self-test is what keeps that honest, but it is a
  guard rail, not a guarantee of a pleasant experience.
- **Search is genuinely secondary on a pad.** If the user's mental model is "the overlay is a
  search box", a pad user does not get that mental model — they get a browser. The design's
  defence is that browse is the *same* list with the *same* rows, so nothing is missing and
  nothing looks different. But the *pitch* of the direction is weaker on a pad than on a
  keyboard. That is a real, honest cost of choosing this direction.
- **The Quick Wheel needs a binding that does not collide with the game.** `L2` is a trigger
  most games use. Realistically this must be `L4`/`R4` on a Deck, or a chord (`Select+L2`), or
  user-configurable — which means another setting, which means a chicken-and-egg for a first-run
  pad user. Mitigation: the first-run notification (which already exists —
  `DrawStartupAnnounce()`) states the binding.

---

## 4. Specific ImGui hazards, and how each is handled

### 4.1 ID stability across a filtering list — **the highest-risk item**

Every keystroke changes the set and order of rows. If row IDs derive from the loop index,
ImGui's `ActiveId` (a hash of the ID stack) can silently transfer from one widget to a different
one mid-interaction: a slider being dragged would "become" the next query's slider at the same
index and keep dragging *that* value. This is a real, nasty, hard-to-reproduce bug class.

**Handled:** `ImGui::PushID( entry.szId )` — the stable registry id, which by construction is
unique and never index-derived. `ActiveId` then belongs to `display.sharpness` regardless of
where it currently sits in the list. Registration asserts id uniqueness, so the invariant is
enforced at startup, not hoped for.

Second-order: if a query change removes the row that owns `ActiveId`, ImGui clears `ActiveId`
naturally the frame the item is not submitted. Correct behaviour — the drag ends.

### 4.2 `ImGuiListClipper` vs. the expanded row

The clipper assumes uniform item height. The selected slider row is 14u, not 10u.

**Handled:** the console knows which index is selected, so it clips at 10u over
`nRows` items and adds a single 4u spacer item when the selected row is inside
`[DisplayStart, DisplayEnd)`. `ImGuiListClipper::IncludeItemByIndex( m_nSel )` (present in
1.90+) additionally guarantees the selected row is always submitted even if scrolled out, which
is what keeps a mid-drag slider alive when the list scrolls under it.

Fallback if this proves awkward: drop the expand animation and always draw the expanded form for
the selected row at a fixed 14u, making *every* row 14u. Costs density; costs nothing else.

### 4.3 Fonts: atlas rebuild is deferred past the requesting frame

`fonts::RebuildAll()` defers, and `ApplyPendingRebuild()` must run before `NewFrame()` — already
handled in `SettingsOverlay.cpp:944`. Two new roles (`Query` 20px Mono 400, `Stream` 12.5px Mono
400) and one removed (`DockHotkey` 8px). Net atlas size is roughly neutral. **No new risk**;
this machinery is already correct and already scale-aware (`Fonts.cpp` bakes at effective scale,
`BuiltScale()` reports it).

One caveat: the Query role at 20px × `display_scale` 2.0 = 40px baked glyphs. Fine — the
existing `Hero` role already bakes 18 × 2.0 = 36px.

### 4.4 Match highlighting

ImGui's `AddText` draws a run; to tint individual characters you must split the run. Approach:
walk the label once with `ImFont::CalcTextSizeA` per segment, emit alternating `AddText` calls
with the two colours, and draw a 1.5u underline rect under highlighted spans. Cost is O(matched
spans), typically 1–3 per row, on ≤ 30 visible rows. **Cheap, but genuinely custom `ImDrawList`
work** — ~60 lines, written once in `RowRender.cpp`, and the reason it must live there rather
than at call sites.

Cheaper fallback if profiling ever complains: highlight only the first matched span. Not needed
at these sizes.

### 4.5 Animation

No animation system exists; everything is manual against `io.DeltaTime`. Four animations, one
helper:

```cpp
inline void Approach( float &fl, float flTarget, float flRate, float flDt )
{
    fl += ( flTarget - fl ) * ( 1.0f - expf( -flDt * flRate ) );
}
```

Exponential smoothing rather than `lerp(a, b, dt * k)` — the naive lerp changes perceived speed
with frame rate, which in a compositor overlay running anywhere between 30 and 240 fps is a
visible defect, not a nitpick. **Easy, but easy to get subtly wrong**, which is exactly why the
helper owns it and `PaneCtx` exposes no timing primitive.

### 4.6 Clipping

`PushClipRect` for the body's scroll region and for label ellipsis. Standard. The one subtlety:
the selected row's expand overdraws into space reserved by the clipper — the clip rect must be
the body region, not the row BB, or the expanded track gets cut. ~5 lines of care.

### 4.7 The Quick Wheel is a second window

Drawn in its own borderless, input-transparent ImGui window at full output size, or (cleaner)
straight onto `ImGui::GetForegroundDrawList()`. It must render while `bDrawPanels` is false —
i.e. the overlay layer must be composited without the console being open. `SettingsOverlay.cpp`
already handles exactly this case for `DrawStartupAnnounce()` (layer at full opacity, panels
not drawn), so the plumbing shape exists. **~350 lines of `ImDrawList` trigonometry**, entirely
self-contained, no ImGui feature dependencies.

---

## 5. Pre-existing problems this direction has to touch

### 5.1 Seven stock sliders in the System Monitor
`slider-widget-spec.md` §8 records that `FpsDisplay.cpp`'s panel sliders never migrated to
`widgets::SliderControl()`, plus a `SetStockSliderFullWidth()` workaround. This direction deletes
those call sites entirely (they become registry entries), which **resolves issue #59's premise
rather than performing it**.

### 5.2 `display_scale` reaching hand-drawn geometry
Already solved (`palette::DisplayScale()`, issue #24). The `u()` helper is a thin rename of the
existing pattern; no new risk.

### 5.3 `ImGui::SetClipboardText` does not reach the system clipboard — **existing bug**
`PanelLog.cpp:132` calls it, but no `io.SetClipboardTextFn` is installed. ImGui's default handler
stores the string in `ImGuiContext::ClipboardHandlerData` — i.e. the "Copy to clipboard" button
copies into ImGui's own memory and nowhere else. Anything pasted outside gamescope gets nothing.

This direction promises `^C` on log lines and entry ids, so it must fix it: install
`io.SetClipboardTextFn` routed to a wlserver `wl_data_source` on the seat. **~80 lines, new
work, worth doing regardless of which direction is chosen.**

### 5.4 Keyboard capture is a user setting
`cv_settings_overlay_keyboard_nav` / `OverlaySettings` can turn keyboard capture *off*, and
`wlserver_dispatch_key()` then forwards keys to the game. With a search-first overlay, that
setting would make the primary interaction silently dead. **Resolution:** the console forces
keyboard capture while open; the setting is repurposed to control only whether the *game*
continues receiving keys in the background, and the Inspector says so. Small, but it must be
decided, not discovered.

---

## 6. Migration cost

Measured against the current tree (10,384 lines in `src/Overlay/`).

### Removed or absorbed

| File | Lines | Fate |
|---|---:|---|
| `PanelDisplay.cpp` | 778 | → ~70 lines of declarations in `Areas/Display.cpp` |
| `PanelShaders.cpp` | 367 | → ~55 lines in `Areas/Shaders.cpp` |
| `PanelAudio.cpp` | 431 | → ~45 lines + one List pane in `Areas/Audio.cpp` |
| `PanelConfig.cpp` | 914 | → ~90 lines + a Confirm pane in `Areas/Profiles.cpp` |
| `PanelLog.cpp` | 185 | → ~15 lines (the 9-line Stream pane + registration) |
| `FpsDisplay.cpp` panel half (≈ L1888–2469) | ~580 | → ~80 lines + one Gauge pane |
| `Chrome.cpp` dock + window chrome | ~900 of 1627 | deleted; icon drawing (~500) and the SVG-transcribed set are kept |
| `Widgets.cpp` `Checkbox`, `BeginGroupBlock`, `ReadoutStrip` | ~200 of 942 | deleted; `SliderControl`/`Toggle`/`SegmentedControl`/`PositionGrid` bodies are reused inside `RowRender.cpp` |
| **Total removed** | **≈ 4,350** | |

### Added

| Component | Lines (est.) |
|---|---:|
| `Registry.h/.cpp` (Entry/Area/builders/asserts/SelfTest) | 400 |
| `Bind.h` | 90 |
| `Match.h/.cpp` (fuzzy + highlight positions) | 150 |
| `Console.h/.cpp` (query line, chips, body, inspector, legend, key handling) | 650 |
| `RowRender.cpp` (twelve types; the only geometry in the tree) | 700 |
| `PaneCtx.h/.cpp` (rows, cards, gauges, sparkline, stream, notes) | 480 |
| `Theme.h/.cpp` (roles, `u()`, type roles, the four animations) | 260 |
| `Areas/*.cpp` (six files of pure declaration) | 400 |
| Clipboard routing (§5.3) | 80 |
| **Total added, no gamepad** | **≈ 3,210** |
| Gamepad: evdev reader + producer + verb mapping | 370 |
| Quick Wheel | 350 |
| Letter wheel | 150 |
| **Total added, with gamepad** | **≈ 4,080** |

**Net: roughly −1,100 lines without gamepad, roughly −270 with it** — while gaining search,
command palette, addressable ids for `gamescopectl`, per-entry reset, pinning, and a gamepad
path that does not exist today.

### Sequencing (each step ships independently)

| Step | Content | Risk |
|---|---|---|
| **1** | `Theme.h`, `Registry`, `Bind`, `Match`, `RowRender` for 5 types; `Console` rendering a hard-coded area list; **no search yet** | low — nothing user-visible changes |
| **2** | Query line, fuzzy search, chips, Inspector. Console opens on `Ctrl+Shift+O` **alongside** the existing dock | low — both UIs coexist; this is the real A/B |
| **3** | Migrate areas one at a time: Display → Shaders → Audio → Profiles → Monitor → Log. Each migrated panel's dock button disappears | low, incremental, revertible per area |
| **4** | `PaneCtx` + the four panes (Stream, Gauge, List, Confirm) | medium — the Gauge pane must reproduce the Statistics tab |
| **5** | Delete the dock, `Chrome`'s window chrome, and the dead widgets | low, mechanical |
| **6** | Gamepad: reader → producer → verbs → Browse posture | **medium-high** — the `EVIOCGRAB` question (§3.2.4) is the unknown |
| **7** | Quick Wheel, letter wheel, `SelfTest()` search-quality gate | medium, self-contained |

Steps 1–3 are where the user's actual complaint gets fixed. Steps 6–7 are where this direction's
distinctive bet gets paid off, and they can be deferred without the rest losing value.

---

## 7. The three things most likely to go wrong

1. **`ActiveId` transfer across a re-filtering list** (§4.1). Mitigated structurally by stable
   string IDs, but if any code path ever derives an ID from an index, the bug is subtle and
   intermittent. *Guard: a debug assert in `RowRender::Draw()` that the current ID stack hash
   matches `ImHashStr(entry.szId)`.*
2. **Gamepad capture semantics** (§3.2.4). Grabbing an evdev pad away from a running game is
   heavier than the keyboard path and interacts with Steam Input in ways this tree has never
   exercised. *Guard: ship the Quick Wheel (passive, no grab) before Browse posture (grab), so
   the valuable half does not depend on the risky half.*
3. **Density at `display_scale` 2.0.** 10u rows at 2× is 80px; a 1080p output fits ~9 rows in the
   body. Browsing a 24-entry area then means real scrolling. *Guard: the group-header jump
   (`L1`/`R1`, `PgUp`/`PgDn`) exists precisely for this, and the ≥ 1.6× branch already widens the
   control column rather than the rows. If it still reads badly in practice, the compact row can
   drop to 9u above 1.6× — a one-line change in `Theme.h`, and only there.*
