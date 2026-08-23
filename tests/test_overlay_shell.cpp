// Unit tests for the E2 SHELL's pure half -- the slab, SPEC §8.3's responsive
// ladder, the three regions' rect arithmetic, and the Inspector's mode
// selection.
//
// WHY THESE ARE TESTABLE AT ALL. P1 kept the kit's arithmetic free of
// imgui.h, and P2 kept the same line: Layout.h decides every region's size,
// which host the Inspector gets and which mode a selection opens in, and it
// includes nothing but Registry.h. Shell.cpp turns those answers into child
// windows and does not compute one of them. So the parts of the shell that
// can actually be WRONG -- a ladder step, a boundary that does not close, a
// mode picked for the wrong kind -- are all reachable without a window, a
// renderer or a font atlas.
//
// SPEC §8.3 prints a full worked table across display_scale 0.5x .. 2.0x and
// says of it: "Two things this table is meant to prove". The first test below
// is that table, transcribed, and it is the load-bearing one -- if the ladder
// ever stops reproducing it, the design's own argument about 2.0x stops being
// true.
//
// Source of truth for every number asserted here:
//   superdoc/planning/redesign/round-2/e2-inspector-plus/SPEC.md  (§5.1, §8.1, §8.3)
//   superdoc/planning/redesign/round-2/e2-inspector-plus/index.html  (ladder())
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Overlay/UI/Lane.h"
#include "Overlay/UI/Layout.h"
#include "Overlay/UI/Registry.h"
#include "Overlay/UI/Tokens.h"

#include <algorithm>

using namespace gamescope;
using Catch::Matchers::WithinAbs;

namespace
{
	// The surface SPEC §8.3's table is worked on.
	constexpr float kSurfW = 1920.0f;
	constexpr float kSurfH = 1080.0f;

	ui::LadderResult LadderAt( float flScale, int nRows = 0,
	                           ui::InspectorHost ePref = ui::InspectorHost::Column )
	{
		const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, flScale );
		return ui::Solve( slab, ePref, nRows );
	}
}

// =========================================================================
//  SPEC §8.3's table, row by row
// =========================================================================
TEST_CASE( "the responsive ladder reproduces SPEC 8.3's worked table", "[overlay_shell]" )
{
	struct Row
	{
		float flScale;
		float flSlabPx;
		float flSlabBase;
		float flRail;
		ui::InspectorHost eHost;
		float flSheetBase;
		int   nStep;
	};

	// Transcribed from SPEC §8.3. The "Inspector" column of the table reads
	// "400" or "drawer 400" -- the WIDTH never changes, only the host, which
	// is exactly the point (Layout.cpp keeps one rect for both).
	const Row kTable[] = {
		{ 0.50f, 1180.0f, 2360.0f, 232.0f, ui::InspectorHost::Column, 1728.0f, -1 },
		{ 0.75f, 1180.0f, 1573.0f, 232.0f, ui::InspectorHost::Column,  941.0f,  0 },
		{ 1.00f, 1560.0f, 1560.0f, 232.0f, ui::InspectorHost::Column,  928.0f,  0 },
		{ 1.25f, 1728.0f, 1382.0f, 232.0f, ui::InspectorHost::Column,  750.0f,  0 },
		{ 1.50f, 1728.0f, 1152.0f,  60.0f, ui::InspectorHost::Column,  692.0f,  1 },
		{ 1.75f, 1728.0f,  988.0f,  60.0f, ui::InspectorHost::Drawer,  928.0f,  2 },
		{ 2.00f, 1728.0f,  864.0f,  60.0f, ui::InspectorHost::Drawer,  804.0f,  2 },
	};

	for ( const Row &row : kTable )
	{
		INFO( "display_scale " << row.flScale );
		const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, row.flScale );
		REQUIRE_THAT( slab.flWidthPx,   WithinAbs( row.flSlabPx,   0.5f ) );
		REQUIRE_THAT( slab.flWidthBase, WithinAbs( row.flSlabBase, 1.0f ) );

		// The area is given enough rows that the CONTENT cap never bites --
		// the table is a statement about width alone.
		const ui::LadderResult L = ui::Solve( slab, ui::InspectorHost::Column, 99 );
		REQUIRE_THAT( L.flRailBase,  WithinAbs( row.flRail, 0.5f ) );
		REQUIRE( L.eHost == row.eHost );
		REQUIRE_THAT( L.flSheetBase, WithinAbs( row.flSheetBase, 1.0f ) );
		REQUIRE( L.nStep == row.nStep );
	}
}

