# `gamescope::ui` — the helper layer (E1)

The user's goal, in their words:

> *"what i want is basically our own framework/helper functions (on top of ImGui), that
> makes it easier for you/AI, to update and extend the UI, while keeping it consistent
> with the rest."*

The test: **adding a setting is one obvious call, and producing something inconsistent is
hard.** E1 keeps E's shell and takes B's registry, because the two solve different halves
of that and the seam between them is clean.

Proposed location:

```
src/Overlay/UI/
    Registry.h / .cpp     // Area, Entry, the fluent builders, RegisterAll()
    Bind.h                // value binding: config key, plain pointer, ConVar, getter/setter
    Shell.h / .cpp        // the slab, the three regions, ComputeLayout(), the ladder
    Sheet.cpp             // registry → rows (and the Raw/list sheet variants)
    Row.h / .cpp          // BeginRow / RowCtx — THE ONLY place an x coordinate exists
    Controls.cpp          // one function per control kind, all rect-taking
    Composite.cpp         // the row-group rule (§4)
    Inspector.cpp         // Panel + Card — the same vocabulary, two-line
    Palette.cpp           // Ctrl+K over the registry
    Match.h / .cpp        // fuzzy scorer + highlight positions          (from B)
    PaneCtx.h / .cpp      // the fenced escape hatch for the LOG and gauges (from B)
    Theme.h / .cpp        // roles, type roles, the three animations
    Lint.cpp              // ui_lint / ui_snapshot / ui_rulers
    Areas/
        Upscaling.cpp Output.cpp FrameLimiter.cpp Hdr.cpp Shaders.cpp Mixer.cpp
        Monitor.cpp Log.cpp Profiles.cpp PerGame.cpp Appearance.cpp
        Areas.cpp         // RegisterAll() — the one place an area is added
```

`Widgets.cpp` survives as the *painting* layer (slider, toggle, segmented control,
position grid move in unchanged behind rect-taking signatures). `Chrome.cpp`'s
dock/window/title-bar/drag/collapse/tiling machinery is deleted.

---

## 1. The division of labour

| Decided by the **caller** | Decided by the **helper** |
|---|---|
| Which setting exists | Where it sits |
| What it is called | How wide its control is, and where its right edge lands |
| What it is bound to | Which control kind is legal for it |
| Its range, default, unit | Every colour, font, alpha and spacing value |
| Its help text and keywords | Hover / focus / press / disabled painting |
| Which area and group it belongs to | Sheet vs Card vs Inspector placement |
| Whether it is `Expert` | Whether it fits, and what to do when it doesn't |

**The rule that makes this hold: no function in the public API accepts a pixel, a
colour, a font, an alignment or an `ImVec2`.** There is no `SetWidth`, no `SameLine`, no
`SetCursorPosX`, no `PushStyleColor`. A caller physically cannot express a misaligned
layout — which is the direct structural answer to critique (2).

---

## 2. Registration — one declaration, three surfaces

Taken from `b-command/API.md` §3, with E's `Area` = a rail item.

