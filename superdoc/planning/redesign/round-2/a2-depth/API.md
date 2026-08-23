# Console Kit + Ledge — the helper layer

`namespace gamescope::ui`. Direction A's kit, plus B's registry, plus the one thing this
refinement exists for: **a setting declares its explanatory depth in the same call that
declares its control.**

Files: `src/Overlay/Console/Console.{h,cpp}` (shell, nav, animation), `Registry.{h,cpp}`
(the declarations), `Rows.cpp` (the taxonomy), `Ledge.cpp`, `Palette.cpp`, `Sheet.cpp`,
`Metrics.h`, `Theme.h`.

**The test this API is written against:** adding a setting is one call; the call has no
parameter that could make it look different from its neighbour; and there is nowhere else
to put the explanation, so the explanation gets written.

---

## 1. The declaration

A merges its per-row modifiers into an `Opt` aggregate. B uses a fluent builder. The
fluent builder wins here for one specific reason: **`.Help()` and `.Default()` can be
enforced as required at registration**, with an error message that names the setting, and
a designated-initialiser aggregate cannot enforce anything.

```cpp
// src/Overlay/Console/Registry.h
namespace gamescope::ui
{
    // A read/write handle to wherever the value actually lives. Three
    // constructions cover every call site in src/Overlay/ today.
    template <typename T> class Bind
    {
    public:
        Bind( T *p );                                    // plain global / struct field
        Bind( gamescope::ConVar<T> &cv );                // ConVar: .Get() / .Set()
        Bind( std::function<T()> get,
              std::function<void(T)> set );              // side-effecting write
    };

    enum class Depth
    {
        Normal,   // appears in the row list. The default.
        Expert,   // appears ONLY inside the raised Ledge, under its host.
    };

    // Returned by every factory. Fluent; every method returns *this.
    // There is no size, position, colour, font, alignment, padding or
    // animation parameter anywhere in this class. That absence is the feature.
    class Entry
    {
    public:
        // ---- the depth declaration. This is the whole point of the file. ----

        // ONE SENTENCE. Renders in the Ledge's resting line and as the palette
        // subtitle. Required: registration asserts without it.
        Entry &Help   ( const char *sz );

        // The paragraph. Renders only in the raised Ledge. Optional -- a setting
        // whose one sentence is genuinely the whole story does not need one, and
        // padding it out would be worse than omitting it.
        Entry &Detail ( const char *sz );

        // Other entries that answer "…and what about". Renders as jump chips in
        // the raised Ledge; clicking one navigates and focuses it.
        Entry &Related( std::initializer_list<const char *> ids );

        // Entries that belong to THIS one and are hosted in the Ledge, not the
        // row list. The named entries are registered normally (so the palette
        // still finds them) but are Depth::Expert, so no screen draws them.
        Entry &Expert ( std::initializer_list<const char *> ids );

        // ---- everything else ----
        Entry &Default( auto v );                     // REQUIRED on value entries
        Entry &Range  ( double lo, double hi );       // REQUIRED on numeric entries
        Entry &Step   ( double coarse, double fine ); // default: range/40, range/200
        Entry &Unit   ( const char *sz );             // "%", "nits", "ms", "px"
        Entry &ZeroMeans( const char *sz );           // "Unlimited" for FPS Limit
        Entry &Keywords( const char *sz );            // extra palette search terms
        Entry &EnabledWhen( std::function<bool()>, const char *szWhyNot );
        Entry &AvailableWhen( std::function<bool()> );   // absent, not greyed
        Entry &Danger ();                             // danger roles + confirm step
        Entry &Writes ( ConfigTarget );               // Global / Routed (default)
        Entry &Group  ( const char *sz );             // overrides the group cursor
    };

    class Screen
    {
    public:
        void Group( const char *szHeading );   // sets the cursor for what follows

        // The complete control taxonomy. There are no other factories.
        Entry &Switch ( const char *szId, const char *szTitle, Bind<bool> );
        Entry &Slider ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Slider ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Number ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Choice ( const char *szId, const char *szTitle, Bind<int>,
                        std::span<const char *const> options );
        Entry &Multi  ( const char *szId, const char *szTitle, Bind<uint32_t>,
                        std::span<const char *const> names );
        Entry &Text   ( const char *szId, const char *szTitle, Bind<std::string> );
        Entry &Hue    ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Action ( const char *szId, const char *szTitle, const char *szVerb,
                        std::function<void()> );
        Entry &Readout( const char *szId, const char *szTitle,
                        std::function<std::string()>, Status = Status::None );
        Entry &Drill  ( const char *szId, const char *szTitle,
                        std::function<std::string()> szSummary,
                        const char *szScreenId );
        Entry &Anchor ( const char *szId, const char *szTitle,   // 3x3, Well only
                        Bind<int> vert, Bind<int> horiz );
    };

    class Section     // a rail item
    {
    public:
        Section &Blurb( const char *sz );   // the Ledge's section card, one sentence
        Screen  &Screen( const char *szId, const char *szTitle, Stage = Stage::List );
    };

    class Registry
    {
    public:
        Section &Section( const char *szId, const char *szTitle, Icon );
        void     SelfTest();     // debug builds -- section 6
    };

    void RegisterAll( Registry & );          // Screens/Screens.cpp, one line per file
    void DrawConsole();                      // the whole overlay, once per frame
    void GoTo( const char *szEntryId );      // programmatic nav (a notification's "open this")
}
```

