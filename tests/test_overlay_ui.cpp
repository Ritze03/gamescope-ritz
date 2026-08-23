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

#include "Overlay/Fonts.h"
#include "Overlay/UI/Band.h"
#include "Overlay/UI/Icons.h"
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

// =========================================================================
//  The baked glyph range (D18)
// =========================================================================
// The shell shipped "inspector ›" with a fallback box in it. U+203A is inside
// Geist but outside the atlas's baked Basic Latin + Latin-1, and the spine is
// only visible after the Inspector is hidden -- a corner nobody visits, which
// is where a box glyph survives longest.
//
// FirstUnbakedCodepoint() turns "is this string drawable" into a question, and
// `overlay_e2_glyphs` asks it of every registered string. These tests pin the
// decoder, because a checker that silently passes bad input is worse than no
// checker at all.
TEST_CASE( "glyphs: plain ASCII and Latin-1 are inside the baked range", "[overlay_ui]" )
{
	REQUIRE( fonts::FirstUnbakedCodepoint( "" ) == 0 );
	REQUIRE( fonts::FirstUnbakedCodepoint( nullptr ) == 0 );
	REQUIRE( fonts::FirstUnbakedCodepoint( "inspector" ) == 0 );
	REQUIRE( fonts::FirstUnbakedCodepoint( "^I  inspector      Tab  region" ) == 0 );

	// The two multi-byte characters the shell DOES use, both Latin-1 and both
	// genuinely baked: the middle dot in every separator, and the degree sign
	// on the colour-temperature row.
	REQUIRE( fonts::FirstUnbakedCodepoint( "rail 60 \xC2\xB7 sheet 804" ) == 0 );
	REQUIRE( fonts::FirstUnbakedCodepoint( "6500\xC2\xB0" ) == 0 );

	// The exact ends of the range.
	REQUIRE( fonts::FirstUnbakedCodepoint( " " ) == 0 );          // U+0020
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xC3\xBF" ) == 0 );   // U+00FF
}

TEST_CASE( "glyphs: the three marks the design wanted are all rejected", "[overlay_ui]" )
{
	// This is the test that would have caught the shipped defect.
	REQUIRE( fonts::FirstUnbakedCodepoint( "inspector \xE2\x80\xBA" ) == 0x203A );  // ›
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xE2\x96\xB8" ) == 0x25B8 );            // ▸
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xE2\x8C\x95" ) == 0x2315 );            // ⌕
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xE2\x86\x92" ) == 0x2192 );            // →
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xE2\x80\xA6" ) == 0x2026 );            // …

	// The FIRST offender is reported, not the last -- so a string with two
	// problems names the one a reader will hit first.
	REQUIRE( fonts::FirstUnbakedCodepoint( "a\xE2\x96\xB8\x62\xE2\x8C\x95" ) == 0x25B8 );

	// Reported from anywhere in the string, including the very end, which is
	// exactly where the spine's marker sat.
	REQUIRE( fonts::FirstUnbakedCodepoint( "trailing\xE2\x80\xBA" ) == 0x203A );

	// Four-byte sequences decode rather than being mistaken for malformed.
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xF0\x9F\x99\x82" ) == 0x1F642 );       // an emoji
}

TEST_CASE( "glyphs: malformed UTF-8 is reported, never skipped", "[overlay_ui]" )
{
	// A truncated or stray sequence is a bug in whatever produced the string.
	// Silently ignoring it is how a mojibake label survives review, so the
	// checker returns the replacement character rather than 0.
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xE2\x96" )     == 0xFFFD );   // truncated 3-byte
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xC2" )         == 0xFFFD );   // truncated 2-byte
	REQUIRE( fonts::FirstUnbakedCodepoint( "\x80" )         == 0xFFFD );   // stray continuation
	REQUIRE( fonts::FirstUnbakedCodepoint( "\xF8\x88\x80" ) == 0xFFFD );   // 5-byte lead
	REQUIRE( fonts::FirstUnbakedCodepoint( "ok\xE2\x96" )   == 0xFFFD );   // after valid text
}

TEST_CASE( "glyphs: tab and newline are not glyphs", "[overlay_ui]" )
{
	// Help text is authored with line breaks in it. Those never reach the
	// atlas, so treating them as unbaked would make every multi-line help
	// string a false positive and the sweep useless.
	REQUIRE( fonts::FirstUnbakedCodepoint( "line one\nline two" ) == 0 );
	REQUIRE( fonts::FirstUnbakedCodepoint( "a\tb\r\n" ) == 0 );

	// Other C0 controls are NOT excused -- they would draw a box like
	// anything else outside the range.
	REQUIRE( fonts::FirstUnbakedCodepoint( "bell\x07" ) == 0x07 );
}

