#pragma once

#include <optional>
#include <functional>
#include <span>
#include <vector>

#include <sys/types.h>

namespace gamescope::Process
{
    void BecomeSubreaper();
    void SetDeathSignal( int nSignal );

    // Direct children of nPid, read from /proc/*/stat. Does not recurse -
    // see KillProcessTree for the pattern to walk a full descendant tree.
    std::vector<pid_t> GetChildPids( pid_t nPid );

    void KillAllChildren( pid_t nParentPid, int nSignal );
    void KillProcess( pid_t nPid, int nSignal );

    std::optional<int> WaitForChild( pid_t nPid );

    // Wait for all children to die,
    // but stop waiting if we hit a specific PID specified by onStopPid.
    // Returns true if we stopped because we hit the pid specified by onStopPid.
    //
    // Similar to what an `init` process would do.
    bool WaitForAllChildren( std::optional<pid_t> onStopPid = std::nullopt );

    bool CloseFd( int nFd );

    void RaiseFdLimit();
    void RestoreFdLimit();
    void ResetSignals();

    void CloseAllFds( std::span<int> nExcludedFds );

    void RemoveSteamOverlayFromPreload();

    // Stashes the LD_PRELOAD we were launched with so a child that can draw the overlay
    // gets handed it instead. Does nothing if we have no overlay or have already done this.
    void RestartWithoutSteamOverlay( char **argv );

    // Puts a stashed Steam overlay back into LD_PRELOAD for our children to inherit.
    // Returns whether there was anything stashed to put back.
    bool RestoreSteamOverlayPreload();

    // nExtraKeepFds: issue #39 -- fds (e.g. a log-capture pipe's write end)
    // that must survive CloseAllFds() in the forked child so fnPreambleInChild
    // can dup2() them onto stdout/stderr before exec. Empty by default; every
    // existing call site is unaffected.
    pid_t SpawnProcess( char **argv, std::function<void()> fnPreambleInChild = nullptr, bool bDoubleFork = false, std::span<const int> nExtraKeepFds = {} );
    pid_t SpawnProcessInWatchdog( char **argv, bool bRespawn = false, std::function<void()> fnPreambleInChild = nullptr, std::span<const int> nExtraKeepFds = {} );

    bool HasCapSysNice();
    void SetNice( int nNice );
    void RestoreNice();

    bool SetRealtime();
    void RestoreRealtime();

    const char *GetProcessName();

}