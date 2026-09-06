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
//   2. THE SLIDER'S HANDLE IS ONE CONSTANT WIDTH, PAINTED WHERE SliderBehavior
//      SAYS. SliderGrab() below is the only code in the kit that names a grab
//      width; it pushes that width into GrabMinSize, calls SliderBehavior(),
//      and gets back the grab rect SliderBehavior itself produced.
//
//      GrabMinSize is a FLOOR, not a ceiling: for a non-decimal data type
//      (every SliderInt) ImGui's own SliderBehaviorT widens the grab past it
//      on purpose, "so a coarse, small-range int slider (Outline Width,
//      Dot > Size, Line > Width -- requests-2026-09-06.md item 4) can
//      represent one unit" -- and that widening has no style-var knob to cap
//      it. SliderGrab() re-centres the returned rect to the kit's own
//      constant width AFTER SliderBehavior has already used its own (wider)
//      grab to resolve this frame's click, drag and keyboard step -- so the
//      CENTRE the painter draws at is still exactly where SliderBehavior put
//      it, nothing about hit-testing or the value changes, and only the
//      drawn WIDTH is a second, later assignment. That is the one deliberate
//      exception to "an atom never recomputes its own geometry" above: it
//      touches only a size, never a position, and never before
//      SliderBehavior has already committed this frame's interaction.
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

		// OPTIONAL gate in front of fnPrimary: runs on the primary press
		// while the modal is STILL OPEN, and a false answer keeps it open --
		// the Profiles modals' "errors inline in the modal, never a toast":
		// the caller records why (a name that sanitizes to something else,
		// a CreateProfile() refusal) in its own state, fnBody prints it on
		// the next frame, and the user's typed fields are still there to be
		// corrected. A true answer (or no gate at all) closes the modal and
		// fires fnPrimary exactly as before.
		std::function<bool()> fnValidate;

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

	// =========================================================================
	//  Dropdown popup -- SPEC gap addendum (2026-09-06): ui-design-guide.md's
	//  Dropdown entry. controls::Dropdown() below draws the CLOSED box inline,
	//  in the row's own place; its OPEN list cannot be, for the identical
	//  reason DrawModal() and the palette cannot be drawn from inside the
	//  sheet's child window (see DrawModal()'s own comment) -- so the list is
	//  its own top-level surface, drawn once per frame from here, in the same
	//  slot Shell.cpp gave the auto-downgraded Choice's own popup: after
	//  DrawModal() (a modal owns the whole surface; a dropdown answers one
	//  row, so a modal opened on top wins outright) and before the palette
	//  (transient and keyboard-only, the one thing allowed to cover
	//  everything else).
	// =========================================================================

	// Call from the shell's own frame prologue, BEFORE a single row is drawn
	// -- swallows a press outside the open popup (and outside the box that
	// owns it, so the SAME click that opened it this frame cannot also close
	// it) so nothing underneath can react to that click too. A no-op when
	// nothing is open. Mirrors Shell.cpp's own former
	// DismissOpenDropdownOnOutsideClick() and its documented reason: decided
	// on the press, before ButtonBehavior on any other control gets a look.
	void DismissDropdownOnOutsideClick( const ImRect &rcSlab );

	// One frame of whatever Dropdown popup is open, or nothing. `rcSlab` is
	// the surface it is clamped inside and the box it drops from is
	// borrowed from -- the exact same rect DrawModal() and DismissDropdown-
	// OnOutsideClick() above are given.
	void DrawDropdownPopup( const ImRect &rcSlab );

	bool IsDropdownPopupOpen();

	// Closes it without commit -- for a caller that must guarantee no two
	// popups are ever open together (the palette's own open gesture; a
	// second Dropdown's box already refuses to open under a live Modal, see
	// controls::Dropdown()'s own comment, so that direction needs no call).
	void CloseDropdownPopup();

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

		// requests-2026-09-06.md item 4: "Outline Width", "Dot > Size" and
		// "Line > Width" drew a grab far wider than every other slider's.
		// ImGui's own SliderBehaviorT widens a non-decimal (every SliderInt)
		// grab past GrabMinSize on purpose -- "if possible have the grab
		// size represent 1 unit" -- which a coarse, small-range int slider
		// turns into a grab spanning a large fraction of the track.
		//
		// This is the pure half of the fix, extracted out of SliderGrab()
		// (Controls.cpp, its only caller) so it is checkable without an
		// ImGui context: recentre `grab` to `flConstantW` wide around its
		// OWN centre -- the centre SliderBehavior already computed for this
		// frame's value/drag/click, left untouched -- clamped to `flHitW`
		// so a pathologically narrow track cannot make the returned rect
		// wider than the row it is drawn in.
		ImRect ConstantWidthGrab( const ImRect &grab, float flConstantW, float flHitW );

		// requests-2026-09-07 item 8/A: "editing any element should
		// automatically select it, so it also pops up in the inspector
		// rail." Before this, a Sheet row was only selected by a raw click
		// (Shell.cpp's DrawEntryRow returning true from its own row-spanning
		// InvisibleButton) -- but D22's own AllowOverlap rule (see that
		// comment in Shell.cpp) means a press that lands ON an atom -- a
		// slider handle, a switch, a stepper's -/+, a segmented cell, a
		// dropdown pick -- resolves the hit test to THAT ATOM, never to the
		// row button beneath it. So dragging a slider or flipping a switch
		// on a row that was not already selected changed the value but left
		// the OLD row highlighted and the Inspector showing the wrong row.
		//
		// This is the pure half of the fix, same reason ConstantWidthGrab()
		// above is one: Shell.cpp's row-drawing functions are file-private
		// (Shell.h's own header comment: "this is the whole of its public
		// surface... deliberately, because... there is no header a category
		// file could include to reach into it") and cannot be reached from a
		// test directly, so the one line of logic the fix actually adds is
		// named and pinned here instead of appearing unexplained, untested,
		// at the call site.
		//
		// 2026-09-08 -- "select on a VALUE CHANGE" was not enough, and the
		// user's report ("Nope, doesnt work. Even editing a slider should
		// make it select the line") was right. Measured, one real click or
		// drag per atom kind: Slider, Switch, Stepper, segmented Choice and
		// Action did select, but a DROPDOWN (opening its list changes no
		// value, so nothing fired) and every COMPOSITE BAND -- the accent
		// hue rail, a colour picker's R/G/B rails, the anchor grid, a list
		// box row -- did not, even though the band's value visibly moved.
		// A composite is where the user's "slider" actually lives: the hue
		// and R/G/B rails ARE sliders, drawn inside a band whose own return
		// value was its bare click.
		//
		// So the rule is now ENGAGEMENT, not change: any press that lands on
		// a row's own control selects that row, whether or not a value moves
		// -- which also makes a slider select on the press instead of only
		// once the value has crossed a step, and a dropdown select when it
		// opens. See ControlEngaged() for how a press is detected.
		bool ShouldSelectRow( bool bClicked, bool bValueChanged, bool bControlEngaged );

		// Did a control drawn inside this row's own draw take ImGui's
		// ActiveId on this frame? The row painter snapshots ImGui::GetActiveID()
		// immediately before submitting its control and again immediately
		// after, and hands both here.
		//
		// Every atom in this kit runs through Begin() -> ItemAdd() +
		// ButtonBehavior() (or SliderBehavior), so a press ALWAYS takes
		// ActiveId -- which makes this one test cover every atom kind at
		// once, including the ones with no value to change, instead of each
		// kind having to remember to report itself.
		//
		// `nAfter != nBefore` is what keeps it to THIS row: while a slider is
		// held, every other row drawn that frame sees the same non-zero id
		// before and after its own control, so only the row that actually
		// took activation reads as engaged -- and only on the frame it took
		// it, which is the frame selection should move.
		bool ControlEngaged( ImGuiID nActiveIdBefore, ImGuiID nActiveIdAfter );

		// requests-2026-09-07 item 12: the Inspector's CONFIGURE header
		// ("PARAMETERS n of N") hardcoded the denominator as a literal "6"
		// in Shell.cpp, so it kept reading "of 6" after Registry.cpp's
		// kParamBudget was raised to 7 (2026-09-06) -- a row at the new
		// ceiling read "PARAMETERS 7 of 6". Pure text formatting, kept
		// free of ImGui (same reason ConstantWidthGrab()/ShouldSelectRow()
		// above are) so it can be pinned against ui::ParamBudget()
		// directly rather than only read by eye in a capture. The caller
		// passes both numbers rather than this function reaching for
		// ParamBudget() itself, so a test can also exercise a budget other
		// than the live one without touching Registry state.
		std::string ParametersHeaderText( size_t nCount, size_t nBudget );

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

		// ---- Dropdown -- ui-design-guide.md's Dropdown entry ---------------
		// A Choice FORCED to present as a dropdown, regardless of whether it
		// would fit segmented -- Registry.h's Entry::Dropdown() is the
		// presentation flag a panel sets to ask for this instead of Choice().
		// Same box/chevron/hover chrome as Choice's own auto-downgrade
		// dropdown branch (one code path draws both), but this one owns its
		// popup ENTIRELY -- open/close state, the anchor rect, the pending
		// commit -- inside Controls.cpp rather than in the Shell's
		// s_sOpenDropdown machinery, which is why it is a distinct entry
		// point and not just another bPopupOpen-style parameter: the two
		// mechanisms never share state, and Controls.cpp has no registry to
		// look an id up in.
		//
		// THE ONE-FRAME-LATE COMMIT. *pnValue only changes (bChanged comes
		// back true) on the frame AFTER an item is picked in the popup, not
		// the frame of the click itself. Reason: *pnValue's address is a
		// caller-side local (Shell.cpp reads a Value into a stack int and
		// passes &that), valid only for the duration of THIS call -- long
		// gone by the time DrawDropdownPopup() runs later in the same frame
		// from its own top-level window (the same z-order requirement that
		// makes the popup a separate draw call at all). So a pick is stashed
		// against this pszId and applied the next time THIS SAME control is
		// called, which -- since every atom here redraws every frame -- is
		// one frame later: imperceptible, and the same trick Text/Stepper's
		// *pbEditing already relies on to cross a frame boundary through
		// caller-owned storage instead of a held pointer.
		//
		// `bRowSelected`: true while the row this box belongs to is the
		// keyboard's current selection, so Enter/Space open it exactly as a
		// click would (SPEC §8.2's "Enter/Space -- activate"). Only one
		// popup open at a time: opening this one closes any other Dropdown
		// popup that was open, and it refuses to open at all while a Modal
		// is up (IsModalOpen()) -- the Modal already owns the whole surface.
		struct DropdownResult
		{
			bool bChanged = false;   // *pnValue was just updated by an earlier pick -- see above
		};
		DropdownResult Dropdown( const RowCtx &row, const char *pszId, int *pnValue,
		                        const Option *pOptions, size_t nOptions,
		                        bool bRowSelected = false );

		// Pure: the popup's rect anchored under rcAnchor (or, flipped, above
		// it), at least as wide as rcAnchor and at least flMinContentWidthPx
		// wide, clamped so it never draws past rcSlab's edges. Tall enough
		// for min(nItemCount, nMaxVisibleRows) rows of tok::kControlH each --
		// ListBox()'s own row height, kept in the same token so the rect
		// this returns and what ListBox() actually draws inside it can
		// never disagree about how tall a row is. Shared by
		// DrawDropdownPopup() and the tests, the same reason
		// ModalPrimaryButtonRect()-style geometry is shared everywhere else
		// in this kit rather than replicated.
		ImRect DropdownPopupRect( const ImRect &rcAnchor, const ImRect &rcSlab,
		                         float flMinContentWidthPx, int nItemCount,
		                         int nMaxVisibleRows = 8 );

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

		// N EQUAL-WIDTH verb chips across a rect the caller already laid out
		// (the List band's Create / Copy / Edit / Delete strip, Band.cpp's
		// BandLayout::rcStrip), kGapSeg apart. Equal rather than measured
		// widths on purpose: the sketch draws four same-size buttons, and a
		// strip whose chips resize as their labels change would jitter when
		// "Delete" dims. Returns the index pressed this frame, or -1.
		struct VerbSpec
		{
			const char *pszLabel  = nullptr;
			Intent      eIntent   = Intent::Accent;
			bool        bEnabled  = true;
		};
		int VerbStrip( const ImRect &rcStrip, const char *pszId, const VerbSpec *pVerbs, size_t nVerbs );

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

		// ---- SPEC §4.4 -- Accent hue: hue rail + 9 swatches ---------------
		// The swatches are the nine 45-degree stops (0..360 inclusive,
		// requests-2026-09-07 item 11: a 9th swatch was added at the
		// rightmost, "basically the same as the leftmost", so the row's
		// swatch centres line up with the rail it drives). They are
		// PRESETS on the same one value the rail sets, not a second
		// setting -- which is why they share *pflHue and return through
		// the same bool.
		bool HueBody( const ImRect &rcBody, const char *pszId, float *pflHue );

		// requests-2026-09-07 item 11: "This will make the slider align
		// with the presets properly." Before this, HueBody() tiled N
		// EQUAL-WIDTH BLOCKS edge to edge across rcBody -- every block's
		// own CENTRE sat half a cell width IN from the true edge, which is
		// the actual misalignment the user was pointing at (adding a 9th
		// block to that same math would not have fixed it). This instead
		// spaces N swatch CENTRES evenly across the full width -- centre(i)
		// = flBodyMinX + i * (width / (nSwatches-1)) -- so centre(0) lands
		// exactly on flBodyMinX and centre(nSwatches-1) exactly on
		// flBodyMaxX: the same two points PlaceFull()'s rail already spans
		// ("the track IS the range"). Every swatch, including the two
		// outer ones, is the same width and drawn symmetrically around its
		// own centre; the outer two consequently extend a little past
		// rcBody's own edges, the same way a slider's handle can slightly
		// overhang its track's endpoints.
		//
		// Pure geometry, kept free of ImGui and returned with a zero Y
		// range (the caller supplies the actual top/bottom) for the same
		// reason ConstantWidthGrab() above is: test_overlay_ui.cpp pins it
		// directly.
		ImRect HueSwatchRect( float flBodyMinX, float flBodyMaxX,
		                     int nIndex, int nSwatches, float flGapPx );

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
