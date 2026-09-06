# Profiles and per-game settings: the concept

**v2, 2026-09-06: the user's list/modal layout replaced v1's rows; inheritance added.**
The file layer for v2 (sections 2-6, 9 below) is implemented -- `src/Config/
ConfigManager.{h,cpp}`, schema 3 -- and documented for users of the code in
[`../features/profiles.md`](../features/profiles.md); the Profiles area UI is the
follow-up. Sections 1 and 7 are kept as written on 2026-09-06 morning, as the history
of how v1 was found wanting. The four decisions v2 adds over v1, each with its why:

1. **A list with Create / Copy / Edit / Delete modals, an Inherits dropdown and a
   "Filter Game Profiles" checkbox, instead of v1's Choice + Action rows.** `Why:` the
   user's own layout; a list shows every profile at once and which one is selected,
   where v1's picker showed one name and hid the rest behind a dropdown.
2. **Two kinds -- general and game -- and only a game profile may inherit, only from a
   general one.** `Why:` the user's model is "a base setup, tweaked per game"; a chain
   (game from game) was never asked for and would turn "where does this value come
   from" from a glance into a walk.
3. **Selecting a profile in the list loads it and is the assignment; every edit is saved
   into it; no load/save buttons.** `Why:` one action, one meaning. v1's separate
   Use / Save / Auto-save switch existed only because a profile was a copy; once the
   profile is the live file there is nothing left for those buttons to do.
4. **Inheritance is live and diff-based** (a game profile stores only what differs from
   its parent; resolution is deep-merge parent then child). `Why live:` the point of a
   base profile is that tuning it reaches every game built on it. `Why diff-based rather
   than per-key plumbing:` every panel writes a whole `Settings` through one funnel, so
   a diff at the funnel gives inheritance to every section, present and future, with no
   panel code -- at the cost of one ceiling: a value set *equal* to the parent's reads
   as inherited (recorded as a `ponytail:` note in `ConfigManager.h` and in
   `features/profiles.md`).

Request: `requests-2026-09-05-round2.md` item 4 -- *"a really good concept for the
Profiles and Game config (Easy and extensible). It should also be possible to load a
certain profile from the command line on launch."*

The one-sentence version: **a profile is the settings file you are editing; every game
remembers which one it selected; a game profile may inherit from a general one;
`--profile` says which one to start with.** Nothing is ever copied, nothing is ever
"applied", and there is exactly one answer to "where did my edit go".

---

## 1. Problem: what is confusing today, in the code

