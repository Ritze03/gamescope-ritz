#include "Row.h"

namespace gamescope::ui
{
	RowCtx RowCtx::ForRow( const Lane &laneBase, float flOriginPx, float flTopPx )
	{
		return ForBandLine( laneBase, flOriginPx, flTopPx, 0 );
	}

	RowCtx RowCtx::ForBandLine( const Lane &laneBase, float flOriginPx, float flTopPx, int nLine )
	{
		const Lane px = laneBase.ToPx( flOriginPx );
		const float flRowH = Px( tok::kRowH );
		const float flTop  = flTopPx + flRowH * (float)nLine;

		RowCtx row;
		row.m_rcBounds  = ImRect( flOriginPx, flTop, flOriginPx + px.flWidth, flTop + flRowH );
		row.m_flLabelMin = px.flLabelMin;
		row.m_flLw       = px.flLw;
		row.m_flCtlMin   = px.flCtlMin;
		row.m_flCtlMax   = px.flCtlMax;
		row.m_flAffMin   = px.flAffMin;
		return row;
	}

	ImRect RowCtx::PlacePx( float flWidthPx ) const
	{
		// API.md §6, verbatim: the right edge is m_flCtlMax, always. This is
		// the one rect construction in the kit that positions a control, and
		// both public entry points route through it, so full-bleed and sized
		// controls provably share one right edge and one vertical centring
		// rule.
		flWidthPx = ImClamp( flWidthPx, 0.0f, CtlWidthPx() );

		// SPEC §3.0: the hit box is kControlH tall and centred in the row,
		// whatever the graphic inside it turns out to be.
		const float flH   = Px( tok::kControlH );
		const float flTop = m_rcBounds.Min.y + ( m_rcBounds.GetHeight() - flH ) * 0.5f;

		return ImRect( m_flCtlMax - flWidthPx, flTop, m_flCtlMax, flTop + flH );
	}

	ImRect RowCtx::Place( float flWidthBase ) const { return PlacePx( Px( flWidthBase ) ); }
	ImRect RowCtx::PlaceFull() const                { return PlacePx( CtlWidthPx() ); }

	void RowCtx::SplitLabelZone( float flValueWidthPx, ImRect *pOutLabel, ImRect *pOutValue ) const
	{
		const float flZoneW = ImMax( m_flLw - m_flLabelMin, 0.0f );
		const float flGap   = Px( tok::kGapLabel );

		// SPEC §2.3: the value ellipsizes at 60% of the label+value zone.
		const float flValueW = ImClamp( flValueWidthPx, 0.0f, flZoneW * tok::kValueMaxFrac );

		// The value is right-bound at Lw; the label gets everything left of it,
		// minus one gap. Both come out of this one subdivision.
		const float flValueMin = m_flLw - flValueW;
		const float flLabelMax = ImMax( m_flLabelMin, flValueW > 0.0f ? flValueMin - flGap : m_flLw );

		if ( pOutLabel )
			*pOutLabel = ImRect( m_flLabelMin, m_rcBounds.Min.y, flLabelMax, m_rcBounds.Max.y );
		if ( pOutValue )
			*pOutValue = ImRect( flValueMin, m_rcBounds.Min.y, m_flLw, m_rcBounds.Max.y );
	}

	ImRect RowCtx::Affordance() const
	{
		return ImRect( m_flAffMin, m_rcBounds.Min.y, m_rcBounds.Max.x, m_rcBounds.Max.y );
	}

	ImRect RowCtx::StateEdge() const
	{
		// SPEC §2.1: 2px at x = 0, full row height. Scaled like every other
		// token -- it is a state signal, not a hairline, so it does not use
		// Hairline()'s floor rule.
		return ImRect( m_rcBounds.Min.x, m_rcBounds.Min.y,
		               m_rcBounds.Min.x + Px( 2.0f ), m_rcBounds.Max.y );
	}
}
