// RowCtx -- the row grammar, and the kit's ONLY control allocator.
//
// SPEC.md §2.2, "Why a call site cannot violate it":
//
//   > The row context exposes ONE allocator and it is right-anchored.
//   > There is no Left(), no Centre(), no SameLine, no SetCursorPosX. A
//   > control painter receives a rect and draws inside it. Left-alignment is
//   > not a discouraged choice; it is an unrepresentable one.
//
// That is the load-bearing property of this header and the reason it is
// shaped the way it is. Read the public surface below as a list of things a
// caller CANNOT say:
//
//   * no x argument anywhere -- Place() takes a width and nothing else;
//   * no ImGui cursor is touched, so SameLine()/SetCursorPosX() have nothing
//     to act on even if someone called them;
//   * PlaceFull() and Place() differ only in width; both return a rect whose
//     Max.x is m_flCtlMax, unconditionally, with no flag to change that;
//   * the label and the value rect are produced by ONE call that splits the
//     label zone between them, so they cannot be made to overlap.
//
// Geometry comes from Lane (base units) and is converted to pixels exactly
// once, here. Nothing downstream sees a base unit.
#pragma once

#include "Lane.h"
#include "Tokens.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImRect

namespace gamescope::ui
{
	class RowCtx
	{
	public:
		// A single 44-tall sheet/inspector row. flTopPx is the row's top edge
		// in window space; flOriginPx its left edge.
		static RowCtx ForRow( const Lane &laneBase, float flOriginPx, float flTopPx );

		// Line `nLine` of an n-line composite band (SPEC §4.2 clause 2: "Line 1
		// reads as a row"). Same columns, same height, offset by n row steps.
		static RowCtx ForBandLine( const Lane &laneBase, float flOriginPx, float flTopPx, int nLine );

		// ---- the one allocator ------------------------------------------
		// Right edge is ALWAYS the control lane's right edge. flWidthBase is a
		// base-unit width from tok::; it is clamped to the lane, so an atom
		// wider than the column shrinks rather than escaping left.
		// Vertically the rect is tok::kControlH tall, centred in the row --
		// SPEC §3.0's "every control occupies an --H-tall hit box".
		ImRect Place( float flWidthBase ) const;

		// == Place( the whole control zone ). Slider, meter and every other
		// control whose track *is* the range (SPEC §2.2's table).
		ImRect PlaceFull() const;

		// ---- the label zone ----------------------------------------------
		// Splits [ labelMin, Lw ] between a right-bound value of the measured
		// width and a left-bound label taking the remainder. One call, two
		// outputs, so the two rects are derived from one subdivision and
		// cannot be computed inconsistently. The value is capped at
		// tok::kValueMaxFrac of the zone (SPEC §2.3: "so a 40-character option
		// name cannot squeeze the label to nothing"); pass 0 for a kind that
		// has no value column at all (SPEC §2.3's table -- the KIND decides,
		// never the caller; see Registry.h's UsesValueColumn()).
		//
		// This is == SplitLabelZone( flValueWidthPx, m_flCtlMin, ... ) below --
		// i.e. "assume the control fills the whole control zone", which is
		// true for Slider/Meter/a full-bleed Composite body and is why they
		// never showed the gap issue #<switch-spinner-anchor> names.
		void SplitLabelZone( float flValueWidthPx, ImRect *pOutLabel, ImRect *pOutValue ) const;

		// As above, but the value's right edge is anchored at
		// `flControlLeftPx - the row's usual gutter` instead of always at Lw.
		// Use this overload whenever the row's actual control is a fixed-size
		// atom right-bound WELL INSIDE the control zone -- Switch's 40-wide
		// track, Stepper's 44-wide group, a Composite's own (possibly
		// narrower-than-full) body -- so the value ends up adjacent to
		// whatever the row actually draws instead of stranded at the lane's
		// content-independent Lw. Pass the control's own left edge (its
		// Place()/PlaceFull() rect's Min.x, or a Composite band's rcBody.Min.x)
		// -- never compute or guess it here.
		//
		// A control cannot start left of flCtlMin, so flControlLeftPx -
		// gutter can never fall below m_flLw; the clamp inside just makes
		// that a guarantee instead of a coincidence, so this can never move
		// the value LEFT of where the single-argument overload would put it,
		// only right, toward its control. For a full-bleed control (Slider,
		// Meter, a full-bleed Composite) flControlLeftPx == flCtlMin and the
		// two overloads agree exactly -- SPEC §8.3's worked numbers, and
		// every Slider/Meter row on screen, are unaffected by this overload
		// existing.
		void SplitLabelZone( float flValueWidthPx, float flControlLeftPx,
		                     ImRect *pOutLabel, ImRect *pOutValue ) const;

		// The affordance column -- exactly one glyph, chosen by the fixed
		// priority in SPEC §2.4. 28 base wide, full row height.
		ImRect Affordance() const;

		// The whole row, for the hover/selected fill, the separator and the
		// 2px state edge.
		ImRect Bounds() const     { return m_rcBounds; }
		ImRect StateEdge() const;

		float  CtlWidthPx() const { return m_flCtlMax - m_flCtlMin; }

		// Measured atoms (segmented, chip bank) size themselves in pixels from
		// CalcTextSize, so they need a pixel-width entry point. Same right
		// edge, same centring, same clamp -- it is literally the same function
		// Place() calls. Not `private` only because Controls.cpp needs it;
		// still takes a width and no x, so the law is untouched.
		ImRect PlacePx( float flWidthPx ) const;

		// A zero RowCtx. Public only so aggregates that hold one (BandLayout)
		// stay aggregates; every real instance comes from a factory above,
		// and a default-constructed one allocates zero-width rects rather
		// than a wrong-width one.
		RowCtx() = default;

	private:
		ImRect m_rcBounds;
		float  m_flLabelMin = 0.0f;
		float  m_flLw       = 0.0f;
		float  m_flCtlMin   = 0.0f;
		float  m_flCtlMax   = 0.0f;
		float  m_flAffMin   = 0.0f;
	};
}
