// EffectPreview.cpp -- see EffectPreview.h.
//
// THE THREE PIECES, AND WHERE EACH ONE'S COST LANDS
//
//  1. THE CAPTURE. vulkan_effects_preview_request() arms one composite; that
//     composite runs cs_effects_preview.comp (a 256x144 dispatch), copies its
//     output and the Adaptive Brightness history into host-mappable staging,
//     and waits once for the GPU. Armed when the strip appears on screen --
//     once per Inspector open -- never per frame.
//
//  2. THE RE-GRADE. abpreview::Compose() runs effects_curve.h over the
//     36,864 captured pixels and writes an RGBA image whose right half is
//     graded. Run only when the capture or a slider actually changed, i.e.
//     at most once per overlay frame while a slider is being dragged.
//
//  3. THE UPLOAD. The image is an ImTextureData registered with the overlay's
//     ImGui context (ImGui::RegisterUserTexture, 1.92's texture API), so the
//     existing ImGui_ImplVulkan backend uploads it exactly as it uploads the
//     font atlas. No Vulkan code here, no second descriptor set to manage.
//
// WHY A CPU RE-GRADE AND NOT A SECOND GPU PASS. The strip has to answer
// "what would THIS frame look like at THIS slider position", which is a
// question about a frame that is no longer being composited. Re-dispatching
// would mean keeping the captured frame on the GPU and running a pass per
// slider tick from the overlay thread, against the compositor's own
// submissions -- more moving parts than a memcpy-sized loop over a picture
// smaller than a phone screenshot. Measured cost is in shader-effects.md.
//
// NOTHING HERE WRITES INTO THE COMPOSITING PATH. The capture reads layer 0
// and the history; this file only ever reads g_nativeEffects, which
// PanelShaders.cpp owns.

#include "EffectPreview.h"

#include "EffectPreviewMath.h"
#include "UI/Colors.h"
#include "UI/Tokens.h"

#include "rendervulkan.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture / ImTextureDataQueueUpload

// The base layer's colourspace, stashed per composite by rendervulkan.cpp.
// Declared the same way PanelShaders.cpp declares it -- one relaxed atomic
// written on the steamcompmgr thread, read here on the same thread.
extern std::atomic<GamescopeAppTextureColorspace> g_eLastBaseLayerColorspace;

namespace gamescope::overlay
{
	using namespace gamescope::ui;

	namespace
	{
		constexpr int kW = (int)kAbPreviewWidth;
		constexpr int kH = (int)kAbPreviewHeight;

		// The strip is "not on screen any more" after this long without a
		// draw, and the next appearance captures a fresh frame.
		//
		// `Why a timeout rather than an overlay-visibility hook:` the strip
		// has more ways to leave the screen than the overlay has of closing
		// -- another row selected, another rail area, the Inspector hidden,
		// the DETAILS page, a scroll that takes it out of view. All of them
		// stop the draw call, and all of them should re-capture, because the
		// contract the user was promised is "the frame when the UI was
		// opened", not "the frame when gamescope started". One rule covers
		// every case, and it is also what makes a stale frame from an
		// earlier session structurally impossible: nothing older than this
		// can ever be displayed.
		constexpr int64_t kStaleMs = 250;

		struct State
		{
			AbPreviewFrame_t frame;          // the captured pixels + statistics
			bool  bHaveFrame = false;

			// The ImGui-side image. Owned for the life of the process (the
			// overlay context is created once and never destroyed), which is
			// why it is a plain member and not something reference-counted.
			ImTextureData tex;
			ImGuiContext *pTexCtx = nullptr;

			// What the currently-uploaded image was built from. The re-grade
			// is skipped when none of these moved.
			uint64_t ulComposedGeneration = 0;
			bool     bComposedValid = false;
			abpreview::Params composed{};

			int64_t nLastDrawMs = 0;
		};

		State &St()
		{
			static State s;
			return s;
		}

		int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
		}

		bool SameParams( const abpreview::Params &a, const abpreview::Params &b )
		{
			return a.bDynamic == b.bDynamic
				&& a.flTarget == b.flTarget
				&& a.flMinGain == b.flMinGain
				&& a.flMaxGain == b.flMaxGain
				&& a.flStrength == b.flStrength
				&& a.flLocal == b.flLocal;
		}

		// The parameters the pre-pass is running with right now. Read from
		// g_nativeEffects rather than from the config: that struct IS what
		// the shader was handed, it is written by PanelShaders.cpp on this
		// same (steamcompmgr) thread, and reading it means the strip can
		// never disagree with the frame about what "current" means.
		abpreview::Params CurrentParams()
		{
			abpreview::Params p;
			p.bDynamic   = g_nativeEffects.bAbDynamic;
			p.flTarget   = g_nativeEffects.flAbTarget;
			p.flMinGain  = g_nativeEffects.flAbMinGain;
			p.flMaxGain  = g_nativeEffects.flAbMaxGain;
			p.flStrength = g_nativeEffects.flAbStrength;
			// Whole image has no per-pixel curve to fit locally, and the host
			// masks the uniform to 0 there (EffectsPushData_t). Mirrored here
			// so the strip shows what the frame does, not what the slider says.
			p.flLocal    = g_nativeEffects.bAbDynamic ? g_nativeEffects.flAbLocal : 0.0f;
			return p;
		}

