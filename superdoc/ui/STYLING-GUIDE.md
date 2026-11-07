# Styling Guide

Layout and control rules for the settings overlay (the **Shell**, `src/Overlay/UI/`). The reusable
helpers are `Tokens.h` (geometry, spacing, type, motion), `Colors.h` (colour roles), `Row.h` /
`Lane.h` / `Band.h` / `Layout.h` (where things go) and `Controls.h` (the atoms). The framework is
described in [../features/overlay-shell.md](../features/overlay-shell.md); the full design history
(every round of user feedback, the original mockup analysis) is in
`superdoc/planning/ui-design-guide.md` and is **not** a style reference: parts of it describe the
2026-08 mockup handoff, which the code has since replaced (the code won every disagreement).

**The top-level rule:** reference a colour by `Role`, a size by a `tok::` constant (through `Px()`),
a font by `TypeRole`, a position by a `RowCtx`. Do NOT write a literal colour, pixel value, font
size or x coordinate at a call site. `Why:` a number that means the same thing in two places is one
token, so a contrast or spacing fix is one edit instead of a hunt, and `Tokens.h` / `Colors.h`
carry no ImGui dependency of their own so the arithmetic is unit-testable.

Most of what follows is enforced by API shape (there is simply no way to say the wrong thing).
Where it is only a convention, the rule says so.

## Adding a setting (the only way to put a control on screen)

Declare it once in an area's `*_RegisterArea()`; the sheet row, palette entry and Inspector page all
come from it.

```cpp
ui::Area &a = reg.Add( "overlay.example", "Example", ui::Section::System );
a.Group( "Look" );
a.Slider( "overlay.example.opacity", "Opacity",
          ui::AnyBind::Of<float>( []{ return s_fOpacity; },
                                  []( float f ){ s_fOpacity = f; QueueSave(); } ) )
    .Range( 0.0f, 1.0f ).Step( 0.05f ).Unit( "%" )
    .Key( "overlay.example.opacity" )
    .Default( 1.0f )
    .Help( "How opaque the example is." )                       // required
    .Keywords( "transparency alpha see-through" )
    .Param( "fade_ms", "Fade", ui::AnyBind::Of<int>( &s_nFadeMs ) );   // id: overlay.example.opacity.fade_ms
```

- Do NOT call ImGui directly from a panel file. There is no custom-draw callback and no `Escape()`,
  on purpose; if a row kind is missing, extend `Registry.h`. `Why:` the escape hatch it replaced
  took the drawing away from the shell and suspended every law.
- Do NOT omit `.Help()`, reuse an id, give a Param a dotted leaf, or exceed `ParamBudget()` (10)
  Params on a row: each aborts at startup. Do NOT add a second hand-copied "of 10" anywhere; read
  `ui::ParamBudget()`.
- Do NOT add a disabled state without a reason: `.DisabledUnless( pred, "Why it is off." )` has no
  overload without one, and the reason renders in the Inspector. Do NOT fold a launch-flag lock into
  it; use `.LockedByLaunchOption()` (a separate slot, because a row can need both).
- Do NOT put a `Facts`/`Meter` where a setting belongs: they are read-only kinds, excluded from the
  palette, and have no setter.
- Use `.Dropdown()` on a `Choice` only when the option set is user-created or unbounded; a fixed few
  words stays a plain `Choice`, which picks segmented vs dropdown by measurement. `Why:` a caller
  cannot pick wrong because a caller does not pick.
- Use `Entry::Confirm( "prompt" )` on any `Action` that destroys something. `Why:` the user, after
  an agent wiped a config: *"There can be a button for it, but never delete configs automatically."*

## Colour

```cpp
const ImU32 col  = ui::Col( ui::Role::TextLabel );     // a named job, never "white at 68%"
const ImU32 wash = ui::Accent( 0.16f );                // accent at a stated alpha, for fills
const ImU32 off  = ui::Dim( col );                     // x0.55, disabled
ui::ScopedDim dim( bDisabled );                        // dims everything drawn under it
```

- **Accent is state, nothing else.** Active edge, on-switch, handle, focus: yes. Decoration: no.
  The accent family follows the live hue (`palette::UpdateAccentFamily()`); a call site never sees
  a hue.
