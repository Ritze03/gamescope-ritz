// E2 colour roles -- SPEC.md §7.1's table, as an enum and one lookup.
//
// Split from Tokens.h because these are the only tokens that need imgui.h and
// Overlay/Palette.h (the accent family is hue-live and recomputed by
// palette::UpdateAccentFamily()). Tokens.h stays free of both so the geometry
// arithmetic is unit-testable on its own.
//
// SPEC §7.7's accent budget is why this is an enum and not a palette of free
// colours: "Accent is spent on state and nothing else". A role names the JOB,
// so a call site asks for `Role::TextLabel`, never for "white at 68%", and a
// contrast fix is one edit here rather than a hunt through every panel's own
// alpha literals -- which is exactly the failure mode issue #62 recorded.
//
// Every alpha below is SPEC §7.3's measured value. The three of direction B's
// own values that FAIL the contrast floor are recorded in SPEC §7.3 so nobody
// re-introduces them; they do not appear here at all.
#pragma once

#include "imgui.h"

namespace gamescope::ui
{
	enum class Role : unsigned char
	{
		// ---- surfaces (SPEC §7.1) ----------------------------------------
		Surface,          // slab base                       rgba(9,10,12,.88)
		SurfaceRail,      // rail, recessed
		SurfaceInspector, // inspector, raised                white 3%
		SurfaceRaised,    // control boxes, inactive segments white 6%

		// ---- lines --------------------------------------------------------
		Line,             // row separators                   white 10%  (decorative, exempt)
		LineRegion,       // region boundaries                white 22%  (decorative, exempt)
		LineControl,      // EVERY interactive boundary       white 42%  (4.09:1)
		TrackOff,         // unfilled slider / hue / meter    white 34%  (3.07:1)

		// ---- text (four roles; TextFaint is deleted, SPEC §7.1) ------------
		TextPrimary,      // selected labels, hero values     92%  14.79:1
		TextBody,         // Inspector prose                  72%   9.25:1
		TextLabel,        // row labels, parameter labels     68%   8.34:1
		TextMeta,         // units, marks, chips, placeholders 52%  5.28:1
		TextSegInactive,  // segmented inactive cell (B's)    50%   4.96:1
		TextStepGlyph,    // stepper - / + glyph (B's)        40%   3.58:1 (UI glyph)
		TextKnobOff,      // switch knob, off (B's)           55%   5.78:1

		// ---- accent: STATE ONLY (SPEC §7.7) -------------------------------
		AccentBase,       // 2px state edge, active borders
		AccentValue,      // the Inspector's own value line when it differs
		AccentText,       // verb chip text, link-style labels
		AccentSeg,        // active segment / active chip text
		AccentKnob,       // switch knob, on
		AccentHandle,     // slider handle
		AccentIcon,       // active rail icon
		AccentGradHi,     // slider fill gradient, right end

		// ---- status (deliberately outside the accent family, SPEC §7.5) ---
		Ok,
		Warn,
		WarnText,
		Danger,
		DangerText,

		Count,
	};

	// The role, packed. Accent roles track palette::UpdateAccentFamily(), so
	// one hue change repaints everything -- the same "no accent literal" rule
	// index.html enforces on itself.
	ImU32 Col( Role eRole );

	// The accent at an arbitrary alpha, for the fills SPEC.md states as a
	// percentage of the accent rather than as a named token (switch track 30%,
	// segment fill 24%, bank fill 22%, verb fill 16%, selected row 8%).
	ImU32 Accent( float flAlpha );

	// SPEC §3.13: a disabled row draws at 0.55, not E's 0.34 -- 3.27:1 rather
	// than an unreadable 2.6:1. Applied to a colour, not pushed as a style.
	ImU32 Dim( ImU32 col, float flFactor = 0.55f );

	// SPEC §3.13 again, and the reason it is HERE rather than at each call
	// site: "row x 0.55" means the WHOLE row -- its label, its value AND its
	// control. The control atoms paint straight onto the draw list with token
	// colours (Controls.cpp), so ImGui's own BeginDisabled() alpha never
	// reaches them; dimming them one atom at a time would mean every atom
	// growing a `bool bDisabled` parameter, and the first atom that forgot it
	// would be a control that greys everywhere except where it matters.
	//
	// Instead the factor lives under Col()/Accent() -- the two functions every
	// pixel in the kit already goes through -- so an atom cannot opt out and a
	// new atom is dimmed correctly before it is written. This is the same
	// "one edit here rather than a hunt through every panel's alpha literals"
	// property the header comment above claims for Role itself.
	//
	// Scoped, so it cannot leak into the next row.
	class ScopedDim
	{
	public:
		explicit ScopedDim( bool bDim, float flFactor = 0.55f );
		~ScopedDim();
		ScopedDim( const ScopedDim & ) = delete;
		ScopedDim &operator=( const ScopedDim & ) = delete;

	private:
		float m_flPrev;
	};
}
