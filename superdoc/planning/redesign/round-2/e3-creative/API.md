# `gamescope::ui` — the helper layer for **The Bench** (E3)

The user's goal, in their words:

> *"what i want is basically our own framework/helper functions (on top of ImGui), that
> makes it easier for you/AI, to update and extend the UI, while keeping it consistent
> with the rest."*

The test: **adding a setting is one obvious call, and producing something inconsistent is
hard.** E3 adds one genuinely new capability (the Probe) and one genuinely new law (the
Lane). Neither is allowed to cost the one-call property, and §7 states the budget this
API is held to.

---

## 1. Files

```
src/Overlay/UI/
    Registry.h / .cpp    // Entry, Area, Registry, the fluent builders
    Bind.h               // value binding: pointer, ConVar, getter/setter, config key
    Probe.h / .cpp       // the six probe factories + the Delta fallback
    Shell.h / .cpp       // slab rects, the ladder, the Spine, the Ledger, peek, input
    Scope.h / .cpp       // the depth region: probe, statement, facts, expert, category
    Row.h / .cpp         // THE ONLY PLACE A CONTROL RECT IS PRODUCED
    Controls.cpp         // one painter per control kind, each taking a lane rect
    Match.h / .cpp       // fuzzy scorer + highlight positions (the palette)
    Theme.h / .cpp       // roles, u(), type roles, Anim()
    Lint.cpp             // ui_lint / ui_snapshot ConCommands
    Areas/
        Display.cpp  Shaders.cpp  Audio.cpp  Monitor.cpp  Log.cpp
        Profiles.cpp Appearance.cpp PerGame.cpp
        Areas.cpp        // RegisterAll() -- the one place an area is added
```

`Palette.h/cpp` and `Fonts.h/cpp` survive as-is; `Theme.h` is a thin role layer on top.
`Widgets.cpp`'s `SliderControl()`, `Toggle()`, `SegmentedControl()` and `PositionGrid()`
are *reused* as the painting bodies inside `Controls.cpp`, with one signature change each
(§6). `Chrome.cpp`'s dock / window / title-bar / tiling / drag / collapse machinery is
deleted.

---

## 2. Registration — one declaration, four products

This is Direction B's registry, adopted whole as `redesign/README.md` records. One
declaration yields:

1. the **Spine row**,
2. the **`Ctrl+K` palette entry**,
3. the **Scope page** — help, facts, provenance, reset, expert params, related links,
4. the **Ledger tick** when the value differs from its default.

```cpp
namespace gamescope::ui
{
    class Entry   // returned by every Area::Xxx() factory; fluent; nothing here is visual
    {
    public:
        Entry &Help    ( const char *sz );          // Scope statement.  REQUIRED.
        Entry &Keywords( const char *sz );          // extra palette search terms
        Entry &Hint    ( const char *sz );          // one line under the label, <=64 chars
        Entry &Unit    ( const char *sz );          // "%", "fps", "ms", "nits", "px", "x"
        Entry &Range   ( double lo, double hi );    // REQUIRED for numeric kinds
        Entry &Step    ( double coarse, double fine = 0 );  // default: range/40, /200
        Entry &Default ( ... );                     // REQUIRED for value kinds
        Entry &ZeroMeans( const char *sz );         // "Unlimited", "Off", ...
        Entry &EnabledWhen ( std::function<bool()>, const char *szWhyNot ); // no overload
        Entry &AvailableWhen( std::function<bool()> );   // absent entirely (no hardware)
        Entry &Related ( std::initializer_list<const char*> ids );  // Scope jump links
        Entry &Writes  ( ConfigTarget );            // Global | Routed (default)
        Entry &Probe   ( ui::Probe );               // OPTIONAL. See section 4.
        Entry &Expert  ( std::function<void(Scope&)> );  // Scope-only parameters
        Entry &Order   ( int );
    };

    class Area
    {
    public:
        Area &Keywords ( const char *sz );
        Area &Section  ( const char *sz );          // "DISPLAY" / "IMAGE" / "SYSTEM" / "SETUP"
        Area &AvailableWhen( std::function<bool()> );
        Area &Badge    ( std::function<std::string()> );  // the accordion header's count chip
        void  Group    ( const char *szName, GroupActions = {} );  // {} | All_None | Custom

        // ---- the complete control taxonomy. There are no other factories. ----
        Entry &Switch  ( const char *szId, const char *szTitle, Bind<bool> );
        Entry &Selector( const char *szId, const char *szTitle, Bind<int>,
                         std::span<const Option> );        // density chosen by measurement
        Entry &Slider  ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Slider  ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Stepper ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Text    ( const char *szId, const char *szTitle, Bind<std::string> );
        Entry &Readout ( const char *szId, const char *szTitle,
                         std::function<std::string()>, std::function<bool()> live = {} );
        Entry &Meter   ( const char *szId, const char *szTitle,
                         std::function<float()>, float lo, float hi );
        Entry &Actions ( const char *szId, const char *szTitle );   // then .Neutral/.Accent/.Danger
        // ---- composites: lane-width, growing downward, promoted in the Scope ----
        Entry &Stage   ( const char *szId, const char *szTitle,
                         Bind<int> anchorV, Bind<int> anchorH,
                         Bind<int> marginV, Bind<int> marginH );
        Entry &Hue     ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Stream  ( const char *szId, const char *szTitle, StreamSource );  // the LOG

        // ---- the fenced escape hatch, unchanged from B ----
        Entry &Pane    ( const char *szId, const char *szTitle,
                         std::function<std::string()> summary,
                         std::function<void(PaneCtx&)> draw );
    };

    class Registry { public: Area &Area( const char*, const char*, Icon ); void SelfTest(); };
    void RegisterAll( Registry & );
}
```

