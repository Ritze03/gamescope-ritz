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
#   bright-dynamic   -- the 245 band comes down below 235, the 30 shadows are
#                       dimmed no further than 30 x min_gain (0.3, widened
#                       from 0.5 on 2026-09-07) and never crushed, order kept
#   mid-dynamic      -- a 0.1..0.9 scene is left alone to within 6 counts
#   temporal-band2   -- switching dark -> bright with Dynamic on: the middle
#                       band's value at 0.2 s / 1 s / 3 s approaches the
#                       settled value monotonically (no oscillation) and is
#                       within 12 counts of it at 3 s (tau = 1 s)
#   transition-gain  -- the same dark -> bright switch, per frame, from
#                       gamescope's `effects_ab_log` readback: the gain and
#                       the probe pixel come down monotonically, never
#                       overshoot where they settle, and are within 5 % of
#                       settled inside 4 s
#   ag-speed-*       -- ADAPTIVE GAMMA'S OWN ADAPTATION SPEED (2026-09-09):
#                       the same dark -> bright step at a fast, a default and
#                       a slow setting must settle at increasing times, and
#                       with the two directions set differently a brightening
#                       and a darkening step must settle far apart. See the
#                       block near the bottom of this script
#   stability-pan    -- a textured dark scene with 2 % lights (the 98th
#                       percentile sits in the histogram's gap) panning under
#                       the tap grid with --periodic, so its true statistics
#                       never change: over 300 frames the gain moves <= 0.06,
#                       the smoothed p98 <= 3 codes, the raw p98 <= 40 codes
#                       (the rank-cut estimator this replaced read 0.81 /
#                       16 / 107 -- "the whole image pulsates", 2026-09-07)
#   stability-static -- the same scene held still: the raw measurement and
#                       the output pixel do not move AT ALL over 300 frames,
#                       the smoothed p98 by < 0.1 code and the gain by < 0.002
#                       (the EMA finishing its last fraction of a percent).
#                       Run three times, at Local adaptation 0 / 50 / 100 %:
#                       the local map must not reintroduce the pulse the
#                       2026-09-07 estimator fix removed
#   halfsplit-off    -- the split scene with the effect off is the identity
#   split-local      -- THE HEADLINE CHECK (2026-09-07): the same half-dark /
#                       half-bright frame under Dynamic at Local adaptation
#                       0 % vs 100 %. Every dark-half band must lift, the
#                       darkest to >= 30, while the bright half is not pushed
#                       up at all and its 245 band stays off the ceiling --
#                       "both halves serviceable", as two sets of numbers
#   halo-halobox-*   -- a flat 200 field with a flat 10 box, and haloinv the
#   halo-haloinv-*      inverse. At Local adaptation 0 the field must come out
#                       FLAT (the control); at 50 % and 100 % the profile out
#                       from the box's edge must be monotone (a ramp, not a
#                       ring) and its amplitude within 12 counts
#   local-pan-*      -- INFO: the same panning scene at three local strengths,
#                       with the map's range and the extreme gains it produces
#   target-moves     -- 2026-09-08: Target brightness 0.3 -> 0.5 -> 0.7 on the
#   maxgain-moves       textured dark scene must brighten the frame by >= 15
#                       counts of frame mean per step, and Max gain 1.5 -> 2
#                       -> 3 likewise. Before the fix both had steps of
#                       EXACTLY zero over the top of their range -- the
#                       user's "does nothing at all", as a number
#   Whole-image captures of every scene are taken too and reported as INFO
#   lines (they show the clipping Dynamic exists to avoid), not asserted.
#   ag-*             2026-09-08, ADAPTIVE GAMMA: ag-noclip-* (a pure
#                     exponent cannot clip or flatten, at every setting),
#                     ag-target-moves / ag-strength-moves / ag-maxlift-moves
#                     (each slider's every step moves the frame mean),
#                     ag-stability-static / ag-stability-pan (a still frame
#                     is exactly constant; a --periodic pan does not pulse),
#                     ag-exclusive (turning either adaptive effect on turns
#                     the other off). See the block at the bottom of this
#                     script for what each one is measuring and why.
#   bloom-*          2026-09-08, BLOOM: the control on a flat field, then the
#                     three knobs each pinned to a DIFFERENT statement about
#                     the line profile out from a bright box on a dark field
#                     (Radius -> reach, Intensity -> brightness, Threshold ->
#                     less glows), the no-clip property at the default and at
#                     the extremes, that nothing away from a source moves,
#                     and the shimmer question as a number. See the block at
#                     the bottom of this script.
#   bmap-*           2026-09-09, BRIGHTNESS MAP (EXPERIMENTAL): the control on
#                     the four-object scene, strength-0 identity, THE HEADLINE
#                     CHECK (a dark object on a bright field must lift, at four
#                     object sizes), the same for the inverse, the halo out
#                     from a hard edge across the strength and radius range,
#                     and the shimmer question on a panning scene. See the
#                     block at the bottom of this script.
#                     2026-09-10: the Radius sweep runs to 2.0, the halo and
#                     the two stability checks are repeated at BOTH ends of
#                     it, and bmap-radius-0.25-unchanged /
#                     bmap-radius-1.0-unchanged pin an existing setting's
#                     picture against what the build before the widening
#                     measured -- the promise made to every saved profile.
#   colors-*         2026-09-08, the Saturation/Vibrancy split: Saturation
#                     (renamed from "Vibrancy") and the new Vibrancy each
#                     pinned against their closed-form formula on a scene
#                     with actual colour (every scene above is greyscale,
#                     where both are a no-op); colors-shape is the headline
#                     check that the two have different SHAPES, not just
#                     different names -- see effects-regression.sh's own
#                     comment above that block and shader-effects.md.
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
AB_LOCAL_ID="image.shaders.adaptive_brightness.local_strength"  # Param, 0.0..1.0 (Local adaptation)
AB_TARGET_ID="image.shaders.adaptive_brightness.target"          # Param, 0.1..0.9 (Target brightness)
AB_MAXGAIN_ID="image.shaders.adaptive_brightness.max_gain"       # Param, 1.0..4.0 (Max gain)
AB_TARGET_DEFAULT=0.5   # == ConfigSchema.h's target_luminance
AB_MAXGAIN_DEFAULT=4.0  # == ConfigSchema.h's max_gain
AB_LOCAL_DEFAULT=0.5   # == ConfigSchema.h's ReshadeAdaptiveBrightnessSettings::local_strength

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
# 0.5, 1 s / 1 s, gains 0.3..4.0 -- widened 2026-09-07 from 0.5..2.0, strength
# 1.0) -- the numbers the doc quotes.
write_config() {
	mkdir -p "$CONFIGHOME/gamescope-ritz/profiles"
	cat > "$CONFIGHOME/gamescope-ritz/global.json" <<-EOF
		{ "schema_version": 4, "profiles": { "last_general": "Effects", "games": {} } }
	EOF
	# "saturation" (renamed from "vibrancy" 2026-09-08) has protect_skin_tones
	# forced off: the "colors" scene's checks compare captures against
	# grade()'s closed-form formula, which is only exact when the skin-tone
	# damper (a separate, approximate mask) is out of the picture.
	cat > "$CONFIGHOME/gamescope-ritz/profiles/Effects.json" <<-EOF
		{
		    "schema_version": 4,
		    "name": "Effects",
		    "kind": "general",
		    "fps_display": { "enabled": false },
		    "crosshair": { "enabled": false },
		    "reshade": {
		        "saturation": { "enabled": false, "protect_skin_tones": false },
		        "vibrancy": { "enabled": false },
		        "pre_sharpen": { "enabled": false },
		        "shadow_lift": { "enabled": false },
		        "bloom": { "enabled": false },
		        "brightness_map": { "enabled": false },
		        "adaptive_brightness": { "enabled": false, "mode": "whole_image", "strength": 1.0,
		                                 "local_strength": 0.0 }
		    }
		}
	EOF
}

