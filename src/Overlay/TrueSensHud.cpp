// True Sens: the lasting hint at the top of the screen while a recording runs (D16).
// Drawn by FpsDisplay.cpp into the HUD layer's background draw list (the layer is drawn
// over the game, under the Shell, and frame generation does not touch it), only while
// TrueSens_RecordingActive(). The text itself is the pure gamescope::truesens::HintLines().
// superdoc/features/true-sens.md, "Recording hint".
//
// Runs on the compositor thread inside the HUD's ImGui frame (its own context, which carries
// the whole font set of Fonts.h).

#include "TrueSens.h"

#include "Fonts.h"
#include "Palette.h"
#include "../Keybinds.h"

#include "log.hpp"
#include "steamcompmgr.hpp"
#include "steamcompmgr_shared.hpp"

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture / ImTextureDataQueueUpload (the box preview's precedent)

#include <algorithm>
#include <cfloat>
#include <string>

namespace gamescope
{
	// ---- the reference picture ---------------------------------------------------
	// One texture for the whole process, created by the first picture and reused by every later
	// one (a new picture is a sub-rectangle upload): nothing grows with the number of recordings.
	// Everything here runs on the compositor thread: the capture callback is called by paint_all.
	namespace
	{
		constexpr int kTexSide = 768; // fits a 1440p crop 1:1; a larger crop is reduced by a whole factor

		struct Reference
		{
			ImTextureData tex;
			ImGuiContext *pCtx = nullptr;   // the context the texture is registered in
			bool bShow = false;             // a picture is held and is drawn
			bool bPendingUpload = false;    // new pixels wait for the HUD context to queue them
			int nW = 0, nH = 0;             // the picture's size inside the texture
			uint64_t ulSeq = 0;             // the request a capture must answer to be accepted
			uint64_t ulGeneration = 0;
		};

		Reference &Ref()
		{
			static Reference r;
			return r;
		}

		void Deliver( uint64_t ulSeq, const uint8_t *pPixels, uint32_t uWidth, uint32_t uHeight, uint32_t uPitch,
		              uint64_t ulCaptureNs, const char *pszFailure )
		{
			Reference &r = Ref();
			if ( ulSeq != r.ulSeq || !TrueSens_RecordingActive() )
				return; // an answer to an older request, or the recording is already over
			if ( !pPixels )
			{
				TrueSens_LogScope().infof( "no reference picture: %s", pszFailure ? pszFailure : "unknown reason" );
				return;
			}
			const truesens::CropRect crop = truesens::ReferenceCrop( (int)uWidth, (int)uHeight );
			if ( !crop.ok )
			{
				TrueSens_LogScope().infof( "no reference picture: the captured picture is too small (%ux%u)", uWidth, uHeight );
				return;
			}
			const int f = truesens::ReferenceDownscale( crop.w, crop.h, kTexSide );
			const int nW = crop.w / f, nH = crop.h / f;

			if ( r.tex.Pixels == nullptr )
				r.tex.Create( ImTextureFormat_RGBA32, kTexSide, kTexSide );
			const int nPitch = r.tex.GetPitch();
			for ( int y = 0; y < nH; y++ )
			{
				uint8_t *pDst = (uint8_t *)r.tex.Pixels + (size_t)y * nPitch;
				for ( int x = 0; x < nW; x++ )
				{
					// Box average of f x f source pixels (B8G8R8A8 in memory, alpha unused).
					uint32_t uB = 0, uG = 0, uR = 0;
					for ( int sy = 0; sy < f; sy++ )
					{
						const uint8_t *pSrc = pPixels + (size_t)( crop.y + y * f + sy ) * uPitch + (size_t)( crop.x + x * f ) * 4;
						for ( int sx = 0; sx < f; sx++, pSrc += 4 )
						{
							uB += pSrc[ 0 ];
							uG += pSrc[ 1 ];
							uR += pSrc[ 2 ];
						}
					}
					const uint32_t uN = (uint32_t)( f * f );
					uint8_t ucR = (uint8_t)( uR / uN ), ucG = (uint8_t)( uG / uN ), ucB = (uint8_t)( uB / uN );
					// The HUD layer's inverted colour mode reads G == 0 as its own marker: never send it.
					if ( ucG == 0 && ( ucR || ucB ) )
						ucG = 1;
					uint8_t *pOut = pDst + (size_t)x * 4;
					pOut[ 0 ] = ucR;
					pOut[ 1 ] = ucG;
					pOut[ 2 ] = ucB;
					pOut[ 3 ] = 255;
				}
			}
			r.nW = nW;
			r.nH = nH;
			r.bPendingUpload = true;
			r.bShow = true;
			r.ulGeneration++;
			force_repaint(); // an idle client would not paint again by itself: the picture must reach the screen
			TrueSens_LogScope().infof( "reference picture: %dx%d (crop %dx%d at %d,%d of %ux%u, reduced x%d), capture took %.2f ms", nW, nH,
				crop.w, crop.h, crop.x, crop.y, uWidth, uHeight, f, (double)ulCaptureNs / 1e6 );
		}
	}

	void TrueSens_ReferenceClear()
	{
		Reference &r = Ref();
		r.ulSeq++; // a capture still on its way is no longer wanted
		if ( r.bShow )
			r.ulGeneration++;
		r.bShow = false;
		r.bPendingUpload = false;
	}

	void TrueSens_ReferenceRequest()
	{
		TrueSens_ReferenceClear();
		const uint64_t ulSeq = Ref().ulSeq;
		GamescopeScreenshotInfo info;
		info.eScreenshotType = GAMESCOPE_CONTROL_SCREENSHOT_TYPE_BASE_PLANE_ONLY; // the game alone: no Shell, no HUD
		info.pfnMemory = [ ulSeq ]( const uint8_t *pPixels, uint32_t uWidth, uint32_t uHeight, uint32_t uPitch,
		                             uint64_t ulCaptureNs, const char *pszFailure )
		{
			Deliver( ulSeq, pPixels, uWidth, uHeight, uPitch, ulCaptureNs, pszFailure );
		};
		CScreenshotManager::Get().TakeScreenshot( std::move( info ) );
	}

	uint64_t TrueSens_ReferenceGeneration()
	{
		return Ref().ulGeneration;
	}

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

		// The reference picture, directly below the plate; none while it cannot be taken.
		Reference &r = Ref();
		if ( r.bShow && r.nW > 0 && r.nH > 0 )
		{
			const truesens::DisplayRect d = truesens::ReferenceDisplay( flWidth, flHeight, p1.y, flScale, (float)r.nW / (float)r.nH );
			if ( d.ok )
			{
				ImGuiContext *pCtx = ImGui::GetCurrentContext();
				if ( r.pCtx != pCtx )
				{
					ImGui::RegisterUserTexture( &r.tex );
					r.pCtx = pCtx;
				}
				if ( r.bPendingUpload )
				{
					r.tex.UseColors = true;
					ImTextureDataQueueUpload( &r.tex, 0, 0, r.nW, r.nH );
					r.bPendingUpload = false;
				}
				const ImVec2 q0( d.x, d.y ), q1( d.x + d.w, d.y + d.h );
				// Half a texel in from the edge, so the filter never blends in the unused rest of the texture.
				const ImVec2 uv0( 0.5f / kTexSide, 0.5f / kTexSide );
				const ImVec2 uv1( ( r.nW - 0.5f ) / kTexSide, ( r.nH - 0.5f ) / kTexSide );
				pDrawList->AddImage( r.tex.GetTexRef(), q0, q1, uv0, uv1 );
				pDrawList->AddRect( q0, q1, palette::Accent( 0.5f ), 0.0f, 0, std::max( 1.0f, flScale ) );

				// The spot the player aimed at: a ring with a dark halo (any picture reads) and a centre dot.
				const ImVec2 c( ( q0.x + q1.x ) * 0.5f, ( q0.y + q1.y ) * 0.5f );
				const float flR = 8.0f * flScale;
				pDrawList->AddCircle( c, flR, IM_COL32( 0, 0, 0, 200 ), 0, 4.0f * flScale );
				pDrawList->AddCircle( c, flR, palette::Accent( 1.0f ), 0, std::max( 1.5f, 2.0f * flScale ) );
				pDrawList->AddCircleFilled( c, std::max( 2.0f, 2.0f * flScale ), palette::Accent( 1.0f ) );
			}
		}
		return p1.y;
	}
}
