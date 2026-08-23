#include "Band.h"

namespace gamescope::ui
{
	BandSpec Band( CompositeKind eKind )
	{
		// API.md §7's kSpecs table, verbatim. x == 0 means "full-bleed to the
		// lane". SPEC §4.4 lists five composites; Colour override is
		// documented there but not registered in the mockup, and it is kept
		// here for the same reason -- one band rule either way, no call site
		// choosing geometry.
		switch ( eKind )
		{
			case CompositeKind::Anchor: return { 3, ImVec2( 96.0f, 96.0f ) };
			case CompositeKind::Hue:    return { 2, ImVec2(  0.0f, 44.0f ) };
			case CompositeKind::Strip:  return { 2, ImVec2(  0.0f, 52.0f ) };
			case CompositeKind::Graph:  return { 3, ImVec2(  0.0f, 96.0f ) };
			case CompositeKind::Color:  return { 2, ImVec2(  0.0f, 52.0f ) };
		}
		return { 2, ImVec2( 0.0f, 44.0f ) };
	}

	BandLayout LayOutBand( const Lane &laneBase, float flOriginPx, float flTopPx, CompositeKind eKind )
	{
		const BandSpec spec = Band( eKind );

		// Clause 1: the height is n x 44, n in {2,3}. Nothing else -- which is
		// what keeps ImGuiListClipper's uniform step exact without a prefix
		// sum (a band is n clipper items whose first item paints the band).
		const int nLines = ImClamp( spec.nLines, tok::kBandMinLines, tok::kBandMaxLines );

		// Clause 2: line 1 is an ordinary row context, so a composite is
		// indistinguishable from a row until your eye reaches the control
		// column.
		const RowCtx line1 = RowCtx::ForBandLine( laneBase, flOriginPx, flTopPx, 0 );
		const RowCtx lineN = RowCtx::ForBandLine( laneBase, flOriginPx, flTopPx, nLines - 1 );

		const ImRect rcBand( line1.Bounds().Min.x, line1.Bounds().Min.y,
		                     line1.Bounds().Max.x, lineN.Bounds().Max.y );

		// Clause 3: the body's right edge is the sheet's control line -- the
		// same vertical line as every switch and slider above and below it.
		// Taking it from PlaceFull() rather than recomputing it is what makes
		// that identical rather than merely equal.
		const float flRight = line1.PlaceFull().Max.x;
		const float flBodyW = spec.bodyBase.x > 0.0f ? Px( spec.bodyBase.x ) : line1.CtlWidthPx();
		const float flBodyH = ImMin( Px( spec.bodyBase.y ), rcBand.GetHeight() );
		const float flCy    = rcBand.GetCenter().y;

		const ImRect rcBody( flRight - ImMin( flBodyW, line1.CtlWidthPx() ), flCy - flBodyH * 0.5f,
		                     flRight,                                        flCy + flBodyH * 0.5f );

		// Clause 4 needs no code: no allocator in this file ever produces a
		// rect in the label column of lines 2..n, so there is nothing to put
		// there. "It is air."
		return BandLayout{ rcBand, rcBody, line1 };
	}
}
