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
	// A read-only accessor for the settings this tab edits. Overlay/
	// CursorArt.cpp's CursorArt_Draw() calls this once per draw and uses
	// flScale/flOutlineWidth/uOutlineRgb/uInlayRgb directly (clamping
	// flScale and flOutlineWidth defensively at the point of use, since a
	// hand-edited global.json bypasses this tab's own slider ranges).
	// uOutlineRgb is already resolved here -- it is the custom colour when
	// bOutlineFollowsAccent is false, and the live accent otherwise -- so
	// CursorArt.cpp does not need to re-check bOutlineFollowsAccent itself.
	struct CursorAppearance
	{
		float    flScale = 1.0f;
		float    flOutlineWidth = 2.0f;
		bool     bOutlineFollowsAccent = true;
		uint32_t uOutlineRgb = 0x000000;  // 0xRRGGBB -- already resolved: the custom colour, or the live accent when bOutlineFollowsAccent is true
		uint32_t uInlayRgb = 0x000000;    // 0xRRGGBB

		// "Use everywhere" -- off (default) leaves the game-side cursor
		// exactly as upstream, on makes this pointer the game's fallback
		// cursor too (steamcompmgr.cpp's SetDefaultCursorImage(), via
		// Overlay/CursorArt.cpp's CursorArt_Rasterise()). Read every frame
		// by steamcompmgr.cpp's ProcessPendingCursorFallbackPolicy(), on
		// the steamcompmgr thread -- see that function's own comment for
		// why touching MouseCursor from anywhere else is a data race.
		bool bEverywhere = false;
	};

	// Backed by the same load-once, cache-in-a-static-and-refresh-on-write
	// pattern PanelCursor.cpp's own rows use (see its EnsureConfigLoaded()) --
	// never a blocking config::LoadGlobal() call per invocation, so this is
	// safe to call every frame from the steamcompmgr thread the way
	// CursorArt_Draw() would need to (ConfigManager.h's threading comment).
	CursorAppearance GetCursorAppearance();
}
