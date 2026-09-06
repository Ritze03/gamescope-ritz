# Profiles

**Profiles v2 (2026-09-06).** A profile is the settings file being edited; each game
remembers which one it selected; a game profile may inherit from a general one;
`--profile <name>` picks one for the session. Concept and the reasons behind it:
[`../planning/profiles-concept.md`](../planning/profiles-concept.md). File layer:
`src/Config/ConfigManager.{h,cpp}` and `src/Config/ConfigSchema.h`; tests in
`tests/test_config.cpp` (file layer, migration, session, CRUD) and
`tests/test_overlay_profiles.cpp` (the pure half of the area).

**The Profiles area itself (`setup.profiles`) is being rebuilt** around a list with
Create / Copy / Edit / Delete modals, an Inherits dropdown and a "Filter Game
Profiles" checkbox. Until it lands, `src/Overlay/PanelConfig.cpp` registers a
placeholder Status row (editing / inherits / launch option / game). The old
Per-game area (`setup.pergame`) is gone for good: a game's settings *are* a profile.

## The model, in plain words

- A **profile** (`profiles/<Name>.json`) is the settings you are editing. **Every
  change is saved into it immediately**; there are no load/save buttons and nothing
  is ever "applied" or copied.
- Two kinds. A **general** profile (`Comp`, `Casual`) stands alone. A **game**
  profile is bound to one app id (shown as `[Game] Rust`) and may **inherit** from
  exactly one general profile. Two levels, no chains -- a game profile can never be
  a parent.
- **Selecting a profile in the list loads it and is the assignment**: the current
  game remembers the selection (`games.<AppId>.selected`). A game seen for the first
  time uses the general profile last selected anywhere (`last_general`). There is
  no "default" row; a fresh install gets `Default`, created from the built-in
  defaults the first time anything asks.
- **Inheritance is live.** A game profile stores only the values that differ from
  its parent; everything else follows the parent as it changes. The UI will mark
  each value *inherited* or *overridden* and offer *Reset to inherited*.
- **Appearance** (`overlay.*`: UI scale, accent, opacities, cursor look, window
  geometry, toast placement) stays in `global.json`, as before. It is about the
  player's screen, not the game, and never rides in a profile.
- The one per-game fact that is not a setting -- which PipeWire stream is this
  game's (`audio_node`) -- lives on the game's entry in `global.json`, not in a
  profile, because it names one game's process and a profile can be shared.

## Files

```
~/.config/gamescope-ritz/
  global.json             schema 3: overlay + the pointers
  profiles/<Name>.json    one per profile: metadata + the per-layer sections
  games/                  schema-2 leftovers, never read again (see Migration)
```

`global.json`:

```json
{
  "schema_version": 3,
  "overlay": { "display_scale": 1.25, "accent_hue": 218.0, "...": "..." },
  "profiles": {
    "last_general": "Comp",
    "games": {
      "252490": { "selected": "Rust", "audio_node": "RustClient.exe" }
    }
  }
}
```

A general profile (`profiles/Comp.json`) -- every section, in full:

```json
{
  "schema_version": 3,
  "name": "Comp",
  "kind": "general",
  "gamescope": { "filter": "FSR", "sharpness": 4, "...": "..." },
  "fps_display": { "...": "..." },
  "crosshair": { "...": "..." },
  "reshade": { "...": "..." },
  "notifications": { "muted": false },
  "system": { "clipboard_sync": true }
}
```

A game profile inheriting from it (`profiles/Rust.json`) -- only what differs:

```json
{
  "schema_version": 3,
  "name": "Rust",
  "kind": "game",
  "app_id": "252490",
  "game_name": "Rust",
  "inherits": "Comp",
  "gamescope": { "sharpness": 9 },
  "reshade": { "vibrancy": { "enabled": true } }
}
```

A standalone game profile (`"inherits": ""`) stores everything, like a general one.
`name` always equals the file name; `game_name` is the focused window's title, written
the first time the game is seen (`NoteFocusedWindowTitle()`), so the list can show
`[Game] Rust` while the game is not running; before that the list shows the app id.

## Which profile a session edits (`SessionProfile()`)

1. the **session override** -- `--profile`, `GS_RITZ_PROFILE`, or the `ritz_profile`
   console command;
