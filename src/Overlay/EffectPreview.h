#pragma once

// EffectPreview.h -- the Inspector's Adaptive Brightness before/after strip.
//
// requests-2026-09-08.md: "a small comparison picture to the inspector rail,
// that is split in half (left/right) with one being the original image
// (captured frame, when the UI was opened) and the one modified by the
// adaptive brightness".
//
// One frozen frame, captured from the compositor when the strip appears,
// shown split down the middle: the left half as the picture is before
// Adaptive Brightness, the right half with the effect applied at whatever the
// sliders currently say. Dragging a slider re-grades the frozen frame on the
// CPU -- it does not wait for, or touch, the game.
//
// The pure arithmetic is split out twice, so both halves are testable without
// a GPU or an ImGui context: EffectPreviewMath.h has the pixel work,
// UI/Controls.h has the block's layout and its placeholder state machine.
// See superdoc/features/shader-effects.md for the whole design.

#include "UI/Controls.h"

namespace gamescope::overlay
{
	// Draws the strip inside rcBlock, whose height the caller has reserved
	// with ui::controls::ComparePreviewHeight(). Safe to call every frame:
	// it captures only when it has to and re-grades only when a slider or the
	// captured frame has actually changed.
	void AbPreview_Draw( const ImRect &rcBlock );
}
