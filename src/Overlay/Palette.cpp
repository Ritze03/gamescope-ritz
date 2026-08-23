// Issue #37: hue-only accent picker. See Palette.h's own comments on the
// accent family (kAccent etc.), OklchToImU32(), and UpdateAccentFamily() --
// this file is only their implementation and the per-token OKLCH table.
#include "Palette.h"

#include <cmath>

namespace gamescope::palette
{
	// ---- OKLCH -> sRGB -----------------------------------------------------
	// Bjorn Ottosson's OKLab, D65, standard sRGB matrices -- the same
	// conversion superdoc/planning/ui-mockup-precise-spec.md §1 used to
	// derive every hex value in the table below by hand ("all oklch values
	// were converted to sRGB with the reference OKLab matrices", per that
	// file's own header). Verified against every one of §1's ten accent
	// tokens at hue 218 (this file's default): all ten reproduce the spec's
	// measured hex exactly.
	ImU32 OklchToImU32( float flL, float flC, float flHueDegrees, float flAlpha )
	{
		const float flHueRad = flHueDegrees * ( 3.14159265358979323846f / 180.0f );
		const float a = flC * cosf( flHueRad );
		const float b = flC * sinf( flHueRad );

		const float l_ = flL + 0.3963377774f * a + 0.2158037573f * b;
		const float m_ = flL - 0.1055613458f * a - 0.0638541728f * b;
		const float s_ = flL - 0.0894841775f * a - 1.2914855480f * b;

		const float l = l_ * l_ * l_;
		const float m = m_ * m_ * m_;
		const float s = s_ * s_ * s_;

		const float flLinR = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
		const float flLinG = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
		const float flLinB = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

		// Linear -> sRGB transfer, clamped to the displayable gamut -- the
		// spec's own gap-list caveat ("a known OKLCH gamut-mapping risk at
		// extreme hues") means an out-of-gamut channel is clipped here
		// rather than the whole color being remapped/desaturated; simple
		// clamping is the same thing a browser's `oklch()` CSS does by
		// default (no `color-gamut` fallback specified).
		auto EncodeChannel = []( float c ) -> int
		{
			if ( c <= 0.0f )
				return 0;
			if ( c >= 1.0f )
				return 255;
			const float flEncoded = ( c <= 0.0031308f )
				? c * 12.92f
				: 1.055f * powf( c, 1.0f / 2.4f ) - 0.055f;
			return (int)( flEncoded * 255.0f + 0.5f );
		};

		return IM_COL32(
			EncodeChannel( flLinR ),
			EncodeChannel( flLinG ),
			EncodeChannel( flLinB ),
			(int)( flAlpha * 255.0f + 0.5f ) );
	}

	// ---- sRGB -> OKLCH -----------------------------------------------------
	// The inverse of OklchToImU32(), same matrices transposed back. Every
	// constant below is the numeric inverse of the one directly above it, so
	// the two functions cannot be updated independently without the round-trip
	// test in tests/test_overlay_atoms.cpp failing.
	void ImU32ToOklch( ImU32 col, float *pflL, float *pflC, float *pflHueDegrees )
	{
		auto DecodeChannel = []( int n ) -> float
		{
			const float c = (float)n / 255.0f;
			return ( c <= 0.04045f ) ? c / 12.92f : powf( ( c + 0.055f ) / 1.055f, 2.4f );
		};

		const float r = DecodeChannel( (int)( ( col >> IM_COL32_R_SHIFT ) & 0xFF ) );
		const float g = DecodeChannel( (int)( ( col >> IM_COL32_G_SHIFT ) & 0xFF ) );
		const float b = DecodeChannel( (int)( ( col >> IM_COL32_B_SHIFT ) & 0xFF ) );

		const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
		const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
		const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

		const float l_ = cbrtf( l );
		const float m_ = cbrtf( m );
		const float s_ = cbrtf( s );

		const float flL = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
		const float flA = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
		const float flB = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;

		if ( pflL ) *pflL = flL;
		if ( pflC ) *pflC = sqrtf( flA * flA + flB * flB );
		if ( pflHueDegrees )
		{
			float flHue = atan2f( flB, flA ) * ( 180.0f / 3.14159265358979323846f );
			if ( flHue < 0.0f )
				flHue += 360.0f;
			*pflHueDegrees = flHue;
		}
	}

