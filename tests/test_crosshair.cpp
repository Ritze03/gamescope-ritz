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
	// 960 only.
	const auto px = Pixels( s.lines );
	REQUIRE( px.size() == 4 * 4 );
	for ( int x = 963; x < 967; x++ ) REQUIRE( px.count( { x, 540 } ) );       // right arm: col 960 + gap 2 -> starts at 963
	for ( int x = 954; x < 958; x++ ) REQUIRE( px.count( { x, 540 } ) );       // left arm: ends at 958 (exclusive)
	for ( int y = 543; y < 547; y++ ) REQUIRE( px.count( { 960, y } ) );       // down arm
	for ( int y = 534; y < 538; y++ ) REQUIRE( px.count( { 960, y } ) );       // up arm
	// Nothing on rows 539/541 or columns 959/961 -- no half-alpha neighbours
	// can exist because nothing is emitted there at all.
	for ( int x = 950; x < 970; x++ )
	{
		REQUIRE_FALSE( px.count( { x, 539 } ) );
		REQUIRE_FALSE( px.count( { x, 541 } ) );
	}
	for ( int y = 530; y < 550; y++ )
	{
		REQUIRE_FALSE( px.count( { 959, y } ) );
		REQUIRE_FALSE( px.count( { 961, y } ) );
	}
}

TEST_CASE( "an even-width line straddles the centre edge symmetrically", "[crosshair]" )
{
	Style st;
	st.flWidth = 2.0f; st.flLength = 3.0f; st.flGap = 3.0f; st.bDot = false; st.bOutline = false;
	Frame fr; fr.flCenterX = 960.0f; fr.flCenterY = 540.0f;

	const Shape s = Build( st, fr, {} );
	const auto px = Pixels( s.lines );
	// Even thickness centres on the pixel EDGE at 960/540: the centre
	// column is 959..960, the centre row 539..540. The gap is measured from
	// that column/row's edge, so the right arm starts at 961 + 3 = 964 and
	// the left arm ends at 959 - 3 = 956 (exclusive).
	for ( int y : { 539, 540 } )
	{
		for ( int x = 964; x < 967; x++ ) REQUIRE( px.count( { x, y } ) );
		for ( int x = 953; x < 956; x++ ) REQUIRE( px.count( { x, y } ) );
	}
	// Vertical arms: columns 959 and 960. Down 544..547, up 533..536.
	for ( int x : { 959, 960 } )
	{
		for ( int y = 544; y < 547; y++ ) REQUIRE( px.count( { x, y } ) );
		for ( int y = 533; y < 536; y++ ) REQUIRE( px.count( { x, y } ) );
	}
	REQUIRE( px.size() == 4 * 3 * 2 );
	// The centre 2x2 stays empty while the gap is open.
	REQUIRE_FALSE( px.count( { 959, 539 } ) );
	REQUIRE_FALSE( px.count( { 960, 540 } ) );
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
	const auto px = Pixels( s.lines );
	REQUIRE( px.count( { 502, 350 } ) ); REQUIRE( px.count( { 503, 350 } ) );
	REQUIRE( px.count( { 497, 350 } ) ); REQUIRE( px.count( { 498, 350 } ) );
	REQUIRE( px.count( { 500, 352 } ) ); REQUIRE( px.count( { 500, 353 } ) );
	REQUIRE( px.count( { 500, 347 } ) ); REQUIRE( px.count( { 500, 348 } ) );
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
	// A 1280x960 game: centre (640, 480). 1px line, length 4, gap 2, no
	// dot, no outline -> column/row 640/480, arms 634..638 and 643..647.
	Style st; st.flWidth = 1.0f; st.flLength = 4.0f; st.flGap = 2.0f; st.bDot = false; st.bOutline = false;
	const Frame gf = GameFrame( 1280, 960 );
	REQUIRE( gf.flScaleX == 1.0f );
	REQUIRE( gf.flScaleY == 1.0f );
	const Shape s = Build( st, gf, {} );

	REQUIRE( BoundingBox( s ) == IRect{ 634, 474, 647, 487 } );
	REQUIRE( RasterRect( s ) == IRect{ 633, 473, 648, 488 } ); // 15 x 15 texels
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
	// black ...
	REQUIRE( At( 642, 480 ) == 0x0000FF00u );
	REQUIRE( At( 643, 479 ) == 0x0000FF00u ); // row above the right arm
	// ... the corners of the margin, which touch nothing, are 0 ...
	REQUIRE( At( tr.x0, tr.y0 ) == 0u );
	REQUIRE( At( tr.x1 - 1, tr.y1 - 1 ) == 0u );
	// ... and the margin column next to the arm's far end has the bleed too.
	REQUIRE( At( 647, 480 ) == 0x0000FF00u );

	// Outline on: the ring is black and opaque; the margin beside it is
	// transparent black (bled black, which IS the outline's colour).
	Style so = st; so.bOutline = true; so.flOutlineWidth = 1.0f;
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
