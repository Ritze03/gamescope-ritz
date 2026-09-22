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
# Actions (choose at most one; no action = interactive menu, Enter defaults
# to Install/reinstall):
#   --install           check deps, build a release binary if needed, install
#                       it (symlink or copy — asked interactively unless
#                       --link/--copy is given; Enter defaults to symlink).
#                       If a Ritz install is detected, also offers to install
#                       this repo's Ritz launcher extension (Enter accepts).
#   --remove            uninstall the binary/symlink this installed. Also
#                       offers to delete a leftover share/gamescope-ritz
#                       directory from an older install (nothing needs one
#                       any more — the bundled scripts and licences are
#                       compiled into the binary).
#                       Also offers to remove the Ritz extension manifest at
#                       ~/.config/ritz/extensions/ritze__gamescope_ritz.json,
#                       if present — since that's the same path Ritz's own
#                       "Gamescope-Ritz" module uses, it may be the user's
#                       pre-existing module rather than one this installer
#                       added, so this defaults to declining and says so.
#                       Nothing else under ~/.config/ritz/extensions is
#                       touched. Never touches ~/.config/gamescope-ritz.
#   --update            git pull --ff-only (a dirty tree only warns; the pull
#                       itself refuses anything unsafe), rebuild, and
#                       reinstall by whichever method (symlink/copy) is
#                       already in place — a symlink install needs no copy
#                       step, the rebuilt binary is live immediately. Also
#                       offers to refresh the Ritz extension manifest if this
#                       previously installed one and the repo's copy has
#                       changed.
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
#   --rebuild           (--install) rebuild even if a release binary exists
#   --prefix DIR        install directory (default: /usr/bin) — override to
#                       install/remove/update into a scratch prefix, e.g. for
#                       testing this script without touching the real system
#   --build-dir DIR     release build directory name, relative to the repo
#                       root (default: build-release)
#   --with-ritz-extension  copy this repo's Ritz launcher extension manifest
#                       (extensions/gamescope-ritz.json) to
#                       ~/.config/ritz/extensions/ritze__gamescope_ritz.json
#                       (or refresh it there on --update), no prompt. That
#                       destination name is deliberate: it's the same name
#                       Ritz's own "Gamescope-Ritz" module already uses, so
#                       this OVERWRITES that module in place (same
#                       Author/Name/Version, so stored per-user values in
#                       Ritz survive) instead of adding a second, duplicate
#                       module. Only does anything if a Ritz install is
#                       detected (~/.config/ritz exists, or the
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

# --- basic build-tool check ---------------------------------------------
# meson and cmake are required by every action below (build, reconfigure,
# even the openvr cmake subproject) -- checked first, before argument
# parsing or anything else, so a missing tool fails fast with one clear
# message instead of a confusing error hundreds of lines into a build.
# Reports every missing command in one run rather than one at a time.
#
# The programs, not the libraries: the pkg-config gate further down covers
# every dependency()' in the meson files, but a find_program() is invisible to
# it. A real report (Fedora 44) got past every library check and then died at
# src/meson.build:55 with "Program 'glslang glslangValidator' not found",
# because the shader compiler is a BINARY, not a pkg-config module.
#
# Only two external programs are hard-required by the meson files:
# wayland-scanner (which arrives with its own pkg-config module, so the
# dependency gate already covers it) and the glslang compiler here. The other
# find_program() calls are this repo's own Python scripts.
#
# The command name and the package name are not the same thing on every
# distro (Fedora ships /usr/bin/ninja in a package called ninja-build), so
# this carries a small map. ponytail: a 5-entry map is fine where a 25-entry
# one would rot -- these are build tools whose names essentially never change,
# unlike the library list, which is scraped for exactly that reason.
gcr_tool_package() {
	local cmd="$1" mgr="$2"
	case "$mgr:$cmd" in
		dnf:ninja)    printf 'ninja-build' ;;
		dnf:pkg-config) printf 'pkgconf' ;;
		apt:ninja)    printf 'ninja-build' ;;
		apt:glslang)  printf 'glslang-tools' ;;
		apt:pkg-config) printf 'pkg-config' ;;
		pacman:pkg-config) printf 'pkgconf' ;;
		*)            printf '%s' "$cmd" ;;
	esac
}

