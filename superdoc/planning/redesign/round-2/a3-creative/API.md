# BLOOM Kit — `gamescope::ui`

**Adding a setting is one statement. That statement contains no geometry, and it is the
only place the setting is ever mentioned.**

The registry is B's, taken as instructed. What changes is what the registry *drives*: not a
search-first UI, but the reel, the bloom, the palette, reset-to-default, config routing,
`gamescopectl`, and the previews.

Files:

```
src/Overlay/UI/
    Registry.h/.cpp     Entry, Section, Registry, the fluent builders. No drawing.
    Bind.h              value binding: raw pointer | ConVar | getter/setter
    Theme.h/.cpp        colour roles, u(), type roles, the contrast guard, Approach()
    Metrics.h           every constant in SPEC §2.3 / §4.7, memoised per frame
    Overlay.cpp         the shell: panel, header, rail, reel, legend, peek
    Bloom.cpp           the nine instruments. The ONLY file with control geometry.
    Preview.cpp         the eight preview strips
    Palette.cpp         Ctrl+K
    Match.h/.cpp        fuzzy scorer
    Sections/
        Display.cpp Shaders.cpp Monitor.cpp Audio.cpp Config.cpp Log.cpp
        Sections.cpp    RegisterAll() — the one place a section is added
```

---

## 1. Why the call site is a declaration and not a draw call

A's kit was imperative: `ui::Slider("Sharpness", &v, 0, 20, "%.0f", {...})` inside a
per-screen draw function. That is genuinely one call and it is a good design. BLOOM cannot
use it, for one structural reason:

**The palette and the bloom both need an entry that is not currently being drawn.**

- The palette must list, score and *adjust* `hdr.sdronhdr` while the reel is showing
  Audio. An imperative kit only knows about a setting during the frame in which its screen
  draws, so A had to accept a lazily-built index and the admission that *"a setting on a
  screen you have never opened this session isn't findable"* (A `FEASIBILITY.md` §2.5).
- A bloom needs the entry's help text, default, range and routing to render — and its
  *dependencies* (`EnabledWhen`) evaluate against entries in other sections.

So entries are declared once at startup and the frame reads them. This is not a bigger
API; it is the same information moved four lines up. Count it:

```cpp
// A, imperative — 4 lines at a call site, plus a default looked up elsewhere,
// plus a help string that only reaches a tooltip.
ui::Slider( "Sharpness", &g_nSharpness, 0, 20, "%d",
            { .pszHelp = "FSR / NIS sharpening strength.",
              .bDisabled = !IsFsrOrNis(), .pszWhyDisabled = "Filter is not FSR or NIS." } );

// BLOOM, declarative — one statement, same shape, and it is now also
// searchable, resettable, routable, previewable and addressable by gamescopectl.
sec.Slider( "display.sharpness", "Sharpness", cv_sharpness )
   .Range( 0, 20 ).Step( 1, 1 ).Default( 2 )
   .Help( "FSR / NIS sharpening strength. Higher is crisper; too high rings "
          "around high-contrast edges." )
   .Keywords( "sharpen rcas cas crisp clarity ringing" )
   .Preview( Preview::Sharpen )
   .EnabledWhen( []{ return IsFsrOrNis(); },
                 "Sharpness only applies to the FSR and NIS filters." );
```

The extra cost over A is exactly `.Preview(...)` and moving `Default` to the call site.
Everything else A also had to supply, somewhere.

---

## 2. The whole public surface

