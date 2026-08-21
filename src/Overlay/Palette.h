// Shared color tokens for the settings overlay's chrome (Chrome.cpp) and
// widget drawing (Widgets.cpp), plus anything else that needs "the accent"
// or one of its derived tones (FpsDisplay.cpp).
//
// Source: superdoc/planning/ui-mockup-precise-spec.md §1 ("Color tokens
// (canonical)") -- every hex value below is that file's own oklch-derived,
// pixel-sampling-verified sRGB conversion, copied here once so every caller
// references the same numbers instead of re-deriving/re-typing them (the
// prior state of this codebase had the accent hex baked separately into
// Widgets.cpp and Chrome.cpp, which is exactly how it drifted to the wrong
// value -- see the spec's gap list item 1).
//
// Internal-only header (not part of the widgets::/chrome:: public API
// surface) -- included only by .cpp files that already pull in imgui.h.
#pragma once

#include "imgui.h"

namespace gamescope::palette
{
	// ---- Accent family (oklch(.74 .12 218) base = #36BDDD) ----------------
	constexpr ImU32 kAccent        = IM_COL32( 0x36, 0xBD, 0xDD, 255 ); // status dot, slider fill lo-end, active borders, dock top-edge base
	constexpr ImU32 kAccentEdge    = IM_COL32( 0x4F, 0xD0, 0xF1, 255 ); // dock active 2px top edge
	constexpr ImU32 kAccentGradHi  = IM_COL32( 0x47, 0xCA, 0xEA, 255 ); // slider fill gradient end (right/handle end)
	constexpr ImU32 kAccentValue   = IM_COL32( 0x78, 0xDB, 0xF6, 255 ); // numeric value readouts
	constexpr ImU32 kAccentText    = IM_COL32( 0x71, 0xD4, 0xEF, 255 ); // chip text, link-style labels
	constexpr ImU32 kAccentSegText = IM_COL32( 0xA9, 0xEA, 0xFD, 255 ); // active segmented-control label
	constexpr ImU32 kAccentKnob    = IM_COL32( 0x93, 0xDE, 0xF4, 255 ); // toggle knob, checkbox inner mark
	constexpr ImU32 kAccentIcon    = IM_COL32( 0xA3, 0xE3, 0xF6, 255 ); // active dock icon strokes
	constexpr ImU32 kAccentHandle  = IM_COL32( 0xBA, 0xE7, 0xF4, 255 ); // slider handle fill
	constexpr ImU32 kAccentLinkDim = IM_COL32( 0x6B, 0xCD, 0xE9, 255 ); // footer action text ("live", title-bar meta)

	// ---- Neutrals (alpha layers -- alpha is load-bearing) -----------------
	constexpr ImU32 kSurfaceRgb    = IM_COL32( 0x09, 0x0A, 0x0C, 255 ); // window/panel/dock base color, .88/.86 alpha applied by callers
	constexpr ImU32 kTextRgb       = IM_COL32( 0xEF, 0xF5, 0xFB, 255 ); // near-white text base

	// Accent at an arbitrary alpha (0-1), as ImU32 -- covers the many
	// alpha steps §1 lists (.13 dock fill, .16 chip fill, .2 checkbox
	// fill, .24 active segment fill, .3 toggle track, .42 focused border,
	// .5 dock active border, .6 active segment border, .65 toggle border,
	// .8 active-group left edge / handle glow) without a named constant
	// per step.
	constexpr ImU32 Accent( float flAlpha )
	{
		return IM_COL32( 0x36, 0xBD, 0xDD, (int)( flAlpha * 255.0f + 0.5f ) );
	}

	constexpr ImU32 White( float flAlpha )
	{
		return IM_COL32( 255, 255, 255, (int)( flAlpha * 255.0f + 0.5f ) );
	}

	// kTextRgb at an arbitrary alpha -- the near-white text base (§1: "text
	// tints ... implement as one base #EFF5FB").
	constexpr ImU32 Text( float flAlpha )
	{
		return IM_COL32( 0xEF, 0xF5, 0xFB, (int)( flAlpha * 255.0f + 0.5f ) );
	}

	constexpr ImU32 Black( float flAlpha )
	{
		return IM_COL32( 0, 0, 0, (int)( flAlpha * 255.0f + 0.5f ) );
	}

	// surface (§1: rgba(9,10,12,.88)) as an ImVec4, for ImGuiStyle.Colors[]
	// slots that want a float color rather than a packed ImU32.
	inline ImVec4 SurfaceVec4( float flAlpha = 0.88f )
	{
		return ImVec4( 0x09 / 255.0f, 0x0A / 255.0f, 0x0C / 255.0f, flAlpha );
	}

	// Any packed ImU32 as an ImVec4, optionally overriding its alpha -- for
	// ImGuiStyle.Colors[]/PushStyleColor() slots that want a float color.
	inline ImVec4 ToVec4( ImU32 col, float flAlphaOverride = -1.0f )
	{
		ImVec4 v = ImGui::ColorConvertU32ToFloat4( col );
		if ( flAlphaOverride >= 0.0f )
			v.w = flAlphaOverride;
		return v;
	}
}
