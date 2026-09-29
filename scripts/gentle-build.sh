#!/usr/bin/env bash
# gentle-build.sh — build gamescope-ritz the way an agent should: a throwaway
# test build that barely disturbs the user's desktop/game while it runs.
#
# The user, verbatim (first ask): "Create a script to build the test builds
# that agents usually do, that isn't as harsh on my cache in memory, since
# basically my whole PC is lagging while it's compiling, and that is like
# really annoying. So create a script for agents they can run to build it
# with the nicest values and some compiler-specific settings that make sure
# that it isn't hitting my cache as hard."
#
# The user, verbatim (correction — read this before touching any knob
# below): "I wasn't talking about RAM. I have enough RAM. That doesn't
# matter. It's the CPU cache that is being hit hard, which is pretty slow on
# my 10900K." This machine (i9-10900K) has 10 cores, SMT OFF (`lscpu -e`
# shows one thread per core here — check yours, don't assume), and ONE 20MB
# L3 cache shared over the ring by every core. It has no Intel CAT/resctrl
# (consumer Comet Lake doesn't support L3 partitioning — verified on this
# machine: no `cat_l3` in `/proc/cpuinfo`, no `/sys/fs/resctrl`), so **no
# amount of CPU pinning can stop a compiler process on core 7 from evicting
# the game's lines from an L3 every core shares**. That rules memory-cgroup
# limits (this script's first draft used `MemoryHigh`) out entirely as the
# wrong lever for this complaint — dropped, along with the GCC
# `--param ggc-min-*` flags that existed only to shrink the compiler's own
# heap for that (now-irrelevant) memory concern; they cost real compile time
# and were not shown to move L3 pressure (no `perf`/no `cat_l3` on this
# machine to even measure that), so they're gone rather than kept on faith.
#
# What actually helps a *shared, unpartitionable* L3, in order:
#   1. **Fewer concurrent compilers** — every cc1plus instance streams a
#      multi-MB working set through that one shared 20MB L3; two or three of
#      them running at once is what actually thrashes it, far more than any
#      one of them alone. This is the main lever. Default `-j1`; `--jobs 2`
#      is the ceiling this script allows.
#   2. **A hard CPU duty-cycle cap** (`-p CPUQuota=`, `--cpu-quota` /
#      `GENTLE_CPU_QUOTA`, default `50%`) on the systemd scope: even a
#      single compiler running flat-out competes with the game for the
#      shared cache continuously. Throttling it to run only half the time
#      gives the game uncontested stretches to refill its own working set,
#      the way `SCHED_IDLE`/`nice` alone cannot — those only step aside when
#      a core is otherwise idle, they don't cap how hard the compiler runs
#      *while* it has the core. Raise it (up to `100%` = no extra cap)
#      if `-j1 --cpu-quota 50%` feels too slow for how gentle you actually
#      need to be.
#   3. **CPU pinning stays, for a narrower reason.** `taskset -c` (process
#      affinity) plus `-p AllowedCPUs=` (the cgroup's own cpuset, belt and
#      braces) keep every compiler/linker thread off the cores the game is
#      most likely scheduled on — protecting THOSE cores' *private* L1/L2 (and
#      their SMT sibling, if your CPU has one — check `lscpu -e` for CORE
#      values shared by two CPU numbers, and keep sibling pairs together in
#      `GENTLE_CPUS` rather than splitting one). It does nothing for the
#      shared L3 (see above) — that's levers 1 and 2's job.
#   4. **Less total compiler work.** No LTO (unchanged from the first
#      draft — LTO's whole-program link is still the worst single spike in
#      this project's build, memory or cache). `-g0` (skip debug info: less
#      I/O, smaller objects) and `-fuse-ld=lld` (used only if present AND a
#      trial link with it succeeds; lld is both lower-memory and
#      meaningfully faster to finish than bfd, which matters here because a
#      shorter link is less time spent hammering the cache) are both kept.
#      `-Doptimization` was measured rather than guessed: timing a from-
#      scratch compile of this project's largest translation unit
#      (`src/rendervulkan.cpp`, 6.6k lines), pinned to one core, `-O1` came
#      in at 4.83s wall against `-O2`'s 4.86s — noise, not a real win — so
#      this script keeps `-O2` (the codegen difference is not zero at
#      runtime either, and there is nothing to trade it for). If your build
#      ever changes shape enough to make `-O1` worth revisiting, re-measure
#      the same way rather than assuming.
#   5. **Avoid full rebuilds.** The single biggest total-cache-time burner
#      is several agent worktrees each doing a from-scratch ~670-target
#      build. This script does its part by reusing one fixed, persistent
#      build dir per worktree (default `build-agent/`, see
#      `gcr_meson_configure` in gamescope-ritz-common.sh: it reconfigures an
#      existing dir in place rather than wiping it) — running this script
#      again in the same worktree is an incremental build, not a clean one,
#      as long as you keep using the same `--dir`. The other half of this —
#      caching object files *across* worktrees/full rebuilds — is `ccache`
#      (`/usr/bin/ccache`, installed on this machine as of 2026-09-29).
#      Auto-detected and wired into `CC`/`CXX` for meson's configure whenever
#      it's on PATH; if a `build-agent/` from before ccache was installed is
#      reused, this script notices the mismatch (a marker file next to the
#      build dir, `.gentle-ccache-state`, recording whether the last
#      configure had it) and wipes+reconfigures once rather than silently
#      keeping the ccache-less compiler forever — meson only reads `CC`/`CXX`
#      at a build dir's FIRST configure, and a plain `--reconfigure` (what an
#      already-built dir gets otherwise) does not re-read them.
#        Every agent worktree is a separate checkout with the SAME relative
#      layout at a DIFFERENT absolute path, and this project's own compile
#      commands already use relative include paths (`-Isrc -I../src ...`,
#      confirmed via `compile_commands.json`) — so the only thing standing
#      between "one shared `~/.cache/ccache`" (ccache's own default,
#      unchanged here) and "every worktree gets its own cold cache anyway" is
#      ccache hashing each worktree's absolute paths into its cache key. Two
#      env vars fix that, set for every compiler invocation this script
#      makes: `CCACHE_BASEDIR=<repo root>` (strip THIS worktree's own
#      absolute prefix before hashing, so the same relative path from two
#      worktrees hashes the same) and `CCACHE_NOHASHDIR=1` (don't fold the
#      compilation's working directory into the hash either — the build dir
#      name/location varies same as the worktree does). Cache size is raised
#      from ccache's 5G default to 15G — the script itself runs `ccache
#      --set-config=max_size=15G` on every invocation (idempotent, so this is
#      not a manual step for anyone to remember) — the user has 63G free on
#      this machine and asked for headroom; 15G sits in the middle of a
#      reasonable 10-20G range for several worktrees' worth of objects
#      without getting close to filling the disk. `GENTLE_CCACHE_MAX_SIZE`
#      overrides it.
#
# This is NOT build-gamescope-ritz.sh. That one builds the optimised,
# LTO'd, `build-release/` tree the user actually launches games through —
# use it (or install.sh) for a real release/install build. This script is
# for agents' own "does it compile, do the tests pass" builds, and only for
# that.
#
# Usage:
#   scripts/gentle-build.sh [--dir build-agent] [--jobs N] [--target T]...
#                            [--setup-only] [--test]
#
# Options:
#   --dir NAME        build directory name, under the repo root (default:
#                      build-agent). Reused/incrementally rebuilt on repeat
#                      runs — pick a fresh name only if you deliberately
#                      want a clean tree.
#   --jobs N           cap ninja parallelism, 1 or 2 (default: 1, or
#                      GENTLE_JOBS). This is the main lever against L3
#                      thrash — see point 1 above. Refused above 2.
#   --cpu-quota PCT    systemd CPUQuota for the whole build (default: 50%,
#                      or GENTLE_CPU_QUOTA). 100% = one core's worth, no
#                      extra duty-cycle throttling beyond --jobs. Only
#                      applied when systemd-run --user --scope works.
#   --target T          a ninja target to build; repeatable. Default: build
#                      everything (same full build agents already run — this
#                      script makes it gentle, not smaller)
#   --setup-only       run meson setup/reconfigure and stop; do not build
#   --test              after building, also run
#                      <dir>/tests/gamescope_tests under the same gentle
#                      wrapper (pass --target tests/gamescope_tests yourself
#                      if you also passed a narrower --target list)
#   -h, --help         show this help and exit
#
# Env overrides:
#   GENTLE_CPUS         taskset/AllowedCPUs core list (default: 6-9). Check
#                       `lscpu -e` first — keep SMT sibling pairs together if
#                       your CPU has SMT (this one doesn't).
#   GENTLE_JOBS         same as --jobs
#   GENTLE_CPU_QUOTA    same as --cpu-quota
#
# Examples:
#   scripts/gentle-build.sh                       # full gentle build -> build-agent/
#   scripts/gentle-build.sh --test                 # build, then run the test suite
#   scripts/gentle-build.sh --jobs 2 --cpu-quota 70%
#   scripts/gentle-build.sh --target src/gamescope # just the binary
#   GENTLE_CPUS=4-9 scripts/gentle-build.sh --jobs 2

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)
# shellcheck source=./gamescope-ritz-common.sh
source "$SCRIPT_DIR/gamescope-ritz-common.sh"

