// UiBoxPreview.cpp -- see UiBoxPreview.h.
//
// THE THREE PIECES
//
//  1. THE CAPTURE is the frame-generation host's (FrameGen/FrameGenHost.cpp,
//     BoxPreviewRecord / BoxPreviewPoll, cs_fg_crop.comp): at most ten times a
//     second, only while BoxPreviewWanted() is being called, one tiny dispatch
//     in the command buffer the host already submits for a new real frame, read
//     back once that has retired. Nothing here waits for the GPU.
//
//  2. THE TEXTURE is an ImTextureData registered with the overlay's ImGui
//     context, the same way EffectPreview.cpp's strip is (ImGui 1.92's user
//     textures: the existing ImGui_ImplVulkan backend uploads it like the font
//     atlas). The crop is enlarged by a WHOLE number on the CPU (nearest, pixel
//     replication) before upload, so each game pixel is a crisp block rather than
//     a bilinear smear -- the picture exists to judge individual pixels.
//
//  3. THE DRAWING: the picture, everything outside the box dimmed, the box's
//     outline (a dark halo under an accent line, so it reads on any content), a
//     caption with the box size in game pixels.
//
// `Why a fixed 192 px texture and not one sized per capture:` the capture's size
// follows the box (24 px box -> ~56 px crop), and re-creating a texture per slider
// tick is exactly the churn ImGui's texture queue is not for. One fixed texture,
// a sub-rectangle uploaded.

#include "UiBoxPreview.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "FrameGen/FrameGenHost.h"
#include "UI/Colors.h"
#include "UI/Tokens.h"
#include "steamcompmgr.hpp"   // get_time_in_nanos()

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture / ImTextureDataQueueUpload

namespace gamescope::overlay
{
	using namespace gamescope::ui;

	namespace
	{
		constexpr int kTexSide = 192;                         // the texture, pixels
		constexpr uint64_t kStaleNs = 2000ull * 1000ull * 1000ull;   // older than this: "waiting for a frame"

		struct State
		{
			std::unique_ptr<fghost::BoxPreview> pPreview;     // the newest capture (64 KB: not on the stack)
			uint64_t ulGeneration = 0;
			bool bHave = false;

			ImTextureData tex;
			ImGuiContext *pTexCtx = nullptr;
			int nScale = 1;                                   // the integer enlargement of the uploaded picture
		};

		State &St()
		{
			static State s;
			return s;
		}

		void EnsureTexture( State &st )
		{
			ImGuiContext *pCtx = ImGui::GetCurrentContext();
			if ( st.pTexCtx == pCtx && st.tex.Pixels != nullptr )
				return;
			if ( st.tex.Pixels == nullptr )
				st.tex.Create( ImTextureFormat_RGBA32, kTexSide, kTexSide );
			if ( st.pTexCtx != pCtx )
			{
				ImGui::RegisterUserTexture( &st.tex );
				st.pTexCtx = pCtx;
			}
		}

		// Enlarge the capture by the largest whole factor that fits the texture and
		// queue the upload.
		void Upload( State &st )
		{
			EnsureTexture( st );
			if ( st.tex.Pixels == nullptr || !st.pPreview )
				return;
			const fghost::BoxPreview &bp = *st.pPreview;
			const int nW = (int)bp.uW, nH = (int)bp.uH;
			if ( nW <= 0 || nH <= 0 )
				return;
			const int nScale = std::max( 1, kTexSide / std::max( nW, nH ) );
			const int nPitch = st.tex.GetPitch();
			for ( int y = 0; y < nH * nScale; y++ )
			{
				uint8_t *pDst = (uint8_t *)st.tex.Pixels + (size_t)y * nPitch;
				const uint8_t *pSrcRow = bp.rgba + (size_t)( y / nScale ) * nW * 4;
				for ( int x = 0; x < nW * nScale; x++ )
					memcpy( pDst + (size_t)x * 4, pSrcRow + (size_t)( x / nScale ) * 4, 4 );
			}
			st.tex.UseColors = true;
			ImTextureDataQueueUpload( &st.tex, 0, 0, nW * nScale, nH * nScale );
			st.nScale = nScale;
		}

		float SideFor( float flWidth )
		{
			return std::min( flWidth, Px( 192.0f ) );
		}

		float CaptionHeight()
		{
			return MeasureText( TypeRole::Meta, "Ag" ).y * 2.0f;
		}

		void DrawPlaceholder( const ImRect &rc, const char *pszMessage )
		{
			ImDrawList *pDl = ImGui::GetWindowDrawList();
			pDl->AddRectFilled( rc.Min, rc.Max, Col( Role::SurfaceRaised ) );
			pDl->AddRect( rc.Min, rc.Max, Col( Role::Line ) );

			// Two centred lines at most, broken on a space (the strip's own placeholder
			// does the same; that helper is private to EffectPreview.cpp).
			const float flMax = rc.GetWidth() - Px( tok::kS ) * 2.0f;
			const float flLineH = MeasureText( TypeRole::Meta, "Ag" ).y;
			std::string sAll( pszMessage ), sLine1 = sAll, sLine2;
			if ( MeasureText( TypeRole::Meta, sAll.c_str() ).x > flMax )
			{
				size_t nBreak = std::string::npos;
				for ( size_t i = sAll.find( ' ' ); i != std::string::npos; i = sAll.find( ' ', i + 1 ) )
				{
					if ( MeasureText( TypeRole::Meta, sAll.substr( 0, i ).c_str() ).x > flMax )
						break;
					nBreak = i;
				}
				if ( nBreak != std::string::npos )
				{
					sLine1 = sAll.substr( 0, nBreak );
					sLine2 = sAll.substr( nBreak + 1 );
				}
			}
			const float flTotal = sLine2.empty() ? flLineH : flLineH * 2.0f;
			const float y = rc.Min.y + ( rc.GetHeight() - flTotal ) * 0.5f;
			DrawText( ImRect( rc.Min.x + Px( tok::kS ), y, rc.Max.x - Px( tok::kS ), y + flLineH ),
			          TypeRole::Meta, Col( Role::TextMeta ), sLine1.c_str(), TextAlign::Center );
			if ( !sLine2.empty() )
				DrawText( ImRect( rc.Min.x + Px( tok::kS ), y + flLineH, rc.Max.x - Px( tok::kS ), y + flLineH * 2.0f ),
				          TypeRole::Meta, Col( Role::TextMeta ), sLine2.c_str(), TextAlign::Center );
		}
	}

