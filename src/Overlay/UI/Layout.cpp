#include "Layout.h"
#include "Tokens.h"   // tok::kRowH / kS / kM -- constants only, imgui-free

#include <algorithm>
#include <cmath>
#include <variant>

namespace gamescope::ui
{
	// =====================================================================
	//  The slab (SPEC §8.1)
	// =====================================================================
	Slab Slab::For( float flSurfaceWPx, float flSurfaceHPx, float flScale )
	{
		Slab slab;
		slab.flScale = flScale <= 0.0f ? 1.0f : flScale;

		//   min( surfaceW x 0.90, max( 1560 x scale, 1180 ) )
		//   min( surfaceH x 0.86, 940 x scale )
		//
		// Note the asymmetry, which is SPEC.md's own and not a slip: width
		// has a 1180 FLOOR inside the max() -- so a small surface still gets
		// a slab wide enough for rail + inspector + a usable sheet -- while
		// height has none, because a short slab merely scrolls.
		slab.flWidthPx  = std::min( flSurfaceWPx * shelltok::kSurfFracW,
		                            std::max( shelltok::kSlabBaseW * slab.flScale, shelltok::kSlabMinW ) );
		slab.flHeightPx = std::min( flSurfaceHPx * shelltok::kSurfFracH,
		                            shelltok::kSlabBaseH * slab.flScale );

		// Never wider or taller than the surface itself: the max(…,1180)
		// above can exceed a genuinely tiny surface, and a slab hanging off
		// the screen edge is worse than a cramped one.
		slab.flWidthPx  = std::min( slab.flWidthPx,  flSurfaceWPx );
		slab.flHeightPx = std::min( slab.flHeightPx, flSurfaceHPx );

		slab.flWidthPx  = std::max( slab.flWidthPx,  0.0f );
		slab.flHeightPx = std::max( slab.flHeightPx, 0.0f );

		slab.flWidthBase  = slab.flWidthPx  / slab.flScale;
		slab.flHeightBase = slab.flHeightPx / slab.flScale;
		return slab;
	}

	// =====================================================================
	//  The ladder (SPEC §8.3)
	// =====================================================================
	LadderResult Solve( const Slab &slab, InspectorHost ePreferred, int nRowsInArea,
	                    bool bUnsplittable )
	{
		LadderResult out;
		const float Wb = slab.flWidthBase;

		out.flRailBase      = shelltok::kRailFull;
		out.flInspectorBase = shelltok::kInspector;
		out.eHost           = InspectorHost::Column;

		// "The ladder is one comparison applied twice." Literally twice, on
		// purpose: the first application collapses the rail, and the SAME
		// predicate is then re-asked with the narrower rail already in hand.
		// Writing it as one helper called twice is what makes it impossible
		// for the two steps to drift apart.
		const auto Cramped = [ & ]() {
			return out.flRailBase + out.flInspectorBase + shelltok::kSheetMin >= Wb;
		};
		if ( Cramped() ) out.flRailBase = shelltok::kRailIcons;
		if ( Cramped() ) out.eHost      = InspectorHost::Drawer;

		// The user's preference, applied against the ladder's answer.
		//
		// THE LADDER WINS DOWNWARD, THE USER WINS AT THE BOTTOM. Asking for
		// a column on a slab the ladder already demoted to a drawer does not
		// promote it back (that is what "the ladder wins over preference" in
		// index.html's own comment means). Asking for Hidden always works,
		// because step 3 is specified as reachable "by choice and persisted,
		// not only by width".
		if ( ePreferred == InspectorHost::Hidden )
			out.eHost = InspectorHost::Hidden;
		else if ( ePreferred == InspectorHost::Drawer && out.eHost == InspectorHost::Column )
			out.eHost = InspectorHost::Drawer;

		// What the sheet is left with. A drawer OVERLAYS, so it costs the
		// sheet nothing; hidden costs exactly the spine's 20.
		const float flHostCost =
			out.eHost == InspectorHost::Column ? out.flInspectorBase :
			out.eHost == InspectorHost::Hidden ? shelltok::kSpine    : 0.0f;
		out.flSheetBase = std::max( 0.0f, Wb - out.flRailBase - flHostCost );

		out.nWidthColumns = out.flSheetBase >= shelltok::kThreeColMin ? 3
		                  : out.flSheetBase >= shelltok::kTwoColMin   ? 2 : 1;

		// SPEC §8.3: "Columns are capped by content, not only by width" --
		// "a 7-row category never spreads into two columns just because it
		// fits", because half-empty columns were the worst thing about E's
		// dense sheets.
		const int nByContent = std::max( 1,
			(int)std::ceil( (double)std::max( 0, nRowsInArea ) / (double)shelltok::kRowsPerColumn ) );
		out.nColumns = std::min( out.nWidthColumns, nByContent );

		// An escaped legacy panel or a content body cannot be split -- see
		// Layout.h. Applied last so it overrides both caps rather than
		// racing them.
		if ( bUnsplittable )
			out.nColumns = 1;

		// SPEC §8.3's "Step" column. Reported rather than used, so a test --
		// and the Shell's own diagnostics area -- can assert the table.
		out.nStep = out.eHost == InspectorHost::Hidden ? 3
		          : out.eHost == InspectorHost::Drawer ? 2
		          : out.RailIsIcons()                  ? 1
		          : out.nWidthColumns == 3             ? -1 : 0;
		return out;
	}

