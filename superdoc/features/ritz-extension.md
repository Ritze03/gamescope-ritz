# Ritz Extension

A [Ritz](https://ritze03.github.io/ritz/extensions.html) launcher module that wraps
**this fork's own binary** (`gamescope-ritz`, never upstream's `/usr/bin/gamescope`) so a
Ritz user can launch a game through it, pick a settings profile for that launch, and set
a handful of the fork's own and standard gamescope flags — all from Ritz's own UI,
without typing a command line.

Manifest: [`extensions/gamescope-ritz.json`](../../extensions/gamescope-ritz.json) (repo
root, one file — the "single manifest" shape the docs allow, no scripts). Offered by the
root installer (`install.sh`, see [build-and-tooling.md](build-and-tooling.md)) as an
optional copy into `~/.config/ritz/extensions/` on `--install`/`--update`/`--remove`;
never installed silently.

## Identity and why `ForkedFrom` is set

`Extension.Author` is `Ritze`, `Name` is `Gamescope Ritz`, `Version` is `1.0` (this is a
new module — nothing was shipped under this Author::Name before, so there is no upgrade
path to protect and no reason to start above `1.0`; Ritz's config is keyed by
`Author::Name::Version`, so a **future** version bump on this file, if the field set
ever changes shape, would leave a user's already-saved values keyed under the *old*
version and orphaned rather than migrated — bump the version only when that's actually
intended, and prefer adding new fields with sensible `Requires`-gated defaults over
bumping it for a compatible change).

`ForkedFrom` is set to `Ritze::Gamescope`, Ritz's own bundled module
(`~/.config/ritz/extensions/default/gamescope.json`). The docs describe `ForkedFrom` as
provenance/display only — it never affects config lookup — and it is honest here: this
manifest's `WRAPPERS` shape (`"<binary> {OPTIONS} --"`, `Priority: 100`, the same
`scaler`/`filter`/`fullscreen`/`force_windows_fullscreen` variable names) was copied
directly from the bundled module's own pattern and adapted for a different, forked
binary — the same relationship gamescope-ritz itself has to upstream gamescope.

## Fields (UI section "Gamescope Ritz")

In display order — `enabled` and `profile` are pinned first by explicit request, so the
profile field is always the second thing a user sees:

| Field | Variable | Flag | Verified against |
| --- | --- | --- | --- |
| Gamescope Ritz Enabled | `enabled` | *(gates the wrapper)* | — |
| Profile | `profile` | `--profile "<name>"` | `src/main.cpp`'s `profile` long option, handled by `ritz_prescan_profile_arg()`/`ritz_use_session_profile()` — see [profiles.md](profiles.md) |
| Nested Width (-w) | `nested_width` | `-w <n>` / `--nested-width` | `src/main.cpp` option table |
| Nested Height (-h) | `nested_height` | `-h <n>` / `--nested-height` | `src/main.cpp` option table |
| Nested Refresh (-r) | `nested_refresh` | `-r <n>` / `--nested-refresh` | `src/main.cpp` option table |
| Fullscreen (-f) | `fullscreen` | `-f` / `--fullscreen` | `src/main.cpp` option table |
| Force Maximize Nested Window | `force_windows_fullscreen` | `--force-windows-fullscreen` | `src/main.cpp` option table; see TERMINOLOGY.md's "Nested window" entry |
| Scaler (-S) | `scaler` | `-S <mode>` / `--scaler` | `src/main.cpp` usage string (`auto, integer, fit, fill, stretch`) |
| Filter (-F) | `filter` | `-F <mode>` / `--filter` | `src/main.cpp` usage string (`linear, nearest, fsr, nis, pixel`) |

Kept deliberately tight — this is a launcher module, not a mirror of every ConVar the
overlay exposes. Every field above maps to a flag confirmed present in this fork's own
`--help` output and option table; nothing here emits an unknown flag.

`--profile` and `--force-windows-fullscreen` are this fork's own additions over
upstream's option table (the rest are standard gamescope flags this fork also accepts).

## The profile-name / shell-split constraint

Ritz's wrapper pipeline space-joins each `WRAPPERS[].Builder[]` entry's rendered text
into `{OPTIONS}`, substitutes that into `CommandSyntax`, then **shell-splits the whole
string into argv** (per the extension docs). A raw, unquoted `--profile {profile}`
would silently break on a profile name containing a space: `My Profile` renders to
`--profile My Profile`, which shell-splits into three tokens — `--profile`, `My`,
`Profile` — and gamescope-ritz would try to launch `Profile` as the game command.

This manifest's builder entry instead emits `--profile "{profile}"` (the value is
wrapped in literal double quotes in the template). Verified by construction — building
the rendered string by hand and splitting it the way a shell would (`shlex.split`,
matching the documented "shell-split into argv" behaviour) — confirms the quoted form
keeps a spaced name as one token:

```
profile field: 'My Profile'
  rendered wrapper text: gamescope-ritz --profile "My Profile" -w 1920 --
  shell-split argv     : ['gamescope-ritz', '--profile', 'My Profile', '-w', '1920', '--']
```

Separately, gamescope-ritz's own argv parsing was exercised headlessly (isolated
`XDG_CONFIG_HOME`, `--profile "My Profile" --ritz-dump-config --help`, no display
needed since the profile is resolved before backend init) and correctly created and
loaded `profiles/My Profile.json`.

**Remaining constraint, stated in the field's own Description**: a profile name
containing a literal double-quote character is not supported — it would prematurely
close the quoted argument and break the launch. This wasn't testable through Ritz's own
UI in this pass (see "Pending: a real Ritz GUI check" below); a person can confirm it in
one try.

## Installer integration

`install.sh --install` detects Ritz by the presence of `~/.config/ritz/` and offers
(prompt, skippable, and `--with-ritz-extension` / `--no-ritz-extension` for
non-interactive use) to copy `extensions/gamescope-ritz.json` into
`~/.config/ritz/extensions/gamescope-ritz.json`. `--update` refreshes that file only if
it was previously installed and differs from the repo's copy. `--remove` offers to
delete only that one file — nothing else in `~/.config/ritz/extensions/` is ever
touched. See `install.sh`'s own `--help` for the exact flags.

## Pending: a real Ritz GUI check

`ritz --print %command%` needs its GUI and could not be driven headlessly in this pass
— see `superdoc/planning/PENDING-USER-TESTS.md`'s Ritz extension entry for the short,
concrete steps to confirm the assembled command in the real UI, including the one thing
this pass could only prove by construction rather than by running Ritz itself: a
profile name with a space surviving the wrapper's shell-split.
