#!/usr/bin/env bash
# install.sh — single entry point for install / update / remove of this fork,
# from a fresh clone. Same core as scripts/install-gamescope-ritz.sh and
# scripts/update-gamescope-ritz.sh: it sources scripts/gamescope-ritz-common.sh
# and reuses its build, privilege-escalation, submodule and safety helpers
# rather than re-implementing any of them.
#
# Installs to /usr/bin/gamescope-ritz, NEVER /usr/bin/gamescope (the user's
# packaged, known-good gamescope — this hard-refuses to touch it; see
# gamescope-ritz-common.sh's gcr_check_target_safety).
#
# Before any build, checks that wlroots is actually usable the way meson's
# build asks for it: pkg-config for the exact module + version constraint
# read out of src/meson.build (a package-manager "installed" is not proof
# pkg-config/meson can see it). On Arch/CachyOS the package is `wlroots0.20`
# — if pkg-config can't find it, this also asks pacman whether it's installed
# at all (a different problem than "installed but not visible to pkg-config")
# and prints the exact fix. On any other distro it names the pkg-config
# module and version needed and lets you find the right package yourself.
# If pkg-config can't see a usable wlroots but this repo's vendored fallback
# (subprojects/wlroots, meson's own `fallback:` for this dependency) is
# checked out, that's reported too but is NOT a failure — meson will just
# build the vendored copy (slower first build). Only a genuinely
# unsatisfiable case (no pkg-config hit, no vendored fallback available)
# fails, and it fails before the build starts, not 200 lines into a compile.
#
# Usage:
#   ./install.sh [--install|--remove|--update] [options]
#
# Actions (choose at most one; no action = interactive menu):
#   --install           check deps, build a release binary if needed, install
#                       it (symlink or copy — asked interactively unless
#                       --link/--copy is given). If a Ritz install is
#                       detected, also offers to install this repo's Ritz
#                       launcher extension.
#   --remove            uninstall the binary/symlink this installed, and any
#                       extras (scripts/looks under share/gamescope-ritz).
#                       Also offers to remove the Ritz extension manifest
#                       this installed, if present — nothing else under
#                       ~/.config/ritz/extensions is touched. Never touches
#                       ~/.config/gamescope-ritz.
#   --update            git pull --ff-only (refuses on a dirty tree unless
#                       --allow-dirty), rebuild, and reinstall by whichever
#                       method (symlink/copy) is already in place — a
#                       symlink install needs no copy step, the rebuilt
#                       binary is live immediately. Also offers to refresh
#                       the Ritz extension manifest if this previously
#                       installed one and the repo's copy has changed.
#   -h, --help          show this help and exit
#
# Options:
#   --link              symlink mode for --install, non-interactive
#                       (target -> the built binary; a later --update just
#                       rebuilds and the installed name is live immediately,
#                       no root needed again — but moving/deleting this repo
#                       breaks it)
#   --copy              copy mode for --install, non-interactive (target is
#                       independent of this repo; --update copies again)
#   --yes, -y           assume "yes" to all confirmation prompts
#   --extras            install/refresh scripts+looks extras, no prompt
#   --no-extras         skip the extras step, no prompt
#   --rebuild           (--install) rebuild even if a release binary exists
#   --allow-dirty       (--update) proceed despite uncommitted local changes
#                       (git pull --ff-only can still refuse on its own)
#   --prefix DIR        install directory (default: /usr/bin) — override to
#                       install/remove/update into a scratch prefix, e.g. for
#                       testing this script without touching the real system
#   --build-dir DIR     release build directory name, relative to the repo
#                       root (default: build-release)
#   --with-ritz-extension  copy this repo's Ritz launcher extension manifest
#                       (extensions/gamescope-ritz.json) into
#                       ~/.config/ritz/extensions/ (or refresh it there on
#                       --update), no prompt. Only does anything if a Ritz
#                       install is detected (~/.config/ritz exists, or the
#                       `ritz` binary is on PATH).
#   --no-ritz-extension skip/decline the Ritz extension step, no prompt
#
# Examples:
#   ./install.sh                          # detect state, offer a menu
#   ./install.sh --install                # ask link/copy, build if needed
#   ./install.sh --install --link --yes   # scripted symlink install
#   ./install.sh --update                 # pull, rebuild, reinstall in place
#   ./install.sh --remove --yes

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)
# shellcheck source=./scripts/gamescope-ritz-common.sh
source "$SCRIPT_DIR/scripts/gamescope-ritz-common.sh"

