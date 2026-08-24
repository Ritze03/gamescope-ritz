# Changelog Maintenance

**Mandatory working rule for agents, linked (force-loaded) from the repo-root `CLAUDE.md`.**

`CHANGELOG.md` in the **repo root** is the user-facing "what changed" — shown in the
settings overlay's **Changelog** area (`src/Overlay/PanelChangelog.cpp`, parsed by
`src/Overlay/ChangelogParse.cpp`). It is for users, so write entries in plain
user-facing language, not implementation detail.

## The shape

The format is strict, because the overlay parses it:

```
# Changelog

<two- or three-line preamble naming the categories>

## YYYY-MM-DD

### Added
- **Short title**: one sentence of what changed for the user.
```

- **`## YYYY-MM-DD`** — the date heading. **No version number.** See
  [Why the date is the version](#why-the-date-is-the-version) below.
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
  your bullet to it under the right category; otherwise create a new block at the top.
  Never change a past block's date, and never rewrite past entries.
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

## Why the date is the version

The reference format this file was adapted from uses `## [x.y.z] – YYYY-MM-DD`, i.e. a
semver bump per block. **This project does not use version numbers**, per
[`documentation-version-policy.md`](documentation-version-policy.md): every dated block
is identified purely by its date, and there is nothing to bump.

That policy and this format agree rather than conflict, because in `gamescope-ritz` the
date *is* the version. The fork carries no tags and `project()` declares no version, so
the patch version this build reports — in `src/RitzVersion.h.in`, and in the overlay's
Changelog area next to the upstream base commit — is **HEAD's commit date** as
`YYYY-MM-DD`. Writing `## [2026-08-24] – 2026-08-24` would state the same fact twice, so
the heading is the bare date: `## 2026-08-24`.

**Never invent, import or bump a semver number here.** If one appears in this file, it
is wrong.

## What the parser guarantees

`ChangelogParse.cpp` classifies each line as a heading, a subheading, a bullet, a bullet
continuation, or plain text. Anything it cannot classify falls through as **plain text**
rather than being dropped, and a missing `CHANGELOG.md` degrades to an embedded
placeholder at build time. So a malformed entry renders as unstyled text — visibly
plain, never garbage, never a crash. That is a safety net, not a licence: keep to the
shape above.
