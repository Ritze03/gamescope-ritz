// The control atoms -- direction B's controls, uplifted ~25%, reconciled with
// E2's lane rule. SPEC.md §3 is the geometry; index.html's `/* --- CONTROLS
// --- */` block is the tiebreaker where the prose is ambiguous.
//
// EVERY ATOM HAS THE SAME SHAPE:
//
//     bool Atom( const RowCtx &row, const char *pszId, <binding>, ... );
//
// It asks the row for a rect, registers THAT RECT with ImGui, and draws inside
// THAT RECT. There is no x parameter, no width parameter and no alignment
// parameter anywhere in this header -- per-control widths are constants in
// Controls.cpp, not a caller's choice (SPEC §2.2), and the right edge is the
// lane's, always.
//
// The atoms are built on ImGui's own ButtonBehavior() / SliderBehavior() /
// ItemAdd() / RenderNavCursor(), exactly as Widgets.cpp already does, so
// hit-testing, keyboard navigation, disabled semantics and ID scoping stay
// stock and this file only decides what things look like.
//
// ---------------------------------------------------------------------------
// THE DRAWN-VS-HIT-TESTED DIVERGENCE, AND WHY IT CANNOT HAPPEN HERE
// ---------------------------------------------------------------------------
// Shipping issue #23 found a real instance of a whole bug class: a slider drew
// its handle from one constant while SliderBehavior() computed the draggable
// grab from style.GrabMinSize -- a second constant. At display_scale 1.0 they
// agreed; at every other scale they silently did not, and the visible handle
// stopped being the thing you could grab.
//
// Two structural rules remove the class rather than the instance:
//
//   1. ONE RECT PER ATOM. The rect handed to ItemAdd() is the same C++ object
//      handed to the painter. An atom never recomputes its own geometry.
//   2. THE SLIDER'S HANDLE IS NOT A CONSTANT. SliderGrab() below is the only
//      code in the kit that names a grab width; it pushes that width into
//      GrabMinSize, calls SliderBehavior(), and returns the grab rect
//      SliderBehavior itself produced. The painter draws that rect. There is
//      no second number, so there is nothing to keep in step.
//
// The same rule applies to the measured atoms: MeasureCells() is the single
// function that decides how wide a segmented group or a chip bank is, and both
// the "does it fit the lane" predicate and the per-cell layout read its output.
// A control cannot be laid out to a width its fit test never saw.
#pragma once

#include "Icons.h"
#include "Registry.h"
#include "Row.h"
#include "Tokens.h"

#include <functional>
#include <string>
#include <vector>

namespace gamescope::ui
{
	// ---- text, inside a rect the allocator already chose ------------------
	// This alignment is text-within-its-own-rect, never control placement:
	// the rect always came from RowCtx, so nothing here can move a control.
	enum class TextAlign : unsigned char { Left, Right, Center };

	ImVec2 MeasureText( TypeRole eRole, const char *pszText, const char *pszEnd = nullptr );
	void   DrawText( const ImRect &rcClip, TypeRole eRole, ImU32 col, const char *pszText,
	                 TextAlign eAlign = TextAlign::Left );

	// ---- drawn glyphs (D18) -----------------------------------------------
	// The shell's icons are STROKED PATHS, not characters. This is a decision
	// with a hard constraint behind it, so it is recorded here rather than
	// left to look like a preference.
	//
	// Fonts.cpp bakes Basic Latin + Latin-1 Supplement, and the shell wants
	// three marks the mockup draws as `▸` (U+25B8), `⌕` (U+2315) and `›`
	// (U+203A). Widening the baked range fixes exactly one of them: the
	// bundled Geist faces -- all five, Sans and Mono -- HAVE no U+25B8 and no
	// U+2315 in their cmap. A wider range would bake a fallback box just as
	// faithfully as a narrow one does.
	//
	// So the choice was never "atlas vs draw" for these glyphs; it was "draw,
	// or ship a different typeface". Drawing also costs nothing at bake time,
	// which matters more here than usual: the atlas is rebuilt per effective
	// scale, so every added range is paid again at every scale change.
	//
	// The knock-on benefit is that the two ASCII stand-ins the shell had been
	// using -- the palette's `>` prompt and the dropdown's lowercase `v`
	// caret -- become the marks they were standing in for. Both were letters
	// pretending to be icons, and both scaled and hinted like letters.
	namespace glyph
	{
		enum class Dir : unsigned char { Right, Left, Up, Down };

