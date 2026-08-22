#!/usr/bin/env bash
# install-gamescope-ritz.sh — interactive installer for this fork.
#
# Installs to /usr/bin/gamescope-ritz, NEVER /usr/bin/gamescope (the user's
# packaged, known-good gamescope 3.16.24 and their daily driver — this
# script hard-refuses to touch it; see gamescope-ritz-common.sh).
#
# Interactively asks whether to:
#   - symlink /usr/bin/gamescope-ritz -> the release build's binary (a
#     rebuild is then instantly live, no reinstall needed — but deleting or
#     moving this repo breaks the link), or
#   - copy the binary in (install is then independent of the repo).
#
# If no release build exists yet, builds one first: --buildtype=release
# -Doptimization=3 -Db_lto=true, into build-release/ (a separate directory
# from the developer's debug build/, which is left untouched).
#
# Usage:
#   scripts/install-gamescope-ritz.sh [options]
#
# Options:
#   --link              symlink mode, non-interactive
#   --copy              copy mode, non-interactive
#   --yes, -y           assume "yes" to all prompts (extras included)
#   --extras            install/update scripts+looks extras, no prompt
#   --no-extras         skip the extras step, no prompt
#   --rebuild           rebuild even if a release binary already exists
#   --prefix DIR        install directory (default: /usr/bin)
#   --build-dir DIR     release build directory (default: build-release)
#   --uninstall         remove /usr/bin/gamescope-ritz and its
#                       share/gamescope-ritz data dir, and exit
#   -h, --help          show this help and exit
#
# Examples:
#   scripts/install-gamescope-ritz.sh                 # ask everything
#   scripts/install-gamescope-ritz.sh --link --yes     # scripted symlink install
#   scripts/install-gamescope-ritz.sh --uninstall --yes

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)
# shellcheck source=./gamescope-ritz-common.sh
source "$SCRIPT_DIR/gamescope-ritz-common.sh"

MODE=""            # "link" or "copy"
GCR_ASSUME_YES=0
EXTRAS=""           # "" = ask, "yes", "no"
REBUILD=0
PREFIX_DIR="$GCR_DEFAULT_PREFIX_DIR"
BUILD_DIR_NAME="$GCR_DEFAULT_BUILD_DIR_NAME"
DO_UNINSTALL=0

print_help() { sed -n '2,37p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
	case "$1" in
		--link) MODE="link" ;;
		--copy) MODE="copy" ;;
		--yes|-y) GCR_ASSUME_YES=1 ;;
		--extras) EXTRAS="yes" ;;
		--no-extras) EXTRAS="no" ;;
		--rebuild) REBUILD=1 ;;
		--prefix) PREFIX_DIR="$2"; shift ;;
		--build-dir) BUILD_DIR_NAME="$2"; shift ;;
		--uninstall) DO_UNINSTALL=1 ;;
		-h|--help) print_help; exit 0 ;;
		*) gcr_err "unknown option: $1"; print_help; exit 1 ;;
	esac
	shift
done
export GCR_ASSUME_YES

REPO_ROOT=$(gcr_repo_root)
BUILD_DIR="$REPO_ROOT/$BUILD_DIR_NAME"
# Resolve only the directory, not the final component: if gamescope-ritz is
# already installed as a symlink, realpath'ing the whole path would silently
# follow it to the build binary and trip the basename safety check below.
PREFIX_DIR=$(realpath -m -- "$PREFIX_DIR")
TARGET="$PREFIX_DIR/$GCR_BIN_NAME"
gcr_check_target_safety "$TARGET"