DIR_NAME="build-agent"
JOBS="${GENTLE_JOBS:-1}"
CPUS="${GENTLE_CPUS:-6-9}"
CPU_QUOTA="${GENTLE_CPU_QUOTA:-50%}"
TARGETS=()
SETUP_ONLY=0
RUN_TESTS=0

print_help() { sed -n '2,152p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dir) DIR_NAME="$2"; shift ;;
		--jobs) JOBS="$2"; shift ;;
		--cpu-quota) CPU_QUOTA="$2"; shift ;;
		--target) TARGETS+=("$2"); shift ;;
		--setup-only) SETUP_ONLY=1 ;;
		--test) RUN_TESTS=1 ;;
		-h|--help) print_help; exit 0 ;;
		*) gcr_err "unknown option: $1"; print_help; exit 1 ;;
	esac
	shift
done

gcr_refuse_root_build

if [ "$JOBS" -gt 2 ] 2>/dev/null; then
	gcr_err "--jobs $JOBS refused: this script caps at 2 (concurrent cc1plus processes are the main cause of the shared-L3 thrash it exists to avoid — see the header comment). Use build-gamescope-ritz.sh if you genuinely need more parallelism."
	exit 1
fi

REPO_ROOT=$(gcr_repo_root)
BUILD_DIR="$REPO_ROOT/$DIR_NAME"

