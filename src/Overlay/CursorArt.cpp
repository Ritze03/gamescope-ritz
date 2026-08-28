#include "CursorArt.h"

#include <algorithm>

#include "Palette.h"
#include "PanelCursor.h"

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

		// Packs a 0xRRGGBB value (as stored by PanelCursor.h's
		// CursorAppearance) into an opaque ImU32 in ImGui's own byte order.
		ImU32 PackOpaque( uint32_t uRgb )
		{
			return IM_COL32( ( uRgb >> 16 ) & 0xffu, ( uRgb >> 8 ) & 0xffu, uRgb & 0xffu, 255 );
		}
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

		// gamescope::GetCursorAppearance() is the Cursor tab's cached,
		// already-resolved config: uOutlineRgb is the outline colour to draw
		// this frame whether or not it is following the accent (PanelCursor.
		// cpp's GetCursorAppearance() does that resolution itself). Clamp
		// defensively here even though the tab's own sliders already
		// constrain their range (ConfigSchema.h's cursor_scale/
		// cursor_outline_width comments) -- this value also reaches us from
		// a hand-edited global.json, which the UI never gets a chance to
		// clamp.
		const gamescope::CursorAppearance appearance = gamescope::GetCursorAppearance();
		const float flUserScale = std::clamp( appearance.flScale, 0.5f, 3.0f );
		const float flOutlineWidth = std::clamp( appearance.flOutlineWidth, 1.0f, 6.0f );
		flScale *= flUserScale;

		const ImVec2 vecPoints[ 3 ] =
		{
			ImVec2( flTipX  + kTipX  * flScale, flTipY + kTipY  * flScale ),
			ImVec2( flTipX  + kFootX * flScale, flTipY + kFootY * flScale ),
			ImVec2( flTipX  + kWingX * flScale, flTipY + kWingY * flScale ),
		};

		// Inlay first, outline over it: the stroke straddles the path, so
		// filling underneath leaves no seam where the two meet.
		pDrawList->AddConvexPolyFilled( vecPoints, 3, PackOpaque( appearance.uInlayRgb ) );
		pDrawList->AddPolyline( vecPoints, 3, PackOpaque( appearance.uOutlineRgb ),
		                        ImDrawFlags_Closed, flOutlineWidth * flScale );
	}
}
