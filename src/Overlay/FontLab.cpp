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
// ROWS 8-11 -- added to chase the ONE remaining untested difference: every
// row above draws directly, via ImGui::GetForegroundDrawList()->AddText().
// The real shell never does that -- every string in the UI goes through
// Controls.cpp's ui::DrawText()/MeasureText(). Nobody had compared the two
// paths side by side until now. Reading DrawText() top to bottom
// (Controls.cpp) turned up THREE differences from this file's own baseline,
// not one, so there are three rows plus the requested fractional-coordinate
// row:
//
//   8  Window draw list    -- PER-DRAW. DrawText() draws onto
//                              `ImGui::GetCurrentWindow()->DrawList`, a
//                              WINDOW draw list -- never
//                              GetForegroundDrawList(), which is what every
//                              row above (and the real launcher's clean
//                              middle row) uses. This is a draw-list CHOICE
//                              difference the task brief specifically asked
//                              to be isolated if found. Same baseline
//                              font/bake/position/text as row 0, the ONLY
//                              change is which draw list receives the
//                              AddText() call. Needed standing up a
//                              throwaway, fully-transparent, input-less
//                              ImGui window for this whole block to draw
//                              into in the first place -- see Draw()'s
//                              "rows 8-11's shared setup" comment for why
//                              that turned out to be load-bearing, not
//                              optional plumbing.
//   9  ui::DrawText()       -- PER-DRAW, CRITICAL ROW. The shell's real
//      (realistic rect)        funnel, called exactly as a real label-only
//                              row calls it: same TypeRole::Label face/size
//                              resolution, same window draw list, and a
//                              REAL label rect from
//                              Lane::ForColumn/RowCtx::ForRow/
//                              SplitLabelZone(0, ...) (Row.cpp/Lane.cpp) --
//                              not an invented rect. That rect is realistic
//                              UI width, not sized for this row's
//                              deliberately long repeated test string, so
//                              DrawText's OWN ellipsis truncation (a FOURTH
//                              bypassed step: the lab's direct AddText()
//                              never truncates anything) will very likely
//                              cut it short. That is correct, expected
//                              behaviour, not a bug in this row -- row 10
//                              removes it as a variable.
//   10 ui::DrawText()       -- PER-DRAW control for row 9: the identical
//      (widened rect)          call, same rect, widened hugely on X so no
//                              glyph is anywhere near the clip edge and the
//                              full sample string survives intact. If 9 and
//                              10 render identically apart from truncation
//                              length, the clip argument itself is
//                              exonerated and whatever 9 shows is really
//                              coming from DrawText's other machinery
//                              (draw list, alignment/centring math, or
//                              per-glyph AddText with a non-null
//                              cpu_fine_clip_rect -- already checked
//                              generically by a previous worker, but never
//                              on THIS exact call shape before now).
//   11 Direct AddText,      -- PER-DRAW, the task brief's requested row:
//      shell's real             same baseline font/bake as row 0, drawn via
//      coordinate               the FOREGROUND draw list exactly like row
//                              0, but positioned at the real fractional
//                              pixel coordinate the SAME Lane/RowCtx/
//                              SplitLabelZone call above resolves for a
//                              label's DRAWN position (DrawText's own
//                              vertical-centring formula, reproduced here
//                              rather than re-derived, so this row's
//                              position and row 9's are provably the same
//                              number). Evaluated at a deliberately
//                              non-default display_scale (1.15, inside
//                              ConfigSchema.h's 0.5..2.0 clamp) rather than
//                              whatever ui::Scale() happens to be live right
//                              now -- at exactly 1.0 every token
//                              multiplication below stays a whole number and
//                              this row would land on an integer by
//                              accident, defeating its own point. The actual
//                              resolved (x, y) is printed in the row's own
//                              caption so it is provably not invented.
//
// Rows 9-11 all run inside one throwaway ImGui::Begin()/End() bracket this
// file did not need before -- ImGui::GetCurrentWindow() (what
// ui::DrawText()/ui::MeasureText() call internally) derefs a null
// g.CurrentWindow if nothing is currently open, and FontLab::Draw() is
// called from SettingsOverlay.cpp with no window open (after the shell's
// own Begin()/End() pairs have already closed for the frame -- see
// FontLab.h's comment on when Draw() runs). That requirement is itself
// evidence for the "draw-list choice" hypothesis: the real shell always has
// a window open when it calls DrawText() (it is called from inside
// Shell.cpp's own row-drawing code), so this is a faithful reconstruction
// of that precondition, not a workaround for a lab-only problem.
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
#include "imgui_internal.h" // ImRect -- same header UI/Row.h itself pulls this from

