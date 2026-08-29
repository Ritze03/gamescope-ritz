// ============================================================================
// TEMPORARY DIAGNOSTIC -- overlay_e2_fontlab -- see FontLab.h's top comment.
// ============================================================================
//
// WHAT THIS DRAWS AND WHY EACH ROW IS HERE
// -----------------------------------------------------------------------
// A half-transparent grey vertical line beside a glyph (the report's example
// is 'y') is the classic signature of atlas neighbour-bleed: the glyph's UV
// rect sampling a sliver of whatever is packed next to it in the font atlas.
// Candidate causes, one row each, every row after the baseline changing
// exactly one thing from it:
//
//   0  BASELINE            -- the launcher's middle row exactly as the real
//                              UI draws it: fonts::Style::Label (Geist Sans
//                              Regular) through fonts::RasterSize(), the
//                              SAME ImFont pointer Controls.cpp's DrawText()
//                              uses -- not a lookalike second copy.
//   1  Oversample 1x1      -- BAKE-TIME. No supersampling. If the sliver is
//                              an oversample/padding interaction, turning
//                              oversampling off should make it vanish.
//   2  Oversample 4x4      -- BAKE-TIME. Heavy supersampling. If the sliver
//                              gets WORSE here, oversampling is widening the
//                              rasterizer's effective footprint into the
//                              padding gap next to it.
//   3  RasterizerDensity 2 -- BAKE-TIME. Denser rasterization (bakes as if
//                              at 2x, distinct mechanism from oversample).
//                              Tests whether the artifact is a
//                              rasterization-resolution effect rather than
//                              an oversample one.
//   4  Pixel-snapped X     -- PER-DRAW. Same baseline font/bake, drawn at
//                              floor(x) instead of the row's real
//                              (realistically fractional) pen position. If
//                              the sliver disappears, sub-pixel positioning
//                              is implicated.
//   5  Sub-pixel X +0.5px  -- PER-DRAW. Same baseline font/bake, deliberately
//                              drawn half a pixel further right. If the
//                              sliver gets worse or moves, that confirms #4.
//   6  Forced NEAREST      -- PER-DRAW. Same baseline bake, same glyph UVs --
//                              only the texture sampler changes, via the
//                              Vulkan backend's own
//                              DrawCallback_SetSamplerNearest draw-list
//                              callback (imgui_impl_vulkan.cpp; this is a
//                              real feature of THIS backend, not something
//                              bolted on here). If the sliver vanishes under
//                              point sampling, the bleed is bilinear
//                              filtering pulling in a neighbour texel --
//                              which points straight at TexGlyphPadding
//                              (see the note below on why padding itself
//                              could not be a live row here).
//   7  Forced LINEAR       -- PER-DRAW control row: same mechanism as #6 but
//                              set back to Linear, to confirm the callback
//                              path itself draws identically to the
//                              baseline when told to.
//
// Every row after the caption repeats the sample string three times, so a
// systematic artifact reads as a repeating pattern rather than a one-off.
//
// BAKE-TIME VS PER-DRAW (say clearly which is which, per the task brief)
// -----------------------------------------------------------------------
// Bake-time = fixed the moment a glyph is rasterized into the atlas; can
// only be varied by adding a NEW font source, never mid-frame or per-cell:
// OversampleH/V, RasterizerDensity, TexGlyphPadding.
// Per-draw = free to vary on every AddText() call against the SAME existing
// bake: pen position (sub-pixel or snapped), explicit requested size, and --
// via the backend's own sampler-swap draw callbacks -- texture filtering.
//
// TexGlyphPadding is NOT a row here, and that took checking the vendored
// ImGui (1.92.9b, subprojects/imgui/) rather than assuming: in this version
// it is a field of ImFontAtlas itself (imgui.h, struct ImFontAtlas), i.e.
// ATLAS-WIDE -- one packer setting shared by every font baked into that
// atlas -- unlike OversampleH/V and RasterizerDensity, which live on
// ImFontConfig (per font SOURCE) and so can coexist safely as separate
// fonts in one atlas, which is exactly how rows 1-3 below get baked. Varying
// padding live would mean either (a) mutating the live SettingsOverlay
// atlas's one shared padding value, which would restyle every real control
// glyph baked afterward -- exactly the "disturb the real UI" outcome the
// task brief rules out -- or (b) a second, fully separate ImGui context
// plus its own Vulkan offscreen render target and timeline semaphore (the
// FpsDisplay.cpp/Notifications.cpp shape), which is a lot of fragile
// plumbing to stand up and tear down again for throwaway lab equipment.
// Skipped for that reason. If every row below comes back clean, padding is
// the next thing to check by hand: temporarily raise the value Fonts.cpp's
// Load() leaves at its default (ImFontAtlas::TexGlyphPadding, default 1)
// and rebuild, one value at a time.

