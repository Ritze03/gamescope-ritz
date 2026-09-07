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
#   hud-margin             -- the configured margin lands the outermost
#                            drawn pixel (backdrop rect, exact; ink/outline
#                            otherwise, within 1px of AA fringe) exactly
#                            that far from the screen edge, at all four
#                            corners and two margins, backdrop off and on,
#                            plus a 4-digit reading (2026-09-07 fix; see
#                            build-release/verify-shots/hud-margin-2026-09-07/
#                            for the full matrix this is a compact subset of)
#   crosshair-geometry     -- all four arms are the configured colour at the
#                            expected offsets, background shows in the gap,
#                            and the crosshair's own outline is black
#   crosshair-gap-invariant -- the hole across the centre is
#                            2*(gap-1)+width, and ALWAYS SYMMETRIC -- both
#                            arms of an axis the same distance from the
#                            centre, at every gap (2026-09-08, revised same
#                            day after the first same-day formula's
#                            even-gap bias shipped a visibly lopsided
#                            crosshair -- crosshair.md's "Gap"): checked at
#                            width 1 and 2, gap 0..4, outline off and on,
#                            both the exact hole width and the symmetry
#                            itself
#   crosshair-shrink-rate  -- Shrink auto-hide: the gap closes and the arms
#                            shorten at the SAME pixels-per-second (request
#                            #11, 2026-09-06), measured from captures at known
#                            times after a debug right-click press
#   crosshair-reverse      -- Animate back (request #13): released half way
#                            through a Shrink hide, the crosshair is part way
#                            back a quarter later and fully back after
#   crosshair-scaled       -- Apply Scaling (request #14): a 640x360 game
#                            stretched 2x onto the 1280x720 output gets arms
#                            2x as long, 2x as wide, a hole of
#                            (2*(gap-1)+width) x scale (2026-09-08, revised
#                            same day), and soft (10..90 % coverage) edges --
#                            measured by coverage, i.e. the composite's
#                            straight-alpha blend, not by colour -- plus the
#                            same symmetry assertion as crosshair-gap-invariant
#   crosshair-scaled-gap   -- the same hole-formula invariant at a second,
#                            smaller gap value, distinguishing it from both
#                            the pre-2026-09-08 (hole=2*gap+width) and the
#                            first same-day (hole=gap, biased) formulas
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

# HUD margin fix (2026-09-07, superdoc/features/fps-display.md's "Margin"):
# compact permanent subset of the full verification matrix (build-release/
# verify-shots/hud-margin-2026-09-07/ has the rest) -- all four corners at
# two margins, backdrop off (ink only) and on, plus one 4-digit reading.
# hud.anchor's Composite binding only exposes its VERTICAL axis under a
# console id (Registry.cpp's own "Prefix Law" comment -- the horizontal
# half, m_BindB, has none), so switching CORNER needs a fresh config file;
# margin_x/margin_y and backdrop_opacity are plain Params/Sliders and stay
# live-settable via overlay_e2_set within that corner's own instance.
HUD_MARGIN_CORNERS=(top-left top-right bottom-left bottom-right)
HUD_MARGIN_VALUES=(0 8)
HUD_MARGIN_DIGITS_FPS=1234        # the 4-digit case's own forced reading
HUD_MARGIN_BOX_SPAN=140           # search box span from the corner, generous around the "60"/"1234" glyphs
HUD_MARGIN_TOL_BACKDROP=0         # a crisp AddRectFilled edge -- exact by construction
HUD_MARGIN_TOL_INK=1              # a real, sub-count font AA fringe right at the edge -- see fps-display.md

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
# which is easy to miss from the spec alone).
#
# 2026-09-08, revised same day: the gap is 2*(gap-1)+width, and -- unlike
# the same-day formula this replaced -- BOTH arms of an axis sit at the
# SAME offset from the centre, at every gap value, odd or even (the
# even-gap bias that put the extra pixel on the high side is gone --
# CrosshairMath.h's HoleSplit). So there is only one offset to compute now:
# E = (width+1)/2 [the column/row's own half-width, as an integer pixel
# distance from the centre] + (gap - 1) [the shared per-side inset].
CH_ARM_OFFSET=$(( (CH_LINE_WIDTH + 1) / 2 + (CH_LINE_GAP - 1) ))
# A scan up from the centre (offset E = CH_ARM_OFFSET) reads: outline
# (black) at E-ow..E-1 (the NEAR ring -- it eats into what a naive
# "gap = background" reading would expect); fill (line colour) at
# E+1..E+length-2, a 1px margin off both boundaries. Because the offset no
# longer differs by side, the SAME window now also works for the other
# three directions -- check_crosshair_geometry below reuses it exactly
# (CH_GEO_LINE_GAP, its former "force symmetry" gap value, is no longer
# needed for that reason but is kept at 15 as a different-from-CH_LINE_GAP
# sanity check that the formula holds at more than one gap value).
CH_OUT_LO=$((CH_ARM_OFFSET - CH_OUTLINE_WIDTH)); CH_OUT_HI=$((CH_ARM_OFFSET - 1))
CH_ARM_LO=$((CH_ARM_OFFSET + 1));                CH_ARM_HI=$((CH_ARM_OFFSET + CH_LINE_LENGTH - 2))

