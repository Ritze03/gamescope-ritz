#pragma once

// Zoom.h -- the compositor-drawn zoom (2026-09-14). The user's request:
// *"Something like a zoom bind ... add a zoom when I hold my right mouse
// button or any other key ... choose how big the projection is, the shape,
// so a circle, rectangle, or square ... toggle or hold ... some kind of
// outline ... it shouldn't be affected by the scaling ... after the game,
// the shaders, and so on"* -- and, the same day, a 1.5..5.0 zoom level and
// an option to divide the mouse speed by it.
//
// WHAT THIS FILE OWNS: the config (`zoom`, ZoomSettings), the settings
// area (`system.zoom`, MISC), the active flag and its two entry points.
// WHAT IT DOES NOT: the picture. That is one compute dispatch inside
// vulkan_composite() (rendervulkan.cpp, cs_zoom.comp), fed by the
// FrameInfo_t::Zoom_t request Zoom_FillRequest() writes each frame, because
// the magnified picture has to be the base layer AFTER the effects pre-pass
// and BEFORE the upscaler, and both of those live only in that function.
// The chord is a keybind action (Keybinds.h, Action::Zoom, RMB by default)
// so it is rebound where every other hotkey is.
//
// Threading: Zoom_OnChord() and Zoom_MouseScale() run on the wlserver
// thread and touch atomics only -- the settings they need are mirrored into
// atomics by the steamcompmgr thread whenever the config is (re)loaded.
// Everything else is the steamcompmgr thread. See superdoc/features/zoom.md.

#include <algorithm>
#include <cstdint>

struct FrameInfo_t;

namespace gamescope
{
	namespace ui { class Registry; }

	void Zoom_RegisterArea( ui::Registry &reg );

	// The chord went down (true) or came apart (false). Hold or toggle is
	// this module's own setting; a release in toggle mode is a no-op.
	void Zoom_OnChord( bool bPressed );

	// Fills pFrameInfo->zoom for this frame (and asks for a full composite
	// while zoomed). Called from paint_all() after the base layer is pushed.
	void Zoom_FillRequest( FrameInfo_t *pFrameInfo );

	// The factor to multiply relative mouse motion by: 1 / magnification
	// while zoomed with "Match mouse speed" on, 1.0 otherwise.
	float Zoom_MouseScale();

	// True only when the zoom is enabled AND "Keep the button from the
	// game" (zoom.consume_button) is on. Read on the wlserver thread, from
	// wlserver_dispatch_mouse_button()'s game branch, BEFORE the seat is
	// told about the press -- see superdoc/features/zoom.md.
	bool Zoom_ConsumesButton();

	// Is the zoom picture on screen right now? Mirrors s_bActive; read on
	// the wlserver thread by the scroll-to-adjust path.
	bool Zoom_IsActive();

	// True when "Scroll to change zoom level" (zoom.scroll_adjust) is on:
	// while zoomed, the wheel steps the zoom level instead of reaching the
	// game. Read on the wlserver thread.
	bool Zoom_ScrollAdjustEnabled();

	// The wheel moved by this many notches while zoomed with scroll_adjust
	// on (positive = zoom in / scroll up, negative = zoom out / scroll
	// down). Steps the LIVE factor atomic by 0.25 per notch, clamped to
	// 1.5..5.0, and asks for a repaint -- the picture and Zoom_MouseScale()
	// react on the very next frame. config:: is single-threaded (this runs
	// on the wlserver thread), so the persisted copy is written later, on
	// the steamcompmgr thread, by Zoom_FillRequest() -- see its own comment.
	void Zoom_OnScroll( double flNotches );

	// Pure clamp-and-step, no compositor deps, so it can be unit-tested with
	// no Zoom.cpp/link-time dependency: `cur + 0.25 * notches`, clamped to
	// the zoom's own 1.5..5.0 range.
	inline float Zoom_StepFactor( float flCur, int nNotches )
	{
		return std::clamp( flCur + 0.25f * (float)nNotches, 1.5f, 5.0f );
	}

	// ---- the staged fade (2026-09-16) --------------------------------
	// One progress float, 0 = no zoom .. 1 = fully zoomed, advanced by real
	// elapsed time and split into TWO phases so the user sees WHERE the
	// projection is before its content starts moving: the shape and its
	// outline fade in at their final size over the first half, then the
	// magnification ramps 1.0 -> factor inside that already-visible shape
	// over the second. Reversed on release. See superdoc/features/zoom.md.

	// Where the shape's fade ends and the magnification ramp begins, as a
	// fraction of the progress. An even split: both phases are a plain
	// linear ramp of the same duration, so neither reads as the fast one.
	inline constexpr float kZoomFadeSplit = 0.5f;

	// The new progress after ulDeltaNs of real time, moving towards 1 while
	// bWanted and towards 0 once released -- from wherever it currently is,
	// so a re-press mid-fade-out reverses instead of restarting (the same
	// integrator shape crosshair::AdvanceHide() uses, and for the same
	// reason: the state is the progress itself, not a timestamp). A
	// non-positive duration means "instant", i.e. exactly the pre-fade
	// behaviour. Pure, so it can be checked without linking Zoom.cpp.
	inline float Zoom_AdvanceFade( float flCur, bool bWanted, uint64_t ulDeltaNs, int nDurationMs )
	{
		if ( nDurationMs <= 0 )
			return bWanted ? 1.0f : 0.0f;
		const float d = (float)( double( ulDeltaNs ) / 1e6 / double( nDurationMs ) );
		return bWanted ? std::min( 1.0f, flCur + d ) : std::max( 0.0f, flCur - d );
	}

	// The shape's opacity at this progress: 0 .. 1 over the first phase,
	// then fully opaque while the magnification ramps.
	inline float Zoom_FadeAlpha( float flProgress )
	{
		return std::clamp( flProgress / kZoomFadeSplit, 0.0f, 1.0f );
	}

	// The magnification at this progress: exactly 1.0 (a picture identical
	// to no zoom at all) until the shape is fully faded in, then a linear
	// ramp to flFactor.
	inline float Zoom_FadeFactor( float flProgress, float flFactor )
	{
		const float t = std::clamp( ( flProgress - kZoomFadeSplit ) / ( 1.0f - kZoomFadeSplit ), 0.0f, 1.0f );
		return 1.0f + ( flFactor - 1.0f ) * t;
	}
}