TEST_CASE( "2.0x leaves the sheet more room than 1.25x does", "[overlay_shell]" )
{
	// SPEC §8.3's own headline claim, and the one most likely to be broken
	// by a plausible-looking "simplification" of the ladder: collapsing the
	// rail and floating the Inspector returns more space than the scale-up
	// consumes, so the most width-hungry setting has the widest content
	// column of the two.
	REQUIRE( LadderAt( 2.00f, 99 ).flSheetBase > LadderAt( 1.25f, 99 ).flSheetBase );
}

TEST_CASE( "the ladder's one comparison is applied in order", "[overlay_shell]" )
{
	// The rail collapses BEFORE the Inspector floats -- never the other way
	// round. 1.5x is the scale that separates the two: it is cramped enough
	// to lose the rail's labels and roomy enough to keep all three regions.
	const ui::LadderResult L = LadderAt( 1.50f, 99 );
	REQUIRE( L.RailIsIcons() );
	REQUIRE( L.eHost == ui::InspectorHost::Column );
}

// =========================================================================
//  Host preference vs. the ladder
// =========================================================================
TEST_CASE( "the ladder overrides a host preference downward but never upward", "[overlay_shell]" )
{
	SECTION( "asking for a column on a slab that cannot seat one yields a drawer" )
	{
		REQUIRE( LadderAt( 2.00f, 99, ui::InspectorHost::Column ).eHost == ui::InspectorHost::Drawer );
	}
	SECTION( "asking for a drawer on a roomy slab is honoured" )
	{
		REQUIRE( LadderAt( 1.00f, 99, ui::InspectorHost::Drawer ).eHost == ui::InspectorHost::Drawer );
	}
	SECTION( "hidden is honoured at every scale -- step 3 is reachable by choice" )
	{
		// SPEC §8.3: "Step 3 is deliberately reachable by choice and
		// persisted, not only by width. That is what keeps the
		// Reachability Law exercised daily."
		for ( float s : { 0.50f, 1.00f, 1.50f, 2.00f } )
		{
			INFO( "display_scale " << s );
			const ui::LadderResult L = LadderAt( s, 99, ui::InspectorHost::Hidden );
			REQUIRE( L.eHost == ui::InspectorHost::Hidden );
			REQUIRE( L.nStep == 3 );
		}
	}
}

TEST_CASE( "a hidden Inspector costs the sheet exactly the spine", "[overlay_shell]" )
{
	// SPEC §8.05: the spine "holds its own width, so the sheet lane shrinks
	// by 20 rather than the sheet being overlapped by an invisible hit
	// strip". A drawer, by contrast, costs the sheet nothing.
	const float flColumn = LadderAt( 1.00f, 99, ui::InspectorHost::Column ).flSheetBase;
	const float flHidden = LadderAt( 1.00f, 99, ui::InspectorHost::Hidden ).flSheetBase;
	const float flDrawer = LadderAt( 1.00f, 99, ui::InspectorHost::Drawer ).flSheetBase;

	REQUIRE_THAT( flHidden - flColumn,
		WithinAbs( ui::shelltok::kInspector - ui::shelltok::kSpine, 0.5f ) );
	REQUIRE_THAT( flDrawer - flColumn, WithinAbs( ui::shelltok::kInspector, 0.5f ) );
}

// =========================================================================
//  The content cap on columns
// =========================================================================
TEST_CASE( "columns are capped by content, not only by width", "[overlay_shell]" )
{
	// SPEC §8.3: "columns = min( widthAllows, ceil( rows / 12 ) )" -- "a
	// 7-row category never spreads into two columns just because it fits".
	// 0.5x is the widest sheet in the table (1728 base, three columns by
	// width), so it is where the cap is easiest to observe.
	REQUIRE( LadderAt( 0.50f, 99 ).nWidthColumns == 3 );

	REQUIRE( LadderAt( 0.50f,  7 ).nColumns == 1 );
	REQUIRE( LadderAt( 0.50f, 12 ).nColumns == 1 );
	REQUIRE( LadderAt( 0.50f, 13 ).nColumns == 2 );
	REQUIRE( LadderAt( 0.50f, 24 ).nColumns == 2 );
	REQUIRE( LadderAt( 0.50f, 25 ).nColumns == 3 );
	REQUIRE( LadderAt( 0.50f, 99 ).nColumns == 3 );

	// An empty area -- every P2 escaped panel -- is one column, never zero.
	REQUIRE( LadderAt( 1.00f, 0 ).nColumns == 1 );

	// And the width still wins when it is the smaller of the two.
	REQUIRE( LadderAt( 1.25f, 99 ).nWidthColumns == 1 );
	REQUIRE( LadderAt( 1.25f, 99 ).nColumns == 1 );
}

