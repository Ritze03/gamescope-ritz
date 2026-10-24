#!/bin/sh
# ritz-get-spec: 1
# install.sh for a program managed by ritz-get.  Spec: https://github.com/Ritze03/ritz-get/blob/main/SPEC.md
# ritz-get runs it from the clone root, stdin /dev/null:
#     ./install.sh --install --yes     first install AND every update/repair (idempotent, safe while running)
#     ./install.sh --remove  --yes     never touches config or user data
# By hand:  ./install.sh --install | --remove
# Copy this file to the repo root and edit ONLY the per-program section. If this file and SPEC.md disagree, SPEC.md wins.
set -eu

# Available everywhere below: $dir (the clone, absolute, from pwd -P), $bin (~/.local/bin), $cfg (XDG config), $data (XDG data),
# $first (non-empty on a first install), $yes (1 under --yes).
dir=$(cd "$(dirname "$0")" && pwd -P)
cd "$dir"
bin=$HOME/.local/bin
cfg=${XDG_CONFIG_HOME:-$HOME/.config}
data=${XDG_DATA_HOME:-$HOME/.local/share}

# ---- per-program section (edit) --------------------------------------------------------
NAME=gamescope-ritz             # the manifest NAME; the command will be ~/.local/bin/NAME
TARGET=build/$NAME              # the executable inside the clone that ~/.local/bin/NAME points to
ROOT_LINK=yes                   # also link /usr/local/bin/NAME (root once, first install only; SPEC rules R32-R36)

# gamescope-ritz is a C++/meson project. The meson build tree lives in build/meson (git-ignored) and
# is reused by every later run, so an update only runs ninja. The compositor is built as
# src/gamescope and copied to build/NAME; this fork's Vulkan WSI layer is copied to build/ too.
# Submodules (src/reshade, thirdparty/SPIRV-Headers, subprojects/*) are checked out by ritz-get.
LAYER=VkLayer_RITZ_gamescope_wsi
layerdir=$data/vulkan/implicit_layer.d

have() { command -v "$1" >/dev/null 2>&1; }
# pkg-config version window: pc_in MODULE MIN_VERSION BELOW_VERSION
pc_in() { pkg-config --atleast-version="$2" "$1" 2>/dev/null && ! pkg-config --atleast-version="$3" "$1" 2>/dev/null; }