The current model (`src/Config/ConfigManager.h`, `src/Overlay/PanelConfig.{h,cpp}`,
`superdoc/features/profiles.md`'s history section) is correct and every state is visible.
It is also nine concepts deep, and the user has to hold them all to predict a slider:

| Concept | Where | What it is |
|---|---|---|
| `global.json` sections | `LoadGlobal()` | the shared settings |
| `games/<AppId>.json` + `override_global` | `LoadPerGameOverride()`, `SnapshotPerGameOverride()`, `ClearPerGameOverride()`, `RestorePerGameOverride()` | a full copy of the settings, on/off in place |
| Profile | `profiles/<Name>.json`, `SaveProfile()`/`LoadProfile()` | a named copy that "does nothing by itself" |
| Use (apply) | `ApplyProfile()` | a one-time copy of 6 of the 8 sections into the routed file |
| `last_applied_profile` | `Settings`, top level, every file | per-file breadcrumb of the last copy |
| Active profile | `Settings::active_profile`, global.json only | the profile "worked against", kept while dirty |
| Auto-save | `Settings::auto_save_profile`, `FanOutToActiveProfile()` | edits also copied *out* into the active profile |
| Dirty count | `ActiveProfileDirtySections()`, `kProfileSections[]` | routed file vs active profile, per section |
| Backup / Restore | `panelconfig::SettingsBackup`, `BackupMatchesRouting()` | in-memory undo of one Use |

Concrete consequences, each grounded in the code:

1. **"Where does an edit go" has four inputs.** `EnqueueRoutedWrite()` picks
   `games/<id>.json` or `global.json` by `IsSessionOverrideActive()`, then
   `FanOutToActiveProfile()` may *also* write the active profile, and the Appearance
   area bypasses all of it (`QueueGeneralSave()` -> `EnqueueGlobalWrite()`). Override
   x active profile x auto-save x area = eight cases. The Status rows state the answer,
   but the user has to read them to know it.
2. **Two "profile:" facts that can disagree.** The Profiles Status row shows
   `active_profile` (global.json); the Per-game Status row shows
   `last_applied_profile` (the per-game file) -- `panelconfig::ProfileFact()` takes
   the latter. Use profile X under game A (per-game on), then launch game B: B's
   Per-game area says `profile: none` while Profiles says `profile: X`. Both are true;
   neither is what the user asked.
3. **Auto-save crosses games by design.** The fan-out runs in *both* routing branches
   (`EnqueueRoutedWrite()`), so with per-game on for Rust and auto-save on, every Rust
   edit rewrites the active profile that some other game will Use later. DECISIONS #20
   calls this "spooky action" and mitigates it by defaulting auto-save off -- the
   mitigation is a fourth switch the user must understand.
4. **The dirty count compares the wrong pair when per-game is on.**
   `ActiveProfileDirtySections()` diffs `CurrentRoutedSettings()` (this game's file)
   against the *global* active profile, which may never have been applied to this
   game. "3 sections changed" is then a fact about two unrelated files.
5. **A profile-aware section list has to be maintained by hand.** `ApplyProfile()`
   copies six named sections and `kProfileSections[]` repeats the list with a comment
   telling the next agent to keep them in step; `tests/test_config.cpp` has "crosshair
   rides in a per-game snapshot and in a profile" precisely because adding a section
   is a thing you can forget. That is the opposite of extensible.
6. **Nothing reaches the command line.** `main.cpp` resolves
   `ResolveEffective( oRitzAppId )` -- per-game file or global -- and seeds the
   startup state (`apply_ritz_config_to_startup_state()`, `PanelShaders_ApplyStartupConfig()`,
   `PanelSystem_SeedFromConfig()`). There is no way to name a profile at launch, and
   with the current semantics there would be no good answer to what one *means*: a
   one-time copy into global.json (the Use semantics) would wipe yesterday's tweaks on
   every launch.
7. **Restore is a safety net for a hazard the model creates.** Use replaces a file
   wholesale, so a backup is needed, so `BackupMatchesRouting()` is needed because the
   routing can change under it. Remove the wholesale replace and the whole chain goes.

Docs vs code, for the record: `superdoc/planning/config-system.md` ("Layering
semantics") recommended per-key fallback *and* a live profile reference; DECISIONS #19
and #20 chose full snapshot and one-time copy, and the code follows DECISIONS. This
concept returns to the *live reference* half of the original recommendation (with the
reason it was rejected -- "three-way resolution to reason about" -- answered by not
layering at all, section 2). `superdoc/README.md`'s index line for the profiles
feature still says "the Phase B list still pending"; Phase B shipped (`e573f02`).
DECISIONS #26 describes `layout_name`/`ResolveLayoutCached()`, removed 2026-09-03
(`ConfigSchema.h`'s `anchor` comment); it is already moot.

---

## 2. The model (v2)

Four concepts. Everything else in the table above is deleted.

- **Profile** -- a named settings file, `profiles/<Name>.json`, carrying every
  per-layer section (`gamescope`, `fps_display`, `crosshair`, `reshade`,
  `notifications`, `system`, and any section added later). **It is the file you are
  editing.** Every change is saved into it immediately. There is always at least one; a
  fresh install has **Default**.
- **Kind** -- a profile is **general** (`Comp`, `Casual`) or a **game** profile, bound to
  one app id and shown as `[Game] Rust`. Only a game profile can **inherit**, and only
  from a general profile: two levels, no chains.
- **Assignment** -- which profile a game uses: `games.<AppId>.selected`, set by
  selecting in the list; plus `last_general`, the general profile most recently
  selected anywhere, which a game seen for the first time starts on. Assignments are
  pointers; switching one changes nothing inside any profile.
- **Session override** -- `--profile <name>` / `GS_RITZ_PROFILE` / `ritz_profile`: the
  profile this session edits, overriding the assignment for the session only.

**The mental model, one sentence:** *you are always editing exactly one profile, its
name is at the top of every area, and each game remembers which one it selected.*

### Where does an edit go

| Situation | Every edit goes to |
|---|---|
| No game identified (persistent session, non-Steam launch) | `last_general` |
| Game identified, never selected anything | `last_general` |
| Game selected `Rust` (a game profile inheriting `Comp`) | `Rust` -- as the keys that differ from `Comp`; the rest follows `Comp` |
| Game selected `Comp` (a general profile) | `Comp` -- shared with every other game on it |
| Launched with `--profile Tourney` (whatever the assignment says) | `Tourney` |
| Appearance area (`overlay`: scale, accent, cursor look, window geometry) | `global.json`, always -- the one exception, because it is about the player's screen, not the game |

There is no second write. No fan-out, no breadcrumb, no dirty state, no backup --
because there is nothing a profile could drift *from*. "Do my edits go into the
profile?" is always yes.

**Sharing is real sharing.** If Rust and CS both select `Comp`, an edit under Rust is an
edit to `Comp` and CS sees it next launch. A player who wants Rust to differ creates a
game profile for it, inheriting `Comp` -- and then only the values they change in Rust
are Rust's; everything else keeps following `Comp`. `Why this and not v1's "copy in,
never link":` the copy model made the user restart to see anything and left every game
silently frozen at the moment of its copy; the link model is what he described wanting
when he asked for auto-save -- the profile *is* his settings.

### Inheritance

- A game profile with `inherits` stores only the keys whose JSON value differs from its
  parent's (`SparseDiff()`, computed on every write against the resolved parent);
  reading deep-merges the parent's sections, then the child's, then parses. Resolution
  is canonical (through the struct), so a parent written by an older build does not
  make a newer section look wholly overridden.
- Per value the UI shows *inherited* vs *overridden* (`OverriddenKeys()`, a set of
  dotted keys) and offers *Reset to inherited* (`ResetKeyToInherited()`).
- **The ceiling, on purpose:** a value set equal to the parent's is stored as nothing
  and reads as inherited -- it will follow the parent later. Per-key "explicitly set"
  plumbing through every panel would remove the ceiling at the cost of touching every
  binding; not worth it until it bites.
- **Delete a parent:** its children get the resolved values baked in and become
  standalone. **Edit** may switch a profile between general and game and change its app
  id, name and parent; turning a parent into a game profile is refused while it has
  children; a rename follows every pointer (assignments, children's `inherits`, the
  session override); switching parents keeps the resolved values, re-expressed against
  the new parent.
- A standalone game profile (`inherits` empty) stores everything, like a general one.

### Extensibility

- **A new section slots in with zero profile code.** `SectionsToJson()` /
  `SettingsFromJson()` serialise the struct; a profile is the struct; the diff is a
  JSON diff. Adding a `filters` or `keybinds` section means one struct, one serializer
  block, one panel -- and inheritance, markers and reset come for free.
- **Partial profiles ("crosshair only"): no, on purpose.** Inheritance already answers
  the real need (a game profile that changes *only* the crosshair stores only the
  crosshair) without a second source per value. Presets remain the answer for "apply
  this crosshair to whatever I am editing"; see `feature-ideas-2026-09-05.md`.
- **Per-game facts that are not settings** live on the game's entry in `global.json`:
  `games.<AppId>.audio_node` (was `AudioSettings::manual_node_binary`), read and written
  by `PanelAudio.cpp` through `config::GameEntry()` / `SetGameAudioNode()`, because it
  names one game's process and a profile can be shared. `notifications.muted` stays a
  normal section (DECISIONS #25 made it per-game *eligible*, not per-game only).

### File layout (schema 3)

```
~/.config/gamescope-ritz/
  global.json                 schema_version 3
                              overlay: { ...unchanged... }
                              profiles: { last_general: "Comp",
                                          games: { "252490": { selected: "Rust", audio_node: "" } } }
  profiles/Comp.json          name, kind "general", every section
  profiles/Rust.json          name, kind "game", app_id, game_name, inherits "Comp",
                              only the differing keys
  games/                      left untouched after migration (section 5); never read again
```

`global.json` stays "the machine's own file": process-level appearance plus the
pointers, and **no per-layer section any more** -- the sections live in profiles, so a
hand-edit to `global.json`'s `gamescope` (which v1 would have honoured) cannot
silently do nothing. Examples of each file: `features/profiles.md`.

---

## 3. The UI afterwards (the user's layout)

One area, **Profiles** (`setup.profiles`). The **Per-game** area (`setup.pergame`) is
retired: a game's settings are a profile.

- **The list.** Every profile, general ones by name, game ones as `[Game] <title>`
  (the app id until a title has been seen). The selected row is the profile the
  session edits; **selecting a row loads it and is the assignment.** A session
  override (`--profile`) shows as the selected row with a `launch option` badge; picking
  any row ends the override for the session and persists as usual.
- **Filter Game Profiles** (checkbox, on by default): other games' game profiles are
  hidden; general profiles always show; this game's own always show.
- **Create** (modal): name, kind (general / game), app id and display name prefilled
  from the session for a game profile, **Inherits** dropdown (general profiles only,
  enabled for a game profile). Values: the current resolved settings.
- **Copy** (modal): the same fields, values from the selected profile's *resolved*
  settings.
- **Edit** (modal): the same fields on the selected profile; refused with a reason
  when it would make a parent a game profile.
- **Delete** (confirm): children are baked, pointers cleared; the session falls
  through the resolution order if it was editing the deleted one.
- **Per value, everywhere:** an *inherited* / *overridden* marker on rows of an
  inheriting game profile, and *Reset to inherited* in the inspector.
- **Every area shows the profile** as its badge (`Rust`, or `Tourney (launch option)`).

Toasts: switching profile, creating one, and the launch case (`Created profile
'Tourney' from '252490'`). Everything else is silent, as edits are today.

---

## 4. Command line

```
gamescope --profile <name> [other flags] -- game
GS_RITZ_PROFILE=<name> gamescope -- game          # equivalent; the flag wins if both are set
gamescopectl ritz_profile <name>                  # the live switch, same code path
```

Steam launch options: `gamescope --profile Comp -- %command%`, or, when the flag is
awkward (a wrapper script that owns argv), `GS_RITZ_PROFILE=Comp gamescope --
%command%`. `Why GS_RITZ_ and not GAMESCOPE_RITZ_:` `GS_RITZ_APPID`
(`src/Config/AppId.cpp`) is the env var users already set.

**Semantics (implemented):**

1. The name goes through `SanitizeProfileName()`; the file is `profiles/<name>.json`.
2. **It selects the profile for this session and nothing else.** The assignment on
   disk is untouched; the next launch without the flag is back to the assignment.
   The flag beats the assignment the way `-w`/`-h`/`-r` beat `nested_width`
   (`apply_ritz_config_to_startup_state()` runs, then getopt overrides).
3. **Edits during the session go into that profile**, like any session. Nothing is
   session-only; nothing is lost at quit.
4. **If the name does not exist, it is created** as a general profile copied from
   what the session would otherwise have used (the game's selection, else
   `last_general`, else `Default`), with a toast. `Why create:` `--profile Comp` in a
   launch option is the natural way to say "give this game a Comp setup"; a fallback
   would send that session's edits into the wrong file, the exact failure this design
   exists to remove. A typo produces a visible, deletable profile rather than an
   invisible fallback.
5. Any profile, even another game's game profile, can be named.
6. `ritz_profile <name>` (a `ConCommand` in `main.cpp`) calls the same
   `ritz_use_session_profile()`, which a future hotkey binds to.
7. Startup order: `ResolveAppId()` -> `--profile`/env pre-scan ->
   `UseSessionProfile()` -> `ResolvedSettings()` -> `apply_ritz_config_to_startup_state()`
   -> getopt. `--ritz-dump-config` prints `session_profile`, `kind`, `inherits`,
   `source` and `launch_option`.

**Why the alternatives lose:** as in v1's analysis -- a one-time copy at launch
overwrites the previous session's edits; a session-only layer loses them at quit;
persisting the flag as the assignment is hidden state; refusing a missing name sends
edits to the wrong file.

---

## 5. Migration (read old, write new, no user action)

Triggered once, when `global.json` is read with `schema_version` below 3
(`EnsureMigrated()` -> `MigrateV2ToV3()`, file-level because it creates files). Profile
files are written **first**, `global.json` **last**, and every step is idempotent, so an
interruption re-runs cleanly on the next launch -- the user's real config has no
backup. The full table with every rule: `features/profiles.md`, "Migration". In short:

| Old | New |
|---|---|
| `global.json` per-layer sections | general profile `Default` (an identical existing profile is used instead; a different `Default` stays and the old values go to `Default 2`) |
| `active_profile` (if it exists) | `last_general`; else `Default` |
| `profiles/*.json` | unchanged; read as general profiles |
| `games/<AppId>.json` | game profile `<AppId>`, `inherits` = its `last_applied_profile` if that exists, else `Default`, stored as the diff; **selected only if `override_global` was true** |
| `audio.manual_node_binary` (game file) | `games.<AppId>.audio_node` |
| `last_applied_profile`, `auto_save_profile`, global `audio` | dropped |

The old `games/` files are **left in place, unread**. `Why not delete or rename:` the
user's own rule -- "never delete configs automatically" (DECISIONS #19, issue #43).

---

## 6. What was deleted from the code

`src/Config/ConfigSchema.h`: `Settings::last_applied_profile`, `active_profile`,
`auto_save_profile`, `AudioSettings` and `Settings::audio`. Added: `ProfileKind`,
`ProfileMeta`, `GameAssignment`, `ProfileAssignments`; `kCurrentSchemaVersion = 3`.

`src/Config/ConfigManager.{h,cpp}` -- deleted: `LoadPerGameOverride()`,
`ResolveEffective()`, `SnapshotPerGameOverride()`, `ClearPerGameOverride()`,
`HasSavedPerGameConfig()`, `RestorePerGameOverride()`, `DeletePerGameOverride()`,
`ApplyProfile()`, `ApplyProfileAtStartup()`, `ActiveProfile()`/`SetActiveProfile()`,
`AutoSaveProfile()`/`SetAutoSaveProfile()`, `FanOutToActiveProfile()`,
`kProfileSections[]`, `ActiveProfileDirtySections()`, `CurrentRoutedSettings()`,
`EnqueuePerGameSnapshot()`, `IsSessionOverrideActive()`/`SetSessionOverrideActive()`,
`ListGameIds()`, `GamePath()`, `RenameProfile()` (folded into `EditProfileMeta()`).
Kept: the path helpers, `SanitizeProfileName()`, `LoadGlobal()`/`SaveGlobal()` (now
overlay + pointers only), `LoadProfile()` (now resolved), `SaveProfile()` (now takes a
`ProfileMeta`), `ListProfiles()` (now returns metas), `ConfigWriter` and its
coalescing (plus `Discard()`), the three `overlay` write paths, `EnqueueRoutedWrite()`
(now: write the session profile), `ConfigGeneration()`/`BumpConfigGeneration()`,
`FlushPendingWrites()`, `DebugDumpEffective()` (now no-arg). Added: `SessionProfile()`,
`SessionProfileOverride()`, `SessionProfileParent()`, `ResolvedSettings()`,
`SelectProfile()`, `UseSessionProfile()`, `CreateProfile()`, `CopyProfile()`,
`EditProfileMeta()`, `DeleteProfile()`, `OverriddenKeys()`, `ResetKeyToInherited()`,
`GameEntry()`, `SetGameAudioNode()`, `NoteFocusedWindowTitle()`, `SessionGameName()`,
`LoadProfileMeta()`, `ProfileExists()`, `GamesDir()` (migration input only).

`src/Overlay/PanelConfig.{h,cpp}`: the whole v1 Profiles/Per-game surface
(`SettingsBackup`, `BackupMatchesRouting()`, `ChangesFact()`, `SavingFact()`,
`UseConfirmPrompt()`, `SaveChangesBlocker()`, `EditsGoTo()`, `OwnSettingsFact()`,
`PerGameFilePath()`, `StatusInputs`/`StatusHash()`, `EnableOverride()`,
`DisableOverride()`, `DeleteSavedPerGameConfig()`, `UseProfile()`,
`RestorePreviousSettings()`, `StartFromProfile()`, `CopySelectedGameConfig()`,
`SaveChangesToActiveProfile()`, `SetAutoSave()`, `BuildPerGameArea()`, the Per-game
registration). Kept in the header for the rebuild: `ClampPickerSelection()`,
`GameFact()`; added `ListLabel()` and `ShowsInList()` (the `[Game]` label and the
filter rule, pinned by tests). The area is a placeholder Status row until the list UI
lands.

`src/main.cpp`: `ResolveEffective( oRitzAppId )` became `config::ResolvedSettings()`
after the `--profile` pre-scan; every panel's `EnsureConfigLoaded()` calls the same.
`src/Overlay/PanelAudio.cpp` reads/writes `GameEntry()`. `src/steamcompmgr.cpp` feeds
the focused window's title to `NoteFocusedWindowTitle()`.

Tests replaced in `tests/test_config.cpp`: every per-game snapshot/clear/restore/delete
case, the ApplyProfile/ApplyProfileAtStartup cases, the Phase B (active profile,
auto-save, dirty count, rename/delete) cases; section round-trips now go through a
profile file, old-config fixtures through `ResolvedSettings()` (which exercises the
migration). New: schema, inheritance/diff, the migration table (one test per row),
session resolution, routing, `UseSessionProfile`, the CRUD rules, markers/reset, the
game name, the dump. `tests/test_overlay_profiles.cpp`: the label, filter, clamp and
save-then-list cases.

DECISIONS superseded: **#19** and **#20** (see their notes and **#28**). **#21** (app id
resolution) and **#25** stand. TERMINOLOGY: Profile, Inherits, Selected profile,
Session profile replace the v1 entries; the history moved to `features/profiles.md`.

---

## 7. Alternatives considered

- **A. Keep the model, revert Phase B** (drop active profile, auto-save, dirty count,
  Save changes; keep per-game snapshot + Use + backup; `--profile` = Use at launch).
  Fewer rows, but "where does an edit go" still has two answers plus Appearance,
  `--profile` still copies and so still loses the session's edits into the other
  file, and it takes away the auto-save the user asked for by name.
- **B. This concept.**
- **C. Layered / partial profiles** (per-key fallback per `config-system.md`; a
  profile may carry a subset of sections; a game file carries deltas). Maximally
  flexible; every value has three possible sources and the UI has to show which.
  The user's complaint was about not knowing where things come from and go; C makes
  that structurally harder to answer. Presets cover the real "crosshair only" need.

---

## 8. Open questions for the user (answered 2026-09-06)

Answers: 1 -- no locked mode; a launch-option profile is edited like any other.
2 -- create it (section 4, point 4). 3 -- one area; the list is the whole surface.
The questions as asked:

1. **A launch-option profile that must not change:** do you ever want `--profile
   Comp` to be *protected* from in-game edits (a tournament setup you tune once)? If
   yes, that is a separate `--profile-locked` (edits stay in memory for the session,
   badge says `Comp (locked)`), not the default -- it reintroduces unsaved state, so
   it should exist only if you will use it.
2. **`--profile <name that does not exist>`:** create it from what the game would
   have used (recommended, section 4 point 4), or refuse and fall back with a toast?
3. **One area or two:** merge Per-game into Profiles (recommended: two rows do not
   justify a rail item), or keep a thin Per-game rail item for discoverability?

Not asked, decided: no partial profiles (presets instead); Appearance stays global;
`notifications.muted` stays a normal section; the built-in profile is called
`Default`; old `games/` files are left on disk.

---

## 9. Implementation (v2)

Phases 1-4 landed together in one commit (the schema removal does not compile without
the new routing, and the migration is entangled with the loaders), with the tests
listed in section 6 and a headless end-to-end
(`build-release/verify-shots/profiles-v2/e2e.sh`, 25 checks: migration, a routed edit
landing as a diff, `--profile` create-if-missing with the assignment untouched,
`GS_RITZ_PROFILE`, `ritz_profile`, the flagless launch back on the assignment).
`scripts/pixel-regression.sh` seeds a schema-3 config (`profiles/Pixel.json` +
pointers) and stays green.

Still to do, in order:

5. **Profiles area rewrite** (the list, the four modals, the filter, the badge on every
   area, the per-value markers and *Reset to inherited*), against the API in
   `features/profiles.md`; `test_overlay_profiles` re-pinned. Laptop check: select,
   create, copy, edit (refusal wording), delete (bake), filter, launch option visible.
6. **CHANGELOG** `Added`/`Removed` lines for the UI once it is visible; the 2026-09-06
   `Info` line already says the model changed underneath.
