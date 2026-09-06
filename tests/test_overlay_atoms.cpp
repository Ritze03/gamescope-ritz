// A headless harness for the E2 control atoms.
//
// ImGui is perfectly happy to run without a renderer: create a context, give
// it a display size and a delta time, run NewFrame/Begin/.../End/Render, and
// every layout, hit-test and behaviour path executes with nothing on screen.
// That is enough to exercise the atoms for real -- ItemAdd(), ButtonBehavior()
// and SliderBehavior() all run -- so the geometry contract can be checked
// against what ImGui actually registered rather than against what the kit
// believes it asked for.
//
// WHAT THIS FILE IS FOR, specifically. The bug class shipping issue #23 found
// is "the thing you see is not the thing you can hit". Controls.cpp removes it
// structurally (one ImRect per atom, and the slider's handle is drawn from the
// rect SliderBehavior itself returned). These tests check the observable half
// of that from outside: for every atom, the rect ImGui registered for
// hit-testing is exactly the rect the row's allocator handed out, at every
// display_scale -- which is the property that was silently false before.
//
// NOTE ON "INPUT". Mouse positions and clicks below go into ImGuiIO's own
// event queue inside this process. Nothing is injected into the desktop, no
// window is created, and no compositor is involved.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "imgui.h"
#include "imgui_internal.h"

#include "Overlay/Fonts.h"
#include "Overlay/Palette.h"
#include "Overlay/UI/Controls.h"
#include "Overlay/UI/Lane.h"
#include "Overlay/UI/Row.h"
#include "Overlay/UI/Tokens.h"

#include <string>

// palette::g_LiveTheme used to be defined HERE, because its real definition
// lived in Chrome.cpp -- the legacy overlay's 1600 lines of dock/window/drag
// machinery -- and linking that in so Palette.cpp could read one float would
// have dragged the entire old UI into a test binary. P5 deleted that file and
// moved the storage to Palette.cpp, which this harness already links, so the
// stub is gone and the test now shares the product's own definition.

using namespace gamescope;
using Catch::Matchers::WithinAbs;

namespace
{
	// One ImGui context for the whole file, torn down at exit. Creating and
	// destroying a context per test would rebuild the font atlas each time for
	// no benefit.
	class Headless
	{
	public:
		static Headless &Get()
		{
			static Headless s_Instance;
			return s_Instance;
		}

		void BeginFrame()
		{
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2( 1920.0f, 1080.0f );
			io.DeltaTime   = 1.0f / 60.0f;

			ImGui::NewFrame();
			ImGui::SetNextWindowPos( ImVec2( 0.0f, 0.0f ) );
			ImGui::SetNextWindowSize( io.DisplaySize );
			ImGui::Begin( "harness", nullptr,
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
				ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground );
		}

		void EndFrame()
		{
			ImGui::End();
			ImGui::Render();
		}

		void MoveMouse( const ImVec2 &pos )  { ImGui::GetIO().AddMousePosEvent( pos.x, pos.y ); }
		void MouseButton( bool bDown )       { ImGui::GetIO().AddMouseButtonEvent( 0, bDown ); }

	private:
		Headless()
		{
			IMGUI_CHECKVERSION();
			m_pCtx = ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2( 1920.0f, 1080.0f );
			io.DeltaTime   = 1.0f / 60.0f;

			// The null-backend contract in ImGui 1.92's dynamic font system:
			// claiming RendererHasTextures tells ImGui to queue texture
			// updates for a backend to consume instead of demanding a legacy
			// pre-built atlas. Nothing consumes them here, which is exactly
			// what "headless" means -- glyphs are still rasterised and
			// measured, they are simply never uploaded anywhere.
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

			// The overlay's real font set, loaded from the same embedded Geist
			// faces the shipping binary uses, so text measurement in the atoms
			// is the measurement the product does.
			fonts::Load( 1.0f );
			palette::UpdateAccentFamily();

			// One warm-up frame: ImGui needs a completed frame before window
			// geometry and hover state settle.
			BeginFrame();
			EndFrame();
		}

		~Headless()
		{
			ImGui::DestroyContext( m_pCtx );
		}

		ImGuiContext *m_pCtx = nullptr;
	};

	struct ScopedScale
	{
		explicit ScopedScale( float fl ) { ui::SetScale( fl ); }
		~ScopedScale() { ui::SetScale( 1.0f ); }
	};

	// A standard row to place atoms in: one 804-base column, top-left, which
	// is SPEC §8.3's worked width.
	ui::RowCtx MakeRow( float flTopPx = 200.0f )
	{
		return ui::RowCtx::ForRow( ui::Lane::ForColumn( 804.0f ), 40.0f, flTopPx );
	}

	// The rect ImGui registered for the item that was just submitted.
	ImRect LastItemRect()
	{
		return ImRect( ImGui::GetItemRectMin(), ImGui::GetItemRectMax() );
	}

	// DrawModal()'s own footer geometry, replicated -- the same technique
	// MakeRow() above already uses for a sheet row. LastItemRect() cannot
	// stand in for this one the way it does for a plain row atom: DrawModal()
	// is self-hosted in its OWN top-level window (Controls.cpp's own
	// comment on why -- the same reason DrawPalette() is), and ImGui's own
	// End() restores g.LastItemData to whatever it was in the CALLER's
	// window before that Begin() (imgui.cpp: `g.LastItemData =
	// window_stack_data.ParentLastItemDataBackup;`) precisely so an item
	// inside a nested window cannot leak out as "the last item" once it
	// closes -- correct behaviour, and the reason this test cannot simply
	// ask ImGui where the button ended up the way the row-atom tests do.
	//
	// Valid only for a spec with no fnBody (every test that uses this
	// leaves it unset), so the body is always exactly one kRowH tall -- the
	// one number this needs that isn't already a token.
	ImRect ModalPrimaryButtonRect( const ImRect &rcSlab, const char *pszPrimaryLabel )
	{
		const float flW      = std::clamp( rcSlab.GetWidth() - ui::Px( ui::tok::kXL ) * 2.0f,
		                                   ui::Px( 240.0f ), ui::Px( 420.0f ) );
		const float flTitleH = ui::Px( ui::tok::kRowH );
		const float flFootH  = ui::Px( ui::tok::kRowH );
		const float flBodyH  = ui::Px( ui::tok::kRowH );   // no fnBody -> exactly one row
		const float flH      = flTitleH + flBodyH + flFootH;

		const float x1 = rcSlab.Min.x + ( rcSlab.GetWidth()  - flW ) * 0.5f + flW;
		const float y1 = rcSlab.Min.y + ( rcSlab.GetHeight() - flH ) * 0.5f + flH;

		const float flPad     = ui::Px( ui::tok::kL );
		const float flBtnH    = ui::Px( ui::tok::kControlH );
		const float flPrimaryW = ui::MeasureText( ui::TypeRole::Meta, pszPrimaryLabel ).x
		                        + ui::Px( ui::tok::kVerbPadX ) * 2.0f;
		const float flBtnY    = ( y1 - flFootH ) + ( flFootH - flBtnH ) * 0.5f;

		return ImRect( x1 - flPad - flPrimaryW, flBtnY, x1 - flPad, flBtnY + flBtnH );
	}