```cpp
// src/Overlay/UI/Registry.h
namespace gamescope::ui
{
    // ---------------------------------------------------------------- binding
    template <typename T> class Bind
    {
    public:
        Bind( T *p );                                  // plain global / field
        Bind( ConVar<T> &cv );                         // a ConVar
        Bind( std::function<T()> get,
              std::function<void(T)> set );            // side-effecting write
        T Get() const; void Set( T ) const;
    };

    // ---------------------------------------------------------------- preview
    // CLOSED vocabulary. A caller picks one; a caller cannot write one.
    enum class Preview : uint8_t
    {
        None, Sharpen, Saturation, LuminanceRamp, TextSample,
        FrametimeRuler, HudGhost, AudioMeters, AccentSweep
    };

    // ---------------------------------------------------------------- entry
    // Fluent. Nothing here describes appearance: there is no size, colour,
    // font, padding, alignment, height, column or position parameter, and
    // adding one would require editing this header.
    class Entry
    {
    public:
        Entry &Help    ( const char * );          // REQUIRED. The bloom's paragraph.
        Entry &Keywords( const char * );          // extra palette search terms
        Entry &Unit    ( const char * );          // "%" "fps" "ms" "nits" "px" "×"
        Entry &Range   ( double lo, double hi );  // REQUIRED for numeric kinds
        Entry &Step    ( double coarse, double fine );      // default: span/40, /400
        Entry &Default ( ... );                   // REQUIRED for value kinds. `R` restores it.
        Entry &ZeroMeans( const char * );         // "Unlimited", "Off"
        Entry &Preview ( Preview );               // default None
        Entry &EnabledWhen ( std::function<bool()>, const char *szReason ); // REQUIRED reason
        Entry &AvailableWhen( std::function<bool()> );      // absent entirely (no hardware)
        Entry &Danger  ();                        // danger roles + hold-to-confirm
        Entry &Writes  ( ConfigTarget );          // Global | Routed (default)
        Entry &Suggestions( std::function<std::vector<std::string>()> );   // Text only
        Entry &Order   ( int );
    };

    // ---------------------------------------------------------------- section
    class Section
    {
    public:
        Section &Keywords( const char * );
        Section &AvailableWhen( std::function<bool()> );
        void     Group( const char *szName );     // group cursor; becomes a heading + a TOC row

        // --- the entire control taxonomy. Nine factories. No others exist. ---
        Entry &Choice ( const char *szId, const char *szTitle, Bind<int>,
                        std::span<const Option> );   // a bool is Choice{off,on}
        Entry &Bool   ( const char *szId, const char *szTitle, Bind<bool> ); // sugar for the above
        Entry &Slider ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Slider ( const char *szId, const char *szTitle, Bind<int>   );
        Entry &Number ( const char *szId, const char *szTitle, Bind<int>   );
        Entry &Text   ( const char *szId, const char *szTitle, Bind<std::string> );
        Entry &Multi  ( const char *szId, const char *szTitle, Bind<uint32_t> mask,
                        std::span<const Option> );
        Entry &Anchor ( const char *szId, const char *szTitle,
                        Bind<int> vert, Bind<int> horiz );
        Entry &Action ( const char *szId, const char *szTitle, const char *szVerb,
                        std::function<void()> );
        Entry &Readout( const char *szId, const char *szTitle,
                        std::function<std::string()>, Status = Status::None );

        // --- the two non-row region kinds, both optional, at most one each ---
        void Canvas( Canvas );                    // AudioMeters | LogToolbar — closed enum
        void Stream( std::function<std::span<const LogLine>()> );  // Log only; see §5
    };

    class Registry
    {
    public:
        Section &Section( const char *szId, const char *szTitle, Icon );
        void     SelfTest();                      // debug builds
    };

    void RegisterAll( Registry & );               // Sections/Sections.cpp

    // ---------------------------------------------------------------- runtime
    void DrawOverlay();                           // ONE call per frame. That is all.
    void GoTo( const char *szEntryId );            // programmatic jump (a notification's "open this")
}
```

**What is deliberately absent, and cannot be added without editing this header:** any
width, height, padding, gap, colour, font, alignment, row-height, column, animation or
z-order parameter; `SameLine`/`Dummy`/`Spacing`/`Indent`; `PushFont`/`PushStyleColor`;
`BeginChild`; `BeginTabBar` (there are no tabs); any tooltip call (there are no tooltips);
any window call (there are no windows); any "sheet"/"modal"/"drill" call.

`ImDrawList`, `ImGui::`, `ImU32` and `ImVec2` appear in **zero** headers a section file
includes. A section file that includes `imgui.h` is a review failure.

---

## 3. Adding things

### 3.1 A toggle