// =========================================================================
//  The lane under an open drawer (D17)
// =========================================================================
// At 2.0x the ladder demotes the Inspector to a drawer, which OVERLAYS the
// sheet rather than taking width from it -- so the sheet stayed 804 base and
// the drawer painted over its entire control column. Every switch, segmented
// control and slider was behind it.
//
// The repair is one clamp at the top of ForColumn(), which is exactly the kind
// of arithmetic that rots silently: it is invisible at 1.0x, where no drawer
// exists, so nothing else in the suite would ever exercise it.
TEST_CASE( "lane: an open drawer pulls the lane's right edge in", "[overlay_ui]" )
{
	// The real 2.0x numbers. Slab 1728 px wide, rail 60 base, sheet 804 base;
	// the sheet's COLUMN is the sheet less two 24-base pads = 756. The drawer
	// is 400 base wide against the slab's right edge, so it covers the column
	// from base 380 onward -- 376 of the column's 756, once the column's own
	// right pad (which the drawer eats first) is taken off.
	constexpr float kCol      = 756.0f;
	constexpr float kOccluded = 376.0f;

	const ui::Lane open   = ui::Lane::ForColumn( kCol, kOccluded );
	const ui::Lane closed = ui::Lane::ForColumn( kCol );

	// Closed: the lane is the column, untouched. Passing no occlusion must be
	// bit-identical to the pre-D17 behaviour or every other test here is
	// asserting a different function than the shell calls.
	REQUIRE_THAT( closed.flWidth, WithinAbs( kCol, 1e-4f ) );

	// Open: the right edge is the drawer's left edge less one gutter.
	REQUIRE_THAT( open.flWidth, WithinAbs( kCol - kOccluded - ui::tok::kM, 1e-4f ) );
	REQUIRE_THAT( open.flWidth, WithinAbs( 368.0f, 1e-4f ) );

	// ...and THIS is the defect, stated as arithmetic: every rect the lane
	// hands out now ends left of where the drawer begins. Before D17 the
	// control zone ran to 728 with the drawer starting at 380.
	REQUIRE( open.flCtlMax <= kCol - kOccluded );
	REQUIRE( open.flAffMin <= kCol - kOccluded );
	REQUIRE( closed.flCtlMax > kCol - kOccluded );   // the bug, pinned as the old behaviour

	// The control zone survives at a usable width rather than collapsing.
	// 128 base still seats a switch (40), a stepper (44) and a slider.
	REQUIRE_THAT( open.CtlWidth(), WithinAbs( 128.0f, 1e-4f ) );
	REQUIRE( open.CtlWidth() > ui::tok::kSwitchW );
	REQUIRE( open.CtlWidth() > ui::tok::kStepperW );

	// The label column gives way too, and it has to: Lw at the full 756 is
	// 348, which alone exceeds the 368 the drawer leaves. Holding Lw and
	// pulling in only the control zone yields a control zone of zero.
	REQUIRE( open.flLw < closed.flLw );
	REQUIRE( open.LabelWidth() > 0.0f );

	// The columns still close on the REDUCED width -- the lane is internally
	// consistent under occlusion, not merely smaller.
	const float flSum = ui::tok::kRowPadLeft + open.LabelWidth() + ui::tok::kM
	                  + open.CtlWidth() + ui::tok::kAffordanceW;
	REQUIRE_THAT( flSum, WithinAbs( open.flWidth, 1e-4f ) );
}

TEST_CASE( "lane: occlusion is one code path, not a special case", "[overlay_ui]" )
{
	// An occluded lane must be *the same function* of its reduced width as an
	// ordinary lane is of its full one. If these ever diverge, the drawer case
	// has grown a second set of rules that the 1.0x path cannot catch.
	for ( float flOcc : { 40.0f, 200.0f, 376.0f, 500.0f } )
	{
		const ui::Lane occluded = ui::Lane::ForColumn( 756.0f, flOcc );
		const ui::Lane plain    = ui::Lane::ForColumn( 756.0f - flOcc - ui::tok::kM );
		INFO( "occlusion " << flOcc );

		REQUIRE_THAT( occluded.flWidth,    WithinAbs( plain.flWidth,    1e-4f ) );
		REQUIRE_THAT( occluded.flLw,       WithinAbs( plain.flLw,       1e-4f ) );
		REQUIRE_THAT( occluded.flCtlMin,   WithinAbs( plain.flCtlMin,   1e-4f ) );
		REQUIRE_THAT( occluded.flCtlMax,   WithinAbs( plain.flCtlMax,   1e-4f ) );
		REQUIRE_THAT( occluded.flAffMin,   WithinAbs( plain.flAffMin,   1e-4f ) );
	}

	// Zero and negative occlusion are the untouched column, not a 12-unit
	// shave -- the gutter belongs to the drawer, so with no drawer there is
	// no gutter.
	REQUIRE_THAT( ui::Lane::ForColumn( 756.0f,  0.0f ).flWidth, WithinAbs( 756.0f, 1e-4f ) );
	REQUIRE_THAT( ui::Lane::ForColumn( 756.0f, -9.0f ).flWidth, WithinAbs( 756.0f, 1e-4f ) );
}

