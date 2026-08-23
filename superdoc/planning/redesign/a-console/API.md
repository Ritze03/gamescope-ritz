# Console Kit — the helper layer

`namespace gamescope::ui` — a small declarative layer over Dear ImGui. New files:
`src/Overlay/Console/Console.{h,cpp}` (shell, nav, animation), `Rows.cpp` (the taxonomy),
`Sheet.cpp`, `Metrics.h`, `Theme.h`.

**The design goal, stated as a test:** adding a setting is one call, and there is no way to
write that call that produces something inconsistent — because the call has no parameters
that could disagree with another screen. No sizes, no positions, no fonts, no colours, no
`SameLine`, no `PushStyleVar`. The caller supplies *semantics*; the kit supplies *pixels*.

---

## 1. The whole public surface

```cpp
// src/Overlay/Console/Console.h
#pragma once
#include <cstddef>
#include <span>

namespace gamescope::ui
{
    // ---------------------------------------------------------------- screens

    enum class Section { Gamescope, Shaders, SystemMonitor, Audio, Config, Log, Count };
    enum class Stage   { List, Wide, Split };

    struct ScreenDesc
    {
        const char *pszId;                 // "gamescope.upscaling" -- stable, used by Drill() and Find
        const char *pszTitle;              // "Upscaling" -- breadcrumb + sub-tab label
        Section     section;
        Stage       stage      = Stage::List;
        bool        bSubTab    = true;     // false = a drill target, not a sub-tab
        void      (*pfnDraw)() = nullptr;  // the row list
        void      (*pfnWell)() = nullptr;  // optional pinned region above the list
        const char *pszBadge   = nullptr;  // routing override, e.g. "global only"
    };

    void RegisterScreen( const ScreenDesc &desc );

    // The whole overlay, one call per frame. Replaces every BeginPanelWindow()/
    // EndPanelWindow()/DrawDock() call site that exists today.
    void DrawConsole();

    // ---------------------------------------------------------------- rows

    enum class Status { None, Ok, Warn, Idle };
    enum class Intent { Normal, Destructive };

    // Per-row modifiers. Designated initialisers at the call site, so a row that
    // needs nothing extra reads as a bare call and a row that needs one thing
    // names exactly that one thing.
    struct Opt
    {
        const char *pszHelp       = nullptr;  // sub-line, Sans meta
        const char *pszUnit       = nullptr;  // appended to the value, Mono
        bool        bDisabled     = false;
        const char *pszWhyDisabled= nullptr;  // REQUIRED when bDisabled (debug assert)
        Status      status        = Status::None;
        Intent      intent        = Intent::Normal;
        float       flStep        = 0.0f;     // 0 = kit picks from the range
        const char *pszDefaultTip = nullptr;  // shown by Y (reset) confirmation
    };

    void Group  ( const char *pszHeading );

    bool Switch ( const char *pszLabel, bool  *pbValue,           const Opt &o = {} );
    bool Slider ( const char *pszLabel, float *pflValue, float flMin, float flMax,
                  const char *pszFmt = "%.2f",                    const Opt &o = {} );
    bool Slider ( const char *pszLabel, int   *pnValue,  int nMin, int nMax,
                  const char *pszFmt = "%d",                      const Opt &o = {} );
    bool Choice ( const char *pszLabel, int   *pnIndex,
                  std::span<const char *const> options,           const Opt &o = {} );
    bool Text   ( const char *pszLabel, char  *pszBuf, size_t cchBuf, const Opt &o = {} );
    bool Color  ( const char *pszLabel, float *pflHueDegrees,      const Opt &o = {} );
    bool Action ( const char *pszLabel, const char *pszVerb,       const Opt &o = {} );
    bool Drill  ( const char *pszLabel, const char *pszSummary,
                  const char *pszScreenId,                        const Opt &o = {} );
    void Readout( const char *pszLabel, const char *pszValue,      const Opt &o = {} );
    bool MultiSelect( const char *pszLabel, bool *pbFlags,
                  std::span<const char *const> names,              const Opt &o = {} );
    void Chips  ( const char *pszLabel, std::span<const char *const> names,
                  const bool *pbOn,                                const Opt &o = {} );

    // ---------------------------------------------------------------- wells

    bool Anchor ( const char *pszLabel, int *pnRow, int *pnCol );  // 3x3 over the preview
    void Meter  ( const char *pszChannel, float flLevel01 );
    void Graph  ( std::span<const float> vecSamples, float flOutlierThreshold );
    void Preview( void (*pfnDrawContent)( void *pDrawList, ImVec2 origin, float flScale ) );

    // ---------------------------------------------------------------- plumbing

    // One save per frame, not one per row. Any row that returns true inside this
    // scope arms the callback; the callback fires once at EndFrame.
    struct SaveScope { explicit SaveScope( void (*pfnSave)() ); ~SaveScope(); };

    // Navigation, for the rare programmatic case (a notification's "open this").
    void GoTo( const char *pszScreenId, const char *pszFocusLabel = nullptr );
}
```