// =========================================================================
//  The slab
// =========================================================================
TEST_CASE( "the slab never exceeds the surface it sits on", "[overlay_shell]" )
{
	// The 1180 floor inside SPEC §8.1's max() can exceed a genuinely small
	// surface. A slab hanging off the screen edge is worse than a cramped
	// one, and it cannot be dragged back -- there is no dragging.
	const ui::Slab slab = ui::Slab::For( 800.0f, 600.0f, 1.0f );
	REQUIRE( slab.flWidthPx  <= 800.0f );
	REQUIRE( slab.flHeightPx <= 600.0f );
}

TEST_CASE( "the slab is a pure function of surface and scale", "[overlay_shell]" )
{
	// There is no stored geometry anywhere in the shell, so the same inputs
	// must give the same slab forever. This is the test that would fail the
	// day someone reintroduces a remembered size.
	const ui::Slab a = ui::Slab::For( kSurfW, kSurfH, 1.0f );
	const ui::Slab b = ui::Slab::For( kSurfW, kSurfH, 1.0f );
	REQUIRE( a.flWidthPx  == b.flWidthPx );
	REQUIRE( a.flHeightPx == b.flHeightPx );
}

// =========================================================================
//  Region rect arithmetic
// =========================================================================
TEST_CASE( "the three regions tile the slab exactly, at every step", "[overlay_shell]" )
{
	for ( float s : { 0.50f, 1.00f, 1.50f, 2.00f } )
	{
		for ( ui::InspectorHost ePref : { ui::InspectorHost::Column,
		                                  ui::InspectorHost::Drawer,
		                                  ui::InspectorHost::Hidden } )
		{
			INFO( "display_scale " << s << " host " << (int)ePref );
			const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, s );
			const ui::LadderResult L = ui::Solve( slab, ePref, 99 );
			const ui::Regions R = ui::Regions::For( slab, L );

			// The bar and the body partition the slab's full height.
			REQUIRE_THAT( R.rcSlabBar.y1, WithinAbs( R.rcBody.y0, 0.01f ) );
			REQUIRE_THAT( R.rcBody.y1,    WithinAbs( slab.flHeightPx, 0.01f ) );

			// The rail's right edge IS the sheet's left edge. One boundary,
			// not two numbers that happen to agree.
			REQUIRE_THAT( R.rcRail.x1, WithinAbs( R.rcSheet.x0, 0.01f ) );

			// The sheet's own three bands partition its height, in order,
			// with no gap and no overlap.
			REQUIRE_THAT( R.rcSheetHead.y1, WithinAbs( R.rcSheetBody.y0, 0.01f ) );
			REQUIRE_THAT( R.rcSheetBody.y1, WithinAbs( R.rcSheetFoot.y0, 0.01f ) );
			REQUIRE_THAT( R.rcSheetFoot.y1, WithinAbs( R.rcSheet.y1, 0.01f ) );

			// Nothing is ever inside-out.
			REQUIRE( R.rcSheet.x1 >= R.rcSheet.x0 );
			REQUIRE( R.rcRail.Width() >= 0.0f );
		}
	}
}

TEST_CASE( "a column Inspector takes width from the sheet and a drawer does not", "[overlay_shell]" )
{
	const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, 1.0f );

	const ui::LadderResult Lc = ui::Solve( slab, ui::InspectorHost::Column, 99 );
	const ui::Regions Rc = ui::Regions::For( slab, Lc );
	// Column: the sheet stops where the Inspector starts. The three regions
	// are edge-to-edge across the whole slab.
	REQUIRE_THAT( Rc.rcSheet.x1, WithinAbs( Rc.rcInspector.x0, 0.01f ) );
	REQUIRE_THAT( Rc.rcInspector.x1, WithinAbs( slab.flWidthPx, 0.01f ) );

	const ui::LadderResult Ld = ui::Solve( slab, ui::InspectorHost::Drawer, 99 );
	const ui::Regions Rd = ui::Regions::For( slab, Ld );
	// Drawer: the sheet runs the full remaining width and the Inspector
	// OVERLAPS it. That overlap is the whole difference between the hosts,
	// and it is why a host change is a 160 ms fade and not a relayout.
	REQUIRE_THAT( Rd.rcSheet.x1, WithinAbs( slab.flWidthPx, 0.01f ) );
	REQUIRE( Rd.rcInspector.x0 < Rd.rcSheet.x1 );
	REQUIRE_THAT( Rd.rcInspector.Width(), WithinAbs( Rc.rcInspector.Width(), 0.01f ) );
}

