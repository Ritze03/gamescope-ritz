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
// ponytail: no runtime UI-scale/rebuild support -- SPEC.md's UI structure
// doesn't call for a user-facing font-size setting, and rebuilding a live
// atlas means re-uploading the Vulkan font texture (a real cost the task
// brief explicitly warns against taking on speculatively). If that's ever
// needed, Load() would need to become idempotent-per-size instead of
// idempotent-per-context.

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
		constexpr int kStyleCount = 6; // keep in sync with Style's enumerator count

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
		// for which role uses which). Plex Mono SemiBold is baked twice, at
		// two different pixel sizes (Title and Hero) -- ImGui bakes a
		// separate ImFont per AddFont call regardless of shared source data,
		// so this is still just "5 files, 6 baked fonts", matching the
		// design guide's own "~4-6 distinct family/weight/size combos"
		// budget in its ImGui feasibility table.
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

		// The design guide's "Scale observed" bullets (ui-design-guide.md),
		// one row each -- see Fonts.h's Style comment for the same mapping.
		const StyleSpec kSpecs[kStyleCount] = {
			{ Style::Title,   &kMonoSemiBold, 11.0f, "Plex Mono SemiBold 11px (Title)" },
			{ Style::Section, &kSansMedium,   12.5f, "Plex Sans Medium 12.5px (Section)" },
			{ Style::Label,   &kSansRegular,  12.0f, "Plex Sans Regular 12px (Label)" },
			{ Style::Value,   &kMonoMedium,   12.5f, "Plex Mono Medium 12.5px (Value)" },
			{ Style::Meta,    &kMonoRegular,  10.0f, "Plex Mono Regular 10px (Meta)" },
			{ Style::Hero,    &kMonoSemiBold, 18.0f, "Plex Mono SemiBold 18px (Hero)" },
		};

		struct FontSet
		{
			ImFont *fonts[kStyleCount] = {};
		};

		// Keyed by ImGuiContext*, not process-global: SettingsOverlay.cpp
		// and FpsDisplay.cpp each own a separate ImGui context (see
		// FpsDisplay.h's file comment), and ImFont* pointers are only valid
		// against the ImFontAtlas of the context they were baked into --
		// mixing them across contexts would sample the wrong atlas texture.
		// There are only ever one or two live contexts in this process, so
		// a small map is simple and cheap; this is not meant to scale past
		// that.
		std::unordered_map<ImGuiContext *, FontSet> g_FontSets;
	}

	void Load()
	{
		ImGuiContext *pContext = ImGui::GetCurrentContext();
		if ( pContext == nullptr )
			return; // nothing to build into -- caller's EnsureImguiInit() didn't create a context

		ImGuiIO &io = ImGui::GetIO();
		FontSet &set = g_FontSets[pContext];

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
				spec.flSizePixels, &fontCfg, pGlyphRanges );

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
			// atlas or leaving any Style unresolved/null.
			io.Fonts->Clear();
			ImFont *pDefault = io.Fonts->AddFontDefault();
			io.FontDefault = pDefault;
			for ( int i = 0; i < kStyleCount; i++ )
				set.fonts[i] = pDefault;
			return;
		}

		for ( int i = 0; i < kStyleCount; i++ )
			set.fonts[i] = builtFonts[i];

		// Style::Label (Plex Sans Regular, body text) becomes the atlas's
		// default -- every pre-existing ImGui::Text/Checkbox/SliderFloat/
		// etc. call that never explicitly pushes a Style picks this up for
		// free, matching the design guide's "IBM Plex Sans for prose".
		io.FontDefault = set.fonts[ IndexForStyle( Style::Label ) ];
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
