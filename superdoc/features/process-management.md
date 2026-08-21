# Process Management — child-process helpers, zombie reaping, and MangoHud-style overlay

Cross-cutting process/OS-level plumbing: the reusable `Process` utility namespace,
the standalone reaper process that guarantees child cleanup, and the MangoHud-compatible
performance-overlay message bridge.

## How it works

- `gamescope::Process` (`src/Utils/Process.h`) is the shared toolbox other subsystems build
  on: subreaper setup (`BecomeSubreaper`), signal handling (`SetDeathSignal`,
  `ResetSignals`), killing (`KillAllChildren`, `KillProcess`), waiting
  (`WaitForChild`, `WaitForAllChildren`), fd hygiene (`CloseFd`, `CloseAllFds`,
  `RaiseFdLimit`/`RestoreFdLimit`), Steam-overlay `LD_PRELOAD` stashing/restoring
  (`RemoveSteamOverlayFromPreload`, `RestartWithoutSteamOverlay`,
  `RestoreSteamOverlayPreload`), spawning (`SpawnProcess`, `SpawnProcessInWatchdog`), and
  realtime/nice priority control (`HasCapSysNice`, `SetNice`/`RestoreNice`,
  `SetRealtime`/`RestoreRealtime`).
- `GamescopeReaperProcess(argc, argv)` (`src/Apps/gamescopereaper.cpp`) is a standalone
  helper process, named `gamescope-reaper` (`pthread_setname_np`), launched as a wrapper
  around Gamescope's actual child process. It becomes a subreaper so orphaned
  grandchildren reparent to it instead of PID 1, installs a signal handler for
  `SIGHUP`/`SIGINT`/`SIGQUIT`/`SIGTERM` that kills all remaining children before exiting,
  and takes `--label`, `--new-session-id`, `--respawn` options plus a `--` separator
  before the real sub-command to run. *Why a whole separate process instead of just a
  signal handler in Gamescope itself:* the header comment states the intent directly —
  "Gamescope can have a lot of bad things happen to it, crashes, segfaults, whatever but
  we always want to make sure that we cleanly kill all of our children when we die" — a
  crashed/segfaulted Gamescope can't run its own cleanup code, so the reaper has to be a
  separate, more crash-resilient process watching from outside.
- `mangoapp.cpp` bridges Gamescope's internal frame-timing/focus state to an external
  [MangoHud](https://github.com/flightlessmango/MangoHud)-compatible performance overlay
  process over a SysV message queue: `init_mangoapp()` creates/attaches the queue via
  `ftok("mangoapp", 65)` + `msgget(..., IPC_CREAT)`; `mangoapp_update(visible_frametime,
  app_frametime_ns, latency_ns)` fills a `mangoapp_msg_v1` struct — frametimes, latency,
  focused PID, output resolution/refresh, FSR upscale/sharpness state, HDR/Steam-focus
  flags, and the focused window's engine name — and sends it non-blocking
  (`msgsnd(..., IPC_NOWAIT)`) so a slow/absent overlay consumer never stalls Gamescope's
  frame loop. `mangoapp_output_update(vblanktime)` tracks base-plane commit changes to
  derive per-frame timing independent of `mangoapp_update`'s caller. The message struct's
  comment `// WARNING: Always ADD fields, never remove or repurpose fields` documents the
  wire-compatibility contract with external MangoHud overlay binaries. *Why non-blocking
  send:* the overlay is an optional external consumer — Gamescope must keep compositing
  even if nobody is reading the queue.

## Using it

The reaper wraps Gamescope's launched game/child process automatically — it is not
something a user invokes directly; it is the executable Gamescope `exec`s as a wrapper via
`Process::SpawnProcess`/`SpawnProcessInWatchdog`. `mangoapp_update`/`mangoapp_output_update`
are called from Gamescope's own frame-presentation path each frame; an external MangoHud
build configured to read the same SysV queue key (`ftok("mangoapp", 65)`) then renders the
overlay.

## Related links

- [build-and-tooling](build-and-tooling.md) — Meson build that produces the `gamescopereaper` binary alongside the main executable.
- [scripting-convars](scripting-convars.md) — `cv_mangoapp_use_output_timing` is a `ConVar<bool>` consumed by `mangoapp_output_update`.