		bool BaseLayerIsSdr()
		{
			const GamescopeAppTextureColorspace eCs =
				g_eLastBaseLayerColorspace.load( std::memory_order_relaxed );
			return eCs == GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR
				|| eCs == GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
		}

		void EnsureTexture( State &st )
		{
			ImGuiContext *pCtx = ImGui::GetCurrentContext();
			if ( st.pTexCtx == pCtx && st.tex.Pixels != nullptr )
				return;
			if ( st.tex.Pixels == nullptr )
				st.tex.Create( ImTextureFormat_RGBA32, kW, kH );
			// Registered per context. In practice this runs once: the
			// settings overlay creates its context at startup and never
			// destroys it (SettingsOverlay.cpp), so there is no second
			// context for this strip to migrate to.
			if ( st.pTexCtx != pCtx )
			{
				ImGui::RegisterUserTexture( &st.tex );
				st.pTexCtx = pCtx;
			}
		}

		void Recompose( State &st, const abpreview::Params &p )
		{
			EnsureTexture( st );
			if ( st.tex.Pixels == nullptr )
				return;

			abpreview::Stats stats;
			stats.flMean = st.frame.flMean;
			stats.flP2   = st.frame.flP2;
			stats.flP50  = st.frame.flP50;
			stats.flP98  = st.frame.flP98;
			stats.pflLocal = st.frame.flLocal;
			stats.nGrid  = (int)kAbPreviewLocalGrid;

			abpreview::Compose( st.frame.rgb, kW, kH, stats, p,
			                    (uint8_t *)st.tex.Pixels );
			st.tex.UseColors = true;
			ImTextureDataQueueUpload( &st.tex, 0, 0, kW, kH );

			st.composed = p;
			st.ulComposedGeneration = st.frame.ulGeneration;
			st.bComposedValid = true;
		}

		// The placeholder's message, broken over at most two centred lines.
		// Written here rather than reusing the Inspector's DrawWrapped()
		// because that one is Shell.cpp's private helper and is
		// left-aligned; the empty state wants its sentence centred in the
		// box it is standing in for.
		void DrawCentredMessage( const ImRect &rcBox, const char *pszMessage )
		{
			const float flMax = rcBox.GetWidth() - Px( tok::kS ) * 2.0f;
			const float flLineH = MeasureText( TypeRole::Meta, "Ag" ).y;

			// Greedy: the longest prefix ending on a space that still fits.
			std::string sAll( pszMessage );
			std::string sLine1 = sAll, sLine2;
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
			float y = rcBox.Min.y + ( rcBox.GetHeight() - flTotal ) * 0.5f;
			const ImRect rcIn( rcBox.Min.x + Px( tok::kS ), y, rcBox.Max.x - Px( tok::kS ), y + flLineH );
			DrawText( rcIn, TypeRole::Meta, Col( Role::TextMeta ), sLine1.c_str(), TextAlign::Center );
			if ( !sLine2.empty() )
			{
				const ImRect rc2( rcIn.Min.x, y + flLineH, rcIn.Max.x, y + flLineH * 2.0f );
				DrawText( rc2, TypeRole::Meta, Col( Role::TextMeta ), sLine2.c_str(), TextAlign::Center );
			}
		}

