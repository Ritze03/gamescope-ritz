// Issue #39: the non-UI half of the LOG panel -- capturing gamescope's own
// log output and (best-effort) the launched game's stdout/stderr into two
// bounded ring buffers that PanelLog.cpp reads as cheap snapshots. See that
// file for the ImGui side; this one owns no ImGui calls at all, matching
// the split PanelAudio.cpp/Audio/Volume.h already uses.
//
// ---- Gamescope tab: a real, already-proven tap point --------------------
// LogScope (log.hpp) already hands every log() call to any registered
// listener before it hits stderr. The gap (see log.hpp's
// AddGlobalLoggingListener() comment) was that there was no single place to
// listen to catch *every* scope at once -- fixed there, not here. This file
// just calls it once and buffers what comes back.
//
// ---- Game tab: real launch-path plumbing, not a free lunch ---------------
// The launched game inherits gamescope's own stdout/stderr fds directly
// (gamescopereaper.cpp explicitly does not close them -- "we want to keep
// the same stdin/stdout"). There is no existing capture point, so
// StartGameCapture() below creates a pipe pair per stream, hands the
// caller (LaunchNestedChildren in steamcompmgr.cpp) a preamble to run in
// the forked child (dup2 the pipe write end onto stdout/stderr before
// exec) and the extra fds SpawnProcess must keep alive for that preamble
// to reach, then starts a background thread per stream that both buffers
// what it reads AND writes it straight back out to gamescope's own
// (untouched -- only the child's fd table was ever touched) stdout/stderr,
// so existing "pipe/redirect gamescope's own output" users keep seeing the
// game's output too, just relayed through gamescope instead of inherited
// directly. See LogCapture.cpp's StartGameCapture() for the full pipe
// lifecycle and why the child cannot block on a full pipe.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "../log.hpp"

namespace gamescope::LogCapture
{
	// One buffered line. Gamescope-tab lines carry the LogScope's own
	// priority/prefix; game-tab lines reuse LogPriority as a cheap
	// stdout/stderr tag (LOG_INFO == stdout, LOG_ERROR == stderr) purely so
	// the panel can color them the same way it already colors real log
	// priorities -- there is no real "priority" for raw process output.
	struct Line
	{
		LogPriority ePriority = LOG_INFO;
		std::string sScope;   // LogScope prefix ("console", "reaper", ...); empty for game-tab lines
		std::string sText;
	};

	// A cheap-to-copy snapshot of one ring buffer. ulGeneration bumps every
	// time a line is appended -- the panel only needs to re-copy
	// vecLines when this changed since its last read (see PanelLog.cpp),
	// so an idle log costs nothing beyond the generation compare.
	struct Snapshot
	{
		std::vector<Line> vecLines; // oldest first, capped -- see LogCapture.cpp's kMaxLines
		uint64_t ulGeneration = 0;
	};

	// Registers the global LogScope listener that feeds the Gamescope tab.
	// Safe to call more than once (only the first call does anything) --
	// same contract as Audio::Init(). Call as early as possible so nothing
	// logged at startup is missed; SettingsOverlay's draw loop calls this
	// once per frame before the panel checks whether it's even open, same
	// as PanelAudio.cpp's EnsureConfigLoaded() pattern.
	void InitGamescopeCapture();

	// Non-blocking read of the Gamescope tab's current buffer.
	Snapshot GetGamescopeLog();

	// Cheap (lock + read one integer, no copy) -- lets a caller that
	// already has a cached snapshot skip GetGamescopeLog()'s full copy on
	// every frame the buffer hasn't actually changed since. PanelLog.cpp
	// is the only real reason this exists.
	uint64_t GetGamescopeLogGeneration();

	// ---- Game tab -------------------------------------------------------

	// What LaunchNestedChildren needs to actually capture the child's
	// output: a preamble to hand SpawnProcess (runs in the forked child,
	// after CloseAllFds(), right before exec) and the fds that preamble
	// needs CloseAllFds() to leave alone. Starts two background reader
	// threads immediately (before the caller ever spawns anything), so the
	// pipes are already being drained by the time the child can possibly
	// write to them.
	//
	// bOk is false if pipe creation failed (e.g. fd exhaustion) -- callers
	// must then spawn the child without capture rather than deadlock on a
	// half-built pipe pair; the Game tab stays empty and says why.
	struct GameCaptureHandles
	{
		bool bOk = false;
		std::function<void()> fnPreambleInChild;      // dup2()s pipe write ends onto stdout/stderr, closes originals -- runs in the forked child
		std::vector<int> vecExtraKeepFds;              // pass straight through to SpawnProcess/SpawnProcessInWatchdog's nExtraKeepFds
		std::function<void()> fnCloseParentWriteEnds;  // call once, in the parent, right after SpawnProcess returns
	};

	// Call once, before spawning the primary child. The caller MUST invoke
	// the returned fnCloseParentWriteEnds() right after SpawnProcess
	// returns, to close this (gamescope's own) process's copies of the pipe
	// write ends -- fork() duplicates them, and until every writable copy
	// is closed the reader threads can never see EOF when the game exits,
	// since a writable fd would still be open here even after the child is
	// long gone.
	GameCaptureHandles StartGameCapture();

	Snapshot GetGameLog();
	uint64_t GetGameLogGeneration();

	// True once StartGameCapture() has been called for this process --
	// lets the panel tell "never captured" (older gamescope invocation
	// this session didn't request it, or pipe creation failed) apart from
	// "captured, just quiet."
	bool IsGameCaptureActive();
}
