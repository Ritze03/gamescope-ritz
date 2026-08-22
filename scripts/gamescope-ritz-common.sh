#!/usr/bin/env bash
# gamescope-ritz-common.sh — shared helpers for install-gamescope-ritz.sh and
# update-gamescope-ritz.sh. Not meant to be run directly; sourced by both.
#
# Everything here exists to serve one non-negotiable rule: this project's
# install target is /usr/bin/gamescope-ritz, and /usr/bin/gamescope (the
# user's packaged, known-good gamescope 3.16.24) must NEVER be written to,
# moved, or removed by these scripts. See gcr_check_target_safety below.

set -euo pipefail

GCR_BIN_NAME="gamescope-ritz"
GCR_FORBIDDEN_TARGET="/usr/bin/gamescope"
GCR_DEFAULT_PREFIX_DIR="/usr/bin"
GCR_DEFAULT_BUILD_DIR_NAME="build-release"

gcr_err() { printf 'error: %s\n' "$*" >&2; }
gcr_info() { printf '==> %s\n' "$*"; }
gcr_warn() { printf 'warning: %s\n' "$*" >&2; }

# Resolve the repo root as the parent of the directory this file lives in
# (scripts/ is always directly under the repo root).
gcr_repo_root() {
	local script_dir
	script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)
	(cd -- "$script_dir/.." >/dev/null 2>&1 && pwd)
}

# Hard safety gate. Called with the fully-resolved (realpath -m) target path.
# Refuses to proceed unless the target's filename is exactly "gamescope-ritz",
# and refuses unconditionally if the target is literally /usr/bin/gamescope,
# no matter how a caller got there.
gcr_check_target_safety() {
	local target="$1"
	local base
	base=$(basename -- "$target")

	if [ "$target" = "$GCR_FORBIDDEN_TARGET" ]; then
		gcr_err "refusing to touch $GCR_FORBIDDEN_TARGET — that is the user's packaged gamescope, never this fork's install target."
		exit 1
	fi
	if [ "$base" != "$GCR_BIN_NAME" ]; then
		gcr_err "refusing to proceed: resolved install target '$target' does not end in '$GCR_BIN_NAME'."
		exit 1
	fi
}

gcr_is_root() { [ "$(id -u)" = "0" ]; }

# True (0) if writing to $1 (a directory) needs a privilege escalation.
gcr_need_priv_for_dir() {
	local dir="$1"
	gcr_is_root && return 1
	[ -w "$dir" ] && return 1
	return 0
}

# Run "$@" directly, or via sudo, depending on whether $GCR_PRIV_DIR needs it.
# Set GCR_PRIV_DIR before calling.
gcr_as_priv() {
	if gcr_need_priv_for_dir "$GCR_PRIV_DIR"; then
		if ! command -v sudo >/dev/null 2>&1; then
			gcr_err "'$GCR_PRIV_DIR' is not writable by $(id -un) and 'sudo' is not installed."
			gcr_err "Re-run this script as root, or install sudo, to write to that path."
			exit 1
		fi
		sudo "$@"
	else
		"$@"
	fi
}

# Ask a yes/no question. Honours GCR_ASSUME_YES=1 (always yes, no prompt).
# $1 = prompt text, $2 = default ("y" or "n").
gcr_confirm() {
	local prompt="$1" default="${2:-n}" reply
	if [ "${GCR_ASSUME_YES:-0}" = "1" ]; then
		return 0
	fi
	if [ ! -t 0 ]; then
		gcr_err "not an interactive terminal and no --yes given; refusing to guess on: $prompt"
		exit 1
	fi
	local hint="y/N"
	[ "$default" = "y" ] && hint="Y/n"
	read -r -p "$prompt [$hint] " reply || true
	reply="${reply:-$default}"
	case "$reply" in
		[Yy]|[Yy][Ee][Ss]) return 0 ;;
		*) return 1 ;;
	esac
}

# The path to the compiled binary inside a release build directory. Fixed by
# this project's meson.build (src/meson.build declares executable('gamescope', ...)),
# verified against this tree — if that ever moves, update this one line.
gcr_release_binary() {
	local build_dir="$1"
	printf '%s/src/gamescope\n' "$build_dir"
}

# Refuse to build as root: a root-owned build/ directory silently breaks the
# developer's normal (non-root) `meson compile` / `ninja` workflow afterwards.
gcr_refuse_root_build() {
	if gcr_is_root; then
		gcr_err "refusing to run the build as root — it would leave root-owned files in the build directory."
		gcr_err "Re-run this script as your normal user; only the final install step needs sudo, and this script asks for it only when it's needed."
		exit 1
	fi
}

# Configure (if needed) and build the release binary. Never runs as root.
# "Highest optimisations": buildtype=release + -Doptimization=3, plus LTO
# (-Db_lto=true). LTO was verified (2026-08-22) to build cleanly and to leave
# `meson test` fully green on this tree, adding negligible wall-clock time
# (~17s for a from-scratch 10-core build of just the gamescope binary) — so it
# stays on. If a future LTO bump ever regresses build time or breaks the
# build, drop -Db_lto=true here rather than fighting it: working beats maximal.
gcr_build_release() {
	local repo_root="$1" build_dir="$2"
	gcr_refuse_root_build

	if [ -f "$build_dir/build.ninja" ]; then
		gcr_info "reconfiguring existing $build_dir to make sure release flags are applied..."
		( cd -- "$repo_root" && meson setup --reconfigure "$build_dir" \
			--buildtype=release -Doptimization=3 -Db_lto=true )
	else
		gcr_info "configuring a release build in $build_dir (buildtype=release, optimization=3, LTO)..."
		( cd -- "$repo_root" && meson setup "$build_dir" \
			--buildtype=release -Doptimization=3 -Db_lto=true )
	fi

	gcr_info "building (ninja -C $build_dir)..."
	ninja -C "$build_dir" src/gamescope
}

# Run default_extras_install.sh (installs scripts/, looks/, and reshade/)
# against $prefix_root/share/gamescope-ritz. Namespaced by binary name so it
# is its own directory, never $prefix_root/share/gamescope -- that path
# belongs to any other gamescope install on the system, including a
# distro-packaged /usr/bin/gamescope, and default_extras_install.sh's own
# safety guard now refuses to rm -rf anything outside share/gamescope-ritz.
gcr_install_extras() {
	local repo_root="$1" prefix_root="$2"
	local script="$repo_root/default_extras_install.sh"
	if [ ! -f "$script" ]; then
		gcr_warn "default_extras_install.sh not found at $script, skipping extras."
		return 0
	fi
	gcr_info "installing scripts/, looks/ and reshade/ to ${prefix_root}/share/gamescope-ritz ..."
	GCR_PRIV_DIR="$prefix_root" gcr_as_priv env \
		MESON_SOURCE_ROOT="$repo_root" \
		MESON_INSTALL_PREFIX="$prefix_root" \
		DESTDIR="" \
		sh "$script"
}

# Path to gamescope-ritz's own namespaced data directory under a given
# prefix root -- what --uninstall removes, and nothing else (never plain
# $prefix_root/share/gamescope, which may belong to a different install).
gcr_extras_dir() {
	local prefix_root="$1"
	printf '%s/share/gamescope-ritz\n' "$prefix_root"
}
