// Unit tests for the E2 overlay kit's pure half -- design tokens, lane
// arithmetic, the row grammar, the composite band rule, and the registry's
// four registration laws.
//
// WHY THESE PARTS AND NOT THE PAINTING. The kit is deliberately split so that
// everything with real arithmetic or a real rule in it (Tokens, Lane, Row,
// Band, Registry) needs neither an ImGui context nor a font atlas nor the
// overlay's live-theme globals. Only Controls.cpp and Colors.cpp do, and both
// are thin: they pick a colour and hand a rect to a stock ImGui primitive.
// That split is the reason this file exists at all.
//
// The laws in particular are the whole point of the design -- SPEC.md §5.2
// replaces a process defence with a structural one -- and a law without a test
// is a convention. Each law below gets a test that makes it FIRE, not just one
// that shows a well-formed registry passing.
//
// Source of truth for every number asserted here:
//   superdoc/planning/redesign/round-2/e2-inspector-plus/SPEC.md
//   superdoc/planning/redesign/round-2/e2-inspector-plus/API.md
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Overlay/UI/Band.h"
#include "Overlay/UI/Lane.h"
#include "Overlay/UI/Registry.h"
#include "Overlay/UI/Row.h"
#include "Overlay/UI/Tokens.h"

#include <cstdio>
#include <string>

using namespace gamescope;
using Catch::Matchers::WithinAbs;

namespace
{
	// Every test that touches geometry sets the scale explicitly rather than
	// inheriting whatever ran before it -- ui::Scale() is process-global by
	// design (one reader of display_scale in the whole kit).
	struct ScopedScale
	{
		explicit ScopedScale( float fl ) { ui::SetScale( fl ); }
		~ScopedScale() { ui::SetScale( 1.0f ); }
	};
}

// =========================================================================
//  Design tokens
// =========================================================================
TEST_CASE( "tokens: the derived switch geometry is derived, not restated", "[overlay_ui]" )
{
	using namespace gamescope::ui::tok;

	// SPEC §3.0 states these as derivations ("swH - 4", "swW - swK - 4").
	// Tokens.h encodes the derivation rather than the result, so these are a
	// check on the derivation matching the spec's stated numbers -- and, more
	// usefully, a tripwire if someone later replaces a derivation with a
	// literal that drifts.
	STATIC_REQUIRE( kSwitchW == 40.0f );
	STATIC_REQUIRE( kSwitchH == 20.0f );
	STATIC_REQUIRE( kSwitchKnob == 16.0f );
	STATIC_REQUIRE( kSwitchTrvl == 20.0f );

	// The two identities the geometry has to satisfy for the knob to sit
	// inside its track at both ends of its travel.
	STATIC_REQUIRE( kSwitchKnob + 2.0f * kSwitchInset == kSwitchH );
	STATIC_REQUIRE( kSwitchTrvl + kSwitchKnob + 2.0f * kSwitchInset == kSwitchW );

	// SPEC §3.5: B's borderless stepper is 18 + 8 + 18.
	STATIC_REQUIRE( kStepperW == 44.0f );

	// SPEC §3.0's "there is exactly one" row height, and the one control
	// height every hit box honours.
	STATIC_REQUIRE( kRowH == 44.0f );
	STATIC_REQUIRE( kControlH == 28.0f );

	// SPEC §3.0's stated proportion: the switch graphic is 71% of the control
	// height, "the same proportion a slider's 8 track holds" in its own way.
	// Both graphics must fit inside the one hit box.
	STATIC_REQUIRE( kSwitchH <= kControlH );
	STATIC_REQUIRE( kHandleH <= kControlH );
	STATIC_REQUIRE( kTrack   <= kControlH );
}

TEST_CASE( "tokens: everything scales with display_scale", "[overlay_ui]" )
{
	using namespace gamescope::ui;

	{
		ScopedScale s( 0.5f );
		REQUIRE_THAT( Px( tok::kRowH ),     WithinAbs( 22.0f, 1e-4f ) );
		REQUIRE_THAT( Px( tok::kControlH ), WithinAbs( 14.0f, 1e-4f ) );
		REQUIRE_THAT( Px( tok::kSwitchW ),  WithinAbs( 20.0f, 1e-4f ) );
		// SPEC §3.0: "at 0.5x the switch is still a 20 x 14 physical target".
		REQUIRE_THAT( Px( tok::kSwitchH ),  WithinAbs( 10.0f, 1e-4f ) );
	}
	{
		ScopedScale s( 2.0f );
		REQUIRE_THAT( Px( tok::kRowH ),    WithinAbs( 88.0f, 1e-4f ) );
		// SPEC §3.0: "at 2.0x it is 80 x 40 and does not look clumsy".
		REQUIRE_THAT( Px( tok::kSwitchW ), WithinAbs( 80.0f, 1e-4f ) );
		REQUIRE_THAT( Px( tok::kSwitchH ), WithinAbs( 40.0f, 1e-4f ) );
	}
}