ACTION=""           # "install", "remove" or "update"; "" = interactive menu
MODE=""             # "link" or "copy" (--install only)
GCR_ASSUME_YES=0
EXTRAS=""           # "" = ask, "yes", "no"
REBUILD=0
PREFIX_DIR="$GCR_DEFAULT_PREFIX_DIR"
BUILD_DIR_NAME="$GCR_DEFAULT_BUILD_DIR_NAME"
ALLOW_DIRTY=0
RITZ_EXT=""         # "" = ask, "yes", "no" (--with-ritz-extension / --no-ritz-extension)

print_help() { sed -n '2,77p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

set_action() {
	if [ -n "$ACTION" ] && [ "$ACTION" != "$1" ]; then
		gcr_err "choose at most one of --install / --remove / --update (got --$ACTION and --$1)."
		exit 1
	fi
	ACTION="$1"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--install) set_action "install" ;;
		--remove|--uninstall) set_action "remove" ;;
		--update) set_action "update" ;;
		--link) MODE="link" ;;
		--copy) MODE="copy" ;;
		--yes|-y) GCR_ASSUME_YES=1 ;;
		--extras) EXTRAS="yes" ;;
		--no-extras) EXTRAS="no" ;;
		--rebuild) REBUILD=1 ;;
		--allow-dirty) ALLOW_DIRTY=1 ;;
		--with-ritz-extension) RITZ_EXT="yes" ;;
		--no-ritz-extension) RITZ_EXT="no" ;;
		--prefix) PREFIX_DIR="$2"; shift ;;
		--build-dir) BUILD_DIR_NAME="$2"; shift ;;
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

# --- wlroots dependency check ------------------------------------------------
# Reads the pkg-config module name and version constraints straight out of
# src/meson.build's `wlroots_dep = dependency(...)` call, so this can't drift
# from what the build actually asks for. Sets GCR_WLROOTS_STATE to one of
# "ok" (system package satisfies it), "vendored-fallback" (not satisfied, but
# meson's own fallback subproject is checked out and will be built instead —
# not a failure) or "fail" (neither available — must not proceed to a build).
# GCR_WLROOTS_MSG is the human-readable status/explanation to print.
gcr_wlroots_pc_module() {
	awk '/wlroots_dep[ \t]*=[ \t]*dependency\(/ { getline; gsub(/[ \t,\x27]/, ""); print; exit }' \
		"$1/src/meson.build" 2>/dev/null
}

gcr_wlroots_pc_constraints() {
	# One bare constraint per line, e.g. ">= 0.20.0"
	local meson_file="$1/src/meson.build"
	awk '/wlroots_dep[ \t]*=[ \t]*dependency\(/ { f=1 } f && /version:/ { print; exit }' "$meson_file" 2>/dev/null \
		| grep -oE "'[^']+'" | tr -d "'"
}

