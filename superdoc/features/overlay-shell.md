# Overlay shell — the settings UI framework (`src/Overlay/UI/`)

The **Shell** (the user's "Click-UI", "the full/clickable overlay") is the settings overlay
proper: a slab with a rail, a sheet and an Inspector. This page documents the framework it is
built from, not any one area's content. Every setting is **declared once** in a registry, and the
sheet row, the command-palette entry and the Inspector content all come out of that one
declaration. The command palette and the Launcher are on their own page:
[command-palette.md](command-palette.md). Styling rules (what a new control or layout may and may
not do) are in [../ui/STYLING-GUIDE.md](../ui/STYLING-GUIDE.md). The design history (2026-08 to
2026-11, every round of user feedback) lives in `superdoc/planning/ui-design-guide.md`,
`superdoc/planning/redesign/` and the specs below; this page keeps the current contract and the
decisions that still matter.

## Files

| File | Role |
| --- | --- |
| `src/Overlay/UI/Registry.{h,cpp}` | The registry: `Registry`, `Area`, `Entry`, `Parameter`, `Kind`, `AnyBind`, the four laws, the rail table (`RailGroup`, `RailOrder()`), the accordion state machine. ImGui-free. |
| `src/Overlay/UI/Shell.{h,cpp}` | Everything drawn and every key handled: slab, rail (`DrawRail`), sheet (`DrawSheetBody`), Inspector (`DrawInspector`, `DrawConfigure`, `DrawDetails`, `DrawOverview`), palette. `Shell.h` is deliberately tiny (see "Threads"). |
| `src/Overlay/UI/Layout.{h,cpp}` | Region geometry as pure arithmetic: `Slab`, the responsive ladder `Solve()`, `Regions`, `LayOutSheetColumns()`, `ModeFor()`, `ScrollView`, `SolvePalettePanel()`. |
| `src/Overlay/UI/Lane.{h,cpp}` | `Lane`: the four columns of a row (label, value, control, affordance). |
| `src/Overlay/UI/Row.{h,cpp}` | `RowCtx`: the only control allocator, right-anchored. |
| `src/Overlay/UI/Band.{h,cpp}` | `Band()` / `LayOutBand()`: composite controls taller than one row. |
| `src/Overlay/UI/Controls.{h,cpp}` | The control atoms (`controls::Switch`, `Slider`, `Stepper`, `Choice`, `Dropdown`, `Text`, `Chord`, `Bank`, `Meter`, `Verb`, `ListBox`, ...), `ui::Modal`, the drawn `glyph::` set. |
| `src/Overlay/UI/Tokens.{h,cpp}` | Geometry, spacing, type roles, motion durations (`tok::`, `TypeRole`, `Ease`, `Approach`). |
| `src/Overlay/UI/Colors.{h,cpp}` | Colour roles (`Role`, `Col()`, `Accent()`, `Dim()`, `ScopedDim`). |
| `src/Overlay/UI/Icons.{h,cpp}` | The drawn rail icons (`IconFor()`, `IconForRailGroup()`). |
| `src/Overlay/UI/CommandPalette.{h,cpp}` | Palette matching and ranking (see the palette page). |
| `src/Overlay/Panel*.cpp`, `Crosshair.cpp`, `Zoom.cpp`, `Autoclicker.cpp`, ... | One `*_RegisterArea( Registry & )` per area. `Shell.cpp`'s `RegisterAll()` calls them. |

Not part of this framework despite the name: `src/Overlay/Palette.{h,cpp}` is the **colour-token
palette** (`gamescope::palette`: the accent family and the live hue), not the command palette.
`Colors.cpp` reads it to produce `Role` colours.

## The registry: one declaration per setting

`Registry::Add( id, title, Section )` returns an `Area`. An area is one rail item and one sheet.
Its factories are the complete taxonomy of what a row can be — there are no others:

```cpp
ui::Area &a = reg.Add( "input.general", "General", ui::Section::System );
a.Keywords( "input grab cursor keyboard mouse" );
a.Summary( []{ return std::string( "cursor grabbed" ); } );
a.Group( "Quick toggles" );
a.Switch( "input.force_grab_cursor", "Force grab cursor",
          ui::AnyBind::Of<bool>( []{ return g_bX; }, []( bool b ){ g_bX = b; QueueSave(); } ) )
    .Key( "gamescope.force_grab_cursor" )   // config key, for the inherited/overridden dot
    .Help( "What it does, in one or two sentences." )   // REQUIRED
    .Default( false )
    .Keywords( "mouse pointer capture" );
```

| Factory | `Kind` | Notes |
| --- | --- | --- |
| `Switch`, `Slider`, `Stepper` | same name | `.Range()`, `.Step()`, `.Unit()`, `.ZeroMeans()`, `.ValueText()` shape the value readout. |
| `Choice` | `Choice` | Draws segmented if it fits the lane, otherwise a dropdown, chosen by measurement; `.Dropdown()` forces the dropdown (for user-made or unbounded option sets). |
| `Text` | `Text` | `.Validate()`; `.Chord( action )` swaps the atom for the key-capture chip. |
| `Bank` | `Bank` | A mask of chips. |
| `Action` | `Action` | A verb chip; `.Confirm( prompt )` makes it two-press (destructive actions). |
| `Meter`, `Facts` | read-only | `IsReadOnly()`; no setter exists. |
| `Composite` | `Composite` | `CompositeKind`: `Anchor`, `Hue`, `Strip`, `Graph`, `Color`, `List`. |
| `Content( fn )` | content area | A body of lines (Log, About) beneath the rows; the call site returns data, never draws. |

Other per-row calls: `.Param( leaf, title, bind )` (inspector-depth parameters), `.Live( leaf, fn )`
(read-only Details readout), `.DisabledUnless( pred, reason )`, `.LockedByLaunchOption( pred, reason )`
([launch-option-lock.md](launch-option-lock.md)), `.Preview( PreviewKind )`, `.HideFromPalette()`,
`.InspectorVerb( label, verb, fn, fnShown )`, `.Key()`, `.Items()` / list verbs for `List`.
Area-level: `.Keywords()`, `.Summary()`, `.Badge()`, `.AvailableWhen()`, `.WarnWhen()`, `.Group()` /
`.GroupCount()`, `.Content()`, `.FollowsTail()`, `.RowsInInspector()`, and the dynamic-area builder
(`Area::Rebuilds()`, used by the Mixer, whose rows are one per live PipeWire stream).

### The four laws (enforced at registration)

`RegisterAll()` runs once at startup; a violation prints and aborts (`ReportViolation()`), so a
broken registry cannot reach a user. Tests catch violations with a `LawRecorder` instead.

1. **Prefix law** — a `Param`'s id is synthesised as `<parent>.<leaf>`; a caller cannot name an
   unrelated setting.
2. **Param budget** — one row owns at most `ParamBudget()` params (`kParamBudget`, `Registry.cpp`,
   currently 10). The enum value is still `Law::SixBudget` and the header calls it "the six
   budget" for historical reasons: the ceiling was raised 6 → 7 → 8 → 10 (last for Adaptive
   Brightness V2's darkening params), the name was not. `Why:` it is a ceiling on "how much a row
   can hide", not a magic number; the Inspector's "PARAMETERS n of N" header reads
   `ParamBudget()` live so a second hand-copied literal cannot disagree with it (it once said "of 6"
   after the budget moved to 7).
3. **Id uniqueness** — every Entry and Param id is unique registry-wide (`ClaimId()`).
4. **Help is required** — `.Help()` must be called with non-empty text (checked in `SelfTest()`,
   after the fluent chain is complete). A `DisabledUnless()` with an empty reason is its own
   violation (`Law::ReasonRequired`).

Two guarantees are enforced by the type system instead, which is stronger than a runtime check:
**one level** (`Parameter` is a different type from `Entry`, and its `Param()` is a *sibling*
factory forwarding to the owner, so a Param owning a Param is unsayable) and **Details is
read-only** (`.Live()` takes a `std::function<Fact()>` and has no `Bind` overload, so a control
cannot be constructed in Details). There is deliberately **no Inspector authoring API** — no
panel, no custom-draw callback. `Why:` an escape hatch that hands a call site the drawing takes
the layout away from the shell; the old `Area::Escape()` did that and was deleted once it reached
zero call sites. A new row kind means editing `Registry.h`, which shows up in a diff.
`Content()` is allowed because it hands the shell *data*; do not add an overload that takes a
draw callback.

The registry is ImGui-free on purpose: registrations are also reached from the console thread
(`overlay_e2_set`/`get`), where asking ImGui anything aborts. The pointer-drag flag
(`SetPointerDragActive()`) lives there for the same reason.

### Values, bindings, applying

`AnyBind::Of<T>( T* )` or `AnyBind::Of<T>( getter, setter )` type-erases a binding; `Value` is a
`std::variant<monostate, bool, int, float, std::string>`. `Applies` (`Live` / `NextFrame` /
`NeedsRestart`) is derived from the binding, never typed into a row. `Adjustable::Of( entry or
param )` plus `AdjustValue()` is the **one** stepping function used by sheet arrow keys, Inspector
arrow keys and the palette's in-place adjust. `Why:` three hosts with their own stepping code is
how two position-grid call sites once drifted apart; a slider must not step by 1 in the sheet and
by its whole range in the palette.

## Regions, the ladder, and the Inspector

The slab is a flat 85% of the surface on both axes (`Slab::For()`; `shelltok::kSurfFrac`), no
absolute pixel cap, centred, and cannot be moved — there is exactly one window. `Why:` 39% of the
old overlay's bug-fix commits were floating-window management, nine of sixteen of those being
fixes to earlier fixes; a bug class is removed by deleting what produces it (the dock and its five
floating windows were deleted in the redesign's P5). There is no `overlay_e2` ConVar any more: a
flag with one reachable value is a control that renders and does nothing.

Inside the slab, `Regions::For()` derives the rail, sheet (head, body, foot), Inspector and mode
strip from one `Slab` and one `LadderResult`, so no two regions can disagree about a boundary.
`Solve()` is the responsive ladder, applied once: the rail drops 232 → 60 base units (icons only,
`RailIsIcons()`), the Inspector goes **Column** → **Drawer** (overlays the sheet's right edge) →
**Hidden** (a 20-unit spine). The user's own Ctrl+I choice can only be overridden *downward*;
asking for Hidden is always honoured. `Why:` that keeps the Hidden step (and the Reachability Law,
below) exercised daily rather than only on a 2.0x machine. Sheet columns (1 to 3,
`LayOutSheetColumns()`) follow width and a rows-per-column cap; an area with a `Content()` body is
forced to one column.

An area can override both automatic rules. `Area::Columns( n )` states the wanted column count
instead of the rows rule (width still caps it, so a narrow sheet drops to 1);
`Area::WantedColumns()` collapses it to the columns that hold a *shown* row, so an area whose
second-column group is entirely `ShownWhen`-hidden lays out as one column. `Area::GroupColumn( c )`
pins the group just declared to column `c` (0-based; beyond the real count it falls back to the last
column); unpinned groups stay greedy and count the pinned weight. The pure placement is
`PackBlocks()` in `Layout.cpp`, with no pins identical to the old inline greedy pack. `Why:` greedy
packing alone cannot give an area under 13 rows a two-column layout, or keep Status in the right
column, which the user laid out on a visual workbench. Nothing uses these yet; panels opt in.
Likewise `Entry::Segmented()` lifts only the 8-character label rule of a Choice: the 5-option cap
and the `MeasureCells()` fit test stay, so a row too wide for its lane still becomes a dropdown.

The **Inspector** is one region in three hosts (`InspectorHost`) and shows the selected row in
one of two modes (`InspectorMode`): **Configure** (the row's own control, then its Params as
ordinary rows) and **Details** (a binding grid and the `.Live()` readouts). `ModeFor( Kind,
CompositeKind )` picks one from the selection, automatically and statelessly; arriving from the
palette on a Param opens Configure. With nothing selected the Inspector shows the **Overview**
(`DrawOverview`), the area's summary plus any Inspector-only verbs. `Why:` three modes became two
(2026-08-23 amendment); an Overview that replaces the strip entirely means an area is never an
empty right column.

**Reachability Law:** every setting must be reachable with the Inspector in any host. A palette
jump to a Param on a Hidden Inspector promotes the host to Drawer (`PaletteJump()`), because
landing on a setting that is not visible is the one failure the palette exists to prevent.

**Inspector-only verb** — `Entry::InspectorVerb( label, verb, fn, fnShown )` (2026-11-05): one
labelled verb chip drawn only in the Inspector, on the row's own Configure/Details page and in the
area's Overview, so it is reachable without selecting the row first. Enter/Space fires it
(Inspector focus; in the Overview, the first shown verb). Meant for a read-only (Facts) row whose
one related action does not deserve a sheet row. Not in the palette (read-only rows are excluded).
`Why:` the user, about Diagnostics' "open the Hyprland docs" button, which first sat in the sheet:
*"It should only show this button on the inspector rail on the right side"*. Used by
`PanelDiagnostics.cpp`.

**Inspector-only switch** — `Entry::InspectorSwitch( label, fnGet, fnSet )` (2026-11-07): one bool
switch on the row's CONFIGURE page, directly under the Inspector verb, drawn by
`DrawInspectorSwitchRow()` in `Shell.cpp` in the Row grammar's lane with `controls::Switch`. The
row stays a readout, so the "nothing here to set" sentence is skipped for it. Not in the sheet or
the palette; the setter persists itself. `Why:` Diagnostics' "Silence warning" belongs on the Direct
scanout row's Inspector (the user marked the spot on a screenshot).

**Row-level hide** — `Entry::ShownWhen( fn )` (2026-11-07): the row exists only while `fn()` is true.
`Area::EntryCount()` and `EntryAt()` return the shown rows only, so the sheet, the column row count
(`Solve()`), keyboard navigation and the command palette all treat a hidden row as undeclared; the
answer may change at run time. `Why:` the Compatibility area's "Block direct scanout" is Wayland-only
([diagnostics.md](diagnostics.md#block-direct-scanout-switch-displayblock_direct_scanout)) and must be neither visible nor findable on other
backends. Area-level hiding stays `Area::AvailableWhen()`.

**Warning marker** — `Area::WarnWhen( fn )` (2026-11-05) draws a plain filled yellow dot
(`glyph::WarnDot()`, `glyph::kWarnDotColor`) on the area's rail row **and** on its group heading,
so a problem shows with the group closed or the rail collapsed (a badge on the icon's top-right in
the icon rail). `fn` runs on the paint thread every frame the rail is drawn: it must be a cheap read
of already-published state, never a blocking call or a child process. `Why:` the user, about
Diagnostics: *"if something is active in the diagnostics, it should show a warning sign on the
OTHER category and the Diagnostics tab name, to tell the user, to look there."* The triangle with
"!" that shipped first was dropped as looking fake. Diagnostics' own warnings are switched off
(see [diagnostics.md](diagnostics.md)); the machinery stays.
`Entry::WarnWhen( fn )` (2026-11-13) is the per-row twin: the same dot on the sheet row, left of the value (and of any `StatusWhen` text). Used by Diagnostics' Direct scanout row.

**`AvailableWhen( fn )`** hides an area entirely when it is false: no rail entry, no palette rows
(`Build()` skips unavailable areas), and `RequestArea()` ignores it. `Why:` a palette route to a row
the rail deliberately hides is the "reachable but not present" state. If the selected area becomes
unavailable the shell falls back to the first available one, so honouring a request for it would
silently drop the user elsewhere; `wlserver.cpp` declines to send the request in that state too
(two guards, not one). Friends uses it (no Steam app id, no area).

## Rail groups

The rail is a fixed table, not derived from `Section` (that enum is still taken by `Registry::Add()`
and nothing draws from it) or from registration order: `RailGroup`, `RailOrder()` (the `kRailOrder`
table in `Registry.cpp`), `RailGroupFor()` / `RailGroupForId()`, walked by `Registry::RailAreas()`.
`Why:` it lives in `Registry.*`, not `Shell.cpp` where the rail is drawn, so a plain unit test can
pin it with no ImGui context (`test_overlay_ui.cpp`, the "rail:" cases). An area id the table does
not name falls back to **Other**; `RailAreas()` skips a slot whose id `FindArea()` cannot resolve
instead of crashing, which let the table and a new area land in separate commits.

**Eight groups** (AUDIO added 2026-10-22, between INPUT and MISC; seven since 2026-10-04, MOTION; six
since 2026-09-27, the I7 regroup):

| Group | Areas (rail order) |
| --- | --- |
| **DISPLAY** | General, Resolution, Upscaling, Frame limiter, HDR, Shaders |
| **MOTION** | Frame generation, Motion blur (Lag spike buffer: disabled 2026-11-07, not registered) |
| **OVERLAY** | HUD, Crosshair, Zoom, Cursor |
| **INPUT** | General, Autoclicker, Null binds |
| **AUDIO** | Mixer, Effects |
| **MISC** | Friends |
| **SETTINGS** | Profiles, System, Appearance, Keybinds |
| **OTHER** | Log, About, Diagnostics |

Area ids behind those names: `display.general`, `display.resolution`, `display.upscaling`,
`display.frame_limiter`, `display.hdr`, `image.shaders`, `image.framegen`, `image.motionblur`,
`system.hud`, `system.crosshair`, `system.zoom`, `setup.cursor`, `input.general`,
`system.autoclicker`, `system.null_binds`, `audio.mixer`, `audio.effects`, `system.friends`,
`setup.profiles`, `system.general`, `setup.appearance`, `setup.keybinds`, `system.log`,
`system.changelog` (labelled "About"; the id was left alone because it keys rail grouping, the icon
table, palette state and every settings-audit row, and no user sees it), `system.diagnostics`.
`Nothing` (not `None`) is the accordion's own "all closed" value and is never an area's group.
`Why` `Nothing`: X11's headers, pulled in transitively by `steamcompmgr.cpp`, `#define None 0L`;
`PreviewKind` and `InheritState` avoid the name for the same reason.

History and rationale, kept in full because the tabs' membership is the user's own table:

- **Origin (2026-09-06, requests item 4).** A three-way DISPLAY / SYSTEM / SETUP grouping keyed off
  each area's `Section` became four fixed groups: DISPLAY, MISC (HUD, Mixer, Crosshair), SETTINGS
  (Profiles, System, Appearance, Cursor), OTHER (Log, Changelog).
- **I7 regroup to six (2026-09-27).** `Why:` the user's own table (lightly reflowed from its
  bulleted form): *"lets reorder some of the categories: Display: General, Resolution, Upscaling.
  Overlay: HUD, Crosshair, Zoom, Cursor. Input: General (Force grab cursor [remove from Display >
  General], Force grab keyboard), Autoclicker, Null binds. Misc: Friends, Mixer."* MISC had grown to
  seven areas spanning three unrelated concerns: things drawn over the game
  (HUD/Crosshair/Zoom/Cursor), things that intercept or rewrite input (Autoclicker/Null binds, soon
  General's grab toggles) and one unrelated social feature (Friends). OVERLAY and INPUT split the
  first two out, leaving MISC as the true two-item leftover. `input.general` was a new area a sibling
  task added after this one reserved its rail slot; until it landed INPUT showed two areas, which is
  what the skip-unresolved-slot tolerance above is for.
- **MOTION (2026-10-04).** Frame generation (moved out of DISPLAY, where it had sat below Shaders
  for a day), Motion blur and the Lag spike buffer: the three features that work on the game's
  frames over time. They are separate switches that work alone and together ("all of them are kind
  of separate, but they can all be turned on at the same time"), so they are one group rather than
  three more rows in DISPLAY. The architecture it sits on, in the user's words: *"you're basically
  only building the GUI in this chat and most of the stuff should go into the frame gen itself"*.
  The Lag spike buffer became the third tab the same day (the group was two tabs for hours).
- **AUDIO (2026-10-22).** `Why:` the user: *"We should have a seperate audio category. 1. Mixer
  (Volume Only) 2. Effects (Compressor Only for now) 3. Visualizer"*. Mixer keeps the volume rows;
  the compressor ("Dynamics") moved out of it into Effects; a Visualizer is a later task with no
  area yet. AUDIO sits between INPUT and MISC (both about the game's I/O, ahead of the leftovers);
  MISC is Friends alone since the Mixer left. Group icon: a speaker (`railgroup.audio`); Effects'
  area icon: a waveform squeezed between two lines.
- **Diagnostics (2026-11-05)** joined OTHER as its last area (see [diagnostics.md](diagnostics.md)).

## The rail accordion

Each group heading is a clickable accordion tab: only one group's areas are drawn at a time, and
the group holding the currently-open area opens itself (2026-09-27, I2, decisions D2/D6). The user,
dictating the fix: *"make the categories that we have now look like tabs and when one of them is
pressed, it extends the options for the category like tabs below it. This would allow us to make the
category smaller too."* Offered a plain tab strip as the alternative, the user picked the accordion.
`Why` it exists: a flat list of 19 areas in 4 groups already needed about 896 px against about
878 px available at 1080p/scale 1 (`0.85 x 1080` for the slab minus the 40 px slab bar), and a 20th
area (Null binds) landed the same week; with one group open the content is a few headers plus that
group's rows, however many areas pile up elsewhere.

- **State and rules** (`Registry.h`, pure and ImGui-free, so the test binary pins them):
  `AccordionOnHeaderClicked( open, clicked )` closes the group if it was the open one and opens it
  otherwise; `RailGroup::Nothing` ("every group closed") is a legal resting state. A collapsed
  group's rows are skipped in `DrawRail()`'s own measure/draw walk (`VisibleRailAreas()`), not
  merely clipped, which is the whole saving. Every header is always drawn.
- **The open group follows the selection.** Every selection route (a rail click, a palette or
  launcher jump, `StepArea()` for Up/Down and Ctrl+Left/Right, the `overlay_e2_select` console
  command, wlserver's area-request path) goes through `Shell.cpp`'s `SetSelectedArea()`, which
  applies `RailGroupForId()`, so a future call site cannot forget it. On the Shell's opening edge
  `ResetTransient()` re-derives the open group from the current selection rather than restoring
  whatever a header click last left; it is not persisted across a close, by design (the next open
  lands on the area you were last looking at). `StepArea()` walks the **full** rail list
  (`RailAreas()`), not just the open group, so stepping off a group's last area spills into the next
  group's first and opens it.
- **Animation.** Opening/closing eases over `tok::kDurRegion` (160 ms). `RailAccordionAnim` +
  `StepRailAccordionAnim( state, newOpen, restHeight, duration, dt )` track an **opening** group
  (eases 0 → its rest height) and a **closing** group (eases its last height → 0)
  **concurrently**. `RowVisibleHeightPx()` does the per-row reveal arithmetic for both roles;
  `RailContentHeightPx()` computes the same total `DrawRail()`'s `Walk()` does, from the shared
  `railmetrics::` constants (`kItemH` 40, `kHeaderH`, `kHeaderGap` 12, `kHeaderGapOpen` 6, `kPad` 8),
  so the two cannot disagree. `RailScroll()` keeps the active item in view when the rail is taller
  than its viewport (2.0x). A debug ConVar, `overlay_e2_rail_anim_scale` (default 1.0, multiplies
  `kDurRegion`), slows the animation so a screenshot lands mid-transition; it is kept because it
  is small, documented and 1.0 by default.
- **The animation fix (I7, 2026-09-27).** `Why:` the user, verbatim: *"When i open a category above
  the currently open one, it animates nicely, but when i open one below, it doesnt. Fix that."*
  Root cause: I5 shared ONE animated height across "the group currently displayed" as a single slot
  that both shrank (closing group) and grew (opening group) in one motion and always relocated under
  the newly opened header; the group that had just closed vanished from `DrawRail()`'s y-walk
  **instantly**, because no second height was left to animate it. Whether that was visible depended
  on table order: a group *below* the newly opened one collapsing is off-screen from where the eye
  is, so "opening above" read as a clean reveal; a group *above* it collapsing shifts every header
  after it, including the one just clicked, up by the old group's full height in the same frame as
  the click, so "opening below" read as the clicked header teleporting. The fix is two independent
  heights, so a closing group above the opening one shrinks in place exactly like one below it;
  neither height's arithmetic knows where the other sits in the table. Switching starts the old
  group shrinking from wherever it was and the new one growing from 0, concurrently; clicking a
  header shut before it finished opening reverses smoothly instead of jumping to full height first.
  *Known tradeoff:* only one closing slot exists. A third header click inside one ~160 ms window
  (two different headers clicked before the first switch finishes) makes the still-shrinking group
  snap shut rather than ease out. The old single-slot design re-targeted on re-clicks too; it is
  just visible on the closing side now. It never produces a stuck or negative height.
- **Full-height indicator (I7).** `Why:` the user, verbatim: *"when a category is expanded, the
  active indicator on the left (colored vertical line) doesnt span the full width of the category
  box, fix that"* ("width" meaning the bar's long, vertical axis). I6's bar was inset by `tok::kXS`
  top and bottom "so it never fights the pill's rounded corners", which at 8 px on a 30 px pill is a
  third of the bar missing. Deleting the inset would have poked a square nub past the pill's rounded
  corner, so the bar now runs the pill's full height with its own two **left** corners rounded to the
  pill's radius (`Shell.cpp`'s `Fill()` takes an `ImDrawFlags` argument, `RoundCornersLeft`),
  tracing the pill's own curve.
- **Headers as tall as the rows (I8, 2026-09-27).** `Why:` the user: *"The individual categories
  seem kind of small, tallness-wise, compared to the actual tabs below them, which looks kind of
  off."* `railmetrics::kHeaderH` had been raised 26 → 30 to fit the 24 px `tok::kIconBox` icon, not
  to match the 40 px row beneath. It is now `= kItemH`, a derived constant, so header and row
  height cannot drift a third time. Header text stays `TypeRole::Title` (Mono 600 14.5 upper), not
  stepped up to `Label`: the deliberate bold-mono-uppercase vs plain-sans distinction between
  categories and modules is kept, and the complaint was height, not glyph size. The icon rail's
  header cell already used `flItemH` directly. Fit budget with I7's busiest group (DISPLAY, 6 areas)
  open: `kPad + (kHeaderH + kHeaderGapOpen) + 6*kItemH + kHeaderGapOpen + 5*(kHeaderH + kHeaderGap) +
  kPad` = 568 px against 878 px at 1080p.
- **Look (I2 → I6, the user's brief that never changed).** *"the categories look a bit 'dull'
  compared to the individual modules inside of them. Plus, there is no animation, when opening them,
  which looks a bit sad. Also, the spacing towards the last shown module and the next category is
  zero, but it should get half of the spacing thats on the top (top decreases, bottom increases). Add
  icons for the individual categories and style them a little nicer."* What that settled into:
  a header is the brightest, heaviest thing in the rail (`Role::TextPrimary` 92% open or closed,
  `TypeRole::Title`); every state differs by an accent **tint** or an outline **role**, not a few
  percent of white (closed rest: `Role::SurfaceRaised` fill + `Role::LineControl` ring; hover adds
  `Accent(0.14)` and an `AccentBase` ring; pressed `Accent(0.26)`; open `Accent(0.22 to 0.34)`, an
  `AccentBase` ring and a 3 px accent bar, the same state edge an active area row draws, at the same
  x, so the open category and the selected module read as one accent column). `Why` `LineControl`:
  it is the role for "every interactive boundary"; `Role::Line` is the role for what you are *not*
  meant to click, and using it dissolved closed pills into the rail. The header runs the same icon
  and label arithmetic as an area row (an icon box of `tok::kIconBox`, label at `x0 + kIconBox +
  kM`; the pill's inset equals the rows' indent), so icons and labels share one left edge; the
  chevron sits on the pill's right. The half-above/half-below spacing split is `kHeaderGapOpen`
  (6) after the header and 6 below the last row; closed-to-closed is 12 (`kHeaderGap`).
- **The icon rail (60 px).** A group is one boxed pill (a `tok::kXS` inset all round) running the same
  state ladder; an area is a bare glyph. **Boxed = category, bare = module.** The corner chevron
  badge was removed: `Why:` a 6 px glyph in a corner of a 60 px cell has no inset at which it looks
  deliberate. The cell's hit rect is the full 60 px (a deliberate break from "a click area is its
  visible bounds": there is no neighbour to steal from). The group glyph is 20 px, a notch under the
  rows' 24. `IconForRailGroup()` is a **separate table** from `IconFor()` on purpose: group buttons
  are not areas, and mixing them in would break the "every registered area has exactly one icon"
  bijection test. Group glyphs: monitor on a stand (DISPLAY), a keycap with a small legend mark
  (INPUT), a square frame with a small plain "+" reticle inside (OVERLAY, unconnected to the frame
  edges so it differs from `system.crosshair`'s own glyph), a speaker (AUDIO), plus MOTION, MISC,
  SETTINGS, OTHER. A missing area icon falls back to the area's initial, never a blank rail.

## Dialog parts: wide modal, panes, checkbox, multi-line text, clipboard, second strip

Built for the Profiles Export and Import dialogs (2026-11; copy and paste JSON text, no files).
All in `Controls.{h,cpp}` unless stated.

- **Wide modal.** `ModalSpec::flWidthBase` (0 = the 240..420 default, unchanged; 760 for the
  dialogs) through `ModalWidthPx()`, clamped to the slab minus 2 x kXL and never under 240.
  `ModalPaneRow( ctx, titles, nPanes, flFirstColBase, flBoxHeightBase )` draws one row of 1 or 2
  titled, framed boxes (call twice for Import's 2x2) and shortens the boxes to
  `ModalBodyCtx::flMaxBodyPx` so the footer stays on screen; `LayOutModalPaneRow()` is the pure
  arithmetic. A box's content scrolls: `ModalPaneBegin` / `ModalPaneNextRow` / `ModalPaneEnd` wrap
  a child window in `ui::ScrollView`. `Why:` panes are data-simple like `ModalNextRow`, and the
  wheel is applied by hand (as the sheet does) but never on top of a native scroll.
- **Backdrop and primary.** `bCloseOnBackdrop` (default false): a press outside the panel runs
  Cancel (`ModalBackdropCloses()`). `fnPrimaryEnabled` dims the primary and ignores click and
  Enter; `fnValidate` stays the press-time gate with an inline message. `Why:` Import must be
  visibly unavailable until the pasted JSON parses, which a press-time check cannot show.
- **Checkbox.** `CheckState { Off, On, Mixed }`, `CheckboxAt( rc, id, &state, label, bDisabled,
  bKeyFocus )` for a tree row, `Checkbox( row, ... )` for an option in a row. Dialogs only (see the
  styling guide). The overlay never enables ImGui keyboard navigation, so the dialog owns a
  cursor and passes `bKeyFocus` (Space toggles); `ListBoxStep()` steps an index.
- **Multi-line text.** `TextMultiline( rc, id, &std::string, bReadOnly, placeholder, bError )`:
  ImGui `InputTextMultiline` with a resize callback (`TextBufferResize()`), Mono Meta, no 256-byte
  limit; read-only selects and copies. Characters outside the baked atlas show as the fallback
  glyph and stay in the string. `Why:` `controls::Text` is single-line and 256 bytes; an export
  is tens of KB.
- **Clipboard.** Read: `std::string gamescope_clipboard_text()` (`steamcompmgr.hpp`), guarded by
  a mutex because the overlay thread reads what the steamcompmgr thread writes. `Shell.cpp`'s
  `Draw()` installs `controls::SetClipboardHooks()` (read and `gamescope_post_selection` write) and
  `InstallImGuiClipboard()` each frame, so Ctrl+V / Ctrl+C / Ctrl+X work in fields and a Paste
  button calls `controls::ClipboardRead()`. `Why:` hooks, not a direct call, because the unit
  tests link `Controls.cpp` without steamcompmgr. The host clipboard only reaches gamescope while
  `system.clipboard_sync` (`gamescope::g_bClipboardSyncEnabled`, `Clipboard/ClipboardSync.h`) is on;
  a dialog that offers Paste should say so when it is off. Ctrl+V reaches ImGui: the key arrives as
  `ImGuiKey_V` with the Ctrl modifier (its control-character text is dropped), and
  `RunKeyboard()` returns early while a modal is open, so no shell shortcut takes it.
- **Second list strip.** `Entry::ListActionRow2()` puts a verb on a second strip
  (`ListVerb::nRow` 1); `ListStripCount()` is 1 or 2. `Band( List, nStrips )` / `LayOutBand()`
  grow the band by exactly one 44-unit line and return `BandLayout::rcStrip2`; the list and the
  first strip keep their place, and a list without a second-strip verb is unchanged. `LinesFor()`
  in `Shell.cpp` reads the same spec, so the scroll height follows. `Why:` Tokens.h's
  `kBandMaxLines` was sized for one strip, so a List clamps by its own spec instead.

## Keyboard map (SPEC section 8.2)

`RunKeyboard()` in `Shell.cpp`: arrows adjust the focused row or the highlighted palette result in
place, Up/Down in the rail step areas, Ctrl+Left/Right step areas from anywhere, Ctrl+I cycles the
Inspector host, Ctrl+K opens the palette, `?` / Ctrl+`/` help, Ctrl+D resets a row, Esc walks a
ladder (modal, dropdown, palette, inline expansion, selection). ImGui keyboard nav is never
enabled. `Why:` turning it on globally would hand every arrow key to ImGui's navigation and take
the adjust grammar away from the rows (the same trade refused for `InputText` in the palette).
Selection follows edit: any press that lands on a row's own control selects that row, value change
or not (`superdoc/planning/ui-design-guide.md`, "Selection follows edit").

## Threads

`Shell.h` exports a handful of functions and nothing else: `Draw()` (once per frame, from
`SettingsOverlay.cpp`), and thread-safe **requests** (`RequestPalette()`, `RequestLauncher()`,
`RequestClosePalette()`, `RequestArea( id )`, `RequestLauncherClosePreservingQuery()`,
`NotifyOverlayHidden()`) plus atomic queries (`LauncherOnlyActive()`, `PaletteActive()`,
`AreaActive( id )`). `Why:` the hotkeys fire on wlserver's thread and all shell state lives on the
draw thread; each request stores one atomic that `Draw()` consumes at the top of the next frame,
so the only cross-thread data is bools. `RequestArea()`'s id must have static lifetime (it is
stored as a bare pointer). It is not the `overlay_e2_select` console path, which assigns the
selection straight from the console thread and is acceptable for a script, not a hotkey. See
[keybinds.md](keybinds.md) for what each hotkey does.

## Scripting and tests

Console: `overlay_e2_select`, `overlay_e2_set`, `overlay_e2_get`, `overlay_e2_key` (real key events
into the overlay's own input queue, the same path a physical key takes, reaching nothing outside
the overlay), `overlay_e2_pointer`, `overlay_e2_palette`. Tests: `tests/test_overlay_ui.cpp` (laws,
lane and row arithmetic, rail table, accordion state machine, palette scoring, icon census) and
`tests/test_overlay_shell.cpp` (drawn behaviour). Pure arithmetic is kept ImGui-free (`Tokens.h`,
`Lane.h`, `Layout.h`, `Registry.h`, `Icons.h`) so a unit test never opens a window.

## Where to look

- **Add a setting**: edit the area's `Panel*.cpp` `*_RegisterArea()`; give it `.Help()`, `.Default()`,
  `.Key()`, keywords. Nothing in the shell needs touching.
- **Add an area**: write `Foo_RegisterArea( Registry & )`, call it from `RegisterAll()` in
  `Shell.cpp`, add its id to `kRailOrder` in `Registry.cpp`, give it an icon in `Icons.cpp`.
- **Add a rail group**: extend `RailGroup`, `RailGroupName()`, the table, `IconForRailGroup()`;
  mind the `Nothing` entry.
- **Change a size or colour**: `Tokens.*` / `Colors.*`, never at a call site (see the styling guide).

## Related

- [command-palette.md](command-palette.md), [keybinds.md](keybinds.md), [profiles.md](profiles.md)
  (inherited / overridden dots), [launch-option-lock.md](launch-option-lock.md), [diagnostics.md](diagnostics.md)
- Design record: `superdoc/planning/ui-design-guide.md`, `superdoc/planning/redesign/round-2/e2-inspector-plus/`
  (`SPEC.md`, `API.md`, `IMPLEMENTATION.md`), `superdoc/planning/redesign/AUTONOMOUS-DECISIONS.md`

## History

- **Shell** (glossary detail): **Shell** — the E2 settings overlay proper: slab, rail, sheet and inspector (`src/Overlay/UI/Shell.cpp`). "The full/clickable overlay" in the user's wording. The launcher is deliberately *not* part of it. Closed with the X in the title bar; there is no separate settings tab for this any more (the former `setup.shell` tab was removed 2026-08-27).