# check_crosshair_geometry's own gap, kept distinct from CH_LINE_GAP simply
# to exercise a second value (symmetry no longer depends on which one is
# picked -- see the comment above).
CH_GEO_LINE_GAP=15
CH_GEO_ARM_OFFSET=$(( (CH_LINE_WIDTH + 1) / 2 + (CH_GEO_LINE_GAP - 1) ))
CH_GEO_GAP_LO=1;                                       CH_GEO_GAP_HI=$((CH_GEO_ARM_OFFSET - CH_OUTLINE_WIDTH - 2))
CH_GEO_OUT_LO=$((CH_GEO_ARM_OFFSET - CH_OUTLINE_WIDTH)); CH_GEO_OUT_HI=$((CH_GEO_ARM_OFFSET - 1))
CH_GEO_ARM_LO=$((CH_GEO_ARM_OFFSET + 1));                CH_GEO_ARM_HI=$((CH_GEO_ARM_OFFSET + CH_LINE_LENGTH - 2))
CH_CENTER_X=$((OUT_W / 2))
CH_CENTER_Y=$((OUT_H / 2))

# Gap invariant (crosshair-gap-invariant, 2026-09-08): gap N is exactly N
# pixels missing across the centre -- see crosshair.md's "Gap" -- checked at
# both line widths the task named, every gap 0..3, with the outline off and
# on, via pixel_regression_sample.py's hole_run() (the bidirectional
# non-fill run through the centre point). max_off just needs to clear the
# longest possible hole plus a margin; the line stays short (6px) so the
# scan never confuses the hole with the far end of the opposite arm.
CH_GAPINV_WIDTHS=( 1 2 )
CH_GAPINV_GAPS=( 0 1 2 3 4 )
CH_GAPINV_LENGTH=10
CH_GAPINV_MAX_OFF=12

# Shrink-rate / Animate-back (crosshair-shrink-rate, crosshair-reverse): the
# pixel path at 1:1, width 1 and the outline off so an arm is one exact run of
# CH_LINE_COLOR along its ray (pixel_regression_sample.py gap_len()). Slow
# animations so screenshot latency (~50-150 ms observed) is a fraction of a
# pixel: 20 px of edge travel over 4 s is 5 px/s.
CH_ANIM_LINE_WIDTH=1
CH_SHRINK_HIDE_MS=4000
CH_SHRINK_T1_MS=400;  CH_SHRINK_T2_MS=1200   # phase 1 (gap closes over the first 8/20 = 1600 ms)
CH_SHRINK_T3_MS=2400; CH_SHRINK_T4_MS=3200   # phase 2 (arms shorten)
CH_SHRINK_T5_MS=2000                          # the 50 % capture (taken between T2 and T3)
CH_SHRINK_TOL_PX=1                            # rate difference over the phase-1 interval, and the 50 % state
CH_REVERSE_HIDE_MS=2000
CH_REVERSE_RELEASE_MS=1000                    # release at 50 %
CH_REVERSE_MID_MS=1500                        # capture at 75 %: expect f = 0.25 -> 5 px travelled -> gap 3 of 8
CH_REVERSE_END_MS=2600                        # capture after the reveal has finished
CH_REVERSE_TOL_PX=1.5                         # 1 px plus timing slack at 10 px/s