**What is deliberately absent, and absent is the feature:**

| Not exposed | Why |
|---|---|
| any `float flWidth` / `ImVec2 size` | the kit owns the control column; there is now exactly one width (SPEC §2.2) |
| `SameLine`, `Dummy`, `Spacing`, `Indent`, `Separator` | `Group()` is the only vertical separation |
| `PushFont` / `PushStyleColor` / `PushStyleVar` | roles are picked per control kind |
| `BeginChild`, `BeginTabBar` | no nested containers; sub-tabs come from the screen list |
| **a tooltip call** | **there is nowhere to write one. `Help()` is the only channel and it goes to the Ledge.** |
| a "panel window" call | there are no windows |
| a way to render an entry outside its screen | `Depth::Expert` is the one exception and only the *registry* can place it |

---

## 2. Declaring depth at the call site

The literal answer to *"how does a setting declare its explanatory depth in the same call
that declares the control"*.

### 2.1 A setting whose one sentence is enough

```cpp
s.Switch( "gamescope.grabcursor", "Force Grab Cursor", &g_bForceGrabCursor )
 .Default( false )
 .Help( "Confines the pointer to the game surface even when the game does not ask." )
 .Keywords( "cursor mouse pointer grab lock confine" );
```

Four lines. That sentence is now: the Ledge's resting line when this row has focus, the
palette's subtitle when it matches a search, and — because it exists — the reason nobody
was tempted to write a tooltip. `.Detail()` is omitted because there is nothing more to
say, and the raised Ledge shows the sentence, the default, the destination file and the id
without it.

### 2.2 A setting that genuinely needs depth

```cpp
s.Slider( "gamescope.sharpness", "Sharpness", cv_sharpness )
 .Range( 0, 20 ).Step( 1, 1 ).Default( 2 )
 .Help( "Strength of the FSR/NIS sharpening pass. Higher is crisper; too high rings." )
 .Detail( "Drives RCAS, the second FSR pass (and the NIS equivalent). It runs *after* "
          "the upscale, so it sharpens output pixels, not source pixels. Above about 12 "
          "you start to see bright halos on high-contrast edges — text, HUDs and foliage "
          "show it first. If you want sharpening that works with every filter, use "
          "Pre-Sharpen on the Shaders screen instead: that one runs before the scaler." )
 .Related( { "gamescope.filter", "shaders.presharpen" } )
 .Keywords( "sharpen crisp clarity rcas cas ringing halo" )
 .EnabledWhen( []{ return g_upscaleFilter == GamescopeUpscaleFilter::FSR
                       || g_upscaleFilter == GamescopeUpscaleFilter::NIS; },
               "The Filter is not FSR or NIS." );
```