	// =====================================================================
	//  The sheet's columns (SPEC §8.3) -- D20.2
	// =====================================================================
	SheetColumnSet LayOutSheetColumns( float flBodyWPx, float flOccludedRightPx,
	                                   int nColumns, float flScale )
	{
		SheetColumnSet out;
		const float s = flScale <= 0.0f ? 1.0f : flScale;

		out.nColumns = std::clamp( nColumns, 1, kMaxSheetColumns );

		// index.html's own pad and gutter: both the sheet's 24-unit pad.
		const float flPad    = tok::kSheetPad * s;
		const float flGutter = tok::kSheetPad * s;

		const float flInner = flBodyWPx - 2.0f * flPad
		                    - (float)( out.nColumns - 1 ) * flGutter;
		float flColW = flInner / (float)out.nColumns;

		// A column narrower than its own furniture is a shell bug, not a
		// caller's -- the same degradation rule Lane::ForColumn applies, for
		// the same reason: a negative width would make every Place() below
		// produce an inverted rect.
		flColW = std::max( flColW, 0.0f );

		// Where the drawer's left edge falls, in the body's own coordinates.
		const float flOccluded  = std::max( flOccludedRightPx, 0.0f );
		const float flDrawerAtX = flBodyWPx - flOccluded;

		for ( int i = 0; i < out.nColumns; ++i )
		{
			SheetColumn &c = out.cols[ i ];

			c.rc.x0 = flPad + (float)i * ( flColW + flGutter );
			c.rc.x1 = c.rc.x0 + flColW;
			// y is the caller's -- a column spans the body's full height and
			// this function has no opinion about where that starts.
			c.rc.y0 = 0.0f;
			c.rc.y1 = 0.0f;

			// How much of THIS column the drawer covers. Zero for every
			// column entirely left of the drawer's edge; the whole overlap
			// for the rightmost. See Layout.h for why this is per column.
			const float flColOccl = std::max( 0.0f, c.rc.x1 - flDrawerAtX );

			c.lane = Lane::ForColumn( flColW / s, flColOccl / s );
		}
		return out;
	}