# --- CPU pinning ----------------------------------------------------------
HAVE_TASKSET=0
command -v taskset >/dev/null 2>&1 && HAVE_TASKSET=1
[ "$HAVE_TASKSET" = "1" ] || gcr_warn "taskset not found — building without CPU pinning."

# --- compiler / linker detection ------------------------------------------
CXX="${CXX:-c++}"
CC="${CC:-cc}"
IS_GCC=0
if "$CXX" --version 2>/dev/null | head -1 | grep -qi 'clang'; then
	IS_GCC=0
elif "$CXX" --version 2>/dev/null | head -1 | grep -qi 'gcc\|g++'; then
	IS_GCC=1
fi

GENTLE_C_ARGS=(-g0)

USE_LLD=0
if command -v ld.lld >/dev/null 2>&1; then
	TMPD=$(mktemp -d)
	printf 'int main(){return 0;}\n' >"$TMPD/probe.cpp"
	if "$CXX" -fuse-ld=lld "$TMPD/probe.cpp" -o "$TMPD/probe" >/dev/null 2>"$TMPD/err"; then
		USE_LLD=1
	else
		gcr_warn "trial link with -fuse-ld=lld failed — falling back to the default linker. Details: $(cat "$TMPD/err")"
	fi
	rm -rf -- "$TMPD"
else
	gcr_warn "ld.lld not found — building with the default linker."
fi

GENTLE_LINK_ARGS=()
[ "$USE_LLD" = "1" ] && GENTLE_LINK_ARGS+=(-fuse-ld=lld)

C_ARGS_STR="${GENTLE_C_ARGS[*]}"
LINK_ARGS_STR="${GENTLE_LINK_ARGS[*]:-}"

