#!/usr/bin/env bash
# pixel-regression.sh -- one-command pixel regression gate for the FPS HUD /
# crosshair inversion, colour and outline behaviour.
#
# WHY THIS EXISTS
#   "Does the Inverted HUD still invert the pixel under it" has regressed
#   TWICE (see superdoc/features/crosshair.md's "Known limitation" history and
#   the grade-screenshots-not-checklists memory note) and each time an agent
#   re-did the capture-and-sample dance by hand on the laptop. This script is
#   that dance, automated, run here on the desktop with NO visible window and
#   no laptop round trip. Non-zero exit gates a commit.
#
# HOW IT SEES PIXELS WITHOUT TOUCHING THE USER'S DESKTOP
#   gamescope's own `--backend headless` was tried and rejected: measured on
#   this rig, `gamescopectl screenshot` against a headless-backend instance
#   captures NO extra composited layer at all -- not the HUD, not the
#   crosshair, not even the cursor plane (see superdoc/features/
#   cursor-pipeline.md's "Verified by direct X11 query, not by compositor
#   screenshot" section). That is a property of this sandbox's headless
#   Vulkan path, not of the feature under test.
#
#   Instead: a private, invisible sway (WLR_BACKENDS=headless, its own
#   isolated XDG_RUNTIME_DIR, no input devices) hosts a REAL nested gamescope
#   (--backend wayland), and `gamescopectl screenshot "<path> 4"` (type 4 =
#   screen_buffer, post-composite) against THAT instance does capture the HUD
#   and crosshair -- this is the exact recipe verified in
#   superdoc/planning/wayland-vrr-buffer-lifetime.md and used to produce the
#   reference values in superdoc/features/fps-display.md's
#   "Verifying Inverted mode (pixel recipe)" section. Nothing here is ever
#   visible on the real desktop: the private sway has no output sink any real
#   compositor can show, and the private XDG_RUNTIME_DIR means no other
#   gamescope/gamescopectl on the machine can address this instance either.
#
# TEST CLIENT
#   superdoc/features/fps-display.md's own verified recipe uses xterm with
#   matching -bg/-fg for a perfectly flat background -- but xterm is not
#   installed on this desktop (confirmed: "Failed to start process xterm: No
#   such file or directory" on the first real run of this script). kitty is
#   installed, so it plays the same role: `-o background=X -o foreground=X
#   -o cursor=X` makes the cell colour, the text colour (nothing is printed
#   anyway) AND the cursor all the same flat colour, and `-c NONE` skips any
#   user kitty config this isolated XDG_CONFIG_HOME wouldn't have found
#   anyway. `sleep 600` as the ran command keeps it blank, no shell prompt.
#   No new test client was written -- kitty was already installed and is a
#   real Wayland client, not a fallback.
#
# DRIVING STATE WITHOUT POINTER INPUT
#   The initial config (fps_display.*, crosshair.*) is a JSON file this
#   script writes into an isolated XDG_CONFIG_HOME (never the user's own
#   ~/.config/gamescope-ritz). Everything that needs to change WHILE an
#   instance is running goes through `overlay_e2_set <id> <value>`
#   (src/Overlay/UI/Shell.cpp) -- the same binding a mouse click writes
#   through -- and `fps_display_force <n>` pins the HUD's displayed digit
#   count so a screenshot doesn't race real frame timing. Both are ConCommands
#   reached over gamescope's OWN control protocol via `gamescopectl`, not OS
#   input.
#
# WHAT EACH CHECK VERIFIES (see the header comment above each check_* function
# for the exact assertion and superdoc/features/fps-display.md /
# crosshair.md for the feature's own spec):
#   inversion            -- HUD digit inverts a dark background
#   inversion-midtone     -- HUD digit still discriminates at a mid-tone
#                            background (the perceptual-floor push, the exact
#                            band a past regression silently broke)
#   inversion-crosshair    -- Inverted HUD + crosshair in ONE layer: digit
#                            still inverts AND the crosshair keeps its own
#                            colour AND its outline stays black, all at once
#                            (the check that regressed unnoticed on
#                            2026-09-05, when this was the two-layer "split
#                            mode")
#   inversion-crosshair-alpha -- same configuration, crosshair at 50 %
#                            opacity: the arm measures the documented
#                            premultiplied-then-coverage blend of its colour
#                            over the background (crosshair.md "Known
#                            limitation"), i.e. the invert marker did not eat
#                            a translucent crosshair
#   layer-budget           -- `layer_budget_stats` high-water mark with the
#                            Inverted HUD and the crosshair both on from
#                            startup: exactly the count one shared HUD layer
#                            gives (the split mode's second layer is gone)
#   fixed                 -- Fixed text-colour mode paints the configured
#                            colour exactly
#   outline                -- the HUD's own black digit outline, on and off
#   crosshair-geometry     -- all four arms are the configured colour at the
#                            expected offsets, background shows in the gap,
#                            and the crosshair's own outline is black
#
# USAGE
#   scripts/pixel-regression.sh                # run everything
#   scripts/pixel-regression.sh --only outline # run one check
#   scripts/pixel-regression.sh --keep         # leave the last instance up
#                                               # for manual `gamescopectl`
#                                               # poking; prints how to reach it
#
# Exits 0 if every check that ran passed, 1 if any failed, 2 on a setup
# problem (binary missing, sway/gamescopectl not found, an instance never
# came up) that isn't a verdict on the feature at all.
#
# Runs the whole session under with-gamescope-lock.sh (self-re-execs under it
# on first invocation) -- see that script's header for why.
set -euo pipefail

