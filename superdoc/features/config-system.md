# Config system — JSON settings files, schema versioning and migrations

How gamescope-ritz stores settings on disk: one `global.json` plus one file per profile,
a plain-data schema header, a version number with a chain of migration steps, and a
background writer. The *profile model* (general vs game profiles, inheritance, selection,
the Profiles area) is a separate topic and lives in [profiles.md](profiles.md); this page is
the storage layer underneath it.

## How it works

- **Where the files are.** `config::ConfigRoot()` (`src/Config/ConfigManager.cpp`) is
  `$XDG_CONFIG_HOME/gamescope-ritz`, else `$HOME/.config/gamescope-ritz`. It re-reads the
  environment on every call, so tests can point it at a temporary directory. Inside:
  `global.json`, `profiles/<Name>.json`, and `games/<AppId>.json` — the last is a schema-2
  leftover that only the 2 -> 3 migration reads and is never deleted.
- **`global.json` versus a profile file.** `global.json` carries `overlay` (process-level
  UI preferences: appearance, panel geometry, keybinds, toast placement) and `profiles`
  (the pointers: `last_general`, and per app id `selected` / `audio_node` / `game_volume`).
  Every other section (`gamescope`, `fps_display`, `reshade`, `framegen`, ...) lives in a
  profile file, never in `global.json`. *Why:* an overlay preference such as UI scale is a
  property of the person at the keyboard, not of a game setup; keeping it out of profiles
  means a per-game profile cannot carry or override it, and the Appearance area can say
  "global only" truthfully (see [system-and-appearance.md](system-and-appearance.md)).
- **Plain-data schema, JSON elsewhere.** `src/Config/ConfigSchema.h` holds only structs
  (`Settings`, `OverlaySettings`, `ProfileMeta`, `GameAssignment`, `ProfileAssignments`, one
  `...Settings` struct per section) and has no JSON dependency. Serialization lives in
  `ConfigManager.cpp` (`SettingsFromJson`, `SectionsToJson`, `GlobalToJson`,
  `AssignmentsFromJson`). *Why:* every panel includes the schema header; keeping nlohmann
  out of it keeps JSON parsing out of every one of those translation units.
- **How a key and its default are declared.** A key is a struct field whose initializer is
  its compiled-in default (`bool low_latency_wakeup = true;`). Reading is one hand-written
  line per key in `SettingsFromJson` using `JGetBool` / `JGetInt` / `JGetFloat` /
  `JGetString` / `JGetOptInt`, each taking the struct's current value as the fallback;
  writing is the matching line in `SectionsToJson`. Where a value has a legal set or range,
  the reader validates (clamps, or keeps the default for an unknown string — e.g. the
  `motion_blur.relative` and `lag_buffer` readers). Panels pass the same default to
  `.Default( config::XSettings{}.field )` so the Inspector's reset chip and the file agree.
- **Absent means default, which is why additive changes need no bump.** A missing key,
  a wrong-typed key or a whole missing section all fall through to the struct default, so a
  new optional field is simply read with its default. An *empty* profile (schema version and
  metadata only) is therefore a valid "everything at defaults" file; `UseSessionProfile()`
  creates one for an unknown `--profile` name via `WriteEmptyProfileNow`.
- **Dropping a field is just not reading it.** A removed key (the FPS HUD's old backdrop and
  profiler keys, for instance) stays in old files until anything next rewrites that file;
  no migration is written for it. *Why:* a migration is only for a key whose *meaning* a
  later reader would get wrong.

## Schema version and migrations

`kCurrentSchemaVersion` (`ConfigSchema.h`, currently 5) is written as `schema_version` into
every file by every writer. `ParseConfigFile()` (`ConfigManager.cpp`) is the single entry
for reading a file and does, in order:

1. Parse without exceptions; a non-object or malformed file is logged and treated as absent
   (callers fall back to defaults, startup never blocks).
2. Read `schema_version` (missing = 0). **Newer than this build** is refused loudly and the
   file is treated as absent. *Why:* guessing at, or silently dropping, fields written by a
   future build would destroy a user's settings on a downgrade.
3. Run the steps whose range includes the file's version, oldest first:

