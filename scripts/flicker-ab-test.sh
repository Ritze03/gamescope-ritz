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
# Upstream regression bisection variants (pure upstream, zero of our code — see
# superdoc/planning/upstream-flicker-regression.md for what each isolates):
#   upstream-3.16.25            upstream at the 3.16.25 tag — 20 commits after 3.16.24
#                               (known clean), 63 before fcc1341 (known flickers).
#   upstream-prebuffermemo      upstream one commit before eb1b304 ("BufferMemo: do not
#                               assert when a destroyed buffer still has texture
#                               references") — a debug build, so if that commit's race
#                               is the cause this variant should *assert/abort*, not
#                               flicker, giving an unambiguous signal instead of a
#                               "watch and squint" one.
# Optimisation-confound variants (THE PAIR THAT MATTERS -- run these two first).
# Both are gamescope 3.16.24, the exact same source. The ONLY difference is the
# optimisation level, and that alone decides whether VRR ever engages:
#
#     3.16.24 debug/-O0  (upstream-3.16.24-debug)     measured 231 fps  -> BELOW 280Hz
#     3.16.24 release    (packaged-3.16.24-release)   measured 282 fps  -> AT the cap
#
# A panel only varies its refresh when the frame rate is BELOW its maximum. Above it,
# the panel pins at max, which is fixed-refresh behaviour -- VRR is effectively off.
# So the debug build exercises the VRR path and the release build does not, from
# identical source. If the debug build flickers and the packaged one does not, then
# "stock gamescope is clean" was measuring optimisation level, not source version, and
# the 3.16.24 -> fcc1341 bisection above is moot.
#   upstream-3.16.24-debug      upstream at tag 3.16.24 -- the user's exact packaged
#                               version -- but built debug/-O0 to match every other
#                               control here. One-time setup:
#                                 git clone --recurse-submodules -b 3.16.24 \
#                                   https://github.com/ValveSoftware/gamescope.git \
#                                   ../gamescope-upstream-3.16.24
#                                 cp subprojects/catch2.wrap ../gamescope-upstream-3.16.24/subprojects/
#                                 meson setup ../gamescope-upstream-3.16.24/build \
#                                   --buildtype=debug -Doptimization=0
#                                 ninja -C ../gamescope-upstream-3.16.24/build src/gamescope
#   packaged-3.16.24-release    the user's installed /usr/bin/gamescope 3.16.24 (a
#                               CachyOS optimised release build) -- the same source as
#                               the variant above, so the pair isolates optimisation.
#
#   upstream-wlroots020         upstream right after the wlroots 0.20 bump (fc6a965),
#                               which also carries the earlier wlroots 0.19 bump and the
#                               BufferMemo fix — brackets everything up to the final
#                               rendervulkan layer-stack refactor (6ab4a7d) and the
#                               steamcompmgr focus-window churn after it.
#
# Options:
#   --no-vrr            do not pass --adaptive-sync (default: pass it). Use this to
#                        check whether the flicker survives without VRR at all.
#   --output NAME        target connector for -O/--prefer-output (default: DP-1)
#   --refresh HZ         nested refresh rate for -r (default: autodetected from
#                        --output via hyprctl, falling back to 280)
#   --width W            output width for -W (default: autodetected, falling back to 1920)
#   --height H           output height for -H (default: autodetected, falling back to 1080)
#   --nested-width W      nested/client width for -w -- the resolution the client
#                        actually renders at (default: same as --width, i.e. 1:1 with
#                        the output, no scaling required -- every run before this flag
#                        existed used that default). THE LEADING HYPOTHESIS as of
#                        2026-08-21: the user's own working gaming launch always scales
#                        (-w 1280 -h 960 against -W 1920 -H 1080) and no test in this
#                        investigation had ever done that until this flag existed. See
#                        superdoc/planning/client-variation-test.md.
#   --nested-height H     nested/client height for -h (default: same as --height)
#   --client CMD         test client launched inside gamescope (default: vkcube). Any
#                        of vkcube, vkcubepp, vkgears, glxgears (installed on this
#                        system today; vkcube-wayland/glmark2 are NOT installed — this
#                        script never installs anything).
#   --present-mode N     append --present_mode N to a vkcube/vkcubepp client (0
#                        immediate, 1 mailbox, 2 fifo, 3 fifo-relaxed — see vkcube
#                        --help). No effect on non-vkcube clients. vkcube's own
#                        built-in default (no flag at all) is already FIFO (2), per
#                        upstream Vulkan-Tools cube.cpp — so --present-mode 2 should be
#                        a behavioural no-op vs. the plain default; the informative
#                        comparison is --present-mode 0 (immediate, tears by design)
#                        against no flag at all.
#   --extra-args "STR"   extra raw args appended to the gamescope command line, split
#                        on whitespace (e.g. a gaming-style launch:
#                        --extra-args "--immediate-flips --filter fsr --sharpness 5
#                        -S stretch --force-grab-cursor --expose-wayland" — this is
#                        the user's actual running CS2 launch's flag set, minus the
#                        per-run -W/-H/-S sizing).
#   --backend NAME        gamescope --backend. Default: none passed at all — gamescope's
#                        own auto_select_backend() picks (Wayland, when WAYLAND_DISPLAY
#                        is set, exactly like a real user's session). See the "WHY NOT
#                        SDL" block near the top of this file before changing this
#                        default back to sdl. Pass --backend sdl explicitly when you
#                        deliberately want the known-broken SDL path — it's confirmed
#                        broken, so running it on purpose is a regression check now,
#                        not the default.
#   -h, --help           show this help
#
# Binary locations (override with these env vars if your layout differs):
#   GAMESCOPE_RITZ_AB_HEAD_BIN                our HEAD binary (default: <repo>/build/src/gamescope)
#   GAMESCOPE_RITZ_AB_UPSTREAM_BIN             upstream fcc1341 binary
#                                              (default: <repo>/build-upstream-fcc1341/src/gamescope)
#   GAMESCOPE_RITZ_AB_UPSTREAM_31625_BIN       upstream 3.16.25 tag binary
#                                              (default: ../gamescope-upstream-3.16.25/build/src/gamescope)
#   GAMESCOPE_RITZ_AB_UPSTREAM_PREBUFFERMEMO_BIN  upstream eb1b304~1 binary
#                                              (default: ../gamescope-upstream-prebuffermemo/build/src/gamescope)
#   GAMESCOPE_RITZ_AB_UPSTREAM_WLROOTS020_BIN  upstream fc6a965 binary
#                                              (default: ../gamescope-upstream-wlroots020/build/src/gamescope)
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