# Apply Scaling (crosshair-scaled): a 640x360 client stretched 2x onto the
# 1280x720 output (--scaler stretch). Width 1 (CH_ANIM_LINE_WIDTH), outline
# off, length/gap as the startup config (CH_LINE_LENGTH / CH_LINE_GAP) ->
# in GAME pixels the hole is 2*(gap-1)+width = 2*(8-1)+1 = 15 (2026-09-08,
# revised same day), which the raster path scales by the same factor as
# everything else -- in OUTPUT pixels: arms 24 long, 2 wide, inner ends 30
# apart (15 x scale 2). Measured by COVERAGE: each pixel's coverage is read
# back through the composite's straight-alpha blend
# (pixel_regression_sample.py coverage_of()), an arm is the run of >= 50 %,
# the pixel past each end / beside each side must be 10..90 % -- the soft
# edge a bilinear stretch of a 1 px line has (75 % / 25 % rows at 2x) -- and
# the two arms must be equidistant from the centre (cmd_scaled_axis's own
# symmetry assertion, `-l1 == r0`).
CH_SCALED_GAME_W=640
CH_SCALED_GAME_H=360
CH_SCALED_SCALER=stretch
CH_SCALED_SCALE=$((OUT_W / CH_SCALED_GAME_W))   # 2 (integer by construction)
CH_SCALED_EXP_LEN=$((CH_LINE_LENGTH * CH_SCALED_SCALE))
CH_SCALED_EXP_HOLE=$(( 2 * (CH_LINE_GAP - 1) + CH_ANIM_LINE_WIDTH ))
CH_SCALED_EXP_SEP=$((CH_SCALED_EXP_HOLE * CH_SCALED_SCALE))
CH_SCALED_EXP_WIDTH=$((CH_ANIM_LINE_WIDTH * CH_SCALED_SCALE))
CH_SCALED_TOL_PX=1
CH_SCALED_SPAN=120
# A second, smaller gap re-run of the same check (crosshair-scaled-gap) to
# pin the formula at a value where the OLD (pre-2026-09-08, and the first
# same-day attempt's biased) formulas would each have given a visibly
# different number: gap 2, width 1 -> hole = 2*(2-1)+1 = 3, sep = 6 (not
# 2*2=4 as a bare "hole=gap" reading would give, nor an even/odd-biased 2
# or 4 either).
CH_SCALED_GAP2=2
CH_SCALED_GAP2_EXP_HOLE=$(( 2 * (CH_SCALED_GAP2 - 1) + CH_ANIM_LINE_WIDTH ))
CH_SCALED_GAP2_EXP_SEP=$((CH_SCALED_GAP2_EXP_HOLE * CH_SCALED_SCALE))
# At 2x the game's centre pixel column [320,321) lands on output [640,642): the
# crosshair is centred on x = 641.0, so scan from pixel 641 / 361.
CH_SCALED_CENTER_X=$((OUT_W / 2 + 1))
CH_SCALED_CENTER_Y=$((OUT_H / 2 + 1))

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

# check_hud_margin()'s own config: same shape as write_config() above, minus
# the crosshair (off, irrelevant to the margin fix) and with anchor/
# margin_x/margin_y parameterised -- switching CORNER needs a fresh config
# file (see HUD_MARGIN_* comment above), so this is called once per corner
# rather than once for the whole script. Overwrites the SAME profile file
# write_config() does; check_hud_margin() calls write_config() again at its
# own end so every check that runs after it still sees the standard config.
write_config_hud_margin() {
	local anchor="$1" mx="$2" my="$3"
	mkdir -p "$CONFIGHOME/gamescope-ritz/profiles"
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
		        "color_mode": "fixed",
		        "outline_strength": 0.0,
		        "lag_detection_enabled": false,
		        "color_fps": $((FIXED_COLOR_HEX)),
		        "anchor": "$anchor",
		        "margin_x": $mx,
		        "margin_y": $my
		    },
		    "crosshair": { "enabled": false }
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
	# Optional: the client's own size and the scaler (crosshair-scaled runs
	# a 640x360 client stretched onto the 1280x720 output); default 1:1.
	local game_w="${2:-$OUT_W}" game_h="${3:-$OUT_H}" scaler="${4:-auto}"
	teardown_instance   # only one instance (one xterm background) at a time

	GS_LOG="$RUNDIR/gamescope-$(date +%s%N).log"
	log "starting gamescope instance, background $bg_hex, client ${game_w}x${game_h}, scaler $scaler"
	WAYLAND_DISPLAY="$SWAY_WL_NAME" XDG_RUNTIME_DIR="$RUNDIR" XDG_CONFIG_HOME="$CONFIGHOME" \
		"$GAMESCOPE_BIN" --backend wayland -w "$game_w" -h "$game_h" -W "$OUT_W" -H "$OUT_H" \
		--scaler "$scaler" --force-windows-fullscreen -- \
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

