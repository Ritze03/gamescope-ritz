#!/usr/bin/env bash
# effects-regression.sh -- one-command pixel regression gate for the Shaders
# area's Adaptive Brightness, both modes, measured off real captures with NO
# visible window and no laptop round trip. Non-zero exit gates a commit.
#
# RECIPE
#   The same private headless sway + nested `gamescope --backend wayland` +
#   `gamescopectl screenshot "<path> 4"` recipe as pixel-regression.sh (read
#   that script's header for why gamescope's own headless backend is not
#   usable for this and why the runtime dir must be short). The client is
#   build-release/tests/effects_scene_client (tests/effects_scene_client.c):
#   an SDL2 window painting three synthetic reference scenes from flat
#   regions at known pixel positions, advanced with SIGUSR1 -- no PNG
#   decoding, no terminal emulator, no input injection. The regions are
#   sampled by scripts/effects_regression_sample.py.
#
# WHAT EACH CHECK VERIFIES (superdoc/features/shader-effects.md, "Adaptive
# Brightness" -> "Dynamic" has the curve and the expected numbers):
#   <scene>-off      -- effect off is the identity (the capture path itself
#                       is honest: bands read back what the client painted)
#   dark-dynamic     -- the darkest band (5) is lifted to >= 30, the 240
#                       highlights stay < 255 and above every band, pure
#                       black stays <= 2, band order is kept
#   bright-dynamic   -- the 245 band comes down below 235, the 30 shadows
#                       are dimmed no further than 30 x min_gain (0.5) and
#                       never crushed, order kept
#   mid-dynamic      -- a 0.1..0.9 scene is left alone to within 6 counts
#   temporal-band2   -- switching dark -> bright with Dynamic on: the middle
#                       band's value at 0.2 s / 1 s / 3 s approaches the
#                       settled value monotonically (no oscillation) and is
#                       within 12 counts of it at 3 s (tau = 1 s)
#   Whole-image captures of every scene are taken too and reported as INFO
#   lines (they show the clipping Dynamic exists to avoid), not asserted.
#
# USAGE
#   scripts/effects-regression.sh                # run everything
#   scripts/effects-regression.sh --keep         # leave the instance up
#
# Exits 0 if every check passed, 1 if any failed, 2 on a setup problem.
# Runs under with-gamescope-lock.sh (self-re-execs under it).
set -euo pipefail

if [[ -z "${EFFECTS_REGRESSION_LOCKED:-}" ]]; then
	export EFFECTS_REGRESSION_LOCKED=1
	SCRIPT_DIR_BOOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	exec "$SCRIPT_DIR_BOOT/with-gamescope-lock.sh" "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SAMPLER="$SCRIPT_DIR/effects_regression_sample.py"
GAMESCOPE_BIN="$REPO_ROOT/build-release/src/gamescope"
GAMESCOPECTL_BIN="$REPO_ROOT/build-release/src/gamescopectl"
CLIENT_BIN="$REPO_ROOT/build-release/tests/effects_scene_client"

OUT_W=1280
OUT_H=720
READY_TIMEOUT_S=20
SWAY_READY_TIMEOUT_S=10
SCREENSHOT_TIMEOUT_S=6
SETTLE_S=0.3      # after a mode change; the history is already warm, this is for the composite
ADAPT_SETTLE_S=5  # after a scene switch, > 4 tau (tau = 1 s) before a "settled" capture

AB_ID="image.shaders.adaptive_brightness"        # Switch: on/off
AB_MODE_ID="image.shaders.adaptive_brightness.mode"  # Param, a Choice: 0 Whole image, 1 Dynamic

KEEP=0
OUT_LABEL=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--keep) KEEP=1; shift ;;
		--label) OUT_LABEL="$2"; shift 2 ;;
		-h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "effects-regression: unknown argument: $1" >&2; exit 2 ;;
	esac
done

TS="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$REPO_ROOT/build-release/verify-shots/effects-regression/${OUT_LABEL:-$TS}"
mkdir -p "$OUT_DIR"
RESULTS_FILE="$OUT_DIR/results.txt"

RUNDIR="$(mktemp -d /tmp/gs-fxreg-run.XXXXXX)"
CONFIGHOME="$(mktemp -d /tmp/gs-fxreg-cfg.XXXXXX)"
SWAY_CFG="$RUNDIR/sway.conf"
PIDFILE="$RUNDIR/client.pid"

SWAY_PID=""
GS_PID=""