`Help` is the one line the row's neighbour on screen would want. `Detail` is the paragraph
you read once. `Related` is the two settings you will reach for next. `EnabledWhen`'s
reason renders *inline on the row* when it is off (SPEC §2.1, amendment 2) **and** as a
warn line at the top of the raised Ledge — one string, two placements, no duplication.

Note the shape of the call: **the depth is not an afterthought bolted onto a `.Help()`
tooltip parameter — it is three graded fields, and which one you reach for is decided by
how much there is to say.** A one-liner is `Help` only. A concept is `Help` + `Detail`. A
setting with knobs is `Help` + `Detail` + `Expert`.

### 2.3 A setting whose depth is other controls

```cpp
s.Choice( "hdr.tonemap", "Tonemap Operator", (int *)&g_eTonemap, kTonemapOps )
 .Default( 1 )
 .Help( "How out-of-range highlights are folded back into what the display can show." )
 .Detail( "Applied when the game asks for more luminance than the connector can "
          "produce. *none* hard-clips… *aces* is the filmic curve most engines already "
          "target… *uncharted2* and *hable* are the same family with more toe/shoulder "
          "control — the three constants below are theirs, and they only do anything "
          "when one of those two is selected." )
 .Expert( { "hdr.tm.shoulder", "hdr.tm.toe", "hdr.tm.white" } )
 .Related( { "hdr.enabled", "hdr.gain" } )
 .Keywords( "tonemap curve aces reinhard hable filmic clip highlight roll off" );

// Declared exactly like any other slider. Depth::Expert is inferred from being
// named in the Expert() list above -- the entry itself does not opt in, so the
// host is the single place that decides, and two hosts cannot claim the same one
// (SelfTest asserts on it).
s.Slider( "hdr.tm.shoulder", "Shoulder strength", &g_tonemap.flShoulder )
 .Range( 0, 1 ).Step( 0.01, 0.001 ).Default( 0.22 )
 .Help( "How early the highlight roll-off begins." )
 .Detail( "Higher values start compressing sooner, protecting peak detail at the cost "
          "of mid-tone contrast." )
 .Keywords( "shoulder highlight rolloff" );
```

Three rows leave `Gamescope › HDR`; the screen goes from 19 rows to 16. They are still
registered, still findable by the palette (`Ctrl+K → "shoulder"` lands on them, labelled
*expert of Tonemap Operator*), still resettable, still routed to the same config file. They
are simply not *drawn* until you raise the Ledge on their host.

### 2.4 A whole screen

```cpp
// src/Overlay/Console/Screens/ScreenHdr.cpp
void gamescope::ui::RegisterHdrScreen( Registry &reg )
{
    auto &sec = reg.Section( "gamescope", "Gamescope", Icon::Display )
        .Blurb( "How the game image reaches the display: resampling, presentation "
                "timing and HDR." );                       // <- the Ledge's section card

    auto &s = sec.Screen( "gamescope.hdr", "HDR" );

    s.Group( "Output" );
    s.Switch( "hdr.enabled", "HDR", &g_bHdrEnabled ) …
    s.Readout( "hdr.cap", "Display capability", []{ return DescribeHdrCap(); }, Status::Ok ) …
    s.Choice ( "hdr.tonemap", "Tonemap Operator", … )      // as above

    s.Group( "Levels" );
    s.Slider( "hdr.sdrbright", "SDR-on-HDR Brightness", cv_sdr_brightness ) …

    s.Group( "Danger zone" );
    s.Action( "hdr.clear", "Clear HDR overrides for this app", "Clear",
              []{ ConfigManager::ClearHdrBlock( SessionAppId() ); } )
     .Danger()
     .Help( "Deletes the HDR block from this game's config; global defaults apply again." )
     .Detail( "Removes only the hdr section of app <id>.json. Other per-game settings are "
              "untouched. There is no undo — the file is rewritten immediately." );
}
```

...plus one line in `Screens/Screens.cpp`. The rail entry, the sub-tab, the breadcrumb, the
scrolling, the focus model, the Ledge's every state, the palette index, the routing badge,
the delta pips, the section card's `n differ` count, the spacing and the theming are all
supplied by the kit.

