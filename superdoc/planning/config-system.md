# Config System — Global + Per-Game Layering for the Settings Overlay

Scouting doc only. No code, no files under `~/.config/`. Grep-verified against this
repo's current `master` (fcc1341) unless marked `[web]` for external research (URLs and
check-date at the bottom of each such note).

## Verdict

The two hard constraints shape everything else: (1) gamescope's own process has **no
existing `getenv("AppId")`** anywhere — the only Steam-appid mechanism in-tree is a
per-window, post-startup `/proc` scrape, so `GS_RITZ_APPID` must be a genuinely new,
self-sufficient env-var read, and it *is* early enough (top of `main()`) precisely
because it doesn't depend on that scrape. (2) gamescope already has a live,
runtime-tunable-value system (`ConVar<T>`) with **no persistence and no cross-thread
synchronization** — the new config system should sit *alongside* it as the thing that
seeds ConVar/global defaults at startup and on profile-apply, not fight it or replace it
wholesale. JSON is the right on-disk format and nothing in the repo currently parses it;
`nlohmann::json` vendored the same way `stb`/`glm` already are (`wrap-git` +
`packagefiles/nlohmann_json/meson.build`) is the natural fit — Lua/`sol2` was
considered and rejected as the config backend (see Q4). Layering semantics (Q6) and the
profile-extensionless-path (Q7) are flagged, not resolved, pending user confirmation.

---

## 1. Environment variables and startup ordering

**Grep-verified: there is no `getenv("AppId")` and no `getenv("SteamAppId")` anywhere in
gamescope's own compositor source (`src/`, `layer/` excluded — see below).** The only
"AppId" handling gamescope itself does is:

- `get_appid_from_pid()` (`src/steamcompmgr.cpp:5285`) — walks up `/proc/<pid>/stat`
  parent links looking for a process named `reaper`, then parses that process's
  `/proc/<pid>/cmdline` for a literal `AppId=<uint>` **argument** (not an env var) inside
  Steam's `reaper AppId=<n> SteamLaunch ... -- <game>` wrapper invocation
  (`:5348`, `sscanf( ..., "AppId=%u", &unAppId )`, gated on the sibling `SteamLaunch`
  argument also being present, `:5343`-`5352`).
- This is called **per-window**, asynchronously, long after startup: once from
  `wlserver.cpp:1951` (`window->appID = get_appid_from_pid( nPid )`, inside
  `waylandy_type_surface_new`, a `wl_client` surface-creation callback — runs on the
  **main thread**, `wlserver_run()`'s dispatch context) and once from
  `steamcompmgr.cpp:5465` (X11 window creation, **steamcompmgr thread**). Different
  windows can carry different `appID`s; there is no single "the app id for this
  gamescope instance" value anywhere in the process.
- The **only** literal env-var read for a Steam app id in this repo is
  `getenv("SteamAppId")` at `layer/VkLayer_FROG_gamescope_wsi.cpp:87` — but that is a
  **Vulkan ICD layer loaded inside the game's own process** (a separate binary from
  gamescope), used there for that layer's own default-flag heuristics
  (documented in `superdoc/features/vk-wsi-layer.md:35`). It is not read by, and not
  directly accessible to, gamescope's own process.

**`[web]` Steam does set `SteamAppId` (and `SteamGameId`) as real env vars on a launched
game's process** — corroborated by community documentation (steamtinkerlaunch's
ENV-Variables wiki) and matches the one confirmed in-tree read above. Whether that env
var is also inherited by **gamescope's own process** (as opposed to only the game
process gamescope spawns as a child) depends on exactly how the Steam client invokes
gamescope for a given title, which this scout pass could not verify from source or from
the web research done. **This is the load-bearing correction to the brief:** the "existing
env var named `AppId`" almost certainly means `SteamAppId`, not literally `AppId` —
flagged in Open Questions.

