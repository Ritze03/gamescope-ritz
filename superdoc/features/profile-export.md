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

760 wide, a 2 x 2 grid. **Source** (top left) has a Multiselector, `Paste JSON | From profile`.
*Paste JSON*: an editable field and a Paste button (Ctrl+V works); it parses on every change and an
error shows inline in **orange** under the field. *From profile*: a list of the existing profiles
(same labels as the destination list); picking one calls
`ExportSettings(name, every group, bResolved = true)` in memory and feeds that text through the
same `ParseExport()` path as a paste, message `Settings of '<name>': N groups`. Each source keeps
its own text, so switching back and forth loses nothing. Everything below the Source pane is
the same for both.

**Destination**: a multi-select list, `+ New profile` first, then every profile. Click selects
only that row; Ctrl+click toggles a row; `+ New profile` is exclusive (clicking it, with or
without Ctrl, selects only it; Ctrl+clicking an existing profile replaces it); the selection is
never empty. Up/Down/Home/End stay single-select. Below the list: for a new profile a name field,
prefilled from the pasted profile name and made free (`Comp 2`), with an error line only while
the name is refused; otherwise `Imports into '<Name>'` or `Imports into N profiles`. Importing
into the source profile itself is allowed (it shows 0 changes).
**What to import**: the same tree; groups the source lacks are greyed and never ticked, the rest
start ticked except Resolution. **Changes**: per destination a headline (count, plus `A backup of
<Name> is kept as <Name>.json.bak` for an existing one), then `Group / setting: old -> new` lines
(labels from the registry, else the dotted key), `N unknown values skipped`; one destination
reads as a single block. The primary is disabled until the JSON is valid, a group is ticked and
(new profile) the name is usable, so an invalid source cannot reach `ApplyImport()`.

The press checks **every** destination first (`CheckImport()`, the dry run of `ApplyImport()`),
and only if all pass writes each one (`panelconfig::ApplyToAll()`). A refusal keeps the dialog
open (toast and inline line); if a write fails midway the message names the profile and how
many were already written. The result is a toast naming destination, applied count and skipped
count, or `Imported into N profiles: ...`. The new profile is **not** selected; the user picks
it in the list. Importing into the session profile bumps the config generation (every panel
reloads) and runs the live-apply hook, so the values take effect at once
(`tests/test_overlay_profiles.cpp`).

Layout: the paste strip and the destination note are sized to their content (chip / message
lines; one note line, or the Name row plus an error line only when there is an error), so the
field and the list fill their pane.

When Clipboard sync (Settings > System) is off, the desktop clipboard does not reach gamescope,
so Paste only sees text copied inside it; the dialog says so.

## Keyboard

The overlay never enables ImGui nav, so each dialog owns a cursor over the tree rows
(`TreeCursorStep()` on `ListBoxStep()`, skipping greyed rows): Up/Down/Home/End move it, Space
toggles, Esc closes. Over the Source and Destination lists, Up/Down/Home/End/Enter go to the
hovered list and the tree ignores them; the mouse wheel scrolls a hovered list with no click
needed. Ctrl+click is the multi-select gesture in the Destination list (a plain
`ListBox` ignores it). Tree keys are ignored while a text field is active, and Enter never
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
- **The source profile is exported with inherited values.** *From profile* uses
  `bResolved = true`, the Export dialog's default: a game profile that inherits stores only its
  differences, and importing that sparse text would copy almost nothing into a general profile.
- **Validate all destinations before writing any.** One bad destination (a profile deleted
  meanwhile, a refused name) must not leave several others already overwritten; only a disk
  failure during the writes can still stop midway, and that case is reported with the count.
- **From profile feeds the paste path, not a second one.** Parse, ticks, Changes and Apply stay
  identical, and nothing is written to disk to hand the text over.
- **A list scrolls away from its selection.** `ListBox` used to pull the selected row back into
  view every frame, undoing each wheel notch (a list whose selection was off-screen could not be
  scrolled until clicked). It now does that only on the frame the selection moved
  (`ListBoxScrollTop()`).