	void RequireSameRect( const ImRect &a, const ImRect &b )
	{
		REQUIRE_THAT( a.Min.x, WithinAbs( b.Min.x, 1e-3f ) );
		REQUIRE_THAT( a.Min.y, WithinAbs( b.Min.y, 1e-3f ) );
		REQUIRE_THAT( a.Max.x, WithinAbs( b.Max.x, 1e-3f ) );
		REQUIRE_THAT( a.Max.y, WithinAbs( b.Max.y, 1e-3f ) );
	}
}

TEST_CASE( "atoms: the registered hit box is the rect the row allocated", "[overlay_atoms]" )
{
	// This is the observable form of Controls.h's rule 1 -- "the rect handed
	// to ItemAdd() is the same C++ object handed to the painter". If an atom
	// ever recomputed its own geometry, the two would drift and this would
	// catch it, at every scale, which is where #23's instance actually hid.
	for ( float flScale : { 0.5f, 1.0f, 1.25f, 2.0f } )
	{
		ScopedScale s( flScale );
		Headless &h = Headless::Get();
		INFO( "display_scale " << flScale );

		h.BeginFrame();
		const ui::RowCtx row = MakeRow();

		bool bValue = false;
		ui::controls::Switch( row, "sw", &bValue );
		RequireSameRect( LastItemRect(), row.Place( ui::tok::kSwitchW ) );

		float flSlider = 0.5f;
		ui::controls::Slider( row, "sl", &flSlider, 0.0f, 1.0f );
		RequireSameRect( LastItemRect(), row.PlaceFull() );

		h.EndFrame();
	}
}

TEST_CASE( "atoms: every atom leaves the ImGui style and ID stacks balanced", "[overlay_atoms]" )
{
	// The slider pushes GrabMinSize around SliderBehavior() and the measured
	// atoms push an ID scope. A leak there would silently reshape whatever
	// the shell drew next -- the failure mode that is hardest to attribute
	// back to its cause, so it gets its own assertion rather than a comment.
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.BeginFrame();

	ImGuiContext &g = *ImGui::GetCurrentContext();
	const int nStyleVars = g.StyleVarStack.Size;
	const int nColors    = g.ColorStack.Size;
	const int nIds       = ImGui::GetCurrentWindow()->IDStack.Size;
	const int nItemFlags = g.ItemFlagsStack.Size;

	const ui::RowCtx row = MakeRow();

	bool bB = false;
	float flF = 0.5f;
	int nI = 3;
	uint32_t nMask = 0b101;
	std::string sText = "profile";
	bool bEditing = false;

	static constexpr ui::Option kOpts[] = {
		{ 0, "auto" }, { 1, "fit" }, { 2, "fill" }, { 3, "integer" },
	};

	ui::controls::Switch( row, "sw", &bB );
	ui::controls::Slider( row, "sl", &flF, 0.0f, 1.0f, 0.25f, true );
	ui::controls::SliderInt( row, "sli", &nI, 0, 20, 2, true );
	ui::controls::Stepper( row, "st", &nI, 0, 1000, 5 );
	ui::controls::Choice( row, "ch", &nI, kOpts, IM_ARRAYSIZE( kOpts ) );
	ui::controls::Text( row, "tx", &sText, &bEditing, "type a name" );
	ui::controls::Bank( row, "bk", &nMask, kOpts, IM_ARRAYSIZE( kOpts ) );
	ui::controls::Meter( row, 0.4f, 0.0f, 1.0f );
	ui::controls::Verb( row, "vb", "reset to default" );
	int nV = 0, nH = 2;
	ui::controls::AnchorGrid( row.Place( 96.0f ), "ag", &nV, &nH );

	// The composite bodies push their own ID scopes and, in ColorBody's
	// case, nest three Rails inside one. A body that leaked a PushID would
	// corrupt every id after it in the sheet, so they are swept here with
	// every other atom rather than trusted.
	float flHue = 218.0f;
	float flR = 60.0f, flG = 189.0f, flB = 221.0f; // request #5: plain 0-255 R/G/B, not OKLCH L/C/H.
	ui::controls::HueBody( row.Place( 200.0f ), "hb", &flHue );
	ui::controls::ColorBody( row.Place( 200.0f ), "cb", &flR, &flG, &flB );
	const float kSamples[] = { 6.9f, 7.1f, 7.0f, 16.4f, 7.2f };
	ui::controls::GraphBody( row.Place( 200.0f ), kSamples, IM_ARRAYSIZE( kSamples ), 20.0f, 12.0f );

	REQUIRE( g.StyleVarStack.Size == nStyleVars );
	REQUIRE( g.ColorStack.Size == nColors );
	REQUIRE( ImGui::GetCurrentWindow()->IDStack.Size == nIds );
	REQUIRE( g.ItemFlagsStack.Size == nItemFlags );

	h.EndFrame();
}

TEST_CASE( "atoms: a switch toggles from a click inside its lane rect", "[overlay_atoms]" )
{
	// Drives the atom through ImGui's own event queue: the click lands on the
	// rect the allocator produced, which is the end-to-end version of the
	// "drawn == hit-tested" property.
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();

	bool bValue = false;
	const float flTop = 200.0f;

	// Frame 1: establish the item and hover it.
	{
		const ui::RowCtx probe = MakeRow( flTop );
		const ImRect rc = probe.Place( ui::tok::kSwitchW );
		h.MoveMouse( rc.GetCenter() );
	}
	h.BeginFrame();
	ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
	h.EndFrame();

	// Frame 2: press.
	h.MouseButton( true );
	h.BeginFrame();
	ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
	h.EndFrame();

	// Frame 3: release -- ButtonBehavior's default is press-on-click-release.
	h.MouseButton( false );
	h.BeginFrame();
	ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
	h.EndFrame();

	REQUIRE( bValue );

	// And a click to the LEFT of the lane -- where a left-aligned control
	// would have been -- does nothing, because nothing is allocated there.
	{
		const ui::RowCtx probe = MakeRow( flTop );
		const ImRect rc = probe.Place( ui::tok::kSwitchW );
		h.MoveMouse( ImVec2( rc.Min.x - 120.0f, rc.GetCenter().y ) );
	}
	for ( bool bDown : { true, false } )
	{
		h.MouseButton( bDown );
		h.BeginFrame();
		ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
		h.EndFrame();
	}
	REQUIRE( bValue );   // unchanged
}

