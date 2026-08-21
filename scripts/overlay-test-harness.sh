#!/usr/bin/env bash
# overlay-test-harness.sh — reusable manual-test loop for gamescope-ritz overlay milestones.
#
# What it does, in order:
#   1. (optionally) build ./build via meson/ninja
#   2. launch the LOCALLY BUILT ./build/src/gamescope, nested, with a test client (default: vkcube)
#   3. wait for it to actually report ready (via log polling, not a fixed sleep)
#   4. (optionally) send a key combo through ydotool — --toggle is shorthand for the
#      overlay hotkey, Ctrl+Shift+O
#   5. capture a screenshot via gamescope's own `screenshot` debug convar (through gamescopectl)
#   6. tear everything down, always — trap on EXIT/INT/TERM, no orphaned processes
#
# Every milestone in superdoc/planning/SPEC.md is verified as "run this, look at what
# rendered" — this script is that loop, factored out so each milestone doesn't reinvent it.
#
# 2026-08-21 hardening: this harness previously ran fixed at 640x480/windowed/SDL-only
# with no failure-mode detection, and missed a real GPUVM fault (invisible below native
# res), real VRR artifacting (unreachable without fullscreen), and a real Wayland-backend
# segfault (untested backend) as a result. Added: native-resolution-by-default,
# --fullscreen, --backend/--all-backends, --soak with GPU-fault log scanning, and a real
# pass/fail exit status — see the option descriptions below for what each one buys.
#
# =============================================================================
# WHY NOT SDL (2026-08-21) — READ BEFORE CHANGING THE --backend DEFAULT BACK
# =============================================================================
# The fullscreen VRR flicker chased for many rounds across this repo's planning docs is
# an SDL-BACKEND BUG, confirmed on real hardware: `DISABLE_LSFG=1 ./build/src/gamescope
# --backend sdl -f -- vkcube` flickers badly; the identical binary with NO --backend
# flag (which auto-selects Wayland — see auto_select_backend() in src/main.cpp) is
# completely clean. The user's packaged upstream 3.16.24 also flickers under SDL, so
# this is an upstream SDL-backend defect, not ours, and version-independent. It went
# unnoticed for a long time because a real user on a Wayland session never takes the
# SDL path — but this harness previously did, by defaulting --backend to sdl, and
# every earlier "SDL-only" run in this file's own history (see the note above) was
# quietly hiding that. The default below is "auto" (no --backend flag at all, so
# gamescope decides same as it would for a real user), not "sdl" and not a hardcoded
# "wayland". --backend sdl remains available and --all-backends still sweeps it — SDL
# coverage is still useful, now as a known-broken regression check, not blind default
# coverage. Draft upstream report:
# superdoc/planning/upstream-sdl-backend-flicker-report.md.
# =============================================================================
#
# Usage:
#   scripts/overlay-test-harness.sh [options]
#
# Options:
#   --build              run `meson setup build` (if needed) and `ninja -C build` first
#   --client CMD         test client to launch inside gamescope (default: vkcube)
#   --toggle             send the overlay hotkey (Ctrl+Shift+O) after launch — shorthand for
#                         --key "ctrl shift o"
#   --key "MODS KEY"     send an arbitrary ydotool key combo instead of/in addition to --toggle
#                         (space-separated ydotool keysym names, e.g. "ctrl shift o")
#   --nested-size WxH    nested (game) surface size, default: host's native resolution,
#                         auto-detected at runtime (see detect_output() below)
#   --output-size WxH    output (gamescope window) size, default same as nested size
#   --refresh HZ         nested refresh rate passed to gamescope -r; default: the detected
#                         output's own refresh rate (rounded), so a 280Hz panel gets tested
#                         at 280Hz, not gamescope's internal default
#   --output NAME        Hyprland monitor name to target for resolution detection and
#                         --fullscreen placement (default: autodetect — focused monitor,
#                         else the first one hyprctl reports)
#   --fullscreen          actually fullscreen gamescope's window on --output. Passes
#                         gamescope's own -f/--fullscreen AND reinforces it with
#                         `hyprctl dispatch` (move to output + fullscreen), because SDL's
#                         own fullscreen flag alone was not enough to reliably land the
#                         window on a *specific* multi-monitor output under Hyprland.
#   --adaptive-sync       pass --adaptive-sync to gamescope, and report back (from
#                         `hyprctl monitors`) whether VRR is actually reported active on
#                         the target output afterwards — a VRR test that silently isn't
#                         running VRR is worse than no test.
#   --backend NAME        gamescope backend: auto (default — no --backend flag passed,
#                         gamescope's own auto_select_backend() picks, same as a real
#                         user's session), or explicitly sdl, wayland, headless, drm,
#                         openvr. See the "WHY NOT SDL" block near the top of this file
#                         before changing this default back to sdl.
#   --all-backends         sweep sdl, wayland, headless against the same check and report
#                         PASS/FAIL per backend (see run_all_backends()). drm/openvr are
#                         excluded from the sweep: drm would try to take over a real KMS
#                         display and openvr needs a running VR runtime — neither is safe
#                         to fire unattended on a shared dev box.
#   --soak SECONDS         soak mode: stay up for SECONDS, toggling the overlay repeatedly
#                         (~1/s when ydotool is available), continuously scanning the log
#                         for GPU faults (see check_gpu_faults()) and process liveness.
#                         Fails the run immediately if either trips.
#   --check-log FILE      standalone mode: run check_gpu_faults() against an existing log
#                         file and exit 0/1 accordingly. Launches nothing. Exists so the
#                         fault-detection logic itself can be exercised/proven without
#                         needing to reproduce a real GPU fault first.
#   --settle-ms N         extra wait after ready before driving input/screenshot, to let
#                         the first few frames actually present (default: 500)
#   --out DIR             artifact directory (default: scratchpad-relative timestamped dir)
#   --real-config          use the caller's real $HOME/.config instead of a throwaway
#                         XDG_CONFIG_HOME (default is throwaway — see below)
#   -h, --help             show this help
#
# Requires (checked at startup, reported plainly if missing — see README notes below):
#   ./build/src/gamescope — the LOCAL build under test. This script refuses to fall back
#                           to /usr/bin/gamescope: testing the system package instead of
#                           the tree under development is the exact mistake this harness
#                           exists to prevent.
#   gamescopectl          — for the screenshot convar. Built alongside gamescope; looked
#                           for at ./build/src/gamescopectl first, then PATH.
#   ydotool + ydotoold    — for input synthesis (--toggle/--key). If ydotoold isn't
#                           reachable, input steps are skipped with a clear warning, not
#                           a silent no-op.
#   hyprctl                — for native-resolution detection, --fullscreen placement, and
#                           VRR reporting. Falls back to wlr-randr, then xrandr, then a
#                           hardcoded 1920x1080 guess (loudly logged as a fallback) if
#                           none are available. --fullscreen and VRR reporting need
#                           hyprctl specifically (Hyprland dispatch/monitor-state); they
#                           degrade to a warning, not a crash, without it.
#
# Environment isolation:
#   DISABLE_LSFG=1 is exported unless already set — the system-wide lsfg-vk implicit
#   Vulkan layer (gate var confirmed against the installed manifest at
#   /etc/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json; NOT the
#   similarly-named DISABLE_LSFGVK some other lsfg-vk builds use) aborts Vulkan clients
#   here unless disabled, so this removes the silent dependency on the caller's shell
#   already having it set.
#   XDG_CONFIG_HOME defaults to a throwaway dir under $OUT_DIR so runs never touch the
#   caller's real ~/.config/gamescope-ritz/ (a prior run polluted it). --real-config opts out.
#
# ponytail: screenshot capture is exactly one method (gamescope's own `screenshot`
# convar over gamescopectl). grim-on-host was investigated and does NOT work here
# because the nested gamescope window has no host-compositor surface grim can
# address in this SDL-nested setup — see the printed environment note.
#
# M2 update: this now requests screenshot_type=3 (full_composition, see
# protocol/gamescope-control.xml's screenshot_type enum), not the M1-era default
# (base_plane_only, type 1) — M1 lost real time to base_plane_only structurally
# truncating any layer above zpos 2, which silently excludes the settings overlay
# from every screenshot this harness takes. Passing the type through required a
# small source fix, not just a flag: gamescopectl's own CLI and the
# gamescope_private.execute Wayland request only ever carried a command name plus
# ONE opaque value string, so a second "type" argument had nowhere to go on the
# wire. Fixed at the source (src/wlserver.cpp's gamescope_private_execute(), which
# now splits that value string on spaces the same way ConCommand::CallWithArgString()
# already splits a typed console command line) rather than widening the protocol —
# so from here, passing "$SHOT_PATH $SCREENSHOT_TYPE" as a single shell argument
# (space embedded, quoted) is sufficient; gamescopectl itself needed no change.
#
# Process-handling traps (hit for real while operating this harness by hand — fixed
# here, commented so nobody rediscovers them the hard way):
#   1. gamescope's main-thread comm is NOT what you'd expect: a *worker* thread inside
#      it renames itself to "gamescope-wl" (src/wlserver.cpp's wlserver_run(), via
#      pthread_setname_np) and comm-based matching ("pgrep -x gamescope") is not
#      reliable across ps/pgrep implementations as a result. So this script never
#      matches by bare comm/-x: liveness uses the tracked $GS_PID from job control
#      ("$!"); cleanup bookkeeping uses `pgrep -f` against the full binary PATH.
#   2. `pkill -f` against a path/args pattern can match the CALLING SHELL's own
#      command line if it happens to contain the same substring (e.g. the invocation
#      itself was `bash scripts/overlay-test-harness.sh ...`), killing the harness
#      instead of its target. Every pkill/pgrep below is scoped with `-g "$RUN_PGID"`
#      (this run's process group only) — see stop_current_run()/cleanup() — never a
#      bare substring broad enough to also catch a shell invocation.