check_tools() {
	need meson pacman:meson dnf:meson apt:meson
	need ninja pacman:ninja dnf:ninja-build apt:ninja-build
	need git pacman:git dnf:git apt:git       # meson fetches glm, imgui, stb and nlohmann_json with it
	have c++ || have g++ || have clang++ || miss g++ "pacman:gcc dnf:gcc-c++ apt:g++"
	have cc || have gcc || have clang || miss gcc "pacman:gcc dnf:gcc apt:gcc"
	have glslang || have glslangValidator || miss glslang "pacman:glslang dnf:glslang apt:glslang-tools"
	need_pc x11 pacman:libx11 dnf:libX11-devel apt:libx11-dev
	need_pc x11-xcb pacman:libx11 dnf:libX11-devel apt:libx11-xcb-dev
	need_pc xcb pacman:libxcb dnf:libxcb-devel apt:libxcb1-dev
	need_pc xcomposite pacman:libxcomposite dnf:libXcomposite-devel apt:libxcomposite-dev
	need_pc xcursor pacman:libxcursor dnf:libXcursor-devel apt:libxcursor-dev
	need_pc xdamage pacman:libxdamage dnf:libXdamage-devel apt:libxdamage-dev
	need_pc xext pacman:libxext dnf:libXext-devel apt:libxext-dev
	need_pc xfixes pacman:libxfixes dnf:libXfixes-devel apt:libxfixes-dev
	need_pc xi pacman:libxi dnf:libXi-devel apt:libxi-dev
	need_pc xmu pacman:libxmu dnf:libXmu-devel apt:libxmu-dev
	need_pc xrender pacman:libxrender dnf:libXrender-devel apt:libxrender-dev
	need_pc xres pacman:libxres dnf:libXres-devel apt:libxres-dev
	need_pc xtst pacman:libxtst dnf:libXtst-devel apt:libxtst-dev
	need_pc xxf86vm pacman:libxxf86vm dnf:libXxf86vm-devel apt:libxxf86vm-dev
	need_pc xkbcommon pacman:libxkbcommon dnf:libxkbcommon-devel apt:libxkbcommon-dev
	need_pc wayland-client pacman:wayland dnf:wayland-devel apt:libwayland-dev
	need_pc wayland-server pacman:wayland dnf:wayland-devel apt:libwayland-dev
	need_pc wayland-scanner pacman:wayland dnf:wayland-devel apt:libwayland-dev
	need_pc wayland-protocols pacman:wayland-protocols dnf:wayland-protocols-devel apt:wayland-protocols
	need_pc vulkan pacman:vulkan-icd-loader,vulkan-headers dnf:vulkan-loader-devel,vulkan-headers apt:libvulkan-dev
	need_pc pixman-1 pacman:pixman dnf:pixman-devel apt:libpixman-1-dev
	need_pc libudev pacman:systemd-libs dnf:systemd-devel apt:libudev-dev
	need_pc libdecor-0 pacman:libdecor dnf:libdecor-devel apt:libdecor-0-dev
	need_pc luajit pacman:luajit dnf:luajit-devel apt:libluajit-5.1-dev
	need_pc libinput pacman:libinput dnf:libinput-devel apt:libinput-dev
	# the libdisplay-info submodule fallback needs hwdata's pkg-config file at configure time
	pkg-config --exists libdisplay-info 2>/dev/null || need_pc hwdata pacman:hwdata dnf:hwdata-devel apt:hwdata
	if have pkg-config; then
		# the loader's pkg-config file does not bring the headers along on every distro
		if pkg-config --exists vulkan 2>/dev/null; then
			# word-split on purpose: --cflags prints a list of -I options
			# shellcheck disable=SC2046
			echo '#include <vulkan/vulkan.h>' | ${CC:-cc} -E -x c $(pkg-config --cflags vulkan) - >/dev/null 2>&1 \
				|| miss vulkan-headers "pacman:vulkan-headers dnf:vulkan-headers apt:libvulkan-dev"
		fi
		# wlroots: a system 0.20 or 0.19, or the checked-out submodule (meson builds it) both count
		if ! pc_in wlroots-0.20 0.20.0 0.21.0 && ! pc_in wlroots-0.19 0.19.0 0.20.0; then
			if [ -f subprojects/wlroots/meson.build ]; then
				need_pc libdrm pacman:libdrm dnf:libdrm-devel apt:libdrm-dev
				need_pc libseat pacman:seatd dnf:libseat-devel apt:libseat-dev
				need_pc xwayland pacman:xorg-xwayland dnf:xorg-x11-server-Xwayland-devel apt:xwayland
				need_pc xcb-composite pacman:libxcb dnf:libxcb-devel apt:libxcb-composite0-dev
				need_pc xcb-render pacman:libxcb dnf:libxcb-devel apt:libxcb-render0-dev
				need_pc xcb-res pacman:libxcb dnf:libxcb-devel apt:libxcb-res0-dev
				need_pc xcb-xfixes pacman:libxcb dnf:libxcb-devel apt:libxcb-xfixes0-dev
				need_pc xcb-ewmh pacman:xcb-util-wm dnf:xcb-util-wm-devel apt:libxcb-ewmh-dev
				need_pc xcb-icccm pacman:xcb-util-wm dnf:xcb-util-wm-devel apt:libxcb-icccm4-dev
			else
				miss wlroots-0.20 "pacman:wlroots0.20 dnf:wlroots-devel apt:libwlroots-0.20-dev"
			fi
		fi
		# OpenVR: a system copy, or meson builds the checked-out submodule with cmake
		if ! pkg-config --atleast-version=2.7 openvr 2>/dev/null; then
			if [ -f subprojects/openvr/CMakeLists.txt ]; then need cmake pacman:cmake dnf:cmake apt:cmake
			else miss openvr "pacman:openvr dnf:openvr-devel apt:libopenvr-dev"; fi
		fi
	fi
}