gcr_check_wlroots() {
	local repo_root="$1" module constraints=() c args=() line ver

	module=$(gcr_wlroots_pc_module "$repo_root")
	[ -n "$module" ] || module="wlroots-0.20"   # meson.build changed shape; degrade gracefully

	while IFS= read -r c; do
		[ -n "$c" ] && constraints+=("$c") && args+=("$module $c")
	done < <(gcr_wlroots_pc_constraints "$repo_root")
	[ "${#args[@]}" -gt 0 ] || args=("$module")
	local need; need=$(IFS=', '; printf '%s' "${constraints[*]:-any version}")

	if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "${args[@]}"; then
		ver=$(pkg-config --modversion "$module" 2>/dev/null || true)
		GCR_WLROOTS_STATE="ok"
		GCR_WLROOTS_MSG="OK — pkg-config finds '$module'${ver:+ ($ver)}, satisfying $need."
		return 0
	fi

	local howto
	if ! command -v pkg-config >/dev/null 2>&1; then
		howto="'pkg-config' itself is not installed, so this cannot even check. Install pkg-config (or pkgconf) first."
	elif command -v pacman >/dev/null 2>&1; then
		if pacman -Q wlroots0.20 >/dev/null 2>&1; then
			local pv; pv=$(pacman -Q wlroots0.20 2>/dev/null | awk '{print $2}')
			howto="pacman shows wlroots0.20 ($pv) installed, but pkg-config cannot find '$module' satisfying $need. Check PKG_CONFIG_PATH/PKG_CONFIG_LIBDIR, and that the installed .pc file's Version actually falls in that range."
		elif pacman -Si wlroots0.20 >/dev/null 2>&1; then
			howto="not installed — install it with: sudo pacman -S wlroots0.20"
		else
			howto="not installed, and 'wlroots0.20' isn't in your configured pacman repos — check the AUR, or see the vendored-fallback note below."
		fi
	else
		howto="pkg-config cannot find '$module' satisfying $need. This script only knows the Arch/CachyOS package name (wlroots0.20 via pacman); on other distros, install whatever package provides a wlroots 0.20.x pkg-config file (module name '$module') through your own package manager."
	fi

	if [ -f "$repo_root/subprojects/wlroots/meson.build" ]; then
		GCR_WLROOTS_STATE="vendored-fallback"
		GCR_WLROOTS_MSG="system package $howto
    Not a failure: this repo's vendored copy (subprojects/wlroots) is checked
    out, and meson's fallback will build that instead (slower first build).
    To use the faster system package next time, fix the above."
		return 0
	fi

	GCR_WLROOTS_STATE="fail"
	GCR_WLROOTS_MSG="system package $howto
    No vendored fallback available either (subprojects/wlroots is not
    checked out here). The build cannot proceed until one of these is
    fixed."
	return 1
}

run_wlroots_check() {
	# $1: "fatal" (about to build — a "fail" state must stop us) or "info"
	# (just reporting, e.g. an existing binary means no build is needed).
	local when="$1"
	gcr_info "checking wlroots dependency..."
	if gcr_check_wlroots "$REPO_ROOT"; then
		gcr_info "wlroots: $GCR_WLROOTS_MSG"
	else
		gcr_err "wlroots: $GCR_WLROOTS_MSG"
		if [ "$when" = "fatal" ]; then
			exit 1
		fi
	fi
}

# --- state detection (used by the interactive menu, and by --update) --------
target_state() {
	if [ -L "$TARGET" ]; then
		printf 'symlink\n'
	elif [ -f "$TARGET" ]; then
		printf 'copy\n'
	elif [ -e "$TARGET" ]; then
		printf 'other\n'
	else
		printf 'absent\n'
	fi
}

# --- Ritz launcher extension ---------------------------------------------
# Optional, offered — never forced. This repo ships a Ritz
# (https://ritze03.github.io/ritz/extensions.html) launcher module at
# extensions/gamescope-ritz.json that wraps THIS binary (gamescope-ritz),
# not upstream gamescope. See superdoc/features/ritz-extension.md.
#
# Honours XDG_CONFIG_HOME (not just $HOME/.config) so a test run can point
# this at a scratch directory instead of the user's real ~/.config/ritz.
ritz_config_dir() {
	printf '%s/ritz\n' "${XDG_CONFIG_HOME:-$HOME/.config}"
}

# True (0) if Ritz looks present on this machine. The docs name no signal
# more specific than its config dir; checking for the `ritz` binary too
# covers a fresh Ritz install that hasn't written that dir yet.
gcr_ritz_present() {
	[ -d "$(ritz_config_dir)" ] && return 0
	command -v ritz >/dev/null 2>&1 && return 0
	return 1
}

ritz_manifest_src() { printf '%s/extensions/gamescope-ritz.json\n' "$REPO_ROOT"; }
ritz_manifest_dst() { printf '%s/extensions/gamescope-ritz.json\n' "$(ritz_config_dir)"; }