		// A chevron centred on vCenterPx, flSizePx across its box. Stroked,
		// never filled: at 0.5x a filled triangle collapses into a blob while
		// a stroke stays two readable lines.
		void Chevron( ImVec2 vCenterPx, float flSizePx, Dir eDir, ImU32 col );

		// The palette's magnifier -- a circle and a handle, which is all the
		// mark has ever been.
		void Magnifier( ImVec2 vCenterPx, float flSizePx, ImU32 col );

		// SPEC §2.4's read-only mark. Drawn, for the same reason the chevron
		// is: U+2337 is not in any bundled face's cmap, so a baked range
		// cannot produce it (AUTONOMOUS-DECISIONS D18.2).
		void Lock( ImVec2 vCenterPx, float flSizePx, ImU32 col );

		// SPEC §8.0's rail glyph, centred on vCenterPx with flBoxPx as the
		// 24-unit grid's edge. The Icon comes from Icons.h; this function
		// contains no coordinates and knows nothing about which glyph it is
		// drawing (D20.1).
		void RailIcon( const Icon &icon, ImVec2 vCenterPx, float flBoxPx, ImU32 col );
	}

	// =========================================================================
	//  Modal -- SPEC gap: the Profiles rebuild's Create/Copy/Edit/Delete
	//  dialogs. A small centred dialog over the sheet, reusing the palette's
	//  own scrim (Shell.cpp's DrawPalette -- see DrawModal()'s definition for
	//  the exact fill it repeats rather than reinvents) so the shell never
	//  grows a second "surface behind me is dimmed" look.
	//
	//  WHY THIS SITS OUTSIDE `controls::` AND NOT INSIDE IT. Every atom in
	//  that namespace draws into a rect an existing allocator (RowCtx, or a
	//  Composite's own rcBody) already produced. A modal has no such parent --
	//  it is a whole SCREEN-LEVEL surface, the same job DrawPalette() already
	//  does for the command palette -- so it belongs beside DrawText()/
	//  MeasureText()/glyph:: at this file's top level, not among the row
	//  atoms.
	//
	//  THE BODY IS ORDINARY ROWS. ModalBodyCtx::NextRow() returns exactly the
	//  RowCtx RowCtx::ForRow() gives the sheet, built from a Lane over the
	//  modal's own content width -- so a caller draws its fields with the
	//  unmodified control atoms above (controls::Switch, controls::Text, ...)
	//  and gets the sheet's own row grammar for free, rather than a second,
	//  modal-only layout language.
	// =========================================================================
	struct ModalBodyCtx
	{
		ImRect rcBody;             // the modal's content rect, already inset from its chrome
		Lane   lane;               // ForColumn() over rcBody's width -- built once, before fnBody runs
		float  flCursorY = 0.0f;   // next row's top, screen space; starts at rcBody.Min.y
	};

	// Advances ctx.flCursorY by one row and returns the RowCtx for it -- a
	// free function rather than a ModalBodyCtx method because RowCtx::ForRow()
	// is itself a free function of a Lane, an origin and a top, and this is
	// nothing more than remembering the top between calls.
	RowCtx ModalNextRow( ModalBodyCtx &ctx );

	// A free-form block below the last row -- the Delete confirmation's
	// prompt paragraph -- at whatever height the caller measured for it.
	ImRect ModalNextBlock( ModalBodyCtx &ctx, float flHeightPx );

	struct ModalSpec
	{
		std::string sTitle;
		std::string sPrimaryLabel;              // "Create" / "Copy" / "Save" / "Delete"
		bool        bPrimaryDanger = false;     // red-tinted primary -- Delete's confirmation

		// Draws the body: one or more controls::Switch/Text/etc. calls, each
		// against a RowCtx from ModalNextRow(ctx), or a Facts-style paragraph
		// drawn into ModalNextBlock(ctx, height). Re-run every frame the modal
		// is open, so a body that reads a live *pbGameSpecific and only calls
		// ModalNextRow() twice when it is off, three times when it is on, is
		// exactly how the sketch's "(when on) GameID + New profile name" is
		// expressed -- there is no separate "declare N rows" step.
		std::function<void( ModalBodyCtx & )> fnBody;

		// Invoked once, after the modal has already closed, when the primary
		// button fires (a click, or Enter while no field is being edited --
		// see DrawModal()'s own comment for why "the LAST Text field" could
		// not be told apart from any other without a focus-order system this
		// kit does not have).
		std::function<void()> fnPrimary;

		// Invoked once, after the modal has already closed, on Cancel or Esc.
		// Optional -- most callers have nothing to undo.
		std::function<void()> fnCancel;

