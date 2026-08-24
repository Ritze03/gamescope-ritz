// The shell's geometry -- SPEC.md §8.1's three regions and §8.3's responsive
// ladder, as pure arithmetic.
//
// This file is the P2 counterpart of what Lane.h is to P1: the one place a
// region's size is decided, kept free of imgui.h so it can be pinned by a unit
// test that never opens a window. Shell.cpp turns the rects below into ImGui
// child windows and draws in them; it does not compute a single one of them.
//
// The three quantities that live here and nowhere else:
//
//   1. THE SLAB      -- min(surfW x 0.90, max(1560 x scale, 1180))
//                       x min(surfH x 0.86, 940 x scale)          (SPEC §8.1)
//   2. THE LADDER    -- rail 232 -> 60, inspector column -> drawer -> hidden,
//                       from ONE comparison applied twice          (SPEC §8.3)
//   3. THE REGIONS   -- rail / sheet / inspector / spine rects derived from
//                       (1) and (2), so they cannot disagree about a boundary.
//
// Plus one decision that is arithmetic in disguise: which Inspector mode a
// selection opens in (SPEC §5.1, "automatic and stateless").
//
// PROVENANCE. Every number below is transcribed from index.html's `ladder()`
// (the mockup is the tiebreaker per the design brief) and cross-checked
// against SPEC §8.3's table. Where the two agree the number is uncommented;
// where SPEC.md is silent and only the mockup decides, there is a comment.
#pragma once

#include "Lane.h"       // the per-column lane LayOutSheetColumns() returns
#include "Registry.h"   // ui::Kind, for ModeFor(). Registry.h is imgui-free.

namespace gamescope::ui
{
	// A rectangle in physical pixels, window-space. Deliberately not ImRect:
	// that type lives in imgui_internal.h and this header must stay usable
	// from a test that links no ImGui at all. Shell.cpp converts.
	struct Rect
	{
		float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

		float Width()  const { return x1 - x0; }
		float Height() const { return y1 - y0; }
		bool  Empty()  const { return x1 <= x0 || y1 <= y0; }
		bool  Contains( float x, float y ) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
	};

	// SPEC §8.1 / §8.05. The Inspector is one region in three hosts, never
	// three regions: `Column` sits in the flow and takes width from the sheet;
	// `Drawer` overlays the sheet's right edge; `Hidden` leaves only the
	// 20-base spine (E1's, adopted verbatim -- SPEC §8.05).
	enum class InspectorHost : unsigned char { Column, Drawer, Hidden };

	// SPEC §5.1, after the 2026-08-23 amendment: three modes became two.
	enum class InspectorMode : unsigned char { Configure, Details };

	// ---- the shell's fixed budget (SPEC §8.3) ---------------------------
	// Base units, i.e. px at display_scale 1.0. Named here because the ladder
	// is the only consumer and a magic 232 in Shell.cpp would be exactly the
	// class of number Tokens.h's rule 1 exists to forbid.
	namespace shelltok
	{
		inline constexpr float kRailFull   = 232.0f;
		inline constexpr float kRailIcons  =  60.0f;
		inline constexpr float kInspector  = 400.0f;
		inline constexpr float kSheetMin   = 560.0f;
		inline constexpr float kSpine      =  20.0f;   // SPEC §8.05's "20-base vertical spine"

		inline constexpr float kSlabBaseW  = 1560.0f;  // the 1.0x slab
		inline constexpr float kSlabMinW   = 1180.0f;  // the floor the max() sets
		inline constexpr float kSlabBaseH  =  940.0f;
		inline constexpr float kSurfFracW  =   0.90f;
		inline constexpr float kSurfFracH  =   0.86f;

		// Chrome heights, base units, from index.html's CSS.
		inline constexpr float kSlabBar    =  40.0f;   // .slabbar
		inline constexpr float kSheetHead  =  56.0f;   // .sheethead
		inline constexpr float kSheetFoot  =  40.0f;   // .sheetfoot
		inline constexpr float kModeStrip  =  56.0f;   // .modestrip

		// Column-count thresholds, base units of sheet width. index.html's
		// `byWidth = sheet>=1680 ? 3 : sheet>=840 ? 2 : 1`. SPEC §8.3 prints
		// the resulting column counts but never these two numbers, so the
		// mockup is the only source -- hence the named constants.
		inline constexpr float kThreeColMin = 1680.0f;
		inline constexpr float kTwoColMin   =  840.0f;

		// SPEC §8.3: "columns = min( widthAllows, ceil( rows / 12 ) )".
		inline constexpr int   kRowsPerColumn = 12;