if [[ -z "${PIXEL_REGRESSION_LOCKED:-}" ]]; then
	export PIXEL_REGRESSION_LOCKED=1
	SCRIPT_DIR_BOOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	exec "$SCRIPT_DIR_BOOT/with-gamescope-lock.sh" "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SAMPLER="$SCRIPT_DIR/pixel_regression_sample.py"
GAMESCOPE_BIN="$REPO_ROOT/build-release/src/gamescope"
GAMESCOPECTL_BIN="$REPO_ROOT/build-release/src/gamescopectl"

# ---------------------------------------------------------------------------
# Named constants -- every threshold a check compares against lives here,
# nowhere else. If a check needs "re-tuning", it happens on this list, in a
# reviewable diff -- never by editing the sampler.
# ---------------------------------------------------------------------------
OUT_W=1280
OUT_H=720
FONT_SIZE=36
MARGIN_X=24
MARGIN_Y=24
FORCED_FPS=60                    # fps_display_force value -- fixed 2-digit reading

# HUD digit sampling box (top-left anchor, generous around the "60" glyphs
# plus room for a 4px outline). See ResolveAnchoredOrigin() in FpsDisplay.cpp.
DIGIT_BOX_X0=$((MARGIN_X - 8)); DIGIT_BOX_Y0=$((MARGIN_Y - 8))
DIGIT_BOX_X1=$((MARGIN_X + 130)); DIGIT_BOX_Y1=$((MARGIN_Y + 70))

# Background sample point: bottom-centre, far from both the top-left HUD box
# and the output-centre crosshair.
BG_SAMPLE_X=$((OUT_W / 2))
BG_SAMPLE_Y=$((OUT_H - 20))

DIFF_THRESH=20      # "is this pixel not the flat background" chebyshev distance
BLACK_THRESH=15     # "is this pixel near-black" -- separates a pushed-dark
                    # inverted digit (e.g. 46,46,46) from a true black outline
DIGIT_TOL=10        # per-channel tolerance vs the reference inversion values
MIN_ENCODED_SEPARATION=90   # floor is 0.40*255=102 (alphamode.h's
                            # kMinEncodedSeparation); a few counts of slack
                            # for cross-driver pow() rounding

FIXED_COLOR_HEX=0x40C0FF   # arbitrary, distinct-from-everything digit colour
FIXED_COLOR_TOL=2          # task spec: "within ±2"

HUD_OUTLINE_STRENGTH=2
HUD_OUTLINE_MIN_BLACK_PX=8   # a 2px outline ring around two glyphs is easily
                            # this many pixels; regressed-to-off would be ~0

# Dark and mid-tone backgrounds, and the reference digit values measured
# 2026-09-05 (superdoc/features/fps-display.md, "Verifying Inverted mode").
BG_DARK_HEX="#333333"; BG_DARK_R=51;  BG_DARK_G=51;  BG_DARK_B=51
EXP_DARK_R=251; EXP_DARK_G=251; EXP_DARK_B=251

BG_MID_HEX="#949494";  BG_MID_R=148; BG_MID_G=148; BG_MID_B=148
EXP_MID_R=46;  EXP_MID_G=46;  EXP_MID_B=46
MID_TOL=20   # the pushed-dark value is more sensitive to exact pow()
             # rounding than a plain true-invert; a real regression (the
             # 2026-09-05 bug) landed ~170 counts off, nowhere near this