		// OPTIONAL: how tall the body will measure on its very first frame,
		// in rows (kRowH each) -- e.g. 3.0f for the sketch's Switch + two
		// Text fields. DrawModal() auto-sizes to whatever fnBody actually
		// drew, but only AFTER a frame has measured it (see DrawModal()'s
		// own comment on why: measuring by drawing twice in one frame is not
		// safe with these atoms' ID/ItemAdd model), so an unhinted dialog
		// opens ONE ROW TALL and grows into shape a frame later. That is
		// invisible against a live, continuously-redrawn overlay -- but a
		// caller that already knows its own row count (every real caller
		// here does: Create/Copy/Edit know their field count from
		// *pbGameSpecific, Delete's confirmation is always the same shape)
		// can skip the flash by stating it. 0 (default) keeps the old
		// one-row-then-grow behaviour.
		float flMinBodyRows = 0.0f;
	};

	// Only one modal at a time. A second OpenModal() while one is already
	// open is a programming error -- IM_ASSERT() in a debug/assertions build,
	// silently ignored (the first modal's spec is untouched) otherwise.
	void OpenModal( ModalSpec spec );

	// One frame of whatever modal is open, or nothing at all when none is.
	// Call from Shell's Draw(), in its own top-level window, opened AFTER the
	// slab and BEFORE the palette -- exactly DrawPalette()'s own placement
	// reasoning (Shell.cpp: a child window's draw list posts after its
	// parent's regardless of submission order, so a sibling window opened
	// later is the only thing guaranteed to paint over both). `rcSlab` is the
	// surface the dialog centres inside and the scrim covers, in screen space.
	void DrawModal( const ImRect &rcSlab );

	// Closes whatever modal is open without invoking either callback. Esc and
	// the two footer buttons already do this internally; exposed for a caller
	// that needs to withdraw a modal it opened for a reason that stopped
	// applying (e.g. the row it was editing disappeared from a rebuilt area).
	void CloseModal();

	bool IsModalOpen();

	namespace controls
	{
		// ---- the pointer drag, and writes that must wait for it to end ----
		// A control's binding is written on EVERY frame of a drag, and for
		// almost everything that is the point: volume, opacity and the accent
		// hue are all meant to follow the pointer.
		//
		// For a setting that RESIZES THE UI ITSELF it is a bug instead. The
		// track is laid out from the very scale being dragged, so applying the
		// value live slides the slider out from under the pointer -- the user,
		// 2026-08-24: *"The UI scale should update, when the slider is
		// released. Otherwise, it is almost impossible, to adjust."*
		//
		// So a binding may hand the reflowing half of its write to
		// DeferToRelease() while ui::IsPointerDragActive() (Registry.h) is
		// true. The pending callable runs exactly once, on the first frame no
		// pointer drag is in flight.
		//
		// WHY THE FLUSH LIVES IN THE SHARED ATOM PROLOGUE and not in the
		// slider: a drag can end anywhere -- the pointer leaves the row, the
		// value stops changing so the slider stops calling Set(), the sheet
		// scrolls. Every atom on screen runs that prologue every frame, so the
		// flush cannot be stranded by where the release happened, and it still
		// cannot run mid-drag because the condition is "nothing is held".
		//
		// Only one write can be pending at a time; a second DeferToRelease()
		// replaces the first, which is what a drag wants -- the last value the
		// drag produced is the one that should land.
		//
		// The drag FLAG deliberately lives in Registry.h, not here: its
		// readers are registrations, and a registration is also reached from
		// the console thread, where asking ImGui anything is an abort (D19.1).
		void DeferToRelease( std::function<void()> fn );

		// ---- SPEC §3.1 -- every binary in the product ---------------------
		// 40 x 20 track, 16 knob, 20 travel, in a 28-tall hit box. There is no
		// Checkbox in this API and there will not be one.
		bool Switch( const RowCtx &row, const char *pszId, bool *pbValue );

		// ---- SPEC §3.4 -- bounded continuous ------------------------------
		// PlaceFull(): "the track IS the range; it must span the zone."
		bool Slider( const RowCtx &row, const char *pszId, float *pflValue,
		             float flMin, float flMax, float flDefault = 0.0f, bool bHasDefault = false );
		bool SliderInt( const RowCtx &row, const char *pszId, int *pnValue,
		                int nMin, int nMax, int nDefault = 0, bool bHasDefault = false );