# RECORD THE FRAME RATE AND THE BUILD TYPE WITH EVERY RESULT.
# Two hidden variables have already invalidated conclusions in this investigation, and
# both looked like "it's intermittent":
#   1. Frame rate. VRR only varies the refresh interval when the frame rate is BELOW
#      the panel's maximum (280Hz on DP-1). An uncapped vkcube on this GPU sits at or
#      above that, which pins the panel at max = fixed refresh = the VRR path is never
#      exercised. A "clean" result from an uncapped run says nothing about this bug.
#   2. Optimisation level. Every control built for this investigation is debug/-O0;
#      the packaged /usr/bin/gamescope is an optimised release build. Measured, that
#      difference alone moves vkcube from 231 fps to 282 fps -- i.e. across the 280Hz
#      threshold, so it decides whether VRR engages at all. See the
#      upstream-3.16.24-debug / packaged-3.16.24-release pair below.
# A result without both numbers recorded cannot be interpreted.
#
# =============================================================================
# WHY NOT SDL (2026-08-21) — READ BEFORE CHANGING THE BACKEND DEFAULT BACK
# =============================================================================
# RESOLVED: the flicker this whole kit was built to chase is an SDL-BACKEND BUG,
# confirmed on real hardware. `DISABLE_LSFG=1 ./build/src/gamescope --backend sdl -f
# -- vkcube` flickers badly; the identical binary with NO --backend (which auto-selects
# Wayland — see auto_select_backend() in src/main.cpp) is completely clean. The user's
# packaged upstream 3.16.24 also flickers under SDL, so this is an upstream SDL-backend
# defect, not ours, and it is version-independent -- it explains every "flickers"
# result this investigation ever produced, because every one of them came through this
# script defaulting to --backend sdl. Every "clean" result was a manual run with no
# --backend, or the user's real (Wayland-session) gaming.
#
# The default below is therefore NO --backend flag at all (gamescope auto-selects,
# same as a real user's Wayland session), not "sdl" and not a hardcoded "wayland" --
# this is what actually gets exercised in the field. --backend sdl remains available
# and is still useful: it is now a *known-broken* regression check, not blind coverage.
# Draft upstream report: superdoc/planning/upstream-sdl-backend-flicker-report.md.
# =============================================================================

