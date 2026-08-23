# Console Kit — the helper layer (A1)

`namespace gamescope::ui`. New files:

```
src/Overlay/Console/
    Console.h / .cpp     // the shell: header, rail (two zones), stage, detail bar, nav, anim
    Registry.h / .cpp    // Entry / Screen / Section declarations + the fluent builder
    Bind.h               // value binding: pointer, ConVar, getter/setter
    Rows.cpp             // the taxonomy — the ONLY file with control geometry
    Sheet.cpp            // the one overlay mechanism
    Palette.cpp          // Ctrl+K, over the registry
    Match.h / .cpp       // the fuzzy scorer (lifted from Direction B)
    Metrics.h            // every constant in SPEC §2.3, and nowhere else
    Theme.h / .cpp       // colour roles + Paint* + the contrast checker
    Screens/             // one file per section: declarations, plus a Well hook if it has one
```

**The design goal, as a test:** adding a setting is one call; that call says *what the
setting is* and never *how it looks*; and there is no parameter it could get wrong,
because there is no parameter that describes appearance.

**What changed from A's `API.md`:** rows are **declared**, not called per frame. A's
imperative `ui::Switch( "Label", &b )` could not feed the detail bar, the palette, the
rail's per-screen counts, reset-to-default or the config router, because all five need the
declaration to exist *before* the screen is drawn — and in immediate mode a screen you have
not visited has not been drawn. Direction B's registry solves exactly that, so A1 takes it.
Drawing stays fully immediate: the kit walks the registry once per frame and paints.

---

## 1. Value binding (from Direction B, verbatim)

```cpp
namespace gamescope::ui
{
    template <typename T>
    class Bind
    {
    public:
        Bind( T *p );                                     // plain global / struct field
        Bind( gamescope::ConVar<T> &cv );                 // a ConVar
        Bind( std::function<T()> get,
              std::function<void(T)> set );               // side-effecting write

        T    Get() const;
        void Set( T v ) const;                            // fires the registry change hook
    };
}
```

`Bind` is the only way a value reaches the UI, so the row for `present.tearing` is
byte-identical whether it is a `bool`, a `ConVar<bool>`, or a setter that also pokes
`steamcompmgr`.

---

## 2. The registration API

```cpp
namespace gamescope::ui
{
    enum class Class  { Fill, Half, Mark };      // decided by the KIND, never by a caller
    enum class Stage  { List, Split, Wide };
    enum class Status { None, Ok, Warn, Idle };
    enum class Target { Routed, Global, ReadOnly };

    // ---- one setting ----------------------------------------------------
    // Returned by every Screen::Xxx() factory. Fluent; every method returns *this.
    // There is no size, colour, font, padding, alignment or animation method here,
    // and adding one would mean editing this header. That is the fence.
    class Entry
    {
    public:
        Entry &Help    ( const char *sz );         // the DETAIL BAR line. REQUIRED.
        Entry &Keywords( const char *sz );         // extra search terms for the palette
        Entry &Unit    ( const char *sz );         // "%", "nits", "ms", "px", "fps", "x"
        Entry &Range   ( double lo, double hi );   // REQUIRED for numeric kinds
        Entry &Step    ( double coarse, double fine ); // default: span/40, span/200
        Entry &Default ( ... );                    // REQUIRED for value kinds. Y restores it.
        Entry &ZeroMeans( const char *sz );        // "Unlimited" for FPS Limit, "Off", ...
        Entry &EnabledWhen ( std::function<bool()>, const char *szWhyNot );
        Entry &AvailableWhen( std::function<bool()> );  // absent entirely (no hardware)
        Entry &Danger  ();                         // state/danger + second-press confirm
        Entry &Status  ( ui::Status );             // the readout dot
        Entry &Writes  ( Target );                 // default Routed; shown in the detail bar
        Entry &Order   ( int );
    };

    // ---- one screen -----------------------------------------------------
    class Screen
    {
    public:
        Screen &Group( const char *szHeading );    // sets the group cursor

        // The complete taxonomy. There are no other factories, and each one's
        // fill class is fixed in Rows.cpp's CLASS table -- see SPEC.md 2.2.
        Entry &Toggle     ( const char *szId, const char *szTitle, Bind<bool> );
        Entry &Slider     ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Slider     ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Stepper    ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Choice     ( const char *szId, const char *szTitle, Bind<int>,
                            std::span<const Option> );   // -> Segmented or Picker, resolved
                                                         //    at registration (SPEC 2.2)
        Entry &MultiSelect( const char *szId, const char *szTitle, Bind<uint32_t>,
                            std::span<const Option> );
        Entry &Chips      ( const char *szId, const char *szTitle, Bind<uint32_t>,
                            std::span<const Option> );
        Entry &Text       ( const char *szId, const char *szTitle, Bind<std::string> );
        Entry &Colour     ( const char *szId, const char *szTitle, Bind<float> hueDeg );
        Entry &Action     ( const char *szId, const char *szTitle, const char *szVerb,
                            std::function<void()> );
        Entry &Drill      ( const char *szId, const char *szTitle,
                            std::function<std::string()> szSummary, const char *szScreenId );
        Entry &Readout    ( const char *szId, const char *szTitle,
                            std::function<std::string()> );

        // The ONE imperative hook: a Well, or a Wide body. Fenced by WellCtx.
        Screen &Well     ( void (*pfn)( WellCtx & ) );
        Screen &WideBody ( void (*pfn)( WideCtx & ) );
        Screen &OnEnter  ( void (*pfn)() );        // side effects live HERE, not in a draw
    };

    // ---- the registry ---------------------------------------------------
    class Registry
    {
    public:
        Section &Section( const char *szId, const char *szTitle, Icon );
        Screen  &Screen ( Section &, const char *szId, const char *szTitle,
                          Stage = Stage::List );
        void     SelfTest();                       // debug builds -- section 6
    };

    void RegisterAll( Registry & );                // Screens/Screens.cpp -- one line per file
    void DrawConsole();                            // the whole overlay, once per frame
    void GoTo( const char *szEntryId );            // programmatic navigation (a toast's "open this")
}
```

