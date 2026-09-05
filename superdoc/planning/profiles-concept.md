# Profiles and per-game settings: a simpler concept

**Status: concept written 2026-09-06, awaiting user approval. No code changed.**
Request: `requests-2026-09-05-round2.md` item 4 -- *"a really good concept for the
Profiles and Game config (Easy and extensible). It should also be possible to load a
certain profile from the command line on launch."*

The one-sentence version: **a profile is the settings file you are editing; every game
points at one; `--profile` says which one to start with.** Nothing is ever copied,
nothing is ever "applied", and there is exactly one answer to "where did my edit go".

---

## 1. Problem: what is confusing today, in the code

The current model (`src/Config/ConfigManager.h`, `src/Overlay/PanelConfig.{h,cpp}`,
`superdoc/features/profiles-and-per-game.md`) is correct and every state is visible.
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

## 2. Proposed model

Three concepts survive. Everything else in the table above is deleted.

- **Profile** -- a named settings file, `profiles/<Name>.json`, carrying every
  per-layer section (`gamescope`, `fps_display`, `crosshair`, `reshade`,
  `notifications`, `system`, and any section added later). **It is the file you are
  editing.** Every change is saved into it immediately, the way `global.json` is today.
  There is always at least one; a fresh install has **Default**.
- **Assignment** -- which profile a game uses: `games: { "<AppId>": { "profile":
  "Rust" } }` plus one `default` entry for games without their own. Assignments are
  pointers. Switching one changes nothing inside any profile.
- **Launch profile** -- `--profile <name>` (or `GS_RITZ_PROFILE`): the profile this
  session starts in, overriding the assignment for the session only (section 4).

**The mental model, one sentence:** *you are always editing exactly one profile, its
name is at the top of every area, and each game remembers which one it uses.*

### Where does an edit go

| Situation | Every edit goes to |
|---|---|
| No game identified (persistent session, non-Steam launch) | the `default` profile |
| Game identified, no assignment of its own | the `default` profile |
| Game assigned to `Rust` | `Rust` |
| Launched with `--profile Comp` (whatever the assignment says) | `Comp` |
| Appearance area (`overlay`: scale, accent, cursor look, window geometry) | `global.json`, always -- the one exception, kept because it is about the player's screen, not the game (`ConfigSchema.h`'s `OverlaySettings` comment); the area's badge already says so |

There is no second write. No fan-out, no breadcrumb, no dirty state, no backup --
because there is nothing a profile could drift *from*. "Do my edits go into the
profile?" is always yes.

**Sharing is real sharing.** If Rust and CS both use `Comp`, an edit under Rust is an
edit to `Comp` and CS sees it next launch. That is what using the same profile means
in every application that has profiles (OBS, browsers, peripheral software), and the
Status row says `used by: 2 games`. A player who wants Rust isolated presses **Give
this game its own copy** once. `Why this and not the current "copy in, never link":`
the copy model made the user restart to see anything and left every game silently
frozen at the moment of its copy; the link model is what he described wanting when he
asked for auto-save ("a toggle for auto-saving") -- he wanted the profile to *be* his
settings, not to chase them.

### Extensibility

- **A new section slots in with zero profile code.** `SettingsToJson()` /
  `SettingsFromJson()` serialise the struct; a profile is the struct. Adding a
  `filters` or `keybinds` section (or crosshair presets inside `crosshair`) means one
  struct, one serializer block, one panel -- exactly what Crosshair and System needed
  in 2026-09-05, minus the `ApplyProfile()`/`kProfileSections[]` edits and their
  ride-along test. The only section-aware code left is the `overlay` gate, which
  stays (`bIncludeOverlay`).
