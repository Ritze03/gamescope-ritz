# The helper layer — `gamescope::ui`

**The registry that powers search is the registry that enforces consistency.** One declaration
feeds: fuzzy search, browse ordering, the gamepad's five verbs, row rendering, reset-to-default,
config routing, the Inspector's help text, and the docs. Adding a setting is one call. Producing
something inconsistent requires reaching a drawing API that call sites do not have.

Design targets, in priority order:

1. **Adding a setting is one obvious call**, and the call site says *what the setting is*, never
   *how it looks*.
2. **Layout, spacing, colour, font and motion are decided by the helper**, never by the caller.
3. **The escape hatch is fenced**: arbitrary drawing goes through `PaneCtx`, which exposes only
   family-consistent primitives and no `ImDrawList`, no `ImGui::`, and no `ImU32`.
4. **Misuse fails loudly at registration**, not subtly at render time.

---

## 1. Files

```
src/Overlay/UI/
    Registry.h / .cpp      // Entry, Area, Registry, the fluent builders
    Bind.h                 // value binding: raw pointer, ConVar, getter/setter
    Match.h / .cpp         // fuzzy scorer + highlight positions
    Console.h / .cpp       // the one window: query line, body, inspector, legend
    RowRender.cpp          // one function per control type — the ONLY place geometry lives
    PaneCtx.h / .cpp       // the fenced escape hatch
    Theme.h / .cpp         // roles, u(), type roles, the four animations
    Areas/
        Display.cpp  Shaders.cpp  Audio.cpp  Monitor.cpp  Profiles.cpp  Log.cpp
        Areas.cpp          // RegisterAll() — the one place an area is added
```

`Palette.h/cpp` and `Fonts.h/cpp` survive largely as-is; `Theme.h` is a thin role layer on top of
them. `Widgets.cpp`'s `SliderControl()`, `Toggle()`, `SegmentedControl()`, `PositionGrid()` are
*reused* as the expanded/inline draw bodies inside `RowRender.cpp` — this layer is a container and
a contract, not a rewrite of the widget drawing that already matches spec.

---

## 2. Value binding

```cpp
namespace gamescope::ui
{
    // A read/write handle to whatever actually stores the value. Three
    // constructions cover every call site in the current codebase.
    template <typename T>
    class Bind
    {
    public:
        Bind( T *p );                                   // plain global / struct field
        Bind( gamescope::ConVar<T> &cv );               // a ConVar (reads .Get(), writes .Set())
        Bind( std::function<T()> get,
              std::function<void(T)> set );             // side-effecting write

        T    Get() const;
        void Set( T v ) const;                          // fires the registry's change hook
    };
}
```

`Bind` is the only way a value reaches the UI. It exists so that the *renderer* never knows
whether it is driving a `bool`, a `ConVar<bool>`, or a setter that also has to poke
`steamcompmgr`. The row for `display.tearing` is byte-identical whichever it is.

---

## 3. Registration API