# Offer to install (mode=install) or refresh (mode=update) the Ritz
# extension manifest. Honours RITZ_EXT ("yes"/"no"/"" = ask). A no-op if
# Ritz isn't detected, or (update mode) if this never installed one here.
ritz_extension_prompt() {
	local mode="$1" src dst
	src=$(ritz_manifest_src)
	dst=$(ritz_manifest_dst)

	gcr_ritz_present || return 0

	if [ "$mode" = "update" ]; then
		if [ ! -f "$dst" ]; then
			# Nothing installed here before -- --update only refreshes an
			# existing copy; offering a new one is --install's job.
			return 0
		fi
		if cmp -s -- "$src" "$dst" 2>/dev/null; then
			gcr_info "Ritz extension: $dst is already up to date."
			return 0
		fi
		if [ "$RITZ_EXT" = "no" ]; then
			gcr_info "Ritz extension: --no-ritz-extension given, leaving $dst as-is."
			return 0
		fi
		if [ "$RITZ_EXT" != "yes" ]; then
			echo
			echo "This repo's Ritz extension manifest has changed since it was last"
			echo "copied to $dst."
			gcr_confirm "Refresh it?" y || { gcr_info "left $dst as-is."; return 0; }
		fi
		cp -f -- "$src" "$dst"
		gcr_info "refreshed Ritz extension: $dst"
		return 0
	fi

	# mode = "install"
	if [ -f "$dst" ] && cmp -s -- "$src" "$dst" 2>/dev/null; then
		gcr_info "Ritz extension: $dst is already up to date."
		return 0
	fi
	if [ "$RITZ_EXT" = "no" ]; then
		gcr_info "Ritz extension: --no-ritz-extension given, skipping."
		return 0
	fi
	if [ "$RITZ_EXT" != "yes" ]; then
		echo
		echo "A Ritz install was detected ($(ritz_config_dir))."
		echo "This repo ships a Ritz launcher extension (extensions/gamescope-ritz.json)"
		echo "wrapping gamescope-ritz — Profile, nested width/height/refresh,"
		echo "fullscreen, force-windows-fullscreen, scaler and filter, all from Ritz's UI."
		gcr_confirm "Install it to $dst?" n || {
			gcr_info "skipped the Ritz extension. Re-run with --with-ritz-extension later if you want it."
			return 0
		}
	fi
	mkdir -p -- "$(dirname -- "$dst")"
	cp -f -- "$src" "$dst"
	gcr_info "installed Ritz extension: $dst"
}

# Offer to remove exactly the one manifest this script may have installed.
# Never touches anything else under ~/.config/ritz/extensions.
ritz_extension_remove_prompt() {
	local dst; dst=$(ritz_manifest_dst)
	[ -f "$dst" ] || return 0
	if [ "$RITZ_EXT" = "no" ]; then
		gcr_info "Ritz extension: --no-ritz-extension given, leaving $dst in place."
		return 0
	fi
	gcr_info "found a Ritz extension manifest: $dst"
	if [ "$RITZ_EXT" = "yes" ] || gcr_confirm "Remove it?" y; then
		rm -f -- "$dst"
		gcr_info "removed $dst."
	else
		gcr_info "left $dst in place."
	fi
}

# --- actions ------------------------------------------------------------
do_install() {
	gcr_info "gamescope-ritz installer"
	gcr_info "repo:   $REPO_ROOT"
	gcr_info "target: $TARGET"

	local release_bin; release_bin=$(gcr_release_binary "$BUILD_DIR")
	if [ ! -x "$release_bin" ] || [ "$REBUILD" = "1" ]; then
		run_wlroots_check fatal
		if [ ! -x "$release_bin" ]; then
			gcr_info "no release build found at $release_bin — building one now."
		else
			gcr_info "--rebuild given — rebuilding $release_bin."
		fi
		gcr_build_release "$REPO_ROOT" "$BUILD_DIR"
	else
		run_wlroots_check info
		gcr_info "found existing release build: $release_bin (pass --rebuild to force a rebuild)"
	fi

	if [ ! -x "$release_bin" ]; then
		gcr_err "build finished but $release_bin was not produced. Aborting."
		exit 1
	fi

	if [ -z "$MODE" ]; then
		if [ "${GCR_ASSUME_YES:-0}" = "1" ]; then
			MODE="copy"
		else
			echo
			echo "How should $TARGET be installed?"
			echo "  1) symlink -> $release_bin"
			echo "     Rebuilds go live instantly (no reinstall needed — a later"
			echo "     ./install.sh --update just rebuilds and the new binary is"
			echo "     live immediately, no root required again). Breaks if you"
			echo "     move or delete this repo."
			echo "  2) copy (independent of this repo; --update copies again)"
			read -r -p "Choose [1/2]: " choice
			case "$choice" in
				1) MODE="link" ;;
				2) MODE="copy" ;;
				*) gcr_err "invalid choice: $choice"; exit 1 ;;
			esac
		fi
	fi

	case "$MODE" in
		link) gcr_info "about to symlink $TARGET -> $release_bin" ;;
		copy) gcr_info "about to copy $release_bin -> $TARGET" ;;
		*) gcr_err "internal error: unknown mode '$MODE'"; exit 1 ;;
	esac
	gcr_confirm "Proceed writing to $TARGET?" y || { gcr_info "aborted, nothing installed."; exit 1; }

	GCR_PRIV_DIR="$PREFIX_DIR"
	gcr_as_priv mkdir -p -- "$PREFIX_DIR"
	case "$MODE" in
		link) gcr_as_priv ln -sf -- "$release_bin" "$TARGET" ;;
		copy) gcr_as_priv cp -f -- "$release_bin" "$TARGET" ;;
	esac
	gcr_info "installed: $TARGET ($MODE mode)"

	install_extras_prompt
	ritz_extension_prompt install
	echo
	gcr_info "done. Run: $TARGET --help"
	[ "$MODE" = "link" ] && gcr_warn "symlink mode: moving or deleting $REPO_ROOT will break $TARGET."
}

