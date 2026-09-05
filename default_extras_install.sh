#!/usr/bin/env sh

# Remove old gamescope-ritz default configs and add our own.
#
# Namespaced to share/gamescope-ritz (not plain share/gamescope): this
# fork installs alongside a packaged /usr/bin/gamescope as
# /usr/bin/gamescope-ritz (see scripts/install-gamescope-ritz.sh), so with
# --prefix /usr its data directory must not collide with the packaged
# gamescope's own share/gamescope/{scripts,looks}.
GAMESCOPE_RITZ_DATA_DIR="${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope-ritz"

# Hard safety gate for the rm -rf calls below. Refuses unless the resolved
# target is exactly one of gamescope-ritz's own data subdirectories under
# GAMESCOPE_RITZ_DATA_DIR -- never anything else, in particular never a path
# under plain share/gamescope, which a distro-packaged gamescope (or any
# other install) may own. This exists because the previous, unnamespaced
# version of this script did "rm -rf $prefix/share/gamescope/scripts" etc.,
# which under --prefix /usr would delete a working packaged gamescope's own
# scripts/looks -- namespacing the path (above) fixes that as a side
# effect, but this guard makes the rm -rf itself unable to reach a foreign
# directory even if the path variable above were ever miscomputed.
gcr_extras_safe_rm() {
	target="$1"
	case "$target" in
		"${GAMESCOPE_RITZ_DATA_DIR}"/scripts|"${GAMESCOPE_RITZ_DATA_DIR}"/looks|"${GAMESCOPE_RITZ_DATA_DIR}"/fonts)
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

# Issue #53: the bundled Geist Sans/Mono weights are compiled directly into the
# binary (Overlay/fonts/embed_font.py via src/meson.build's font_embed_gen), not
# copied from disk at runtime -- but the OFL requires the license to travel
# with any redistribution of the font, and shipping a compositor binary with
# the glyphs baked into its atlas counts as redistribution. So install just
# the license text here, alongside every other gamescope-ritz data asset,
# rather than leaving it sitting only in the source tree.
gcr_extras_safe_rm "${GAMESCOPE_RITZ_DATA_DIR}/fonts"
mkdir -p "${GAMESCOPE_RITZ_DATA_DIR}/fonts"
cp "${MESON_SOURCE_ROOT}/src/Overlay/fonts/LICENSE-OFL.txt" "${GAMESCOPE_RITZ_DATA_DIR}/fonts/LICENSE-OFL.txt"

