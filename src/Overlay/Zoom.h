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
#include <cmath>
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
	// while zoomed with "Match mouse speed" on, 1.0 otherwise. `bCursorHidden`
	// is the caller's own read of whether the game's cursor is hidden right
	// now (wlserver.cpp computes it -- see wlserver_mousemotion()'s own
	// comment for the exact rule); when "Only while the cursor is hidden"
	// (zoom.mouse_scale_hidden_only) is on, the scale is 1.0 whenever
	// `bCursorHidden` is false, so a game menu's own visible cursor is never
	// slowed. Ignored (the parameter is simply unread) while that switch is
	// off, which is why every caller may pass a cheap-to-compute bool
	// unconditionally rather than short-circuiting first.
	float Zoom_MouseScale( bool bCursorHidden );

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

	// ---- the staged fade (2026-09-16, re-split 2026-09-23) -----------
	// One progress float, 0 = no zoom .. 1 = fully zoomed, advanced by real
	// elapsed time and split into TWO phases so the user sees WHERE the
	// projection is before its content starts moving: the shape and its
	// outline fade in at their final size over the first phase, then the
	// magnification ramps 1.0 -> factor inside that already-visible shape
	// over the second. Reversed on release. See superdoc/features/zoom.md.

	// Where the shape's fade ends and the magnification ramp begins, as a
	// fraction of the progress. 2026-09-23: the user asked for the outline
	// to be quick and the zoom-in to get the rest of the time -- *"The
	// outline of the projector should fade in in the first 10% of the set
	// time and the other 90% should be used for increasing the zoom inside
	// of the projector."* Was an even 0.5 split (both phases the same
	// linear-ramp duration) until this change.
	inline constexpr float kZoomFadeSplit = 0.1f;

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

	// ---- sharpen (2026-09-22) ------------------------------------------
	// The Sharpen slider (zoom.sharpen) has to be scaled to 0 for as long
	// as the fade's phase 1 holds the picture at exactly 1.0x -- that phase
	// is a measured, documented guarantee (zoom.md's fade-identity
	// invariant) that the projector is byte-identical to the unzoomed
	// frame beneath it, and sharpening at 1.0x (where RCAS's 5-tap cross
	// samples the SAME texel five times, since u_srcPerDst is source texels
	// per output pixel and at 1.0x that is one texel per pixel) would still
	// perturb rounding at the shape's own edges and break it. So the ramp
	// rides the SAME progress as the magnification: 0 while flCurFactor is
	// still 1.0, 1 once it reaches flTargetFactor, in between exactly the
	// magnification's own fraction of the way there -- the two finish
	// together at the top of the fade instead of disagreeing about when
	// the picture is "fully zoomed". Pure, so it needs no Zoom.cpp link, as
	// the fade helpers above do not.
	inline float Zoom_SharpenRamp( float flCurFactor, float flTargetFactor )
	{
		if ( flTargetFactor <= 1.0f )
			return 0.0f;
		return std::clamp( ( flCurFactor - 1.0f ) / ( flTargetFactor - 1.0f ), 0.0f, 1.0f );
	}

	// ---- sharpen amount mapping (2026-09-22, retuned same day) ---------
	// zoom.sharpen is 0..1 in the UI, but cs_zoom.comp's unsharp-mask
	// u_sharpenAmount is NOT a linear rescale of it -- the operator's own
	// gain saturates fast (its per-channel min/max clamp is what stops a
	// large amount from ever ringing, but it also means a large amount
	// stops buying anything once most edge pixels are already pinned to
	// their local lo/hi). Mean gradient-energy gain vs amount, measured on
	// the default test client (headless captures, factor 3.0, circle, size
	// 0.5 -- see superdoc/features/zoom.md's "Sharpen" section for the full
	// rig and both mid-tone clients):
	//
	//   amount   0.5    1     2     4     8     16    32    64   2000(*)
	//   gain %   7.9   11.2  14.4  17.7  20.7  22.7  23.7  24.3  25.1
	//
	// (*) 2000 is the measured ASYMPTOTE -- amount -> infinity is safe
	// (the clamp bounds it) but buys almost nothing past a few dozen.
	//
	// A LINEAR amount = kMax * k spends almost the whole slider in the flat
	// top of that curve -- the first cut shipped exactly that, with
	// kMax=64, and k=0.5 already reached ~97% of k=1.0's effect, so the
	// bottom half of the slider all looked the same. This inverts a
	// Michaelis-Menten fit
	// of the table above, gain(amount) = Ginf*amount/(amount+K), instead:
	// solving amount(k) so that gain(amount(k)) lands at roughly k of the
	// gain reached at k=1 (kZoomSharpenMaxAmount itself) makes the SLIDER,
	// not just the amount, roughly linear in its own effect --
	// k=0.25/0.5/0.75/1.0 land at roughly a quarter/half/three-quarters/
	// the chosen ceiling of the reachable gain instead of bunching near
	// the top. kZoomSharpenHalfK is that fit's K (half-saturation amount,
	// least-squares over the table above with Ginf fixed at the measured
	// 25.12 asymptote). The formula is exact at both ends by construction
	// regardless of fit error: amount(0) = 0, amount(1) = kZoomSharpenMaxAmount.
	//
	// kZoomSharpenMaxAmount was RETUNED 2026-09-22 from 32 down to 1.3 (a
	// visual QC pass, not a second gradient-energy measurement -- see
	// superdoc/features/zoom.md's "Sharpen" section for the full writeup).
	// The per-channel min/max clamp this operator applies (cs_zoom.comp)
	// stops it *overshooting* past the local neighbourhood, but not
	// *collapsing onto* it: at large amounts, mid-grey pixels inside a
	// glyph stroke get pulled all the way down to the neighbourhood
	// minimum, gouging apertures to black and stair-stepping curves at the
	// 3px magnification period -- and gradient-energy REWARDS this (it's a
	// bigger gradient), so the metric that picked 32 as "94.5% of
	// asymptotic gain" was measuring the wrong thing. 1.3 is amount(k=0.5)
	// under the OLD ceiling of 32 -- the strength a human grader found
	// clean and clearly crisper than off, with objectionable gouging
	// already visible by the old k=0.75 (amount ~3.55). Reusing the same
	// Michaelis-Menten formula and kZoomSharpenHalfK with this new,
	// smaller ceiling keeps the same "slider spreads gain evenly toward
	// the ceiling" property (gain(amount(k)) = k * gain(kZoomSharpenMaxAmount)
	// exactly, by construction, independent of the ceiling's value) and
	// amount(0) = 0 exactly (sharpen 0 stays byte-identical -- the shader
	// branches out on amount == 0). If a stronger ceiling is wanted later,
	// prefer tightening the clamp itself (lerp the min/max bounds toward
	// the original pixel as amount grows) over raising this number again --
	// that targets the actual failure (collapse-to-rail) instead of
	// trading it for less overall sharpening.
	inline constexpr float kZoomSharpenMaxAmount = 1.3f;
	inline constexpr float kZoomSharpenHalfK = 1.39f;

	inline float Zoom_SharpenAmount( float k )
	{
		k = std::clamp( k, 0.0f, 1.0f );
		return ( kZoomSharpenMaxAmount * kZoomSharpenHalfK * k )
			/ ( kZoomSharpenMaxAmount * ( 1.0f - k ) + kZoomSharpenHalfK );
	}

	// ---- clicks land on what the projector shows (2026-09-27) ----------
	// The user's bug report: "the position of the mouse on screen and what
	// is being selected in the game doesn't match up" while zoomed. Fix
	// (their own pick, (b) over moving the drawn arrow): while the cursor
	// is visible AND sits inside the projector's shape, the ABSOLUTE
	// pointer position handed to the game becomes centre + (p-centre) /
	// liveFactor -- the point the projector is showing under the arrow --
	// instead of the arrow's own on-screen position. The drawn arrow
	// itself is untouched; only the copy sent to the client changes. See
	// superdoc/features/zoom.md's "Clicks land on what the projector
	// shows" for the full writeup, the accepted edge-of-shape jump, and
	// why this works in the game SURFACE's own coordinate space rather
	// than output pixels.
	//
	// Everything below is pure -- no atomics, no globals, no Zoom.cpp link
	// -- so it is unit-testable on its own (tests/test_pointer_mapping.cpp)
	// even though that test binary never links Zoom.cpp. Zoom_MapPointerForGame()
	// (declared at the bottom, defined in Zoom.cpp) is the runtime
	// accessor that fills a ZoomRemapGeometry from this frame's published
	// atomics and calls ZoomRemapPoint() with it -- the one thing that DOES
	// need Zoom.cpp, so wlserver.cpp calls that instead of these directly.

	enum class ZoomShape { Circle, Box };  // Box covers both rectangle and square: same test, equal half-extents for a square.

	// A projector's shape and placement, entirely in the game SURFACE's own
	// coordinate space (the space wlserver.mouse_surface_cursorx/y live in)
	// -- NOT output pixels. flHalfW/flHalfH are therefore the on-screen
	// shape's half-extents divided by "output pixels per surface pixel"
	// per axis, so a circle that is round on screen can be an ELLIPSE here
	// whenever the game is scaled non-uniformly per axis (letterboxed to a
	// different aspect than it renders at) -- ZoomPointInsideShape() tests
	// it as one on purpose.
	struct ZoomRemapGeometry
	{
		bool      bVisible = false;          // a projector is actually being drawn this frame (not just "enabled")
		ZoomShape shape = ZoomShape::Box;
		double    flCenterX = 0.0, flCenterY = 0.0;   // surface px
		double    flHalfW = 0.0, flHalfH = 0.0;       // surface px, half-extents, at the shape's FINAL size (unaffected by the fade's alpha)
		double    flFactor = 1.0;            // the LIVE, already fade-ramped magnification (1.0 = no-op by construction)
	};

	// True when (x,y) -- surface px -- falls inside the shape `geo`
	// describes. Mirrors cs_zoom.comp's own edge test: a circle is the
	// normalized-ellipse form (exact when flHalfW == flHalfH, which is
	// what an on-screen circle normally gives), a box is a plain per-axis
	// bound (correct for both a rectangle and a square).
	inline bool ZoomPointInsideShape( double x, double y, const ZoomRemapGeometry &geo )
	{
		if ( geo.flHalfW <= 0.0 || geo.flHalfH <= 0.0 )
			return false;
		const double dx = x - geo.flCenterX;
		const double dy = y - geo.flCenterY;
		if ( geo.shape == ZoomShape::Circle )
		{
			const double nx = dx / geo.flHalfW;
			const double ny = dy / geo.flHalfH;
			return ( nx * nx + ny * ny ) <= 1.0;
		}
		return std::abs( dx ) <= geo.flHalfW && std::abs( dy ) <= geo.flHalfH;
	}

	// The remap itself. Leaves (x,y) untouched -- zero cost, zero risk of
	// a wrong answer -- whenever the cursor isn't genuinely visible, no
	// projector is being drawn this frame, the point is outside the shape,
	// or the live magnification is at/under 1.0 (not zoomed, or still in
	// the fade's pinned-at-1.0x first phase: a no-op by construction, not
	// a special case). Otherwise: centre + (p - centre) / factor. Points
	// exactly on the shape's edge (or one frame apart from it) map on
	// different sides of the divide by design -- the accepted cost the
	// user signed off on in exchange for (b) over (a).
	inline void ZoomRemapPoint( double &x, double &y, bool bCursorVisible, const ZoomRemapGeometry &geo )
	{
		if ( !bCursorVisible || !geo.bVisible || geo.flFactor <= 1.0 )
			return;
		if ( !ZoomPointInsideShape( x, y, geo ) )
			return;
		x = geo.flCenterX + ( x - geo.flCenterX ) / geo.flFactor;
		y = geo.flCenterY + ( y - geo.flCenterY ) / geo.flFactor;
	}

	// The runtime accessor wlserver.cpp actually calls, from the ONE place
	// a wl_pointer.motion leaves for the client
	// (wlserver_send_absolute_motion(), which both wlserver_mousemotion()
	// and wlserver_mousewarp() funnel through): remaps the copy of
	// (x,y) -- already in surface space -- about to be sent to the game,
	// leaving wlserver.mouse_surface_cursorx/y (the accumulator the drawn
	// cursor and every relative-motion delta read) untouched.
	// `bCursorVisible` is the caller's own
	// `wlserver.bCursorHasImage && !wlserver_pointer_is_locked()` --
	// deliberately NOT `wlserver.bCursorHidden`, which also covers
	// gamescope's own idle auto-hide and would suppress the remap on an
	// idle-but-visible menu cursor. Defined in Zoom.cpp (needs this
	// frame's published atomics); a no-op, including while Zoom.cpp is
	// never linked at all (nothing here calls it), for anything that only
	// wants the pure functions above.
	void Zoom_MapPointerForGame( double &x, double &y, bool bCursorVisible );
}
