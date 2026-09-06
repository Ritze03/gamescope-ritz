#!/usr/bin/env bash
# pointer-regression.sh -- one-command headless regression gate for three
# pointer rules: "a LOCKED pointer never receives an absolute motion event",
# "the absolute-pointer re-sync fires exactly once per real mapping change",
# and -- the one that actually fixed CS2 -- "every movement carries relative
# motion, so Xwayland's master pointer never changes device under a game".
#
# WHY THIS EXISTS
#   2026-09-06, CS2: mouse look in play "behaved like a joystick, constantly
#   moving back to centre" while the menus were fine. A game in play holds a
#   zwp_locked_pointer_v1 (Xwayland asks for it when the game hides its
#   cursor and warps it) and reads relative motion only. Any wl_pointer.motion
#   that reaches it anyway is republished by Xwayland as an XI2 raw event, and
#   SDL's relative mode reads the raw valuators as a delta -- so one absolute
#   event becomes a jump the size of the pointer position (measured here:
#   xrel=640 yrel=360 for a sample at the output centre). The fork's own
#   re-sync of the absolute pointer after a mapping change
#   (wlserver_resync_absolute_pointer(), 2026-09-05) was such an emitter, and
#   so were upstream's warps (a host absolute sample before the host itself
#   went relative, a focus-change warp to centre). wlserver_mousewarp() now
#   refuses all of them while locked. This script keeps it that way. See
#   superdoc/features/cursor-pipeline.md, "Locked pointer => never an
#   absolute event".
#
#   2026-09-06, later the same day: "Still broken. Same behavior." The gate
#   above passed because its client locked before it ever saw a menu. CS2's
#   order is menu first: absolute pointer, THEN relative mode. Xwayland feeds
#   absolute motion through one X slave device (xwayland-pointer, absolute
#   axes) and relative motion through another (xwayland-relative-pointer),
#   and the master "Virtual core pointer" copies the axis classes of
#   whichever slave posted last. SDL3 (CS2) caches the master's classes at
#   the first raw event it sees and never refreshes them, so a menu driven
#   by absolute-only events makes it read the match's relative deltas as
#   absolute positions and difference them: one jump the size of the last
#   menu position, nothing while moving steadily, a kick back on every
#   change of direction. That is the joystick, and it survived the warp
#   gate because no absolute event was involved any more. Every real
#   compositor sends zwp_relative_pointer_v1 motion with every movement --
#   Xwayland then routes both channels through the RELATIVE slave (the
#   absolute half with no raw event), and the master never changes device.
#   Upstream gamescope did the same in wlserver_mousemotion(); this fork's
#   32b98bb (2026-08-28) had narrowed it to "only while locked", and the
#   host-sample warp path never had it. Both now send it. The x11-* checks
#   below replay CS2's order with a native SDL3 client and, with a virtual
#   host mouse plugged into the private sway, through the nested backend's
#   real host path with force-grab off and on. See cursor-pipeline.md, "The
#   device SDL3 remembers".
#
#   2026-09-08, Rust: "Changing the Resolution breaks the mouse positioning."
#   Rust is a Unity game under Proton, so the client is Wine. Measured with
#   tests/pointer_probe_win32.c under the system wine: a runtime nested-mode
#   change resizes the Xwayland root, gamescope resizes the fullscreen game
#   window to it, and Wine puts the window straight back to its own size and
#   drops _NET_WM_STATE_FULLSCREEN. When the new mode is SMALLER than that
#   window, X confines the pointer to the root while gamescope keeps mapping
#   the whole output onto the whole window: a host sample at 95% of the
#   output reached the game at the clamped root corner (959,539 instead of
#   the 912,513 gamescope had mapped). The fix keeps a focused game window
#   within the screen -- a window heading for a size larger than the root
#   is resized to the root, as a fullscreen one is -- and Wine accepts that
#   second resize. The oversized-window and wine-* checks below pin it,
#   plus "no flapping": the first version of the rule judged the window's
#   geometry, and the size-hints branch grew it back every frame. See
#   cursor-pipeline.md, "A window larger than the screen".
#
# HOW IT RUNS WITHOUT TOUCHING THE USER'S DESKTOP
#   Same recipe as pixel-regression.sh (read that script's header for the
#   full reasoning): a private, invisible sway (WLR_BACKENDS=headless, its own
#   XDG_RUNTIME_DIR, no input devices) hosts a real nested
#   `gamescope --backend wayland`; state is driven over gamescope's own
#   control socket with `gamescopectl` ConCommands, never OS input. Nothing
#   is ever visible on the real desktop and no other gamescope on the machine
#   can address this instance.
#
# THE TEST CLIENTS
#   build-release/tests/pointer_lock_client (tests/pointer_lock_client.c): a
#   real SDL2 window inside gamescope's Xwayland. With --lock it calls
#   SDL_SetRelativeMouseMode(SDL_TRUE) -- exactly what a first-person game
#   does in play -- which makes Xwayland request the LOCKED constraint from
#   gamescope. It prints one MOTION line per SDL_MOUSEMOTION it receives, so
#   the count of what "the game" saw is read straight off its log.
#
#   build-release/tests/pointer_grab_client_x11 (tests/pointer_grab_client_x11.c):
#   the CS2-shaped one -- native SDL3, SDL_SetWindowRelativeMouseMode, with
#   --lock-after S so it is a plain menu first, and an XI2 tap that prints
#   every raw event with its device (RAW dev= src=) and the master's axis
#   mode as SDL3 saw it (DEVICE ... axis0=rel|abs). Needs SDL3 at build time;
#   the x11-* checks are SKIPped without it.
#
#   build-release/tests/pointer_probe_win32.exe (tests/pointer_probe_win32.c):
#   the Rust-shaped one -- a Win32 program run with the system wine inside
#   gamescope's Xwayland: a screen-sized popup (Wine marks it fullscreen and
#   clips the cursor to it), Unity's Locked mode with --lock-after S
#   (hidden cursor + ClipCursor + SetCursorPos every frame, WM_INPUT raw
#   deltas) and --unlock-after S to leave it. Prints MOTION (client and
#   screen coordinates), RAW dx/dy, LOCKED/UNLOCKED. Needs the mingw cross
#   compiler at build time and wine at run time; the wine-* checks are
#   SKIPped without them. A private WINEPREFIX is created per run.
#
#   build-release/tests/virtual_pointer_tool (tests/virtual_pointer_tool.c):
#   a zwlr_virtual_pointer_v1 client that plugs a host mouse into the
#   private sway (which has no input devices) so the x11-host check drives
#   the nested backend's real host path. Kept alive for the whole run: sway
#   drops the seat's pointer the moment the last virtual pointer goes.
#
# WHAT IS DRIVEN
#   wlserver_debug_mouse_motion     relative host motion (the grabbed path)
#   wlserver_debug_absolute_motion  an absolute host sample (force grab off)
#   overlay_e2_set display.filter.scaler <n>   the scaler (4 = Stretch) --
#                                   a mapping change with no window resize
#   steamcompmgr_debug_set_nested_mode "<w> <h> 0"   a runtime resolution
#                                   change, the item-10 path
#   wlserver_pointer_stats          the counters: motions / motions_locked /
#                                   relatives / resyncs /
#                                   warps_suppressed_locked, and the
#                                   constraint state
#   virtual_pointer_tool            host absolute samples and host relative
#                                   motion through the private sway
#
# CHECKS (each is a check_* function; assertions are the named constants)
#   locked-relative   relative motion still reaches the locked client
#                     (sanity: the lock is real and the relative path alive)
#   locked-absolute   absolute host samples while locked: client sees +0
#                     motion, motions_locked stays 0
#   locked-mapping    scaler toggle and nested-mode change while locked:
#                     resyncs +0, motions_locked 0, client +0
#   unlocked-resync   with no constraint: one absolute sample, then a scaler
#                     toggle -> resyncs +1 and client +1; the same value
#                     again -> +0; 2 s idle -> +0; a nested-mode change -> +1
#   x11-menu-lock     CS2's order, SDL3 client: absolute samples while a
#                     menu, then relative mode, then 10 relative injections
#                     -> the master pointer SDL3 cached is relative, all 10
#                     arrive as xrel=5, relatives >= motions
#   x11-locked-abs    the SDL3 client, locked: 6 absolute samples -> +0
#   x11-host          the same order through the nested backend's real host
#                     path: a virtual host mouse moves absolutely (force-grab
#                     off), the client locks, the host confirms the lock and
#                     sends relative motion -> 10 x xrel=5; force-grab on ->
#                     5 more x xrel=5
#
#   oversized-window  the SDL3 client as a 1280x720 window with size hints,
#                     then nested mode 960x540 (smaller than the window):
#                     the window is brought to 960x540 exactly once (no
#                     flapping over 2 s), and a host sample at (0.95, 0.95)
#                     reaches the client at the position gamescope mapped,
#                     912,513 -- not the root-clamped 959,539
#   wine-mode         the Wine probe as a menu (screen-sized, cursor shown):
#                     the same mode change -> the window converges to the
#                     root (mapping 0.75), the sample reaches the game at
#                     the mapped 912,513
#   wine-lock-mode    the Wine probe locked (Unity's Locked): 10 relative
#                     injections before and after the mode change arrive as
#                     RAW dx=5, motions_locked 0; then it unlocks, the first
#                     relative nudge lets Wine define a cursor and Xwayland
#                     drop the lock, and an absolute sample lands at 912,513
#
# USAGE
#   scripts/pointer-regression.sh                     # every check (~90s)
#   scripts/pointer-regression.sh --only locked-absolute
#   scripts/pointer-regression.sh --only x11-host
#   scripts/pointer-regression.sh --keep              # leave the last instance up
#   scripts/pointer-regression.sh --gamescope <bin>   # another binary (e.g. the
#                                                     # pre-fix one, for a baseline;
#                                                     # compositor-side counters
#                                                     # are SKIPped if it lacks
#                                                     # wlserver_pointer_stats)
#
# Runs the whole session under with-gamescope-lock.sh (self-re-execs under it
# on first invocation) -- see that script's header for why.
set -euo pipefail

