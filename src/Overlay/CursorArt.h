// The pointer the settings overlay draws for itself: a plain triangle, accent-
// coloured outline, black inlay. The black-inside-colour combination is the
// point -- it stays readable over both dark and bright game content without a
// drop shadow.
//
// SCOPE. This is the geometry gamescope draws for its OWN pointer, and
// nothing else -- it is drawn two ways, sharing one definition of the shape
// so the two can never drift apart (that drift was the original #69 bug):
//
//   * the settings overlay draws it with ImGui, as vector geometry, straight
//     into its own texture (CursorArt_Draw) -- always, whenever the overlay
//     is open;
//   * the game side gets it rasterised to a small premultiplied-ARGB32
//     bitmap for MouseCursor::setCursorImage() (CursorArt_Rasterise), but
//     ONLY when the Cursor tab's "Use everywhere" toggle is on
//     (PanelCursor.h's CursorAppearance::bEverywhere). Off (default), the
//     game-side cursor is untouched, exactly as upstream -- see
//     superdoc/features/cursor-pipeline.md for why that used to be this
//     file's whole story and why it changed.
//
// Both renderers read PanelCursor.h's GetCursorAppearance() for scale,
// outline width and colours, so the two also look identical to each other,
// not just share corners.
#pragma once

#include <cstdint>
#include <vector>

struct ImDrawList;

namespace gamescope::overlay
{
	// Draws with the tip -- the hotspot -- landing exactly on (flTipX, flTipY).
	void CursorArt_Draw( ImDrawList *pDrawList, float flTipX, float flTipY, float flScale );

	// Rasterised path, for X11's game-side fallback cursor. Fills vecArgb
	// with premultiplied ARGB32 (the PictStandardARGB32 layout
	// XRenderCreateCursor wants) and reports the bitmap size and the hotspot
	// within it. Returns false only if the geometry came out degenerate,
	// which the caller must treat as "leave the cursor alone" rather than
	// "draw nothing" (steamcompmgr.cpp's SetDefaultCursorImage() falls back
	// to left_ptr in that case).
	bool CursorArt_Rasterise( std::vector<uint32_t> &vecArgb,
	                          int *pnWidth, int *pnHeight,
	                          int *pnHotX, int *pnHotY );

	// The accent the outline is currently drawn in, as 0x00RRGGBB. Follows the
	// user's accent-hue slider.
	uint32_t CursorArt_AccentRgb();
}