gcr_check_build_tools() {
	local missing=() cmd mgr pkgs=""

	for cmd in meson cmake ninja pkg-config git; do
		command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
	done

	# The shader compiler: meson accepts EITHER name (src/meson.build:55
	# lists both), so only the absence of both is a failure.
	if ! command -v glslang >/dev/null 2>&1 && ! command -v glslangValidator >/dev/null 2>&1; then
		missing+=("glslang")
	fi

	[ "${#missing[@]}" = "0" ] && return 0

	if command -v dnf >/dev/null 2>&1; then mgr="dnf"
	elif command -v pacman >/dev/null 2>&1; then mgr="pacman"
	elif command -v apt-get >/dev/null 2>&1; then mgr="apt"
	else mgr="none"
	fi

	for cmd in "${missing[@]}"; do pkgs="$pkgs $(gcr_tool_package "$cmd" "$mgr")"; done

	gcr_err "missing required command(s): ${missing[*]}"
	case "$mgr" in
		dnf)    gcr_err "install them with: sudo dnf install$pkgs" ;;
		pacman) gcr_err "install them with: sudo pacman -S --needed$pkgs" ;;
		apt)    gcr_err "install them with: sudo apt install$pkgs" ;;
		*)      gcr_err "install them through your distro's package manager first." ;;
	esac
	exit 1
}
gcr_check_build_tools

ACTION=""           # "install", "remove" or "update"; "" = interactive menu
MODE=""             # "link" or "copy" (--install only)
GCR_ASSUME_YES=0
REBUILD=0
PREFIX_DIR="$GCR_DEFAULT_PREFIX_DIR"
BUILD_DIR_NAME="$GCR_DEFAULT_BUILD_DIR_NAME"
RITZ_EXT=""         # "" = ask, "yes", "no" (--with-ritz-extension / --no-ritz-extension)

# The whole header comment above, minus the shebang and the trailing blank.
# The end line was 77 and had been for a while, which quietly cut the help
# off in the middle of --with-ritz-extension's description -- so
# --no-ritz-extension and the Examples block were documented in the file and
# unreachable from --help. Corrected here because this line had to be touched
# anyway (the --extras/--no-extras entries above it are gone).
print_help() { sed -n '2,94p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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
		--rebuild) REBUILD=1 ;;
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
# Reads the pkg-config module names and version constraints straight out of
# src/meson.build's `wlroots_dep = dependency(...)` probes, so this can't drift
# from what the build actually asks for. Since 2026-09-22 there is more than
# one probe (system 0.20, then system 0.19, then the vendored fallback), so
# this enumerates ALL of them and accepts the first one pkg-config satisfies --
# exactly the order meson itself tries them in.
#
# Sets GCR_WLROOTS_STATE to one of "ok" (a system package satisfies it),
# "vendored-fallback" (none did, but meson's own fallback subproject is checked
# out and will be built instead -- not a failure) or "fail" (neither available
# -- must not proceed to a build). GCR_WLROOTS_MSG is the human-readable
# status/explanation to print.

# Every wlroots probe src/meson.build makes, in the order meson tries them, one
# per line as "module<TAB>constraint,constraint" (constraints may be empty).
#
# Newlines are flattened to spaces FIRST so that both spellings parse: the
# one-line `dependency('wlroots-0.20', version: [...], required: false)` and
# the multi-line fallback block. Scraping line-by-line is what broke when the
# 0.19 probe landed -- the old awk did `getline` and read `if not
# wlroots_dep.found()` as the module name.
#
# `[^)]*` stops each chunk at its own closing paren, which is safe because no
# wlroots probe contains a nested `(`. The only quoted strings inside a chunk
# that start with a comparison operator are the version constraints, so
# grepping for those needs no position tracking.
gcr_wlroots_candidates() {
	local chunk mod cons seen=""
	while IFS= read -r chunk; do
		mod=$(printf '%s' "$chunk" | grep -oE "wlroots-[0-9]+\.[0-9]+" | head -n1)
		[ -n "$mod" ] || continue
		# The vendored fallback repeats a module already probed; one line each.
		case " $seen " in *" $mod "*) continue ;; esac
		seen="$seen $mod"
		cons=$(printf '%s' "$chunk" | grep -oE "'(>=|<=|==|!=|>|<)[^']*'" | tr -d "'" | paste -sd, -)
		printf '%s\t%s\n' "$mod" "$cons"
	done < <(tr '\n' ' ' < "$1/src/meson.build" 2>/dev/null \
		| grep -oE "dependency\([ ]*'wlroots-[0-9]+\.[0-9]+'[^)]*")
}