```cpp
namespace gamescope::ui
{
    // ---- one entry ------------------------------------------------------
    // Returned by every Area::Xxx() factory. Fluent; every method returns *this.
    // Nothing here describes appearance. There is no size, no colour, no font,
    // no position, no padding parameter anywhere in this class -- by design.
    class Entry
    {
    public:
        Entry &Help( const char *sz );        // Inspector line 1. REQUIRED.
        Entry &Keywords( const char *sz );    // space-separated extra search terms
        Entry &Unit( const char *sz );        // "%", "fps", "ms", "nits", "px", "dB"
        Entry &Range( double lo, double hi ); // numeric types. REQUIRED for them.
        Entry &Step( double coarse, double fine ); // <-/-> and L2+<-/->.  Default: range/40, /200
        Entry &Default( ... );                // REQUIRED. X / ^R restore this.
        Entry &ZeroMeans( const char *sz );   // "Unlimited" for FPS Limit, "Off", ...
        Entry &EnabledWhen( std::function<bool()> pred,
                            const char *szWhyNot );  // greys the row; Inspector says why
        Entry &AvailableWhen( std::function<bool()> pred ); // absent entirely (no hardware)
        Entry &Danger();                      // state/danger roles + Confirm pane
        Entry &Pinnable( bool = true );       // default true for values, false for actions
        Entry &Order( int );                  // within its group; default = registration order
        Entry &Group( const char *sz );       // overrides the Area's current group cursor
        Entry &Suggestions( std::function<std::vector<std::string>()> ); // Text only
        Entry &KeyboardOnly();                // Text only; excluded from gamepad browse
        Entry &Writes( ConfigTarget );        // Global / Routed (default) -- Inspector shows it
    };

    // ---- one area -------------------------------------------------------
    class Area
    {
    public:
        Area &Keywords( const char *sz );
        Area &AvailableWhen( std::function<bool()> );
        Area &Summary( std::function<std::string()> ); // Home tile's live one-liner
        void  Group( const char *szName );    // sets the group cursor for following entries

        // --- the entire control taxonomy. There are no other factories. ---
        Entry &Toggle ( const char *szId, const char *szTitle, Bind<bool> );
        Entry &Slider ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Slider ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Number ( const char *szId, const char *szTitle, Bind<int> );   // stepper
        Entry &Choice ( const char *szId, const char *szTitle, Bind<int>,
                        std::span<const Option> );        // <=4 -> segmented, >4 -> value+caret
        Entry &Multi  ( const char *szId, const char *szTitle, Bind<uint32_t> bitfield,
                        std::span<const Option> );        // chip bank
        Entry &Hue    ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Position( const char *szId, const char *szTitle,
                         Bind<int> vert, Bind<int> horiz );// the 3x3
        Entry &Text   ( const char *szId, const char *szTitle, Bind<std::string> );
        Entry &Action ( const char *szId, const char *szTitle, const char *szVerb,
                        std::function<void()> );
        Entry &Readout( const char *szId, const char *szTitle,
                        std::function<std::string()>, bool bLiveDot = false );
        Entry &Pane   ( const char *szId, const char *szTitle,
                        std::function<std::string()> szSummary,
                        std::function<void(PaneCtx&)> draw );
    };

    class Registry
    {
    public:
        Area &Area( const char *szId, const char *szTitle, Icon );
        void  SelfTest();                     // debug builds; see section 7
    };

    void RegisterAll( Registry & );           // Areas/Areas.cpp
}
```

Note what is **absent** and cannot be added without editing this header: any width, height,
padding, gap, colour, font, alignment or animation parameter. A call site physically cannot
express "make this one a bit wider".

---

## 4. Call sites

### 4.1 Adding a toggle — one call

```cpp
area.Toggle( "display.tearing", "Allow Tearing", &g_bAllowTearing )
    .Default( false )
    .Help( "Present frames the moment they are ready, ignoring vblank. "
           "Lowest latency, visible tear lines." )
    .Keywords( "immediate flips vsync latency tear" );
```

That is the whole thing. It is now: searchable by five spellings, browsable in the Display area,
adjustable with `←→` or `A` or a click, resettable with `X`/`^R`, pinnable to the Quick Wheel,
routed to the correct config file, and documented in the Inspector — with no UI code written.

### 4.2 Adding a slider — one call

```cpp
area.Slider( "display.sharpness", "Sharpness", cv_sharpness )
    .Range( 0, 20 ).Step( 1, 1 )
    .Default( 2 )
    .Help( "FSR / NIS sharpening strength. Higher is sharper; too high adds "
           "ringing around high-contrast edges." )
    .Keywords( "sharpen fsr rcas cas crisp clarity" )
    .EnabledWhen( []{ return g_upscaleFilter == GamescopeUpscaleFilter::FSR
                          || g_upscaleFilter == GamescopeUpscaleFilter::NIS; },
                  "the scaling filter is not FSR or NIS" );
```

The collapsed mini-track, the expanded full `SliderControl()` with min/max marks, the tabular
Mono value, the disabled treatment, the fine step under `L2` — all of it follows from these six
lines. Compare `PanelDisplay.cpp` today: a `PushItemWidth`, a `widgets::SliderFloat`, a
`ImGui::BeginDisabled`/`EndDisabled` pair, a `TextDisabled` hint, a `Spacing`, and a hand-placed
`SameLine` — each an opportunity to differ from the panel next door.