That is the entire API. Twelve row calls, one group call, four well calls, one registration
call, one draw call.

**What is deliberately absent** — and absent is the feature:

| Not exposed | Why |
|---|---|
| any `float flWidth` / `ImVec2 size` | the kit owns the control column |
| `SameLine`, `Dummy`, `Spacing`, `Indent`, `Separator` | the only vertical separation is `Group()` |
| `PushFont` / `PushStyleColor` / `PushStyleVar` | roles are picked per control kind |
| `BeginChild` / `BeginGroupBlock` | the console has no nested containers |
| `BeginTabBar` | sub-tabs come from `ScreenDesc`, one mechanism |
| a tooltip call | help goes on the sub-line (SPEC §2.3) |
| a "panel window" call | there are no windows |

If a screen author needs one of these, that is the signal that a *new row kind* belongs in
the kit — added once, in `Rows.cpp`, correct for every screen forever. That is the whole
bet.

---

## 2. Adding a setting

The literal answer to "what does adding a new toggle plus a new slider to an existing
screen look like".

Today, in `PanelShaders.cpp`:

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

Same two settings, with the kit:

```cpp
if ( ui::Switch( "Protect skin tones", &v.protect_skin_tones ) )
    SetRuntimeUniformBool( "vibrancy_protect_skin_tones", v.protect_skin_tones );

if ( ui::Slider( "Strength", &*s.strength, 0.0f, 2.0f, "%.2f",
     { .pszHelp = "Runs before the scaler — works with any Filter.",
       .bDisabled = !s.enabled,
       .pszWhyDisabled = "Turn Pre-Sharpen on first." } ) )
    SetRuntimeUniformFloat( "pre_sharpen_strength", *s.strength );
```

What went away, per call site: the `QueueSave()` (the enclosing `SaveScope` does it), the
`##id` suffix (the kit pushes the screen id, so labels only need to be unique on a screen),
the `PushFont`/`TextDisabled`/`PopFont` sandwich for help text, the manual
`BeginDisabled`/`EndDisabled` bracket, and the free-floating prose line that was neither a
row nor a label and rendered differently in every panel that had one.

What arrived for free: the row is 44px like every other row, its label sits at the same x
as every other label, its value is Mono/accent, its help is Meta at 46%, Y resets it, `/`
finds it, the disabled state explains itself, and it is one hit target for a mouse and one
focus stop for a controller.

**A brand-new setting is exactly one line plus a handler.** There is no second place to
register it, no layout to update, no width to pick.

---

## 3. Adding a whole screen

```cpp
// src/Overlay/Console/ScreenAudio.cpp
#include "Console.h"

namespace
{
    void DrawAudioWell()
    {
        ui::Meter( "L", g_flPeakL );
        ui::Meter( "R", g_flPeakR );
    }

    void DrawAudio()
    {
        ui::SaveScope save( &QueueSave );

        ui::Group( "Output" );
        ui::Readout( "Device", g_sSinkName.c_str(), { .status = ui::Status::Ok } );

        if ( ui::Slider( "Volume", &g_nVolumePercent, 0, 150, "%d",
             { .pszUnit = "%" } ) )
            ApplyVolume();

        if ( ui::Switch( "Mute", &g_bMuted ) )
            ApplyMute();

        ui::Group( "Routing" );
        ui::Drill( "Pick a stream manually", g_sStreamSummary.c_str(), "audio.streams" );

        if ( ui::Action( "Clear manual override", "Clear" ) )
            ClearOverride();
    }

    const ui::ScreenDesc kAudio =
    {
        .pszId   = "audio.output",
        .pszTitle= "Output",
        .section = ui::Section::Audio,
        .stage   = ui::Stage::List,
        .pfnDraw = &DrawAudio,
        .pfnWell = &DrawAudioWell,
    };
    UI_REGISTER_SCREEN( kAudio );   // static-init helper, one line
}
```

