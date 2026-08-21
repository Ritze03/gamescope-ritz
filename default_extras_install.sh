#!/usr/bin/env sh

# Remove old Gamescope default configs and add our own.
mkdir -p "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope"
rm -rf "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/scripts" || true
rm -rf "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/looks" || true
cp -r "${MESON_SOURCE_ROOT}/scripts" "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/scripts"
cp -r "${MESON_SOURCE_ROOT}/looks" "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/looks"

# M6: the built-in Vibrancy/Pre-Sharpen ReShade effect (superdoc/planning/SPEC.md
# Feature 2). ReshadeEffectPipeline::init() (src/reshade_effect_manager.cpp) loads
# FX source from $prefix/share/gamescope/reshade/Shaders/<path>, local-usr
# ($HOME/.local/share/...) first, then this global-usr path -- mirror that layout
# exactly, same copy pattern as scripts/looks above.
rm -rf "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/reshade" || true
cp -r "${MESON_SOURCE_ROOT}/reshade" "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/reshade"