**What is deliberately absent** — absent *is* the feature:

| Not exposed | Why |
|---|---|
| any `float flWidth` / `ImVec2 size` | the kit owns the control column and its three classes |
| a `Class` argument on any factory | the class comes from the kind (SPEC §2.2); a caller cannot widen one control |
| `SameLine` / `Dummy` / `Spacing` / `Indent` / `Separator` | `Group()` is the only vertical separation |
| `PushFont` / `PushStyleColor` / `PushStyleVar` | roles are picked per kind in `Theme::Paint*` |
| `BeginChild` / a group-block call | the console has no nested containers |
| `BeginTabBar` | **there are no tabs.** Screens are rail entries (SPEC §1) |
| **a tooltip call** | explanation goes to the detail bar; `Help()` is required (SPEC §5) |
| a "panel window" call | there are no windows |

---

## 3. Adding a setting

The literal answer to "what does adding a toggle plus a slider look like".

**Today**, in `PanelShaders.cpp`:

```cpp
if ( widgets::Toggle( "Protect skin tones", &v.protect_skin_tones ) )
{
    SetRuntimeUniformBool( "vibrancy_protect_skin_tones", v.protect_skin_tones );
    QueueSave();
}
ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
ImGui::TextDisabled( "Pre-upscale -- works with any Filter, unlike Sharpness (Display panel)." );
ImGui::PopFont();
if ( !s.enabled ) ImGui::BeginDisabled();
if ( widgets::SliderFloat( "Strength##presharpen", &( *s.strength ), 0.0f, 2.0f, "%.2f" ) )
{
    SetRuntimeUniformFloat( "pre_sharpen_strength", *s.strength );
    QueueSave();
}
if ( !s.enabled ) ImGui::EndDisabled();
```

**With the kit**, in `Screens/Shaders.cpp`:

```cpp
scr.Toggle( "vibrancy.skin", "Protect skin tones",
            Bind<bool>( &v.protect_skin_tones ) )
   .Default( true )
   .Help( "Masks hues near skin so faces do not go orange as strength rises." )
   .Keywords( "skin tone protect mask faces" );

scr.Slider( "presharpen.strength", "Strength",
            Bind<float>( []{ return *s.strength; },
                         []( float f ){ *s.strength = f;
                                        SetRuntimeUniformFloat( "pre_sharpen_strength", f ); } ) )
   .Range( 0.0, 2.0 ).Default( 0.5 )
   .Help( "Runs before the scaler, so it works with any Filter — unlike Sharpness." )
   .Keywords( "presharpen strength amount before scaler" )
   .EnabledWhen( []{ return s.enabled; }, "Pre-Sharpen is off." );
```