TEST_CASE( "tokens: the hairline rule is a floor, not a multiply", "[overlay_ui]" )
{
	using namespace gamescope::ui;

	// SPEC §8.3: "Every hairline is max(1, floor(1 x scale))" -- 1px from 0.5x
	// to 1.99x, 2px at 2.0x, "so a rule never disappears at 0.5x nor thickens
	// into a border at 2.0x". A plain Px(1) would give 0.5 at the bottom end,
	// which is the bug this rule exists to prevent.
	{ ScopedScale s( 0.5f );  REQUIRE_THAT( Hairline(), WithinAbs( 1.0f, 1e-4f ) ); }
	{ ScopedScale s( 1.0f );  REQUIRE_THAT( Hairline(), WithinAbs( 1.0f, 1e-4f ) ); }
	{ ScopedScale s( 1.99f ); REQUIRE_THAT( Hairline(), WithinAbs( 1.0f, 1e-4f ) ); }
	{ ScopedScale s( 2.0f );  REQUIRE_THAT( Hairline(), WithinAbs( 2.0f, 1e-4f ) ); }
}

TEST_CASE( "tokens: SetScale clamps to the config schema's range", "[overlay_ui]" )
{
	using namespace gamescope::ui;

	// ConfigSchema.h pins display_scale to 0.5..2.0. A corrupt config must not
	// become a zero-height row.
	ScopedScale s( 1.0f );
	SetScale( 0.0f );  REQUIRE_THAT( Scale(), WithinAbs( 0.5f, 1e-4f ) );
	SetScale( 9.0f );  REQUIRE_THAT( Scale(), WithinAbs( 2.0f, 1e-4f ) );
	SetScale( -3.0f ); REQUIRE_THAT( Scale(), WithinAbs( 0.5f, 1e-4f ) );
}

TEST_CASE( "tokens: the six type roles pick their own family and size", "[overlay_ui]" )
{
	using namespace gamescope::ui;
	ScopedScale s( 1.0f );

	// SPEC §7.6: "Numbers are always Geist Mono, prose always Geist Sans, and
	// the helper picks the font per zone so a caller cannot put a number in
	// Sans." That guarantee is only real if the role table says so.
	REQUIRE( Type( TypeRole::Value ).eFamily == Family::Mono );
	REQUIRE( Type( TypeRole::Meta ).eFamily  == Family::Mono );
	REQUIRE( Type( TypeRole::Label ).eFamily == Family::Sans );
	REQUIRE( Type( TypeRole::Body ).eFamily  == Family::Sans );

	REQUIRE( Type( TypeRole::Title ).bUpper );
	REQUIRE( Type( TypeRole::Section ).bUpper );
	REQUIRE_FALSE( Type( TypeRole::Label ).bUpper );

	// Sizes are base units and scale like every other token.
	const float flValue1x = TypeSizePx( TypeRole::Value );
	{
		ScopedScale s2( 2.0f );
		REQUIRE_THAT( TypeSizePx( TypeRole::Value ), WithinAbs( flValue1x * 2.0f, 1e-3f ) );
	}
}

TEST_CASE( "tokens: the one easing is monotone and lands on its endpoints", "[overlay_ui]" )
{
	using namespace gamescope::ui;

	// SPEC §8.4: one easing, 1 - (1 - t)^3.
	REQUIRE_THAT( Ease( 0.0f ), WithinAbs( 0.0f, 1e-6f ) );
	REQUIRE_THAT( Ease( 1.0f ), WithinAbs( 1.0f, 1e-6f ) );
	REQUIRE_THAT( Ease( 0.5f ), WithinAbs( 0.875f, 1e-6f ) );
	REQUIRE( Ease( -1.0f ) == 0.0f );   // clamped, not extrapolated
	REQUIRE( Ease( 2.0f ) == 1.0f );

	// Approach() must land exactly on the target when a whole duration
	// elapses in one frame, so a paused overlay never leaves a half-animated
	// control on screen.
	REQUIRE_THAT( Approach( 0.0f, 1.0f, tok::kDurState, tok::kDurState ), WithinAbs( 1.0f, 1e-5f ) );
	REQUIRE_THAT( Approach( 0.0f, 1.0f, tok::kDurState, 0.0f ), WithinAbs( 1.0f, 1e-5f ) );
}

// =========================================================================
//  Lane arithmetic
// =========================================================================
TEST_CASE( "lane: SPEC 8.3's worked example at 804 base comes out exactly", "[overlay_ui]" )
{
	// SPEC §8.3: "At 804 base with one column: Lw = clamp(round(0.46 x 804),
	// ...) = 370. Label column 358, control column 394, affordance 28."
	//
	// This is the test that pins Lane.h's resolution of the doc's own
	// contradictory clamp bounds: the printed bounds (`W - 420` / `W - 200`)
	// cannot produce 370, and the literal bounds (200 / 420) reproduce all
	// three of §8.3's numbers.
	const ui::Lane lane = ui::Lane::ForColumn( 804.0f );

	REQUIRE_THAT( lane.flLw,          WithinAbs( 370.0f, 1e-4f ) );
	REQUIRE_THAT( lane.LabelWidth(),  WithinAbs( 358.0f, 1e-4f ) );
	REQUIRE_THAT( lane.CtlWidth(),    WithinAbs( 394.0f, 1e-4f ) );
	REQUIRE_THAT( lane.flWidth - lane.flAffMin, WithinAbs( 28.0f, 1e-4f ) );

	// And the columns close: 12 + 358 + 12 + 394 + 28 == 804.
	const float flSum = ui::tok::kRowPadLeft + lane.LabelWidth() + ui::tok::kM
	                  + lane.CtlWidth() + ui::tok::kAffordanceW;
	REQUIRE_THAT( flSum, WithinAbs( 804.0f, 1e-4f ) );
}