That is the *entire* file's UI surface. The rail entry, the sub-tab, the breadcrumb, the
scrolling, the focus model, the legend, the search index, the routing badge, the spacing
and the theming are all supplied by the kit. There is no `Begin`, no `End`, no window, no
size, no position, no dock registration, no `PanelId` enum to extend.

Compare to today: a new panel needs a `PanelId` enumerator, a `Chrome.h` icon, a dock
button, a `BeginPanelWindow`/`EndPanelWindow` pair, a default size, a tiled position, a
tab bar, and its own idea of what a section header looks like.

A **Wide** screen is the same shape with one field changed:

```cpp
const ui::ScreenDesc kLog =
{
    .pszId="log.gamescope", .pszTitle="Gamescope", .section=ui::Section::Log,
    .stage = ui::Stage::Wide,          // <- rail collapses, body goes full width
    .pfnDraw = &DrawGamescopeLog,
};
```

and `DrawGamescopeLog()` uses the one body helper a Wide screen gets:

```cpp
void DrawGamescopeLog()
{
    ui::LogView( "gamescope", LogCapture::Gamescope().Lines() );  // Rows.cpp, virtualised
}
```

---

## 4. How the kit is built (the parts that matter)

### 4.1 Metrics — computed once per frame, read by everything

```cpp
struct Metrics
{
    float u, base;            // spacing unit, type base
    float flRowH, flRowPadY, flStagePadX;
    float flRailW, flCtlW, flCtlWideW, flValueSlotW;
    float flConsoleW, flConsoleH;

    static const Metrics &Frame();   // memoised per frame off display_scale + surface size
};
```

Every constant in `SPEC.md` §2.2 lives here and nowhere else. No `.cpp` in the kit contains
a bare pixel literal that isn't `1.0f` (hairlines) — enforced by review and by
`ui.audit`.

### 4.2 One row frame, every kind through it

```cpp
// Rows.cpp -- private
struct RowFrame
{
    ImRect bb, bbLabel, bbCtl;
    bool   bHovered, bHeld, bFocused, bDisabled;
};

static RowFrame BeginRow( const char *pszLabel, const Opt &o, bool bWideCtl );
static void     EndRow  ( const RowFrame &rf, const Opt &o );
```

`BeginRow` does: `ImGui::PushID(label)` → measure help line → `ItemSize`/`ItemAdd` over the
whole row → `ButtonBehavior` on the whole row (so the row is one hit target) →
`RenderNavCursor` suppressed and replaced by the accent tick → paint fill/divider/label/help
→ hand back the control rect. `EndRow` pops the ID and registers the label into the search
index.

**Every** row kind is `BeginRow` + draw-into-`bbCtl` + `EndRow`. There is no second path.
This is where the discipline the current `Widgets.cpp` already has
(`ButtonBehavior`/`SliderBehavior`/`ItemAdd`) is preserved — it just moves up one level, so
each kind inherits it instead of re-deriving it.

Example, the full switch:

```cpp
bool Switch( const char *pszLabel, bool *pbValue, const Opt &o )
{
    const Metrics &m = Metrics::Frame();
    RowFrame rf = BeginRow( pszLabel, o, /*wide*/ false );

    const ImVec2 sz( m.u * 11, m.u * 6 );
    const ImVec2 tl( rf.bbCtl.Max.x - sz.x, rf.bbCtl.GetCenter().y - sz.y * 0.5f );

    bool bChanged = false;
    if ( !o.bDisabled && ( rf.bHovered && ImGui::IsMouseReleased( 0 ) ) )
        { *pbValue = !*pbValue; bChanged = true; }
    if ( !o.bDisabled && rf.bFocused && NavStep() )      // <- ← / → adjust, §4.4
        { *pbValue = !*pbValue; bChanged = true; }

    Theme::PaintSwitch( ImGui::GetWindowDrawList(), tl, sz, *pbValue, rf );

    EndRow( rf, o );
    if ( bChanged ) SaveScope::Arm();
    return bChanged;
}
```