// D22. The defect that shipped, as a test.
//
// Every sheet row draws a full-width InvisibleButton FIRST -- the row
// selector, so clicking a row's label selects it -- and then draws the row's
// control INTO THE SAME RECT. That is the arrangement the whole sheet is
// built on, and it was silently broken: ImGui's ItemHoverable() rejects a
// later item while an earlier one still holds g.HoveredId (and again on
// g.ActiveId while a button is held), so the row's own selector won the hit
// test for the entire row and no control inside it could be hovered or
// clicked. Every switch, slider, segmented cell, stepper, chip and colour
// rail in the product rendered perfectly and did nothing.
//
// The fix is one call -- SetNextItemAllowOverlap() before the row selector --
// and the reason it needs a test is that nothing about the drawing changes
// when it is missing. A screenshot cannot see this; only a click can.
//
// The two halves are both load-bearing, so both are asserted: the control
// must win where it covers the row, and the row must still win everywhere
// the control does not.
TEST_CASE( "atoms: a control inside a full-width row selector still takes the click", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();

	const float flTop = 200.0f;
	const ui::RowCtx probe = MakeRow( flTop );
	const ImRect rcRow    = probe.Bounds();
	const ImRect rcSwitch = probe.Place( ui::tok::kSwitchW );

	bool bValue = false;
	int  nRowClicks = 0;

	// One frame of the real arrangement: row selector first, control second.
	const auto Frame = [ & ]()
	{
		h.BeginFrame();
		ImGui::SetCursorScreenPos( rcRow.Min );
		ImGui::PushID( "row" );
		ImGui::SetNextItemAllowOverlap();
		if ( ImGui::InvisibleButton( "##row", rcRow.GetSize() ) )
			nRowClicks++;
		ImGui::PopID();
		ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
		h.EndFrame();
	};

	// ---- a click ON the switch drives the switch, not the row ----
	h.MoveMouse( rcSwitch.GetCenter() );
	Frame();
	h.MouseButton( true );
	Frame();
	h.MouseButton( false );
	Frame();

	REQUIRE( bValue );          // the control got it...
	REQUIRE( nRowClicks == 0 ); // ...and the row did not

	// ---- a click on the row's LABEL zone still selects the row ----
	h.MoveMouse( ImVec2( rcRow.Min.x + 8.0f, rcRow.GetCenter().y ) );
	Frame();
	h.MouseButton( true );
	Frame();
	h.MouseButton( false );
	Frame();

	REQUIRE( nRowClicks == 1 ); // the row got it...
	REQUIRE( bValue );          // ...and the switch is untouched
}

// D22. The OTHER half of the same bug, pinned as an executable rule.
//
// DrainInputQueue() (SettingsOverlay.cpp) used to call AddMouseButtonEvent()
// before flushing the pending cursor position, so a click that arrived in the
// same drain as its own motion reached ImGui as [button, position] rather
// than [position, button]. ImGui's input trickling stops at a MousePos that
// follows a button change in the same frame, so the press was applied at the
// STALE pointer position and the move was deferred a frame -- the widget
// under the cursor never saw the press, and whatever sat under the old
// position took ActiveId and then released outside itself.
//
// This is a property of ImGui, not of our code, which is exactly why it is
// worth a test: the drain's ordering is only correct BECAUSE this rule holds,
// and nothing else in the tree says so. If a future ImGui changes it, this
// fails and points at the drain.
TEST_CASE( "atoms: a queued click applies at the position queued BEFORE it", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();

	const float flTop = 200.0f;
	const ImRect rcSwitch = MakeRow( flTop ).Place( ui::tok::kSwitchW );

	// Park the pointer far away, so a press applied at the STALE position
	// cannot possibly land on the switch.
	h.MoveMouse( ImVec2( 10.0f, 10.0f ) );
	h.BeginFrame();
	bool bValue = false;
	ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
	h.EndFrame();

	// The correct order: move, THEN press -- both queued before one frame,
	// exactly as the drain now emits them.
	h.MoveMouse( rcSwitch.GetCenter() );
	h.MouseButton( true );
	h.BeginFrame();
	ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
	h.EndFrame();

	h.MouseButton( false );
	h.BeginFrame();
	ui::controls::Switch( MakeRow( flTop ), "sw", &bValue );
	h.EndFrame();

	REQUIRE( bValue );
}

TEST_CASE( "atoms: Choice downgrades to a dropdown when segmented will not fit", "[overlay_atoms]" )
{
	// SPEC §3.2: "The helper measures and auto-downgrades to a dropdown if any
	// of the three conditions fails or if the measured group does not fit the
	// lane; a caller passing six options gets a dropdown and cannot ship a
	// cramped row." One predicate, decided by the helper, never by a caller.
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.BeginFrame();

	int nValue = 0;

	SECTION( "a short static set of four stays segmented" )
	{
		static constexpr ui::Option kFew[] = { { 0, "auto" }, { 1, "fit" }, { 2, "fill" }, { 3, "integer" } };
		const auto res = ui::controls::Choice( MakeRow(), "few", &nValue, kFew, IM_ARRAYSIZE( kFew ) );
		REQUIRE( res.bSegmented );
	}

	SECTION( "six options is over the cap, so it is a dropdown" )
	{
		static constexpr ui::Option kMany[] = {
			{ 0, "a" }, { 1, "b" }, { 2, "c" }, { 3, "d" }, { 4, "e" }, { 5, "f" },
		};
		const auto res = ui::controls::Choice( MakeRow(), "many", &nValue, kMany, IM_ARRAYSIZE( kMany ) );
		REQUIRE_FALSE( res.bSegmented );
	}

	SECTION( "a label over eight characters is over the cap too" )
	{
		static constexpr ui::Option kLong[] = { { 0, "auto" }, { 1, "nearest-neighbour" } };
		const auto res = ui::controls::Choice( MakeRow(), "long", &nValue, kLong, IM_ARRAYSIZE( kLong ) );
		REQUIRE_FALSE( res.bSegmented );
	}

	SECTION( "a lane too narrow for the measured group downgrades as well" )
	{
		// The same registration, in a column narrow enough that the measured
		// cells no longer fit -- "the helper measures the column it actually
		// got" (SPEC §8.3).
		static constexpr ui::Option kFew[] = { { 0, "auto" }, { 1, "fit" }, { 2, "fill" }, { 3, "integer" } };
		const ui::RowCtx narrow = ui::RowCtx::ForRow( ui::Lane::ForColumn( 420.0f ), 40.0f, 300.0f );
		ScopedScale big( 2.0f );
		const auto res = ui::controls::Choice( narrow, "narrow", &nValue, kFew, IM_ARRAYSIZE( kFew ) );
		REQUIRE_FALSE( res.bSegmented );
	}

	h.EndFrame();
}

TEST_CASE( "atoms: a segmented group's cells are right-bound and never overlap", "[overlay_atoms]" )
{
	// The measured atoms are the ones where a fit test and a layout could
	// disagree. MeasureCells() is the single function both read, and this is
	// the observable consequence: the last cell ends exactly on the lane, and
	// the cells tile without overlapping.
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.BeginFrame();

	static constexpr ui::Option kOpts[] = { { 0, "auto" }, { 1, "fit" }, { 2, "fill" }, { 3, "integer" } };
	const ui::RowCtx row = MakeRow();

	int nValue = 0;
	const auto res = ui::controls::Choice( row, "seg", &nValue, kOpts, IM_ARRAYSIZE( kOpts ) );
	REQUIRE( res.bSegmented );

	// The last cell submitted is the rightmost, and its right edge is the
	// lane's control line -- the same line every other control ends on.
	REQUIRE_THAT( LastItemRect().Max.x, WithinAbs( row.PlaceFull().Max.x, 1e-3f ) );
	REQUIRE_THAT( LastItemRect().GetHeight(), WithinAbs( ui::Px( ui::tok::kControlH ), 1e-3f ) );

	h.EndFrame();
}