TEST_CASE( "lane: SPEC 2.2's 470-wide control zone at the 1.0x sheet width", "[overlay_ui]" )
{
	// §2.2 describes the control zone at the 1.0x one-column sheet width (928
	// base) as carrying "a 470-wide slider" and warns against "stretching a
	// 470-wide box". Under the resolved clamp the zone is 468 -- the number
	// the prose is describing. Under the doc's printed bounds it would be 380,
	// which is what makes those bounds the transcription slip.
	const ui::Lane lane = ui::Lane::ForColumn( 928.0f );
	REQUIRE_THAT( lane.flLw,       WithinAbs( 420.0f, 1e-4f ) );
	REQUIRE_THAT( lane.CtlWidth(), WithinAbs( 468.0f, 1e-4f ) );
}

TEST_CASE( "lane: the two hard vertical lines hold at every width", "[overlay_ui]" )
{
	// SPEC §2.1: "Two hard vertical lines run the whole sheet: the value right
	// edge at Lw and the control right edge at W - 28."
	for ( float flW : { 420.0f, 560.0f, 640.0f, 804.0f, 928.0f, 1268.0f, 1728.0f } )
	{
		const ui::Lane lane = ui::Lane::ForColumn( flW );
		INFO( "column width " << flW );

		REQUIRE_THAT( lane.flCtlMax, WithinAbs( flW - ui::tok::kAffordanceW, 1e-4f ) );
		REQUIRE_THAT( lane.flCtlMin, WithinAbs( lane.flLw + ui::tok::kM, 1e-4f ) );
		REQUIRE( lane.flLabelMin == ui::tok::kRowPadLeft );
		REQUIRE( lane.CtlWidth() > 0.0f );

		// The label+value zone stays inside its stated bounds whatever the
		// column does -- content never moves it, and neither does width.
		REQUIRE( lane.flLw >= ui::tok::kLaneMin );
		REQUIRE( lane.flLw <= ui::tok::kLaneMax );
	}
}

TEST_CASE( "lane: the clamp binds at both ends", "[overlay_ui]" )
{
	// Narrow: 0.46 x 400 = 184, below the 200 floor. The floor is what stops a
	// two- or three-column layout squeezing labels to nothing.
	REQUIRE_THAT( ui::Lane::ForColumn( 400.0f ).flLw, WithinAbs( 200.0f, 1e-4f ) );

	// Wide: 0.46 x 2000 = 920, above the 420 ceiling. The ceiling is what
	// stops a wide sheet handing 1200 units to a 40-wide switch.
	REQUIRE_THAT( ui::Lane::ForColumn( 2000.0f ).flLw, WithinAbs( 420.0f, 1e-4f ) );

	// In between, the fraction governs.
	REQUIRE_THAT( ui::Lane::ForColumn( 600.0f ).flLw, WithinAbs( 276.0f, 1e-4f ) );
}

TEST_CASE( "lane: a degenerate column produces no inverted rect", "[overlay_ui]" )
{
	// A column narrower than its own furniture is a shell bug, not a caller's.
	// It must degrade to a zero-width control zone rather than an inverted one
	// that would make every Place() below produce garbage.
	for ( float flW : { 0.0f, 10.0f, 40.0f, 120.0f, 220.0f } )
	{
		const ui::Lane lane = ui::Lane::ForColumn( flW );
		INFO( "column width " << flW );
		REQUIRE( lane.CtlWidth() >= 0.0f );
		REQUIRE( lane.LabelWidth() >= 0.0f );
		REQUIRE( lane.flCtlMin <= lane.flCtlMax );
	}
}

TEST_CASE( "lane: the pixel lane is the base lane, scaled once", "[overlay_ui]" )
{
	// The kit does its arithmetic in base units and converts at the edge. The
	// px lane must therefore be exactly the base lane times the scale, offset
	// by the column origin -- never a second, independent computation.
	const ui::Lane base = ui::Lane::ForColumn( 804.0f );

	for ( float flScale : { 0.5f, 1.0f, 1.25f, 2.0f } )
	{
		ScopedScale s( flScale );
		const float flOrigin = 137.0f;
		const ui::Lane px = base.ToPx( flOrigin );
		INFO( "scale " << flScale );

		REQUIRE_THAT( px.flWidth,    WithinAbs( base.flWidth * flScale, 1e-3f ) );
		REQUIRE_THAT( px.flLw,       WithinAbs( flOrigin + base.flLw * flScale, 1e-3f ) );
		REQUIRE_THAT( px.flCtlMax,   WithinAbs( flOrigin + base.flCtlMax * flScale, 1e-3f ) );
		REQUIRE_THAT( px.flCtlMin,   WithinAbs( flOrigin + base.flCtlMin * flScale, 1e-3f ) );
	}
}