# Crosshair geometry (apply_scaling off -- exact pixel path).
CH_LINE_LENGTH=12
CH_LINE_WIDTH=3
CH_LINE_GAP=8
CH_LINE_COLOR_HEX=0x00FF00
CH_OUTLINE_WIDTH=2
CH_OUTLINE_COLOR_R=0; CH_OUTLINE_COLOR_G=0; CH_OUTLINE_COLOR_B=0
CH_COLOR_TOL=8
# Semi-transparent crosshair (inversion-crosshair-alpha): the arm at this
# opacity over BG_DARK must measure the premultiplied-then-coverage blend
# pixel_regression_sample.py's coverage_blend_expected() derives -- for the
# green above that is (35, 99, 35), measured identically on the last split-mode
# build (2026-09-06, build-release/verify-shots/split-retire/before/) and on
# the single-layer one. NOT the ideal c*0.5 + bg*0.5 (which would be 153 or
# 190 in G): see that function's docstring for the arithmetic and why the
# check pins the real mechanism rather than an ideal it never had.
CH_ALPHA_OPACITY=0.5
CH_ALPHA_TOL=3                 # task spec: +/-3
CH_ALPHA_TEXEL_BITS=16         # Inverted + crosshair share a 16-bit texture (FpsDisplay.cpp ResolveTextureFormat)

# Layer budget (layer-budget): `layer_budget_stats`' high-water mark is global
# since startup, and on this harness the startup itself peaks at 3 layers
# plus whatever the HUD pushes (measured 2026-09-06: 4 with the readout alone,
# on every build) -- so the readout AND the crosshair must be on FROM THE
# CONFIG FILE for the mark to include the HUD's crosshair handling at all.
# With both on at startup, the last split-mode build (two HUD layers) measured
# 5 (build-release/verify-shots/split-retire/hwm2/gs-true.log) and the
# single-layer build measures 4. Asserted as an exact value: a 5 here means a
# second HUD layer is back.
EXPECTED_LAYER_HWM=4
# Safe sample windows along each arm's own axis, offset from centre in px.
# Measured directly off a real capture (crosshair.md's outline is "strictly
# outside the fill", on EVERY side -- including the side facing the centre,
# which is easy to miss from the spec alone): with gap=8, outline_width=2,
# length=12, a scan from the centre outward reads
#   offset   0.. 7  background        (the gap itself)
#   offset   8.. 9  outline (black)   (the NEAR ring, gap .. gap+ow-1)
#   offset  10..21  fill (line colour) (gap+ow .. gap+ow+length-1)
#   offset  22..23  outline (black)   (the FAR ring)
#   offset  24..    background
# i.e. the near ring eats into what a naive "gap = background" reading would
# expect -- the true background-only band is only [0, gap-1]. Windows below
# keep a 1px margin off every boundary; the near ring is checked exactly
# (only outline_width px wide, no margin room, but the scan showed it solid).
CH_GAP_LO=1;                                    CH_GAP_HI=$((CH_LINE_GAP - 2))
CH_OUT_LO=$CH_LINE_GAP;                          CH_OUT_HI=$((CH_LINE_GAP + CH_OUTLINE_WIDTH - 1))
CH_ARM_LO=$((CH_LINE_GAP + CH_OUTLINE_WIDTH + 1)); CH_ARM_HI=$((CH_LINE_GAP + CH_OUTLINE_WIDTH + CH_LINE_LENGTH - 2))
CH_CENTER_X=$((OUT_W / 2))
CH_CENTER_Y=$((OUT_H / 2))

READY_TIMEOUT_S=20
SWAY_READY_TIMEOUT_S=10
SCREENSHOT_TIMEOUT_S=6
SETTLE_S=0.25   # brief settle after a config change before requesting a screenshot

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
KEEP=0
ONLY=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--keep) KEEP=1; shift ;;
		--only) ONLY="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,90p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*) echo "pixel-regression: unknown argument: $1" >&2; exit 2 ;;
	esac
done

should_run() {
	[[ -z "$ONLY" || "$ONLY" == "$1" ]]
}

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------
TS="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$REPO_ROOT/build-release/verify-shots/pixel-regression/$TS"
mkdir -p "$OUT_DIR"
RESULTS_FILE="$OUT_DIR/results.txt"

# Short-path scratch dirs -- MUST be short: a unix socket path is capped at
# 108 bytes and this repo's own scratchpad path is long enough to overflow it
# (see superdoc/planning/wayland-vrr-buffer-lifetime.md's sway recipe note).
# Never the scratchpad, never the user's real ~/.config.
RUNDIR="$(mktemp -d /tmp/gs-pixreg-run.XXXXXX)"
CONFIGHOME="$(mktemp -d /tmp/gs-pixreg-cfg.XXXXXX)"
SWAY_CFG="$RUNDIR/sway.conf"

SWAY_PID=""
GS_PID=""

log() { echo "[pixel-regression] $*" >&2; }

