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
	// ---- the shape, in pixels, tip at the origin --------------------------
	//
	// A classic pointer silhouette reduced to its three corners: vertical left
	// edge, long hypotenuse back up to the tip. Not an equilateral triangle --
	// that reads as an icon, not a pointer. The ~0.68 width:height ratio is
	// what makes it read as an arrow at a glance.
	//
	// Public (not CursorArt.cpp-local) so a third consumer can build the same
	// triangle without a fourth copy of these numbers: UI/Icons.cpp's Cursor
	// tab rail icon reads them directly, at its own fixed scale, alongside
	// CursorArt_Draw() (the overlay's own pointer) and CursorArt_Rasterise()
	// (the game-side fallback) below -- see this file's SCOPE comment for why
	// a single definition of this triangle is load-bearing.
	inline constexpr float kTipX = 0.0f,  kTipY = 0.0f;    // path vertex -- NOT the hotspot, see CursorArt_TipOffset()
	inline constexpr float kFootX = 0.0f, kFootY = 20.0f;  // bottom of the left edge
	inline constexpr float kWingX = 13.5f, kWingY = 14.2f; // the right corner

	// The outline is a CENTRED stroke: it extends flOutlineWidth/2 outward
	// from the (kTipX, kTipY)/kFoot/kWing path, so the visible tip of the
	// drawn arrow sits outside that path vertex, not on it -- the hotspot
	// needs to move out to meet it. How far depends on the JOIN each
	// renderer actually draws at the tip corner, and the two renderers
	// genuinely draw different joins (this is pre-existing, not something
	// this offset changes):
	//
	//   * CursorArt_Draw() strokes with ImGui's AddPolyline(), which miters
	//     sharp corners: the outer boundary is pushed out along the interior
	//     angle's bisector by (flOutlineWidth/2)/sin(theta/2), theta being
	//     the interior angle at the tip (verified against imgui_draw.cpp's
	//     own IM_FIXNORMAL2F join math -- this is exact, not an
	//     approximation).
	//   * CursorArt_Rasterise() is a signed-distance fill, which always
	//     rounds a corner: for any point beyond the tip vertex's edge-normal
	//     cone, the nearest boundary point is the vertex itself, so the
	//     outer edge there is a circular arc of radius flOutlineWidth/2
	//     centred on the vertex -- reaching only flOutlineWidth/2 out, not
	//     the larger miter distance above.
	//
	// Neither renderer's actual drawing changes here -- these compute where
	// each one's EXISTING pixels really end, from the shared geometry
	// constants above, sharing the corner's bisector direction and half-
	// angle so the two formulas can't drift apart even though their
	// magnitudes differ. See superdoc/features/cursor-pipeline.md.
	void CursorArt_TipMiterOffset( float flOutlineWidth, float *pflOffsetX, float *pflOffsetY );
	void CursorArt_TipRoundOffset( float flOutlineWidth, float *pflOffsetX, float *pflOffsetY );

	// Draws with the visible tip -- the true hotspot -- landing exactly on
	// (flTipX, flTipY).
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
