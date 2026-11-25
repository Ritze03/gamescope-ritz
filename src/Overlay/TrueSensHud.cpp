// True Sens: the lasting hint at the top of the screen while a recording runs (D16), and the
// picture pair under it (D25): the left half a frozen crop of the middle of the game picture, the
// right half a live view of the same part built by the composite (FrameInfo_t::zoom, placed).
// Drawn by FpsDisplay.cpp into the HUD layer's background draw list (the layer is drawn
// over the game, under the Shell, and frame generation does not touch it), only while
// TrueSens_RecordingActive() (armed or recording). The text itself is the pure gamescope::truesens::HintLines().
// superdoc/features/true-sens.md, "Recording hint".
//
// Runs on the compositor thread inside the HUD's ImGui frame (its own context, which carries
// the whole font set of Fonts.h).

#include "TrueSens.h"

#include "Fonts.h"
#include "Palette.h"

#include "log.hpp"
#include "steamcompmgr.hpp"
#include "steamcompmgr_shared.hpp"
#include "rendervulkan.hpp"
#include "Zoom.h"

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
			// Which part of the game picture the frozen half shows, as fractions of the CAPTURED
			// picture: the live half asks the composite for exactly this part of its base texture.
			float flCenterFracX = 0.5f, flCenterFracY = 0.5f, flHeightFrac = (float)truesens::kReferenceHeightFraction;
			bool bFailureLogged = false;    // the "live view cannot be built" line, once per recording
		};

		// The live half: DrawHud decides each paint whether the pair is shown and leaves the rectangle
		// here for TrueSens_FillLiveRequest(), called later in the same paint_all(). Consumed (pending
		// reset) by the filler, so a paint where DrawHud did not run requests nothing.
		struct LiveRequest
		{
			bool bPending = false;
			truesens::PairRect pair;
		};

		LiveRequest &Live()
		{
			static LiveRequest l;
			return l;
		}

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
			r.flCenterFracX = ( (float)crop.x + (float)crop.w * 0.5f ) / (float)uWidth;
			r.flCenterFracY = ( (float)crop.y + (float)crop.h * 0.5f ) / (float)uHeight;
			r.flHeightFrac = (float)crop.h / (float)uHeight;

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
			// One extra column and row that repeat the edge, so the filter at the picture's
			// edge never blends in the unused rest of the texture (the picture is sampled edge to
			// edge, so it lines up texel for texel with the live half).
			if ( nW < kTexSide )
				for ( int y = 0; y < nH; y++ )
				{
					uint8_t *pRow = (uint8_t *)r.tex.Pixels + (size_t)y * nPitch;
					memcpy( pRow + (size_t)nW * 4, pRow + (size_t)( nW - 1 ) * 4, 4 );
				}
			if ( nH < kTexSide )
				memcpy( (uint8_t *)r.tex.Pixels + (size_t)nH * nPitch, (uint8_t *)r.tex.Pixels + (size_t)( nH - 1 ) * nPitch,
				        (size_t)std::min( nW + 1, kTexSide ) * 4 );
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
		r.bFailureLogged = false;
		Live().bPending = false;
		g_nPlacedZoomFailure.store( 0, std::memory_order_relaxed ); // the next recording starts optimistic
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

		// Two lines per phase (armed: aim and click; recording: turn and click again).
		const truesens::HintText hint = truesens::HintLines( TrueSens_RecordingTarget(), TrueSens_RecordingPhase() );
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

		// The reference picture, directly below the plate; none while it cannot be taken. Two square
		// halves (left frozen, right live) as long as the composite can build the live half; if it
		// cannot, only the frozen square, centred. The live half is drawn by the COMPOSITE under
		// this layer, so here the right half stays transparent: only border, divider and mark.
		Live().bPending = false;
		Reference &r = Ref();
		if ( r.bShow && r.nW > 0 && r.nH > 0 )
		{
			const truesens::PairRect pr = truesens::ReferencePair( (int)flWidth, (int)flHeight, p1.y, flScale );
			if ( pr.ok )
			{
				const truesens::Phase phase = TrueSens_RecordingPhase();
				const bool bLiveWanted = ( phase == truesens::Phase::Recording || phase == truesens::Phase::Finished ) && !Zoom_IsActive();
				const int nFailure = g_nPlacedZoomFailure.load( std::memory_order_relaxed );
				const bool bPair = bLiveWanted && nFailure == 0;
				if ( bLiveWanted && nFailure != 0 && !r.bFailureLogged )
				{
					r.bFailureLogged = true;
					TrueSens_LogScope().infof( "live view not built (%s): showing the frozen picture alone",
						nFailure == 2 ? "no free layer slot" : "no usable base picture (HDR, YCbCr or none) or no texture" );
				}

				ImGuiContext *pCtx = ImGui::GetCurrentContext();
				if ( r.pCtx != pCtx )
				{
					ImGui::RegisterUserTexture( &r.tex );
					r.pCtx = pCtx;
				}
				if ( r.bPendingUpload )
				{
					r.tex.UseColors = true;
					ImTextureDataQueueUpload( &r.tex, 0, 0, std::min( r.nW + 1, kTexSide ), std::min( r.nH + 1, kTexSide ) );
					r.bPendingUpload = false;
				}

				const float S = (float)pr.side;
				const float flLeft = bPair ? (float)pr.x : (float)truesens::ReferenceSingleX( (int)flWidth, pr.side );
				const ImVec2 q0( flLeft, (float)pr.y ), q1( flLeft + S, (float)pr.y + S );
				// Edge to edge when the texture has room for the repeated edge texel; else half a texel in.
				const float flIn = ( r.nW < kTexSide && r.nH < kTexSide ) ? 0.0f : 0.5f;
				const ImVec2 uv0( flIn / kTexSide, flIn / kTexSide );
				const ImVec2 uv1( ( r.nW - flIn ) / kTexSide, ( r.nH - flIn ) / kTexSide );
				pDrawList->AddImage( r.tex.GetTexRef(), q0, q1, uv0, uv1 );

				const float flLine = std::max( 1.0f, flScale );
				const ImU32 uBorder = palette::Accent( 0.5f );
				const float flRight = bPair ? flLeft + 2.0f * S : q1.x;
				pDrawList->AddRect( q0, ImVec2( flRight, q1.y ), uBorder, 0.0f, 0, flLine );
				if ( bPair )
					pDrawList->AddRectFilled( ImVec2( flLeft + S, q0.y ), ImVec2( flLeft + S + flLine, q1.y ), uBorder );

				// The spot the player aimed at: a ring with a dark halo (any picture reads) and a centre dot, on each half.
				const float flR = 8.0f * flScale;
				for ( int nHalf = 0; nHalf < ( bPair ? 2 : 1 ); nHalf++ )
				{
					const ImVec2 c( flLeft + S * 0.5f + S * (float)nHalf, q0.y + S * 0.5f );
					pDrawList->AddCircle( c, flR, IM_COL32( 0, 0, 0, 200 ), 0, 4.0f * flScale );
					pDrawList->AddCircle( c, flR, palette::Accent( 1.0f ), 0, std::max( 1.5f, 2.0f * flScale ) );
					pDrawList->AddCircleFilled( c, std::max( 2.0f, 2.0f * flScale ), palette::Accent( 1.0f ) );
				}

				if ( bPair )
				{
					Live().bPending = true;
					Live().pair = pr;
				}
			}
		}
		return p1.y;
	}

	void TrueSens_FillLiveRequest( FrameInfo_t *pFrameInfo )
	{
		LiveRequest &l = Live();
		if ( !l.bPending )
			return;
		l.bPending = false;
		const Reference &r = Ref();
		// The slot is the user's Zoom's when it is active; the two cannot meet (a recording refuses
		// Zoom and Zoom spoils it), so this is only a guard.
		if ( !r.bShow || !l.pair.ok || pFrameInfo->zoom.bActive )
			return;
		FrameInfo_t::Zoom_t &z = pFrameInfo->zoom;
		z = FrameInfo_t::Zoom_t{};
		z.bActive = true;
		z.bPlaced = true;
		z.flPlaceX = (float)( l.pair.x + l.pair.side );
		z.flPlaceY = (float)l.pair.y;
		z.flPlaceW = (float)l.pair.side;
		z.flPlaceH = (float)l.pair.side;
		z.flSrcCenterX = r.flCenterFracX;
		z.flSrcCenterY = r.flCenterFracY;
		z.flSrcHeightFrac = r.flHeightFrac;
		z.flAlpha = 1.0f;
		z.flSharpen = 0.0f;
		// The projection samples the base layer, so the base must be IN the composite (as for Zoom).
		pFrameInfo->bNeedsDestinationBlend = true;
	}
}
