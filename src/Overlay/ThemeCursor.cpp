#include "ThemeCursor.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <X11/Xcursor/Xcursor.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace gamescope::overlay
{
	namespace
	{
		// ---- the theme image itself, loaded once per process ------------
		//
		// XcursorLibraryLoadImage() resolves a cursor NAME through libXcursor
		// against XCURSOR_THEME/XCURSOR_SIZE and returns raw pixels. It reads
		// the theme off disk and needs no X display at all, which is what lets
		// the overlay -- which has no X connection of its own -- show the same
		// image steamcompmgr.cpp's SetDefaultCursorImage() puts on the game's
		// root window via the display-bound XcursorShapeLoadCursor().
		struct ThemeImage
		{
			bool bLoadAttempted = false;
			bool bValid = false;
			int nWidth = 0;
			int nHeight = 0;
			int nHotX = 0;
			int nHotY = 0;
			// Straight (non-premultiplied) alpha, in ImGui's RGBA32 byte
			// order, ready to memcpy into the atlas.
			std::vector<ImU32> vecPixels;
		};

		ThemeImage &GetThemeImage()
		{
			static ThemeImage s_Image;
			if ( s_Image.bLoadAttempted )
				return s_Image;

			s_Image.bLoadAttempted = true;

			int nSize = 24;
			if ( const char *pszSize = getenv( "XCURSOR_SIZE" ) )
			{
				const int nParsed = atoi( pszSize );
				if ( nParsed > 0 && nParsed <= 512 )
					nSize = nParsed;
			}

			// A null theme is fine and is the documented "use the default"
			// argument: libXcursor then falls back to the "default" theme and,
			// failing that, to its own built-in arrow. It returns null only
			// when it can find no image at all, which is the one case we have
			// to hand back to ImGui.
			XcursorImage *pImage = XcursorLibraryLoadImage(
				"left_ptr", getenv( "XCURSOR_THEME" ), nSize );
			if ( !pImage )
				return s_Image;

			if ( pImage->width > 0 && pImage->height > 0 &&
			     pImage->width <= 512 && pImage->height <= 512 )
			{
				s_Image.nWidth = (int)pImage->width;
				s_Image.nHeight = (int)pImage->height;
				s_Image.nHotX = (int)pImage->xhot;
				s_Image.nHotY = (int)pImage->yhot;
				s_Image.vecPixels.resize( (size_t)s_Image.nWidth * s_Image.nHeight );

				// Xcursor hands back ARGB32 with PREMULTIPLIED alpha; ImGui
				// blends with the ordinary (straight-alpha) src/1-src rule and
				// would darken every partly-transparent edge pixel if handed
				// premultiplied data. Undo the multiply here rather than
				// changing the overlay's blend state, which is shared with
				// every glyph and panel it draws.
				for ( size_t i = 0; i < s_Image.vecPixels.size(); i++ )
				{
					const XcursorPixel uPixel = pImage->pixels[ i ];
					const unsigned uA = ( uPixel >> 24 ) & 0xffu;
					unsigned uR = ( uPixel >> 16 ) & 0xffu;
					unsigned uG = ( uPixel >> 8 ) & 0xffu;
					unsigned uB = ( uPixel ) & 0xffu;

					if ( uA == 0 )
					{
						uR = uG = uB = 0;
					}
					else if ( uA < 255 )
					{
						uR = ( uR * 255u + uA / 2u ) / uA;
						uG = ( uG * 255u + uA / 2u ) / uA;
						uB = ( uB * 255u + uA / 2u ) / uA;
						if ( uR > 255u ) uR = 255u;
						if ( uG > 255u ) uG = 255u;
						if ( uB > 255u ) uB = 255u;
					}

					s_Image.vecPixels[ i ] = IM_COL32( uR, uG, uB, uA );
				}

				s_Image.bValid = true;
			}

			XcursorImageDestroy( pImage );
			return s_Image;
		}

		// ---- per-ImGui-context atlas residency --------------------------
		//
		// The image lives as a custom rectangle inside each context's own font
		// atlas, so it rides the texture the overlay already uploads instead
		// of introducing a second one. Custom rects do not survive a font
		// rebuild (Fonts.cpp's Load() calls ClearFonts(), which destroys the
		// packer) and their coordinates are invalidated whenever the atlas
		// texture is repacked or resized, so every field the rect depends on
		// is recorded and re-checked each frame rather than cached once.
		struct AtlasSlot
		{
			ImFontAtlas *pAtlas = nullptr;
			ImTextureData *pTexData = nullptr;
			int nTexWidth = 0;
			int nTexHeight = 0;
			ImFontAtlasRectId nRectId = ImFontAtlasRectId_Invalid;
			ImFontAtlasRect rect{};
			bool bReady = false;
		};

		std::unordered_map<ImGuiContext *, AtlasSlot> &GetSlots()
		{
			static std::unordered_map<ImGuiContext *, AtlasSlot> s_Slots;
			return s_Slots;
		}

		void BlitIntoAtlas( ImFontAtlas *pAtlas, ImTextureData *pTexData,
		                    const ImFontAtlasRect &rect, const ThemeImage &image )
		{
			for ( int y = 0; y < image.nHeight; y++ )
			{
				void *pDst = pTexData->GetPixelsAt( rect.x, rect.y + y );
				memcpy( pDst, &image.vecPixels[ (size_t)y * image.nWidth ],
				        (size_t)image.nWidth * sizeof( ImU32 ) );
			}

			ImFontAtlasTextureBlockQueueUpload(
				pAtlas, pTexData, rect.x, rect.y, rect.w, rect.h );
		}
	}

	bool ThemeCursor_Prepare()
	{
		const ThemeImage &image = GetThemeImage();
		if ( !image.bValid )
			return false;

		ImGuiContext *pContext = ImGui::GetCurrentContext();
		if ( !pContext )
			return false;

		ImFontAtlas *pAtlas = ImGui::GetIO().Fonts;
		if ( !pAtlas )
			return false;

		ImTextureData *pTexData = pAtlas->TexData;

		// Everything below assumes a 4-byte colour atlas we can memcpy into.
		// An Alpha8 atlas is a legal ImGui configuration this build does not
		// currently produce; rather than silently writing garbage into it,
		// hand the frame back to ImGui's arrow.
		if ( !pTexData || pTexData->Pixels == nullptr ||
		     pTexData->Format != ImTextureFormat_RGBA32 ||
		     pTexData->BytesPerPixel != (int)sizeof( ImU32 ) )
			return false;

		if ( pTexData->Width < image.nWidth || pTexData->Height < image.nHeight )
			return false;

		AtlasSlot &slot = GetSlots()[ pContext ];

		const bool bAtlasChanged =
			slot.pAtlas != pAtlas ||
			slot.pTexData != pTexData ||
			slot.nTexWidth != pTexData->Width ||
			slot.nTexHeight != pTexData->Height ||
			slot.nRectId == ImFontAtlasRectId_Invalid;

		if ( bAtlasChanged )
		{
			// Colour pixels, not just coverage -- tells the backend this
			// atlas genuinely needs its RGB channels.
			pAtlas->TexPixelsUseColors = true;

			ImFontAtlasRect rect;
			const ImFontAtlasRectId nRectId =
				pAtlas->AddCustomRect( image.nWidth, image.nHeight, &rect );
			if ( nRectId == ImFontAtlasRectId_Invalid )
			{
				slot.bReady = false;
				return false;
			}

			// AddCustomRect() can itself grow the texture, which moves
			// TexData's pixel buffer -- so re-read it rather than blitting
			// into the pointer captured above.
			pTexData = pAtlas->TexData;
			if ( !pTexData || pTexData->Pixels == nullptr ||
			     pTexData->Format != ImTextureFormat_RGBA32 )
			{
				slot.bReady = false;
				return false;
			}

			BlitIntoAtlas( pAtlas, pTexData, rect, image );

			slot.pAtlas = pAtlas;
			slot.pTexData = pTexData;
			slot.nTexWidth = pTexData->Width;
			slot.nTexHeight = pTexData->Height;
			slot.nRectId = nRectId;
			slot.rect = rect;
			slot.bReady = true;
			return true;
		}

		// Unchanged atlas: the rect is still ours, but its UVs are only valid
		// for the current texture, so refresh them every frame as the API
		// requires.
		if ( !pAtlas->GetCustomRect( slot.nRectId, &slot.rect ) )
		{
			slot.nRectId = ImFontAtlasRectId_Invalid;
			slot.bReady = false;
			return false;
		}

		slot.bReady = true;
		return true;
	}

	void ThemeCursor_Draw()
	{
		const ThemeImage &image = GetThemeImage();
		if ( !image.bValid )
			return;

		ImGuiContext *pContext = ImGui::GetCurrentContext();
		if ( !pContext )
			return;

		auto iter = GetSlots().find( pContext );
		if ( iter == GetSlots().end() || !iter->second.bReady )
			return;

		const AtlasSlot &slot = iter->second;

		ImGuiIO &io = ImGui::GetIO();
		if ( !ImGui::IsMousePosValid( &io.MousePos ) )
			return;

		// Hotspot-corrected, so the image's own "point" lands on the pointer
		// position the same way the composited plane's does.
		const ImVec2 vecMin(
			io.MousePos.x - (float)image.nHotX,
			io.MousePos.y - (float)image.nHotY );
		const ImVec2 vecMax(
			vecMin.x + (float)image.nWidth,
			vecMin.y + (float)image.nHeight );

		// Foreground draw list: the same list ImGui's own software cursor uses,
		// so this lands above every panel exactly as that one did.
		ImGui::GetForegroundDrawList()->AddImage(
			slot.pAtlas->TexRef, vecMin, vecMax, slot.rect.uv0, slot.rect.uv1 );
	}
}
