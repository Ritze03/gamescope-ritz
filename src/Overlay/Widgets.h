// M8 part 2 (issue #14): the settings overlay's shared ImGuiStyle setup.
// superdoc/planning/ui-design-guide.md's "Component styling" section is the
// authority for every value used here.
//
// WHAT THIS FILE USED TO BE. It also owned a custom widget set -- a toggle
// switch, a checkbox, two sliders, a segmented control, a 3x3 position grid,
// a readout strip and a group block -- drawn with ImDrawList because the
// design guide flagged them as unreachable from ImGuiStyle alone. Every one
// of them was a legacy-panel control, and P5 deleted the panels; the E2
// shell draws its own controls through Overlay/UI/Controls.cpp against the
// Row grammar, and never called any of these. ApplyStyle() is what survives,
// because the shell's own ImGui context still needs a styled baseline for
// the stock primitives it does use.
//
#pragma once

namespace gamescope::widgets
{
	// Applies the "glass instrument" palette + metrics from the design
	// guide's Component styling section to the *currently current* ImGui
	// context's ImGuiStyle: flat/square corners, 1px hairline borders, cyan
	// accent, near-black translucent surfaces, and every ImGuiCol_* the
	// guide's stock-widget rows call out. Must run after ImGui::CreateContext()
	// and before the first frame is drawn, same contract as
	// gamescope::fonts::Load() (see Fonts.h) -- SettingsOverlay.cpp's
	// EnsureImguiInit() calls both, in either order (they touch disjoint
	// state: this touches ImGuiStyle, Fonts::Load() touches the font atlas).
	void ApplyStyle();

}