# HUD margin fix (2026-09-07): the configured margin is the distance from
# the screen edge to the OUTERMOST drawn pixel -- exact (tol 0) when the
# backdrop is drawn, within a 1px font-AA fringe (tol 1) otherwise. See the
# HUD_MARGIN_* constants' own comment for why this restarts the instance
# once per corner, and build-release/verify-shots/hud-margin-2026-09-07/
# for the full matrix this is a compact, permanent subset of. Manages its
# own instance restarts (not the shared "dark" instance the checks above
# use), and restores the standard config at its own end.
#
# Uses BG_MID, not BG_DARK: the near-black backdrop (9,11,14) at opacity
# 0.5 blended over BG_DARK (51,51,51) measures too close to BG_DARK itself
# (empirically under DIFF_THRESH), which silently degenerated the
# backdrop-on cases into re-measuring the ink underneath. Over BG_MID
# (148,148,148) the same blend measures ~(108,108,108) -- a safely
# separated ~40.
check_hud_margin() {
	should_run hud-margin || { skip_check hud-margin "--only excluded it"; return; }

	local anchor mval edges bx0 by0 bx1 by1 shot
	for anchor in "${HUD_MARGIN_CORNERS[@]}"; do
		case "$anchor" in
			top-left)     edges="left,top";     bx0=0; by0=0 ;;
			top-right)    edges="right,top";    bx0=$((OUT_W - HUD_MARGIN_BOX_SPAN)); by0=0 ;;
			bottom-left)  edges="left,bottom";  bx0=0; by0=$((OUT_H - HUD_MARGIN_BOX_SPAN)) ;;
			bottom-right) edges="right,bottom"; bx0=$((OUT_W - HUD_MARGIN_BOX_SPAN)); by0=$((OUT_H - HUD_MARGIN_BOX_SPAN)) ;;
		esac
		bx1=$((bx0 + HUD_MARGIN_BOX_SPAN)); by1=$((by0 + HUD_MARGIN_BOX_SPAN))

		for mval in "${HUD_MARGIN_VALUES[@]}"; do
			write_config_hud_margin "$anchor" "$mval" "$mval"
			start_instance "$BG_MID_HEX"
			apply_fps_force

			shot="$(take_screenshot "20-hud-margin-${anchor}-m${mval}-ink")"
			run_sampler margin "$shot" "$bx0" "$by0" "$bx1" "$by1" \
				"$BG_MID_R" "$BG_MID_G" "$BG_MID_B" "$DIFF_THRESH" "$edges" \
				"$mval" "$mval" "$HUD_MARGIN_TOL_INK" "hud-margin-${anchor}-m${mval}-ink"

			set_val "hud.backdrop_opacity" 0.5
			shot="$(take_screenshot "21-hud-margin-${anchor}-m${mval}-bd")"
			run_sampler margin "$shot" "$bx0" "$by0" "$bx1" "$by1" \
				"$BG_MID_R" "$BG_MID_G" "$BG_MID_B" "$DIFF_THRESH" "$edges" \
				"$mval" "$mval" "$HUD_MARGIN_TOL_BACKDROP" "hud-margin-${anchor}-m${mval}-bd"
			set_val "hud.backdrop_opacity" 0
		done
	done

	# One 4-digit reading (top-left, this script's own default margin): the
	# digit-count/pinned-width interaction the 2026-09-06 alignment fix and
	# this margin fix both touch.
	write_config_hud_margin "top-left" "$MARGIN_X" "$MARGIN_Y"
	start_instance "$BG_MID_HEX"
	gsctl fps_display_force "$HUD_MARGIN_DIGITS_FPS" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	shot="$(take_screenshot "22-hud-margin-digits4")"
	run_sampler margin "$shot" 0 0 "$HUD_MARGIN_BOX_SPAN" "$HUD_MARGIN_BOX_SPAN" \
		"$BG_MID_R" "$BG_MID_G" "$BG_MID_B" "$DIFF_THRESH" "left,top" \
		"$MARGIN_X" "$MARGIN_Y" "$HUD_MARGIN_TOL_INK" "hud-margin-digits4"

	# Every check above restarted the instance with its own config -- put
	# the standard one back so anything run after this in the same
	# invocation sees what it expects.
	write_config
}