### 4.3 A side-effecting write

```cpp
area.Number( "display.fps_limit", "FPS Limit",
             ui::Bind<int>( []{ return g_nSteamCompMgrTargetFPS; },
                            []( int v ){ steamcompmgr_set_fps_limit( v ); } ) )
    .Range( 0, 360 ).Step( 5, 1 ).Unit( "fps" )
    .Default( 0 ).ZeroMeans( "Unlimited" )
    .Help( "Caps the game's frame rate. 0 disables the limiter entirely." )
    .Keywords( "frame rate cap limiter throttle" );
```

`.ZeroMeans()` exists because "0 means Unlimited" is exactly the kind of one-off the current
codebase hand-codes differently in two places. Declared once, it renders neutrally (not in
`accent/value`), reads correctly on the Quick Wheel, and is spoken correctly by the Inspector.

### 4.4 A choice, a multi-select, a position grid

```cpp
static constexpr ui::Option kFilters[] = {
    { "linear", "Linear" }, { "nearest", "Nearest" }, { "fsr", "FSR" },
    { "nis", "NIS" }, { "pixel", "Pixel" } };

area.Choice( "display.filter", "Scaling Filter", (int*)&g_upscaleFilter, kFilters )
    .Default( 0 )
    .Help( "How the game's image is resampled to the output resolution." )
    .Keywords( "upscale scaling resample fsr nis" );
// 5 options -> renders as value + caret with <-/-> cycling and a searchable List pane on Enter.
// Drop one option and it renders as an inline segmented control instead. The call site does not
// change, and does not get a say.

area.Multi( "monitor.modules", "Modules", &g_fpsHud.uModuleMask, kMonitorModules )
    .Default( kModFps | kModGpu )
    .Help( "Which modules the monitor draws. Every enabled module renders at the "
           "widest enabled module's width." )
    .Keywords( "fps frametime cpu gpu media rows which show" );

area.Position( "monitor.anchor", "Placement",
               &g_fpsHud.nAnchorVert, &g_fpsHud.nAnchorHoriz )
    .Default( 0, 2 )   // top-right
    .Help( "Which screen corner the monitor is anchored to." )
    .Keywords( "anchor placement position corner where" );
```

### 4.5 A destructive action

```cpp
area.Action( "profiles.delete", "Delete Saved Config", "DELETE",
             []{ ConfigManager::DeleteAppConfig( SessionAppId() ); } )
    .Danger()
    .Help( "Deletes this game's saved config permanently. This cannot be undone." )
    .Keywords( "delete remove destroy reset saved config" )
    .AvailableWhen( []{ return ConfigManager::HasAppConfig( SessionAppId() ); } );
```

`.Danger()` is the only thing needed: `state/danger` roles on the verb chip and value, and a
Confirm pane on `Enter`/`A` instead of running immediately. The call site never writes a colour
and never builds a modal.

### 4.6 Text with a gamepad-safe fallback

```cpp
area.Text( "profiles.name", "New profile name", &g_sNewProfileName )
    .Default( "" )
    .Help( "Name for a new saved profile." )
    .Keywords( "profile name preset save new" )
    .Suggestions( []{
        std::string sGame = AppIdDisplayName( SessionAppId() );
        return std::vector<std::string>{
            sGame + " - Quality", sGame + " - 60 fps", sGame + " - Battery" };
    } );
```

Omitting both `.Suggestions()` and `.KeyboardOnly()` **asserts at registration**:

```
ui::Registry: entry "profiles.name" is a Text entry with neither .Suggestions() nor
.KeyboardOnly(). Every text field must be reachable without a keyboard, or must
declare that it is not. See SPEC.md section 3.4.
```

### 4.7 Adding a whole new area

