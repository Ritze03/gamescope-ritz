// Unit tests for Overlay/CrosshairMath.h -- the crosshair's pure half: the
// right-click hide animation's arithmetic and the integer-snapped
// rectangle geometry (superdoc/features/crosshair.md). Nothing here needs
// ImGui, Vulkan or the config system, which is why the maths was split out
// into a header in the first place.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Overlay/CrosshairMath.h"

#include <set>

using namespace gamescope::crosshair;
using Catch::Matchers::WithinAbs;

namespace
{
	// Every pixel covered by a rect set, for exact coverage assertions.
	std::set<std::pair<int, int>> Pixels( const std::vector<IRect> &rects )
	{
		std::set<std::pair<int, int>> px;
		for ( const IRect &r : rects )
			for ( int y = r.y0; y < r.y1; y++ )
				for ( int x = r.x0; x < r.x1; x++ )
					px.insert( { x, y } );
		return px;
	}

	// A rect set is non-overlapping iff its pixel count equals the sum of
	// its rect areas.
	bool NonOverlapping( const std::vector<IRect> &rects )
	{
		size_t nArea = 0;
		for ( const IRect &r : rects )
			nArea += size_t( r.x1 - r.x0 ) * size_t( r.y1 - r.y0 );
		return Pixels( rects ).size() == nArea;
	}

	bool Disjoint( const std::vector<IRect> &a, const std::vector<IRect> &b )
	{
		const auto pa = Pixels( a );
		for ( const auto &p : Pixels( b ) )
			if ( pa.count( p ) )
				return false;
		return true;
	}

	// The bidirectional run of NOT-covered points through (cx, cy) along
	// (dx, dy) -- 0 if (cx, cy) itself is covered. This is the crosshair
	// gap invariant (2026-09-08, crosshair.md's "Gap"): a gap of N means
	// exactly N missing pixels across the centre, counting the centre
	// pixel once, not N per side. Mirrors
	// scripts/pixel_regression_sample.py's hole_run(), which pins the same
	// invariant against a real screen capture.
	int HoleRun( const std::set<std::pair<int, int>> &px, int cx, int cy, int dx, int dy )
	{
		if ( px.count( { cx, cy } ) )
			return 0;
		int nLow = 0, nHigh = 0;
		for ( int off = -1; ; off-- )
			if ( px.count( { cx + dx * off, cy + dy * off } ) ) { nLow = off + 1; break; }
		for ( int off = 1; ; off++ )
			if ( px.count( { cx + dx * off, cy + dy * off } ) ) { nHigh = off - 1; break; }
		return nHigh - nLow + 1;
	}
}

// ---------------------------------------------------------------------
// Hide animation
// ---------------------------------------------------------------------