- Do NOT write `IM_COL32(...)` or an alpha literal. The sole exception is `glyph::kWarnDotColor`
  (a literal yellow; `Role::Warn` is orange and the dot must read on the rail's idle and selected
  backgrounds).
- Do NOT draw a disabled control by hand-dimming one atom. `Dim()` / `ScopedDim` sit under
  `Col()` and `Accent()`, which every pixel in the kit goes through, so no atom can opt out and a
  new atom is dimmed correctly before it is written. Disabled means the whole row at 0.55
  (label, value and control); a reason must be shown.
- Interactive boundary = `Role::LineControl` (white 42%); `Role::Line` / `Role::LineRegion` are
  decorative rules and exempt from contrast. `Why:` `Line` on a clickable thing makes it dissolve
  into the background (a rail header once did).
- To make a colour a stated opacity (window transparency) use `WithAlpha( col, a )`, which
  *replaces* alpha. Do NOT use `Dim( Col( Role::Surface ), 1.0f )` for "opaque": the surface role
  is baked at 88% alpha, so that stayed translucent.
- Text roles: `TextPrimary` selected labels / hero values; `TextBody` Inspector prose; `TextLabel`
  row labels; `TextMeta` units, marks, chips, placeholders. There is no fifth "faint" role.
- Do NOT use red for anything but `Role::Danger` / `DangerText` (destructive confirmations).

## Type

```cpp
ui::controls::DrawText( rcCell, ui::TypeRole::Value, ui::Col( ui::Role::AccentValue ), "42%",
                        ui::controls::TextAlign::Right );
```

Six roles: `Title` (Mono 600 UPPER, slab and region titles), `Section` (Mono 500 UPPER, group
bands), `Label` (Sans 400, row labels), `Body` (Sans 400, Inspector prose), `Value` (Mono 500, every
numeric or state readout), `Meta` (Mono 400, units, chips, and the Log/About body). Sizes are in
`Tokens.cpp`'s table.

- Do NOT put a number in a Sans run. Numbers and state words are `Value` or `Meta` (Mono).
  `Why:` the font family is the role's, so the helper picks it and a caller cannot get it wrong.
- Do NOT use a glyph outside Basic Latin + Latin-1 in a string. `Fonts.cpp` bakes only those ranges
  and the bundled faces lack the arrows and magnifier code points entirely, so anything else draws
  as the fallback box. Marks are **drawn**: `glyph::Chevron`, `Magnifier`, `Lock`, `WarnDot`,
  `RailIcon`. `Why:` widening the range could not help (the faces have no such glyph) and the atlas
  is rebuilt per scale, so every baked range is paid again at each scale change.
- Do NOT ship an icon font or an icon file. Area icons are stroked paths on a 24-unit grid in
  `Icons.cpp` (the constants `kIconGrid`, `kIconStroke` 1.7 live in `Icons.h`); fills only where a fill carries meaning. A missing
  icon falls back to the area's initial, never a blank.

## Size, spacing and the scale

All geometry is authored in **base units** (px at `display_scale` 1.0) and converted once.

```cpp
const float flPad  = ui::Px( ui::tok::kM );     // spacing: kXS 4, kS 8, kM 12, kL 16, kXL 24, kXXL 32
const float flRule = ui::Hairline();            // max(1, floor(scale)): 1 px up to 1.99x, 2 px at 2x
```

- Do NOT write `Px( 1 )` for a rule or a literal `12.0f` for a gap. A rule is `Hairline()`; a gap is
  one of the spacing steps or a named gap (`kGapAtom`, `kGapSeg`, `kGapLabel`). `Why:` `Px( 1 )`
  vanishes at 0.5x and thickens into a border at 2x.
- One control height: every hit box is `tok::kControlH` (28) tall; a graphic deliberately shorter
  (switch 40x20, slider, meter) is centred in it. One row height: `tok::kRowH` (44), for sheet and
  Inspector rows alike. Do NOT invent a third row height.
- Region padding: sheet `kSheetPad` (24), Inspector `kInspectorPad` (16). Group band rhythm: 16
  above, 8 below (`kGroupSpaceAbove` / `kGroupSpaceBelow`).