set -u -o pipefail

export DISABLE_LSFG=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

HEAD_BIN="${GAMESCOPE_RITZ_AB_HEAD_BIN:-$REPO_ROOT/build/src/gamescope}"
UPSTREAM_BIN="${GAMESCOPE_RITZ_AB_UPSTREAM_BIN:-$REPO_ROOT/build-upstream-fcc1341/src/gamescope}"
UPSTREAM_31625_BIN="${GAMESCOPE_RITZ_AB_UPSTREAM_31625_BIN:-$REPO_ROOT/../gamescope-upstream-3.16.25/build/src/gamescope}"
UPSTREAM_PREBUFFERMEMO_BIN="${GAMESCOPE_RITZ_AB_UPSTREAM_PREBUFFERMEMO_BIN:-$REPO_ROOT/../gamescope-upstream-prebuffermemo/build/src/gamescope}"
UPSTREAM_WLROOTS020_BIN="${GAMESCOPE_RITZ_AB_UPSTREAM_WLROOTS020_BIN:-$REPO_ROOT/../gamescope-upstream-wlroots020/build/src/gamescope}"
UPSTREAM_31624_DEBUG_BIN="${GAMESCOPE_RITZ_AB_UPSTREAM_31624_DEBUG_BIN:-$REPO_ROOT/../gamescope-upstream-3.16.24/build/src/gamescope}"
PACKAGED_BIN="${GAMESCOPE_RITZ_AB_PACKAGED_BIN:-/usr/bin/gamescope}"

DO_VRR=1
TARGET_OUTPUT="DP-1"
REFRESH_HZ=""
OUT_W=""
OUT_H=""
CLIENT_CMD="vkcube"
PRESENT_MODE=""
EXTRA_ARGS=""
NESTED_W=""
NESTED_H=""
# "auto" = pass no --backend flag at all; gamescope's own auto_select_backend() then
# picks (Wayland, under a real Wayland session) -- NOT "sdl". See the "WHY NOT SDL"
# block above: every flicker this kit ever reproduced went through --backend sdl, and
# the SDL backend is now a confirmed upstream bug. Do not default this back to sdl.
BACKEND="auto"
VARIANT=""

log()  { echo "[flicker-ab] $*"; }
fail() { log "FAIL: $*"; exit 1; }

usage() { sed -n '2,107p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# ---------- args ----------
if [[ $# -eq 0 ]]; then usage; exit 1; fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --list)
      echo "overlay-inert no-dynamic-rendering-ext no-convar-seed normal upstream upstream-3.16.25 upstream-prebuffermemo upstream-wlroots020 upstream-3.16.24-debug packaged-3.16.24-release"
      exit 0
      ;;
    -h|--help) usage; exit 0 ;;
    --no-vrr) DO_VRR=0; shift ;;
    --output) TARGET_OUTPUT="$2"; shift 2 ;;
    --refresh) REFRESH_HZ="$2"; shift 2 ;;
    --width) OUT_W="$2"; shift 2 ;;
    --height) OUT_H="$2"; shift 2 ;;
    --client) CLIENT_CMD="$2"; shift 2 ;;
    --present-mode) PRESENT_MODE="$2"; shift 2 ;;
    --extra-args) EXTRA_ARGS="$2"; shift 2 ;;
    --nested-width) NESTED_W="$2"; shift 2 ;;
    --nested-height) NESTED_H="$2"; shift 2 ;;
    --backend) BACKEND="$2"; shift 2 ;;
    overlay-inert|no-dynamic-rendering-ext|no-convar-seed|normal|upstream|upstream-3.16.25|upstream-prebuffermemo|upstream-wlroots020|upstream-3.16.24-debug|packaged-3.16.24-release)
      VARIANT="$1"; shift ;;
    *) fail "unknown argument: $1 (see --help)" ;;
  esac