TEST_CASE( "atoms: a chip bank never escapes the lane", "[overlay_atoms]" )
{
	// SPEC §2.2's right-bound law is universal, and a Bank is the one measured
	// atom with NO downgrade to fall back to (Choice() has the dropdown). Its
	// run used to be laid out at full measured width from the lane's left
	// edge, so an over-wide bank ran straight out of the control zone -- at
	// 2.0x with the drawer open, `log.severity`'s four chips finished 80 base
	// units past the sheet's right edge, where the last of them was invisible
	// and unclickable.
	Headless &h = Headless::Get();

	static constexpr ui::Option kFour[] = {
		{ 0, "error" }, { 1, "warn" }, { 2, "info" }, { 3, "debug" },
	};

	// The measurements are taken inside the frame and asserted after it, so a
	// failing REQUIRE cannot leave ImGui mid-frame and cascade into every test
	// that runs afterwards.
	SECTION( "a run that fits is untouched and ends on the lane" )
	{
		ScopedScale s( 1.0f );
		h.BeginFrame();
		const ui::RowCtx row = MakeRow();
		uint32_t nMask = 0b0101;
		ui::controls::Bank( row, "bank", &nMask, kFour, IM_ARRAYSIZE( kFour ) );
		const ImRect rcLast = LastItemRect();
		const ImRect rcLane = row.PlaceFull();
		h.EndFrame();

		REQUIRE_THAT( rcLast.Max.x, WithinAbs( rcLane.Max.x, 1.0f ) );
	}

	SECTION( "a run too wide for the lane is scaled into it, not spilled out of it" )
	{
		// D17's occluded lane: 2.0x with the drawer open leaves the sheet a
		// 368-base column, which is the case that produced the defect.
		ScopedScale s( 2.0f );
		h.BeginFrame();
		const ui::RowCtx narrow =
			ui::RowCtx::ForRow( ui::Lane::ForColumn( 756.0f, 388.0f ), 40.0f, 300.0f );
		uint32_t nMask = 0b1111;
		ui::controls::Bank( narrow, "banknarrow", &nMask, kFour, IM_ARRAYSIZE( kFour ) );
		const ImRect rcLast = LastItemRect();
		const ImRect rcLane = narrow.PlaceFull();
		h.EndFrame();

		// The last chip is the rightmost, and it must land ON the lane, never
		// past it.
		REQUIRE( rcLast.Max.x <= rcLane.Max.x + 1.0f );
		REQUIRE( rcLast.Min.x >= rcLane.Min.x - 1.0f );
		REQUIRE( rcLast.GetWidth() > 0.0f );
	}
}

TEST_CASE( "atoms: nothing crashes or inverts at the extremes of display_scale", "[overlay_atoms]" )
{
	// The ladder reaches 0.5x and 2.0x, and a narrow three-column sheet at
	// 0.5x hands an atom a column at its floor. Every atom must survive both
	// without producing an inverted rect.
	static constexpr ui::Option kOpts[] = { { 0, "auto" }, { 1, "fit" } };

	for ( float flScale : { 0.5f, 2.0f } )
	{
		for ( float flColumn : { 420.0f, 560.0f, 1728.0f } )
		{
			ScopedScale s( flScale );
			Headless &h = Headless::Get();
			INFO( "scale " << flScale << " column " << flColumn );

			h.BeginFrame();
			const ui::RowCtx row = ui::RowCtx::ForRow( ui::Lane::ForColumn( flColumn ), 40.0f, 300.0f );

			bool bB = false; float flF = 0.5f; int nI = 3; uint32_t nMask = 1;
			std::string sText = "x"; bool bEditing = false;

			ui::controls::Switch( row, "sw", &bB );
			REQUIRE( LastItemRect().Min.x <= LastItemRect().Max.x );

			ui::controls::Slider( row, "sl", &flF, 0.0f, 1.0f );
			REQUIRE( LastItemRect().Min.x <= LastItemRect().Max.x );

			ui::controls::Stepper( row, "st", &nI, 0, 100, 1 );
			ui::controls::Choice( row, "ch", &nI, kOpts, IM_ARRAYSIZE( kOpts ) );
			ui::controls::Text( row, "tx", &sText, &bEditing );
			ui::controls::Bank( row, "bk", &nMask, kOpts, IM_ARRAYSIZE( kOpts ) );
			ui::controls::Meter( row, 0.4f, 0.0f, 1.0f );
			ui::controls::Verb( row, "vb", "reset" );

			h.EndFrame();
		}
	}
}

// =========================================================================
//  Typed entry on a Stepper -- request #14 (2026-09-05)
// =========================================================================
// The pure half: what a typed string becomes. Parse -> clamp to the range,
// and NOTHING else: the step is the buttons' increment, not a grid a typed
// value has to land on (Controls.h's Why). The worked cases are the
// custom-resolution row (320..7680, step 8) the request named and the
// fps-limit row (0..480, step 10) whose off-grid 144 is D13.3's proof.
TEST_CASE( "atoms: a typed stepper value is parsed and clamped, never snapped", "[overlay_atoms]" )
{
	int n = -1;

	// In range: exactly what was typed, on the step-8 row's grid or off it.
	REQUIRE( ui::controls::ParseClampedInt( "1000", 320, 7680, &n ) );
	REQUIRE( n == 1000 );
	REQUIRE( ui::controls::ParseClampedInt( "1003", 320, 7680, &n ) );
	REQUIRE( n == 1003 );
	REQUIRE( ui::controls::ParseClampedInt( "1005", 320, 7680, &n ) );
	REQUIRE( n == 1005 );

	// 144 on the step-10 fps row means 144.
	REQUIRE( ui::controls::ParseClampedInt( "144", 0, 480, &n ) );
	REQUIRE( n == 144 );

	// Out of range: clamped to the end.
	REQUIRE( ui::controls::ParseClampedInt( "99999", 320, 7680, &n ) );
	REQUIRE( n == 7680 );
	REQUIRE( ui::controls::ParseClampedInt( "0", 320, 7680, &n ) );
	REQUIRE( n == 320 );
	REQUIRE( ui::controls::ParseClampedInt( "-50", 320, 7680, &n ) );
	REQUIRE( n == 320 );

	// Surrounding whitespace is tolerated; a sign is a sign.
	REQUIRE( ui::controls::ParseClampedInt( "  1600 ", 320, 7680, &n ) );
	REQUIRE( n == 1600 );
	REQUIRE( ui::controls::ParseClampedInt( "+144", 24, 500, &n ) );
	REQUIRE( n == 144 );

	// Not a whole number: refused, and the output is untouched -- that is
	// what "cancel with the old value" is built on.
	n = 4242;
	REQUIRE_FALSE( ui::controls::ParseClampedInt( "abc",    320, 7680, &n ) );
	REQUIRE_FALSE( ui::controls::ParseClampedInt( "",       320, 7680, &n ) );
	REQUIRE_FALSE( ui::controls::ParseClampedInt( "   ",    320, 7680, &n ) );
	REQUIRE_FALSE( ui::controls::ParseClampedInt( "12.5",   320, 7680, &n ) );
	REQUIRE_FALSE( ui::controls::ParseClampedInt( "1e3",    320, 7680, &n ) );
	REQUIRE_FALSE( ui::controls::ParseClampedInt( "1600px", 320, 7680, &n ) );
	REQUIRE_FALSE( ui::controls::ParseClampedInt( "99999999999999999999", 320, 7680, &n ) );
	REQUIRE_FALSE( ui::controls::ParseClampedInt( nullptr,  320, 7680, &n ) );
	REQUIRE( n == 4242 );
}

