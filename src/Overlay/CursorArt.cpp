#include "CursorArt.h"
#include "Palette.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

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

		// Bitmap padding around the silhouette: half the stroke, plus one
		// pixel for the antialiased falloff to land in.
		constexpr float kPad = kOutlineHalf + 1.0f;

		// Signed distance from p to the triangle, negative inside. The usual
		// three-edge formulation: distance to the nearest edge segment, signed
		// by which side of the winding p falls on.
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

	bool CursorArt_Rasterise( std::vector<uint32_t> &vecArgb,
	                          int *pnWidth, int *pnHeight,
	                          int *pnHotX, int *pnHotY )
	{
		const int nWidth  = (int)std::ceil( kWingX + kPad * 2.0f );
		const int nHeight = (int)std::ceil( kFootY + kPad * 2.0f );
		if ( nWidth <= 0 || nHeight <= 0 )
			return false;

		const uint32_t uAccent = CursorArt_AccentRgb();
		const float flAccentR = (float)( ( uAccent >> 16 ) & 0xffu );
		const float flAccentG = (float)( ( uAccent >> 8 ) & 0xffu );
		const float flAccentB = (float)( uAccent & 0xffu );

		// Shift the geometry by kPad so the stroke's outer edge fits.
		const float ax = kTipX + kPad,  ay = kTipY + kPad;
		const float bx = kFootX + kPad, by = kFootY + kPad;
		const float cx = kWingX + kPad, cy = kWingY + kPad;

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
					std::clamp( ( kOutlineHalf - flDist ) + 0.5f, 0.0f, 1.0f );
				if ( flAlpha <= 0.0f )
					continue;

				// How far inside the stroke's inner edge we are: 1 = inlay,
				// 0 = outline, fading across one pixel between them.
				const float flInlay =
					std::clamp( ( -kOutlineHalf - flDist ) + 0.5f, 0.0f, 1.0f );

				const float flR = flAccentR * ( 1.0f - flInlay );
				const float flG = flAccentG * ( 1.0f - flInlay );
				const float flB = flAccentB * ( 1.0f - flInlay );

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
		// The tip is the hotspot, and it sits kPad in from the bitmap corner.
		if ( pnHotX ) *pnHotX = (int)std::lround( kTipX + kPad );
		if ( pnHotY ) *pnHotY = (int)std::lround( kTipY + kPad );
		return true;
	}
}
