#include "Icons.h"

#include <cmath>
#include <cstring>

namespace gamescope::ui
{
	namespace
	{
		// ---- shape constructors -------------------------------------------
		// Named so the table below reads as the SVG it was transcribed from
		// rather than as a wall of brace initialisers. `Line` is `Poly` with
		// two points and exists only because four of the eleven glyphs are
		// mostly straight rules and `Line( a, b )` says that.
		constexpr IconShape Poly( IconPt a, IconPt b, IconPt c )
		{
			return IconShape{ IconOp::Polyline, 3, 0.0f, { a, b, c } };
		}
		constexpr IconShape Line( IconPt a, IconPt b )
		{
			return IconShape{ IconOp::Polyline, 2, 0.0f, { a, b } };
		}
		constexpr IconShape Rect( float x0, float y0, float x1, float y1 )
		{
			return IconShape{ IconOp::Loop, 4, 0.0f,
				{ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } } };
		}
		constexpr IconShape Circ( float cx, float cy, float r )
		{
			return IconShape{ IconOp::Circle, 1, r, { { cx, cy } } };
		}
		constexpr IconShape Bar( float x0, float y0, float x1, float y1 )
		{
			return IconShape{ IconOp::FillRect, 2, 0.0f, { { x0, y0 }, { x1, y1 } } };
		}

