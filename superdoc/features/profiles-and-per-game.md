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
- The **active profile** (`Settings::active_profile`, global.json only) is the
  profile you last chose to work against -- set by **Use this profile**, **Start
  from profile** and **Save as new profile**, renamed with the profile, cleared
  when it is deleted. It *keeps its name while your settings drift away from
  it*; the Status row shows the drift as `changes since applied: N sections
  changed` instead. `Why:` making the name vanish on the first edit would hide
  it exactly when the user is looking for it.
- **Auto-save to profile** (`Settings::auto_save_profile`, global.json only,
  **off by default**): when on, every edit you make while a profile is active is
  also written *into* that profile, so it follows you. Off, the profile keeps
  its old values until you press **Save changes to profile**. `Why off:` on by
  default, one profile Used by several games would change under every other
  game's future Use the moment any of them touched a slider -- the "spooky
  action" the config research argued against; off honours the user's own "a
  toggle for auto-saving" as opt-in (`DECISIONS.md` #20, Phase B).
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
| Status | `profiles.status` | In use *(Facts)* | `profile: <active \| none>`, `changes since applied: none \| N section(s) changed \| n/a -- no profile is active`, `edits go to: global.json \| this game (games/<id>.json)`, `saving: every change is saved to disk immediately[, and automatically into the profile \| ; the profile only when you press Save changes]`, and -- only while a backup exists -- `backup: your settings from before '<X>'`. |
| Use a profile | `profiles.list` | Profile *(Choice)* | Picks a saved profile. **Always present**: with no profiles yet it is a disabled row reading "no profiles yet" with the reason "save one in the Profiles area first". Rename and Delete below act on this selection too. |
| Use a profile | `profiles.apply` | **Use this profile** / `use` | Replaces the live settings with the profile's, after taking the one-step backup, and makes it the active profile. **Confirms only when there is something to lose**: the live settings differ from the active profile *and* auto-save is off -- then the verb reads `discard N unsaved change(s)?` and needs a second press. Clean, or auto-save on: a plain press, the backup is the safety net. |
| Use a profile | `profiles.restore` | **Restore previous settings** / `restore` | Only exists after a Use this session. Writes the backup back to the file it came from, puts the active profile back to what it was before the Use, and forgets the backup. |
| Save | `profiles.name` | New profile name *(Text)* | Validated live: character set, and "already exists". |
| Save | `profiles.save` | **Save as new profile** / `save` | Saves the current settings under the new name and makes it the active profile. The name appears in the picker **immediately** and is pre-selected; a toast confirms. |
| Save | `profiles.save_changes` | **Save changes to profile** / `save` | Writes the current settings into the *active* profile, replacing what it had. No confirm. Disabled with exactly one of three reasons: `no profile is active`, `auto-save is on -- already saved`, `nothing has changed`. |
| Save | `profiles.autosave` | **Auto-save to profile** *(Switch)* | See "The model" above. Turning it on first saves the current settings into the active profile, so it starts clean rather than carrying existing drift until the next edit. Disabled with `no profile is active` when none. |
| Manage selected profile | `profiles.rename_to` | New name *(Text)* | The rename target; same validation as `profiles.name`. Its own row, not a Param on Rename: a registry Param can only be a Switch, Choice or Slider (`Registry.cpp`'s `AddParam()`), never a Text control. |
| Manage selected profile | `profiles.rename` | **Rename profile** / `rename` | Renames the *selected* profile to the typed name (`config::RenameProfile()`, containment-checked both ends, refuses to overwrite an existing profile). If it was the active profile, the active name follows; the picker re-selects the new name. |
| Manage selected profile | `profiles.delete` | **Delete profile** / `delete...` (Confirm `delete permanently?`) | Deletes the *selected* profile (`config::DeleteProfile()`). Clears the active profile if it was the one deleted. The settings you are running are not affected -- a profile is a saved copy. |
| Diagnostics | `profiles.facts` | Profiles *(Facts)* | Count, the profiles directory, last action. |

## Per-game area (`setup.pergame`)

| Group | Row id | Title / verb | What it does |
|---|---|---|---|
| Status | `config.status` | This game *(Facts)* | `game: app <id> \| none identified`, `own settings: on -- ... \| off -- using the shared global settings`, `profile: <last applied \| none>`, `saved settings on disk: yes -- games/<id>.json \| no`, `changes since applied: <same fact as the Profiles area>`. The bare "resolved app id" that used to sit at the bottom of Diagnostics moved here. |
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
removes the fear; a confirm only makes it *slow*. The one confirm Use does
carry is gated on the dirty count with auto-save off -- the case where the thing
being replaced was never saved anywhere but the live file.

The backup also records the active profile at the time (`sPreviousActiveProfile`)
and Restore puts it back *before* its routed write. `Why:` Restore writes through
the same `EnqueueRoutedWrite()` Use did, and with auto-save on that write fans
out to the *active* profile -- without restoring the name first, "undo using X"
would write the old settings into X.

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
profile** (Status rows), **the backup's presence and which file it was taken
for** (the Restore rows), and -- Phase B -- **the active profile, the auto-save
switch and the dirty count**. The last three decide, at build time, the Status
facts, Save changes' disabled reason and whether Use carries a confirm. The two
lists are separated in the hash so a name moving from one to the other cannot
collide.

All three Phase B inputs are in-memory reads, so the hash function can run every
frame the area is shown without touching the disk: the active profile and
auto-save come from `ConfigManager`'s mirror of global.json, and the dirty count
is cached on a mutation sequence (see below).

## The dirty count ("changes since applied")

`config::ActiveProfileDirtySections()`: how many of the sections a profile
carries differ between what the user is running and the active profile's file.
`std::nullopt` when no profile is active or its file cannot be read; `0` clean;
`N` otherwise. Surfaced as `panelconfig::ChangesFact()`.

- **What is compared.** Exactly the sections `ApplyProfile()` copies --
  `gamescope`, `fps_display`, `crosshair`, `reshade`, `notifications`, `system`.
  Not `audio` (`ApplyProfile` never copies it, so it would read as a permanent
  "1 section changed" against any profile saved from another game), not
  `overlay` (global-only), not `last_applied_profile` (provenance). The two
  lists are kept in step by a comment at each.
- **How.** Both sides go through `SettingsToJson(x, false)` and each section's
  canonical dump is compared as text. `Settings` has no `operator==`, and the
  JSON form is what both sides are made of anyway, so equal values dump equal.
- **Which "live".** `config::CurrentRoutedSettings()` -- the in-memory mirror of
  the last routed write, falling back to disk only when nothing has been written
  this process. `Why:` since coalescing (below) a disk read can be up to half a
  second behind the slider the user just moved; the mirror never is. The same
  goes for the profile side: the last profile this process wrote is mirrored
  too, so an auto-save fan-out still sitting in the queue already counts as
  saved.
- **When.** Cached on a **mutation sequence** every write path in
  `ConfigManager.cpp` bumps (queued or synchronous, global/per-game/profile,
  rename/delete, the two session setters). The recompute -- one profile-file
  read at most -- only happens after something changed, so
  `ConfigGenerationHash()` can ask every frame and still notice a slider moved
  in another area the moment the user comes back to Profiles. Never per frame.

## Auto-save: the fan-out

`EnqueueRoutedWrite()` is the single funnel every panel's edits go through, so
that is where auto-save lives (`FanOutToActiveProfile()`): when
`AutoSaveProfile()` is on and `ActiveProfile()` names a profile, the same
settings are *also* queued as a profile write to it -- in **both** routing
branches, since "any setting you change while a profile is active" does not
depend on which file the change itself lands in. The profile write drops
`overlay` and the session fields as every profile write does, so a profile never
learns which profile is active (itself) or that auto-save is on.

Where the session fields live: `active_profile` / `auto_save_profile` are read
through `config::ActiveProfile()` / `AutoSaveProfile()` (the global.json mirror,
no disk read) and written through `SetActiveProfile()` / `SetAutoSaveProfile()`
(a global write via the background writer). They are **never routed** into a
per-game file, and `EnqueueRoutedWrite()`'s global branch substitutes the live
values for whatever stale copy the calling panel loaded at open time -- the same
protection `overlay` already had, for the same "General-tab edit didn't stick"
class of bug.

## Write coalescing

`ConfigWriter` (the background writer in `ConfigManager.cpp`) now coalesces,
which the header had anticipated since M0:

- a write queued for a path that already has one pending **replaces** it (last
  wins, position kept);
- the worker waits **50 ms of quiet** before taking a batch, capped at **500 ms**
  since the oldest pending write (so a long drag loses at most that much to a
  crash, not the whole drag);
- `FlushPendingWrites()` skips the quiet period.

`Why now:` with auto-save on, every routed write is two writes. A slider drag
that used to cost one `fsync`+`rename` per tick would have cost two; it now
costs one per file per pause. It is also why the panel and the dirty count read
in-memory mirrors rather than the disk (above).

## Startup

Nothing to do: the session fields are read lazily from global.json the first
time anything asks (`CurrentFullSettings()`), and an old config with neither key
parses to "no profile active, auto-save off".
