# Profiles and Per-game settings

The two Setup areas that decide *which file* your settings live in and let you
keep named copies of them. `src/Overlay/PanelConfig.{h,cpp}` (areas
`setup.profiles` "Profiles" and `setup.pergame` "Per-game"); the file layer is
`src/Config/ConfigManager.{h,cpp}`; tests in `tests/test_overlay_profiles.cpp`
(the pure half) and `tests/test_config.cpp` (the file layer).

## The model, in plain words

- Your settings are one file: `~/.config/gamescope-ritz/global.json`. **Every
  change is saved to disk immediately** -- there is no unsaved state to lose.
- A **profile** (`profiles/<Name>.json`) is a *named copy* of those settings that
  you saved on purpose. It does nothing by itself.
- **Using** a profile copies its values *into* your live settings, once. Editing
  the profile afterwards does not change anything that already used it, and
  editing your settings afterwards does not change the profile
  (`DECISIONS.md` #20). The Status row says `profile: <name>` afterwards -- that
  is *where the values came from*, not a live link.
- **Per-game settings** (`games/<AppId>.json`) give one game its *own copy* of the
  settings, loaded whenever that game starts. While they are on, every edit in
  every area goes to that file instead of `global.json` (the routing in
  `ConfigManager.h`: `SessionAppId()` / `IsSessionOverrideActive()` /
  `EnqueueRoutedWrite()`). Turning them off keeps the file on disk, switched
  off; only the confirmed **Delete** removes it (`DECISIONS.md` #19).

Both areas open with a **Status** group that states this outright, because the
old areas never did: the user's report (2026-09-05) was *"I'm almost scared to
touch it"*, and the fear was not knowing what was in use, where edits went, or
how to get back what you had.

## Profiles area (`setup.profiles`)

| Group | Row id | Title / verb | What it does |
|---|---|---|---|
| Status | `profiles.status` | In use *(Facts)* | `profile: <last applied \| none>`, `edits go to: global.json \| this game (games/<id>.json)`, `saving: every change is saved to disk immediately`, and -- only while a backup exists -- `backup: your settings from before '<X>'`. |
| Use a profile | `profiles.list` | Profile *(Choice)* | Picks a saved profile. **Always present**: with no profiles yet it is a disabled row reading "no profiles yet" with the reason "save one in the Profiles area first". |
| Use a profile | `profiles.apply` | **Use this profile** / `use` | Replaces the live settings with the profile's, after taking the one-step backup. No confirm -- the backup is the safety net. |
| Use a profile | `profiles.restore` | **Restore previous settings** / `restore` | Only exists after a Use this session. Writes the backup back to the file it came from and forgets it. |
| Save | `profiles.name` | New profile name *(Text)* | Validated live: character set, and "already exists". |
| Save | `profiles.save` | **Save as new profile** / `save` | Saves the current settings under the new name. The name appears in the picker **immediately** and is pre-selected; a toast confirms. |
| Diagnostics | `profiles.facts` | Profiles *(Facts)* | Count, the profiles directory, last action. |

## Per-game area (`setup.pergame`)

| Group | Row id | Title / verb | What it does |
|---|---|---|---|
| Status | `config.status` | This game *(Facts)* | `game: app <id> \| none identified`, `own settings: on -- ... \| off -- using the shared global settings`, `profile: <last applied \| none>`, `saved settings on disk: yes -- games/<id>.json \| no`. The bare "resolved app id" that used to sit at the bottom of Diagnostics moved here. |
| Settings for this game | `config.override` | **Use separate settings for this game** *(Switch)* | On: this game gets its own copy of the settings (restoring a saved-but-off file if one exists, else snapshotting the current settings). Off: back to global; the file is kept. Was "Override global config". |
| Settings for this game | `config.start_from_profile` | **Start from profile** / `use`, Param `profile` | Turns separate settings on if they were off, then copies the chosen profile into this game's file. Same backup as Use. This is "this game -> profile X" said once, in one place; it used to take the switch plus a trip to the Profiles area. |
| Settings for this game | `config.restore` | **Restore previous settings** / `restore` | Only exists after a Use/Start-from-profile that was taken *for this game's file*. Same function as `profiles.restore`. |
| Settings for this game | `config.copy` | Copy another game's settings / `copy`, Param `source` | Unchanged behaviour; now a synchronous write. |
| Settings for this game | `config.delete` | Delete saved settings / `delete...` (Confirm) | Unchanged -- the one destructive action, armed by a second press. |
| Diagnostics | `config.routing` | Using *(Facts)* | `settings file`, `set aside`, `if a value is missing`, config directory, last action. Was "Resolution order" with "2 layers". |

Every row has plain-language `.Help()`, `.Keywords()`, and a `.DisabledUnless()`
reason wherever it cannot act.

## The one-step backup ("Restore previous settings")

`panelconfig::SettingsBackup` (`PanelConfig.h`): the target file's `Settings`
exactly as they were the instant before a profile was copied over them, the
profile's name, and which file it was taken for. Held in `s_oBackup`, a file
static in `PanelConfig.cpp`, **in memory only** -- no new config field, no new
`ConfigManager` function. A second Use replaces it: one step back, never two.

Restore writes it back through the same `EnqueueRoutedWrite()` Use wrote through,
so it lands in the same file -- *provided the routing has not changed*. If the
user used a profile while on `global.json`, then switched separate settings on,
the routed path would now point at the per-game file; `BackupMatchesRouting()`
catches that and the row is disabled with a reason instead of writing the old
global values into the wrong file. Flip the switch back and Restore is live again.

`Why` a backup rather than a confirm: a confirm dialog on Use would make the
user answer "are you sure?" for a reversible action while still leaving them
with no way back once they said yes. The backup makes Use *safe*, which is what
removes the fear; a confirm only makes it *slow*. (Phase B adds a confirm gated
on the dirty count, for the case where there really is something to lose.)

## `Why:` the panel writes synchronously

`SaveCurrentAsNewProfile()` used to call `config::EnqueueProfileWrite()` -- a
queued write on a background thread -- and then `RefreshLists()` re-listed the
`profiles/` directory. The list was read before the file landed, so the cached
`s_ProfileNames` never moved, the `Rebuilds()` hash that is computed from it
never moved, and the area was never rebuilt: the new profile appeared on the
next *process*. Worse, the picker row was only registered when the list was
non-empty, so the very first profile left the area with no picker at all until
restart. That is the user's "I need to restart the whole game to actually see a
freshly created profile".

The fix is `config::SaveProfile()` -- synchronous, atomic (temp + fsync +
rename), returned before the list is read. `DisableOverride()` had always done
its one-off write inline with exactly this reasoning ("one click, not a slider
tick"); the `Enqueue*` family exists for the per-tick writes the *other* panels
make during a slider drag. The same latent race was in `EnableOverride()`'s
snapshot path (the Delete row hangs off `HasSavedPerGameConfig()`, read from disk
right after) and `CopySelectedGameConfig()`; both are synchronous now. The one
queued write left in the panel is Use/Restore's `EnqueueRoutedWrite()`, which
nothing re-reads afterwards -- the panel sets its own state from the value it
just wrote.

`tests/test_overlay_profiles.cpp` pins the sequence (save, list, hash moves) and
every hash input, so a row cannot silently stop appearing again.

## Rebuild inputs

Both areas are dynamic (`ui::Area::Rebuilds`) on `ConfigGenerationHash()`, which
is `panelconfig::StatusHash()` over `StatusInputs`: app id, override on/off,
saved-file-exists, the profile list, the other-games list, **the last applied
profile** (Status rows) and **the backup's presence and which file it was taken
for** (the Restore rows). The two lists are separated in the hash so a name
moving from one to the other cannot collide.

## Phase B (pending)

Everything below needs a new config field or `ConfigManager` function and is
**not** in the UI yet -- a row that does nothing must not exist. Seams are marked
`PHASE B SEAM` in `PanelConfig.cpp`.

- **Auto-save to profile** switch (`Settings::auto_save_profile`, global-only;
  fan-out in `EnqueueRoutedWrite()`). Decided: **off by default**; the active
  profile keeps its name while dirty. `Why:` on-by-default makes one profile used
  by several games change under every other game's future Use -- spooky action
  the config research argued against; off honours the user's "a toggle for
  auto-saving" as an opt-in.
- **Changes since applied** (dirty count) in both Status rows, always visible;
  needs `Settings::active_profile` and a diff against the profile file. The
  dirty-count-gated confirm on Use lands with it.
- **Save changes to profile** (needs `active_profile`).
- **Rename profile** (`RenameProfile`, sanitised both ends).
- **Delete profile** (`DeleteProfile`, containment-checked like
  `DeletePerGameOverride`, with `Confirm()`).