// The drawn half. The edit field is a real ImGui InputText swapped into the
// value column, so the thing to check headlessly is that opening it,
// committing it and cancelling it all leave ImGui's stacks where they were,
// that the field's width is the one measurement the row split against, and
// that a click on the number is what opens it.
TEST_CASE( "atoms: a stepper's typed entry opens on the number and keeps the stacks balanced",
           "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();

	const float flTop = 300.0f;
	int nValue = 1280;
	bool bEditing = false;

	// The row's split, exactly as the shell does it: the value's measured
	// width, right-bound one gutter before the stepper group.
	auto Split = [&]( const ui::RowCtx &row, float flW )
	{
		ImRect rcLabel, rcValue;
		row.SplitLabelZone( flW, row.Place( ui::tok::kStepperW ).Min.x, &rcLabel, &rcValue );
		return rcValue;
	};
	auto Draw = [&]( const ui::RowCtx &row )
	{
		const float flW = bEditing
			? ui::controls::StepperEditWidthPx( "px" )
			: ui::MeasureText( ui::TypeRole::Value, "1280 px" ).x;
		ui::controls::StepperEdit edit;
		edit.rcValue   = Split( row, flW );
		edit.pbEditing = &bEditing;
		edit.pszUnit   = "px";
		return ui::controls::Stepper( row, "st", &nValue, 320, 7680, 8, &edit );
	};

	// Frame 0: idle. Where is the number?
	h.BeginFrame();
	const ImRect rcNumber = Split( MakeRow( flTop ), ui::MeasureText( ui::TypeRole::Value, "1280 px" ).x );
	Draw( MakeRow( flTop ) );
	h.EndFrame();
	REQUIRE( rcNumber.GetWidth() > 0.0f );

	// The field is wider than the number, and carries the unit outside it.
	REQUIRE( ui::controls::StepperEditWidthPx( "px" ) > ui::controls::StepperEditWidthPx( nullptr ) );
	REQUIRE( ui::controls::StepperEditWidthPx( nullptr ) > ui::MeasureText( ui::TypeRole::Value, "7680" ).x );

	// Click on the number: hover, press, release.
	h.MoveMouse( rcNumber.GetCenter() );
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	REQUIRE( bEditing );

	// Editing frames: the InputText is live. Stacks must balance across it.
	{
		h.BeginFrame();
		ImGuiContext &g = *ImGui::GetCurrentContext();
		const int nStyleVars = g.StyleVarStack.Size;
		const int nColors    = g.ColorStack.Size;
		const int nIds       = ImGui::GetCurrentWindow()->IDStack.Size;
		const int nItemFlags = g.ItemFlagsStack.Size;
		REQUIRE_FALSE( Draw( MakeRow( flTop ) ) );
		REQUIRE( g.StyleVarStack.Size == nStyleVars );
		REQUIRE( g.ColorStack.Size == nColors );
		REQUIRE( ImGui::GetCurrentWindow()->IDStack.Size == nIds );
		REQUIRE( g.ItemFlagsStack.Size == nItemFlags );
		h.EndFrame();
	}
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	REQUIRE( bEditing );
	REQUIRE( ImGui::IsAnyItemActive() );

	// A key press, twice. ImGui routes InputText's Esc through Shortcut(),
	// and a shortcut route requested on frame N is granted on frame N+1
	// (SetShortcutRouting: RoutingNext -> RoutingCurr at NewFrame), so the
	// very first Esc after a field opens is a routing request, not a
	// cancel. The product never notices -- the shell's own Esc handler
	// drops the editing bit the same frame -- but this harness has no
	// shell, so it presses twice: the second press lands with the route
	// held. Returns whether any frame reported a commit.
	auto Press = [&]( ImGuiKey eKey )
	{
		bool bAny = false;
		for ( int i = 0; i < 2; ++i )
		{
			ImGui::GetIO().AddKeyEvent( eKey, true );
			h.BeginFrame(); bAny |= Draw( MakeRow( flTop ) ); h.EndFrame();
			ImGui::GetIO().AddKeyEvent( eKey, false );
			h.BeginFrame(); bAny |= Draw( MakeRow( flTop ) ); h.EndFrame();
		}
		return bAny;
	};

	// Esc: ImGui reverts and deactivates; the atom drops the bit and the
	// value is untouched.
	REQUIRE_FALSE( Press( ImGuiKey_Escape ) );
	REQUIRE_FALSE( bEditing );
	REQUIRE( nValue == 1280 );

	// Open again, type a replacement (AutoSelectAll: the typed text replaces
	// the pre-filled number), Enter commits through the parse/clamp.
	bEditing = true;
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	REQUIRE( ImGui::IsAnyItemActive() );
	for ( char c : std::string( "1603" ) )
		ImGui::GetIO().AddInputCharacter( (unsigned int)c );
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	REQUIRE( Press( ImGuiKey_Enter ) );
	REQUIRE_FALSE( bEditing );
	REQUIRE( nValue == 1603 );   // exactly as typed -- off the step-8 grid, on purpose

	// Buttons still work alongside the field: no edit in flight, so "+"
	// steps as it always has.
	const ImRect rcPlus = ImRect(
		MakeRow( flTop ).Place( ui::tok::kStepperW ).Max.x - ui::Px( ui::tok::kStepperGlyphW ),
		MakeRow( flTop ).Place( ui::tok::kStepperW ).Min.y,
		MakeRow( flTop ).Place( ui::tok::kStepperW ).Max.x,
		MakeRow( flTop ).Place( ui::tok::kStepperW ).Max.y );
	h.MoveMouse( rcPlus.GetCenter() );
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); Draw( MakeRow( flTop ) ); h.EndFrame();
	REQUIRE( nValue == 1611 );
	REQUIRE_FALSE( bEditing );
}

// =========================================================================
//  OKLCH round trip -- the Colour override composite's binding
// =========================================================================
TEST_CASE( "palette: sRGB survives a round trip through OKLCH", "[overlay_atoms]" )
{
	// SPEC §4.4's Colour override edits L, C and H, but the config format it
	// is bound to is a packed sRGB integer -- so every edit converts out and
	// back. If ImU32ToOklch() and OklchToImU32() ever stop being inverses, a
	// colour would drift a little further every time the control was touched,
	// which is the kind of bug that only surfaces weeks later as "my colour
	// keeps changing on its own".
	//
	// The tolerance is 8-bit quantisation, not an approximation allowance:
	// one step per channel is the finest the packed format can express.
	const ImU32 kCases[] = {
		IM_COL32( 0x36, 0xBD, 0xDD, 255 ),   // the accent family at hue 218
		IM_COL32( 0x6E, 0xD2, 0x74, 255 ),   // SPEC §4.4's own worked example
		IM_COL32( 0xFF, 0x00, 0x00, 255 ),
		IM_COL32( 0x80, 0x80, 0x80, 255 ),
		IM_COL32( 0x12, 0x34, 0x56, 255 ),
	};

	for ( ImU32 col : kCases )
	{
		float flL = 0.0f, flC = 0.0f, flH = 0.0f;
		gamescope::palette::ImU32ToOklch( col, &flL, &flC, &flH );
		const ImU32 back = gamescope::palette::OklchToImU32( flL, flC, flH );

		INFO( "L " << flL << " C " << flC << " H " << flH );
		for ( int nShift : { IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT } )
		{
			const int a = (int)( ( col  >> nShift ) & 0xFF );
			const int b = (int)( ( back >> nShift ) & 0xFF );
			REQUIRE( ( a - b <= 1 && b - a <= 1 ) );
		}
	}
}

