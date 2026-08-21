#!/usr/bin/env bash
# overlay-test-harness.sh — reusable manual-test loop for gamescope-ritz overlay milestones.
#
# What it does, in order:
#   1. (optionally) build ./build via meson/ninja
#   2. launch the LOCALLY BUILT ./build/src/gamescope, nested, with a test client (default: vkcube)
#   3. wait for it to actually report ready (via --ready-fd, not a fixed sleep)
#   4. (optionally) send a key combo through ydotool — --toggle is shorthand for the
#      overlay hotkey, Ctrl+Shift+O
#   5. capture a screenshot via gamescope's own `screenshot` debug convar (through gamescopectl)
#   6. tear everything down, always — trap on EXIT/INT/TERM, no orphaned processes
#
# Every milestone in superdoc/planning/SPEC.md is verified as "run this, look at what
# rendered" — this script is that loop, factored out so each milestone doesn't reinvent it.
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
#   --nested-size WxH    nested (game) surface size, default 640x480
#   --output-size WxH    output (gamescope window) size, default same as nested size
#   --backend NAME       gamescope backend, default: sdl (host session here is Wayland-nested)
#   --settle-ms N        extra wait after ready-fd fires before driving input/screenshot,
#                         to let the first few frames actually present (default: 500)
#   --out DIR            artifact directory (default: scratchpad-relative timestamped dir)
#   -h, --help           show this help
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
#
# ponytail: screenshot capture is exactly one method (gamescope's own `screenshot`
# convar over gamescopectl, base_plane_only). grim-on-host was investigated and does NOT
# work here because the nested gamescope window has no host-compositor surface grim can
# address in this SDL-nested setup — see the printed environment note. If a milestone
# later needs the composited overlay layer (not just the base plane), extend the
# TakeScreenshot call to pass screenshot_type=2 (full_composition); gamescopectl's CLI
# only forwards a single arg today so that needs a source change, not a flag here.

set -u -o pipefail

# ---------- defaults ----------
DO_BUILD=0
CLIENT_CMD="vkcube"
DO_TOGGLE=0
KEY_COMBO=""
NESTED_SIZE="640x480"
OUTPUT_SIZE=""
BACKEND="sdl"
SETTLE_MS=500
SCRATCH_ROOT="/tmp/claude-1000/-home-mo-GitHubProjects-gamescope-ritz/4093488d-4af1-4f8c-89e8-a866399b2919/scratchpad"
OUT_DIR=""
YDOTOOL_SOCKET_DEFAULT="/home/mo/.YDOTOOL-SOCKET"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() { sed -n '2,45p' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) DO_BUILD=1; shift ;;
    --client) CLIENT_CMD="$2"; shift 2 ;;
    --toggle) DO_TOGGLE=1; shift ;;
    --key) KEY_COMBO="$2"; shift 2 ;;
    --nested-size) NESTED_SIZE="$2"; shift 2 ;;
    --output-size) OUTPUT_SIZE="$2"; shift 2 ;;
    --backend) BACKEND="$2"; shift 2 ;;
    --settle-ms) SETTLE_MS="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

[[ -z "$OUTPUT_SIZE" ]] && OUTPUT_SIZE="$NESTED_SIZE"
NESTED_W="${NESTED_SIZE%x*}"; NESTED_H="${NESTED_SIZE#*x}"
OUTPUT_W="${OUTPUT_SIZE%x*}"; OUTPUT_H="${OUTPUT_SIZE#*x}"

TS="$(date +%Y%m%d-%H%M%S)"
[[ -z "$OUT_DIR" ]] && OUT_DIR="$SCRATCH_ROOT/overlay-harness-runs/$TS"
mkdir -p "$OUT_DIR"
GAMESCOPE_LOG="$OUT_DIR/gamescope.log"
SUMMARY="$OUT_DIR/summary.txt"

log() { echo "[harness] $*" | tee -a "$SUMMARY"; }
fail() { log "FAIL: $*"; exit 1; }

# ---------- teardown: always runs, on success, failure, or interrupt ----------
GS_PID=""
# The actual process-group id (not always == $$, depending on how this script was
# invoked) — used to scope cleanup to only this run's processes on a shared machine.
RUN_PGID="$(ps -o pgid= -p $$ | tr -d ' ')"
cleanup() {
  local ec=$?
  set +e
  if [[ -n "$GS_PID" ]] && kill -0 "$GS_PID" 2>/dev/null; then
    log "tearing down gamescope (pid $GS_PID) and its children"
    kill -TERM "$GS_PID" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$GS_PID" 2>/dev/null || break; sleep 0.2; done
    kill -0 "$GS_PID" 2>/dev/null && kill -KILL "$GS_PID" 2>/dev/null
  fi
  # Belt-and-braces: our own child tree can leave the client (vkcube) orphaned if
  # gamescope itself was already dead. Sweep only THIS run's process group ("-g $$")
  # — never a bare `pkill -f vkcube`, which would also hit unrelated gamescope/vkcube
  # runs elsewhere on a shared machine (e.g. other milestones' harness runs).
  pkill -TERM -g "$RUN_PGID" -f "$CLIENT_CMD" 2>/dev/null
  sleep 0.3
  pkill -KILL -g "$RUN_PGID" -f "$CLIENT_CMD" 2>/dev/null
  # These counts are scoped to our own process group too, for the same reason —
  # a system-wide `pgrep -f vkcube` would misreport other runs' processes as ours.
  log "leftover gamescope processes (this run's group): $(pgrep -g "$RUN_PGID" -f "$GAMESCOPE_BIN" | wc -l)"
  log "leftover vkcube-ish processes (this run's group): $(pgrep -g "$RUN_PGID" -f "$CLIENT_CMD" | wc -l)"
  log "artifacts in: $OUT_DIR"
  exit "$ec"
}
trap cleanup EXIT INT TERM

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
log "nested size      : ${NESTED_W}x${NESTED_H}, output ${OUTPUT_W}x${OUTPUT_H}, backend $BACKEND"

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

