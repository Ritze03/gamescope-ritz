# `gamescope::ui` — the helper layer for E2

The user's goal, in their words:

> *"what i want is basically our own framework/helper functions (on top of ImGui), that
> makes it easier for you/AI, to update and extend the UI, while keeping it consistent
> with the rest."*

The test: **adding a setting is one obvious call, and producing something inconsistent
is hard.** E2 adds a second test, from the direction's own thesis: **one registration
must yield the row, the palette entry *and* the inspector content — and the inspector
must be something no call site can author.**

Proposed location `src/Overlay/UI/`:

```
Registry.h/.cpp     Area, Entry, Param, the fluent builders, the registration asserts
Bind.h              value binding: pointer, ConVar, getter/setter, config key
Shell.h/.cpp        the slab, the three regions, ComputeLayout(), the ladder, the palette
Row.h/.cpp          RowCtx — the ONLY right-bound allocator; the four columns
Controls.cpp        one painter per taxonomy kind, each takes an ImRect
Composite.cpp       the n×44 band rule (§4)
Inspector.cpp       renders Configure / Details / Overview from a selection
Match.h/.cpp        fuzzy scorer for Ctrl+K (adopted from Direction B)
Lint.cpp            ui_lint, ui_snapshot, ui_lint --host=inline
Areas/              one file per category — declarations only, no geometry, no ImGui
```

