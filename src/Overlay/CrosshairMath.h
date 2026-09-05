// The crosshair's arithmetic, kept free of ImGui, Vulkan and the config
// system so tests/test_crosshair.cpp can pin it down exactly. Two halves:
//
//   1. The hide animation -- HideProgress() turns "right button held since
//      <ns>" plus a time-to-hide into a 0..1 progress, and EvaluateHide()
//      turns that progress plus a mode into three multipliers (alpha, gap,
//      length) the geometry below consumes.
//   2. The geometry -- Build() turns a Style (sizes in whatever unit the
//      caller chose: output pixels, or game pixels with a per-axis scale)
//      into integer-snapped, half-open pixel rectangles: the four arms as a
//      non-overlapping union, the dot, and the outline as a ring strictly
//      OUTSIDE every fill.
//
// Why integer rects and not lines/circles: the user asked for a proper 1px
// mode. Dear ImGui's line primitives are anti-aliased by default and even
// with AA off a 1-wide line lands on a half-pixel; an axis-aligned filled
// rect on whole-pixel coordinates, drawn with anti-aliased fill disabled,
// is the only primitive that yields exactly one solid pixel column with no
// half-alpha neighbours. Everything here is therefore a rect, including the
// dot (always a square, at every size -- superdoc/features/crosshair.md).
//
// Why the union/difference decomposition: with a small gap the arms'
// outlines overlap each other and the dot's, and with gap 0 (the Focus and
// Shrink hide modes drive it there) the arms overlap outright. Drawing the
// overlapping rects one after another at a partial alpha would leave
// visibly darker squares where two of them meet. Decompose() rebuilds each
// set as non-overlapping bands instead, so every pixel of a given element
// is painted exactly once, and the outline is computed as
// expand(everything) minus fill(everything) so it never sits under a
// translucent fill either -- the game shows through a translucent line,
// not the black outline.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace gamescope::crosshair
{
	// ------------------------------------------------------------------
	// Hide animation
	// ------------------------------------------------------------------

	enum class HideMode
	{
		Fade,   // opacity only
		Focus,  // gap closes over the first half, then everything fades over the second
		Shrink, // gap closes over the first half, then arms (and the dot) shrink to nothing
	};

	// The stable on-disk keys config::CrosshairSettings::hide_mode stores.
	// An unrecognised value reads as Fade, the default.
	inline HideMode ParseHideMode( std::string_view sv )
	{
		if ( sv == "focus" )  return HideMode::Focus;
		if ( sv == "shrink" ) return HideMode::Shrink;
		return HideMode::Fade;
	}

	inline const char *HideModeKey( HideMode eMode )
	{
		switch ( eMode )
		{
			case HideMode::Focus:  return "focus";
			case HideMode::Shrink: return "shrink";
			default:               return "fade";
		}
	}

	// 0 while the button is not held (ulPressNs == 0), else the fraction of
	// nTimeToHideMs elapsed since the press, clamped to [0, 1]. A
	// non-positive time-to-hide means "hide at once".
	inline float HideProgress( uint64_t ulPressNs, uint64_t ulNowNs, int nTimeToHideMs )
	{
		if ( ulPressNs == 0 )
			return 0.0f;
		if ( nTimeToHideMs <= 0 || ulNowNs <= ulPressNs )
			return nTimeToHideMs <= 0 ? 1.0f : 0.0f;
		const double flElapsedMs = double( ulNowNs - ulPressNs ) / 1e6;
		return (float)std::clamp( flElapsedMs / double( nTimeToHideMs ), 0.0, 1.0 );
	}

	// Multipliers applied to the Style before Build(): flAlpha scales every
	// element's opacity (outline included), flGap the arms' gap, flLength
	// the arms' length AND the dot's size (Shrink's second half shrinks the
	// dot too -- a dot left behind would defeat the point of hiding).
	struct HideState
	{
		float flAlpha = 1.0f;
		float flGap = 1.0f;
		float flLength = 1.0f;
	};

	inline HideState EvaluateHide( HideMode eMode, float f )
	{
		f = std::clamp( f, 0.0f, 1.0f );
		HideState s;
		switch ( eMode )
		{
			case HideMode::Fade:
				s.flAlpha = 1.0f - f;
				break;
			case HideMode::Focus:
				if ( f < 0.5f )
					s.flGap = 1.0f - 2.0f * f;
				else
				{
					s.flGap = 0.0f;
					s.flAlpha = 2.0f - 2.0f * f;
				}
				break;
			case HideMode::Shrink:
				if ( f < 0.5f )
					s.flGap = 1.0f - 2.0f * f;
				else
				{
					s.flGap = 0.0f;
					s.flLength = 2.0f - 2.0f * f;
				}
				break;
		}
		return s;
	}

	// ------------------------------------------------------------------
	// Geometry
	// ------------------------------------------------------------------

	// Sizes in the caller's unit. With apply_scaling off that unit is the
	// output pixel and Frame::flScaleX/Y are 1; with it on the unit is the
	// game pixel and the scales are "output pixels per game pixel" per axis.
	struct Style
	{
		bool bLine = true;
		float flLength = 6.0f;
		float flWidth = 2.0f;
		float flGap = 3.0f;

		bool bDot = false;
		float flDotSize = 2.0f;

		bool bOutline = true;
		float flOutlineWidth = 1.0f;
	};

	struct Frame
	{
		float flCenterX = 0.0f; // output pixels; the centre of the game's on-screen rect
		float flCenterY = 0.0f;
		float flScaleX = 1.0f;  // output px per unit, per axis
		float flScaleY = 1.0f;
	};

	// Half-open [x0, x1) x [y0, y1) in whole output pixels.
	struct IRect
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		bool Empty() const { return x1 <= x0 || y1 <= y0; }
		bool operator==( const IRect &o ) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
	};

	struct Shape
	{
		std::vector<IRect> outline; // draw first
		std::vector<IRect> lines;   // then the arms (one colour)
		std::vector<IRect> dot;     // then the dot (its own colour); 0 or 1 rect
		bool Empty() const { return outline.empty() && lines.empty() && dot.empty(); }
	};

	namespace detail
	{
		struct Span { int a, b; }; // half-open [a, b)

		inline void MergeSpans( std::vector<Span> &v )
		{
			if ( v.empty() )
				return;
			std::sort( v.begin(), v.end(), []( const Span &l, const Span &r ) { return l.a < r.a; } );
			size_t nOut = 0;
			for ( size_t i = 1; i < v.size(); i++ )
			{
				if ( v[i].a <= v[nOut].b )
					v[nOut].b = std::max( v[nOut].b, v[i].b );
				else
					v[++nOut] = v[i];
			}
			v.resize( nOut + 1 );
		}

		// A minus B, both already merged and sorted.
		inline std::vector<Span> SubtractSpans( const std::vector<Span> &A, const std::vector<Span> &B )
		{
			std::vector<Span> out;
			for ( Span s : A )
			{
				int a = s.a;
				for ( const Span &b : B )
				{
					if ( b.b <= a ) continue;
					if ( b.a >= s.b ) break;
					if ( b.a > a )
						out.push_back( { a, b.a } );
					a = std::max( a, b.b );
					if ( a >= s.b ) break;
				}
				if ( a < s.b )
					out.push_back( { a, s.b } );
			}
			return out;
		}

		// Snaps a centre coordinate to the pixel grid for an element of
		// thickness `t` across that axis: an odd thickness centres on a
		// pixel (a half-integer coordinate), an even one on a pixel edge (an
		// integer coordinate), so `c - t/2` is always a whole pixel and the
		// element is exactly `t` pixels, symmetric about the snapped centre.
		inline double SnapCenter( float c, int t )
		{
			return ( t % 2 != 0 ) ? std::floor( c ) + 0.5 : std::round( c );
		}

		inline int SnapSize( float fl )
		{
			return (int)std::lround( std::max( 0.0f, fl ) );
		}
	}

	// The union of `add` minus the union of `sub`, as non-overlapping rects
	// (horizontal bands, merged vertically where consecutive bands agree).
	inline std::vector<IRect> Decompose( const std::vector<IRect> &add, const std::vector<IRect> &sub )
	{
		using detail::Span;
		std::vector<int> ys;
		for ( const IRect &r : add )
		{
			if ( r.Empty() ) continue;
			ys.push_back( r.y0 );
			ys.push_back( r.y1 );
		}
		for ( const IRect &r : sub )
		{
			if ( r.Empty() ) continue;
			ys.push_back( r.y0 );
			ys.push_back( r.y1 );
		}
		std::sort( ys.begin(), ys.end() );
		ys.erase( std::unique( ys.begin(), ys.end() ), ys.end() );

		std::vector<IRect> out;
		std::vector<Span> prevSpans;
		int nPrevY0 = 0, nPrevY1 = 0;
		size_t nPrevFirst = 0; // index into `out` of the previous band's first rect

		auto Flush = [&]( int ya, int yb, const std::vector<Span> &spans )
		{
			nPrevFirst = out.size();
			for ( const Span &s : spans )
				out.push_back( { s.a, ya, s.b, yb } );
			prevSpans = spans;
			nPrevY0 = ya;
			nPrevY1 = yb;
		};

		for ( size_t i = 0; i + 1 < ys.size(); i++ )
		{
			const int ya = ys[i], yb = ys[i + 1];
			std::vector<Span> A, B;
			for ( const IRect &r : add )
				if ( !r.Empty() && r.y0 <= ya && r.y1 >= yb )
					A.push_back( { r.x0, r.x1 } );
			if ( A.empty() )
				continue;
			detail::MergeSpans( A );
			for ( const IRect &r : sub )
				if ( !r.Empty() && r.y0 <= ya && r.y1 >= yb )
					B.push_back( { r.x0, r.x1 } );
			detail::MergeSpans( B );
			std::vector<Span> R = B.empty() ? A : detail::SubtractSpans( A, B );
			if ( R.empty() )
				continue;

			// Same spans as the band directly above -> extend those rects
			// instead of stacking a second row of them.
			bool bSame = !prevSpans.empty() && nPrevY1 == ya && R.size() == prevSpans.size();
			for ( size_t k = 0; bSame && k < R.size(); k++ )
				bSame = R[k].a == prevSpans[k].a && R[k].b == prevSpans[k].b;
			if ( bSame )
			{
				for ( size_t k = nPrevFirst; k < out.size(); k++ )
					out[k].y1 = yb;
				nPrevY1 = yb;
				(void)nPrevY0;
			}
			else
				Flush( ya, yb, R );
		}
		return out;
	}

	// Builds the whole crosshair for one frame. Every output rect is in
	// whole output pixels; the arms are symmetric about the snapped centre
	// (see detail::SnapCenter), so a 1px-wide arm is exactly one pixel wide
	// and a 1px gap is exactly one pixel on each side of the centre.
	inline Shape Build( const Style &st, const Frame &fr, const HideState &hs )
	{
		Shape shape;
		const float sx = std::max( fr.flScaleX, 1e-3f );
		const float sy = std::max( fr.flScaleY, 1e-3f );

		std::vector<IRect> arms, dot;

		if ( st.bLine )
		{
			// Thickness of the horizontal arms is measured along y, of the
			// vertical arms along x -- under Apply Scaling they differ.
			const int tH = std::max( 1, detail::SnapSize( st.flWidth * sy ) );
			const int tV = std::max( 1, detail::SnapSize( st.flWidth * sx ) );
			const int lenX = detail::SnapSize( st.flLength * hs.flLength * sx );
			const int lenY = detail::SnapSize( st.flLength * hs.flLength * sy );
			const int gapX = detail::SnapSize( st.flGap * hs.flGap * sx );
			const int gapY = detail::SnapSize( st.flGap * hs.flGap * sy );

			// The centre column is tV wide and the centre row tH tall: snap
			// the centre to their parity so both cross-axes are symmetric.
			const double cx = detail::SnapCenter( fr.flCenterX, tV );
			const double cy = detail::SnapCenter( fr.flCenterY, tH );

			// Horizontal arms: y extent is the centre row; x starts one
			// gap out from the centre column's own edge.
			const int rowY0 = (int)std::lround( cy - tH / 2.0 );
			const int colX0 = (int)std::lround( cx - tV / 2.0 );
			if ( lenX >= 1 )
			{
				// Right arm starts at the centre column's right edge + gap;
				// left arm ends at its left edge - gap. With an odd tV the
				// column is the single pixel at floor(cx); with an even one
				// the column's edge is cx itself -- either way the two arms
				// are mirror images about the snapped centre.
				const int rightX0 = (int)std::lround( cx + tV / 2.0 ) + gapX;
				const int leftX1 = colX0 - gapX;
				arms.push_back( { rightX0, rowY0, rightX0 + lenX, rowY0 + tH } );
				arms.push_back( { leftX1 - lenX, rowY0, leftX1, rowY0 + tH } );
			}
			if ( lenY >= 1 )
			{
				const int downY0 = (int)std::lround( cy + tH / 2.0 ) + gapY;
				const int upY1 = rowY0 - gapY;
				arms.push_back( { colX0, downY0, colX0 + tV, downY0 + lenY } );
				arms.push_back( { colX0, upY1 - lenY, colX0 + tV, upY1 } );
			}

			// The gap is measured from the centre column/row's own edge, so
			// at gap 0 the arms TOUCH the centre square (tV x tH) without
			// covering it. A bar that has closed its gap should read as one
			// continuous line, not two arms with a pixel missing between
			// them -- Focus and Shrink drive the gap to exactly 0 -- so the
			// centre square joins the arms as soon as either axis' gap hits
			// zero. Decompose() below absorbs the overlap.
			if ( ( lenX >= 1 && gapX == 0 ) || ( lenY >= 1 && gapY == 0 ) )
				arms.push_back( { colX0, rowY0, colX0 + tV, rowY0 + tH } );
		}

		if ( st.bDot )
		{
			const int tX = detail::SnapSize( st.flDotSize * hs.flLength * sx );
			const int tY = detail::SnapSize( st.flDotSize * hs.flLength * sy );
			if ( tX >= 1 && tY >= 1 )
			{
				const double cx = detail::SnapCenter( fr.flCenterX, tX );
				const double cy = detail::SnapCenter( fr.flCenterY, tY );
				const int x0 = (int)std::lround( cx - tX / 2.0 );
				const int y0 = (int)std::lround( cy - tY / 2.0 );
				dot.push_back( { x0, y0, x0 + tX, y0 + tY } );
			}
		}

		if ( st.bOutline && ( !arms.empty() || !dot.empty() ) )
		{
			const int ox = std::max( 1, detail::SnapSize( st.flOutlineWidth * sx ) );
			const int oy = std::max( 1, detail::SnapSize( st.flOutlineWidth * sy ) );
			std::vector<IRect> expanded, fill;
			for ( const IRect &r : arms )
			{
				expanded.push_back( { r.x0 - ox, r.y0 - oy, r.x1 + ox, r.y1 + oy } );
				fill.push_back( r );
			}
			for ( const IRect &r : dot )
			{
				expanded.push_back( { r.x0 - ox, r.y0 - oy, r.x1 + ox, r.y1 + oy } );
				fill.push_back( r );
			}
			shape.outline = Decompose( expanded, fill );
		}

		shape.lines = Decompose( arms, {} );
		shape.dot = dot;
		return shape;
	}

	// ------------------------------------------------------------------
	// Apply Scaling's raster path (superdoc/features/crosshair.md, "Two
	// rendering paths"). With Apply Scaling ON the crosshair is not
	// re-drawn as a vector at output resolution; it is Build() at the
	// GAME's resolution (GameFrame(), scale 1, centre = the game's own
	// centre), rasterised on the CPU into a small texture that covers just
	// its bounding box plus a one-texel transparent margin (RasterRect()),
	// and stretched onto the output as ONE linearly-sampled quad
	// (ScaledQuad()) -- the way a stretched in-game raster crosshair looks:
	// a 1 px game line spanning 1.5 output px softly, the outline's colour
	// mixing into the fill's at the edge.
	// ------------------------------------------------------------------

	// One transparent texel around the drawing, so the bilinear filter has
	// something to blend the outermost pixels against instead of clamping.
	constexpr int kRasterMargin = 1;

	// The frame Build() takes to draw in game pixels: centre = the game's
	// own centre, so parity snapping (SnapCenter) behaves exactly as an
	// in-game crosshair at "screen centre" of a buffer that size would.
	inline Frame GameFrame( uint32_t uGameW, uint32_t uGameH )
	{
		Frame fr;
		fr.flCenterX = (float)uGameW * 0.5f;
		fr.flCenterY = (float)uGameH * 0.5f;
		fr.flScaleX = 1.0f;
		fr.flScaleY = 1.0f;
		return fr;
	}

	// Bounding box of every rect in the shape; empty for an empty shape.
	inline IRect BoundingBox( const Shape &shape )
	{
		IRect bb;
		bool bAny = false;
		auto Grow = [&]( const std::vector<IRect> &rects )
		{
			for ( const IRect &r : rects )
			{
				if ( r.Empty() ) continue;
				if ( !bAny ) { bb = r; bAny = true; continue; }
				bb.x0 = std::min( bb.x0, r.x0 ); bb.y0 = std::min( bb.y0, r.y0 );
				bb.x1 = std::max( bb.x1, r.x1 ); bb.y1 = std::max( bb.y1, r.y1 );
			}
		};
		Grow( shape.outline ); Grow( shape.lines ); Grow( shape.dot );
		return bAny ? bb : IRect{};
	}

	// The texture's footprint in game pixels: the bounding box grown by
	// kRasterMargin on every side. Texel (0,0) is game pixel (x0, y0).
	inline IRect RasterRect( const Shape &shape )
	{
		IRect bb = BoundingBox( shape );
		if ( bb.Empty() )
			return IRect{};
		return { bb.x0 - kRasterMargin, bb.y0 - kRasterMargin, bb.x1 + kRasterMargin, bb.y1 + kRasterMargin };
	}

	struct FRect
	{
		float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	};

	// Where the raster lands on the output, in output pixels (ImGui vertex
	// coordinates: integer = pixel edge, pixel i covers [i, i+1)).
	//
	// Derivation. composite.h's sampleLayerEx samples layer 0 at texel
	// t = (o + offset) * scale for output position o, so game pixel g sits
	// at o = g * s + origin, with s = "output px per game px" (fr.flScale)
	// and origin = -offset = the game rect's top-left. The frame carries
	// the game rect's CENTRE rather than its origin, and the centre is
	// gameW/2 game pixels in: origin = centre - (gameW/2) * s. A texel k of
	// the raster is game pixel texRect.x0 + k, so the quad's left edge is
	// origin + texRect.x0 * s and its width is texRect width * s -- then
	// texel centres (k + 0.5) land exactly on game pixel centres
	// (texRect.x0 + k + 0.5) * s + origin, the same spot the composite
	// puts the game's own pixel under it. No half-pixel offset anywhere:
	// both ImGui and the composite treat integer coordinates as pixel
	// edges, so the identity is exact.
	inline FRect ScaledQuad( const IRect &texRect, uint32_t uGameW, uint32_t uGameH, const Frame &fr )
	{
		const float sx = fr.flScaleX, sy = fr.flScaleY;
		const float flOriginX = fr.flCenterX - (float)uGameW * 0.5f * sx;
		const float flOriginY = fr.flCenterY - (float)uGameH * 0.5f * sy;
		FRect q;
		q.x0 = flOriginX + (float)texRect.x0 * sx;
		q.y0 = flOriginY + (float)texRect.y0 * sy;
		q.x1 = flOriginX + (float)texRect.x1 * sx;
		q.y1 = flOriginY + (float)texRect.y1 * sy;
		return q;
	}

	// Straight-alpha 0xAARRGGBB, which is exactly the little-endian memory
	// order of a B8G8R8A8 texel (bytes B, G, R, A).
	using Argb = uint32_t;

	inline Argb PackArgb( int nRgb, float flAlpha )
	{
		const int a = (int)std::lround( std::clamp( flAlpha, 0.0f, 1.0f ) * 255.0f );
		return ( (Argb)a << 24 ) | ( (Argb)nRgb & 0xFFFFFF );
	}

	namespace detail
	{
		// `src` OVER `dst`, both straight alpha -- what ImGui's
		// SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend does when the dot is
		// drawn over an arm in the vector path, so the two paths agree.
		inline Argb Over( Argb src, Argb dst )
		{
			const float sa = ( ( src >> 24 ) & 0xFF ) / 255.0f;
			const float da = ( ( dst >> 24 ) & 0xFF ) / 255.0f;
			if ( sa <= 0.0f ) return dst;
			if ( da <= 0.0f || sa >= 1.0f ) return src;
			const float oa = sa + da * ( 1.0f - sa );
			Argb out = (Argb)std::lround( oa * 255.0f ) << 24;
			for ( int shift : { 16, 8, 0 } )
			{
				const float sc = ( ( src >> shift ) & 0xFF ) / 255.0f;
				const float dc = ( ( dst >> shift ) & 0xFF ) / 255.0f;
				const float oc = ( sc * sa + dc * da * ( 1.0f - sa ) ) / oa;
				out |= (Argb)std::clamp( (long)std::lround( oc * 255.0f ), 0l, 255l ) << shift;
			}
			return out;
		}
	}

	// Paints the shape into a width x height buffer whose texel (0,0) is
	// game pixel (texRect.x0, texRect.y0): outline, then arms, then the dot
	// composited over (the vector path's order). Then every fully
	// transparent texel that touches a painted one takes that neighbour's
	// RGB (alpha stays 0): a linear filter interpolates colour and alpha
	// independently, so a transparent texel left at RGB 0 would drag the
	// edge of a green line towards black -- a dark fringe an in-game
	// raster does not have. With the bleed, the edge is the line's own
	// colour at half alpha, which is the "soft" edge the user asked for.
	inline std::vector<Argb> Rasterize( const Shape &shape, const IRect &texRect,
	                                    Argb outline, Argb lines, Argb dot )
	{
		const int w = texRect.x1 - texRect.x0, h = texRect.y1 - texRect.y0;
		std::vector<Argb> px;
		if ( w <= 0 || h <= 0 )
			return px;
		px.assign( (size_t)w * (size_t)h, 0u );

		auto Paint = [&]( const std::vector<IRect> &rects, Argb col )
		{
			if ( ( col >> 24 ) == 0 )
				return;
			for ( const IRect &r : rects )
				for ( int y = std::max( r.y0, texRect.y0 ); y < std::min( r.y1, texRect.y1 ); y++ )
					for ( int x = std::max( r.x0, texRect.x0 ); x < std::min( r.x1, texRect.x1 ); x++ )
					{
						Argb &d = px[(size_t)( y - texRect.y0 ) * w + ( x - texRect.x0 )];
						d = detail::Over( col, d );
					}
		};
		Paint( shape.outline, outline );
		Paint( shape.lines, lines );
		Paint( shape.dot, dot );

		// Colour bleed into the transparent neighbours (edge-adjacent
		// first, then diagonal), from a snapshot so the bleed itself
		// never propagates further than one texel.
		const std::vector<Argb> snap = px;
		constexpr int kN[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
		for ( int y = 0; y < h; y++ )
			for ( int x = 0; x < w; x++ )
			{
				if ( ( snap[(size_t)y * w + x] >> 24 ) != 0 )
					continue;
				for ( const auto &n : kN )
				{
					const int nx = x + n[0], ny = y + n[1];
					if ( nx < 0 || ny < 0 || nx >= w || ny >= h )
						continue;
					const Argb v = snap[(size_t)ny * w + nx];
					if ( ( v >> 24 ) != 0 )
					{
						px[(size_t)y * w + x] = v & 0x00FFFFFF;
						break;
					}
				}
			}
		return px;
	}
}