**Note what is absent and cannot be added without editing this header:** any width,
height, padding, gap, colour, font, **alignment**, or animation parameter. A call site
physically cannot express *"make this one a bit wider"* or *"put this one on the left"*.

`SettingsOverlay.cpp`'s per-frame body becomes, in full:

```cpp
ui::Shell::Draw();
```

---

## 3. Call sites

### 3.1 A switch — the whole thing

```cpp
area.Switch( "display.tearing", "Allow Tearing", &g_bAllowTearing )
    .Default( false )
    .Help( "Presents frames the moment they are ready instead of waiting for vblank. "
           "Lowest latency; a horizontal seam can appear mid-screen." )
    .Keywords( "immediate flips vsync latency tear" );
```

What that produced, none of which the caller asked for: a 42-base row hairline-separated
from its neighbours; the label in Sans 13.5 `TextBody` on the same vertical line as every
other label; a 34×17 switch flush to the lane's right edge, on the same vertical line as
every other control; hover, press and keyboard-focus painting; a state edge and a Ledger
tick the moment the value leaves its default; `Ctrl+D` reset; the help routed to the
Scope along with `default: off`, `range: —` and `writes: games/1245620.json`, all read
from the binding; a palette entry under five spellings; and the **Delta probe** at the top
of the Scope, generated from the binding.

### 3.2 A slider with a dependency

```cpp
area.Slider( "display.sharpness", "Sharpness", cv_sharpness )
    .Range( 0, 20 ).Step( 1, 1 ).Default( 2 )
    .Hint( "higher = sharper" )
    .Help( "Strength of FSR's RCAS / NIS's sharpening pass. Too high adds ringing "
           "around high-contrast edges." )
    .Keywords( "sharpen rcas cas clarity ringing" )
    .Related( { "display.filter" } )
    .EnabledWhen( []{ return IsSharpeningFilter(); },
                  "the scaling filter is not fsr, nis or pixel" )
    .Probe( Probe::Frame( FrameEffect::Sharpen ) );
```

Absent: no format string, no min/max label strings, no width, no `ImGuiSliderFlags`, no
alignment. The format comes from the binding's declared type and unit; the marks from the
range; the default tick and the Trail from the default; `AlwaysClamp` is always on
because there is no reason for it not to be.

`EnabledWhen()` has **no overload without a reason string**. That single signature is the
entire enforcement mechanism for the most common inconsistency in the current code — a
control that greys out and does not say why.

### 3.3 A Selector — the caller does not choose the density

```cpp
static constexpr ui::Option kFilters[] = {
    {"linear","linear"},{"nearest","nearest"},{"fsr","fsr"},{"nis","nis"},{"pixel","pixel"} };

area.Selector( "display.filter", "Filter", (int*)&g_upscaleFilter, kFilters )
    .Default( 2 )
    .Help( "How the game's image is resampled to the output resolution. FSR and NIS add "
           "a sharpening pass; pixel is a nearest-neighbour integer path for pixel art." )
    .Keywords( "upscale scaling resample fsr nis crisp" )
    .Probe( Probe::Frame( FrameEffect::Upscale ) );
```