# Crosshair geometry: all four arms are the configured colour at the expected
# offset from the game-rect centre, background in the gap, black outline just
# past each arm's far end. apply_scaling is off (pixel path, exact) -- see
# superdoc/features/crosshair.md's Geometry section for the offsets' derivation.
check_crosshair_geometry() {
	should_run crosshair-geometry || { skip_check crosshair-geometry "--only excluded it"; return; }
	set_val "crosshair.enabled" 1
	# CH_GEO_LINE_GAP, not CH_LINE_GAP -- see that constant's comment: it
	# keeps this check's four directions symmetric under the 2026-09-08
	# total-hole gap semantics without disturbing CH_LINE_GAP, which the
	# shrink-rate/reverse checks below also depend on.
	set_val "crosshair.line_gap" "$CH_GEO_LINE_GAP"
	local shot; shot="$(take_screenshot 07-crosshair-geometry)"
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))

	local dir dx dy
	for dir in up:0:-1 down:0:1 left:-1:0 right:1:0; do
		IFS=':' read -r dirname dx dy <<<"$dir"
		run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" "$dx" "$dy" "$CH_GEO_GAP_LO" "$CH_GEO_GAP_HI" all \
			"$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" "$DIFF_THRESH" "crosshair-gap-$dirname"
		run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" "$dx" "$dy" "$CH_GEO_ARM_LO" "$CH_GEO_ARM_HI" all \
			"$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "crosshair-arm-$dirname"
		run_sampler line "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" "$dx" "$dy" "$CH_GEO_OUT_LO" "$CH_GEO_OUT_HI" all \
			"$CH_OUTLINE_COLOR_R" "$CH_OUTLINE_COLOR_G" "$CH_OUTLINE_COLOR_B" "$CH_COLOR_TOL" \
			"crosshair-outline-$dirname"
	done
	set_val "crosshair.line_gap" "$CH_LINE_GAP"
	set_val "crosshair.enabled" 0
}

# Gap invariant (2026-09-08, revised same day: "basically like the old
# formula, just with the gap with 1 deducted", after the first same-day
# attempt's even-gap bias shipped a visibly lopsided crosshair -- see
# crosshair.md's "Gap"): the hole across the centre is 2*(gap-1)+width, and
# it is ALWAYS SYMMETRIC -- both arms of an axis sit the same distance from
# the centre, at every gap value, odd or even. Checked at width 1 and width
# 2, gap 0..4, outline off and on, on both axes: the exact hole width via
# pixel_regression_sample.py's hole_run() (the bidirectional non-fill run
# through the centre point -- background OR outline both count as
# "missing", since neither is the line's own fill), AND the symmetry
# itself via its new `symmetry` command (the near end of each of the two
# arms on an axis, measured from the crossing's own two edge pixels, must
# be the same distance out).
check_crosshair_gap_invariant() {
	should_run crosshair-gap-invariant || { skip_check crosshair-gap-invariant "--only excluded it"; return; }
	set_val "crosshair.enabled" 1
	set_val "crosshair.line_length" "$CH_GAPINV_LENGTH"
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))

	local width gap outline
	for width in "${CH_GAPINV_WIDTHS[@]}"; do
		set_val "crosshair.line_width" "$width"
		# The crossing's own low/high edge PIXELS on this width -- e.g.
		# width 1: both edges are the crossing's one and only pixel
		# (CH_CENTER_X itself); width 2: the crossing straddles the centre,
		# so its two edges are CH_CENTER_X-1 and CH_CENTER_X. Each is
		# independently symmetric about the TRUE (possibly half-pixel, for
		# an odd width) centre -- see cmd_symmetry's own docstring and
		# CrosshairMath.h's detail::SnapCenter -- which a single shared
		# integer anchor is not, for an even width.
		local half=$(( width / 2 ))
		local lo_x=$(( CH_CENTER_X - half )); local hi_x=$(( lo_x + width - 1 ))
		local lo_y=$(( CH_CENTER_Y - half )); local hi_y=$(( lo_y + width - 1 ))
		for outline in 0 1; do
			set_val "crosshair.outline" "$outline"
			for gap in "${CH_GAPINV_GAPS[@]}"; do
				set_val "crosshair.line_gap" "$gap"
				local expect_hole=0
				if (( gap >= 1 )); then expect_hole=$(( 2 * (gap - 1) + width )); fi
				local shot; shot="$(take_screenshot "12-gap-invariant-w${width}-g${gap}-o${outline}")"
				run_sampler hole "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" 1 0 \
					"$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "$CH_GAPINV_MAX_OFF" "$expect_hole" \
					"crosshair-gap-inv-x-w${width}-g${gap}-o${outline}"
				run_sampler hole "$shot" "$CH_CENTER_X" "$CH_CENTER_Y" 0 1 \
					"$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "$CH_GAPINV_MAX_OFF" "$expect_hole" \
					"crosshair-gap-inv-y-w${width}-g${gap}-o${outline}"
				run_sampler symmetry "$shot" "$lo_x" "$CH_CENTER_Y" "$hi_x" "$CH_CENTER_Y" 1 0 \
					"$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "$CH_GAPINV_MAX_OFF" \
					"crosshair-gap-sym-x-w${width}-g${gap}-o${outline}"
				run_sampler symmetry "$shot" "$CH_CENTER_X" "$lo_y" "$CH_CENTER_X" "$hi_y" 0 1 \
					"$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "$CH_GAPINV_MAX_OFF" \
					"crosshair-gap-sym-y-w${width}-g${gap}-o${outline}"
			done
		done
	done

	set_val "crosshair.line_gap" "$CH_LINE_GAP"
	set_val "crosshair.line_width" "$CH_LINE_WIDTH"
	set_val "crosshair.line_length" "$CH_LINE_LENGTH"
	set_val "crosshair.outline" 1
	set_val "crosshair.enabled" 0
}