log() { echo "[effects-regression] $*" >&2; }

teardown_instance() {
	if [[ -n "$GS_PID" ]] && kill -0 "$GS_PID" 2>/dev/null; then
		kill "$GS_PID" 2>/dev/null || true
		wait "$GS_PID" 2>/dev/null || true
	fi
	GS_PID=""
}
teardown_all() {
	teardown_instance
	if [[ -n "$SWAY_PID" ]] && kill -0 "$SWAY_PID" 2>/dev/null; then
		kill "$SWAY_PID" 2>/dev/null || true
		wait "$SWAY_PID" 2>/dev/null || true
	fi
	SWAY_PID=""
}
cleanup() {
	local status=$?
	if [[ "$KEEP" -eq 1 ]]; then
		log "--keep: leaving sway (pid ${SWAY_PID:-none}) and gamescope (pid ${GS_PID:-none}) running."
		log "  XDG_RUNTIME_DIR=$RUNDIR  XDG_CONFIG_HOME=$CONFIGHOME"
		[[ -n "${GS_WL_NAME:-}" ]] && log "  Poke it with: XDG_RUNTIME_DIR=$RUNDIR GAMESCOPE_WAYLAND_DISPLAY=$GS_WL_NAME $GAMESCOPECTL_BIN <cmd>"
		exit "$status"
	fi
	teardown_all
	rm -rf "$RUNDIR" "$CONFIGHOME"
	exit "$status"
}
trap cleanup EXIT INT TERM

for bin in "$GAMESCOPE_BIN" "$GAMESCOPECTL_BIN" "$CLIENT_BIN"; do
	[[ -x "$bin" ]] || { log "FATAL: $bin not found -- run scripts/build-gamescope-ritz.sh --test first."; exit 2; }
done
command -v sway >/dev/null 2>&1 || { log "FATAL: sway not found."; exit 2; }
python3 -c 'import PIL' 2>/dev/null || { log "FATAL: python3 PIL not importable."; exit 2; }

start_sway() {
	cat > "$SWAY_CFG" <<-EOF
		output HEADLESS-1 resolution ${OUT_W}x${OUT_H} position 0,0
		default_border none
		default_floating_border none
		gaps inner 0
		gaps outer 0
		focus_follows_mouse no
		seat seat0 hide_cursor 1
	EOF
	log "starting private sway (headless, XDG_RUNTIME_DIR=$RUNDIR)"
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 XDG_RUNTIME_DIR="$RUNDIR" \
		sway -c "$SWAY_CFG" > "$RUNDIR/sway.log" 2>&1 9>&- &
	SWAY_PID=$!
	local waited=0
	SWAY_WL_NAME=""
	while (( waited < SWAY_READY_TIMEOUT_S * 10 )); do
		SWAY_WL_NAME="$(find "$RUNDIR" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' -printf '%f\n' 2>/dev/null | head -1)"
		[[ -n "$SWAY_WL_NAME" ]] && break
		kill -0 "$SWAY_PID" 2>/dev/null || { log "FATAL: sway exited -- see $RUNDIR/sway.log"; cat "$RUNDIR/sway.log" >&2; exit 2; }
		sleep 0.1; waited=$((waited + 1))
	done
	[[ -n "$SWAY_WL_NAME" ]] || { log "FATAL: sway never created a socket"; exit 2; }
	log "private sway ready: pid $SWAY_PID, socket $SWAY_WL_NAME"
}

# Everything off; the HUD and crosshair too, so the captures are the game
# alone. Adaptive Brightness's parameters are the schema defaults (target
# 0.5, 1 s / 1 s, gains 0.5..2.0, strength 1.0) -- the numbers the doc quotes.
write_config() {
	mkdir -p "$CONFIGHOME/gamescope-ritz/profiles"
	cat > "$CONFIGHOME/gamescope-ritz/global.json" <<-EOF
		{ "schema_version": 3, "profiles": { "last_general": "Effects", "games": {} } }
	EOF
	cat > "$CONFIGHOME/gamescope-ritz/profiles/Effects.json" <<-EOF
		{
		    "schema_version": 3,
		    "name": "Effects",
		    "kind": "general",
		    "fps_display": { "enabled": false },
		    "crosshair": { "enabled": false },
		    "reshade": {
		        "vibrancy": { "enabled": false },
		        "pre_sharpen": { "enabled": false },
		        "shadow_lift": { "enabled": false },
		        "adaptive_brightness": { "enabled": false, "mode": "whole_image", "strength": 1.0 }
		    }
		}
	EOF
}