`Widgets.cpp` survives as the *painting* layer (slider, toggle, segmented, position grid
move in behind rect-taking signatures). `Chrome.cpp`'s dock / window / title-bar / drag /
collapse machinery is deleted. **`widgets::Checkbox` is deleted outright** — it has zero
callers (#60) and E2 has one binary affordance.

---


---

## Amendments

### 2026-08-27 — the launcher stops offering rows it cannot act on

Read-only rows (`Kind::Meter`, `Kind::Facts`, the `Composite(Graph)` shape) used to
appear in the command palette's search results despite `Enter` on one having nothing to
do — a launcher exists to jump to and *change* a setting, not to display one. Two
mechanisms cover this, and they are deliberately not the same one:

- **`Entry::ReadOnly()`** (`Registry.h`) is the existing taxonomy answer, already used by
  `DrawEntryRow`'s dimmed-control path. `CommandPalette.cpp`'s `Build()` excludes any
  entry where `ReadOnly()` is true — structurally, by kind, not by hand-tagging each
  registration. A future read-only kind is excluded for free; a future writable kind is
  included for free.
- **`Entry &HideFromPalette()` / `bool ExcludedFromPalette() const`** (`Registry.h`,
  issue #91) is the escape hatch for an entry that is *not* read-only but still
  shouldn't be a launcher destination for its own reason (`Build()` checks
  `e.ReadOnly() || e.ExcludedFromPalette()`). Kept separate from `ReadOnly()` on purpose:
  the two questions — "can this be acted on" and "should this be jumped to" — are
  independent, and folding the second into the first would make `ReadOnly()` lie for
  anything hidden for a reason other than being unwritable.

### 2026-08-23 — two Inspector modes, direction B's controls, one control height

1. **`EXPLAIN / CONFIGURE / DIAGNOSE` → `CONFIGURE / DETAILS`.** The four generators are
   unchanged; only where their output lands moved (§4.1). No category file changes,
   because no category file ever named a mode — which is the cheapest possible proof that
   clause 0 of the Attachment Law is doing its job.
2. **The mode strip carries counts instead of dimming** (§4.4), and Configure draws the
   selected row's own control before its parameters, using the same `Row::Draw` the Sheet
   uses. `Kind::IsReadOnly()` is the one dimmed state that survives.
3. **`ui::Widgets` control geometry is direction B's, uplifted ~25%** and pinned to one
   control height, `kControlH = 28` (`SPEC.md` §3.0). `Place()` is unchanged; the atoms
   it places are new. See §6 for the alignment mechanics, which did not move.
4. **`Checkbox` is gone from every header**, not just unused. `Bank` is added (§3.12 in
   `SPEC.md`) for a setting whose value is a set.
5. **One disabled mechanism.** `.DisabledUnless( pred, reason )` collapses the previous
   two spellings; a `Param` inherits its parent's reason unless it is the cause of it.
6. **`.Validate( fn )`** on text kinds, evaluated per keystroke; the message renders in
   Configure and dependent entries re-evaluate live.
7. **`Danger().Confirm()` is a two-stage arm** at the widget layer, not a modal.

## 1. Who decides what

| Decided by the **caller** | Decided by the **helper** |
|---|---|
| Which setting exists, what it is called | Where it sits, how tall its row is |
| What it is bound to, its range and default | Which control kind is legal for it |
| Its help text and keywords | Every colour, font, alpha, spacing value |
| Which category it belongs to | Whether depth lives in the Inspector or inline |
| Which of its parameters are depth | What the Inspector shows, in all four modes |
| — | Scroll, clipping, ID scoping, keyboard nav, the ladder |

**No public function accepts a pixel, a colour, a font, an alpha or an `ImVec2`.** There
is no `SetWidth`, no `SameLine`, no `PushStyleColor`, and — the E2 addition — **no
function that draws into the Inspector at all.**

---

## 2. Registration

```cpp
// UI/Registry.h
namespace gamescope::ui
{
    class Entry;                 // one setting: row + palette entry + inspector content
    class Param;                 // inspector-preferred depth attached to an Entry

    class Area                   // one rail item / one sheet
    {
    public:
        Area &Keywords( const char *sz );
        Area &Summary ( std::function<std::string()> );   // Overview card's live line
        Area &AvailableWhen( std::function<bool()> );
        void  Group   ( const char *szName );             // group-band cursor
        void  GroupCount( const char *szName );           // band + "4 / 7" + all/none

        // ---- the complete taxonomy. There are no other factories. ----------
        Entry &Switch   ( const char *szId, const char *szTitle, Bind<bool> );
        Entry &Slider   ( const char *szId, const char *szTitle, Bind<float> );
        Entry &Slider   ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Stepper  ( const char *szId, const char *szTitle, Bind<int> );
        Entry &Choice   ( const char *szId, const char *szTitle, Bind<int>,
                          std::span<const Option> );      // segmented or dropdown: helper decides
        Entry &Text     ( const char *szId, const char *szTitle, Bind<std::string> );
        Entry &Action   ( const char *szId, const char *szTitle, const char *szVerb,
                          std::function<void()> );
        Entry &Meter    ( const char *szId, const char *szTitle,
                          std::function<double()>, double lo, double hi );
        Entry &Facts    ( const char *szId, const char *szTitle,
                          std::function<std::string()> szSummary );   // §5.3 collapse
        Composite &Anchor( const char *szId, const char *szTitle,
                          Bind<int> vert, Bind<int> horiz );
        Composite &Hue   ( const char *szId, const char *szTitle, Bind<float> );
        Composite &Strip ( const char *szId, const char *szTitle, /* fader+meter */ ... );

        // ---- dynamic collections (shader effects, audio streams, profiles) ---
        // Re-expanded only when nCount changes; NOT rebuilt per frame.
        void Repeat( const char *szIdPrefix,
                     std::function<int()> nCount,
                     std::function<void( Area &, int i )> build );
    };

    class Registry
    {
    public:
        Area &Add( const char *szId, const char *szTitle, const char *szSection, Icon );
        void  SelfTest();                     // debug builds
    };

    void RegisterAll( Registry & );           // Areas/Areas.cpp — one line per area
    void DrawShell();                         // SettingsOverlay.cpp's entire frame body
}
```

`SettingsOverlay.cpp`'s per-frame body, in full:

```cpp
ui::DrawShell();
```

---

## 3. One registration, three products

### 3.1 A switch

```cpp
area.Switch( "display.allow_tearing", "Allow Tearing", &g_bAllowTearing )
    .Default( false )
    .Help( "Lets the game present without waiting for the display's refresh. "
           "Lowest latency; can show a horizontal seam on fast camera pans." )
    .Keywords( "immediate flip vsync latency tear" );
```

Four lines. What they produced, none of it asked for:

**The row** — 44 base, hairline-separated; label Sans 14 `TextLabel` left-bound at x=12
on the same vertical line as every label in the sheet; a 30×15 switch whose **right edge
is at `W − 28`**, on the same vertical line as every control in the sheet; hover, press
and focus painting; a `differs` state edge and a reset dot the moment the value leaves
its default; a contribution to the header's `differs N` chip.

**The palette entry** — `Ctrl+K` finds it under "Allow Tearing", "tearing", "vsync",
"latency", "flip", and `display.allow_tearing`. Selecting it jumps to Display ▸ Output
with the row selected.

**The Inspector's Configure body** — the help prose in `TextBody`, plus a fact grid that
nobody typed:

```
  ALLOW TEARING                                              switch

  Lets the game present without waiting for the display's
  refresh. Lowest latency; can show a horizontal seam on
  fast camera pans.

  now       on                    key       display.allow_tearing
  default   off                   writes    games/1174180.json
  applies   next frame            related   VRR / Adaptive Sync →

  [ reset to default ]                                        ^D
```

`now`, `default`, `key`, `writes` and `applies` come from the `Bind`; `related` comes
from shared keywords. None of it is typeable and none of it can be forgotten, because
forgetting it would mean not binding the setting.

### 3.2 A slider with a dependency

```cpp
area.Slider( "display.sharpness", "Sharpness", cv_sharpness )
    .Range( 0, 20 ).Step( 1 )
    .Default( 2 )
    .Help( "Strength of the FSR RCAS / NIS sharpening pass. Higher is crisper; "
           "too high adds ringing around high-contrast edges." )
    .Keywords( "sharpen fsr rcas cas crisp clarity ringing" )
    .DisabledUnless( IsSharpeningFilter(),
                     "the scaling filter is not fsr, nis or pixel" );
```

Absent: format string, min/max labels, width, `ImGuiSliderFlags`, `BeginDisabled` pair,
hint line, `Spacing`, `SameLine`. The format comes from the binding's declared type and
unit; the marks from the range; the default tick from the default; `AlwaysClamp` is
always on because there is no reason for it not to be.

`.DisabledUnless()` **has no overload without a reason string** — that is the entire
enforcement mechanism for the most common inconsistency in the current code, a control
that greys out and does not say why. The reason renders in Configure, and the row draws at
× 0.55 (3.27:1, `SPEC.md` §7.3) instead of E's × 0.34 (2.6:1).

**There is no `.Hint()`.** It was deleted from the API. A one-line hint is help; help
goes to Configure. This is the single largest calming change and it is enforced by the
absence of the method, not by a style note.

---

## 4. How a setting declares Inspector depth — `.Param()`

This is the E2-specific API and the place the Attachment Law lives.

```cpp
area.Slider( "display.sharpness", "Sharpness", cv_sharpness )
    .Range( 0, 20 ).Default( 2 )
    .Help( "..." )

    // ---- depth: Inspector ▸ CONFIGURE -------------------------------------
    .Param( "rcas_denoise", "RCAS denoise", &g_bRcasDenoise )
        .Default( true )
        .Help( "Suppresses the grain RCAS can amplify in dark, noisy areas." )
    .Param( "nis_variant", "NIS variant", &g_nNisVariant, kNisVariants )
        .Default( 0 )
        .Help( "Which NIS scaling kernel the sharpen pass pairs with." )

    // ---- depth: Inspector ▸ DETAILS (read-only by type) --------------------
    .Live( "effective", []{ return ui::Fact{ "effective RCAS",
                            std::format( "{:.2f}", RcasEffectiveStrength() ) }; } )
    .Live( "pass_cost", []{ return ui::Fact{ "pass cost",
                            std::format( "{:.2f} ms", g_flSharpenMs ) }; } );
```

### 4.1 The four generators, and the asserts behind them

> **Amended 2026-08-23.** The Inspector now has **two modes**, `CONFIGURE` and `DETAILS`
> (`SPEC.md` §5.1). The generators are unchanged in number and in kind — only where their
> output lands changed. That is the point of the amendment: re-sorting content between
> modes cost nothing at this layer, because a category file never named a mode.

| Generator | Feeds | Enforcement |
|---|---|---|
| `.Help( sz )` | **Configure** — the short description of what this does, above the values | **required**; aborts at registration if missing; `ui_lint` caps at 240 chars / 3 sentences |
| the `Bind` | **Details** — the binding grid | not typeable — derived from the schema |
| `.Param( leaf, title, bind )` | **Configure** rows, under the row's own control | **Prefix Law**, **Six Budget**, **One Level** — below |
| `.Live( leaf, fn )` | **Details** readouts | `fn` returns `ui::Fact`/`ui::Series`; **there is no `Bind` overload**, so a control cannot be constructed |

There is no fifth. Adding one is an edit to `Registry.h`, visible in a diff, landing in
this table and in `SPEC.md` §5.2.

**Configure also draws the row's own control**, which is not a generator — it is the same
`Row::Draw` the Sheet calls, on the same entry, at the same `--row` height. That is why a
parameter and its parent look identical in the Inspector: they are painted by one
function. For a read-only kind (`Facts`, `Meter`, `Graph`) there is no control to draw and
Configure says so in one sentence.

### 4.2 The Prefix Law, mechanically

`.Param()` takes a **leaf**, not an id. The full id is synthesised:

```cpp
Param &Entry::Param( const char *szLeaf, const char *szTitle, BindAny b )
{
    GAMESCOPE_ASSERT_MSG( !strchr( szLeaf, '.' ),
        "ui::Param leaf '%s' on '%s' contains a dot. A Param is one level deep; "
        "it cannot own Params. See SPEC.md 5.2 clause 3.", szLeaf, m_szId );

    GAMESCOPE_ASSERT_MSG( m_nParams < 6,
        "ui::Entry '%s' has 7 parameters. A row may own at most 6. "
        "Promote '%s' to a category — see SPEC.md 5.2 clause 3.", m_szId, m_szId );

    m_Params[ m_nParams++ ] = { .szId = Intern( "%s.%s", m_szId, szLeaf ), ... };
    return m_Params[ m_nParams - 1 ];
}
```

Because the id is *synthesised from the parent*, a Param's config key is a child of its
parent's key by construction — the law is not checked, it is unrepresentable to break.
To hide an unrelated setting in the Inspector you must give it a config key that lies
about what it belongs to, which is visible in `ui_snapshot`, visible in the on-disk JSON,
and wrong to a reviewer without opening the UI.

`Param` itself is a **distinct type** whose fluent surface is a strict subset of
`Entry`'s: `Default`, `Help`, `Range`, `Step`, `Unit`, `ZeroMeans`, `DisabledUnless`,
`Keywords`. It has **no `.Param()` and no `.Live()`**. One level, enforced by the type
system rather than by an assert.

### 4.3 There is no Inspector authoring API

E's `ui::Panel` / `Inspect( []( ui::Panel &i ){ ... } )` and B's `PaneCtx` are **deleted**.
Nothing in `Registry.h` or anywhere else a category file may include exposes a function
that draws into the Inspector. `Inspector.cpp` is:

```cpp
void ui::Inspector::Draw( const Entry *pSel, ImRect rc )
{
    if ( !pSel ) { DrawOverview( CurrentArea(), rc ); return; }

    const bool bWritable = !Kind::IsReadOnly( pSel->Kind() );
    const int  nCfg = ( bWritable ? 1 : 0 ) + pSel->ParamCount();
    const int  nDet = pSel->LiveCount() + DetailFactCount( pSel );

    // two cells, each carrying the count of what it holds
    Mode eMode = DrawModeStrip( rc, nCfg, nDet, bWritable );

    switch ( eMode )
    {
        case Mode::Configure:
            DrawHelp( pSel );                                  // .Help()
            DrawDisabledReason( pSel );                        // mandatory when disabled
            DrawValidationError( pSel );                       // text kinds only
            if ( bWritable ) Row::Draw( *pSel, rc );           // the row's OWN control
            else             DrawReadOnlyNote( pSel );
            for ( const Param &p : pSel->Params() ) Row::Draw( p, rc );
            DrawResetAction( pSel );
            break;

        case Mode::Details:
            DrawBindingGrid( pSel );                           // derived, typed by nobody
            DrawRelated( pSel );
            DrawKeyLine( pSel->Kind() );
            for ( const Live &l : pSel->Lives() ) DrawFact( l() );
            DrawKeyLogTail( pSel->Id(), 5 );
            break;
    }
}
```

Every branch reads the registration. **The Inspector is a pure function of the
selection**, holds no state, and cannot receive content that did not pass one of the four
generators.

### 4.4 The mode strip is a depth readout, not a tab bar

With three modes, an empty cell was dimmed. With two modes both cells are always
non-empty — `.Help()` is required, and the binding grid is always derivable — so the
readout moves from *dimming* to a **count on each cell**: `CONFIGURE 4 · DETAILS 9`. A
user looking at a row still sees at a glance how much is (and is not) hiding behind it,
and the number is more informative than the on/off it replaced.

The one dimmed state that survives is `ro`: a read-only kind has nothing to configure, so
its CONFIGURE cell reads `ro` and its body is one sentence pointing at Details. That state
is `Kind::IsReadOnly()`, not a flag anyone sets, so there is no way to have Configure
content and an `ro` cell.

---

## 5. The Reachability Law in code — one path, two hosts

`SPEC.md` §6.3: *every setting is editable with the Inspector closed.* This is what makes
it a mechanism rather than a promise.

```cpp
// UI/Shell.cpp
void ui::Sheet::DrawEntry( const Entry &e )
{
    Row::Draw( e, m_column );                       // the 44-tall row, always

    if ( e.ParamCount() && Layout().eInspectorHost == Host::Inline )
    {
        // Same painter, same grammar, same right-bound allocator — in the sheet.
        if ( m_Expanded.contains( e.Id() ) )
            for ( const Param &p : e.Params() )
                Row::Draw( p, m_column.Indented() );
    }
    // In Host::Column and Host::Drawer the params are drawn by Inspector.cpp,
    // from the same e.Params(), through the same Row::Draw.
}
```

`Row::Draw` does not know which host it is in. The shell picks the host from the ladder
(`SPEC.md` §8.3) or from the persisted `ui.inspector ∈ {column, drawer, hidden}`.

### 5.1 It is testable headlessly

```
] ui_lint --host=inline
ui_lint: rendering 11 areas with the Inspector forced off …
  areas 11 · entries 74 · params 41 · composites 5
  entries with a rect      74 / 74
  params  with a rect      41 / 41
  unreachable              0
ui_lint: PASS — the Reachability Law holds.
```

The command walks the registry, runs the sheet painter into a null draw list, and asserts
every registered entry *and every Param* received a rect. A setting that became
unreachable fails a command, not a code review. That is the difference between E's
Inspector Contract and E2's Reachability Law.

---

## 6. Right-bound alignment, mechanically

> Fix #2 — *"control alignment is inconsistent; left, right and centre basically random."*

`Row.h` exposes exactly one allocator, and it is right-anchored:

```cpp
// UI/Row.h
class RowCtx
{
public:
    ImRect Place    ( float flWidthBase ) const;   // right edge == m_ctl.Max.x, ALWAYS
    ImRect PlaceFull() const { return m_ctl; }     // == Place( zone width )
    ImRect Value    () const;                      // right-bound at Lw; only for kinds
                                                   // that cannot self-display (SPEC 2.3)
private:
    ImRect m_ctl;                                  // [ Lw + 12 , W − 28 ]
};

ImRect RowCtx::Place( float w ) const
{
    const float px = ImMin( w * Layout().scale, m_ctl.GetWidth() );
    return ImRect( m_ctl.Max.x - px, m_ctl.Min.y, m_ctl.Max.x, m_ctl.Max.y );
}
```

There is no `Left()`, no `Centre()`, no `SetCursorPosX`, no `SameLine`. A painter receives
a rect and draws inside it:

```cpp
// Every Place() below is --H tall.  kControlH is the ONE control height (SPEC 3.0);
// a control whose graphic is deliberately shorter is centred in the rect it is given.
constexpr float kControlH = 28.f;
constexpr float kSwitchW  = 40.f, kSwitchH = 20.f, kSwitchKnob = 16.f;

void Controls::Switch ( const RowCtx &r, Bind<bool> b )
{
    widgets::Toggle( r.Place( kSwitchW ), b );        // 40 x 20 graphic, 28-tall hit box
    Text::Value( r.Value(), b ? "on" : "off" );       // SPEC 2.3 — a switch cannot self-display
}
void Controls::Stepper( const RowCtx &r, ... )
{
    widgets::Stepper( r.Place( 44 ), ... );           // B's borderless "- +", 18 + 8 + 18
    Text::Value( r.Value(), ... );                    // the number lives in the value column
}
void Controls::Slider ( const RowCtx &r, ... )
{
    widgets::Slider( r.PlaceFull(), ... );            // B's paint, E2's lane (SPEC 3.4)
    Text::Value( r.Value(), ... );
}
void Controls::Choice ( const RowCtx &r, const Options &o )
{
    // the helper measures and auto-downgrades; the caller has no say.
    // ONE predicate, used for every host — sheet, inspector and inline fallback alike.
    const bool bSeg = widgets::SegmentedFits( o, r.PlaceFull().GetWidth() );
    bSeg ? widgets::Segmented( r.Place( widgets::SegmentedWidth( o ) ), o )
         : widgets::Dropdown ( r.PlaceFull(), o );    // value + caret; self-displaying
}
void Controls::Bank( const RowCtx &r, BindSet b, const Options &o )
{
    widgets::ChipBank( r.Place( widgets::BankWidth( o ) ), b, o );
}
```

The per-kind widths live only here and in `SPEC.md` §3, and nowhere else.

> **Amended 2026-08-23.** `Place( 30 )` / `Place( 96 )` / `Place( n × 92 )` /
> `Place( 280 )` are gone. The switch grew to B's geometry uplifted 25%; the stepper lost
> its box (B draws two glyphs, not a spinbox); segmented cells are content-sized, so their
> width is measured rather than assumed; and dropdown and text are self-displaying, so they
> take the full lane and leave the value column empty. `Place()` itself did not change —
> which is the whole argument for having it.

Left alignment is not discouraged; it is unrepresentable. Full-bleed controls satisfy the
rule for free because `PlaceFull()`'s right edge is the same `m_ctl.Max.x`.

`Lw` is computed once per column by the shell and never by content:

```cpp
const float Lw = ImClamp( ImFloor( 0.46f * W ), W - 420.f * s, W - 200.f * s );
```

---

## 7. Composites — the Anchor fix, in code

> Fix #3 — *"composite controls look orphaned. Named case: Monitor ▸ Placement ▸ Anchor."*

```cpp
area.Group( "Placement" );

area.Anchor( "monitor.anchor", "Placement",
             &g_fpsHud.nAnchorVert, &g_fpsHud.nAnchorHoriz )
    .Default( 0, 2 )                                  // top-right
    .Help( "Which screen corner the monitor is anchored to. The offsets nudge it "
           "away from that corner." )
    .Keywords( "anchor placement position corner where margin offset" )
    .Param( "margin_v", "Vertical margin",   &g_fpsHud.nMarginV )
        .Default( 32 ).Unit( "px" ).Range( 0, 400 )
        .Help( "Distance from the anchored horizontal edge." )
    .Param( "margin_h", "Horizontal margin", &g_fpsHud.nMarginH )
        .Default( 32 ).Unit( "px" ).Range( 0, 400 )
        .Help( "Distance from the anchored vertical edge." );
```

The two steppers that used to float beside the grid — the orphaning — are now **Params**:
`monitor.anchor.margin_v` / `.margin_h`. Prefix Law satisfied by construction. They live
in Configure, render inline beneath the band when the Inspector is hidden, and are found
by `Ctrl+K → "margin"`.

`Composite` is a distinct return type; its fluent surface is `Entry`'s plus nothing, and
its geometry is decided by `Composite.cpp`:

```cpp
// UI/Composite.cpp — the ONLY place a band's geometry is computed.
struct BandSpec { int nLines; ImVec2 bodyBase; };            // n ∈ {2,3}

static constexpr BandSpec kSpecs[] = {
    /* Anchor */ { 3, { 96, 96 } },     /* Hue    */ { 2, {  0, 44 } },  // 0 = full-bleed
    /* Strip  */ { 2, {  0, 52 } },     /* Graph  */ { 3, {  0, 96 } },
    /* Colour */ { 2, {  0, 52 } },
};

void Composite::Draw( const Entry &e, const ImRect &col )
{
    const BandSpec &S  = kSpecs[ (int)e.CompositeKind() ];
    const ImRect band  = AllocateRows( S.nLines );                 // exactly nLines × 44
    const RowCtx line1 = RowCtx::ForBand( band, /*line*/ 0 );

    Text::Label( line1, e.Title() );                               // reads as a row
    Text::Value( line1.Value(), e.ResolvedString() );              // "top-right · 32 / 32"

    const ImRect body = S.bodyBase.x > 0                           // right-bound, spans 1..n
        ? RightBound( band, S.bodyBase )
        : ImRect( line1.CtlMin(), band.Max );
    Painters[ (int)e.CompositeKind() ]( body, e );

    Hairline( band.Min.y ); Hairline( band.Max.y );
}
```

Four properties fall out and no call site can affect any of them: the band's height is a
whole number of row steps, its first line reads as an ordinary row, its body's right edge
is the sheet's control line, and lines 2..n of the label column are air.

---

## 8. Dynamic collections

The registry is declarative, but three panels are collections of live objects. `Repeat`
re-expands only when the count changes, so the registry is still built once per topology
change and never per frame:

```cpp
area.Repeat( "shaders.fx", []{ return (int)g_effects.size(); },
    []( ui::Area &a, int i )
    {
        ReshadeEffect &fx = g_effects[ i ];
        a.Switch( fx.Key(), fx.Name(), &fx.bEnabled )
         .Default( false )
         .Help( fx.Description() )
         .Keywords( fx.Keywords() )
         .Param( "strength", "Strength", ui::Cfg( fx.Key( "strength" ) ) )
            .Range( -1.f, 1.f ).Default( 0.f ).Help( "..." )
         .Param( "protect_skin", "Protect skin tones", ui::Cfg( fx.Key( "protect_skin" ) ) )
            .Default( true ).Help( "..." )
         .Live( "preview", [&fx]{ return ui::Series{ fx.HistogramBins() }; } );
    } );
```

Adaptive Brightness's six tuning values are six `.Param()`s — exactly at the Six Budget,
which is the intended pressure: a seventh means it should be a category.

---

## 9. Bindings

```cpp
// UI/Bind.h
template <typename T> struct Binding
{
    T    Get() const;
    void Set( T v );                     // routes through ConfigManager's write queue
    T    Default() const;
    bool Differs() const;
    const char *Key() const;             // "display.sharpness"
    const char *DestinationFile() const; // "global.json" / "games/1174180.json"
    const char *Unit() const;
    Applies     When() const;            // Live | NextFrame | NeedsRestart
};

template <typename T = auto> Binding<T> Cfg ( const char *pszKey );  // preferred: schema-backed
template <typename T>        Binding<T> Bind( T *p );                // transient/session state
template <typename T>        Binding<T> Bind( T (*get)(), void (*set)( T ) );
```

`Cfg` is the seam where "which file does this write" — computed ad hoc by `PanelConfig`
today (`SessionAppId()` / `IsSessionOverrideActive()`) — is computed once. `Bind` degrades
honestly: no schema means no default, so Details shows `session only` and the reset
affordance is suppressed rather than lying.

---

## 10. The command palette (adopted from Direction B, as a feature)

One registration already produced the palette entry (§3.1). The palette is ~40 lines over
the registry plus B's fuzzy scorer:

```
  ⌕ margin                                       11 areas · 74 settings · 41 params
  ───────────────────────────────────────────────────────────────────────────────
  ▸ System / Monitor ▸ Placement    Vertical margin      param   32 px    differs
    System / Monitor ▸ Placement    Horizontal margin    param   32 px
    Setup  / Appearance             Notification margin          24 px
  ───────────────────────────────────────────────────────────────────────────────
  ↑↓ move    ↵ jump & select    ⇧↵ edit inline    ESC dismiss
```

The `param` chip is load-bearing for the anti-junk-drawer argument: **Params are searchable
exactly like Sheet rows**, and `Enter` on one selects the parent row, opens the Inspector
in Configure, and focuses that param. A setting living in the Inspector is therefore *one
keystroke* from anywhere in the product — a stronger reachability guarantee than most
Sheet rows get, and the answer to "is depth the same as hidden?"

---

## 11. Adding a whole area — complete, nothing omitted

```cpp
// src/Overlay/UI/Areas/FrameLimiter.cpp
#include "UI/Registry.h"

void gamescope::ui::RegisterFrameLimiterArea( Registry &reg )
{
    auto &area = reg.Add( "display.frame_limiter", "Frame Limiter", "DISPLAY",
                          Icon::Performance )
        .Keywords( "fps cap limit vrr adaptive sync refresh" )
        .Summary( []{ return g_nTargetFps ? std::format( "{} fps cap", g_nTargetFps )
                                          : std::string( "uncapped" ); } );

    area.Group( "Frame limiter" );

    area.Stepper( "display.fps_limit", "FPS Limit",
                  ui::Bind<int>( []{ return g_nSteamCompMgrTargetFPS; },
                                 []( int v ){ steamcompmgr_set_fps_limit( v ); } ) )
        .Range( 0, 1000 ).Step( 5 ).Unit( "fps" )
        .Default( 0 ).ZeroMeans( "Unlimited" )
        .Help( "Caps presentation rate. 0 removes the cap. Applied live via the "
               "debug_set_fps_limit convar." )
        .Keywords( "frame rate cap limiter throttle" )
        .Param( "pacing", "Pacing", &g_nPacingMode, kPacingModes )
            .Default( 0 ).Help( "How the limiter spaces frames when the cap is active." );

    area.Switch( "display.adaptive_sync", "VRR / Adaptive Sync", ui::Cfg( "display.adaptive_sync" ) )
        .Default( false )
        .DisabledUnless( BackendSupportsVrr(), "the current backend has no VRR path" )
        .Help( "Lets the display's refresh follow the game's frame rate." )
        .Keywords( "vrr freesync gsync adaptive tearing" )
        .Live( "range", []{ return ui::Fact{ "refresh range", VrrRangeString() }; } );

    area.Facts( "display.limiter_state", "Limiter state",
                []{ return std::format( "{} · {:.2f} ms budget",
                        g_bLimiterActive ? "active" : "idle", FrameBudgetMs() ); } )
        .Help( "What the limiter is currently doing." )
        .Live( "sleep",  []{ return ui::Fact{ "sleep",  std::format( "{:.2f} ms", g_flSleepMs  ); } )
        .Live( "missed", []{ return ui::Fact{ "missed", std::format( "{}", g_nMissed ) }; } );
}
```

...plus one line in `Areas/Areas.cpp`. That is the whole task: **a rail item, a sheet,
palette entries for every setting and every param, breadcrumb, per-category reset,
provenance, Configure / Details content, keyboard reachability, the inline
fallback, and the responsive ladder — from ~30 lines with no geometry in them.**

---

## 12. What stops inconsistency, mechanically

Seven mechanisms, strongest first. The first four are type-level.

1. **No geometry, colour or font in the API.** No pixels, no `SameLine`, no
   `PushStyleColor`. Enforced by the signatures.
2. **No Inspector authoring API at all** (§4.3). The strongest E2-specific guarantee:
   the Inspector cannot receive content that did not pass one of the four generators.
3. **One right-bound allocator** (§6). Left alignment is unrepresentable.
4. **`Param` is a type without `.Param()` or `.Live()`.** Depth is one level, by the type
   system.
5. **Required arguments where omission is the common bug.** `.Help()` required.
   `.DisabledUnless()` requires a reason. `.Danger()` returns a type whose only method is
   `.Confirm()`. `Slider`/`Stepper` require a range. `.Default()` required for anything
   resettable.
6. **Auto-downgrade instead of caller choice.** `Choice()` measures and picks segmented vs
   dropdown. The Sheet picks 1–3 columns. The shell picks the Inspector's host. A caller
   cannot pick wrong because a caller does not pick.
7. **`ui_lint` and `ui_snapshot`.**

```
] ui_lint
ui_lint: 5 findings
  display.output/Force Grab Cursor     no Help() — registration would abort in debug
  monitor.font_size                    Bind() with no default — reset suppressed
  setup.appearance/Background veil     Help() is 340 chars — cap is 240
  monitor.modules                      6 params, at the budget — next one must promote
  image.shaders/AdaptiveBrightness     Live() count 4 — Details paints 2 animated elements, cap 1
ui_lint: run `ui_lint --host=inline` to verify the Reachability Law.
```

```
] ui_snapshot | head
display.upscaling                area   "Upscaling"        DISPLAY
  display.filter                 choice  linear|nearest|fsr|nis|pixel  def=linear  now=fsr  *
  display.sharpness              slider  0..20  def=2  now=5  unit=—  writes=games/1174180.json  *
    display.sharpness.rcas_denoise   param  switch  def=on   now=on
    display.sharpness.nis_variant    param  choice  def=0    now=0
  display.scaler                 choice  auto|fit|fill|stretch|integer  def=auto  now=integer  *
```

Two `ui_snapshot` runs diffed is a complete, reviewable record of a UI change — and the
indentation makes a Prefix Law violation impossible to miss, because a Param that does not
belong to its parent would not be able to indent under it.

---

## 13. Migration seam

```cpp
// A category whose body is still legacy panel code, hosted verbatim in the sheet.
area.Escape( []{ PanelDisplay_DrawLegacyBody(); } );
```

`Escape()` pushes the old `ImGuiStyle`, runs the old code inside the sheet's child, pops.
It looks wrong on purpose — it is visibly the un-migrated part — it is the only function
in the API that permits arbitrary ImGui, and `ui_lint` counts remaining call sites as
severity `migration`. Expected to reach zero. Sequencing and cost in `FEASIBILITY.md` §7.