```cpp
namespace gamescope::ui
{
    enum class Depth  { Sheet, Expert };      // Expert → Inspector; see SPEC §1.4
    enum class Writes { Routed, Global };     // Routed = app json when an override is on

    // Fluent. Nothing here describes appearance: no size, colour, font, position,
    // padding or alignment parameter exists in this class, by design.
    class Entry
    {
    public:
        Entry &Help    ( const char *sz );                 // Inspector body. REQUIRED.
        Entry &Hint    ( const char *sz );                 // one line under the label, <= 64 chars
        Entry &Keywords( const char *sz );                 // extra search terms
        Entry &Unit    ( const char *sz );                 // "%", "fps", "ms", "nits", "px"
        Entry &Range   ( double lo, double hi );           // REQUIRED for numeric kinds
        Entry &Step    ( double coarse, double fine = 0 );
        Entry &Default ( auto v );                         // REQUIRED. Ctrl+D restores it.
        Entry &ZeroMeans( const char *sz );                // "Unlimited", "Off"
        Entry &DisabledUnless( std::function<bool()>, const char *szReason ); // no overload
        Entry &AvailableWhen ( std::function<bool()> );    // absent entirely (no hardware)
        Entry &Related ( std::span<const char *const> ids );
        Entry &Depth   ( ui::Depth );                      // default Sheet
        Entry &Writes  ( ui::Writes );                     // default Routed
        Entry &Group   ( const char *sz );                 // overrides the area's group cursor
        Entry &OnChange( std::function<void()> );
    };

    class Area                                             // == one rail item
    {
    public:
        Area &Section  ( const char *sz );                 // rail section header: "DISPLAY"
        Area &Describe ( const char *sz );                 // the Category Card's first line
        Area &Badge    ( std::function<std::string()> );   // rail trailing count chip
        Area &AvailableWhen( std::function<bool()> );
        void  Group    ( const char *szName );             // group cursor for what follows

        // ── the complete control taxonomy. There are no other factories. ──
        Entry &Switch  ( const char *szId, const char *szTitle, Bind<bool> );
        Entry &Slider  ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Slider  ( const char *szId, const char *szTitle, Bind<int>   );
        Entry &Number  ( const char *szId, const char *szTitle, Bind<int>   );
        Entry &Choice  ( const char *szId, const char *szTitle, Bind<int>,
                         std::span<const Option> );        // segmented OR dropdown — helper picks
        Entry &Text    ( const char *szId, const char *szTitle, Bind<std::string> );
        Entry &Action  ( const char *szId, const char *szTitle, const char *szVerb,
                         std::function<void()> );
        Entry &Meter   ( const char *szId, const char *szTitle,
                         std::function<float()>, std::span<const char *const> chans );

        // ── composites: SPEC §2.4. Each returns a Composite, not an Entry. ──
        Composite &Anchor    ( const char *szId, const char *szTitle,
                               Bind<int> corner, Bind<int> marginV, Bind<int> marginH );
        Composite &Fader     ( const char *szId, const char *szTitle,
                               Bind<int> volume, std::function<Levels()> peaks );
        Composite &SwitchList( const char *szId, const char *szTitle );  // .Member() below
        Composite &Colour    ( const char *szId, const char *szTitle, Bind<float> hue );

        // ── lists: SPEC §1.5 ──
        List &Items ( const char *szId, const char *szTitle );

        // ── Card-only and Inspector-only. NOT AVAILABLE ON A SHEET. ──
        Entry &Readout( const char *szId, const char *szTitle,
                        std::function<std::string()>, bool bLiveDot = false );

        // ── the fenced escape hatch, for the LOG and the live gauges ──
        Entry &Pane   ( const char *szId, const char *szTitle,
                        std::function<void(PaneCtx&)> );
    };

    class Registry
    {
    public:
        Area &Area( const char *szId, const char *szTitle, Icon );
        void  SelfTest();                                  // debug builds; §7
    };

    void RegisterAll( Registry & );                        // Areas/Areas.cpp
    void DrawShell();                                      // once per frame. The only entry point.
}
```

`SettingsOverlay.cpp`'s per-frame body becomes, in full:

```cpp
ui::DrawShell();
```

`DrawShell()` owns the slab rect, the three region rects, the responsive ladder, the rail,
the accent thread, the breadcrumb, the footer, selection, the palette, keyboard routing,
and the dispatch from registry to sheet, card and inspector.

### 2.1 What one registration produces

```cpp
area.Switch( "display.allow_tearing", "Allow Tearing", ui::Cfg( "display.allow_tearing" ) )
    .Default( false )
    .Hint   ( "lower latency · can show a horizontal seam" )
    .Help   ( "Presents frames the moment they are ready instead of waiting for vblank." )
    .Keywords( "immediate flip vsync latency tear" );
```

Five lines. What they produced, none of which the caller asked for:

**The row** — 44 base, hairline-separated; label Sans 14 `TextLabel` at x=12; a 30×15
switch whose **right edge is on the sheet rail**, on the same vertical line as every
segmented control, stepper, slider and 3×3 grid in the product; hover / press / focus
painting; a `differs` state edge and a reset dot the moment the value leaves `false`; a
contribution to the header's `differs N` chip.

**The palette entry** — searchable from the first frame by "Allow Tearing", "tearing",
"latency", "vsync", "immediate", "flip", "display.allow_tearing" and "Upscaling", with
its current value in the result's value column, and `Enter` jumping to it.

**The Inspector page** — the switch again in two-line form, `default off`, the help text,
`writes global.json` with the routing reason, related links, and the registry id and
keyword list for anyone (or any agent) auditing the surface.

---

## 3. Adding a setting — every kind

### 3.1 Slider with a dependency

