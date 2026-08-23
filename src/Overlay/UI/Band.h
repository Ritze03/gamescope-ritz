// Composite bands -- SPEC.md §4.2's n x 44 rule, and API.md §7's kSpecs table.
//
// Fix #3 was "composite controls look orphaned; the named case is Monitor >
// Placement > Anchor (the 3x3 grid)". The cure is that a control taller than
// one row is not a row at all:
//
//   > A control taller than one row is not a row. It is a Composite -- a
//   > full-width band whose height is exactly n x 44 base, which participates
//   > in the row table's columns, and whose body may contain only controls
//   > from the taxonomy.
//
// This file is the ONLY place a band's geometry is computed -- which is what
// stops today's two Position Grid call sites (notification placement, monitor
// placement) drifting apart again, and why a call site can choose neither n
// nor the body size.
//
// Pure arithmetic over Lane and RowCtx: no ImGui state, no fonts, no draw
// list, so all four clauses are directly testable.
#pragma once

#include "Lane.h"
#include "Registry.h"   // CompositeKind
#include "Row.h"

namespace gamescope::ui
{
	struct BandSpec
	{
		int    nLines   = 2;      // n, in {2, 3}
		ImVec2 bodyBase = {};     // base-unit body size; x == 0 means full-bleed
	};

	// API.md §7's kSpecs, verbatim.
	BandSpec Band( CompositeKind eKind );

	struct BandLayout
	{
		ImRect rcBand;    // the whole n x 44 band
		ImRect rcBody;    // right-bound to the lane's control edge, spanning lines 1..n
		RowCtx line1;     // "line 1 reads as a row" -- clause 2
	};

	// The four clauses, all mechanical, all satisfied here:
	//   1. height quantisation -- rcBand is exactly nLines x kRowH;
	//   2. line 1 reads as a row -- `line1` is an ordinary RowCtx, so the
	//      label and value land in the sheet's own columns;
	//   3. the body is right-bound to the control lane's right edge and spans
	//      lines 1..n;
	//   4. nothing may occupy the label column on lines 2..n -- "it is air".
	//      Needs no enforcement: no allocator here ever produces a rect there.
	BandLayout LayOutBand( const Lane &laneBase, float flOriginPx, float flTopPx, CompositeKind eKind );
}