GS_LOG=""
GS_WL_NAME=""
CLIENT_LOG=""
start_instance() {
	teardown_instance
	GS_LOG="$RUNDIR/gamescope.log"
	CLIENT_LOG="$OUT_DIR/client.log"
	: > "$CLIENT_LOG"
	rm -f "$PIDFILE"
	log "starting gamescope + effects_scene_client (dark,bright,mid)"
	WAYLAND_DISPLAY="$SWAY_WL_NAME" XDG_RUNTIME_DIR="$RUNDIR" XDG_CONFIG_HOME="$CONFIGHOME" \
		"$GAMESCOPE_BIN" --backend wayland -w "$OUT_W" -h "$OUT_H" -W "$OUT_W" -H "$OUT_H" \
		--force-windows-fullscreen -- \
		sh -c "SDL_VIDEODRIVER=x11 exec '$CLIENT_BIN' --scenes dark,bright,mid --width $OUT_W --height $OUT_H --seconds 600 --pidfile '$PIDFILE' > '$CLIENT_LOG' 2>&1" \
		> "$GS_LOG" 2>&1 9>&- &
	GS_PID=$!

	local waited=0
	GS_WL_NAME=""
	while (( waited < READY_TIMEOUT_S * 10 )); do
		GS_WL_NAME="$(grep -oP "wayland display '\K[^']+" "$GS_LOG" 2>/dev/null | head -1 || true)"
		[[ -n "$GS_WL_NAME" ]] && break
		kill -0 "$GS_PID" 2>/dev/null || { log "FATAL: gamescope exited early -- see $GS_LOG"; tail -n 40 "$GS_LOG" >&2; exit 2; }
		sleep 0.1; waited=$((waited + 1))
	done
	[[ -n "$GS_WL_NAME" ]] || { log "FATAL: no wayland display within ${READY_TIMEOUT_S}s"; tail -n 40 "$GS_LOG" >&2; exit 2; }

	waited=0
	while (( waited < READY_TIMEOUT_S * 10 )); do
		[[ -s "$PIDFILE" ]] && grep -q "refresh cycle" "$GS_LOG" 2>/dev/null && break
		kill -0 "$GS_PID" 2>/dev/null || { log "FATAL: gamescope exited before the client presented -- see $GS_LOG"; tail -n 40 "$GS_LOG" >&2; exit 2; }
		sleep 0.1; waited=$((waited + 1))
	done
	CLIENT_PID="$(cat "$PIDFILE" 2>/dev/null || true)"
	[[ -n "$CLIENT_PID" ]] || { log "FATAL: the client never wrote its pid"; exit 2; }
	sleep 1   # let the first frames land before the first capture
	log "instance ready: gamescope pid $GS_PID, client pid $CLIENT_PID, control socket $GS_WL_NAME"
}

gsctl() {
	XDG_RUNTIME_DIR="$RUNDIR" GAMESCOPE_WAYLAND_DISPLAY="$GS_WL_NAME" "$GAMESCOPECTL_BIN" "$@"
}

set_ab() {   # 0 off, 1 whole image, 2 dynamic
	# 2026-09-06 (requests-2026-09-07.md item 7): the row went back to a
	# plain Switch and the mode moved to its own Param
	# (image.shaders.adaptive_brightness.mode, 0 whole_image / 1 dynamic),
	# addressable the same way any Param is (Shell.cpp's overlay_e2_set
	# resolves a Param id through Registry::FindParam() exactly like an
	# Entry id). Off leaves the mode alone, matching the panel's own
	# behaviour; the caller's 0/1/2 vocabulary is unchanged.
	case "$1" in
		0) gsctl overlay_e2_set "$AB_ID 0" >/dev/null 2>&1 || true ;;
		1) gsctl overlay_e2_set "$AB_MODE_ID 0" >/dev/null 2>&1 || true
		   gsctl overlay_e2_set "$AB_ID 1" >/dev/null 2>&1 || true ;;
		2) gsctl overlay_e2_set "$AB_MODE_ID 1" >/dev/null 2>&1 || true
		   gsctl overlay_e2_set "$AB_ID 1" >/dev/null 2>&1 || true ;;
	esac
	sleep "$SETTLE_S"
}

next_scene() {
	# Only ever the pid the client itself wrote -- never a name match.
	kill -USR1 "$CLIENT_PID"
}