```cpp
sec.Bool( "display.tearing", "Allow Tearing", &g_bAllowTearing )
   .Default( false )
   .Help( "Present frames the moment they are ready, ignoring vblank. "
          "Lowest latency, visible tear lines." )
   .Keywords( "immediate flip vsync latency tear" )
   .EnabledWhen( []{ return !g_bVRR; },
                 "Tearing cannot be enabled while VRR / Adaptive Sync is on — "
                 "the display is already refreshing on demand." );
```

It is now: a rest row reading `Allow Tearing … off`; a bloom with a two-cell segmented
bank, the help paragraph, `default off`, `writes app 1245620` and the id; adjustable with
`←→`, click or the palette; resettable with `R`; findable by five spellings; dimmed with a
readable explanation whenever VRR is on. No UI code was written.

### 3.2 A slider with a preview

```cpp
sec.Slider( "shaders.presharpen.strength", "Strength", &*s.strength )
   .Range( 0.0, 2.0 ).Step( 0.01, 0.001 ).Default( 0.50 )
   .Help( "Unsharp-mask amount. Above ~1.2 halos become visible on thin geometry." )
   .Keywords( "strength amount sharpen halo unsharp" )
   .Preview( Preview::Sharpen )
   .EnabledWhen( []{ return s.enabled; }, "Turn Pre-Sharpen on first." );
```

`.Preview(Preview::Sharpen)` is the whole cost of "this setting shows its own effect". The
preview implementation lives once in `Preview.cpp` and is reused by
`display.sharpness`. **The enum is the anti-sprawl device**: a caller cannot hand the kit
a lambda that draws something bespoke, so there is no path by which two settings acquire
two different-looking previews of the same idea.

### 3.3 A side-effecting number with a zero case

```cpp
sec.Number( "limiter.fps", "FPS Limit",
            Bind<int>( []{ return g_nSteamCompMgrTargetFPS; },
                       []( int v ){ steamcompmgr_set_fps_limit( v ); } ) )
   .Range( 0, 360 ).Step( 5, 1 ).Unit( "fps" )
   .Default( 0 ).ZeroMeans( "Unlimited" )
   .Preview( Preview::FrametimeRuler )
   .Help( "Caps the game frame rate. 0 disables the limiter entirely." )
   .Keywords( "frame rate cap limiter throttle unlimited" );
```

`ZeroMeans` renders the word neutrally (not in `accent/value`, because "Unlimited" is not
a number), reads correctly in the palette, and makes the `FrametimeRuler` preview draw no
target line.

### 3.4 A destructive action

```cpp
sec.Action( "config.delete", "Delete Saved Config", "delete",
            []{ ConfigManager::DeleteAppConfig( SessionAppId() ); } )
   .Danger()
   .Help( "Deletes this game's saved config permanently. This cannot be undone." )
   .Keywords( "delete remove destroy wipe saved config" )
   .AvailableWhen( []{ return ConfigManager::HasAppConfig( SessionAppId() ); } );
```

`.Danger()` supplies `state/danger` roles **and** hold-to-confirm: 900 ms, the button fill
sweeps left to right, releasing early cancels. The call site never builds a modal — there
is no modal to build, because there are no modals.

### 3.5 An entire new section

```cpp
// src/Overlay/UI/Sections/Lsfg.cpp — a whole feature area. Zero UI code.
void gamescope::ui::RegisterLsfgSection( Registry &reg )
{
    auto &sec = reg.Section( "lsfg", "Frame Gen", Icon::Performance )
        .Keywords( "lsfg lossless fg interpolation smoothing frame generation" )
        .AvailableWhen( []{ return lsfg::IsAvailable(); } );

    sec.Group( "Generation" );
    sec.Bool( "lsfg.enabled", "Frame Generation", &g_lsfg.bEnabled )
       .Default( false )
       .Help( "Generates intermediate frames between rendered ones. Adds latency." )
       .Keywords( "enable on off frame generation" );

    static constexpr Option kMult[] = { {"2x"}, {"3x"}, {"4x"} };
    sec.Choice( "lsfg.multiplier", "Multiplier", &g_lsfg.nMultiplier, kMult )
       .Default( 0 )
       .Help( "How many frames are produced per rendered frame." )
       .Keywords( "multiplier 2x 3x 4x how many" )
       .EnabledWhen( []{ return g_lsfg.bEnabled; }, "Frame Generation is off." );

    sec.Group( "Diagnostics" );
    sec.Readout( "lsfg.generated", "Generated frames",
                 []{ return std::format( "{} / s", lsfg::GeneratedPerSec() ); }, Status::Ok )
       .Help( "Live counters from the frame-generation pass." )
       .Keywords( "stats counters generated dropped" );
}
```