# --- ccache auto-detect -----------------------------------------------------
# Point 5 above: caching object files across runs/worktrees is the other
# half of "avoid full rebuilds", and it's ccache's job, not this script's own
# reinvention of it. Wired in automatically, with no config file to edit,
# whenever ccache happens to be on PATH; a plain warning (not a failure)
# otherwise, since it's an opt-in system package, not a hard requirement.
USE_CCACHE=0
if command -v ccache >/dev/null 2>&1; then
	USE_CCACHE=1
	CC="ccache $CC"
	CXX="ccache $CXX"
	# Cross-worktree reuse: every worktree is a separate checkout at a
	# different absolute path with the same relative layout, and this
	# project's own compile commands already pass relative include paths
	# (-Isrc -I../src ...), so BASEDIR+NOHASHDIR is what actually lets two
	# worktrees' identical sources hit the same cache entry instead of each
	# worktree cold-starting its own. Exported (not just set) so every
	# ninja-spawned compiler process inherits them.
	export CCACHE_BASEDIR="$REPO_ROOT"
	export CCACHE_NOHASHDIR=1
	# Raise ccache's 5G default once; harmless to repeat (idempotent), cheap
	# compared to a build, and keeps this script self-contained rather than
	# needing a one-time manual `ccache -M` step documented elsewhere.
	ccache --set-config=max_size="${GENTLE_CCACHE_MAX_SIZE:-15G}" 2>/dev/null || true
	gcr_info "ccache found — wiring it into this build's CC/CXX (CCACHE_BASEDIR=$REPO_ROOT, CCACHE_NOHASHDIR=1, max_size=${GENTLE_CCACHE_MAX_SIZE:-15G})."
else
	gcr_warn "ccache not installed — repeat/incremental compiles across full rebuilds get no cross-run object cache. 'sudo pacman -S ccache' would fix that; this script will auto-detect and use it once it's present."
fi

# --- meson setup ------------------------------------------------------------
MESON_OPTS=(
	--buildtype=release
	-Doptimization=2
	-Db_lto=false
	-Denable_tests=true
	-Dc_args="$C_ARGS_STR"
	-Dcpp_args="$C_ARGS_STR"
)
[ -n "$LINK_ARGS_STR" ] && MESON_OPTS+=(-Dc_link_args="$LINK_ARGS_STR" -Dcpp_link_args="$LINK_ARGS_STR")

gcr_ensure_submodules "$REPO_ROOT"
# Meson fixes the compiler identity (CC/CXX) at a build dir's FIRST configure
# only; a plain --reconfigure (what an existing build dir gets from
# gcr_meson_configure below) does not re-read them. So an existing
# build-agent/ from before ccache was installed would otherwise keep using
# the ccache-less compiler forever. A marker file records whether the last
# configure had ccache wired in; a mismatch against what THIS run wants
# means the build dir is wiped once and reconfigured clean rather than
# silently going stale.
CCACHE_MARKER="$BUILD_DIR/.gentle-ccache-state"
DESIRED_CCACHE_STATE=$([ "$USE_CCACHE" = "1" ] && echo yes || echo no)
if [ -f "$BUILD_DIR/build.ninja" ] && [ -f "$CCACHE_MARKER" ] \
	&& [ "$(cat "$CCACHE_MARKER")" != "$DESIRED_CCACHE_STATE" ]; then
	gcr_info "ccache availability changed since $BUILD_DIR was last configured (was: $(cat "$CCACHE_MARKER"), now: $DESIRED_CCACHE_STATE) — wiping and reconfiguring so CC/CXX picks it up."
	rm -rf -- "$BUILD_DIR"
fi
CC="$CC" CXX="$CXX" gcr_meson_configure "$REPO_ROOT" "$BUILD_DIR" "${MESON_OPTS[@]}"
printf '%s\n' "$DESIRED_CCACHE_STATE" >"$CCACHE_MARKER"

if [ "$SETUP_ONLY" = "1" ]; then
	gcr_info "--setup-only: configured $BUILD_DIR, not building."
	exit 0
fi

# --- systemd-run availability check -----------------------------------------
HAVE_SYSTEMD_SCOPE=0
if command -v systemd-run >/dev/null 2>&1 \
	&& systemd-run --user --scope --quiet -p CPUQuota=50% -- /bin/true >/dev/null 2>&1; then
	HAVE_SYSTEMD_SCOPE=1
