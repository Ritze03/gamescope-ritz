# Settings export / import

**2026-11-07.** Share chosen settings with a friend, or copy settings from one profile to
another, as **JSON text through the clipboard**. Two dialogs in the Profiles area
(`setup.profiles`), on the second strip under the list: **Export** and **Import**. Logic:
`src/Config/ProfileExport.{h,cpp}` (tests `tests/test_profile_export.cpp`); the dialogs and every
decision about them: `src/Overlay/PanelConfig.{h,cpp}` (`panelconfig` namespace, tests
`tests/test_overlay_profiles.cpp`). Dialog parts (wide modal, panes, checkbox, multi-line text,
clipboard) are described in [overlay-shell.md](overlay-shell.md); profiles themselves in
[profiles.md](profiles.md).

`Why text only, no files:` the user: *"Export and import using manual json is fine, tbh. Dont do
file stuff."* There is no exports folder, no file name and no save button.

## Format

```json
{
  "gamescope_ritz_export": 1,
  "profile": "Comp",
  "settings": { "framegen": { "enabled": true }, "gamescope": { "fps_limit": 144 } }
}
```

`settings` nests keys exactly as a profile file does, so a value is addressed by its dotted key
(`gamescope.fps_limit`). `profile` is the source name (used to suggest a name on import). A
version above 1 is refused ("from a newer version").

## Groups

Keys are selected by **group**, a two-level tree (rail group, then area), `Groups()`. Every leaf
key of the serialised settings is in exactly one group or in `ExcludedPaths()`; a test fails on a
leaf in neither, so a new setting is always a conscious choice. Excluded: `overlay.*` (global.json,
never in a profile), `lag_buffer` (being withdrawn) and the metadata (`schema_version`, `name`,
`kind`, `app_id`, `game_name`, `inherits`). **Resolution** is a group that starts unticked: it
depends on the machine's display.

## Export dialog

760 wide, title `Export '<profile>'`, left **What to export** (the tree, starts from
`DefaultTicks()`; a parent shows Off / On / Mixed and sets all its children), right **Preview**
(read-only text of exactly what will be copied, recomputed on a change only). For a game profile
that inherits, **Include inherited values** (ticked by default): ticked writes the full resolved
values so the file works without the parent, unticked only what the profile changes. Primary
**Copy to clipboard** is disabled with nothing ticked; it copies, then toasts `Settings copied:
N groups`. When Clipboard sync is off a muted note says the copy stays inside gamescope.

## Import dialog

760 wide, a 2 x 2 grid. **Paste JSON** (editable field, a Paste button, Ctrl+V works) parses on every
change; an error shows inline in **orange** under the field. **Destination**: `+ New profile`
first, then every profile; a name field for a new one, prefilled from the pasted profile name and
made free (`Comp 2`). **What to import**: the same tree; groups the paste lacks are greyed and
never ticked, the rest start ticked except Resolution. **Changes**: `Group / setting: old -> new`
lines (labels from the registry, else the dotted key), the count, `N unknown values skipped`, and
for an existing destination `A backup of <Name> is kept as <Name>.json.bak`. The primary is
disabled until the JSON is valid, a group is ticked and (new profile) the name is usable, so an
invalid paste cannot reach `ApplyImport()`. A refusal at press time keeps the dialog open (toast
and inline line). The result is a toast naming destination, applied count and skipped count. The
new profile is **not** selected; the user picks it in the list. Importing into the session
profile bumps the config generation (every panel reloads) and runs the live-apply hook, so the
values take effect at once (`tests/test_overlay_profiles.cpp`).

When Clipboard sync (Settings > System) is off, the desktop clipboard does not reach gamescope,
so Paste only sees text copied inside it; the dialog says so.

## Keyboard

The overlay never enables ImGui nav, so each dialog owns a cursor over the tree rows
(`TreeCursorStep()` on `ListBoxStep()`, skipping greyed rows): Up/Down/Home/End move it, Space
toggles, Esc closes. Tree keys are ignored while a text field is active, and Enter never
confirms while the paste field has focus (the modal's own rule: no confirm with an active item).

## Why

- **Validated before anything changes.** The reference design applied first and complained later.
  `ParseExport()` rejects the whole text, and `ApplyImport()` re-validates, so nothing is touched
  on bad input.
- **Pasted values are checked like ConfigManager reads them.** An integer setting takes only an
  integer that fits an `int`, a float setting any finite number, bool and string their own type;
  anything else is unreadable and skipped, and text over 1 MB or nested deeper than 16 levels is
  refused before parsing (real exports nest 4). `Why:` the paste comes from other people, and a
  value ConfigManager would drop to its default (`144.5` for an integer) would show in Changes
  but write something else; deep nesting would cost the overlay thread on every keystroke.
- **A backup.** An import overwrites a profile's file; `<Name>.json.bak` (one per profile, the
  older overwritten) is written first, so overwriting is not a danger action and needs no second
  dialog.
- **Skipped values are shown.** Keys this build does not know, and values of the wrong type, are
  counted and listed in Changes instead of being dropped silently.
- **Orange, never red.** Red is for destructive confirmations only (styling guide).
- **No temp profile file.** Settings <-> sections goes through the pure
  `config::SettingsToSectionsJson` / `SettingsFromSectionsJson`; an earlier version wrote a
  throwaway file into the real `profiles/` directory, which could flash up in the list and be left
  behind by a crash.
