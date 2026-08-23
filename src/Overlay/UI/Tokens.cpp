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
		// SPEC §7.6's table, verbatim. Sizes are base units.
		static constexpr TypeSpec kTable[ (int)TypeRole::Count ] = {
			/* Title   */ { Family::Mono, 600, 11.0f,  true,  0.10f },
			/* Section */ { Family::Mono, 500, 10.5f,  true,  0.10f },
			/* Label   */ { Family::Sans, 400, 14.0f,  false, 0.0f  },
			/* Body    */ { Family::Sans, 400, 14.0f,  false, 0.0f  },
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