# Kill only the exact PIDs this script started -- never a name/pattern match
# (see the track-your-own-gamescope-instance memory note).
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
		if [[ -n "${GS_WL_NAME:-}" ]]; then
			log "  Poke it with: XDG_RUNTIME_DIR=$RUNDIR GAMESCOPE_WAYLAND_DISPLAY=$GS_WL_NAME $GAMESCOPECTL_BIN <cmd>"
		fi
		log "  Nothing else was removed; clean up by killing those two PIDs and rm -rf the two dirs above."
		exit "$status"
	fi
	teardown_all
	rm -rf "$RUNDIR" "$CONFIGHOME"
	exit "$status"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
if [[ ! -x "$GAMESCOPE_BIN" ]]; then
	log "FATAL: $GAMESCOPE_BIN not found. Building is not this script's job --"
	log "  run scripts/build-gamescope-ritz.sh yourself (under its own lock) first."
	exit 2
fi
if [[ ! -x "$GAMESCOPECTL_BIN" ]]; then
	log "FATAL: $GAMESCOPECTL_BIN not found (should build alongside gamescope)."
	exit 2
fi
if ! command -v sway >/dev/null 2>&1; then
	log "FATAL: sway not found -- needed as the private, invisible host compositor."
	exit 2
fi
if ! python3 -c 'import PIL' 2>/dev/null; then
	log "FATAL: python3's PIL is not importable -- see scripts/README.md's pixel-regression section."
	exit 2
fi

# ---------------------------------------------------------------------------
# Private sway host (headless backend, no input devices, nothing visible)
# ---------------------------------------------------------------------------
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

	log "starting private sway (headless backend, XDG_RUNTIME_DIR=$RUNDIR)"
	# `9>&-`: with-gamescope-lock.sh holds its flock open on fd 9 for this
	# whole script's lifetime, and a background child inherits open fds by
	# default -- so without closing it here, a leaked/kept-alive sway or
	# gamescope would keep the machine-wide lock held forever even after
	# this script itself has exited (found the hard way: a --keep run left
	# fd 9 open in gamescope and every later invocation hung on the lock).
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 XDG_RUNTIME_DIR="$RUNDIR" \
		sway -c "$SWAY_CFG" > "$RUNDIR/sway.log" 2>&1 9>&- &
	SWAY_PID=$!

	local waited=0
	SWAY_WL_NAME=""
	while (( waited < SWAY_READY_TIMEOUT_S * 10 )); do
		SWAY_WL_NAME="$(find "$RUNDIR" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' -printf '%f\n' 2>/dev/null | head -1)"
		if [[ -n "$SWAY_WL_NAME" ]]; then break; fi
		kill -0 "$SWAY_PID" 2>/dev/null || { log "FATAL: sway exited before creating a socket -- see $RUNDIR/sway.log"; cat "$RUNDIR/sway.log" >&2; exit 2; }
		sleep 0.1
		waited=$((waited + 1))
	done
	if [[ -z "$SWAY_WL_NAME" ]]; then
		log "FATAL: sway never created a wayland socket within ${SWAY_READY_TIMEOUT_S}s -- see $RUNDIR/sway.log"
		cat "$RUNDIR/sway.log" >&2
		exit 2
	fi
	log "private sway ready: pid $SWAY_PID, socket $SWAY_WL_NAME"
}

# ---------------------------------------------------------------------------
# Config file this script writes and owns -- never the user's real one.
#
# The crosshair starts ON (with the Inverted readout) so that layer-budget
# sees the HUD's startup layer count -- see EXPECTED_LAYER_HWM; every check
# that wants it off switches it off first (run section below).
# ---------------------------------------------------------------------------
write_config() {
	mkdir -p "$CONFIGHOME/gamescope-ritz/profiles"
	local color_fps_dec ch_line_color_dec
	color_fps_dec=$((FIXED_COLOR_HEX))
	ch_line_color_dec=$((CH_LINE_COLOR_HEX))
	# Schema 3 (Profiles v2, 2026-09-06): global.json carries only the
	# overlay appearance and the profile pointers; the per-layer sections
	# live in profiles/<Name>.json. The session resolves to last_general.
	cat > "$CONFIGHOME/gamescope-ritz/global.json" <<-EOF
		{
		    "schema_version": 3,
		    "profiles": { "last_general": "Pixel", "games": {} }
		}
	EOF
	cat > "$CONFIGHOME/gamescope-ritz/profiles/Pixel.json" <<-EOF
		{
		    "schema_version": 3,
		    "name": "Pixel",
		    "kind": "general",
		    "fps_display": {
		        "enabled": true,
		        "font_size": $FONT_SIZE,
		        "backdrop_opacity": 0.0,
		        "text_opacity": 1.0,
		        "update_mode": "smoothing",
		        "hide_above_enabled": false,
		        "color_mode": "inverted",
		        "outline_strength": 0.0,
		        "lag_detection_enabled": false,
		        "color_fps": $color_fps_dec,
		        "anchor": "top-left",
		        "margin_x": $MARGIN_X,
		        "margin_y": $MARGIN_Y
		    },
		    "crosshair": {
		        "enabled": true,
		        "line_enabled": true,
		        "line_length": $CH_LINE_LENGTH,
		        "line_width": $CH_LINE_WIDTH,
		        "line_gap": $CH_LINE_GAP,
		        "line_color": $ch_line_color_dec,
		        "line_opacity": 1.0,
		        "dot_enabled": false,
		        "dot_size": 2,
		        "dot_color": $ch_line_color_dec,
		        "dot_opacity": 1.0,
		        "outline_enabled": true,
		        "outline_width": $CH_OUTLINE_WIDTH,
		        "outline_opacity": 1.0,
		        "outline_color": 0,
		        "hide_on_right_click": false,
		        "hide_mode": "fade",
		        "hide_time_ms": 200,
		        "apply_scaling": false
		    }
		}
	EOF
}