gcr_check_wlroots() {
	local repo_root="$1" line mod cons c args need ver tried=()

	if ! command -v pkg-config >/dev/null 2>&1; then
		GCR_WLROOTS_STATE="fail"
		GCR_WLROOTS_MSG="'pkg-config' itself is not installed, so this cannot even check. Install pkg-config (or pkgconf) first."
		[ -f "$repo_root/subprojects/wlroots/meson.build" ] || return 1
		GCR_WLROOTS_STATE="vendored-fallback"
		return 0
	fi

	while IFS=$'\t' read -r mod cons; do
		[ -n "$mod" ] || continue
		args=()
		if [ -n "$cons" ]; then
			# "a,b" -> pkg-config args "mod a" "mod b"
			local IFS_SAVE="$IFS"; IFS=','
			for c in $cons; do args+=("$mod $c"); done
			IFS="$IFS_SAVE"
			need="$mod ($(printf '%s' "$cons" | tr ',' ' '))"
		else
			args=("$mod")
			need="$mod (any version)"
		fi
		tried+=("$need")

		if pkg-config --exists "${args[@]}" 2>/dev/null; then
			ver=$(pkg-config --modversion "$mod" 2>/dev/null || true)
			GCR_WLROOTS_STATE="ok"
			GCR_WLROOTS_MSG="OK — pkg-config finds '$mod'${ver:+ ($ver)}; the build will use it."
			return 0
		fi
	done < <(gcr_wlroots_candidates "$repo_root")

	if [ "${#tried[@]}" = "0" ]; then
		# meson.build changed shape and nothing parsed; degrade gracefully
		# rather than claiming the dependency is missing.
		tried=("wlroots-0.20" "wlroots-0.19")
	fi

	local howto="none of the wlroots versions this build accepts were found by pkg-config.
    Tried, in the order meson does: $(printf '%s; ' "${tried[@]}")"
	if command -v pacman >/dev/null 2>&1; then
		local pkg pv installed=""
		for pkg in wlroots0.20 wlroots0.19; do
			if pacman -Q "$pkg" >/dev/null 2>&1; then
				pv=$(pacman -Q "$pkg" 2>/dev/null | awk '{print $2}')
				installed="$installed $pkg ($pv)"
			fi
		done
		if [ -n "$installed" ]; then
			howto="$howto
    pacman shows$installed installed, but pkg-config cannot see a matching
    module. Check PKG_CONFIG_PATH/PKG_CONFIG_LIBDIR, and that the installed
    .pc file's Version really falls in the range above."
		else
			howto="$howto
    Install one with: sudo pacman -S wlroots0.20   (or wlroots0.19)"
		fi
	else
		howto="$howto
    This script only knows the Arch/CachyOS package names (wlroots0.20 /
    wlroots0.19 via pacman); on another distro install whatever package
    provides one of those pkg-config modules."
	fi

	if [ -f "$repo_root/subprojects/wlroots/meson.build" ]; then
		GCR_WLROOTS_STATE="vendored-fallback"
		GCR_WLROOTS_MSG="$howto
    Not a failure: this repo's vendored copy (subprojects/wlroots) is checked
    out, and meson's fallback will build that instead (slower first build).
    To use the faster system package next time, fix the above."
		return 0
	fi

	GCR_WLROOTS_STATE="fail"
	GCR_WLROOTS_MSG="$howto
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

# --- hard pkg-config dependency check ---------------------------------------
# wlroots gets its own check above because it has several acceptable versions
# and a vendored fallback. Everything here is the opposite: a flat list of
# pkg-config modules meson has no fallback for, so a missing one is a hard
# stop. Without this the failure lands hundreds of lines into `meson setup`
# (a real report: wayland-protocols missing on Fedora 44 failed at
# protocol/meson.build:7 after a full compiler probe and an openvr cmake
# configure), which reads like a build bug rather than a missing package.
#
# The list is SCRAPED, not hardcoded, for the same reason the wlroots check
# scrapes: a hand-kept copy drifts from meson.build and then lies. A
# dependency() whose line carries `required` is gated by a meson option or is
# optional, so it is not a hard stop and is skipped.
gcr_hard_pkgconfig_modules() {
	local repo_root="$1" f mod
	for f in meson.build src/meson.build protocol/meson.build layer/meson.build; do
		[ -f "$repo_root/$f" ] || continue
		# Only `required: false` and `required: get_option(...)` mean optional.
		# `required: true` is a HARD dependency that happens to say so out loud
		# (src/meson.build's libinput), and dropping every line matching
		# "required" would silently skip it. The [ ]* allows `dependency( 'x' )`.
		grep -hE "dependency\([ ]*'[a-zA-Z0-9._-]+'" "$repo_root/$f" \
			| grep -vE "required[ ]*:[ ]*(false|get_option)"
	done \
		| grep -oE "dependency\([ ]*'[a-zA-Z0-9._-]+'" | sed "s/dependency([ ]*'//; s/'//" \
		| while IFS= read -r mod; do
			case "$mod" in
				# Not pkg-config modules: a meson builtin, a subproject
				# dependency object, and one that force_fallback_for pins to
				# the vendored copy no matter what the system has.
				threads|openvr_api|vkroots) ;;
				# wlroots has its own multi-version check above.
				wlroots-*) ;;
				*) printf '%s\n' "$mod" ;;
			esac
		done | sort -u
}