set -u -o pipefail

# lsfg-vk's implicit layer aborts Vulkan clients unless disabled; see note above.
export DISABLE_LSFG="${DISABLE_LSFG:-1}"

# ---------- defaults ----------
DO_BUILD=0
CLIENT_CMD="vkcube"
DO_TOGGLE=0
KEY_COMBO=""
NESTED_SIZE=""
OUTPUT_SIZE=""
REFRESH_HZ=""
TARGET_OUTPUT=""
# "auto" = pass no --backend flag; gamescope's own auto_select_backend() then picks
# (Wayland, under a real Wayland session) -- NOT "sdl". See the "WHY NOT SDL" block
# above: the SDL backend is a confirmed upstream flicker bug. Do not default back to sdl.
BACKEND="auto"
ALL_BACKENDS=0
DO_FULLSCREEN=0
DO_ADAPTIVE_SYNC=0
SOAK_SECONDS=0
CHECK_LOG_FILE=""
SETTLE_MS=500
SCRATCH_ROOT="/tmp/claude-1000/-home-mo-GitHubProjects-gamescope-ritz/4093488d-4af1-4f8c-89e8-a866399b2919/scratchpad"
OUT_DIR=""
USE_REAL_CONFIG=0
YDOTOOL_SOCKET_DEFAULT="/home/mo/.YDOTOOL-SOCKET"
KNOWN_BACKENDS="auto sdl wayland headless drm openvr"
SWEEP_BACKENDS="sdl wayland headless"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() { sed -n '2,105p' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) DO_BUILD=1; shift ;;
    --client) CLIENT_CMD="$2"; shift 2 ;;
    --toggle) DO_TOGGLE=1; shift ;;
    --key) KEY_COMBO="$2"; shift 2 ;;
    --nested-size) NESTED_SIZE="$2"; shift 2 ;;
    --output-size) OUTPUT_SIZE="$2"; shift 2 ;;
    --refresh) REFRESH_HZ="$2"; shift 2 ;;
    --output) TARGET_OUTPUT="$2"; shift 2 ;;
    --fullscreen) DO_FULLSCREEN=1; shift ;;
    --adaptive-sync) DO_ADAPTIVE_SYNC=1; shift ;;
    --backend) BACKEND="$2"; shift 2 ;;
    --all-backends) ALL_BACKENDS=1; shift ;;
    --soak) SOAK_SECONDS="$2"; shift 2 ;;
    --check-log) CHECK_LOG_FILE="$2"; shift 2 ;;
    --settle-ms) SETTLE_MS="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --real-config) USE_REAL_CONFIG=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

