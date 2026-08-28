#include "CursorArt.h"

#include "Palette.h"

#include "imgui.h"


namespace gamescope::overlay
{
	namespace
	{
		// ---- the shape, in pixels, tip at the origin --------------------
		//
		// A classic pointer silhouette reduced to its three corners: vertical
		// left edge, long hypotenuse back up to the tip. Not an equilateral
		// triangle -- that reads as an icon, not a pointer. The ~0.68
		// width:height ratio is what makes it read as an arrow at a glance.
		constexpr float kTipX = 0.0f,  kTipY = 0.0f;   // hotspot
		constexpr float kFootX = 0.0f, kFootY = 20.0f; // bottom of the left edge
		constexpr float kWingX = 13.5f, kWingY = 14.2f; // the right corner

		// Total stroke width. Thin enough to read as a line rather than a
		// slab, thick enough to survive being drawn over bright game content.
		constexpr float kOutlineWidth = 2.0f;
		constexpr float kOutlineHalf = kOutlineWidth * 0.5f;

	}

	uint32_t CursorArt_AccentRgb()
	{
		// kAccent is ImU32, i.e. IM_COL32's little-endian R,G,B,A byte order.
		const ImU32 uAccent = palette::kAccent;
		const uint32_t uR = ( uAccent >> IM_COL32_R_SHIFT ) & 0xffu;
		const uint32_t uG = ( uAccent >> IM_COL32_G_SHIFT ) & 0xffu;
		const uint32_t uB = ( uAccent >> IM_COL32_B_SHIFT ) & 0xffu;
		return ( uR << 16 ) | ( uG << 8 ) | uB;
	}

	void CursorArt_Draw( ImDrawList *pDrawList, float flTipX, float flTipY, float flScale )
	{
		if ( !pDrawList )
			return;

		if ( !( flScale > 0.0f ) )
			flScale = 1.0f;

		const ImVec2 vecPoints[ 3 ] =
		{
			ImVec2( flTipX  + kTipX  * flScale, flTipY + kTipY  * flScale ),
			ImVec2( flTipX  + kFootX * flScale, flTipY + kFootY * flScale ),
			ImVec2( flTipX  + kWingX * flScale, flTipY + kWingY * flScale ),
		};

		// Inlay first, outline over it: the stroke straddles the path, so
		// filling underneath leaves no seam where the two meet.
		pDrawList->AddConvexPolyFilled( vecPoints, 3, IM_COL32( 0, 0, 0, 255 ) );
		pDrawList->AddPolyline( vecPoints, 3, palette::kAccent,
		                        ImDrawFlags_Closed, kOutlineWidth * flScale );
	}
}
