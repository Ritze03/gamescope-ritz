// M8 part 1 (issue #13) -- see Fonts.h for the API this implements and the
// per-Style rationale. This file owns the actual atlas build: five IBM Plex
// weights (OFL-licensed, see Overlay/fonts/LICENSE-OFL.txt), embedded as C
// byte arrays at build time (Overlay/fonts/embed_font.py, wired in
// src/meson.build) rather than loaded from a filesystem path -- gamescope
// already installs its ReShade assets to a share/ prefix at runtime
// (PanelShaders.cpp's k_pszEffectPath comment), but a font that must always
// be present for basic UI legibility is worth compiling in rather than
// depending on an install-time copy step succeeding.
//
// System-font check (per the task brief): `fc-list | grep -i plex` on the
// machine this milestone was built on found no IBM Plex install, so this
// bundles the fonts rather than trying to locate a system copy -- and even
// if this *build* machine had one, a user's machine might not, so runtime
// fontconfig discovery would still need this exact bundle-and-fall-back
// path to exist anyway. Skipping runtime system-font detection (which would
// mean linking/using fontconfig here) and just always using the bundled
// data is the simpler thing that's correct either way.
//
// Issue #38: Load() is no longer idempotent-per-context-only -- it now takes
// an effective-scale parameter and can be called again later to re-bake a
// context's atlas at a new scale (RebuildAll() below does exactly that, from
// every context that has ever called it). This replaces the milestone-8
// "no runtime UI-scale/rebuild support" limitation the comment used to note
// here.
//
// Empirical correction to #38's own premise (the task brief explicitly asked
// for this to be checked at runtime, not assumed from the header): this
// ImGui version's font system is NOT the older "one fixed-size bake,
// FontGlobalScale just stretches the bitmap" model #38 was written against.
// It lazily bakes a fresh, fully crisp ImFontBaked per *exact* pixel size on
// first use of that size (ImFont::GetFontBaked()/ImFontAtlasBakedGetOrAdd(),
// imgui_draw.cpp) -- and both ImGuiIO::FontGlobalScale/style.FontScaleMain
// (imgui.cpp's UpdateCurrentFontSize(), which folds them into g.FontSize
// before g.Font->GetFontBaked(g.FontSize)) and every explicit-size draw call
// this codebase already makes (Notifications.cpp/Widgets.cpp's
// AddText(font, size, ...)/CalcTextSizeA(size, ...)) already go through that
// exact same size-keyed path. Confirmed by measurement, not just reading:
// FpsDisplay's own font_size slider already renders the HUD number crisp at
// several times its atlas AddFont() size with zero code here involved.
// So this rebuild is not fixing observed soft/blurry text -- there wasn't
// any to find. What it does do, and why it's still worth having: it keeps
// every Style's ImFont::LegacySize (the *default* size ImGui::PushFont()
// falls back to with no explicit size -- imgui.cpp's PushFont(), "font->
// LegacySize > 0.0f ? font->LegacySize : ...") matching the real effective
// scale, which is what ordinary ImGui::Text()/Checkbox()/etc. calls actually
// render at; without it those would stay pinned to the compiled-in baseline
// size regardless of display_scale. Given that's genuinely useful and #38
// asks for it explicitly (and un-widening the #24 clamp is a separate
// issue), this still implements a real, working rebuild -- just with an
// honest account of what problem it does and doesn't solve here.
//
// No manual texture create/destroy call to pair this with: this ImGui
// version's Vulkan backend (subprojects/imgui/backends/imgui_impl_vulkan.cpp)
// removed ImGui_ImplVulkan_CreateFontsTexture()/DestroyFontsTexture() in
// favour of ImGuiBackendFlags_RendererHasTextures -- each ImTextureData in
// ImDrawData::Textures carries its own Create/Update/Destroy status, and
// ImGui_ImplVulkan_RenderDrawData() walks that list and does the upload/
// teardown itself, deferring actual destruction until the backend's own
// ImageCount-frame-old check confirms nothing in flight still samples it
// (see that file's ImGui_ImplVulkan_UpdateTexture()/DestroyTexture()).
//
// But this doesn't mean "just call Clear() and re-add fonts" is safe, and
// that DID need an actual run to find, not just the header: ImFontAtlas::
// Clear() also calls the now-"[OBSOLETE]" ClearTexData(), which frees every
// existing ImTextureData's CPU pixel buffer outright -- fine for a context's
// very first build (nothing exists yet), but on a real rebuild of an
// already-rendered atlas it frees Pixels out from under a texture object
// that's still sitting in atlas->TexList with Status still ImTextureStatus_OK
// (nothing has told the backend to actually retire it), so the very next
// glyph the dynamic packer tries to rasterize into that texture calls
// ImTextureData::GetPixelsAt() on a NULL buffer and asserts/crashes -- this
// reliably reproduced the first time a rebuild was actually exercised
// against a warm context. See Load()'s own comment for the fix (ClearFonts()
// instead of Clear()) and why it's the actually-supported live-rebuild path.