GS_LOG=""
GS_WL_NAME=""
CLIENT_LOG=""
# The client's scene ring, and the ONE reason it is a variable. Several checks
# below navigate by "advance N to wrap back round to `dark`", so the ring's
# LENGTH is load-bearing arithmetic scattered through this script -- appending
# a scene to it silently walks every one of those to the wrong picture, and
# the checks then measure a scene they were never written for and fail in ways
# that look like the effect broke (measured, 2026-09-09: 28 pre-existing
# checks went red the first time two scenes were appended here). So a block
# that wants new scenes RESTARTS the instance with its own ring instead of
# extending this one -- see the Brightness Map block at the bottom.
SCENES_DEFAULT="dark,bright,mid,texdark,halfsplit,halobox,haloinv,colors"
SCENES="$SCENES_DEFAULT"
start_instance() {
	teardown_instance
	GS_LOG="$RUNDIR/gamescope.log"
	CLIENT_LOG="$OUT_DIR/client.log"
	: > "$CLIENT_LOG"
	rm -f "$PIDFILE"
	log "starting gamescope + effects_scene_client ($SCENES)"
	# texdark: 2 % lights so the 98th percentile sits in the histogram's gap,
	# --periodic so the 3 px/frame pan changes nothing but where the taps
	# land (the stability checks). --motion only moves the textured scene.
	WAYLAND_DISPLAY="$SWAY_WL_NAME" XDG_RUNTIME_DIR="$RUNDIR" XDG_CONFIG_HOME="$CONFIGHOME" \
		"$GAMESCOPE_BIN" --backend wayland -w "$OUT_W" -h "$OUT_H" -W "$OUT_W" -H "$OUT_H" \
		--force-windows-fullscreen -- \
		sh -c "SDL_VIDEODRIVER=x11 exec '$CLIENT_BIN' --scenes $SCENES --motion 3 --lights 2.0 --periodic --width $OUT_W --height $OUT_H --seconds 900 --pidfile '$PIDFILE' > '$CLIENT_LOG' 2>&1" \
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

set_local() {   # Local adaptation strength, 0.0 .. 1.0
	gsctl overlay_e2_set "$AB_LOCAL_ID $1" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
}

set_target() {   # Target brightness, 0.1 .. 0.9
	gsctl overlay_e2_set "$AB_TARGET_ID $1" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
}

set_maxgain() {   # Max gain, 1.0 .. 4.0
	gsctl overlay_e2_set "$AB_MAXGAIN_ID $1" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
}

next_scene() {
	# Only ever the pid the client itself wrote -- never a name match.
	kill -USR1 "$CLIENT_PID"
}
toggle_motion() { kill -USR2 "$CLIENT_PID"; }

# Per-frame statistics from gamescope's own `effects_ab_log` readback
# (rendervulkan.cpp): arm N composites with the probe on the middle band,
# wait for them to land in the gamescope log, and cut the last N lines into
# a file the sampler's `ablog` subcommand reads.
AB_LOG_TIMEOUT_S=20
AB_LOG_SEEN=0
ab_log_count() { grep -c 'ab_log n=' "$GS_LOG" 2>/dev/null || true; }
arm_ab_log() {   # <count>; probe (1200, 360): the middle band, clear of the rectangles
	AB_LOG_SEEN="$(ab_log_count)"; AB_LOG_SEEN="${AB_LOG_SEEN:-0}"
	gsctl effects_ab_log "$1 1200 360" >/dev/null 2>&1 || true
}
wait_ab_log() {   # <count> <out-file>: the <count> lines the last arm_ab_log produced
	local n="$1" out="$2" waited=0 seen
	while (( waited < AB_LOG_TIMEOUT_S * 10 )); do
		seen="$(ab_log_count)"
		[[ "${seen:-0}" -ge $(( AB_LOG_SEEN + n )) ]] && break
		sleep 0.1; waited=$((waited + 1))
	done
	grep 'ab_log n=' "$GS_LOG" | tail -n "+$(( AB_LOG_SEEN + 1 ))" | head -n "$n" > "$out"
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
AB_FRAMES=300
arm_ab_log "$AB_FRAMES"    # per-frame trace across the switch (transition-gain)
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
wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-02-transition.txt"
run_sampler ablog "$OUT_DIR/ablog-02-transition.txt" transition

# Scene 2: bright, settled.
capture_scene bright 03

# Scene 3: mid.
next_scene
sleep "$ADAPT_SETTLE_S"
capture_scene mid 04

# Scene 4: texdark, panning (periodic) -- sampling noise only -- then still.
# Dynamic is still on from capture_scene.
next_scene
sleep "$ADAPT_SETTLE_S"
arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-05-pan.txt"
run_sampler ablog "$OUT_DIR/ablog-05-pan.txt" pan
take_screenshot 05-texdark-pan-dynamic >/dev/null
toggle_motion
sleep "$ADAPT_SETTLE_S"
arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-06-static.txt"
run_sampler ablog "$OUT_DIR/ablog-06-static.txt" static
take_screenshot 06-texdark-still-dynamic >/dev/null

# ---------------------------------------------------------------------------
# THE TWO SLIDERS ACTUALLY MOVE THE PICTURE (2026-09-08). The report was
# *"anything above target brightness 0.5 and max gain 2.0 [doesn't] do
# anything at all"*, and it was true: Target reached the picture only through
# an exponent clamped to a fixed [0.5, 1.5], and Max gain only through a
# white-point demand that a realistic frame satisfies at about 2. Both are
# pinned here, on texdark -- the scene with a CONTINUOUS histogram, chosen
# deliberately over the flat band charts, which hide the Max gain half of the
# bug (their p98 is so low that every setting up to 4.0 bites).
#
# Measured before the fix, frame mean at max_gain 4: target 0.3 / 0.5 / 0.7
# read 84.9 / 121.0 / 121.0 -- the last step was EXACTLY zero. And at target
# 0.5, max_gain 1.5 / 2 / 3 read 89.1 / 102.4 / 121.0 with 3 -> 4 exactly
# zero. After: 84.9 / 129.8 / 175.4 and 64.9 / 102.4 / 129.8.
# ---------------------------------------------------------------------------
set_local 0
set_maxgain "$AB_MAXGAIN_DEFAULT"
declare -a TARGET_SHOTS=()
for T in 0.3 0.5 0.7; do
	set_target "$T"
	TARGET_SHOTS+=( "$(take_screenshot "06b-texdark-target-$T")" )
done
run_sampler slider target-moves 15 "${TARGET_SHOTS[@]}"
set_target "$AB_TARGET_DEFAULT"

declare -a MAXGAIN_SHOTS=()
for G in 1.5 2.0 3.0; do
	set_maxgain "$G"
	MAXGAIN_SHOTS+=( "$(take_screenshot "06c-texdark-maxgain-$G")" )
done
run_sampler slider maxgain-moves 15 "${MAXGAIN_SHOTS[@]}"
set_maxgain "$AB_MAXGAIN_DEFAULT"
set_local "$AB_LOCAL_DEFAULT"

# ---------------------------------------------------------------------------
# Scenes 5-7: Local adaptation (2026-09-07). The measure pass writes a 16x16
# map of smoothed local means beside the four global statistics, and the
# apply pass fits each pixel's curve to its own neighbourhood. Three things
# have to be true and none of them can be argued -- they have to be measured:
#   * a still frame is still constant, at every local strength (the pulse
#     fix's standard, re-run three times below on texdark);
#   * the half-dark / half-bright frame becomes serviceable in BOTH halves;
#   * a hard edge does not grow a ring.
# ---------------------------------------------------------------------------

# Still stability at three local strengths, on the scene already held still.
for L in 0.5 1.0; do
	set_local "$L"
	arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-06-static-local$L.txt"
	run_sampler ablog "$OUT_DIR/ablog-06-static-local$L.txt" static
done
# ... and the same scene panning again, reported not asserted (see the
# sampler's `panlocal`: a pan genuinely changes each cell's own content).
toggle_motion
sleep "$ADAPT_SETTLE_S"
for L in 0 0.5 1.0; do
	set_local "$L"
	arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-06-pan-local$L.txt"
	run_sampler ablog "$OUT_DIR/ablog-06-pan-local$L.txt" panlocal
done
toggle_motion

# Scene 5: halfsplit -- the headline case.
next_scene
sleep "$ADAPT_SETTLE_S"
set_ab 0; set_local 0
S_OFF="$(take_screenshot 07-halfsplit-off)"
run_sampler split "$S_OFF" off
set_ab 2
set_local 0;                  S_L0="$(take_screenshot 07-halfsplit-local-0)"
set_local "$AB_LOCAL_DEFAULT"; S_LD="$(take_screenshot 07-halfsplit-local-default)"
set_local 1.0;                S_L1="$(take_screenshot 07-halfsplit-local-100)"
run_sampler split "$S_L0" "local-0"
run_sampler split "$S_LD" "local-$AB_LOCAL_DEFAULT"
run_sampler split "$S_L1" "local-100"
run_sampler splitcmp "$S_L0" "$S_L1"
run_sampler splitcmp "$S_L0" "$S_LD"
arm_ab_log 150; wait_ab_log 150 "$OUT_DIR/ablog-07-halfsplit.txt"
run_sampler ablog "$OUT_DIR/ablog-07-halfsplit.txt" panlocal

# Scenes 6 and 7: the halo pair. Dynamic stays on from above.
for pair in "08 halobox" "09 haloinv"; do
	set -- $pair
	idx="$1"; scene="$2"
	next_scene
	sleep "$ADAPT_SETTLE_S"
	set_local 0;   SH0="$(take_screenshot "${idx}-${scene}-local-0")"
	set_local "$AB_LOCAL_DEFAULT"; SHD="$(take_screenshot "${idx}-${scene}-local-default")"
	set_local 1.0; SH1="$(take_screenshot "${idx}-${scene}-local-100")"
	run_sampler halo "$SH0" "$scene" off
	run_sampler halo "$SHD" "$scene" on 12    # the 50 % default: measured 8 counts
	run_sampler halo "$SH1" "$scene" on 18    # 100 %:            measured 15 counts
done
set_local "$AB_LOCAL_DEFAULT"

# ---------------------------------------------------------------------------
# Scene 8: colors -- the Saturation/Vibrancy split (2026-09-08). Every scene
# above is greyscale, where both colour effects are an exact no-op, so this
# is the only place either is actually exercised. Adaptive Brightness is
# switched off for all of it: these checks compare a capture against
# grade()'s Saturation/Vibrancy formula alone, and AB's gain/gamma running
# on top would confound that.
#   colors-off               identity (the capture path itself is honest)
#   colors-saturation-<S>    Saturation at strength S matches the formula
#                             exactly (protect_skin_tones is off in
#                             write_config(), see its comment) -- pins that
#                             the 2026-09-08 rename left the maths untouched
#   colors-vibrancy-<S>      Vibrancy (NEW) at strength S matches its own
#                             formula; the grey band (saturation 0) is
#                             untouched at every strength, both effects
#   colors-shape              THE HEADLINE CHECK: Saturation's per-band
#                             boost ratio (captured saturation / input
#                             saturation) is FLAT across bands -- the same
#                             relative boost for every pixel, exactly what
#                             an iPhone "Saturation" slider does -- while
#                             Vibrancy's ratio strictly INCREASES with the
#                             pixel's own existing saturation. Numeric proof
#                             the two effects are shaped differently, not
#                             just differently named.
# ---------------------------------------------------------------------------
SAT_ID="image.shaders.saturation"
SAT_STRENGTH_ID="image.shaders.saturation.strength"
VIB_ID="image.shaders.vibrancy"
VIB_STRENGTH_ID="image.shaders.vibrancy.strength"

set_saturation() {   # 0 off, or a strength 0.0..3.0
	if [[ "$1" == "0" ]]; then
		gsctl overlay_e2_set "$SAT_ID 0" >/dev/null 2>&1 || true
	else
		gsctl overlay_e2_set "$SAT_STRENGTH_ID $1" >/dev/null 2>&1 || true
		gsctl overlay_e2_set "$SAT_ID 1" >/dev/null 2>&1 || true
	fi
	sleep "$SETTLE_S"
}
set_vibrancy() {   # 0 off, or a strength 0.0..2.0
	if [[ "$1" == "0" ]]; then
		gsctl overlay_e2_set "$VIB_ID 0" >/dev/null 2>&1 || true
	else
		gsctl overlay_e2_set "$VIB_STRENGTH_ID $1" >/dev/null 2>&1 || true
		gsctl overlay_e2_set "$VIB_ID 1" >/dev/null 2>&1 || true
	fi
	sleep "$SETTLE_S"
}

set_ab 0
next_scene   # haloinv -> colors (last scene in --scenes)
sleep "$ADAPT_SETTLE_S"

set_saturation 0; set_vibrancy 0
run_sampler colorcheck "$(take_screenshot 10-colors-off)" off 0

for S in 0.0 0.5 1.0 2.0; do
	set_saturation "$S"
	SHOT="$(take_screenshot "10-colors-saturation-$S")"
	run_sampler colorcheck "$SHOT" saturation "$S"
	# 0.5, not 2.0: colorshape wants Saturation's PURE "m" regime (strength
	# <= 1.0, no adaptive "boost" term -- see cmd_colorshape's own comment)
	# so its ratio is genuinely flat, not just muted-first-shaped.
	[[ "$S" == "0.5" ]] && SHOT_SAT_SHAPE="$SHOT"
done
set_saturation 0

for S in 0.5 1.0 2.0; do
	set_vibrancy "$S"
	SHOT="$(take_screenshot "10-colors-vibrancy-$S")"
	run_sampler colorcheck "$SHOT" vibrancy "$S"
	[[ "$S" == "1.0" ]] && SHOT_VIB_SHAPE="$SHOT"
done
set_vibrancy 0

run_sampler colorshape "$SHOT_SAT_SHAPE" "$SHOT_VIB_SHAPE"

# ---------------------------------------------------------------------------
# ADAPTIVE GAMMA (2026-09-08). "Make something similar, but make it gamma
# based." Same statistics, ONE exponent: no levels gain, no shoulder. Four
# things have to be measured, not argued:
#
#   ag-noclip-*        the property the shape promises and Adaptive
#                      Brightness's cannot -- a pure exponent maps [0,1] onto
#                      [0,1] with 0 and 1 as exact fixed points, so nothing
#                      clips and nothing flattens, AT EVERY SETTING. Run over
#                      a matrix of Target / Strength / Max lift / Max darken
#                      on the dark scene (whose 240 highlights Whole-image
#                      mode drives to a clipped 255) and on the bright scene
#                      (whose top band already IS 255).
#   ag-target-moves    Target, swept on the textured dark scene -- the scene
#   ag-strength-moves  with a CONTINUOUS histogram, chosen for the same
#   ag-maxlift-moves   reason the 2026-09-08 Adaptive Brightness sweeps use
#                      it: a flat band chart flatters this kind of operator.
#                      Every step must move the frame mean.
#   ag-stability-*     a still frame must be EXACTLY constant and a
#                      --periodic pan must not pulse, to the standard the
#                      2026-09-07 fix set -- watching the exponent, which is
#                      this effect's entire state.
#   ag-exclusive       the two adaptive effects are mutually exclusive:
#                      turning either on turns the other off, whichever way
#                      round, so the double correction is unreachable.
#
# Every INFO `bind-*` line names which limit the frame's own classifier says
# is binding, in the panel's exact words -- that is how "where does this
# slider stop working" is answered by the gate rather than by a user.
# ---------------------------------------------------------------------------
AG_ID="image.shaders.adaptive_gamma"
AG_TARGET_ID="image.shaders.adaptive_gamma.target"
AG_STRENGTH_ID="image.shaders.adaptive_gamma.strength"
AG_LIFT_ID="image.shaders.adaptive_gamma.max_lift"
AG_DARKEN_ID="image.shaders.adaptive_gamma.max_darken"
AG_LOCAL_ID="image.shaders.adaptive_gamma.local_strength"
# This row's own adaptation speeds (2026-09-09). Before them the measure
# pass's EMA always ran on adaptive_brightness's pair -- sliders that are
# unreachable while Adaptive Gamma is the effect that is on, because the two
# are mutually exclusive.
AG_UP_ID="image.shaders.adaptive_gamma.up_speed"
AG_DOWN_ID="image.shaders.adaptive_gamma.down_speed"
AG_TARGET_DEFAULT=0.5   # == ConfigSchema.h's ReshadeAdaptiveGammaSettings
AG_LIFT_DEFAULT=4.0
AG_DARKEN_DEFAULT=1.5
AG_SPEED_DEFAULT=1.0   # == adaptive_brightness's, deliberately

set_ag() { gsctl overlay_e2_set "$AG_ID $1" >/dev/null 2>&1 || true; sleep "$SETTLE_S"; }
set_ag_param() { gsctl overlay_e2_set "$1 $2" >/dev/null 2>&1 || true; sleep "$SETTLE_S"; }
# The live value of one row or param, as the registry itself reports it
# (`overlay_e2_get` prints "<id> <kind> <value>"; a bool is "on"/"off").
# Every stage is guarded: under `set -euo pipefail` an unmatched grep inside
# a command substitution would abort the whole script rather than yield an
# empty string, and an empty string is what the caller wants to see reported.
get_e2() {
	local out
	out="$( gsctl overlay_e2_get "$1" 2>&1 || true )"
	printf '%s\n' "$out" | grep -F "$1" | tail -1 | awk '{ print $3 }' || true
}
ag_defaults() {
	set_ag_param "$AG_TARGET_ID" "$AG_TARGET_DEFAULT"
	set_ag_param "$AG_STRENGTH_ID" 1.0
	set_ag_param "$AG_LIFT_ID" "$AG_LIFT_DEFAULT"
	set_ag_param "$AG_DARKEN_ID" "$AG_DARKEN_DEFAULT"
	set_ag_param "$AG_LOCAL_ID" 0.0
	set_ag_param "$AG_UP_ID" "$AG_SPEED_DEFAULT"
	set_ag_param "$AG_DOWN_ID" "$AG_SPEED_DEFAULT"
}
set_ag_speed() { set_ag_param "$AG_UP_ID" "$1"; set_ag_param "$AG_DOWN_ID" "$2"; }

set_saturation 0; set_vibrancy 0; set_ab 0
ag_defaults
set_ag 1

# Scene 9: back round to `dark` (SIGUSR1 wraps).
next_scene
sleep "$ADAPT_SETTLE_S"
run_sampler noclip "$(take_screenshot 11-dark-ag-default)" dark ag-default
# The no-clip property over a matrix of settings, not just the defaults:
# these are the four corners of what the row can be set to.
for spec in "target 0.3:$AG_TARGET_ID:0.3" "target 0.9:$AG_TARGET_ID:0.9" \
            "strength 0.5:$AG_STRENGTH_ID:0.5" "maxlift 1.5:$AG_LIFT_ID:1.5" \
            "maxdarken 4.0:$AG_DARKEN_ID:4.0" "local 1.0:$AG_LOCAL_ID:1.0"; do
	LBL="${spec%%:*}"; REST="${spec#*:}"; PID="${REST%%:*}"; PVAL="${REST#*:}"
	set_ag_param "$PID" "$PVAL"
	run_sampler noclip "$(take_screenshot "11-dark-ag-${LBL// /}")" dark "ag-${LBL// /-}"
	ag_defaults
done
# Max lift is the binding limit on this flat chart at EVERY setting (its
# median is 0.047, below every floor in range), so every step of it must
# move the picture -- and the readout must say Max lift is what binds.
declare -a AG_LIFT_SHOTS=()
for L in 1.5 2.0 3.0 4.0; do
	set_ag_param "$AG_LIFT_ID" "$L"
	AG_LIFT_SHOTS+=( "$(take_screenshot "11-dark-ag-maxlift-$L")" )
done
run_sampler slider ag-maxlift-moves 8 "${AG_LIFT_SHOTS[@]}"
arm_ab_log 30; wait_ab_log 30 "$OUT_DIR/ablog-11-dark-ag.txt"
run_sampler ablog "$OUT_DIR/ablog-11-dark-ag.txt" bind
ag_defaults

# Scene 10: bright -- the darkening side, and the harder no-clip case (its
# top band is already 255, so "did anything below white get flattened into
# it" is the whole question).
next_scene
sleep "$ADAPT_SETTLE_S"
run_sampler noclip "$(take_screenshot 12-bright-ag-default)" bright ag-default
# On THIS scene (median 0.898) the exponent the target asks for is 6.4 at
# the default target and 11.2 at 0.3 -- far above Max darken -- so Target is
# genuinely inert here and Max darken is the control that moves the picture.
# That is measured as such: Max darken swept DOWNWARD (a negative step), and
# the INFO bind line below has to be naming the darkening limit. An inert
# Target that the readout NAMES is the design; an inert Target that says
# nothing is the 2026-09-08 bug.
declare -a AG_DARKEN_SHOTS=()
for D in 1.0 1.5 2.0 3.0; do
	set_ag_param "$AG_DARKEN_ID" "$D"
	AG_DARKEN_SHOTS+=( "$(take_screenshot "12-bright-ag-maxdarken-$D")" )
done
run_sampler slider ag-maxdarken-moves -8 "${AG_DARKEN_SHOTS[@]}"
run_sampler noclip "${AG_DARKEN_SHOTS[3]}" bright ag-maxdarken-3.0
ag_defaults
arm_ab_log 30; wait_ab_log 30 "$OUT_DIR/ablog-12-bright-ag.txt"
run_sampler ablog "$OUT_DIR/ablog-12-bright-ag.txt" bind

# Scene 11: mid -- a 0.1..0.9 scene is very nearly the identity.
next_scene
sleep "$ADAPT_SETTLE_S"
run_sampler noclip "$(take_screenshot 13-mid-ag-default)" mid ag-default
run_sampler regions "$(take_screenshot 13-mid-ag-default)" mid

# Scene 12: texdark -- the continuous histogram. The two sweeps that pin
# "Target moves the picture" and "Strength moves the picture", then
# stability (the scene is panning; toggle_motion holds it still).
next_scene
sleep "$ADAPT_SETTLE_S"
# Target, over the range it is FREE on this frame. Its median is 0.076, so
# with Max lift at the top of its slider (exponent floor 0.25) the highest
# target the exponent can still reach is 0.076^0.25 = 0.52 -- above that the
# floor holds it and Target stops moving the picture. That is the residual
# ceiling of an exponent-only operator and it is stated, not hidden: the
# sweep below covers 0.2 .. 0.5, and the capture after it at 0.7 must have
# the readout naming Max lift.
declare -a AG_TARGET_SHOTS=()
for T in 0.2 0.35 0.5; do
	set_ag_param "$AG_TARGET_ID" "$T"
	AG_TARGET_SHOTS+=( "$(take_screenshot "14-texdark-ag-target-$T")" )
done
run_sampler slider ag-target-moves 15 "${AG_TARGET_SHOTS[@]}"
set_ag_param "$AG_TARGET_ID" 0.7
take_screenshot 14-texdark-ag-target-0.7 >/dev/null
arm_ab_log 30; wait_ab_log 30 "$OUT_DIR/ablog-14-ag-target0.7.txt"
run_sampler ablog "$OUT_DIR/ablog-14-ag-target0.7.txt" bind
set_ag_param "$AG_TARGET_ID" "$AG_TARGET_DEFAULT"

declare -a AG_STRENGTH_SHOTS=()
for S in 0.0 0.5 1.0; do
	set_ag_param "$AG_STRENGTH_ID" "$S"
	AG_STRENGTH_SHOTS+=( "$(take_screenshot "14-texdark-ag-strength-$S")" )
done
run_sampler slider ag-strength-moves 8 "${AG_STRENGTH_SHOTS[@]}"
set_ag_param "$AG_STRENGTH_ID" 1.0

# STILL FIRST, then panning. The Local-adaptation section above left the
# client's motion PAUSED (it toggles three times), so the still measurement
# is the one that needs no toggle -- getting this the wrong way round
# measures a pan and calls it a still frame, which is exactly the mistake
# this comment exists to stop the next author repeating.
for L in 0.0 1.0; do
	set_ag_param "$AG_LOCAL_ID" "$L"
	# LET THE EMA FINISH BEFORE ASKING IT TO HOLD STILL. This check demands
	# a peak-to-peak of exactly ZERO on the output pixel over 300 frames,
	# which is only meaningful once the history has arrived; the sweeps
	# above leave it a fraction of a percent short, and one run in two that
	# tail was still worth a single code of output during the measurement
	# (measured 2026-09-09: px p2p 0 on one run, 1 on the next, from a
	# gamma p2p of 6e-5). The bar is right -- a settled still frame really
	# must not move at all -- so the fix is to wait, not to loosen it.
	sleep "$ADAPT_SETTLE_S"
	arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-14-ag-static-local$L.txt"
	run_sampler ablog "$OUT_DIR/ablog-14-ag-static-local$L.txt" agstatic
done
set_ag_param "$AG_LOCAL_ID" 0.0
toggle_motion   # -> panning, --periodic: the true statistics do not change
sleep "$ADAPT_SETTLE_S"
arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-14-ag-pan.txt"
run_sampler ablog "$OUT_DIR/ablog-14-ag-pan.txt" agpan
toggle_motion   # leave the client as it was found

# THE EXCLUSION, both ways round. overlay_e2_set writes through the binding
# -- the same setter the switch uses -- so this exercises exactly what a
# click does, and the greying the panel does on top is not what enforces it.
set_ag 1
X_AG="$(take_screenshot 15-exclusive-1-gamma)"
set_ab 2
AG_AFTER_AB="$(get_e2 "$AG_ID")"
X_AB="$(take_screenshot 15-exclusive-2-brightness)"
set_ag 1
AB_AFTER_AG="$(get_e2 "$AB_ID")"
X_AG2="$(take_screenshot 15-exclusive-3-gamma-again)"
# What the pictures say, beside what the config says: the frame is one
# effect's or the other's and never a compounding of both, and switching
# back returns the first one's picture.
run_sampler means ag-exclusive-pictures "$X_AG" "$X_AB" "$X_AG2"
record_line "INFO	ag-exclusive-raw	overlay_e2_get after each flip: adaptive_gamma='$AG_AFTER_AB' adaptive_brightness='$AB_AFTER_AG'"
if [[ "$AG_AFTER_AB" == "off" && "$AB_AFTER_AG" == "off" ]]; then
	record_line "PASS	ag-exclusive	turning Adaptive Brightness on left Adaptive Gamma '$AG_AFTER_AB'; turning Adaptive Gamma on left Adaptive Brightness '$AB_AFTER_AG'"
else
	record_line "FAIL	ag-exclusive	expected both 'off'; got adaptive_gamma='$AG_AFTER_AB' adaptive_brightness='$AB_AFTER_AG'"
fi
set_ag 0

# ---------------------------------------------------------------------------
# BLOOM (2026-09-08). "Add a bloom shader for more casual games." A glow
# around bright areas: a bright pass at 1/8 resolution, a separable Gaussian
# over it, and a SCREEN composite back onto the picture. Five things have to
# be measured rather than argued:
#
#   bloom-off-flat     the control. haloinv's flat 15 field with the effect
#                      off must come out flat, so every profile below is a
#                      statement about the glow and not about the capture.
#   bloom-radius       the glow's spatial EXTENT grows with Radius -- measured
#                      as the distance at which the line profile out from the
#                      bright box falls back into the field, not as "the
#                      picture changed".
#   bloom-intensity    its BRIGHTNESS beside the source grows with Intensity.
#   bloom-threshold    and raising Threshold SHRINKS what glows, so the same
#                      amplitude comes back down. Three different statements,
#                      because the three knobs do three different things.
#   bloom-unchanged-*  nothing away from a bright source moves: the dark
#                      scene's bands are sampled 80 px from the nearest
#                      highlight, five sigma at the default Radius.
#   bloom-noclip-*     the no-clip property: the bands stay strictly ordered
#                      and nothing whose input was below white comes out ON
#                      white, at the default AND at Intensity 2.0, the top of
#                      the slider. Adding light is the one operation here
#                      that naturally blows highlights out; the composite's
#                      shape is what stops it. (`noclip-*-bloom-default` runs
#                      Adaptive Gamma's stricter check at the shipped
#                      defaults on top of it; the pair at threshold 0 +
#                      Intensity 2.0 is REPORTED, not asserted -- see the
#                      block at the bottom of this script.)
#   bloom-stability-*  a still frame must produce a bit-identical output
#                      pixel, and turning Bloom on must not widen the frame
#                      mean's frame-to-frame spread on a PANNING --periodic
#                      scene -- the shimmer question a thresholded effect
#                      always has to answer.
# ---------------------------------------------------------------------------
BLOOM_ID="image.shaders.bloom"
BLOOM_THRESHOLD_ID="image.shaders.bloom.threshold"
BLOOM_INTENSITY_ID="image.shaders.bloom.intensity"
BLOOM_RADIUS_ID="image.shaders.bloom.radius"
BLOOM_THRESHOLD_DEFAULT=0.75   # == ConfigSchema.h's ReshadeBloomSettings
BLOOM_INTENSITY_DEFAULT=0.8
BLOOM_RADIUS_DEFAULT=0.5

set_bloom() { gsctl overlay_e2_set "$BLOOM_ID $1" >/dev/null 2>&1 || true; sleep "$SETTLE_S"; }
set_bloom_param() { gsctl overlay_e2_set "$1 $2" >/dev/null 2>&1 || true; sleep "$SETTLE_S"; }
bloom_defaults() {
	set_bloom_param "$BLOOM_THRESHOLD_ID" "$BLOOM_THRESHOLD_DEFAULT"
	set_bloom_param "$BLOOM_INTENSITY_ID" "$BLOOM_INTENSITY_DEFAULT"
	set_bloom_param "$BLOOM_RADIUS_ID" "$BLOOM_RADIUS_DEFAULT"
}

# Still first, then panning -- the same polarity trap the Adaptive Gamma
# block above documents: the section before this one leaves the client's
# motion PAUSED, so the still measurement needs no toggle.
bloom_defaults
set_bloom 1
arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-16-bloom-static.txt"
run_sampler ablog "$OUT_DIR/ablog-16-bloom-static.txt" bloomstatic

# The shimmer question: the same panning --periodic frame, six captures with
# the effect off and six with it on. --periodic fixes the frame's light
# population, so the frame mean's spread across captures is the noise, and
# turning Bloom on must not add to it.
toggle_motion
sleep 1
declare -a BLOOM_JIT=()
set_bloom 0
for k in 1 2 3 4 5 6; do BLOOM_JIT+=( "$(take_screenshot "16-texdark-bloom-off-$k")" ); done
set_bloom 1
for k in 1 2 3 4 5 6; do BLOOM_JIT+=( "$(take_screenshot "16-texdark-bloom-on-$k")" ); done
run_sampler bloomjitter bloom-stability-pan 6 "${BLOOM_JIT[@]}"
toggle_motion   # leave the client as it was found

# Scenes 13-15: texdark -> halfsplit -> halobox -> haloinv, the bright box on
# a dark field the line profiles are measured on.
#
# ONE SIGUSR1 PER FRAME, NOT THREE IN A ROW. effects_scene_client.c advances
# by exactly ONE scene per repaint when it notices its counter moved -- it
# does not consume a backlog -- and SIGUSR1 is a plain (non-realtime) signal,
# so several delivered inside one 16 ms frame collapse into a single advance.
# Every existing next_scene in this script happens to be followed by a sleep;
# the bloom section was the first to want three in a row, got one, and
# measured halfsplit's flat right half while calling it haloinv. If you add a
# multi-scene skip, keep the sleeps.
advance_scenes() { for _ in $(seq 1 "$1"); do next_scene; sleep 0.5; done; }
advance_scenes 3
sleep "$ADAPT_SETTLE_S"

set_bloom 0
run_sampler bloomflat "$(take_screenshot 17-haloinv-bloom-off)"
set_bloom 1

declare -a BLOOM_RADIUS_SHOTS=()
for R in 0.0 0.5 1.0; do
	set_bloom_param "$BLOOM_RADIUS_ID" "$R"
	BLOOM_RADIUS_SHOTS+=( "$(take_screenshot "17-haloinv-bloom-radius-$R")" )
done
run_sampler bloomline bloom-radius extent "${BLOOM_RADIUS_SHOTS[@]}"
bloom_defaults

declare -a BLOOM_INTENSITY_SHOTS=()
for I in 0.4 0.8 1.6; do
	set_bloom_param "$BLOOM_INTENSITY_ID" "$I"
	BLOOM_INTENSITY_SHOTS+=( "$(take_screenshot "17-haloinv-bloom-intensity-$I")" )
done
run_sampler bloomline bloom-intensity amp-up "${BLOOM_INTENSITY_SHOTS[@]}"
bloom_defaults

# Threshold swept UPWARD, so the glow must come DOWN. haloinv's box is 220
# (0.863 encoded), so 0.85 is above almost all of its emission and 0.5 well
# below it -- the slider's whole useful span on this source.
declare -a BLOOM_THRESHOLD_SHOTS=()
for T in 0.5 0.7 0.85; do
	set_bloom_param "$BLOOM_THRESHOLD_ID" "$T"
	BLOOM_THRESHOLD_SHOTS+=( "$(take_screenshot "17-haloinv-bloom-threshold-$T")" )
done
run_sampler bloomline bloom-threshold amp-down "${BLOOM_THRESHOLD_SHOTS[@]}"
bloom_defaults

# Scenes 16-17: haloinv -> colors -> dark, then bright. The no-clip pair, and
# the "nothing away from a source moved" pair.
advance_scenes 2
sleep "$ADAPT_SETTLE_S"
set_bloom 0; B_DARK_OFF="$(take_screenshot 18-dark-bloom-off)"
set_bloom 1; B_DARK_ON="$(take_screenshot 18-dark-bloom-default)"
run_sampler bloomsame "$B_DARK_OFF" "$B_DARK_ON" dark
run_sampler bloomnoclip "$B_DARK_ON" dark default
run_sampler noclip "$B_DARK_ON" dark bloom-default
set_bloom_param "$BLOOM_INTENSITY_ID" 2.0
run_sampler bloomnoclip "$(take_screenshot 18-dark-bloom-intensity-2.0)" dark intensity-2.0
# THE WORST CASE, REPORTED RATHER THAN ASSERTED: threshold 0 makes EVERY
# pixel emit and Intensity 2.0 is the top of the slider, so this is the most
# light the effect can possibly add. On the dark scene the result is still
# comfortably inside the range; on the bright scene below it is not, and the
# numbers are printed either way rather than a bar being chosen that both
# happen to clear. See shader-effects.md's "what it costs" note.
set_bloom_param "$BLOOM_THRESHOLD_ID" 0.0
run_sampler regions "$(take_screenshot 18-dark-bloom-worstcase)" dark
record_line "INFO	bloom-worst-dark	(above: threshold 0.0 + Intensity 2.0, every pixel emitting)"
bloom_defaults

advance_scenes 1
sleep "$ADAPT_SETTLE_S"
B_BRIGHT_ON="$(take_screenshot 19-bright-bloom-default)"
run_sampler bloomnoclip "$B_BRIGHT_ON" bright default
run_sampler noclip "$B_BRIGHT_ON" bright bloom-default
set_bloom_param "$BLOOM_INTENSITY_ID" 2.0
run_sampler bloomnoclip "$(take_screenshot 19-bright-bloom-intensity-2.0)" bright intensity-2.0
set_bloom_param "$BLOOM_THRESHOLD_ID" 0.0
run_sampler regions "$(take_screenshot 19-bright-bloom-worstcase)" bright
record_line "INFO	bloom-worst-bright	(above: threshold 0.0 + Intensity 2.0 on the brightest scene -- the honest limit)"
bloom_defaults
set_bloom 0

# ---------------------------------------------------------------------------
# ADAPTIVE GAMMA'S ADAPTATION SPEED (2026-09-09). The user: "For adaptive
# gamma, there should also be some value, to adjust the speed of it."
#
# Until this row had its own pair, the measure pass's EMA ran unconditionally
# on adaptive_brightness's adapt_up_speed / adapt_down_speed -- so Adaptive
# Gamma's adaptation rate was set by an effect it is MUTUALLY EXCLUSIVE with,
# i.e. by two sliders a user cannot reach while this is the effect that is
# on. Three things are measured, because three different things could break:
#
#   ag-speed-brighten   THE POINT OF THE FEATURE. The same dark -> bright
#                       step at a fast, a default and a slow setting: the
#                       time for the exponent to come within 5 % of where it
#                       lands must grow with the slider. Reported against
#                       the closed form (3 tau) rather than only against
#                       each other, and each trace carries the tau the frame
#                       actually used (`ab_log`'s tau= field), so a slider
#                       that silently did not take shows up as a wrong tau
#                       instead of a mysterious time.
#   ag-speed-asymmetry  WHY THERE ARE TWO of them. With "adapt to brighter"
#                       fast and "adapt to darker" slow, a brightening step
#                       and a darkening step must settle at visibly
#                       different times -- the capability a single combined
#                       speed could not express.
#   ag-stability-*-fast STABILITY MUST NOT REGRESS AT THE FAST END. The
#                       2026-09-07 pulse fix's own bar (a still frame moves
#                       by nothing at all, a --periodic pan by noise), rerun
#                       at the FASTEST setting the slider reaches -- where
#                       an oscillation would appear first, because that is
#                       where the EMA hides the least.
# ---------------------------------------------------------------------------
set_bloom 0
set_ab 0
ag_defaults
set_ag 1

# 800 composites is ~13 s at 60 Hz: 3 tau at the slowest setting tested here
# is 9 s, so the trace outlives the transition it is timing. The log wait has
# to outlive the trace, hence the timeout bump.
AG_SETTLE_FRAMES=800
AB_LOG_TIMEOUT_S=60

# The client's scene ring is dark,bright,mid,texdark,halfsplit,halobox,
# haloinv,colors and the Bloom block above left it on `bright`, so 7 single
# advances wrap back round to `dark` -- one per frame, per advance_scenes'
# own warning about collapsing signals.
declare -a AG_SPEED_SPECS=()
for spec in "fast 0.3" "default 1.0" "slow 3.0"; do
	L="${spec%% *}"; T="${spec##* }"
	set_ag_speed "$T" "$T"
	advance_scenes 7          # -> dark
	# Settle INTO dark at this same tau before timing the step out of it:
	# an unsettled start would make the first frame's exponent, and so the
	# 5 %-of-span target, a different number for every speed.
	sleep "$(python3 -c "print(max(6.0, 7.0 * $T))")"
	arm_ab_log "$AG_SETTLE_FRAMES"
	next_scene                # -> bright: the statistics RISE, so `up` applies
	wait_ab_log "$AG_SETTLE_FRAMES" "$OUT_DIR/ablog-20-ag-speed-$L.txt"
	AG_SPEED_SPECS+=( "$L:$T:$OUT_DIR/ablog-20-ag-speed-$L.txt" )
done
run_sampler agsettle ag-speed-brighten 3.0 "${AG_SPEED_SPECS[@]}"

# The asymmetry, on the two directions of the same ring: dark -> bright
# rises (so `up` is the constant in play) and bright -> mid falls (`down`).
# Both are SINGLE hops, so each trace times one clean step.
set_ag_speed 0.3 3.0
advance_scenes 7             # -> dark
sleep 24                     # settling into dark is a FALL, i.e. at down=3.0
arm_ab_log "$AG_SETTLE_FRAMES"
next_scene                   # -> bright
wait_ab_log "$AG_SETTLE_FRAMES" "$OUT_DIR/ablog-20-ag-asym-up.txt"
arm_ab_log "$AG_SETTLE_FRAMES"
next_scene                   # -> mid
wait_ab_log "$AG_SETTLE_FRAMES" "$OUT_DIR/ablog-20-ag-asym-down.txt"
run_sampler agsettle ag-speed-asymmetry 3.0 \
	"brighten:0.3:$OUT_DIR/ablog-20-ag-asym-up.txt" \
	"darken:3.0:$OUT_DIR/ablog-20-ag-asym-down.txt"

# Stability at the FASTEST the slider goes. Still first, then panning --
# the same polarity trap the two blocks above document: the sections before
# this one leave the client's motion PAUSED.
set_ag_speed 0.1 0.1
advance_scenes 1             # -> texdark, the continuous-histogram scene
sleep "$ADAPT_SETTLE_S"      # the same "let the EMA arrive" wait as above
arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-20-ag-fast-static.txt"
run_sampler ablog "$OUT_DIR/ablog-20-ag-fast-static.txt" agstatic
toggle_motion
sleep "$ADAPT_SETTLE_S"
arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-20-ag-fast-pan.txt"
run_sampler ablog "$OUT_DIR/ablog-20-ag-fast-pan.txt" agpan
toggle_motion                # leave the client as it was found
take_screenshot 20-texdark-ag-fast >/dev/null   # the picture behind the numbers

ag_defaults
set_ag 0

# ---------------------------------------------------------------------------
# BRIGHTNESS MAP (2026-09-09, EXPERIMENTAL). The user: "When the world is
# rather bright and player models are rather dark, the player models turn
# almost black... Lets add an experimental mode, that creates a brightness map
# of the whole image and then adjusts based on that."
#
# THE HEADLINE CASE IS A SIZE QUESTION, so the scene is four objects at once.
# Adaptive Gamma's local adaptation fails this request not because it is
# switched off but because its map is blurred to sigma ~4.7 cells of a 16x16
# grid and CANNOT SEE a player; the only honest way to show this operator does
# better is to sweep the object size and say which sizes it reaches. Hence
# `models` (16 / 32 / 64 / 128 px dark boxes on a bright field) and its
# inverse, and hence a radius sweep reported as a table rather than as a
# threshold nobody could pick honestly in advance.
#
# IT RUNS ON ITS OWN INSTANCE, with its own scene ring. Appending the two new
# scenes to the shared ring instead broke 28 checks above it in one run: they
# navigate by "advance N to wrap round to dark", so the ring's length is
# arithmetic several blocks depend on. Restarting is a few seconds and it also
# means this block starts from a known scene rather than from wherever the
# previous one left the client.
#
#   bmap-models-off       the control: every box and the field read back what
#                         the client painted, so every number below is about
#                         the operator and not about the capture.
#   bmap-models-identity  Strength 0 with the switch ON is identical to the
#                         switch being off -- the request's "0 is the original
#                         image", measured on real captures rather than only
#                         asserted on the header text.
#   bmap-headline         THE DELIVERABLE. A Strength sweep on the dark-on-
#                         bright scene: every box lifts monotonically, the
#                         largest by a real number of counts, and the field
#                         does not overshoot the target.
#   bmap-headline-inv     the same for a bright object on a dark field, where
#                         the correction has to run the other way.
#   bmap-size-radius      INFO: the per-size table at Radius 0 / 0.5 / 1, i.e.
#                         WHICH OBJECT SIZES survive each setting of the halo
#                         control. That is the trade-off, as a table.
#   bmap-min/max-*        INFO: the same table across each guard rail, which is
#                         how "what do Min and Max brightness actually do"
#                         becomes numbers instead of a description.
#   bmap-halo-*           the artefact: no ring at any setting (the map is a
#                         non-negative blur of a step, so a turning point would
#                         mean something is wrong), and the amplitude reported
#                         at low / default / high strength and at both ends of
#                         Radius.
#   bmap-stability-*      a still frame must produce a bit-identical output
#                         pixel, and turning the effect on must not widen the
#                         frame mean's spread on a PANNING --periodic scene.
#                         A fine spatial filter is more prone to this than the
#                         coarse one, so it is measured rather than argued.
# ---------------------------------------------------------------------------
BMAP_ID="image.shaders.brightness_map"
BMAP_STRENGTH_ID="image.shaders.brightness_map.strength"
BMAP_RADIUS_ID="image.shaders.brightness_map.radius"
BMAP_TARGET_ID="image.shaders.brightness_map.target"
BMAP_MIN_ID="image.shaders.brightness_map.min_brightness"
BMAP_MAX_ID="image.shaders.brightness_map.max_brightness"
BMAP_STRENGTH_DEFAULT=0.5   # == ConfigSchema.h's ReshadeBrightnessMapSettings
BMAP_RADIUS_DEFAULT=0.25
BMAP_TARGET_DEFAULT=0.5
BMAP_MIN_DEFAULT=0.10
BMAP_MAX_DEFAULT=0.80

set_bmap() { gsctl overlay_e2_set "$BMAP_ID $1" >/dev/null 2>&1 || true; sleep "$SETTLE_S"; }
set_bmap_param() { gsctl overlay_e2_set "$1 $2" >/dev/null 2>&1 || true; sleep "$SETTLE_S"; }
bmap_defaults() {
	set_bmap_param "$BMAP_STRENGTH_ID" "$BMAP_STRENGTH_DEFAULT"
	set_bmap_param "$BMAP_RADIUS_ID" "$BMAP_RADIUS_DEFAULT"
	set_bmap_param "$BMAP_TARGET_ID" "$BMAP_TARGET_DEFAULT"
	set_bmap_param "$BMAP_MIN_ID" "$BMAP_MIN_DEFAULT"
	set_bmap_param "$BMAP_MAX_ID" "$BMAP_MAX_DEFAULT"
}

SCENES="models,modelsinv,halobox,haloinv,texdark"
start_instance
sleep "$ADAPT_SETTLE_S"

# --- models: the control, the identity, and THE HEADLINE ---------------------
BMAP_MODELS_OFF="$(take_screenshot 21-models-bmap-off)"
run_sampler modelsoff "$BMAP_MODELS_OFF" models
set_bmap_param "$BMAP_STRENGTH_ID" 0.0
set_bmap 1
run_sampler modelsid "$BMAP_MODELS_OFF" "$(take_screenshot 21-models-bmap-strength-0.0)" models
declare -a BMAP_LIFT=()
for X in 0.25 0.5 0.75 1.0; do
	set_bmap_param "$BMAP_STRENGTH_ID" "$X"
	BMAP_LIFT+=( "$(take_screenshot "21-models-bmap-strength-$X")" )
done
run_sampler modelslift bmap-headline models 25 "${BMAP_LIFT[@]}"
bmap_defaults

# WHICH SIZES SURVIVE EACH RADIUS -- reported, not asserted. There is no bar
# anybody could set honestly here: a wider map SHOULD lose the smaller
# objects, that is precisely what the control does. The sweep runs to 2.0
# since 2026-09-10, when the slider was widened ("Also increase the max
# radius to 2.0 effectively") and its floor made finer ("Cant we make it, so
# a Radius of 0 is actually pixel perfect?" -- it cannot be, see
# shader-effects.md, but 0 now averages 4 source pixels rather than 8).
declare -a BMAP_SIZE=()
declare -A BMAP_SIZE_SHOT=()
for R in 0.0 0.25 0.5 1.0 1.5 2.0; do
	set_bmap_param "$BMAP_RADIUS_ID" "$R"
	BMAP_SIZE_SHOT[$R]="$(take_screenshot "21-models-bmap-radius-$R")"
	BMAP_SIZE+=( "${BMAP_SIZE_SHOT[$R]}" )
done
run_sampler modelsinfo bmap-size-radius "${BMAP_SIZE[@]}"

# THE COMPATIBILITY PIN. Widening a slider must not change what the values
# already stored in every saved profile look like. These two rows are the
# numbers the SHIPPED build measured on this same scene before the change
# (build-release/verify-shots/brightness-map-2026-09-09/captures/), so if the
# map's reduction, its kernel or the Radius -> sigma curve ever moves an
# existing setting's picture, it fails here rather than in a user's game.
# 2 counts of tolerance: the map is built at a different reduction now (1/4
# at the default, where it used to be 1/8) and reconstructed through a
# correspondingly finer bilinear upsample, which is worth a few tenths.
run_sampler modelspin bmap-radius-0.25-unchanged "${BMAP_SIZE_SHOT[0.25]}" \
	20.7 37.2 61.9 76.1 164.0 2.0
run_sampler modelspin bmap-radius-1.0-unchanged "${BMAP_SIZE_SHOT[1.0]}" \
	16.0 17.8 29.0 52.9 164.0 2.0
bmap_defaults

# The two guard rails, at full strength so their effect is at its largest:
# raising Min must SHRINK the lift the dark objects get (their neighbourhood
# is treated as brighter than it is), and raising Max must let the bright
# field be pulled down harder.
set_bmap_param "$BMAP_STRENGTH_ID" 1.0
declare -a BMAP_RAILS=()
for M in 0.02 0.10 0.30 0.50; do
	set_bmap_param "$BMAP_MIN_ID" "$M"
	BMAP_RAILS+=( "$(take_screenshot "21-models-bmap-min-$M")" )
done
run_sampler modelsinfo bmap-min-brightness "${BMAP_RAILS[@]}"
set_bmap_param "$BMAP_MIN_ID" "$BMAP_MIN_DEFAULT"
declare -a BMAP_RAILS2=()
for M in 0.50 0.70 0.90; do
	set_bmap_param "$BMAP_MAX_ID" "$M"
	BMAP_RAILS2+=( "$(take_screenshot "21-models-bmap-max-$M")" )
done
run_sampler modelsinfo bmap-max-brightness "${BMAP_RAILS2[@]}"
bmap_defaults
set_bmap 0

# --- modelsinv: the inverse, a bright object on a dark field ----------------
advance_scenes 1
sleep "$ADAPT_SETTLE_S"
run_sampler modelsoff "$(take_screenshot 22-modelsinv-bmap-off)" modelsinv
set_bmap 1
declare -a BMAP_LIFT_I=()
for X in 0.25 0.5 0.75 1.0; do
	set_bmap_param "$BMAP_STRENGTH_ID" "$X"
	BMAP_LIFT_I+=( "$(take_screenshot "22-modelsinv-bmap-strength-$X")" )
done
run_sampler modelslift bmap-headline-inv modelsinv 25 "${BMAP_LIFT_I[@]}"
bmap_defaults
set_bmap 0

# --- halobox / haloinv: the artefact ----------------------------------------
advance_scenes 1
sleep "$ADAPT_SETTLE_S"
set_bmap 1
declare -a BMAP_HALO_S=()
for X in 0.25 0.5 1.0; do
	set_bmap_param "$BMAP_STRENGTH_ID" "$X"
	BMAP_HALO_S+=( "$(take_screenshot "23-halobox-bmap-strength-$X")" )
done
run_sampler bmapline bmap-halo-strength-halobox "${BMAP_HALO_S[@]}"
bmap_defaults
run_sampler bmaphalo bmap-halo-halobox-default "$(take_screenshot 23-halobox-bmap-default)" halobox 50
declare -a BMAP_HALO_R=()
for R in 0.0 0.5 1.0 2.0; do
	set_bmap_param "$BMAP_RADIUS_ID" "$R"
	BMAP_HALO_R+=( "$(take_screenshot "23-halobox-bmap-radius-$R")" )
done
run_sampler bmapline bmap-halo-radius-halobox "${BMAP_HALO_R[@]}"
# THE TWO EXTREMES, ASSERTED, not only printed: both ends of the widened
# slider are worse than the middle and they are worse in DIFFERENT ways --
# the fine end puts a tall, tight rim right at the edge, the wide end a low
# one smeared over a hundred pixels. Both must still be monotone (a
# non-negative blur of a step cannot ring), and each carries its own honest
# amplitude budget rather than one bar picked so that both happen to clear.
set_bmap_param "$BMAP_RADIUS_ID" 0.0
run_sampler bmaphalo bmap-halo-halobox-radius-0 "$(take_screenshot 23-halobox-bmap-radius-min)" halobox 50
set_bmap_param "$BMAP_RADIUS_ID" 2.0
run_sampler bmaphalo bmap-halo-halobox-radius-2 "$(take_screenshot 23-halobox-bmap-radius-max)" halobox 50
bmap_defaults

advance_scenes 1   # -> haloinv, the inverse edge
sleep "$ADAPT_SETTLE_S"
run_sampler bmaphalo bmap-halo-haloinv-default "$(take_screenshot 24-haloinv-bmap-default)" haloinv 50
set_bmap_param "$BMAP_STRENGTH_ID" 1.0
# The budget rises with strength ON PURPOSE, and this one is generous: at
# full strength on a hard-edged 320 px box the rim is the operator working,
# not a defect (measured -82 counts at Radius 0.5). What the bar catches is a
# regression, and what the user acts on is the number at the shipped default
# a few lines above -- so both are printed and both are asserted, at their own
# honest sizes rather than at one bar chosen so that both happen to clear.
run_sampler bmaphalo bmap-halo-haloinv-full "$(take_screenshot 24-haloinv-bmap-full)" haloinv 95
bmap_defaults
declare -a BMAP_HALO_RI=()
for R in 0.0 0.5 1.0 2.0; do
	set_bmap_param "$BMAP_RADIUS_ID" "$R"
	BMAP_HALO_RI+=( "$(take_screenshot "24-haloinv-bmap-radius-$R")" )
done
run_sampler bmapline bmap-halo-radius-haloinv "${BMAP_HALO_RI[@]}"
bmap_defaults
set_bmap 0

# --- texdark: stability, still and then panning ------------------------------
advance_scenes 1
sleep "$ADAPT_SETTLE_S"
toggle_motion      # this instance starts with --motion 3 RUNNING; pause it
sleep 1
set_bmap 1
arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-25-bmap-static.txt"
run_sampler ablog "$OUT_DIR/ablog-25-bmap-static.txt" bmapstatic
# ... and at BOTH ENDS of the widened slider (2026-09-10). The fine end is
# the one that could pulse -- it is the setting whose map can resolve the
# scene's own 16 px cells -- and the wide end changes the map's reduction, so
# neither is covered by the default's run above.
for R in 0.0 2.0; do
	set_bmap_param "$BMAP_RADIUS_ID" "$R"
	arm_ab_log "$AB_FRAMES"; wait_ab_log "$AB_FRAMES" "$OUT_DIR/ablog-25-bmap-static-r$R.txt"
	run_sampler ablog "$OUT_DIR/ablog-25-bmap-static-r$R.txt" bmapstatic "-radius-$R"
done
bmap_defaults

# The shimmer question. --periodic fixes the frame's light population, so the
# frame mean's spread across captures is the harness's own noise, and a map
# this fine must not add to it.
toggle_motion
sleep 1
declare -a BMAP_OFF_JIT=()
set_bmap 0
for k in 1 2 3 4 5 6; do BMAP_OFF_JIT+=( "$(take_screenshot "25-texdark-bmap-off-$k")" ); done
set_bmap 1
declare -a BMAP_JIT=( "${BMAP_OFF_JIT[@]}" )
for k in 1 2 3 4 5 6; do BMAP_JIT+=( "$(take_screenshot "25-texdark-bmap-on-$k")" ); done
run_sampler bloomjitter bmap-stability-pan 6 "${BMAP_JIT[@]}"
# ... and at BOTH ENDS of the widened Radius (2026-09-10), against the same
# effect-off baseline. The fine end is the one that could pulse: it is the
# only setting whose map can resolve this scene's own 16 px cells, so if a
# finer map were going to make the exposure breathe under a pan, it would
# breathe here.
for R in 0.0 2.0; do
	set_bmap_param "$BMAP_RADIUS_ID" "$R"
	declare -a BMAP_JIT_R=( "${BMAP_OFF_JIT[@]}" )
	for k in 1 2 3 4 5 6; do BMAP_JIT_R+=( "$(take_screenshot "25-texdark-bmap-on-r$R-$k")" ); done
	run_sampler bloomjitter "bmap-stability-pan-radius-$R" 6 "${BMAP_JIT_R[@]}"
done
bmap_defaults
set_bmap 0

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
