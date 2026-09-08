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

#include <string>

#include "UI/Controls.h"

namespace gamescope::overlay
{
	// Draws the strip inside rcBlock, whose height the caller has reserved
	// with ui::controls::ComparePreviewHeight(). Safe to call every frame:
	// it captures only when it has to and re-grades only when a slider or the
	// captured frame has actually changed.
	void AbPreview_Draw( const ImRect &rcBlock );

	// WHICH LIMIT IS BINDING, for the Shaders area's Diagnostics facts row
	// (2026-09-08). A clamped slider looks exactly like a working one, which
	// is what made "Target brightness above 0.5 does nothing" cost a session
	// to find; this puts the answer on screen. Returns a short plain-words
	// line -- effects_curve.h's ab_binding_text(), the SAME wording the
	// `effects_ab_log` trace prints, classified from the same statistics --
	// or false while nothing has been measured yet (the effect off, an HDR
	// base layer, or the first frame or two after the panel opened). It arms
	// a capture itself, so the row fills in on its own; the capture is
	// idempotent and costs one 256x144 dispatch per Inspector open.
	bool AbPreview_BindingLine( std::string &sOut );
}
