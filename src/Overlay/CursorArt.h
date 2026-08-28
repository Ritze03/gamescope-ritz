// The one pointer gamescope draws for itself: a plain triangle, accent-coloured
// outline, black inlay.
//
// WHY ONE FILE FOR TWO RENDERERS. The pointer has to appear in two completely
// different pipelines and they must not drift apart -- that disagreement is the
// bug this replaced (see superdoc/features/cursor-pipeline.md):
//
//   * the settings overlay draws it with ImGui, as vector geometry, straight
//     into its own texture (CursorArt_Draw);
//   * the game side needs an X11 cursor, so the same geometry is rasterised to
//     a small ARGB32 bitmap and handed to MouseCursor::setCursorImage()
//     (CursorArt_Rasterise).
//
// Both read the shape from the same table in CursorArt.cpp, so there is exactly
// one description of what the pointer looks like.
//
// There is no setting. An earlier attempt at this shipped a "Use system cursor
// theme" toggle and a libXcursor theme lookup; both are gone.
#pragma once

#include <cstdint>
#include <vector>

struct ImDrawList;

namespace gamescope::overlay
{
	// Vector path, for the overlay. Draws with the tip -- the hotspot -- landing
	// exactly on (flTipX, flTipY), so it lines up with the rasterised bitmap
	// below when the two are shown in turn.
	void CursorArt_Draw( ImDrawList *pDrawList, float flTipX, float flTipY, float flScale );

	// Rasterised path, for X11. Fills vecArgb with premultiplied ARGB32 (the
	// PictStandardARGB32 layout XRenderCreateCursor wants) and reports the
	// bitmap size and the hotspot within it. Returns false only if the geometry
	// came out degenerate, which the caller must treat as "leave the cursor
	// alone" rather than "draw nothing".
	bool CursorArt_Rasterise( std::vector<uint32_t> &vecArgb,
	                          int *pnWidth, int *pnHeight,
	                          int *pnHotX, int *pnHotY );

	// The accent the two renderers above are currently using, as 0x00RRGGBB.
	// Changes when the user moves the accent-hue slider.
	uint32_t CursorArt_AccentRgb();
}