# Build into build/meson with the options the project's release build always used, then copy the
# compositor and the layer to build/ under a temporary name and mv them into place (atomic: a running
# gamescope keeps its old file, and a failed build never reaches the mv, because of set -e).
build() {
	# ritz-get checks the submodules out before --install; a plain clone made by hand may lack them
	for f in src/reshade/source thirdparty/SPIRV-Headers/include subprojects/FrameGen/gpu/framegen.h subprojects/vkroots/meson.build subprojects/libliftoff/meson.build; do
		[ -e "$f" ] || { echo "$NAME: $f is missing: the git submodules are not checked out (clone with --recurse-submodules)" >&2; exit 1; }
	done
	absent=
	for m in libdrm sdl2 libcap libpipewire-0.3 libeis-1.0 libavif; do pkg-config --exists "$m" 2>/dev/null || absent="$absent $m"; done
	[ -z "$absent" ] || echo "note: optional libraries not found, building without:$absent"
	# a meson tree holds absolute paths: if the clone moved, start it afresh (fetched subprojects stay)
	if [ -f build/.srcdir ] && [ "$(cat build/.srcdir)" != "$dir" ]; then rm -rf build/meson; fi
	# prefix and datadir make the layer manifest path baked into the binary the one linked below
	set -- --buildtype=release -Doptimization=3 -Db_lto=true -Denable_tests=false --prefix="$HOME/.local" -Ddatadir="$data"
	if [ -f build/meson/build.ninja ]; then
		echo "reconfiguring build/meson"
		meson setup --reconfigure build/meson "$@" >build/meson-setup.log 2>&1 || {
			echo "reconfigure failed, configuring build/meson afresh"
			meson setup --wipe build/meson "$@" >build/meson-setup.log 2>&1 || { tail -n 40 build/meson-setup.log >&2; exit 1; }
		}
	else
		echo "configuring build/meson (a first build fetches a few small libraries and takes a while)"
		meson setup build/meson "$@" >build/meson-setup.log 2>&1 || { tail -n 40 build/meson-setup.log >&2; exit 1; }
	fi
	echo "$dir" > build/.srcdir
	# the layer's target name carries the CPU family, so ask ninja for it
	lt=$(ninja -C build/meson -t targets all 2>/dev/null | sed -n 's|^\(layer/lib[A-Za-z0-9_]*gamescope_wsi[A-Za-z0-9_]*\.so\):.*|\1|p' | head -n 1)
	[ -n "$lt" ] || echo "note: WSI layer target not found, layer not rebuilt"
	echo "building with ninja"
	if have nice; then nice -n 10 ninja -C build/meson src/gamescope $lt; else ninja -C build/meson src/gamescope $lt; fi
	cp build/meson/src/gamescope "build/.$NAME.new"
	mv -f "build/.$NAME.new" "build/$NAME"
	if [ -n "$lt" ]; then
		cp "build/meson/$lt" build/.layer.new
		mv -f build/.layer.new "build/$(basename "$lt")"
		# the manifest names the library by an absolute path into this clone
		esc=$(printf '%s' "$dir/build/$(basename "$lt")" | sed 's/[\\&|]/\\&/g')
		for j in build/meson/layer/"$LAYER".*.json; do
			sed "s|\"library_path\"[[:space:]]*:[[:space:]]*\"[^\"]*\"|\"library_path\": \"$esc\"|" "$j" > build/.layer.json.new
			mv -f build/.layer.json.new "build/$(basename "$j")"
		done
	fi
}

install_files() {
	link "$dir/$TARGET" "$bin/$NAME"
	# this fork's own Vulkan WSI layer, user level: the compositor looks for this manifest at startup
	for j in build/"$LAYER".*.json; do
		[ -f "$j" ] || continue
		link "$dir/$j" "$layerdir/$(basename "$j")"
	done
	for s in build/libVkLayer_RITZ_gamescope_wsi_*.so; do
		[ -f "$s" ] || continue
		link "$dir/$s" "$layerdir/$(basename "$s")"
	done
	[ ! -e /usr/bin/gamescope-ritz ] || echo "note: /usr/bin/gamescope-ritz is a leftover of the old installer; you may delete it (/usr/local/bin comes first in PATH)"
	[ ! -e /usr/local/share/vulkan/implicit_layer.d/VkLayer_RITZ_gamescope_wsi.x86_64.json ] \
		|| echo "note: /usr/local/share/vulkan/implicit_layer.d/ holds an old copy of the layer manifest; you may delete it"
	for s in /usr/local/lib*/libVkLayer_RITZ_gamescope_wsi_*.so; do
		[ ! -e "$s" ] || echo "note: $s is an old copy of the layer library; you may delete it (by hand, as root)"
	done
	echo "note: real-time scheduling needs, as root: setcap cap_sys_nice+ep $dir/$TARGET (repeat after every rebuild)"
}

remove_files() {
	unlink_ours "$bin/$NAME"
	for l in "$layerdir/$LAYER".*.json "$layerdir"/libVkLayer_RITZ_gamescope_wsi_*.so; do
		[ -L "$l" ] || continue
		unlink_ours "$l"
	done
}
# ---- end per-program section -----------------------------------------------------------

