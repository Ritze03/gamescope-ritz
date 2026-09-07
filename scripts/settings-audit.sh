#!/usr/bin/env bash
# settings-audit.sh -- prove, by measurement, that every setting the overlay
# registers is actually saved: reaches the right file, at its declared key,
# and comes back after gamescope restarts.
#
# WHY THIS EXISTS
#   "Is every setting actually being saved?" had no answer that was not a
#   hand-check of a few rows. The unit tests pin the config layer's
#   round-trip and the profile diff, and the pixel/pointer gates pin the
#   pictures -- but nothing walked EVERY registered row through the real
#   binding, watched the real file and relaunched the real binary. This
#   does, and it is an AUDIT: it reports, it never fixes.
#
# WHAT IT PROVES, PER ROW, IN THREE ROUTING SITUATIONS
#   (a) no game identified, editing a general profile;
#   (b) a game identified whose game profile inherits a general one -- the
#       value must be stored as a diff in the child and the parent must not
#       move;
#   (c) the same game, but `--profile <name>` forcing the session -- the
#       value must land in the forced profile and nowhere else.
#
#   For every row overlay_e2_dump_keys lists (the registry's own walk, so
#   the audit cannot drift from the code): read the live value, set a
#   different valid one, read it back (live-get), watch the isolated config
#   directory for the write and diff every JSON file (on-disk: the declared
#   key changed in the session profile -- or, for the Appearance/Cursor/
#   Profiles-filter rows, in global.json's `overlay` section -- and
#   correct-file: nothing else changed anywhere), then restart gamescope
#   with the identical environment and flags and read every row again
#   (survives-restart), and finally restore every original value (restore).
#   Every row gets a verdict; anything the round-trip cannot exercise is
#   listed as NOT COVERED with the reason and what covers it instead.
#
# HOW IT RUNS WITHOUT TOUCHING THE DESKTOP
#   The same recipe as pixel-regression.sh: a private, invisible sway
#   (WLR_BACKENDS=headless, its own XDG_RUNTIME_DIR, no input devices) hosts
#   a real nested `gamescope --backend wayland` -- so the Resolution area,
#   which needs a nested backend (INestedHints), is available too. No
#   client is launched: the registry answers without one. Every value is
#   driven and read over gamescopectl (overlay_e2_set / overlay_e2_get /
#   overlay_e2_dump_keys / ritz_profile / settings_overlay_visible), never
#   OS input; every launch uses a throwaway XDG_CONFIG_HOME under /tmp,
#   never ~/.config/gamescope-ritz. --headless swaps sway for gamescope's
#   own headless backend (faster, but the Resolution area is then NOT
#   COVERED). A kitty client with a flat background (the pixel gate's own
#   client) is launched inside gamescope: without any client nothing is
#   composited, the overlay never draws, and the dynamic areas (Profiles,
#   Mixer) never build their rows.
#
# USAGE
#   scripts/settings-audit.sh                     # all three situations (~5 min)
#   scripts/settings-audit.sh --situations a      # one situation
#   scripts/settings-audit.sh --only hud.enabled,crosshair.line_gap
#   scripts/settings-audit.sh --out DIR           # default build-release/verify-shots/settings-audit-<date>/
#   scripts/settings-audit.sh --headless          # no sway; Resolution area not covered
#   scripts/settings-audit.sh --gamescope <bin>   # audit another build
#
# OUTPUT
#   <out>/results.txt   the table (one row per setting per situation), the
#                       ranked failure list, the NOT COVERED list and the
#                       SUMMARY line -- see scripts/README.md for reading it
#   <out>/steps.jsonl   every set/get/diff as JSON, the evidence behind a cell
#   <out>/situation-*/  the registry dump and the config directory as it was
#                       at the baseline, after the sets, after the restart
#                       and after the restore, plus each launch's log
#
# Exits 0 if every audited row passed, 1 if any failed, 2 on a setup
# problem (binary missing, sway not found, an instance never came up) that
# is not a verdict on persistence at all.
#
# Runs the whole session under with-gamescope-lock.sh (self-re-execs under
# it on first invocation) -- see that script's header for why.
set -euo pipefail

if [[ -z "${SETTINGS_AUDIT_LOCKED:-}" ]]; then
	export SETTINGS_AUDIT_LOCKED=1
	SCRIPT_DIR_BOOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	exec "$SCRIPT_DIR_BOOT/with-gamescope-lock.sh" "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DRIVER="$SCRIPT_DIR/settings_audit.py"
GAMESCOPE_BIN="$REPO_ROOT/build-release/src/gamescope"
GAMESCOPECTL_BIN="$REPO_ROOT/build-release/src/gamescopectl"

OUT_W=1280
OUT_H=720
SWAY_READY_TIMEOUT_S=10

OUT_DIR=""
SITUATIONS="a,b,c"
ONLY=""
HEADLESS=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--out) OUT_DIR="$2"; shift 2 ;;
		--situations) SITUATIONS="$2"; shift 2 ;;
		--only) ONLY="$2"; shift 2 ;;
		--headless) HEADLESS=1; shift ;;
		--gamescope) GAMESCOPE_BIN="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,70p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*) echo "settings-audit: unknown argument: $1" >&2; exit 2 ;;
	esac
done

[[ -n "$OUT_DIR" ]] || OUT_DIR="$REPO_ROOT/build-release/verify-shots/settings-audit-$(date +%Y-%m-%d)"
mkdir -p "$OUT_DIR"

log() { echo "[settings-audit] $*" >&2; }