- Rail metrics are `railmetrics::` constants in `Registry.h` (`kItemH` 40, `kHeaderH == kItemH`,
  `kHeaderGap` 12, `kHeaderGapOpen` 6). Do NOT retype them in `Shell.cpp`; `RailContentHeightPx()`
  and `DrawRail()` read the same ones so they cannot disagree.
- Slab, rail, Inspector and palette sizes live in `Layout.h` (`shelltok::`). Do NOT add a pixel cap
  to the slab: it is a flat 85% of the surface.

## Rows and the lane

A row has four columns: a 2 px state edge and left-bound label, a right-bound value, the control,
and a 28-unit affordance. Two vertical lines run the whole sheet: the value's right edge at `Lw`
(`clamp( round( 0.46 x W ), 200, 420 )`, computed once per column by `Lane::ForColumn()`; content
never moves it) and the control's right edge at `W - 28`. The only allocator is `RowCtx`, and it is
**right-anchored**:

```cpp
const ui::RowCtx row = ui::RowCtx::ForRow( lane, flOriginPx, flTopPx );
bool bChanged = ui::controls::Switch( row, "##my_switch", &bValue );
```

- Do NOT look for a `Left()`, `Centre()`, `SameLine` or `SetCursorPosX`: there is none, so
  left-aligned controls are unrepresentable. An atom receives a rect and draws inside it.
- Do NOT pass an x or a width to an atom. Per-control widths are constants in `Controls.cpp`;
  `Place()` clamps to the lane so an over-wide atom shrinks rather than escaping left.
- Do NOT draw a value readout beside a control that displays its own value. `UsesValueColumn( Kind )`
  decides: `Switch`, `Slider`, `Stepper`, `Meter`, `Composite` use the value column; `Choice`,
  `Text`, `Bank`, `Action`, `Facts` self-display.
- Use `SplitLabelZone()` (label and value from one subdivision) rather than computing two rects.
  For a fixed-size atom well inside the control zone (a switch, a stepper) pass the control's left edge
  so the value sits next to it instead of stranded at `Lw`.

## Controls

Every atom has one shape, `bool Atom( const RowCtx &row, const char *pszId, <binding>, ... )`: it
registers **one rect** with ImGui and draws inside **that same rect**, so the drawn and hit-tested
geometry cannot diverge (the slider-grab bug class).

- There is **no Checkbox** and there will not be one: `Switch` is every binary in the product.
- **Sliders**: the fill end, the handle centre and the default tick all come from
  `SliderValueX()`, and `SliderGrab()` always drives `SliderBehavior` with a float (`SliderInt`
  rounds). Do NOT paint a handle at a different x than the one that is hit-tested, and do NOT use
  ImGui's int-grab path. `Why:` its grab widens to "represent one unit" and insets its travel, so the
  handle sat off where the fill ended (Motion blur Samples 2..8, 1/14 of the track off at each end;
  the user: *"the dragger is wrong compared to where the line actually fills to"*). The handle is a
  constant `kHandleW` wide whatever the range. Do NOT make a slider's width depend on its range.
- **Segmented vs dropdown**: `MeasureCells()` decides how wide a segmented group is, and both the
  fit test and the layout read it. Do NOT hand-measure a Choice.