#include "Geist-Regular.h" // same embedded bytes Fonts.cpp bakes Style::Label from -- see src/meson.build's font_embed_gen / Overlay/fonts/embed_font.py

// The shell's real text funnel this file is comparing itself against
// (ui::DrawText()/ui::MeasureText()), and the row-geometry primitives
// (ui::Lane, ui::RowCtx) rows 9-11 use to resolve a REAL label rect/
// coordinate rather than an invented one. `ui::` below resolves via
// ordinary enclosing-namespace lookup -- this file lives in
// gamescope::fontlab, Controls.h's contents live in gamescope::ui, same
// pattern PanelConfig.cpp already uses.
#include "UI/Controls.h"

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

		enum class RowKind
		{
			Baseline, Variant, PixelSnapX, SubpixelX, Nearest, Linear,
			// Rows 8-11 -- see this file's top comment for what each isolates.
			WindowDrawListDirect, ThroughDrawTextRealistic, ThroughDrawTextWidened, DirectAtShellCoord
		};

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
			{ RowKind::WindowDrawListDirect,   -1, "8  window draw list, direct AddText (isolates draw-list choice, same baseline bake/pos)" },
			{ RowKind::ThroughDrawTextRealistic, -1, "9  ui::DrawText(), REAL label rect -- CRITICAL, expect truncation, see file header" },
			{ RowKind::ThroughDrawTextWidened, -1, "10 ui::DrawText(), same rect widened -- rules out clipping/truncation as the cause" },
			{ RowKind::DirectAtShellCoord,     -1, "11 " }, // suffix (the resolved coordinate) appended at draw time -- see Draw()
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

		// ---- rows 8-11's shared setup ---------------------------------
		// ui::DrawText()/ui::MeasureText() (Controls.cpp) draw onto
		// ImGui::GetCurrentWindow()->DrawList, which derefs a null
		// g.CurrentWindow if nothing is open -- and nothing is open here
		// (FontLab::Draw() runs after the shell's own windows have already
		// closed for the frame; see this file's top comment). Stand up one
		// throwaway, fully-transparent, input-less window spanning the
		// whole display so rows 8-11 have the same "a window is open"
		// precondition the real shell always has when it calls DrawText()
		// from inside its own row-drawing code.
		ImGui::SetNextWindowPos( ImVec2( 0.0f, 0.0f ) );
		ImGui::SetNextWindowSize( io.DisplaySize );
		ImGui::SetNextWindowBgAlpha( 0.0f );
		const bool  bWindowOpen = ImGui::Begin( "##fontlab_window_ctx", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNav );
		ImDrawList *pWindowDraw = bWindowOpen ? ImGui::GetWindowDrawList() : nullptr; // == ImGui::GetCurrentWindow()->DrawList -- exactly what DrawText() uses, NOT GetForegroundDrawList()

		// Rows 9-11's shared geometry source: the shell's own
		// Lane::ForColumn (Lane.cpp), evaluated at a deliberately
		// non-default display_scale rather than whatever ui::Scale()
		// happens to be live right now -- at exactly 1.0 every token
		// multiplication below stays a whole number and row 11 would land
		// on an integer by accident, defeating its own point. 1.15 sits
		// inside ConfigSchema.h's 0.5..2.0 clamp, so it is a value a real
		// user's display_scale setting can genuinely be. Restored right
		// after the loop below -- see the comment there for why that is
		// safe.
		const float    flSavedUiScale = ui::Scale();
		ui::SetScale( 1.15f );
		const ui::Lane simLane = ui::Lane::ForColumn( 480.0f / ui::Scale() ); // 480px, a representative single sheet column

		for ( const Row &row : kRows )
		{
			const ImVec2 cellMin( flMarginX, flY );
			const ImVec2 cellMax( io.DisplaySize.x - flMarginX, flY + flRowH - 6.0f );
			pDraw->AddRectFilled( cellMin, cellMax, colCellBg, 4.0f );

			// The REAL label rect a shell row starting at THIS row's own
			// on-screen position would resolve to, via
			// RowCtx::ForRow()/SplitLabelZone(0, ...) (Row.cpp) -- 0 passed
			// per Row.h's own documented use for "a kind with no value
			// column". Anchored to cellMin.y (not a separately-invented Y)
			// purely so rows 9-11's drawn text stays inside their own cell
			// on screen; the rect itself is the same call a real row makes.
			const ui::RowCtx simRow = ui::RowCtx::ForRow( simLane, flMarginX, cellMin.y );
			ImRect rcRealLabel, rcRealValueUnused;
			simRow.SplitLabelZone( 0.0f, &rcRealLabel, &rcRealValueUnused );
			ImRect rcRealLabelWide = rcRealLabel;
			rcRealLabelWide.Max.x  = rcRealLabel.Min.x + 2000.0f; // comfortably larger than the sample string -- no glyph anywhere near the clip edge

			// DrawText()'s own vertical-centring formula (Controls.cpp),
			// reproduced here rather than re-derived, so row 11's position
			// is PROVABLY the same number row 9 draws its rect's text at.
			const ImVec2 realTextSize = ui::MeasureText( ui::TypeRole::Label, kSample );
			const ImVec2 realDirectPos( rcRealLabel.Min.x,
			                            rcRealLabel.Min.y + ( rcRealLabel.GetHeight() - realTextSize.y ) * 0.5f );

			char szCoordCaption[ 96 ];
			std::snprintf( szCoordCaption, sizeof( szCoordCaption ),
			               "(%.3f, %.3f) via Lane/RowCtx @ scale 1.15", realDirectPos.x, realDirectPos.y );

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
			else if ( row.eKind == RowKind::DirectAtShellCoord )
			{
				// Same append mechanism as the Variant branch above, just a
				// different source string: the coordinate this row is
				// actually about to draw at, computed above, not invented.
				const ImVec2 prefixSize = pMetaFont->CalcTextSizeA( flCaptionSize, FLT_MAX, 0.0f, row.pszCaption );
				pDraw->AddText( pMetaFont, flCaptionSize,
				                ImVec2( flCaptionX + prefixSize.x, flCaptionY ), colCaption,
				                szCoordCaption );
			}

			// Deliberately non-integer: a realistic pen position, the way
			// ImGui layout arithmetic actually lands most of the time (see
			// row 4/5 for the two explicit stress tests either side of it).
			const float flBaselineX = cellMin.x + 12.37f;
			const float flSampleY   = std::floor( cellMin.y + 30.0f );
			const ImVec2 samplePos( flBaselineX, flSampleY );

			// Rows 8-10 draw onto the lab's own throwaway window (see
			// above); degrade to a plain caption rather than dereference a
			// null draw list if it somehow did not open this frame. Row 11
			// draws onto the foreground list like every other row here --
			// it only borrows this window's Lane/RowCtx MATH, not its draw
			// list -- so it does not need this guard.
			const bool bRowNeedsWindow =
				row.eKind == RowKind::WindowDrawListDirect       ||
				row.eKind == RowKind::ThroughDrawTextRealistic   ||
				row.eKind == RowKind::ThroughDrawTextWidened;
			if ( bRowNeedsWindow && pWindowDraw == nullptr )
			{
				pDraw->AddText( pMetaFont, flCaptionSize, samplePos, colHeader,
				                "(lab's own ImGui window did not open this frame -- skipped)" );
				flY += flRowH;
				continue;
			}

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

				case RowKind::WindowDrawListDirect:
					// Same baseline font/bake/position/text as row 0 -- the
					// ONLY change is which draw list receives the AddText()
					// call. Isolates the draw-list choice itself.
					pWindowDraw->AddText( pLabelFont, flBaselineSizePx, samplePos, colSample, kSample );
					break;

				case RowKind::ThroughDrawTextRealistic:
					// The shell's real funnel, verbatim: same TypeRole,
					// same window draw list, a REAL label rect. This
					// string is realistically too long for that rect, so
					// DrawText's own ellipsis logic (Controls.cpp) will
					// likely truncate it -- expected, not a bug in this row.
					ui::DrawText( rcRealLabel, ui::TypeRole::Label, colSample, kSample );
					break;

				case RowKind::ThroughDrawTextWidened:
					// Identical call, same rect widened so nothing
					// truncates -- rules out clipping/truncation as the
					// difference between this row and row 9.
					ui::DrawText( rcRealLabelWide, ui::TypeRole::Label, colSample, kSample );
					break;

				case RowKind::DirectAtShellCoord:
					// Row 0's exact draw call, only the position changes:
					// the real fractional coordinate computed above, via
					// the same Lane/RowCtx call rows 9-10 draw their rect
					// at (see szCoordCaption for the printed number).
					pDraw->AddText( pLabelFont, flBaselineSizePx, realDirectPos, colSample, kSample );
					break;
			}

			flY += flRowH;
		}

		ImGui::End(); // matches ImGui::Begin() above -- must run whether or not it returned true

		// Restore -- this override must never leak into any OTHER frame's
		// real rendering. Safe even without this: the shell overwrites
		// ui::Scale() from its own live display_scale at the very top of
		// every Draw() it runs (Shell.cpp:5965), and FontLab::Draw() always
		// runs AFTER the shell's for this same frame (SettingsOverlay.cpp),
		// so nothing downstream of here this frame reads the overridden
		// value either. Restored anyway rather than relying on that.
		ui::SetScale( flSavedUiScale );
	}
}