**Ordering answer:** `main()` (`src/main.cpp:723`) starts with `g_argc = argc;
g_argv = argv;` then a `setenv()` call and CLI option parsing — no subsystem is
initialized before this point. A `getenv("GS_RITZ_APPID")` (and, if confirmed live,
`getenv("SteamAppId")`) call inserted at the very top of `main()` is **as early as
architecturally possible** — strictly before `wlserver` init, backend selection,
`steamCompMgrThread` spawn (`:1101`), and `wlserver_run()` (`:1111`). **This means
`GS_RITZ_APPID`, being a plain env var on gamescope's own process, is known early enough
to load a per-game config before anything it configures initializes** — this is
fundamentally different from (and much earlier than) the existing per-window
`get_appid_from_pid()` scrape, which is *not* known at startup and *cannot* be, since it
depends on a client window existing first. Recommendation: identity resolution is a
**pure env-var read at the top of `main()`**, independent of and not blocked by the
existing per-window Steam-appid machinery.

`[web]` Sources checked 2026-08-21:
- steamtinkerlaunch ENV-Variables wiki — https://github.com/sonic2kk/steamtinkerlaunch/wiki/ENV-Variables
- Community Steamworks integration notes on `SteamAppId`/`SteamGameId`/`steam_appid.txt` — https://partner.steamgames.com/doc/sdk/api (official docs page; specific env-var behavior for non-Steam launches confirmed only indirectly, not from a primary Valve statement)

## 2. No app id available (standalone / non-Steam launch)

Concretely: `GS_RITZ_APPID` unset **and** (whatever Steam-appid signal is ultimately
wired in, per Q1's open question) also unset. Recommended behaviour:

- **Global config only.** No `games/<AppId>.json` is looked up, read, or created —
  consistent with the "never create a game file the user didn't opt into" rule from Q3
  below, generalized: there's no identity to key a file on in the first place.
- **Per-game UI tab:** shown but in a disabled/informational state — e.g. "No game
  identity detected for this session (`GS_RITZ_APPID` not set, no Steam app id found).
  Per-game overrides are unavailable; showing Global config." The "Override Global
  Config" checkbox should be disabled/hidden rather than interactable, since there is no
  file path to write it to.
- **Mid-session appearance of an app id is out of scope for v1** given Q1's finding that
  the per-window Steam-appid scrape happens well after startup and per-window (not
  per-process) — treating a late-arriving appid as "now let's hot-swap in a per-game
  config" is a bigger design (config reload, re-resolving already-applied settings) than
  this doc assumes. Recommend: only `GS_RITZ_APPID` (read once, at startup, before
  anything else) selects a per-game config for v1; the Steam-discovered per-window appid
  is not wired into config resolution at all until a follow-up decision. Flagged in Open
  Questions.

## 3. Relationship to the existing ConVar system

`ConVar<T>` (`src/convar.h:127`) is a named, typed, runtime-settable value with an
optional change callback, auto-registered into the Lua scripting layer when
`HAVE_SCRIPTING` is on (`src/convar_script.cpp:34`/`:40`). Critically, **it has no
built-in persistence** (no save/load, no "archive" flag anywhet in the class) and **no
thread-safety** — `m_Value` is a plain `T` field (`src/convar.h`, `SetValue` just
assigns), confirmed already risky in practice by the sibling scout doc
(`superdoc/planning/runtime-knobs-and-fps.md` §A3): `gamescope_private.execute` writes
ConVars from the main thread while `paint_all()` reads some of them per-frame on the
steamcompmgr thread with no lock at the read site
(`frameInfo.allowVRR = cv_adaptive_sync;`, `steamcompmgr.cpp:2620`) — a pre-existing,
already-shipped, unsynchronized cross-thread pattern.

Three options considered:

1. **Config writes *through* ConVars exclusively** (config system is just a
   load-JSON-then-`SetValue()`-every-ConVar-it-knows-about layer, no independent state).
   Rejected as the sole mechanism: many settings the user wants
   (FPS-display font size/backdrop, ReShade per-effect parameter sets, LSFG-VK settings,
   the `Lossless.dll` path) have **no existing ConVar** — they're new state, not
   overrides of something that already exists. Also, not every gamescope-tunable value
   the config touches is even a `ConVar` today: filter/scaler/sharpness are **plain
   globals** (`g_wantedUpscaleFilter` etc., confirmed by the sibling scout doc §A2-A3),
   not ConVars — a "config writes through ConVars only" design silently can't reach
   those without first turning them into ConVars itself (a separate, real piece of work,
   not assume-away-able).
2. **Config sits entirely alongside ConVars, in its own struct(s), never touching
   ConVars at all** — clean separation but means every existing ConVar the config
   overlaps with (e.g. `cv_adaptive_sync`, `cv_hdr_enabled`, `cv_tearing_enabled`) now
   has *two* sources of truth that can silently disagree (ConVar says one thing, config
   says another, whichever was written last or read last wins by accident).
3. **Hybrid, recommended: config seeds/writes ConVars where a matching ConVar already
   exists (VRR, HDR, tearing — confirmed in sibling doc §A5), and owns independent
   struct state for everything else (ReShade effect+parameter sets, FPS-display
   styling, LSFG-VK, General/`Lossless.dll` path, and gamescope options that are
   currently plain globals like filter/scaler/sharpness).** Concretely: on config
   load/profile-apply, for each setting that maps to a real `ConVar<T>`, call
   `cv.SetValue(json_value)` (cheap, already how `gamescope_private.execute` does it);
   for plain-global gamescope options (filter/scaler/sharpness), write through the
   **existing X11-property mechanism** the sibling doc identifies as the
   already-serialized, already-tested live-write path (§A2/A3), rather than reaching
   into `g_wanted*` directly from a new thread; for everything with no existing
   backing variable, the config system's own struct *is* the source of truth and the
   consuming code (ReShade pipeline setup, FPS overlay draw, LSFG-VK init) reads it
   directly. This avoids inventing a second "ConVar-like" system for values gamescope
   already exposes at runtime, avoids fighting the plain-global path the sibling scout
   found for filter/sharpness, and keeps genuinely-new settings out of the ConVar
   registry (which is a global namespace with Lua-console/`gamescopectl` surface area —
   not obviously desirable for every UI slider, e.g. per-ReShade-effect parameter
   blobs).

**Mapping — user's roadmap settings vs. existing ConVars (grep-verified, not
exhaustive — other scouts own the authoritative parameter lists):**