if [[ -z "${POINTER_REGRESSION_LOCKED:-}" ]]; then
	export POINTER_REGRESSION_LOCKED=1
	SCRIPT_DIR_BOOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	exec "$SCRIPT_DIR_BOOT/with-gamescope-lock.sh" "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GAMESCOPE_BIN="$REPO_ROOT/build-release/src/gamescope"
GAMESCOPECTL_BIN="$REPO_ROOT/build-release/src/gamescopectl"
CLIENT_BIN="$REPO_ROOT/build-release/tests/pointer_lock_client"
CLIENT_X11_BIN="$REPO_ROOT/build-release/tests/pointer_grab_client_x11"
VP_TOOL_BIN="$REPO_ROOT/build-release/tests/virtual_pointer_tool"
PROBE_WIN32_EXE="$REPO_ROOT/build-release/tests/pointer_probe_win32.exe"

# ---------------------------------------------------------------------------
# Named constants
# ---------------------------------------------------------------------------
OUT_W=1280
OUT_H=720
REL_DX=5
REL_COUNT=10                 # relative injections; the locked client must see all of them
ABS_SAMPLES=6                # absolute injections while locked; the client must see none
MENU_SAMPLES=4               # absolute samples delivered to a menu before it locks (x11-*)
LOCK_AFTER_S=3               # how long the SDL3 client stays a menu before locking
HOST_REL_DX=5                # host relative motion per step (x11-host), force-grab off
HOST_FG_COUNT=5              # host relative steps after force-grab on
SCALER_AUTO=0                # GamescopeUpscaleScaler::AUTO
SCALER_STRETCH=4             # GamescopeUpscaleScaler::STRETCH
# Nested mode changes (refresh 0 = follow host). MODE_A has a different
# aspect from the 640x480 client on purpose: under Auto a nested mode with
# the window's own aspect gives the same min(out/src) ratio, so the mapping
# would not move and the check would prove nothing (1280x1024 -> 1.406,
# 1280x720 -> 1.5).
MODE_A="1280 1024 0"
MODE_B="1280 720 0"
# A mode SMALLER than a 1280x720 window (oversized-window, wine-*): the
# window must be brought within it, and a sample at OVERSIZED_ABS maps to
# (0.95*1280, 0.95*720) * (960/1280) = (912, 513) once it is. Before the
# fix the client got X's root-clamped (959, 539).
MODE_SMALL="960 540 0"
OVERSIZED_ABS="0.95 0.95"
OVERSIZED_EXPECT="912,513"
WINE_LOCK_AFTER_S=3          # the Wine probe stays a menu this long, then locks
WINE_UNLOCK_AFTER_S=14       # ...and leaves the lock this long after start
UNLOCK_TIMEOUT_S=20          # how long wine-lock-mode waits for that unlock
# The absolute sample the unlocked check re-syncs from. Off-centre on
# purpose: the output centre maps to the window centre under every scaler,
# so a re-sync from it would move nothing and be (correctly) skipped.
ABS_SAMPLE="0.25 0.25"
SETTLE_S=1.0                 # a mapping change needs a painted frame to reach update_touch_scaling()
IDLE_S=2.0                   # steady-state window: resyncs must not advance
READY_TIMEOUT_S=20
SWAY_READY_TIMEOUT_S=10
LOCK_TIMEOUT_S=8

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
KEEP=0
ONLY=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--keep) KEEP=1; shift ;;
		--only) ONLY="$2"; shift 2 ;;
		--gamescope) GAMESCOPE_BIN="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,70p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*) echo "pointer-regression: unknown argument: $1" >&2; exit 2 ;;
	esac
done

should_run() {
	[[ -z "$ONLY" || "$ONLY" == "$1" ]]
}

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------
TS="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$REPO_ROOT/build-release/verify-shots/pointer-regression/$TS"
mkdir -p "$OUT_DIR"
RESULTS_FILE="$OUT_DIR/results.txt"