# Milliseconds since the epoch, for the timed captures below.
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# Sleeps until `since_ms + target_ms`, if that is still in the future.
sleep_until() {
	local since_ms="$1" target_ms="$2" now
	now="$(now_ms)"
	local wait_ms=$(( since_ms + target_ms - now ))
	if (( wait_ms > 0 )); then
		sleep "$(awk "BEGIN { printf \"%.3f\", $wait_ms / 1000 }")"
	fi
}

# Requests a screenshot and records the request time (ms since the press
# given in $2) into the variable named by $3 and the path into TS_PATH --
# called directly, never in a $(...) (a subshell would drop both). The
# capture itself lands on the next paint, and the animation forces one per
# frame, so the request time is the capture time to within a frame.
TS_PATH=""
timed_shot() {
	local name="$1" since_ms="$2" var="$3"
	local path="$OUT_DIR/$name.png"
	local t=$(( $(now_ms) - since_ms ))
	gsctl screenshot "$path 4" >/dev/null 2>&1 || true
	printf -v "$var" '%s' "$t"
	TS_PATH="$path"
	# Wait for the file to stabilise (as take_screenshot), so the next
	# request does not race this one's write.
	local waited=0 last_size=-1 size=0
	while (( waited < SCREENSHOT_TIMEOUT_S * 20 )); do
		if [[ -f "$path" ]]; then
			size="$(stat -c%s "$path" 2>/dev/null || echo 0)"
			if [[ "$size" -gt 0 && "$size" == "$last_size" ]]; then break; fi
			last_size="$size"
		fi
		sleep 0.05
		waited=$((waited + 1))
	done
}

# Puts the crosshair into the animation checks' configuration: pixel path,
# 1 px line, outline off, Shrink, a given hide time and Animate back state.
crosshair_anim_setup() {
	local hide_ms="$1" animate_back="$2"
	set_val "crosshair.enabled" 1
	set_val "crosshair.outline" 0
	set_val "crosshair.line_width" "$CH_ANIM_LINE_WIDTH"
	set_val "crosshair.hide" 1
	set_val "crosshair.hide_mode" 2          # kHideModeOptions: 0 fade, 1 focus, 2 shrink
	set_val "crosshair.hide_time" "$hide_ms"
	set_val "crosshair.hide_animate_back" "$animate_back"
}

crosshair_anim_teardown() {
	gsctl wlserver_debug_mouse_button "273 0" >/dev/null 2>&1 || true
	set_val "crosshair.hide" 0
	set_val "crosshair.hide_animate_back" 1
	set_val "crosshair.line_width" "$CH_LINE_WIDTH"
	set_val "crosshair.outline" 1
	set_val "crosshair.enabled" 0
}