#include "Fonts.h"

#include "imgui.h"

#include "IBMPlexSans-Regular.h"
#include "IBMPlexSans-Medium.h"
#include "IBMPlexMono-Regular.h"
#include "IBMPlexMono-Medium.h"
#include "IBMPlexMono-SemiBold.h"

#include <cstring>
#include <unordered_map>

namespace gamescope::fonts
{
	namespace
	{
		constexpr int kStyleCount = 10; // keep in sync with Style's enumerator count

		int IndexForStyle( Style style )
		{
			return (int)style;
		}

		struct FontFace
		{
			const unsigned char *pData;
			unsigned int uSize;
		};

		// One TTF per weight actually used (see Fonts.h's per-Style comment
		// for which role uses which). Several weights are baked more than
		// once at different pixel sizes (e.g. Mono SemiBold for both Title
		// and Hero, Mono Medium for Value/SegmentLabel) -- ImGui bakes a
		// separate ImFont per AddFont call regardless of shared source data,
		// so this is "5 files, 10 baked fonts": still just the spec's own
		// §2 "bake only the combinations actually used" set, not a general
		// arbitrary-size atlas.
		const FontFace kSansRegular   = { g_Font_IBMPlexSans_Regular_Data,   g_Font_IBMPlexSans_Regular_Size };
		const FontFace kSansMedium    = { g_Font_IBMPlexSans_Medium_Data,    g_Font_IBMPlexSans_Medium_Size };
		const FontFace kMonoRegular   = { g_Font_IBMPlexMono_Regular_Data,   g_Font_IBMPlexMono_Regular_Size };
		const FontFace kMonoMedium    = { g_Font_IBMPlexMono_Medium_Data,    g_Font_IBMPlexMono_Medium_Size };
		const FontFace kMonoSemiBold  = { g_Font_IBMPlexMono_SemiBold_Data,  g_Font_IBMPlexMono_SemiBold_Size };

		struct StyleSpec
		{
			Style style;
			const FontFace *pFace;
			float flSizePixels;
			const char *pszDebugName;
		};

		// superdoc/planning/ui-mockup-precise-spec.md §2's Typography table,
		// one row each -- see Fonts.h's Style comment for the same mapping.
		// Issue #23: every role except DockHotkey (dock-owned, explicitly
		// excluded -- "except the dock") raised ~20-25% above the spec's own
		// measured values, at the user's explicit request to depart from the
		// mockup baseline -- see superdoc/planning/ui-mockup-precise-spec.md's
		// §2 Typography table for a note pointing back here so a future pass
		// doesn't "fix" these back down to match the mockup pixels.
		const StyleSpec kSpecs[kStyleCount] = {
			{ Style::Title,         &kMonoSemiBold, 13.5f, "Plex Mono SemiBold 13.5px (Title)" },
			{ Style::Section,       &kSansMedium,   16.0f, "Plex Sans Medium 16px (Group name)" },
			{ Style::Label,         &kSansRegular,  14.0f, "Plex Sans Regular 14px (Param label)" },
			{ Style::Value,         &kMonoMedium,   16.0f, "Plex Mono Medium 16px (Value)" },
			{ Style::Meta,          &kMonoRegular,  13.0f, "Plex Mono Regular 13px (Meta)" },
			{ Style::Hero,          &kMonoSemiBold, 22.0f, "Plex Mono SemiBold 22px (Hero)" },
			{ Style::SegmentLabel,  &kMonoMedium,   14.0f, "Plex Mono Medium 14px (Segment inactive)" },
			{ Style::SegmentActive, &kMonoSemiBold, 14.0f, "Plex Mono SemiBold 14px (Segment active)" },
			{ Style::ScaleMark,     &kMonoRegular,  11.5f, "Plex Mono Regular 11.5px (Scale mark)" },
			{ Style::DockHotkey,    &kMonoMedium,    8.0f, "Plex Mono Medium 8px (Dock hotkey)" }, // dock-owned, unchanged
		};

