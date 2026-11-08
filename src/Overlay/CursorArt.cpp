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

	namespace
	{
		// Signed distance to an arbitrary (possibly concave) closed polygon,
		// negative inside (even-odd). Used for the Pointy style, whose notch
		// the three-edge triangle routine above cannot express.
		float SignedDistanceToPolygon( float px, float py, const float *pfX, const float *pfY, int nCount )
		{
			float flDistSq = 1e30f;
			bool bInside = false;
			for ( int i = 0, j = nCount - 1; i < nCount; j = i++ )
			{
				const float ex = pfX[ i ] - pfX[ j ], ey = pfY[ i ] - pfY[ j ];
				const float vx = px - pfX[ j ], vy = py - pfY[ j ];
				const float flLenSq = ex * ex + ey * ey;
				const float flT = flLenSq > 0.0f ? std::clamp( ( vx * ex + vy * ey ) / flLenSq, 0.0f, 1.0f ) : 0.0f;
				const float dx = vx - ex * flT, dy = vy - ey * flT;
				flDistSq = std::min( flDistSq, dx * dx + dy * dy );
				if ( ( pfY[ i ] > py ) != ( pfY[ j ] > py ) &&
				     px < pfX[ j ] + ex * ( py - pfY[ j ] ) / ey )
					bInside = !bInside;
			}
			return std::sqrt( flDistSq ) * ( bInside ? -1.0f : 1.0f );
		}
	}

	namespace
	{
		// The tip corner's outward bisector direction and sin(interior
		// angle/2) -- everything both CursorArt_TipMiterOffset() and
		// CursorArt_TipRoundOffset() need, computed once from the shared
		// geometry constants so the two formulas can't disagree about the
		// corner itself, only (correctly) about how far each renderer's own
		// join reaches along it. Scale-invariant: kFootX/Y and kWingX/Y
		// scale uniformly, so the angle between them doesn't change with it.
		bool TipBisector( float *pflDirX, float *pflDirY, float *pflSinHalfTheta )
		{
			const float flToFootX = kFootX - kTipX, flToFootY = kFootY - kTipY;
			const float flToWingX = kWingX - kTipX, flToWingY = kWingY - kTipY;
			const float flFootLen = std::sqrt( flToFootX * flToFootX + flToFootY * flToFootY );
			const float flWingLen = std::sqrt( flToWingX * flToWingX + flToWingY * flToWingY );
			if ( flFootLen <= 0.0001f || flWingLen <= 0.0001f )
				return false;

			const float flU1x = flToFootX / flFootLen, flU1y = flToFootY / flFootLen;
			const float flU2x = flToWingX / flWingLen, flU2y = flToWingY / flWingLen;

			// Interior angle at the tip between the two edges.
			const float flCosTheta = std::clamp( flU1x * flU2x + flU1y * flU2y, -1.0f, 1.0f );
			const float flSinHalfTheta = std::sin( std::acos( flCosTheta ) * 0.5f );

			// Outward direction: the negated, normalised sum of the two edge
			// unit vectors -- points away from the shape's interior, straight
			// out through the sharp tip.
			float flBisX = -( flU1x + flU2x );
			float flBisY = -( flU1y + flU2y );
			const float flBisLen = std::sqrt( flBisX * flBisX + flBisY * flBisY );
			if ( flBisLen <= 0.0001f || flSinHalfTheta <= 0.0001f )
				return false;

			*pflDirX = flBisX / flBisLen;
			*pflDirY = flBisY / flBisLen;
			*pflSinHalfTheta = flSinHalfTheta;
			return true;
		}
	}

	void CursorArt_TipMiterOffset( float flOutlineWidth, float *pflOffsetX, float *pflOffsetY )
	{
		float flDirX = 0.0f, flDirY = 0.0f, flSinHalfTheta = 1.0f;
		float flMagnitude = 0.0f;
		if ( TipBisector( &flDirX, &flDirY, &flSinHalfTheta ) )
			flMagnitude = ( flOutlineWidth * 0.5f ) / flSinHalfTheta;

		if ( pflOffsetX ) *pflOffsetX = flDirX * flMagnitude;
		if ( pflOffsetY ) *pflOffsetY = flDirY * flMagnitude;
	}

	void CursorArt_TipRoundOffset( float flOutlineWidth, float *pflOffsetX, float *pflOffsetY )
	{
		float flDirX = 0.0f, flDirY = 0.0f, flSinHalfTheta = 1.0f;
		float flMagnitude = 0.0f;
		if ( TipBisector( &flDirX, &flDirY, &flSinHalfTheta ) )
			flMagnitude = flOutlineWidth * 0.5f;

		if ( pflOffsetX ) *pflOffsetX = flDirX * flMagnitude;
		if ( pflOffsetY ) *pflOffsetY = flDirY * flMagnitude;
	}

	void CursorArt_PointyTipMiterOffset( float flOutlineWidth, float *pflOffsetX, float *pflOffsetY )
	{
		// Edges leave the tip towards the last vertex and the first wing.
		float x0, y0, x1, y1, x3, y3;
		CursorArt_PointyVertex( 0, 0.0f, 0.0f, 1.0f, &x0, &y0 );
		CursorArt_PointyVertex( 1, 0.0f, 0.0f, 1.0f, &x1, &y1 );
		CursorArt_PointyVertex( kPointyCount - 1, 0.0f, 0.0f, 1.0f, &x3, &y3 );
		const float l1 = std::sqrt( x1 * x1 + y1 * y1 ), l3 = std::sqrt( x3 * x3 + y3 * y3 );
		float flDirX = 0.0f, flDirY = 0.0f, flMagnitude = 0.0f;
		if ( l1 > 0.0001f && l3 > 0.0001f )
		{
			const float u1x = x1 / l1, u1y = y1 / l1, u3x = x3 / l3, u3y = y3 / l3;
			const float flCos = std::clamp( u1x * u3x + u1y * u3y, -1.0f, 1.0f );
			const float flSinHalf = std::sin( std::acos( flCos ) * 0.5f );
			const float bx = -( u1x + u3x ), by = -( u1y + u3y );
			const float bl = std::sqrt( bx * bx + by * by );
			if ( bl > 0.0001f && flSinHalf > 0.0001f )
			{
				flDirX = bx / bl; flDirY = by / bl;
				flMagnitude = ( flOutlineWidth * 0.5f ) / flSinHalf;
			}
		}
		if ( pflOffsetX ) *pflOffsetX = flDirX * flMagnitude;
		if ( pflOffsetY ) *pflOffsetY = flDirY * flMagnitude;
	}

	// Rounded-join (signed-distance fill) tip offset for Pointy: along the
	// same bisector as the miter offset, but only flOutlineWidth/2 long.
	static void PointyTipRoundOffset( float flOutlineWidth, float *pflOffsetX, float *pflOffsetY )
	{
		float x1, y1, x3, y3;
		CursorArt_PointyVertex( 1, 0.0f, 0.0f, 1.0f, &x1, &y1 );
		CursorArt_PointyVertex( kPointyCount - 1, 0.0f, 0.0f, 1.0f, &x3, &y3 );
		const float l1 = std::sqrt( x1 * x1 + y1 * y1 ), l3 = std::sqrt( x3 * x3 + y3 * y3 );
		const float bx = -( x1 / l1 + x3 / l3 ), by = -( y1 / l1 + y3 / l3 );
		const float bl = std::sqrt( bx * bx + by * by );
		*pflOffsetX = bl > 0.0001f ? bx / bl * flOutlineWidth * 0.5f : 0.0f;
		*pflOffsetY = bl > 0.0001f ? by / bl * flOutlineWidth * 0.5f : 0.0f;
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

		// The path's tip vertex is not the visible tip -- AddPolyline() below
		// miters this corner, so the drawn point sits out past the path
		// vertex by the miter offset. (flTipX, flTipY) is the true hotspot,
		// i.e. where the *visible* tip must land, so the path is drawn
		// shifted back by that same offset (see CursorArt_TipMiterOffset()'s
		// comment in the header).
		float flOffsetX, flOffsetY;
		const bool bPointy = CursorArt_IsPointy();
		if ( bPointy )
			CursorArt_PointyTipMiterOffset( flOutlineWidth * flScale, &flOffsetX, &flOffsetY );
		else
			CursorArt_TipMiterOffset( flOutlineWidth * flScale, &flOffsetX, &flOffsetY );
		const float flOriginX = flTipX - flOffsetX;
		const float flOriginY = flTipY - flOffsetY;

		if ( bPointy )
		{
			ImVec2 v[ kPointyCount ];
			for ( int i = 0; i < kPointyCount; i++ )
			{
				CursorArt_PointyVertex( i, flOriginX, flOriginY, flScale, &v[ i ].x, &v[ i ].y );
			}
			const ImU32 uInlay = PackOpaque( appearance.uInlayRgb );
			// Concave (notch at v[2]): two triangles sharing the tip-notch
			// diagonal. Anti-aliased fill is switched off for them so no
			// faint seam shows along that interior edge; the outline stroke
			// is centred on the true boundary and covers the hard edges.
			const ImDrawListFlags uOldFlags = pDrawList->Flags;
			pDrawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;
			pDrawList->AddTriangleFilled( v[ 0 ], v[ 1 ], v[ 2 ], uInlay );
			pDrawList->AddTriangleFilled( v[ 0 ], v[ 2 ], v[ 3 ], uInlay );
			pDrawList->Flags = uOldFlags;
			pDrawList->AddPolyline( v, kPointyCount, PackOpaque( appearance.uOutlineRgb ),
			                        ImDrawFlags_Closed, flOutlineWidth * flScale );
			return;
		}

		const ImVec2 vecPoints[ 3 ] =
		{
			ImVec2( flOriginX + kTipX  * flScale, flOriginY + kTipY  * flScale ),
			ImVec2( flOriginX + kFootX * flScale, flOriginY + kFootY * flScale ),
			ImVec2( flOriginX + kWingX * flScale, flOriginY + kWingY * flScale ),
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

		// Pointy: polygon vertices (tip at the origin) instead of the triangle.
		const bool bPointy = CursorArt_IsPointy();
		float flPolyX[ kPointyCount ], flPolyY[ kPointyCount ];
		float flMaxX = kWingX, flMaxY = kFootY;
		if ( bPointy )
		{
			flMaxX = flMaxY = 0.0f;
			for ( int i = 0; i < kPointyCount; i++ )
			{
				CursorArt_PointyVertex( i, 0.0f, 0.0f, 1.0f, &flPolyX[ i ], &flPolyY[ i ] );
				flMaxX = std::max( flMaxX, flPolyX[ i ] );
				flMaxY = std::max( flMaxY, flPolyY[ i ] );
			}
			for ( int i = 0; i < kPointyCount; i++ )
			{
				flPolyX[ i ] = flPolyX[ i ] * flScale + flPad;
				flPolyY[ i ] = flPolyY[ i ] * flScale + flPad;
			}
		}

		const int nWidth  = (int)std::ceil( flMaxX * flScale + flPad * 2.0f );
		const int nHeight = (int)std::ceil( flMaxY * flScale + flPad * 2.0f );
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
				const float flDist = bPointy
					? SignedDistanceToPolygon( px, py, flPolyX, flPolyY, kPointyCount )
					: SignedDistanceToTriangle( px, py, ax, ay, bx, by, cx, cy );

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
		// The hotspot is the VISIBLE tip -- the outer corner of the centred
		// stroke -- not the path vertex the geometry constants place at
		// (kTipX, kTipY). This fill is a signed-distance dilation, which
		// ROUNDS the corner rather than mitering it (see the header comment
		// on CursorArt_TipRoundOffset()), so it reaches only flOutlineWidth/2
		// out along the bisector -- NOT CursorArt_Draw()'s larger miter
		// offset, which would overshoot past this shape's actual opaque
		// pixels. Added rather than subtracted here since ax/ay above is the
		// path vertex and the offset points outward from it towards the
		// drawn point.
		float flOffsetX, flOffsetY;
		if ( bPointy )
			PointyTipRoundOffset( flOutlineWidth, &flOffsetX, &flOffsetY );
		else
			CursorArt_TipRoundOffset( flOutlineWidth, &flOffsetX, &flOffsetY );
		// Pointy's tip vertex is also at (flPad, flPad): tip at the origin.
		if ( pnHotX ) *pnHotX = (int)std::lround( kTipX * flScale + flPad + flOffsetX );
		if ( pnHotY ) *pnHotY = (int)std::lround( kTipY * flScale + flPad + flOffsetY );
		return true;
	}
}
