# About — `system.changelog`

The **About** area of the **Shell** (the settings overlay): what you are running, what changed, and what it is made of.
It sits in the OTHER rail group and hands a list of lines to the Shell's content body, which draws and scrolls them.
Its sibling is [log.md](log.md); both were one page until 2026-11-07 (text moved verbatim).


Code: `src/Overlay/PanelChangelog.cpp`, `src/Overlay/ChangelogParse.{h,cpp}`,
`src/Overlay/embed_changelog.py`, `src/RitzVersion.h.in`. The area was labelled "Changelog"
until 2026-09-09 and is "About" since; the **id stayed** `system.changelog`.
*Why:* an id is not a display string. It keys the rail grouping (`UI/Registry.cpp`), the icon
table (`UI/Icons.cpp`), palette state and the settings-audit rows, and no user sees it; the
label and keywords carry the rename, with both "about" and "changelog" searchable in the
command palette.

It answers three questions on one page:

1. **What am I running.** Three identity facts, none substitutes for another:
   - the gamescope-ritz semver (`g_Changelog_Version`);
   - the upstream base commit this fork is built on (`k_szRitzBaseCommit`, with
     `k_bRitzBaseVerified`: checked against git at configure time; if the recorded base is no
     longer an ancestor the row says "unknown" instead of printing a stale hash);
   - HEAD's commit date (`k_szRitzPatchDate`, with a trailing `+` when `k_bRitzDirty`).
   Plus gamescope's own reported version string (`k_szGamescopeVersion`).
   *Why a commit date, not a build time:* two builds of the same source then agree.
2. **What changed.** `CHANGELOG.md` rendered from the embedded text.
3. **What it is made of.** The licences of everything compiled into the binary, printed below
   the changelog: the fork's own `LICENSE`, `THIRD-PARTY-LICENSES.md`, and the Geist fonts'
   `LICENSE-OFL.txt`. These are separate byte arrays (assets embedded by `src/meson.build`)
   so each stays the one authoritative file in the tree. *Why embedded:* the OFL only permits
   shipping the font data if the licence travels with it, and a file installed beside the
   binary can be moved or lost. System libraries (wlroots, Vulkan, LuaJIT and the like) are
   loaded dynamically and are named but not reproduced. *Why on this page:* the user's call
   (rename Changelog to About and add the licences); a twelfth rail item nobody reads would be
   a worse home for text that has to be reachable.

## The changelog is the version marker

There is no other version marker in the repo (no tags, `project()` declares none).
`embed_changelog.py` reads the first `## [x.y.z] – YYYY-MM-DD` heading with a strict regex,
and the build **fails** if the newest heading cannot be read. The overlay displays that
number. *Why:* a second place to write the version would eventually disagree with the
changelog; deriving it makes disagreement unrepresentable. A missing `CHANGELOG.md` builds
with an embedded placeholder and the version shows "unknown". Format rules for authors are in
`superdoc/claude-instructions/changelog.md`.

*Why embedded rather than read from disk:* the text must describe the exact build showing it
(an old process or a mismatched installed copy would lie), needs no path search or "not
found" state, and costs no I/O on the render thread.

## The parser

`ParseChangelog()` is deliberately not a Markdown renderer: it reads the one document shape
the changelog policy fixes, five line kinds (`ContentKind`: Heading for `#`/`##`,
Subheading for `###`, Bullet, BulletCont, Plain). It guarantees one output line per input
line, in order, so an unrecognised line comes back as `Plain` with its text intact, never
dropped; a malformed changelog renders as unstyled text. Details worth knowing:

- `**` markers are stripped everywhere (the screen has no bold face); a bullet's `**Lead**`
  becomes `sLead`, the colon after it is dropped, and an *unclosed* `**` is not a lead-in.
- A continuation is only an indented line directly after a bullet or another continuation; a
  blank line ends the run.
- A `#` without a following space is not a heading. A trailing newline does not add a blank
  final line; LF and CRLF both work.
- It is pure (no ImGui, fonts or I/O) and runs once into a function-local static
  (`ChangelogLines()`, `AboutLines()`), not per frame. *Why at load time, not in the embed
  script:* it stays unit-testable in C++ (`tests/test_changelog_parse.cpp`) and the embed step
  keeps a single job. The licence documents go through the same parser, so plain text stays
  plain and the notice file's `##` headings become headings.
- The area does not follow the tail (`FollowsTail` false): newest entries are first.

## Related links

- [log.md](log.md) — the other read-only text area, Log
- `superdoc/claude-instructions/changelog.md` — the changelog format the parser relies on

## History

- **About** (glossary detail): the settings overlay's `system.changelog` area, labelled "About" since 2026-09-09. It answers three questions on one page: what am I running (the gamescope-ritz semver, the upstream base commit, HEAD's date), what changed (`CHANGELOG.md`, compiled in), and what is it made of (the licences of everything compiled into the binary — the fork's own `LICENSE`, `THIRD-PARTY-LICENSES.md` and the Geist fonts' OFL, all compiled in and printed below the changelog). Searching the command palette for either "changelog" or "about" finds it.