---

## 3. Drawing the Ledge

The registry holds declarations. Drawing stays fully immediate-mode; the Ledge adds **one
float** of animation state and **two ints** of nav state to the existing `ConsoleAnim`.

```cpp
// Console.cpp
struct LedgeState
{
    bool         bRaised    = false;   // persisted as ConVar ui.ledge
    bool         bFocusIn   = false;   // focus is on an Expert row inside the Ledge
    int          nExpertSel = 0;
    float        flHeight   = 0.0f;    // Approach()ed between kRest and kRaised
};

void DrawLedge( const Entry *pFocused, const Section *pSection, const LogLine *pLine )
{
    const Metrics &m = Metrics::Frame();
    LedgeState &L = Ledge();

    const float flTarget = L.bRaised ? Metrics::LedgeRaisedH( m ) : m.flLedgeH;
    L.flHeight = Approach( L.flHeight, flTarget, 16.0f, ImGui::GetIO().DeltaTime );

    // one band, three subjects, never empty
    if      ( pFocused ) DrawEntryLedge  ( *pFocused );   // a setting
    else if ( pLine )    DrawLogLineLedge( *pLine    );   // LOG has no rows
    else                 DrawSectionCard ( *pSection );   // focus is on the rail
}
```

`DrawEntryLedge` resting is: caret, `Help()` clipped to one line, then a right-aligned Mono
strip of `value · def <default> · <destination>` and the three contextual key glyphs.
Raised, it adds a `BeginChild` — **the second and last child region in the design** — and
inside it draws `Detail()`, the fact line, the `Related()` chips, and then loops the
`Expert()` ids through the **same `Rows.cpp` entry points the stage uses**:

```cpp
for ( const char *szId : e.Expert() )
    Rows::Draw( Registry::Get( szId ), /*bInLedge*/ true );
```

There is no second row renderer, no second row grammar, and no way for a Ledge row to look
different from a stage row — which is the property that keeps the mechanism from
reintroducing the "every module looks different" problem it exists to cure.

### 3.1 Where the focused entry comes from

