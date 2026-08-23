// M8 part 2 (issue #14) -- see Widgets.h for the file-level scope comment.
//
// imgui_internal.h is needed for ButtonBehavior()/ItemAdd()/ItemSize()/
// RenderText()/RenderNavCursor()/MarkItemEdited() -- Toggle()/Checkbox()
// below are deliberately built on the exact same primitives ImGui's own
// stock ImGui::Checkbox() uses (imgui_widgets.cpp), just with different
// geometry/coloring, so keyboard nav activation (Space/Enter/gamepad),
// mouse click, hover/active highlighting, and ImGui::BeginDisabled()
// semantics all come along for free and stay byte-for-byte identical to
// every other ImGui widget in this overlay -- the task brief's "keep the
// widget's behaviour identical to the ImGui original" requirement, taken
// literally by reusing the original's own behavior code rather than
// reimplementing it.
#include "Widgets.h"
#include "Fonts.h"

// ImVec2/ImRect operator+ etc. aren't exported by imgui.h by default (to
// avoid clashing with a host app's own math types) -- every one of ImGui's
// own .cpp files defines this locally, per-translation-unit, before
// including imgui.h/imgui_internal.h (see imgui_widgets.cpp's own top-of-
// file #define of the same name); this file's ButtonBehavior()-based
// widgets below need the same ImVec2 arithmetic Checkbox() itself uses.
// Must come before *any* include of imgui.h in this translation unit
// (imgui_internal.h itself #errors otherwise) -- so Palette.h, which also
// includes imgui.h, is pulled in only after this and the real imgui.h
// include below, not before.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "Palette.h"

#include <cfloat>
#include <cstdio>