	// =====================================================================
	//  The regions (SPEC §8.1)
	// =====================================================================
	Regions Regions::For( const Slab &slab, const LadderResult &ladder )
	{
		Regions r;
		const float s  = slab.flScale;
		const float W  = slab.flWidthPx;
		const float H  = slab.flHeightPx;

		const float flBarH   = shelltok::kSlabBar   * s;
		const float flHeadH  = shelltok::kSheetHead * s;
		const float flFootH  = shelltok::kSheetFoot * s;
		const float flStripH = shelltok::kModeStrip * s;

		r.rcSlabBar = { 0.0f, 0.0f, W, std::min( flBarH, H ) };
		r.rcBody    = { 0.0f, r.rcSlabBar.y1, W, H };

		const float flRailPx  = ladder.flRailBase      * s;
		const float flInspPx  = ladder.flInspectorBase * s;
		const float flSpinePx = shelltok::kSpine       * s;

		r.rcRail = { r.rcBody.x0, r.rcBody.y0, r.rcBody.x0 + flRailPx, r.rcBody.y1 };

		// The sheet's right edge is the ONE place the host difference lands.
		// A column takes width from the sheet; a drawer does not (it floats
		// over it); hidden takes only the spine. Everything else about the
		// sheet is identical in all three, which is why the host change is a
		// 160 ms region animation and not a relayout.
		float flSheetRight = r.rcBody.x1;
		if ( ladder.eHost == InspectorHost::Column )      flSheetRight -= flInspPx;
		else if ( ladder.eHost == InspectorHost::Hidden ) flSheetRight -= flSpinePx;
		flSheetRight = std::max( flSheetRight, r.rcRail.x1 );

		r.rcSheet = { r.rcRail.x1, r.rcBody.y0, flSheetRight, r.rcBody.y1 };

		r.rcSheetHead = { r.rcSheet.x0, r.rcSheet.y0, r.rcSheet.x1,
		                  std::min( r.rcSheet.y0 + flHeadH, r.rcSheet.y1 ) };
		r.rcSheetFoot = { r.rcSheet.x0, std::max( r.rcSheet.y1 - flFootH, r.rcSheetHead.y1 ),
		                  r.rcSheet.x1, r.rcSheet.y1 };
		r.rcSheetBody = { r.rcSheet.x0, r.rcSheetHead.y1, r.rcSheet.x1, r.rcSheetFoot.y0 };

		if ( ladder.eHost == InspectorHost::Hidden )
		{
			// SPEC §8.05: the spine "holds its own width, so the sheet lane
			// shrinks by 20 rather than the sheet being overlapped by an
			// invisible hit strip". That is why it is carved out of the
			// sheet above rather than drawn on top of it here.
			r.rcSpine = { r.rcBody.x1 - flSpinePx, r.rcBody.y0, r.rcBody.x1, r.rcBody.y1 };
		}
		else
		{
			// Column and drawer share one rect -- right-aligned, full body
			// height. They differ only in z-order and in whether the sheet
			// already gave up the width (above), never in geometry.
			r.rcInspector = { std::max( r.rcBody.x1 - flInspPx, r.rcRail.x1 ),
			                  r.rcBody.y0, r.rcBody.x1, r.rcBody.y1 };
			r.rcModeStrip = { r.rcInspector.x0, r.rcInspector.y0, r.rcInspector.x1,
			                  std::min( r.rcInspector.y0 + flStripH, r.rcInspector.y1 ) };
			r.rcInspectorBody = { r.rcInspector.x0, r.rcModeStrip.y1,
			                      r.rcInspector.x1, r.rcInspector.y1 };
		}
		return r;
	}

	// =====================================================================
	//  Mode selection (SPEC §5.1)
	// =====================================================================
	InspectorMode ModeFor( Kind eKind, CompositeKind eComposite )
	{
		// "Selecting a Facts, Meter or Graph row opens Details; everything
		// else -- including arriving from the palette on a parameter --
		// opens Configure."
		//
		// Graph is the composite that has no control in it at all, so it
		// belongs with the other two read-only kinds even though its Kind is
		// Composite. Asking Registry.h's own IsReadOnly() would NOT be
		// equivalent: that predicate is about whether a control can be
		// constructed, and a Composite is writable in general.
		if ( eKind == Kind::Facts || eKind == Kind::Meter )
			return InspectorMode::Details;
		if ( eKind == Kind::Composite && eComposite == CompositeKind::Graph )
			return InspectorMode::Details;
		return InspectorMode::Configure;
	}