TEST_CASE( "the hidden Inspector leaves a spine and nothing else", "[overlay_shell]" )
{
	const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, 1.0f );
	const ui::LadderResult L = ui::Solve( slab, ui::InspectorHost::Hidden, 99 );
	const ui::Regions R = ui::Regions::For( slab, L );

	REQUIRE( R.rcInspector.Empty() );
	REQUIRE( R.rcModeStrip.Empty() );
	REQUIRE( !R.rcSpine.Empty() );

	// SPEC §8.05's 20 base units, scaled -- and carved OUT of the sheet
	// rather than laid over it, so a click on the spine cannot also be a
	// click on a row.
	REQUIRE_THAT( R.rcSpine.Width(), WithinAbs( ui::shelltok::kSpine * slab.flScale, 0.01f ) );
	REQUIRE_THAT( R.rcSheet.x1, WithinAbs( R.rcSpine.x0, 0.01f ) );
	REQUIRE_THAT( R.rcSpine.x1, WithinAbs( slab.flWidthPx, 0.01f ) );
}

TEST_CASE( "region rects scale with display_scale and nothing else", "[overlay_shell]" )
{
	// The slab bar is 40 base units at every scale. A region whose chrome
	// stopped scaling would still LOOK plausible in a screenshot at 1.0x,
	// which is exactly why it is asserted here rather than eyeballed.
	for ( float s : { 0.50f, 1.00f, 2.00f } )
	{
		INFO( "display_scale " << s );
		const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, s );
		const ui::Regions R = ui::Regions::For( slab, ui::Solve( slab, ui::InspectorHost::Column, 99 ) );
		REQUIRE_THAT( R.rcSlabBar.Height(), WithinAbs( ui::shelltok::kSlabBar * s, 0.01f ) );
		REQUIRE_THAT( R.rcSheetHead.Height(), WithinAbs( ui::shelltok::kSheetHead * s, 0.01f ) );
		REQUIRE_THAT( R.rcSheetFoot.Height(), WithinAbs( ui::shelltok::kSheetFoot * s, 0.01f ) );
	}
}

