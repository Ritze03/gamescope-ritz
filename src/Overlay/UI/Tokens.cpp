#include "Tokens.h"

namespace gamescope::ui
{
	namespace
	{
		// The kit's single copy of display_scale. Seeded from
		// palette::g_LiveTheme by the shell once per frame (P2); defaults to
		// 1.0 so every pure consumer -- and every unit test -- has a sane value
		// without the overlay ever having started.
		float s_flScale = 1.0f;
	}

	void SetScale( float flScale )
	{
		// ConfigSchema.h pins display_scale to 0.5..2.0; clamping here rather
		// than trusting the caller keeps a corrupt config out of the geometry
		// instead of turning it into a zero-height row.
		s_flScale = std::clamp( flScale, 0.5f, 2.0f );
	}

	float Scale() { return s_flScale; }

	const TypeSpec &Type( TypeRole eRole )
	{
		// SPEC §7.6's table, WITH A DELIBERATE DEPARTURE at the small end
		// (2026-08-24, direct user feedback -- the same discipline issue #23
		// set: raise the origin constants themselves and record why, so
		// nobody later "corrects" it back to the measured spec). Sizes are
		// base units.
		//
		// The complaint named three roles explicitly: row/control labels,
		// category (group/section) labels, and titles -- "really small...
		// increase the smallest of the used fonts by 1-2px". Value (16,
		// mono, already the largest and unnamed) and Meta (11.5, deliberately
		// the quietest auxiliary tier -- units, marks, chips -- also unnamed)
		// are untouched: raising only the complained-about roles is the
		// "look at the whole ladder" call the brief asked for, not a blanket
		// bump.
		//
		//   Title    11.0 -> 13.0  (+2.0, +18%)  named ("titles and such")
		//   Section  10.5 -> 12.0  (+1.5, +14%)  named ("category labels")
		//   Label    14.0 -> 15.0  (+1.0,  +7%)  named ("control...labels")
		//   Body     14.0 -> 15.0  (+1.0, tied to Label -- see below)
		//   Value    16.0 -> 16.0  (unchanged)
		//   Meta     11.5 -> 11.5  (unchanged)
		//
		// Body is not named by the complaint but is kept in lockstep with
		// Label: the two have always shared one literal (Sans 400 14)
		// because row labels and Inspector prose are meant to read as the
		// same register. Moving one without the other would newly mismatch
		// two things the table has always tied together -- a new defect this
		// change should not introduce.
		//
		// New ascending order: Meta 11.5 < Section 12.0 < Title 13.0 <
		// Label/Body 15.0 < Value 16.0. Meta -- never a complained-about role
		// -- becomes the numeric floor, displacing Section/Title, which is
		// the intended outcome: Meta's job (units, footnotes, chips) is to
		// read quieter than the labels and titles a user reads for
		// navigation, not to out-rank them in size. Every adjacent gap in
		// the new ladder is >= 0.5 base units (Meta->Section 0.5,
		// Section->Title 1.0, Title->Label 2.0, Label->Value 1.0), and
		// Section->Title itself widened from 0.5 to 1.0, so the steps stay
		// distinguishable rather than collapsing into each other.
		//
		// Contrast (SPEC §7.3): every role above keeps its existing colour
		// role unchanged, and all six sizes remain well under the WCAG
		// large-text threshold (18.66px normal / ~14px bold), so the 4.5:1
		// small-text floor still applies to all of them, not the relaxed
		// 3:1 -- raising a size only widens that margin, never narrows it.
		// TextMeta (52%, 5.28:1) and TextSegInactive (50%, 4.96:1 --
		// Controls.cpp's inactive segmented label, drawn in TypeRole::Section
		// -- the worst text case on record) both still clear the 4.5:1 floor
		// unchanged.
		static constexpr TypeSpec kTable[ (int)TypeRole::Count ] = {
			/* Title   */ { Family::Mono, 600, 13.0f,  true,  0.10f },
			/* Section */ { Family::Mono, 500, 12.0f,  true,  0.10f },
			/* Label   */ { Family::Sans, 400, 15.0f,  false, 0.0f  },
			/* Body    */ { Family::Sans, 400, 15.0f,  false, 0.0f  },
			// SPEC §7.6 says Mono 500 15; the mockup's `.val` and every atom
			// that self-displays (`.dd .tv`, `.tin .tv`) draw 16. The mockup is
			// the tiebreaker per the brief, and §2.3 independently calls the
			// value "B's `.val` size", which is 16. 16 it is.
			/* Value   */ { Family::Mono, 500, 16.0f,  false, 0.0f  },
			/* Meta    */ { Family::Mono, 400, 11.5f,  false, 0.0f  },
		};
		return kTable[ (int)eRole ];
	}

	float Approach( float flCurrent, float flTarget, float flDurationSeconds, float flDeltaTime )
	{
		if ( flDurationSeconds <= 0.0f || flDeltaTime <= 0.0f )
			return flTarget;

		// Advance the 0..1 progress by dt/duration and ease it. Framed as a
		// per-frame exponential approach so it is stable at any frame rate --
		// a fixed per-frame lerp constant (the usual mistake) makes a 240 Hz
		// overlay animate four times faster than a 60 Hz one.
		const float t = std::clamp( flDeltaTime / flDurationSeconds, 0.0f, 1.0f );
		return flCurrent + ( flTarget - flCurrent ) * Ease( t );
	}
}