namespace gamescope::widgets
{
	void ApplyStyle()
	{
		ImGuiStyle &style = ImGui::GetStyle();

		// ---- Metrics -------------------------------------------------------
		// Design guide "Spacing & layout": window radius 3-4px, control
		// radius 0px (flat) -- deliberately different values, so window-level
		// rounding is #15's call (touched here only because ImGuiStyle has a
		// single shared WindowRounding field with no way for two files to
		// each own half of it; leaving it at the stock-ImGui-adjacent 0.0f a
		// widget-only pass would pick isn't a real choice being made against
		// #15's ownership -- #15 is free to change it).
		style.WindowRounding    = 0.0f;
		style.ChildRounding     = 0.0f;
		style.FrameRounding     = 0.0f; // controls: flat/square, per the guide's hard rule
		style.PopupRounding     = 0.0f;
		style.ScrollbarRounding = 0.0f;
		style.GrabRounding      = 0.0f;
		style.TabRounding       = 0.0f;

		style.WindowBorderSize = 1.0f; // "1px hairline everywhere"
		style.ChildBorderSize  = 1.0f;
		style.FrameBorderSize  = 1.0f;
		style.PopupBorderSize  = 1.0f;

		style.WindowPadding    = ImVec2( 14.0f, 14.0f ); // spec §3: "window padding 14px all sides" -- was 12/10 (gap list item 10)
		style.FramePadding     = ImVec2( 8.0f, 4.0f );
		style.ItemSpacing      = ImVec2( 8.0f, 6.0f );   // guide: control groups 12-13px -- ItemSpacing is the per-row analog
		style.ItemInnerSpacing = ImVec2( 6.0f, 4.0f );   // guide: label->control gap ~5px

		// Feeds ImGui::SliderBehavior()'s own grab-position math (used by
		// SliderControl() below) so the 8px-wide fraction it computes lines
		// up with the spec's 8x18px handle that gets custom-drawn on top of
		// it -- see SliderControl()'s comment for the full slider story.
		style.GrabMinSize = 8.0f;

		// Disabled controls: guide's "Sliders" section calls the 34%-opacity
		// treatment "the standard 'control present but currently inert'
		// treatment, reusable anywhere" -- DisabledAlpha is exactly that,
		// applied globally via ImGui::BeginDisabled(), which every panel
		// already wraps its inert controls in (PanelDisplay's Sharpness
		// slider under Linear/Nearest/Pixel, PanelAudio's Volume/Mute when
		// no stream is detected, PanelShaders under HDR).
		style.DisabledAlpha = 0.34f;

		// ---- Palette ---------------------------------------------------
		// oklch(.74 .12 218) (cyan accent) and its siblings, converted to
		// sRGB once and pixel-verified against the rendered mockup -- see
		// superdoc/planning/ui-mockup-precise-spec.md §1's Color tokens
		// table (Palette.h carries the same numbers for Chrome.cpp/
		// FpsDisplay.cpp so all three files stay byte-identical instead of
		// three independent hex transcriptions -- see the spec's gap list
		// item 1 for what happens when they drift).
		const ImVec4 accent      = ImVec4( 0x36 / 255.0f, 0xbd / 255.0f, 0xdd / 255.0f, 1.00f );
		const ImVec4 accentHi    = ImVec4( 0xba / 255.0f, 0xe7 / 255.0f, 0xf4 / 255.0f, 1.00f ); // accent-handle -- brightest accent tone, handles/knobs
		const ImVec4 accentSoft  = ImVec4( accent.x, accent.y, accent.z, 0.22f );                // active-state fills, 12-24% alpha band
		const ImVec4 surface     = ImVec4( 0x09 / 255.0f, 0x0a / 255.0f, 0x0c / 255.0f, 0.88f );
		const ImVec4 raised      = ImVec4( 1.00f, 1.00f, 1.00f, 0.05f );
		const ImVec4 hairline    = ImVec4( 1.00f, 1.00f, 1.00f, 0.10f );
		const ImVec4 text        = ImVec4( 0.92f, 0.94f, 0.95f, 1.00f );
		const ImVec4 textDim     = ImVec4( 0.92f, 0.94f, 0.95f, 0.50f );
		const ImVec4 transparent = ImVec4( 0.0f, 0.0f, 0.0f, 0.0f );

		ImVec4 *colors = style.Colors;
		colors[ImGuiCol_Text]              = text;
		colors[ImGuiCol_TextDisabled]      = textDim;
		colors[ImGuiCol_WindowBg]          = surface;
		colors[ImGuiCol_ChildBg]           = transparent;
		colors[ImGuiCol_PopupBg]           = surface; // combo/popup panel chrome: same glass surface, per the guide's window/panel chrome section
		colors[ImGuiCol_Border]            = hairline;
		colors[ImGuiCol_BorderShadow]      = transparent;

		// Frame widgets (sliders, combos, text inputs): "flat-hairline-box"
		// per the guide's Text inputs section, explicitly named as the
		// closest analog for every uncovered flat-box control too.
		colors[ImGuiCol_FrameBg]           = raised;
		colors[ImGuiCol_FrameBgHovered]    = accentSoft;
		colors[ImGuiCol_FrameBgActive]     = accentSoft;

		colors[ImGuiCol_TitleBg]           = raised;
		colors[ImGuiCol_TitleBgActive]     = raised;
		colors[ImGuiCol_TitleBgCollapsed]  = raised;

		colors[ImGuiCol_CheckMark]         = accent;
		colors[ImGuiCol_SliderGrab]        = accent;
		colors[ImGuiCol_SliderGrabActive]  = accentHi;

		// Buttons: the guide has no standalone button in the handoff and
		// says to extrapolate from segmented-control cell styling (flat
		// rect, hairline border, accent fill when active) -- see the
		// Buttons section's explicit "this is an extrapolation" flag.
		colors[ImGuiCol_Button]            = raised;
		colors[ImGuiCol_ButtonHovered]     = accentSoft;
		colors[ImGuiCol_ButtonActive]      = accent;

		// Header: drives ImGui::Selectable()'s hover/active fill, which is
		// what every combo popup's option list uses internally (PanelConfig's
		// profile/copy-game pickers) -- accent-fill-when-active matches the
		// guide's segmented-control "active segment" language, the closest
		// captured analog for a selected list row.
		colors[ImGuiCol_Header]            = accentSoft;
		colors[ImGuiCol_HeaderHovered]     = accentSoft;
		colors[ImGuiCol_HeaderActive]      = accent;

		colors[ImGuiCol_Separator]         = hairline; // guide: "1px horizontal rule, rgba(255,255,255,.06-.07)"
		colors[ImGuiCol_SeparatorHovered]  = hairline;
		colors[ImGuiCol_SeparatorActive]   = accent;

		colors[ImGuiCol_ResizeGrip]        = accentSoft;
		colors[ImGuiCol_ResizeGripHovered] = accent;
		colors[ImGuiCol_ResizeGripActive]  = accent;

		// Scrollbars: guide says "not present ... undefined by the handoff,
		// will need ImGui default styling reskinned to match the hairline/
		// flat language if any window ends up needing to scroll" -- no
		// panel currently scrolls, but style this now (cheap, style-only)
		// so a future scrolling panel isn't stuck with ImGui's default grey.
		colors[ImGuiCol_ScrollbarBg]        = transparent;
		colors[ImGuiCol_ScrollbarGrab]      = raised;
		colors[ImGuiCol_ScrollbarGrabHovered] = accentSoft;
		colors[ImGuiCol_ScrollbarGrabActive]  = accent;

		// Tabs: not currently used by any panel (each panel is its own
		// top-level window, no tab strips), styled anyway for the same
		// forward-looking reason as scrollbars above.
		colors[ImGuiCol_Tab]                = raised;
		colors[ImGuiCol_TabHovered]         = accentSoft;
		colors[ImGuiCol_TabSelected]        = accentSoft;
		colors[ImGuiCol_TabSelectedOverline] = accent;

		// Tooltips: "solid panel rgba(6,8,10,.94), 1px rgba(255,255,255,.12)
		// border, no blur" per the guide's Tooltips section -- PopupBg above
		// already covers ImGui's tooltip window (ImGui::SetTooltip() reuses
		// the popup/tooltip window path, which draws WindowBg + Border), so
		// no separate ImGuiCol_* exists to override here; the guide's tooltip
		// values are close enough to `surface`/`hairline` that no distinct
		// override is needed.
	}

}