// =========================================================================
//  The row grammar -- the one right-bound allocator
// =========================================================================
TEST_CASE( "row: every control's right edge is the lane's, always", "[overlay_ui]" )
{
	// SPEC §2.1: "Every control's right edge is on the second line -- a
	// 30-wide switch, a 96-wide stepper, a full-bleed slider and a 3x3 anchor
	// grid all end there." This is the property that makes left alignment
	// unrepresentable rather than merely discouraged.
	ScopedScale s( 1.0f );
	const ui::Lane lane = ui::Lane::ForColumn( 804.0f );
	const ui::RowCtx row = ui::RowCtx::ForRow( lane, 0.0f, 0.0f );

	const float flExpected = lane.flCtlMax;

	REQUIRE_THAT( row.Place( ui::tok::kSwitchW ).Max.x,  WithinAbs( flExpected, 1e-4f ) );
	REQUIRE_THAT( row.Place( ui::tok::kStepperW ).Max.x, WithinAbs( flExpected, 1e-4f ) );
	REQUIRE_THAT( row.Place( 1.0f ).Max.x,               WithinAbs( flExpected, 1e-4f ) );
	REQUIRE_THAT( row.PlaceFull().Max.x,                 WithinAbs( flExpected, 1e-4f ) );
	REQUIRE_THAT( row.PlacePx( 3.0f ).Max.x,             WithinAbs( flExpected, 1e-4f ) );
}

TEST_CASE( "row: an oversized control shrinks rather than escaping left", "[overlay_ui]" )
{
	ScopedScale s( 1.0f );
	const ui::Lane lane = ui::Lane::ForColumn( 420.0f );
	const ui::RowCtx row = ui::RowCtx::ForRow( lane, 0.0f, 0.0f );

	// Ask for far more than the lane holds. The rect must be clamped to the
	// lane, not allowed to start left of it and overrun the label column.
	const ImRect rc = row.Place( 5000.0f );
	REQUIRE_THAT( rc.Max.x, WithinAbs( lane.flCtlMax, 1e-4f ) );
	REQUIRE( rc.Min.x >= lane.flCtlMin - 1e-4f );
	REQUIRE_THAT( rc.GetWidth(), WithinAbs( lane.CtlWidth(), 1e-4f ) );

	// A negative width cannot invert the rect either.
	const ImRect rcNeg = row.PlacePx( -100.0f );
	REQUIRE( rcNeg.Min.x <= rcNeg.Max.x );
}

TEST_CASE( "row: every control sits in a kControlH hit box, centred", "[overlay_ui]" )
{
	// SPEC §3.0's mechanically checkable rule: "every control occupies an
	// --H-tall hit box. A control with a box border fills it; a control whose
	// graphic is deliberately shorter is centred in it."
	for ( float flScale : { 0.5f, 1.0f, 2.0f } )
	{
		ScopedScale s( flScale );
		const ui::Lane lane = ui::Lane::ForColumn( 804.0f );
		const float flTop = 100.0f;
		const ui::RowCtx row = ui::RowCtx::ForRow( lane, 0.0f, flTop );
		INFO( "scale " << flScale );

		const ImRect rcRow = row.Bounds();
		REQUIRE_THAT( rcRow.GetHeight(), WithinAbs( ui::Px( ui::tok::kRowH ), 1e-3f ) );

		for ( ImRect rc : { row.Place( ui::tok::kSwitchW ), row.PlaceFull(), row.Place( ui::tok::kStepperW ) } )
		{
			REQUIRE_THAT( rc.GetHeight(), WithinAbs( ui::Px( ui::tok::kControlH ), 1e-3f ) );
			REQUIRE_THAT( rc.GetCenter().y, WithinAbs( rcRow.GetCenter().y, 1e-3f ) );
		}
	}
}

TEST_CASE( "row: the label and value split one zone and cannot overlap", "[overlay_ui]" )
{
	ScopedScale s( 1.0f );
	const ui::Lane lane = ui::Lane::ForColumn( 804.0f );
	const ui::RowCtx row = ui::RowCtx::ForRow( lane, 0.0f, 0.0f );

	ImRect rcLabel, rcValue;

	SECTION( "a value is right-bound at Lw and the label takes the remainder" )
	{
		row.SplitLabelZone( 60.0f, &rcLabel, &rcValue );
		REQUIRE_THAT( rcValue.Max.x, WithinAbs( lane.flLw, 1e-4f ) );
		REQUIRE_THAT( rcValue.GetWidth(), WithinAbs( 60.0f, 1e-4f ) );
		REQUIRE( rcLabel.Max.x <= rcValue.Min.x );
		REQUIRE_THAT( rcLabel.Min.x, WithinAbs( lane.flLabelMin, 1e-4f ) );
	}

	SECTION( "a value cannot squeeze the label to nothing" )
	{
		// SPEC §2.3: "The value column ellipsizes at 60% of the label+value
		// zone, so a 40-character option name cannot squeeze the label to
		// nothing."
		row.SplitLabelZone( 100000.0f, &rcLabel, &rcValue );
		REQUIRE_THAT( rcValue.GetWidth(),
			WithinAbs( lane.LabelWidth() * ui::tok::kValueMaxFrac, 1e-3f ) );
		REQUIRE( rcLabel.GetWidth() > 0.0f );
		REQUIRE( rcLabel.Max.x <= rcValue.Min.x );
	}

	SECTION( "a kind with no value column gives the label the whole zone" )
	{
		row.SplitLabelZone( 0.0f, &rcLabel, &rcValue );
		REQUIRE_THAT( rcLabel.Max.x, WithinAbs( lane.flLw, 1e-4f ) );
		REQUIRE_THAT( rcValue.GetWidth(), WithinAbs( 0.0f, 1e-4f ) );
	}
}

