#pragma once

// UiBoxPreview.h -- the Inspector's picture of what lies under Frame generation's
// UI-protection box.
//
// The user: "a slider, so the user can adjust it for himself, and then in the
// right inspector rail we can show an image of the area that's underneath the
// current UI, so he can see if the whole crosshair fits in, and then the user can
// configure it, like, really precise."
//
// A crop of the game's centre (the box plus context), pixel-exact, with the box
// outlined and everything outside it dimmed. The pixels come from the
// frame-generation host (fghost::GetBoxPreview(): a small compute pass on the
// game's real frame, captured while this picture is on screen and read back
// without a wait); this file only owns the ImGui texture and the drawing.
// See superdoc/features/frame-generation.md, "The box preview".

#include "UI/Controls.h"

namespace gamescope::overlay
{
	// The block's height for a content width, asked for BEFORE drawing so the rows
	// below never move whether or not there is a frame yet.
	float UiBoxPreview_Height( float flWidth );

	// Draws the picture inside rcBlock (UiBoxPreview_Height() tall). Safe to call
	// every frame: it asks the renderer for a capture (which lapses by itself when
	// the calls stop) and re-uploads only when a new capture arrived.
	void UiBoxPreview_Draw( const ImRect &rcBlock );
}