| Step | Runs when version is | What it does |
| --- | --- | --- |
| `Migrate_1_to_2` | below 2 | `reshade.vibrancy.strength` moved from an additive -1..+1 boost to a 0..3 multiplier: adds 1.0 and clamps, so an untouched 0.0 does not turn into greyscale. |
| (2 -> 3) `MigrateV2ToV3` | below 3, **not** in the chain above | The Profiles v2 split: old `global.json` sections become a general profile, each `games/<AppId>.json` a game profile. Driven by `EnsureMigrated()` against `global.json` only. |
| `Migrate_3_to_4` | below 4 | Renames the `reshade.vibrancy` object to `reshade.saturation` (the effect kept its maths; a new, unrelated Vibrancy effect took the old name). |
| `Migrate_4_to_5` | below 5 | Splits the shared `reshade.dark_floor` into `reshade.adaptive_brightness.dark_floor` and `reshade.adaptive_gamma.dark_floor`, copying the value into both. |

- **Order matters.** `Migrate_1_to_2` must run before `Migrate_3_to_4` (it still looks under
  the old `vibrancy` key), and `Migrate_3_to_4` before `Migrate_4_to_5` (both touch the
  `reshade` object). The checks are `nVersion < N`, so a version-0 file takes every step.
- **A migration is in memory first.** The steps edit the parsed JSON; the file on disk is
  rewritten the next time the normal write path touches it, with the current version.
- **When to bump.** Bump `kCurrentSchemaVersion` for a rename, removal or meaning change
  that a reader of an old file would get wrong, add a `Migrate_N_to_M( nlohmann::json & )`
  next to the others, gate it in `ParseConfigFile`, and write the reasoning in the
  `kCurrentSchemaVersion` comment. A migration only runs when the old key is present and of
  the expected type, so most files pass through each step untouched.
- *Why a version chain at all:* it was built before there were files in the wild without one
  (the scouting doc, [config-system planning](../planning/config-system.md), argues it costs
  one integer now and is very expensive to retrofit).

## Writes, threads and merging

- **Atomic files.** `WriteFileAtomic` writes `<file>.tmp-<pid>`, fsyncs and renames.
  `SweepStaleTempFiles` removes a temp file left by a killed process, but only once
  `kill(pid, 0)` says that pid is gone; on any doubt it leaves the file alone.
- **No disk I/O on the paint thread.** `EnqueueRoutedWrite`, `EnqueueGlobalWrite`,
  `EnqueueOverlayWrite`, `EnqueueGeometryWrite` and `EnqueueProfileWrite` hand the text to a
  background writer. Writes are coalesced per path (`kWriteCoalesceMs` of quiet, capped at
  `kWriteCoalesceMaxMs`), so a slider drag costs one write per pause. `FlushPendingWrites()`
  is for shutdown and tests; CRUD and `SelectProfile()` are synchronous.
  *Why:* the overlay draws inside the vblank-paced compositor frame, where one fsync would
  show up as a late frame.
- **Reads are mirrored.** `LoadGlobal()` and `ResolvedSettings()` return the in-process
  mirror of the last write when this process made one, else the file; they are cheap but not
  per-frame calls. Panels cache a copy and reload when `ConfigGeneration()` moves — the
  `EnsureConfigLoaded()` / `EnsureGeneralSettingsLoaded()` pattern in each `Panel*.cpp`.
  `ConfigGeneration()` is bumped by select, create, edit, delete and reset-to-inherited,
  never by an ordinary slider edit.
- **Writes are merged, not whole.** Many panels each keep their own `Settings` copy, so
  writing one whole would undo the others' edits made since its last reload.
  `EnqueueRoutedWrite` merges *per section* (`CallerEditMerge`) and `EnqueueGlobalWrite`
  merges *per field* of `overlay`, identifying the caller by the address of its persistent
  struct. Callers must pass that long-lived struct, not a temporary.
- **Inheritance on disk.** A game profile with a parent stores only keys that differ:
  `SparseDiff` on write, `DeepMerge` onto the parent on read. A value set equal to the
  parent's is stored as nothing and reads as inherited. Semantics are in
  [profiles.md](profiles.md).

## Using it

- `--ritz-dump-config` prints `DebugDumpEffective()`: the session profile, its parent, the
  rule that chose it and the resolved settings, as JSON.
- Profile names are user input and become a path component, so `SanitizeProfileName()`
  keeps only `[A-Za-z0-9 _-]`; no separator or `.` can survive.
- Tests: `tests/test_config.cpp` (`[config]` cases, each against a temporary config home;
  `ResetSessionRoutingForTests()` resets the cached session state).

## Related links

- [profiles.md](profiles.md) — the profile model, selection, migration to Profiles v2
- [system-and-appearance.md](system-and-appearance.md) — the `overlay` section's main editor
- [config-system planning scout](../planning/config-system.md) — the original design
  reasoning; written before the profile model, so its per-game file layout is superseded