# ---- ritz-get plumbing: do not edit below ----------------------------------------------
usage() { echo "usage: $0 --install | --remove [--yes]" >&2; exit 2; }
mode= yes=
for a; do
	case $a in
	--install|--remove) [ -z "$mode" ] || usage; mode=$a ;;
	--yes) yes=1 ;;
	*) usage ;;
	esac
done
[ -n "$mode" ] || usage
[ "$(id -u)" -ne 0 ] || { echo "$NAME: do not run install.sh as root" >&2; exit 1; }

first=; [ -L "$bin/$NAME" ] || first=1      # first install = no ~/.local/bin/NAME link yet (decided before anything changes)
stable=$HOME/.local/share/ritz-get/links/$NAME    # fixed path (not XDG): the system link points here, so it never needs root again
sys=/usr/local/bin/$NAME
need_root= missing= pcdone=
# $yes is 1 under --yes: if you ever ask a question, ask only when it is empty. With --yes never read stdin.

link() { mkdir -p "$(dirname "$2")"; ln -sfn "$1" "$2"; }       # link TARGET LINK  (absolute TARGET)
unlink_ours() {                                                  # delete LINK only if it points into this clone
	case $(readlink "$1" 2>/dev/null || true) in "$dir"/*) rm -f "$1"; echo "removed $1" ;; esac
}
miss() { echo "missing: $1 (try: $2)" >&2; missing=1; }
need() {
	t=$1; shift
	command -v "$t" >/dev/null 2>&1 || miss "$t" "$*"
}
need_pc() {
	if [ -z "$pcdone" ]; then pcdone=1; need pkg-config pacman:pkgconf dnf:pkgconf-pkg-config apt:pkg-config; fi
	command -v pkg-config >/dev/null 2>&1 || return 0
	l=$1; shift
	pkg-config --exists "$l" 2>/dev/null || miss "$l" "$*"
}

# The only root: one call, ln -sfn / rm -f, values as arguments (pkexec drops the environment).
run_root() {                                                     # run_root 'SCRIPT using "$1" "$2"' ARG...
	r=0; s=$1; shift
	$RITZ_GET_SUDO /bin/sh -c "$s" sh "$@" || r=$?
	case $r in
	0) ;;
	126) echo "cancelled: no root permission was given" >&2; exit 1 ;;
	127) echo "needs root: no password agent running; run ritz-get ${mode#--} $NAME in a terminal" >&2; exit 4 ;;
	*) echo "$NAME: the root step failed (exit $r)" >&2; exit 1 ;;
	esac
}
root_check() {                                                   # before the build: exit 1 / 4 now, nothing changed yet
	[ "$ROOT_LINK" = yes ] || return 0
	cur=$(readlink "$sys" 2>/dev/null || true)
	[ "$cur" = "$stable" ] && return 0
	if [ -e "$sys" ] || [ -L "$sys" ]; then
		# only a dead ritz-get link may be replaced; a user file, a distro file or a live link is never ours
		if [ -L "$sys" ] && [ ! -e "$sys" ]; then
			case $cur in */ritz-get/links/"$NAME") ;; *) cur= ;; esac
		else cur=; fi
		[ -n "$cur" ] || { echo "$NAME: $sys exists and is not ours; not replacing it" >&2; exit 1; }
	fi
	need_root=1
	[ -n "${RITZ_GET_SUDO:-}" ] || { echo "needs root: run ritz-get install $NAME" >&2; exit 4; }
}
root_link() {                                                    # after the build
	[ "$ROOT_LINK" = yes ] || return 0
	link "$dir/$TARGET" "$stable"                                # user level: follows the clone wherever it moves
	[ -n "$need_root" ] || return 0
	run_root 'ln -sfn "$1" "$2"' "$stable" "$sys"
}
root_unlink() {                                                  # first thing in --remove
	[ "$ROOT_LINK" = yes ] || return 0
	[ "$(readlink "$sys" 2>/dev/null || true)" = "$stable" ] || return 0
	[ -n "${RITZ_GET_SUDO:-}" ] || { echo "needs root: run ritz-get remove $NAME" >&2; exit 4; }
	run_root 'rm -f "$1"' "$sys"
}

case $mode in
--install)
	check_tools
	[ -z "$missing" ] || exit 3
	root_check
	mkdir -p build
	build
	install_files
	root_link
	echo "$NAME installed"
	case ":$PATH:" in *":$bin:"*) ;; *) echo "note: $bin is not in your PATH" ;; esac
	;;
--remove)
	root_unlink
	remove_files
	if [ "$ROOT_LINK" = yes ]; then unlink_ours "$stable"; fi
	echo "$NAME removed (config and user data left alone)"
	;;
esac
