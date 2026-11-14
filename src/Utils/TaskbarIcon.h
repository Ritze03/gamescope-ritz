#pragma once

// The taskbar icon of gamescope's own window. See
// superdoc/features/backend-wayland.md "Window icon and app id".
//
// Hosts without xdg-toplevel-icon-v1 (Hyprland, GNOME) find a window's icon
// only from its app_id, through a .desktop entry. So the running game's icon
// is written as a hicolor PNG plus a NoDisplay .desktop entry named after a
// key derived from the icon pixels, and the window's app_id is set to that
// key.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gamescope::taskbar_icon
{
	// The app_id used when the game has no icon.
	inline constexpr const char *k_szFallbackKey = "gamescope-ritz";

	// GAMESCOPE_APP_ID, read once. When non-empty it is the fixed app_id of the
	// whole session (to group with an existing launcher shortcut): no
	// per-game switching, no generated .desktop files.
	const std::string &EnvAppId();

	// The Name= of the fallback entry, and of any entry with no usable title.
	inline constexpr const char *k_szFallbackName = "Gamescope";
	inline constexpr size_t k_nMaxNameLen = 100;

	struct Image
	{
		uint32_t uWidth = 0;
		uint32_t uHeight = 0;
		const uint32_t *pPixels = nullptr; // ARGB, native uint32, uWidth*uHeight
	};

	// _NET_WM_ICON holds several [w, h, pixels...] images back to back. Picks
	// the largest well-formed one; false if there is none.
	bool LargestImage( const std::vector<uint32_t> &data, Image &out );

	// "gamescope-ritz-<8 hex>": FNV-1a over width, height and the pixels.
	std::string KeyForImage( const Image &img );

	// PNG (RGBA) of an ARGB image. Empty on failure.
	std::vector<uint8_t> EncodePng( const Image &img );

	// A window title as a .desktop Name: control characters (CR, LF...) dropped,
	// trimmed, capped at k_nMaxNameLen bytes (not mid UTF-8 character);
	// k_szFallbackName when nothing is left.
	std::string SanitizeName( const std::string &szName );

	// Text of the NoDisplay .desktop entry for key; szName goes through SanitizeName.
	std::string DesktopEntryText( const std::string &szKey, const std::string &szName, const std::string &szIconPath );

	// Remembers the focused window's title (the Name of per-game entries
	// written from now on) and rewrites szKey's .desktop with the new Name if szKey is a
	// per-game key written by this process (not the fallback). Skipped when
	// the file already has that content.
	void UpdateName( const std::string &szKey, const std::string &szTitle );

	// Writes (rewritten only when changed) the PNG and .desktop for the game's
	// icon under $XDG_DATA_HOME and returns the key, or "" if there is no
	// usable icon or the write failed. Files exist before this returns, so the
	// caller may set the app_id right after.
	std::string EnsureForIcon( const std::vector<uint32_t> &data );

	// Same for the embedded project icon; returns k_szFallbackKey, or "" on failure.
	std::string EnsureFallback();

	// The embedded project icon as [w, h, ARGB...] (the _NET_WM_ICON layout),
	// for protocols/APIs that take pixels. Empty on decode failure.
	const std::vector<uint32_t> &ProjectIconPixels();
}
