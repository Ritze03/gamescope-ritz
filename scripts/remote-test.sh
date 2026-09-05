#!/usr/bin/env bash
# remote-test.sh — build gamescope-ritz HERE, ship the binary to the remote test
# laptop, and run it there with a real compositor. Exists because this desktop can
# only test headlessly: no visible overlay, no real force-grab, no soak testing, and
# two past runs put a test window on the user's own display by accident.
#
# WHY BUILD HERE, NOT THERE
#   The laptop is an Intel Kaby Lake-U part; a full LTO release build would take a
#   long time and make the machine unusable while it ran. This project's build
#   already compiles at the GCC default (generic x86-64 baseline, no -march flag
#   anywhere in meson.build or this repo's build scripts — verified 2026-09-02:
#   `readelf -n` on build-release/src/gamescope reports "x86 ISA used:
#   x86-64-baseline"), so the binary this script ships is already safe to run on
#   any x86_64 machine, the laptop included. Nothing here pins -march explicitly;
#   there is simply nothing riskier than baseline to pin away from.
#
# WHAT GETS SHIPPED
#   Only build-release/src/gamescope itself by default. gamescopectl is a separate,
#   distro-packaged binary (owned by the `gamescope-git` pacman package, not this
#   repo's meson build) and is already present on the laptop the same way it is
#   here — nothing to transfer. Lua config scripts and looks are optional at
#   runtime (gamescope fails safe if their directories don't exist) so they are
#   not shipped unless --extras is given. The bundled shader effects are compiled
#   into the binary, so there is no reshade/ tree to ship.
#
# TWO SSH FACTS THIS SCRIPT EXISTS TO HIDE
#   1. Every `ssh host 'cmd'` is a fresh, non-interactive shell — none of the
#      target session's WAYLAND_DISPLAY / XDG_RUNTIME_DIR / HYPRLAND_INSTANCE_SIGNATURE
#      are inherited. They have to be read from the live compositor's own /proc
#      environ and re-exported on every single command.
#   2. A long-running remote process dies with its SSH session unless detached.
#      `run` backgrounds the target command with `setsid` (new session, so it
#      doesn't get the terminal's SIGHUP) and redirects all three std streams away
#      from the SSH pty, so the ssh invocation returns immediately and the remote
#      process keeps going after this script exits.
#
# USAGE
#   scripts/remote-test.sh sync [--extras] [--no-build]
#       Build locally (release; niced +10 / ionice idle by the shared build
#       helper — see gcr_build in gamescope-ritz-common.sh), then rsync the
#       binary (and, with --extras, scripts/looks) to the remote test
#       dir. --no-build skips the local build and ships whatever's already
#       there.
#
#   scripts/remote-test.sh run [--wait] -- <command...>
#       Run <command> on the remote host with the compositor's env exported and
#       the remote bin dir on PATH, detached so it survives this script exiting.
#       Output goes to a timestamped log under the remote captures dir; the log
#       path is printed. --wait runs it attached instead (foreground, for short
#       commands like --version).
#
#   scripts/remote-test.sh screenshot <remote-path.png> [local-path.png]
#       Capture via `gamescopectl screenshot "<remote-path> 4"` (type 4 =
#       screen_buffer — the default type truncates zpos >= 2, and the overlay
#       sits at zpos 6; gamescopectl also silently collapses trailing args if
#       the path+type aren't one quoted argument, so this is done for you) and
#       scp it back to local-path (default: ./<basename> in cwd).
#
#   scripts/remote-test.sh env
#       Print the `export ...` lines this script resolved for the live
#       compositor, for a caller who wants to ssh in by hand.
#
# CONFIG (env overrides)
#   GCR_REMOTE_HOST   default: mo@192.0.2.167
#   GCR_REMOTE_DIR    default: ~/gamescope-ritz-remote  (binary + captures/ live here)
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)
# shellcheck source=./gamescope-ritz-common.sh
source "$SCRIPT_DIR/gamescope-ritz-common.sh"

REMOTE_HOST="${GCR_REMOTE_HOST:-mo@192.0.2.167}"
REMOTE_DIR="${GCR_REMOTE_DIR:-~/gamescope-ritz-remote}"
SSH=(ssh -o BatchMode=yes "$REMOTE_HOST")
REPO_ROOT=$(gcr_repo_root)
# GCR_DEFAULT_BUILD_DIR_NAME already honours a GCR_BUILD_DIR override (see
# gamescope-ritz-common.sh) — read it through there rather than hardcoding
# build-release, so `GCR_BUILD_DIR=build-laptop scripts/remote-test.sh sync`
# builds and ships that directory's binary, never the user's own launch path.
LOCAL_BIN=$(gcr_release_binary "$REPO_ROOT/$GCR_DEFAULT_BUILD_DIR_NAME")