TS="$(date +%Y%m%d-%H%M%S)"
[[ -z "$OUT_DIR" ]] && OUT_DIR="$SCRATCH_ROOT/overlay-harness-runs/$TS"
mkdir -p "$OUT_DIR"
SUMMARY="$OUT_DIR/summary.txt"

log() { echo "[harness] $*" | tee -a "$SUMMARY"; }
fail() { log "FAIL: $*"; exit 1; }

# ---------- standalone --check-log mode: prove the fault-detection logic works ----------
# GPU-fault/device-loss/validation markers this harness treats as a hard failure. The
# GPUVM fault that motivated this whole update was sitting in the log in plain text and
# nothing was grepping for it — this list (and check_gpu_faults()) is that grep, made
# reusable so it can be proven correct against a canned log, not just trusted blindly.
FAULT_PATTERN='GPUVM|DEVICE_LOST|context is lost|VK_ERROR_DEVICE_LOST|validation error|validation layer'
check_gpu_faults() {
  local logfile="$1"
  [[ -r "$logfile" ]] || return 1
  grep -iEo "$FAULT_PATTERN" "$logfile" 2>/dev/null | sort -u
}

if [[ -n "$CHECK_LOG_FILE" ]]; then
  [[ -r "$CHECK_LOG_FILE" ]] || fail "--check-log: '$CHECK_LOG_FILE' does not exist or is not readable"
  hits="$(check_gpu_faults "$CHECK_LOG_FILE")"
  if [[ -n "$hits" ]]; then
    log "FAIL: GPU fault markers found in $CHECK_LOG_FILE:"
    echo "$hits" | while read -r h; do log "  - $h"; done
    exit 1
  fi
  log "PASS: no GPU fault markers found in $CHECK_LOG_FILE"
  exit 0
fi

[[ -z "$OUTPUT_SIZE" && -n "$NESTED_SIZE" ]] && OUTPUT_SIZE="$NESTED_SIZE"