		// One Inspector section label (VALUES, PARAMETERS, LIVE) plus the
		// gap under it. Named because ConfigureRowsHeight() and the drawing
		// code in Shell.cpp must agree on it -- a bare 20 in both places is
		// two numbers that only look like one.
		inline constexpr float kSectionLine = 20.0f;

		// The Inspector title's line plus the gap under it, in DrawConfigure
		// and DrawDetails alike. Same reasoning as kSectionLine.
		inline constexpr float kTitleLine = 24.0f;
	}

	// The slab, in both units. Base is what the ladder reasons in; px is what
	// gets drawn. Computed once so the two can never be derived separately.
	struct Slab
	{
		float flScale    = 1.0f;
		float flWidthPx  = 0.0f, flHeightPx = 0.0f;
		float flWidthBase= 0.0f, flHeightBase = 0.0f;

		// SPEC §8.1's slab formula, verbatim.
		static Slab For( float flSurfaceWPx, float flSurfaceHPx, float flScale );
	};

	// The ladder's answer for one frame.
	struct LadderResult
	{
		float flRailBase      = shelltok::kRailFull;
		float flInspectorBase = shelltok::kInspector;
		InspectorHost eHost   = InspectorHost::Column;
		float flSheetBase     = 0.0f;   // the sheet's own width, after the host takes its cut
		int   nColumns        = 1;      // after the content cap
		int   nWidthColumns   = 1;      // before the content cap -- what the width alone allows
		int   nStep           = 0;      // SPEC §8.3's "Step" column: -1 .. 3

		bool RailIsIcons() const { return flRailBase <= shelltok::kRailIcons; }
	};

	// SPEC §8.3. `ePreferred` is the user's own Ctrl+I choice, which the
	// ladder may OVERRIDE downward but never upward: asking for a column on a
	// slab that cannot seat one yields a drawer, while asking for `Hidden`
	// is always honoured (that is what keeps step 3 -- and with it the
	// Reachability Law -- exercised daily rather than only on a 2.0x machine).
	//
	// nRowsInArea feeds the content cap; pass 0 for an area with no rows (a
	// P2 escaped panel), which yields one column.
	//
	// `bUnsplittable` forces one column regardless of width or row count.
	// Two kinds of area need it and both would otherwise be drawn wrong
	// rather than merely tightly: an ESCAPED legacy panel (it lays itself
	// out with ImGui's own cursor and has never heard of a column), and an
	// area with a CONTENT body (Area::Content() -- the Log's line list),
	// which is one scrolling body under the rows and has no meaningful way
	// to be cut in half. Answering it HERE rather than at the drawing site
	// is the point: `shell.layout` prints Solve()'s number, so a number
	// decided anywhere else would be a printed column count that disagrees
	// with the screen -- which is the exact defect D20.2 exists to remove.
	LadderResult Solve( const Slab &slab, InspectorHost ePreferred, int nRowsInArea,
	                    bool bUnsplittable = false );

	// SPEC §8.3's three-column ceiling.
	inline constexpr int kMaxSheetColumns = 3;

	// One sheet column: where it is, and the lane its rows are bound to.
	struct SheetColumn
	{
		Rect rc;     // physical px, x relative to the sheet body's own x0
		Lane lane;   // base units, already reduced by any occlusion
	};

	struct SheetColumnSet
	{
		int         nColumns = 1;
		SheetColumn cols[ kMaxSheetColumns ];
	};

	// THE ONE PLACE A SHEET COLUMN'S GEOMETRY IS DECIDED (D20.2).
	//
	// Column width follows index.html's own formula, which is the tiebreaker
	// the design brief names:
	//
	//     colW = ( sheet - 2 x pad - (cols - 1) x gutter ) / cols
	//
	// with pad and gutter both the sheet's 24-unit pad.
	//
	// WHY THE OCCLUSION IS PER COLUMN AND NOT A SHEET-WIDE SUBTRACTION.
	// D17 pulls the lane's right edge in when the Inspector is a drawer
	// floating over the sheet. With one column that was a single
	// subtraction; with three it is a question each column answers
	// separately, because the drawer covers the RIGHTMOST column entirely
	// and may not touch the leftmost at all. So the occlusion is computed
	// from each column's own right edge against the drawer's left edge --
	// which reduces to exactly D17's single subtraction when nColumns is 1,
	// and is why the two rules are one rule rather than two that can drift.
	//
	// flOccludedRightPx is how much of the BODY's right edge the drawer
	// covers; 0 in every case but that one.
	SheetColumnSet LayOutSheetColumns( float flBodyWPx, float flOccludedRightPx,
	                                   int nColumns, float flScale );

