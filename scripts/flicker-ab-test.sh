#!/usr/bin/env bash
# flicker-ab-test.sh — A/B test kit for the fullscreen VRR flicker the user sees on
# real hardware with our build but not with stock /usr/bin/gamescope --adaptive-sync.
# See superdoc/planning/flicker-ab-test-plan.md for the test plan this drives:
# what each variant isolates and what conclusion follows from each outcome.
#
# Usage:
#   scripts/flicker-ab-test.sh <variant> [options]
#   scripts/flicker-ab-test.sh --list
#
# Variants (all but "upstream" are ONE binary — ./build/src/gamescope — selected by
# environment variable, per the test plan's ordering, most-informative first):
#   overlay-inert              our HEAD, but the settings overlay and FPS HUD never
#                               initialize ImGui, allocate a texture, or add a layer,
#                               for the whole process lifetime (not just "closed").
#   no-dynamic-rendering-ext   our HEAD, but device creation does not enable the
#                               VK_KHR_dynamic_rendering extension string (M1 change).
#   no-convar-seed             our HEAD, but startup does not seed cv_adaptive_sync/
#                               cv_hdr_enabled/cv_tearing_enabled from the gamescope-ritz
#                               config system before argv parsing.
#   normal                     our HEAD, unmodified — the known-bad case.
#   upstream                   the tree built at fcc1341 (this fork's exact upstream
#                               base) — the control. Needs its own build directory,
#                               see the test plan doc for the one-time setup command.
#
# Options:
#   --no-vrr            do not pass --adaptive-sync (default: pass it). Use this to
#                        check whether the flicker survives without VRR at all.
#   --output NAME        target connector for -O/--prefer-output (default: DP-1)
#   --refresh HZ         nested refresh rate for -r (default: autodetected from
#                        --output via hyprctl, falling back to 280)
#   --width W            output width for -W (default: autodetected, falling back to 1920)
#   --height H           output height for -H (default: autodetected, falling back to 1080)
#   --client CMD         test client launched inside gamescope (default: vkcube)
#   --backend NAME        gamescope --backend (default: sdl — nested under the host
#                        compositor, matching the user's own two-layer VRR setup)
#   -h, --help           show this help
#
# Binary locations (override with these env vars if your layout differs):
#   GAMESCOPE_RITZ_AB_HEAD_BIN      our HEAD binary (default: <repo>/build/src/gamescope)
#   GAMESCOPE_RITZ_AB_UPSTREAM_BIN  upstream fcc1341 binary
#                                   (default: <repo>/build-upstream-fcc1341/src/gamescope)
#
# Fullscreen note: gamescope's own -f/--fullscreen (SDL_WINDOW_FULLSCREEN_DESKTOP) is
# used and verified to have actually landed, via a READ-ONLY `hyprctl clients -j` check
# for fullscreen:2 on the gamescope window. This script does NOT call `hyprctl dispatch`
# or probe Hyprland's Lua dispatch API — a previous agent blind-probed that and closed
# one of the user's windows. If -f alone doesn't land the window on --output, this
# script prints a clear WARNING and tells you to focus that monitor yourself first
# (click on it) before re-running — it will not try to move the window itself.
#
# DISABLE_LSFG=1 is exported unconditionally: the system-wide lsfg-vk implicit Vulkan
# layer aborts Vulkan clients otherwise, and every launch here is a Vulkan launch.
#
# XDG_CONFIG_HOME is set to a fresh throwaway directory per run, so no run ever reads
# or writes the caller's real ~/.config/gamescope-ritz or ~/.config/gamescope.
#
# Teardown: gamescope renames its own worker thread to "gamescope-wl" (see
# src/wlserver.cpp), so `pgrep -x gamescope` never matches it — this script never
# relies on that. Cleanup is scoped to this run's own process group (like
# scripts/overlay-test-harness.sh's RUN_PGID pattern) so it can never touch another
# run's or another shell's processes.

set -u -o pipefail

export DISABLE_LSFG=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

HEAD_BIN="${GAMESCOPE_RITZ_AB_HEAD_BIN:-$REPO_ROOT/build/src/gamescope}"
UPSTREAM_BIN="${GAMESCOPE_RITZ_AB_UPSTREAM_BIN:-$REPO_ROOT/build-upstream-fcc1341/src/gamescope}"

