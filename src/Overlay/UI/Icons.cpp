#include "Icons.h"

#include <cmath>
#include <cstring>

// setup.cursor's glyph reuses the pointer's own three corner constants (see
// the entry below) rather than a fourth hand-transcribed copy of the
// triangle -- this is the only reason this otherwise ImGui-free, panel-free
// data file includes anything from outside UI/. CursorArt.h itself pulls in
// no ImGui (only CursorArt.cpp does), so this stays as light an include as
// every other one here.
#include "../CursorArt.h"

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
		constexpr IconShape RoundRect( float x0, float y0, float x1, float y1, float r )
		{
			return IconShape{ IconOp::RoundRect, 2, r, { { x0, y0 }, { x1, y1 } } };
		}

		// =================================================================
		//  THE SET (SPEC §8.0)
		// =================================================================
		// Sixteen glyphs, one 24-unit grid, one stroke weight. Eleven are
		// transcribed from index.html's ICONS table (SPEC §8.0's own count);
		// display.general is the twelfth, added when the user's direct
		// correction to D13.1 (2026-08-24) gave DISPLAY a new rail item that
		// predates neither SPEC nor index.html, so it has no source to
		// transcribe from -- see PanelDisplay.cpp's RegisterGeneral().
		// setup.cursor is the thirteenth (2026-08-29), and sourced neither
		// way: it is the actual pointer shape, reused from CursorArt.h's own
		// corner constants -- see that entry below. system.general is the
		// fourteenth (2026-09-05), the System tab's own area -- see
		// PanelSystem.cpp. display.resolution and system.crosshair are the
		// fifteenth and sixteenth (2026-09-05, requests item 12): both areas
		// had shipped drawing the rail's letter fallback (`R`, `C`) and were
		// drawn freehand here in the set's own style -- see their entries.
		// system.friends is the seventeenth (2026-09-08), for exactly the
		// same reason a third time: it shipped on the letter fallback (`F`),
		// and it is later than the mockup, so it is freehand too.
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

		// setup.cursor's fixed icon-local transform: maps CursorArt.h's
		// triangle (0..kWingX by 0..kFootY, tip at the origin) onto this
		// set's usual ~3.5-unit margin on a 24-unit box, matching the
		// visual weight of e.g. the clock/HDR circles' 17-unit diameter.
		// Deliberately NOT gamescope::GetCursorAppearance().flScale -- see
		// this file's own header comment on why the rail glyph must not
		// track the live cursor.scale setting.
		constexpr float kCursorIconScale = 17.0f / gamescope::overlay::kFootY;
		constexpr float kCursorIconOffsetX = ( kIconGrid - gamescope::overlay::kWingX * kCursorIconScale ) * 0.5f;
		constexpr float kCursorIconOffsetY = ( kIconGrid - gamescope::overlay::kFootY * kCursorIconScale ) * 0.5f;

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

		{ "display.resolution", 2, {
			// REDRAWN 2026-09-07 (requests-2026-09-07.md item 6): the user
			// rejected the 2026-09-06 angle fix outright ("still looks the
			// same as before") -- rightly, since that pass only straightened
			// the old shaft and arrowheads without changing the underlying
			// idea, and a diagonal double-headed arrow inside a frame is
			// exactly the mark being rejected, correctly drawn or not. This
			// throws that idea away rather than tuning it again: no arrow,
			// no diagonal, no line at all.
			//
			// Two concentric frames, uniformly inset -- a picture held
			// inside a smaller picture, which is what a resolution change
			// actually is (the same image, at a different size). Read
			// against its rail neighbours: Upscaling (just above) is a
			// SMALL centred square with two L-brackets pulling AWAY from
			// it, open at the corners and drawing nothing at the box's own
			// edge; this is a LARGE outer frame flush with the box's edge
			// with a second, smaller, fully-closed frame concentric inside
			// it -- two closed rectangles, not brackets and a square. At
			// 12 px it reads as a frame-in-a-frame; at 48 px it is still
			// exactly that, no new detail earned by the extra pixels.
			Rect( 3.5f, 5.5f, 20.5f, 18.5f ),
			Rect( 7.5f, 8.5f, 16.5f, 15.5f ) } },

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
		{ "system.general", 5, {
			// A chip: a square body with one pin centred on each of its
			// four edges -- the "system/device" mark. Distinct from every
			// neighbour's shape mix: no other glyph in the set pairs a
			// single stroked rectangle with lines radiating from its
			// edge midpoints (Mixer's lines sit ON its rectangles, not
			// off them; display.general's lines run corner to corner
			// with circles, not off a square).
			Rect( 7.5f, 7.5f, 16.5f, 16.5f ),
			Line( { 12.0f, 7.5f }, { 12.0f, 4.0f } ),
			Line( { 12.0f, 16.5f }, { 12.0f, 20.0f } ),
			Line( { 7.5f, 12.0f }, { 4.0f, 12.0f } ),
			Line( { 16.5f, 12.0f }, { 20.0f, 12.0f } ) } },

		{ "setup.keybinds", 5, {
			// A keyboard: one wide, shallow outline with three filled keycaps
			// in a row and one long bar under them -- the spacebar. The
			// SILHOUETTE is the identity, which is why the caps are filled
			// (SPEC 8.0's licensed fill, same argument as the HUD's bars):
			// three stroked cap outlines inside a stroked body reads as a
			// hatched rectangle at 12 px. Distinct from Mixer (stroked caps
			// on lines, no enclosing body) and from system.general's chip
			// (a square with pins radiating OFF its edges, nothing inside).
			Rect( 2.5f, 6.5f, 21.5f, 17.5f ),
			Bar( 5.0f, 9.0f, 8.0f, 11.5f ),
			Bar( 10.5f, 9.0f, 13.5f, 11.5f ),
			Bar( 16.0f, 9.0f, 19.0f, 11.5f ),
			Bar( 7.0f, 13.5f, 17.0f, 15.5f ) } },

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

		{ "system.hud", 4, {
			// A bar chart standing on a baseline -- SPEC §8.0's second
			// licensed fill. Solid bars, because the silhouette IS the
			// identity here: three filled blocks of different heights.
			Line( { 3.0f, 20.5f }, { 21.0f, 20.5f } ),
			Bar( 4.5f, 12.0f, 8.5f, 18.0f ),
			Bar( 10.0f, 6.5f, 14.0f, 18.0f ),
			Bar( 15.5f, 15.0f, 19.5f, 18.0f ) } },

		{ "system.crosshair", 5, {
			// A reticle: a ring with four arms that cross its edge and stop
			// short of the centre, leaving it open. The arms cross the ring
			// on purpose -- four arms alone with a gap read as a dashed
			// plus at 12 px (tried, rejected), and the ring is what says
			// "sight" rather than "add". Read against system.general's
			// chip, its nearest shape mix: the chip's pins stand OFF the
			// edges of a small square, these arms run THROUGH the edge of
			// a large circle and the centre is a hole. Freehand
			// (2026-09-05) -- the crosshair feature itself is a later
			// addition than the mockup; see Crosshair.cpp.
			Circ( 12.0f, 12.0f, 7.0f ),
			Line( { 12.0f, 2.5f }, { 12.0f, 9.0f } ),
			Line( { 12.0f, 15.0f }, { 12.0f, 21.5f } ),
			Line( { 2.5f, 12.0f }, { 9.0f, 12.0f } ),
			Line( { 15.0f, 12.0f }, { 21.5f, 12.0f } ) } },

		{ "system.zoom", 2, {
			// A MAGNIFYING GLASS: a ring with a handle running off it to the
			// lower right. Freehand (2026-09-14), later than the mockup like
			// the three before it. Read against system.crosshair, its
			// nearest neighbour: that one's arms cross the ring on all four
			// sides and stop at an open centre; this ring is whole, and the
			// one stroke leaves it at a single diagonal.
			Circ( 10.0f, 10.0f, 6.5f ),
			Line( { 14.8f, 14.8f }, { 21.0f, 21.0f } ) } },

		{ "input.general", 4, {
			// A KEYCAP AND A MOUSE, SIDE BY SIDE: the twenty-first glyph
			// (2026-09-27), for input.general -- the two things this area's
			// switches force-grab. Freehand, later than every mockup.
			// Distinct from railgroup.input just above IconForRailGroup()
			// below (a single rounded body with a bar -- read as a
			// controller/pad), from system.autoclicker below (one TALL
			// rounded body with a filled corner) and from system.null_binds
			// further down (two SQUARE keycaps joined by a chevron): this
			// is the only glyph pairing a small plain keycap with a small
			// plain mouse body, neither filled, side by side rather than
			// stacked or joined.
			//
			// Left: a small stroked keycap. Right: a small stroked mouse
			// body with the classic top-view two-button split -- a
			// vertical line down the centre of its upper third, a
			// horizontal line closing that third off from the lower body --
			// deliberately the plainest possible "mouse" reading, since the
			// fill this set reserves for meaning already went to
			// system.autoclicker's held button; this glyph draws capture,
			// not a click.
			RoundRect( 2.5f, 7.5f, 10.0f, 16.5f, 1.4f ),
			RoundRect( 13.5f, 4.0f, 21.0f, 19.5f, 3.7f ),
			Line( { 17.25f, 4.0f }, { 17.25f, 10.0f } ),
			Line( { 13.5f, 10.0f }, { 21.0f, 10.0f } ) } },

		{ "system.autoclicker", 3, {
			// A MOUSE SEEN FROM ABOVE with its left button held down: a
			// rounded vertical body, a line across under the two buttons,
			// and the left button filled solid. Freehand (2026-09-18),
			// later than the mockup like the four before it.
			//
			// REDRAWN 2026-09-22: the user found the first cut's square-
			// cornered body "looks kind of weird and not really like a mouse,
			// just use a rounded vertical rectangle". The body is now a pill
			// (radius = half its 11-unit width, so the top and bottom are
			// semicircles -- IconOp::RoundRect exists for this).
			//
			// NOTHING IS DRAWN TWICE, and every edge meets another edge
			// exactly. The rail's idle colour is translucent, so any overlap
			// shows as a brighter seam, and a stroke is centred on its path:
			// a line at y=10 covers 9.15..10.85 (kIconStroke 1.7). So the
			// button line runs all the way across between the outline's
			// INNER edges (x 7.35..16.65), and the fill stops at the line's
			// TOP edge (y 9.15) and at the outline's inner edge -- its arc
			// is the body's corner arc shrunk by half a stroke (centre
			// (12, 8), radius 5.5 - 0.85 = 4.65), sampled at 0/30/60/90
			// degrees: six points, exactly kIconMaxPts. Its right edge is
			// the seam between the buttons. The same day's earlier cut
			// ended the fill at the line's CENTRE and stroked only the right
			// half of the line, which left that half hanging half a stroke
			// below the filled button: "offset and thus look weird".
			//
			// The fill is SPEC 8.0's "only where a fill carries meaning"
			// again: the whole identity of this area is "a button is being
			// held", and a solid quarter is what says that at 12 px where a
			// second outline would just be noise. Read against its nearest
			// neighbours: audio.mixer is two WIDE rectangles on tracks and
			// system.hud is three bars on a baseline -- no other glyph is a
			// single TALL rounded outline with a filled corner inside it.
			RoundRect( 6.5f, 2.5f, 17.5f, 21.5f, 5.5f ),
			IconShape{ IconOp::FillPoly, 6, 0.0f,
				{ { 12.0f, 9.15f }, { 7.35f, 9.15f }, { 7.35f, 8.0f },
				  { 7.97f, 5.68f }, { 9.68f, 3.97f }, { 12.0f, 3.35f } } },
			Line( { 7.35f, 10.0f }, { 16.65f, 10.0f } ) } },

		{ "system.null_binds", 4, {
			// TWO KEYCAPS with a priority arrow between them: the SOCD-
			// cleaning identity in one glyph -- of a pair of keys, only one
			// is ever "down" in the game at a time, and priority moves to
			// whichever was pressed last. Freehand (2026-09-27), later than
			// the mockup like every glyph after system.zoom.
			//
			// Left keycap is a plain stroked square (not currently the
			// winner); right keycap is the same square with a smaller
			// FillRect inset (the current winner, "held down"); the chevron
			// between them is priority handing off left-to-right, the same
			// three-point Poly() the accordion's own chevrons use elsewhere
			// in this file's style, just rotated to point right instead of
			// down. Read against its nearest neighbours: system.autoclicker
			// is one TALL rounded body with a filled CORNER, and no other
			// glyph in the set is two SEPARATE square outlines joined by a
			// chevron.
			RoundRect( 2.0f, 8.0f, 10.0f, 16.0f, 1.6f ),
			RoundRect( 14.0f, 8.0f, 22.0f, 16.0f, 1.6f ),
			Bar( 16.0f, 10.0f, 20.0f, 14.0f ),
			Poly( { 10.8f, 10.2f }, { 13.2f, 12.0f }, { 10.8f, 13.8f } ) } },

		{ "system.friends", 2, {
			// A PERSON: a head over a pair of shoulders. Freehand
			// (2026-09-08), like the crosshair and resolution glyphs before
			// it, and for the same reason -- the friends list is a later
			// addition than the mockup, so there is nothing to transcribe.
			//
			// `Why one person and not two:` two heads at 12 physical px is
			// two dots and a smear. One circle over one open trapezoid is a
			// single silhouette that survives the collapse, which is this
			// table's stated acceptance criterion. Read against its nearest
			// neighbours: setup.profiles is two offset RECTANGLES, and
			// setup.cursor is a closed three-point triangle -- no other
			// glyph pairs a circle with an OPEN flaring path, and none of
			// the three shares an outline with another.
			Circ( 12.0f, 8.0f, 4.0f ),
			IconShape{ IconOp::Polyline, 4, 0.0f, { { 3.5f, 20.5f }, { 6.5f, 14.5f },
			                                        { 17.5f, 14.5f }, { 20.5f, 20.5f } } } } },

		{ "system.log", 4, {
			// Four rules of decreasing length: lines of text, ragged right.
			Line( { 3.5f, 5.5f }, { 20.5f, 5.5f } ),
			Line( { 3.5f, 10.5f }, { 20.5f, 10.5f } ),
			Line( { 3.5f, 15.5f }, { 14.5f, 15.5f } ),
			Line( { 3.5f, 20.5f }, { 10.5f, 20.5f } ) } },

		{ "system.changelog", 3, {
			// REDRAWN 2026-09-09: the area's label changed from
			// "Changelog" to "About" (version rows, the changelog and
			// the licences all live here now), and the old six-shape
			// "three dated entries" bullet-list mark stopped fitting an
			// area that isn't just a list of dated bullets any more --
			// the id stayed `system.changelog` (see PanelChangelog.cpp's
			// own comment on that), only the glyph needed to change.
			//
			// The conventional information mark: a circle, a dot near
			// the top, a short stem below it. Built from the same two
			// primitives the rest of the set already leans on rather
			// than a new one -- the outer ring is the exact Circ(12,12,
			// 8.5) display.hdr and display.frame_limiter already use
			// (so it reads as "one of this set's circles", not a new
			// weight of line), and the dot is a FillRect the same
			// 2.5x2.5 size as this glyph's own former bullet, not a
			// circle -- there is no filled-circle op in this set (see
			// Icons.h's IconOp), and a tiny stroked ring for the dot
			// would vanish against the stroked outer ring at 12 px.
			// The stem is a second, taller FillRect rather than a
			// stroked line for the same reason HUD's bars and this
			// glyph's own former bullets are filled: a 1.7-unit stroked
			// line reads as a hairline crack at small sizes, where a
			// filled block of the same width reads as a clean stem.
			//
			// The 2-unit gap between the dot's bottom (9.0) and the
			// stem's top (11.0) is the whole answer to "must the dot
			// and stem stay distinct at small sizes" -- at the 24-unit
			// grid's own default scale that gap is roughly a tenth of
			// the glyph's diameter, wide enough that the two blocks
			// never anti-alias into one blob the way a smaller gap did
			// when first tried. Read against its neighbours: Log (just
			// above) is four ragged rules with no round shape at all,
			// and no other glyph in the set pairs a full outer ring
			// with two small filled blocks on its own centreline.
			Circ( 12.0f, 12.0f, 8.5f ),
			Bar( 10.75f, 6.5f, 13.25f, 9.0f ),
			Bar( 10.75f, 11.0f, 13.25f, 17.0f ) } },

		// ---- SETUP --------------------------------------------------------
		{ "setup.profiles", 2, {
			// Two offset cards -- the "one of several saved copies" mark.
			// The back card is an open path, so the two never draw a
			// doubled edge where they overlap.
			Rect( 3.5f, 7.5f, 16.5f, 20.5f ),
			IconShape{ IconOp::Polyline, 5, 0.0f, { { 7.5f, 7.5f }, { 7.5f, 3.5f },
			                                        { 20.5f, 3.5f }, { 20.5f, 16.5f },
			                                        { 16.5f, 16.5f } } } } },

		{ "setup.appearance", 1, {
			// A droplet -- the colour/paint mark. The only curve in the set
			// that is not a circle, and the reason IconOp::Teardrop exists:
			// see Icons.cpp's Stroke() for the tangent construction.
			IconShape{ IconOp::Teardrop, 2, 6.0f,
				{ { 12.0f, 3.4f }, { 12.0f, 13.7f } } } } },

		{ "setup.cursor", 1, {
			// The pointer itself, as a plain stroked outline -- no fill, no
			// accent colour, no black inlay: the icon set's own single-colour
			// convention (Controls.h picks the stroke colour based on
			// selected/dimmed state) rather than CursorArt_Draw()'s two-tone
			// look, so this behaves like every neighbouring glyph instead of
			// standing out as the one hardcoded to the live accent.
			// kTipX/kFootX/kWingX etc. are CursorArt.h's own corner
			// constants, reused verbatim -- see this file's header comment
			// and CursorArt.h's own -- at the fixed kCursorIconScale/Offset
			// transform above, never gamescope::GetCursorAppearance().flScale.
			IconShape{ IconOp::Loop, 3, 0.0f, {
				{ kCursorIconOffsetX + gamescope::overlay::kTipX  * kCursorIconScale,
				  kCursorIconOffsetY + gamescope::overlay::kTipY  * kCursorIconScale },
				{ kCursorIconOffsetX + gamescope::overlay::kFootX * kCursorIconScale,
				  kCursorIconOffsetY + gamescope::overlay::kFootY * kCursorIconScale },
				{ kCursorIconOffsetX + gamescope::overlay::kWingX * kCursorIconScale,
				  kCursorIconOffsetY + gamescope::overlay::kWingY * kCursorIconScale } } } } },
		};

		constexpr size_t kIconN = sizeof( kIcons ) / sizeof( kIcons[ 0 ] );

		// =================================================================
		//  RAIL GROUPS (I2, 2026-09-27; redrawn I3; regrouped to six, I7)
		// =================================================================
		// Six glyphs, one per RailGroup (Display/Overlay/Input/Misc/
		// Settings/Other, in that order -- see the switch in
		// IconForRailGroup() below, which is what actually binds each entry
		// to its group rather than array position). Freehand, like every
		// icon added after index.html: there is no group tab in the mockup
		// to transcribe, because the mockup predates the accordion
		// entirely.
		//
		// Kept in a SEPARATE table from kIcons[] above -- these are group
		// buttons, not areas, and mixing them in would break
		// test_overlay_ui.cpp's "every registered area has exactly one
		// icon, IconCount() == the area count" bijection.
		//
		// I3 REDRAW: the I2 cut's Misc (three dots) and Settings (a ring
		// with four DETACHED tick marks) read, per the QC pass this
		// implements, as "more/overflow" and "a dim target" respectively --
		// both borrowed an existing UI convention (an overflow ellipsis, a
		// crosshair reticle) for an unrelated meaning, and Settings'
		// silhouette in particular sat close to system.crosshair's own
		// AREA icon (a big ring with four lines through it), which the two
		// can appear on screen together (a closed Settings header beside an
		// open Misc group's Crosshair row). Other's box-with-tab was judged
		// legible enough as a shape but not as "everything left over" --
		// QC's plainer "a card" read was closer to setup.profiles' own two-
		// offset-cards glyph than intended.
		//
		// I7 (2026-09-27, rail regroup): MISC split into three groups
		// (Overlay/Input/Misc -- see Registry.h's own comment on the
		// RailGroup enum for why), so two new silhouettes joined the four
		// above. Every one of the six is distinct from every glyph in
		// kIcons[] and from the other five here:
		//   Display  a monitor on a stand -- one rect with a neck and base
		//            below it, unlike display.resolution's two CONCENTRIC
		//            rects (nothing else in the set draws a rect with legs).
		//            Unchanged since I2 -- QC never flagged it.
		//   Overlay  a frame with a small reticle mark at its centre -- a
		//            LARGE square outline (nearly the whole grid, "the
		//            screen") with a short plain "+" (two crossing lines,
		//            not a ring) sitting inside it, unconnected to the
		//            frame's own edges. Read against system.crosshair's own
		//            AREA icon (a ring with four arms crossing ITS edge)
		//            and against Misc's asterisk below (three DIAGONAL
		//            lines with no frame at all): this is the only glyph in
		//            either table that draws a rectangle enclosing a
		//            separate crossing mark -- "something drawn ON TOP of
		//            the screen" is exactly the Overlay group's own
		//            identity (HUD, Crosshair, Zoom, Cursor).
		//   Input    one wide keycap (a single rounded rectangle) with a
		//            small filled legend mark low on its face -- distinct
		//            from setup.keybinds' AREA icon (a wide body with THREE
		//            outlined caps plus a spacebar, five shapes) and from
		//            system.null_binds' AREA icon (TWO separate square
		//            keycaps joined by a priority chevron, four shapes): no
		//            other glyph in either table is a single keycap shape.
		//   Misc     a six-ray asterisk (three lines crossing through one
		//            centre point) -- the plain "extras/sparkle" mark. No
		//            other glyph in either table draws crossing diameters
		//            with no enclosing frame (Overlay's "+" above always
		//            sits inside its own rect); three SEPARATE same-size
		//            dots (the I2 shape) is the standard overflow-menu
		//            glyph almost everywhere else, which is exactly the
		//            wrong association for a group that opens INLINE, not
		//            into a hidden menu.
		//   Settings a hex-nut: a six-sided outline with a round hole at
		//            its centre -- the bolt-head/mechanism read "settings"
		//            already carries elsewhere in this app's own iconography
		//            (Windows/GNOME "gear" is a hex-nut with teeth
		//            simplified; six shapes was I2's own kIconMaxShapes
		//            ceiling, so this drops the teeth and keeps the
		//            silhouette). Two shapes (one Loop, one Circle) reads
		//            unambiguously as hardware/adjustment, not a target --
		//            no other glyph anywhere in either table draws a
		//            hexagon.
		//   Other    a folder: a small tab rect sitting on a larger body
		//            rect, the plain filesystem "everything else" mark --
		//            distinct from setup.profiles' two FULL-SIZE offset
		//            cards (a folder's tab is a fraction of its body's
		//            width; Profiles' back card is nearly the same size as
		//            the front one) and from system.log's ragged text
		//            lines / system.changelog's info-circle, its two new
		//            group-mates in the icon rail.
		constexpr Icon kGroupIcons[] = {
			{ "railgroup.display", 3, {
				Rect( 3.5f, 4.5f, 20.5f, 15.5f ),
				Line( { 12.0f, 15.5f }, { 12.0f, 18.5f } ),
				Line( { 7.5f, 19.5f }, { 16.5f, 19.5f } ) } },

			{ "railgroup.overlay", 3, {
				Rect( 3.5f, 3.5f, 20.5f, 20.5f ),
				Line( { 8.5f, 12.0f }, { 15.5f, 12.0f } ),
				Line( { 12.0f, 8.5f }, { 12.0f, 15.5f } ) } },

			{ "railgroup.input", 2, {
				RoundRect( 4.5f, 8.0f, 19.5f, 16.0f, 2.5f ),
				Bar( 9.5f, 12.0f, 14.5f, 13.8f ) } },

			{ "railgroup.misc", 3, {
				Line( { 5.0f, 12.0f }, { 19.0f, 12.0f } ),
				Line( { 8.5f, 5.9f },  { 15.5f, 18.1f } ),
				Line( { 15.5f, 5.9f }, { 8.5f, 18.1f } ) } },

			{ "railgroup.settings", 2, {
				IconShape{ IconOp::Loop, 6, 0.0f, {
					{ 12.0f, 3.0f }, { 19.8f, 7.5f }, { 19.8f, 16.5f },
					{ 12.0f, 21.0f }, { 4.2f, 16.5f }, { 4.2f, 7.5f } } },
				Circ( 12.0f, 12.0f, 3.5f ) } },

			{ "railgroup.other", 2, {
				Rect( 3.5f, 5.5f, 10.5f, 8.5f ),
				Rect( 3.5f, 8.5f, 20.5f, 19.5f ) } },
		};
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

	const Icon *IconForRailGroup( RailGroup eGroup )
	{
		switch ( eGroup )
		{
			case RailGroup::Display:  return &kGroupIcons[ 0 ];
			case RailGroup::Overlay:  return &kGroupIcons[ 1 ];
			case RailGroup::Input:    return &kGroupIcons[ 2 ];
			case RailGroup::Misc:     return &kGroupIcons[ 3 ];
			case RailGroup::Settings: return &kGroupIcons[ 4 ];
			case RailGroup::Other:    return &kGroupIcons[ 5 ];
			case RailGroup::Nothing:     return nullptr;
		}
		return nullptr;
	}
}