Five options whose labels do not all fit the lane → the helper renders the **compressed**
density (active label plus slivers plus `‹ ›`). Drop `nearest` and it renders **expanded**
(four labelled cells). Add four more streams and it stays compressed and grows a
searchable list on click. **The call site does not change and does not get a say** — the
measurement is `Row::LaneWidth()` against `ImGui::CalcTextSize()`, done once per frame in
`Controls.cpp`.

### 3.4 The Stage — one call replaces three rows

```cpp
area.Group( "Placement" );
area.Stage( "monitor.anchor", "Placement",
            &g_fpsHud.nAnchorVert,  &g_fpsHud.nAnchorHoriz,
            &g_fpsHud.nMarginVert,  &g_fpsHud.nMarginHoriz )
    .Default( 0, 2, 32, 32 )                      // top-right, 32 / 32
    .Help( "Which screen edge the monitor is pinned to, and how far in from it. Drag the "
           "block on the Scope's Stage, or click one of the nine anchors." )
    .Keywords( "anchor placement position corner margin offset where" )
    .Probe( Probe::Stage( &FpsDisplay::Geometry ) );   // the SAME function the HUD uses
```

That is the whole of `Monitor ▸ Placement`. It replaces, today, a `widgets::PositionGrid`
call plus two `widgets::SliderFloat` calls plus the hand-rolled row layout around them —
and it replaces the *second* copy of all of that in the notifications panel, because
there is now exactly one way to express "pick a corner and an inset".

`Probe::Stage( geomFn )` is the part that matters: the Stage draws the block by calling
the *same* `FpsDisplay::Geometry()` that positions the real readout. There is no second
implementation to drift, and dragging the block in the Scope moves the real HUD over the
game because they are the same numbers.

`.Default()` on a `Stage` takes four values, and `Registry::SelfTest()` asserts if it
takes any other count. That is the one place in the API where an entry kind changes an
argument count, and it is checked at registration rather than at render.

### 3.5 A destructive action

```cpp
area.Actions( "pergame.acts", "Config actions" )
    .Neutral( "copy from another game", &OnCopyConfig )
    .Danger ( "delete saved config", &OnDeleteConfig )
      .Confirm( "This deletes games/1245620.json permanently. This cannot be undone." )
    .AvailableWhen( []{ return ConfigManager::HasAppConfig( SessionAppId() ); } );
```

`Confirm()` is a **compile-time** requirement, not a convention: `Danger()` returns a
distinct `DangerAction` type whose only method is `Confirm()`, and only `Confirm()`
returns the chainable `ActionRow&`. Code that forgets it does not build.

### 3.6 Expert parameters — the Inspector Contract, made mechanical

```cpp
area.Switch( "fx.adaptive", "Adaptive Brightness", &g_fx.adaptive.bEnabled )
    .Default( false ).Hint( "6 tuning values in the Scope" )
    .Help( "Continuously re-exposes the frame towards a target average brightness, the "
           "way an eye adapts walking out of a cave." )
    .Keywords( "adaptive brightness eye exposure auto gain" )
    .Probe( Probe::Trace( TraceSource::AdaptiveGain ) )
    .Expert( []( ui::Scope &s )
    {
        s.Group( "Adaptation" );
        s.Slider( "fx.adaptive.target",  "Target brightness", cv_adapt_target  ).Range(0,1).Default(.42);
        s.Slider( "fx.adaptive.mingain", "Min gain",  cv_adapt_min ).Range(0,2).Default(.8 ).Unit("x");
        s.Slider( "fx.adaptive.maxgain", "Max gain",  cv_adapt_max ).Range(0,4).Default(1.6).Unit("x");
        s.Slider( "fx.adaptive.up",   "Brighten speed", cv_adapt_up   ).Range(0,5).Default(1.2);
        s.Slider( "fx.adaptive.down", "Darken speed",   cv_adapt_down ).Range(0,5).Default(2.4);
    } );
```

`ui::Scope` exposes **the same vocabulary as `ui::Area`**, so a parameter looks and
behaves identically wherever it lives, and moving one between the Spine and the Scope is
a one-word change. `Expert()`'s lambda runs **only for the selected entry**, so nothing is
built for the effects you are not looking at — which is what keeps this
immediate-mode-cheap.