TEST_CASE( "row: the affordance column is 28 base and ends at the row edge", "[overlay_ui]" )
{
	// SPEC §2.4: 28 base units holding at most one glyph.
	ScopedScale s( 1.0f );
	const ui::Lane lane = ui::Lane::ForColumn( 804.0f );
	const ui::RowCtx row = ui::RowCtx::ForRow( lane, 0.0f, 0.0f );

	const ImRect rcAff = row.Affordance();
	REQUIRE_THAT( rcAff.GetWidth(), WithinAbs( ui::Px( ui::tok::kAffordanceW ), 1e-4f ) );
	REQUIRE_THAT( rcAff.Max.x, WithinAbs( row.Bounds().Max.x, 1e-4f ) );

	// It never overlaps the control zone -- the control's right edge is the
	// affordance column's left edge.
	REQUIRE_THAT( rcAff.Min.x, WithinAbs( row.PlaceFull().Max.x, 1e-4f ) );

	// SPEC §2.1: the state edge is 2px at x = 0.
	REQUIRE_THAT( row.StateEdge().GetWidth(), WithinAbs( 2.0f, 1e-4f ) );
	REQUIRE_THAT( row.StateEdge().Min.x, WithinAbs( row.Bounds().Min.x, 1e-4f ) );
}

// =========================================================================
//  Composite bands
// =========================================================================
TEST_CASE( "band: height is quantised to n x 44 with n in {2,3}", "[overlay_ui]" )
{
	// SPEC §4.2 clause 1. This is what keeps ImGuiListClipper's uniform step
	// exact without a prefix sum.
	ScopedScale s( 1.0f );
	const ui::Lane lane = ui::Lane::ForColumn( 804.0f );

	for ( ui::CompositeKind eKind : { ui::CompositeKind::Anchor, ui::CompositeKind::Hue,
	                                  ui::CompositeKind::Strip,  ui::CompositeKind::Graph,
	                                  ui::CompositeKind::Color } )
	{
		const ui::BandSpec spec = ui::Band( eKind );
		INFO( "composite kind " << (int)eKind );
		REQUIRE( spec.nLines >= ui::tok::kBandMinLines );
		REQUIRE( spec.nLines <= ui::tok::kBandMaxLines );

		const ui::BandLayout band = ui::LayOutBand( lane, 0.0f, 0.0f, eKind );
		REQUIRE_THAT( band.rcBand.GetHeight(),
			WithinAbs( (float)spec.nLines * ui::Px( ui::tok::kRowH ), 1e-3f ) );
	}
}

TEST_CASE( "band: line 1 reads as a row and the body is on the control line", "[overlay_ui]" )
{
	// SPEC §4.2 clauses 2 and 3. "Scanning the sheet, a composite is
	// indistinguishable from a row until your eye reaches the control column."
	ScopedScale s( 1.0f );
	const ui::Lane lane = ui::Lane::ForColumn( 804.0f );
	const ui::RowCtx plainRow = ui::RowCtx::ForRow( lane, 0.0f, 0.0f );
	const ui::BandLayout band = ui::LayOutBand( lane, 0.0f, 0.0f, ui::CompositeKind::Anchor );

	// Line 1's columns are identical to an ordinary row's.
	REQUIRE_THAT( band.line1.Bounds().GetHeight(), WithinAbs( plainRow.Bounds().GetHeight(), 1e-4f ) );
	REQUIRE_THAT( band.line1.PlaceFull().Max.x,    WithinAbs( plainRow.PlaceFull().Max.x, 1e-4f ) );
	REQUIRE_THAT( band.line1.Affordance().Min.x,   WithinAbs( plainRow.Affordance().Min.x, 1e-4f ) );

	// Clause 3: the body's right edge is the sheet's control line -- the same
	// vertical as every switch and slider.
	REQUIRE_THAT( band.rcBody.Max.x, WithinAbs( plainRow.PlaceFull().Max.x, 1e-4f ) );

	// SPEC §4.3: the anchor body is 96 x 96, and it spans, vertically, within
	// the band it belongs to.
	REQUIRE_THAT( band.rcBody.GetWidth(),  WithinAbs( 96.0f, 1e-3f ) );
	REQUIRE_THAT( band.rcBody.GetHeight(), WithinAbs( 96.0f, 1e-3f ) );
	REQUIRE( band.rcBody.Min.y >= band.rcBand.Min.y - 1e-3f );
	REQUIRE( band.rcBody.Max.y <= band.rcBand.Max.y + 1e-3f );
}