2. `games.<AppId>.selected`, when a game is identified (`SessionAppId()`, DECISIONS
   #21) and that profile exists;
3. `last_general`, if it exists;
4. `Default`, created from the struct defaults if there is nothing at all.

`SelectProfile(name)` persists the choice -- `games.<AppId>.selected` when a game is
identified, and `last_general` whenever the profile is general -- clears any session
override, and bumps `ConfigGeneration()` so every panel's `EnsureConfigLoaded()`
reloads through `ResolvedSettings()`. With no app id a *game* profile can only be
selected for the session (there is no game to remember it for); a general one is
remembered as `last_general`.

## Inheritance: how the diff works, and its ceiling

On every write of an inheriting game profile (`EnqueueRoutedWrite()` from any panel,
`SaveProfile()`, the CRUD below) the resolved settings and the resolved parent are both
serialised to canonical JSON and only the keys whose value differs are stored
(`SparseDiff()`; nested objects recurse and vanish when nothing inside differs).
Reading is the reverse: the parent's sections, the child's sparse keys deep-merged
over them, then parsed (`DeepMerge()`, `ResolvedSectionsJson()`). Both sides are
canonicalised through the struct first, so a parent written by an older build (missing
a section added since) does not make every key of that section look overridden.

`OverriddenKeys()` is the set of dotted keys the session profile stores itself
(`"reshade.vibrancy.strength"`, `"fps_display.enabled"`), cached on the write
sequence so the UI can ask every frame; `ResetKeyToInherited(key)` drops one and bumps
the generation.

`ponytail:` **the ceiling.** A value the user sets *equal* to the parent's is stored as
nothing, reads as inherited, and will follow the parent later. The alternative --
per-key "explicitly set" plumbing through every panel's binding -- was judged not worth
it for this case; if it ever bites, the fix is a per-key marker on the write path, not
a redesign. Stated in `ConfigManager.h` beside `OverriddenKeys()`.

`Why diff-based and not per-key plumbing:` every panel keeps a whole `Settings` struct
and writes the whole thing through one funnel; a diff at that funnel gives inheritance
to every existing and future section with zero panel code, which is the extensibility
the concept was written for.

## The API the Profiles area calls (`Config/ConfigManager.h`)

| Need | Call |
|---|---|
| the list | `ListProfiles()` -> `std::vector<ProfileMeta>`; `LoadProfileMeta(name)`, `ProfileExists(name)` |
| what the session edits | `SessionProfile()`, `SessionProfileParent()`, `SessionProfileOverride()`, `SessionAppId()`, `SessionGameName()` |
| the values | `ResolvedSettings()` (every panel's `EnsureConfigLoaded()`), `LoadProfile(name)` (resolved) |
| select | `SelectProfile(name)` |
| Create / Copy / Edit / Delete | `CreateProfile(meta, from=nullptr)`, `CopyProfile(src, meta)`, `EditProfileMeta(old, meta)`, `DeleteProfile(name)` -- each returns a `ProfileOp` (`ok`, `error` text for the modal) |
| markers | `OverriddenKeys()`, `ResetKeyToInherited(dottedKey)` |
| the game entry | `GameEntry(appId)` -> `{ selected, audio_node }`, `SetGameAudioNode(appId, binary)` |
| launch option / live switch | `UseSessionProfile(rawName)` -> `{ ok, name, created, copied_from }` |
| writes | `EnqueueRoutedWrite(settings)` (every panel), `SaveProfile(meta, settings)` (sync), `EnqueueProfileWrite(meta, settings)` |

Rules the CRUD enforces (each refusal comes back as `ProfileOp::error`): a name must
survive `SanitizeProfileName()` unchanged and not exist yet; only a game profile may
inherit, and only from a general profile that exists (never itself); a game profile
needs an app id; **a general profile with children cannot become a game profile**;
**rename follows every pointer** (assignments, children's `inherits`, the session
override); **deleting a parent bakes its children** (resolved values written,
`inherits` cleared); switching a profile's parent, or its kind, keeps its *resolved*
values (re-diffed against the new parent, or baked in).

`SetGameAudioNode()` is what `PanelAudio.cpp`'s stream picker writes now (it used to
be `AudioSettings::manual_node_binary` in the routed settings); with no app id the
pick is session-only.

## Where a write goes, and when

- **Every panel** -> `EnqueueRoutedWrite(settings)` -> the session profile's file,
  queued on the coalescing background writer (50 ms quiet, 500 ms cap, per-path
  last-wins); `overlay` is ignored. For an inheriting game profile the diff is
  computed at enqueue time against a cached copy of the parent's resolved sections
  (`s_oSessionParentSections`), so a slider tick never reads the parent's file.
- **Appearance** -> `EnqueueGlobalWrite` / `EnqueueOverlayWrite` /
  `EnqueueGeometryWrite` -> `global.json`, always. The pointers are taken from the
  in-memory mirror on every global write, so a panel's stale copy of `Settings` can
  never overwrite them.
- **The CRUD and `ResetKeyToInherited`** write synchronously (a button press followed
  by a directory re-read, the 2026-09-05 "restart to see a new profile" lesson) and
  **discard any queued write for the same file first** -- the synchronous write was
  computed from the mirror and is the newer of the two; the queued one landing later
  would undo it (`WriteProfileNow()`, `ConfigWriter::Discard()`).
- **The mirror** (`s_Written`, per profile name) is what this process last queued or
  wrote for each profile: `ResolvedSettings()`, `OverriddenKeys()`, the CRUD and the
  diff base all read it before the disk, so nothing in the process is ever behind a
  queued write.

## Command line and console

```
gamescope --profile <name> [flags] -- game
GS_RITZ_PROFILE=<name> gamescope -- game     # the flag wins if both are set
gamescopectl ritz_profile <name>             # live switch, same code path
```

Selects `<name>` **for this session only**: the assignment on disk is untouched, the
next flagless launch is back on it. Any profile, even another game's. Edits during the
session go into `<name>`. A name that does not exist is **created** as a general
profile copied from what the session would otherwise have used, with a toast
(`Created profile 'Tourney' from '252490'`); a name that sanitizes to nothing is
refused with a toast. `main.cpp` pre-scans `argv` for `--profile`/`--profile=` (stopping
at `--`) before `apply_ritz_config_to_startup_state()`, for the same reason
`-w`/`-h`/`-r` win over `nested_width`; `ritz_use_session_profile()` is the one path
behind the flag, the env var and the `ritz_profile` ConCommand (a later hotkey binds to
it). `--ritz-dump-config` prints `resolved_app_id`, `session_profile`, `kind`,
`inherits`, `source` (which rule chose it), `launch_option` and the resolved settings.

`Why session-only, and why create a missing name:` section 4 of the concept doc -- a
launch option that silently changed the next flagless launch is hidden state, and a
typo that fell back to the shared profile would send the session's edits into the
wrong file.

## Migration (schema 2 -> 3)

Runs once, the first time anything reads a `global.json` whose `schema_version` is
below 3 (`EnsureMigrated()` -> `MigrateV2ToV3()`, file-level because it creates files).
Every profile file is written **first** and `global.json` **last**, so an interruption
leaves a schema-2 `global.json` that simply re-runs it; every step is idempotent (an
identical `Default` is found, not duplicated; a game profile already bound to its app
id is kept). The user's real config has no backup, which is why.

| Old | New |
|---|---|
| `global.json` per-layer sections | general profile `Default` -- unless an existing general profile has identical sections (canonicalised), which is used instead; a different profile already named `Default` keeps its name and the old values go to `Default 2` |
| `global.json` `overlay` | stays in `global.json` |
| `active_profile`, if it names an existing general profile | `last_general`; else `last_general` = the Default from row 1 |
| `profiles/*.json` | unchanged on disk; they read as general profiles (no `kind` key) and gain the metadata on their next write |
| `games/<AppId>.json` | game profile named `<AppId>` (or `Game <AppId>` if that name is a different profile), `app_id` = `<AppId>`, `inherits` = its `last_applied_profile` if that is an existing general profile, else the Default; sections stored as the diff against the parent |
| `games/<AppId>.json` with `override_global: true` | `games.<AppId>.selected` = that profile |
| `games/<AppId>.json` with `override_global: false` | profile created, **not** selected (the values were deliberately kept, DECISIONS #19) |
| `games/<AppId>.json` `audio.manual_node_binary` | `games.<AppId>.audio_node` |
| `global.json` `audio.manual_node_binary` | dropped -- no game to bind it to |
| `last_applied_profile`, `auto_save_profile` | dropped |
| the `games/` directory | **left in place, unread** -- the user's rule: never delete a config automatically |

A fresh install (no `global.json`) and a schema-3 file are left alone; reading never
writes. A `schema_version` newer than the build still falls back to defaults and logs,
as `ParseConfigFile()` always did. The 1 -> 2 vibrancy step runs first on the old
file, so a schema-1 config migrates through both.

Verified headless (2026-09-06, `build-release/verify-shots/profiles-v2/e2e.sh`, 25
checks): an old-schema config with a global, a profile and a `games/` file migrates as
the table says; an `overlay_e2_set` edit lands in the selected game profile as a diff;
`--profile Tourney` creates it from the game's profile and leaves the assignment alone;
`GS_RITZ_PROFILE` selects; `ritz_profile` switches live; the next flagless launch is
back on the assignment.

## What v1 had, and where it went (history)

v1 (2026-09-05, `superdoc/planning/DECISIONS.md` #19/#20) had `global.json` as the
shared settings, `games/<AppId>.json` as a full per-game snapshot switched by **Use
separate settings for this game**, profiles as named copies that did nothing until
**Use** copied them in (with a one-step in-memory backup and **Restore previous
settings**), an **active profile** breadcrumb with a dirty count, an opt-in
**Auto-save to profile** fan-out, and `--profile` as Use-at-launch. All of it went
with the model: a profile is the live file now, so there is nothing to apply, back
up, fan out to, or count drift against; auto-save is simply how it works. The
per-game snapshot's surviving rule -- never delete a config automatically -- is kept
by the migration leaving `games/` alone and by Delete being the only destructive
action.