```cpp
area.Slider( "display.sharpness", "Sharpness", ui::Cfg( "display.sharpness" ) )
    .Range( 0, 20 ).Step( 1 ).Default( 3 )
    .Hint( "higher = sharper" )
    .Help( "Strength of the FSR RCAS / NIS sharpening pass. 0 disables sharpening." )
    .Keywords( "sharpen rcas cas crisp clarity" )
    .DisabledUnless( []{ return IsSharpeningFilter(); }, "only fsr, nis and pixel sharpen" );
```

Absent: format string, min/max labels, width, `ImGuiSliderFlags`, a `BeginDisabled` pair,
a hand-placed hint. The format comes from the bound type and `Unit()`; the marks from
`Range()`; the default tick from `Default()`; `AlwaysClamp` is always on because there is
no reason for it not to be.

`DisabledUnless` **has no overload without a reason string.** That single signature is the
entire enforcement for the most common inconsistency in the current code — a control that
greys out and does not say why. The reason is then drawn at full strength *outside* the
dimmed row (SPEC §2.3.13), which is both a legibility fix and a WCAG one.

### 3.2 Choice — the helper picks the control

```cpp
static constexpr ui::Option kFilters[] = { {"linear"},{"nearest"},{"fsr"},{"nis"},{"pixel"} };
area.Choice( "display.filter", "Filter", (int *)&g_upscaleFilter, kFilters ).Default( 2 ) …

static constexpr ui::Option kModes[] = { {"1280 × 720"},{"1920 × 1080"},{"2560 × 1440"},{"3840 × 2160"} };
area.Choice( "display.resolution", "Resolution", &g_nMode, kModes ).Default( 3 ) …
```

Five short labels → **segmented**. Four long labels → **dropdown**. The call site is
identical and does not get a say: `Controls.cpp` measures with the real font at the real
scale and applies SPEC §2.3.2's rule. A caller cannot ship a cramped segmented row, and a
segmented control cannot become a tab bar because `BeginTabBar` does not exist in the API.

### 3.3 Number, text, action

```cpp
area.Number( "display.fps_limit", "FPS Limit",
             ui::Bind<int>( []{ return g_nTargetFPS; },
                            []( int v ){ steamcompmgr_set_fps_limit( v ); } ) )
    .Range( 0, 1000 ).Step( 5 ).Unit( "fps" ).Default( 0 ).ZeroMeans( "Unlimited" )
    .Help( "Caps the game frame rate. 0 removes the cap entirely." )
    .Keywords( "frame rate cap limiter throttle" );

area.Text( "profiles.name", "Name", &g_sNewProfileName )
    .Default( "" ).Placeholder( "e.g. Handheld 40 fps" )
    .Validate( ui::Validator::Filename )
    .Help( "Letters, digits, space, hyphen and underscore only." );

area.Action( "pergame.delete", "Delete Saved Config", "delete", &OnDeleteAppConfig )
    .Danger().Confirm( "This removes games/1174180.json. It cannot be undone." )
    .AvailableWhen( []{ return ConfigManager::HasAppConfig( SessionAppId() ); } )
    .Help( "Deletes this game's saved config permanently." );
```

`.Danger()` returns a distinct `DangerAction` whose only method is `Confirm()`, and only
`Confirm()` returns the chainable `Entry&`. A destructive action without a confirmation
step **does not compile**.

B's `Text()` registration assertion (`.Suggestions()` or `.KeyboardOnly()` required) is
**dropped**: it existed to guarantee gamepad reachability, and gamepad is out of scope.

### 3.4 Read-only facts — Card only

```cpp
area.Readout( "upscaling.path", "Composite path",
              []{ return CompositePathString(); }, /*live*/ true );
```

> **`Readout()` is a method of `Area` that the Sheet renderer never reads.** It emits into
> the **Category Card**'s `Live` block. There is no `Sheet::Readout()`, so a read-only row
> in a settings sheet is not a style violation — it is a call that does not exist.

That one absence is the structural half of the fix for critique (3). The taste half is the
Placement Test in SPEC §3.1, which is answered from the declaration (`Readout` has no
bind; `Expert` says so; `Help` is a field), so the helper applies it rather than the author.

### 3.5 Expert depth

```cpp
area.Choice( "monitor.blend", "Blend mode", &g_hud.eBlend, kBlend )
    .Default( 0 ).Depth( ui::Depth::Expert )
    .Help( "How monitor pixels are combined with the game. "
           "Inverted stays readable on any background." );
```