		// ---- SPEC §3.5 -- exact or unbounded ------------------------------
		// B's borderless "- +". Carries no number; the number is in the value
		// column, which the row draws.
		//
		// TYPED ENTRY (request #14, 2026-09-05). The number is also
		// EDITABLE: a click on it, or Enter on the focused row, swaps in the
		// same inline input Text() uses (one editor -- see EditField in
		// Controls.cpp), pre-selected so typing replaces. Enter or an outside
		// click commits, Esc reverts. The user's words: *"so the user can
		// enter custom values, instead of having to keep pressing on the
		// buttons"* -- a 320..7680 range at step 8 is hundreds of presses.
		//
		// The row owns the value column, so the row hands the atom the value
		// rect it split (SplitLabelZone's second output) and its unit, and
		// keeps the one bit of editing state exactly as it does for Text.
		// Pass nullptr and the stepper is buttons-only, as before.
		struct StepperEdit
		{
			ImRect      rcValue;              // the value column the row split
			bool       *pbEditing = nullptr;  // the caller's one bit of state
			const char *pszUnit   = nullptr;  // drawn OUTSIDE the field: the user types the number only
		};
		bool Stepper( const RowCtx &row, const char *pszId, int *pnValue,
		              int nMin, int nMax, int nStep = 1, const StepperEdit *pEdit = nullptr );

		// The width the value column needs while a Stepper's number is being
		// typed: the field plus the unit. ONE measurement, two consumers --
		// the row's SplitLabelZone() asks it so the label ellipsizes
		// correctly, and Stepper() lays the field out inside whatever rect
		// that split produced. Same rule as MeasureCells().
		float StepperEditWidthPx( const char *pszUnit );

		// What a typed value becomes. Pure, so the tests can pin it:
		// whole-number parse (surrounding whitespace allowed, nothing else)
		// -> clamp to [nMin, nMax] -> that is the value. Returns false for
		// anything that is not a whole number ("abc", "", "12.5") and leaves
		// *pnOut alone -- the caller keeps the old value, which is what
		// "cancel" means.
		//
		// NO SNAPPING TO Step(). Why: a Stepper's step is the increment the
		// "-"/"+" buttons and the arrow keys move by, not a grid of valid
		// values -- Registry.cpp's Parameter::Step() says so for the drag
		// path ("a Stepper anchored somewhere off its own grid is a
		// documented, wanted state", D13.3: an fps_limit of 144 on a step-10
		// row loads, displays and works), and typed entry exists precisely
		// so a user can reach a value the buttons cannot. Someone who types
		// 144 means 144; someone who types 1603 for a width means 1603. The
		// domain setter behind the binding still owns its own validity
		// (SetFpsLimit's floor, SetCustomWidth's clamp), as it does for
		// every other write path.
		bool ParseClampedInt( const char *pszText, int nMin, int nMax, int *pnOut );

		// ---- SPEC §3.2 / §3.3 -- mutually exclusive -----------------------
		// One helper for both hosts. It MEASURES and auto-downgrades to a
		// dropdown when the segmented group does not fit the lane it actually
		// got; the caller has no say (API.md §12.6).
		struct ChoiceResult
		{
			bool bChanged    = false;  // the value moved (segmented)
			bool bWantsPopup = false;  // the dropdown trigger was activated
			bool bSegmented  = false;  // which form was drawn, for the shell's popup anchor
		};
		ChoiceResult Choice( const RowCtx &row, const char *pszId, int *pnValue,
		                     const Option *pOptions, size_t nOptions, bool bPopupOpen = false );

		// ---- SPEC §3.6 -- free text ---------------------------------------
		// B's value + pencil; clicking swaps in a real input. *pbEditing is
		// the caller's one bit of state; the atom owns the transition.
		bool Text( const RowCtx &row, const char *pszId, std::string *psValue,
		           bool *pbEditing, const char *pszPlaceholder = nullptr,
		           const char *pszError = nullptr );

		// ---- SPEC §3.12 -- a multi-select whose value is a set -------------
		// One setting whose value is a set. N independent binaries are still N
		// switch rows -- that rule, not the bank, is what a reviewer holds.
		// nFocusChip is the index of the chip the KEYBOARD is pointed at, or
		// -1 for none. A bank is the one kind with no ordered value for an
		// arrow key to walk (SPEC §3.12: "one setting whose value is a set"),
		// so the cursor has to be a chip -- and a cursor nobody can see is not
		// a cursor, which is why the ring is the atom's job and not the
		// caller's.
		bool Bank( const RowCtx &row, const char *pszId, uint32_t *pnMask,
		           const Option *pOptions, size_t nOptions, int nFocusChip = -1 );