# Short-path scratch dirs -- a unix socket path is capped at 108 bytes.
RUNDIR="$(mktemp -d /tmp/gs-ptrreg-run.XXXXXX)"
CONFIGHOME="$(mktemp -d /tmp/gs-ptrreg-cfg.XXXXXX)"
SWAY_CFG="$RUNDIR/sway.conf"

SWAY_PID=""
GS_PID=""
VP_PID=""
VP_FIFO=""

log() { echo "[pointer-regression] $*" >&2; }

# Kill only the exact PIDs this script started -- never a name match.
teardown_instance() {
	if [[ -n "$GS_PID" ]] && kill -0 "$GS_PID" 2>/dev/null; then
		kill "$GS_PID" 2>/dev/null || true
		# Give gamescope its orderly teardown, then sweep the process group it
		# leads (setsid in start_instance): a wine client does not always
		# leave with its X server, and the wait below would hang on it.
		local waited=0
		while (( waited < 30 )) && kill -0 "$GS_PID" 2>/dev/null; do sleep 0.1; waited=$((waited + 1)); done
		kill -9 -- -"$GS_PID" 2>/dev/null || true
		wait "$GS_PID" 2>/dev/null || true
	fi
	GS_PID=""
}

teardown_all() {
	teardown_instance
	if [[ -n "$VP_PID" ]] && kill -0 "$VP_PID" 2>/dev/null; then
		vp quit 2>/dev/null || true
		exec 8>&- 2>/dev/null || true
		kill "$VP_PID" 2>/dev/null || true
		wait "$VP_PID" 2>/dev/null || true
	fi
	VP_PID=""
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
		exit "$status"
	fi
	teardown_all
	# Keep the logs as evidence (outside the shared scratchpad).
	cp "$RUNDIR"/*.log "$OUT_DIR/" 2>/dev/null || true
	rm -rf "$RUNDIR" "$CONFIGHOME"
	exit "$status"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
for bin in "$GAMESCOPE_BIN" "$GAMESCOPECTL_BIN" "$CLIENT_BIN"; do
	if [[ ! -x "$bin" ]]; then
		log "FATAL: $bin not found. Build first: scripts/build-gamescope-ritz.sh --test"
		log "  (the test client needs -Denable_tests, which --test sets)."
		exit 2
	fi
done
if ! command -v sway >/dev/null 2>&1; then
	log "FATAL: sway not found -- needed as the private, invisible host compositor."
	exit 2
fi
X11_CLIENT_AVAILABLE=1
[[ -x "$CLIENT_X11_BIN" ]] || { X11_CLIENT_AVAILABLE=0; log "note: $CLIENT_X11_BIN not built (needs SDL3) -- x11-* checks will be SKIPped"; }
VP_TOOL_AVAILABLE=1
[[ -x "$VP_TOOL_BIN" ]] || { VP_TOOL_AVAILABLE=0; log "note: $VP_TOOL_BIN not built -- x11-host will be SKIPped"; }
WINE_AVAILABLE=1
[[ -f "$PROBE_WIN32_EXE" ]] || { WINE_AVAILABLE=0; log "note: $PROBE_WIN32_EXE not built (needs x86_64-w64-mingw32-gcc) -- wine-* checks will be SKIPped"; }
command -v wine >/dev/null 2>&1 || { WINE_AVAILABLE=0; log "note: wine not found -- wine-* checks will be SKIPped"; }

# ---------------------------------------------------------------------------
# Private sway host
# ---------------------------------------------------------------------------
start_sway() {
	cat > "$SWAY_CFG" <<-EOF
		output HEADLESS-1 resolution ${OUT_W}x${OUT_H} position 0,0
		default_border none
		default_floating_border none
		focus_follows_mouse no
		seat seat0 hide_cursor 1
	EOF

	log "starting private sway (headless backend, XDG_RUNTIME_DIR=$RUNDIR)"
	# 9>&-: do not inherit with-gamescope-lock.sh's flock fd into a child that
	# might outlive this script (see pixel-regression.sh).
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 XDG_RUNTIME_DIR="$RUNDIR" \
		sway -c "$SWAY_CFG" > "$RUNDIR/sway.log" 2>&1 9>&- &
	SWAY_PID=$!

	local waited=0
	SWAY_WL_NAME=""
	while (( waited < SWAY_READY_TIMEOUT_S * 10 )); do
		SWAY_WL_NAME="$(find "$RUNDIR" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' -printf '%f\n' 2>/dev/null | head -1)"
		if [[ -n "$SWAY_WL_NAME" ]]; then break; fi
		kill -0 "$SWAY_PID" 2>/dev/null || { log "FATAL: sway exited early -- see $RUNDIR/sway.log"; cat "$RUNDIR/sway.log" >&2; exit 2; }
		sleep 0.1
		waited=$((waited + 1))
	done
	[[ -n "$SWAY_WL_NAME" ]] || { log "FATAL: sway never created a socket"; cat "$RUNDIR/sway.log" >&2; exit 2; }
	log "private sway ready: pid $SWAY_PID, socket $SWAY_WL_NAME"
}

# The host mouse: one virtual_pointer_tool for the whole run, fed through a
# fifo held open on fd 8 (closing it would end the tool and, with it, the
# seat's pointer). Started before any gamescope instance so every instance
# binds a wl_pointer from the start, as it does on a real desktop.
start_vp() {
	[[ "$VP_TOOL_AVAILABLE" -eq 1 ]] || return 0
	VP_FIFO="$RUNDIR/vp.fifo"
	mkfifo "$VP_FIFO"
	WAYLAND_DISPLAY="$SWAY_WL_NAME" XDG_RUNTIME_DIR="$RUNDIR" \
		"$VP_TOOL_BIN" < "$VP_FIFO" > "$RUNDIR/virtual-pointer.log" 2>&1 9>&- &
	VP_PID=$!
	exec 8>"$VP_FIFO"
	local waited=0
	while (( waited < 50 )); do
		grep -q '^READY' "$RUNDIR/virtual-pointer.log" 2>/dev/null && break
		kill -0 "$VP_PID" 2>/dev/null || { log "note: virtual_pointer_tool exited early -- x11-host will be SKIPped"; cat "$RUNDIR/virtual-pointer.log" >&2; VP_TOOL_AVAILABLE=0; VP_PID=""; return 0; }
		sleep 0.1
		waited=$((waited + 1))
	done
	log "host mouse ready: virtual_pointer_tool pid $VP_PID"
}

# vp <command line> -- one line to the tool (abs x y w h | move dx dy | ...).
vp() { echo "$*" >&8; }

# A private Wine prefix for the wine-* checks, made once per run with no
# display attached (wineboot needs none), so the first probe start inside
# gamescope is not also the prefix's first boot. mscoree/mshtml disabled:
# no Mono/Gecko prompts in a headless run.
WINEPREFIX_DIR=""
start_wine_prefix() {
	[[ "$WINE_AVAILABLE" -eq 1 ]] || return 0
	WINEPREFIX_DIR="$CONFIGHOME/wineprefix"
	log "creating a private wine prefix at $WINEPREFIX_DIR"
	if ! env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX="$WINEPREFIX_DIR" WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all \
		wineboot -i > "$RUNDIR/wineboot.log" 2>&1; then
		log "note: wineboot failed -- wine-* checks will be SKIPped"; cat "$RUNDIR/wineboot.log" >&2
		WINE_AVAILABLE=0
		return 0
	fi
	# The boot's wineserver (and its display-less explorer.exe) lingers a few
	# seconds; a probe started into it gets no desktop and CreateWindowEx
	# fails. Stop it and wait for it to be gone.
	WINEPREFIX="$WINEPREFIX_DIR" wineserver -k 2>/dev/null || true
	WINEPREFIX="$WINEPREFIX_DIR" wineserver -w 2>/dev/null || true
}
# Environment the wine probe's command line carries, as one string.
wine_env() { echo "WINEPREFIX='$WINEPREFIX_DIR' WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all"; }

# ---------------------------------------------------------------------------
# One gamescope instance with one client (locked or not). Windowed client,
# no --force-windows-fullscreen: a 640x480 window in a 1280x720 output is
# what makes a scaler toggle (Auto <-> Stretch) a real mapping change with
# no resize involved, and a nested-mode change moves the Auto mapping too.
# ---------------------------------------------------------------------------
GS_LOG=""
CLIENT_LOG=""
GS_WL_NAME=""

start_instance() {
	local lockarg="$1"          # "--lock", "--lock-after N" or "" (any client args, really)
	local client="${2:-$CLIENT_BIN}"
	local tag="${3:-${lockarg#--}}"
	local launcher="${4:-}"     # "wine" for the Win32 probe, empty for a native client
	teardown_instance

	GS_LOG="$RUNDIR/gamescope-${tag}.log"
	CLIENT_LOG="$RUNDIR/client-${tag}.log"
	: > "$CLIENT_LOG"
	local env_prefix="SDL_VIDEODRIVER=x11"
	[[ "$launcher" == wine ]] && env_prefix="$(wine_env)"
	log "starting gamescope instance, client ${launcher:+$launcher }$(basename "$client") ${lockarg:-unlocked}"
	# setsid: the client's own children (wine's server and its X connections)
	# must die with the instance, so the whole tree is one process group.
	WAYLAND_DISPLAY="$SWAY_WL_NAME" XDG_RUNTIME_DIR="$RUNDIR" XDG_CONFIG_HOME="$CONFIGHOME" \
		setsid "$GAMESCOPE_BIN" --backend wayland -w "$OUT_W" -h "$OUT_H" -W "$OUT_W" -H "$OUT_H" -- \
		sh -c "$env_prefix exec $launcher '$client' $lockarg --seconds 600 >> '$CLIENT_LOG' 2>&1" \
		> "$GS_LOG" 2>&1 9>&- &
	GS_PID=$!

	local waited=0
	GS_WL_NAME=""
	while (( waited < READY_TIMEOUT_S * 10 )); do
		GS_WL_NAME="$(grep -oP "wayland display '\K[^']+" "$GS_LOG" 2>/dev/null | head -1 || true)"
		if [[ -n "$GS_WL_NAME" ]] && grep -q '^READY' "$CLIENT_LOG" 2>/dev/null; then break; fi
		kill -0 "$GS_PID" 2>/dev/null || { log "FATAL: gamescope exited early -- see $GS_LOG"; tail -n 40 "$GS_LOG" >&2; exit 2; }
		sleep 0.1
		waited=$((waited + 1))
	done
	if [[ -z "$GS_WL_NAME" ]] || ! grep -q '^READY' "$CLIENT_LOG"; then
		log "FATAL: instance/client not ready within ${READY_TIMEOUT_S}s -- see $GS_LOG / $CLIENT_LOG"
		tail -n 40 "$GS_LOG" >&2
		exit 2
	fi
	sleep 1   # first frames, focus, pointer enter
	log "gamescope instance ready: pid $GS_PID, control socket $GS_WL_NAME"
	probe_stats
}

gsctl() {
	XDG_RUNTIME_DIR="$RUNDIR" GAMESCOPE_WAYLAND_DISPLAY="$GS_WL_NAME" "$GAMESCOPECTL_BIN" "$@"
}

# ---------------------------------------------------------------------------
# Readings
# ---------------------------------------------------------------------------
STATS_AVAILABLE=1

# Echoes the stats line ("constraint=... motions=... ...") or nothing.
stats_line() {
	local out
	out="$(gsctl wlserver_pointer_stats 2>&1 || true)"
	grep -o 'wlserver_pointer_stats:.*' <<<"$out" | tail -1 | sed 's/wlserver_pointer_stats: //' || true
}

# Once per instance, in the main shell (a $(...) subshell cannot set the
# flag): does this binary have wlserver_pointer_stats at all? A pre-fix
# binary (--gamescope for a baseline) does not; its compositor-side
# assertions are then SKIPped and the client-side counts stand alone.
probe_stats() {
	if [[ -n "$(stats_line)" ]]; then
		STATS_AVAILABLE=1
	else
		STATS_AVAILABLE=0
		log "wlserver_pointer_stats unavailable in this binary: compositor-side counters will be SKIPped"
	fi
}

# stat <key> [line]: the value of key=... in the stats line; 0 when absent so
# arithmetic on it never aborts the run.
stat() {
	local key="$1" line="${2:-}" v=""
	[[ -n "$line" ]] || line="$(stats_line)"
	v="$(grep -o "$key=[^ ]*" <<<"$line" | head -1 | cut -d= -f2- || true)"
	echo "${v:-0}"
}

client_motions() { grep -c '^MOTION' "$CLIENT_LOG" 2>/dev/null || true; }
# The Wine probe's raw-input packets carrying exactly dx=<n> dy=0.
client_raw_dx() { grep -c "^RAW dx=$1 dy=0 " "$CLIENT_LOG" 2>/dev/null || true; }
# The last MOTION line's position as "x,y" (integer; both clients print it
# first as x=.. y=..).
client_last_pos() { grep '^MOTION' "$CLIENT_LOG" 2>/dev/null | tail -1 | sed -E 's/^MOTION x=([0-9]+)(\.[0-9]+)? y=([0-9]+)(\.[0-9]+)?.*/\1,\3/' || true; }
# gamescope's own idea of the pointer, "x,y" (integer), from the stats line.
stats_cursor() { stat cursor "${1:-}" | tr -d '()' | sed -E 's/([0-9]+)(\.[0-9]+)?,([0-9]+)(\.[0-9]+)?/\1,\3/'; }
# Lines the client prints when its window changes size (SIZE for SDL3,
# WINDOW for the Wine probe).
client_resizes() { grep -c '^SIZE\|^WINDOW' "$CLIENT_LOG" 2>/dev/null || true; }

wait_for_client_line() {
	local pattern="$1" timeout="${2:-$LOCK_TIMEOUT_S}" waited=0
	while (( waited < timeout * 10 )); do
		grep -q "$pattern" "$CLIENT_LOG" 2>/dev/null && return 0
		sleep 0.1; waited=$((waited + 1))
	done
	return 1
}
# wait_for_constraint <state> [<state>...]: until the constraint is one of them.
wait_for_constraint() {
	local waited=0 want
	[[ "$STATS_AVAILABLE" -eq 1 ]] || return 0
	while (( waited < LOCK_TIMEOUT_S * 10 )); do
		for want in "$@"; do [[ "$(stat constraint)" == "$want" ]] && return 0; done
		sleep 0.1; waited=$((waited + 1))
	done
	return 1
}

# Evidence trail: the full stats line at each step, into the run log.
trace() { log "  [$1] $(stats_line) client_motions=$(client_motions)"; }
# (with no stats the line is just the client count -- still a trail)

wait_for_lock() {
	local waited=0
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		while (( waited < LOCK_TIMEOUT_S * 10 )); do
			[[ "$(stat constraint)" == "locked" ]] && return 0
			sleep 0.1
			waited=$((waited + 1))
		done
		return 1
	fi
	# No stats (a pre-fix binary): probe through behaviour -- a 1px relative
	# injection that shows up at the client as xrel=1. (Weak: an unlocked
	# client reports the same from core motion; it only rules out "nothing
	# arrives at all".) The probe lines are counted before the checks start,
	# so they do not pollute them.
	while (( waited < LOCK_TIMEOUT_S * 2 )); do
		local before; before="$(grep -c '^MOTION.*xrel=1 ' "$CLIENT_LOG" || true)"
		gsctl wlserver_debug_mouse_motion "1 0 1" >/dev/null 2>&1 || true
		sleep 0.5
		local after; after="$(grep -c '^MOTION.*xrel=1 ' "$CLIENT_LOG" || true)"
		(( after > before )) && return 0
		waited=$((waited + 1))
	done
	return 1
}

# ---------------------------------------------------------------------------
# Results table
# ---------------------------------------------------------------------------
declare -a RESULT_LINES=()
FAIL_COUNT=0
SKIP_COUNT=0

record() {
	local status="$1" name="$2" detail="$3"
	RESULT_LINES+=("$status	$name	$detail")
	case "$status" in
		FAIL) FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
		SKIP) SKIP_COUNT=$((SKIP_COUNT + 1)) ;;
	esac
	log "$status  $name  $detail"
}