	// The four region rects, in physical pixels relative to the slab's own
	// top-left (0,0). Derived from one Slab and one LadderResult, so no two
	// regions can disagree about where the boundary between them is.
	struct Regions
	{
		Rect rcSlabBar;     // the 40-base title strip across the top
		Rect rcBody;        // everything below it -- the three regions' container
		Rect rcRail;
		Rect rcSheet;       // the WHOLE sheet region (head + body + foot)
		Rect rcSheetHead;
		Rect rcSheetBody;   // where a category's rows -- or an escaped panel -- draw
		Rect rcSheetFoot;
		Rect rcInspector;   // empty when the host is Hidden
		Rect rcModeStrip;   // empty when the host is Hidden
		Rect rcInspectorBody;
		Rect rcSpine;       // empty UNLESS the host is Hidden

		static Regions For( const Slab &slab, const LadderResult &ladder );
	};

	// SPEC §5.1: "Mode selection is automatic and stateless: selecting a
	// Facts, Meter or Graph row opens Details; everything else opens
	// Configure." One function, because the shell asks this in two places
	// (selection change, and the palette's jump) and a second copy would be a
	// second answer.
	//
	// `Graph` is a CompositeKind, not a Kind, so a composite has to be asked
	// about both -- hence the two-argument form.
	InspectorMode ModeFor( Kind eKind, CompositeKind eComposite );

	// The mode strip's two counters (SPEC §5.1: "each carries the count of
	// what it holds, so the strip stays ... visible proof of how much is (and
	// is not) hiding behind the row"). Configure counts the row's own control
	// plus its Params; Details counts the binding-grid facts plus .Live()
	// readouts. `bReadOnly` marks Configure's `ro` form.
	struct ModeCounts
	{
		int  nConfigure = 0;
		int  nDetails   = 0;
		bool bReadOnly  = false;
	};
	ModeCounts CountsFor( const Entry &entry );

	// ---- how tall a CONFIGURE body's rows are (P3b) ---------------------
	// The fixed-height part of the Inspector's CONFIGURE body: the VALUES
	// band (its section label plus the entry's own row) and, when the entry
	// has parameters, the PARAMETERS band (its label plus one row each).
	//
	// WHY THIS IS A FUNCTION AND NOT A COMMENT. P3a shipped an Inspector
	// that could not scroll, and the way it was found was a screenshot --
	// at 2.0x an entry with six params ran off the bottom of the drawer and
	// the last row was simply not there. Nothing failed; the pixels just
	// stopped. Expressing the row arithmetic here makes "this body is
	// taller than the region it draws into" a comparison a unit test can
	// make against Regions::rcInspectorBody, with no window open.
	//
	// It is a LOWER BOUND, deliberately. The title, the help paragraph and
	// any disabled reasons all need font metrics to measure, and this
	// header is imgui-free on purpose. Excluding them can only make the
	// answer too small, which is the safe direction: if even the lower
	// bound overflows, the real body certainly does.
	float ConfigureRowsHeight( int nParams, float flScale );

	// ---- how far the rail is scrolled (P5) ------------------------------
	// The rail's item column is taller than the rail itself from 2.0x: at
	// that scale eleven areas and three section breaks do not fit, and
	// before P5 the surplus was drawn past the rail's bottom edge and lost,
	// leaving Appearance and Shell unreachable by pointer.
	//
	// Given the current offset and where the ACTIVE item sits, answer the
	// offset the next frame should use: unchanged when the item is already
	// fully visible, otherwise the smallest scroll that brings it back --
	// then clamped into [0, contentH - viewH]. All distances are rail-
	// relative physical px, so flActiveTop is the item's top measured from
	// the rail's own y0.
	//
	// WHY THIS IS A FUNCTION AND NOT FOUR LINES IN DrawRail. Same argument
	// as ConfigureRowsHeight above: the clamp is the part that can be
	// wrong, and it is wrong in a way that only a screenshot at one
	// particular scale would show. As a pure function it is a table of
	// cases a unit test can walk with no window open -- including the two
	// that matter most, "content fits, so never scroll" and "scrolled to
	// the bottom stays pinned to the bottom".
	//
	// Pass flActiveTop < 0 when no item is active; the offset is then only
	// clamped, never moved.
	float RailScroll( float flCurrent, float flContentH, float flViewH,
	                  float flActiveTop, float flItemH, float flPad );