else
	gcr_warn "systemd-run --user --scope isn't usable in this session — falling back to chrt/nice/ionice/taskset alone (no CPUQuota/CPUWeight/IOWeight/AllowedCPUs cgroup limits)."
fi

# --- the gentle wrapper ------------------------------------------------------
# Runs "$@" at the lowest CPU/IO priority the kernel offers, pinned to
# GENTLE_CPUS, and — when available — inside a transient user scope that
# duty-cycle-caps its total CPU time (CPUQuota) and reinforces the pin at
# the cgroup level (AllowedCPUs). `systemd-run --scope` (not the default
# service mode) runs the command as a direct child of this shell, inheriting
# its environment and cwd and returning its real exit code, so it composes
# like a plain command despite the wrapping.
gentle_run() {
	local inner=()
	command -v chrt >/dev/null 2>&1 && inner+=(chrt --idle 0)
	inner+=(nice -n 19)
	command -v ionice >/dev/null 2>&1 && inner+=(ionice -c3)
	[ "$HAVE_TASKSET" = "1" ] && inner+=(taskset -c "$CPUS")
	inner+=("$@")

	if [ "$HAVE_SYSTEMD_SCOPE" = "1" ]; then
		systemd-run --user --scope --quiet --collect \
			-p CPUWeight=idle \
			-p IOWeight=1 \
			-p CPUQuota="$CPU_QUOTA" \
			-p AllowedCPUs="$CPUS" \
			-- "${inner[@]}"
	else
		"${inner[@]}"
	fi
}

# --- summary -----------------------------------------------------------------
gcr_info "gentle-build: dir=$BUILD_DIR jobs=$JOBS cpus=${CPUS}$([ "$HAVE_TASKSET" = "1" ] || echo ' (unpinned: no taskset)')"
gcr_info "gentle-build: sched=chrt(SCHED_IDLE)+nice19+ionice(idle) cgroup=$([ "$HAVE_SYSTEMD_SCOPE" = "1" ] && echo "CPUWeight=idle IOWeight=1 CPUQuota=$CPU_QUOTA AllowedCPUs=$CPUS (systemd-run --user --scope)" || echo "none (systemd-run scope unavailable)")"
gcr_info "gentle-build: cc=$([ "$IS_GCC" = "1" ] && echo gcc || echo non-gcc/unknown) c_args='${C_ARGS_STR}' ld=$([ "$USE_LLD" = "1" ] && echo lld || echo default) ccache=$([ "$USE_CCACHE" = "1" ] && echo yes || echo no)"

# --- build -------------------------------------------------------------------
NINJA_ARGS=(-C "$BUILD_DIR" -j "$JOBS")
[ "${#TARGETS[@]}" -gt 0 ] && NINJA_ARGS+=("${TARGETS[@]}")

START_TS=$(date +%s)
if ! gentle_run ninja "${NINJA_ARGS[@]}"; then
	END_TS=$(date +%s)
	gcr_err "build FAILED after $((END_TS - START_TS))s (ninja ${NINJA_ARGS[*]}) — see the output above for the failing target."
	exit 1
fi
END_TS=$(date +%s)
ELAPSED=$((END_TS - START_TS))
gcr_info "build finished in ${ELAPSED}s ($BUILD_DIR)"

# --- optional test run --------------------------------------------------------
if [ "$RUN_TESTS" = "1" ]; then
	TEST_BIN="$BUILD_DIR/tests/gamescope_tests"
	if [ ! -x "$TEST_BIN" ]; then
		gcr_err "--test: $TEST_BIN not found or not executable — was it built? (pass --target tests/gamescope_tests if you narrowed --target)"
		exit 1
	fi
	gcr_info "running $TEST_BIN under the same gentle wrapper..."
	TEST_START=$(date +%s)
	if ! gentle_run "$TEST_BIN"; then
		TEST_END=$(date +%s)
		gcr_err "tests FAILED after $((TEST_END - TEST_START))s — see the output above."
		exit 1
	fi
	TEST_END=$(date +%s)
	gcr_info "tests finished in $((TEST_END - TEST_START))s"
fi

gcr_info "done. [$BUILD_DIR]"