TEST_CASE( "palette: a neutral grey has no meaningful chroma", "[overlay_atoms]" )
{
	// Guards the inverse's hue branch: atan2 of two near-zero components is
	// numerically unstable, and a grey that came back with a large chroma
	// would make the Colour composite's C rail jump the moment someone
	// picked a neutral.
	float flL = 0.0f, flC = 0.0f, flH = 0.0f;
	gamescope::palette::ImU32ToOklch( IM_COL32( 0x80, 0x80, 0x80, 255 ), &flL, &flC, &flH );

	REQUIRE( flC < 0.01f );
	REQUIRE( flL > 0.4f );
	REQUIRE( flL < 0.7f );
}

// D27, item 2. The user: "The UI scale should update, when the slider is
// released. Otherwise, it is almost impossible, to adjust."
//
// `overlay.display_scale` is the one setting whose value decides the geometry
// of the control that edits it, so applying it live slides the track out from
// under the pointer. Its binding hands the whole apply to
// controls::DeferToRelease() while ui::IsPointerDragActive() is true.
//
// This pins the MECHANISM, not the setting: the apply must run exactly once,
// and never on a frame where the pointer is still holding a control -- which
// is the half a screenshot cannot show and a console command cannot reach.
TEST_CASE( "atoms: a deferred write waits for the drag to end, and runs once",
           "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();

	const float flTop = 320.0f;
	float flValue = 0.5f;
	int nApplied = 0;

	// Every frame of the drag re-queues the apply, exactly as a per-tick
	// setter does. The count must still end at 1.
	auto DrawSlider = [ & ]
	{
		h.BeginFrame();
		const ui::RowCtx row = MakeRow( flTop );
		if ( ui::controls::Slider( row, "scale", &flValue, 0.5f, 2.0f ) )
		{
			// What the real setter does: store now, defer the expensive half.
			if ( ui::IsPointerDragActive() )
				ui::controls::DeferToRelease( [ &nApplied ] { nApplied++; } );
			else
				nApplied++;
		}
		h.EndFrame();
	};

	const ImRect rcTrack = MakeRow( flTop ).PlaceFull();

	// Frame 1: hover the middle of the track.
	h.MoveMouse( rcTrack.GetCenter() );
	DrawSlider();

	// Frame 2: press. ImGui takes ActiveId DURING this frame, so the
	// frame-start view of "is anything held" is still false here -- this is
	// exactly the frame NoteDragOnLastItem() exists for, and a press that
	// jumps the value to the click point must already be deferred.
	h.MouseButton( true );
	DrawSlider();
	REQUIRE( nApplied == 0 );

	// Frames 3-5: drag left, then right. The value moves every frame; the
	// apply must not run on any of them.
	for ( float flX : { 0.30f, 0.55f, 0.80f } )
	{
		h.MoveMouse( ImVec2( rcTrack.Min.x + rcTrack.GetWidth() * flX,
		                     rcTrack.GetCenter().y ) );
		DrawSlider();
		REQUIRE( nApplied == 0 );
		REQUIRE( ui::IsPointerDragActive() );
	}

	// The value itself DID move while the apply was held back -- that is what
	// keeps the row's readout tracking the pointer.
	REQUIRE( flValue > 0.5f );
	REQUIRE( flValue < 2.0f );

	// Release, then further frames: the flush lives in the shared atom
	// prologue, so it lands on the first frame after nothing is held.
	h.MouseButton( false );
	DrawSlider();
	DrawSlider();

	REQUIRE( nApplied == 1 );
	REQUIRE_FALSE( ui::IsPointerDragActive() );

	// And it stays at one: a flushed deferral is cleared, not replayed.
	DrawSlider();
	DrawSlider();
	REQUIRE( nApplied == 1 );

	// Leave the shared harness idle: pointer parked off every allocated rect,
	// button up, two settled frames. Catch2 may run these cases in any order.
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	DrawSlider();
	DrawSlider();
}

// The other half of the same contract: OFF the drag path -- an arrow key, the
// reset chip, `overlay_e2_set` from the console thread -- there is no drag, so
// IsPointerDragActive() is false and the write applies immediately. A binding
// deferred there would strand the value until someone happened to touch a
// control, which is a worse failure than applying live.
TEST_CASE( "atoms: with no drag in flight a write is not deferred", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();

	// Park the pointer away from everything and settle two frames.
	h.MouseButton( false );
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	for ( int i = 0; i < 2; i++ )
	{
		h.BeginFrame();
		float flDummy = 0.5f;
		ui::controls::Slider( MakeRow( 320.0f ), "scale", &flDummy, 0.5f, 2.0f );
		h.EndFrame();
	}

	REQUIRE_FALSE( ui::IsPointerDragActive() );
}

// =========================================================================
//  ListBox -- the interactive half. The pure nav/scroll/layout arithmetic
//  is tested without a context in test_overlay_ui.cpp; this is the click
//  and keyboard-through-ImGui's-own-queue half that needs one.
// =========================================================================
namespace
{
	// The sketch's own four lines, verbatim.
	constexpr ui::controls::ListBoxItem kProfileItems[] = {
		{ "Rust",     "[Game]", nullptr },
		{ "Profile1", nullptr,  nullptr },
		{ "Comp",     nullptr,  nullptr },
		{ "Casual",   nullptr,  "inherits Comp" },
	};
}

TEST_CASE( "atoms: listbox selects and activates the row that was clicked", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );

	int nSelected = -1;
	const float flRowH = ui::Px( ui::tok::kControlH );
	const ImRect rcBody( 40.0f, 200.0f, 40.0f + ui::Px( 300.0f ), 200.0f + flRowH * 10.0f );

	auto Draw = [ & ]
	{
		return ui::controls::ListBox( rcBody, "profiles", &nSelected,
		                              kProfileItems, IM_ARRAYSIZE( kProfileItems ) );
	};

	h.BeginFrame(); Draw(); h.EndFrame();
	REQUIRE( nSelected == -1 );   // nothing selected yet

	// Click row index 1 ("Profile1"): hover, press, release.
	const ImVec2 vRow1Center( rcBody.GetCenter().x, rcBody.Min.y + flRowH * 1.5f );
	h.MoveMouse( vRow1Center );
	h.BeginFrame(); Draw(); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); Draw(); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame();
	const ui::controls::ListBoxResult res = Draw();
	h.EndFrame();

	REQUIRE( nSelected == 1 );
	REQUIRE( res.bChanged );
	REQUIRE( res.bActivated );   // Controls.h: "a click ... same as Enter"

	// Clicking the ALREADY-selected row changes nothing but still activates
	// -- "Enter = activate = same as click" holds for a click too.
	h.MoveMouse( vRow1Center );
	h.BeginFrame(); Draw(); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); Draw(); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame();
	const ui::controls::ListBoxResult res2 = Draw();
	h.EndFrame();

	REQUIRE( nSelected == 1 );
	REQUIRE_FALSE( res2.bChanged );
	REQUIRE( res2.bActivated );

	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );
	h.BeginFrame(); Draw(); h.EndFrame();
}