# Short-path scratch dirs -- a unix socket path is capped at 108 bytes.
# Never the scratchpad, never the user's real ~/.config.
RUNDIR="$(mktemp -d /tmp/gs-audit-run.XXXXXX)"
CONFIGHOME="$(mktemp -d /tmp/gs-audit-cfg.XXXXXX)"
SWAY_PID=""
SWAY_WL_NAME=""

cleanup() {
	local status=$?
	# Only the exact PID this script started (the driver stops its own
	# gamescope PIDs the same way) -- never a name match.
	if [[ -n "$SWAY_PID" ]] && kill -0 "$SWAY_PID" 2>/dev/null; then
		kill "$SWAY_PID" 2>/dev/null || true
		wait "$SWAY_PID" 2>/dev/null || true
	fi
	rm -rf "$RUNDIR" "$CONFIGHOME"
	exit "$status"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
if [[ ! -x "$GAMESCOPE_BIN" ]]; then
	log "FATAL: $GAMESCOPE_BIN not found -- run scripts/build-gamescope-ritz.sh first."
	exit 2
fi
if [[ ! -x "$GAMESCOPECTL_BIN" ]]; then
	log "FATAL: $GAMESCOPECTL_BIN not found (builds alongside gamescope)."
	exit 2
fi
if ! grep -q overlay_e2_dump_keys "$GAMESCOPE_BIN"; then
	log "FATAL: this binary has no overlay_e2_dump_keys ConCommand -- rebuild from a tree that has it."
	exit 2
fi
command -v python3 >/dev/null 2>&1 || { log "FATAL: python3 not found"; exit 2; }
command -v kitty >/dev/null 2>&1 || { log "FATAL: kitty not found -- the client the overlay draws over (see the header)"; exit 2; }
if [[ "$HEADLESS" -eq 0 ]] && ! command -v sway >/dev/null 2>&1; then
	log "FATAL: sway not found -- needed as the private host compositor (or pass --headless)."
	exit 2
fi

# ---------------------------------------------------------------------------
# Private sway host (headless backend, no input devices, nothing visible)
# ---------------------------------------------------------------------------
if [[ "$HEADLESS" -eq 0 ]]; then
	SWAY_CFG="$RUNDIR/sway.conf"
	cat > "$SWAY_CFG" <<-EOC
		output HEADLESS-1 resolution ${OUT_W}x${OUT_H} position 0,0
		default_border none
		focus_follows_mouse no
		seat seat0 hide_cursor 1
	EOC
	log "starting private sway (headless backend, XDG_RUNTIME_DIR=$RUNDIR)"
	# 9>&-: drop the lock's fd so a leaked sway can never hold the machine
	# lock after this script exits (pixel-regression.sh found that the hard way).
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 XDG_RUNTIME_DIR="$RUNDIR" \
		sway -c "$SWAY_CFG" > "$RUNDIR/sway.log" 2>&1 9>&- &
	SWAY_PID=$!
	waited=0
	while (( waited < SWAY_READY_TIMEOUT_S * 10 )); do
		SWAY_WL_NAME="$(find "$RUNDIR" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' -printf '%f\n' 2>/dev/null | head -1)"
		[[ -n "$SWAY_WL_NAME" ]] && break
		kill -0 "$SWAY_PID" 2>/dev/null || { log "FATAL: sway exited -- see $RUNDIR/sway.log"; cat "$RUNDIR/sway.log" >&2; exit 2; }
		sleep 0.1
		waited=$((waited + 1))
	done
	if [[ -z "$SWAY_WL_NAME" ]]; then
		log "FATAL: sway never created a wayland socket within ${SWAY_READY_TIMEOUT_S}s"
		cat "$RUNDIR/sway.log" >&2
		exit 2
	fi
	log "private sway ready: pid $SWAY_PID, socket $SWAY_WL_NAME"
fi

COMMIT="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# Audit a private COPY of the binary: another agent's build can replace
# build-release/src/gamescope mid-run, and the restart leg must relaunch the
# very same bytes the first leg ran (found the hard way: "Permission denied"
# on the relaunch while the linker was writing the file).
if ! cp "$GAMESCOPE_BIN" "$RUNDIR/gamescope" 2>/dev/null; then
	log "FATAL: could not copy $GAMESCOPE_BIN (a build in progress?) -- retry in a moment."
	exit 2
fi
cp "$GAMESCOPECTL_BIN" "$RUNDIR/gamescopectl"
chmod +x "$RUNDIR/gamescope" "$RUNDIR/gamescopectl"
SHA256="$(sha256sum "$RUNDIR/gamescope" | cut -c1-16)"
log "auditing $GAMESCOPE_BIN (sha256 $SHA256...) via a private copy"

# ---------------------------------------------------------------------------
# The driver does the rest (see settings_audit.py's header).
# ---------------------------------------------------------------------------
status=0
AUDIT_GAMESCOPE="$RUNDIR/gamescope" AUDIT_GAMESCOPECTL="$RUNDIR/gamescopectl" AUDIT_SHA256="$SHA256" \
AUDIT_RUNDIR="$RUNDIR" AUDIT_SWAY_WL="$SWAY_WL_NAME" AUDIT_CONFIGHOME="$CONFIGHOME" \
AUDIT_OUT="$OUT_DIR" AUDIT_COMMIT="$COMMIT" \
AUDIT_BACKEND="$([[ "$HEADLESS" -eq 1 ]] && echo headless || echo wayland)" \
	python3 "$DRIVER" --situations "$SITUATIONS" ${ONLY:+--only "$ONLY"} 9>&- || status=$?

log "results: $OUT_DIR/results.txt (exit $status)"
exit "$status"