# Print the distro-appropriate command for installing the missing modules.
# Fedora/RHEL is exact: dnf resolves `pkgconfig(foo)` virtual provides, so the
# pkg-config module name IS the package name and no lookup table is needed.
# ponytail: no module->package map for the others. Maintaining one for three
# distros is exactly the kind of table that rots; the file-search command
# below answers the same question and stays true on its own.
gcr_pkgconfig_install_hint() {
	local mods=("$@") m out=""
	if command -v dnf >/dev/null 2>&1; then
		for m in "${mods[@]}"; do out="$out 'pkgconfig($m)'"; done
		printf 'sudo dnf install%s' "$out"
	elif command -v pacman >/dev/null 2>&1; then
		for m in "${mods[@]}"; do out="$out /usr/lib/pkgconfig/$m.pc"; done
		printf 'find the packages with: pacman -F%s' "$out"
	elif command -v apt-get >/dev/null 2>&1; then
		for m in "${mods[@]}"; do out="$out $m.pc"; done
		printf 'find the packages with: apt-file search%s' "$out"
	else
		printf 'install whatever your distro calls the -dev/-devel packages providing those modules'
	fi
}

gcr_check_pkgconfig_deps() {
	local repo_root="$1" mod missing=()

	if ! command -v pkg-config >/dev/null 2>&1; then
		gcr_err "'pkg-config' is not installed, so the build's dependencies cannot be checked."
		return 1
	fi

	while IFS= read -r mod; do
		[ -n "$mod" ] || continue
		pkg-config --exists "$mod" 2>/dev/null || missing+=("$mod")
	done < <(gcr_hard_pkgconfig_modules "$repo_root")

	[ "${#missing[@]}" = "0" ] && return 0

	gcr_err "missing required development package(s) — pkg-config cannot find: ${missing[*]}"
	gcr_err "$(gcr_pkgconfig_install_hint "${missing[@]}")"
	return 1
}

run_pkgconfig_check() {
	# $1: "fatal" (about to build) or "info" (just reporting).
	local when="$1"
	gcr_info "checking build dependencies..."
	if gcr_check_pkgconfig_deps "$REPO_ROOT"; then
		gcr_info "build dependencies: all present."
	elif [ "$when" = "fatal" ]; then
		exit 1
	fi
}