# assert_eq <name> <what> <expected> <actual> -> records PASS/FAIL
assert_eq() {
	local name="$1" what="$2" expected="$3" actual="$4"
	if [[ "$expected" == "$actual" ]]; then
		record PASS "$name" "$what = $actual"
	else
		record FAIL "$name" "$what expected $expected, got $actual"
	fi
}

# ---------------------------------------------------------------------------
# Checks -- the locked instance
# ---------------------------------------------------------------------------
check_locked_relative() {
	should_run locked-relative || { record SKIP locked-relative "--only excluded it"; return; }
	local before after
	before="$(client_motions)"
	gsctl wlserver_debug_mouse_motion "$REL_DX 0 $REL_COUNT" >/dev/null 2>&1 || true
	sleep 0.5
	after="$(client_motions)"
	assert_eq locked-relative "client MOTION lines from $REL_COUNT relative injections" "$REL_COUNT" "$((after - before))"
	local dxs; dxs="$(grep '^MOTION' "$CLIENT_LOG" | tail -n "$REL_COUNT" | grep -c "xrel=$REL_DX " || true)"
	assert_eq locked-relative "of which carried xrel=$REL_DX" "$REL_COUNT" "$dxs"
}

check_locked_absolute() {
	should_run locked-absolute || { record SKIP locked-absolute "--only excluded it"; return; }
	local before after i
	before="$(client_motions)"
	for (( i = 0; i < ABS_SAMPLES / 2; i++ )); do
		gsctl wlserver_debug_absolute_motion "0.25 0.25" >/dev/null 2>&1 || true
		gsctl wlserver_debug_absolute_motion "0.75 0.75" >/dev/null 2>&1 || true
	done
	sleep 0.5
	after="$(client_motions)"
	assert_eq locked-absolute "client MOTION lines from $ABS_SAMPLES absolute samples while locked" 0 "$((after - before))"
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		local line; line="$(stats_line)"
		assert_eq locked-absolute "motions_locked" 0 "$(stat motions_locked "$line")"
		assert_eq locked-absolute "constraint still" locked "$(stat constraint "$line")"
	else
		record SKIP locked-absolute "wlserver_pointer_stats not in this binary (compositor-side counters)"
	fi
}

