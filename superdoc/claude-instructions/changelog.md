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

## [x.y.z] – YYYY-MM-DD

### Added
- **Short title**: one sentence of what changed for the user.
```

- **`## [x.y.z] – YYYY-MM-DD`** — a semantic version, an **en dash**, then the date.
  Every block is a bump relative to the block below it, sized per
  [`documentation-version-policy.md`](documentation-version-policy.md). See
  [Where the version number goes](#where-the-version-number-goes) below.
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
  your bullet to it under the right category; otherwise create a new block at the top
  and give it a fresh version bump. Never change a past block's date **or version**, and
  never rewrite past entries.
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

## Where the version number goes

**This file is the project's version marker.** The fork carries no tags and
`meson.build`'s `project()` declares no version, so there is no manifest to keep in step
with the changelog — which is the usual way the two drift apart and start contradicting
each other.

So the number is not copied anywhere. `Overlay/embed_changelog.py` reads the **top
block's** version at build time and emits it beside the embedded text, and the overlay's
Changelog area displays that. The version in the binary is therefore *derived from* this
file rather than kept in sync with it, and the two cannot disagree.

What that means when you write here:

- **Bump the top block and the build follows.** Adding a new block at the top with a new
  version is the whole of "update the version marker" — there is no second file.
- **Keep the heading exactly `## [x.y.z] – YYYY-MM-DD`.** The extractor wants a
  bracketed, three-part, all-numeric version on the first `## ` line. A heading it
  cannot read makes the build **fail loudly** rather than ship a binary whose version is
  a guess.
- **The version is not the whole identity.** The Changelog area shows three
  things and none substitutes for another: this semver (what the fork calls itself), the
  upstream gamescope commit it is built on (what it is a fork *of*), and HEAD's commit
  date (which build of it you are running). Those last two come from git via
  `src/RitzVersion.h.in`, not from here.

## What the parser guarantees

`ChangelogParse.cpp` classifies each line as a heading, a subheading, a bullet, a bullet
continuation, or plain text. Anything it cannot classify falls through as **plain text**
rather than being dropped, and a missing `CHANGELOG.md` degrades to an embedded
placeholder at build time. So a malformed entry renders as unstyled text — visibly
plain, never garbage, never a crash. That is a safety net, not a licence: keep to the
shape above.