- **Partial profiles ("crosshair only"): no, on purpose.** A profile carrying a subset
  of sections means the missing sections resolve from *somewhere else*, which is
  layering -- and layering brings back exactly the question this design removes ("this
  value: from the game's layer, the profile, or global?"). The need behind "crosshair
  only" is real but is a **preset**: a named value for one section, applied by copying
  those fields into the current profile (a `Choice` row in the Crosshair area, values
  stored as a list in `global.json` or `presets/crosshair/<name>.json`). Presets are
  small, copy-once, and never affect routing. See `feature-ideas-2026-09-05.md`.
- **Per-game facts that are not settings** move to the assignment entry. Today
  `AudioSettings::manual_node_binary` "names one game's process" and `ApplyProfile()`
  has to special-case it; in a shared profile it would point CS's volume control at
  Rust's process. It becomes `games.<AppId>.audio_node`, read by `PanelAudio.cpp`
  through a `config::GameEntry()` accessor. `notifications.muted` stays a normal
  section (DECISIONS #25 made it per-game *eligible*, not per-game only); a game that
  wants toasts muted gets its own profile, which is one press.

### File layout (schema 3)

```
~/.config/gamescope-ritz/
  global.json                 schema_version 3
                              overlay: { ...unchanged... }
                              profiles: { default: "Default",
                                          games: { "252490": { profile: "Rust", audio_node: "" } } }
  profiles/Default.json       name + the per-layer sections (the file shape of today's profiles)
  profiles/Rust.json
  games/                      left untouched after migration (section 5); never read again
```

`global.json` keeps its name and stays "the machine's own file": process-level
appearance plus the pointers. Assignments live there rather than in a separate file
so there is one place to look and one file to hand-edit.

---

## 3. The UI afterwards

One area, **Profiles** (`setup.profiles`). The **Per-game** area (`setup.pergame`)
is retired: under this model it would be one Choice row and one Action, and a rail
item that thin is a concept the user has to place. The palette keywords
("per-game", "this game", "override") point at the rows below. Appearance is
unchanged.

| Group | Row id | Control | What it does |
|---|---|---|---|
| Status | `profiles.status` | Facts | `editing: Rust -- every change is saved into it immediately` · `used by: this game, 1 other, and games without their own` · `game: app 252490 \| none identified` · `launch option: Comp (this session only)` when `--profile` was given |
| This game *(only with an app id)* | `profiles.game` | Choice | **This game uses** -- `Same as other games` first, then every profile. Changing it switches the session at once (the reload path Use triggers today: `BumpConfigGeneration()`, every panel's `EnsureConfigLoaded()` re-resolves and pushes live) and saves the assignment. |
| This game | `profiles.copy_for_game` | Action | **Give this game its own copy** -- saves the profile in use as `Game 252490` (rename later) and assigns it. One press, no toggle. Disabled with `already has its own` when assigned to a profile no other game uses. |
| All other games | `profiles.default` | Choice | **Games without their own choice use** -- the `default` pointer. Same switch-at-once behaviour when it is the one in effect. |
| Manage | `profiles.name` + `profiles.save` | Text + Action | **Save a copy as** -- copies the profile being edited to the new name and switches this game (or the default) to it. Validation as today (`SanitizeProfileName()`, "already exists"). |
| Manage | `profiles.rename_to` + `profiles.rename` | Text + Action | **Rename** the selected profile; assignments follow (`RenameProfile()` rewrites the pointers, not just `name`). Separate Text row because a registry Param cannot be Text (`Registry.cpp`'s `AddParam()`), as today. |
| Manage | `profiles.list` + `profiles.delete` | Choice + Action (Confirm) | **Delete** the selected profile. **Refused with a reason while any assignment points at it** (`in use by this game` / `in use by 2 games` / `it is the default`), so nothing ever has to fall back. The last remaining profile cannot be deleted. |
| Diagnostics | `profiles.facts` | Facts | count, `profiles/` path, last action. |

Rows that **stay** (renamed where noted): the Status facts (reworded), the profile
picker, Save as new (now "Save a copy as"), Rename + its Text row, Delete + confirm,
Diagnostics.

Rows that **go**, and why: `profiles.apply` Use this profile (nothing is copied any
more; the picker *is* the switch), `profiles.restore` Restore previous settings (no
wholesale replace to undo), `profiles.save_changes` (every edit is already in the
profile), `profiles.autosave` (always on, so not a switch), `config.override` Use
separate settings (replaced by the Choice + the copy Action), `config.start_from_profile`
(is the Choice), `config.restore`, `config.copy` Copy another game's settings (pick
that game's profile, or Save a copy as), `config.delete` Delete saved settings (a
game's own settings are a profile; Delete profile covers it), `config.routing` Using
(the Status row says it in one line). 19 rows today, 10 afterwards, one area instead
of two.

**Every area shows the profile.** `RoutedBadge()` (`PanelConfig.cpp`) already derives
an `Area::Badge` for the Setup areas; the same badge -- `Rust`, or `Comp (launch)`
-- goes on every area, or once in the shell header. That is the whole answer to
"the user should never have to think about where an edit goes": it is written above
the slider.

Toasts: switching profile (`Now editing 'Comp'`), creating a copy, and the launch
case (`Started with profile 'Comp' from the launch options`). Everything else is
silent, as edits are today.

---

## 4. Command line

```
gamescope --profile <name> [other flags] -- game
GS_RITZ_PROFILE=<name> gamescope -- game          # equivalent; the flag wins if both are set
```

Steam launch options: `gamescope --profile Comp -- %command%`, or, when the flag is
awkward (a wrapper script that owns argv, or `GS_RITZ_APPID`-style setups),
`GS_RITZ_PROFILE=Comp gamescope -- %command%`. `Why GS_RITZ_ and not GAMESCOPE_RITZ_:`
`GS_RITZ_APPID` (`src/Config/AppId.cpp`) is the env var users already set; the
`GAMESCOPE_RITZ_AB_*` family (`main.cpp`, `steamcompmgr.cpp`) is test tooling. The two
prefixes coexist today; the user-facing one is `GS_RITZ_`.

**Exact semantics (recommended):**

1. The name goes through `SanitizeProfileName()`; the file is `profiles/<name>.json`.
2. **It selects the profile for this session and nothing else.** The assignment on
   disk is untouched; the next launch without the flag is back to the assignment.
   The flag beats the assignment the way `-w`/`-h`/`-r` beat `nested_width` today
   (`apply_ritz_config_to_startup_state()` runs, then getopt overrides).
3. **Edits during the session go into that profile**, like any session. Nothing is
   session-only; nothing is lost at quit. The Status row and badge say
   `Comp (launch option)` so it is never a surprise.
4. **If the name does not exist, it is created** as a copy of the profile the game
   would otherwise have used (its assignment, else the default), with a toast:
   `Created profile 'Comp' from 'Default'`. `Why create:` `--profile Comp` in a launch
   option is the natural way to say "give this game a Comp setup"; a fallback to the
   shared profile would send that session's edits into the wrong file, which is the
   exact failure mode this design exists to remove. A typo produces a visible,
   deletable profile rather than an invisible fallback.
5. `--profile` combined with an existing per-game assignment: the flag wins for the
   session; the Per-game rows show both (`This game uses: Rust` and `launch option:
   Comp`), and choosing anything in the picker saves that as the assignment as usual.
   A one-press **Keep using this** is unnecessary: the picker already lists `Comp`.
6. A live switch is the same code path from the console: `gamescopectl ritz_profile
   <name>` (a `ConCommand` calling `config::SetSessionProfile()`), which is also what
   a future hotkey binds to.
7. Startup order: `ResolveAppId()` -> read `global.json` pointers -> `--profile`/env
   override -> `LoadProfile()` -> `apply_ritz_config_to_startup_state()` -> getopt.
   `--profile` must be known *before* the config is applied and the config is applied
   *before* getopt, so `main()` pre-scans `argv` for `--profile` / `--profile=` up to
   `--` (about fifteen lines); the option is still in `gamescope_options[]` so
   `--help` lists it and the loop's own case is a no-op. `--ritz-dump-config` gains a
   `session_profile` line.

**Why the alternatives lose:**

- *One-time copy into the live settings (today's Use, at launch):* every launch
  overwrites the previous session's edits; the profile stays pristine and the
  changes land in some other file. Restores the "where did it go" problem.
- *A session-only layer never written to disk:* edits vanish at quit; "every change is
  saved immediately" stops being true for exactly the sessions the user cares most
  about (the ones he bothered to name a profile for).
- *Persist it as the assignment / active profile:* a per-launch flag silently changing
  what the *next* flagless launch uses is the kind of hidden state this removes; and
  Steam launch options are per-game, so persisting globally is wrong by construction.
- *Refuse a missing name:* see point 4; a warning in a log nobody reads, and the
  session's edits in the wrong file.

---

## 5. Migration (read old, write new, no user action)

Triggered once, when `global.json` is read with `schema_version` 2 (or none). Written
as `Migrate_2_to_3()` beside `Migrate_1_to_2()` in `ConfigManager.cpp`, but at file
level rather than key level because it creates files. Rules:

| Old | New |
|---|---|
| `global.json` per-layer sections | `profiles/Default.json` (if a profile named `Default` already exists: `Default (migrated)`). `overlay` stays in `global.json`. |
| `global.json` `active_profile` + `auto_save_profile: true`, profile exists | `default` pointer = that profile. `Why:` with auto-save on the two files were kept identical by the fan-out, so the user was in effect already editing that profile. |
| `active_profile` with auto-save off, or no active profile | `default` pointer = `Default`. The active name was a breadcrumb; the values in `global.json` are what was running. |
| `profiles/*.json` | Unchanged. Their `schema_version` bumps on next write. |
| `games/<AppId>.json`, `override_global: true` | `profiles/Game <AppId>.json` + `games.<AppId>.profile = "Game <AppId>"`. Its `audio.manual_node_binary` -> `games.<AppId>.audio_node`. |
| `games/<AppId>.json`, `override_global: false` | `profiles/Game <AppId>.json` too (the values were deliberately kept, DECISIONS #19 amendment), but **not** assigned. The user sees it in the picker. |
| `last_applied_profile` (every file) | Dropped. Nothing is applied any more. |
| `auto_save_profile`, `active_profile`, `override_global` | Dropped after the rules above. |

The old `games/` files are **left in place, unread**. `Why not delete or rename:` the
user's own rule -- "never delete configs automatically" (DECISIONS #19, issue #43).
Re-running is prevented by `global.json` being schema 3 afterwards; a schema-2 file
is never looked at again once the pointers exist. A fresh install with no
`global.json` skips migration and creates `Default` from `Settings{}` on first write.
A newer schema than the build understands still falls back to defaults and logs, as
`ParseConfigFile()` does today.

---

## 6. What gets deleted from the code

`src/Config/ConfigSchema.h`: `Settings::last_applied_profile`, `active_profile`,
`auto_save_profile`; `AudioSettings` leaves `Settings` (its one field moves to the
game entry). Added: `ProfileAssignments { std::string default_profile; std::map<AppId,
GameEntry> games; }` on the global file only (same `bIncludeOverlay`-style gate).

`src/Config/ConfigManager.{h,cpp}` -- delete: `LoadPerGameOverride()`,
`ResolveEffective( oAppId )` (replaced by `SessionSettings()`),
`SnapshotPerGameOverride()`, `ClearPerGameOverride()`, `HasSavedPerGameConfig()`,
`RestorePerGameOverride()`, `DeletePerGameOverride()`, `ApplyProfile()`,
`ActiveProfile()`/`SetActiveProfile()`, `AutoSaveProfile()`/`SetAutoSaveProfile()`,
`FanOutToActiveProfile()`, `kProfileSections[]`, `ActiveProfileDirtySections()`,
`CurrentRoutedSettings()`, `EnqueuePerGameSnapshot()`, `IsSessionOverrideActive()`/
`SetSessionOverrideActive()`, `ListGameIds()`, `GamePath()`/`GamesDir()` (kept only
inside the migration), `RememberPerGame()`/`ForgetPerGame()` and the per-game mirror.
Keep: the path helpers, `SanitizeProfileName()`, `LoadProfile()`/`SaveProfile()`/
`RenameProfile()`/`DeleteProfile()`/`ListProfiles()`, `ConfigWriter` and its
coalescing, `EnqueueGlobalWrite()`/`EnqueueOverlayWrite()`/`EnqueueGeometryWrite()`,
`EnqueueProfileWrite()`, `EnqueueRoutedWrite()` (now: "write the session profile"),
`ConfigGeneration()`/`BumpConfigGeneration()`, `CurrentFullSettings()`,
`FlushPendingWrites()`, `DebugDumpEffective()`. Add: `SessionProfile()`,
`SetSessionProfile()`, `Assignments()`/`SetAssignment()`, `GameEntry()`.
`EnqueueRoutedWrite()` shrinks to: substitute the fresh `overlay`, write
`profiles/<session>.json`, done.

`src/Overlay/PanelConfig.h`: `SettingsBackup`, `BackupMatchesRouting()`,
`ChangesFact()`, `SavingFact()`, `UseConfirmPrompt()`, `SaveChangesBlocker()`,
`EditsGoTo()`, `OwnSettingsFact()`, `PerGameFilePath()`; `StatusInputs` shrinks to
app id, profile list, session profile, assignment of this game, default pointer,
launch-flag name. Keep `StatusHash()`, `ClampPickerSelection()`, `ProfileFact()`/
`GameFact()` (reworded).

`src/Overlay/PanelConfig.cpp`: `EnableOverride()`/`EnableOverrideNoToast()`,
`DisableOverride()`, `DeleteSavedPerGameConfig()`, `UseProfile()`,
`UseSelectedProfile()`, `RestorePreviousSettings()`, `StartFromProfile()`,
`CopySelectedGameConfig()`, `SaveChangesToActiveProfile()`, `SetAutoSave()`,
`BuildPerGameArea()`, `s_oBackup`, `s_bOverrideActive`, `s_sLastAppliedProfile`,
`s_nStartFromProfile`, `s_nSelectedCopyGame`, `s_OtherGameIds`. The Per-game
registration in `PanelConfig_RegisterAreas()`.

`src/main.cpp`: `ResolveEffective( oRitzAppId )` call becomes
`config::SessionSettings()` after the `--profile` pre-scan; `PanelSystem_SeedFromConfig()`
and every panel's `EnsureConfigLoaded()` call the same function (they currently call
`ResolveEffective( SessionAppId() )`, e.g. `PanelDisplay.cpp:144`).

Tests replaced: in `tests/test_config.cpp` the per-game snapshot/clear/restore/delete
cases, "active_profile and auto_save_profile round-trip", "a config predating Phase
B", "SetActiveProfile / SetAutoSaveProfile persist", "auto-save fans a routed write
out", "ActiveProfileDirtySections counts", "crosshair rides in a per-game snapshot
and in a profile"; in `tests/test_overlay_profiles.cpp` the backup, changes-fact,
saving-fact, Use-confirm, Save-changes-blocker and Phase B hash cases. Migration
tests are new (fixture files for each row of the table in section 5).

DECISIONS superseded: **#19** (no per-game snapshot; the rule that survives is "never
delete a config automatically", now enforced by Delete refusing while in use and by
migration leaving `games/` alone), **#20** (there is no apply; the profile is the
live source -- the opposite of #20, chosen on purpose, and the half of
`config-system.md`'s recommendation the original implementation declined), **#26**
(already moot). **#21** (app id resolution) and **#25** (toast placement global,
muting per-game-eligible) stand. TERMINOLOGY entries to rewrite: Profile, Per-game
settings, Use, Active profile, Auto-save (the last three become "removed 2026-09-xx,
see profiles-concept.md" notes for one release, then go).

Plainly: this is a **replacement of the model executed mostly as deletion**, not a
rewrite. The file layer loses about twenty functions and gains four; the panel loses
one area and eleven rows; the schema loses three fields and gains one small struct.
"Keep the current model and remove X and Y" was considered (section 7, alternative
A) and loses on all three criteria in the brief.

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

## 8. Open questions for the user

Only the ones that change the design:

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

## 9. Implementation sketch (one commit + test each)

1. **Schema 3 read/write, no behaviour change.** `ProfileAssignments` in
   `ConfigSchema.h`; serialise under `profiles` in `global.json` only; `Assignments()`
   / `SetAssignment()`. Test: round-trip, absent-key defaults.
2. **Migration 2 -> 3.** `Migrate_2_to_3()` per section 5, gated on the global
   file's version; fixture-driven tests for every row of the table (including the
   name-collision and the auto-save-on rule), and "games/ files are still there
   afterwards".
3. **Session resolution.** `SessionProfile()` (assignment -> default -> `Default`),
   `SessionSettings()`, `SetSessionProfile()` (bumps generation), `EnqueueRoutedWrite()`
   writing the session profile; delete the per-game API and the Phase B API and their
   tests. Every panel's `EnsureConfigLoaded()` and `main.cpp` call `SessionSettings()`.
   Test: routing writes the session profile and only it; switching reloads.
4. **`--profile` / `GS_RITZ_PROFILE` / `ritz_profile`.** Pre-scan in `main()`,
   option in the table, env fallback, create-if-missing with toast, `--ritz-dump-config`
   shows `session_profile`. Test: `test_config` for the resolver with an injected
   env/flag; a `--ritz-dump-config` run under a temp `XDG_CONFIG_HOME` for the
   end-to-end (the DECISIONS #21 verification pattern).
5. **Profiles area rewrite, Per-game area retired.** Rows per section 3; `PanelConfig.h`
   pure helpers trimmed; `test_overlay_profiles` re-pinned (hash inputs, Delete
   refusal wording, picker agreement). Laptop check: switch, copy, rename, delete
   refusal, launch option visible.
6. **Badge everywhere.** The session profile as every area's `Badge`, or in the
   shell header. Screenshot check.
7. **Audio node to the game entry.** `PanelAudio.cpp` reads/writes `GameEntry()`;
   `AudioSettings` leaves `Settings`; migration row covered in step 2's test.
8. **Docs.** `features/profiles-and-per-game.md` rewritten (rename to
   `profiles.md`), TERMINOLOGY, DECISIONS #19/#20 superseded notes plus a new entry,
   CHANGELOG `Added`/`Removed`/`Info` lines, README index.

Steps 1-4 are invisible to the user and can land first; the UI (5-6) is where the
change becomes real. Total: about the size of the 2026-09-05 Phase A + B work, with a
negative line count.
