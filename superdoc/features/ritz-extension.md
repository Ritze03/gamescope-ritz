# Ritz Extension

A [Ritz](https://ritze03.github.io/ritz/extensions.html) launcher module that wraps
**this fork's own binary** (`gamescope-ritz`, never upstream's `/usr/bin/gamescope`).

Manifest: [`extensions/gamescope-ritz.json`](../../extensions/gamescope-ritz.json) (repo
root, one file — the "single manifest" shape the docs allow, no scripts).

## What this actually is

**This is the user's own, pre-existing "Gamescope-Ritz" Ritz module** (`Author` `Ritze`,
`Name` `Gamescope-Ritz`, `Version` `1.1`) — the one they had already built for
themselves in Ritz, covering every General/Resolution/Sync & Input/Upscaling/HDR/Nested
Window/Embedded Display (DRM)/Cursor/Integration/ReShade/VR Overlay/Debug section this
fork's `--help` exposes — with **exactly one field added**: a free-text **Profile**
box, inserted right after the "Gamescope Enabled" toggle in the General section, plus
the one `WRAPPERS` builder entry that makes it emit `--profile "<name>"`.

This is not the file's original history. A first pass (2026-09-08) misread the request
and wrote a brand-new, deliberately minimal nine-field module from scratch instead of
touching the user's real one — wrong instruction-following, corrected the same day. The
lesson, so it isn't relearned: when asked to add one field to an *existing* module,
start from that module verbatim and touch only what was asked, never redesign it.
Nothing here is "kept tight" by choice — it is exactly as large as the user's own
module always was.

## Fields (UI section "General")

In display order — `enabled` and `profile` are pinned first, so the profile field is
always the second thing a user sees, directly under the enable toggle:

| Field | Variable | Flag |
| --- | --- | --- |
| Gamescope Enabled | `enabled` | *(gates the wrapper)* |
| Profile | `profile` | `--profile "<name>"` |
| Fullscreen | `fullscreen` | `-f` |
| Backend | `backend` | `--backend <mode>` |
| Scaler | `scaler` | `-S <mode>` |
| Mouse Sensitivity Multiplier | `mouse_sensitivity` | `--mouse-sensitivity <n>` |
| MangoApp Overlay | `mangoapp` | `--mangoapp` |

The remaining ~70 fields, across the other eleven UI sections (Resolution, Sync &
Input, Upscaling, HDR, Nested Window, Embedded Display (DRM), Cursor, Integration,
ReShade, VR Overlay, Debug), are the user's own pre-existing module content, unchanged
— see the manifest itself for the full field list rather than duplicating it here.

## Version note

`Version` stays `1.1` — the version the user's module already carried. Ritz keys stored
per-user values by `Author::Name::Version`, so bumping it here would orphan every value
they'd already set across their games; adding a field with a `Requires`-gated default
(as done here) is the compatible way to extend a module without a version bump. Bump it
only if a future change actually needs a fresh keyspace.

## The profile-name / shell-split constraint

Ritz's wrapper pipeline space-joins each `WRAPPERS[].Builder[]` entry's rendered text
into `{OPTIONS}`, substitutes that into `CommandSyntax`, then **shell-splits the whole
string into argv** (per the extension docs). A raw, unquoted `--profile {profile}`
would silently break on a profile name containing a space: `My Profile` renders to
`--profile My Profile`, which shell-splits into three tokens — `--profile`, `My`,
`Profile` — and gamescope-ritz would try to launch `Profile` as the game command.

This manifest's builder entry instead emits `--profile "{profile}"` (the value is
wrapped in literal double quotes in the template, matching the style of this module's
other quoted string fields like `--mura-map "{mura_map}"` and `--cursor
"{cursor_image}"`). Verified by construction — building the rendered string by hand and
splitting it the way a shell would (`shlex.split`, matching the documented "shell-split
into argv" behaviour) — confirms the quoted form keeps a spaced name as one token:

```
profile field: 'My Profile'
  rendered:    --profile "My Profile"
  shlex.split: ['--profile', 'My Profile']
```

**Remaining constraint, stated in the field's own Description**: a profile name
containing a literal double-quote character is not supported — it would prematurely
close the quoted argument and break the launch.

## Installer integration

`install.sh --install` detects Ritz by the presence of `~/.config/ritz/` and offers
(prompt, skippable, and `--with-ritz-extension` / `--no-ritz-extension` for
non-interactive use) to copy `extensions/gamescope-ritz.json` to
**`~/.config/ritz/extensions/ritze__gamescope_ritz.json`**.

That destination name deliberately does **not** match the source file's own name. Ritz
names an author's module file `<author>__<name>.json` (lowercased, spaces/hyphens to
underscores), so the user's real "Gamescope-Ritz" module by "Ritze" already lives at
`ritze__gamescope_ritz.json`. Installing under the source's own `gamescope-ritz.json`
name instead would create a **second** module with the same `Author::Name::Version`
identity sitting next to the first — exactly the duplication this fork is trying not to
cause. Targeting the same filename makes an install an **in-place overwrite** of that
one module rather than a second copy, and the prompt says so plainly (and defaults to
declining) whenever a file is already there.

`--update` refreshes that file only if it was previously installed and differs from the
repo's copy. `--remove` offers to delete only that one file — nothing else in
`~/.config/ritz/extensions/` is ever touched — and, because the destination name is
shared with the user's own pre-existing module rather than something guaranteed to have
been installed by this script, the remove prompt defaults to **declining** and says so,
rather than assuming ownership. See `install.sh`'s own `--help` for the exact flags.