#include "FontLab.h"

#include "Fonts.h"
#include "Palette.h"

#include "imgui.h"

#include "Geist-Regular.h" // same embedded bytes Fonts.cpp bakes Style::Label from -- see src/meson.build's font_embed_gen / Overlay/fonts/embed_font.py

#include "convar.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gamescope::fontlab
{
	namespace
	{
		ConVar<bool> cv_overlay_e2_fontlab(
			"overlay_e2_fontlab", false,
			"TEMPORARY diagnostic (delete once resolved): draw a comparison grid of the same sample "
			"text under different font bake/draw settings, over everything, to find what causes the "
			"reported glyph-neighbour-bleed artifact (a grey sliver beside e.g. 'y'). Off by default. "
			"`overlay_e2_fontlab 1` to show, `overlay_e2_fontlab 0` to hide. See FontLab.cpp." );

		// Repeated three times per the task brief, so a systematic artifact
		// reads as a repeating pattern rather than a one-off. \xC2\xBB is
		// UTF-8 for U+00BB (»), which Fonts.cpp's Latin-1-Supplement default
		// glyph range already covers -- same character the real launcher
		// draws in e.g. "Vibrancy » Strength".
		constexpr const char *kSample =
			"yjgpq Vibrancy \xC2\xBB Strength il1|   "
			"yjgpq Vibrancy \xC2\xBB Strength il1|   "
			"yjgpq Vibrancy \xC2\xBB Strength il1|";

		constexpr float kVariantBakeSizePx = 16.0f; // matches TypeRole::Label's base (Tokens.cpp), pre-display_scale

		struct Variant
		{
			const char *pszCaption;
			int         nOversampleH; // 0 == leave at ImGui's own auto default
			int         nOversampleV;
			float       flRasterizerDensity; // 0 == leave at ImGui's own default (1.0)
			ImFont     *pFont = nullptr;      // resolved once by EnsureBaked()
		};

		Variant s_variants[] = {
			{ "oversample 1x1 (bake-time)",         1, 1, 0.0f },
			{ "oversample 4x4 (bake-time)",         4, 4, 0.0f },
			{ "rasterizer density 2.0 (bake-time)", 0, 0, 2.0f },
		};

		enum class RowKind { Baseline, Variant, PixelSnapX, SubpixelX, Nearest, Linear };

		struct Row
		{
			RowKind     eKind;
			int         nVariantIdx; // only meaningful when eKind == Variant
			const char *pszCaption;
		};

		const Row kRows[] = {
			{ RowKind::Baseline,   -1, "0  BASELINE -- launcher middle row (Style::Label, Sans 400, RasterSize(16px))" },
			{ RowKind::Variant,     0, "1  " },
			{ RowKind::Variant,     1, "2  " },
			{ RowKind::Variant,     2, "3  " },
			{ RowKind::PixelSnapX, -1, "4  pixel-snapped X, floor(x) (per-draw, same baseline bake)" },
			{ RowKind::SubpixelX,  -1, "5  sub-pixel X, baseline +0.5px (per-draw, same baseline bake)" },
			{ RowKind::Nearest,    -1, "6  forced NEAREST sampler (per-draw, same baseline bake/UVs)" },
			{ RowKind::Linear,     -1, "7  forced LINEAR sampler -- control row (per-draw, same baseline bake/UVs)" },
		};

		bool s_bBaked = false;

		// Logs the one objective proof the task brief asks for: that the
		// bakes actually differ. Reads back the CONFIGURED oversample/
		// density this font's source was baked with (not the requested
		// value we asked for, in case ImGui's own auto-resolution changed
		// it) and the packed atlas-texel footprint of 'y' at the shared
		// diagnostic size -- a font baked with a wider oversample factor
		// packs a physically wider glyph rect, which is a directly
		// observable, numeric difference between rows, not just an
		// assumption that the settings "should" differ.
		void LogRowBake( const char *pszLabel, ImFont *pFont, float flSizePx )
		{
			if ( pFont == nullptr )
			{
				console_log.errorf( "[fontlab] %-28s FAILED TO BAKE", pszLabel );
				return;
			}

			ImFontBaked *pBaked = pFont->GetFontBaked( flSizePx );
			ImFontGlyph *pGlyph = pBaked != nullptr ? pBaked->FindGlyph( (ImWchar)'y' ) : nullptr;
			const ImFontConfig *pCfg = pFont->Sources.Size > 0 ? pFont->Sources[ 0 ] : nullptr;

			float flTexelW = -1.0f, flTexelH = -1.0f;
			if ( pGlyph != nullptr && pFont->OwnerAtlas != nullptr && pFont->OwnerAtlas->TexData != nullptr )
			{
				flTexelW = ( pGlyph->U1 - pGlyph->U0 ) * (float)pFont->OwnerAtlas->TexData->Width;
				flTexelH = ( pGlyph->V1 - pGlyph->V0 ) * (float)pFont->OwnerAtlas->TexData->Height;
			}

			console_log.infof(
				"[fontlab] %-28s baked=%.2fpx oversampleH=%d oversampleV=%d density=%.2f "
				"'y'-rect=%.2fx%.2f-texels atlas-padding=%d",
				pszLabel,
				pBaked != nullptr ? pBaked->Size : -1.0f,
				pCfg != nullptr ? (int)pCfg->OversampleH : -1,
				pCfg != nullptr ? (int)pCfg->OversampleV : -1,
				pCfg != nullptr ? pCfg->RasterizerDensity : -1.0f,
				flTexelW, flTexelH,
				pFont->OwnerAtlas != nullptr ? pFont->OwnerAtlas->TexGlyphPadding : -1 );
		}

		// Bakes every variant font ONCE, additively, into whatever context is
		// current when the lab is first switched on (the SettingsOverlay
		// context -- Draw() is only ever called from inside its frame). This
		// is a plain AddFontFromMemoryTTF() call per variant, deliberately
		// NOT a ClearFonts()+rebuild: it only grows the live atlas with new
		// sources, so every font the real shell already baked is completely
		// untouched. See this file's top comment and Fonts.cpp's Load() for
		// why a same-frame ClearFonts() here would be the dangerous move.
		void EnsureBaked()
		{
			if ( s_bBaked )
				return;
			s_bBaked = true;

			ImGuiIO &io = ImGui::GetIO();
			const ImWchar *pRanges = io.Fonts->GetGlyphRangesDefault();

			for ( Variant &v : s_variants )
			{
				ImFontConfig cfg;
				cfg.FontDataOwnedByAtlas = false; // static embedded data -- never ask ImGui to free() it
				std::snprintf( cfg.Name, sizeof( cfg.Name ), "FontLab: %s", v.pszCaption );
				if ( v.nOversampleH > 0 )        cfg.OversampleH       = (ImS8)v.nOversampleH;
				if ( v.nOversampleV > 0 )        cfg.OversampleV       = (ImS8)v.nOversampleV;
				if ( v.flRasterizerDensity > 0.0f ) cfg.RasterizerDensity = v.flRasterizerDensity;

				v.pFont = io.Fonts->AddFontFromMemoryTTF(
					(void *)g_Font_Geist_Regular_Data, (int)g_Font_Geist_Regular_Size,
					kVariantBakeSizePx, &cfg, pRanges );
			}

			console_log.infof( "[fontlab] baked %d variant font(s) into the live atlas (additive, no rebuild)",
			                    (int)( sizeof( s_variants ) / sizeof( s_variants[ 0 ] ) ) );

			ImFont *pBaselineFont = fonts::Get( fonts::Style::Label );
			const float flBaselineSizePx = fonts::RasterSize( kVariantBakeSizePx * palette::DisplayScale() );
			LogRowBake( "0 baseline (Style::Label)", pBaselineFont, flBaselineSizePx );
			for ( const Variant &v : s_variants )
				LogRowBake( v.pszCaption, v.pFont, kVariantBakeSizePx );
		}
	}

	bool Enabled()
	{
		return cv_overlay_e2_fontlab.Get();
	}

	void Draw()
	{
		if ( !Enabled() )
			return;

		EnsureBaked();

		ImGuiIO    &io    = ImGui::GetIO();
		ImDrawList *pDraw = ImGui::GetForegroundDrawList();

		const ImU32 colBackdrop = IM_COL32( 8, 9, 11, 245 );
		const ImU32 colCellBg   = IM_COL32( 255, 255, 255, 10 );
		const ImU32 colCaption  = IM_COL32( 235, 235, 240, 255 ); // known-good style -- see below
		const ImU32 colSample   = IM_COL32( 255, 255, 255, 235 );
		const ImU32 colHeader   = IM_COL32( 255, 200, 90, 255 );

		pDraw->AddRectFilled( ImVec2( 0.0f, 0.0f ), io.DisplaySize, colBackdrop );

		// Captions are drawn in fonts::Style::Meta (Mono 400) at an
		// fonts::RasterSize()'d -- therefore already integer-bake -- size:
		// a DIFFERENT font/bake than whatever a row is testing, so the
		// labels themselves stay legible no matter what the row's own
		// sample text does. Positions are floor()'d for the same reason.
		ImFont     *pMetaFont     = fonts::Get( fonts::Style::Meta );
		const float flScale       = palette::DisplayScale();
		const float flCaptionSize = fonts::RasterSize( 13.0f * flScale );
		const float flHeaderSize  = fonts::RasterSize( 16.0f * flScale );

		ImFont     *pLabelFont      = fonts::Get( fonts::Style::Label );
		const float flBaselineSizePx = fonts::RasterSize( kVariantBakeSizePx * flScale );

		const float flMarginX = 32.0f;
		const float flRowH    = std::floor( ( io.DisplaySize.y - 90.0f ) /
		                                    (float)( sizeof( kRows ) / sizeof( kRows[ 0 ] ) ) );
		float flY = 56.0f;

		pDraw->AddText( pMetaFont, flHeaderSize, ImVec2( std::floor( flMarginX ), 16.0f ), colHeader,
		                "FONT LAB (temporary diagnostic) -- overlay_e2_fontlab 0 to close. "
		                "Every row is the baseline plus exactly one change; see console log for "
		                "resolved bake info." );

		for ( const Row &row : kRows )
		{
			const ImVec2 cellMin( flMarginX, flY );
			const ImVec2 cellMax( io.DisplaySize.x - flMarginX, flY + flRowH - 6.0f );
			pDraw->AddRectFilled( cellMin, cellMax, colCellBg, 4.0f );

			const float flCaptionX = std::floor( cellMin.x + 12.0f );
			const float flCaptionY = std::floor( cellMin.y + 8.0f );
			pDraw->AddText( pMetaFont, flCaptionSize, ImVec2( flCaptionX, flCaptionY ), colCaption,
			                row.pszCaption );
			if ( row.eKind == RowKind::Variant )
			{
				// Append the variant's own settings text right after the
				// shared numeric prefix in kRows, using the same known-good
				// caption style/measurement so nothing here is drawn with
				// whatever font the row itself is testing.
				const ImVec2 prefixSize = pMetaFont->CalcTextSizeA( flCaptionSize, FLT_MAX, 0.0f, row.pszCaption );
				pDraw->AddText( pMetaFont, flCaptionSize,
				                ImVec2( flCaptionX + prefixSize.x, flCaptionY ), colCaption,
				                s_variants[ row.nVariantIdx ].pszCaption );
			}

			// Deliberately non-integer: a realistic pen position, the way
			// ImGui layout arithmetic actually lands most of the time (see
			// row 4/5 for the two explicit stress tests either side of it).
			const float flBaselineX = cellMin.x + 12.37f;
			const float flSampleY   = std::floor( cellMin.y + 30.0f );
			const ImVec2 samplePos( flBaselineX, flSampleY );

			switch ( row.eKind )
			{
				case RowKind::Baseline:
					pDraw->AddText( pLabelFont, flBaselineSizePx, samplePos, colSample, kSample );
					break;

				case RowKind::Variant:
				{
					ImFont *pVariantFont = s_variants[ row.nVariantIdx ].pFont;
					if ( pVariantFont != nullptr )
						pDraw->AddText( pVariantFont, flBaselineSizePx, samplePos, colSample, kSample );
					break;
				}

				case RowKind::PixelSnapX:
					pDraw->AddText( pLabelFont, flBaselineSizePx,
					                ImVec2( std::floor( flBaselineX ), flSampleY ), colSample, kSample );
					break;

				case RowKind::SubpixelX:
					pDraw->AddText( pLabelFont, flBaselineSizePx,
					                ImVec2( flBaselineX + 0.5f, flSampleY ), colSample, kSample );
					break;

				case RowKind::Nearest:
				{
					// DrawCallback_SetSamplerNearest/Linear are set
					// unconditionally by imgui_impl_vulkan.cpp's Init() on
					// this backend, but guard anyway rather than assume.
					ImDrawCallback pSetNearest = ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest;
					ImDrawCallback pSetLinear  = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear;
					if ( pSetNearest != nullptr && pSetLinear != nullptr )
					{
						pDraw->AddCallback( pSetNearest );
						pDraw->AddText( pLabelFont, flBaselineSizePx, samplePos, colSample, kSample );
						pDraw->AddCallback( pSetLinear ); // restore -- nothing drawn after this in the same frame should inherit point sampling
					}
					else
					{
						pDraw->AddText( pMetaFont, flCaptionSize, samplePos, colHeader,
						                "(backend did not provide DrawCallback_SetSamplerNearest -- skipped)" );
					}
					break;
				}

				case RowKind::Linear:
				{
					ImDrawCallback pSetLinear = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear;
					if ( pSetLinear != nullptr )
						pDraw->AddCallback( pSetLinear );
					pDraw->AddText( pLabelFont, flBaselineSizePx, samplePos, colSample, kSample );
					break;
				}
			}

			flY += flRowH;
		}
	}
}
