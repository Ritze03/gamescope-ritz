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

#include "Registry.h"
#include "Row.h"
#include "Tokens.h"

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

	namespace controls
	{
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
		bool Stepper( const RowCtx &row, const char *pszId, int *pnValue,
		              int nMin, int nMax, int nStep = 1 );

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
		bool Bank( const RowCtx &row, const char *pszId, uint32_t *pnMask,
		           const Option *pOptions, size_t nOptions );

		// ---- SPEC §3.8 -- a live scalar, read-only ------------------------
		void Meter( const RowCtx &row, float flValue, float flMin, float flMax );

		// ---- SPEC §3.9 -- the verb chip -----------------------------------
		enum class Intent : unsigned char { Accent, Neutral, Danger };
		bool Verb( const RowCtx &row, const char *pszId, const char *pszVerb,
		           Intent eIntent = Intent::Accent, bool bEnabled = true );

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

		// ---- SPEC §4.4 -- Colour override: L/C/H rails + swatch -----------
		// Operates in OKLCH because that is the space the accent family and
		// every palette token already live in (Palette.h), so a colour picked
		// here is expressible in the same terms as everything around it.
		// The caller owns the sRGB<->OKLCH round trip, so the config format
		// (a packed RGB int) is untouched by this control existing.
		bool ColorBody( const ImRect &rcBody, const char *pszId,
		                float *pflL, float *pflC, float *pflH );

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
