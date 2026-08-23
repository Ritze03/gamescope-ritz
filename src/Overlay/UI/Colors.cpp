#include "Colors.h"

#include "../Palette.h"

namespace gamescope::ui
{
	namespace
	{
		// index.html's fixed status hues, which are deliberately NOT part of
		// the accent family so they can never collide with it at any hue
		// (SPEC §7.5: "a green 'delete permanently' is a bug").
		constexpr ImU32 kOk        = IM_COL32( 0x6E, 0xD2, 0x74, 255 );
		constexpr ImU32 kWarn      = IM_COL32( 0xF3, 0x82, 0x1D, 255 );
		constexpr ImU32 kWarnText  = IM_COL32( 0xF7, 0xA8, 0x5C, 255 );
		constexpr ImU32 kDanger    = IM_COL32( 0xEF, 0x6B, 0x5A, 255 );
		constexpr ImU32 kDangerText= IM_COL32( 0xF2, 0xA9, 0x9E, 255 );

		// The active ScopedDim factor. 1.0 means "not dimmed"; a disabled row
		// sets 0.55 for its own duration. File-static because the kit draws
		// one row at a time on one thread -- the same single-thread contract
		// every other piece of overlay state relies on.
		float s_flDim = 1.0f;
	}

	ScopedDim::ScopedDim( bool bDim, float flFactor )
		: m_flPrev( s_flDim )
	{
		// Multiplies rather than assigns, so a dimmed param inside a dimmed
		// parent does not accidentally brighten back up.
		if ( bDim )
			s_flDim *= flFactor;
	}

	ScopedDim::~ScopedDim() { s_flDim = m_flPrev; }

	ImU32 Col( Role eRole )
	{
		namespace pal = gamescope::palette;

		if ( s_flDim < 1.0f )
		{
			// Recurse ONCE with the dim lifted, then scale the result -- so
			// there is still exactly one table below and no dimmed copy of it
			// to keep in step.
			const float flSaved = s_flDim;
			s_flDim = 1.0f;
			const ImU32 col = Col( eRole );
			s_flDim = flSaved;
			return Dim( col, flSaved );
		}

		switch ( eRole )
		{
			case Role::Surface:          return IM_COL32( 0x09, 0x0A, 0x0C, (int)( 0.88f * 255 ) );
			case Role::SurfaceRail:      return pal::Black( 0.22f );        // over Surface
			case Role::SurfaceInspector: return pal::White( 0.03f );
			case Role::SurfaceRaised:    return pal::White( 0.06f );

			case Role::Line:             return pal::White( 0.10f );
			case Role::LineRegion:       return pal::White( 0.22f );
			case Role::LineControl:      return pal::White( 0.42f );
			case Role::TrackOff:         return pal::White( 0.34f );

			case Role::TextPrimary:      return pal::Text( 0.92f );
			case Role::TextBody:         return pal::Text( 0.72f );
			case Role::TextLabel:        return pal::Text( 0.68f );
			case Role::TextMeta:         return pal::Text( 0.52f );
			case Role::TextSegInactive:  return pal::Text( 0.50f );
			case Role::TextStepGlyph:    return pal::Text( 0.40f );
			case Role::TextKnobOff:      return pal::Text( 0.55f );

			case Role::AccentBase:       return pal::kAccent;
			case Role::AccentValue:      return pal::kAccentValue;
			case Role::AccentText:       return pal::kAccentText;
			case Role::AccentSeg:        return pal::kAccentSegText;
			case Role::AccentKnob:       return pal::kAccentKnob;
			case Role::AccentHandle:     return pal::kAccentHandle;
			case Role::AccentIcon:       return pal::kAccentIcon;
			case Role::AccentGradHi:     return pal::kAccentGradHi;

			case Role::Ok:               return kOk;
			case Role::Warn:             return kWarn;
			case Role::WarnText:         return kWarnText;
			case Role::Danger:           return kDanger;
			case Role::DangerText:       return kDangerText;

			case Role::Count:            break;
		}
		return IM_COL32_WHITE;
	}

	ImU32 Accent( float flAlpha )
	{
		return Dim( gamescope::palette::Accent( flAlpha ), s_flDim );
	}

	ImU32 Dim( ImU32 col, float flFactor )
	{
		// Scale the alpha, not the RGB: dimming toward the background is what
		// keeps the measured ratio predictable (SPEC §7.3's disabled row is a
		// measurement of exactly this operation on TextLabel).
		const unsigned nA = ( col >> IM_COL32_A_SHIFT ) & 0xFF;
		const unsigned nNew = (unsigned)( (float)nA * flFactor + 0.5f );
		return ( col & ~( 0xFFu << IM_COL32_A_SHIFT ) ) | ( ( nNew & 0xFF ) << IM_COL32_A_SHIFT );
	}
}