// =========================================================================
//  Inspector mode selection (SPEC §5.1)
// =========================================================================
TEST_CASE( "mode selection is automatic and follows the kind", "[overlay_shell]" )
{
	// "Selecting a Facts, Meter or Graph row opens Details; everything else
	// -- including arriving from the palette on a parameter -- opens
	// Configure." Stateless: there is no argument here for "what the user
	// last chose", by construction.
	REQUIRE( ui::ModeFor( ui::Kind::Facts, ui::CompositeKind::Anchor ) == ui::InspectorMode::Details );
	REQUIRE( ui::ModeFor( ui::Kind::Meter, ui::CompositeKind::Anchor ) == ui::InspectorMode::Details );
	REQUIRE( ui::ModeFor( ui::Kind::Composite, ui::CompositeKind::Graph ) == ui::InspectorMode::Details );

	REQUIRE( ui::ModeFor( ui::Kind::Switch,  ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );
	REQUIRE( ui::ModeFor( ui::Kind::Slider,  ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );
	REQUIRE( ui::ModeFor( ui::Kind::Choice,  ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );
	REQUIRE( ui::ModeFor( ui::Kind::Text,    ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );
	REQUIRE( ui::ModeFor( ui::Kind::Bank,    ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );
	REQUIRE( ui::ModeFor( ui::Kind::Action,  ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );
	REQUIRE( ui::ModeFor( ui::Kind::Stepper, ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );

	// A non-Graph composite is writable, so it configures. The distinction
	// matters: Registry.h's IsReadOnly() would answer differently, and using
	// it here would silently send every Anchor to Details.
	REQUIRE( ui::ModeFor( ui::Kind::Composite, ui::CompositeKind::Anchor ) == ui::InspectorMode::Configure );
	REQUIRE( ui::ModeFor( ui::Kind::Composite, ui::CompositeKind::Hue ) == ui::InspectorMode::Configure );
}

// =========================================================================
//  The mode strip's counters (SPEC §5.1)
// =========================================================================
TEST_CASE( "the mode strip counts what the registration actually holds", "[overlay_shell]" )
{
	// SPEC §5.1: the cells "are not tabs the designer fills; they are a
	// readout of what depth this selection actually has". So the counts are
	// derived, and a row with no depth must not be able to advertise any.
	bool bA = false, bB = false, bC = false;

	ui::Registry reg;
	ui::LawRecorder rec;
	ui::Area &area = reg.Add( "t", "T", ui::Section::Display );

	ui::Entry &plain = area.Switch( "t.plain", "Plain", ui::Bind( &bA ) );
	plain.Help( "h" ).Default( false );

	ui::Entry &deep = area.Switch( "t.deep", "Deep", ui::Bind( &bB ) );
	deep.Help( "h" ).Default( false )
		.Param( "one", "One", ui::Bind( &bC ) ).Help( "h" );

	ui::Entry &facts = area.Facts( "t.facts", "Facts", []{ return std::string( "x" ); } );
	facts.Help( "h" ).Live( "l", []{ return ui::Fact{ "a", "b" }; } );

	REQUIRE( reg.SelfTest() == 0 );

	SECTION( "Configure counts the row's own control plus its params" )
	{
		REQUIRE( ui::CountsFor( plain ).nConfigure == 1 );
		REQUIRE( ui::CountsFor( deep ).nConfigure == 2 );
	}
	SECTION( "a read-only row's Configure cell is marked ro and counts nothing" )
	{
		const ui::ModeCounts c = ui::CountsFor( facts );
		REQUIRE( c.bReadOnly );
		REQUIRE( c.nConfigure == 0 );
	}
	SECTION( "Details counts only the grid rows that actually have a value" )
	{
		// `now`, `kind` and `key` are always present; `default` only when
		// one was declared; `range`/`options` only for a kind that has
		// them. A counter that always read 9 would be decoration.
		REQUIRE( ui::CountsFor( plain ).nDetails == 4 );          // now, default, kind, key
		REQUIRE( ui::CountsFor( facts ).nDetails == 3 + 1 );      // now, kind, key + one .Live()
	}
	SECTION( "adding a param moves Configure's counter and leaves Details' alone" )
	{
		REQUIRE( ui::CountsFor( deep ).nConfigure > ui::CountsFor( plain ).nConfigure );
		REQUIRE( ui::CountsFor( deep ).nDetails == ui::CountsFor( plain ).nDetails );
	}
}

// =========================================================================
//  The migration seam's one law
// =========================================================================
TEST_CASE( "an area is legacy or E2, never both", "[overlay_shell]" )
{
	// Area::Escape() is P2's temporary hatch for hosting a legacy panel
	// body in the sheet. The state that would let it survive P3 forever is
	// the HALF-migrated area -- a few real rows plus an escaped tail, which
	// mostly works and therefore never gets finished. So the mixture is a
	// violation in both directions.
	SECTION( "escaping a populated area fires" )
	{
		bool b = false;
		ui::Registry reg;
		ui::LawRecorder rec;
		ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
		area.Switch( "a.s", "S", ui::Bind( &b ) ).Help( "h" ).Default( false );
		area.Escape( []{} );
		REQUIRE( rec.Caught( ui::Law::Escaped ) );
		REQUIRE( !area.IsEscaped() );
	}
	SECTION( "populating an escaped area fires" )
	{
		bool b = false;
		ui::Registry reg;
		ui::LawRecorder rec;
		ui::Area &area = reg.Add( "a", "A", ui::Section::Display );
		area.Escape( []{} );
		area.Switch( "a.s", "S", ui::Bind( &b ) ).Help( "h" ).Default( false );
		REQUIRE( rec.Caught( ui::Law::Escaped ) );
		REQUIRE( area.EntryCount() == 0 );
	}
	SECTION( "an empty escape function fires" )
	{
		ui::Registry reg;
		ui::LawRecorder rec;
		reg.Add( "a", "A", ui::Section::Display ).Escape( nullptr );
		REQUIRE( rec.Caught( ui::Law::Escaped ) );
	}
	SECTION( "EscapeCount is what a lint would report as migration debt" )
	{
		ui::Registry reg;
		ui::LawRecorder rec;
		reg.Add( "a", "A", ui::Section::Display ).Escape( []{} );
		reg.Add( "b", "B", ui::Section::System ).Escape( []{} );
		reg.Add( "c", "C", ui::Section::Setup );
		REQUIRE( reg.EscapeCount() == 2 );
		REQUIRE( reg.SelfTest() == 0 );   // an escaped area needs no Help()
	}
}

TEST_CASE( "an escaped area is found by id like any other", "[overlay_shell]" )
{
	ui::Registry reg;
	ui::LawRecorder rec;
	reg.Add( "display.gamescope", "Gamescope", ui::Section::Display ).Escape( []{} );
	const ui::Area *pArea = reg.FindArea( "display.gamescope" );
	REQUIRE( pArea != nullptr );
	REQUIRE( pArea->IsEscaped() );
	REQUIRE( pArea->EntryCount() == 0 );
	REQUIRE( reg.FindArea( "nope" ) == nullptr );
}

// =========================================================================
//  The Inspector's scroll range (P3b)
// =========================================================================
// P3a shipped an Inspector whose CONFIGURE body could not scroll, and the
// symptom was invisible to every test that existed: the body drew with an
// absolute y onto the draw list, so content taller than the region simply
// stopped at the region's edge. Nothing failed, no assertion fired, the
// last row was just missing.
//
// These tests make that condition arithmetic. ConfigureRowsHeight() is the
// body's fixed-height part; Regions::rcInspectorBody is the space it has.
// Comparing the two is the check the screenshot was doing by eye.
TEST_CASE( "an entry at the six-param budget overflows the drawer at 2.0x", "[overlay_shell]" )
{
	// D13.4's case, and the one that found the bug: adaptive brightness
	// sits on exactly six params, the Six Budget's ceiling.
	const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, 2.0f );
	const ui::LadderResult ladder = ui::Solve( slab, ui::InspectorHost::Column, 9 );
	const ui::Regions regions = ui::Regions::For( slab, ladder );

	const float flRows = ui::ConfigureRowsHeight( 6, 2.0f );
	INFO( "rows " << flRows << " vs body " << regions.rcInspectorBody.Height() );

	// The rows ALONE -- before the title, the help paragraph and the pad,
	// none of which this lower bound counts -- already exceed the body.
	// So the body must scroll; there is no layout that fits it.
	REQUIRE( flRows > regions.rcInspectorBody.Height() );
}

TEST_CASE( "the configure body's height is linear in its parameter count", "[overlay_shell]" )
{
	// One row per param at the one control height, so the difference
	// between n and n+1 params is exactly one row -- the property that
	// makes the budget's cost predictable rather than emergent.
	for ( float flScale : { 1.0f, 1.25f, 2.0f } )
	{
		const float flRowPx = ui::tok::kRowH * flScale;
		for ( int n = 1; n < 6; ++n )
		{
			REQUIRE_THAT( ui::ConfigureRowsHeight( n + 1, flScale )
			              - ui::ConfigureRowsHeight( n, flScale ),
			              WithinAbs( flRowPx, 0.01f ) );
		}

		// A parameterless entry pays for no PARAMETERS band at all.
		REQUIRE( ui::ConfigureRowsHeight( 0, flScale )
		         < ui::ConfigureRowsHeight( 1, flScale ) - flRowPx );
	}
}

TEST_CASE( "a parameterless entry still fits its body at every scale", "[overlay_shell]" )
{
	// The complement of the overflow test: scrolling must be the exception
	// the deep rows need, not something every selection triggers.
	for ( float flScale : { 0.5f, 1.0f, 1.25f, 2.0f } )
	{
		const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, flScale );
		const ui::LadderResult ladder = ui::Solve( slab, ui::InspectorHost::Column, 6 );
		const ui::Regions regions = ui::Regions::For( slab, ladder );
		if ( ladder.eHost == ui::InspectorHost::Hidden )
			continue;

		INFO( "scale " << flScale );
		REQUIRE( ui::ConfigureRowsHeight( 0, flScale ) < regions.rcInspectorBody.Height() );
	}
}

// =========================================================================
//  The drawer over the sheet's controls (D17)
// =========================================================================
// The companion to the lane tests in test_overlay_ui.cpp. Those pin the
// arithmetic against numbers typed into the test; this one DERIVES the
// occlusion from the same Slab/Solve/Regions the shell uses, so it fails if
// the ladder's numbers move underneath the fix, not only if the lane's own
// formula changes. That is the shape the original defect had: the lane was
// right about the width it was given, and the width was wrong.
TEST_CASE( "at 2.0x the drawer no longer covers the sheet's control column", "[overlay_shell]" )
{
	const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, 2.0f );
	const ui::LadderResult ladder = ui::Solve( slab, ui::InspectorHost::Column, 9 );
	const ui::Regions regions = ui::Regions::For( slab, ladder );

	// Precondition: 2.0x is the step that produces a drawer at all. If the
	// ladder ever stops demoting here this test measures nothing, and it
	// should say so loudly rather than pass vacuously.
	REQUIRE( ladder.eHost == ui::InspectorHost::Drawer );
	REQUIRE( ladder.nStep == 2 );
	REQUIRE_THAT( ladder.flSheetBase, WithinAbs( 804.0f, 1e-4f ) );

	// What Shell.cpp's DrawSheetBody computes, in the same order.
	const float flScale = 2.0f;
	const float flPad   = ui::tok::kSheetPad * flScale;
	const float flColW  = regions.rcSheetBody.Width() - 2.0f * flPad;
	const float flOccPx = regions.rcSheetBody.x1 - regions.rcInspector.x0;

	REQUIRE( flOccPx > 0.0f );   // the drawer really does overlap the sheet

	const ui::Lane lane = ui::Lane::ForColumn(
		flColW / flScale, std::max( 0.0f, flOccPx - flPad ) / flScale );

	const float flColX0    = regions.rcSheetBody.x0 + flPad;
	const float flDrawerX0 = regions.rcInspector.x0;

	// THE ASSERTION THE SCREENSHOT WAS MAKING BY EYE: the right edge of every
	// rect the lane can hand out sits left of the drawer. Before D17 the
	// control zone ended some 700 px INSIDE it.
	const float flCtlMaxPx = flColX0 + lane.flCtlMax * flScale;
	const float flAffMaxPx = flColX0 + lane.flWidth  * flScale;
	INFO( "control right " << flCtlMaxPx << " vs drawer left " << flDrawerX0 );

	REQUIRE( flCtlMaxPx <= flDrawerX0 );
	REQUIRE( flAffMaxPx <= flDrawerX0 );

	// And there is still a control zone to reach: a fix that satisfied the
	// assertion above by collapsing the column would be worse than the defect.
	REQUIRE( lane.CtlWidth() > ui::tok::kSwitchW );
}

TEST_CASE( "closing the drawer gives the sheet's lane its full column back", "[overlay_shell]" )
{
	// D17's other half: opening and closing the drawer costs no relayout
	// ELSEWHERE. The regions are computed without reference to the lane, so
	// they are untouched, and the lane returns to exactly the column when the
	// occlusion goes away.
	const ui::Slab slab = ui::Slab::For( kSurfW, kSurfH, 2.0f );
	const ui::LadderResult drawer = ui::Solve( slab, ui::InspectorHost::Column, 9 );
	const ui::Regions rDrawer = ui::Regions::For( slab, drawer );

	const ui::LadderResult hidden = ui::Solve( slab, ui::InspectorHost::Hidden, 9 );
	const ui::Regions rHidden = ui::Regions::For( slab, hidden );

	const float flScale = 2.0f;
	const float flPad   = ui::tok::kSheetPad * flScale;

	// With the drawer up the sheet REGION is untouched by the fix: still
	// wider than the hidden case, which pays the spine. That is what "the
	// drawer still overlays the sheet's background" means concretely.
	REQUIRE( rDrawer.rcSheet.Width() > rHidden.rcSheet.Width() );

	// Same region, no occlusion -> the lane is the whole column again.
	const float flColW = rDrawer.rcSheetBody.Width() - 2.0f * flPad;
	REQUIRE_THAT( ui::Lane::ForColumn( flColW / flScale, 0.0f ).flWidth,
	              WithinAbs( flColW / flScale, 1e-4f ) );

	// The occluded lane is strictly narrower, and that is the only difference.
	const float flOccPx = rDrawer.rcSheetBody.x1 - rDrawer.rcInspector.x0;
	const ui::Lane open = ui::Lane::ForColumn(
		flColW / flScale, std::max( 0.0f, flOccPx - flPad ) / flScale );
	REQUIRE( open.flWidth < flColW / flScale );
}
