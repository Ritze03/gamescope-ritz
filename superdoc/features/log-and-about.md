# Log and About — the two read-only pages at the end of the settings overlay

Two areas of the **Shell** (the settings overlay) that show text instead of settings:
**Log** (`system.log`, what gamescope and the game printed this session) and **About**
(`system.changelog`, what you are running, what changed, and what it is made of). Both sit in
the OTHER rail group, and both hand a list of lines to the Shell's content body, which draws
and scrolls them.

## Log — `system.log`

Code: `src/Overlay/PanelLog.cpp` (the area), `src/Overlay/LogCapture.{h,cpp}` (capture, no
ImGui). Not to be confused with the *Diagnostics* group inside this area (the buffer's
facts) or the separate Diagnostics area (checks of the setup around gamescope; see
[diagnostics.md](diagnostics.md)).

### How it works

- **Two sources, two rings.** `LogCapture::InitGamescopeCapture()` registers one global
  `LogScope` listener (`LogScope::AddGlobalLoggingListener`, `src/log.hpp`), so every `log()`
  call in the process lands in the gamescope ring. `StartGameCapture()` feeds the game ring
  from the launched game's stdout and stderr. Both rings are bounded (`kMaxLines`, 4000
  lines each; the oldest line is dropped), guarded by a mutex, and carry a `generation`
  counter that bumps per line.
- **Startup wiring.** `steamcompmgr.cpp` calls `InitGamescopeCapture()` early, before the
  overlay's first frame, so nothing logged at startup is missed; it calls
  `StartGameCapture()` only when a primary child command exists, right before
  `SpawnProcessInWatchdog`.
- **How the game's output is captured.** The game inherits gamescope's own stdout/stderr, so
  there is no tap to listen on. `StartGameCapture()` creates a pipe per stream and returns a
  preamble (`dup2` the write ends onto fds 1/2 in the forked child), the extra fds
  `SpawnProcess` must keep open, and `fnCloseParentWriteEnds`, which the caller must run
  right after the spawn. Without that last step gamescope's own copies of the write ends keep
  the pipes open and the reader threads never see EOF when the game exits. One detached
  reader thread per stream splits on newlines and pushes lines. If pipe creation fails,
  `bOk` is false, the game is spawned without capture, and the area says "not capturing".
  *Why:* a reader thread that always drains also keeps the game from ever blocking on a full
  pipe.
- **The reader relays what it reads.** Every chunk is written verbatim to gamescope's own
  stdout/stderr before it is buffered. *Why:* people who redirect gamescope's output kept
  seeing the game's output when it was inherited; routing it through a pipe must not remove
  that, and a stalled terminal must never stop the buffering.
- **One sequence for both rings.** Each `Line` is stamped at `Push()` time with a global
  atomic `ulSeq` (shared by both rings) and a wall-clock `ulRealtimeMs`. `BuildLines()` in
  `PanelLog.cpp` merges the two filtered rings sorted on `ulSeq`, so a game line and the
  gamescope line that caused it stay adjacent. *Why:* appending one ring after the other put
  related lines thousands of rows apart; timestamps alone can tie within a millisecond, a
  sequence cannot. Stamping at push time is required because the ring evicts lines and the
  merge happens out of order.
- **Cheap while idle.** The panel keeps a cached copy per ring (`RefreshTab`) and re-copies
  only when the generation moved; `CountShown()` counts matches without building the view.
- **Severity of game lines.** Raw process output has no priority, so stdout is tagged
  `LOG_INFO` and stderr `LOG_ERROR` purely so the existing colouring and severity filter
  work. This means a game that prints normal messages on stderr shows them as errors.

### Using it

- **Filter group** (rows live in the Inspector, `RowsInInspector()`, so the sheet is the log
  itself): *Sources* (chip bank: gamescope, game), *Severity* (error, warn, info, debug),
  *Text filter* (substring, case-sensitive), *Auto-scroll* (`FollowsTail`; scrolling up
  pauses it regardless). Hidden lines are still captured; widening a filter brings them back.
- **Diagnostics group**: *Buffer* facts (lines held, error count, per-ring counts, how many
  are shown).
- **Copy to clipboard** (`log.copy`): sends the *visible* lines as `scope: text` through
  `gamescope_post_selection()`, i.e. to the compositor's clipboard and, nested, the host's.
  See [clipboard-sync.md](clipboard-sync.md). *Why not `ImGui::SetClipboardText`:* with no
  clipboard callback wired for the overlay's context it wrote to a buffer nothing outside the
  process could read.
- **Filter state is deliberately not persisted** (statics in `PanelLog.cpp`, no config key):
  a filter that survived a restart would hide a later session's lines for a reason nobody
  remembers setting.

## About — `system.changelog`

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

### The changelog is the version marker

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

### The parser

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

- [diagnostics.md](diagnostics.md), [clipboard-sync.md](clipboard-sync.md)
- [system-and-appearance.md](system-and-appearance.md) — the System and Appearance areas
- `superdoc/claude-instructions/changelog.md` — the changelog format the parser relies on

## History

- **About** (terminology entry, moved here 2026-11-07): It answers three questions on one page: what am I running (the gamescope-ritz semver, the upstream base commit, HEAD's date), what changed (`CHANGELOG.md`, compiled in), and what is it made of (the licences of everything compiled into the binary — the fork's own `LICENSE`, `THIRD-PARTY-LICENSES.md` and the Geist fonts' OFL, all compiled in and printed below the changelog). Searching the command palette for either "changelog" or "about" finds it.