# ---------- native-resolution / output detection ----------
# hyprctl -> wlr-randr -> xrandr -> hardcoded guess, in that order. Only used to fill in
# values the user didn't explicitly override with --nested-size/--output-size/--output.
DET_OUTPUT_NAME=""; DET_W=""; DET_H=""; DET_HZ=""; DET_VRR=""
detect_output() {
  local want="$1"
  if command -v hyprctl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
    local raw; raw="$(hyprctl monitors -j 2>/dev/null)"
    if [[ -n "$raw" && "$raw" != "[]" ]]; then
      local sel
      if [[ -n "$want" ]]; then
        sel="$(jq -r --arg n "$want" '[.[] | select(.name==$n)][0]' <<<"$raw")"
      else
        sel="$(jq -r '([.[] | select(.focused==true)] + .)[0]' <<<"$raw")"
      fi
      if [[ -n "$sel" && "$sel" != "null" ]]; then
        DET_OUTPUT_NAME="$(jq -r '.name' <<<"$sel")"
        DET_W="$(jq -r '.width' <<<"$sel")"
        DET_H="$(jq -r '.height' <<<"$sel")"
        DET_HZ="$(jq -r '.refreshRate | floor' <<<"$sel")"
        DET_VRR="$(jq -r '.vrr' <<<"$sel")"
        log "detected output via hyprctl: $DET_OUTPUT_NAME ${DET_W}x${DET_H}@${DET_HZ}Hz (vrr currently: $DET_VRR)"
        return 0
      fi
    fi
  fi
  if command -v wlr-randr >/dev/null 2>&1; then
    # current mode line looks like: "  1920x1080 px, 60.000000 Hz (current)"
    local cur; cur="$(wlr-randr 2>/dev/null | grep '(current)' | head -1)"
    local wh; wh="$(grep -oP '\d+x\d+(?= px)' <<<"$cur")"
    if [[ -n "$wh" ]]; then
      DET_W="${wh%x*}"; DET_H="${wh#*x}"
      DET_HZ="$(grep -oP '[\d.]+(?= Hz)' <<<"$cur")"; DET_HZ="${DET_HZ%.*}"
      log "detected output via wlr-randr: ${DET_W}x${DET_H}@${DET_HZ:-60}Hz (name/VRR unknown via wlr-randr)"
      return 0
    fi
  fi
  if command -v xrandr >/dev/null 2>&1; then
    local wh; wh="$(xrandr 2>/dev/null | grep '\*' | grep -oP '^\d+x\d+' | head -1)"
    if [[ -n "$wh" ]]; then
      DET_W="${wh%x*}"; DET_H="${wh#*x}"
      log "detected output via xrandr: ${DET_W}x${DET_H} (refresh/name/VRR unknown via xrandr)"
      return 0
    fi
  fi
  DET_W=1920; DET_H=1080; DET_HZ=60
  log "WARNING: could not detect native resolution (no hyprctl+jq/wlr-randr/xrandr found or none reported a mode) — falling back to hardcoded ${DET_W}x${DET_H}@${DET_HZ}Hz, NOT the real host resolution"
  return 1
}
detect_output "$TARGET_OUTPUT"

[[ -z "$NESTED_SIZE" ]] && NESTED_SIZE="${DET_W}x${DET_H}"
[[ -z "$OUTPUT_SIZE" ]] && OUTPUT_SIZE="$NESTED_SIZE"
[[ -z "$REFRESH_HZ" && -n "$DET_HZ" ]] && REFRESH_HZ="$DET_HZ"
[[ -z "$TARGET_OUTPUT" && -n "$DET_OUTPUT_NAME" ]] && TARGET_OUTPUT="$DET_OUTPUT_NAME"

NESTED_W="${NESTED_SIZE%x*}"; NESTED_H="${NESTED_SIZE#*x}"
OUTPUT_W="${OUTPUT_SIZE%x*}"; OUTPUT_H="${OUTPUT_SIZE#*x}"

if [[ -n "$BACKEND" ]] && ! grep -qw "$BACKEND" <<<"$KNOWN_BACKENDS"; then
  fail "unknown --backend '$BACKEND' (known: $KNOWN_BACKENDS)"
fi

# ---------- throwaway XDG_CONFIG_HOME (default) ----------
if [[ "$USE_REAL_CONFIG" -eq 0 ]]; then
  export XDG_CONFIG_HOME="$OUT_DIR/xdg-config"
  mkdir -p "$XDG_CONFIG_HOME"
  log "XDG_CONFIG_HOME: $XDG_CONFIG_HOME (throwaway — pass --real-config to use ~/.config instead)"
else
  log "XDG_CONFIG_HOME: caller's real config (--real-config was passed)"
fi

# ---------- teardown: always runs, on success, failure, or interrupt ----------
GS_PID=""
# The actual process-group id (not always == $$, depending on how this script was
# invoked) — used to scope cleanup to only this run's processes on a shared machine.
RUN_PGID="$(ps -o pgid= -p $$ | tr -d ' ')"

# Shared by both the EXIT trap and the --all-backends sweep loop (which tears down and
# relaunches between iterations without exiting the script). See trap note above about
# why this only ever matches by tracked PID or by full binary path / client command,
# scoped to $RUN_PGID — never a bare process-name/comm guess.
#
# Sets LAST_TEARDOWN_CRASHED=1 if gamescope died by a crash signal here rather than
# exiting cleanly on our own TERM/KILL — found for real while building this: an
# intermittent SIGSEGV on ordinary shutdown, unrelated to any --ready-fd path, that a
# `kill -0`-only liveness check can never see (a zombie still answers kill -0 until
# reaped). A crash during shutdown we ourselves requested is still a real bug, not
# teardown noise, so it must not be swallowed silently.
LAST_TEARDOWN_CRASHED=0
stop_current_run() {
  LAST_TEARDOWN_CRASHED=0
  if [[ -n "$GS_PID" ]]; then
    if kill -0 "$GS_PID" 2>/dev/null; then
      log "tearing down gamescope (pid $GS_PID) and its children"
      kill -TERM "$GS_PID" 2>/dev/null
      for _ in $(seq 1 20); do kill -0 "$GS_PID" 2>/dev/null || break; sleep 0.2; done
      kill -0 "$GS_PID" 2>/dev/null && kill -KILL "$GS_PID" 2>/dev/null
    fi
    local ec=0
    wait "$GS_PID" 2>/dev/null; ec=$?
    if is_crash_exit_code "$ec"; then
      log "FAIL: gamescope crashed during its own shutdown (exit $ec, CRASH SIGNAL)"
      LAST_TEARDOWN_CRASHED=1
    fi
    GS_PID=""
  fi
  # Belt-and-braces: our own child tree can leave the client (vkcube) orphaned if
  # gamescope itself was already dead. Sweep only THIS run's process group ("-g
  # $RUN_PGID") — never a bare `pkill -f vkcube`, which would also hit unrelated
  # gamescope/vkcube runs elsewhere on a shared machine (e.g. other milestones' runs).
  pkill -TERM -g "$RUN_PGID" -f "$CLIENT_CMD" 2>/dev/null
  sleep 0.3
  pkill -KILL -g "$RUN_PGID" -f "$CLIENT_CMD" 2>/dev/null
}

