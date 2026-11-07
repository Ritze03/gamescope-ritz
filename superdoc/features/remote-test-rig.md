# Remote test laptop (`scripts/remote-test.sh`)

The second real-hardware rig: a separate test machine reached over SSH, driven by `scripts/remote-test.sh`.
Split out of [build-and-tooling.md](build-and-tooling.md) on 2026-11-07; the text below is moved verbatim.

A second real-hardware rig, distinct from the SteamOS device tooling above: a separate
test machine on the local network, reachable over SSH with key auth, running a live
Wayland session. Its connection details and hardware notes live in the untracked local file
`.claude/local/test-laptop-notes.md`, not in this repo. *Why it exists:* every overlay bug this project has hit took multiple failed
rounds because agents could only test headlessly on the main development
machine — no visible overlay, no real force-grab as a launch flag, no soak testing, and
twice a test window landed on the development machine's own display by accident. The test machine is a
real, dedicated Wayland session nobody else is using, reachable non-interactively.

*Why build-here-ship-there, not build-on-the-test-machine:* its CPU is much slower than the
development machine's; a full LTO release build there would take a long time and make the
machine unusable while it ran. The desktop already compiles at GCC's default `-march`
(generic x86-64 baseline — nothing in `meson.build` or `scripts/build-gamescope-ritz.sh`
sets `-march=native` or anything else; `/etc/makepkg.conf`'s `-march=native` only affects
`makepkg`, not a plain `meson`/`ninja` invocation), so the binary this produces already
runs on any x86_64 CPU including the test machine's — verified via `readelf -n` reporting `x86
ISA used: x86-64-baseline`. So the desktop builds once (fast, on the faster development machine) and `scripts/remote-test.sh sync` ships the compiled binary
over rather than repeating the build on weaker hardware.

- `scripts/remote-test.sh sync [--no-build]` — builds locally
  (`nice -n 19`, release) and rsyncs `build-release/src/gamescope` to
  `~/gamescope-ritz-remote/gamescope-ritz` on the laptop, then runs `--version` there to
  confirm the transferred binary actually starts (catches both an ISA mismatch and
  missing shared libraries immediately, rather than mid-investigation later). There is
  nothing else to sync: the Lua scripts, the shader effects and the licence texts are all
  compiled into the binary, which is why the old `--extras` flag is gone. `gamescopectl`
  is not synced — it's
  a separate binary from the distro's gamescope package, already present on the
  laptop the same way it is on the desktop.
- `scripts/remote-test.sh run [--wait] -- <command...>` — runs a command on the laptop
  with the session's `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` and compositor-instance variables exported (read
  live from the running compositor process's own `/proc/<pid>/environ` on every call, since
  every `ssh host 'cmd'` is a fresh non-interactive shell that inherits none of the
  target session's environment) and the remote bin dir on `PATH`. Without `--wait` the
  command is launched via `setsid nohup ... & disown` so it survives the SSH session
  ending — the second SSH fact this script exists to hide: a long-running remote process
  dies with its SSH session unless explicitly detached.
- `scripts/remote-test.sh screenshot <remote-path> [local-path]` — runs `gamescopectl
  screenshot "<remote-path> 4"` (type `4` = `screen_buffer`; the default type truncates
  `zpos >= 2` and the overlay sits at `zpos 6`, and gamescopectl silently collapses
  trailing args if the path and type aren't one quoted argument — a quirk that has
  independently cost six prior investigations, so the script always does this for you)
  and `scp`s the result back.
- `scripts/remote-test.sh env` — prints the resolved `export` lines for a caller who
  wants to `ssh` in by hand instead.

**Known gap (2026-09-02):** the test machine was missing the system `wlroots` 0.20
library, so a synced binary failed its `--version` self-check with "shared libraries:
libwlroots-0.20.so: cannot open shared object file". Install the package there
(details in the local notes file). Its Vulkan driver and loader were already fine; only
the `vulkaninfo` tool was missing.

## Related

- [build-and-tooling.md](build-and-tooling.md) — Meson build, feature flags, what is compiled in.
- [steamos-device-tooling.md](steamos-device-tooling.md) — the SteamOS device scripts.
- [gentle-build.md](gentle-build.md) — the build the binary is made with.