`Expert()` is also the *only* way to put a control in the Scope. That is what makes the
Inspector Contract mechanical rather than a process defence: a caller cannot decide "this
one goes in the depth panel" for an ordinary setting, because the only door into the Scope
is a method named `Expert`, and `ui_lint` reports any `Expert()` block with more than six
rows as *"the Scope is becoming a dumping ground"*.

### 3.7 A whole new area

```cpp
// src/Overlay/UI/Areas/Lsfg.cpp  -- an entire new category. No UI code.
void gamescope::ui::RegisterLsfgArea( Registry &reg )
{
    auto &area = reg.Area( "lsfg", "Frame Generation", Icon::Performance )
        .Section( "DISPLAY" )
        .Keywords( "lsfg lossless fg interpolation smoothing frame gen" )
        .AvailableWhen( []{ return lsfg::IsAvailable(); } )
        .Badge( []{ return lsfg::IsEnabled() ? std::format("{}x", lsfg::Multiplier())
                                             : std::string("off"); } );

    area.Group( "Generation" );
    area.Switch( "lsfg.enabled", "Enable", &g_lsfg.bEnabled )
        .Default( false )
        .Help( "Generates intermediate frames between rendered ones. Adds latency." )
        .Keywords( "enable on off frame generation" )
        .Probe( Probe::Trace( TraceSource::Frametime ) );

    static constexpr Option kMult[] = { {"2","2x"},{"3","3x"},{"4","4x"} };
    area.Selector( "lsfg.multiplier", "Multiplier", &g_lsfg.nMultiplier, kMult )
        .Default( 0 )
        .EnabledWhen( []{ return g_lsfg.bEnabled; }, "frame generation is off" )
        .Help( "How many frames are produced per rendered frame." )
        .Keywords( "multiplier 2x 3x 4x how many" );
}
```

...plus **one line** in `Areas/Areas.cpp`. The new category now appears in the Spine under
DISPLAY with a live badge, is searchable, is keyboard-reachable, has a live frametime
probe in the Scope, routes to the right config file, and looks exactly like every other
category — because nobody drew it.

Compare today: a new panel needs a `PanelId` enum entry, an icon, a dock button, a
`BeginPanelWindow`/`EndPanelWindow` pair, a default size, a tile position, a title string,
a draw function that hand-rolls its own tab bar and row layout, and a call in
`SettingsOverlay.cpp` — seven places, six of which are opportunities to diverge.

---

## 4. `Probe` — the new capability, and its budget

The Scope's top block. **Six factories, a closed set.** A caller picks one or picks
nothing; there is no way to draw into a probe.

```cpp
namespace gamescope::ui
{
    enum class FrameEffect  { Upscale, Sharpen, Gamut, Vibrancy, PreSharpen, Custom };
    enum class TraceSource  { Frametime, Refresh, AdaptiveGain };
    enum class CurveKind    { Pq, Tonemap, Gamma };
    enum class MeterSource  { AudioPeak, GpuBusy, CpuLoad, Vram };

    class Probe   // opaque; constructed only by these factories
    {
    public:
        static Probe Frame   ( FrameEffect );                  // before | after wipe
        static Probe Trace   ( TraceSource );                  // 240-sample strip
        static Probe Curve   ( CurveKind );                    // transfer curve + marker
        static Probe Meter   ( MeterSource );                  // live segmented bar(s)
        static Probe Stage   ( GeometryFn );                   // the output miniature
        static Probe Swatches();                               // the OKLCH accent family
        // and, implicitly, Probe::Delta() -- never written by a caller
    };
}
```

**`Probe` is optional, and its absence is the interesting case.** An entry with no
`.Probe()` gets the **Delta** probe, generated from `Binding` alone: the current value at
Mono 600 30, a number line with the default ticked, the current value marked, and the span
between them filled. This is what makes *"the Scope is always a live instrument"* a
guarantee rather than an aspiration — a caller who never thinks about the Scope still gets
a good Scope, and **no selection can produce an empty Scope**.

### Why this does not violate "one obvious call"

It adds **one optional chained method whose argument comes from a closed enum**. It adds
no geometry, no colour, no size, no callback with a draw context, and no new concept at
the call site: `Probe::Frame( FrameEffect::Sharpen )` is a *declaration of what this
setting affects*, in exactly the register of `.Unit( "fps" )` or `.Range( 0, 20 )`.

