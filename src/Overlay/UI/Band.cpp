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
			// The Profiles list (2026-09-06): five lines of list rows plus
			// one for Create / Copy / Edit / Delete. Edge to edge, because
			// the sketch has the list LEADING the sheet, not sitting in a
			// row's control column -- so this is the one band that spends
			// its label column, and clauses 2 and 4 are deliberately not
			// honoured for it (line 1 is list rows, not a labelled row).
			case CompositeKind::List:   return { tok::kListBandLines, ImVec2( 0.0f, 0.0f ), true };
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

		if ( spec.bSpansRow )
		{
			// The List band. Left edge: the label column's own left, taken
			// from the one call that knows it (SplitLabelZone) rather than a
			// second copy of Lane's inset. Right edge: the control line, as
			// every body. Height: whole list rows only -- the ListBox draws
			// floor(height / kControlH) rows and would leave a blank strip
			// of box under a fractional remainder -- centred in the n-1
			// lines above the strip. The strip takes the last line.
			ImRect rcLabel, rcValue;
			line1.SplitLabelZone( 0.0f, &rcLabel, &rcValue );
			const float flLeft  = rcLabel.Min.x;
			const float flRowPx = Px( tok::kRowH );
			const float flItemH = Px( tok::kControlH );
			const float flListAreaH = flRowPx * (float)( nLines - 1 );
			const int   nItems  = ImMax( 1, (int)( ( flListAreaH - Px( tok::kS ) * 2.0f ) / flItemH ) );
			const float flListH = flItemH * (float)nItems;
			const float flListY = rcBand.Min.y + ( flListAreaH - flListH ) * 0.5f;
			const ImRect rcList( flLeft, flListY, flRight, flListY + flListH );

			const float flStripTop = rcBand.Max.y - flRowPx;
			const ImRect rcStrip( flLeft, flStripTop + ( flRowPx - flItemH ) * 0.5f,
			                      flRight, flStripTop + ( flRowPx + flItemH ) * 0.5f );
			return BandLayout{ rcBand, rcList, line1, rcStrip };
		}

		const ImRect rcBody( flRight - ImMin( flBodyW, line1.CtlWidthPx() ), flCy - flBodyH * 0.5f,
		                     flRight,                                        flCy + flBodyH * 0.5f );

		// Clause 4 needs no code: no allocator in this file ever produces a
		// rect in the label column of lines 2..n, so there is nothing to put
		// there. "It is air."
		return BandLayout{ rcBand, rcBody, line1, ImRect() };
	}
}
