// SPEC.md §8.0's rail icon set -- eleven stroked glyphs on one 24-unit grid.
//
// WHY THIS FILE EXISTS AT ALL. The rail drew the area title's first character
// until now, and three of the eleven pairs collided the moment the ladder
// collapsed the rail to icons at 1.5x: Mixer/Monitor are both `M`,
// Profiles/Per-game both `P`, Shaders/Shell both `S`. In the collapsed rail the
// label is gone, so the mark carries the entire meaning of the item -- a letter
// that three areas share is not an identifier, and the pre-P5 shell test found
// exactly that (SHELL-TEST-REPORT §7.1).
//
// WHY DRAWN AND NOT BAKED. The same argument Controls.h's `glyph` namespace
// records for the chevron and the lock, and it is stronger here: there is no
// character to bake. These are not typographic marks with a Unicode code point
// that a wider font range could reach -- a bar chart and a droplet are drawings.
// Baking them would mean shipping an icon font, which SPEC §8.0 forbids in the
// same breath as the external asset ("no icon font and no external file"), and
// the atlas is rebuilt per effective scale, so every baked range is paid again
// at every scale change.
//
// WHY THE DATA IS IMGUI-FREE. Same reason Lane.h and Layout.h are: the
// geometry is the part worth testing, and a test should not need a graphics
// context to ask whether all eleven glyphs exist, stay inside their box, and
// differ from one another. Icons.cpp turns a shape into ImDrawList calls and
// contains no coordinates of its own.
//
// PROVENANCE. Every coordinate below is transcribed from index.html's `ICONS`
// table -- the inline SVG the mockup draws, on the same `viewBox="0 0 24 24"`.
// One glyph has no mockup original: this build's twelfth area is
// `display.general`, which predates neither SPEC nor index.html and so has
// no source to transcribe from. That one is noted where it is declared.
//
// A second, differently-sourced exception: `setup.cursor`'s glyph is not
// transcribed OR freehand -- it reuses Overlay/CursorArt.h's own
// kTipX/kFootY/kWingX triangle constants directly, at a fixed icon-local
// scale, so the rail icon and the actual pointer can never draw two
// different triangles. See that entry's own comment in Icons.cpp.
#pragma once

#include <cstddef>

namespace gamescope::ui
{
	// A point on the 24-unit grid. Deliberately not ImVec2 -- see the header
	// comment for why this file links no ImGui.
	struct IconPt
	{
		float x = 0.0f, y = 0.0f;
	};

	// SPEC §8.0: "Fill is used only where a fill carries meaning -- HDR's
	// half-filled disc, the Monitor's bar chart." That sentence is the whole
	// reason this enum has both stroked and filled members: a fill is a
	// deliberate, licensed exception here, not a drawing convenience.
	enum class IconOp : unsigned char
	{
		Polyline,   // open stroked path through nPoints points
		Loop,       // closed stroked path (a rectangle is four points)
		Circle,     // stroked; pt[0] is the centre, flRadius the radius
		FillRect,   // filled; pt[0] is the min corner, pt[1] the max
		FillPoly,   // filled closed path through nPoints points
		HalfDisc,   // filled right half; pt[0] centre, flRadius radius -- HDR
		Teardrop,   // stroked; pt[0] apex, pt[1] centre, flRadius radius
	};

	// Six points is the most any glyph below needs (Profiles' back card). A
	// fixed array rather than a pointer keeps the whole set constexpr and
	// keeps the data in one contiguous block the compiler can fold.
	inline constexpr size_t kIconMaxPts = 6;

	struct IconShape
	{
		IconOp  eOp       = IconOp::Polyline;
		size_t  nPoints   = 0;
		float   flRadius  = 0.0f;
		IconPt  pts[ kIconMaxPts ] {};
	};

	// Six shapes is the most any glyph below needs (Mixer's two faders).
	inline constexpr size_t kIconMaxShapes = 6;

	struct Icon
	{
		const char *pszKey  = nullptr;   // the area id this glyph belongs to
		size_t      nShapes = 0;
		IconShape   shapes[ kIconMaxShapes ] {};
	};

	// The set, in rail order. Exposed as a span-like pair rather than a
	// std::vector so it stays a compile-time table with no initialisation
	// order to reason about.
	const Icon *IconSet();
	size_t      IconCount();

	// The glyph for an area id, or nullptr when the id has none.
	//
	// A MISSING ICON IS NOT AN ERROR HERE, on purpose: a new area registered
	// tomorrow must not disappear from the rail because nobody drew it a
	// glyph yet. DrawRail falls back to the area's initial, which is exactly
	// what the whole rail did before this file existed -- so the failure mode
	// of a forgotten icon is the old behaviour for one item, not a blank rail.
	const Icon *IconFor( const char *pszAreaId );

	// SPEC §8.0's grid. Every coordinate above is in [0, 24].
	inline constexpr float kIconGrid   = 24.0f;
	// "stroke 1.7" -- on the 24-unit grid, so it scales with the box.
	inline constexpr float kIconStroke = 1.7f;
}