		struct FontSet
		{
			ImFont *fonts[kStyleCount] = {};
			// 0.0f is the sentinel for "never built" -- flScale is always a
			// real positive display_scale value (never 0) once Load() has
			// actually run for this context, so this can never collide with
			// a legitimate already-built scale.
			float flBuiltScale = 0.0f;
		};

		// Keyed by ImGuiContext*, not process-global: SettingsOverlay.cpp,
		// FpsDisplay.cpp and Notifications.cpp each own a separate ImGui
		// context (see FpsDisplay.h's file comment), and ImFont* pointers
		// are only valid against the ImFontAtlas of the context they were
		// baked into -- mixing them across contexts would sample the wrong
		// atlas texture. There are only ever up to three live contexts in
		// this process, so a small map is simple and cheap; this is not
		// meant to scale past that. Also doubles as RebuildAll()'s "every
		// context that has ever built an atlas" set -- see that function.
		std::unordered_map<ImGuiContext *, FontSet> g_FontSets;
	}

	void Load( float flScale )
	{
		ImGuiContext *pContext = ImGui::GetCurrentContext();
		if ( pContext == nullptr )
			return; // nothing to build into -- caller's EnsureImguiInit() didn't create a context

		FontSet &set = g_FontSets[pContext];
		if ( set.flBuiltScale == flScale )
			return; // already built at this exact scale -- nothing to do (see RebuildAll())

		ImGuiIO &io = ImGui::GetIO();

		// Issue #38: safe to call this more than once per context now --
		// ClearFonts() releases the atlas's previously-built font/glyph
		// bookkeeping before every fresh AddFontFromMemoryTTF() pass below,
		// including the very first call for a context (ClearFonts() on an
		// already-empty atlas is a harmless no-op, so this doesn't need an
		// "is this the first build" branch).
		//
		// Deliberately ClearFonts(), NOT the full Clear(): this needed an
		// actual runtime check, not just reading the header (per the task
		// brief) -- Clear() additionally calls the now-"[OBSOLETE]"
		// ClearTexData(), which frees every existing ImTextureData's CPU
		// pixel buffer immediately (DestroyPixels()) without changing its
		// Status away from ImTextureStatus_OK. On a context that has
		// already rendered at least one frame (i.e. every real rebuild --
		// the very first Load() for a context never hits this), that
		// leaves atlas->TexData pointing at a texture whose Pixels is NULL
		// but whose Status still claims it's live and packable; the very
		// next glyph the dynamic atlas tries to rasterize into it calls
		// ImTextureData::GetPixelsAt(), which asserts Pixels != NULL and
		// crashes (confirmed empirically -- calling Clear() here reliably
		// reproduced exactly that crash the first time this was actually
		// exercised against a warm context, e.g. via the General tab's
		// Display-scale slider). ClearFonts() alone -- imgui.h's own
		// comment: "Clear input+output font data/glyphs. New fonts and
		// textures will be recreated afterwards." -- never touches
		// TexList/Pixels at all: the freed Style slots just become
		// reclaimable space the dynamic packer reuses (or grows into a
		// new ImTextureData for) as AddFontFromMemoryTTF()'s new fonts get
		// their glyphs baked on demand, with every existing ImTextureData
		// staying exactly as valid as it already was. This is the
		// supported live-rebuild path for this backend generation, not
		// Clear()'s.
		io.Fonts->ClearFonts();

		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false; // the byte arrays are static const, compiled-in data -- never ask ImGui to free() them
		const ImWchar *pGlyphRanges = io.Fonts->GetGlyphRangesDefault(); // Basic Latin + Latin-1 Supplement -- this UI is English-only, no reason to bake the rest of Unicode

		bool bOk = true;
		ImFont *builtFonts[kStyleCount] = {};

		for ( int i = 0; i < kStyleCount && bOk; i++ )
		{
			const StyleSpec &spec = kSpecs[i];
			ImFontConfig fontCfg = cfg;
#if defined( _MSC_VER )
			strncpy_s( fontCfg.Name, spec.pszDebugName, sizeof( fontCfg.Name ) - 1 );
#else
			std::strncpy( fontCfg.Name, spec.pszDebugName, sizeof( fontCfg.Name ) - 1 );
			fontCfg.Name[ sizeof( fontCfg.Name ) - 1 ] = '\0';
#endif

			ImFont *pFont = io.Fonts->AddFontFromMemoryTTF(
				(void *)spec.pFace->pData, (int)spec.pFace->uSize,
				spec.flSizePixels * flScale, &fontCfg, pGlyphRanges );

			if ( pFont == nullptr )
				bOk = false;
			else
				builtFonts[ IndexForStyle( spec.style ) ] = pFont;
		}

		if ( !bOk )
		{
			// Fallback (required by the task brief, not optional): wipe
			// whatever partial atlas state the failed attempt above left
			// behind and fall back cleanly to ImGui's own built-in default
			// font for every role, rather than rendering with a half-built
			// atlas or leaving any Style unresolved/null. ClearFonts(), not
			// Clear() -- same live-atlas reasoning as above; this fallback
			// path is just as reachable on a rebuild (a bad flScale is not
			// exclusive to first boot) as the main path is.
			io.Fonts->ClearFonts();
			ImFont *pDefault = io.Fonts->AddFontDefault();
			io.FontDefault = pDefault;
			for ( int i = 0; i < kStyleCount; i++ )
				set.fonts[i] = pDefault;
			set.flBuiltScale = flScale;
			return;
		}

		for ( int i = 0; i < kStyleCount; i++ )
			set.fonts[i] = builtFonts[i];
		set.flBuiltScale = flScale;

		// Style::Label (Plex Sans Regular, body text) becomes the atlas's
		// default -- every pre-existing ImGui::Text/Checkbox/SliderFloat/
		// etc. call that never explicitly pushes a Style picks this up for
		// free, matching the design guide's "IBM Plex Sans for prose".
		io.FontDefault = set.fonts[ IndexForStyle( Style::Label ) ];
	}