plus **one line** in `Sections.cpp`. The section now has a rail icon, a rail TOC with two
group rows, a reel, blooms, palette coverage, reset, routing, and `gamescopectl ui set
lsfg.multiplier 1` — because nobody drew it.

Compare today: a new panel needs a `PanelId` enumerator, a `Chrome.h` icon, a dock button,
a `BeginPanelWindow`/`EndPanelWindow` pair, a default size, a tiled position, a tab bar,
and its own idea of what a section header looks like.

---

## 4. How the kit renders it

### 4.1 One frame

```cpp
void ui::DrawOverlay()
{
    if ( !g_bOverlayOpen ) return;
    const Metrics &m = Metrics::Frame();          // memoised off display_scale + surface
    Anim().Tick( ImGui::GetIO().DeltaTime );      // five floats
    Theme::SetPanelAlpha( GuardAlpha( cv_background_darkening ), cv_ui_transparency );

    DrawHalo( m );                                // one blurred rect behind the panel
    ImGui::SetNextWindowPos ( m.PanelPos() );
    ImGui::SetNextWindowSize( m.PanelSize() );
    ImGui::Begin( "##bloom", nullptr, ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus );

        DrawHeader( m );
        DrawRail  ( m, Anim().flRailLabelW );
        BeginReel ( m );
            if ( CurrentSection().HasCanvas() ) DrawCanvas();
            DrawRows();          // clipped; rest rows only; uniform height
            DrawBloom();         // the focused entry, into a foreground channel
        EndReel();
        DrawLegend( m );
    ImGui::End();

    DrawPalette();               // stock BeginPopupModal, painted by us
}
```

