# Log — `system.log`

The **Log** area of the **Shell** (the settings overlay): what gamescope and the game printed this session. It sits in
the OTHER rail group and hands a list of lines to the Shell's content body, which draws and scrolls them. Its
sibling is [about.md](about.md); both were one page until 2026-11-07 (text moved verbatim).


Code: `src/Overlay/PanelLog.cpp` (the area), `src/Overlay/LogCapture.{h,cpp}` (capture, no
ImGui). Not to be confused with the *Diagnostics* group inside this area (the buffer's
facts) or the separate Diagnostics area (checks of the setup around gamescope; see
[diagnostics.md](diagnostics.md)).

## How it works

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

## Using it

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

## Related links

- [diagnostics.md](diagnostics.md), [clipboard-sync.md](clipboard-sync.md)
- [about.md](about.md) — the other read-only text area, About
- [system-general.md](system-general.md), [appearance.md](appearance.md) — the SETTINGS-group areas

