// True Sens: the lasting hint at the top of the screen while a recording runs (D16).
// Drawn by FpsDisplay.cpp into the HUD layer's background draw list (the layer is drawn
// over the game, under the Shell, and frame generation does not touch it), only while
// TrueSens_RecordingActive(). The text itself is the pure gamescope::truesens::HintLines().
// superdoc/features/true-sens.md, "Recording hint (I3)".
//
// Runs on the compositor thread inside the HUD's ImGui frame (its own context, which carries
// the whole font set of Fonts.h).

#include "TrueSens.h"

#include "Fonts.h"
#include "Palette.h"
#include "../Keybinds.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <string>

namespace gamescope
{
	float TrueSens_DrawHud( ImDrawList *pDrawList, float flWidth, float flHeight, float flYOffset )
	{
		if ( !pDrawList || !TrueSens_RecordingActive() )
			return 0.0f;

		// The chord that opens the Shell; the reserved chord when that action is unbound.
		const truesens::HintText hint = truesens::HintLines(
			TrueSens_RecordingTarget(), keybinds::ChordTextFor( keybinds::Action::Shell ), keybinds::ReservedChordText() );
		if ( hint.title.empty() )
			return 0.0f;

		palette::EnsureThemeLoaded(); // the HUD may run before the Shell ever has

		// Scale with the output height, the same on 1080p, 1440p and 4K. Section (Sans 500) for the
		// title, Label (Sans 400) for the instruction.
		float flScale = std::clamp( flHeight / 1080.0f, 0.8f, 4.0f );
		ImFont *pTitleFont = fonts::Get( fonts::Style::Section );
		ImFont *pBodyFont = fonts::Get( fonts::Style::Label );
		float flTitleSize = 22.0f * flScale;
		float flBodySize = 17.0f * flScale;

		// Never wider than 80 % of the output (a narrow window): shrink the whole thing.
		float flTextW = std::max( pTitleFont->CalcTextSizeA( flTitleSize, FLT_MAX, 0.0f, hint.title.c_str() ).x,
		                          pBodyFont->CalcTextSizeA( flBodySize, FLT_MAX, 0.0f, hint.action.c_str() ).x );
		const float flMaxText = flWidth * 0.8f - 2.0f * 14.0f * flScale;
		if ( flTextW > flMaxText && flMaxText > 0.0f )
		{
			const float flFit = flMaxText / flTextW;
			flScale *= flFit;
			flTitleSize *= flFit;
			flBodySize *= flFit;
			flTextW = flMaxText;
		}

		const float flPadX = 14.0f * flScale;
		const float flPadY = 10.0f * flScale;
		const float flGap = 4.0f * flScale;
		const float flMargin = flHeight * 0.025f; // top margin: 2.5 % of the height
		const float flPlateW = flTextW + 2.0f * flPadX;
		const float flPlateH = 2.0f * flPadY + flTitleSize + flGap + flBodySize;

		const ImVec2 p0( ( flWidth - flPlateW ) * 0.5f, flMargin + flYOffset );
		const ImVec2 p1( p0.x + flPlateW, p0.y + flPlateH );
		if ( p0.x < 0.0f || p1.y > flHeight )
			return 0.0f;

		// Dark, slightly see-through plate in the overlay's surface colour, so any game picture reads.
		const ImU32 uPlate = ( palette::kSurfaceRgb & 0x00FFFFFFu ) | ( 0xE0u << 24 ); // ~88 % opaque
		pDrawList->AddRectFilled( p0, p1, uPlate, 8.0f * flScale );
		pDrawList->AddRect( p0, p1, palette::Accent( 0.5f ), 8.0f * flScale, 0, std::max( 1.0f, flScale ) );

		// Title in the accent colour (a stronger line), the instruction in the light text colour.
		const float flTitleW = pTitleFont->CalcTextSizeA( flTitleSize, FLT_MAX, 0.0f, hint.title.c_str() ).x;
		const float flBodyW = pBodyFont->CalcTextSizeA( flBodySize, FLT_MAX, 0.0f, hint.action.c_str() ).x;
		const float flCx = ( p0.x + p1.x ) * 0.5f;
		pDrawList->AddText( pTitleFont, flTitleSize, ImVec2( flCx - flTitleW * 0.5f, p0.y + flPadY ), palette::kAccentText, hint.title.c_str() );
		pDrawList->AddText( pBodyFont, flBodySize, ImVec2( flCx - flBodyW * 0.5f, p0.y + flPadY + flTitleSize + flGap ), palette::Text( 1.0f ), hint.action.c_str() );

		// I4 (the reference picture): its slot is directly below this plate, centred on the
		// same x, starting at y = p1.y + 8 * flScale and no wider than the plate. Not built yet.
		return p1.y;
	}
}