cleanup() {
  # $1, if given, is the exit code to use (see the INT/TERM traps below) — an
  # interrupted run must not be able to inherit whatever ambient $? happened to be
  # set by the last inner command at the instant the signal landed (observed to be 0
  # mid-soak, e.g. from a `while [...]` test succeeding right before delivery), which
  # would misreport an aborted, incomplete run as a clean PASS. Plain EXIT (no arg)
  # keeps using $?, which by then is always an explicit `exit N`/`fail` from this
  # script's own body.
  local ec="${1:-$?}"
  set +e
  trap - EXIT INT TERM   # this function's own `exit` below must not re-trigger itself
  stop_current_run
  # These counts are scoped to our own process group too, for the same reason —
  # a system-wide `pgrep -f vkcube` would misreport other runs' processes as ours.
  # Matches the full binary PATH (cmdline), never a bare "gamescope" name/comm guess
  # — see the process-handling-traps note at the top of this file.
  log "leftover gamescope processes (this run's group): $(pgrep -g "$RUN_PGID" -f "$GAMESCOPE_BIN" 2>/dev/null | wc -l)"
  log "leftover vkcube-ish processes (this run's group): $(pgrep -g "$RUN_PGID" -f "$CLIENT_CMD" 2>/dev/null | wc -l)"
  log "artifacts in: $OUT_DIR"
  exit "$ec"
}
trap cleanup EXIT
trap 'cleanup 130' INT    # 128+SIGINT, standard convention
trap 'cleanup 143' TERM   # 128+SIGTERM

# ---------- resolve binaries (the local build, never the system package) ----------
GAMESCOPE_BIN="$REPO_ROOT/build/src/gamescope"
if [[ ! -x "$GAMESCOPE_BIN" && "$DO_BUILD" -eq 1 ]]; then
  log "building tree ($REPO_ROOT/build)"
  meson setup "$REPO_ROOT/build" "$REPO_ROOT" >>"$OUT_DIR/build.log" 2>&1 || fail "meson setup failed, see $OUT_DIR/build.log"
  ninja -C "$REPO_ROOT/build" >>"$OUT_DIR/build.log" 2>&1 || fail "ninja build failed, see $OUT_DIR/build.log"
fi
[[ -x "$GAMESCOPE_BIN" ]] || fail "$GAMESCOPE_BIN not found/executable. Build the tree first (--build), or run ninja -C build yourself. Refusing to fall back to \$PATH gamescope — that would test the wrong binary."

GAMESCOPECTL_BIN="$REPO_ROOT/build/src/gamescopectl"
[[ -x "$GAMESCOPECTL_BIN" ]] || GAMESCOPECTL_BIN="$(command -v gamescopectl || true)"

command -v "$CLIENT_CMD" >/dev/null 2>&1 || fail "test client '$CLIENT_CMD' not found on PATH"

log "gamescope binary : $GAMESCOPE_BIN"
log "gamescopectl     : ${GAMESCOPECTL_BIN:-<not found>}"
log "client           : $CLIENT_CMD"
log "nested size      : ${NESTED_W}x${NESTED_H}, output ${OUTPUT_W}x${OUTPUT_H}, refresh ${REFRESH_HZ:-<default>}Hz"
log "backend          : $BACKEND$( [[ $ALL_BACKENDS -eq 1 ]] && echo " (overridden by --all-backends sweep: $SWEEP_BACKENDS)" )"
log "fullscreen       : $([[ $DO_FULLSCREEN -eq 1 ]] && echo "yes, target output: ${TARGET_OUTPUT:-<autodetect>}" || echo "no")"
log "adaptive-sync    : $([[ $DO_ADAPTIVE_SYNC -eq 1 ]] && echo "yes" || echo "no")"
log "soak             : $([[ $SOAK_SECONDS -gt 0 ]] && echo "${SOAK_SECONDS}s" || echo "no")"

# ---------- ydotool availability (report plainly, don't pretend) ----------
YDOTOOL_OK=0
if command -v ydotool >/dev/null 2>&1; then
  export YDOTOOL_SOCKET="${YDOTOOL_SOCKET:-$YDOTOOL_SOCKET_DEFAULT}"
  if [[ -S "$YDOTOOL_SOCKET" ]] && pgrep -x ydotoold >/dev/null 2>&1; then
    YDOTOOL_OK=1
    log "ydotool: available, ydotoold running, socket $YDOTOOL_SOCKET"
  else
    log "ydotool: binary present but ydotoold/socket ($YDOTOOL_SOCKET) not reachable — input steps will be skipped"
  fi
else
  log "ydotool: not installed — input steps will be skipped"
fi

