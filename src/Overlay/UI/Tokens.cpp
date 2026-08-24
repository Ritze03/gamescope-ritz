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
		// SPEC §7.6's table, WITH A DELIBERATE DEPARTURE at the small end.
		// Two passes of direct user feedback now sit on top of the measured
		// spec, both following the discipline issue #23 set: raise the ORIGIN
		// CONSTANTS and record why, never multiply by a correction factor, so
		// nobody later "corrects" it back to the mockup. Sizes are base units.
		//
		// ---- pass 1 (2026-08-24, D23) ------------------------------------
		// "really small... increase the smallest of the used fonts by 1-2px",
		// naming row/control labels, category labels and titles:
		//
		//   Title 11.0 -> 13.0, Section 10.5 -> 12.0, Label/Body 14.0 -> 15.0.
		//   Value (16) and Meta (11.5) left alone as unnamed.
		//
		// ---- pass 2 (2026-08-24, D27 -- this table) ----------------------
		// "Make the Log and Changelog font 1-2px bigger. In fact, we should
		// increase all of the smaller font sizes by 1-2px."
		//
		// Pass 1's one mistake was leaving Meta alone on the theory that it
		// is a quiet auxiliary tier (units, marks, chips) that must not
		// out-rank the labels it annotates. That is true of Meta's auxiliary
		// job -- but Meta is ALSO the entire body text of the two surfaces
		// the user just named: Shell.cpp's DrawContentBody draws every Log
		// and Changelog line -- number, timestamp, scope tag AND the message
		// itself -- in TypeRole::Meta. So the role pass 1 deliberately did
		// not raise was precisely the role the complaint was about, which is
		// why one round of raising did not land. Meta gets the largest raise
		// here for that reason.
		//
		//   role      old    new    delta   gap to the role above
		//   Meta      11.5 -> 13.0  (+1.5)  -- the floor
		//   Section   12.0 -> 13.5  (+1.5)  Meta   -> Section  0.5
		//   Title     13.0 -> 14.5  (+1.5)  Section-> Title    1.0
		//   Label     15.0 -> 16.0  (+1.0)  Title  -> Label    1.5
		//   Body      15.0 -> 16.0  (+1.0)  (tied to Label)
		//   Value     16.0 -> 16.5  (+0.5)  Label  -> Value    0.5
		//
		// Why TAPERED and not a flat +1.5 on all six: the request is for the
		// SMALL sizes, and a flat raise is the "just adjust the scale" move
		// #23 rejected in another guise -- it inflates the whole slab to fix
		// the bottom of it. Tapering keeps the top of the ladder nearly
		// still, which is what keeps kRowH (44) and kControlH (28) valid at
		// 2.0x without a second geometry change, while the floor moves the
		// full 1.5. Value moves at all only because Label passing it would
		// invert the ladder; +0.5 is the least that keeps Value on top.
		//
		// Why Body tracks Label: the two have shared one literal since the
		// original table (Sans 400 14) because row labels and Inspector prose
		// are meant to read as the same register. Moving one without the
		// other would newly mismatch two things the table has always tied
		// together.
		//
		// Ascending order is preserved and no two roles collide:
		//   Meta 13.0 < Section 13.5 < Title 14.5 < Label/Body 16.0 < Value 16.5
		// Every adjacent gap is >= 0.5 base units, the same floor pass 1 set.
		// The two 0.5 gaps are both across a register boundary, so neither is
		// carrying the distinction on size alone: Meta->Section is
		// lowercase -> UPPERCASE with 0.10em tracking (Section's caps read
		// visibly taller than Meta's x-height at the same nominal size), and
		// Label->Value is Sans -> Mono.
		//
		// Contrast (SPEC §7.3): every role keeps its existing colour role,
		// and all six sizes remain under the WCAG large-text threshold
		// (18.66px normal / 14px bold) at 1.0x, so the 4.5:1 small-text floor
		// still applies to all of them rather than the relaxed 3:1 --
		// raising a size only widens that margin, never narrows it. The two
		// tightest cases on record are unchanged and still clear the floor:
		// TextMeta (52%, 5.28:1) -- which is what the Log body is drawn in,
		// so the surface that got bigger is also the one whose margin
		// matters most -- and TextSegInactive (50%, 4.96:1), Controls.cpp's
		// inactive segmented label, drawn in TypeRole::Section.
		static constexpr TypeSpec kTable[ (int)TypeRole::Count ] = {
			/* Title   */ { Family::Mono, 600, 14.5f,  true,  0.10f },
			/* Section */ { Family::Mono, 500, 13.5f,  true,  0.10f },
			/* Label   */ { Family::Sans, 400, 16.0f,  false, 0.0f  },
			/* Body    */ { Family::Sans, 400, 16.0f,  false, 0.0f  },
			// SPEC §7.6 says Mono 500 15; the mockup's `.val` and every atom
			// that self-displays (`.dd .tv`, `.tin .tv`) draw 16, which is
			// where the pre-D27 16.0 came from. The mockup is the tiebreaker
			// per the brief; D27's +0.5 sits on top of that resolution.
			/* Value   */ { Family::Mono, 500, 16.5f,  false, 0.0f  },
			/* Meta    */ { Family::Mono, 400, 13.0f,  false, 0.0f  },
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