# Every pre-build dependency gate, in one call so the two build sites cannot
# drift apart on which checks they run.
run_dependency_checks() {
	run_pkgconfig_check "$1"
	run_wlroots_check "$1"
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
# Optional, offered — never forced. This repo ships the user's own Ritz
# (https://ritze03.github.io/ritz/extensions.html) "Gamescope-Ritz" launcher
# module at extensions/gamescope-ritz.json, with one field added (Profile)
# so it can also pass --profile; everything else in it is unchanged from
# what Ritz itself would have written. See superdoc/features/ritz-extension.md.
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
# The destination name is NOT the same as the source's — deliberately.
# Ritz names an author's module file "<author>__<name>.json"
# (lowercased, spaces/hyphens to underscores), so the user's existing
# "Gamescope-Ritz" module by "Ritze" already lives at
# ritze__gamescope_ritz.json. Landing our copy under the source's own
# gamescope-ritz.json name would create a SECOND module with the same
# Extension identity (Author::Name::Version) sitting next to the first —
# exactly the duplication this is meant to avoid. Using the same
# destination name makes this an in-place update of that one module
# instead.
ritz_manifest_dst() { printf '%s/extensions/ritze__gamescope_ritz.json\n' "$(ritz_config_dir)"; }

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
			gcr_info "Ritz extension: $dst already matches this repo's copy — nothing to refresh."
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
		gcr_info "Ritz extension: $dst already matches this repo's copy — nothing to install."
		return 0
	fi
	if [ "$RITZ_EXT" = "no" ]; then
		gcr_info "Ritz extension: --no-ritz-extension given, skipping."
		return 0
	fi
	if [ "$RITZ_EXT" != "yes" ]; then
		echo
		echo "A Ritz install was detected ($(ritz_config_dir))."
		echo "This repo ships a Ritz launcher extension wrapping gamescope-ritz"
		echo "(the same 'Gamescope-Ritz' module Ritz itself uses, plus a Profile field)."
		if [ -f "$dst" ]; then
			echo "$dst already exists and will be OVERWRITTEN."
			echo "Author/Name/Version are unchanged, so Ritz's own stored per-game"
			echo "values for this module carry over — but any hand-edits made"
			echo "directly to that file will be lost."
		fi
		gcr_confirm "Install it to $dst?" y || {
			gcr_info "skipped the Ritz extension. Re-run with --with-ritz-extension later if you want it."
			return 0
		}
	fi
	mkdir -p -- "$(dirname -- "$dst")"
	cp -f -- "$src" "$dst"
	gcr_info "installed Ritz extension: $dst"
}

# Offer to remove the Ritz extension manifest at $dst. Never touches
# anything else under ~/.config/ritz/extensions.
#
# IMPORTANT: because ritz_manifest_dst() now names the SAME file Ritz's own
# pre-existing "Gamescope-Ritz" module already lives at (see the comment on
# ritz_manifest_dst above), $dst existing is not proof this installer ever
# put it there — it is very possibly the user's own module, installed by
# Ritz itself, that this repo never touched. So this defaults the confirm
# to "no" and says so plainly, instead of assuming ownership the way a
# script that only ever wrote this file itself safely could.
ritz_extension_remove_prompt() {
	local dst; dst=$(ritz_manifest_dst)
	[ -f "$dst" ] || return 0
	if [ "$RITZ_EXT" = "no" ]; then
		gcr_info "Ritz extension: --no-ritz-extension given, leaving $dst in place."
		return 0
	fi
	gcr_info "found a Ritz extension manifest: $dst"
	if [ "$RITZ_EXT" = "yes" ]; then
		rm -f -- "$dst"
		gcr_info "removed $dst."
		return 0
	fi
	echo "This is the same path Ritz's own 'Gamescope-Ritz' module uses, so this"
	echo "may be your pre-existing module rather than something this installer"
	echo "added — removing it deletes it either way."
	if gcr_confirm "Remove it?" n; then
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
		run_dependency_checks fatal
		if [ ! -x "$release_bin" ]; then
			gcr_info "no release build found at $release_bin — building one now."
		else
			gcr_info "--rebuild given — rebuilding $release_bin."
		fi
		gcr_build_release "$REPO_ROOT" "$BUILD_DIR"
	else
		run_dependency_checks info
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
			read -r -p "Choose [1/2, default 1]: " choice
			choice="${choice:-1}"
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

	ritz_extension_prompt install
	echo
	gcr_info "done. Run: $TARGET --help"
	[ "$MODE" = "link" ] && gcr_warn "symlink mode: moving or deleting $REPO_ROOT will break $TARGET."
}