TEST_CASE( "atoms: listbox keyboard nav applies only while the pointer hovers it", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MouseButton( false );

	int nSelected = 0;
	const float flRowH = ui::Px( ui::tok::kControlH );
	const ImRect rcBody( 40.0f, 200.0f, 40.0f + ui::Px( 300.0f ), 200.0f + flRowH * 10.0f );

	auto Draw = [ & ]
	{
		return ui::controls::ListBox( rcBody, "profiles2", &nSelected,
		                              kProfileItems, IM_ARRAYSIZE( kProfileItems ) );
	};

	// Hovering the list, Down moves the selection.
	h.MoveMouse( rcBody.GetCenter() );
	h.BeginFrame(); Draw(); h.EndFrame();

	ImGui::GetIO().AddKeyEvent( ImGuiKey_DownArrow, true );
	h.BeginFrame();
	const ui::controls::ListBoxResult resDown = Draw();
	h.EndFrame();
	ImGui::GetIO().AddKeyEvent( ImGuiKey_DownArrow, false );
	h.BeginFrame(); Draw(); h.EndFrame();

	REQUIRE( nSelected == 1 );
	REQUIRE( resDown.bChanged );

	// Pointer OFF the list: the same key does nothing. This widget has no
	// keyboard-focus system of its own (Controls.h's ListBox() comment) --
	// hovering is the whole of how it claims the keyboard.
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.BeginFrame(); Draw(); h.EndFrame();

	ImGui::GetIO().AddKeyEvent( ImGuiKey_DownArrow, true );
	h.BeginFrame(); Draw(); h.EndFrame();
	ImGui::GetIO().AddKeyEvent( ImGuiKey_DownArrow, false );
	h.BeginFrame(); Draw(); h.EndFrame();

	REQUIRE( nSelected == 1 );   // unchanged

	// Home/End, back over the list.
	h.MoveMouse( rcBody.GetCenter() );
	h.BeginFrame(); Draw(); h.EndFrame();
	ImGui::GetIO().AddKeyEvent( ImGuiKey_End, true );
	h.BeginFrame(); Draw(); h.EndFrame();
	ImGui::GetIO().AddKeyEvent( ImGuiKey_End, false );
	h.BeginFrame(); Draw(); h.EndFrame();
	REQUIRE( nSelected == 3 );

	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.BeginFrame(); Draw(); h.EndFrame();
}

// =========================================================================
//  Modal -- Esc/primary/Enter, against a live frame. Open/close bookkeeping
//  on its own is tested without a context in test_overlay_ui.cpp.
// =========================================================================
TEST_CASE( "atoms: modal Esc cancels and calls fnCancel exactly once", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );   // clear of the footer buttons
	h.MouseButton( false );

	int nCancel = 0, nPrimary = 0;
	ui::ModalSpec spec;
	spec.sTitle        = "Delete profile?";
	spec.sPrimaryLabel = "Delete";
	spec.bPrimaryDanger = true;
	spec.fnCancel  = [ & ] { ++nCancel; };
	spec.fnPrimary = [ & ] { ++nPrimary; };
	ui::OpenModal( spec );

	const ImRect rcSlab( ImVec2( 0.0f, 0.0f ), ImGui::GetIO().DisplaySize );

	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();
	REQUIRE( ui::IsModalOpen() );
	REQUIRE( nCancel == 0 );

	ImGui::GetIO().AddKeyEvent( ImGuiKey_Escape, true );
	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();
	ImGui::GetIO().AddKeyEvent( ImGuiKey_Escape, false );
	h.BeginFrame(); h.EndFrame();

	REQUIRE_FALSE( ui::IsModalOpen() );
	REQUIRE( nCancel == 1 );
	REQUIRE( nPrimary == 0 );

	// A further frame draws nothing -- the modal is closed -- so the count
	// does not move again.
	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();
	REQUIRE( nCancel == 1 );
}

TEST_CASE( "atoms: modal primary button fires fnPrimary exactly once, and closes it", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );

	int nPrimary = 0, nCancel = 0;
	ui::ModalSpec spec;
	spec.sTitle        = "Create profile";
	spec.sPrimaryLabel = "Create";
	spec.fnPrimary = [ & ] { ++nPrimary; };
	spec.fnCancel  = [ & ] { ++nCancel; };
	ui::OpenModal( spec );

	const ImRect rcSlab( ImVec2( 0.0f, 0.0f ), ImGui::GetIO().DisplaySize );
	const ImRect rcPrimary = ModalPrimaryButtonRect( rcSlab, "Create" );

	h.MoveMouse( rcPrimary.GetCenter() );
	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();

	REQUIRE_FALSE( ui::IsModalOpen() );
	REQUIRE( nPrimary == 1 );
	REQUIRE( nCancel == 0 );

	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();
	REQUIRE( nPrimary == 1 );   // closed -- a further frame cannot fire it again

	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );
	h.BeginFrame(); h.EndFrame();
}

TEST_CASE( "atoms: modal Enter confirms once nothing is being edited", "[overlay_atoms]" )
{
	// Controls.h's ModalSpec::fnPrimary records the exact simplification:
	// with no cross-field tab order in this kit, "the LAST Text field"
	// cannot be told apart from any other, so Enter fires the primary
	// whenever no field is active -- which this body never makes active at
	// all (it draws no Text control), so the very first Enter confirms.
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );

	int nPrimary = 0;
	ui::ModalSpec spec;
	spec.sTitle        = "Create profile";
	spec.sPrimaryLabel = "Create";
	spec.fnPrimary = [ & ] { ++nPrimary; };
	ui::OpenModal( spec );

	const ImRect rcSlab( ImVec2( 0.0f, 0.0f ), ImGui::GetIO().DisplaySize );

	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();

	ImGui::GetIO().AddKeyEvent( ImGuiKey_Enter, true );
	h.BeginFrame(); ui::DrawModal( rcSlab ); h.EndFrame();
	ImGui::GetIO().AddKeyEvent( ImGuiKey_Enter, false );
	h.BeginFrame(); h.EndFrame();

	REQUIRE_FALSE( ui::IsModalOpen() );
	REQUIRE( nPrimary == 1 );
}