print_help() { sed -n '2,55p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# Resolve the live compositor's WAYLAND_DISPLAY / XDG_RUNTIME_DIR /
# HYPRLAND_INSTANCE_SIGNATURE by reading them out of the running Hyprland
# process's own environ on the remote host, and print them as `export` lines.
# Nothing here is cached — re-resolved every call, since it's cheap and a
# stale signature after a compositor restart is a worse failure than a
# redundant ssh round-trip.
remote_env_exports() {
	"${SSH[@]}" '
		set -e
		pid=$(pgrep -x Hyprland | head -1)
		if [ -z "$pid" ]; then
			echo "remote-test: no live Hyprland process found on the remote host" >&2
			exit 1
		fi
		rtdir=$(tr "\0" "\n" < /proc/$pid/environ | sed -n "s/^XDG_RUNTIME_DIR=//p")
		if [ -z "$rtdir" ]; then
			echo "remote-test: could not read XDG_RUNTIME_DIR from Hyprland (pid $pid) environ" >&2
			exit 1
		fi
		wdisplay=""
		for child in $(pgrep -P "$pid") $pid; do
			wdisplay=$(tr "\0" "\n" < "/proc/$child/environ" 2>/dev/null | sed -n "s/^WAYLAND_DISPLAY=//p")
			[ -n "$wdisplay" ] && break
		done
		if [ -z "$wdisplay" ]; then
			echo "remote-test: could not find WAYLAND_DISPLAY on any Hyprland child (pid $pid)" >&2
			exit 1
		fi
		sig=$(ls "$rtdir/hypr/" 2>/dev/null | head -1)
		echo "export XDG_RUNTIME_DIR=$rtdir"
		echo "export WAYLAND_DISPLAY=$wdisplay"
		[ -n "$sig" ] && echo "export HYPRLAND_INSTANCE_SIGNATURE=$sig"
	'
}

cmd_env() { remote_env_exports; }

cmd_sync() {
	local do_build=1 extras=0
	while [ $# -gt 0 ]; do
		case "$1" in
			--no-build) do_build=0 ;;
			--extras) extras=1 ;;
			*) gcr_err "sync: unknown option: $1"; exit 1 ;;
		esac
		shift
	done

	if [ "$do_build" = "1" ]; then
		gcr_info "building locally (release; niced +10 by the shared build helper)..."
		"$SCRIPT_DIR/build-gamescope-ritz.sh" --release
	fi
	[ -x "$LOCAL_BIN" ] || { gcr_err "no binary at $LOCAL_BIN — run without --no-build, or build first."; exit 1; }

	gcr_info "rsyncing binary to $REMOTE_HOST:$REMOTE_DIR ..."
	"${SSH[@]}" "mkdir -p $REMOTE_DIR/captures"
	rsync -avz --progress "$LOCAL_BIN" "$REMOTE_HOST:$REMOTE_DIR/gamescope-ritz"
	"${SSH[@]}" "chmod +x $REMOTE_DIR/gamescope-ritz"

	if [ "$extras" = "1" ]; then
		gcr_info "rsyncing scripts/looks extras..."
		for d in scripts looks; do
			[ -d "$REPO_ROOT/$d" ] && rsync -avz --delete "$REPO_ROOT/$d/" "$REMOTE_HOST:$REMOTE_DIR/$d/"
		done
	fi

	gcr_info "verifying the transferred binary actually runs..."
	local ver
	ver=$("${SSH[@]}" "$REMOTE_DIR/gamescope-ritz --version" 2>&1) || {
		gcr_err "transferred binary failed to run on $REMOTE_HOST:"
		printf '%s\n' "$ver" >&2
		gcr_err "check 'ldd $REMOTE_DIR/gamescope-ritz' on the remote host for missing libraries."
		exit 1
	}
	gcr_info "remote binary reports: $ver"
}

cmd_run() {
	local wait=0
	while [ $# -gt 0 ]; do
		case "$1" in
			--wait) wait=1; shift ;;
			--) shift; break ;;
			*) break ;;
		esac
	done
	[ $# -gt 0 ] || { gcr_err "run: no command given (usage: run [--wait] -- <command...>)"; exit 1; }

	local exports
	exports=$(remote_env_exports) || exit 1

	# Quote the command back together as one string to hand to the remote shell.
	local cmd
	printf -v cmd '%q ' "$@"

	if [ "$wait" = "1" ]; then
		"${SSH[@]}" "$exports; export PATH=$REMOTE_DIR:\$PATH; $cmd"
	else
		local logfile="$REMOTE_DIR/captures/run-$(date +%Y%m%d-%H%M%S).log"
		gcr_info "launching detached on $REMOTE_HOST, log: $logfile"
		"${SSH[@]}" "$exports; export PATH=$REMOTE_DIR:\$PATH; setsid nohup $cmd > $logfile 2>&1 < /dev/null & disown; sleep 1; echo launched"
		gcr_info "tail it with: ssh $REMOTE_HOST tail -f $logfile"
	fi
}

cmd_screenshot() {
	local remote_path="${1:-}" local_path="${2:-}"
	[ -n "$remote_path" ] || { gcr_err "screenshot: no remote path given"; exit 1; }
	[ -n "$local_path" ] || local_path="./$(basename -- "$remote_path")"

	local exports
	exports=$(remote_env_exports) || exit 1

	gcr_info "capturing (type 4 = screen_buffer, so zpos>=2 layers like the overlay aren't truncated)..."
	# The path+type MUST be one quoted argument to gamescopectl, or it silently
	# collapses trailing args and drops the type. Do not "simplify" this.
	"${SSH[@]}" "$exports; gamescopectl screenshot \"$remote_path 4\""

	gcr_info "copying back to $local_path ..."
	scp -q "$REMOTE_HOST:$remote_path" "$local_path"
	gcr_info "saved: $local_path"
}

[ $# -gt 0 ] || { print_help; exit 1; }
subcmd="$1"; shift
case "$subcmd" in
	sync) cmd_sync "$@" ;;
	run) cmd_run "$@" ;;
	screenshot) cmd_screenshot "$@" ;;
	env) cmd_env "$@" ;;
	-h|--help) print_help ;;
	*) gcr_err "unknown subcommand: $subcmd"; print_help; exit 1 ;;
esac
