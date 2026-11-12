# Changelog Maintenance

**Mandatory working rule for agents, linked (force-loaded) from the repo-root `CLAUDE.md`.**

`CHANGELOG.md` in the **repo root** is the user-facing "what changed" — shown in the
settings overlay's **About** area (`src/Overlay/PanelChangelog.cpp`, parsed by
`src/Overlay/ChangelogParse.cpp`; the area was labelled "Changelog" until
2026-09-09 and its id is still `system.changelog`). It is for users, so write
entries in plain user-facing language, not implementation detail.

## The shape

The format is strict, because the overlay parses it:

```
# Changelog

<short preamble naming the categories>

## YYYY-MM-DD

### Added
- **Short title**: one sentence of what changed for the user.
```

- **`## YYYY-MM-DD`** — the date of the block, nothing else: **no version number, no
  bumping** (date-based policy, see
  [`documentation-version-policy.md`](documentation-version-policy.md)). Old blocks of the
  form `## [x.y.z] – YYYY-MM-DD` stay as they are; the parser accepts both.
- **`### Added` / `### Fixed` / `### Removed` / `### Info`** — the only four categories.
  *Added* = new features, *Fixed* = bug and behaviour fixes, *Removed* = things taken
  out, *Info* = notes worth knowing (reorganisations, renames, policy). Omit a category
  that has no entries. Do not invent a fifth.
- **`- **Short title**: one sentence.`** — bold lead-in, colon, then **one** plain
  sentence. Two sentences only when the first would otherwise lie (e.g. a fix that
  changes what a stored value displays as).

## Rules

- **Maintain it as you work.** A user-facing change gets a bullet under the right
  heading in **today's** dated block, in the **same commit** as the change.
- **One block per calendar day, newest on top.** If today's block already exists, add
  your bullet to it under the right category; otherwise create a new `## YYYY-MM-DD` block at
  the top. Never change a past block's heading, and never rewrite past entries.
- **Be ruthless.** One line per change. What changed *for the user*, not how it was
  implemented. Reasoning, mechanism, measurements, issue archaeology and rejected
  alternatives all belong in the commit message, in `superdoc/`, or in the relevant
  `AUTONOMOUS-DECISIONS.md` — never here. If a bullet needs a paragraph to justify
  itself, the paragraph goes in the docs and the bullet stays one line.
- **No agent-facing content.** No instructions, no policy readings, no notes to the
  next agent, no build or branch procedure. This file is read by users.
- **Purely internal work gets no entry.** A refactor, a test, a doc change or a build
  tweak with no user-visible effect adds nothing here — and a day of only such work
  adds no block.
- **Hard-wrap at 88 columns**, with continuation lines indented **two spaces**. The
  overlay's content body does not word-wrap, so an unwrapped 200-character bullet would
  need horizontal scrolling to read. The parser rejoins a two-space-indented line to the
  bullet above it, so the wrap is invisible on screen.
- **Inline `**bold**` is only for the lead-in.** The parser strips `**` wherever it
  appears, so bold in the middle of a sentence renders as plain text — do not rely on
  it. Backticks are fine and are kept verbatim.

## What version the build reports

**This file is the project's version marker.** The fork carries no tags and
`meson.build`'s `project()` declares no version, so the number is not copied anywhere.
`Overlay/embed_changelog.py` reads the **top block's** heading at build time and emits it
beside the embedded text; the overlay's About area displays that. The version in the binary
is therefore *derived from* this file and cannot disagree with it.

- **Date-only heading (`## 2026-11-12`)**: the build reports the date, `2026-11-12`.
- **Legacy heading (`## [0.14.14] – 2026-11-09`)**: the build reports `0.14.14`. Only
  matters while no date-only block sits above it.
- **Anything else** as the first `## ` heading makes the build **fail loudly** rather than
  ship a guessed version.
- **The version is not the whole identity.** The About area also shows the upstream
  gamescope commit this fork is built on and HEAD's commit date; those come from git via
  `src/RitzVersion.h.in`, not from here.

## What the parser guarantees

`ChangelogParse.cpp` classifies each line as a heading, a subheading, a bullet, a bullet
continuation, or plain text. Anything it cannot classify falls through as **plain text**
rather than being dropped, and a missing `CHANGELOG.md` degrades to an embedded
placeholder at build time. So a malformed entry renders as unstyled text — visibly
plain, never garbage, never a crash. That is a safety net, not a licence: keep to the
shape above.