The Ledge needs to know what has focus, which in immediate mode is not something you can
ask after the fact. The kit already routes every row through one `BeginRow`/`EndRow` pair
(A's §4.2), so:

```cpp
// Rows.cpp, inside EndRow()
if ( rf.bFocused ) Ledge().pPendingSubject = &e;
```

The stage is drawn before the Ledge, so by the time `DrawLedge()` runs, `pPendingSubject`
is this frame's answer. One pointer, set at most once per frame, cleared at the top of the
frame. No retained widget tree, no lookup, no ID hashing.

---

## 4. The palette

```cpp
void DrawPalette()
{
    if ( !Palette().bOpen ) return;

    DrawQueryLine();                                  // io.InputQueueCharacters

    if ( Palette().sQuery != Palette().sLast )
        Palette().Recompute();                        // ~200 fuzzy matches, < 60 us

    ImGuiListClipper clip;
    clip.Begin( (int)Palette().vecHits.size(), Metrics::Frame().flPaletteRowH );
    while ( clip.Step() )
        for ( int i = clip.DisplayStart; i < clip.DisplayEnd; i++ )
        {
            const Entry &e = *Palette().vecHits[ i ];
            ImGui::PushID( e.Id() );                  // STABLE string id, never the index
            Rows::DrawPaletteHit( e, i == Palette().nSel );
            ImGui::PopID();
        }
    clip.End();
}
```

`Rows::DrawPaletteHit` renders `title` + `Help()` + the screen path + **the live control**,
so `←→` adjusts a value without leaving the search. The Ledge is drawn *after* the palette
and takes the highlighted hit as its subject, so search results explain themselves in the
same place everything else does.

`PushID` on the stable string id, not the loop index — a filtered list changes contents on
every keystroke, and index-derived ids would make ImGui think a slider being dragged became
a different widget mid-drag (B's `API.md` §6 makes the same point; it is worth repeating
because it is the one bug that will definitely happen if it is not written down).

**Jumping to an Expert entry:**

```cpp
void GoTo( const char *szId )
{
    const Entry &e = Registry::Get( szId );
    if ( const Entry *pHost = Registry::HostOf( e ) )       // e is Depth::Expert
    {
        Nav().GoToScreen( pHost->ScreenId() );
        Nav().FocusRow  ( pHost->Id() );
        Ledge().bRaised = true;
        Ledge().bFocusIn = true;
        Ledge().nExpertSel = IndexIn( pHost->Expert(), szId );
    }
    else { Nav().GoToScreen( e.ScreenId() ); Nav().FocusRow( e.Id() ); }
}
```

---

## 5. What A's `Opt` maps to

For anyone porting the A proposal's call sites:

| A (`a-console/API.md`) | A2 |
|---|---|
| `Opt::pszHelp` → sub-line under the label | `.Help()` → the Ledge's resting line. **Not drawn on the row.** |
| `Opt::pszWhyDisabled` + `bDisabled` | `.EnabledWhen( pred, reason )` — one call, and there is no overload without a reason |
| `Opt::pszDefaultTip` | `.Default()` — the value itself, which the Ledge formats and the RESET button names |
| `Opt::pszUnit` | `.Unit()` |
| `Opt::flStep` | `.Step( coarse, fine )` |
| `Opt::status` | the `Status` parameter of `Readout()` |
| `Opt::intent = Destructive` | `.Danger()` |
| `SaveScope` | unchanged — one save per frame, armed by any entry whose `Bind::Set` fired |
| the kit's wide-vs-narrow control column measurement | **deleted.** One column width (SPEC §2.2) |

---

## 6. Enforcement — how an inconsistent or unexplained screen becomes hard to build

Mechanisms, not guidelines. Items 1–5 are A's; 6–10 are what this refinement adds.

1. **No geometry in the API.** You cannot pass a size, so you cannot pass a wrong one.
2. **`Group()` is the only vertical spacing.**
3. **`PushID(screen)` per screen** removes the `##suffix` habit.
4. **Unregistered screens do not render.** There is no way to draw a stray window.
5. **The kit owns the only `Theme::Paint*` functions.** A screen file that includes
   `imgui.h` at all is a review flag.
6. **`.Help()` is required.** Registration asserts, in every build, naming the entry:
   `ui::Registry: entry "hdr.tonemap" has no .Help(). The Ledge is the only explanation
   channel in this design and it has nowhere else to look. See SPEC.md §0.`
7. **`.Default()` is required on every value entry**, because the delta pip, the slider's
   default tick, `Ctrl+D`, and the section card's `n differ` all read it.
8. **An `Expert()` id must exist and must be claimed exactly once.** `SelfTest()` reports
   an expert entry with two hosts, an expert entry with no host (it would be unreachable
   except by search — which is a legitimate design, so this is a warning, not an assert),
   and a host claiming more than six.
9. **`ui.contrast 1`** composites every painted text run against the surface it landed on
   and flags anything below its role's floor. The floors live next to the role table
   (SPEC §3.2), so the code and the spec cannot drift.
10. **`ui.audit 1`** overlays the row grid and the control-column boundary, asserts on any
    control painting outside `bbCtl`, and — new — asserts that the rail icon's painted
    centre equals `stage_pad_x + icon_w/2` in **both** rail states, so the collapse can
    never start shifting icons again (SPEC §3.3).

### What "one obvious call" means, measured

| Task | Today | A | A2 |
|---|---|---|---|
| add a toggle | ~6 lines + pick grouping/spacing/label style; no search, no reset | 1 line + handler | 4 lines, and it is explained, searchable, resettable and routed |
| explain a setting | a `PushFont`/`TextDisabled`/`PopFont` sandwich, styled per panel | a sub-line that makes the row taller | `.Help()` — costs the row no height |
| add a tune-once constant | a row nobody wants, on the main screen | a row nobody wants, on the main screen | `.Expert()` — off the screen, still findable |
| make everything consistent | manual review of 6 files | not possible to break | not possible to break, and not possible to leave unexplained |
