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

			// Issue #51: deferred-rebuild request for THIS context -- see
			// RebuildAll()/ApplyPendingRebuild() below for why this exists
			// and who is expected to consume it.
			bool bRebuildPending = false;
			float flPendingScale = 0.0f;
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

		ImGuiIO &io = ImGui::GetIO();

		// Issue #48: the atlas bake below (flSizePixels * flScale per Style)
		// is the *sole* scaling mechanism for this context's fonts. Two
		// other call sites (Chrome.cpp's EnsureLiveThemeLoaded(),
		// PanelConfig.cpp's PushLiveTheme()) also assign
		// ImGuiIO::FontGlobalScale = display_scale on this same context --
		// a second, pre-#38 scaling mechanism that, left alone, stacks on
		// top of this one: ImGui folds FontGlobalScale into the resolved
		// font size on every pushed-font draw (imgui.cpp's
		// UpdateCurrentFontSize(), which reads g.Font->LegacySize --
		// itself already flSizePixels * flScale from this bake -- then
		// multiplies by FontGlobalScale/FontScaleMain before
		// GetFontBaked()), so ImGui::Text()/TextDisabled() render at
		// roughly scale^2 while explicit-size AddText() calls (the title
		// bar; Notifications.cpp/Widgets.cpp scale their own literal pixel
		// size by DisplayScale() directly and never touch FontGlobalScale)
		// stay correct at scale^1 -- confirmed by pixel measurement: the
		// two paths visibly disagreed before this fix.
		//
		// The atlas bake is kept as the one true mechanism, not
		// FontGlobalScale: #38 rebuilds the atlas at the effective scale
		// specifically so glyphs are crisp at that size rather than
		// resampled from a fixed-size bake (see this file's top-of-file
		// comment); leaving FontGlobalScale live instead would silently
		// re-introduce that resampled softness at 2.0x and effectively
		// revert #38. Rather than edit those two other call sites directly
		// -- Chrome.cpp is another worker's active file for #49;
		// PanelConfig.cpp's write exists only to give the numeric value
		// itself a live per-tick preview while a drag is in progress, ahead
		// of the debounced-to-release RebuildAll() call that actually
		// re-bakes the atlas (see DrawDisplayScaleSlider()'s own comment)
		// -- Load() (the one place that actually knows the atlas's true
		// baked-in scale) unconditionally neutralizes FontGlobalScale back
		// to a no-op every time it runs, so the atlas bake is the only
		// mechanism left in effect once a context is at rest (its steady
		// state after a Load()/RebuildAll() call -- what every measurement
		// in #48's verification, and every other reader of a font's
		// resolved size, actually observes). At flScale == 1.0 this is a
		// true no-op both ways (1.0 * 1.0), so 1.0x rendering is unchanged.
		io.FontGlobalScale = 1.0f;

		FontSet &set = g_FontSets[pContext];
		if ( set.flBuiltScale == flScale )
			return; // already built at this exact scale -- nothing to do (see RebuildAll())

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

		// Issue #51: every real call site (Chrome.cpp's EnsureLiveThemeLoaded()
		// catch-up, PanelConfig.cpp's DrawDisplayScaleSlider() on slider
		// release) runs from *inside* pPrevContext's own active ImGui frame
		// -- i.e. after that context's ImGui::NewFrame() and before its
		// Render(), mid-way through a widget-drawing pass. Rebuilding
		// pPrevContext's atlas synchronously right here -- Load()'s
		// ClearFonts() deletes every ImFont/ImFontBaked/glyph-rect this
		// context has, including the ones already used by whatever this
		// same frame already drew before reaching this call (the dock/
		// title bar, earlier General-tab rows, ...) -- invalidates glyph
		// state those already-recorded draw commands still reference. This
		// is the classic ImGui dynamic-atlas rebuild trap: it reliably
		// reproduced as corrupted/"awful" text for whatever had already
		// drawn this frame, confirmed by reading imgui_draw.cpp's own
		// packer (ImFontAtlasPackAddRect can call
		// ImFontAtlasTextureMakeSpace(), whose own comment warns "may
		// recreate a new texture and therefore change atlas->TexData" --
		// exactly the kind of mid-frame texture-identity change that would
		// strand earlier-this-frame draw commands pointing at a texture
		// object that's about to stop being the one actually bound) --
		// see this file's top-of-file comment for the write-up.
		//
		// Every *other* context in g_FontSets is safe to rebuild
		// immediately, right here: RebuildAll() is only ever reached from
		// code that runs inside pPrevContext's own frame, and nothing
		// re-enters another context's NewFrame()/Render() bracket from
		// within that call stack (SettingsOverlay.cpp, FpsDisplay.cpp and
		// Notifications.cpp each drive their own frame independently, one
		// at a time) -- so any other context sitting in this map is
		// between its own frames right now, with nothing outstanding that
		// could reference glyph state Load() is about to invalidate.
		//
		// So: defer pPrevContext's own rebuild to the start of its NEXT
		// frame (ApplyPendingRebuild(), called once per frame before any
		// widget draws -- see SettingsOverlay.cpp's call site), and apply
		// every other context's rebuild immediately, same as before.
		for ( auto &kv : g_FontSets )
		{
			if ( kv.first == pPrevContext )
				continue; // handled below, uniformly with "never built an atlas yet" below

			ImGui::SetCurrentContext( kv.first );
			Load( flScale );
		}

		// pPrevContext itself may not be in g_FontSets yet (Chrome.cpp's
		// startup catch-up can run before this context's very first Load()
		// ever populated an entry) -- g_FontSets[pPrevContext] here
		// default-constructs one (flBuiltScale == 0.0f, "never built"),
		// which ApplyPendingRebuild() below then fills in on this
		// context's next frame, same as every other path into this map.
		if ( pPrevContext != nullptr )
		{
			FontSet &set = g_FontSets[ pPrevContext ];
			set.bRebuildPending = true;
			set.flPendingScale = flScale;
		}

		ImGui::SetCurrentContext( pPrevContext );
	}

	void ApplyPendingRebuild()
	{
		ImGuiContext *pContext = ImGui::GetCurrentContext();
		if ( pContext == nullptr )
			return;

		auto it = g_FontSets.find( pContext );
		if ( it == g_FontSets.end() || !it->second.bRebuildPending )
			return; // nothing pending for this context -- the common case, every frame but the one right after a rebuild request

		// Clear the flag before calling Load(), not after: Load() itself
		// only touches g_FontSets[pContext].flBuiltScale/fonts, so this
		// ordering doesn't matter for correctness here, but it keeps the
		// "pending" flag's lifetime obviously scoped to exactly one
		// ApplyPendingRebuild() call rather than however long Load() takes.
		it->second.bRebuildPending = false;
		Load( it->second.flPendingScale );
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

	float BuiltScale()
	{
		ImGuiContext *pContext = ImGui::GetCurrentContext();
		if ( pContext == nullptr )
			return 1.0f;

		auto it = g_FontSets.find( pContext );
		if ( it == g_FontSets.end() || it->second.flBuiltScale <= 0.0f )
			return 1.0f; // never built yet (FontSet's own "never built" sentinel) -- nothing baked in to divide out

		return it->second.flBuiltScale;
	}
}
