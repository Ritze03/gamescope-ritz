#!/usr/bin/env sh

# Remove old gamescope-ritz default configs and add our own.
#
# Namespaced to share/gamescope-ritz (not plain share/gamescope): this
# fork installs alongside a packaged /usr/bin/gamescope as
# /usr/bin/gamescope-ritz (see scripts/install-gamescope-ritz.sh), so with
# --prefix /usr its data directory must not collide with the packaged
# gamescope's own share/gamescope/{scripts,looks,reshade}.
GAMESCOPE_RITZ_DATA_DIR="${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope-ritz"

# Hard safety gate for the rm -rf calls below. Refuses unless the resolved
# target is exactly one of gamescope-ritz's own data subdirectories under
# GAMESCOPE_RITZ_DATA_DIR -- never anything else, in particular never a path
# under plain share/gamescope, which a distro-packaged gamescope (or any
# other install) may own. This exists because the previous, unnamespaced
# version of this script did "rm -rf $prefix/share/gamescope/scripts" etc.,
# which under --prefix /usr would delete a working packaged gamescope's own
# scripts/looks/reshade -- namespacing the path (above) fixes that as a side
# effect, but this guard makes the rm -rf itself unable to reach a foreign
# directory even if the path variable above were ever miscomputed.
gcr_extras_safe_rm() {
	target="$1"
	case "$target" in
		"${GAMESCOPE_RITZ_DATA_DIR}"/scripts|"${GAMESCOPE_RITZ_DATA_DIR}"/looks|"${GAMESCOPE_RITZ_DATA_DIR}"/reshade)
			rm -rf "$target"
			;;
		*)
			echo "default_extras_install.sh: refusing to rm -rf '$target': not one of gamescope-ritz's own data subdirectories under ${GAMESCOPE_RITZ_DATA_DIR}" >&2
			exit 1
			;;
	esac
}

mkdir -p "${GAMESCOPE_RITZ_DATA_DIR}"
gcr_extras_safe_rm "${GAMESCOPE_RITZ_DATA_DIR}/scripts"
gcr_extras_safe_rm "${GAMESCOPE_RITZ_DATA_DIR}/looks"
cp -r "${MESON_SOURCE_ROOT}/scripts" "${GAMESCOPE_RITZ_DATA_DIR}/scripts"
cp -r "${MESON_SOURCE_ROOT}/looks" "${GAMESCOPE_RITZ_DATA_DIR}/looks"

# M6: the built-in Vibrancy/Pre-Sharpen ReShade effect (superdoc/planning/SPEC.md
# Feature 2). ReshadeEffectPipeline::init() (src/reshade_effect_manager.cpp) loads
# FX source from $prefix/share/gamescope-ritz/reshade/Shaders/<path>, local-usr
# ($HOME/.local/share/...) first, then this global-usr path -- mirror that layout
# exactly, same copy pattern as scripts/looks above.
gcr_extras_safe_rm "${GAMESCOPE_RITZ_DATA_DIR}/reshade"
cp -r "${MESON_SOURCE_ROOT}/reshade" "${GAMESCOPE_RITZ_DATA_DIR}/reshade"