take_screenshot() {
	local name="$1" path="$OUT_DIR/$1.png"
	gsctl screenshot "$path 4" >/dev/null 2>&1 || true
	local waited=0 last_size=-1 size=0
	while (( waited < SCREENSHOT_TIMEOUT_S * 20 )); do
		if [[ -f "$path" ]]; then
			size="$(stat -c%s "$path" 2>/dev/null || echo 0)"
			if [[ "$size" -gt 0 && "$size" == "$last_size" ]]; then echo "$path"; return 0; fi
			last_size="$size"
		fi
		sleep 0.05; waited=$((waited + 1))
	done
	log "WARNING: screenshot $name never stabilised"
	echo "$path"
}

declare -a RESULT_LINES=()
FAIL_COUNT=0
record_line() {
	local line="$1" status name detail
	IFS=$'\t' read -r status name detail <<<"$line"
	RESULT_LINES+=("$status	$name	$detail")
	[[ "$status" == "FAIL" ]] && FAIL_COUNT=$((FAIL_COUNT + 1))
	log "$status  $name  $detail"
}
run_sampler() {
	local out
	out="$(python3 "$SAMPLER" "$@" 2>>"$OUT_DIR/sampler.log")" || true
	record_line "$out"
}
band2_of() {   # middle-band value out of a `regions` INFO line's image
	python3 "$SAMPLER" regions "$1" "$2" | grep -oP 'band2=\K[0-9.]+'
}

# ---------------------------------------------------------------------------
# One scene's three captures: off (asserted identity), whole image (INFO),
# dynamic (asserted). Leaves Dynamic ON so the next scene switch is measured
# under it.
# ---------------------------------------------------------------------------
capture_scene() {
	local scene="$1" idx="$2" shot
	set_ab 0; shot="$(take_screenshot "${idx}-${scene}-off")";     run_sampler check "$shot" "$scene" off
	set_ab 1; shot="$(take_screenshot "${idx}-${scene}-whole")";   run_sampler regions "$shot" "$scene"
	           record_line "INFO	${scene}-whole	(above: Whole image, not asserted)"
	set_ab 2; shot="$(take_screenshot "${idx}-${scene}-dynamic")"; run_sampler check "$shot" "$scene" dynamic
}

START_TS=$(date +%s)
write_config
start_sway
start_instance

# Scene 1: dark. Then switch to bright under Dynamic and watch it adapt.
capture_scene dark 01

elapsed() { python3 -c "import time,sys; print(f'{time.time()-float(sys.argv[1]):.2f}')" "$1"; }
T0=$(date +%s.%N)
next_scene
sleep 0.2;  S1="$(take_screenshot 02-bright-dynamic-t0.2s)"; E1=$(elapsed "$T0")
sleep 0.75; S2="$(take_screenshot 02-bright-dynamic-t1s)";   E2=$(elapsed "$T0")
sleep 1.9;  S3="$(take_screenshot 02-bright-dynamic-t3s)";   E3=$(elapsed "$T0")
sleep "$ADAPT_SETTLE_S"
S4="$(take_screenshot 02-bright-dynamic-settled)"
record_line "INFO	temporal-elapsed	captures requested at ${E1}s ${E2}s ${E3}s after the switch (tau 1 s)"
run_sampler regions "$S1" bright; run_sampler regions "$S2" bright; run_sampler regions "$S3" bright
run_sampler temporal "$(band2_of "$S4" bright)" "$(band2_of "$S1" bright)" "$(band2_of "$S2" bright)" "$(band2_of "$S3" bright)" band2

# Scene 2: bright, settled.
capture_scene bright 03

# Scene 3: mid.
next_scene
sleep "$ADAPT_SETTLE_S"
capture_scene mid 04

END_TS=$(date +%s)
{
	echo "effects-regression.sh -- $TS"
	echo "runtime: $((END_TS - START_TS))s"
	echo "binary: $(sha256sum "$GAMESCOPE_BIN" | cut -c1-12)  commit: $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
	echo
	printf '%-8s %-24s %s\n' "STATUS" "CHECK" "DETAIL"
	for line in "${RESULT_LINES[@]}"; do
		IFS=$'\t' read -r status name detail <<<"$line"
		printf '%-8s %-24s %s\n' "$status" "$name" "$detail"
	done
} | tee "$RESULTS_FILE" >&2

log "captures + results: $OUT_DIR"
[[ "$FAIL_COUNT" -gt 0 ]] && exit 1
exit 0