		// =================================================================
		//  THE SET (SPEC §8.0)
		// =================================================================
		// Twelve glyphs, one 24-unit grid, one stroke weight. Eleven are
		// transcribed from index.html's ICONS table (SPEC §8.0's own count);
		// display.general is the twelfth, added when the user's direct
		// correction to D13.1 (2026-08-24) gave DISPLAY a new rail item that
		// predates neither SPEC nor index.html, so it has no source to
		// transcribe from -- see PanelDisplay.cpp's RegisterGeneral().
		//
		// THE ACCEPTANCE CRITERION THIS TABLE IS WRITTEN AGAINST is not
		// "does it look like the thing" -- it is "is it ONE SILHOUETTE at 12
		// physical px, and does it gain no detail at 48". That is why no
		// glyph here has an interior stroke finer than the others, why the
		// three that carry a fill carry it as a solid block rather than a
		// hatch, and why the pairs the shell test found colliding as letters
		// were given deliberately different OUTLINES rather than different
		// details:
		//
		//   Mixer   two faders -- rectangles ON vertical tracks
		//   Monitor three solid bars standing on a baseline
		//   Profiles two offset cards
		//   Per-game one page with a folded corner
		//   Shaders  three stacked layers, a rhombus on top
		//   Shell    a framed window with a rail down its left side
		//
		// At 12 px those six read as: two blocks, three bars, two squares,
		// one square, a stack, a frame. None of them is another one.
		constexpr Icon kIcons[] = {
		// ---- DISPLAY ------------------------------------------------------
		{ "display.general", 4, {
			// Two toggle switches, one off (knob left) and one on (knob
			// right) -- the "quick toggle" mark. Distinct from Mixer's
			// vertical fader tracks (audio.mixer, below) and from every
			// other glyph's shape mix: no other icon pairs a bare line with
			// a single offset circle.
			Line( { 4.0f, 8.0f }, { 20.0f, 8.0f } ),
			Circ( 8.5f, 8.0f, 2.3f ),
			Line( { 4.0f, 16.0f }, { 20.0f, 16.0f } ),
			Circ( 15.5f, 16.0f, 2.3f ) } },

		{ "display.upscaling", 3, {
			// Two corner brackets pulling away from a centre square: the
			// mark for "resample up to a bigger frame".
			Poly( { 3.5f, 9.5f }, { 3.5f, 3.5f }, { 9.5f, 3.5f } ),
			Poly( { 20.5f, 14.5f }, { 20.5f, 20.5f }, { 14.5f, 20.5f } ),
			Rect( 8.5f, 8.5f, 15.5f, 15.5f ) } },

		{ "display.frame_limiter", 2, {
			// A clock. The hands are one open polyline so the join at the
			// centre is a single miter rather than two strokes crossing.
			Circ( 12.0f, 12.0f, 8.5f ),
			Poly( { 12.0f, 6.5f }, { 12.0f, 12.0f }, { 16.0f, 14.5f } ) } },

		{ "display.hdr", 2, {
			// A disc half filled -- SPEC §8.0 names this as one of the two
			// places a fill carries meaning. It is the dynamic-range mark:
			// the same circle, half of it at full luminance.
			Circ( 12.0f, 12.0f, 8.5f ),
			IconShape{ IconOp::HalfDisc, 1, 8.5f, { { 12.0f, 12.0f } } } } },

		{ "image.shaders", 3, {
			// Three stacked layers. The top one is closed (a rhombus seen in
			// plan); the two beneath are open Vs, which is what gives the
			// stack its depth without adding a third weight of line.
			IconShape{ IconOp::Loop, 4, 0.0f, { { 12.0f, 3.2f }, { 20.3f, 7.6f },
			                                    { 12.0f, 12.0f }, { 3.7f, 7.6f } } },
			Poly( { 3.7f, 12.4f }, { 12.0f, 16.8f }, { 20.3f, 12.4f } ),
			Poly( { 3.7f, 16.6f }, { 12.0f, 21.0f }, { 20.3f, 16.6f } ) } },

		// ---- SYSTEM -------------------------------------------------------
		{ "audio.mixer", 6, {
			// Two faders: a track above and below each cap. The cap is a
			// stroked rectangle, NOT a filled one, which is the single
			// difference that keeps this glyph from reading as Monitor's
			// bars at 12 px -- an outline block against a solid one.
			Line( { 7.0f, 3.5f }, { 7.0f, 8.0f } ),
			Line( { 7.0f, 14.5f }, { 7.0f, 20.5f } ),
			Line( { 17.0f, 3.5f }, { 17.0f, 12.0f } ),
			Line( { 17.0f, 18.5f }, { 17.0f, 20.5f } ),
			Rect( 4.0f, 8.0f, 10.0f, 14.5f ),
			Rect( 14.0f, 12.0f, 20.0f, 18.5f ) } },

		{ "system.monitor", 4, {
			// A bar chart standing on a baseline -- SPEC §8.0's second
			// licensed fill. Solid bars, because the silhouette IS the
			// identity here: three filled blocks of different heights.
			Line( { 3.0f, 20.5f }, { 21.0f, 20.5f } ),
			Bar( 4.5f, 12.0f, 8.5f, 18.0f ),
			Bar( 10.0f, 6.5f, 14.0f, 18.0f ),
			Bar( 15.5f, 15.0f, 19.5f, 18.0f ) } },

		{ "system.log", 4, {
			// Four rules of decreasing length: lines of text, ragged right.
			Line( { 3.5f, 5.5f }, { 20.5f, 5.5f } ),
			Line( { 3.5f, 10.5f }, { 20.5f, 10.5f } ),
			Line( { 3.5f, 15.5f }, { 14.5f, 15.5f } ),
			Line( { 3.5f, 20.5f }, { 10.5f, 20.5f } ) } },

		// ---- SETUP --------------------------------------------------------
		{ "setup.profiles", 2, {
			// Two offset cards -- the "one of several saved copies" mark.
			// The back card is an open path, so the two never draw a
			// doubled edge where they overlap.
			Rect( 3.5f, 7.5f, 16.5f, 20.5f ),
			IconShape{ IconOp::Polyline, 5, 0.0f, { { 7.5f, 7.5f }, { 7.5f, 3.5f },
			                                        { 20.5f, 3.5f }, { 20.5f, 16.5f },
			                                        { 16.5f, 16.5f } } } } },

		{ "setup.pergame", 3, {
			// One page with a folded corner: a per-title file, not a stack.
			// The fold is filled -- it is the third licensed fill, and it is
			// what distinguishes this outline from Profiles' front card at
			// 12 px, where the corner notch alone would close up.
			IconShape{ IconOp::Loop, 5, 0.0f, { { 13.5f, 3.5f }, { 6.5f, 3.5f },
			                                    { 6.5f, 20.5f }, { 17.5f, 20.5f },
			                                    { 17.5f, 7.5f } } },
			IconShape{ IconOp::FillPoly, 3, 0.0f, { { 13.5f, 3.5f }, { 17.5f, 7.5f },
			                                        { 13.5f, 7.5f } } },
			Line( { 9.5f, 14.5f }, { 14.5f, 14.5f } ) } },

		{ "setup.appearance", 1, {
			// A droplet -- the colour/paint mark. The only curve in the set
			// that is not a circle, and the reason IconOp::Teardrop exists:
			// see Icons.cpp's Stroke() for the tangent construction.
			IconShape{ IconOp::Teardrop, 2, 6.0f,
				{ { 12.0f, 3.4f }, { 12.0f, 13.7f } } } } },

		{ "setup.shell", 3, {
			// THE ONE GLYPH WITH NO MOCKUP ORIGINAL. index.html's eleventh
			// area is `display.output` (a screen on a stand), which this
			// build does not register; this build's eleventh is
			// `setup.shell`, which the mockup never drew.
			//
			// Drawn as the shell itself: a framed window with a rail down
			// its left edge and a header across the rest. That is literally
			// what the area configures, and it is the reading that keeps it
			// away from Shaders' stack -- a frame with interior divisions
			// against three floating layers.
			Rect( 2.5f, 4.5f, 21.5f, 19.5f ),
			Line( { 8.5f, 4.5f }, { 8.5f, 19.5f } ),
			Line( { 8.5f, 9.5f }, { 21.5f, 9.5f } ) } },
		};

		constexpr size_t kIconN = sizeof( kIcons ) / sizeof( kIcons[ 0 ] );
	}

	const Icon *IconSet()   { return kIcons; }
	size_t      IconCount() { return kIconN; }

	const Icon *IconFor( const char *pszAreaId )
	{
		if ( !pszAreaId )
			return nullptr;
		for ( size_t i = 0; i < kIconN; ++i )
			if ( std::strcmp( kIcons[ i ].pszKey, pszAreaId ) == 0 )
				return &kIcons[ i ];
		return nullptr;
	}
}