The cost is real and it is paid **once, in `Probe.cpp`**, not per call site: six draw
implementations totalling perhaps 400 lines. That is the trade this direction is making
deliberately — a fixed one-time cost in the helper, in exchange for the overlay showing
what a setting does rather than describing it, which is the one thing a compositor's own
UI can do that a desktop settings dialog cannot.

**If a probe implementation is cut** (see `FEASIBILITY.md` §5 — `Frame` is the likely
candidate), the factory is removed, the affected call sites drop one chained method, and
they fall back to `Delta`. Nothing structural changes. The API is designed so the most
ambitious part is the most removable part.

---

## 5. Bindings — where "one obvious call" actually comes from

```cpp
namespace gamescope::ui
{
    template <typename T> class Bind
    {
    public:
        Bind( T *p );                                       // plain global / struct field
        Bind( gamescope::ConVar<T> &cv );                   // a ConVar
        Bind( std::function<T()> get, std::function<void(T)> set );  // side-effecting write
        Bind( CfgKey k );                                   // the config schema, by key

        T    Get() const;
        void Set( T v ) const;                              // routes through the write queue
        T    Default() const;
        bool Differs() const;
        const char *DestinationFile() const;                // "global.json" / "games/…json"
        const char *RoutingReason()  const;                 // why that file
    };
}
```

This is the load-bearing idea, unchanged from B and E. Because the binding knows the key,
**one call site produces the control, the default tick, the Trail, the reset action, the
Ledger tick, the provenance line, the routing reason, the palette entry, and the Delta
probe.** None of it is typed by the caller, and none of it can be forgotten, because
forgetting it would mean not binding the setting at all.

`Bind( CfgKey )` is also the seam where the "which file does this write" logic that
`PanelConfig.cpp` computes ad hoc today (`SessionAppId()` / `IsSessionOverrideActive()`)
is computed once.

A `Bind( T* )` to a transient variable has no default and no destination; the Scope says
`session only` and the reset affordance is suppressed. The API degrades **honestly**
rather than lying.

---

## 6. The Lane Law, mechanically

```cpp
// UI/Row.h -- the ONLY struct in the tree that carries a control rect.
struct RowCtx
{
    ImRect row;      // the full row
    ImRect label;    // x .. lane.Min.x - gap
    ImRect lane;     // FIXED 190u wide, flush to the container's right inner edge
    bool   selected, differs, disabled;
};

// UI/Row.cpp -- every control funnels through this. One function, one file.
RowCtx Row::Begin( const char *pszLabel, RowHeight h, const BindingView *pBind )
{
    const Layout &L    = Shell::Layout();
    const float   lane = L.u( 190 );
    const ImRect  bb   = AllocateRow( h );                 // ItemSize + ItemAdd
    ...
    return RowCtx{ bb,
                   ImRect( bb.Min.x + L.u(22), bb.Min.y, bb.Max.x - lane - L.u(16), bb.Max.y ),
                   ImRect( bb.Max.x - lane,    bb.Min.y, bb.Max.x,                  bb.Max.y ),
                   bSel, pBind && pBind->Differs(), bDisabled };
}
```

Every painter in `Controls.cpp` has the shape:

```cpp
bool controls::Switch  ( const RowCtx &, Bind<bool>  & );
bool controls::Selector( const RowCtx &, Bind<int>   &, std::span<const Option> );
bool controls::Slider  ( const RowCtx &, Bind<float> &, const NumericMeta & );
bool controls::Stage   ( const RowCtx &, const StageBinds &, GeometryFn );
```

and draws **only inside `ctx.lane`**. Three enforcement layers, in decreasing strength:

1. **The painters are handed a rect.** None of them calls `GetContentRegionAvail()`,
   `GetCursorPos()`, `SameLine()`, `PushItemWidth()` or `SetCursorPosX()`. They cannot ask
   how much room there is, so they cannot make a decision about it.
2. **There is no alignment parameter anywhere in `Registry.h`.** A caller cannot express
   the question that produced *"switches are left bound, right bound, centered (basically
   random)"*.
3. **`ui_lint` greps.** A float literal in `Areas/`, or any of the five forbidden ImGui
   calls in `Controls.cpp`, is a reported finding.

`Widgets.cpp`'s existing painters need exactly one change each: take an `ImRect` instead
of consuming `GetContentRegionAvail()`. `Toggle`, `SegmentedControl` and `PositionGrid`
already draw into a computed box and move over almost untouched.