		// ---- SPEC §3.8 -- a live scalar, read-only ------------------------
		void Meter( const RowCtx &row, float flValue, float flMin, float flMax );

		// ---- SPEC §3.9 -- the verb chip -----------------------------------
		enum class Intent : unsigned char { Accent, Neutral, Danger };
		bool Verb( const RowCtx &row, const char *pszId, const char *pszVerb,
		           Intent eIntent = Intent::Accent, bool bEnabled = true );

		// =====================================================================
		//  ListBox -- SPEC gap: the Profiles rebuild's tall list of saved
		//  profiles (ui-design-guide.md's own "List rows" section already
		//  flags this as undesigned -- no scrolling list appears anywhere in
		//  the handoff). A row-spanning body, like Facts is allowed to be: it
		//  takes the rcBody the CALLER sized and draws entirely inside it.
		// =====================================================================
		struct ListBoxItem
		{
			const char *pszLabel     = nullptr;  // e.g. "Rust" -- never empty
			const char *pszTag       = nullptr;  // e.g. "[Game]"; nullptr draws no tag
			const char *pszSecondary = nullptr;  // e.g. "inherits Comp"; nullptr draws none
		};

		// Pure -- no ImGui, no draw call, testable with a bare int. Up/Down
		// clamp at the ends rather than wrapping (D16.6's rule, applied here
		// too); Home/End jump regardless of the current selection. nCount <= 0
		// always answers -1: an empty list has nothing to select. -1 in means
		// "nothing selected yet"; Up and Down from there both land on the
		// first row, the same "first arrow lands inside the list" rule the
		// sheet's own dropdown nav uses.
		enum class ListBoxNav : unsigned char { Up, Down, Home, End };
		int ListBoxStep( int nSelected, int nCount, ListBoxNav eNav );

		// Pure: the first visible row (`nScrollTop`, updated) that keeps
		// `nSelected` inside a `nVisibleRows`-tall window, clamped so the list
		// never scrolls past its own last page. Called every frame with last
		// frame's answer as `nScrollTop`, exactly like an "ensure visible"
		// scroll in any list widget.
		int ListBoxScrollForSelection( int nScrollTop, int nSelected,
		                               int nVisibleRows, int nCount );

		// Pure geometry for one item's row: where the tag, the label and the
		// secondary text sit, at the priority the sketch implies -- the LABEL
		// is never sacrificed; the secondary is the one dropped
		// (bSecondaryShown false) when the row is too narrow to hold all three
		// without clipping the label to nothing. By construction
		// rcTag.Max.x <= rcLabel.Min.x and rcLabel.Max.x <= rcSecondary.Min.x
		// always -- there is no code path that can make two of these overlap,
		// which is the property tests/test_overlay_ui.cpp checks at the
		// narrowest sheet width the shell allows.
		struct ListBoxItemLayout
		{
			ImRect rcTag;                     // zero-width at the left edge when pszTag is absent
			ImRect rcLabel;
			ImRect rcSecondary;                // zero-width at the right edge when dropped
			bool   bSecondaryShown = false;
		};
		ListBoxItemLayout LayoutListBoxItem( const ImRect &rcItem, float flTagWidthPx,
		                                     float flSecondaryWidthPx, float flPadPx );

		struct ListBoxResult
		{
			bool bChanged   = false;  // *pnSelected moved this frame (click or Up/Down/Home/End)
			bool bActivated = false;  // a click, or Enter on the current selection -- "open/act on this"
		};

		// Draws N items inside rcBody, one row `tok::kControlH` tall each,
		// selection outlined in the accent with no fill (the sketch's own
		// styling), a wire background/frame and no rounding beyond what every
		// other atom in the kit uses.
		//
		// SCROLLING. `nMaxVisibleRows` caps the box before an inner
		// scrollbar/mouse-wheel take over -- capped again by however many
		// whole rows actually fit rcBody's height. The scroll offset is NOT
		// caller state (unlike *pnSelected): it lives in ImGui's own per-ID
		// storage, keyed off pszId, so a caller only ever owns the one int a
		// Choice-style control already asks for. Mouse wheel scrolls while the
		// pointer is over the list and touches nothing outside rcBody, so it
		// cannot fight the sheet's own scrolling.
		//
		// KEYBOARD. Up/Down/Home/End/Enter apply only while the pointer hovers
		// rcBody. This kit deliberately never turns on ImGui's own nav
		// (Shell.cpp's dropdown-nav comment: "the adjust grammar of SPEC §8.2
		// away from the rows"), and a standalone widget has no generic
		// keyboard-focus system to hook a "this list owns the keyboard right
		// now" state into without one being built for it (out of scope here --
		// flagged in this task's report). Hover-to-navigate is therefore the
		// whole of it; a future panel that wants Up/Down to reach the list
		// from elsewhere has to forward those keys itself.
		ListBoxResult ListBox( const ImRect &rcBody, const char *pszId, int *pnSelected,
		                      const ListBoxItem *pItems, size_t nItems,
		                      int nMaxVisibleRows = 10 );