# ---------- launch gamescope nested, wait for real signals instead of a fixed sleep ----------
# `--ready-fd` (steamcompmgr.cpp's dprintf of nested X/wayland display names on the
# first fully-focused frame) was tried here first and investigated hard:
#   - pointed at a FIFO (mkfifo + `exec 9<>fifo`): gamescope reliably segfaults on
#     SIGTERM shutdown shortly after — reproduced repeatedly.
#   - pointed at a plain file: no crash, but the file was NEVER written even after
#     40s of polling, although gamescope was fully up and rendering the whole time
#     (confirmed: a manual `gamescopectl screenshot` against it succeeded and showed
#     the spinning vkcube). So in this SDL-nested/single-virtual-connector config,
#     the ready-fd write path is apparently never reached at all.
# ponytail: root cause not chased into steamcompmgr.cpp — reported as a finding.
# Instead: wait for the log line wlserver.cpp itself always prints early
# ("Running compositor on wayland display '<name>'") to learn the nested wayland
# display name, then wait briefly for evidence a client actually presented a frame
# ("Swapchain received new refresh cycle") before treating it as up. Both come from
# polling the log file, not a fixed sleep guess.
"$GAMESCOPE_BIN" \
  --backend "$BACKEND" \
  -w "$NESTED_W" -h "$NESTED_H" \
  -W "$OUTPUT_W" -H "$OUTPUT_H" \
  -- "$CLIENT_CMD" >"$GAMESCOPE_LOG" 2>&1 &
GS_PID=$!
log "gamescope launched, pid $GS_PID"

GS_WL_DISPLAY=""
for _ in $(seq 1 100); do   # up to 30s
  GS_WL_DISPLAY="$(grep -oP "wayland display '\K[^']+" "$GAMESCOPE_LOG" 2>/dev/null | head -1)"
  [[ -n "$GS_WL_DISPLAY" ]] && break
  kill -0 "$GS_PID" 2>/dev/null || break
  sleep 0.3
done
[[ -n "$GS_WL_DISPLAY" ]] || fail "gamescope never reported its wayland display name in time (see $GAMESCOPE_LOG); is the local build up to date?"
log "nested wayland display: $GS_WL_DISPLAY"
kill -0 "$GS_PID" 2>/dev/null || fail "gamescope exited before the client could present a frame (see $GAMESCOPE_LOG)"

for _ in $(seq 1 50); do   # up to 15s for the client's first presented frame
  grep -q "refresh cycle" "$GAMESCOPE_LOG" 2>/dev/null && break
  kill -0 "$GS_PID" 2>/dev/null || fail "gamescope exited before the client could present a frame (see $GAMESCOPE_LOG)"
  sleep 0.3
done

SETTLE_S="$(awk -v ms="$SETTLE_MS" 'BEGIN{printf "%.3f", ms/1000}')"
sleep "$SETTLE_S"

# ---------- optional input ----------
if [[ "$DO_TOGGLE" -eq 1 && -z "$KEY_COMBO" ]]; then
  KEY_COMBO="ctrl shift o"   # the overlay toggle hotkey per SPEC.md
fi
if [[ -n "$KEY_COMBO" ]]; then
  if [[ "$YDOTOOL_OK" -eq 1 ]]; then
    # translate space-separated keysym names to ydotool's key1:1 key1:0 down/up pairs
    read -r -a KEYS <<<"$KEY_COMBO"
    DOWN=(); UP=()
    for k in "${KEYS[@]}"; do DOWN+=("$k:1"); UP=("$k:0" "${UP[@]}"); done
    log "sending key combo: $KEY_COMBO"
    if ydotool key "${DOWN[@]}" "${UP[@]}" 2>>"$SUMMARY"; then
      log "ydotool key send: ok"
    else
      log "ydotool key send: FAILED (non-fatal, continuing)"
    fi
  else
    log "skipping key send ('$KEY_COMBO') — ydotool not usable, see note above"
  fi
fi

sleep 0.3

# ---------- screenshot ----------
SHOT_PATH="$OUT_DIR/screenshot.png"
if [[ -n "$GAMESCOPECTL_BIN" ]]; then
  GAMESCOPE_WAYLAND_DISPLAY="$GS_WL_DISPLAY" "$GAMESCOPECTL_BIN" screenshot "$SHOT_PATH" >>"$SUMMARY" 2>&1
  # screenshot_taken is reported async; poll briefly rather than guessing one sleep
  for _ in $(seq 1 20); do [[ -s "$SHOT_PATH" ]] && break; sleep 0.25; done
  if [[ -s "$SHOT_PATH" ]]; then
    log "screenshot captured via gamescopectl: $SHOT_PATH ($(stat -c%s "$SHOT_PATH") bytes)"
  else
    log "screenshot NOT captured — $SHOT_PATH is empty/missing (see summary log above for gamescopectl output)"
  fi
else
  log "screenshot skipped — gamescopectl not found (build it, or ensure ./build/gamescopectl exists)"
fi

log "PASS: gamescope launched nested, client ran, teardown will follow on exit"
# cleanup() runs automatically via the EXIT trap
