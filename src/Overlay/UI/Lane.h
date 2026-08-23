// The lane -- SPEC.md §2.1's four columns, as pure arithmetic.
//
//   x=0  x=12                                 Lw   Lw+12                W-28    W
//    |#|  Label text ................ 5      |  < gap > ............ [control] | <> |
//     2    left-bound              right-bound            right-bound to W-28    28
//          |------ label column ------|      |------- control column -------|  afford.
//
// Two hard vertical lines run the whole sheet: the value's right edge at Lw,
// and the control's right edge at W - 28. THIS FILE IS THE ONLY PLACE EITHER
// IS COMPUTED. RowCtx (Row.h) turns a Lane into rects; Controls.cpp draws
// inside those rects; nothing else may compute an x.
//
// Everything here is in BASE units and free of ImGui, on purpose: the lane is
// the one piece of the design with real arithmetic in it, so it is the one
// piece most worth testing, and a test should not need a graphics context to
// ask where a column starts. tests/test_overlay_ui.cpp pins every number below
// against SPEC.md §8.3's worked example.
#pragma once

#include "Tokens.h"

namespace gamescope::ui
{
	struct Lane
	{
		float flWidth    = 0.0f;  // the column's own width, base units
		float flLw       = 0.0f;  // the value column's right edge
		float flLabelMin = 0.0f;  // label zone  [ flLabelMin, flLw ]
		float flCtlMin   = 0.0f;  // control zone [ flCtlMin, flCtlMax ]
		float flCtlMax   = 0.0f;
		float flAffMin   = 0.0f;  // affordance  [ flAffMin, flWidth ]

		float LabelWidth() const { return flLw - flLabelMin; }
		float CtlWidth() const   { return flCtlMax - flCtlMin; }

		// SPEC §2.2: "Lw is computed once per column by the shell. Content never
		// moves it -- two sheets of the same width have their columns in the
		// same place regardless of the longest label."
		//
		//     Lw = clamp( round( 0.46 x W ), 200, 420 )
		//
		// AMBIGUITY, RESOLVED (recorded because SPEC.md is self-contradictory
		// here). SPEC §2.2 and API.md §6 both print the bounds as `W - 420` and
		// `W - 200`. Those bounds cannot be right: SPEC §8.3 works the same
		// formula at W = 804 and states `clamp( 370, 384, 604 ) = 370`, which
		// is arithmetically false -- 370 is below its own stated lower bound.
		// Reading the bounds as literal base widths of the label+value zone
		// makes every number in §8.3 come out exactly:
		//
		//     W = 804  ->  Lw = clamp( 370, 200, 420 ) = 370
		//                  label column 370 - 12 = 358      (§8.3 says 358)
		//                  control column 804 - 28 - 382 = 394 (§8.3 says 394)
		//                  12 + 358 + 12 + 394 + 28 = 804      (it closes)
		//
		// and it also matches §2.2's own prose elsewhere, which describes a
		// "470-wide" control zone at the 1.0x sheet width of 928 base
		// (928 - 40 - clamp(427,200,420) = 468). The `W -` prefix is a
		// transcription slip in both documents; the bounds are the label zone's.
		//
		// ---- flOccludedRightBase (D17) --------------------------------------
		// How much of this column's RIGHT side is covered by a surface that
		// floats over it -- in practice the Inspector when the ladder has
		// demoted it to a drawer (SPEC §8.3 step 2). Pass 0 -- the default --
		// when nothing overlays the column, which is every case but that one.
		//
		// WHY THE LANE AND NOT THE REGION. At 2.0x the slab is 1728 px, the
		// sheet 804 base and the drawer 400, so the drawer's left edge falls
		// at base 380 of a 756-wide sheet column while the control zone runs
		// [360, 728]: every switch, segmented control and slider on the sheet
		// is painted underneath it. Making the sheet REGION narrower would fix
		// it by turning the drawer into a column, which is what ladder step 1
		// already is (D17 rejects that). So the region is left alone -- the
		// drawer still overlays the sheet's background, and opening or closing
		// it still relayouts nothing -- and the one thing that moves is the
		// lane's right edge, which is the single place control geometry is
		// decided and the only thing that has to move for a control to be
		// reachable.
		//
		// Every derived quantity follows from the reduced width, Lw included.
		// Holding Lw at its full-width value and pulling in only the control
		// zone was tried and does not work: at 2.0x Lw is 348 and the visible
		// width 368, which leaves the control zone 20 units -- i.e. zero after
		// the affordance. The label column has to give way too.
		//
		// COST, per D17: the control zone at 2.0x with the drawer open is 128
		// base rather than 368, so a segmented control with long labels
		// downgrades to a dropdown sooner. That is SPEC §3.3's existing,
		// specified downgrade, not a new failure mode.
		static Lane ForColumn( float flWidthBase, float flOccludedRightBase = 0.0f );

		// The lane in physical pixels, for a column whose left edge sits at
		// flOriginPx. Derived from *this* by one multiplication by Scale() --
		// the px lane is never computed from scratch, so the two can never
		// disagree about where a column is. Pinned by a test at 0.5x/1.0x/2.0x.
		Lane ToPx( float flOriginPx ) const;
	};
}