# ---------------------------------------------------------------------------
# One nested gamescope instance against a given flat xterm background.
# ---------------------------------------------------------------------------
GS_LOG=""
GS_WL_NAME=""

start_instance() {
	local bg_hex="$1"
	teardown_instance   # only one instance (one xterm background) at a time

	GS_LOG="$RUNDIR/gamescope-$(date +%s%N).log"
	log "starting gamescope instance, background $bg_hex"
	WAYLAND_DISPLAY="$SWAY_WL_NAME" XDG_RUNTIME_DIR="$RUNDIR" XDG_CONFIG_HOME="$CONFIGHOME" \
		"$GAMESCOPE_BIN" --backend wayland -w "$OUT_W" -h "$OUT_H" -W "$OUT_W" -H "$OUT_H" \
		--force-windows-fullscreen -- \
		kitty -c NONE -o background="$bg_hex" -o foreground="$bg_hex" -o cursor="$bg_hex" \
			-o cursor_blink_interval=0 -o remember_window_size=no \
			sleep 600 \
		> "$GS_LOG" 2>&1 9>&- &
	GS_PID=$!

	# Same readiness recipe as overlay-test-harness.sh: poll the log for
	# gamescope's own "wayland display '<name>'" line, then for the client's
	# first presented frame ("refresh cycle") -- never a fixed sleep.
	local waited=0
	GS_WL_NAME=""
	while (( waited < READY_TIMEOUT_S * 10 )); do
		# `|| true`: under `set -o pipefail`, grep finding nothing yet (still
		# starting up) makes the whole pipeline's status 1 even though head
		# succeeds, which would abort the script right here under `set -e`.
		GS_WL_NAME="$(grep -oP "wayland display '\K[^']+" "$GS_LOG" 2>/dev/null | head -1 || true)"
		if [[ -n "$GS_WL_NAME" ]]; then break; fi
		kill -0 "$GS_PID" 2>/dev/null || { log "FATAL: gamescope exited before reporting its display -- see $GS_LOG"; tail -n 40 "$GS_LOG" >&2; exit 2; }
		sleep 0.1
		waited=$((waited + 1))
	done
	if [[ -z "$GS_WL_NAME" ]]; then
		log "FATAL: gamescope never reported its wayland display within ${READY_TIMEOUT_S}s -- see $GS_LOG"
		tail -n 40 "$GS_LOG" >&2
		exit 2
	fi

	waited=0
	while (( waited < READY_TIMEOUT_S * 10 )); do
		if grep -q "refresh cycle" "$GS_LOG" 2>/dev/null; then break; fi
		kill -0 "$GS_PID" 2>/dev/null || { log "FATAL: gamescope exited before the client presented a frame -- see $GS_LOG"; tail -n 40 "$GS_LOG" >&2; exit 2; }
		sleep 0.1
		waited=$((waited + 1))
	done
	log "gamescope instance ready: pid $GS_PID, control socket $GS_WL_NAME"
}

gsctl() {
	XDG_RUNTIME_DIR="$RUNDIR" GAMESCOPE_WAYLAND_DISPLAY="$GS_WL_NAME" "$GAMESCOPECTL_BIN" "$@"
}