What went away per call site: `QueueSave()` (the registry's change hook does it), the
`##id` suffix (ids are explicit and stable), the `PushFont`/`TextDisabled`/`PopFont`
sandwich, the `BeginDisabled`/`EndDisabled` bracket, and the free-floating prose line that
was neither a row nor a label and rendered differently in every panel that had one.

What arrived for free: the row is 44px like every other row; its label sits at the same x;
its slider is 292px of track like every other slider; its value is Mono/accent; `Y` resets
it to 0.5; `Ctrl+K` finds it by five spellings; the detail bar explains it; the disabled
state explains *itself*; the rail's screen count goes up by one; `gamescopectl ui set
presharpen.strength 0.8` works; and the config router knows it belongs in the app file.

**A brand-new setting is one declaration. There is no second place to register it, no
layout to update, no width to pick, and no help text to route anywhere.**

---

## 4. Adding a whole screen

```cpp
// src/Overlay/Console/Screens/Audio.cpp
#include "Console/Registry.h"
using namespace gamescope::ui;

static void DrawAudioWell( WellCtx &w )      // the one fenced imperative hook
{
    w.Meter( "L", g_flPeakL );
    w.Meter( "R", g_flPeakR );
    w.Caption( "%.1f dB · %d kHz · %d ch", g_flPeakDb, g_nRate / 1000, g_nChannels );
}

void RegisterAudioSection( Registry &reg )
{
    auto &sec = reg.Section( "audio", "Audio", Icon::Audio );

    // ---- screen 1: it becomes a rail entry in zone 2. No tab bar exists. ----
    auto &out = reg.Screen( sec, "audio.output", "Output" )
                   .Well( &DrawAudioWell );

    out.Group( "Output" );
    out.Readout( "audio.device", "Device", []{ return g_sSinkName; } )
       .Status( Status::Ok ).Writes( Target::ReadOnly )
       .Help( "The PipeWire sink gamescope is routing the focused app into." )
       .Keywords( "device sink output pipewire" );

    out.Slider( "audio.volume", "Volume",
                Bind<int>( []{ return g_nVolumePct; }, &ApplyVolume ) )
       .Range( 0, 150 ).Unit( "%" ).Default( 100 )
       .Help( "Per-app volume on the matched sink-input. Above 100% is digital gain and can clip." )
       .Keywords( "volume loudness gain level" );

    out.Toggle( "audio.mute", "Mute", Bind<bool>( []{ return g_bMuted; }, &ApplyMute ) )
       .Default( false )
       .Help( "Mutes the focused app only, not the system." );

    out.Group( "Routing" );
    out.Drill( "audio.stream", "Pick a stream manually",
               []{ return g_sStreamSummary; }, "audio.streams" )
       .Help( "Overrides PID matching when a game spawns more than one audio client." );

    out.Action( "audio.clear", "Clear manual override", "Clear", &ClearOverride )
       .Help( "Returns to automatic PID-based stream matching." );

    // ---- screen 2 ------------------------------------------------------
    auto &str = reg.Screen( sec, "audio.streams", "Streams" );
    /* ... */
}
```

...plus **one line** in `Screens/Screens.cpp`:

```cpp
void gamescope::ui::RegisterAll( Registry &reg )
{
    RegisterGamescopeSection( reg );
    RegisterShadersSection  ( reg );
    RegisterMonitorSection  ( reg );
    RegisterAudioSection    ( reg );      // <- the whole integration
    RegisterConfigSection   ( reg );
    RegisterLogSection      ( reg );
}
```

The section now has a rail icon, its screens are rail entries with live counts, every row
is searchable, the detail bar explains each one, `Y` resets them, the routing badge is
correct, and the layout is identical to every other screen — because nobody drew it.

Compare today: a new panel needs a `PanelId` enumerator, a `Chrome.h` icon, a dock button,
a `BeginPanelWindow`/`EndPanelWindow` pair, a default size, a tiled position, a tab bar,
and its own idea of what a section header looks like.

A **Wide** screen is the same shape with one argument changed:

```cpp
reg.Screen( sec, "log.gamescope", "Gamescope", Stage::Wide )
   .WideBody( []( WideCtx &c ){
        c.SourceSwitch( kLogSources );      // <- the Wide screen's screen-switcher slot
        c.Find(); c.Levels(); c.Follow(); c.Copy();
        c.LogView( LogCapture::Gamescope().Lines() );   // virtualised
   } );
```

`c.SourceSwitch()` is the only place a screen switcher lives outside the rail, and it
exists because a Wide screen contracts the rail to icons (SPEC §1.3). The kit puts it in
the toolbar's first slot; the caller cannot place it anywhere else.

---

## 5. How the kit is built (the parts that matter)

### 5.1 Metrics — computed once per frame

```cpp
struct Metrics
{
    float u, base;
    float flRowH, flRowPadY, flStagePadX, flLabelGap;
    float flRailW, flRailCollapsedW, flIcon;       // flRailCollapsedW = 2*flStagePadX + flIcon
    float flCtlW, flCtlHalfW, flValueSlotW;        // flCtlHalfW == flCtlW * 0.5f, always
    float flConsoleW, flConsoleH;

    static const Metrics &Frame();                 // memoised off display_scale + surface size
};
```

Every number in SPEC §2.3 lives here and nowhere else. Two of them are *derived*, not
typed, and that is the whole of fixes 2 and 4:

```cpp
m.flRailCollapsedW = m.flStagePadX * 2.0f + m.flIcon;   // fix 4: the icon cannot move
m.flCtlHalfW       = m.flCtlW * 0.5f;                   // fix 2: half is half, at every step
```

No `.cpp` in the kit contains a bare pixel literal that is not `1.0f` (hairlines).
Enforced by review and by `ui.audit 1`.

### 5.2 One row frame, every kind through it

```cpp
// Rows.cpp -- private
static constexpr Class kClassOf[] = {           // SPEC 2.2, the whole of "same kind, same size"
    /* Toggle      */ Class::Mark,
    /* Slider      */ Class::Fill,
    /* Stepper     */ Class::Half,
    /* Segmented   */ Class::Fill,
    /* Picker      */ Class::Half,
    /* MultiSelect */ Class::Half,
    /* Chips       */ Class::Fill,
    /* Text        */ Class::Fill,
    /* Colour      */ Class::Half,
    /* Action      */ Class::Half,
    /* Drill       */ Class::Mark,
    /* Readout     */ Class::Mark,
};

struct RowFrame { ImRect bb, bbLabel, bbCtl; bool bHovered, bHeld, bFocused, bDisabled; };

static RowFrame BeginRow( const Entry &e );
static void     EndRow  ( const RowFrame &, const Entry & );
```

`BeginRow` does: `PushID(e.szId)` → `ItemSize`/`ItemAdd` over the whole row →
`ButtonBehavior` on the whole row (one hit target) → suppress `RenderNavCursor` and paint
the accent tick instead → paint fill / divider / label / conditional sub-line → compute
`bbCtl` **from `kClassOf[e.kind]`** and hand it back:

```cpp
ImRect CtlRect( const ImRect &row, Class c )
{
    const Metrics &m = Metrics::Frame();
    const float right = row.Max.x;
    switch ( c )
    {
        case Class::Fill: return ImRect( right - m.flCtlW,     row.Min.y, right, row.Max.y );
        case Class::Half: return ImRect( right - m.flCtlHalfW, row.Min.y, right, row.Max.y );
        case Class::Mark: return ImRect( right - m.flCtlW,     row.Min.y, right, row.Max.y );
    }                                    // Mark paints right-aligned inside the full column
}
```

Every kind is `BeginRow` → draw into `bbCtl` → `EndRow`. There is no second path, and no
kind can ask for a different rect, because it does not choose its class.

The slider's shipped invariant carries over verbatim (`slider-widget-spec.md` §3):
`kHandleW` is computed once, pushed as `ImGuiStyleVar_GrabMinSize` **and** reused as the
painted half-width, so `SliderBehavior()`'s hit-test and the paint cannot read two
different widths. Issue #23 cannot recur.

### 5.3 The shell

```cpp
void DrawConsole()
{
    const Metrics &m = Metrics::Frame();
    ImGui::SetNextWindowPos ( CenteredPos( m ) );
    ImGui::SetNextWindowSize( ImVec2( m.flConsoleW, m.flConsoleH ) );
    ImGui::Begin( "##console", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus );

    Anim().Tick( ImGui::GetIO().DeltaTime );

    DrawHeader();                       // breadcrumb, pips, routing badge, live cluster
    DrawRailZone1( Anim().flRailW );    // sections    -- fixed height, fixed icon x
    DrawRailZone2( Anim().flRailW );    // screens     -- replaces the tab bar
    BeginStage();
      const Screen &s = CurrentScreen();
      if ( s.pfnWell ) DrawWell( s );
      for ( const Entry &e : s.Entries() )       // <- declarations, walked
      {
          if ( e.pfnAvailable && !e.pfnAvailable() ) continue;
          if ( e.szGroup != pszGroupCursor ) { DrawGroup( e.szGroup ); pszGroupCursor = e.szGroup; }
          Rows::Draw( e );
      }
    EndStage();
    DrawSheetIfOpen();
    DrawPaletteIfOpen();                // Ctrl+K, over the same registry
    DrawDetailBar( FocusedEntry() );    // <- fix 5: where help lives
    ImGui::End();
}
```

One window, one draw list (plus the two modals). Stock nav inside it.

### 5.4 `←→` adjusts the focused row without entering it

Unchanged from A, minus the gamepad keys:

```cpp
int NavStep()   // -1 / 0 / +1 for the focused row this frame, with repeat
{
    if ( !ImGui::IsItemFocused() ) return 0;
    const float fine = ImGui::IsKeyDown( ImGuiMod_Shift ) ? 0.25f : 1.0f;
    const int n = ImGui::GetKeyPressedAmount( ImGuiKey_RightArrow, 0.35f, 0.06f )
                - ImGui::GetKeyPressedAmount( ImGuiKey_LeftArrow,  0.35f, 0.06f );
    if ( n ) { ImGui::SetNavCursorVisible( true ); ImGui::NavMoveRequestCancel(); }
    return n;
}
```

It works because there is exactly one item per row, so ImGui's own horizontal nav has
nothing to move to and the keys are free.

### 5.5 The detail bar

```cpp
void DrawDetailBar( const Entry *pE )
{
    if ( !pE ) { PaintPlaceholder(); return; }
    Theme::Text( Role::Primary, pE->szTitle );
    Theme::Text( Role::Label,   " — %s", pE->szHelp );      // REQUIRED at registration
    char szFacts[ 256 ];                                     // built from the declaration
    Facts( *pE, szFacts );   // "range 0–20 · default 2 · writes 1245620.json"
    Theme::Text( Role::Meta, szFacts );
    PaintKeyHints();                                         // fixed; never changes
}
```

No storage, no state, no per-row work: it reads the focused entry's declaration. That is
the whole reason `Help()` had to become mandatory.

### 5.6 The palette

```cpp
void DrawPaletteIfOpen()
{
    if ( !g_bPaletteOpen ) return;
    // stock BeginPopupModal -> focus trapping and Esc for free (FEASIBILITY 2.2)
    DrawQueryLine();                                  // reads io.InputQueueCharacters
    if ( g_sQuery != g_sLastQuery ) Recompute();      // ~90 fuzzy matches, < 40 us
    ImGuiListClipper clip; clip.Begin( (int)g_Results.size(), Metrics::Frame().flRowH * 1.1f );
    while ( clip.Step() )
        for ( int i = clip.DisplayStart; i < clip.DisplayEnd; i++ )
        {
            const Entry &e = *g_Results[ i ];
            ImGui::PushID( e.szId );                  // STABLE id -- a drag survives a re-sort
            Rows::DrawPaletteRow( e, i == g_nSel );   // path caption + the real control, Half class
            ImGui::PopID();
        }
    if ( Activated() ) { GoTo( g_Results[ g_nSel ]->szId ); g_bPaletteOpen = false; }
}
```

`GoTo()` selects the section, selects the screen, sets focus, scrolls the row into view and
flashes it. **The palette always ends in the rail.** There is no search mode to be stuck in.

---

## 6. Making inconsistency hard — the enforcement list

Mechanisms, not guidelines:

1. **Call sites have no drawing API.** `Registry.h`, `WellCtx.h` and `WideCtx.h` are the
   only headers a `Screens/` file includes. None exposes `ImDrawList`, `ImGui::`, `ImU32`,
   `ImVec2`, a size or a colour. A `Screens/` file that includes `imgui.h` is a review fail.
2. **No pixel is nameable outside `Metrics.h`, `Rows.cpp` and `Theme.cpp`.** A float literal
   in `Screens/` fails review.
3. **Fill class is a table, not a parameter.** `kClassOf[]` is the only place it exists.
4. **The builder asserts at registration**, at startup, in every build:
   - missing `.Help()` → *"every entry must explain itself; the detail bar has nowhere else to look"*
   - missing `.Default()` on a value entry → *"Y must have something to restore"*
   - missing `.Range()` on a numeric entry
   - `.EnabledWhen()` without a reason string
   - duplicate id, or an id whose prefix does not match its section
   - a `Drill` whose target screen id does not exist
5. **`Registry::SelfTest()`** (debug builds) additionally reports:
   - the accent family swept across all 360 hues against every surface, asserting each
     token's floor from SPEC §3.2 — a future palette edit cannot introduce a failing hue;
   - entries whose 3-character prefix collides with more than three others and that have no
     distinguishing keyword (palette quality as a build-time invariant);
   - the median characters-to-unique across the registry, printed once at startup.
6. **`ui.audit 1`** (ConVar) overlays the row grid, the control column, the half-class line
   and every text baseline, and asserts when a control paints outside its class box.
7. **`ui.contrast 1`** (ConVar) composites every painted text run against its actual
   surface using the SPEC §3.1 chain and flags anything below its role's floor. This is
   what stops "too dark to read" complaint number four.

### What "one obvious call" means, measured

| Task | Today | Here |
|---|---|---|
| add a toggle | ~6 lines + decide grouping, spacing and label style; no search, no reset, no help | 4 lines, everything included |
| add a slider with a dependency | ~10 lines + a `BeginDisabled` pair + a hint line, styled per panel | 6 lines |
| add a screen | new `Panel*.cpp/h` (~400 lines), a `PanelId`, an icon, a dock slot, a size, a tile position, a tab entry | a declaration block; the rail entry is automatic |
| make it consistent | manual review of six files | not expressible otherwise |

---

## 7. What the registry gives away for free

```cpp
// config routing -- the entry declares it, the detail bar shows it
entry.Writes( Target::Global );

// reset a whole screen, because "Reset this screen" is an Action over the registry
reg.Screen( "gamescope.upscaling" ).ResetAll();

// gamescopectl gets an addressable surface, because ids are stable strings
//   gamescopectl ui get  upscale.sharpness
//   gamescopectl ui set  upscale.sharpness 8
//   gamescopectl ui find "frame"

// a toast when something changes from outside the console
reg.OnChanged( []( const Entry &e ){
    Notifications::Post( "%s → %s", e.szTitle, e.FormatValue().c_str() );
    Notifications::OnClick( [&e]{ ui::GoTo( e.szId ); } );   // lands in the rail, on the row
} );
```

That last one is the clearest statement of why B is worth adopting as a feature: the
registry that exists so you can *find* a setting is the same registry that lets the overlay
*talk about* one, and the same registry that guarantees it *looks like* every other one.

---

## 8. Migration bridge

The kit ships behind `overlay.console 1` (ConVar, default off) as a *seventh panel* —
`DrawConsole()` called where `DrawDock()` is today. Old windows and the new console coexist
for the duration of the port, one screen at a time; the flag flips when the last screen
lands. Nothing is deleted until then.

Order, smallest risk first:

1. **Registry + Metrics + Theme + the row frame** — no screens yet, one throwaway test screen.
2. **Shaders** (3 screens, sliders and toggles) — proves the row grid and the FILL class.
3. **Gamescope** (4 screens) — proves Segmented / Picker and the ≤5-option switchover.
4. **Config** (3 screens) — proves Colour, Text, Action, Destructive, and write routing.
5. **Audio** (2 screens) — the first Well.
6. **Log** — the first Wide screen, and the toolbar's `SourceSwitch`.
7. **System Monitor** last — its Well is the only genuinely hard piece (`FEASIBILITY.md` §5).
8. **Palette** — after ≥ 3 sections exist, so it has something to search.