// =========================================================================
//  Dropdown -- the click-driven half. DropdownPopupRect()'s pure geometry
//  and the IsDropdownPopupOpen()/CloseDropdownPopup() bookkeeping are
//  tested without a context in test_overlay_ui.cpp; this is open/commit/
//  cancel through a live frame, the same split ListBox and Modal already
//  use above.
// =========================================================================
namespace
{
	// profiles.inherits' own option set (PanelConfig.cpp), small enough to
	// reuse verbatim.
	constexpr ui::Option kInheritOptions[] = {
		{ 0, "None" }, { 1, "Casual" }, { 2, "Comp" }, { 3, "Profile1" },
	};

	// Replicates DrawDropdownPopup()'s own file-local minimum content width
	// -- the same "replicate the internal geometry to compute a click
	// target" approach ModalPrimaryButtonRect() above already uses for
	// DrawModal()'s footer.
	float InheritPopupMinWidth()
	{
		float flW = ui::Px( 180.0f );
		for ( const ui::Option &o : kInheritOptions )
			flW = std::max( flW, ui::MeasureText( ui::TypeRole::Value, o.pszLabel ).x + ui::Px( ui::tok::kXL ) * 2.0f );
		return flW;
	}
}

TEST_CASE( "atoms: dropdown click opens the popup", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );

	int nValue = 1;   // "Casual"
	const ui::RowCtx row = MakeRow();
	auto DrawRow = [ & ]
	{
		return ui::controls::Dropdown( row, "inherits1", &nValue,
		                               kInheritOptions, IM_ARRAYSIZE( kInheritOptions ) );
	};

	h.BeginFrame(); DrawRow(); h.EndFrame();
	REQUIRE_FALSE( ui::IsDropdownPopupOpen() );

	const ImRect rcBox = row.PlaceFull();
	h.MoveMouse( rcBox.GetCenter() );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame();
	const ui::controls::DropdownResult res = DrawRow();
	h.EndFrame();

	REQUIRE( ui::IsDropdownPopupOpen() );
	REQUIRE_FALSE( res.bChanged );   // opening never changes the value by itself
	REQUIRE( nValue == 1 );

	ui::CloseDropdownPopup();   // leave shared state clean for the next test
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );
	h.BeginFrame(); h.EndFrame();
}

TEST_CASE( "atoms: dropdown click on an item commits once", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );

	int nValue = 0;   // "None"
	const ui::RowCtx row = MakeRow();
	auto DrawRow = [ & ]
	{
		return ui::controls::Dropdown( row, "inherits2", &nValue,
		                               kInheritOptions, IM_ARRAYSIZE( kInheritOptions ) );
	};
	const ImRect rcBox  = row.PlaceFull();
	const ImRect rcSlab( ImVec2( 0.0f, 0.0f ), ImGui::GetIO().DisplaySize );

	// Open it.
	h.MoveMouse( rcBox.GetCenter() );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	REQUIRE( ui::IsDropdownPopupOpen() );

	// The list's own rect -- DropdownPopupRect() is the one function that
	// decides this, shared by production and this click target.
	const ImRect rcPopup = ui::controls::DropdownPopupRect( rcBox, rcSlab, InheritPopupMinWidth(),
	                                                        IM_ARRAYSIZE( kInheritOptions ), 8 );
	const float flRowH = ui::Px( ui::tok::kControlH );
	const ImVec2 vItem1( rcPopup.GetCenter().x, rcPopup.Min.y + flRowH * 1.5f );   // "Casual"

	h.MoveMouse( vItem1 );
	h.BeginFrame(); ui::DrawDropdownPopup( rcSlab ); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); ui::DrawDropdownPopup( rcSlab ); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); ui::DrawDropdownPopup( rcSlab ); h.EndFrame();

	// The popup closes the moment it commits.
	REQUIRE_FALSE( ui::IsDropdownPopupOpen() );

	// Controls.h's documented one-frame-late commit: *pnValue changes on
	// the OWNING control's next call, not the frame of the click itself.
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );
	h.BeginFrame();
	const ui::controls::DropdownResult res = DrawRow();
	h.EndFrame();

	REQUIRE( res.bChanged );
	REQUIRE( nValue == 1 );   // "Casual"

	// Exactly once -- a further frame with nothing pending changes nothing.
	h.BeginFrame();
	const ui::controls::DropdownResult res2 = DrawRow();
	h.EndFrame();
	REQUIRE_FALSE( res2.bChanged );
}

TEST_CASE( "atoms: dropdown Esc cancels", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );

	int nValue = 2;   // "Comp"
	const ui::RowCtx row = MakeRow();
	auto DrawRow = [ & ]
	{
		return ui::controls::Dropdown( row, "inherits3", &nValue,
		                               kInheritOptions, IM_ARRAYSIZE( kInheritOptions ) );
	};
	const ImRect rcBox  = row.PlaceFull();
	const ImRect rcSlab( ImVec2( 0.0f, 0.0f ), ImGui::GetIO().DisplaySize );

	h.MoveMouse( rcBox.GetCenter() );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	REQUIRE( ui::IsDropdownPopupOpen() );

	ImGui::GetIO().AddKeyEvent( ImGuiKey_Escape, true );
	h.BeginFrame(); ui::DrawDropdownPopup( rcSlab ); h.EndFrame();
	ImGui::GetIO().AddKeyEvent( ImGuiKey_Escape, false );
	h.BeginFrame(); h.EndFrame();

	REQUIRE_FALSE( ui::IsDropdownPopupOpen() );

	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.BeginFrame();
	const ui::controls::DropdownResult res = DrawRow();
	h.EndFrame();
	REQUIRE_FALSE( res.bChanged );
	REQUIRE( nValue == 2 );   // unchanged
}

TEST_CASE( "atoms: dropdown click outside cancels", "[overlay_atoms]" )
{
	ScopedScale s( 1.0f );
	Headless &h = Headless::Get();
	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.MouseButton( false );

	int nValue = 0;   // "None"
	const ui::RowCtx row = MakeRow();
	auto DrawRow = [ & ]
	{
		return ui::controls::Dropdown( row, "inherits4", &nValue,
		                               kInheritOptions, IM_ARRAYSIZE( kInheritOptions ) );
	};
	const ImRect rcBox  = row.PlaceFull();
	const ImRect rcSlab( ImVec2( 0.0f, 0.0f ), ImGui::GetIO().DisplaySize );

	h.MoveMouse( rcBox.GetCenter() );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( true );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); DrawRow(); h.EndFrame();
	REQUIRE( ui::IsDropdownPopupOpen() );

	// Clear of both the box and the popup it dropped.
	const ImVec2 vOutside( 4.0f, ImGui::GetIO().DisplaySize.y - 4.0f );
	h.MoveMouse( vOutside );
	h.BeginFrame(); h.EndFrame();

	h.MouseButton( true );
	h.BeginFrame();
	ui::DismissDropdownOnOutsideClick( rcSlab );
	h.EndFrame();
	h.MouseButton( false );
	h.BeginFrame(); h.EndFrame();

	REQUIRE_FALSE( ui::IsDropdownPopupOpen() );

	h.MoveMouse( ImVec2( 4.0f, 4.0f ) );
	h.BeginFrame();
	const ui::controls::DropdownResult res = DrawRow();
	h.EndFrame();
	REQUIRE_FALSE( res.bChanged );
	REQUIRE( nValue == 0 );   // unchanged
}