# Sets: forces the digit and waits out one settle window. Degrades gracefully
# if a build predates fps_display_force (a parallel agent's addition) --
# skips the digit-count pin and says why, per the task brief.
FPS_FORCE_AVAILABLE=1
apply_fps_force() {
	if [[ "$FPS_FORCE_AVAILABLE" -eq 0 ]]; then
		return
	fi
	local out
	out="$(gsctl fps_display_force "$FORCED_FPS" 2>&1)" || true
	if grep -qi "no such\|unknown command\|not found" <<<"$out"; then
		log "fps_display_force not present in this build -- skipping the forced-digit pin (older binary)."
		FPS_FORCE_AVAILABLE=0
	fi
}

set_val() {
	gsctl overlay_e2_set "$1 $2" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
}

take_screenshot() {
	local name="$1"
	local path="$OUT_DIR/$name.png"
	gsctl screenshot "$path 4" >/dev/null 2>&1 || true
	# Poll for existence and a non-growing size instead of a fixed sleep --
	# the PNG write happens on a background thread (steamcompmgr.cpp).
	local waited=0 last_size=-1 size=0
	while (( waited < SCREENSHOT_TIMEOUT_S * 20 )); do
		if [[ -f "$path" ]]; then
			size="$(stat -c%s "$path" 2>/dev/null || echo 0)"
			if [[ "$size" -gt 0 && "$size" == "$last_size" ]]; then
				echo "$path"
				return 0
			fi
			last_size="$size"
		fi
		sleep 0.05
		waited=$((waited + 1))
	done
	log "WARNING: screenshot $name never stabilised within ${SCREENSHOT_TIMEOUT_S}s (last size $last_size)"
	echo "$path"
}

# ---------------------------------------------------------------------------
# Results table
# ---------------------------------------------------------------------------
declare -a RESULT_LINES=()
FAIL_COUNT=0
SKIP_COUNT=0

record_line() {
	# $1 = raw "STATUS<TAB>name<TAB>detail" line from the sampler, or a
	# synthetic "SKIP<TAB>name<TAB>reason" this script produced itself.
	local line="$1"
	local status name detail
	IFS=$'\t' read -r status name detail <<<"$line"
	RESULT_LINES+=("$status	$name	$detail")
	case "$status" in
		FAIL) FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
		SKIP) SKIP_COUNT=$((SKIP_COUNT + 1)) ;;
	esac
	log "$status  $name  $detail"
}

run_sampler() {
	# Never let `set -e` kill the whole run on one FAIL -- the sampler's exit
	# code IS the verdict, not a script error.
	local out
	out="$(python3 "$SAMPLER" "$@" 2>>"$OUT_DIR/sampler.log")" || true
	record_line "$out"
}

skip_check() {
	record_line "SKIP	$1	$2"
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

# Inversion: HUD text mode Inverted, crosshair off. Digit core vs the
# background pixel beside the box; dark bg expects a near-total invert,
# mid-tone bg expects the perceptual-floor push (fps-display.md's
# "Text colour: Fixed vs. Inverted" contrast guard).
check_inversion() {
	should_run inversion || { skip_check inversion "--only excluded it"; return; }
	local shot; shot="$(take_screenshot 01-inversion-dark)"
	run_sampler digit "$shot" "$DIGIT_BOX_X0" "$DIGIT_BOX_Y0" "$DIGIT_BOX_X1" "$DIGIT_BOX_Y1" \
		"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "$BLACK_THRESH" \
		"$EXP_DARK_R" "$EXP_DARK_G" "$EXP_DARK_B" "$DIGIT_TOL" "$MIN_ENCODED_SEPARATION" \
		"inversion-dark"
}

check_inversion_midtone() {
	should_run inversion-midtone || { skip_check inversion-midtone "--only excluded it"; return; }
	# Needs the mid-tone-background instance -- run_midtone_instance() below
	# calls this after switching backgrounds.
	local shot; shot="$(take_screenshot 06-inversion-midtone)"
	run_sampler digit "$shot" "$DIGIT_BOX_X0" "$DIGIT_BOX_Y0" "$DIGIT_BOX_X1" "$DIGIT_BOX_Y1" \
		"$BG_MID_R" "$BG_MID_G" "$BG_MID_B" "$DIFF_THRESH" "$BLACK_THRESH" \
		"$EXP_MID_R" "$EXP_MID_G" "$EXP_MID_B" "$MID_TOL" "$MIN_ENCODED_SEPARATION" \
		"inversion-midtone"
}

# The check that regressed unnoticed: Inverted HUD + crosshair on (split
# mode). Digit still inverts, AND the crosshair keeps its configured colour
# (not inverted), AND its own outline is black.
check_inversion_crosshair() {
	should_run inversion-crosshair || { skip_check inversion-crosshair "--only excluded it"; return; }
	set_val "crosshair.enabled" 1
	local shot; shot="$(take_screenshot 02-inversion-crosshair)"
	run_sampler digit "$shot" "$DIGIT_BOX_X0" "$DIGIT_BOX_Y0" "$DIGIT_BOX_X1" "$DIGIT_BOX_Y1" \
		"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "$BLACK_THRESH" \
		"$EXP_DARK_R" "$EXP_DARK_G" "$EXP_DARK_B" "$DIGIT_TOL" "$MIN_ENCODED_SEPARATION" \
		"inversion-crosshair-digit"

	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))
	run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" 0 -1 "$CH_ARM_LO" "$CH_ARM_HI" all \
		"$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "inversion-crosshair-arm-not-inverted"
	run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" 0 -1 "$CH_OUT_LO" "$CH_OUT_HI" all \
		"$CH_OUTLINE_COLOR_R" "$CH_OUTLINE_COLOR_G" "$CH_OUTLINE_COLOR_B" "$CH_COLOR_TOL" \
		"inversion-crosshair-outline-black"
	set_val "crosshair.enabled" 0
}

