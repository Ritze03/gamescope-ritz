# Gentle test builds for agents (`scripts/gentle-build.sh`)

How agents build and test in this repo (`scripts/gentle-build.sh`, build dir `build-agent/`): the CPU/cache-friendly throwaway build, never the LTO `build-release/` build.
Split out of [build-and-tooling.md](build-and-tooling.md) on 2026-11-07; the text below is moved verbatim, except that the author's CPU model is no longer named.

Every agent working in this repo does its own throwaway "does it compile, do the tests
pass" build, each in its own worktree, and `build-gamescope-ritz.sh`'s defaults are
tuned for the opposite job — the one, optimal, LTO'd `build-release/` binary the user
actually launches games through. Running several of *those* full ~670-target builds at
once, on a desktop the user is actively gaming on, was reported as lagging the whole
machine. `scripts/gentle-build.sh` is the agent-facing answer: same full build, a
separate `build-agent/` dir, tuned to disturb the desktop as little as possible. **Use
it — not a raw `ninja`/`meson compile`, not `build-release/` — for every agent test
build**; see `CLAUDE.md`'s "Building (agents)" bullet.

**`Why:` the levers changed mid-implementation, and it matters which ones actually
work.** The first draft capped the build's memory (`systemd-run`'s `MemoryHigh`) on the
theory that a big compile evicts the user's RAM working set. The user corrected that
directly: *"I wasn't talking about RAM. I have enough RAM. That doesn't matter. It's the
CPU cache that is being hit hard, which is pretty slow on my [desktop CPU]."* That's a
different resource with different rules: the development machine (a desktop CPU) has 10 cores, SMT
**off** (`lscpu -e`: one thread per core — check yours, don't assume), and **one 20MB L3
cache shared by every core over the ring**, with no Intel CAT/resctrl to partition it
(verified: no `cat_l3` in `/proc/cpuinfo`, no `/sys/fs/resctrl` on this machine —
consumer Comet Lake doesn't support it). Memory limits do nothing for that, and neither
does CPU pinning — a compiler on core 7 still evicts the game's lines from the
one L3 every core shares. What actually helps a *shared, unpartitionable* L3:

1. **Fewer concurrent compilers, first and foremost.** Every `cc1plus` instance streams
   a multi-MB working set through that single 20MB L3; several running at once is what
   actually thrashes it. `gentle-build.sh` defaults to `-j1` and refuses `--jobs` above
   `2`.
2. **A hard CPU duty-cycle cap**, `systemd-run -p CPUQuota=` (`--cpu-quota`, default
   `50%`): even one compiler running flat-out competes with the game continuously.
   Throttling it to run only half the time gives the game uncontested stretches to
   refill its own working set — `SCHED_IDLE`/`nice` alone don't do this, since they only
   step aside when a core would otherwise sit idle, not cap how hard a process runs
   *while* it holds the core.
3. **Less total compiler work**: no LTO (`-Db_lto=false` — LTO's whole-program link is
   still this project's single worst spike, memory or cache), `-g0` (skip debug info),
   `-fuse-ld=lld` when a trial link with it succeeds (lower memory *and* a measurably
   faster link than bfd — less time spent hammering the cache). `-Doptimization` was
   measured, not guessed: timing a from-scratch, single-core compile of this project's
   largest translation unit (`src/rendervulkan.cpp`, 6.6k lines) put `-O1` at 4.83s
   against `-O2`'s 4.86s — noise, not a real win — so the script keeps `-O2`. The GCC
   `--param ggc-min-*` flags from the first (RAM-focused) draft are gone: they cost real
   compile time and were never shown to move L3 pressure (no `perf`, no `cat_l3` on this
   machine to even measure that), so they're dropped rather than kept on faith.
4. **Avoid full rebuilds altogether where possible.** The single biggest total-cache-time
   cost is several agent worktrees each doing a from-scratch ~670-target build.
   `gentle-build.sh` reuses one fixed, persistent `build-agent/` per worktree
   (`gcr_meson_configure` reconfigures an existing dir in place rather than wiping it),
   so a second run in the same worktree is incremental. It also auto-detects `ccache`
   (installed on this machine, `/usr/bin/ccache` 4.14) on `PATH` and wires it into
   `CC`/`CXX` for the configure when present, with two env vars set so the reuse works
   **across worktrees, not just within one**: `CCACHE_BASEDIR=<repo root>` and
   `CCACHE_NOHASHDIR=1`. *Why both are needed:* every agent worktree is a separate
   checkout at a different absolute path but with identical relative layout, and this
   project's own compile commands already pass relative include paths (`-Isrc
   -I../src ...`, confirmed via `compile_commands.json`) — `CCACHE_BASEDIR` strips each
   worktree's own absolute prefix before hashing so the same relative path from two
   worktrees lands on the same cache key, and `CCACHE_NOHASHDIR` stops the compilation's
   own working directory (which varies with the build dir, same as the worktree does)
   from being folded into that key regardless. `ccache`'s default 5G cache is raised to
   15G (`ccache --set-config=max_size=15G`, run idempotently by the script itself, no
   separate one-time step to remember) — the user has 63G free on this machine and asked
   for headroom for several worktrees' worth of objects; 15G is the middle of a
   reasonable 10-20G range without getting close to filling the disk. If a `build-agent/`
   predates ccache being installed, the script notices (a marker file records whether
   the last configure had it) and wipes+reconfigures once — meson only reads `CC`/`CXX`
   at a build dir's FIRST configure, and a plain `--reconfigure` does not re-read them,
   so without this check a pre-ccache build dir would silently never pick it up.

Measured on the development machine (a 10-core desktop CPU, `-j1`, `--cpu-quota 50%`, pinned to cores
6-9 (history: the script pinned then), the user actively running CS2 through a separate gamescope-ritz instance the whole
time): a from-scratch (empty ccache) full build of all 679 ninja targets, including
`tests/gamescope_tests`, took **14m43s wall** (7m3s user, 19s sys — the gap between wall
and user time IS the CPUQuota throttle doing its job); `meson test` equivalent
(`gamescope_tests` run directly under the same gentle wrapper) passed **all 588 test
cases, 15,953,549 assertions**, in 3s. `ccache -s` after that cold run already showed
79/533 hits (14.8%) — several subprojects (`libliftoff`'s four example binaries) compile
the same shared source under identical flags, so even a "cold" run gets some reuse
within itself. The systemd scope's limits were confirmed live via `systemctl --user show
-p CPUQuotaPerSecUSec -p AllowedCPUs run-<id>.scope` while the build ran (`500ms` and
`6-9` respectively; historical, the script no longer sets `AllowedCPUs`), not just assumed from the flags passed.

A second full build into a **fresh** `build-agent2/` directory (simulating a second
agent worktree building the same commit, cache warm from the first run) took **18s
wall** for the same 679 targets plus the same full test pass — a ~49x speedup. Its own
533 cacheable compiler calls hit the cache **533/533 (100%)**; the cumulative `ccache -s`
after both runs read `Hits: 612/1066 (57.4%)`, `Misses: 454/1066` (the 454 misses are
entirely from the first, genuinely-cold run). Total on-disk cache size after both builds:
19MB (`du -sh ~/.cache/ccache`), nowhere near the 15G cap — this project's non-cacheable
steps (linking, shader/font/protocol generation) dominate a warm build's remaining time,
not the compiler cache's own size. This is the `CCACHE_BASEDIR`/`CCACHE_NOHASHDIR`
cross-worktree design working as intended: `build-agent2/` never shared a build directory
with the first run, only the object cache, which is exactly the "different worktree,
same commit" case this was built for.

```sh
scripts/gentle-build.sh                       # full gentle build -> build-agent/
scripts/gentle-build.sh --test                 # build, then run gamescope_tests gently
scripts/gentle-build.sh --jobs 2 --cpu-quota 70%
scripts/gentle-build.sh --target src/gamescope # just the binary
```

`Why:` (2026-10-07) the script no longer pins CPUs (`taskset`, cgroup `AllowedCPUs=` and
`GENTLE_CPUS` are gone): the user now enforces core affinity system-wide (*"No need to
manually pin cores anymore. I have it enforced system wide now."*), so a second pin in
the script was redundant. Pinning never helped the shared L3 anyway.

Full option/knob reference: `scripts/gentle-build.sh --help` (the script's own header
comment; kept in sync there rather than duplicated in full here) and `scripts/README.md`'s
own section on it.

## Related

- [build-and-tooling.md](build-and-tooling.md) — Meson build, feature flags, what is compiled in.
- [unit-tests.md](unit-tests.md) — the test binary the build compiles.
- `scripts/README.md` — the script's own reference.