```cpp
// src/Overlay/UI/Areas/Lsfg.cpp  -- an entire new area. No UI code.
#include "UI/Registry.h"

void gamescope::ui::RegisterLsfgArea( Registry &reg )
{
    auto &area = reg.Area( "lsfg", "Frame Generation", Icon::Performance )
        .Keywords( "lsfg lossless fg interpolation smoothing frame gen" )
        .AvailableWhen( []{ return lsfg::IsAvailable(); } )
        .Summary( []{ return lsfg::IsEnabled()
                        ? std::format( "{}x - {} fps out", lsfg::Multiplier(), lsfg::OutFps() )
                        : std::string( "off" ); } );

    area.Group( "Generation" );
    area.Toggle( "lsfg.enabled", "Enable", &g_lsfg.bEnabled )
        .Default( false )
        .Help( "Generates intermediate frames between rendered ones. Adds latency." )
        .Keywords( "enable on off frame generation" );

    static constexpr Option kMult[] = { {"2","2x"}, {"3","3x"}, {"4","4x"} };
    area.Choice( "lsfg.multiplier", "Multiplier", &g_lsfg.nMultiplier, kMult )
        .Default( 0 )
        .EnabledWhen( []{ return g_lsfg.bEnabled; }, "frame generation is off" )
        .Help( "How many frames are produced per rendered frame." )
        .Keywords( "multiplier 2x 3x 4x how many" );

    area.Group( "Diagnostics" );
    area.Pane( "lsfg.stats", "Generation stats",
               []{ return std::format( "{} generated / s", lsfg::GeneratedPerSec() ); },
               &DrawLsfgStats )
        .Help( "Live counters from the frame-generation pass." )
        .Keywords( "stats counters diagnostics generated dropped" );
}
```

...plus **one line** in `Areas/Areas.cpp`:

```cpp
void gamescope::ui::RegisterAll( Registry &reg )
{
    RegisterDisplayArea ( reg );
    RegisterShadersArea ( reg );
    RegisterAudioArea   ( reg );
    RegisterMonitorArea ( reg );
    RegisterProfilesArea( reg );
    RegisterLogArea     ( reg );
    RegisterLsfgArea    ( reg );   // <- the whole integration
}
```

The new area now appears on Home with a live summary, is scopable with `↹`, is searchable
(`lsfg`, `frame gen`, `interpolation`, `2x`), works on a pad with the same five verbs, and looks
exactly like every other area — because nobody drew it.

---

## 5. The fenced escape hatch: `PaneCtx`

```cpp
namespace gamescope::ui
{
    // Handed to a Pane's draw function. This is the complete surface -- there is
    // no ImDrawList, no ImGui::, no ImU32, no ImVec2 anywhere in it. A pane that
    // needs something not listed here does not hack around it; it adds a method
    // here, which means the new thing acquires a style and every future caller
    // gets it for free. That is the mechanism, not a convention.
    class PaneCtx
    {
    public:
        // --- rows: the same grammar the result list uses -----------------
        bool Row     ( const char *szLabel, RowValue );      // one taxonomy row
        void RowGroup( const char *szHeader );

        // --- cards / gauges: the only other layout in the system ---------
        void BeginCards( int nColumns );                      // 1..4
        void   Gauge   ( const char *szLabel, double v, const char *szUnit,
                         GaugeStyle = GaugeStyle::Number );   // Number | Meter | Sparkline
        void   Gauge   ( const char *szLabel, std::string_view sText,
                         std::string_view sSub );             // the Media card shape
        void   Sparkline( std::span<const float> vals, std::span<const bool> outliers );
        void   CardSub ( const char *szFmt, ... );
        void EndCards();

        // --- streams: monospace line lists (the LOG) ---------------------
        void BeginStream( StreamOpts );                       // filter comes from the query line
        void   Line     ( Severity, std::string_view sTimestamp, std::string_view sText );
        void EndStream();

        // --- static text, in the family's roles only ---------------------
        void Note   ( const char *szFmt, ... );               // text/meta
        void Warning( const char *szFmt, ... );               // state/warn
        void Error  ( const char *szFmt, ... );               // state/danger

        // --- query access, for panes that want to filter themselves ------
        std::string_view Query() const;
        bool             Matches( std::string_view sHaystack ) const;
    };
}
```

A `Pane` draw function for the System Monitor's live gauges:

```cpp
static void DrawMonitorGauges( ui::PaneCtx &ctx )
{
    const SystemStats &s = Metrics::Current();

    ctx.BeginCards( 3 );
      ctx.Gauge( "Frame rate", s.flFps, "fps", ui::GaugeStyle::Sparkline );
        ctx.Sparkline( s.FpsHistory(), s.FpsOutliers() );
        ctx.CardSub( "1%% %.0f  0.1%% %.0f  avg %.0f", s.fl1Low, s.fl01Low, s.flAvgFps );
      ctx.Gauge( "Frametime", s.flFrametimeMs, "ms", ui::GaugeStyle::Sparkline );
        ctx.Sparkline( s.FrametimeHistory(), s.FrametimeOutliers() );
        ctx.CardSub( "%d outlier(s) in last 240 frames", s.nOutliers );
      ctx.Gauge( "CPU load (1 m avg)", s.flCpuPct, "%", ui::GaugeStyle::Meter );
        ctx.CardSub( "RAM %.1f / %.1f GB", s.flRamUsedGb, s.flRamTotalGb );
      ctx.Gauge( "GPU busy", s.flGpuPct, "%", ui::GaugeStyle::Meter );
        ctx.CardSub( "VRAM %.1f / %.1f GB", s.flVramUsedGb, s.flVramTotalGb );
      ctx.Gauge( "GPU temperature", s.flGpuTempC, "°C", ui::GaugeStyle::Meter );
        ctx.CardSub( "GPU power %.0f W", s.flGpuWatts );
      if ( s.media.bPresent )
          ctx.Gauge( "Media", s.media.sTitle, s.media.sArtist );
      else
          ctx.Gauge( "Media", "no media playing", "" );
    ctx.EndCards();
}
```

Every card is the same size, the same padding, the same type roles, the same sparkline height —
because the caller supplied *data*, not geometry. Compare `FpsDisplay.cpp`'s 2469 lines today.

**The LOG pane is nine lines:**

```cpp
static void DrawLogStream( ui::PaneCtx &ctx )
{
    ctx.BeginStream( { .bFollowTail = g_bLogFollow } );
    for ( const LogCapture::Line &l : LogCapture::Snapshot( g_eLogSource ) )
        if ( ctx.Matches( l.sText ) )                 // query line == the filter
            ctx.Line( l.severity, l.sTimestamp, l.sText );
    ctx.EndStream();
}
```

---

## 6. Rendering — how a registry survives immediate mode

The registry holds **declarations** (binds, predicates, metadata) built once at startup. It never
holds widget state. Drawing stays fully immediate-mode:

```cpp
void ui::Console::Draw()          // once per frame, from SettingsOverlay.cpp
{
    if ( !m_bOpen ) return;

    // 1. Query line: hand-rolled (see FEASIBILITY.md section 2), reading
    //    io.InputQueueCharacters -- which wlserver already fills with
    //    layout-correct UTF-8 via xkb_state_key_get_utf8().
    DrawQueryLine();

    // 2. Result set: recomputed only when the query, the chip stack, or the
    //    registry generation counter changes. NOT every frame.
    if ( m_sQuery != m_sLastQuery || m_uChipGen != m_uLastChipGen )
        Recompute();                                  // ~150 fuzzy matches, < 50 us

    // 3. Rows: clipped. Only visible rows are laid out or drawn.
    ImGuiListClipper clip;
    clip.Begin( (int)m_Rows.size(), RowHeight() );
    while ( clip.Step() )
        for ( int i = clip.DisplayStart; i < clip.DisplayEnd; i++ )
        {
            const Entry &e = *m_Rows[ i ].pEntry;
            ImGui::PushID( e.szId );                  // STABLE string id -- see below
            RowRender::Draw( e, m_Rows[ i ].matchPositions, i == m_nSel );
            ImGui::PopID();
        }
    clip.End();

    DrawInspector( m_nSel < (int)m_Rows.size() ? m_Rows[ m_nSel ].pEntry : nullptr );
    DrawLegend();
}
```

Two details that matter and that a naive implementation gets wrong:

- **IDs are the entry's stable string id**, not the loop index. A filtered list changes its
  contents every keystroke; index-derived IDs would make ImGui think a slider being dragged
  became a different widget mid-drag, dropping the drag. `PushID("display.sharpness")` is stable
  across every possible query, so a drag survives the list re-sorting underneath it.
