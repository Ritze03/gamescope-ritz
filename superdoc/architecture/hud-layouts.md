# HUD layouts (manual per-module placement)

**Status: Phase 0 only — schema and persistence, no rendering, no UI.** This page
documents the data model and store `src/Config/ConfigSchema.h` /
`src/Config/ConfigManager.{h,cpp}` now have; later phases wire `src/Overlay/FpsDisplay.cpp`
and an editor UI onto it. Everything below already builds, is unit-tested
(`tests/test_config.cpp`, `[config]` tag), and changes nothing anyone sees on screen yet.

## Why this replaces the old anchored-stack layout

The performance HUD used to auto-stack its four modules (Fps/Cpu/Gpu/Media,
`FpsDisplay.cpp`'s `ModuleKind`) under one anchor + pixel margin
(`FpsDisplaySettings::placement`/`margin_vertical`/`margin_horizontal`). Getting that
auto-layout right across every resolution, UI scale, and combination of enabled modules
is a large surface. The user's call: replace it with **manual per-module placement**,
edited visually — which reduces the remaining problem to per-module sizing, not layout
correctness. See `superdoc/planning/DECISIONS.md` #26 for the recorded decision and its
alternatives.

## The data model

`HudLayoutModule` (`ConfigSchema.h`) is one module's placement:

| field     | range        | meaning                                                          |
|-----------|--------------|-------------------------------------------------------------------|
| `enabled` | bool         | whether this layout shows the module at all                       |
| `x`, `y`  | `0.0..1.0`   | normalised position of `origin` on the display (resolution-independent) |
| `origin`  | one of 9 strings | which of the module's own 9 points sits at `(x, y)`; see below |
| `scale`   | `0.25..4.0`  | per-module, per-layout size multiplier                            |

**`origin`** reuses the exact 9 strings `Overlay/Notifications.cpp`'s `kPlacements`
table and `FpsDisplaySettings::placement`/`OverlaySettings::notification_placement`
already use (`"top-left"` … `"bottom-right"`), rather than inventing a new
representation — a later layout editor can drive it with the same `AnchorGrid` widget
(`Overlay/UI/Controls.cpp`) those fields already use. Without an origin, `x`/`y` alone
could only ever place a module's own top-left corner, which clips any module anchored
toward a screen edge (`origin: "bottom-right"` at `(1.0, 1.0)` needs the module's
bottom-right corner there, not its top-left, to stay fully on screen).

**`scale` bounds (`0.25..4.0`):** floored above zero so a module can never shrink to an
unreadable/zero-size sliver (and nothing that later multiplies by this can divide by
zero); capped at 4× so one mistyped/hand-edited value can't render something absurdly
larger than the display, while still comfortably covering a deliberate
accessibility-sized single module.

`HudLayoutFpsModule` adds the Fps module's own sub-row toggles, since frametime/graph/
percentiles/the unit label are sub-rows drawn *inside* the Fps module
(`FpsDisplay.cpp`'s `MeasureFpsModule()`), not independently placeable — no `x`/`y`/
`origin`/`scale` of their own, just on/off:

```
HudLayoutFpsModule { placement: HudLayoutModule, frametime_enabled, graph_enabled, percentiles_enabled, fps_label_enabled }
```

`HudLayout` is the whole named entity — explicit fields, not a map, because
`ModuleKind` is a fixed, compile-time-known set of four (mirrors `GamescopeSettings`'
own convention; contrast `OverlaySettings::panel_geometry`, a `std::map` because panel
ids really do grow/rename over time):

```
HudLayout { fps: HudLayoutFpsModule, cpu: HudLayoutModule, gpu: HudLayoutModule, media: HudLayoutModule }
```

**A default-constructed `HudLayout{}` has every module's `enabled` false.** That is
already the whole of "an unset/empty layout is a completely valid state that renders
nothing" — no special-casing needed anywhere a layout gets resolved. Per the user's
explicit call, **this fork ships no default layouts** — "Simple"/"Advanced" don't exist
until someone (later, the editor) creates them.

## On-disk shape

`layouts/<name>.json`, parallel to `profiles/<name>.json` and `games/<AppId>.json`
under `~/.config/gamescope-ritz/` (or `$XDG_CONFIG_HOME/gamescope-ritz`):

```jsonc
{
  "schema_version": 1,
  "name": "Advanced",              // stamped by SaveLayout, not part of the HudLayout struct itself (mirrors SaveProfile's own "name" key)
  "modules": {
    "fps":   { "enabled": true, "x": 0.98, "y": 0.02, "origin": "top-right", "scale": 1.0,
               "frametime_enabled": true, "graph_enabled": false, "percentiles_enabled": true, "fps_label_enabled": true },
    "cpu":   { "enabled": true, "x": 0.02, "y": 0.5, "origin": "center-left", "scale": 0.75 },
    "gpu":   { "enabled": false, "x": 0.0, "y": 0.0, "origin": "top-left", "scale": 1.0 },
    "media": { "enabled": true, "x": 0.5, "y": 0.98, "origin": "bottom-center", "scale": 2.0 }
  }
}
```

`schema_version` here is `kCurrentLayoutSchemaVersion` (`ConfigSchema.h`) — **its own
constant, deliberately separate from `kCurrentSchemaVersion`** (the one every
`global.json`/`profiles/*.json`/`games/*.json` file carries). A layout is a different
on-disk shape entirely (never a `Settings`), so a breaking change to one shape has no
reason to force a version bump — and a "newer than this build understands, falling
back" refusal — on files of the other shape. Same parsing discipline as `Settings`:
`nlohmann::json::parse(..., allow_exceptions=false)` plus type-checked field access
throughout (`ConfigManager.cpp`'s `JGet*` helpers), so malformed JSON or an unreadable
`schema_version` degrades to "not found" rather than aborting (this project's
`-fno-exceptions` build means a thrown exception is `std::terminate()`, not something a
`catch` could recover from — the same reason `Settings` parsing avoids throwing paths).

## How a profile/game references a layout — and why it's a reference, not a copy

`FpsDisplaySettings` (part of the ordinary `Settings` shape every layer already has)
gained one new field:

```cpp
std::string layout_name; // empty = no layout referenced = renders nothing
```

It is layered globally/per-profile/per-game **exactly like every other
`fps_display.*` field** — the existing three-layer machinery
(`LoadGlobal`/`LoadPerGameOverride`/`ResolveEffective`) is reused as-is for the field
itself, and `ApplyProfile()` already copies it for free (it's just another member of
the `fps_display` struct `ApplyProfile()` assigns wholesale).

**What is deliberately *not* reused: `ApplyProfile()`'s copy semantics for the
layout's *content*.** `ApplyProfile()` (`DECISIONS.md` #20) is a documented **one-time
copy** — editing a profile afterward never retroactively changes anything that already
applied it. That is the *opposite* of what a named, shared layout needs: "editing
'Advanced' updates every profile/game that names it" is the explicit product
requirement. So:

- `ApplyProfile()` copies the **name string** `layout_name` (like any other
  `fps_display` field — "this profile now points profile-applier at layout X" is a
  one-time choice, same as every other setting a profile carries).
- The layout's **content** (module positions, toggles) is *never* copied into
  `Settings` at all, at any point. It lives only in `layouts/<name>.json` and is
  resolved from the name **at use time**, via `ResolveLayoutCached()` below — never
  baked into a profile/game/global snapshot the way `gamescope`/`reshade`/
  `notifications` are.

This is the one part of this phase most likely to get "fixed" wrongly later by someone
pattern-matching on `ApplyProfile()`'s existing copy behaviour — hence recording it
here and in `DECISIONS.md` #26 explicitly, not just in code comments.

## Resolving a missing/deleted/renamed layout

Every place a layout name can fail to resolve — `layout_name` was never set (empty
string, the default), the file was deleted, the name was mistyped by hand-editing
JSON, or the layout file's own `schema_version` is newer than this build understands —
degrades **identically** to a process-wide empty `HudLayout{}` (nothing enabled, i.e.
"render nothing"). Never a crash, never stale/resurrected data from a previous
successful load under that name. `ResolveLayoutCached()` (`ConfigManager.h`) returns a
`const HudLayout&` to that shared empty instance rather than `std::optional`, so a
render path never has to branch on "found vs. not" — an unresolved name behaves exactly
like an unset one.

`LoadLayout()` itself *does* return `std::optional<HudLayout>` (distinguishing "found,
empty" from "not found" matters for anything that needs to tell the two apart, e.g. a
future editor listing what exists) — `ResolveLayoutCached()` is the layer that
collapses both cases to "nothing to draw."

## The cache, and when it refreshes

A render path resolving `layout_name` at ~60fps must never hit disk per frame — the
same rule this file's header comment already states for `ResolveEffective()` itself.
`ResolveLayoutCached(name)` is an in-memory `std::map<std::string, HudLayout>` lookup,
populated by `ReloadLayoutCache()` reading every `layouts/*.json` once.

`ReloadLayoutCache()` runs automatically at the same "a `Settings` got (re)loaded"
boundary the rest of this file already treats as the non-per-frame checkpoint:
`LoadGlobal()` calls it directly, and `ResolveEffective()`'s per-game-override branch
(which returns without ever calling `LoadGlobal()`) makes the same call itself, so
both resolution paths refresh it. Ordinary startup / profile-apply / override-toggle
flows therefore need nothing extra. A caller that edits a layout file out from under an
already-running process (a test, or a future editor immediately after
`FlushPendingWrites()`) can call `ReloadLayoutCache()` directly.

## Reused verbatim vs. built parallel

Reused directly, unchanged: `WriteFileAtomic` (write-temp-then-rename), the
`SweepStaleTempFiles` orphan cleanup, the background `ConfigWriter` singleton/thread,
the `JGet*` type-checked JSON accessors, and the sanitisation rule itself (see
`SanitizeNameForPathComponent` — `SanitizeProfileName`/`SanitizeLayoutName` are two
public entry points over one shared body, deliberately not aliases of each other since
profile names and layout names are independent namespaces).

Parallel, not reused: `ApplyProfile()`'s copy path (explained above), and the
directory/version namespace (`LayoutsDir()`/`LayoutPath()`/
`kCurrentLayoutSchemaVersion`, distinct from `ProfilesDir()`/`ProfilePath()`/
`kCurrentSchemaVersion`).

## API surface (`ConfigManager.h`)

```
LayoutsDir() / LayoutPath(name)
SanitizeLayoutName(name) -> optional<string>       // same rules as SanitizeProfileName
LoadLayout(sanitizedName) -> optional<HudLayout>    // nullopt: missing, malformed, or too-new schema_version
SaveLayout(sanitizedName, layout) -> bool
DeleteLayout(sanitizedName) -> bool                 // the ONLY function that deletes a layout file; missing file is still success
ListLayouts() -> vector<string>                     // sorted, mirrors ListProfiles/ListGameIds
EnqueueLayoutWrite(name, layout)                    // background-thread write, for a future editor's live edits
ResolveLayoutCached(name) -> const HudLayout&        // in-memory, safe at ~60fps; see above
ReloadLayoutCache()                                  // called automatically by LoadGlobal()/ResolveEffective()
```

## What Phase 0 deliberately does not touch

`src/Overlay/FpsDisplay.cpp` is unchanged. It still reads
`FpsDisplaySettings::cpu_enabled`/`gpu_enabled`/`media_enabled`/`fps_enabled`/
`graph_enabled`/`percentiles_enabled`/`frametime_enabled`/`fps_label_enabled`/
`placement`/`margin_vertical`/`margin_horizontal` exactly as before — none of that
rendering behaviour has changed yet. Those fields stay on `FpsDisplaySettings` for now
even though their *conceptual* home is now a layout's per-module `enabled` and the Fps
module's own sub-row toggles (the "content toggles belong to the layout" decision) —
removing them this phase would have broken `FpsDisplay.cpp`, which is out of this
phase's scope and owned by a later, parallel phase. A later phase switches
`FpsDisplay.cpp` to read a `ResolveLayoutCached()` result instead and retires these
copies from `FpsDisplaySettings`.

No default layouts ship, and no UI exists yet to create one — `HudLayout` is reachable
today only via `ConfigManager`'s API and `tests/test_config.cpp`'s direct coverage of
it.
