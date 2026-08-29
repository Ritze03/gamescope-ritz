#include "CursorArt.h"

#include <algorithm>
#include <cmath>

#include "Palette.h"
#include "PanelCursor.h"

#include "imgui.h"


namespace gamescope::overlay
{
	namespace
	{
		// kTipX/kFootY/kWingX and friends: moved to CursorArt.h so
		// UI/Icons.cpp can reuse the exact same triangle for the Cursor tab's
		// rail icon -- see that header's comment on them.

		// Packs a 0xRRGGBB value (as stored by PanelCursor.h's
		// CursorAppearance) into an opaque ImU32 in ImGui's own byte order.
		ImU32 PackOpaque( uint32_t uRgb )
		{
			return IM_COL32( ( uRgb >> 16 ) & 0xffu, ( uRgb >> 8 ) & 0xffu, uRgb & 0xffu, 255 );
		}

		// Signed distance from p to the triangle, negative inside. The usual
		// three-edge formulation: distance to the nearest edge segment, signed
		// by which side of the winding p falls on. Used only by the
		// rasteriser below -- CursorArt_Draw() gets its antialiasing and
		// outline/inlay split from ImGui's own polygon fill/stroke instead.
		float SignedDistanceToTriangle( float px, float py,
		                                float ax, float ay,
		                                float bx, float by,
		                                float cx, float cy )
		{
			const float e0x = bx - ax, e0y = by - ay;
			const float e1x = cx - bx, e1y = cy - by;
			const float e2x = ax - cx, e2y = ay - cy;
			const float v0x = px - ax, v0y = py - ay;
			const float v1x = px - bx, v1y = py - by;
			const float v2x = px - cx, v2y = py - cy;

			const auto SegDistSq = [ & ]( float vx, float vy, float ex, float ey )
			{
				const float flLenSq = ex * ex + ey * ey;
				const float flT = flLenSq > 0.0f
					? std::clamp( ( vx * ex + vy * ey ) / flLenSq, 0.0f, 1.0f )
					: 0.0f;
				const float dx = vx - ex * flT;
				const float dy = vy - ey * flT;
				return dx * dx + dy * dy;
			};

			const float flDistSq = std::min( std::min(
				SegDistSq( v0x, v0y, e0x, e0y ),
				SegDistSq( v1x, v1y, e1x, e1y ) ),
				SegDistSq( v2x, v2y, e2x, e2y ) );

			const float flWinding = e0x * e2y - e0y * e2x;
			const float flSign = flWinding < 0.0f ? -1.0f : 1.0f;
			const float flInside = std::min( std::min(
				flSign * ( v0x * e0y - v0y * e0x ),
				flSign * ( v1x * e1y - v1y * e1x ) ),
				flSign * ( v2x * e2y - v2y * e2x ) );

			return std::sqrt( flDistSq ) * ( flInside > 0.0f ? -1.0f : 1.0f );
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

	bool CursorArt_Rasterise( std::vector<uint32_t> &vecArgb,
	                          int *pnWidth, int *pnHeight,
	                          int *pnHotX, int *pnHotY )
	{
		// Same resolved appearance CursorArt_Draw() reads -- one definition
		// of what the pointer looks like, whichever renderer is asking.
		const gamescope::CursorAppearance appearance = gamescope::GetCursorAppearance();
		const float flScale = std::clamp( appearance.flScale, 0.5f, 3.0f );
		const float flOutlineWidth = std::clamp( appearance.flOutlineWidth, 1.0f, 6.0f ) * flScale;
		const float flOutlineHalf = flOutlineWidth * 0.5f;

		// Bitmap padding around the silhouette: half the stroke, plus one
		// pixel for the antialiased falloff to land in.
		const float flPad = flOutlineHalf + 1.0f;

		const int nWidth  = (int)std::ceil( kWingX * flScale + flPad * 2.0f );
		const int nHeight = (int)std::ceil( kFootY * flScale + flPad * 2.0f );
		if ( nWidth <= 0 || nHeight <= 0 )
			return false;

		const uint32_t uOutline = appearance.uOutlineRgb;
		const float flOutlineR = (float)( ( uOutline >> 16 ) & 0xffu );
		const float flOutlineG = (float)( ( uOutline >> 8 ) & 0xffu );
		const float flOutlineB = (float)( uOutline & 0xffu );

		const uint32_t uInlay = appearance.uInlayRgb;
		const float flInlayR = (float)( ( uInlay >> 16 ) & 0xffu );
		const float flInlayG = (float)( ( uInlay >> 8 ) & 0xffu );
		const float flInlayB = (float)( uInlay & 0xffu );

		// Shift the scaled geometry by flPad so the stroke's outer edge fits.
		const float ax = kTipX  * flScale + flPad, ay = kTipY  * flScale + flPad;
		const float bx = kFootX * flScale + flPad, by = kFootY * flScale + flPad;
		const float cx = kWingX * flScale + flPad, cy = kWingY * flScale + flPad;

		vecArgb.assign( (size_t)nWidth * nHeight, 0u );

		for ( int y = 0; y < nHeight; y++ )
		{
			for ( int x = 0; x < nWidth; x++ )
			{
				const float px = (float)x + 0.5f;
				const float py = (float)y + 0.5f;
				const float flDist =
					SignedDistanceToTriangle( px, py, ax, ay, bx, by, cx, cy );

				// Silhouette coverage: fades out across the one pixel
				// straddling the stroke's outer edge.
				const float flAlpha =
					std::clamp( ( flOutlineHalf - flDist ) + 0.5f, 0.0f, 1.0f );
				if ( flAlpha <= 0.0f )
					continue;

				// How far inside the stroke's inner edge we are: 1 = inlay,
				// 0 = outline, fading across one pixel between them.
				const float flInlayT =
					std::clamp( ( -flOutlineHalf - flDist ) + 0.5f, 0.0f, 1.0f );

				const float flR = flOutlineR + ( flInlayR - flOutlineR ) * flInlayT;
				const float flG = flOutlineG + ( flInlayG - flOutlineG ) * flInlayT;
				const float flB = flOutlineB + ( flInlayB - flOutlineB ) * flInlayT;

				// PictStandardARGB32 is PREMULTIPLIED, so the colour is scaled
				// by coverage here rather than at blend time.
				const uint32_t uA = (uint32_t)std::lround( flAlpha * 255.0f );
				const uint32_t uR = (uint32_t)std::lround( flR * flAlpha );
				const uint32_t uG = (uint32_t)std::lround( flG * flAlpha );
				const uint32_t uB = (uint32_t)std::lround( flB * flAlpha );

				vecArgb[ (size_t)y * nWidth + x ] =
					( uA << 24 ) | ( uR << 16 ) | ( uG << 8 ) | uB;
			}
		}

		if ( pnWidth )  *pnWidth  = nWidth;
		if ( pnHeight ) *pnHeight = nHeight;
		// The tip is the hotspot, and it sits flPad in from the bitmap corner.
		if ( pnHotX ) *pnHotX = (int)std::lround( kTipX * flScale + flPad );
		if ( pnHotY ) *pnHotY = (int)std::lround( kTipY * flScale + flPad );
		return true;
	}
}
