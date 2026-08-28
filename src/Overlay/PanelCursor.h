// The "Cursor" tab -- lets the user design the overlay's own custom-drawn
// pointer (Overlay/CursorArt.cpp: a triangle silhouette, outline over a
// solid inlay, drawn by the overlay for itself while it is open). See
// superdoc/planning/overlay-presentation-architecture.md for where this
// tab sits among the rest of the settings overlay.
//
// Registered exactly like PanelDisplay.h's panel: declared rows, no ImGui
// in this header, called once at startup from Overlay/UI/Shell.cpp's
// RegisterAll(). See PanelCursor.cpp for the fields, ranges and defaults,
// and for the accessor below's own comment.
#pragma once

#include <cstdint>

#include "UI/Registry.h"

namespace gamescope
{
	void PanelCursor_RegisterArea( ui::Registry &reg );

	// ---- the wiring seam --------------------------------------------------
	// A read-only accessor for the settings this tab edits, meant for
	// Overlay/CursorArt.cpp's drawing code to call. NOT YET CALLED FROM
	// THERE as of this writing (2026-08-28) -- CursorArt.cpp is owned by a
	// different, concurrently-running change (the cursor's game-side
	// rasterisation is being removed there), so wiring this accessor in is
	// a deliberate follow-up rather than something this file does itself.
	//
	// The one-line change that follow-up needs, once CursorArt.cpp is
	// free to take it:
	//
	//   - CursorArt_Draw()'s `flScale` parameter should be multiplied by
	//     gamescope::CursorAppearance().flScale (or the caller should pass
	//     that value in directly, same effect);
	//   - kOutlineWidth should be replaced with
	//     gamescope::CursorAppearance().flOutlineWidth;
	//   - CursorArt_AccentRgb()'s result (used for both the live draw's
	//     outline colour and CursorArt_Rasterise()'s outline/inlay split)
	//     should fall back to gamescope::CursorAppearance().uOutlineRgb
	//     when CursorAppearance().bOutlineFollowsAccent is false;
	//   - the inlay fill (currently the literal IM_COL32(0,0,0,255) in
	//     CursorArt_Draw() and the hardcoded black lerp target in
	//     CursorArt_Rasterise()) should read
	//     gamescope::CursorAppearance().uInlayRgb instead.
	//
	// Until that lands, every control on this tab is fully functional and
	// persists to config, but has NO effect on what the pointer looks like
	// on screen -- see PanelCursor.cpp's file comment.
	struct CursorAppearance
	{
		float    flScale = 1.0f;
		float    flOutlineWidth = 2.0f;
		bool     bOutlineFollowsAccent = true;
		uint32_t uOutlineRgb = 0x000000;  // 0xRRGGBB, only meaningful when bOutlineFollowsAccent is false
		uint32_t uInlayRgb = 0x000000;    // 0xRRGGBB
	};

	// Backed by the same load-once, cache-in-a-static-and-refresh-on-write
	// pattern PanelCursor.cpp's own rows use (see its EnsureConfigLoaded()) --
	// never a blocking config::LoadGlobal() call per invocation, so this is
	// safe to call every frame from the steamcompmgr thread the way
	// CursorArt_Draw() would need to (ConfigManager.h's threading comment).
	CursorAppearance GetCursorAppearance();
}
