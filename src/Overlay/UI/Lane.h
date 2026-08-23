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
		static Lane ForColumn( float flWidthBase );

		// The lane in physical pixels, for a column whose left edge sits at
		// flOriginPx. Derived from *this* by one multiplication by Scale() --
		// the px lane is never computed from scratch, so the two can never
		// disagree about where a column is. Pinned by a test at 0.5x/1.0x/2.0x.
		Lane ToPx( float flOriginPx ) const;
	};
}