check_locked_mapping() {
	should_run locked-mapping || { record SKIP locked-mapping "--only excluded it"; return; }
	local before after r0 r1 line
	# The last input is an absolute sample (locked-absolute left one, or make
	# one now), so the re-sync path is armed -- it must still send nothing.
	gsctl wlserver_debug_absolute_motion "$ABS_SAMPLE" >/dev/null 2>&1 || true
	sleep 0.3
	before="$(client_motions)"
	r0="$(stat resyncs)"
	trace "locked, before mapping changes"
	gsctl overlay_e2_set "display.filter.scaler $SCALER_STRETCH" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	trace "locked, after Auto->Stretch"
	gsctl steamcompmgr_debug_set_nested_mode "$MODE_A" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	trace "locked, after nested mode $MODE_A"
	gsctl overlay_e2_set "display.filter.scaler $SCALER_AUTO" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	trace "locked, after Stretch->Auto"
	gsctl steamcompmgr_debug_set_nested_mode "$MODE_B" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	trace "locked, after nested mode $MODE_B"
	after="$(client_motions)"
	assert_eq locked-mapping "client MOTION lines across 2 scaler toggles + 2 mode changes while locked" 0 "$((after - before))"
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		line="$(stats_line)"
		r1="$(stat resyncs "$line")"
		assert_eq locked-mapping "resyncs sent" 0 "$((r1 - r0))"
		assert_eq locked-mapping "motions_locked" 0 "$(stat motions_locked "$line")"
		# The change detection itself ran (the mapping really moved), or the
		# check proved nothing: the suppressed counter must have advanced.
		local sup; sup="$(stat warps_suppressed_locked "$line")"
		if (( sup > 0 )); then
			record PASS locked-mapping "warps_suppressed_locked = $sup (the re-sync was reached and refused)"
		else
			record FAIL locked-mapping "warps_suppressed_locked = $sup -- no mapping change reached the re-sync; the check is void"
		fi
	else
		record SKIP locked-mapping "wlserver_pointer_stats not in this binary (compositor-side counters)"
	fi
}