# Semi-transparent crosshair in the Inverted configuration: the arm at 50 %
# opacity measures the documented blend of its colour over the background
# (CH_ALPHA_* above). The old brightness selector would have inverted or
# darkened a translucent bright crosshair; the marker must leave it alone.
check_inversion_crosshair_alpha() {
	should_run inversion-crosshair-alpha || { skip_check inversion-crosshair-alpha "--only excluded it"; return; }
	set_val "crosshair.enabled" 1
	set_val "crosshair.line_opacity" "$CH_ALPHA_OPACITY"
	local shot; shot="$(take_screenshot 08-inversion-crosshair-alpha)"
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))
	run_sampler line_blend "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" 0 -1 "$CH_ARM_LO" "$CH_ARM_HI" all \
		"$ch_r" "$ch_g" "$ch_b" "$CH_ALPHA_OPACITY" "$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$CH_ALPHA_TEXEL_BITS" \
		"$CH_ALPHA_TOL" "inversion-crosshair-alpha-arm"
	# The digit must still invert with a translucent crosshair in the layer.
	run_sampler digit "$shot" "$DIGIT_BOX_X0" "$DIGIT_BOX_Y0" "$DIGIT_BOX_X1" "$DIGIT_BOX_Y1" \
		"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "$BLACK_THRESH" \
		"$EXP_DARK_R" "$EXP_DARK_G" "$EXP_DARK_B" "$DIGIT_TOL" "$MIN_ENCODED_SEPARATION" \
		"inversion-crosshair-alpha-digit"
	set_val "crosshair.line_opacity" 1.0
	set_val "crosshair.enabled" 0
}

# Layer budget: with the Inverted readout and the crosshair both on from
# startup (write_config), `layer_budget_stats`' high-water mark must be
# exactly EXPECTED_LAYER_HWM. Runs FIRST on the dark instance, before any
# other check changes what is on screen.
check_layer_budget() {
	should_run layer-budget || { skip_check layer-budget "--only excluded it"; return; }
	local out hwm
	out="$(gsctl layer_budget_stats 2>&1 || true)"
	hwm="$(grep -oP 'high-water mark \K[0-9]+' <<<"$out" | head -1 || true)"
	if [[ -z "$hwm" ]]; then
		record_line "FAIL	layer-budget	layer_budget_stats gave no high-water mark (output: ${out//$'\n'/ | })"
		return
	fi
	if [[ "$hwm" -eq "$EXPECTED_LAYER_HWM" ]]; then
		record_line "PASS	layer-budget	high-water mark $hwm / 6 with Inverted readout + crosshair from startup (expected $EXPECTED_LAYER_HWM; the split-mode build measured 5)"
	else
		record_line "FAIL	layer-budget	high-water mark $hwm / 6, expected exactly $EXPECTED_LAYER_HWM (the split-mode build measured 5: a second HUD layer is back, or the startup layer set changed)"
	fi
}

# Fixed colour mode: digit equals the configured colour within +/-2.
check_fixed() {
	should_run fixed || { skip_check fixed "--only excluded it"; return; }
	set_val "hud.color_mode" 0   # 0 = fixed (kColorModeOptions order: fixed, inverted)
	local shot; shot="$(take_screenshot 03-fixed)"
	local fr fg fb
	fr=$(( (FIXED_COLOR_HEX >> 16) & 0xFF )); fg=$(( (FIXED_COLOR_HEX >> 8) & 0xFF )); fb=$(( FIXED_COLOR_HEX & 0xFF ))
	run_sampler digit "$shot" "$DIGIT_BOX_X0" "$DIGIT_BOX_Y0" "$DIGIT_BOX_X1" "$DIGIT_BOX_Y1" \
		"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "$BLACK_THRESH" \
		"$fr" "$fg" "$fb" "$FIXED_COLOR_TOL" 0 "fixed-color"
	set_val "hud.color_mode" 1   # back to inverted for the checks that follow
}