TEST_CASE( "HideProgress is 0 while not held, clamps to 1, and 0 ms hides at once", "[crosshair]" )
{
	REQUIRE( HideProgress( 0, 5'000'000'000ull, 200 ) == 0.0f );
	REQUIRE_THAT( HideProgress( 1'000'000'000ull, 1'100'000'000ull, 200 ), WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE( HideProgress( 1'000'000'000ull, 9'000'000'000ull, 200 ) == 1.0f );
	REQUIRE( HideProgress( 1'000'000'000ull, 1'000'000'000ull, 200 ) == 0.0f );
	REQUIRE( HideProgress( 1'000'000'000ull, 1'000'000'001ull, 0 ) == 1.0f );
}

TEST_CASE( "Fade only touches alpha", "[crosshair]" )
{
	for ( float f : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f } )
	{
		const HideState s = EvaluateHide( HideMode::Fade, f );
		REQUIRE_THAT( s.flAlpha, WithinAbs( 1.0f - f, 1e-6f ) );
		REQUIRE( s.flGap == 1.0f );
		REQUIRE( s.flLength == 1.0f );
	}
}

TEST_CASE( "Focus closes the gap over the first half, then fades", "[crosshair]" )
{
	HideState s = EvaluateHide( HideMode::Focus, 0.25f );
	REQUIRE_THAT( s.flGap, WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE( s.flAlpha == 1.0f );
	REQUIRE( s.flLength == 1.0f );

	s = EvaluateHide( HideMode::Focus, 0.5f );
	REQUIRE( s.flGap == 0.0f );
	REQUIRE_THAT( s.flAlpha, WithinAbs( 1.0f, 1e-6f ) );

	s = EvaluateHide( HideMode::Focus, 0.75f );
	REQUIRE( s.flGap == 0.0f );
	REQUIRE_THAT( s.flAlpha, WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE( s.flLength == 1.0f );

	s = EvaluateHide( HideMode::Focus, 1.0f );
	REQUIRE_THAT( s.flAlpha, WithinAbs( 0.0f, 1e-6f ) );
}

TEST_CASE( "Shrink closes the gap over the first half, then shrinks the length; alpha untouched", "[crosshair]" )
{
	HideState s = EvaluateHide( HideMode::Shrink, 0.25f );
	REQUIRE_THAT( s.flGap, WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE( s.flLength == 1.0f );
	REQUIRE( s.flAlpha == 1.0f );

	s = EvaluateHide( HideMode::Shrink, 0.75f );
	REQUIRE( s.flGap == 0.0f );
	REQUIRE_THAT( s.flLength, WithinAbs( 0.5f, 1e-6f ) );
	REQUIRE( s.flAlpha == 1.0f );

	s = EvaluateHide( HideMode::Shrink, 1.0f );
	REQUIRE_THAT( s.flLength, WithinAbs( 0.0f, 1e-6f ) );
}

TEST_CASE( "hide mode keys round-trip and unknown reads as fade", "[crosshair]" )
{
	for ( HideMode m : { HideMode::Fade, HideMode::Focus, HideMode::Shrink } )
		REQUIRE( ParseHideMode( HideModeKey( m ) ) == m );
	REQUIRE( ParseHideMode( "nonsense" ) == HideMode::Fade );
	REQUIRE( ParseHideMode( "" ) == HideMode::Fade );
}

// ---------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------

TEST_CASE( "a 1px line is exactly one pixel wide, symmetric about an even-size output's centre", "[crosshair]" )
{
	Style st;
	st.bLine = true; st.flWidth = 1.0f; st.flLength = 4.0f; st.flGap = 2.0f;
	st.bDot = false; st.bOutline = false;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f; // 1920x1080

	const Shape s = Build( st, fr, {} );
	REQUIRE( s.outline.empty() );
	REQUIRE( s.dot.empty() );
	REQUIRE( NonOverlapping( s.lines ) );

	// Odd thickness centres on pixel column 960 / row 540 (floor of the
	// centre). Horizontal arms occupy row 540 only; vertical arms column
	// 960 only. Gap 2 -> hole = 2*(2-1)+1 = 3 pixels (2026-09-08, revised
	// same day), SYMMETRIC: one pixel on each side of the centre pixel,
	// plus the centre pixel itself -- no stagger between the two arms of
	// an axis, at any gap value.
	const auto px = Pixels( s.lines );
	REQUIRE( px.size() == 4 * 4 );
	for ( int x = 962; x < 966; x++ ) REQUIRE( px.count( { x, 540 } ) );       // right arm starts at 962
	for ( int x = 955; x < 959; x++ ) REQUIRE( px.count( { x, 540 } ) );       // left arm ends at 958 (exclusive of 959)
	for ( int y = 542; y < 546; y++ ) REQUIRE( px.count( { 960, y } ) );       // down arm
	for ( int y = 535; y < 539; y++ ) REQUIRE( px.count( { 960, y } ) );       // up arm
	// The hole is exactly columns/rows 959/960/961 -- equidistant on both
	// sides of the centre pixel, nothing else nearby is touched.
	for ( int x = 950; x < 970; x++ )
	{
		if ( x >= 959 && x <= 961 ) continue;
		REQUIRE_FALSE( px.count( { x, 539 } ) );
		REQUIRE_FALSE( px.count( { x, 541 } ) );
	}
	for ( int y = 530; y < 550; y++ )
	{
		if ( y >= 539 && y <= 541 ) continue;
		REQUIRE_FALSE( px.count( { 959, y } ) );
		REQUIRE_FALSE( px.count( { 961, y } ) );
	}
	// The symmetry itself: nothing at all inside the 3-pixel hole.
	for ( int x = 959; x <= 961; x++ ) REQUIRE_FALSE( px.count( { x, 540 } ) );
	for ( int y = 539; y <= 541; y++ ) REQUIRE_FALSE( px.count( { 960, y } ) );
}

TEST_CASE( "an even-width line straddles the centre edge symmetrically", "[crosshair]" )
{
	Style st;
	st.flWidth = 2.0f; st.flLength = 3.0f; st.flGap = 3.0f; st.bDot = false; st.bOutline = false;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f;

	const Shape s = Build( st, fr, {} );
	const auto px = Pixels( s.lines );
	// Even thickness centres on the pixel EDGE at 960/540: the centre
	// column is 959..960, the centre row 539..540. Gap 3 -> hole =
	// 2*(3-1)+2 = 6 pixels (2026-09-08, revised same day), symmetric --
	// 957..962 horizontally, 537..542 vertically -- so the right arm
	// starts at 963 and the left arm ends at 957 (exclusive).
	for ( int y : { 539, 540 } )
	{
		for ( int x = 963; x < 966; x++ ) REQUIRE( px.count( { x, y } ) );
		for ( int x = 954; x < 957; x++ ) REQUIRE( px.count( { x, y } ) );
	}
	// Vertical arms: columns 959 and 960. Down 543..545, up 534..536.
	for ( int x : { 959, 960 } )
	{
		for ( int y = 543; y < 546; y++ ) REQUIRE( px.count( { x, y } ) );
		for ( int y = 534; y < 537; y++ ) REQUIRE( px.count( { x, y } ) );
	}
	REQUIRE( px.size() == 4 * 3 * 2 );
	// The 6-wide/6-tall hole around the centre stays empty, symmetrically.
	REQUIRE_FALSE( px.count( { 959, 539 } ) );
	REQUIRE_FALSE( px.count( { 960, 540 } ) );
	REQUIRE_FALSE( px.count( { 957, 539 } ) );
	REQUIRE_FALSE( px.count( { 962, 540 } ) );
}

TEST_CASE( "Gap N produces a hole of 2*(N-1)+width, exactly, at width 1 and width 2", "[crosshair]" )
{
	// 2026-09-08, revised same day ("basically like the old formula, just
	// with the gap with 1 deducted" -- the user's own words, after the
	// first same-day attempt gave a visibly lopsided crosshair at even
	// gaps): each arm's own inset from the crossing is (gap - 1), so the
	// hole is 2*(gap-1) + the line's own width. Gap 0 is no hole at all
	// (the crossing square joins the arms into a solid plus -- see the
	// Focus/Shrink gap-0 case below); gap 1 puts the arms right at the
	// crossing's own edges (hole == width).
	for ( int nWidth : { 1, 2 } )
	{
		for ( int nGap = 0; nGap <= 4; nGap++ )
		{
			Style st;
			st.flWidth = (float)nWidth; st.flLength = 6.0f; st.flGap = (float)nGap;
			st.bDot = false; st.bOutline = false;
			Frame fr; fr.flCenterX = 100.0f; fr.flCenterY = 100.0f;

			const int nExpectedHole = nGap <= 0 ? 0 : 2 * ( nGap - 1 ) + nWidth;
			const auto px = Pixels( Build( st, fr, {} ).lines );
			REQUIRE( HoleRun( px, 100, 100, 1, 0 ) == nExpectedHole );
			REQUIRE( HoleRun( px, 100, 100, 0, 1 ) == nExpectedHole );
		}
	}
}

TEST_CASE( "an outline redraws over part of the hole but the true gap invariant still holds", "[crosshair]" )
{
	// The outline sits INSIDE the geometric hole (crosshair.md: it expands
	// outward from the fill, eating into what would otherwise be
	// background), so with the outline on, "missing" means "not the
	// line's own fill colour" -- background OR outline -- while the count
	// of such pixels between the two arms is still exactly the hole
	// (2*(gap-1)+width).
	for ( int nWidth : { 1, 2 } )
	{
		for ( int nGap = 0; nGap <= 4; nGap++ )
		{
			Style st;
			st.flWidth = (float)nWidth; st.flLength = 6.0f; st.flGap = (float)nGap;
			st.bDot = false; st.bOutline = true; st.flOutlineWidth = 1.0f;
			Frame fr; fr.flCenterX = 100.0f; fr.flCenterY = 100.0f;

			// HoleRun against `lines` alone: the outline paints part of
			// this same run, but never the arm's own fill colour, so the
			// run's ENDPOINTS (where the fill starts) do not move.
			const int nExpectedHole = nGap <= 0 ? 0 : 2 * ( nGap - 1 ) + nWidth;
			const auto px = Pixels( Build( st, fr, {} ).lines );
			REQUIRE( HoleRun( px, 100, 100, 1, 0 ) == nExpectedHole );
			REQUIRE( HoleRun( px, 100, 100, 0, 1 ) == nExpectedHole );
		}
	}
}

TEST_CASE( "the hole is always symmetric: both arms of an axis sit equidistant from the centre", "[crosshair]" )
{
	// The bias rejected the same day it shipped (crosshair.md's "Gap": the
	// user's reference picture showed a staggered gap 2 next to a
	// symmetric one and called the staggered one wrong) is gone --
	// detail::HoleSplit's nLow and nHigh are literally the same
	// expression now, so there is no gap value, odd or even, and no
	// width, at which the two arms of an axis differ in their distance
	// from the centre.
	//
	// Measured via the whole shape's bounding box rather than a raw pixel
	// scan from a fixed integer anchor: an ODD line width centres on a
	// PIXEL (detail::SnapCenter) but an EVEN one centres on a pixel EDGE,
	// so the reference point to measure symmetry against is the SNAPPED
	// centre for that thickness, not the raw requested Frame centre (an
	// odd thickness is legitimately, and unrelatedly, offset by up to 0.5
	// px from the requested centre -- that is SnapCenter's own pre-existing
	// contract, not part of this fix). Since both arms of an axis share
	// the same configured length, a symmetric far edge (this bounding box,
	// about the snapped centre) is exactly equivalent to a symmetric near
	// edge (the hole itself) -- HoleSplit's nLow == nHigh iff the box is
	// centred on it.
	for ( int nWidth = 1; nWidth <= 3; nWidth++ )
	{
		for ( int nGap = 0; nGap <= 8; nGap++ )
		{
			Style st;
			st.flWidth = (float)nWidth; st.flLength = 6.0f; st.flGap = (float)nGap;
			st.bDot = false; st.bOutline = false;
			Frame fr; fr.flCenterX = 100.0f; fr.flCenterY = 100.0f;

			const Shape s = Build( st, fr, {} );
			const IRect bb = BoundingBox( s );
			REQUIRE_FALSE( bb.Empty() );
			const double cxs = detail::SnapCenter( fr.flCenterX, nWidth );
			const double cys = detail::SnapCenter( fr.flCenterY, nWidth );
			REQUIRE_THAT( cxs - (double)bb.x0, WithinAbs( (double)bb.x1 - cxs, 1e-6 ) );
			REQUIRE_THAT( cys - (double)bb.y0, WithinAbs( (double)bb.y1 - cys, 1e-6 ) );
		}
	}
}

TEST_CASE( "Apply Scaling's raster path scales the hole by the same factor as everything else", "[crosshair]" )
{
	// The gap invariant is measured in GAME pixels inside Build() (the
	// raster path draws at the game's own resolution, unscaled -- see
	// crosshair.md's "Two rendering paths"), so it is unaffected by this
	// change on its own; what this test pins is that the coverage-based
	// hole -- 2*(gap-1)+width in game pixels (2026-09-08, revised same
	// day) -- survives the CPU resample at the same scale as the arms
	// themselves.
	Style st; st.flWidth = 1.0f; st.flLength = 6.0f; st.bDot = false; st.bOutline = false;
	const Frame gf = GameFrame( 200, 200 );
	Frame outFr; outFr.flCenterX = 100.0f; outFr.flCenterY = 100.0f; outFr.flScaleX = 2.0f; outFr.flScaleY = 2.0f;

	for ( int nGap : { 1, 2, 3 } )
	{
		st.flGap = (float)nGap;
		const Shape s = Build( st, gf, {} );
		const IRect tr = RasterRect( s );
		const Argb green = PackArgb( 0x00FF00, 1.0f );
		const std::vector<Argb> gamePx = Rasterize( s, tr, 0u, green, 0u );
		const OutputRaster out = ResampleToOutput( gamePx, tr, 200, 200, outFr );
		const int ow = out.rect.x1 - out.rect.x0;
		auto Cov = [&]( int ox, int oy ) -> int
		{
			if ( ox < out.rect.x0 || oy < out.rect.y0 || ox >= out.rect.x1 || oy >= out.rect.y1 )
				return 0;
			return (int)( out.px[(size_t)( oy - out.rect.y0 ) * ow + ( ox - out.rect.x0 )] >> 24 );
		};
		// The run of >= 50 % coverage on each side of the centre row, and
		// the gap between those two runs -- exactly the game-pixel hole
		// (2*(gap-1)+width) times scale.
		int nRightStart = -1;
		for ( int ox = 100; ox < 200; ox++ )
			if ( Cov( ox, 100 ) >= 128 ) { nRightStart = ox; break; }
		int nLeftEnd = -1;
		for ( int ox = 100; ox > 0; ox-- )
			if ( Cov( ox, 100 ) >= 128 ) { nLeftEnd = ox; break; }
		REQUIRE( nRightStart > 0 );
		REQUIRE( nLeftEnd > 0 );
		const int nExpectedHole = 2 * ( nGap - 1 ) + 1; // width 1
		REQUIRE( nRightStart - nLeftEnd - 1 == nExpectedHole * 2 );
	}
}

TEST_CASE( "the dot is a centred square and a 1px dot is one pixel", "[crosshair]" )
{
	Style st; st.bLine = false; st.bOutline = false; st.bDot = true;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f;

	st.flDotSize = 1.0f;
	Shape s = Build( st, fr, {} );
	REQUIRE( s.dot.size() == 1 );
	REQUIRE( s.dot[0] == IRect{ 960, 540, 961, 541 } );

	st.flDotSize = 2.0f;
	s = Build( st, fr, {} );
	REQUIRE( s.dot[0] == IRect{ 959, 539, 961, 541 } );

	st.flDotSize = 3.0f;
	s = Build( st, fr, {} );
	REQUIRE( s.dot[0] == IRect{ 959, 539, 962, 542 } );

	st.flDotSize = 9.0f; // still a square, never a circle
	s = Build( st, fr, {} );
	REQUIRE( s.dot[0] == IRect{ 956, 536, 965, 545 } );
}

TEST_CASE( "the outline sits strictly outside every fill and is exactly one ring wide", "[crosshair]" )
{
	Style st;
	st.flWidth = 2.0f; st.flLength = 4.0f; st.flGap = 3.0f;
	st.bDot = true; st.flDotSize = 2.0f;
	st.bOutline = true; st.flOutlineWidth = 1.0f;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f;

	const Shape s = Build( st, fr, {} );
	REQUIRE_FALSE( s.outline.empty() );
	REQUIRE( NonOverlapping( s.outline ) );
	REQUIRE( NonOverlapping( s.lines ) );
	REQUIRE( Disjoint( s.outline, s.lines ) );
	REQUIRE( Disjoint( s.outline, s.dot ) );

	// Every outline pixel is within 1px (Chebyshev) of a fill pixel, and
	// every fill pixel's 1px neighbourhood is either fill or outline.
	std::vector<IRect> fills = s.lines;
	fills.insert( fills.end(), s.dot.begin(), s.dot.end() );
	const auto fill = Pixels( fills );
	const auto ol = Pixels( s.outline );
	for ( const auto &p : ol )
	{
		bool bNear = false;
		for ( int dy = -1; dy <= 1 && !bNear; dy++ )
			for ( int dx = -1; dx <= 1 && !bNear; dx++ )
				bNear = fill.count( { p.first + dx, p.second + dy } ) > 0;
		REQUIRE( bNear );
	}
	for ( const auto &p : fill )
		for ( int dy = -1; dy <= 1; dy++ )
			for ( int dx = -1; dx <= 1; dx++ )
			{
				const std::pair<int, int> q{ p.first + dx, p.second + dy };
				REQUIRE( ( fill.count( q ) || ol.count( q ) ) );
			}
}

TEST_CASE( "arms driven to gap 0 by Focus/Shrink are a non-overlapping plus, painted once", "[crosshair]" )
{
	Style st;
	st.flWidth = 2.0f; st.flLength = 5.0f; st.flGap = 4.0f; st.bDot = false; st.bOutline = true; st.flOutlineWidth = 1.0f;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f;

	const Shape s = Build( st, fr, EvaluateHide( HideMode::Focus, 0.5f ) );
	REQUIRE( NonOverlapping( s.lines ) );
	REQUIRE( NonOverlapping( s.outline ) );
	REQUIRE( Disjoint( s.outline, s.lines ) );
	// A 2px plus with 5px arms: 4 arms * 5 * 2 = 40 pixels plus the 2x2
	// centre square that joins them once the gap is 0 -- one continuous
	// plus, no hole, no double coverage.
	REQUIRE( Pixels( s.lines ).size() == 44 );
	REQUIRE( Pixels( s.lines ).count( { 960, 540 } ) );
	REQUIRE( Pixels( s.lines ).count( { 959, 539 } ) );
}

TEST_CASE( "Shrink at 100% draws nothing; Shrink at 75% halves the arms and the dot", "[crosshair]" )
{
	Style st;
	st.flWidth = 1.0f; st.flLength = 8.0f; st.flGap = 2.0f; st.bDot = true; st.flDotSize = 4.0f; st.bOutline = true;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f;

	REQUIRE( Build( st, fr, EvaluateHide( HideMode::Shrink, 1.0f ) ).Empty() );

	const Shape s = Build( st, fr, EvaluateHide( HideMode::Shrink, 0.75f ) );
	REQUIRE( Pixels( s.lines ).size() == 4 * 4 + 1 ); // length 8 -> 4, plus the centre pixel (gap is 0 by now)
	REQUIRE( s.dot.size() == 1 );
	REQUIRE( s.dot[0].x1 - s.dot[0].x0 == 2 );      // dot 4 -> 2
}

TEST_CASE( "Apply Scaling stretches the crosshair per axis and keeps whole-pixel snapping", "[crosshair]" )
{
	// A 1280x960 (4:3) game stretched onto 1920x1080: 1.5x across, 1.125x down.
	Style st;
	st.flWidth = 1.0f; st.flLength = 4.0f; st.flGap = 2.0f; st.bDot = true; st.flDotSize = 2.0f; st.bOutline = false;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f; fr.flScaleX = 1.5f; fr.flScaleY = 1.125f;

	const Shape s = Build( st, fr, {} );
	REQUIRE( NonOverlapping( s.lines ) );
	// Vertical arms are round(1 * 1.5) = 2 wide; horizontal arms round(1.125) = 1 tall.
	// Horizontal arm length round(4 * 1.5) = 6; vertical arm length round(4 * 1.125) = 5 (4.5 rounds away from zero).
	int nHorizontalArmPixels = 0, nVerticalArmPixels = 0;
	for ( const IRect &r : s.lines )
	{
		const int w = r.x1 - r.x0, h = r.y1 - r.y0;
		if ( w > h ) { REQUIRE( h == 1 ); REQUIRE( w == 6 ); nHorizontalArmPixels += w * h; }
		else         { REQUIRE( w == 2 ); REQUIRE( h == 5 ); nVerticalArmPixels += w * h; }
	}
	REQUIRE( nHorizontalArmPixels == 12 );
	REQUIRE( nVerticalArmPixels == 20 );
	// Dot: round(2*1.5)=3 wide, round(2*1.125)=2 tall.
	REQUIRE( s.dot[0].x1 - s.dot[0].x0 == 3 );
	REQUIRE( s.dot[0].y1 - s.dot[0].y0 == 2 );
}

TEST_CASE( "an off-centre, letterboxed game centre is honoured exactly", "[crosshair]" )
{
	// A 1280x720 client letterboxed inside 1920x1080 at integer scale: on
	// screen it is 1280x720 at (320, 180); centre (960, 540). A different,
	// off-centre case: game rect at (100, 50), 800x600 -> centre (500, 350).
	Style st; st.flWidth = 1.0f; st.flLength = 2.0f; st.flGap = 1.0f; st.bDot = true; st.flDotSize = 1.0f; st.bOutline = false;
	Frame fr; fr.flCenterX = 500.0f; fr.flCenterY = 350.0f;
	const Shape s = Build( st, fr, {} );
	REQUIRE( s.dot[0] == IRect{ 500, 350, 501, 351 } );
	// Gap 1 is the TOTAL hole (2026-09-08): exactly the centre pixel
	// (500, 350) itself is missing, so the arms touch its edges directly.
	const auto px = Pixels( s.lines );
	REQUIRE( px.count( { 501, 350 } ) ); REQUIRE( px.count( { 502, 350 } ) );
	REQUIRE( px.count( { 498, 350 } ) ); REQUIRE( px.count( { 499, 350 } ) );
	REQUIRE( px.count( { 500, 351 } ) ); REQUIRE( px.count( { 500, 352 } ) );
	REQUIRE( px.count( { 500, 348 } ) ); REQUIRE( px.count( { 500, 349 } ) );
	REQUIRE( px.size() == 8 );
}

TEST_CASE( "Decompose merges overlapping rects and subtracts exactly", "[crosshair]" )
{
	const std::vector<IRect> add = { { 0, 0, 4, 4 }, { 2, 2, 6, 6 } };
	const std::vector<IRect> sub = { { 1, 1, 3, 3 } };
	const auto out = Decompose( add, sub );
	REQUIRE( NonOverlapping( out ) );
	const auto px = Pixels( out );
	REQUIRE( px.size() == 16 + 16 - 4 - 4 ); // union 28, minus the 2x2 hole
	REQUIRE_FALSE( px.count( { 1, 1 } ) );
	REQUIRE_FALSE( px.count( { 2, 2 } ) );
	REQUIRE( px.count( { 0, 0 } ) );
	REQUIRE( px.count( { 5, 5 } ) );
	REQUIRE( px.count( { 3, 3 } ) );
	REQUIRE_FALSE( px.count( { 4, 0 } ) );
}

TEST_CASE( "line and dot switched off yield an empty shape (nothing to draw)", "[crosshair]" )
{
	Style st; st.bLine = false; st.bDot = false; st.bOutline = true;
	REQUIRE( Build( st, {}, {} ).Empty() );
}

// ---------------------------------------------------------------------
// Apply Scaling's raster path (crosshair.md, "Two rendering paths")
// ---------------------------------------------------------------------

TEST_CASE( "RasterRect is the game-pixel bounding box plus a one-texel margin", "[crosshair]" )
{
	// A 1280x960 game: centre (640, 480). 1px line, length 4, gap 2 -> hole
	// = 2*(2-1)+1 = 3 pixels (639/640/641), no dot, no outline -> right arm
	// 642..645, left arm 635..638, down arm 482..485, up arm 475..478.
	Style st; st.flWidth = 1.0f; st.flLength = 4.0f; st.flGap = 2.0f; st.bDot = false; st.bOutline = false;
	const Frame gf = GameFrame( 1280, 960 );
	REQUIRE( gf.flScaleX == 1.0f );
	REQUIRE( gf.flScaleY == 1.0f );
	const Shape s = Build( st, gf, {} );

	REQUIRE( BoundingBox( s ) == IRect{ 635, 475, 646, 486 } );
	REQUIRE( RasterRect( s ) == IRect{ 634, 474, 647, 487 } ); // 13 x 13 texels
	REQUIRE( kRasterMargin == 1 );

	// Empty shape -> empty footprint (nothing to upload or draw).
	Style off; off.bLine = false; off.bDot = false;
	REQUIRE( RasterRect( Build( off, gf, {} ) ).Empty() );
	REQUIRE( BoundingBox( Shape{} ).Empty() );
}

TEST_CASE( "ScaledQuad puts every raster texel centre on the game pixel centre the composite samples", "[crosshair]" )
{
	// 1280x960 (4:3) stretched over 1920x1080: layer 0 has offset (0,0) and
	// scale = tex/out = (2/3, 8/9); "output px per game px" = (1.5, 1.125).
	// composite.h: texel t = (o + offset) * scale  =>  o = t / scale - offset.
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f; fr.flScaleX = 1.5f; fr.flScaleY = 1.125f;
	const IRect texRect{ 633, 473, 648, 488 };
	const FRect q = ScaledQuad( texRect, 1280, 960, fr );
	REQUIRE_THAT( q.x0, WithinAbs( 949.5f, 1e-3f ) );
	REQUIRE_THAT( q.x1, WithinAbs( 972.0f, 1e-3f ) );
	REQUIRE_THAT( q.y0, WithinAbs( 532.125f, 1e-3f ) );
	REQUIRE_THAT( q.y1, WithinAbs( 549.0f, 1e-3f ) );

	const float offX = 0.0f, offY = 0.0f, layerScaleX = 1280.0f / 1920.0f, layerScaleY = 960.0f / 1080.0f;
	const int w = texRect.x1 - texRect.x0, h = texRect.y1 - texRect.y0;
	for ( int k = 0; k < w; k++ )
	{
		const float flQuadTexelCentre = q.x0 + ( k + 0.5f ) * ( q.x1 - q.x0 ) / (float)w;
		const float flGamePixelCentre = ( (float)( texRect.x0 + k ) + 0.5f ) / layerScaleX - offX;
		REQUIRE_THAT( flQuadTexelCentre, WithinAbs( flGamePixelCentre, 1e-3f ) );
	}
	for ( int k = 0; k < h; k++ )
	{
		const float flQuadTexelCentre = q.y0 + ( k + 0.5f ) * ( q.y1 - q.y0 ) / (float)h;
		const float flGamePixelCentre = ( (float)( texRect.y0 + k ) + 0.5f ) / layerScaleY - offY;
		REQUIRE_THAT( flQuadTexelCentre, WithinAbs( flGamePixelCentre, 1e-3f ) );
	}

	// Letterboxed 1280x720 at 1:1 inside 1920x1080: layer offset (-320,
	// -180), scale 1. The quad's left edge must be exactly -offset + x0.
	Frame lb; lb.flCenterX = 320.0f + 640.0f; lb.flCenterY = 180.0f + 360.0f; lb.flScaleX = 1.0f; lb.flScaleY = 1.0f;
	const FRect q2 = ScaledQuad( IRect{ 630, 350, 651, 371 }, 1280, 720, lb );
	REQUIRE_THAT( q2.x0, WithinAbs( 320.0f + 630.0f, 1e-3f ) );
	REQUIRE_THAT( q2.y0, WithinAbs( 180.0f + 350.0f, 1e-3f ) );
	REQUIRE_THAT( q2.x1 - q2.x0, WithinAbs( 21.0f, 1e-3f ) );

	// The quad's size is the footprint times the per-axis scale.
	REQUIRE_THAT( q.x1 - q.x0, WithinAbs( 15.0f * 1.5f, 1e-3f ) );
	REQUIRE_THAT( q.y1 - q.y0, WithinAbs( 15.0f * 1.125f, 1e-3f ) );
}

TEST_CASE( "Rasterize paints exact texels, bleeds colour into the transparent margin, composites the dot over", "[crosshair]" )
{
	Style st; st.flWidth = 1.0f; st.flLength = 4.0f; st.flGap = 2.0f; st.bDot = false; st.bOutline = false;
	const Frame gf = GameFrame( 1280, 960 );
	const Shape s = Build( st, gf, {} );
	const IRect tr = RasterRect( s );
	const int w = tr.x1 - tr.x0;

	const Argb green = PackArgb( 0x00FF00, 1.0f );
	REQUIRE( green == 0xFF00FF00u );
	const std::vector<Argb> px = Rasterize( s, tr, PackArgb( 0x000000, 0.0f ), green, PackArgb( 0xFF0000, 1.0f ) );
	REQUIRE( px.size() == (size_t)w * (size_t)( tr.y1 - tr.y0 ) );
	auto At = [&]( int gx, int gy ) { return px[(size_t)( gy - tr.y0 ) * w + ( gx - tr.x0 )]; };

	// A line pixel is exactly the line colour at full alpha ...
	REQUIRE( At( 643, 480 ) == green );
	REQUIRE( At( 640, 484 ) == green );
	// ... the gap and the centre stay fully transparent but take the line's
	// RGB (they touch a painted texel), so the filter never mixes towards
	// black -- gap 2 -> hole = 2*(2-1)+1 = 3 pixels (639/640/641) ...
	REQUIRE( At( 641, 480 ) == 0x0000FF00u );
	REQUIRE( At( 643, 479 ) == 0x0000FF00u ); // row above the right arm
	// ... the corners of the margin, which touch nothing, are 0 ...
	REQUIRE( At( tr.x0, tr.y0 ) == 0u );
	REQUIRE( At( tr.x1 - 1, tr.y1 - 1 ) == 0u );
	// ... and the margin column next to the arm's far end has the bleed too.
	REQUIRE( At( 646, 480 ) == 0x0000FF00u );

	// Outline on: the ring is black and opaque; the margin beside it is
	// transparent black (bled black, which IS the outline's colour). Gap 3
	// -> hole = 2*(3-1)+1 = 5 pixels, comfortably wider than the two 1px
	// outline rings encroaching from each side, so the centre stays
	// genuinely transparent to test the bleed against.
	Style so = st; so.bOutline = true; so.flOutlineWidth = 1.0f; so.flGap = 3.0f;
	const Shape s2 = Build( so, gf, {} );
	const IRect tr2 = RasterRect( s2 );
	const int w2 = tr2.x1 - tr2.x0;
	const std::vector<Argb> px2 = Rasterize( s2, tr2, PackArgb( 0x000000, 1.0f ), green, 0u );
	auto At2 = [&]( int gx, int gy ) { return px2[(size_t)( gy - tr2.y0 ) * w2 + ( gx - tr2.x0 )]; };
	REQUIRE( At2( 643, 480 ) == green );
	REQUIRE( At2( 643, 479 ) == 0xFF000000u ); // outline above the arm
	REQUIRE( At2( 643, 478 ) == 0x00000000u ); // margin above the outline
	REQUIRE( At2( 640, 480 ) == 0x00000000u ); // centre pixel: gap, transparent, next to outline -> black RGB

	// A half-transparent red dot over an opaque green plus (gap 0) blends
	// OVER, as the vector path's SRC_ALPHA blend does: ~(128, 127, 0), alpha 1.
	Style sd; sd.flWidth = 1.0f; sd.flLength = 3.0f; sd.flGap = 0.0f; sd.bDot = true; sd.flDotSize = 1.0f; sd.bOutline = false;
	const Shape s3 = Build( sd, gf, {} );
	const IRect tr3 = RasterRect( s3 );
	const int w3 = tr3.x1 - tr3.x0;
	const std::vector<Argb> px3 = Rasterize( s3, tr3, 0u, green, PackArgb( 0xFF0000, 0.5f ) );
	const Argb c = px3[(size_t)( 480 - tr3.y0 ) * w3 + ( 640 - tr3.x0 )];
	REQUIRE( ( c >> 24 ) == 0xFF );
	const int r = ( c >> 16 ) & 0xFF, g = ( c >> 8 ) & 0xFF, b = c & 0xFF;
	REQUIRE( ( r >= 127 && r <= 128 ) );
	REQUIRE( ( g >= 127 && g <= 128 ) );
	REQUIRE( b == 0 );
}

TEST_CASE( "PackArgb is B8G8R8A8 memory order and clamps alpha", "[crosshair]" )
{
	REQUIRE( PackArgb( 0x112233, 1.0f ) == 0xFF112233u );
	REQUIRE( PackArgb( 0x112233, 0.0f ) == 0x00112233u );
	REQUIRE( PackArgb( 0x112233, 2.0f ) == 0xFF112233u );
	REQUIRE( PackArgb( 0x112233, -1.0f ) == 0x00112233u );
}

// ---------------------------------------------------------------------
// 2026-09-06: Shrink's phase split, the reversible hide animation, and
// the CPU-stretched raster (requests #11, #13, #14)
// ---------------------------------------------------------------------

TEST_CASE( "ShrinkSplit gives each Shrink phase time in proportion to the distance its edge travels", "[crosshair]" )
{
	REQUIRE_THAT( ShrinkSplit( 3.0f, 6.0f ), WithinAbs( 1.0f / 3.0f, 1e-6f ) );
	REQUIRE( ShrinkSplit( 0.0f, 6.0f ) == 0.0f );
	REQUIRE( ShrinkSplit( 6.0f, 0.0f ) == 1.0f );
	REQUIRE( ShrinkSplit( 0.0f, 0.0f ) == 0.5f );

	// Gap 3, length 6: the gap closes over the first third, the arms
	// shrink over the remaining two thirds ...
	const float p = ShrinkSplit( 3.0f, 6.0f );
	HideState s = EvaluateHide( HideMode::Shrink, 1.0f / 6.0f, p );
	REQUIRE_THAT( s.flGap, WithinAbs( 0.5f, 1e-5f ) );
	REQUIRE( s.flLength == 1.0f );
	s = EvaluateHide( HideMode::Shrink, 1.0f / 3.0f, p );
	REQUIRE_THAT( s.flGap, WithinAbs( 0.0f, 1e-5f ) );
	REQUIRE_THAT( s.flLength, WithinAbs( 1.0f, 1e-5f ) );
	s = EvaluateHide( HideMode::Shrink, 2.0f / 3.0f, p );
	REQUIRE( s.flGap == 0.0f );
	REQUIRE_THAT( s.flLength, WithinAbs( 0.5f, 1e-5f ) );
	REQUIRE( EvaluateHide( HideMode::Shrink, 1.0f, p ).flLength == 0.0f );

	// ... so the visible edge's travel -- gap closed plus length lost, in
	// pixels -- is LINEAR in f: the same speed in both phases (#11).
	auto Travel = [&]( float f )
	{
		const HideState h = EvaluateHide( HideMode::Shrink, f, p );
		return 3.0f * ( 1.0f - h.flGap ) + 6.0f * ( 1.0f - h.flLength );
	};
	for ( int i = 0; i <= 8; i++ )
		REQUIRE_THAT( Travel( i / 8.0f ), WithinAbs( 9.0f * i / 8.0f, 1e-4f ) );

	// The old 50/50 split is still what a caller gets by default, and
	// with no gap the whole time shrinks the arms.
	REQUIRE( EvaluateHide( HideMode::Shrink, 0.5f ).flGap == 0.0f );
	REQUIRE( EvaluateHide( HideMode::Shrink, 0.5f ).flLength == 1.0f );
	REQUIRE_THAT( EvaluateHide( HideMode::Shrink, 0.25f, 0.0f ).flLength, WithinAbs( 0.75f, 1e-5f ) );
	REQUIRE_THAT( EvaluateHide( HideMode::Shrink, 0.25f, 1.0f ).flGap, WithinAbs( 0.75f, 1e-5f ) );
	REQUIRE( EvaluateHide( HideMode::Shrink, 0.25f, 1.0f ).flLength == 1.0f );
	// Focus keeps its 50/50 split whatever the style.
	REQUIRE( EvaluateHide( HideMode::Focus, 0.5f, 0.1f ).flGap == 0.0f );
	REQUIRE( EvaluateHide( HideMode::Focus, 0.5f, 0.1f ).flAlpha == 1.0f );
}

TEST_CASE( "AdvanceHide climbs from the press like HideProgress, reverses from the current progress on release, and never jumps", "[crosshair]" )
{
	constexpr int T = 200; // ms
	constexpr uint64_t ms = 1'000'000ull;
	HideAnim a;

	// Press at 1000 ms; the first frame lands at 1100 ms: half way, exactly
	// what HideProgress() would have said.
	REQUIRE_THAT( AdvanceHide( a, true, 1000 * ms, 1100 * ms, T, true ), WithinAbs( 0.5f, 1e-5f ) );
	REQUIRE( HideAnimating( a ) );
	REQUIRE_THAT( AdvanceHide( a, true, 1000 * ms, 1150 * ms, T, true ), WithinAbs( 0.75f, 1e-5f ) );

	// Release at 1150 ms: the reveal starts from 0.75, at the same rate.
	REQUIRE_THAT( AdvanceHide( a, false, 1150 * ms, 1200 * ms, T, true ), WithinAbs( 0.5f, 1e-5f ) );
	REQUIRE( HideAnimating( a ) );
	REQUIRE_THAT( AdvanceHide( a, false, 1150 * ms, 1250 * ms, T, true ), WithinAbs( 0.25f, 1e-5f ) );

	// Press again at 1250 ms, mid-reveal: the hide resumes from 0.25 (#13:
	// "reverses from the current state, no jump").
	REQUIRE_THAT( AdvanceHide( a, true, 1250 * ms, 1300 * ms, T, true ), WithinAbs( 0.5f, 1e-5f ) );
	REQUIRE_THAT( AdvanceHide( a, true, 1250 * ms, 1400 * ms, T, true ), WithinAbs( 1.0f, 1e-5f ) );
	REQUIRE_FALSE( HideAnimating( a ) ); // fully hidden: static, no forced frames

	// A release seen one frame late is accounted from its own timestamp:
	// 0.05 more of hide up to the release, then 0.2 of reveal.
	HideAnim b;
	REQUIRE_THAT( AdvanceHide( b, true, 1000 * ms, 1050 * ms, T, true ), WithinAbs( 0.25f, 1e-5f ) );
	REQUIRE_THAT( AdvanceHide( b, false, 1060 * ms, 1100 * ms, T, true ), WithinAbs( 0.1f, 1e-5f ) );
	REQUIRE_THAT( AdvanceHide( b, false, 1060 * ms, 1200 * ms, T, true ), WithinAbs( 0.0f, 1e-5f ) );
	REQUIRE_FALSE( HideAnimating( b ) ); // fully back: static again

	// Animate back OFF: a release restores at once, whatever f was.
	HideAnim c;
	REQUIRE_THAT( AdvanceHide( c, true, 1000 * ms, 1150 * ms, T, false ), WithinAbs( 0.75f, 1e-5f ) );
	REQUIRE( AdvanceHide( c, false, 1150 * ms, 1151 * ms, T, false ) == 0.0f );
	REQUIRE_FALSE( HideAnimating( c ) );

	// Time to hide 0: at once, both ways.
	HideAnim d;
	REQUIRE( AdvanceHide( d, true, 1000 * ms, 1000 * ms + 1, 0, true ) == 1.0f );
	REQUIRE( AdvanceHide( d, false, 1001 * ms, 1001 * ms + 1, 0, true ) == 0.0f );

	// Not held and never pressed: nothing to do.
	HideAnim e;
	REQUIRE( AdvanceHide( e, false, 0, 5000 * ms, T, true ) == 0.0f );
	REQUIRE_FALSE( HideAnimating( e ) );
}

namespace
{
	// Straight-alpha channel helpers for OutputRaster texels.
	int A8( Argb v ) { return (int)( ( v >> 24 ) & 0xFF ); }
	int G8( Argb v ) { return (int)( ( v >> 8 ) & 0xFF ); }
	int R8( Argb v ) { return (int)( ( v >> 16 ) & 0xFF ); }
}

TEST_CASE( "ResampleToOutput at 2x: a 1 px line is two rows at 75 % and two at 25 %, gap and length scale with it", "[crosshair]" )
{
	// A 640x360 game drawn 2x onto 1280x720: centre (640, 360), scale (2, 2).
	Style st; st.flWidth = 1.0f; st.flLength = 12.0f; st.flGap = 8.0f; st.bDot = false; st.bOutline = false;
	const Frame gf = GameFrame( 640, 360 );
	const Shape s = Build( st, gf, {} );
	const IRect tr = RasterRect( s );
	const Argb green = PackArgb( 0x00FF00, 1.0f );
	const std::vector<Argb> gamePx = Rasterize( s, tr, 0u, green, 0u );

	Frame fr; fr.flCenterX = 640.0f; fr.flCenterY = 360.0f; fr.flScaleX = 2.0f; fr.flScaleY = 2.0f;
	const OutputRaster out = ResampleToOutput( gamePx, tr, 640, 360, fr );
	REQUIRE_FALSE( out.Empty() );
	const FRect q = ScaledQuad( tr, 640, 360, fr );
	REQUIRE( out.rect.x0 == (int)std::floor( q.x0 ) );
	REQUIRE( out.rect.x1 == (int)std::ceil( q.x1 ) );
	const int ow = out.rect.x1 - out.rect.x0;
	auto At = [&]( int ox, int oy ) -> Argb
	{
		if ( ox < out.rect.x0 || oy < out.rect.y0 || ox >= out.rect.x1 || oy >= out.rect.y1 )
			return 0u;
		return out.px[(size_t)( oy - out.rect.y0 ) * ow + ( ox - out.rect.x0 )];
	};

	// In game pixels the right arm is x 328..339 on row 180 (centre column
	// 320; gap 8 -> hole = 2*(8-1)+1 = 15 game pixels, 313..327, 2026-09-08
	// revised same day). Stretched 2x: output x 656..679, rows 360 and
	// 361. Across the arm the bilinear filter puts 75 % on the two rows it
	// covers and 25 % on the two beside them -- at the LINE's colour, not a
	// darker one (#14: "properly thicker and sub-pixel blurry").
	REQUIRE( A8( At( 663, 360 ) ) == 191 ); REQUIRE( G8( At( 663, 360 ) ) == 255 ); REQUIRE( R8( At( 663, 360 ) ) == 0 );
	REQUIRE( A8( At( 663, 361 ) ) == 191 );
	REQUIRE( A8( At( 663, 359 ) ) == 64 );  REQUIRE( G8( At( 663, 359 ) ) == 255 );
	REQUIRE( A8( At( 663, 362 ) ) == 64 );
	REQUIRE( A8( At( 663, 358 ) ) == 0 );
	REQUIRE( A8( At( 663, 363 ) ) == 0 );
	// Along the arm: the end pixels are 75 % x 75 %, the ones past them
	// 25 % x 75 %, and the gap itself is empty.
	REQUIRE( A8( At( 656, 360 ) ) == 143 );
	REQUIRE( A8( At( 655, 360 ) ) == 48 );
	REQUIRE( A8( At( 679, 360 ) ) == 143 );
	REQUIRE( A8( At( 680, 360 ) ) == 48 );
	REQUIRE( A8( At( 681, 360 ) ) == 0 );
	REQUIRE( A8( At( 640, 360 ) ) == 0 );
	REQUIRE( A8( At( 645, 360 ) ) == 0 );

	// Measured the way scripts/pixel-regression.sh measures: the run of
	// >= 50 % coverage along the row is the arm -- starts at 656, ends at
	// 679; length 24 = 12 x 2; width 2.
	int nFirst = -1, nLast = -1;
	for ( int x = 634; x < 700; x++ )
		if ( A8( At( x, 360 ) ) >= 128 ) { if ( nFirst < 0 ) nFirst = x; nLast = x; }
	REQUIRE( nFirst == 656 );
	REQUIRE( nLast == 679 );
	int nWidth = 0;
	for ( int y = 350; y < 370; y++ )
		if ( A8( At( 663, y ) ) >= 128 ) nWidth++;
	REQUIRE( nWidth == 2 );

	// Coverage is conserved: 48 opaque game texels x 4 output px each.
	double flSum = 0.0;
	for ( Argb v : out.px )
		flSum += A8( v ) / 255.0;
	REQUIRE_THAT( flSum, WithinAbs( 48.0 * 4.0, 1.0 ) );

	// Opacity 0.5: the texel carries the colour the pixel path's blend
	// would have written (colour x opacity) and coverage x opacity as
	// alpha, so an interior pixel is (0, 128, 0) at 75 % x 50 %.
	const std::vector<Argb> halfPx = Rasterize( s, tr, 0u, PackArgb( 0x00FF00, 0.5f ), 0u );
	const OutputRaster half = ResampleToOutput( halfPx, tr, 640, 360, fr );
	const Argb h = half.px[(size_t)( 360 - half.rect.y0 ) * ( half.rect.x1 - half.rect.x0 ) + ( 663 - half.rect.x0 )];
	REQUIRE( ( G8( h ) >= 127 && G8( h ) <= 129 ) );
	REQUIRE( ( A8( h ) >= 95 && A8( h ) <= 97 ) );

	// An outline edge mixes the outline's black into the line's green at
	// the line's own coverage -- never towards a transparent texel's RGB.
	Style so = st; so.bOutline = true; so.flOutlineWidth = 1.0f;
	const Shape s2 = Build( so, gf, {} );
	const IRect tr2 = RasterRect( s2 );
	const OutputRaster o2 = ResampleToOutput( Rasterize( s2, tr2, PackArgb( 0x000000, 1.0f ), green, 0u ), tr2, 640, 360, fr );
	const int ow2 = o2.rect.x1 - o2.rect.x0;
	auto At2 = [&]( int ox, int oy ) { return o2.px[(size_t)( oy - o2.rect.y0 ) * ow2 + ( ox - o2.rect.x0 )]; };
	REQUIRE( A8( At2( 663, 359 ) ) == 255 );                 // fully covered: 25 % line + 75 % outline
	REQUIRE( ( G8( At2( 663, 359 ) ) >= 63 && G8( At2( 663, 359 ) ) <= 65 ) );
	REQUIRE( A8( At2( 663, 357 ) ) == 64 );                  // outline's own soft edge
	REQUIRE( G8( At2( 663, 357 ) ) == 0 );

	// An empty raster (Shrink at 100 %) resamples to nothing.
	Style off; off.bLine = false; off.bDot = false;
	const Shape s3 = Build( off, gf, {} );
	REQUIRE( ResampleToOutput( {}, RasterRect( s3 ), 640, 360, fr ).Empty() );
}
