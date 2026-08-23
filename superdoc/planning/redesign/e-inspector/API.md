# `gamescope::ui` — the helper layer

The user's actual goal, in their words:

> *"what i want is basically our own framework/helper functions (on top of ImGui), that
> makes it easier for you/AI, to update and extend the UI, while keeping it consistent
> with the rest."*

The test this API is designed against: **adding a setting is one obvious call, and
producing something inconsistent is hard.** Everything below follows from that.

Proposed location: `src/Overlay/UI/` — `Shell.h/cpp`, `Sheet.h/cpp`, `Row.h/cpp`,
`Controls.cpp`, `Bind.h`, `Registry.h/cpp`, `Lint.cpp`. `Widgets.cpp` survives as the
*painting* layer underneath (the slider, toggle, checkbox, segmented control and
position grid move in unchanged); `Chrome.cpp`'s dock/window/title-bar machinery is
deleted.

---

## 1. The shape of the thing

It is **declarative-inside-immediate**. Calls still run every frame, in ImGui's style,
with no retained widget tree. What changed is *who decides what*:

| Decided by the **caller** | Decided by the **helper** |
|---|---|
| Which setting exists | Where it sits |
| What it is called | How tall the row is |
| What it is bound to | Which control kind is legal for it |
| Its range and default | Every colour, font, alpha, and spacing value |
| Its help text | Hover / focus / press / disabled painting |
| Which category it belongs to | Scroll, clipping, ID scoping, keyboard nav |
| — | Whether it fits, and what to do when it doesn't |

The rule that makes this hold: **no function in the public API accepts a pixel value,
a colour, a font, or an `ImVec2`.** There is no `SetWidth`, no `SameLine`, no
`PushStyleColor`. A caller physically cannot express an inconsistent layout.

---

## 2. Registration — the whole navigation model in one struct

```cpp
// UI/Registry.h
namespace gamescope::ui
{
    enum class Density { Comfort, Dense, Raw };

    struct CategoryDesc
    {
        const char     *pszId;        // stable: "display.upscaling" — palette + deep links
        const char     *pszSection;   // rail section header: "DISPLAY"
        const char     *pszTitle;     // rail label + breadcrumb: "Upscaling"
        chrome::Icon    icon;
        Density         density;
        void          (*pfnDraw)( Sheet & );
        int           (*pfnBadge)()   = nullptr; // optional trailing count chip
    };

    // Call once, at static-init or from EnsureImguiInit(). Order of registration is
    // rail order within a section; section order is first-seen order.
    void RegisterCategory( const CategoryDesc & );

    // Drawn once per frame by SettingsOverlay.cpp, replacing every Panel*_Draw()
    // call and chrome::DrawDock(). This is the only entry point.
    void DrawShell();
}
```

`DrawShell()` owns: the slab rect, the three region rects, the responsive ladder, the
rail, the breadcrumb, the footer hint, the command palette, the region focus model, the
gamepad routing, and the dispatch into the selected category's `pfnDraw`.

`SettingsOverlay.cpp`'s per-frame body becomes, in full:

```cpp
ui::DrawShell();
```

---

## 3. Adding a setting

### 3.1 A toggle

```cpp
s.Switch( "Allow Tearing", ui::Cfg( "display.allow_tearing" ) )
 .Help( "Lets the game present without waiting for the display's refresh. "
        "Reduces latency, can show a horizontal seam." );
```

That is the entire call site. What it produced, none of which the caller asked for:

- a 44-base row, hairline-separated from its neighbours;
- the label in Sans 14 `TextLabel`, left-aligned in the label column, on the same
  vertical line as every other label in the sheet;
- a 30×15 switch at the left edge of the control column, on the same vertical line as
  every other control;
- hover, press, keyboard-focus and gamepad-focus painting;
- a `differs` state edge and a reset dot the moment the value leaves its default;
- the help text routed to the Inspector, along with `default: off` and
  `writes: games/1174180.json` — both read from the binding, not typed;
- the row registered for the `Ctrl+K` palette under "Allow Tearing", "tearing",
  and "display.allow_tearing";
- an entry in the sheet header's `differs N` count.

### 3.2 A slider

```cpp
s.Slider( "Sharpness", ui::Cfg( "display.sharpness" ), 0, 20 )
 .Hint( "higher = sharper" )
 .Help( "FSR's RCAS / NIS sharpening pass strength. 0 disables sharpening entirely." )
 .DisabledUnless( IsSharpeningFilter(), "Only fsr, nis and pixel sharpen." );
```

Note what is *absent*: no format string, no min/max label strings, no width, no
`ImGuiSliderFlags`. The format comes from the binding's declared type and unit; the
min/max marks are rendered from the range; the default tick from the binding's default;
`AlwaysClamp` is always on because there is no reason for it not to be.

