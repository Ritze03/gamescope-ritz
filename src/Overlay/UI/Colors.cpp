#include "Colors.h"

#include "../Palette.h"

namespace gamescope::ui
{
	namespace
	{
		// index.html's fixed status hues, which are deliberately NOT part of
		// the accent family so they can never collide with it at any hue
		// (SPEC §7.5: "a green 'delete permanently' is a bug"). Issue #93
		// widened the accent hue to drive every neutral in the kit too (see
		// TintedNeutral() below) -- these five are the deliberate exception,
		// per the user's own call: "Ok/Warn/Danger keep their own hues, so
		// green still means good and red still means bad" regardless of
		// where the user parks the accent slider. Do not route these through
		// TintedNeutral() or OklchToImU32(..., g_LiveTheme.flAccentHue, ...)
		// -- that would let a user's hue choice wash out or collide with a
		// status meaning, which is the exact bug SPEC §7.5 already forbade
		// for the accent family.
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

		// ---- Issue #93: hue-tinted neutrals --------------------------------
		// The user's spec (verbatim): "convert the colors to HSV/OKLCH, and
		// make H dependent on the accent color the user selected -- keep
		// every color's own saturation/chroma and lightness exactly as they
		// are, only the hue moves." The first pass here got this wrong: it
		// manufactured a fixed kNeutralTintChroma (0.012) and pushed EVERY
		// role through it regardless of what chroma that role actually had,
		// which visibly over-tinted roles that were previously pure greys
		// (pal::White()/pal::Black(), chroma exactly 0). That constant and
		// the blanket routing are gone.
		//
		// The corrected rule: a role that was already chroma-0
		// (pal::White()/pal::Black() -- true neutrals) has nothing for a hue
		// to rotate, so it stays exactly pal::White()/pal::Black() -- a
		// no-op, and that is correct, not a missed case. A role built from a
		// literal that happens to carry its own small non-zero chroma
		// (Role::Surface's 0x09,0x0A,0x0C; pal::Text()'s 0xEF,0xF5,0xFB, both
		// slightly cool-cast rather than pure grey) keeps that exact chroma
		// but now points at the live accent hue instead of its old hardcoded
		// one -- TintedNeutral() below, reusing Palette.cpp's OklchToImU32()
		// per the user's own framing ("OKLCH is this codebase's already-
		// adopted, better-behaved equivalent" of the HSV they described).
		//
		// L/C pairs below are each base's own measured OKLCH decomposition
		// (via Palette.cpp's ImU32ToOklch()), not tuned figures.
		constexpr float kTextL = 0.9673f; // pal::Text() / kTextRgb (0xEF,0xF5,0xFB)
		constexpr float kTextC = 0.0103f;

		constexpr float kSurfaceL = 0.1445f; // Role::Surface's own base (0x09,0x0A,0x0C)
		constexpr float kSurfaceC = 0.0047f;

		// ---- SurfaceInspector: a scoped, user-requested tint ---------------
		// NOT a return of Issue #93's rejected global kNeutralTintChroma --
		// that pushed a manufactured chroma onto every neutral role in the
		// kit and was reverted for over-tinting roles that were previously
		// pure grey. This is different: the user explicitly asked for the
		// inspector rail specifically (and only the inspector) to read
		// darker and subtly hue-tinted, closer to SurfaceRail's dark-base-
		// at-low-alpha treatment, instead of its previous light wash off
		// pal::White(). kInspectorAlpha matches SurfaceRail's own 0.22 so
		// the two read as the same weight of darkening; kInspectorC is kept
		// well below Issue #93's rejected 0.012 -- both are named here so
		// either can be nudged in one place without hunting.
		constexpr float kInspectorL     = 0.10f;
		constexpr float kInspectorC     = 0.03f;
		constexpr float kInspectorAlpha = 0.22f;

		// flL/flC are a base's own OKLCH lightness/chroma (kTextL/kTextC,
		// kSurfaceL/kSurfaceC, ...) -- unchanged from what the literal they
		// replace already meant; only the hue argument is live.
		ImU32 TintedNeutral( float flL, float flC, float flAlpha )
		{
			return gamescope::palette::OklchToImU32(
				flL, flC, gamescope::palette::g_LiveTheme.flAccentHue, flAlpha );
		}
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
			// Role::Surface and Role::SurfaceInspector carry their own small
			// non-zero chroma (see TintedNeutral()'s and kInspectorC's own
			// comments); every other Surface*/Line*/TrackOff role below is
			// pal::White()/pal::Black() -- true chroma-0 neutrals, so a hue
			// swap on them is a no-op and they stay exactly what they were
			// before issue #93.
			case Role::Surface:          return TintedNeutral( kSurfaceL, kSurfaceC, 0.88f );
			case Role::SurfaceRail:      return pal::Black( 0.22f );        // over Surface
			case Role::SurfaceInspector: return TintedNeutral( kInspectorL, kInspectorC, kInspectorAlpha ); // dark base + subtle hue tint, over Surface -- see comment above
			case Role::SurfaceRaised:    return pal::White( 0.06f );

			case Role::Line:             return pal::White( 0.10f );
			case Role::LineRegion:       return pal::White( 0.22f );
			case Role::LineControl:      return pal::White( 0.42f );
			case Role::TrackOff:         return pal::White( 0.34f );

			case Role::TextPrimary:      return TintedNeutral( kTextL, kTextC, 0.92f );
			case Role::TextBody:         return TintedNeutral( kTextL, kTextC, 0.72f );
			case Role::TextLabel:        return TintedNeutral( kTextL, kTextC, 0.68f );
			case Role::TextMeta:         return TintedNeutral( kTextL, kTextC, 0.52f );
			case Role::TextSegInactive:  return TintedNeutral( kTextL, kTextC, 0.50f );
			case Role::TextStepGlyph:    return TintedNeutral( kTextL, kTextC, 0.40f );
			case Role::TextKnobOff:      return TintedNeutral( kTextL, kTextC, 0.55f );

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