	void RebuildAll( float flScale )
	{
		ImGuiContext *pPrevContext = ImGui::GetCurrentContext();

		// g_FontSets already holds exactly "every context whose
		// EnsureImguiInit() has run at least once" -- Load() populates an
		// entry (via operator[]) the first time it runs for a context, and
		// nothing ever erases one. A context that has never called Load()
		// (an FPS HUD the user has never enabled, or a Notifications
		// context that has never shown a toast) simply isn't in this map
		// yet, and correctly picks up flScale on its own the first time it
		// does call Load() -- see Fonts.h's comment.
		for ( auto &kv : g_FontSets )
		{
			ImGui::SetCurrentContext( kv.first );
			Load( flScale );
		}

		ImGui::SetCurrentContext( pPrevContext );
	}

	ImFont *Get( Style style )
	{
		ImGuiContext *pContext = ImGui::GetCurrentContext();
		if ( pContext != nullptr )
		{
			auto it = g_FontSets.find( pContext );
			if ( it != g_FontSets.end() )
			{
				ImFont *pFont = it->second.fonts[ IndexForStyle( style ) ];
				if ( pFont != nullptr )
					return pFont;
			}
		}

		// Defensive only (see Fonts.h): every real call site's context has
		// Load() called on it before any drawing happens, so this path is
		// not expected to be hit in practice.
		return ImGui::GetIO().FontDefault;
	}
}