do_remove() {
	# A LEFTOVER, NOT AN INSTALL. Nothing puts anything in
	# $prefix/share/gamescope-ritz any anymore (the bundled Lua and the
	# licence texts are compiled into the binary as of 2026-09-09) -- but
	# installs made BEFORE that ran default_extras_install.sh and left a copy
	# there. Removing the binary and silently orphaning that directory would
	# leave cruft nothing can ever clean up again, so --remove still offers
	# to delete it. It offers; it never assumes.
	local prefix_root data_dir
	prefix_root=$(dirname -- "$PREFIX_DIR")
	data_dir="$prefix_root/share/gamescope-ritz"

	gcr_info "gamescope-ritz remover"
	gcr_info "target: $TARGET"

	if [ ! -e "$TARGET" ] && [ ! -L "$TARGET" ] && [ ! -e "$data_dir" ] && [ ! -f "$(ritz_manifest_dst)" ]; then
		gcr_info "$TARGET does not exist and $data_dir does not exist, nothing to remove."
		gcr_info "your settings (~/.config/gamescope-ritz) are never touched by this script."
		exit 0
	fi

	if [ -e "$TARGET" ] || [ -L "$TARGET" ]; then
		if [ -L "$TARGET" ]; then
			gcr_info "$TARGET is a symlink -> $(readlink -f -- "$TARGET" 2>/dev/null || readlink -- "$TARGET")"
		else
			gcr_info "$TARGET is a regular file ($(du -h -- "$TARGET" | cut -f1))."
		fi
		gcr_confirm "Remove $TARGET?" n || { gcr_info "aborted, nothing removed."; exit 1; }
		GCR_PRIV_DIR="$PREFIX_DIR"
		gcr_as_priv rm -f -- "$TARGET"
		gcr_info "removed $TARGET."
	fi

	if [ -e "$data_dir" ]; then
		# Mirrors gcr_check_target_safety's spirit: only ever remove our own
		# namespaced data directory, never plain $prefix_root/share/gamescope
		# (a distro-packaged gamescope's).
		case "$data_dir" in
			*/share/gamescope-ritz)
				gcr_info "found $data_dir — a leftover from an install made before the"
				gcr_info "bundled scripts and licences moved inside the binary. Nothing needs it."
				if gcr_confirm "Also remove $data_dir?" n; then
					GCR_PRIV_DIR="$prefix_root"
					gcr_as_priv rm -rf -- "$data_dir"
					gcr_info "removed $data_dir."
				else
					gcr_info "left $data_dir in place."
				fi
				;;
			*)
				gcr_err "internal error: refusing to remove unexpected data dir '$data_dir'"
				exit 1
				;;
		esac
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

	if [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
		gcr_warn "uncommitted changes in $REPO_ROOT — pulling anyway."
		gcr_warn "git pull --ff-only below refuses on its own if the update would clobber anything."
		git -C "$REPO_ROOT" status --short >&2
	fi

	gcr_info "git pull --ff-only (branch $branch) ..."
	if ! git -C "$REPO_ROOT" pull --ff-only; then
		gcr_err "git pull failed (conflict, diverged history, or network error)."
		gcr_err "Nothing was built or installed. Resolve the git state by hand and re-run."
		exit 1
	fi

	run_dependency_checks fatal

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

	ritz_extension_prompt update
	echo
	gcr_info "done. $TARGET is up to date."
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
	read -r -p "Choose [1-4, default 1]: " choice
	choice="${choice:-1}"
	case "$choice" in
		1) do_install ;;
		2) do_update ;;
		3) do_remove ;;
		4) gcr_info "nothing to do." ;;
		*) gcr_err "invalid choice: $choice"; exit 1 ;;
	esac
}

case "$ACTION" in
	install) do_install ;;
	remove) do_remove ;;
	update) do_update ;;
	"") interactive_menu ;;
esac