`.DisabledUnless()` has **no overload without a reason string**. That is the entire
enforcement mechanism for the most common inconsistency in the current code — a control
that greys out and does not say why.

### 3.3 The other kinds, for completeness

```cpp
s.Choice ( "Filter",  ui::Cfg( "display.filter" ),
           { "linear", "nearest", "fsr", "nis", "pixel" } );   // segmented or dropdown — helper decides
s.Choice ( "Stream",  ui::Bind( &g_selectedStream ), streamNames );  // 9 entries → dropdown, automatically

s.Number ( "FPS Limit", ui::Cfg( "display.fps_limit" ), 0, 1000 )
 .Unit( "fps" ).ZeroMeans( "Unlimited" ).Step( 5 );

s.Text   ( "New profile name", &g_profileNameBuf )
 .Validate( ui::Validator::Filename )
 .Placeholder( "e.g. Handheld 40 fps" );

s.Colour ( "Accent Colour", ui::Cfg( "overlay.accent_hue" ) )  // OKLCH hue rail + swatches
 .OnChange( []{ palette::UpdateAccentFamily(); } );

s.Anchor ( "Placement", ui::Cfg( "monitor.anchor_v" ), ui::Cfg( "monitor.anchor_h" ) )
 .Offsets( ui::Cfg( "monitor.margin_v" ), ui::Cfg( "monitor.margin_h" ) );   // the 3×3 grid

s.Readout( "Focused app HDR metadata", HdrMetadataString() ).Live( g_bHdrActive );
s.Meter  ( "GPU busy", g_flGpuBusy, 0.f, 1.f ).Unit( "%" );

s.Actions()                                            // a right-aligned button row
 .Neutral( "copy another game's config", &OnCopyConfig )
 .Danger ( "delete saved config", &OnDeleteConfig )
 .Confirm( "This cannot be undone." );                 // required for Danger; won't compile without
```

`.Confirm()` being required on `.Danger()` is a compile-time constraint, not a
convention: `Danger()` returns a distinct `DangerAction` type whose only method is
`Confirm()`, and only `Confirm()` returns the chainable `ActionRow&`.

### 3.4 Groups and structure

```cpp
void DrawUpscaling( ui::Sheet &s )
{
    s.Group( "Scaling filter" );
    s.Choice( "Filter", ui::Cfg( "display.filter" ), kFilters );
    s.Slider( "Sharpness", ui::Cfg( "display.sharpness" ), 0, 20 ).Hint( "higher = sharper" );
    s.Choice( "Scaler", ui::Cfg( "display.scaler" ), kScalers );

    s.Group( "Presentation" );
    s.Switch( "Allow Tearing", ui::Cfg( "display.allow_tearing" ) );
    s.Switch( "Force Grab Cursor", ui::Cfg( "display.force_grab_cursor" ) );

    s.Note( "Pixel: sharp only when the scale factor is a whole number." );
}
```

`Group()` takes a name and nothing else — no padding, no fill, no border, no flag for
"active". (§SPEC 2.2 removed the active-group variant: "which group is doing something"
is now carried by the per-row state edge, which is per-*setting* and therefore actually
true, where the old group-level accent edge was a hand-set boolean that drifted.)

`Note()` is the only free-text output, capped at two lines, `TextMeta`, full column
width. Anything longer belongs in `.Help()` and therefore in the Inspector.

---

## 4. Bindings — where "one obvious call" actually comes from

```cpp
// UI/Bind.h
namespace gamescope::ui
{
    // A typed handle to a value plus everything the UI needs to know about it.
    template <typename T> struct Binding
    {
        T    Get() const;
        void Set( T v );          // routes through ConfigManager's write queue
        T    Default() const;
        bool Differs() const { return Get() != Default(); }
        const char *DestinationFile() const;   // "global.json" / "games/1174180.json"
        const char *Key() const;               // "display.sharpness"
        const char *Unit() const;
    };

    // Bind to the config schema by key: default, type, range, unit and destination
    // file all come from Config/ConfigSchema.h. This is the preferred form.
    template <typename T = auto> Binding<T> Cfg( const char *pszKey );

    // Bind to a plain variable (transient UI state, a live convar, a scratch buffer).
    // Default and destination are unknown, so the Inspector shows "session only" and
    // the reset affordance is suppressed — the API degrades honestly rather than lying.
    template <typename T> Binding<T> Bind( T *p );
    template <typename T> Binding<T> Bind( T (*get)(), void (*set)( T ) );
}
```

This is the load-bearing idea. Because the binding knows the schema key, **one call
site produces the control, the default tick, the reset action, the `differs` chip, the
provenance line, the palette entry, and the per-game routing badge.** None of that is
typed by the caller and none of it can be forgotten, because forgetting it would mean
not binding the setting at all.