DO_VRR=1
TARGET_OUTPUT="DP-1"
REFRESH_HZ=""
OUT_W=""
OUT_H=""
CLIENT_CMD="vkcube"
BACKEND="sdl"
VARIANT=""

log()  { echo "[flicker-ab] $*"; }
fail() { log "FAIL: $*"; exit 1; }

usage() { sed -n '2,58p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# ---------- args ----------
if [[ $# -eq 0 ]]; then usage; exit 1; fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --list)
      echo "overlay-inert no-dynamic-rendering-ext no-convar-seed normal upstream"
      exit 0
      ;;
    -h|--help) usage; exit 0 ;;
    --no-vrr) DO_VRR=0; shift ;;
    --output) TARGET_OUTPUT="$2"; shift 2 ;;
    --refresh) REFRESH_HZ="$2"; shift 2 ;;
    --width) OUT_W="$2"; shift 2 ;;
    --height) OUT_H="$2"; shift 2 ;;
    --client) CLIENT_CMD="$2"; shift 2 ;;
    --backend) BACKEND="$2"; shift 2 ;;
    overlay-inert|no-dynamic-rendering-ext|no-convar-seed|normal|upstream)
      VARIANT="$1"; shift ;;
    *) fail "unknown argument: $1 (see --help)" ;;
  esac
done
[[ -n "$VARIANT" ]] || fail "no variant given. One of: overlay-inert no-dynamic-rendering-ext no-convar-seed normal upstream (see --help)"

# ---------- resolve binary + env for the chosen variant ----------
GAMESCOPE_BIN=""
declare -a VARIANT_ENV=()
case "$VARIANT" in
  overlay-inert)
    GAMESCOPE_BIN="$HEAD_BIN"
    VARIANT_ENV+=(GAMESCOPE_RITZ_AB_NO_OVERLAY=1)
    ;;
  no-dynamic-rendering-ext)
    GAMESCOPE_BIN="$HEAD_BIN"
    VARIANT_ENV+=(GAMESCOPE_RITZ_AB_NO_DYNAMIC_RENDERING_EXT=1)
    ;;
  no-convar-seed)
    GAMESCOPE_BIN="$HEAD_BIN"
    VARIANT_ENV+=(GAMESCOPE_RITZ_AB_NO_CONVAR_SEED=1)
    ;;
  normal)
    GAMESCOPE_BIN="$HEAD_BIN"
    ;;
  upstream)
    GAMESCOPE_BIN="$UPSTREAM_BIN"
    ;;
esac

[[ -x "$GAMESCOPE_BIN" ]] || fail "$GAMESCOPE_BIN not found/executable. Build it first — see superdoc/planning/flicker-ab-test-plan.md's setup step for this variant. Refusing to fall back to \$PATH gamescope: that would silently test the wrong binary."

for kv in "${VARIANT_ENV[@]+"${VARIANT_ENV[@]}"}"; do
  export "${kv?}"
done

# ---------- throwaway XDG_CONFIG_HOME ----------
CONFIG_TMP="$(mktemp -d "${TMPDIR:-/tmp}/flicker-ab-config.XXXXXX")"
export XDG_CONFIG_HOME="$CONFIG_TMP"

# ---------- best-effort output detection (read-only hyprctl, never dispatch) ----------
if command -v hyprctl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
  MON_JSON="$(hyprctl monitors -j 2>/dev/null | jq -c --arg n "$TARGET_OUTPUT" '.[] | select(.name==$n)')"
  if [[ -n "$MON_JSON" ]]; then
    [[ -z "$OUT_W" ]] && OUT_W="$(jq -r '.width' <<<"$MON_JSON")"
    [[ -z "$OUT_H" ]] && OUT_H="$(jq -r '.height' <<<"$MON_JSON")"
    [[ -z "$REFRESH_HZ" ]] && REFRESH_HZ="$(jq -r '.refreshRate | floor' <<<"$MON_JSON")"
    IS_FOCUSED="$(jq -r '.focused' <<<"$MON_JSON")"
    if [[ "$IS_FOCUSED" != "true" ]]; then
      log "WARNING: $TARGET_OUTPUT is not the currently focused monitor. -f fullscreens on whichever monitor the window opens on; click/focus $TARGET_OUTPUT yourself before running if it doesn't land there. This script will not dispatch a move for you."
    fi
  else
    log "WARNING: hyprctl monitors doesn't report an output named '$TARGET_OUTPUT' — falling back to defaults"
  fi