	float UiBoxPreview_Height( float flWidth )
	{
		return SideFor( flWidth ) + Px( tok::kS ) + CaptionHeight();
	}

	void UiBoxPreview_Draw( const ImRect &rcBlock )
	{
		State &st = St();
		if ( !st.pPreview )
			st.pPreview = std::make_unique<fghost::BoxPreview>();

		// Ask the renderer for captures; the request lapses by itself.
		fghost::BoxPreviewWanted();
		if ( fghost::GetBoxPreview( st.pPreview.get(), st.ulGeneration ) )
		{
			st.ulGeneration = st.pPreview->ulGeneration;
			st.bHave = true;
			Upload( st );
		}

		const float flSide = SideFor( rcBlock.GetWidth() );
		const ImRect rcImage( rcBlock.Min.x, rcBlock.Min.y, rcBlock.Min.x + flSide, rcBlock.Min.y + flSide );
		const ImRect rcCaption( rcBlock.Min.x, rcImage.Max.y + Px( tok::kS ), rcBlock.Max.x, rcBlock.Max.y );

		const fghost::BoxPreview &bp = *st.pPreview;
		const bool bFresh = st.bHave && bp.ulCapturedNs
			&& get_time_in_nanos() - bp.ulCapturedNs < kStaleNs;

		if ( !fghost::Active() )
		{
			DrawPlaceholder( rcImage, "Turn Frame generation on to see what is under the box." );
			return;
		}
		if ( !bFresh || st.pTexCtx == nullptr )
		{
			DrawPlaceholder( rcImage, "Waiting for a game frame." );
			return;
		}

		ImDrawList *pDl = ImGui::GetWindowDrawList();
		const float flUv = (float)( (int)bp.uW * st.nScale ) / (float)kTexSide;
		pDl->AddImage( st.tex.GetTexRef(), rcImage.Min, rcImage.Max, ImVec2( 0, 0 ), ImVec2( flUv, flUv ) );

		// The box, in screen space. Picture pixels -> screen: flSide / uW each.
		const float flK = flSide / (float)bp.uW;
		const ImVec2 bmin( rcImage.Min.x + bp.flBoxX * flK, rcImage.Min.y + bp.flBoxY * flK );
		const ImVec2 bmax( bmin.x + bp.flBoxW * flK, bmin.y + bp.flBoxH * flK );

		// Everything outside the box dimmed, so what the box covers is what stands out.
		const ImU32 colDim = IM_COL32( 0, 0, 0, 120 );
		pDl->AddRectFilled( rcImage.Min, ImVec2( rcImage.Max.x, bmin.y ), colDim );                    // above
		pDl->AddRectFilled( ImVec2( rcImage.Min.x, bmax.y ), rcImage.Max, colDim );                   // below
		pDl->AddRectFilled( ImVec2( rcImage.Min.x, bmin.y ), ImVec2( bmin.x, bmax.y ), colDim );      // left
		pDl->AddRectFilled( ImVec2( bmax.x, bmin.y ), ImVec2( rcImage.Max.x, bmax.y ), colDim );      // right

		// Two coats, like the strip's divider: a dark halo under the accent line so it
		// reads on bright and dark content alike.
		const float flHalo = std::max( 3.0f, Px( 3.0f ) );
		const float flLine = std::max( 1.0f, Px( 1.5f ) );
		pDl->AddRect( bmin, bmax, IM_COL32( 0, 0, 0, 200 ), 0.0f, 0, flHalo );
		pDl->AddRect( bmin, bmax, Accent( 1.0f ), 0.0f, 0, flLine );
		pDl->AddRect( rcImage.Min, rcImage.Max, Col( Role::Line ) );

		char sz[ 160 ];
		std::snprintf( sz, sizeof( sz ), "Box %u × %u px%s", bp.uBoxW, bp.uBoxH,
			bp.uFactor > 1 ? "  (picture reduced)" : "" );
		const float flLineH = MeasureText( TypeRole::Meta, "Ag" ).y;
		DrawText( ImRect( rcCaption.Min.x, rcCaption.Min.y, rcCaption.Max.x, rcCaption.Min.y + flLineH ),
		          TypeRole::Meta, Col( Role::TextLabel ), sz, TextAlign::Left );
		std::snprintf( sz, sizeof( sz ), bp.bUiOn ? "Game frame %u × %u" : "Game frame %u × %u · UI protection is off",
			bp.uGameW, bp.uGameH );
		DrawText( ImRect( rcCaption.Min.x, rcCaption.Min.y + flLineH, rcCaption.Max.x, rcCaption.Min.y + flLineH * 2.0f ),
		          TypeRole::Meta, Col( Role::TextMeta ), sz, TextAlign::Left );
	}
}