TEST_CASE( "band: a full-bleed body still ends on the control line", "[overlay_ui]" )
{
	ScopedScale s( 1.0f );
	const ui::Lane lane = ui::Lane::ForColumn( 804.0f );
	const ui::BandLayout band = ui::LayOutBand( lane, 0.0f, 0.0f, ui::CompositeKind::Graph );

	REQUIRE_THAT( band.rcBody.Max.x, WithinAbs( lane.flCtlMax, 1e-4f ) );
	REQUIRE_THAT( band.rcBody.GetWidth(), WithinAbs( lane.CtlWidth(), 1e-3f ) );
}

// =========================================================================
//  The taxonomy's derived decisions
// =========================================================================
TEST_CASE( "kind: the value column is decided by the kind, never the caller", "[overlay_ui]" )
{
	// SPEC §2.3's table. A control that displays its own value must never
	// duplicate it in the value column.
	STATIC_REQUIRE( ui::UsesValueColumn( ui::Kind::Switch ) );
	STATIC_REQUIRE( ui::UsesValueColumn( ui::Kind::Slider ) );
	STATIC_REQUIRE( ui::UsesValueColumn( ui::Kind::Stepper ) );
	STATIC_REQUIRE( ui::UsesValueColumn( ui::Kind::Meter ) );
	STATIC_REQUIRE( ui::UsesValueColumn( ui::Kind::Composite ) );

	STATIC_REQUIRE_FALSE( ui::UsesValueColumn( ui::Kind::Choice ) );
	STATIC_REQUIRE_FALSE( ui::UsesValueColumn( ui::Kind::Text ) );
	STATIC_REQUIRE_FALSE( ui::UsesValueColumn( ui::Kind::Bank ) );
	STATIC_REQUIRE_FALSE( ui::UsesValueColumn( ui::Kind::Facts ) );
	STATIC_REQUIRE_FALSE( ui::UsesValueColumn( ui::Kind::Action ) );
}

TEST_CASE( "kind: read-only is a property of the kind, not a flag", "[overlay_ui]" )
{
	// SPEC §5.1: selecting a Facts, Meter or Graph row opens Details, and its
	// CONFIGURE cell reads `ro`. "That state is Kind::IsReadOnly(), not a flag
	// anyone sets, so there is no way to have Configure content and an `ro`
	// cell."
	STATIC_REQUIRE( ui::IsReadOnly( ui::Kind::Facts ) );
	STATIC_REQUIRE( ui::IsReadOnly( ui::Kind::Meter ) );
	STATIC_REQUIRE_FALSE( ui::IsReadOnly( ui::Kind::Switch ) );

	ui::Registry reg;
	ui::Area &area = reg.Add( "test.area", "Test", ui::Section::Display );

	bool bDummy = false;
	int  nA = 0, nB = 0;
	REQUIRE_FALSE( area.Switch( "test.sw", "Switch", ui::Bind( &bDummy ) ).Help( "h" ).ReadOnly() );
	REQUIRE( area.Facts( "test.facts", "Facts", [] { return std::string( "x" ); } ).Help( "h" ).ReadOnly() );

	// The frametime Graph composite is read-only; the other composites are not.
	REQUIRE( area.Composite( "test.graph", "Graph", ui::CompositeKind::Graph, {} ).Help( "h" ).ReadOnly() );
	REQUIRE_FALSE( area.Composite( "test.anchor", "Anchor", ui::CompositeKind::Anchor,
		ui::Bind( &nA ), ui::Bind( &nB ) ).Help( "h" ).ReadOnly() );
}

// =========================================================================
//  The registration laws
// =========================================================================
TEST_CASE( "law: a Param's id is synthesised from its parent -- the Prefix Law", "[overlay_ui]" )
{
	// SPEC §5.2 clause 2 / API.md §4.2: ".Param() takes a leaf, not an id. The
	// full id is synthesised ... Because the id is synthesised from the
	// parent, a Param's config key is a child of its parent's key by
	// construction -- the law is not checked, it is unrepresentable to break."
	ui::Registry reg;
	ui::Area &area = reg.Add( "display.upscaling", "Upscaling", ui::Section::Display );

	float flSharp = 0.0f;
	bool  bDenoise = false;

	ui::Entry &e = area.Slider( "display.sharpness", "Sharpness", ui::Bind( &flSharp ) )
		.Range( 0.0f, 20.0f ).Help( "Strength of the sharpening pass." );
	e.Param( "rcas_denoise", "RCAS denoise", ui::Bind( &bDenoise ) ).Help( "Suppresses grain." );

	REQUIRE( e.ParamCount() == 1 );
	REQUIRE( e.ParamAt( 0 ).Id() == "display.sharpness.rcas_denoise" );

	// The registry can find it by that id -- which is what makes a param
	// searchable in the palette exactly like a sheet row (SPEC §5.2).
	REQUIRE( reg.FindParam( "display.sharpness.rcas_denoise" ) != nullptr );
	REQUIRE( reg.FindParam( "display.hdr_mode" ) == nullptr );

	// A well-formed registry reports nothing.
	ui::LawRecorder rec;
	REQUIRE( reg.SelfTest() == 0 );
	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "law: a Param leaf containing a dot is rejected -- the One-Level Rule", "[overlay_ui]" )
{
	// The type system already makes a Param-owning-a-Param unsayable. A dot in
	// the leaf is the other way someone could try to express one, by smuggling
	// the nesting into the id, so it is caught here.
	ui::Registry reg;
	ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
	bool b = false;
	ui::Entry &e = area.Switch( "a.thing", "Thing", ui::Bind( &b ) ).Help( "h" );

	ui::LawRecorder rec;
	e.Param( "deep.leaf", "Deep", ui::Bind( &b ) );

	REQUIRE( rec.Caught( ui::Law::OneLevel ) );
	REQUIRE( e.ParamCount() == 0 );   // the param was NOT added

	// An empty leaf is a Prefix Law violation -- there is nothing to
	// synthesise an id from.
	e.Param( "", "Empty", ui::Bind( &b ) );
	REQUIRE( rec.Caught( ui::Law::Prefix ) );
	REQUIRE( e.ParamCount() == 0 );
}