`Expert` entries render in the Card's `Expert` block and in the group's chevron target.
They are still registered, so `Ctrl+K` finds them and `gamescopectl ui set monitor.blend
inverted` works. Per SPEC §1.4 the chevron re-opens the Inspector if it is closed, so the
Inspector Contract is not merely respected — it is now actually true, which it was not in E.

---

## 4. Composites — the fix for critique (5), as an API

```cpp
namespace gamescope::ui
{
    // A Composite IS a row group (SPEC §2.4). It owns a head row and N part rows.
    // It has no layout knobs: the head is a Row, the parts are Rows, they share
    // one state edge and one closing hairline, and every part obeys the rail.
    class Composite
    {
    public:
        Composite &Help    ( const char *sz );
        Composite &Keywords( const char *sz );
        Composite &Summary ( std::function<std::string()> );  // the head row's rail value
        Composite &Chip    ( std::function<std::string()> );  // one status chip, label zone
        Composite &Sub     ( std::function<std::string()> );  // one sub-line under the label

        // parts. Sub-label + control, indented 12, right edge on the rail.
        Composite &Part    ( const char *szSubLabel, Entry & );
        Composite &Member  ( const char *szId, const char *szTitle,
                             Bind<bool>, bool bDraggable = false );  // SwitchList only
    };
}
```

Call sites — **the two Anchor sites that drifted in the current codebase now cannot**:

```cpp
// Areas/Monitor.cpp
monitor.Group( "Placement" );
monitor.Anchor( "monitor.placement", "Anchor",
                &g_hud.nAnchor, &g_hud.nMarginV, &g_hud.nMarginH )
       .Summary( []{ return std::format( "{} · {} / {}",
                     kAnchorNames[g_hud.nAnchor], g_hud.nMarginV, g_hud.nMarginH ); } )
       .Help( "Which screen corner the monitor is pinned to, and how far in it sits." )
       .Keywords( "anchor placement position corner margin offset where" );

// Areas/Appearance.cpp — same call, different binds. One implementation.
appearance.Anchor( "notify.placement", "Anchor",
                   &g_notify.nAnchor, &g_notify.nMarginV, &g_notify.nMarginH ) …
```

`Anchor()` internally is three `Part()` calls (`Corner` → the 96-wide `RowBlock` grid,
`Vertical margin` and `Horizontal margin` → steppers). The caller supplies three binds and
a summary; it cannot supply an arrangement, so the two sites are identical by construction.

```cpp
// A switch list — what used to be a column of checkboxes
fps.SwitchList( "mod.fps.rows", "Rows" )
   .Summary( []{ return std::format( "{} of 4", CountEnabledRows() ); } )
   .Help( "Which lines the FPS module draws, top to bottom." )
   .Member( "mod.fps.r1", "Frame rate",                       &g_hud.bRowFps,    true )
   .Member( "mod.fps.r2", "Frametime readout (ms)",           &g_hud.bRowMs,     true )
   .Member( "mod.fps.r3", "Frametime graph",                  &g_hud.bRowGraph,  true )
   .Member( "mod.fps.r4", "Percentile row (1% / 0.1% / avg)", &g_hud.bRowPct,    true );
```

Every member is a **switch**, because `Member()` takes a `Bind<bool>` and there is no
other binary painter to reach. The `all` / `none` actions and the `3 of 4` count are the
composite head's, produced by `SwitchList()` itself.

```cpp
// A fader — head value, slider, meters, actions
mixer.Fader( "audio.stream.244", "244 — eldenring.exe",
             ui::Bind<int>( GetStreamVolume, SetStreamVolume ), GetStreamPeaks )
     .Sub    ( []{ return "wine · pid 40217 · matched by process"; } )
     .Chip   ( []{ return "matched"; } )
     .Summary( []{ return std::format( "{}%", GetStreamVolume() ); } )
     .Help   ( "Volume of the stream gamescope matched to the launched game." );
```

---

## 5. Bindings