# HUD outline: on at strength 2, ring pixels adjacent to the digit strokes
# are black; off, they are background.
check_outline() {
	should_run outline || { skip_check outline "--only excluded it"; return; }
	set_val "hud.outline_strength" "$HUD_OUTLINE_STRENGTH"
	local shot_on; shot_on="$(take_screenshot 04-hud-outline-on)"
	run_sampler blackcount "$shot_on" "$DIGIT_BOX_X0" "$DIGIT_BOX_Y0" "$DIGIT_BOX_X1" "$DIGIT_BOX_Y1" \
		"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "$BLACK_THRESH" \
		"$HUD_OUTLINE_MIN_BLACK_PX" 1 "hud-outline-on"

	set_val "hud.outline_strength" 0
	local shot_off; shot_off="$(take_screenshot 05-hud-outline-off)"
	run_sampler blackcount "$shot_off" "$DIGIT_BOX_X0" "$DIGIT_BOX_Y0" "$DIGIT_BOX_X1" "$DIGIT_BOX_Y1" \
		"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "$BLACK_THRESH" \
		"$HUD_OUTLINE_MIN_BLACK_PX" 0 "hud-outline-off"
}

# Crosshair geometry: all four arms are the configured colour at the expected
# offset from the game-rect centre, background in the gap, black outline just
# past each arm's far end. apply_scaling is off (pixel path, exact) -- see
# superdoc/features/crosshair.md's Geometry section for the offsets' derivation.
check_crosshair_geometry() {
	should_run crosshair-geometry || { skip_check crosshair-geometry "--only excluded it"; return; }
	set_val "crosshair.enabled" 1
	local shot; shot="$(take_screenshot 07-crosshair-geometry)"
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))

	local dir dx dy
	for dir in up:0:-1 down:0:1 left:-1:0 right:1:0; do
		IFS=':' read -r dirname dx dy <<<"$dir"
		run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" "$dx" "$dy" "$CH_GAP_LO" "$CH_GAP_HI" all \
			"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "crosshair-gap-$dirname"
		run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" "$dx" "$dy" "$CH_ARM_LO" "$CH_ARM_HI" all \
			"$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "crosshair-arm-$dirname"
		run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" "$dx" "$dy" "$CH_OUT_LO" "$CH_OUT_HI" all \
			"$CH_OUTLINE_COLOR_R" "$CH_OUTLINE_COLOR_G" "$CH_OUTLINE_COLOR_B" "$CH_COLOR_TOL" \
			"crosshair-outline-$dirname"
	done
	set_val "crosshair.enabled" 0
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
START_TS=$(date +%s)

write_config
start_sway

need_dark=0; need_mid=0
for c in layer-budget inversion inversion-crosshair inversion-crosshair-alpha fixed outline crosshair-geometry; do
	should_run "$c" && need_dark=1
done
should_run inversion-midtone && need_mid=1

if [[ "$need_dark" -eq 1 ]]; then
	start_instance "$BG_DARK_HEX"
	check_layer_budget           # first: reads the startup high-water mark
	set_val "crosshair.enabled" 0  # the config starts it on (see write_config); the checks below want it off
	apply_fps_force
	check_inversion
	check_inversion_crosshair
	check_inversion_crosshair_alpha
	check_fixed
	check_outline
	check_crosshair_geometry
fi

if [[ "$need_mid" -eq 1 ]]; then
	start_instance "$BG_MID_HEX"
	set_val "crosshair.enabled" 0  # as above
	apply_fps_force
	check_inversion_midtone
fi

END_TS=$(date +%s)
RUNTIME_S=$((END_TS - START_TS))

{
	echo "pixel-regression.sh -- $TS"
	echo "runtime: ${RUNTIME_S}s"
	echo
	printf '%-8s %-32s %s\n' "STATUS" "CHECK" "DETAIL"
	for line in "${RESULT_LINES[@]}"; do
		IFS=$'\t' read -r status name detail <<<"$line"
		printf '%-8s %-32s %s\n' "$status" "$name" "$detail"
	done
} | tee "$RESULTS_FILE" >&2

log "captures + results: $OUT_DIR"

if [[ "$FAIL_COUNT" -gt 0 ]]; then
	exit 1
fi
exit 0