TEST_CASE( "law: the seventh Param aborts registration -- the Six Budget", "[overlay_ui]" )
{
	// SPEC §5.2 clause 3: "A row may own at most 6 Params ... The 7th .Param()
	// is a registration abort whose message is 'monitor.modules has 7
	// parameters -- promote it to a category.' Junk-drawer pressure is thereby
	// converted into a structural decision that shows up in the rail."
	ui::Registry reg;
	ui::Area &area = reg.Add( "system.monitor", "Monitor", ui::Section::System );
	bool b = false;
	ui::Entry &e = area.Switch( "monitor.modules", "Modules", ui::Bind( &b ) ).Help( "h" );

	// Six is the budget, and exactly six must be legal -- API.md §8 registers
	// Adaptive Brightness at exactly six on purpose ("the intended pressure").
	{
		ui::LawRecorder rec;
		for ( int i = 0; i < 6; ++i )
		{
			char szLeaf[ 16 ];
			snprintf( szLeaf, sizeof( szLeaf ), "p%d", i );
			e.Param( szLeaf, "P", ui::Bind( &b ) ).Help( "h" );
		}
		REQUIRE( rec.Count() == 0 );
		REQUIRE( e.ParamCount() == 6 );
	}

	// The seventh fires, and is not added.
	{
		ui::LawRecorder rec;
		e.Param( "seventh", "Seventh", ui::Bind( &b ) ).Help( "h" );
		REQUIRE( rec.Caught( ui::Law::SixBudget ) );
		REQUIRE( e.ParamCount() == 6 );
		REQUIRE( rec.Violations().front().sMessage.find( "at most 6" ) != std::string::npos );
	}
}

TEST_CASE( "law: ids are unique registry-wide, params included", "[overlay_ui]" )
{
	ui::Registry reg;
	ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
	bool b = false;

	area.Switch( "a.dup", "One", ui::Bind( &b ) ).Help( "h" );

	SECTION( "a second entry with the same id" )
	{
		ui::LawRecorder rec;
		area.Switch( "a.dup", "Two", ui::Bind( &b ) ).Help( "h" );
		REQUIRE( rec.Caught( ui::Law::UniqueId ) );
		REQUIRE( area.EntryCount() == 1 );   // the duplicate was not added
	}

	SECTION( "a param whose synthesised id collides with an entry" )
	{
		// This is the case that makes the law worth having registry-wide
		// rather than per-entry: a param's id and an entry's id live in the
		// same namespace, because the palette and ui_snapshot address both by
		// it.
		ui::Entry &e = area.Switch( "a.parent", "Parent", ui::Bind( &b ) ).Help( "h" );
		area.Switch( "a.parent.leaf", "Collides", ui::Bind( &b ) ).Help( "h" );

		ui::LawRecorder rec;
		e.Param( "leaf", "Leaf", ui::Bind( &b ) ).Help( "h" );
		REQUIRE( rec.Caught( ui::Law::UniqueId ) );
		REQUIRE( e.ParamCount() == 0 );
	}

	SECTION( "an area id collides with an entry id" )
	{
		ui::LawRecorder rec;
		reg.Add( "a", "A again", ui::Section::Setup );
		REQUIRE( rec.Caught( ui::Law::UniqueId ) );
	}
}

TEST_CASE( "law: Help() is required and cannot be empty", "[overlay_ui]" )
{
	// API.md §4.1: ".Help( sz ) -- required; aborts at registration if
	// missing". Two halves: the empty-text call fires immediately, and the
	// never-called case can only be decided once registration is over.
	SECTION( "empty text fires at the call" )
	{
		ui::Registry reg;
		ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
		bool b = false;

		ui::LawRecorder rec;
		area.Switch( "a.x", "X", ui::Bind( &b ) ).Help( "" );
		REQUIRE( rec.Caught( ui::Law::HelpRequired ) );
	}

	SECTION( "a null string fires at the call" )
	{
		ui::Registry reg;
		ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
		bool b = false;

		ui::LawRecorder rec;
		area.Switch( "a.x", "X", ui::Bind( &b ) ).Help( nullptr );
		REQUIRE( rec.Caught( ui::Law::HelpRequired ) );
	}

	SECTION( "never calling it at all fires at SelfTest" )
	{
		ui::Registry reg;
		ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
		bool b = false;
		area.Switch( "a.x", "X", ui::Bind( &b ) );   // no Help()

		ui::LawRecorder rec;
		REQUIRE( reg.SelfTest() == 1 );
		REQUIRE( rec.Caught( ui::Law::HelpRequired ) );
	}

	SECTION( "a param without Help() fires too" )
	{
		ui::Registry reg;
		ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
		bool b = false;
		area.Switch( "a.x", "X", ui::Bind( &b ) ).Help( "h" )
			.Param( "leaf", "Leaf", ui::Bind( &b ) );   // no Help()

		ui::LawRecorder rec;
		REQUIRE( reg.SelfTest() == 1 );
		REQUIRE( rec.Caught( ui::Law::HelpRequired ) );
		REQUIRE( rec.Violations().front().sId == "a.x.leaf" );
	}
}