# Shrink rate (request #11): press, capture twice while the gap closes and
# twice while the arms shorten, plus once at 50 %; the sampler compares the
# two edge speeds and the 50 % state against crosshair::ShrinkSplit's model.
check_crosshair_shrink_rate() {
	should_run crosshair-shrink-rate || { skip_check crosshair-shrink-rate "--only excluded it"; return; }
	crosshair_anim_setup "$CH_SHRINK_HIDE_MS" 0
	local t0 s1 s2 s3 s4 s5 t1 t2 t3 t4 t5
	t0="$(now_ms)"
	gsctl wlserver_debug_mouse_button "273 1" >/dev/null 2>&1 || true
	sleep_until "$t0" "$CH_SHRINK_T1_MS"; timed_shot 09-shrink-t1 "$t0" t1; s1="$TS_PATH"
	sleep_until "$t0" "$CH_SHRINK_T2_MS"; timed_shot 09-shrink-t2 "$t0" t2; s2="$TS_PATH"
	sleep_until "$t0" "$CH_SHRINK_T5_MS"; timed_shot 09-shrink-t5-mid "$t0" t5; s5="$TS_PATH"
	sleep_until "$t0" "$CH_SHRINK_T3_MS"; timed_shot 09-shrink-t3 "$t0" t3; s3="$TS_PATH"
	sleep_until "$t0" "$CH_SHRINK_T4_MS"; timed_shot 09-shrink-t4 "$t0" t4; s4="$TS_PATH"
	gsctl wlserver_debug_mouse_button "273 0" >/dev/null 2>&1 || true
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))
	run_sampler shrink_rate "$s1" "$t1" "$s2" "$t2" "$s3" "$t3" "$s4" "$t4" "$s5" "$t5" \
		"$CH_CENTER_X" "$CH_CENTER_Y" "$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" \
		"$CH_SHRINK_HIDE_MS" "$CH_LINE_GAP" "$CH_LINE_LENGTH" "$CH_SHRINK_TOL_PX" "crosshair-shrink-rate"
	crosshair_anim_teardown
}

# Animate back (request #13): press, release at 50 %, capture at 75 % (part
# way back: gap 3 of 8 on the model) and after the reveal (fully back).
check_crosshair_reverse() {
	should_run crosshair-reverse || { skip_check crosshair-reverse "--only excluded it"; return; }
	crosshair_anim_setup "$CH_REVERSE_HIDE_MS" 1
	local t0 s_mid s_end t_rel t_mid t_end
	t0="$(now_ms)"
	gsctl wlserver_debug_mouse_button "273 1" >/dev/null 2>&1 || true
	sleep_until "$t0" "$CH_REVERSE_RELEASE_MS"
	t_rel=$(( $(now_ms) - t0 ))
	gsctl wlserver_debug_mouse_button "273 0" >/dev/null 2>&1 || true
	sleep_until "$t0" "$CH_REVERSE_MID_MS"; timed_shot 10-reverse-mid "$t0" t_mid; s_mid="$TS_PATH"
	sleep_until "$t0" "$CH_REVERSE_END_MS"; timed_shot 10-reverse-end "$t0" t_end; s_end="$TS_PATH"
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))
	run_sampler reverse "$s_mid" "$s_end" "$t_rel" "$t_mid" "$t_end" \
		"$CH_CENTER_X" "$CH_CENTER_Y" "$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" \
		"$CH_REVERSE_HIDE_MS" "$CH_LINE_GAP" "$CH_LINE_LENGTH" "$CH_REVERSE_TOL_PX" "crosshair-reverse"
	crosshair_anim_teardown
}

# Apply Scaling (request #14): needs the stretched instance -- see the run
# section. Row and column through the centre, each must show two arms of
# the stretched length, separation and width, with soft edges.
check_crosshair_scaled() {
	should_run crosshair-scaled || { skip_check crosshair-scaled "--only excluded it"; return; }
	set_val "crosshair.enabled" 1
	set_val "crosshair.outline" 0
	set_val "crosshair.line_width" "$CH_ANIM_LINE_WIDTH"
	set_val "crosshair.apply_scaling" 1
	local shot; shot="$(take_screenshot 11-crosshair-scaled)"
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))
	run_sampler scaled_axis "$shot" "$CH_SCALED_CENTER_X" "$CH_SCALED_CENTER_Y" 1 0 "$CH_SCALED_SPAN" \
		"$ch_r" "$ch_g" "$ch_b" "$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" \
		"$CH_SCALED_EXP_LEN" "$CH_SCALED_EXP_SEP" "$CH_SCALED_EXP_WIDTH" "$CH_SCALED_TOL_PX" "crosshair-scaled-row"
	run_sampler scaled_axis "$shot" "$CH_SCALED_CENTER_X" "$CH_SCALED_CENTER_Y" 0 1 "$CH_SCALED_SPAN" \
		"$ch_r" "$ch_g" "$ch_b" "$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" \
		"$CH_SCALED_EXP_LEN" "$CH_SCALED_EXP_SEP" "$CH_SCALED_EXP_WIDTH" "$CH_SCALED_TOL_PX" "crosshair-scaled-column"
	# Apply Scaling OFF on the same stretched instance: the pixel path draws
	# in output pixels, so the arm is the UNstretched length again, exact.
	set_val "crosshair.apply_scaling" 0
	local shot_off; shot_off="$(take_screenshot 11-crosshair-unscaled)"
	run_sampler armscan "$shot_off" "$CH_CENTER_X" "$CH_CENTER_Y" 1 0 "$ch_r" "$ch_g" "$ch_b" "$CH_COLOR_TOL" "crosshair-scaled-off-armscan"
	set_val "crosshair.line_width" "$CH_LINE_WIDTH"
	set_val "crosshair.outline" 1
	set_val "crosshair.enabled" 0
}