# ---------- key-name -> evdev keycode table (ydotool `key` wants raw codes only) ----------
# ydotool's `key` subcommand takes RAW numeric evdev keycodes only ("Since there's no
# way to know how many keyboard layouts are there in the world, we're using raw
# keycodes now" -- ydotool's own --help) and its own docs say a non-numeric token
# ("ctrl", "shift", ...) is silently treated as a bare delay, not an error and not a
# key press. Fixed here with an explicit name->keycode table (linux/input-event-codes.h)
# rather than passing names through; unmapped names fail loudly instead of silently
# no-opping.
keycode_for_name()
{
  case "$1" in
    ctrl|leftctrl)   echo 29 ;;
    rightctrl)       echo 97 ;;
    shift|leftshift) echo 42 ;;
    rightshift)      echo 54 ;;
    alt|leftalt)     echo 56 ;;
    rightalt)        echo 100 ;;
    super|meta|leftmeta) echo 125 ;;
    rightmeta)       echo 126 ;;
    o) echo 24 ;;
    a) echo 30 ;; b) echo 48 ;; c) echo 46 ;; d) echo 32 ;; e) echo 18 ;;
    f) echo 33 ;; g) echo 34 ;; h) echo 35 ;; i) echo 23 ;; j) echo 36 ;;
    k) echo 37 ;; l) echo 38 ;; m) echo 50 ;; n) echo 49 ;; p) echo 25 ;;
    q) echo 16 ;; r) echo 19 ;; s) echo 31 ;; t) echo 20 ;; u) echo 22 ;;
    v) echo 47 ;; w) echo 17 ;; x) echo 45 ;; y) echo 21 ;; z) echo 44 ;;
    0) echo 11 ;; 1) echo 2 ;; 2) echo 3 ;; 3) echo 4 ;; 4) echo 5 ;;
    5) echo 6 ;; 6) echo 7 ;; 7) echo 8 ;; 8) echo 9 ;; 9) echo 10 ;;
    space) echo 57 ;; enter|return) echo 28 ;; tab) echo 15 ;; esc|escape) echo 1 ;;
    *) echo "" ;;
  esac
}

send_key_combo() {
  local combo="$1"
  [[ "$YDOTOOL_OK" -eq 1 ]] || { log "skipping key send ('$combo') — ydotool not usable, see note above"; return 1; }
  read -r -a KEYS <<<"$combo"
  local DOWN=() UP=() BAD=0
  for k in "${KEYS[@]}"; do
    local code; code="$(keycode_for_name "$k")"
    if [[ -z "$code" ]]; then
      log "unknown key name in --key/--toggle: '$k' (add it to keycode_for_name() in this script) — aborting this key send"
      BAD=1
      break
    fi
    DOWN+=("$code:1"); UP=("$code:0" "${UP[@]}")
  done
  [[ "$BAD" -eq 0 ]] || return 1
  if ydotool key "${DOWN[@]}" "${UP[@]}" 2>>"$SUMMARY"; then
    return 0
  else
    log "ydotool key send: FAILED (non-fatal, continuing)"
    return 1
  fi
}

if [[ "$DO_TOGGLE" -eq 1 && -z "$KEY_COMBO" ]]; then
  KEY_COMBO="ctrl shift o"   # the overlay toggle hotkey per SPEC.md
fi

# ---------- crash-signal exit-code check ----------
# bash reports a process killed by signal N as exit status 128+N when you `wait` it.
# Distinguish "we asked it to die" (SIGTERM/SIGKILL, expected, not a failure) from
# "it actually crashed" (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE) so a segfault shows up
# as a hard failure instead of being folded into ordinary teardown.
is_crash_exit_code() {
  local code="$1"
  case "$code" in
    132|134|135|139) return 0 ;;  # SIGILL, SIGABRT, SIGBUS, SIGSEGV
    *) return 1 ;;
  esac
}

# Logs "gamescope exited $2" with its real exit code, flagging crash signals. Used at
# every early-death check point below instead of repeating the wait/is_crash dance.
report_early_death() {
  local backend="$1" stage="$2"
  local ec; wait "$GS_PID" 2>/dev/null; ec=$?
  log "[$backend] FAIL: gamescope exited $stage (exit $ec$(is_crash_exit_code "$ec" && echo ", CRASH SIGNAL"))"
  GS_PID=""  # already reaped above; stop_current_run() must not wait on it again
}

