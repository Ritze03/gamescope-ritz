# HUD layouts (manual per-module placement)

**Status: Phase 3 — the drag editor is live.** This page documents the data model and
store (`src/Config/ConfigSchema.h` / `src/Config/ConfigManager.{h,cpp}`, built in
Phase 0), the render-side change that makes `src/Overlay/FpsDisplay.cpp` actually draw
from a resolved `HudLayout` instead of the old single anchored stack (Phase 1), and the
visual editor that lets a user place a layout by dragging instead of hand-editing
`layouts/<name>.json` (Phase 3, `src/Overlay/UI/HudLayoutEditor.{h,cpp}` — see
"Phase 3: the drag editor" below). Phase 2 (wiring `HudLayoutModule::scale`) has not
landed; every module still renders at its natural content size.

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

## Phase 1: the render change

`FpsDisplay.cpp`'s `DrawReadout()` used to run two passes: measure all four modules
into one shared-width vertical stack, then anchor that single stack to a 3x3 cell
(`FpsDisplaySettings::placement`) with pixel margins and walk a cursor down it. Phase 1
replaces pass 2 (and simplifies pass 1, which no longer needs a shared stack width or
an edge-relative draw order) — pass 1 still measures each module before it has a
position to draw at (`Measure*Module()`, unchanged internally except for where the Fps
module's sub-row toggles come from, below), but each module now resolves its own box
position independently:

- `DrawReadout()` calls `config::ResolveLayoutCached( cfg.layout_name )` fresh on every
  call — a cheap in-memory map lookup (see that function's own comment), safe at this
  render path's cadence, so a layout change is visible on the very next repaint rather
  than added latency on top of whatever cadence already governs when that repaint
  happens (the HUD's existing 500ms `EnsureRepaintTimerThread`, untouched by this
  phase).
- `MeasureModule()` (`FpsDisplay.cpp`) now gates each module's presence on the resolved
  layout's own `enabled` (`HudLayoutModule::enabled` for Cpu/Gpu/Media,
  `HudLayout::fps.placement.enabled` for Fps) instead of
  `FpsDisplaySettings::cpu_enabled`/`gpu_enabled`/`media_enabled`/`fps_enabled`. Those
  `FpsDisplaySettings` fields still exist (see below) but are no longer read by the
  render path.
- `MeasureFpsModule()` gained a second parameter, `const config::HudLayoutFpsModule
  &fpsPlacement`, and reads `fpsPlacement.frametime_enabled`/`graph_enabled`/
  `percentiles_enabled`/`fps_label_enabled` instead of the same-named
  `FpsDisplaySettings` fields — the one place Phase 0's "content toggles belong to the
  layout" decision reaches into a `Measure*Module()` function, since those four are the
  Fps module's own sub-rows, not top-level module presence.
- `ResolveModuleOrigin( const HudLayoutModule &placement, ImVec2 boxSize, ImVec2
  ioDisplay )` (new, `FpsDisplay.cpp`) is the actual position math: it turns
  `placement.origin` (one of `kPlacements`' nine strings) into a fraction pair via the
  existing `ParsePlacement()` (index 0/1/2 on each axis is exactly 0.0/0.5/1.0 of the
  box's own size), computes the normalised point `(x, y) * ioDisplay`, and pulls the
  box's top-left back by `boxSize * fraction` so the *named* corner/edge/center of the
  box — not always its top-left — lands on that point. A `"bottom-right"` module at
  `(0.95, 0.95)` therefore keeps its bottom-right corner near that point instead of
  clipping off past it.
- **Clamping**: the result is clamped to `[0, ioDisplay - boxSize]` on each axis, same
  shape as the old single-stack anchor's own clamp. A legitimate, in-bounds
  `(x, y, origin)` triple already computes into exactly that range, so this never moves
  a correctly-authored placement — it only pulls back a coordinate outside `0..1` or a
  module bigger than the screen, which stays fully visible per the task's own
  "does not clip" requirement. It is not collision avoidance: two modules whose boxes
  overlap are drawn exactly where each says to, in `kModuleOrder`'s fixed z-order
  (Fps, Cpu, Gpu, Media) — overlap is the user's problem by design, unchanged from the
  brief.
- **`scale` (`HudLayoutModule::scale`) is not wired at all this phase**, not even
  partially. The Fps module's Hero number is the only content that already takes an
  explicit size parameter (`cfg.font_size` into `CalcTextSizeA`); Cpu/Gpu/Media's
  `Measure`/`Draw` functions draw every string via `ImGui::PushFont()` at `Fonts.h`'s
  fixed baked-atlas sizes, with no size parameter to scale at all. Scaling just the box
  geometry (or just the Fps number) without scaling everything a module draws would
  produce a backdrop mismatched against its own content — a worse, half-wired result
  than deferring wholesale. Fully wiring `scale` needs threading an explicit size
  through Cpu/Gpu/Media's content functions, left for Phase 2.

## Settings removed this phase

`FpsDisplaySettings::placement`/`margin_vertical`/`margin_horizontal` (the old shared
anchor + margins) and `module_spacing` (the old shared-stack gap) are gone from
`ConfigSchema.h` — manual per-module placement gives none of the four any remaining
meaning, and their settings-panel rows (the old "Placement" group, and the "Module
spacing" slider) are gone with them. `ConfigManager.cpp` no longer reads or writes
those JSON keys (same "an old file's leftover key is just never looked up" precedent
`dock_scale`/`opacity_background` already established).

`FpsDisplaySettings::fps_enabled`/`cpu_enabled`/`gpu_enabled`/`media_enabled`/
`graph_enabled`/`percentiles_enabled`/`frametime_enabled`/`fps_label_enabled` are
**not** removed, even though the render path stopped reading them in Phase 1 (previous
section) and the settings-panel "Modules" rows stopped reading/writing them in Phase 3
(next-but-one section) — `ConfigManager.cpp` still serializes every one of them as part
of the ordinary `Settings`/`FpsDisplaySettings` round-trip, and `tests/test_config.cpp`
has dedicated round-trip coverage asserting exactly that (`"fps_display.fps_enabled
round-trips"` and its siblings) — so removing the fields would break real, still-passing
tests over real, still-persisted JSON keys, not just an internal convention. They are
dead weight for the HUD specifically, not for the config format generally.

## The rename: `system.monitor` → `system.hud`

Same commit: the settings-panel area id/label (`FpsDisplay.cpp`'s
`FpsDisplay_RegisterArea()`) moved from `"system.monitor"`/`"Monitor"` to
`"system.hud"`/`"HUD"`, and every `monitor.*` entry id under it to `hud.*`
(`hud.enabled`, `hud.mod_fps`, `hud.font_size`, `hud.blend_mode`, `hud.color_fps`,
`hud.sampling`, `hud.stats_*`, …). `Overlay/UI/Icons.cpp`'s area-to-glyph map key moved
with it. "Monitor" collided with this project's own "Overlay" naming (the E2 settings
overlay is *Shell*/*Click-UI*, per `superdoc/meta/TERMINOLOGY.md`, which now has a
**HUD** entry). The on-disk JSON key is the separate `"fps_display"`
(`ConfigManager.cpp`), untouched by this rename, so no config migration is needed —
existing users' saved settings load exactly as before under the new UI labels.

## No default layouts

No default layouts ship — `HudLayout` is reachable via `ConfigManager`'s API,
`tests/test_config.cpp`'s direct coverage of it, hand-edited `layouts/<name>.json`
files, and now the Phase 3 editor below. Per the user's locked decision (Phase 0),
there was also no migration: an existing user's HUD position went blank on upgrading
into Phase 1 (`layout_name` was never set, so it resolved to an empty `HudLayout{}`)
and had to be re-placed once this editor existed — see `CHANGELOG.md`'s `[0.3.5]` entry
for the user-facing warning issued at the time.

## Phase 3: the drag editor

`src/Overlay/UI/HudLayoutEditor.{h,cpp}` — a small, self-contained public surface
(`IsActive()` / `Begin()` / `Draw()` / `HandleEscape()`) that lives in its own
`gamescope::ui::hudedit` namespace, called from three places: `FpsDisplay.cpp`'s
`FpsDisplay_RegisterArea()` (`Begin()`, wired to the `hud.edit_layout` "Edit placement"
Action, in the same `system.hud` area, where the old "Placement" settings-panel group
used to sit — not a new area), and `Shell.cpp`'s `Draw()`/`RunKeyboard()` (`Draw()` and
`HandleEscape()` respectively — see "Why it lives in the Shell's context" below for the
reasoning behind that split).

**Why it lives in the Shell's ImGui context, not the HUD's.** `FpsDisplay.cpp`'s own
file-header comment explains why the HUD keeps a fully separate ImGui context, offscreen
texture and submission path from the settings overlay's: lifetime independence (the HUD
must keep rendering every frame regardless of whether the settings panel is even open),
plus isolation from concurrent work on the settings panel. One consequence of that
separation matters here: **the HUD's context receives no pointer input at all** — only
`SettingsOverlay.cpp`'s `DrainInputQueue()` feeds `io.AddMousePosEvent`/
`AddMouseButtonEvent` into the Shell's own context, so `ImGui::IsMouseDown()`/
`GetMousePos()`/etc. only work from inside `Shell::Draw()`'s own call chain. An editor
that needs to drag things has to run there, full stop — hence `HudLayoutEditor.cpp`
draws from the Shell's context and reads the HUD's own geometry through a small,
explicit export (`FpsDisplay_GetModuleRects()`, below) rather than reaching into the
HUD's context at all.

**`Shell::Draw()`'s early return.** Follows `s_bLauncherOnly`'s own precedent (see that
branch's comment in `Shell.cpp`) exactly: while `hudedit::IsActive()`, `Draw()` calls
only `hudedit::Draw()` and returns — no slab, rail, sheet, inspector, drawer, dropdown or
spine. An early return rather than threading an `if` through 200 lines of region drawing,
for the identical reason the launcher's own comment gives: a guard can be forgotten by a
future region; a `return` cannot. One difference from the launcher: this early return
sits *after* `RunKeyboard()` runs (the launcher's sits *before* it), because
`hudedit::HandleEscape()` is a rung in `RunKeyboard()`'s own D26 Esc-precedence chain
(checked right after the armed-destructive-action rung, before the mid-edit-text-field
rung) rather than handled inline in the early-return branch itself — Esc has to reach
`RunKeyboard()` for the editor to be able to cancel itself.

**`FpsDisplay_GetModuleRects()`** (`FpsDisplay.h`/`.cpp`) is the render-side export the
editor's live preview reuses rather than reinventing: it factors `DrawReadout()`'s own
measure-then-resolve pipeline (`MeasureModule()` → `ModulePlacement()` →
`ResolveModuleOrigin()`, all still `static`/file-local) into a callable that takes a
`config::HudLayout` **by value from the caller** — deliberately the editor's own
not-yet-saved working copy, not `config::ResolveLayoutCached(cfg.layout_name)` read
internally — so a drag's effect on the layout is visible in the SAME frame's preview,
before anything is saved. It returns each module's box (`HudModuleRect{x,y,w,h,
bEnabled}`) in **HUD display-pixel space** (the HUD's own `io.DisplaySize`, i.e. its
offscreen texture's resolution, reported via the out-params `pflDisplayW`/`pflDisplayH`)
— safe to call from a *different* ImGui context (the Shell's) because it measures text
against whatever context is current on entry (close enough for a drag preview; it does
not chase pixel-perfect parity with the live HUD's own render) and reads only already-
cached plain numeric state for everything else (last-known smoothed FPS/frametime/
percentiles, and the HUD's own last-known texture size — falling back to the
compositor's current output size, and finally to a hardcoded 1920×1080, if the HUD has
never drawn a frame yet, so the result is never a division-by-zero waiting to happen).

**Two display spaces, one simple ratio.** The Shell's own `io.DisplaySize` and the HUD's
(from `FpsDisplay_GetModuleRects()`) are not assumed to match — `HudLayoutEditor.cpp`
converts every module rect from HUD space into Shell space by a plain per-axis ratio
(`shellSize / hudSize`, applied to both position and size) each frame, and converts a
drag's resulting position back the same way before writing `HudLayoutModule::x`/`y`.
Deliberately **not** `overlay.display_scale` anywhere in this conversion — that scale
governs the Shell's own *widget* sizing (`Overlay/UI/Tokens.h`'s `Scale()`/`Px()`), not
a mapping between two independent ImGui contexts' pixel spaces; the two happen to be
unrelated numbers that could be confused for the same job.

**Dragging.** Mouse-down inside an *enabled* module's box (hit-tested topmost-first,
i.e. `kModuleOrder`'s reverse — Media before Gpu before Cpu before Fps, matching that
z-order's own "later entries draw on top" rule) grabs it; while held, the box's top-left
follows the pointer (via a fixed grab offset recorded at mouse-down, not accumulated
frame-to-frame deltas). Every frame, the module's `x`/`y` are **recomputed from the new
top-left**, never nudged incrementally: `x`/`y` describe where the module's `origin`
corner sits, not the box itself, and — see snapping below — the origin can change
mid-drag, so top-left is the only value that stays meaningful across that change.

**Snapping.** While dragging, each axis independently snaps within 8 physical
(Shell-space) pixels: the box's own three reference points on that axis (min/centre/max)
are tested against the screen's own three anchors (left/centre/right, or top/middle/
bottom) and every *other enabled* module's own three edges/centre on that axis; the
globally closest in-tolerance pair wins, and a thin accent guide line is drawn along
that snapped coordinate for as long as the snap holds. **Only a snap to a *screen*
anchor rewrites `HudLayoutModule::origin`** (e.g. snapping to the screen's right edge
sets the horizontal half of `origin` to `"right"`; the horizontal centre sets it to
`"center"`) — a snap to another module's edge aligns the box visually without rewriting
`origin`, because it says nothing about where on the *screen* this module belongs.
`Why:` `origin`/`x`/`y` together are what makes a placement resolution-independent
(`ResolveModuleOrigin()`'s own comment) — a module dragged flush against the *screen's*
right edge should stay flush against it at a different resolution too, which only holds
if `origin` says `"...-right"` and `x` is close to `1.0`; if snapping only nudged `x`/`y`
without also updating `origin`, a module pinned to the right edge at 1920×1080 would
drift away from that edge at 3840×2160, silently reintroducing the exact
resolution-dependence this whole rework exists to remove. A module-to-module snap has no
equivalent resolution-independence claim to make (two modules "next to each other" is
just as true, or just as false, at any resolution, regardless of `origin`), so it is
left alone.

**Chrome and lifecycle.** A one-line bar (`Save` / `Cancel` / an "Esc to cancel" hint)
drawn as ordinary ImGui buttons inside the editor's own full-surface, no-title-bar,
no-background window — the same window shape `Shell.cpp`'s own launcher branch (D25)
already uses for "one surface covering the whole display, no slab underneath it."
`Begin()` resolves the layout the active session's HUD currently shows
(`FpsDisplay_ActiveLayoutName()` + `ResolveLayoutCached()`) into a working copy plus a
snapshot; `Save()` writes the working copy via `SaveLayout()` + `ReloadLayoutCache()`
(synchronous, not `EnqueueLayoutWrite()` — a discrete click, not a per-frame drag, so
there is no render-thread-stall concern, and the synchronous pair means the very next
frame's HUD immediately reflects the edit rather than waiting on the cache's own
"refreshed automatically at the next `Settings` load" boundary), naming and selecting
`"custom"` first if no layout was named yet (`FpsDisplay_SetActiveLayoutName()`); `Cancel()`
/ `HandleEscape()` simply discard the working copy — nothing was ever persisted
mid-drag, so there is nothing to roll back on disk. `Draw()` calls `force_repaint()`
unconditionally on every frame it runs, since the HUD's own 500ms repaint-timer thread
(`EnsureRepaintTimerThread()`) is far too slow to carry a live drag.

**The "Modules" rewire.** The same change rewired the settings panel's seven
`hud.mod_*` toggle rows (and the Fps module's `label` param) off the now-render-inert
`FpsDisplaySettings` fields (previous section) and onto the resolved *active* layout —
`FpsDisplay.cpp`'s `MutateActiveLayout()` is the one place that reads
`ResolveLayoutCached(cfg.layout_name)`, applies an edit, and saves it back (creating and
selecting `"custom"` first under the identical empty-name rule the editor's own `Save()`
follows, for the identical reason: a toggle flipped before any layout exists needs
somewhere durable to land). Same synchronous `SaveLayout()` + `ReloadLayoutCache()`
choice as the editor's `Save()`, for the same reason (a discrete click, immediate
feedback wanted, `EnqueueLayoutWrite()` alone would leave the very next repaint showing
the old value).