		// ---- SPEC §4.3 -- the anchor grid, a composite body ---------------
		// 3x3 cells on the control module, so the grid agrees with every other
		// control instead of being a foreign object (fix #3).
		bool AnchorGrid( const ImRect &rcBody, const char *pszId, int *pnVert, int *pnHoriz );

		// ===================================================================
		//  The other composite bodies -- SPEC §4.4's table
		// ===================================================================
		// Each takes the rcBody that Band.cpp computed and nothing else: the
		// band decides how tall and how wide, the body only decides what goes
		// inside it. That split is the whole reason two Position Grid call
		// sites cannot drift apart again -- geometry has exactly one author.
		//
		// A body atom never asks ImGui for a cursor, a content region or a
		// window width. If one did, it would be choosing its own size, and
		// Band.cpp's four clauses would stop being the only answer.

		// A gradient rail: a track whose fill is sampled from `fnColorAt`
		// (t in 0..1) rather than a flat colour, with a marker at the current
		// value. Click or drag anywhere on it to set.
		//
		// WHY A COLOUR FUNCTION AND NOT A COLOUR PAIR. Issue #37's accent
		// gradient samples the REAL OklchToImU32() the accent family is built
		// from, so the strip can never be an approximate rainbow that
		// visibly disagrees with the accent it is choosing. Interpolating
		// between two endpoint colours would reintroduce exactly that lie.
		using RailColorFn = ImU32 ( * )( float flT, void *pUser );
		bool Rail( const ImRect &rcRail, const char *pszId, float *pflValue,
		           float flMin, float flMax, RailColorFn fnColorAt, void *pUser = nullptr );

		// ---- SPEC §4.4 -- Accent hue: hue rail + 8 swatches ---------------
		// The swatches are the eight 45-degree stops. They are PRESETS on the
		// same one value the rail sets, not a second setting -- which is why
		// they share *pflHue and return through the same bool.
		bool HueBody( const ImRect &rcBody, const char *pszId, float *pflHue );

		// ---- SPEC §4.4 -- Colour override: R/G/B rails + swatch -----------
		// Plain sRGB, each component 0-255 -- the familiar convention for a
		// new user editing a colour by hand, and request #5 (2026-09-04)
		// replaced this control's former OKLCH L/C/H rails with it for that
		// reason. The accent hue slider (HueBody above) is NOT this control:
		// it stays a single hue driving the derived OKLCH palette, untouched
		// by this change. The caller owns the packing, so the config format
		// (a packed 0xRRGGBB int) is untouched by this control existing.
		bool ColorBody( const ImRect &rcBody, const char *pszId,
		                float *pflR, float *pflG, float *pflB );

		// ---- SPEC §4.4 -- Frametime graph: a 240-sample sparkline ---------
		// Read-only (Entry::ReadOnly() returns true for CompositeKind::Graph),
		// so it takes no binding and returns nothing. Samples are newest-last.
		// `flOutlierMs` marks the threshold above which a bar is drawn in the
		// warn colour; pass 0 to mark none.
		//
		// `nAxisSlots` selects between the TWO graph conventions this product
		// legitimately has, rather than letting a call site improvise either:
		//
		//   0  -- a rolling sparkline. Bars are taken from the TAIL and drawn
		//         right-aligned, so a narrow band shows "right now". This is
		//         the frametime graph's convention.
		//   >0 -- a FIXED axis of that many slots, filled from the LEFT, with
		//         the unfilled remainder left blank. This is issue #40's
		//         explicit requirement for the 60-second statistics graphs:
		//         "a partially-filled window must never read as a complete
		//         one", which is exactly what right-aligning a handful of
		//         samples across the full width would do.
		void GraphBody( const ImRect &rcBody, const float *pflSamples, size_t nSamples,
		                float flCeiling, float flOutlierMs, size_t nAxisSlots = 0 );
	}
}