One window. One child (the reel's scroll region). One modal, only when the palette is open.

### 4.2 The rest row — clipped, uniform, cheap

```cpp
void DrawRows()
{
    const auto &rows = CurrentSection().VisibleRows();     // groups + entries, flat
    ImGuiListClipper clip;
    clip.Begin( (int)rows.size(), Metrics::Frame().flRowH );
    while ( clip.Step() )
        for ( int i = clip.DisplayStart; i < clip.DisplayEnd; i++ )
        {
            if ( rows[i].bGroupHeading ) { PaintGroup( rows[i] ); continue; }
            const Entry &e = *rows[i].pEntry;
            ImGui::PushID( e.szId );                       // STABLE string id
            RestRow( e );                                  // ItemAdd + ButtonBehavior + paint
            ImGui::PopID();
        }
    clip.End();
}
```

Row height is uniform **forever**, because the bloom overlays instead of expanding
(SPEC §3.1). That is not a styling preference; it is what keeps `ImGuiListClipper` valid
and a 200-entry section free.

`RestRow` is ~25 lines: `ItemSize`/`ItemAdd` over the full row rect, `ButtonBehavior` so
the row is one hit target, `RenderNavCursor` suppressed, then `AddText` twice and
`AddLine` once. There is no second path — every kind's rest row is this function; only the
value string differs, and that comes from `Entry::FormatValue()`.

### 4.3 The bloom — draw order vs hit order

```cpp
void DrawBloom()
{
    const Entry *e = Focused(); if ( !e ) return;
    const float h = BloomHeight( *e );                 // one of four constants
    ImRect bb = ClampToReel( CentredOn( FocusedRowRect(), h ) );

    // Draw ABOVE the rows: a channel split, not a second window.
    ImDrawListSplitter &sp = Reel().Splitter();
    sp.SetCurrentChannel( ImGui::GetWindowDrawList(), kChannelBloom );
    PaintBloomChrome( bb );                            // shadow, fill, border, accent edge
    PaintHeader( bb, *e );                             // title + help + big value
    PaintMetaBand( bb, *e );                           // default / range / routing / id

    // Hit-test BELOW the rows everywhere except the instrument's own rect:
    // header and meta band never call ItemAdd, so the rows underneath keep
    // receiving hover. This is what removes accordion oscillation.
    ImGui::SetCursorScreenPos( InstrumentOrigin( bb, *e ) );
    ImGui::SetNextItemAllowOverlap();
    Instrument( *e, InstrumentRect( bb, *e ) );        // the switch over nine kinds
    sp.SetCurrentChannel( ImGui::GetWindowDrawList(), kChannelRows );
}
```

`Bloom.cpp` is the **only** file in the tree that contains a geometry constant other than
`1.0f`. It is one `switch` over nine kinds, each ~30–50 lines, each built from
`Metrics::Frame()` multiples, each delegating its painting to `Theme::Paint*`.

The existing `Widgets.cpp` discipline is preserved, not discarded: `SliderBehavior()` on
the track rect, `ButtonBehavior()` on segmented cells and chips, `ItemAdd` for everything,
`RenderNavCursor` replaced by the accent edge. The `GrabMinSize`-vs-drawn-handle invariant
from `slider-widget-spec.md` §3 carries over verbatim.

### 4.4 `←→` adjusts the bloomed row without activating it

One helper, the same recipe A landed on, and it is *more* defensible here because there is
provably exactly one item per row and the bloom's instrument is the only item in the
window that wants horizontal keys:

```cpp
int NavStep()   // -1 / 0 / +1 for the bloomed entry this frame, with repeat
{
    const int n = ImGui::GetKeyPressedAmount( ImGuiKey_RightArrow, 0.35f, 0.06f )
                - ImGui::GetKeyPressedAmount( ImGuiKey_LeftArrow,  0.35f, 0.06f );
    if ( n ) { ImGui::NavMoveRequestCancel(); ImGui::SetNavCursorVisible( true ); }
    return n;
}
```

### 4.5 Peek is four lines in the shell

```cpp
Anim().flPeek = Approach( Anim().flPeek, AnyControlHeld() ? 1.0f : 0.0f, 9.0f, dt );
Theme::PushGlobalAlpha( Lerp( 1.0f, 0.12f, Anim().flPeek ) );   // chrome + rest rows
Theme::PushPanelAlpha ( Lerp( PanelAlpha(), 0.10f, Anim().flPeek ) );
BackendCompositeHints().flBlur = Lerp( cv_background_blur, 0.0f, Anim().flPeek );
BackendCompositeHints().flDarken = Lerp( cv_background_darkening, 0.0f, Anim().flPeek );
```

`AnyControlHeld()` is `ImGui::IsAnyItemActive() && FocusedIsAdjustable()`. The bloom draws
outside the pushed alpha, which is the whole trick.

The two `BackendCompositeHints` writes are the only lines in the kit that reach outside
the overlay. See `FEASIBILITY.md` §4.1 — this is the riskiest thing in the design and it
has a designed-in fallback.

---

## 5. The one fenced escape hatch

Exactly one section (Log) is not a list of entries. It declares a stream:

```cpp
sec.Stream( []{ return LogCapture::Snapshot( g_eLogSource ); } );
```

`LogLine` is `{ Severity, std::string_view sTimestamp, std::string_view sScope,
std::string_view sText }`. The kit owns the virtualisation (`ImGuiListClipper`), the
monospace roles, the level colouring, the match highlighting, the follow-tail behaviour
and the toolbar. The caller supplies **lines**, not geometry — same contract as everywhere
else.

There is no `PaneCtx`, no `ImDrawList` handoff, and no general-purpose custom-draw hook.
If a future feature needs one, the answer is a tenth control kind or a ninth preview,
added once in `Bloom.cpp` / `Preview.cpp`, correct for every caller forever. That is the
bet, and it is the same bet A and B both made.

---

## 6. Enforcement — how inconsistency becomes hard

1. **Call sites have no drawing API.** `Registry.h` is the only header a section file
   includes; it exposes no size, colour, font, position or draw list.
2. **No float literal in `Sections/`.** A grep hit is a review failure; `ui.audit 1`
   additionally draws the row grid and the four bloom-height bands.
3. **The builder asserts at registration, in every build:**
   - missing `.Help()` → *"every entry must explain itself; the bloom has nowhere else to look"*
   - missing `.Default()` on a value kind → *"`R` must have something to restore"*
   - missing `.Range()` on a numeric kind
   - `.EnabledWhen()` without a reason string → *"a dimmed row that cannot say why is a bug"*
   - duplicate id, or an id not of the form `section.thing` matching its section
4. **`SelfTest()`** (debug) reports entries with no keywords, entries whose 3-char prefix
   collides with more than three others (palette quality as a build-time invariant), and
   the median characters-to-unique across the registry.
5. **`ui.contrast 1`** recomputes every painted role against the *current* composited
   panel colour — game average × darkening × guard alpha — and flags anything under its
   role floor. This is the mockup's `contrast` panel, shipped. It turns
   `Palette.h`'s issue #62 from a per-call-site bug class into a build-time and runtime
   invariant.
6. **Adding a control kind is deliberately awkward**: a new enum case, a new `Section::`
   factory, a new `Bloom.cpp` case, a new rest-value formatter, a new `←→` semantic, and a
   new row in SPEC §2.4's table. Five minutes, but *visible* — it cannot slip in inside a
   section file.

### Measured

| Task | Today | BLOOM |
|---|---|---|
| add a toggle | ~6 lines + decide grouping, spacing, label style; no search, no reset, no help | 1 statement, everything included |
| add a slider with a dependency | ~10 lines + `BeginDisabled` pair + a hand-styled hint line | 1 statement |
| add a preview of its effect | not possible | `.Preview( Preview::Sharpen )` |
| add a section | new `Panel*.cpp/h` (~400 lines), `PanelId` enum, icon, dock slot, `_Draw()` call, default tile position, window size | one declaration file + one line |
| make it consistent | manual review of 6 files | not expressible otherwise |

---

## 7. What falls out for free

```cpp
// Config routing — the entry declares it, the bloom's meta band shows it.
entry.Writes( ConfigTarget::Global );

// Reset a whole section:
reg.Section( "display" ).ResetAll();

// gamescopectl gets an addressable surface, because "stable string id" is the
// same property the palette needs:
//   gamescopectl ui set display.sharpness 7
//   gamescopectl ui get monitor.anchor
//   gamescopectl ui find "frame"

// A notification when something changes from outside the overlay:
reg.OnChanged( []( const Entry &e ){
    Notifications::Post( std::format( "{} → {}", e.szTitle, e.FormatValue() ) );
} );

// And the notification's "open this" is one call, because the id is a route:
ui::GoTo( "display.sharpness" );   // switches section, scrolls, blooms that row
```

---

## 8. Migration

Ships behind `overlay.bloom 1` (ConVar, default off) drawn from the same place
`DrawDock()` is today. Old windows and the new overlay coexist per-section during the
port; the flag flips when the last section lands.

Order, smallest risk first:

1. **Registry + Theme + Metrics + rest rows + the reel** — the skeleton, no instruments.
2. **Shaders** (11 entries: bool, slider) — proves the row grammar and two bloom kinds.
3. **Display** (15 entries) — proves Choice long/short, Number, Readout, `EnabledWhen`.
4. **Palette** — pure gain once the registry exists, ~250 lines.
5. **Config** (11) — Text, Hue, Action, Danger, hold-to-confirm.
6. **Audio** (6) + the first sticky canvas.
7. **Log** — the stream escape hatch.
8. **Monitor** (15) + `HudGhost`, last, because the ghost is the one hard preview.
9. **Peek**, last of all, behind its own ConVar, because it is the one thing that reaches
   into the compositor.

Then delete `Chrome.cpp`'s window management. Line accounting is in `FEASIBILITY.md` §6.