done
[[ -n "$VARIANT" ]] || fail "no variant given. One of: overlay-inert no-dynamic-rendering-ext no-convar-seed normal upstream upstream-3.16.25 upstream-prebuffermemo upstream-wlroots020 upstream-3.16.24-debug packaged-3.16.24-release (see --help)"

# ---------- build the client command line (client + optional --present_mode) ----------
# CLIENT_CMD is word-split, not shell-eval'd -- keep it to plain space-separated
# tokens (matches every client this script targets: vkcube, vkcubepp, vkgears,
# glxgears take no arguments that need quoting).
read -ra CLIENT_ARGS <<<"$CLIENT_CMD"
if [[ -n "$PRESENT_MODE" ]]; then
  case "${CLIENT_ARGS[0]}" in
    *vkcube*) CLIENT_ARGS+=(--present_mode "$PRESENT_MODE") ;;
    *) log "WARNING: --present-mode has no effect on client '${CLIENT_ARGS[0]}' (only vkcube/vkcubepp support it) -- ignoring" ;;
  esac
fi
read -ra EXTRA_ARGS_ARR <<<"$EXTRA_ARGS"

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
  upstream-3.16.25)
    GAMESCOPE_BIN="$UPSTREAM_31625_BIN"
    ;;
  upstream-prebuffermemo)
    GAMESCOPE_BIN="$UPSTREAM_PREBUFFERMEMO_BIN"
    ;;
  upstream-wlroots020)
    GAMESCOPE_BIN="$UPSTREAM_WLROOTS020_BIN"
    ;;
  upstream-3.16.24-debug)
    GAMESCOPE_BIN="$UPSTREAM_31624_DEBUG_BIN"
    ;;
  packaged-3.16.24-release)
    GAMESCOPE_BIN="$PACKAGED_BIN"
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
# Nested (-w/-h, the client's own render size) defaults to the output size (-W/-H) --
# i.e. 1:1, no scaling required. That default matches every run before this flag
# existed. Pass --nested-width/--nested-height smaller than --width/--height to force
# gamescope to scale every frame -- the user's own working gaming launch always does
# this (-w 1280 -h 960 against -W 1920 -H 1080), which no test in this investigation
# had done until this flag existed. See superdoc/planning/client-variation-test.md.
[[ -z "$NESTED_W" ]] && NESTED_W="$OUT_W"
[[ -z "$NESTED_H" ]] && NESTED_H="$OUT_H"

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
log "nested (-w/-h)     : ${NESTED_W}x${NESTED_H} $([[ "$NESTED_W" == "$OUT_W" && "$NESTED_H" == "$OUT_H" ]] && echo '(1:1, no scaling)' || echo '(SCALED to output)')"
log "adaptive-sync      : $([[ $DO_VRR -eq 1 ]] && echo yes || echo no)"
log "backend            : $([[ "$BACKEND" == "auto" ]] && echo "auto (no --backend flag -- gamescope auto-selects, see WHY NOT SDL above)" || echo "$BACKEND")"
log "client             : ${CLIENT_ARGS[*]}"
log "extra gamescope args: ${EXTRA_ARGS_ARR[*]:-(none)}"
log "XDG_CONFIG_HOME    : $CONFIG_TMP (throwaway)"
log "=============================================================="

# ---------- launch ----------
ARGS=(-W "$OUT_W" -H "$OUT_H" -w "$NESTED_W" -h "$NESTED_H" -r "$REFRESH_HZ" -O "$TARGET_OUTPUT" -f)
# "auto" means: pass no --backend flag, let gamescope auto-select (see WHY NOT SDL).
[[ "$BACKEND" != "auto" ]] && ARGS+=(--backend "$BACKEND")
[[ "$DO_VRR" -eq 1 ]] && ARGS+=(--adaptive-sync)
[[ ${#EXTRA_ARGS_ARR[@]} -gt 0 ]] && ARGS+=("${EXTRA_ARGS_ARR[@]}")

log "launching: $GAMESCOPE_BIN ${ARGS[*]} -- ${CLIENT_ARGS[*]}"
"$GAMESCOPE_BIN" "${ARGS[@]}" -- "${CLIENT_ARGS[@]}" &
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
