// M8 part 2 (issue #14): custom widget rendering for the settings overlay --
// superdoc/planning/ui-design-guide.md's "Component styling" section is the
// authority for every value used here.
//
// Two routes, used as the design guide's own "ImGui feasibility notes" table
// says to: ImGuiStyle configuration wherever that alone gets the look (most
// of it -- panel chrome, buttons, sliders, combos, text inputs, separators,
// list rows), and a small amount of custom ImDrawList drawing only for the
// handful of widgets the guide explicitly flags as unreachable from style
// alone (the toggle switch's always-accent track + sliding square knob, and
// the checkbox's checked/unchecked-dependent border+fill with a filled-
// square mark instead of a checkmark glyph -- neither can be expressed by
// ImGuiStyle, which only varies color by hover/active, not by the widget's
// own boolean value).
//
// Scope boundary (see the task brief / issue #15's sibling worker): this
// file owns widget-level styling and drawing only -- buttons, sliders,
// checkboxes/toggles, combos, text inputs, separators, list rows, and the
// shared ImGuiStyle color/metric setup. Window/panel chrome (title bars,
// borders, the accent left-edge marking a live group) belongs to #15 and is
// never touched here.
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

	// Design guide's "Toggles (switches)" component: a 30x15px square-cornered
	// track that renders in accent color *regardless of on/off state* (only
	// the 11x11 knob's position -- left/right -- carries the value; see the
	// design guide's Toggles section and its open question 6 for why "off"
	// has no distinct captured color) plus a label, matching
	// ImGui::Checkbox's call signature/return value (true the frame the value
	// changed) and its interaction/keyboard/disabled semantics exactly
	// (built on the same ButtonBehavior()/ItemAdd() primitives Checkbox
	// itself uses -- see the .cpp for why that's what "identical behavior"
	// means here). Used for this overlay's general settings on/off controls
	// (Display/Audio/Shaders/Config panels).
	bool Toggle( const char *pszLabel, bool *pbValue );

	// Design guide's "Checkboxes" component: a 12x12px box whose border and
	// fill genuinely depend on *pbValue (accent border/fill when checked,
	// dim hairline when not) -- not just hover/active state, which is why
	// this can't be done via ImGuiStyle's ImGuiCol_Checkmark/FrameBg alone --
	// plus a centered 5x5 *filled square* mark (not ImGui's stock checkmark
	// glyph) when checked. Same call signature/behavior contract as Toggle()
	// above. Used specifically for this overlay's FPS-HUD settings row list
	// (FpsDisplay.cpp), matching the design guide's own "List rows" section,
	// which names exactly that control ("FPS HUD's row toggles: 11x11
	// checkbox + label") as the checkbox-style repeating row pattern.
	bool Checkbox( const char *pszLabel, bool *pbValue );

	// Design guide's typography "hard rule": never mix a number into a sans
	// run -- every numeric readout is Plex Mono, tabular, and (per the
	// guide's Sliders section) usually accent-colored. Stock
	// ImGui::SliderFloat/SliderInt bake their live value into the *same*
	// draw call as the trailing label, in whatever font is active for the
	// whole widget (see Fonts.h's Style::Value comment) -- there is no way
	// to give the label Sans and the value Mono from ImGuiStyle/PushFont
	// alone. These wrap the stock slider with format="" (suppressing its
	// own in-box text) and redraw just the numeric value on top, centered
	// in the same box, in Style::Value + accent -- same geometry/behavior
	// as ImGui::SliderFloat/SliderInt (drag, click, keyboard, scroll all
	// still come from the stock widget), only the value's font/color differ.
	// pszValueFormat formats the value alone (e.g. "%.2f", "%d%%"), not the
	// whole slider line -- unlike ImGui's own `format`, which historically
	// also had to contain the widget's %-spec placement quirks.
	// nFlags forwards straight to the underlying ImGui::SliderFloat/SliderInt
	// (e.g. ImGuiSliderFlags_AlwaysClamp) -- typed int here so this header
	// doesn't need to include imgui.h, same reasoning as DrawIcon()'s ImU32
	// below.
	bool SliderFloat( const char *pszLabel, float *pflValue, float flMin, float flMax, const char *pszValueFormat = "%.2f", int nFlags = 0 );
	bool SliderInt( const char *pszLabel, int *pnValue, int nMin, int nMax, const char *pszValueFormat = "%d", int nFlags = 0 );

	// Design guide's "Tabs / segmented controls" component -- explicitly
	// named for "mode pickers like filter type" (ui-design-guide.md), which
	// is exactly PanelDisplay's Filter/Scaler rows. Replaces stock
	// ImGui::RadioButton (an inherently circular widget with no flat/square
	// equivalent reachable via ImGuiStyle) with a row of equal-width flat
	// segments: hairline border + dim fill + Mono 500 dim text when
	// inactive, accent fill/border + Mono 600 bright-accent text when
	// active. `nCount` options starting at pszLabels[0]; returns true (and
	// updates *pnSelected) the frame a different segment is clicked.
	bool SegmentedControl( const char *pszId, int *pnSelected, const char *const *pszLabels, int nCount );
}
