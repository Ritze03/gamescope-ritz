// The pointer the settings overlay draws for itself: a plain triangle, accent-
// coloured outline, black inlay. The black-inside-colour combination is the
// point -- it stays readable over both dark and bright game content without a
// drop shadow.
//
// SCOPE, deliberately: this is the OVERLAY's pointer and nothing else. While
// the overlay is closed gamescope does not touch the cursor at all -- the game
// keeps whatever cursor it or the host set, exactly as upstream does. An
// earlier version of this file also rasterised the same triangle into an X11
// cursor for the game side; that override was removed on request. See
// superdoc/features/cursor-pipeline.md.
#pragma once

#include <cstdint>

struct ImDrawList;

namespace gamescope::overlay
{
	// Draws with the tip -- the hotspot -- landing exactly on (flTipX, flTipY).
	void CursorArt_Draw( ImDrawList *pDrawList, float flTipX, float flTipY, float flScale );

	// The accent the outline is currently drawn in, as 0x00RRGGBB. Follows the
	// user's accent-hue slider.
	uint32_t CursorArt_AccentRgb();
}