| Setting area | Existing ConVar / global? | Evidence |
|---|---|---|
| VRR / adaptive sync | `ConVar<bool> cv_adaptive_sync` | `steamcompmgr.cpp:295`(approx, per sibling doc §A5), read `:2620` |
| HDR toggle | `ConVar<bool> cv_hdr_enabled` | sibling doc §A5 |
| Tearing | `ConVar<bool> cv_tearing_enabled` | sibling doc §A5, liveness "likely" not fully traced |
| Scaling filter / scaler / sharpness | **Plain globals**, not ConVars (`g_wantedUpscaleFilter`, `g_wantedUpscaleScaler`, `g_upscaleFilterSharpness`) | `src/main.hpp:35-66`, sibling doc §A2/A4 |
| FPS limiter | Plain `int g_nSteamCompMgrTargetFPS` | sibling doc §A5 |
| ReShade effect selection + uniform values | **No ConVar** — driven by `reshade_effect_manager_set_effect()`/`_set_uniform_variable()` free functions (`src/reshade_effect_manager.hpp:110`) via the `gamescope_reshade` Wayland protocol, not a ConVar | `superdoc/features/reshade-effects.md` |
| FPS-display styling (font size, backdrop, blending) | **No existing state at all** — this UI doesn't exist yet | new, sibling doc §B5 |
| Audio volume | **Not found in this scout's pass** — not grep-verified, likely belongs to another scout's scope (mangoapp/audio not covered by scripting-convars.md or control-ipc.md) | TBD-from-sibling-scouts |
| LSFG-VK settings | **Not found in this repo at all** — no `LSFG`/`lossless` symbol under `src/` in this pass | TBD-from-sibling-scouts; likely entirely new state, no ConVar to map to |
| `Lossless.dll` path (General) | **New** — no existing config/settings storage for arbitrary file paths found | new |

## 4. JSON handling — what's available

**Nothing.** `grep`-verified: no `nlohmann`, `jsoncpp`, `rapidjson`, or `cJSON` in
`meson.build`'s `dependency()` calls, and no matching `.wrap` file in `subprojects/`
(current contents: `glm.wrap`, `libdisplay-info`, `libliftoff`, `openvr`, `stb.wrap`,
`vkroots`, `wlroots`, plus `packagefiles/`). `thirdparty/` has `sol` (Lua, for the
scripting console) and `SPIRV-Headers` — no JSON library either place.