- **Stepper** is borderless `- +` glyphs; the number is in the value column and can be typed.
- **Settings that resize the UI** (`overlay.display_scale`) must hand the reflowing half of their
  write to `controls::DeferToRelease()` while `ui::IsPointerDragActive()` is true. `Why:` the track is
  laid out from the very scale being dragged, so applying it live slides the slider out from under
  the pointer (user: *"The UI scale should update, when the slider is released. Otherwise, it is
  almost impossible, to adjust."*).
- **Selection follows edit**: any press on a row's own control selects the row so the Inspector
  shows it. New control atoms and composite bands must route their press through the same rule
  (`ShouldSelectRow()` / `ControlEngaged()`); do NOT rely on the row's full-width selector button,
  which an atom's press never reaches.
- **Dialogs**: `ui::OpenModal( ModalSpec )`, one at a time; the body is ordinary rows from
  `ModalNextRow( ctx )` using the unmodified atoms, with errors drawn inline (`fnValidate`) rather
  than as a toast. State `flMinBodyRows` if you know the row count, or it opens one row tall and grows.
- **Composites** (a control taller than one row) are full-width bands of exactly n x 44
  (`Band( CompositeKind )`, `LayOutBand()`), with line 1 reading as a normal row and the body
  right-bound to the control lane. Do NOT put anything in the label column on lines 2..n, and do NOT
  choose n or the body size yourself.

## Scrolling bodies and frame rules

```cpp
const ui::ScrollView sv = ui::ScrollView::Begin( rcRegion, ImGui::GetCursorScreenPos().y );
// ... paint rows at absolute y starting from sv.flOriginY ...
ImGui::Dummy( ImVec2( 0, sv.ContentHeight( flBottomY, flPad ) ) );
```

- A body paints at absolute y but MUST **start at the child's cursor and hand back the height it
  used**. `Why:` ImGui scrolls by moving the cursor, not by translating the draw list; a body that
  ignores the cursor is nailed to the screen ("scrollbar moves, content doesn't") and one that
  reports no height has no scroll range. It was missed twice (Inspector, then the sheet).
- **Every rule pixel is painted by exactly one draw call.** `Role::Line` / `LineRegion` are
  translucent, so two stacked composite to a brighter tone. Where rules meet, make one yield: inset
  an `HLine` by `Hairline()` at each end against a frame border (`HLine( x0 + Hairline(), x1 -
  Hairline(), y, col )`), split a vertical into two segments around a horizontal rule, draw row
  washes one `Hairline()` short of a divider and draw the divider after the rows. Also: a
  `PushStyleColor( ImGuiCol_ChildBg )` outlives its `BeginChild`, so a nested child re-paints the
  fill over the parent's divider unless it pushes a transparent one. `Why` not pre-blended opaque
  rule colours: that would need one authored colour per background a rule can sit over.
- The sheet body scrolls with the mouse wheel; keep `ApplyChildWheelScroll()` in the path of any
  new scrolling region.

## Motion

Three durations, one easing: `tok::kDurState` (90 ms: hover, knob travel, focus), `kDurRegion`
(160 ms: host change, rail collapse, inline expansion), `kDurSurface` (240 ms: overlay open and
close); `Ease()` is `1 - (1 - t)^3`.

```cpp
flAnim = ui::Approach( flAnim, flTarget, ui::tok::kDurRegion, ImGui::GetIO().DeltaTime );
```

- Do NOT lerp with a fixed per-frame constant: it animates four times faster at 240 Hz than at
  60 Hz. `Approach()` takes `io.DeltaTime`.
- Do NOT add a fourth duration or a second easing.

## Rail and Inspector conventions

- Rail group heading: `TypeRole::Title`, `Role::TextPrimary` (92%, above even an active module's
  label); closed = `SurfaceRaised` fill + `LineControl` ring, hover/pressed/open add accent **tints**
  and an `AccentBase` ring, open adds a full-height left accent bar with its left corners rounded to
  the pill. Height `kHeaderH == kItemH`. Boxed pill = category, bare glyph = module (icon rail).
  Do NOT make states differ by a few percent of white; use a hue tint or a different outline role.
- A new rail group needs a glyph in `IconForRailGroup()`; area icons live in a separate table
  (`IconFor()`) so the area-to-icon bijection test stays true.
- Warnings on the rail are `Area::WarnWhen()`'s plain filled dot; the predicate must be a cheap
  read of published state (it runs every frame on the paint thread).
- A button that belongs only in the Inspector is `Entry::InspectorVerb()`, not a sheet row; a
  switch that does is `Entry::InspectorSwitch()`.
- Launch-flag lock tag ("LAUNCH OPTION"): drawn under `ScopedUndim` so the explanation does not fade
  with the dim row it explains.

## Checks before you finish

1. `grep -nE 'IM_COL32|ImVec4\(|[0-9]+\.[0-9]+f' ` over your new UI code: any literal is a smell.
2. Run `tests/gamescope_tests '[overlay_ui]'` (via the build script in `scripts/`, see
   [../features/gentle-build.md](../features/gentle-build.md)): it pins the laws, lane
   arithmetic, rail table and accordion.
3. Check at display scale 0.5, 1.0 and 2.0; the slab, lane and drawer behaviour change between them.