# ---------------------------------------------------------------------------
# Checks -- the unlocked instance
# ---------------------------------------------------------------------------
check_unlocked_resync() {
	should_run unlocked-resync || { record SKIP unlocked-resync "--only excluded it"; return; }
	local line c0 c1 r0 r1
	line="$(stats_line)"
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		assert_eq unlocked-resync "constraint" none "$(stat constraint "$line")"
	fi

	# One absolute sample: arms the re-sync, and the client sees exactly it.
	c0="$(client_motions)"
	gsctl wlserver_debug_absolute_motion "$ABS_SAMPLE" >/dev/null 2>&1 || true
	sleep 0.5
	c1="$(client_motions)"
	assert_eq unlocked-resync "client MOTION lines from 1 absolute sample" 1 "$((c1 - c0))"

	# Steady state: nothing may re-sync per frame.
	r0="$(stat resyncs)"; c0="$(client_motions)"
	sleep "$IDLE_S"
	r1="$(stat resyncs)"; c1="$(client_motions)"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq unlocked-resync "resyncs over ${IDLE_S}s idle" 0 "$((r1 - r0))"
	assert_eq unlocked-resync "client MOTION lines over ${IDLE_S}s idle" 0 "$((c1 - c0))"

	# A real mapping change: Auto -> Stretch on a windowed 640x480 client.
	trace "before Auto->Stretch"
	r0="$(stat resyncs)"; c0="$(client_motions)"
	gsctl overlay_e2_set "display.filter.scaler $SCALER_STRETCH" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	trace "after Auto->Stretch"
	r1="$(stat resyncs)"; c1="$(client_motions)"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq unlocked-resync "resyncs from scaler Auto->Stretch" 1 "$((r1 - r0))"
	assert_eq unlocked-resync "client MOTION lines from scaler Auto->Stretch" 1 "$((c1 - c0))"

	# The same value again: no mapping change, no re-sync.
	r0="$(stat resyncs)"; c0="$(client_motions)"
	gsctl overlay_e2_set "display.filter.scaler $SCALER_STRETCH" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	r1="$(stat resyncs)"; c1="$(client_motions)"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq unlocked-resync "resyncs from a no-op scaler set" 0 "$((r1 - r0))"
	assert_eq unlocked-resync "client MOTION lines from a no-op scaler set" 0 "$((c1 - c0))"

	# Back to Auto, then a nested-mode change (item 10's path): one each.
	gsctl overlay_e2_set "display.filter.scaler $SCALER_AUTO" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	trace "before nested mode $MODE_A"
	r0="$(stat resyncs)"; c0="$(client_motions)"
	gsctl steamcompmgr_debug_set_nested_mode "$MODE_A" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"
	trace "after nested mode $MODE_A"
	r1="$(stat resyncs)"; c1="$(client_motions)"
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		local out; out="$(gsctl steamcompmgr_debug_set_nested_mode "$MODE_B" 2>&1 || true)"
		assert_eq unlocked-resync "resyncs from nested mode $MODE_A" 1 "$((r1 - r0))"
		assert_eq unlocked-resync "client MOTION lines from nested mode $MODE_A" 1 "$((c1 - c0))"
		sleep "$SETTLE_S"
		line="$(stats_line)"
		assert_eq unlocked-resync "motions_locked (never locked here)" 0 "$(stat motions_locked "$line")"
	else
		record SKIP unlocked-resync "nested-mode change needs steamcompmgr_debug_set_nested_mode (not in this binary)"
	fi
}

# ---------------------------------------------------------------------------
# Checks -- the SDL3 (CS2-shaped) client, CS2's order
# ---------------------------------------------------------------------------
# x11_menu_phase: MENU_SAMPLES absolute samples delivered to the not-yet-locked
# client, then wait for its lock. Shared by x11-menu-lock and x11-locked-abs,
# which run on one instance.
x11_menu_phase() {
	local i
	for (( i = 0; i < MENU_SAMPLES; i++ )); do
		gsctl wlserver_debug_absolute_motion "0.$((30 + i * 5)) 0.$((30 + i * 3))" >/dev/null 2>&1 || true
		sleep 0.1
	done
	trace "x11 menu phase: $MENU_SAMPLES absolute samples"
}

check_x11_menu_lock() {
	should_run x11-menu-lock || { record SKIP x11-menu-lock "--only excluded it"; return; }
	if ! grep -q '^LOCKED' "$CLIENT_LOG"; then
		record FAIL x11-menu-lock "the SDL3 client never entered relative mode: $(stats_line)"
		return
	fi
	local before after
	before="$(client_motions)"
	gsctl wlserver_debug_mouse_motion "$REL_DX 0 $REL_COUNT" >/dev/null 2>&1 || true
	sleep 0.5
	after="$(client_motions)"
	assert_eq x11-menu-lock "client MOTION lines from $REL_COUNT relative injections after a menu" "$REL_COUNT" "$((after - before))"
	local dxs; dxs="$(grep '^MOTION' "$CLIENT_LOG" | tail -n "$REL_COUNT" | grep -c "xrel=$REL_DX.0 " || true)"
	assert_eq x11-menu-lock "of which carried xrel=$REL_DX.0 (not differenced)" "$REL_COUNT" "$dxs"
	# The device SDL3 cached for the master pointer -- the whole mechanism.
	local axis; axis="$(grep '^DEVICE id=2 ' "$CLIENT_LOG" | head -1 | grep -o 'axis0=[a-z]*' | cut -d= -f2 || true)"
	assert_eq x11-menu-lock "master pointer axis mode as SDL3 cached it" rel "${axis:-none}"
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		local line m r; line="$(stats_line)"
		m="$(stat motions "$line")"; r="$(stat relatives "$line")"
		if (( r >= m && r > 0 )); then
			record PASS x11-menu-lock "relatives=$r >= motions=$m (every movement carried relative motion)"
		else
			record FAIL x11-menu-lock "relatives=$r motions=$m -- a movement went out absolute-only"
		fi
	fi
}

check_x11_locked_absolute() {
	should_run x11-locked-abs || { record SKIP x11-locked-abs "--only excluded it"; return; }
	local before after i
	before="$(client_motions)"
	for (( i = 0; i < ABS_SAMPLES / 2; i++ )); do
		gsctl wlserver_debug_absolute_motion "0.25 0.25" >/dev/null 2>&1 || true
		gsctl wlserver_debug_absolute_motion "0.75 0.75" >/dev/null 2>&1 || true
	done
	sleep 0.5
	after="$(client_motions)"
	assert_eq x11-locked-abs "client MOTION lines from $ABS_SAMPLES absolute samples while locked (SDL3)" 0 "$((after - before))"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq x11-locked-abs "motions_locked" 0 "$(stat motions_locked)"
}