# ---------- one full launch+check cycle for a single backend ----------
# Returns 0 on PASS, 1 on FAIL. Writes into $1 (a per-backend subdirectory of $OUT_DIR).
run_one_backend() {
  local backend="$1" run_dir="$2"
  mkdir -p "$run_dir"
  local gs_log="$run_dir/gamescope.log"
  local run_failed=0

  local args=(-w "$NESTED_W" -h "$NESTED_H" -W "$OUTPUT_W" -H "$OUTPUT_H")
  # "auto" means: pass no --backend flag, let gamescope auto-select (see WHY NOT SDL).
  [[ "$backend" != "auto" ]] && args+=(--backend "$backend")
  [[ -n "$REFRESH_HZ" ]] && args+=(-r "$REFRESH_HZ")
  [[ "$DO_FULLSCREEN" -eq 1 ]] && args+=(-f)
  [[ "$DO_ADAPTIVE_SYNC" -eq 1 ]] && args+=(--adaptive-sync)

  log "[$backend] launching: $GAMESCOPE_BIN ${args[*]} -- $CLIENT_CMD"
  "$GAMESCOPE_BIN" "${args[@]}" -- "$CLIENT_CMD" >"$gs_log" 2>&1 &
  GS_PID=$!
  log "[$backend] pid $GS_PID"

  # `--ready-fd` was tried here first and investigated hard:
  #   - pointed at a FIFO (mkfifo + `exec 9<>fifo`): gamescope reliably segfaults on
  #     SIGTERM shutdown shortly after — reproduced repeatedly.
  #   - pointed at a plain file: no crash, but the file was NEVER written even after
  #     40s of polling, although gamescope was fully up and rendering the whole time.
  # ponytail: root cause not chased into steamcompmgr.cpp — reported as a finding.
  # Instead: poll the log for the line wlserver.cpp always prints early ("Running
  # compositor on wayland display '<name>'"), then for evidence a client actually
  # presented a frame ("Swapchain received new refresh cycle"). Headless has no
  # separate nested wayland display line in the same form, so also accept the client
  # simply having presented as sufficient readiness there.
  local gs_wl_display=""
  local ready=0
  for _ in $(seq 1 100); do   # up to 30s
    gs_wl_display="$(grep -oP "wayland display '\K[^']+" "$gs_log" 2>/dev/null | head -1)"
    [[ -n "$gs_wl_display" ]] && { ready=1; break; }
    kill -0 "$GS_PID" 2>/dev/null || break
    sleep 0.3
  done
  if [[ "$ready" -eq 0 && "$backend" != "headless" ]]; then
    if ! kill -0 "$GS_PID" 2>/dev/null; then
      report_early_death "$backend" "before reporting ready"
    else
      log "[$backend] FAIL: gamescope never reported its wayland display name in time (see $gs_log)"
    fi
    stop_current_run
    return 1
  fi
  [[ -n "$gs_wl_display" ]] && log "[$backend] nested wayland display: $gs_wl_display"

  if ! kill -0 "$GS_PID" 2>/dev/null; then
    report_early_death "$backend" "before the client could present a frame"
    stop_current_run   # sweep for an orphaned client even though gamescope is already dead
    return 1
  fi

  for _ in $(seq 1 50); do   # up to 15s for the client's first presented frame
    grep -q "refresh cycle" "$gs_log" 2>/dev/null && break
    if ! kill -0 "$GS_PID" 2>/dev/null; then
      report_early_death "$backend" "before the client could present a frame"
      stop_current_run
      return 1
    fi
    sleep 0.3
  done

  local settle_s; settle_s="$(awk -v ms="$SETTLE_MS" 'BEGIN{printf "%.3f", ms/1000}')"
  sleep "$settle_s"

  # ---------- optional fullscreen reinforcement via hyprctl ----------
  # ponytail: gamescope's own -f flag (SDL_WINDOW_FULLSCREEN_DESKTOP) was verified
  # sufficient on its own during development — it lands on the correct output at full
  # native size. This block is a best-effort extra push for setups where it isn't. Some
  # Hyprland builds have moved `dispatch` to a Lua-call ABI (`hl.dispatch(...)`) that
  # rejects the classic "dispatcher arg" string form used below; that's exactly the
  # "insufficient" case this exists for, so it degrades to a logged WARNING and keeps
  # going rather than chasing an undocumented, version-specific Lua API here.
  if [[ "$DO_FULLSCREEN" -eq 1 && "$backend" != "headless" ]]; then
    if command -v hyprctl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
      # Hyprland >= 0.56 dispatches are LUA ONLY. The classic
      # `hyprctl dispatch <name> <args>` string form is rejected (by hyprctl AND
      # by the raw IPC socket) with "expected a dispatcher (e.g.
      # hl.dsp.window.close())" — which hyprctl reports on stderr while still
      # exiting 0, so the old code here silently believed it had fullscreened
      # the window when it had done nothing at all. Every --fullscreen run on
      # this machine was therefore testing a TILED window, which is precisely
      # the configuration the VRR artifacting cannot be reproduced in.
      #
      # Correct form: pass a Lua expression that RETURNS a dispatcher; hyprctl
      # wraps it in hl.dispatch(...) itself. Resolve the window by address, not
      # by focusing a class regex, and ASSERT the end state — window.fullscreen()
      # toggles, so firing it blind can just as easily leave the window windowed.
      local addr
      addr="$(hyprctl clients -j 2>/dev/null | jq -r '[.[] | select(.class|test("(?i)^gamescope$"))][0].address')"
      if [[ -n "$addr" && "$addr" != "null" ]]; then
        [[ -n "$TARGET_OUTPUT" ]] && hyprctl dispatch \
          "hl.dsp.window.move({ window = \"address:$addr\", monitor = \"$TARGET_OUTPUT\" })" >>"$SUMMARY" 2>&1
        sleep 0.5
        local fsnow
        fsnow="$(hyprctl clients -j 2>/dev/null | jq -r --arg a "$addr" '.[] | select(.address==$a) | .fullscreen')"
        if [[ "$fsnow" != "2" ]]; then
          hyprctl dispatch "hl.dsp.window.fullscreen({ window = \"address:$addr\", mode = 0 })" >>"$SUMMARY" 2>&1
          sleep 0.5
          fsnow="$(hyprctl clients -j 2>/dev/null | jq -r --arg a "$addr" '.[] | select(.address==$a) | .fullscreen')"
        fi
        if [[ "$fsnow" == "2" ]]; then
          log "[$backend] hyprctl: gamescope window is FULLSCREEN on ${TARGET_OUTPUT:-current output} (verified fullscreen=2)"
        else
          log "[$backend] WARNING: gamescope window is NOT fullscreen (fullscreen=$fsnow) — the fullscreen-only VRR path is NOT being exercised"
        fi
      else
        log "[$backend] WARNING: could not find the gamescope window by class via hyprctl clients — fullscreen reinforcement skipped"
      fi
    else
      log "[$backend] WARNING: --fullscreen needs hyprctl+jq — relying on gamescope's own -f flag only"
    fi
  fi

  # ---------- VRR reporting ----------
  if [[ "$DO_ADAPTIVE_SYNC" -eq 1 && -n "$TARGET_OUTPUT" ]] && command -v hyprctl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
    sleep 0.3
    local vrr_now
    vrr_now="$(hyprctl monitors -j 2>/dev/null | jq -r --arg n "$TARGET_OUTPUT" '.[] | select(.name==$n) | .vrr')"
    log "[$backend] VRR reported active on $TARGET_OUTPUT: ${vrr_now:-<unknown>} (--adaptive-sync was requested; if this is 'false', the VRR path is NOT actually being exercised)"
  fi

  # ---------- optional input ----------
  if [[ -n "$KEY_COMBO" ]]; then
    log "[$backend] sending key combo: $KEY_COMBO"
    send_key_combo "$KEY_COMBO" && log "[$backend] ydotool key send: ok"
  fi

  sleep 0.3

  # ---------- soak: stay up, toggle repeatedly, watch for GPU faults ----------
  if [[ "$SOAK_SECONDS" -gt 0 ]]; then
    log "[$backend] soak: running ${SOAK_SECONDS}s with repeated overlay toggles"
    local elapsed=0
    while [[ "$elapsed" -lt "$SOAK_SECONDS" ]]; do
      if ! kill -0 "$GS_PID" 2>/dev/null; then
        report_early_death "$backend" "during soak at ${elapsed}s"
        run_failed=1
        break
      fi
      local faults; faults="$(check_gpu_faults "$gs_log")"
      if [[ -n "$faults" ]]; then
        log "[$backend] FAIL: GPU fault marker(s) during soak at ${elapsed}s: $(echo "$faults" | tr '\n' ' ')"
        run_failed=1
        break
      fi
      [[ -n "$KEY_COMBO" ]] && send_key_combo "$KEY_COMBO" >/dev/null 2>&1
      sleep 1
      elapsed=$((elapsed + 1))
    done
    log "[$backend] soak finished after ${elapsed}s"
  fi

  # ---------- screenshot ----------
  if [[ "$run_failed" -eq 0 ]]; then
    local shot_path="$run_dir/screenshot.png"
    # screenshot_type 3 == full_composition (protocol/gamescope-control.xml) -- see the
    # M2 update note above the top-of-file ponytail comment for why this has to be a
    # single shell argument with the type embedded after a space, not a separate one.
    if [[ -n "$GAMESCOPECTL_BIN" ]]; then
      GAMESCOPE_WAYLAND_DISPLAY="$gs_wl_display" "$GAMESCOPECTL_BIN" screenshot "$shot_path 3" >>"$SUMMARY" 2>&1
      for _ in $(seq 1 20); do [[ -s "$shot_path" ]] && break; sleep 0.25; done
      if [[ -s "$shot_path" ]]; then
        log "[$backend] screenshot captured: $shot_path ($(stat -c%s "$shot_path") bytes)"
      else
        log "[$backend] FAIL: screenshot NOT captured — $shot_path is empty/missing"
        run_failed=1
      fi
    else
      log "[$backend] screenshot skipped — gamescopectl not found (build it, or ensure ./build/gamescopectl exists)"
    fi
  fi

  # ---------- final fault scan (not just soak — every run gets checked) ----------
  local faults; faults="$(check_gpu_faults "$gs_log")"
  if [[ -n "$faults" ]]; then
    log "[$backend] FAIL: GPU fault marker(s) found in log: $(echo "$faults" | tr '\n' ' ')"
    run_failed=1
  fi

  stop_current_run
  [[ "$LAST_TEARDOWN_CRASHED" -eq 1 ]] && run_failed=1
  return "$run_failed"
}

# ---------- dispatch: single backend, or the --all-backends sweep ----------
declare -A BACKEND_RESULT
if [[ "$ALL_BACKENDS" -eq 1 ]]; then
  overall=0
  for b in $SWEEP_BACKENDS; do
    if run_one_backend "$b" "$OUT_DIR/$b"; then
      BACKEND_RESULT[$b]="PASS"
    else
      BACKEND_RESULT[$b]="FAIL"
      overall=1
    fi
  done
  log "===== per-backend results ====="
  for b in $SWEEP_BACKENDS; do
    log "  $b: ${BACKEND_RESULT[$b]}"
  done
  if [[ "$overall" -eq 0 ]]; then
    log "PASS: all backends ($SWEEP_BACKENDS) passed"
  else
    log "FAIL: at least one backend failed — see per-backend results above"
  fi
  exit "$overall"
else
  if run_one_backend "$BACKEND" "$OUT_DIR"; then
    log "PASS: backend $BACKEND — gamescope launched, client ran, screenshot captured, no GPU faults, teardown will follow on exit"
    exit 0
  else
    log "FAIL: backend $BACKEND — see errors above"
    exit 1
  fi
fi
# cleanup() runs automatically via the EXIT trap
