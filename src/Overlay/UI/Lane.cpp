#include "Lane.h"

namespace gamescope::ui
{
	Lane Lane::ForColumn( float flWidthBase, float flOccludedRightBase )
	{
		using namespace tok;

		Lane lane;
		lane.flWidth = std::max( flWidthBase, 0.0f );

		// D17: a surface floating over this column's right side (the Inspector
		// as a drawer) takes the lane's right edge with it, minus one gutter,
		// so that nothing the lane places can land underneath it. See Lane.h
		// for why this is the lane's job rather than the region's.
		//
		// This is the WHOLE fix: it lands before every derivation below, so Lw,
		// the control zone and the affordance all follow from the reduced width
		// without a second rule anywhere. The occluded case and the ordinary
		// case are therefore the same arithmetic, not two branches that can
		// drift.
		const float flOccluded = std::max( flOccludedRightBase, 0.0f );
		if ( flOccluded > 0.0f )
			lane.flWidth = std::max( lane.flWidth - flOccluded - kM, 0.0f );

		// The label+value zone. See the header for why the clamp bounds are
		// literal widths rather than SPEC.md's printed `W - 420` / `W - 200`.
		const float flLwRaw = std::round( kLaneFrac * lane.flWidth );
		lane.flLw = std::clamp( flLwRaw, kLaneMin, kLaneMax );

		lane.flLabelMin = kRowPadLeft;
		lane.flAffMin   = lane.flWidth - kAffordanceW;
		lane.flCtlMin   = lane.flLw + kM;   // the 12-unit gutter of SPEC §2.1's diagram
		lane.flCtlMax   = lane.flAffMin;

		// A column narrower than its own furniture is a shell bug, not a
		// caller's; degrade to a zero-width control zone rather than an
		// inverted rect that would make every Place() below produce garbage.
		lane.flCtlMax = std::max( lane.flCtlMax, lane.flCtlMin );
		lane.flLw     = std::min( lane.flLw, lane.flCtlMin );
		lane.flLabelMin = std::min( lane.flLabelMin, lane.flLw );

		return lane;
	}

	Lane Lane::ToPx( float flOriginPx ) const
	{
		const float s = Scale();

		Lane px;
		px.flWidth    = flWidth * s;
		px.flLw       = flOriginPx + flLw * s;
		px.flLabelMin = flOriginPx + flLabelMin * s;
		px.flCtlMin   = flOriginPx + flCtlMin * s;
		px.flCtlMax   = flOriginPx + flCtlMax * s;
		px.flAffMin   = flOriginPx + flAffMin * s;
		return px;
	}
}
