#!/usr/bin/env bash
# build-gamescope-ritz.sh — single entry point for building gamescope-ritz,
# so nobody has to remember the meson invocation or rediscover this repo's
# setup quirks.
#
# Handles, so you don't have to hit them first:
#   - uninitialised submodules (src/reshade, subprojects/wlroots,
#     libdisplay-info, libliftoff, SPIRV-Headers) otherwise fail meson with a
#     confusing "Include dir reshade/source does not exist" — this detects
#     and runs `git submodule update --init --recursive` for you.
#   - a stale build directory that can't just be reconfigured in place —
#     falls back to a clean wipe+configure automatically.
#
# Two build trees coexist on purpose and are never mixed up:
#   build-release/  --buildtype=release -Doptimization=3 -Db_lto=true (default)
#   build/          --buildtype=debug                                (--debug)
# Why the split matters: this project once burned a long debugging detour
# because an -O0 debug binary was slow enough to change VRR behaviour. Which
# tree you're running should always be obvious — hence separate directories
# and this script always printing which one it just built.
#
# Usage:
#   scripts/build-gamescope-ritz.sh [options]
#
# Options:
#   --release       build the optimised tree in build-release/ (default)
#   --debug         build the debug tree in build/ instead
#   --test          enable tests (-Denable_tests=true) and run `meson test`
#                   after building
#   --clean         remove the target build directory first, then configure
#                   clean (use if a reconfigure keeps failing)
#   --jobs N        cap ninja parallelism (default: let ninja pick)
#   -h, --help      show this help and exit
#
# Examples:
#   scripts/build-gamescope-ritz.sh                  # release -> build-release/
#   scripts/build-gamescope-ritz.sh --debug           # debug   -> build/
#   scripts/build-gamescope-ritz.sh --test            # release build + run tests
#   scripts/build-gamescope-ritz.sh --debug --clean --jobs 8

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)
# shellcheck source=./gamescope-ritz-common.sh
source "$SCRIPT_DIR/gamescope-ritz-common.sh"

BUILDTYPE="release"
RUN_TESTS=0
DO_CLEAN=0
JOBS=""

print_help() { sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
	case "$1" in
		--release) BUILDTYPE="release" ;;
		--debug) BUILDTYPE="debug" ;;
		--test) RUN_TESTS=1 ;;
		--clean) DO_CLEAN=1 ;;
		--jobs) JOBS="$2"; shift ;;
		-h|--help) print_help; exit 0 ;;
		*) gcr_err "unknown option: $1"; print_help; exit 1 ;;
	esac
	shift
done

gcr_refuse_root_build

REPO_ROOT=$(gcr_repo_root)
if [ "$BUILDTYPE" = "release" ]; then
	BUILD_DIR="$REPO_ROOT/$GCR_DEFAULT_BUILD_DIR_NAME"
else
	BUILD_DIR="$REPO_ROOT/build"
fi

if [ "$DO_CLEAN" = "1" ] && [ -d "$BUILD_DIR" ]; then
	gcr_info "removing $BUILD_DIR (--clean)..."
	rm -rf -- "$BUILD_DIR"
fi

EXTRA_OPTS=()
if [ "$RUN_TESTS" = "1" ]; then
	# A previous brief asked for -Denable_tests=false *and* passing tests,
	# which is contradictory: tests can't pass if the suite is disabled.
	# --test means what it says: enable the suite, build it, run it.
	EXTRA_OPTS+=(-Denable_tests=true)
	GCR_NINJA_TARGET=""   # build everything, including gamescope_tests
fi
[ -n "$JOBS" ] && GCR_NINJA_JOBS="$JOBS"

gcr_info "gamescope-ritz build script"
gcr_info "repo:       $REPO_ROOT"
gcr_info "buildtype:  $BUILDTYPE"
gcr_info "build dir:  $BUILD_DIR"
[ "$RUN_TESTS" = "1" ] && gcr_info "tests:      enabled, will run after build"

START_TS=$(date +%s)
gcr_build "$REPO_ROOT" "$BUILD_DIR" "$BUILDTYPE" "${EXTRA_OPTS[@]}"
END_TS=$(date +%s)
ELAPSED=$((END_TS - START_TS))

BIN="$(gcr_release_binary "$BUILD_DIR")"
echo
gcr_info "build finished in ${ELAPSED}s ($BUILDTYPE, $BUILD_DIR)"
if [ -x "$BIN" ]; then
	gcr_info "binary: $BIN ($(du -h -- "$BIN" | cut -f1))"
else
	gcr_warn "expected binary not found at $BIN"
fi

if [ "$RUN_TESTS" = "1" ]; then
	echo
	gcr_info "running meson test -C $BUILD_DIR ..."
	meson test -C "$BUILD_DIR"
fi

echo
gcr_info "done. [$BUILDTYPE] $BIN"
if [ "$BUILDTYPE" = "debug" ]; then
	gcr_warn "this is an UNOPTIMISED debug binary — do not use it to judge performance or VRR behaviour."
fi