		void DrawPlaceholder( const ImRect &rcImage, const char *pszMessage )
		{
			ImDrawList *pDl = ImGui::GetWindowDrawList();
			// Never a black rectangle: an empty preview that looks like a
			// broken one is exactly what this wording exists to avoid. The
			// same raised fill + hairline every empty block in the shell uses.
			pDl->AddRectFilled( rcImage.Min, rcImage.Max, Col( Role::SurfaceRaised ) );
			pDl->AddRect( rcImage.Min, rcImage.Max, Col( Role::Line ) );
			DrawCentredMessage( rcImage, pszMessage );
		}
	}

	bool AbPreview_BindingLine( std::string &sOut )
	{
		State &st = St();

		const bool bEnabled   = g_nativeEffects.bAdaptiveBrightness;
		const bool bSupported = BaseLayerIsSdr();
		if ( !bEnabled || !bSupported )
			return false;

		// Same capture the strip uses, same idempotent request. Asking here
		// too is what lets the fact stand on its own: the row is in the same
		// area as the Adaptive Brightness switch, so a frame is always a
		// composite or two away.
		vulkan_effects_preview_request();
		if ( vulkan_effects_preview_fetch( &st.frame, st.frame.ulGeneration ) )
			st.bHaveFrame = true;
		if ( st.frame.ulGeneration == 0 )
			return false;

		const abpreview::Params p = CurrentParams();
		if ( !p.bDynamic )
		{
			// Whole image has no curve to clamp: its single gain is
			// target/mean inside the user's bounds, so name the bound that
			// bites rather than pretending the Dynamic classifier applies.
			const float flWant = p.flTarget / std::max( st.frame.flMean, 0.001f );
			sOut = flWant > p.flMaxGain ? "gain is at Max gain"
			     : flWant < p.flMinGain ? "gain is at Min gain"
			                            : "none -- the average is on Target brightness";
			return true;
		}

		namespace ec = gamescope::effects_curve;
		sOut = ec::ab_binding_text( ec::ab_dyn_binding( st.frame.flP2, st.frame.flP50, st.frame.flP98,
		                                               p.flTarget, p.flMinGain, p.flMaxGain ) );
		return true;
	}

	void AbPreview_Draw( const ImRect &rcBlock )
	{
		State &st = St();

		const int64_t nNow = NowMs();
		const bool bReappeared = ( nNow - st.nLastDrawMs ) > kStaleMs;
		st.nLastDrawMs = nNow;

		const controls::ComparePreviewLayout lay = controls::LayoutComparePreview( rcBlock );

		const bool bEnabled   = g_nativeEffects.bAdaptiveBrightness;
		const bool bSupported = BaseLayerIsSdr();

		// Coming back on screen drops whatever was held: the promise is a
		// frame from when the UI was opened, and a frame from the last time
		// it was open is exactly the "stale frame from another session" this
		// must never show.
		if ( bReappeared )
		{
			st.bHaveFrame = false;
			st.bComposedValid = false;
		}

		// Ask for a frame while the effect is on and we do not have one. The
		// request is idempotent, and a composite that cannot serve it (HDR
		// content, the pre-pass not running) simply leaves it armed, which
		// is what keeps the placeholder up instead of a black box.
		if ( bEnabled && bSupported && !st.bHaveFrame )
			vulkan_effects_preview_request();

		// ALWAYS against the last generation we have ever seen, never against
		// 0: after bReappeared cleared bHaveFrame, asking for "anything at
		// all" would hand back the frame from the LAST time the strip was
		// open -- the stale-frame case this whole dance exists to prevent.
		// Only a capture taken since then is strictly newer.
		if ( vulkan_effects_preview_fetch( &st.frame, st.frame.ulGeneration ) )
		{
			st.bHaveFrame = true;
			st.bComposedValid = false;
		}

		// The labels are drawn whatever the state, so the strip keeps its
		// shape (and the panel below it keeps its position) while it waits
		// for a frame.
		DrawText( lay.rcLeftLabel,  TypeRole::Meta, Col( Role::TextMeta ), "BEFORE", TextAlign::Left );
		DrawText( lay.rcRightLabel, TypeRole::Meta, Col( Role::TextMeta ), "AFTER",  TextAlign::Right );

		const controls::ComparePreviewStatus status =
			controls::ComparePreviewStatusFor( bEnabled, bSupported, st.bHaveFrame );
		if ( status.eState != controls::ComparePreviewState::Ready )
		{
			DrawPlaceholder( lay.rcImage, status.pszMessage );
			return;
		}

		const abpreview::Params p = CurrentParams();
		if ( !st.bComposedValid || st.ulComposedGeneration != st.frame.ulGeneration
		     || !SameParams( st.composed, p ) )
		{
			Recompose( st, p );
		}
		if ( !st.bComposedValid )
		{
			DrawPlaceholder( lay.rcImage, "Preview unavailable." );
			return;
		}

		ImDrawList *pDl = ImGui::GetWindowDrawList();
		pDl->AddImage( st.tex.GetTexRef(), lay.rcImage.Min, lay.rcImage.Max );

		// The divider. Two coats on purpose: the strip sits on whatever the
		// game was showing, so a single hairline of any one colour vanishes
		// against half the content it could land on. A dark line with a
		// bright one down its middle is visible on both.
		const float flHalf = ImMax( 2.0f, Px( 2.0f ) );
		pDl->AddRectFilled( ImVec2( lay.flDividerX - flHalf, lay.rcImage.Min.y ),
		                    ImVec2( lay.flDividerX + flHalf, lay.rcImage.Max.y ),
		                    IM_COL32( 0, 0, 0, 140 ) );
		const float flThin = ImMax( 1.0f, Px( 1.0f ) ) * 0.5f;
		pDl->AddRectFilled( ImVec2( lay.flDividerX - flThin, lay.rcImage.Min.y ),
		                    ImVec2( lay.flDividerX + flThin, lay.rcImage.Max.y ),
		                    IM_COL32( 255, 255, 255, 216 ) );

		// The same hairline boundary every block in the Inspector carries.
		pDl->AddRect( lay.rcImage.Min, lay.rcImage.Max, Col( Role::Line ) );
	}
}