	// ---- where the palette / launcher panel sits (D27) -------------------
	// THE PANEL'S TOP EDGE IS A FUNCTION OF THE GEOMETRY AND OF NOTHING
	// ELSE. That sentence is the whole fix, and the parameter list below is
	// how it is enforced.
	//
	// The user's report: *"make sure that it is vertically centered, in its
	// initial state ... do not dynamically move it according to the current
	// height ... So it doesnt start moving the search bar, all of the
	// sudden"*. A panel centred on its CURRENT height slides up and down
	// under the reader as results filter, and the query line -- the one
	// thing they are looking at while typing -- moves with it.
	//
	// So the panel is centred at its MAXIMUM row count, and that position is
	// kept whatever the list actually holds. The list then grows and shrinks
	// DOWNWARD inside a frame whose top edge never moves.
	//
	// WHY A PURE FUNCTION AND NOT A CACHED y0. A cache would need an
	// invalidation rule ("recompute on display_scale, on surface size, but
	// not on the match count"), and that rule is a thing to get wrong later.
	// Here the rule is the SIGNATURE: the inputs are the frame, the row
	// metrics (which are scale-derived) and the CAP. The number of matches
	// is not an input, so no amount of typing can move the answer. Recompute
	// it every frame; it cannot drift.
	//
	// `nRowCap` is the list's own row cap -- the most rows the panel will
	// ever show -- not the current match count.
	struct PalettePanel
	{
		float flTop    = 0.0f;  // the panel's top edge == the query line's y
		int   nMaxRows = 1;     // rows this fixed frame has room for
		float flMaxH   = 0.0f;  // the panel's height at nMaxRows
		bool  bFits    = true;  // false: not even ONE row fits -- see below
	};

	// The 2.0x rule, stated once here so nobody has to infer it:
	//
	//   * The maximum is the FITTING maximum. Where the cap's worth of rows
	//     does not fit -- 2.0x on a short surface -- nMaxRows drops to what
	//     does, and the panel is centred at THAT height. It is still centred
	//     once and still never moves; it is simply a shorter panel.
	//   * Where not even one row fits, `bFits` is false and the panel is
	//     anchored at the top margin instead of centred. Centring a panel
	//     taller than its frame pushes the query line off the TOP edge --
	//     losing the thing you type into. Top-anchored, the query line and
	//     the first result stay on screen and the footer legend is what
	//     falls off the bottom, which is the cheaper loss.
	PalettePanel SolvePalettePanel( float flFrameY0, float flFrameH,
	                                float flQueryH, float flRowH, float flFootH,
	                                float flMarginPx, int nRowCap );

	// ---- laying out INSIDE a scrolling child (P5b) -----------------------
	// THE ONE MECHANISM BY WHICH ANY BODY IN THIS SHELL SCROLLS. Read this
	// before writing a new body; getting it wrong is silent.
	//
	// Every body here -- the sheet's rows, the Inspector's, the explain
	// page's, a content area's -- lays out by painting at an absolute `y`
	// rather than by walking ImGui's cursor. That is deliberate and is what
	// keeps the row grammar and the lane identical everywhere (SPEC §5.3: a
	// promoted parameter has to land in the sheet unchanged). But an
	// absolute y has a trap in it, and the shell fell into it twice:
	//
	//   * If the y starts at the REGION's y0 -- a fixed screen coordinate --
	//     then nothing moves when the child scrolls. ImGui applies scroll by
	//     moving the child's CURSOR, not by translating the draw list, so a
	//     body that ignores the cursor is nailed to the screen. The
	//     scrollbar still slides, because the invisible buttons the rows
	//     submit push the content extent out; only the pixels stay put. That
	//     is exactly the "scrollbar moves, content doesn't" report.
	//   * If nothing ever tells ImGui how tall the result was, there is no
	//     scroll range at all and the surplus is simply drawn past the
	//     bottom edge and lost.
	//
	// Both halves are one rule: START at the child's own cursor, and HAND
	// BACK the height you used. This struct is that rule as arithmetic --
	// `Begin()` before drawing, `ContentHeight()` after.
	//
	// WHY IT LIVES HERE AND NOT AS FOUR LINES AT EACH CALL SITE. It was four
	// lines at each call site: P3b wrote them into the Inspector body and
	// the explain page, and the sheet -- the biggest scrolling region in the
	// shell -- never got them. A fix that has to be remembered per region is
	// a fix that is missing from the next region. As a named function it is
	// also imgui-free, so the translation and the clamp are a table a unit
	// test walks with no window open.
	struct ScrollView
	{
		float flOriginY = 0.0f;   // the child's cursor y THIS frame: region y0 - scroll
		Rect  rcBody;             // the region rect, rebased onto flOriginY

		// `rcRegion` is where the region sits on screen; `flCursorY` is what
		// ImGui::GetCursorScreenPos().y returns just inside the child. The
		// body's height is preserved exactly -- only its origin moves.
		static ScrollView Begin( const Rect &rcRegion, float flCursorY );

		// What to hand ImGui as the child's content extent, given the y the
		// body's last row ended at. Never negative -- an empty body must not
		// ask for a negative scroll range -- and always includes the trailing
		// pad, so a fully-scrolled body does not end flush against the frame.
		float ContentHeight( float flBottomY, float flPad ) const;
	};
}