# Apply Scaling's hole at a SECOND gap value (2026-09-08): pins "the hole
# should be gap x scale" at a number the OLD (2*gap+width) formula would
# have gotten visibly wrong (gap 2 -> sep 4, not 2*2+2=6) -- see
# CH_SCALED_GAP2's comment. Same stretched instance as check_crosshair_scaled.
check_crosshair_scaled_gap() {
	should_run crosshair-scaled-gap || { skip_check crosshair-scaled-gap "--only excluded it"; return; }
	set_val "crosshair.enabled" 1
	set_val "crosshair.outline" 0
	set_val "crosshair.line_width" "$CH_ANIM_LINE_WIDTH"
	set_val "crosshair.line_gap" "$CH_SCALED_GAP2"
	set_val "crosshair.apply_scaling" 1
	local shot; shot="$(take_screenshot 13-crosshair-scaled-gap2)"
	local ch_r ch_g ch_b
	ch_r=$(( (CH_LINE_COLOR_HEX >> 16) & 0xFF )); ch_g=$(( (CH_LINE_COLOR_HEX >> 8) & 0xFF )); ch_b=$(( CH_LINE_COLOR_HEX & 0xFF ))
	run_sampler scaled_axis "$shot" "$CH_SCALED_CENTER_X" "$CH_SCALED_CENTER_Y" 1 0 "$CH_SCALED_SPAN" \
		"$ch_r" "$ch_g" "$ch_b" "$BG_DARK_R" "$BG_DARK_G" "$BG_DARK_B" \
		"$CH_SCALED_EXP_LEN" "$CH_SCALED_GAP2_EXP_SEP" "$CH_SCALED_EXP_WIDTH" "$CH_SCALED_TOL_PX" "crosshair-scaled-gap2-row"
	set_val "crosshair.apply_scaling" 0
	set_val "crosshair.line_gap" "$CH_LINE_GAP"
	set_val "crosshair.line_width" "$CH_LINE_WIDTH"
	set_val "crosshair.outline" 1
	set_val "crosshair.enabled" 0
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
START_TS=$(date +%s)

write_config
start_sway

need_dark=0; need_mid=0; need_scaled=0
for c in layer-budget inversion inversion-crosshair inversion-crosshair-alpha fixed outline crosshair-geometry crosshair-gap-invariant crosshair-shrink-rate crosshair-reverse; do
	should_run "$c" && need_dark=1
done
should_run inversion-midtone && need_mid=1
for c in crosshair-scaled crosshair-scaled-gap; do
	should_run "$c" && need_scaled=1
done

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
	check_crosshair_gap_invariant
	check_crosshair_shrink_rate
	check_crosshair_reverse
fi

if [[ "$need_mid" -eq 1 ]]; then
	start_instance "$BG_MID_HEX"
	set_val "crosshair.enabled" 0  # as above
	apply_fps_force
	check_inversion_midtone
fi

if [[ "$need_scaled" -eq 1 ]]; then
	start_instance "$BG_DARK_HEX" "$CH_SCALED_GAME_W" "$CH_SCALED_GAME_H" "$CH_SCALED_SCALER"
	set_val "crosshair.enabled" 0  # as above
	check_crosshair_scaled
	check_crosshair_scaled_gap
fi

# Self-contained: guards itself on should_run before starting anything, and
# manages its own instance restarts (one per corner) rather than the shared
# "dark" instance above -- see its own comment.
check_hud_margin

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