TEST_CASE( "law: DisabledUnless has no spelling without a reason", "[overlay_ui]" )
{
	// API.md §3.2: ".DisabledUnless() has no overload without a reason string
	// -- that is the entire enforcement mechanism for the most common
	// inconsistency in the current code, a control that greys out and does not
	// say why." The overload is gone at compile time; an empty string is the
	// residual runtime hole, and it is closed.
	ui::Registry reg;
	ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
	bool b = false;

	ui::LawRecorder rec;
	area.Switch( "a.x", "X", ui::Bind( &b ) ).Help( "h" )
		.DisabledUnless( [] { return false; }, "" );
	REQUIRE( rec.Caught( ui::Law::ReasonRequired ) );

	// With a reason, the row reports it only while the predicate is false.
	bool bAllowed = false;
	ui::Entry &e = area.Switch( "a.y", "Y", ui::Bind( &b ) ).Help( "h" )
		.DisabledUnless( [ &bAllowed ] { return bAllowed; }, "the backend has no VRR path" );
	REQUIRE( e.DisabledReason() == "the backend has no VRR path" );
	bAllowed = true;
	REQUIRE( e.DisabledReason().empty() );
}

TEST_CASE( "registry: the Anchor declaration from API.md 7 registers as documented", "[overlay_ui]" )
{
	// API.md §7 chains two .Param() calls off one Entry: the second is made on
	// the *Parameter* the first returned, and must produce another child of
	// the same Entry. That is what keeps the documented spelling compiling
	// while leaving "a Param owning a Param" unsayable -- and it is the fix
	// for SPEC §4.1's orphaned offset steppers, which become Params here.
	ui::Registry reg;
	ui::Area &area = reg.Add( "system.monitor", "Monitor", ui::Section::System );
	area.Group( "Placement" );

	int nV = 0, nH = 0, nMarginV = 0, nMarginH = 0;

	area.Composite( "monitor.anchor", "Placement", ui::CompositeKind::Anchor,
			ui::Bind( &nV ), ui::Bind( &nH ) )
		.Default( 0, 2 )
		.Help( "Which screen corner the monitor is anchored to. The offsets nudge it away." )
		.Keywords( "anchor placement position corner where margin offset" )
		.Param( "margin_v", "Vertical margin", ui::Bind( &nMarginV ) )
			.Default( 32 ).Unit( "px" ).Range( 0.0f, 400.0f )
			.Help( "Distance from the anchored horizontal edge." )
		.Param( "margin_h", "Horizontal margin", ui::Bind( &nMarginH ) )
			.Default( 32 ).Unit( "px" ).Range( 0.0f, 400.0f )
			.Help( "Distance from the anchored vertical edge." );

	REQUIRE( area.EntryCount() == 1 );

	const ui::Entry &e = area.EntryAt( 0 );
	REQUIRE( e.ParamCount() == 2 );
	REQUIRE( e.ParamAt( 0 ).Id() == "monitor.anchor.margin_v" );
	REQUIRE( e.ParamAt( 1 ).Id() == "monitor.anchor.margin_h" );

	// Both are children of the same Entry -- siblings, not a chain of nesting.
	REQUIRE( e.ParamAt( 0 ).Owner() == &e );
	REQUIRE( e.ParamAt( 1 ).Owner() == &e );

	// Both are reachable by the id the Prefix Law synthesised, which is the
	// searchability guarantee SPEC §5.2 leans on.
	REQUIRE( reg.FindParam( "monitor.anchor.margin_h" ) != nullptr );

	ui::LawRecorder rec;
	REQUIRE( reg.SelfTest() == 0 );
}

TEST_CASE( "registry: a binding round-trips its value", "[overlay_ui]" )
{
	int nValue = 3;
	const ui::AnyBind bind = ui::Bind( &nValue );

	REQUIRE( bind.IsBound() );
	REQUIRE( std::get<int>( bind.Get() ) == 3 );
	bind.Set( ui::Value( 11 ) );
	REQUIRE( nValue == 11 );

	// An unbound AnyBind degrades honestly rather than crashing -- API.md §9's
	// "Bind degrades honestly: no schema means no default".
	const ui::AnyBind empty;
	REQUIRE_FALSE( empty.IsBound() );
	REQUIRE( std::holds_alternative<std::monostate>( empty.Get() ) );
	empty.Set( ui::Value( 1 ) );   // must not crash

	// A switch reads "on"/"off" in the value column (SPEC §2.3's amendment).
	REQUIRE( ui::ValueToString( ui::Value( true ) ) == "on" );
	REQUIRE( ui::ValueToString( ui::Value( false ) ) == "off" );
}