# The host path: a virtual mouse in the private sway. Force-grab off, so the
# menu's samples arrive as absolute host motion (wlserver_touchmotion ->
# wlserver_mousewarp), the game's lock makes gamescope lock the HOST pointer,
# and once the host confirms it the host's relative motion is what the
# client gets. Then force-grab on, which keeps the host lock, for good measure.
check_x11_host() {
	should_run x11-host || { record SKIP x11-host "--only excluded it"; return; }
	# The private sway has no keyboard, so gamescope never gets keyboard
	# focus; by default it drops host relative motion without it.
	gsctl wayland_mouse_relmotion_without_keyboard_focus 1 >/dev/null 2>&1 || true
	local c0 c1 i
	c0="$(client_motions)"
	vp abs 400 300 "$OUT_W" "$OUT_H"; sleep 0.2
	for (( i = 0; i < MENU_SAMPLES; i++ )); do vp move 30 10; sleep 0.15; done
	sleep 0.3
	c1="$(client_motions)"
	trace "x11-host menu phase: 1 absolute + $MENU_SAMPLES host moves, force-grab off"
	if (( c1 - c0 < MENU_SAMPLES )); then
		record FAIL x11-host "host motion did not reach the client in the menu (client +$((c1 - c0)) from $((MENU_SAMPLES + 1)) host samples)"
		return
	fi
	record PASS x11-host "host absolute samples reached the menu (client +$((c1 - c0)))"
	if ! wait_for_lock; then
		record FAIL x11-host "no LOCKED constraint within ${LOCK_TIMEOUT_S}s after the client asked: $(stats_line)"
		return
	fi
	sleep 1.0   # the host has to confirm gamescope's own lock before relative motion flows
	c0="$(client_motions)"
	for (( i = 0; i < REL_COUNT; i++ )); do vp move "$HOST_REL_DX" 0; sleep 0.05; done
	sleep 0.5
	c1="$(client_motions)"
	assert_eq x11-host "client MOTION lines from $REL_COUNT host relative steps, locked, force-grab off" "$REL_COUNT" "$((c1 - c0))"
	local dxs; dxs="$(grep '^MOTION' "$CLIENT_LOG" | tail -n "$REL_COUNT" | grep -c "xrel=$HOST_REL_DX.0 " || true)"
	assert_eq x11-host "of which carried xrel=$HOST_REL_DX.0" "$REL_COUNT" "$dxs"
	local axis; axis="$(grep '^DEVICE id=2 ' "$CLIENT_LOG" | head -1 | grep -o 'axis0=[a-z]*' | cut -d= -f2 || true)"
	assert_eq x11-host "master pointer axis mode as SDL3 cached it (host-driven menu)" rel "${axis:-none}"
	# Force-grab on: the host lock is already held; motion must keep flowing.
	gsctl debug_set_force_relative_mouse 1 >/dev/null 2>&1 || true
	sleep 0.5
	c0="$(client_motions)"
	for (( i = 0; i < HOST_FG_COUNT; i++ )); do vp move "$HOST_REL_DX" 0; sleep 0.05; done
	sleep 0.5
	c1="$(client_motions)"
	assert_eq x11-host "client MOTION lines from $HOST_FG_COUNT host relative steps, force-grab on" "$HOST_FG_COUNT" "$((c1 - c0))"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq x11-host "motions_locked" 0 "$(stat motions_locked)"
	trace "x11-host done"
}

# ---------------------------------------------------------------------------
# Checks -- a window larger than the screen (the Rust report, 2026-09-08)
# ---------------------------------------------------------------------------
# Shared tail of oversized-window and wine-mode: the instance holds a
# 1280x720 client that fills the 1280x720 output; the nested mode goes to
# 960x540, smaller than the window. The window must be brought within the
# root exactly once, and an absolute sample must reach the client where
# gamescope mapped it -- which is only possible once it has.
oversized_tail() {
	local name="$1" reports_size="$2" s0 s1 s2 r1 r2 line pos cur
	s0="$(client_resizes)"
	trace "$name: before nested mode $MODE_SMALL"
	gsctl steamcompmgr_debug_set_nested_mode "$MODE_SMALL" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"; sleep "$SETTLE_S"   # root change, our resize, the client's own reply, our second resize
	s1="$(client_resizes)"
	trace "$name: after nested mode $MODE_SMALL"
	# The SDL3 client reports every ConfigureNotify; Wine keeps a WM resize
	# to itself (its Win32 window stays 1280x720, measured), so for the Wine
	# probe the X-side evidence is the mapping below instead.
	if [[ "$reports_size" -eq 1 ]]; then
		if (( s1 - s0 >= 1 )); then
			record PASS "$name" "client window resized after nested mode $MODE_SMALL (+$((s1 - s0)) size events)"
		else
			record FAIL "$name" "client window never resized after nested mode $MODE_SMALL"
		fi
	fi
	# No flapping: a window bounced between two sizes re-syncs the pointer
	# every frame (measured: resyncs=70 in 6 s on the first cut of the fix).
	r1="$(stat resyncs)"
	sleep "$IDLE_S"
	s2="$(client_resizes)"; r2="$(stat resyncs)"
	assert_eq "$name" "size events over ${IDLE_S}s idle (no flapping)" 0 "$((s2 - s1))"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq "$name" "resyncs over ${IDLE_S}s idle (no flapping)" 0 "$((r2 - r1))"
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		line="$(stats_line)"
		local mapping; mapping="$(stat mapping "$line" | cut -d, -f1 | tr -d '(')"
		assert_eq "$name" "mapping scale (the window is 960x540 in a 1280x720 output)" "0.7500" "$mapping"
	fi
	gsctl wlserver_debug_absolute_motion "$OVERSIZED_ABS" >/dev/null 2>&1 || true
	sleep 0.5
	pos="$(client_last_pos)"
	assert_eq "$name" "client position from a sample at ($OVERSIZED_ABS)" "$OVERSIZED_EXPECT" "$pos"
	if [[ "$STATS_AVAILABLE" -eq 1 ]]; then
		cur="$(stats_cursor)"
		assert_eq "$name" "the same position as gamescope's own cursor" "$cur" "$pos"
	fi
	trace "$name: done"
}

check_oversized_window() {
	should_run oversized-window || { record SKIP oversized-window "--only excluded it"; return; }
	if ! wait_for_client_line '^SIZE 1280x720'; then
		record FAIL oversized-window "the SDL3 client never became 1280x720: $(stats_line)"
		return
	fi
	sleep 0.5
	oversized_tail oversized-window 1
}

check_wine_mode() {
	should_run wine-mode || { record SKIP wine-mode "--only excluded it"; return; }
	# Wine's fullscreen clip plus no cursor yet (nothing has moved) is a
	# LOCK to Xwayland; one relative nudge makes Wine define its cursor and
	# the lock become the clip's CONFINE (none once the window no longer
	# covers the screen). Then the menu is a menu.
	gsctl wlserver_debug_mouse_motion "3 0 3" >/dev/null 2>&1 || true
	if ! wait_for_constraint confined none; then
		record FAIL wine-mode "the Wine menu never left its lock: $(stats_line)"
		return
	fi
	record PASS wine-mode "the Wine menu is $(stat constraint) after one relative nudge (locked until Wine defined a cursor)"
	oversized_tail wine-mode 0
}

