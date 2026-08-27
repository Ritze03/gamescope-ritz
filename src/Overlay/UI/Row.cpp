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
		// The common case -- a control that fills the whole control zone --
		// is exactly flControlLeftPx == flCtlMin, which the two-argument
		// overload reduces back to Lw (see its own comment).
		SplitLabelZone( flValueWidthPx, m_flCtlMin, pOutLabel, pOutValue );
	}

	void RowCtx::SplitLabelZone( float flValueWidthPx, float flControlLeftPx,
	                             ImRect *pOutLabel, ImRect *pOutValue ) const
	{
		const float flZoneW = ImMax( m_flLw - m_flLabelMin, 0.0f );
		const float flGap    = Px( tok::kGapLabel );
		const float flGutter = Px( tok::kM );

		// SPEC §2.3: the value ellipsizes at 60% of the label+value zone.
		// Sized off the ZONE (labelMin..Lw), not off however far right the
		// anchor below ends up -- a narrow atom's dead space becomes room
		// the value visually crosses to reach it, not extra room the 60% cap
		// grows into, so a too-long value still clips exactly as before.
		const float flValueW = ImClamp( flValueWidthPx, 0.0f, flZoneW * tok::kValueMaxFrac );

		// The value is right-bound just before wherever THIS row's control
		// actually starts, less the row's usual gutter -- Lw is only the
		// floor. A control can never start left of flCtlMin == Lw + gutter,
		// so this can never place the anchor left of Lw; it only ever moves
		// the value right, toward its control, which is a no-op for a
		// full-bleed control (flControlLeftPx == flCtlMin) and the fix for a
		// narrow one right-bound deeper in the zone (Switch, Stepper, a
		// Composite's own body).
		const float flAnchor = ImMax( flControlLeftPx - flGutter, m_flLw );

		// The label gets everything left of the value, minus one gap. Both
		// come out of this one subdivision.
		const float flValueMin = flAnchor - flValueW;
		const float flLabelMax = ImMax( m_flLabelMin, flValueW > 0.0f ? flValueMin - flGap : flAnchor );

		if ( pOutLabel )
			*pOutLabel = ImRect( m_flLabelMin, m_rcBounds.Min.y, flLabelMax, m_rcBounds.Max.y );
		if ( pOutValue )
			*pOutValue = ImRect( flValueMin, m_rcBounds.Min.y, flAnchor, m_rcBounds.Max.y );
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