Roughly 20 lines per kind, twelve kinds. All the painting lives in `Theme::Paint*`, which
is the only place a colour token is read.

### 4.3 Screens and the shell

```cpp
void DrawConsole()
{
    const Metrics &m = Metrics::Frame();

    ImGui::SetNextWindowPos ( CenteredPos( m ) );
    ImGui::SetNextWindowSize( ImVec2( m.flConsoleW, m.flConsoleH ) );
    ImGui::Begin( "##console", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus );

    Anim().Tick( ImGui::GetIO().DeltaTime );

    DrawHeader();                 // breadcrumb, pips, badge, live cluster
    DrawRail( Anim().flRailW );   // animated width
    BeginStage();                 //   clip rect + scroll region
      const ScreenDesc *s = CurrentScreen();
      ImGui::PushID( s->pszId );  //   one namespace per screen -> labels need no ##id
      DrawSubTabs( s );
      if ( s->pfnWell ) DrawWell( s->pfnWell );
      s->pfnDraw();
      ImGui::PopID();
    EndStage();
    DrawSheetIfOpen();            // stock BeginPopupModal, styled as a sheet
    DrawLegend();

    ImGui::End();
}
```

One window. One draw list (plus the modal's). Stock nav inside it.

### 4.4 ← / → adjusts the focused row without "entering" it

This is the one place the kit swims against ImGui's grain, so it is centralised in exactly
one helper:

```cpp
// Returns -1 / 0 / +1 for the focused row this frame, with repeat + acceleration.
int NavStep()
{
    if ( !ImGui::IsItemFocused() ) return 0;
    const int n = ImGui::GetKeyPressedAmount( ImGuiKey_GamepadDpadRight, 0.35f, 0.06f )
                - ImGui::GetKeyPressedAmount( ImGuiKey_GamepadDpadLeft,  0.35f, 0.06f )
                + ImGui::GetKeyPressedAmount( ImGuiKey_RightArrow,       0.35f, 0.06f )
                - ImGui::GetKeyPressedAmount( ImGuiKey_LeftArrow,        0.35f, 0.06f );
    if ( n ) ImGui::SetNavCursorVisible( true );
    return n;
}
```

with `io.ConfigNavMoveSetMousePos = false` and the window's nav layer configured so
horizontal nav does not try to move focus between items (there is only ever one item per
row, so there is nothing horizontal for ImGui's own nav to move *to* — which is why this
works cleanly here and would not in a two-column layout). See `FEASIBILITY.md` §2.

### 4.5 Enforcement — how an inconsistent screen becomes hard to build

1. **No geometry in the API.** You cannot pass a size, so you cannot pass a wrong one.
2. **`Group()` is the only vertical spacing.** Two adjacent rows are always exactly one
   divider apart.
3. **`PushID(screen)` per screen** removes the `##suffix` habit that leaks into labels.
4. **Unregistered screens don't render.** There is no way to draw a stray window.
5. **`ui.audit 1`** (ConVar) overlays the row grid, the control column boundary and the
   baseline of every text run, and asserts in debug builds when a control paints outside
   `bbCtl` or a `bDisabled` row has no `pszWhyDisabled`.
6. **`ui.contrast 1`** flags any painted text whose composited alpha over the current
   surface falls below the role's floor — catches the "this reads too dark" class of bug
   (`Palette.h`'s issue #62) at the source instead of one call site at a time.
7. **The kit owns the only `Theme::Paint*` functions.** A screen file that includes
   `imgui.h` at all is a review flag; screen files include `Console.h` and nothing else.

---

## 5. Migration bridge

The kit ships behind `overlay.console 1` (ConVar, default off at first) as a *seventh
panel* — `DrawConsole()` called from the same place `DrawDock()` is today. Old windows and
the new console coexist for the duration of the port, one screen at a time, and the flag
flips when the last screen lands. Nothing is deleted until then.

Suggested order (smallest risk first): **Shaders** (3 screens, all sliders/switches — the
pilot that proves the row grid) → **Gamescope** (4 screens, exercises Choice/segmented
width picking) → **Config** (3 screens, exercises Colour, Text, Action, Destructive) →
**Audio** (2 screens, first Well) → **Log** (first Wide) → **System Monitor** last, because
its Well is the only genuinely hard piece (`FEASIBILITY.md` §5).