TEST_CASE( "lane: an absurd occlusion degrades like an absurd width", "[overlay_ui]" )
{
	// The degenerate guard already in ForColumn() must cover the occluded
	// path too: a drawer wider than the column it floats over is a shell bug,
	// and it must produce a zero-width control zone rather than an inverted
	// rect that makes every Place() below it garbage.
	for ( float flOcc : { 700.0f, 756.0f, 2000.0f } )
	{
		const ui::Lane lane = ui::Lane::ForColumn( 756.0f, flOcc );
		INFO( "occlusion " << flOcc );
		REQUIRE( lane.flWidth >= 0.0f );
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

TEST_CASE( "band: the four clauses hold at every display scale", "[overlay_ui]" )
{
	// The anchor grid is the control under the most scrutiny in this design
	// (fix #3), and 2.0x is where a band would break first: the body is the
	// only part of a row whose size is a token rather than a measurement, so
	// a missing Px() would leave a 96x96 grid pinned at 96 physical pixels
	// inside a band that had doubled around it. Checked at every rung of the
	// responsive ladder rather than only at 1.0x, because at 1.0x a scale bug
	// is invisible by definition.
	for ( float flScale : { 0.5f, 1.0f, 1.25f, 2.0f } )
	{
		ScopedScale s( flScale );
		INFO( "display_scale " << flScale );

		const ui::Lane lane = ui::Lane::ForColumn( 804.0f );
		const ui::RowCtx plainRow = ui::RowCtx::ForRow( lane, 0.0f, 0.0f );
		const ui::BandLayout band = ui::LayOutBand( lane, 0.0f, 0.0f, ui::CompositeKind::Anchor );

		// Clause 1: exactly n x 44, IN PIXELS, so the clipper's uniform step
		// stays exact at this scale too.
		REQUIRE_THAT( band.rcBand.GetHeight(),
			WithinAbs( 3.0f * ui::Px( ui::tok::kRowH ), 1e-3f ) );

		// Clause 2: line 1 is still an ordinary row -- same height, same
		// control edge, same affordance column as a switch above it.
		REQUIRE_THAT( band.line1.Bounds().GetHeight(),
			WithinAbs( plainRow.Bounds().GetHeight(), 1e-4f ) );
		REQUIRE_THAT( band.line1.PlaceFull().Max.x,
			WithinAbs( plainRow.PlaceFull().Max.x, 1e-4f ) );

		// Clause 3: right-bound to the same vertical as every other control,
		// and the body scales with everything else rather than staying at its
		// base size.
		REQUIRE_THAT( band.rcBody.Max.x, WithinAbs( plainRow.PlaceFull().Max.x, 1e-4f ) );
		REQUIRE_THAT( band.rcBody.GetWidth(),  WithinAbs( ui::Px( 96.0f ), 1e-3f ) );
		REQUIRE_THAT( band.rcBody.GetHeight(), WithinAbs( ui::Px( 96.0f ), 1e-3f ) );

		// The body stays inside its band at every scale -- the 96 base body
		// against a 3 x 44 band has 36 base units of headroom, which is only
		// headroom if both sides scale together.
		REQUIRE( band.rcBody.Min.y >= band.rcBand.Min.y - 1e-3f );
		REQUIRE( band.rcBody.Max.y <= band.rcBand.Max.y + 1e-3f );

		// Clause 4 is structural: the body's left edge never reaches back
		// into the label column, so lines 2..n stay air.
		REQUIRE( band.rcBody.Min.x > band.line1.Bounds().Min.x );
	}
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

// ===========================================================================
//  P3 part A -- the read side a POPULATED registry needs
// ===========================================================================
// P2 had one real area, three rows, and none of the kinds below. P3 part A is
// the first phase that declares sliders, steppers, units, zero-words and group
// bands, so these pin the pieces the renderer now reads off a registration.

TEST_CASE( "param: the kind follows from the declaration, never from a name", "[overlay_ui]" )
{
	// Registry.cpp's AddParam(): "a param never names its own control".
	// Options present means Choice, a range means Slider, neither means
	// Switch. The point is that a caller states a FACT about the value and the
	// control follows -- so a param cannot be declared as one kind and bound
	// as another.
	ui::Registry reg;
	ui::Area &area = reg.Add( "image.shaders", "Shaders", ui::Section::Display );

	bool  bOn = false;
	float flStrength = 0.5f;
	int   nVariant = 0;

	ui::Entry &e = area.Switch( "image.shaders.vibrancy", "Vibrancy", ui::Bind( &bOn ) ).Help( "h" );

	const ui::Parameter &pSwitch = e.Param( "protect_skin", "Protect skin", ui::Bind( &bOn ) ).Help( "h" );
	const ui::Parameter &pSlider = e.Param( "strength", "Strength", ui::Bind( &flStrength ) )
		.Help( "h" ).Range( -1.0f, 1.0f ).Unit( "x" );

	static const ui::Option kOpts[] = { { 0, "a" }, { 1, "b" } };
	const ui::Parameter &pChoice = e.Param( "variant", "Variant", ui::Bind( &nVariant ), kOpts, 2 ).Help( "h" );

	REQUIRE( pSwitch.GetKind() == ui::Kind::Switch );
	REQUIRE( pSlider.GetKind() == ui::Kind::Slider );
	REQUIRE( pChoice.GetKind() == ui::Kind::Choice );

	// A Choice that is also given a range stays a Choice -- "a bounded set of
	// options" is still a Choice, not a slider over option indices.
	const ui::Parameter &pBoth = e.Param( "both", "Both", ui::Bind( &nVariant ), kOpts, 2 )
		.Help( "h" ).Range( 0.0f, 1.0f );
	REQUIRE( pBoth.GetKind() == ui::Kind::Choice );

	// The renderer reads the bounds and the unit off the declaration.
	REQUIRE( pSlider.HasRange() );
	REQUIRE( pSlider.Lo() == -1.0f );
	REQUIRE( pSlider.Hi() ==  1.0f );
	REQUIRE( pSlider.Unit() == "x" );

	// And a Slider uses the value column while a Choice does not -- the same
	// the-kind-decides-it rule an Entry obeys (SPEC §2.3).
	REQUIRE( pSlider.UsesValue() );
	REQUIRE_FALSE( pChoice.UsesValue() );

	ui::LawRecorder rec;
	REQUIRE( reg.SelfTest() == 0 );
}

TEST_CASE( "param: a param's disabled reason is its own, never its parent's", "[overlay_ui]" )
{
	// SPEC §3.13: "A parameter inherits its parent's reason, EXCEPT when it is
	// the cause of it -- otherwise turning Mute on would disable the Mute
	// switch, which was a real bug in the first version."
	//
	// That exception is why Parameter::DisabledReason() reads only the param's
	// OWN predicate and never walks to Owner(). The inherit half is the
	// renderer's job (it dims the whole block); the exception is structural,
	// and this is what pins it.
	ui::Registry reg;
	ui::Area &area = reg.Add( "audio.mixer", "Mixer", ui::Section::System );

	bool  bMuted  = true;
	float flVolume = -6.0f;

	ui::Entry &e = area.Slider( "audio.volume", "Volume", ui::Bind( &flVolume ) )
		.Help( "h" ).Range( -60.0f, 0.0f )
		.DisabledUnless( [ & ]{ return !bMuted; }, "the stream is muted" );

	const ui::Parameter &pMute = e.Param( "mute", "Mute", ui::Bind( &bMuted ) ).Help( "h" );

	// The parent is disabled BECAUSE the param is on...
	REQUIRE( e.DisabledReason() == "the stream is muted" );
	// ...and the param that caused it is still live, so it can be turned off.
	REQUIRE( pMute.DisabledReason().empty() );

	bMuted = false;
	REQUIRE( e.DisabledReason().empty() );

	ui::LawRecorder rec;
	REQUIRE( reg.SelfTest() == 0 );
}

TEST_CASE( "area: an entry remembers the group band open when it was declared", "[overlay_ui]" )
{
	// The sheet emits a band when the group index CHANGES, so the index has to
	// come from the entry rather than from a first-entry range comparison --
	// otherwise a band declared with no entries under it would still claim the
	// next one, and the sheet would draw a heading over rows that belong to a
	// different group.
	ui::Registry reg;
	ui::Area &area = reg.Add( "display.upscaling", "Upscaling", ui::Section::Display );

	bool b = false;
	area.Group( "Scaling filter" );
	area.Switch( "display.a", "A", ui::Bind( &b ) ).Help( "h" );
	area.Switch( "display.b", "B", ui::Bind( &b ) ).Help( "h" );
	area.Group( "Empty" );              // declared, never populated
	area.Group( "Presentation" );
	area.Switch( "display.c", "C", ui::Bind( &b ) ).Help( "h" );

	REQUIRE( area.Groups().size() == 3 );
	REQUIRE( area.EntryCount() == 3 );
	REQUIRE( area.GroupOf( 0 ) == 0 );
	REQUIRE( area.GroupOf( 1 ) == 0 );
	// The empty band (index 1) owns nothing, so nothing points at it and the
	// sheet never emits a heading with no rows under it.
	REQUIRE( area.GroupOf( 2 ) == 2 );
	REQUIRE( area.Groups()[ 2 ].sName == "Presentation" );

	ui::LawRecorder rec;
	REQUIRE( reg.SelfTest() == 0 );
}

TEST_CASE( "entry: zero-means and unit are declared, never baked into a value", "[overlay_ui]" )
{
	// index.html's stated rule: "units are declared with `u`, never baked into
	// the value string." The frame limiter is the row that needs both -- 0 is
	// the word "Unlimited", every other value is "<n> fps".
	ui::Registry reg;
	ui::Area &area = reg.Add( "display.frame_limiter", "Frame limiter", ui::Section::Display );

	int nFps = 0;
	ui::Entry &e = area.Stepper( "display.fps_limit", "FPS limit", ui::Bind( &nFps ) )
		.Help( "h" ).Range( 0.0f, 480.0f ).Step( 10.0f ).Unit( "fps" ).ZeroMeans( "Unlimited" );

	REQUIRE( e.GetKind() == ui::Kind::Stepper );
	REQUIRE( e.Unit() == "fps" );
	REQUIRE( e.ZeroWord() == "Unlimited" );

	// ISSUE #67's gap, expressed as arithmetic rather than as a special case.
	// The valid set is 0 OR [10, 480] -- 1-9 fps is a trap, because at that
	// rate the overlay itself repaints a few times a second and can no longer
	// practically be driven, including to undo the setting. With a step of 10
	// anchored at 0 the reachable set IS {0, 10, 20, ...}: the step is what
	// creates the hole, so there is no special case for anyone to maintain.
	REQUIRE( e.StepSize() == 10.0f );
	REQUIRE( e.Lo() == 0.0f );
	const int nStep = (int)e.StepSize();
	REQUIRE( 0 + nStep == 10 );          // up from unlimited lands on the floor
	REQUIRE( 10 - nStep == 0 );          // down from the floor lands on unlimited

	ui::LawRecorder rec;
	REQUIRE( reg.SelfTest() == 0 );
}

// =========================================================================
//  Dynamic areas (P3b) -- the registry's answer to a row set that changes
// =========================================================================
// Audio is the first area whose rows are not known when RegisterAll() runs:
// a row exists because an application is making a sound right now. These
// tests exist because that widens WHEN a law violation can fire -- from
// boot only, to any frame -- and the mitigation is that a dynamic row is
// GENERATED, so one test against a fabricated stream list covers every row
// the generator will ever emit. See Registry.h's Rebuilds().
namespace
{
	// Stands in for Audio::StreamCandidate. The point of the tests below is
	// the registry's contract, not PipeWire's -- a fake keeps them runnable
	// with no audio server, which is the only way they run in CI at all.
	struct FakeStream
	{
		int         nNodeId;
		std::string sName;
	};

	std::vector<FakeStream> g_vecFakeStreams;

	uint64_t FakeGeneration()
	{
		uint64_t ulHash = 1469598103934665603ull;
		for ( const FakeStream &s : g_vecFakeStreams )
		{
			for ( unsigned char c : ( std::to_string( s.nNodeId ) + s.sName ) )
			{
				ulHash ^= c;
				ulHash *= 1099511628211ull;
			}
		}
		return ulHash;
	}

	// The shape of Audio's real builder: one Slider per stream, id derived
	// from the NODE, one mute Param each.
	void BuildFakeArea( ui::Area &a )
	{
		a.Group( "Streams" );
		for ( const FakeStream &s : g_vecFakeStreams )
		{
			const std::string sId = "audio.node." + std::to_string( s.nNodeId );
			a.Slider( sId.c_str(), s.sName.c_str(), ui::AnyBind() )
				.Help( "Volume of this application's audio stream." )
				.Range( 0.0f, 150.0f )
				.Unit( "%" )
				.Param( "mute", "Mute", ui::AnyBind() )
					.Help( "Silences this stream without changing its volume." );
		}
	}
}

TEST_CASE( "dynamic: a rebuild tracks the row set as streams come and go", "[overlay_ui]" )
{
	ui::Registry reg;
	ui::LawRecorder rec;

	g_vecFakeStreams = { { 244, "eldenring.exe" }, { 334, "Floorp" } };
	ui::Area &a = reg.Add( "audio.mixer", "Mixer", ui::Section::System );
	a.Rebuilds( FakeGeneration, BuildFakeArea );

	REQUIRE( a.IsDynamic() );
	REQUIRE( a.EntryCount() == 0 );          // nothing is built until the first sync

	REQUIRE( reg.SyncDynamicAreas() == 1 );
	REQUIRE( a.EntryCount() == 2 );
	REQUIRE( reg.FindEntry( "audio.node.244" ) != nullptr );
	REQUIRE( reg.FindEntry( "audio.node.334" ) != nullptr );

	// An unchanged generation must NOT rebuild -- a rebuild under the
	// pointer would drop the row being dragged.
	REQUIRE( reg.SyncDynamicAreas() == 0 );
	REQUIRE( a.EntryCount() == 2 );

	// A stream ends.
	g_vecFakeStreams = { { 334, "Floorp" } };
	REQUIRE( reg.SyncDynamicAreas() == 1 );
	REQUIRE( a.EntryCount() == 1 );
	REQUIRE( reg.FindEntry( "audio.node.244" ) == nullptr );
	REQUIRE( reg.FindEntry( "audio.node.334" ) != nullptr );

	// A new one starts, and the SURVIVING row keeps its id -- this is the
	// property a positional slot pool would not have. 334 is still Floorp
	// after 512 appears, so a slider mid-drag still means Floorp.
	g_vecFakeStreams = { { 334, "Floorp" }, { 512, "mpv" } };
	REQUIRE( reg.SyncDynamicAreas() == 1 );
	REQUIRE( reg.FindEntry( "audio.node.334" ) != nullptr );
	REQUIRE( reg.FindEntry( "audio.node.512" ) != nullptr );

	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "dynamic: rebuilding the same ids does not trip id uniqueness", "[overlay_ui]" )
{
	// The law that a dynamic area is most likely to break, and the reason
	// SyncIfStale() releases its ids before the builder runs. Without that
	// release the SECOND build of the same stream would abort the process.
	ui::Registry reg;
	ui::LawRecorder rec;

	g_vecFakeStreams = { { 244, "eldenring.exe" } };
	ui::Area &a = reg.Add( "audio.mixer", "Mixer", ui::Section::System );
	a.Rebuilds( FakeGeneration, BuildFakeArea );
	reg.SyncDynamicAreas();

	// Same ids, different titles -- a rename, which is a real rebuild
	// (media.name changes when a browser tab does).
	g_vecFakeStreams = { { 244, "Elden Ring" } };
	REQUIRE( reg.SyncDynamicAreas() == 1 );
	REQUIRE( a.EntryCount() == 1 );
	REQUIRE( a.EntryAt( 0 ).Title() == "Elden Ring" );
	REQUIRE( a.EntryAt( 0 ).Id() == "audio.node.244" );

	// Params are released with their parent, or the second build's `mute`
	// would collide with the first's.
	REQUIRE( reg.FindParam( "audio.node.244.mute" ) != nullptr );
	REQUIRE( rec.Caught( ui::Law::UniqueId ) == false );
	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "dynamic: a row built after SelfTest is still held to the laws", "[overlay_ui]" )
{
	// The hole a dynamic area opens: Registry::SelfTest() runs once after
	// RegisterAll(), so a row built later has never been through it. If a
	// rebuild did not re-check, a generated row could ship with no Help()
	// at all -- and Help is the Inspector's only description.
	ui::Registry reg;
	ui::LawRecorder rec;

	g_vecFakeStreams = { { 244, "eldenring.exe" } };
	ui::Area &a = reg.Add( "audio.mixer", "Mixer", ui::Section::System );
	a.Rebuilds( FakeGeneration, []( ui::Area &area )
	{
		// Deliberately malformed: no Help().
		area.Slider( "audio.node.244", "eldenring.exe", ui::AnyBind() ).Range( 0.0f, 150.0f );
	} );

	REQUIRE( reg.SelfTest() == 0 );   // nothing built yet -- nothing to catch
	reg.SyncDynamicAreas();

	// The rebuild's own self-test caught it, mid-session, exactly as boot
	// would have.
	REQUIRE( rec.Caught( ui::Law::HelpRequired ) );
}

TEST_CASE( "dynamic: a rebuild needs both a generation and a builder", "[overlay_ui]" )
{
	// The escaped-or-dynamic half of this case went with Area::Escape() in
	// P5. What remains is the guard that is still reachable: a dynamic area
	// declared with only half of what a rebuild needs must not come out
	// dynamic, because SyncIfStale() would then have a generation to
	// compare and nothing to run.
	{
		ui::Registry reg;
		ui::LawRecorder rec;
		ui::Area &a = reg.Add( "audio.mixer", "Mixer", ui::Section::System );
		a.Rebuilds( FakeGeneration, nullptr );
		REQUIRE( rec.Caught( ui::Law::Dynamic ) );
		REQUIRE( !a.IsDynamic() );
	}
	{
		ui::Registry reg;
		ui::LawRecorder rec;
		ui::Area &a = reg.Add( "audio.mixer", "Mixer", ui::Section::System );
		a.Rebuilds( nullptr, BuildFakeArea );
		REQUIRE( rec.Caught( ui::Law::Dynamic ) );
		REQUIRE( !a.IsDynamic() );
	}
}

TEST_CASE( "dynamic: an empty stream set is a valid build, not a violation", "[overlay_ui]" )
{
	// Silence is the common case at startup, and it must not look like a
	// failure -- this is also why the help law needs its own test above:
	// with no streams the row-building code never runs at all.
	ui::Registry reg;
	ui::LawRecorder rec;

	g_vecFakeStreams.clear();
	ui::Area &a = reg.Add( "audio.mixer", "Mixer", ui::Section::System );
	a.Rebuilds( FakeGeneration, BuildFakeArea );
	reg.SyncDynamicAreas();

	REQUIRE( a.EntryCount() == 0 );
	REQUIRE( rec.Count() == 0 );
}

// =========================================================================
//  Destructive actions (P3b)
// =========================================================================
// The user's rule, after an agent wiped one of their configs: "There can be
// a button for it, but never delete configs automatically." Confirm() is
// that rule expressed in the declaration, so an action that destroys
// something is armed by construction rather than by a call site remembering
// to open a modal.
TEST_CASE( "confirm: a destructive action declares its own second press", "[overlay_ui]" )
{
	ui::Registry reg;
	ui::LawRecorder rec;
	ui::Area &a = reg.Add( "setup.pergame", "Per-game", ui::Section::Setup );

	int nInvocations = 0;
	a.Action( "config.delete", "Delete saved config", "delete...",
		[ &nInvocations ]{ ++nInvocations; } )
		.Confirm( "delete permanently?" )
		.Help( "Permanently deletes this game's saved config." );

	const ui::Entry *pEntry = reg.FindEntry( "config.delete" );
	REQUIRE( pEntry != nullptr );
	REQUIRE( pEntry->NeedsConfirm() );
	REQUIRE( pEntry->ConfirmPrompt() == "delete permanently?" );

	// The declaration alone invokes nothing -- registering a destructive
	// action must never perform it.
	REQUIRE( nInvocations == 0 );

	// And the binding is still reachable exactly once when it IS invoked,
	// so arming cannot double-fire.
	pEntry->Invoke();
	REQUIRE( nInvocations == 1 );

	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "confirm: an ordinary action does not ask", "[overlay_ui]" )
{
	// The complement, and the thing that keeps the prompt meaningful: if
	// every action asked, the second press would become reflex.
	ui::Registry reg;
	ui::LawRecorder rec;
	ui::Area &a = reg.Add( "setup.profiles", "Profiles", ui::Section::Setup );

	a.Action( "profiles.apply", "Apply profile", "apply", []{} )
		.Help( "Copies the selected profile's values in." );

	const ui::Entry *pEntry = reg.FindEntry( "profiles.apply" );
	REQUIRE( pEntry != nullptr );
	REQUIRE( !pEntry->NeedsConfirm() );
	REQUIRE( pEntry->ConfirmPrompt().empty() );
	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "badge: an area declares which config layer it writes to", "[overlay_ui]" )
{
	// Issue #43's question -- "where does what I change here get written?"
	// The awkward case is the one that made a per-area badge necessary:
	// Appearance writes global.json even when a per-game override is
	// active, which no amount of session state can express.
	ui::Registry reg;
	ui::LawRecorder rec;

	bool bOverrideActive = false;
	ui::Area &pergame = reg.Add( "setup.pergame", "Per-game", ui::Section::Setup );
	pergame.Badge( [ &bOverrideActive ]{
		return bOverrideActive ? std::string( "app 1174180" ) : std::string( "global" );
	} );

	ui::Area &appearance = reg.Add( "setup.appearance", "Appearance", ui::Section::Setup );
	appearance.Badge( []{ return std::string( "global only" ); } );

	REQUIRE( pergame.BadgeText() == "global" );
	bOverrideActive = true;
	REQUIRE( pergame.BadgeText() == "app 1174180" );

	// Unchanged by the override -- the whole point.
	REQUIRE( appearance.BadgeText() == "global only" );

	// An area that never declared one has none, rather than an empty box.
	ui::Area &shell = reg.Add( "setup.shell", "Shell", ui::Section::Setup );
	REQUIRE( shell.BadgeText().empty() );

	REQUIRE( rec.Count() == 0 );
}

// =========================================================================
//  Reset (P3b)
// =========================================================================
// D6 decided reset moves into the Inspector, but no phase had implemented
// it -- so the E2 shell could not reset anything, and migrating the Config
// panel would have silently dropped its four per-group reset links (#43).
// Reset covers the row AND its parameters, which is what makes it the
// successor to a GROUP link: the old "UI Scale" group is exactly the
// `UI scale` row plus its dock and notification params.
TEST_CASE( "reset: a row restores itself and its parameters together", "[overlay_ui]" )
{
	ui::Registry reg;
	ui::LawRecorder rec;
	ui::Area &a = reg.Add( "setup.appearance", "Appearance", ui::Section::Setup );

	float flScale = 1.0f, flDock = 1.0f, flNotif = 1.0f;
	a.Slider( "overlay.display_scale", "UI scale", ui::Bind( &flScale ) )
		.Help( "Multiplies every base unit in the overlay." )
		.Range( 0.5f, 2.0f )
		.Default( 1.0f )
		.Param( "dock", "Dock scale", ui::Bind( &flDock ) )
			.Help( "Size of the dock." ).Range( 0.85f, 2.0f ).Default( 1.0f )
		.Param( "notifications", "Notification scale", ui::Bind( &flNotif ) )
			.Help( "Size of toasts." ).Range( 0.6f, 1.6f ).Default( 1.0f );

	const ui::Entry *pEntry = reg.FindEntry( "overlay.display_scale" );
	REQUIRE( pEntry != nullptr );
	REQUIRE( pEntry->HasDefault() );
	REQUIRE( pEntry->IsAtDefault() );

	// A PARAMETER differing is enough to arm the row's reset -- otherwise a
	// group link would not restore what the old one did.
	flDock = 1.4f;
	REQUIRE( !pEntry->IsAtDefault() );

	pEntry->ResetToDefault();
	REQUIRE( flDock == 1.0f );
	REQUIRE( pEntry->IsAtDefault() );

	// And the row's own value, together with its params, in one action.
	flScale = 1.75f;
	flDock  = 1.4f;
	flNotif = 1.1f;
	REQUIRE( !pEntry->IsAtDefault() );
	pEntry->ResetToDefault();
	REQUIRE( flScale == 1.0f );
	REQUIRE( flDock  == 1.0f );
	REQUIRE( flNotif == 1.0f );

	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "reset: a composite's SECOND axis counts as part of the row", "[overlay_ui]" )
{
	// An anchor is one setting whose value is a PAIR. If "differs from
	// default" and "reset" read only the first binding, an anchor sitting at
	// the right column but the wrong row reports itself as unchanged -- and
	// the half that did move can never be reset. That is the state edge (D6)
	// lying about the sheet, so it is checked here rather than left to the
	// renderer.
	ui::Registry reg;
	ui::LawRecorder rec;
	ui::Area &a = reg.Add( "system.monitor", "Monitor", ui::Section::System );

	int nV = 0, nH = 2;
	a.Composite( "monitor.anchor", "Placement", ui::CompositeKind::Anchor,
			ui::Bind( &nV ), ui::Bind( &nH ) )
		.Default( 0, 2 )
		.Help( "Which screen corner the monitor is anchored to." );

	const ui::Entry *pEntry = reg.FindEntry( "monitor.anchor" );
	REQUIRE( pEntry != nullptr );
	REQUIRE( pEntry->HasDefault() );
	REQUIRE( pEntry->IsAtDefault() );

	// Axis A alone moving is enough.
	nV = 2;
	REQUIRE( !pEntry->IsAtDefault() );
	pEntry->ResetToDefault();
	REQUIRE( nV == 0 );
	REQUIRE( nH == 2 );

	// Axis B alone moving is equally enough -- this is the half that a
	// single-binding implementation silently drops.
	nH = 0;
	REQUIRE( !pEntry->IsAtDefault() );
	pEntry->ResetToDefault();
	REQUIRE( nV == 0 );
	REQUIRE( nH == 2 );

	// And both at once, in one action.
	nV = 1; nH = 1;
	REQUIRE( !pEntry->IsAtDefault() );
	pEntry->ResetToDefault();
	REQUIRE( nV == 0 );
	REQUIRE( nH == 2 );

	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "reset: a row with no declared default offers nothing to reset to", "[overlay_ui]" )
{
	// The alternative -- treating "no default" as zero -- would let a reset
	// link destroy a value it was never told how to restore.
	ui::Registry reg;
	ui::LawRecorder rec;
	ui::Area &a = reg.Add( "setup.profiles", "Profiles", ui::Section::Setup );

	std::string sName = "Handheld 40 fps";
	a.Text( "profiles.name", "Name", ui::Bind( &sName ) )
		.Help( "Name for a new profile." );

	const ui::Entry *pEntry = reg.FindEntry( "profiles.name" );
	REQUIRE( pEntry != nullptr );
	REQUIRE( !pEntry->HasDefault() );

	// Resetting is a no-op rather than a clear.
	pEntry->ResetToDefault();
	REQUIRE( sName == "Handheld 40 fps" );
	REQUIRE( rec.Count() == 0 );
}

TEST_CASE( "reset: a float default survives a round-trip comparison", "[overlay_ui]" )
{
	// A value that has been through JSON and back is the same SETTING to a
	// user. Exact float equality here would leave the reset link lit
	// forever on a config that was merely saved and reloaded.
	ui::Registry reg;
	ui::LawRecorder rec;
	ui::Area &a = reg.Add( "setup.appearance", "Appearance", ui::Section::Setup );

	float flBlur = 1.0f;
	a.Slider( "overlay.background_blur", "Backdrop blur", ui::Bind( &flBlur ) )
		.Help( "How much the game behind the overlay is blurred." )
		.Range( 0.0f, 1.0f )
		.Default( 0.9f );

	const ui::Entry *pEntry = reg.FindEntry( "overlay.background_blur" );
	flBlur = 0.89999998f;                 // what 0.9 comes back as
	REQUIRE( pEntry->IsAtDefault() );

	flBlur = 0.8f;                        // a real difference still registers
	REQUIRE( !pEntry->IsAtDefault() );

	REQUIRE( rec.Count() == 0 );
}

// =========================================================================
//  SPEC §8.0 -- the rail icon set
// =========================================================================
// These pin the PROPERTY the icons exist for, not their appearance.
//
// The rail drew the area title's initial until D20, and the pre-P5 shell test
// found that unusable in the collapsed rail: three pairs shared a letter
// (Mixer/Monitor, Profiles/Per-game, Shaders/Shell), so three of eleven areas
// were unidentifiable at 1.5x and above -- the exact state in which the mark
// carries the item's whole meaning.
//
// "No two are confusable" is finally a question about pixels, and it is
// answered by the screenshots the change was verified with. What a unit test
// CAN hold is the layer underneath that: every area has a glyph, no two
// glyphs are the same drawing, and every glyph stays inside the 24-unit box
// it is specified on. A set that fails any of those cannot be legible no
// matter how it is drawn.
TEST_CASE( "icons: every registered area has one, and no two are the same drawing", "[overlay_ui]" )
{
	// The twelve this build registers. Written out rather than walked off
	// the live registry because the areas are declared in the panel files,
	// which this test binary deliberately does not link -- so the list is
	// the test's own statement of what the rail must be able to draw.
	// display.general is the twelfth, added by the user's direct correction
	// to D13.1 (2026-08-24) -- see PanelDisplay.cpp's RegisterGeneral().
	const char *pszAreas[] = {
		"display.general", "display.upscaling", "display.frame_limiter", "display.hdr",
		"image.shaders", "audio.mixer", "system.monitor", "system.log",
		"setup.profiles", "setup.pergame", "setup.appearance", "setup.shell",
	};
	const size_t nAreas = sizeof( pszAreas ) / sizeof( pszAreas[ 0 ] );

	REQUIRE( ui::IconCount() == nAreas );

	for ( size_t i = 0; i < nAreas; ++i )
	{
		INFO( "area " << pszAreas[ i ] );
		REQUIRE( ui::IconFor( pszAreas[ i ] ) != nullptr );
	}

	// An id with no glyph answers nullptr rather than a wrong glyph -- the
	// rail's fallback depends on being able to tell the difference.
	REQUIRE( ui::IconFor( "setup.nonexistent" ) == nullptr );
	REQUIRE( ui::IconFor( nullptr ) == nullptr );

	// THE ANTI-COLLISION ASSERTION. Two areas sharing a drawing is the
	// letters bug again in another alphabet, so it is the one property
	// worth failing the build over.
	for ( size_t i = 0; i < ui::IconCount(); ++i )
	{
		for ( size_t j = i + 1; j < ui::IconCount(); ++j )
		{
			const ui::Icon &a = ui::IconSet()[ i ];
			const ui::Icon &b = ui::IconSet()[ j ];
			INFO( a.pszKey << " vs " << b.pszKey );

			bool bIdentical = ( a.nShapes == b.nShapes );
			for ( size_t s = 0; bIdentical && s < a.nShapes; ++s )
			{
				const ui::IconShape &x = a.shapes[ s ];
				const ui::IconShape &y = b.shapes[ s ];
				if ( x.eOp != y.eOp || x.nPoints != y.nPoints || x.flRadius != y.flRadius )
					bIdentical = false;
				for ( size_t p = 0; bIdentical && p < x.nPoints; ++p )
					if ( x.pts[ p ].x != y.pts[ p ].x || x.pts[ p ].y != y.pts[ p ].y )
						bIdentical = false;
			}
			REQUIRE( !bIdentical );
		}
	}
}

TEST_CASE( "icons: every glyph stays inside SPEC 8.0's 24-unit grid", "[overlay_ui]" )
{
	// A coordinate outside the box is not a style problem: the rail centres
	// the 24-unit box on the item, so an outlier is drawn over the item's
	// neighbour or clipped away by the rail's edge. Circles and teardrops
	// are checked at their extents, not just their centres, because that is
	// where they would escape.
	for ( size_t i = 0; i < ui::IconCount(); ++i )
	{
		const ui::Icon &icon = ui::IconSet()[ i ];
		INFO( "icon " << icon.pszKey );

		REQUIRE( icon.nShapes >= 1 );
		REQUIRE( icon.nShapes <= ui::kIconMaxShapes );

		for ( size_t s = 0; s < icon.nShapes; ++s )
		{
			const ui::IconShape &sh = icon.shapes[ s ];
			INFO( "shape " << s );

			// An op that needs a radius must have one; an op that needs
			// points must have enough of them to be a path.
			switch ( sh.eOp )
			{
				case ui::IconOp::Circle:
				case ui::IconOp::HalfDisc:
					REQUIRE( sh.flRadius > 0.0f );
					REQUIRE( sh.nPoints == 1 );
					break;
				case ui::IconOp::Teardrop:
					REQUIRE( sh.flRadius > 0.0f );
					REQUIRE( sh.nPoints == 2 );
					break;
				case ui::IconOp::FillRect:
					REQUIRE( sh.nPoints == 2 );
					break;
				default:
					REQUIRE( sh.nPoints >= 2 );
					break;
			}
			REQUIRE( sh.nPoints <= ui::kIconMaxPts );

			for ( size_t p = 0; p < sh.nPoints; ++p )
			{
				// The radius is added on both axes for the round ops, so
				// a circle tangent to the box passes and one hanging out
				// of it does not. A teardrop's apex (point 0) is a plain
				// point -- only its centre carries the radius.
				const float r = ( sh.eOp == ui::IconOp::Circle ||
				                  sh.eOp == ui::IconOp::HalfDisc ||
				                  sh.eOp == ui::IconOp::Teardrop ) ? sh.flRadius : 0.0f;
				const float rr = ( sh.eOp == ui::IconOp::Teardrop && p == 0 ) ? 0.0f : r;

				REQUIRE( sh.pts[ p ].x - rr >= 0.0f );
				REQUIRE( sh.pts[ p ].y - rr >= 0.0f );
				REQUIRE( sh.pts[ p ].x + rr <= ui::kIconGrid );
				REQUIRE( sh.pts[ p ].y + rr <= ui::kIconGrid );
			}
		}
	}
}