else
  log "WARNING: hyprctl+jq not available — falling back to hardcoded defaults, and fullscreen verification below will be skipped"
fi
[[ -z "$OUT_W" || "$OUT_W" == "null" ]] && OUT_W=1920
[[ -z "$OUT_H" || "$OUT_H" == "null" ]] && OUT_H=1080
[[ -z "$REFRESH_HZ" || "$REFRESH_HZ" == "null" ]] && REFRESH_HZ=280

# ---------- teardown ----------
GS_PID=""
RUN_PGID="$(ps -o pgid= -p $$ | tr -d ' ')"

cleanup() {
  local ec="${1:-$?}"
  set +e
  trap - EXIT INT TERM
  if [[ -n "$GS_PID" ]] && kill -0 "$GS_PID" 2>/dev/null; then
    log "tearing down gamescope (pid $GS_PID) and its children"
    kill -TERM "$GS_PID" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$GS_PID" 2>/dev/null || break; sleep 0.2; done
    kill -0 "$GS_PID" 2>/dev/null && kill -KILL "$GS_PID" 2>/dev/null
    wait "$GS_PID" 2>/dev/null
  fi
  # Belt-and-braces, scoped to THIS run's process group only — never a bare
  # system-wide pkill, which could hit another concurrent run or the calling shell.
  pkill -TERM -g "$RUN_PGID" -f "$CLIENT_CMD" 2>/dev/null
  sleep 0.3
  pkill -KILL -g "$RUN_PGID" -f "$CLIENT_CMD" 2>/dev/null
  rm -rf "$CONFIG_TMP"
  exit "$ec"
}
trap cleanup EXIT
trap 'cleanup 130' INT
trap 'cleanup 143' TERM

# ---------- banner ----------
log "=============================================================="
log "VARIANT          : $VARIANT"
log "binary            : $GAMESCOPE_BIN"
if [[ ${#VARIANT_ENV[@]} -eq 0 ]]; then
  log "variant env       : (none)"
else
  log "variant env       : ${VARIANT_ENV[*]}"
fi
log "output/refresh    : $TARGET_OUTPUT, ${OUT_W}x${OUT_H}@${REFRESH_HZ}Hz"
log "adaptive-sync      : $([[ $DO_VRR -eq 1 ]] && echo yes || echo no)"
log "backend            : $BACKEND"
log "client             : $CLIENT_CMD"
log "XDG_CONFIG_HOME    : $CONFIG_TMP (throwaway)"
log "=============================================================="

# ---------- launch ----------
ARGS=(--backend "$BACKEND" -W "$OUT_W" -H "$OUT_H" -w "$OUT_W" -h "$OUT_H" -r "$REFRESH_HZ" -O "$TARGET_OUTPUT" -f)
[[ "$DO_VRR" -eq 1 ]] && ARGS+=(--adaptive-sync)

log "launching: $GAMESCOPE_BIN ${ARGS[*]} -- $CLIENT_CMD"
"$GAMESCOPE_BIN" "${ARGS[@]}" -- "$CLIENT_CMD" &
GS_PID=$!
log "pid $GS_PID"

sleep 3
if ! kill -0 "$GS_PID" 2>/dev/null; then
  fail "gamescope exited immediately — check the variant's binary/env above"
fi

# ---------- verify fullscreen actually happened (read-only, no dispatch) ----------
if command -v hyprctl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
  FS_STATE="$(hyprctl clients -j 2>/dev/null | jq -r '[.[] | select(.class|test("(?i)^gamescope$"))][0].fullscreen // "unknown"')"
  if [[ "$FS_STATE" == "2" ]]; then
    log "fullscreen verified: gamescope window reports fullscreen:2"
  else
    log "WARNING: gamescope window does NOT report fullscreen:2 (got: $FS_STATE) — this run is NOT exercising the fullscreen-only VRR/flicker path. Focus $TARGET_OUTPUT and re-run, or check the client actually started."
  fi
else
  log "WARNING: hyprctl+jq not available — could not verify fullscreen actually happened"
fi

log "running. Look at $TARGET_OUTPUT now. Press Ctrl+C here when done observing."
wait "$GS_PID"
GS_EXIT=$?
log "gamescope exited (code $GS_EXIT)"