---

## 7. What stops inconsistency, mechanically

Seven mechanisms, in decreasing order of strength:

1. **No geometry in the API.** No pixels, no colours, no fonts, no alignment, no
   `SameLine`. Enforced by type signatures — the strongest form.
2. **The Lane Law** (§6). One lane, one right edge, produced in one function.
3. **Required arguments where omission is the common bug.** `EnabledWhen` requires a
   reason. `Danger` requires `Confirm` (at compile time). `Slider`/`Stepper` require a
   range. Every value entry requires a `Default`. Every entry requires `Help`.
4. **Auto-selection instead of caller choice.** The Selector's density is measured. The
   ladder step is computed. The probe falls back to Delta. A caller cannot pick wrong
   because a caller does not pick.
5. **Two row heights, one lane width, six type roles, three text alphas.** There is no
   third, no fourth, no seventh to reach for.
6. **`ui_lint`** — a ConCommand that audits the live registry and the last frame:
   ```
   ] ui_lint
   ui_lint: 5 findings
     display.output/Force Grab Cursor   no Help() -- the Scope has nowhere to look
     monitor.font_size                  bound with Bind(T*), no default -- reset suppressed
     setup.appearance/Background veil   Hint() is 94 chars -- cap is 64, use Help()
     fx.adaptive                        Expert() block has 9 rows -- the Scope is becoming
                                        a dumping ground (cap 6, see SPEC.md 2.3)
     Areas/Monitor.cpp:118              float literal in an Areas/ file
   ```
7. **`ui_snapshot`** — dumps the whole registry as text: category, row, kind, key,
   default, probe, help. Two runs diffed is a complete, reviewable record of a UI change,
   attachable to a PR, which is exactly what a doc-discipline repo wants.

### What "one obvious call" means, measured

| Task | Today | Here |
|---|---|---|
| add a switch | ~6 lines in a panel + decide grouping / spacing / label style; no search, no reset, no help channel | **4 lines**, everything included |
| add a slider with a dependency | ~10 lines + a `BeginDisabled` pair + a hint line, styled per-panel | **6 lines** |
| add a slider with a live preview | not possible | **7 lines** (one extra `.Probe()`) |
| add an anchor + margins control | `PositionGrid` + 2 sliders + hand layout, done twice in two files | **1 call**, one call site |
| add a whole category | new `Panel*.cpp/h` (~400 lines), enum entry, icon, dock slot, `_Draw()` call, tile position, window size | one declarations file + one line in `RegisterAll()` |
| make everything consistent | manual review of 6 files | not possible to break |

---

## 8. Migration — `Escape()` and the incremental path

```cpp
// A category whose draw body is legacy panel code, hosted verbatim in the Spine.
// Available from PR 1; deleted category-by-category as each is converted.
area.Escape( []{ PanelDisplay_DrawLegacyBody(); } );
```

`Escape()` pushes the old `ImGuiStyle`, runs the old code inside the Spine's child, pops.
It looks wrong — that is the point, it is visibly the un-migrated part — it is the only
function in the API that permits arbitrary ImGui, and it is expected to have zero call
sites when the port completes. `ui_lint` counts remaining `Escape()` sites at severity
`migration`. Sequencing and cost are in `FEASIBILITY.md` §8.

---

## 9. What falls out for free

Because every entry carries a stable id, a bind, a default and a probe:

```cpp
// gamescopectl / the script console get an addressable surface, unchanged from B:
//   gamescopectl ui set display.sharpness 7
//   gamescopectl ui get monitor.anchor
//   gamescopectl ui search "frame"

// reset a whole category -- what the Scope's "reset category" button runs:
reg.Area( "display.upscaling" ).ResetAll();

// a notification when something changes from outside the overlay:
reg.OnChanged( []( const Entry &e ) {
    Notifications::Post( std::format( "{} -> {}", e.Title(), e.FormatValue() ) );
} );

// the Ledger, in its entirety:
for ( const Entry &e : reg.All() )
    if ( e.Differs() )
        Ledger::Tick( e.NormalisedIndex(), &e );
```

That last one is the clearest illustration of the direction's thesis: the registry that
exists so you can *find* a setting is the same registry that lets the overlay *show you*
which settings you have moved, and the same registry that guarantees they all *look like*
each other.
