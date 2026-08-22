#!/usr/bin/env bash
# update-gamescope-ritz.sh — pull, rebuild release, and refresh the install
# made by install-gamescope-ritz.sh.
#
# 1. git pull --ff-only (refuses on local changes or a non-fast-forward
#    remote; never stashes, resets, or otherwise discards your work).
# 2. rebuilds the release binary (--buildtype=release -Doptimization=3
#    -Db_lto=true) into its build directory.
# 3. if /usr/bin/gamescope-ritz is a symlink, does nothing more — it already
#    points at the binary just rebuilt. If it's a regular file (copy mode),
#    copies the fresh binary over it. Never touches /usr/bin/gamescope.
#
# Usage:
#   scripts/update-gamescope-ritz.sh [options]
#
# Options:
#   --yes, -y           assume "yes" to all prompts (extras included)
#   --extras            also refresh scripts+looks extras, no prompt
#   --no-extras         skip the extras step, no prompt
#   --prefix DIR        install directory to look for gamescope-ritz in (default: /usr/bin)
#   --build-dir DIR     release build directory, copy-mode only (default: build-release;
#                       symlink mode always rebuilds whatever the link already points at)
#   --allow-dirty       proceed even with uncommitted local changes (git pull may still
#                       refuse; this only skips this script's own pre-check)
#   -h, --help          show this help and exit

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)
# shellcheck source=./gamescope-ritz-common.sh
source "$SCRIPT_DIR/gamescope-ritz-common.sh"

GCR_ASSUME_YES=0
EXTRAS=""
PREFIX_DIR="$GCR_DEFAULT_PREFIX_DIR"
BUILD_DIR_NAME="$GCR_DEFAULT_BUILD_DIR_NAME"
ALLOW_DIRTY=0

print_help() { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
	case "$1" in
		--yes|-y) GCR_ASSUME_YES=1 ;;
		--extras) EXTRAS="yes" ;;
		--no-extras) EXTRAS="no" ;;
		--prefix) PREFIX_DIR="$2"; shift ;;
		--build-dir) BUILD_DIR_NAME="$2"; shift ;;
		--allow-dirty) ALLOW_DIRTY=1 ;;
		-h|--help) print_help; exit 0 ;;
		*) gcr_err "unknown option: $1"; print_help; exit 1 ;;
	esac
	shift
done
export GCR_ASSUME_YES

REPO_ROOT=$(gcr_repo_root)
# Resolve only the directory, not the final component — see the matching
# comment in install-gamescope-ritz.sh: an already-installed symlink must not
# be dereferenced here, or the basename safety check below sees the build
# binary's name instead of "gamescope-ritz".
PREFIX_DIR=$(realpath -m -- "$PREFIX_DIR")
TARGET="$PREFIX_DIR/$GCR_BIN_NAME"
gcr_check_target_safety "$TARGET"

gcr_info "gamescope-ritz updater"
gcr_info "repo:   $REPO_ROOT"
gcr_info "target: $TARGET"

if [ ! -e "$TARGET" ] && [ ! -L "$TARGET" ]; then
	gcr_err "$TARGET does not exist. Run scripts/install-gamescope-ritz.sh first."
	exit 1
fi

if [ -L "$TARGET" ]; then
	INSTALL_MODE="symlink"
elif [ -f "$TARGET" ]; then
	INSTALL_MODE="copy"
else
	gcr_err "$TARGET exists but is neither a symlink nor a regular file. Refusing to guess."
	exit 1
fi
gcr_info "detected install mode: $INSTALL_MODE"

# --- step 1: git pull, refusing to discard anything ------------------------
if [ "$ALLOW_DIRTY" != "1" ]; then
	if [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
		gcr_err "uncommitted changes in $REPO_ROOT — refusing to pull over them."
		gcr_err "Commit or stash your changes, or re-run with --allow-dirty to skip only this check"
		gcr_err "(git pull --ff-only below will still refuse a non-fast-forward on its own)."
		git -C "$REPO_ROOT" status --short >&2
		exit 1
	fi
fi

gcr_info "git pull --ff-only ..."
if ! git -C "$REPO_ROOT" pull --ff-only; then
	gcr_err "git pull failed (conflict, diverged history, or network error)."
	gcr_err "Nothing was built or installed. Resolve the git state by hand and re-run."
	exit 1
fi

# --- step 2: rebuild release -------------------------------------------------
if [ "$INSTALL_MODE" = "symlink" ]; then
	# Rebuild whatever the symlink already points at, not whatever --build-dir
	# defaults to, so a custom --build-dir given at install time is honoured.
	LINK_TARGET=$(readlink -f -- "$TARGET")
	BUILD_DIR=$(dirname -- "$(dirname -- "$LINK_TARGET")")
	gcr_info "symlink points at $LINK_TARGET -> rebuilding $BUILD_DIR"
else
	BUILD_DIR="$REPO_ROOT/$BUILD_DIR_NAME"
fi

gcr_build_release "$REPO_ROOT" "$BUILD_DIR"
RELEASE_BIN=$(gcr_release_binary "$BUILD_DIR")
if [ ! -x "$RELEASE_BIN" ]; then
	gcr_err "build finished but $RELEASE_BIN was not produced. Aborting."
	exit 1
fi

# --- step 3: refresh the install --------------------------------------------
if [ "$INSTALL_MODE" = "symlink" ]; then
	gcr_info "symlink mode: $TARGET already points at the binary just rebuilt, nothing to copy."
else
	gcr_info "copy mode: copying $RELEASE_BIN -> $TARGET"
	GCR_PRIV_DIR="$PREFIX_DIR"
	gcr_as_priv cp -f -- "$RELEASE_BIN" "$TARGET"
fi

# --- extras -------------------------------------------------------------
PREFIX_ROOT=$(dirname -- "$PREFIX_DIR")
if [ -z "$EXTRAS" ]; then
	echo
	echo "Also refresh scripts/, looks/ and reshade/ in ${PREFIX_ROOT}/share/gamescope-ritz?"
	echo "(namespaced by binary name — never touches a distro-packaged"
	echo "/usr/bin/gamescope's own share/gamescope — see install-gamescope-ritz.sh"
	echo "for details.)"
	if gcr_confirm "Run default_extras_install.sh now?" n; then EXTRAS="yes"; else EXTRAS="no"; fi
fi
if [ "$EXTRAS" = "yes" ]; then
	gcr_install_extras "$REPO_ROOT" "$PREFIX_ROOT"
else
	gcr_info "skipped extras refresh. Re-run with --extras later if needed."
fi

echo
gcr_info "done. $TARGET is up to date."