```cpp
namespace gamescope::ui
{
    template <typename T> struct Binding
    {
        T    Get() const;
        void Set( T v );                       // routes through ConfigManager's write queue
        T    Default() const;
        bool Differs() const;
        const char *DestinationFile() const;   // "global.json" / "games/1174180.json"
        const char *Key() const;
        const char *Unit() const;
    };

    // Preferred: default, type, range, unit and destination all come from ConfigSchema.h.
    template <typename T = auto> Binding<T> Cfg ( const char *pszKey );

    // Transient UI state, a live ConVar, a scratch buffer. Default and destination are
    // unknown, so the Inspector shows "session only" and the reset dot is suppressed —
    // the API degrades honestly rather than lying.
    template <typename T> Binding<T> Bind( T *p );
    template <typename T> Binding<T> Bind( gamescope::ConVar<T> & );
    template <typename T> Binding<T> Bind( std::function<T()>, std::function<void(T)> );
}
```

`ui::Cfg` is where the "which file does this write" logic that `PanelConfig` computes ad
hoc today (`SessionAppId()` / `IsSessionOverrideActive()`) is computed once, for everyone.

---

## 6. Adding a whole area

Complete, nothing omitted:

```cpp
// src/Overlay/UI/Areas/FrameLimiter.cpp
#include "UI/Registry.h"

void gamescope::ui::RegisterFrameLimiterArea( Registry &reg )
{
    auto &a = reg.Area( "display.frame_limiter", "Frame Limiter", Icon::Performance )
        .Section ( "DISPLAY" )
        .Describe( "Caps how often gamescope presents. Applied live via debug_set_fps_limit." )
        .Badge   ( []{ return g_nTargetFPS ? std::to_string( g_nTargetFPS ) : "—"; } );

    a.Group( "Limiter" );
    a.Number( "display.fps_limit", "FPS Limit", ui::Cfg( "display.fps_limit" ) )
     .Range( 0, 1000 ).Step( 5 ).Unit( "fps" ).Default( 0 ).ZeroMeans( "Unlimited" )
     .Help( "Caps the game frame rate. 0 removes the cap entirely." )
     .Keywords( "frame rate cap limiter throttle" );

    a.Group( "Pacing" );
    a.Switch( "display.low_latency", "Low-latency pacing", ui::Cfg( "display.low_latency" ) )
     .Default( false ).Hint( "holds the frame until the last safe moment" )
     .Help( "Delays the submit so the presented frame is as fresh as possible." )
     .Keywords( "latency pacing lag input delay" );

    a.Slider( "display.pacing_headroom", "Pacing headroom", ui::Cfg( "display.pacing_headroom" ) )
     .Range( 0, 5 ).Step( 0.1 ).Unit( "ms" ).Default( 1.0 ).Depth( ui::Depth::Expert )
     .Help( "How much slack the pacer leaves before the deadline." )
     .Keywords( "headroom safety margin pacing" );

    a.Readout( "limiter.vrr_range", "VRR range", []{ return VrrRangeString(); }, true );
    a.Readout( "limiter.pacing",    "Current pacing", []{ return PacingString(); }, true );
}
```

…plus **one line** in `Areas/Areas.cpp`. That is the whole task: a new rail item, a sheet
with two groups, a Category Card with a live block and an expert block, palette entries,
breadcrumb, per-category reset, provenance, keyboard reachability and the responsive
ladder — from ~25 lines with no geometry in them.

Compare today: a new panel needs a `PanelId` enum entry, an icon, a dock button, a
`BeginPanelWindow`/`EndPanelWindow` pair, a default size and tile position, a title
string, a draw function that hand-rolls its own tab bar and row layout, and a call added
to `SettingsOverlay.cpp`'s frame body — seven places, six of which can diverge.

---

## 7. The Inspector and the Card

```cpp
// Both hosts expose the SAME vocabulary as a sheet. What differs is what the
// helper does with it: rows are two-line, the label column is gone, the rail is
// the inspector's own, and Expert groups start collapsed.
class Panel { /* Group, Switch, Slider, Choice, Number, Readout, Note, Actions */ };
class Card  : public Panel { /* + Differs(), Live(), Provenance(), Expert() */ };
```

Nothing calls them directly for settings — the shell generates both from the registry:

```cpp
void Inspector::Draw( const Selection &sel )
{
    if ( sel.IsRow() )       DrawEntryPage( *sel.pEntry );   // control, default, range,
    else if ( sel.IsItem() ) DrawItemPage ( *sel.pItem  );   // help, writes, related, id
    else                     DrawCategoryCard( *CurrentArea() );
}
```

A list item owns an inspector page through a lambda that runs **only for the selected
item**, so nothing is built for the 200 effects you are not looking at:

```cpp
auto &fx = shaders.Items( "image.effects", "Effects" );
for ( ReshadeEffect &e : g_effects )
    fx.Item( e.pszName )
      .Switch( ui::Bind( &e.bEnabled ) )         // the list row's rail slot — a SWITCH
      .Meta  ( [&e]{ return e.SummaryString(); } )
      .Inspect( [&e]( ui::Panel &i )
      {
          i.Group( "Parameters" );
          i.Slider( "Strength", ui::Cfg( e.Key( "strength" ) ) ).Range( -1, 1 ).Default( 0 );
          i.Switch( "Protect skin tones", ui::Cfg( e.Key( "protect_skin_tones" ) ) ).Default( true );

          i.Group( "Adaptation", ui::Depth::Expert );      // starts collapsed
          i.Slider( "Min gain", ui::Cfg( e.Key( "min_gain" ) ) ).Range( 0, 2 ).Default( 0.8 );
          /* … */
      } );
```

A parameter looks and behaves identically in the sheet and in the inspector, so moving one
between them is a one-word change (`.Depth( Expert )`).

---

## 8. Internals — enough to judge feasibility

```cpp
// UI/Shell.cpp — the only place that computes region geometry.
struct ShellLayout
{
    ImRect slab, rail, sheet, inspector, header, footer;
    float  scale;             // palette::DisplayScale()
    bool   bRailIcons;        // ladder step 1
    bool   bInspectorDrawer;  // ladder step 2
    int    nSheetColumns;     // 1 or 2 (step 3, >= 1520 base only)
    float  flContentWidth;    // min( 720, sheet - 48 )
};
static ShellLayout ComputeLayout( ImVec2 surface );   // pure, unit-testable
```

```cpp
// UI/Row.cpp — every control funnels through this. The ONLY place an x exists.
ui::RowCtx ui::Sheet::BeginRow( const char *pszLabel, RowHeight h, const Entry *pEntry )
{
    const ShellLayout &L = Layout();
    const float w    = m_flContentWidth;
    const float rail = m_flOriginX + w - 36.0f * L.scale;      // ← THE RAIL
    const float ctlW = ImClamp( 0.50f * w, 180.0f * L.scale, 320.0f * L.scale );

    ImGui::PushID( pEntry ? pEntry->szId : pszLabel );          // STABLE string id
    const ImRect bb = AllocateRow( h );
    DrawStateEdge     ( bb, IsSelected(), pEntry && pEntry->Differs() );
    DrawRowBackground ( bb, IsSelected(), ImGui::IsItemHovered() );
    DrawLabel         ( bb, rail - ctlW - 16.0f * L.scale, pszLabel );

    ImGui::PushClipRect( ImVec2( bb.Min.x, bb.Min.y ), ImVec2( rail, bb.Max.y ), true );
    return RowCtx{ bb, rail, ctlW };
}

// A control chooses a WIDTH. It never chooses an x.
ImRect ui::RowCtx::Place( float flWidth ) const
{ return ImRect( m_flRail - ImMin( flWidth, m_flCtlW ), m_bb.Min.y,
                 m_flRail,                              m_bb.Max.y ); }
ImRect ui::RowCtx::Fill() const { return Place( m_flCtlW ); }
```

There is no other placement API. `widgets::SliderControl()` needs one change — take a rect
instead of using `GetContentRegionAvail()`. Toggle, segmented and position grid already
draw into a computed box and move over untouched.

Composites:

```cpp
void ui::Composite::Draw( Sheet &s )
{
    const ImVec2 top = ImGui::GetCursorScreenPos();
    s.BeginGroupEdge();                              // one state edge for the whole group
    s.Row( m_szTitle, RowHeight::Row, this ).Value( m_Summary() ).Affordance();
    for ( Part &p : m_Parts )
        s.Row( p.szSubLabel, p.Height(), p.pEntry ).Indent().NoHairline();
    s.EndGroupEdge( top );                           // one closing hairline
}
```

`NoHairline()` is private to `Composite.cpp`; a sheet call site cannot reach it.

Scroll and clipping: each region is a `BeginChild`; the sheet scrolls plainly (≤ ~40 rows
after §3); the LOG uses `ImGuiListClipper` (uniform 20-unit lines); selection uses
`ImGui::SetScrollHereY( 0.5f )`. Animation is one helper,
`float ui::Anim( float &cur, float target, float ms )`, available nowhere else, so a
caller cannot animate a value.

---

## 9. What stops inconsistency, mechanically

In decreasing order of strength:

1. **No geometry in the API.** No pixels, colours, fonts, alignments or `SameLine`.
   Enforced by type signatures.
2. **One placement primitive.** `RowCtx::Place( width )`. The rail is not a convention a
   call site follows; it is the only thing a call site can reach. Plus the clip rect,
   which truncates a violation visibly instead of misaligning it silently.
3. **Factories that do not exist.** No `Checkbox()`, no `Sheet::Readout()`, no
   `BeginTabBar()`. Three whole classes of critique are unreachable rather than
   discouraged.
4. **Required arguments where omission is the common bug.** `DisabledUnless` requires a
   reason. `Danger` requires `Confirm`. Numeric kinds require `Range` and `Default`.
   Every entry requires `Help`.
5. **Auto-downgrade instead of caller choice.** `Choice()` measures and picks segmented vs
   dropdown; the shell picks 1 or 2 columns; the ladder picks drawer vs column. A caller
   cannot pick wrong because a caller does not pick.
6. **Two row heights, one declared exception, two width classes.** There is no third to
   reach for.
7. **Assertions at registration**, at startup, in every build: missing `Help` / `Default`
   / `Range`; duplicate id; id not of the form `area.thing`; a `Danger` action without
   `Confirm`; a `Hint` over 64 characters.
8. **`ui_lint`** — a ConCommand that audits the live registry and the last frame:

   ```
   ] ui_lint
   ui_lint: 4 findings
     display.output/Force composite     no Help() — the Inspector would be empty
     monitor.font_size                  Bind() with no default — reset suppressed
     setup.appearance/Background veil    Hint() is 94 chars — cap is 64, use Help()
     setup.darken = 0.55                 below 0.65: TextMeta falls under 4.5:1 (SPEC §5)
   ```

9. **`ui_rulers`** — draws the control rail and the label origin over every sheet, so
   critique (2) is auditable from one screenshot rather than by eye.
10. **`ui_snapshot`** — dumps the whole registry as text (area, group, id, kind, bind,
    default, depth, help). Two runs diffed is a complete, reviewable record of a UI change
    — exactly what a doc-discipline repo wants attached to a PR.

### What "one obvious call" means, measured

| Task | Today | Here |
|---|---|---|
| add a toggle | ~6 lines in a panel + decide grouping/spacing/label style; no search, no reset, no help | 5 lines, everything included |
| add a slider with a dependency | ~10 lines + `BeginDisabled` pair + hint line, styled per panel | 6 lines |
| add a multi-part control | invent a layout | `Anchor()` / `SwitchList()` / `Fader()` — no layout to invent |
| add a new area | new `Panel*.cpp/h` (~400 lines), enum entry, icon, dock slot, `_Draw()` call, tile position, window size | one declaration file + one line in `RegisterAll()` |
| make everything consistent | manual review of 6 files | not expressible otherwise |

---

## 10. Config, palette and ConVars fall out for free

Because every entry carries an id, a bind, a default and keywords:

```cpp
// Ctrl+K, over the whole registry, from the first frame — not just visited categories.
ui::Palette::Search( "sharp" );      // -> Upscaling/Sharpness, Shaders/Pre-Sharpen, …

// Reset a whole area:
reg.Area( "display.upscaling" ).ResetAll();

// An addressable surface for gamescopectl and the script console:
//   gamescopectl ui set display.sharpness 7
//   gamescopectl ui get monitor.placement
//   gamescopectl ui search "frame"

// A notification when something changes from outside the overlay:
reg.OnChanged( []( const Entry &e ){
    Notifications::Post( std::format( "{} → {}", e.szTitle, e.FormatValue() ) ); } );
```

That last one is the thesis: the registry that exists so you can *find* a setting is the
same registry that lets the overlay *talk about* a setting, and the same registry that
guarantees it *looks like* every other setting.

---

## 11. Migration shape

```cpp
// A category whose draw function is still legacy panel code, hosted verbatim.
// Available from day one; deleted area-by-area as each is converted.
a.Escape( []{ PanelDisplay_DrawLegacyBody(); } );
```

`Escape()` pushes the old `ImGuiStyle`, runs the old code inside the sheet's child, pops.
It looks wrong — that is the point — it is the only function in the API that permits
arbitrary ImGui, and it is expected to have zero call sites when the port completes.
`ui_lint` counts remaining `Escape()` sites as severity `migration`.

Cost and sequencing: `FEASIBILITY.md` §7.