if [ "$DO_UNINSTALL" = "1" ]; then
	PREFIX_ROOT=$(dirname -- "$PREFIX_DIR")
	EXTRAS_DIR=$(gcr_extras_dir "$PREFIX_ROOT")

	if [ ! -e "$TARGET" ] && [ ! -L "$TARGET" ] && [ ! -e "$EXTRAS_DIR" ]; then
		gcr_info "$TARGET does not exist and $EXTRAS_DIR does not exist, nothing to uninstall."
		exit 0
	fi

	if [ -e "$TARGET" ] || [ -L "$TARGET" ]; then
		if [ -L "$TARGET" ]; then
			gcr_info "$TARGET is a symlink -> $(readlink -f -- "$TARGET" 2>/dev/null || readlink -- "$TARGET")"
		else
			gcr_info "$TARGET is a regular file ($(du -h -- "$TARGET" | cut -f1))."
		fi
		gcr_confirm "Remove $TARGET?" y || { gcr_info "aborted, nothing removed."; exit 1; }
		GCR_PRIV_DIR="$PREFIX_DIR"
		gcr_as_priv rm -f -- "$TARGET"
		gcr_info "removed $TARGET."
	fi

	if [ -e "$EXTRAS_DIR" ]; then
		# Safety check mirrors gcr_check_target_safety's spirit: only ever
		# remove our own namespaced data directory, never plain
		# $PREFIX_ROOT/share/gamescope (a distro-packaged gamescope's).
		case "$EXTRAS_DIR" in
			*/share/gamescope-ritz)
				if gcr_confirm "Also remove $EXTRAS_DIR (scripts/looks/reshade extras)?" y; then
					GCR_PRIV_DIR="$PREFIX_ROOT"
					gcr_as_priv rm -rf -- "$EXTRAS_DIR"
					gcr_info "removed $EXTRAS_DIR."
				else
					gcr_info "left $EXTRAS_DIR in place."
				fi
				;;
			*)
				gcr_err "internal error: refusing to remove unexpected extras dir '$EXTRAS_DIR'"
				exit 1
				;;
		esac
	else
		gcr_info "$EXTRAS_DIR does not exist, nothing to remove there."
	fi
	exit 0
fi

gcr_info "gamescope-ritz installer"
gcr_info "repo:   $REPO_ROOT"
gcr_info "target: $TARGET"

RELEASE_BIN=$(gcr_release_binary "$BUILD_DIR")
if [ ! -x "$RELEASE_BIN" ] || [ "$REBUILD" = "1" ]; then
	if [ ! -x "$RELEASE_BIN" ]; then
		gcr_info "no release build found at $RELEASE_BIN — building one now."
	else
		gcr_info "--rebuild given — rebuilding $RELEASE_BIN."
	fi
	gcr_build_release "$REPO_ROOT" "$BUILD_DIR"
else
	gcr_info "found existing release build: $RELEASE_BIN"
fi

if [ ! -x "$RELEASE_BIN" ]; then
	gcr_err "build finished but $RELEASE_BIN was not produced. Aborting."
	exit 1
fi

if [ -z "$MODE" ]; then
	if [ "${GCR_ASSUME_YES:-0}" = "1" ]; then
		MODE="copy"
	else
		echo
		echo "How should $TARGET be installed?"
		echo "  1) symlink -> $RELEASE_BIN   (rebuilds go live instantly; breaks if you move/delete this repo)"
		echo "  2) copy    (independent of this repo; re-run this installer/updater to refresh)"
		read -r -p "Choose [1/2]: " choice
		case "$choice" in
			1) MODE="link" ;;
			2) MODE="copy" ;;
			*) gcr_err "invalid choice: $choice"; exit 1 ;;
		esac
	fi
fi

case "$MODE" in
	link)
		gcr_info "about to symlink $TARGET -> $RELEASE_BIN"
		;;
	copy)
		gcr_info "about to copy $RELEASE_BIN -> $TARGET"
		;;
	*)
		gcr_err "internal error: unknown mode '$MODE'"; exit 1 ;;
esac
gcr_confirm "Proceed writing to $TARGET?" y || { gcr_info "aborted, nothing installed."; exit 1; }

GCR_PRIV_DIR="$PREFIX_DIR"
gcr_as_priv mkdir -p -- "$PREFIX_DIR"
case "$MODE" in
	link)
		gcr_as_priv ln -sf -- "$RELEASE_BIN" "$TARGET"
		;;
	copy)
		gcr_as_priv cp -f -- "$RELEASE_BIN" "$TARGET"
		;;
esac
gcr_info "installed: $TARGET ($MODE mode)"

PREFIX_ROOT=$(dirname -- "$PREFIX_DIR")
if [ -z "$EXTRAS" ]; then
	echo
	echo "default_extras_install.sh copies this repo's scripts/, looks/ and"
	echo "reshade/ into ${PREFIX_ROOT}/share/gamescope-ritz — namespaced by"
	echo "binary name, so it never touches a distro-packaged /usr/bin/gamescope's"
	echo "own share/gamescope. Needed for gamescope-ritz's scripts/ReShade/looks"
	echo "features to find their files."
	if gcr_confirm "Run it now?" n; then EXTRAS="yes"; else EXTRAS="no"; fi
fi
if [ "$EXTRAS" = "yes" ]; then
	gcr_install_extras "$REPO_ROOT" "$PREFIX_ROOT"
else
	gcr_info "skipped extras (scripts/looks). Re-run with --extras later if needed."
fi

echo
gcr_info "done. Run: $TARGET --help"
if [ "$MODE" = "link" ]; then
	gcr_warn "symlink mode: moving or deleting $REPO_ROOT will break $TARGET."
fi