	// ---- Accent family (defaults match hue 218 = today's #36BDDD family) --
	ImU32 kAccent        = IM_COL32( 0x36, 0xBD, 0xDD, 255 );
	ImU32 kAccentEdge    = IM_COL32( 0x4F, 0xD0, 0xF1, 255 );
	ImU32 kAccentGradHi  = IM_COL32( 0x47, 0xCA, 0xEA, 255 );
	ImU32 kAccentValue   = IM_COL32( 0x78, 0xDB, 0xF6, 255 );
	ImU32 kAccentText    = IM_COL32( 0x71, 0xD4, 0xEF, 255 );
	ImU32 kAccentSegText = IM_COL32( 0xA9, 0xEA, 0xFD, 255 );
	ImU32 kAccentKnob    = IM_COL32( 0x93, 0xDE, 0xF4, 255 );
	ImU32 kAccentIcon    = IM_COL32( 0xA3, 0xE3, 0xF6, 255 );
	ImU32 kAccentHandle  = IM_COL32( 0xBA, 0xE7, 0xF4, 255 );
	ImU32 kAccentLinkDim = IM_COL32( 0x6B, 0xCD, 0xE9, 255 );

	// Each token's own OKLCH lightness/chroma at the spec's hue (218) --
	// copied straight from ui-mockup-precise-spec.md §1's "Source (oklch)"
	// column, which already reverse-derived every token's own L/C from its
	// measured hex (they are NOT uniform offsets from the base -- e.g.
	// accent-seg-text is .9/.07 while accent-handle is .9/.05, same L,
	// different C). Rotating the family is then just: keep every L/C fixed,
	// swap in the live hue.
	namespace
	{
		struct AccentToken
		{
			ImU32 *pOut;
			float flL;
			float flC;
		};

		AccentToken s_AccentTable[] =
		{
			{ &kAccent,        0.74f, 0.12f },
			{ &kAccentEdge,    0.80f, 0.12f },
			{ &kAccentGradHi,  0.78f, 0.12f },
			{ &kAccentValue,   0.84f, 0.10f },
			{ &kAccentText,    0.82f, 0.10f },
			{ &kAccentSegText, 0.90f, 0.07f },
			{ &kAccentKnob,    0.86f, 0.08f },
			{ &kAccentIcon,    0.88f, 0.07f },
			{ &kAccentHandle,  0.90f, 0.05f },
			{ &kAccentLinkDim, 0.80f, 0.10f },
		};

		// Accent()'s cached base bytes -- kept in sync with kAccent (same
		// L/C/hue) so Accent(flAlpha) never has to redo the OKLCH conversion
		// per call (it can run many times per frame, once per alpha step a
		// draw call needs).
		ImU8 s_nAccentBaseR = 0x36;
		ImU8 s_nAccentBaseG = 0xBD;
		ImU8 s_nAccentBaseB = 0xDD;
	}

	void UpdateAccentFamily()
	{
		const float flHue = g_LiveTheme.flAccentHue;
		for ( AccentToken &token : s_AccentTable )
			*token.pOut = OklchToImU32( token.flL, token.flC, flHue );

		s_nAccentBaseR = (ImU8)( ( kAccent >> IM_COL32_R_SHIFT ) & 0xFF );
		s_nAccentBaseG = (ImU8)( ( kAccent >> IM_COL32_G_SHIFT ) & 0xFF );
		s_nAccentBaseB = (ImU8)( ( kAccent >> IM_COL32_B_SHIFT ) & 0xFF );
	}

	ImU32 Accent( float flAlpha )
	{
		return IM_COL32( s_nAccentBaseR, s_nAccentBaseG, s_nAccentBaseB, (int)( flAlpha * 255.0f + 0.5f ) );
	}
}