- **`ImGuiListClipper` needs a uniform row height**, which the taxonomy guarantees. The one
  expanded row is handled by clipping at compact height and letting the selected row overdraw
  into its own reserved extra 4u (reserved by adding 4u to the clipper's total when a row is
  expanded). This is the one place where the taxonomy's rigidity buys a concrete implementation
  simplification.

`RowRender::Draw()` is the **only** file in the tree that contains geometry constants, and it is
one `switch` over the twelve control types. Each case builds its control-column content from
`Theme::u()` multiples and delegates the actual painting to the existing `widgets::` functions
where one already matches spec (`SliderControl`, `Toggle`, `SegmentedControl`, `PositionGrid`).

---

## 7. Making inconsistency hard — the enforcement list

These are mechanisms, not guidelines:

1. **Call sites have no drawing API.** `Registry.h` and `PaneCtx.h` are the only headers an area
   file includes. Neither exposes `ImDrawList`, `ImGui::`, `ImU32`, `ImVec2`, a size, or a colour.
2. **No pixel is nameable outside `Theme.h` and `RowRender.cpp`.** A grep for a float literal in
   `Areas/` is a review failure.
3. **The builder asserts at registration**, at startup, in every build:
   - missing `.Help()` → *"every entry must explain itself; the Inspector has nowhere else to look"*
   - missing `.Default()` on a value entry → *"X / ^R must have something to restore"*
   - missing `.Range()` on a numeric entry
   - `Text()` without `.Suggestions()` or `.KeyboardOnly()`
   - duplicate id
   - id not of the form `area.thing` matching its area
4. **`Registry::SelfTest()`** (debug builds) additionally reports:
   - entries whose 3-character prefix collides with > 3 others and have no distinguishing keyword
     → search quality is a build-time invariant, which is what makes the gamepad letter wheel
     honest (SPEC §3.3)
   - entries with no keywords at all
   - the median and p90 characters-to-unique across the whole registry, printed once at startup
5. **Adding a control type is deliberately awkward** — a new `enum` case, a new `Area::` factory,
   a new `RowRender` case, and a new gamepad `←→` semantic. It is a five-minute change, but it is
   a *visible* one that lands in the taxonomy table, not something that slips in inside a panel.

### What "one obvious call" means, measured

| Task | Today | Here |
|---|---|---|
| add a toggle | ~6 lines in a panel + decide grouping/spacing/label style; no search, no gamepad, no reset | 4 lines, everything included |
| add a slider with a dependency | ~10 lines + a `BeginDisabled` pair + a hint line, styled per-panel | 6 lines |
| add a new area | new `Panel*.cpp/h` (~400 lines), a `PanelId` enum entry, an icon, a dock slot, a `_Draw()` call, a default tile position, a window size | one file of declarations + one line in `RegisterAll()` |
| make everything consistent | manual review of 6 files | not possible to break |

---

## 8. Config, notifications and ConVars fall out for free

Because every entry carries an id, a bind and a default, three existing subsystems collapse into
registry consumers rather than parallel implementations:

```cpp
// Config write routing -- ConfigManager already knows global vs. app-routed;
// the Entry just declares which, and the Inspector shows it.
entry.Writes( ConfigTarget::Global );     // General/Notifications settings
// (default is ConfigTarget::Routed -- app <id>.json when an override is active)

// Reset-to-default for a whole area, for free:
reg.Area( "display" ).ResetAll();          // what ">Reset this area" runs

// gamescopectl / the script console get an addressable surface for free:
//   gamescopectl ui set display.sharpness 7
//   gamescopectl ui get monitor.anchor
//   gamescopectl ui search "frame"
// -- because "addressable by a stable string id" is the same property search needs.

// A notification when something changes from outside the overlay, for free:
reg.OnChanged( []( const Entry &e ) {
    Notifications::Post( std::format( "{} -> {}", e.szTitle, e.FormatValue() ) );
} );
```

That last one is the clearest illustration of the direction's thesis: the registry that exists so
you can *find* a setting is the same registry that lets the overlay *talk about* a setting, and
the same registry that guarantees it *looks like* every other setting.