`ui::Cfg` is also the seam where the "which file does this write" logic that PanelConfig
computes ad hoc today (`SessionAppId()` / `IsSessionOverrideActive()`) is computed once.

---

## 5. Adding a whole category

Complete, nothing omitted:

```cpp
// src/Overlay/UI/Categories/CatFrameLimiter.cpp
#include "UI/Sheet.h"

namespace
{
    void Draw( gamescope::ui::Sheet &s )
    {
        s.Group( "Frame limiter" );
        s.Number( "FPS Limit", ui::Cfg( "display.fps_limit" ), 0, 1000 )
         .Unit( "fps" ).ZeroMeans( "Unlimited" ).Step( 5 )
         .Help( "Caps presentation rate. 0 removes the cap. Applied live via the "
                "debug_set_fps_limit convar." );

        s.Switch( "VRR / Adaptive Sync", ui::Cfg( "display.adaptive_sync" ) )
         .DisabledUnless( BackendSupportsVrr(), "The current backend has no VRR path." )
         .Help( "Lets the display refresh follow the game's frame rate." );

        s.Readout( "Refresh range", VrrRangeString() ).Live( g_bVrrActive );
    }

    const gamescope::ui::CategoryDesc kDesc = {
        .pszId    = "display.frame_limiter",
        .pszSection = "DISPLAY",
        .pszTitle = "Frame Limiter",
        .icon     = chrome::Icon::Performance,
        .density  = gamescope::ui::Density::Comfort,
        .pfnDraw  = &Draw,
    };
    const gamescope::ui::AutoRegister kReg{ kDesc };   // static registrar
}
```

Add the file to `meson.build`. That is the whole task: **a new rail item, a new sheet,
palette entries for its settings, breadcrumb, per-category reset, provenance, keyboard
and gamepad reachability, and the responsive ladder — from ~25 lines with no geometry
in them.**

Compare with today: a new panel needs a `PanelId` enum entry, an icon, a dock button,
a `BeginPanelWindow`/`EndPanelWindow` pair, a default size and tile position, a title
string, a draw function that hand-rolls its own tab bar and row layout, and a call added
to `SettingsOverlay.cpp`'s frame body — seven places, six of which are opportunities to
diverge.

---

## 6. The Inspector API — one row grammar, two hosts

```cpp
// A list item can own an Inspector page. The lambda runs ONLY for the selected item.
void DrawShaders( ui::Sheet &s )
{
    auto list = s.List( "Effects" );

    for ( ReshadeEffect &fx : g_effects )
    {
        list.Item( fx.pszName )
            .Switch( ui::Bind( &fx.bEnabled ) )       // inline on/off, in the sheet
            .Meta( fx.bEnabled ? fx.SummaryString() : "off" )
            .Dot( fx.bEnabled )
            .Inspect( [&fx]( ui::Panel &i )
            {
                i.Group( "Parameters" );
                i.Slider( "Strength", ui::Cfg( fx.Key( "strength" ) ), -1.f, 1.f );
                i.Switch( "Protect skin tones", ui::Cfg( fx.Key( "protect_skin_tones" ) ) );

                i.Group( "Adaptation", ui::Depth::Expert );
                i.Slider( "Target brightness", ui::Cfg( fx.Key( "target_luminance" ) ), 0.f, 1.f );
                i.Slider( "Min gain",  ui::Cfg( fx.Key( "min_gain"  ) ), 0.f, 2.f );
                i.Slider( "Max gain",  ui::Cfg( fx.Key( "max_gain"  ) ), 0.f, 4.f );
                i.Slider( "Brighten speed", ui::Cfg( fx.Key( "adapt_up_speed"   ) ), 0.f, 5.f );
                i.Slider( "Darken speed",   ui::Cfg( fx.Key( "adapt_down_speed" ) ), 0.f, 5.f );

                i.Note( "gamescope-ritz.fx · compiled once at startup, "
                        "no recompile stutter on toggle." );
            } );
    }

    s.Note( "Effects are SDR-only for now — disable HDR to use them." );
}
```

`ui::Panel` (the Inspector host) exposes **the same vocabulary as `ui::Sheet`** —
`Group`, `Switch`, `Slider`, `Choice`, `Number`, `Readout`, `Note`, `Actions`. It
differs only in what the helper does with it: rows are two-line, the column split is
gone, and `ui::Depth::Expert` groups start collapsed. A parameter therefore looks and
behaves identically whether it is in the sheet or the inspector, and a setting can be
moved between them by changing one word.

`Inspect()` being a lambda invoked only for the selected item is what keeps this
immediate-mode-cheap: nothing is built for the 200 effects you are not looking at.

---

## 7. What stops inconsistency, mechanically

Six mechanisms, in decreasing order of strength:

1. **No geometry in the API.** No pixels, no colours, no fonts, no `SameLine`. Enforced
   by the type signatures — the strongest form.
2. **Required arguments where omission is the common bug.** `DisabledUnless` requires a
   reason. `Danger` requires `Confirm`. `Slider`/`Number` require a range.
3. **Auto-downgrade instead of caller choice.** `Choice()` measures and picks segmented
   vs dropdown. `Sheet` picks 1/2/3 columns. The Inspector picks drawer vs column. A
   caller cannot pick wrong because a caller does not pick.
4. **Two row heights and two layout modes.** There is no third to reach for.
5. **`ui_lint` — a ConCommand that audits the live UI.** Walks the registry and every
   row drawn in the last frame and reports:
   ```
   ] ui_lint
   ui_lint: 4 findings
     display.output/Force Grab Cursor      no Help() — will show an empty Inspector
     monitor.modules/Font size             bound with Bind(), no default — reset suppressed
     setup.appearance/Background veil      Hint() is 94 chars — cap is 64, use Help()
     setup.profiles/delete                 Danger action, Confirm() text is generic
   ```
   This is how the discipline stays true over time: it is checkable in one command,
   including by an agent, without a screenshot or a human eye.
6. **`ui_snapshot` — dumps the whole registry as text.** Category, row, kind, binding
   key, default, help. Two runs diffed is a complete, reviewable record of a UI change,
   which is exactly what a doc-discipline repo like this one wants attached to a PR.

---

## 8. Sketch of the internals (enough to judge feasibility)

```cpp
// UI/Shell.cpp — the only place that computes geometry.
struct ShellLayout
{
    ImRect  slab, rail, sheet, inspector, header, footer;
    float   scale;            // palette::DisplayScale()
    bool    bRailIcons;       // ladder step 1
    bool    bInspectorDrawer; // ladder step 2
    bool    bInspectorStrip;  // ladder step 3
    int     nSheetColumns;    // 1..3, ladder steps 4 / −1
};
static ShellLayout ComputeLayout( ImVec2 surface );   // pure function, unit-testable
```

```cpp
// UI/Row.cpp — every control funnels through this.
ui::RowCtx ui::Sheet::BeginRow( const char *pszLabel, RowHeight h )
{
    const ShellLayout &L = Layout();
    const float w   = m_columnWidth;
    const float lab = ImMax( 0.44f * w, w - 180.f * L.scale ) - 12.f * L.scale;

    ImGui::PushID( m_nRowIndex++ );
    const ImRect bb = AllocateRow( h );          // ItemSize + ItemAdd
    const bool bSel = IsSelectedRow();

    DrawStateEdge( bb, bSel, m_pCurBinding && m_pCurBinding->Differs() );
    DrawRowBackground( bb, bSel, ImGui::IsItemHovered() );
    DrawLabel( bb, lab, pszLabel, bSel );
    RegisterForPalette( pszLabel, m_pCurBinding );

    return RowCtx{ bb, ImRect( bb.Min.x + lab, bb.Min.y,
                               bb.Max.x - 36.f * L.scale, bb.Max.y ) };
}
```

Every control then draws **only inside `RowCtx::control`**, using the existing
`Widgets.cpp` painters. `widgets::SliderControl()` needs one change: take a rect
instead of using `GetContentRegionAvail()`. Everything else — toggle, checkbox,
segmented, position grid — already draws into a computed box and moves over untouched.

Scroll and clipping: the Sheet is a `BeginChild` with `ImGuiChildFlags_None |
ImGuiWindowFlags_NoBackground`; long lists use `ImGuiListClipper` (legal because of
the two-height rule); the Inspector is its own `BeginChild`. `PushClipRect` is only
needed for the drawer state, where the Inspector paints over the sheet's child.

Animation: one shared helper,

```cpp
float ui::Anim( float &flCur, float flTarget, float flDurationMs );  // lerps vs io.DeltaTime
```

used for exactly three things (§SPEC 3.5) and available nowhere else, so a caller
cannot animate a value.

---

## 9. Migration shape

The API is designed so the port can be incremental rather than a big-bang rewrite:

```cpp
// A category whose draw function is legacy panel code, hosted verbatim inside the
// sheet. Available from day one; deleted category-by-category as each is converted.
s.Escape( []{ PanelDisplay_DrawLegacyBody(); } );
```

`Escape()` pushes the old ImGuiStyle, runs the old code inside the sheet's child, pops.
It looks wrong (that is the point — it is visibly the un-migrated part), it is the only
function in the API that permits arbitrary ImGui, and it is expected to have zero call
sites when the port completes. `ui_lint` counts remaining `Escape()` call sites as
finding severity `migration`.

Cost and sequencing are in `FEASIBILITY.md` §7.
