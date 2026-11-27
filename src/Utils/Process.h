#pragma once

#include <optional>
#include <functional>
#include <span>
#include <string>
#include <string_view>
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

    // Would execvp() find this program, and is it executable?
    //
    // `Why this exists at all:` execvp() failing inside a forked child is a
    // message nobody sees -- the fork already happened, the child's stderr goes
    // wherever gamescope's does, and the caller is left with a feature that
    // silently did nothing. Resolving argv[0] the same way execvp would, BEFORE
    // forking, is what lets a caller turn "that program isn't installed" into a
    // sentence a user can act on. A name with a '/' in it is a path and is
    // checked as one; anything else is searched along PATH (and along execvp's
    // own fallback when PATH is unset or empty).
    bool ExecutableExists( const std::string &sProgram );

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

    // ---- Steam overlay "dummy window" route (superdoc/features/steam-overlay.md) ----
    // Under it the overlay lives in a helper process, never in us and never in the game.

    // The env override of the "Steam overlay control" key: GS_RITZ_STEAM_OVERLAY_DUMMY
    // 1 forces the route on for this launch, 0 forces it off, unset/empty = nullopt (the
    // key decides: SteamOverlay_ControlAtLaunch(), Overlay/SteamOverlay.h).
    std::optional<bool> SteamOverlayDummyOverride();

    // main() decides once, after the backend is known (the route only exists on
    // Wayland, SDL and Headless), and every later caller just asks.
    void SetSteamOverlayDummyActive( bool bActive );
    bool SteamOverlayDummyActive();

    // What one process should see of the overlay: its LD_PRELOAD (nullopt = unset)
    // and whether the Vulkan layer VK_LAYER_VALVE_steam_overlay is on for it.
    struct SteamOverlayEnv_t
    {
        std::optional<std::string> oLdPreload;
        bool bVulkanLayer = false;
    };

    // Pure helpers, no environment access (tests drive them directly).
    std::string StripOverlayFromPreloadList( std::string_view svPreload );
    std::string OverlayEntriesOfPreloadList( std::string_view svPreload );
    // First existing <root>/steamrt64|ubuntu12_64/gameoverlayrenderer.so, or "".
    std::string FindSteamOverlayRenderer( const std::vector<std::string> &vecSteamRoots );
    // The game and every other child: no overlay preload entry, layer off.
    SteamOverlayEnv_t ComputeGameOverlayEnv( const std::optional<std::string> &oLdPreload );
    // The dummy window: only the overlay entries (from the stash, else LD_PRELOAD, else
    // sFallbackRenderer if non-empty), layer on.
    SteamOverlayEnv_t ComputeDummyOverlayEnv( const std::optional<std::string> &oLdPreload,
        const std::optional<std::string> &oStash, const std::string &sFallbackRenderer );

    // Environment-touching wrappers around the above.
    void ApplySteamOverlayEnv( const SteamOverlayEnv_t &env );
    // Reads and consumes the stash (unlike RestoreSteamOverlayPreload it leaves LD_PRELOAD alone).
    std::optional<std::string> TakeStashedOverlayPreload();
    // Where this machine's Steam installs usually are (under $HOME, plus the Steam-set override).
    std::vector<std::string> SteamRootCandidates();

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