	ModeCounts CountsFor( const Entry &entry )
	{
		ModeCounts c;
		c.bReadOnly = entry.ReadOnly();

		// CONFIGURE holds "the row's own control drawn as an Inspector row,
		// followed by its <= 6 parameters". A read-only row's values block
		// is replaced by one sentence, so it contributes no settable rows
		// and its counter reads `ro` -- represented here as 0 + bReadOnly,
		// with the "ro" spelling left to the drawing code.
		c.nConfigure = c.bReadOnly ? 0 : 1;
		c.nConfigure += (int)entry.ParamCount();

		// DETAILS holds the derived binding grid, plus the .Live() block.
		// Only the grid rows that ACTUALLY have a value are counted -- a
		// counter that always read 9 would be decoration, and SPEC §5.1 is
		// explicit that the strip is "a readout of what depth this selection
		// actually has".
		int nGrid = 1;                                    // `now`  -- always present
		if ( !std::holds_alternative<std::monostate>( entry.DefaultValue() ) ) nGrid++;  // default
		if ( entry.HasRange() )               nGrid++;    // range
		if ( !entry.Options().empty() )       nGrid++;    // options
		nGrid++;                                          // kind   -- always present
		nGrid++;                                          // key    -- always present (the id)
		c.nDetails = nGrid + (int)entry.LiveCount();
		return c;
	}

	// Mirrors the row arithmetic in Shell.cpp's DrawConfigure, band for
	// band. Kept here rather than there because Shell.cpp cannot be reached
	// from a test -- it needs an ImGui context -- and this is the quantity
	// the scroll range exists to accommodate.
	float ConfigureRowsHeight( int nParams, float flScale )
	{
		// The chrome above the rows that is also a fixed height: the body's
		// top pad and the title's line. Only the help paragraph and any
		// disabled reason are left out, because only those need font
		// metrics -- and both can only make the real body taller.
		float flH = ( tok::kInspectorPad + shelltok::kTitleLine ) * flScale;

		// The VALUES band: the rule's gap, the section label's line, and
		// the entry's own row at the one control height.
		flH += ( tok::kS + shelltok::kSectionLine + tok::kRowH ) * flScale;

		// The PARAMETERS band, only when there is one.
		if ( nParams > 0 )
			flH += ( tok::kM + shelltok::kSectionLine + (float)nParams * tok::kRowH ) * flScale;

		return flH;
	}

	float RailScroll( float flCurrent, float flContentH, float flViewH,
	                  float flActiveTop, float flItemH, float flPad )
	{
		const float flMax = std::max( 0.0f, flContentH - flViewH );

		float flScroll = flCurrent;

		// Only chase the active item when there is somewhere to scroll TO.
		// When the content fits, flMax is 0 and the clamp below returns 0
		// regardless -- so a rail that fits can never be left scrolled,
		// which is what makes a scale change back down self-correcting.
		if ( flMax > 0.0f && flActiveTop >= 0.0f )
		{
			// Above the fold: pull up by exactly the shortfall, so the item
			// lands against the top pad rather than being centred. Moving
			// the least possible keeps the rail visually stable while the
			// selection walks.
			const float flAbove = ( flActiveTop - flPad ) - flScroll;
			if ( flAbove < 0.0f )
				flScroll += flAbove;

			// Below the fold: same, against the bottom edge.
			const float flBelow = ( flActiveTop + flItemH + flPad ) - ( flScroll + flViewH );
			if ( flBelow > 0.0f )
				flScroll += flBelow;
		}

		return std::clamp( flScroll, 0.0f, flMax );
	}
}