do_remove() {
	local prefix_root extras_dir
	prefix_root=$(dirname -- "$PREFIX_DIR")
	extras_dir=$(gcr_extras_dir "$prefix_root")

	gcr_info "gamescope-ritz remover"
	gcr_info "target: $TARGET"

	if [ ! -e "$TARGET" ] && [ ! -L "$TARGET" ] && [ ! -e "$extras_dir" ] && [ ! -f "$(ritz_manifest_dst)" ]; then
		gcr_info "$TARGET does not exist and $extras_dir does not exist, nothing to remove."
		gcr_info "your settings (~/.config/gamescope-ritz) are never touched by this script."
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

	if [ -e "$extras_dir" ]; then
		# Mirrors gcr_check_target_safety's spirit: only ever remove our own
		# namespaced data directory, never plain $prefix_root/share/gamescope
		# (a distro-packaged gamescope's).
		case "$extras_dir" in
			*/share/gamescope-ritz)
				if gcr_confirm "Also remove $extras_dir (scripts/looks extras)?" y; then
					GCR_PRIV_DIR="$prefix_root"
					gcr_as_priv rm -rf -- "$extras_dir"
					gcr_info "removed $extras_dir."
				else
					gcr_info "left $extras_dir in place."
				fi
				;;
			*)
				gcr_err "internal error: refusing to remove unexpected extras dir '$extras_dir'"
				exit 1
				;;
		esac
	else
		gcr_info "$extras_dir does not exist, nothing to remove there."
	fi

	ritz_extension_remove_prompt

	echo
	gcr_info "your settings (~/.config/gamescope-ritz) are kept — this script never touches them."
}