check_wine_lock_mode() {
	should_run wine-lock-mode || { record SKIP wine-lock-mode "--only excluded it"; return; }
	if ! wait_for_client_line '^LOCKED' || ! wait_for_lock; then
		record FAIL wine-lock-mode "the Wine probe never locked: $(stats_line)"
		return
	fi
	sleep 0.5
	local r0 r1
	r0="$(client_raw_dx "$REL_DX")"
	gsctl wlserver_debug_mouse_motion "$REL_DX 0 $REL_COUNT" >/dev/null 2>&1 || true
	sleep 0.5
	r1="$(client_raw_dx "$REL_DX")"
	assert_eq wine-lock-mode "RAW dx=$REL_DX packets from $REL_COUNT relative injections, locked" "$REL_COUNT" "$((r1 - r0))"
	trace "wine-lock-mode: before nested mode $MODE_SMALL, locked"
	gsctl steamcompmgr_debug_set_nested_mode "$MODE_SMALL" >/dev/null 2>&1 || true
	sleep "$SETTLE_S"; sleep "$SETTLE_S"
	trace "wine-lock-mode: after nested mode $MODE_SMALL, locked"
	r0="$(client_raw_dx "$REL_DX")"
	gsctl wlserver_debug_mouse_motion "$REL_DX 0 $REL_COUNT" >/dev/null 2>&1 || true
	sleep 0.5
	r1="$(client_raw_dx "$REL_DX")"
	assert_eq wine-lock-mode "RAW dx=$REL_DX packets from $REL_COUNT relative injections after the mode change" "$REL_COUNT" "$((r1 - r0))"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq wine-lock-mode "motions_locked" 0 "$(stat motions_locked)"
	if ! wait_for_client_line '^UNLOCKED' "$UNLOCK_TIMEOUT_S"; then
		record FAIL wine-lock-mode "the Wine probe never unlocked"
		return
	fi
	# Unlocked in Windows terms, still LOCKED to Xwayland until Wine defines
	# a cursor, which takes a mouse move: the host lock is still held at this
	# point, so that move is relative.
	gsctl wlserver_debug_mouse_motion "2 0 3" >/dev/null 2>&1 || true
	if wait_for_constraint none confined; then
		record PASS wine-lock-mode "constraint $(stat constraint) after the unlock and a relative nudge"
	else
		record FAIL wine-lock-mode "constraint still $(stat constraint) after the unlock and a relative nudge"
		return
	fi
	gsctl wlserver_debug_absolute_motion "$OVERSIZED_ABS" >/dev/null 2>&1 || true
	sleep 0.5
	local pos; pos="$(client_last_pos)"
	assert_eq wine-lock-mode "client position from a sample at ($OVERSIZED_ABS) after the unlock" "$OVERSIZED_EXPECT" "$pos"
	[[ "$STATS_AVAILABLE" -eq 1 ]] && assert_eq wine-lock-mode "the same position as gamescope's own cursor" "$(stats_cursor)" "$pos"
	trace "wine-lock-mode: done"
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
log "binary: $GAMESCOPE_BIN"
log "results: $OUT_DIR"
start_sway
start_vp

need_locked=0; need_unlocked=0; need_x11=0; need_x11_host=0; need_oversized=0; need_wine=0
for c in locked-relative locked-absolute locked-mapping; do should_run "$c" && need_locked=1; done
should_run unlocked-resync && need_unlocked=1
for c in x11-menu-lock x11-locked-abs; do should_run "$c" && need_x11=1; done
should_run x11-host && need_x11_host=1
should_run oversized-window && need_oversized=1
for c in wine-mode wine-lock-mode; do should_run "$c" && need_wine=1; done

if [[ "$need_locked" -eq 1 ]]; then
	start_instance "--lock"
	# Xwayland only locks once the pointer is inside the window (its warp
	# emulator needs pointer focus): one absolute sample puts it there.
	gsctl wlserver_debug_absolute_motion "0.5 0.5" >/dev/null 2>&1 || true
	if wait_for_lock; then
		[[ "$STATS_AVAILABLE" -eq 1 ]] && log "lock established: $(stats_line)"
		check_locked_relative
		check_locked_absolute
		check_locked_mapping
	else
		record FAIL locked-relative "no LOCKED constraint within ${LOCK_TIMEOUT_S}s: $(stats_line)"
		record SKIP locked-absolute "no lock"
		record SKIP locked-mapping "no lock"
	fi
fi

if [[ "$need_unlocked" -eq 1 ]]; then
	start_instance ""
	check_unlocked_resync
fi

if [[ "$need_x11" -eq 1 ]]; then
	if [[ "$X11_CLIENT_AVAILABLE" -eq 1 ]]; then
		start_instance "--lock-after $LOCK_AFTER_S" "$CLIENT_X11_BIN" "x11-menu"
		x11_menu_phase
		if wait_for_lock; then
			sleep 0.5
			check_x11_menu_lock
			check_x11_locked_absolute
		else
			record FAIL x11-menu-lock "no LOCKED constraint within ${LOCK_TIMEOUT_S}s: $(stats_line)"
			record SKIP x11-locked-abs "no lock"
		fi
	else
		record SKIP x11-menu-lock "pointer_grab_client_x11 not built (needs SDL3)"
		record SKIP x11-locked-abs "pointer_grab_client_x11 not built (needs SDL3)"
	fi
fi

if [[ "$need_x11_host" -eq 1 ]]; then
	if [[ "$X11_CLIENT_AVAILABLE" -eq 1 && "$VP_TOOL_AVAILABLE" -eq 1 ]]; then
		start_instance "--lock-after $(( LOCK_AFTER_S + 2 ))" "$CLIENT_X11_BIN" "x11-host"
		check_x11_host
	else
		record SKIP x11-host "needs pointer_grab_client_x11 (SDL3) and virtual_pointer_tool"
	fi
fi

if [[ "$need_oversized" -eq 1 ]]; then
	if [[ "$X11_CLIENT_AVAILABLE" -eq 1 ]]; then
		# --fullscreen: SDL3 ends up with a plain 1280x720 window carrying
		# 1280x720 size hints (it recreates the window), which is exactly the
		# shape Wine leaves a game in after a mode change -- see the header.
		start_instance "--fullscreen" "$CLIENT_X11_BIN" "oversized"
		check_oversized_window
	else
		record SKIP oversized-window "pointer_grab_client_x11 not built (needs SDL3)"
	fi
fi

if [[ "$need_wine" -eq 1 ]]; then
	start_wine_prefix
	if [[ "$WINE_AVAILABLE" -eq 1 ]]; then
		if should_run wine-mode; then
			start_instance "--screen" "$PROBE_WIN32_EXE" "wine-mode" wine
			check_wine_mode
		fi
		if should_run wine-lock-mode; then
			start_instance "--screen --lock-after $WINE_LOCK_AFTER_S --unlock-after $WINE_UNLOCK_AFTER_S" "$PROBE_WIN32_EXE" "wine-lock" wine
			check_wine_lock_mode
		fi
	else
		record SKIP wine-mode "needs pointer_probe_win32.exe (mingw) and wine"
		record SKIP wine-lock-mode "needs pointer_probe_win32.exe (mingw) and wine"
	fi
fi

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
{
	echo "pointer-regression $TS"
	echo "binary: $GAMESCOPE_BIN"
	printf '%s\n' "${RESULT_LINES[@]}"
} > "$RESULTS_FILE"

echo
echo "pointer-regression: $(( ${#RESULT_LINES[@]} - FAIL_COUNT - SKIP_COUNT )) passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
echo "  results: $RESULTS_FILE"
[[ "$FAIL_COUNT" -eq 0 ]]