**Lua/`sol2` as the config backend — considered and rejected.** The Lua scripting
system (`src/Script/Script.h:37`, `CScriptManager`) already has a `Config.Base` /
`Config.KnownDisplays` Lua-table convention (`Script.cpp:103`-`107`) and an established
user-config directory precedent: `cv_script_use_user_scripts`
(`Script.cpp:18`) gates loading `$XDG_CONFIG_HOME/gamescope-ritz/scripts` (falling back to
the plain unnamespaced `$XDG_CONFIG_HOME/gamescope/scripts` for a pre-existing setup) via
`gamescope::GetConfigDir()` (`src/Utils/DirHelpers.h:10`/`.cpp:31`, itself
`XDG_CONFIG_HOME` with a `$HOME/.config` fallback). This is real precedent for "gamescope
already loads user files from a config directory," but it's the wrong shape for this
feature: Lua config files are **arbitrary executable code**, not data — hostile or
malformed to a script-injection concern in a way a data format isn't, hard to
round-trip losslessly from a UI ("write back exactly this Lua file after a slider drag"
is much harder than "write back exactly this JSON"), and not naturally diffable/portable
the way the user's "copy another game's config" and "apply a profile" config-manager
features want. `Config.KnownDisplays` is an EDID-quirk **input** table Lua scripts
populate for gamescope to read, not a user-editable settings file with a UI in front of
it — different problem shape.

**Recommendation: vendor `nlohmann::json`** (header-only, MIT) via the exact pattern
already used for `stb`/`glm` — `subprojects/nlohmann_json.wrap` (`[wrap-git]` pinned to
a tagged release) with, if upstream's own `meson.build` isn't used directly, a
`subprojects/packagefiles/nlohmann_json/meson.build` overlay exposing a
`declare_dependency(include_directories: ...)`. `[web]` nlohmann/json ships its own
`meson.build` and is listed on Meson WrapDB (`meson wrap install nlohmann_json`), so the
packagefile overlay may not even be necessary — a plain `wrap-git`/WrapDB entry
consumed via `dependency('nlohmann_json', fallback: ...)` likely suffices, matching the
`openvr_dep`-style `dependency()`-with-fallback pattern already in `meson.build:59`-`70`.
Being header-only avoids adding a new compiled artifact to the build, consistent with
`stb`/`glm` also being header-only vendored deps in this repo.

`[web]` Sources checked 2026-08-21:
- nlohmann/json Meson integration docs — https://json.nlohmann.me/integration/package_managers/
- nlohmann/json `meson.build` in upstream repo — https://github.com/nlohmann/json/blob/develop/meson.build

## 5. Threading

Per `superdoc/architecture/overview.md` and confirmed directly here: **main thread**
runs `wlserver_run()` (`src/wlserver.cpp:2254`, dispatch for Wayland/XDG surface
events — this is also where the per-window `get_appid_from_pid()` call for XDG windows
happens, `wlserver.cpp:1951`); **steamcompmgr thread**
(`steamCompMgrThreadRun` → `steamcompmgr_main`, spawned `main.cpp:1101`) owns window
management, X11 event handling, and the vblank-paced `paint_all()` → `vulkan_composite()`
→ `Present()` path (`steamcompmgr.cpp:2564`, called at `:9488`). The sibling ImGui
feasibility scout (`superdoc/planning/imgui-overlay-feasibility.md` §3) independently
recommends the settings overlay itself render **on the steamcompmgr thread**, inside or
immediately before `paint_all()` — this is the load-bearing fact for config threading:

- **Config lives as an in-memory struct (or a small handful of them: global, active
  per-game/profile-resolved), owned by the steamcompmgr thread**, mirroring how
  filter/scaler/sharpness plain globals and ConVar reads already work per-frame with no
  lock (§3 above; sibling doc §A3). If the ImGui UI itself runs on that same thread
  (per the sibling doc's recommendation), a UI-driven edit (slider drag) can write
  directly into that struct with **no synchronization needed at all** — same thread
  writes, same thread reads, exactly the existing (if implicit) safety model the
  X11-property/ConVar pattern already relies on.
- **Per-frame reads must stay cheap**: `paint_all()` reading a plain struct field (a
  `float`/`int`/`enum`) each frame is free — this is exactly what it already does for
  `cv_adaptive_sync`/`g_upscaleFilterSharpness`. No lock, no allocation, no string
  parsing on the hot path — any string/path fields (e.g. ReShade effect path,
  `Lossless.dll` path) should be resolved into whatever cheap handle the consumer
  actually needs (an already-open file/pipeline handle) at write-time, not re-parsed
  every frame.
- **Disk I/O must never happen on the steamcompmgr thread synchronously** — vblank-paced,
  and a single `fsync()`/rename() stall would show up as a dropped/late frame. Writes
  (see §7) should be queued (a small lock-protected `std::string` blob, or a
  single-producer-single-consumer queue) and flushed by either the main thread (which is
  otherwise mostly idle in `wl_display` dispatch waits) or a small dedicated one-shot
  worker thread, never inline in `paint_all()`.
- If the UI ends up **not** rendering on the steamcompmgr thread (see the sibling doc's
  Open Question 2 — separate-process overlay is a live alternative), then config writes
  from that other context must go through an explicit, serialized hand-off — mirroring
  either the X11-property mechanism (already the tested live-write path for
  filter/scaler/sharpness) or a dedicated lock-protected "pending config" double-buffer
  the steamcompmgr thread swaps in once per frame, **not** a raw cross-thread struct
  write, since that inherits exactly the unsynchronized-ConVar risk flagged in §3.

---

## Proposed schema

Schema version field included. Every numeric range/default the roadmap needs
(ReShade per-effect parameter ranges, FPS-display exact values, audio volume curve,
LSFG-VK parameters) is **not this scout's to invent** — marked `TBD-from-sibling-scout`
below. FPS-display fields are taken directly from the sibling scout's own proposal
(`runtime-knobs-and-fps.md` §B5, itself marked as "proposals ... not verified", so
still provisional there too) since that's the one place a concrete shape already
exists; everything else is structural only.

```jsonc
// ~/.config/gamescope-ritz/global.json
{
  "schema_version": 1,          // integer, monotonically increasing. See "Schema migrations" below.
  "general": {
    "lossless_dll_path": "",     // string, absolute path. TBD-from-sibling-scout: validation rules (must exist? extension check?)
    // additional General-section fields: TBD — "whatever else proves necessary" per brief,
    // no candidates found in this scout's source pass beyond the LSFG-VK dependency.
  },
  "gamescope": {
    "filter": "LINEAR",          // enum: LINEAR | NEAREST | FSR | NIS | PIXEL — src/main.hpp:35-42
    "scaler": "AUTO",            // enum: AUTO | INTEGER | FIT | FILL | STRETCH — src/main.hpp:53-59
    "sharpness": 2,              // int 0..20, raw/unnormalized — see Risks re: FSR/NIS inversion (runtime-knobs-and-fps.md §A4)
    "vrr_enabled": false,        // bool, maps to cv_adaptive_sync
    "hdr_enabled": false,        // bool, maps to cv_hdr_enabled
    "tearing_enabled": false     // bool, maps to cv_tearing_enabled (liveness not fully confirmed, see sibling doc)
  },
  "fps_display": {
    "enabled": false,
    "font_size": 18.0,           // TBD-from-sibling-scout: final range/default; 10-48 proposed in runtime-knobs-and-fps.md §B5
    "backdrop_enabled": true,
    "backdrop_opacity": 0.5,
    "backdrop_rounding": 4.0,
    "backdrop_padding": 6.0,
    "blend_mode": "alpha",       // enum: alpha | additive
    "text_opacity": 1.0
  },
  "reshade": {
    "enabled": false,
    "effect_path": "",           // string, path to .fx file — reshade_effect_manager_set_effect()
    // Per the brief: adaptive brightness, vibrancy, sharpness are named effect *parameters*,
    // not fixed schema fields — ReShade effects are open-ended (arbitrary .fx files,
    // reshade-effects.md), so parameters are a name->value map, not hardcoded keys.
    "uniforms": {
      // "<uniform_name>": <value>   -- TBD-from-sibling-scout: which uniforms/effects ship
      // by default, their types/ranges. Schema supports arbitrary key/value pairs by design
      // since reshade_effect_manager_set_uniform_variable() takes an arbitrary name.
    }
  },
  "lsfg_vk": {
    // TBD-from-sibling-scout entirely. Grep-verified: no LSFG symbol found anywhere
    // under src/ in this pass. Structure is a placeholder pending that scout's findings.
    "enabled": false
  },
  "audio": {
    "volume": 1.0                // TBD-from-sibling-scout: 0..1 vs 0..100, curve (linear vs log), no audio
                                  // subsystem found in scripting-convars.md/control-ipc.md by this scout.
  }
}
```

```jsonc
// ~/.config/gamescope-ritz/profiles/<ProfileName>   (see §7 — extension flagged)
{
  "schema_version": 1,
  "name": "FPS",                 // redundant with filename by design — makes a copied/renamed
                                  // file still self-describing, and gives the UI a display name
                                  // independent of filesystem-safe naming (see §7 sanitisation).
  // Same shape as the settings blocks above (gamescope/fps_display/reshade/lsfg_vk/audio) —
  // NOT the "general" block, which is process-level (Lossless.dll path etc.), not a
  // per-game/profile concern. A profile is a bundle of the per-game-relevant settings only.
  "gamescope": { /* ... */ },
  "fps_display": { /* ... */ },
  "reshade": { /* ... */ },
  "lsfg_vk": { /* ... */ },
  "audio": { /* ... */ }
}
```

```jsonc
// ~/.config/gamescope-ritz/games/<AppId>.json  (only ever created once "Override Global Config" is turned on — see §6/§7)
{
  "schema_version": 1,
  "override_global": true,       // mirrors the UI checkbox state; see Layering semantics for what this actually gates
  "profile": null,                // string | null — name of an applied profile, if the "live reference" reading of Q6 is chosen; omitted/unused if "one-time copy" is chosen instead
  "gamescope": { /* ... */ },
  "fps_display": { /* ... */ },
  "reshade": { /* ... */ },
  "lsfg_vk": { /* ... */ },
  "audio": { /* ... */ }
}
```

### Schema migrations

- `schema_version` is a flat integer, bumped on any breaking rename/removal/type-change
  of a field (additive-only changes — a new optional field with a sensible default —
  don't strictly need a bump, but bumping anyway costs nothing and keeps the history
  legible).
- On load: if `schema_version` is older than current, run an ordered chain of small
  migration functions (`migrate_1_to_2(json&)`, `migrate_2_to_3(json&)`, ...), each doing
  one step's worth of rename/reshape, then re-save. If `schema_version` is *newer* than
  current (a config written by a future gamescope-ritz build, opened by an older one),
  fail loudly rather than guessing — do not attempt to load or silently drop unknown
  fields as "future-proofing", since that risks quietly discarding a user's settings on
  a downgrade. Missing `schema_version` entirely (hand-written file) is treated as
  version 0 / oldest-known and run through the full migration chain, or rejected per
  the malformed-file policy in §7 — this is a judgment call, not resolved here.
- This is cheap to design now (a version int + a migration-function list) and expensive
  to retrofit onto files already in the wild without one — including it from the first
  shipped schema is a one-line-of-cost, high-value decision, not TBD.

---

## Layering semantics

**Resolution order, most-specific wins:** `per-game file` → `profile` (if the per-game
file references one) → `global file` → `hardcoded defaults`. Per-key fallback is
recommended, **not** whole-file replacement (see the flagged decision below), meaning:
a per-game file only needs to contain the keys the user actually changed for that game;
anything absent falls through to the profile (if any) then to global then to the
compiled-in default.

**Two explicit decisions flagged for user confirmation, because each has a materially
different UI and file format:**

1. **Is "Override Global Config" all-or-nothing or per-setting?**
   - *Reading A — all-or-nothing:* the checkbox being on means the entire
     `games/<AppId>.json` is authoritative and the global file is not consulted at all
     for that game; being off means the file (if it somehow exists) is ignored entirely
     and every value comes from global. Simple mental model, simple code (`override_global
     ? gameConfig : globalConfig`, no per-key merge), but means turning the checkbox on
     with an otherwise-empty game file would reset *everything* to schema defaults
     instead of "everything follows global until you touch something" — probably not
     what a user expects from a checkbox they'd naturally read as "let me tweak a couple
     of things for this game."
   - *Reading B — per-setting fallback (recommended):* the checkbox only gates *whether
     the per-game file is consulted at all* (matching §2's "don't even create the file if
     it's off"); once on, each individual key in the per-game file that's present
     overrides global, and any key **absent** from the per-game file still falls through
     to global. This matches the schema above (games file only needs the deltas) and
     matches the "just pick a profile and be done" UX the brief describes — a game file
     can be nearly empty (just `profile: "FPS"`) and still fully resolve.
   - **Recommendation: Reading B (per-setting).** It matches the brief's own framing
     ("enables the per-game config for that game" reads as "start deviating from
     global," not "replace global wholesale") and is the only reading compatible with
     the profile-apply UX the brief explicitly wants. **Flagged for user confirmation
     regardless — this is exactly the kind of two-readings-diverge-completely decision
     the brief calls out, and it changes both the JSON shape above and how much UI work
     "resolve effective settings for this game" is.**

2. **Where does a profile sit in the resolution chain — one-time copy, or live
   reference?**
   - *One-time copy:* "apply profile FPS" copies FPS's values into the per-game file at
     that moment; the per-game file has no memory of which profile it came from; editing
     the profile later doesn't affect games that already applied it.
   - *Live reference (recommended):* the per-game file stores `"profile": "FPS"` (as
     modeled in the schema above) and resolves through it at read time — editing the
     "FPS" profile later updates every game referencing it, matching the brief's own
     example almost exactly ("for a game just pick the profile and be done" implies an
     ongoing relationship, not a one-off stamp) and matching how the "per-setting
     fallback" reading above already needs a multi-level resolution chain. Con: a user
     who tweaks one setting for one game *after* picking a profile now has a three-way
     resolution (game-key present? → profile? → global?) to reason about, and deleting a
     referenced profile needs defined behaviour (fall through to global, per this doc's
     recommendation, with a UI warning).
   - **Recommendation: live reference**, for the reasons above, but this is a real
     product decision (copy = predictable/frozen, reference = convenient/dynamic) and
     genuinely could go either way depending on how users actually use it —
     **flagged for user confirmation.**

---

## 7. File lifecycle and safety — see also §5 (threading) for why writes can't be inline

- **When files are written:** not on every slider drag. Recommend a debounced/batched
  save (e.g. mark the in-memory config dirty on any edit, flush at most once per N
  hundred ms of inactivity, and always on overlay-close/app-exit) rather than either
  extreme (every keystroke, or only on an explicit Save button) — sliders in particular
  make "every change" prohibitively frequent, and "only on close" risks losing edits on
  a crash. An explicit Save button can still exist as a "flush now" affordance without
  being the *only* write trigger.
- **Atomic writes:** write to a temp file in the same directory
  (`<target>.json.tmp-<pid>` or similar) and `rename()` over the real path — `rename()`
  within the same filesystem is atomic on Linux, so a crash mid-write leaves either the
  old file intact or the new one fully written, never a half-written JSON. This is a
  small, self-contained piece of I/O code, not something the repo already has a
  precedent for (no existing gamescope code writes JSON/config files today), so it's new
  but standard.
- **Malformed/hand-edited JSON:** recommend **fail loudly for that one layer, fall back
  to defaults for the resolved value, and surface it in the UI** — not a silent
  best-effort partial-parse. Concretely: if `games/<AppId>.json` fails to parse, treat it
  as if `override_global` were off for this session (fall through fully to
  global+profile+defaults) and show a visible warning ("per-game config for <AppId>
  failed to load: <error>; using global settings") rather than either crashing gamescope
  or silently discarding the user's edits with no indication anything went wrong. Global
  config failing to parse is more severe (it's the last fallback before hardcoded
  defaults) — same treatment, fall back to hardcoded defaults, but the warning should be
  more prominent since there's nowhere further to fall back to.
- **File permissions:** standard user-file permissions (`0644` for files, `0755` for the
  created directories) are sufficient — this is user-owned config in `$HOME`, not a
  secrets file; no evidence in this codebase of any existing config-adjacent file using
  tighter permissions (`GetConfigDir()`'s consumer, the Lua user-scripts directory, does
  not set special permissions either, per `Script.cpp:132`-`135`).
- **`profiles/<ProfileName>` has no `.json` extension in the brief — flagged as a likely
  oversight.** Every other path the brief gives (`global.json`, `games/<AppId>.json`)
  has one; a bare `profiles/<ProfileName>` breaks "open this folder and every file is
  obviously JSON" for no apparent reason, complicates any file-picker/editor
  association, and is inconsistent with itself. Recommend `profiles/<ProfileName>.json`
  unless the user specifically wants extensionless profile files for some reason not
  stated in the brief. **Flagged for confirmation, not silently corrected.**
- **Profile names come from user input and must be sanitised.** `<ProfileName>` becomes
  a path component directly — an unsanitised name like `../../etc/passwd` or containing
  `/` would escape `profiles/` entirely. Recommend: reject/strip anything that isn't
  `[A-Za-z0-9 _-]` (or a similarly narrow allowlist), reject empty names and names that
  are only `.`/`..` after trimming, and cap length — standard "user string becomes a
  filename" hardening, not something this repo has an existing precedent for (no
  user-string-to-filename code found in this pass) but a well-known, cheap-to-apply
  pattern.

---

## Risks

- **`GS_RITZ_APPID` vs. the brief's "existing env var named `AppId`" is very likely a
  naming mismatch** (§1) — building against a literal `getenv("AppId")` that does not
  exist anywhere in this codebase (or, per web research, in Steam's documented
  behaviour) would simply never fire. This needs resolving before implementation, not
  discovered during it.
- **ConVar/global cross-thread write safety is inherited, not solved, by this design**
  (§3/§5) — the hybrid recommendation reduces new risk (new settings get their own
  struct read only by the steamcompmgr thread) but does not fix the pre-existing
  unsynchronized ConVar-write pattern for the settings that *do* map onto existing
  ConVars.
- **Large swaths of the schema are placeholders** (ReShade uniform ranges, FPS-display
  final values, audio volume curve, all of LSFG-VK) pending sibling scouts — the
  structure is sound but the leaf values are explicitly not authoritative yet.
- **Live-reference profiles (§6, recommended) add a third resolution level** that must
  be re-derived correctly every time the UI needs to show "effective" settings for a
  game — more surface area for a resolution bug (e.g. showing stale profile values
  after a profile edit) than the simpler one-time-copy alternative would have.
- **No audio subsystem or LSFG-VK code was found in this repo at all** in this scout's
  pass (`grep`-checked against `scripting-convars.md`/`control-ipc.md` and a targeted
  `src/` search) — the config schema for those sections is close to pure speculation
  structurally, not just parameter-wise, until those scouts report back.

## Open questions for the user

1. **Is the "existing env var named `AppId`" actually `SteamAppId`** (the only literal
   Steam-appid env var found anywhere, `layer/VkLayer_FROG_gamescope_wsi.cpp:87`, and
   corroborated by external Steam documentation as a real Steam-set var), and if so, is
   it confirmed to be inherited by gamescope's own process (not just the game process
   gamescope launches)? This needs a real answer before `GS_RITZ_APPID`-vs-`AppId`
   override logic can be written correctly. (§1)
2. **"Override Global Config" — all-or-nothing or per-setting fallback?** Recommended:
   per-setting. This changes both the JSON file shape and the UI's "effective settings"
   computation. (Layering semantics)
3. **Does applying a profile mean a one-time copy into the game file, or a live
   reference the game file keeps tracking?** Recommended: live reference. Materially
   different UX (frozen snapshot vs. "editing the profile updates every game using it")
   and different deletion/orphan-handling requirements. (Layering semantics)
4. **Should a late-arriving Steam-discovered app id (post-startup, per-window, per §2)
   ever trigger loading/switching to a per-game config mid-session, or is `GS_RITZ_APPID`
   (read once, at startup) the only identity source for config purposes in v1?**
   Recommended: startup-only for v1, given the ordering findings in §1. (§1/§2)
5. **What exactly belongs in the General section beyond `Lossless.dll` path?** The brief
   says "whatever else proves necessary" — nothing else surfaced in this scout's source
   pass; needs the user's own list or a later pass once LSFG-VK/audio scouts report.
   (§5 schema)
6. **Should config writes for settings that already have a live external-write path
   (X11 properties for filter/scaler/sharpness, `ConVar::SetValue` for VRR/HDR/tearing)
   go through those existing mechanisms, or should the config system own a fully
   independent write path that happens to also update those variables?** This doc
   recommends reusing the existing mechanisms (§3), but it's a real architectural
   fork with maintenance-burden implications either way.
7. **Malformed hand-edited JSON — is "fail loudly for that layer, fall back one level,
   and show a UI warning" (§7, recommended) the right failure mode, or should gamescope
   refuse to start / block the overlay until the user fixes or deletes the bad file?**
   The recommendation here optimizes for "gamescope should never fail to launch a game
   over a config typo," but that's a judgment call the user should confirm.