do_update() {
	local branch install_mode

	gcr_info "gamescope-ritz updater"
	gcr_info "repo:   $REPO_ROOT"
	gcr_info "target: $TARGET"

	if [ ! -e "$TARGET" ] && [ ! -L "$TARGET" ]; then
		gcr_err "$TARGET does not exist. Run ./install.sh --install first."
		exit 1
	fi

	case "$(target_state)" in
		symlink) install_mode="symlink" ;;
		copy) install_mode="copy" ;;
		*) gcr_err "$TARGET exists but is neither a symlink nor a regular file. Refusing to guess."; exit 1 ;;
	esac
	gcr_info "detected install mode: $install_mode"

	branch=$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)
	gcr_info "branch: $branch"

	if [ "$ALLOW_DIRTY" != "1" ]; then
		if [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
			gcr_err "uncommitted changes in $REPO_ROOT — refusing to pull over them."
			gcr_err "Commit or stash your changes, or re-run with --allow-dirty to skip only this check"
			gcr_err "(git pull --ff-only below will still refuse a non-fast-forward on its own)."
			git -C "$REPO_ROOT" status --short >&2
			exit 1
		fi
	fi

	gcr_info "git pull --ff-only (branch $branch) ..."
	if ! git -C "$REPO_ROOT" pull --ff-only; then
		gcr_err "git pull failed (conflict, diverged history, or network error)."
		gcr_err "Nothing was built or installed. Resolve the git state by hand and re-run."
		exit 1
	fi

	run_wlroots_check fatal

	local build_dir
	if [ "$install_mode" = "symlink" ]; then
		# Rebuild whatever the symlink already points at, not whatever
		# --build-dir defaults to, so a custom build dir given at install
		# time is honoured.
		local link_target; link_target=$(readlink -f -- "$TARGET")
		build_dir=$(dirname -- "$(dirname -- "$link_target")")
		gcr_info "symlink points at $link_target -> rebuilding $build_dir"
	else
		build_dir="$BUILD_DIR"
	fi

	gcr_build_release "$REPO_ROOT" "$build_dir"
	local release_bin; release_bin=$(gcr_release_binary "$build_dir")
	if [ ! -x "$release_bin" ]; then
		gcr_err "build finished but $release_bin was not produced. Aborting."
		exit 1
	fi

	if [ "$install_mode" = "symlink" ]; then
		gcr_info "symlink mode: $TARGET already points at the binary just rebuilt, nothing to copy."
	else
		gcr_info "copy mode: copying $release_bin -> $TARGET"
		GCR_PRIV_DIR="$PREFIX_DIR"
		gcr_as_priv cp -f -- "$release_bin" "$TARGET"
	fi

	install_extras_prompt
	ritz_extension_prompt update
	echo
	gcr_info "done. $TARGET is up to date."
}

install_extras_prompt() {
	local prefix_root; prefix_root=$(dirname -- "$PREFIX_DIR")
	if [ -z "$EXTRAS" ]; then
		echo
		echo "default_extras_install.sh copies this repo's scripts/, looks/ and"
		echo "the bundled font license into ${prefix_root}/share/gamescope-ritz"
		echo "— namespaced by binary name, so it never touches a distro-packaged"
		echo "/usr/bin/gamescope's own share/gamescope."
		if gcr_confirm "Run it now?" n; then EXTRAS="yes"; else EXTRAS="no"; fi
	fi
	if [ "$EXTRAS" = "yes" ]; then
		gcr_install_extras "$REPO_ROOT" "$prefix_root"
	else
		gcr_info "skipped extras (scripts/looks). Re-run with --extras later if needed."
	fi
}

interactive_menu() {
	local state link_target="" dirty="clean" branch deps_line
	state=$(target_state)
	if [ "$state" = "symlink" ]; then
		link_target=$(readlink -f -- "$TARGET" 2>/dev/null || readlink -- "$TARGET")
	fi
	branch=$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
	[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ] && dirty="dirty"

	if gcr_check_wlroots "$REPO_ROOT" >/dev/null 2>&1; then
		deps_line="$GCR_WLROOTS_MSG"
	else
		deps_line="$GCR_WLROOTS_MSG"
	fi

	echo
	echo "gamescope-ritz install.sh — detected state"
	echo "  repo:        $REPO_ROOT (branch $branch, $dirty)"
	echo "  target:      $TARGET"
	case "$state" in
		symlink) echo "  installed:   yes, symlink -> $link_target" ;;
		copy)    echo "  installed:   yes, copy" ;;
		other)   echo "  installed:   something is there, but not a symlink or regular file" ;;
		absent)  echo "  installed:   no" ;;
	esac
	echo "  wlroots dep: $deps_line"
	echo
	echo "What would you like to do?"
	echo "  1) Install$([ "$state" != "absent" ] && echo "/reinstall")"
	echo "  2) Update (git pull + rebuild + reinstall in place)"
	echo "  3) Remove"
	echo "  4) Quit"
	read -r -p "Choose [1-4]: " choice
	case "$choice" in
		1) do_install ;;
		2) do_update ;;
		3) do_remove ;;
		4|"") gcr_info "nothing to do." ;;
		*) gcr_err "invalid choice: $choice"; exit 1 ;;
	esac
}

case "$ACTION" in
	install) do_install ;;
	remove) do_remove ;;
	update) do_update ;;
	"") interactive_menu ;;
esac
