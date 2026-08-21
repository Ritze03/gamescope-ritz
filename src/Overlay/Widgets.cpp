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

// ImVec2/ImRect operator+ etc. aren't exported by imgui.h by default (to
// avoid clashing with a host app's own math types) -- every one of ImGui's
// own .cpp files defines this locally, per-translation-unit, before
// including imgui.h/imgui_internal.h (see imgui_widgets.cpp's own top-of-
// file #define of the same name); this file's ButtonBehavior()-based
// widgets below need the same ImVec2 arithmetic Checkbox() itself uses.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"

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

		style.WindowPadding    = ImVec2( 12.0f, 10.0f ); // guide: window padding 14px, close enough (#15 also has a say via chrome)
		style.FramePadding     = ImVec2( 8.0f, 4.0f );
		style.ItemSpacing      = ImVec2( 8.0f, 6.0f );   // guide: control groups 12-13px -- ItemSpacing is the per-row analog
		style.ItemInnerSpacing = ImVec2( 6.0f, 4.0f );   // guide: label->control gap ~5px

		// Slider grab: the guide's 8x18px fixed rectangular handle can't come
		// from style alone (ImGui only exposes grab *width* via GrabMinSize;
		// grab height always equals the frame height) -- GrabMinSize gets us
		// the width, which is the achievable part; see the ponytail note
		// below for why the full geometry isn't custom-drawn on top of that.
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
		// sRGB once per the design guide's own note that ImGui needs plain
		// RGBA -- see ui-design-guide.md's Color palette table for the
		// source values this was converted from.
		const ImVec4 accent      = ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, 1.00f );
		const ImVec4 accentHi    = ImVec4( 0xcf / 255.0f, 0xef / 255.0f, 0xf7 / 255.0f, 1.00f ); // "brightest accent tone" -- handles/knobs
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

	// ponytail: the design guide's slider look (a thin 5px track separate
	// from a taller frame, an accent/.5->accent gradient fill, and a fixed
	// 8x18px rectangular handle) genuinely can't come entirely from
	// ImGuiStyle -- but a stock ImGui::SliderInt/SliderFloat styled via
	// ApplyStyle() above (flat FrameBg track, 0px rounding, accent grab,
	// GrabMinSize=8) already reads as "flat, square, accent-highlighted
	// slider," which is the design's actual intent; only the exact track/
	// handle proportions differ. The task brief calls a hand-rolled widget
	// that looks like a styled stock one "pure liability" -- writing ~40
	// lines of ImDrawList code to move the handle from "full-frame-height
	// rect" to "8x18 rect" for a purely cosmetic difference isn't worth that
	// liability. Sliders are therefore left as ImGui::SliderInt/SliderFloat,
	// style-only, everywhere in this overlay.

	// Shared implementation for Toggle()/Checkbox() below: both are ImGui
	// items built on ButtonBehavior()/ItemAdd(), differing only in the
	// geometry/colors they draw -- see Widgets.h's per-function comments for
	// what each represents design-wise.
	namespace
	{
		bool BooleanControl( const char *pszLabel, bool *pbValue, ImVec2 controlSize,
			void ( *DrawFn )( ImDrawList *pDrawList, const ImRect &controlBB, bool bValue, bool bHovered, bool bHeld ) )
		{
			ImGuiWindow *pWindow = ImGui::GetCurrentWindow();
			if ( pWindow->SkipItems )
				return false;

			ImGuiContext &g = *GImGui;
			const ImGuiStyle &style = g.Style;
			const ImGuiID id = pWindow->GetID( pszLabel );
			const char *pszLabelEnd = ImGui::FindRenderedTextEnd( pszLabel );
			const ImVec2 labelSize = ImGui::CalcTextSize( pszLabel, pszLabelEnd, false );

			const ImVec2 pos = pWindow->DC.CursorPos;
			const ImRect controlBB( pos, pos + controlSize );
			const ImRect totalBB( pos, pos + ImVec2(
				controlSize.x + ( labelSize.x > 0.0f ? style.ItemInnerSpacing.x + labelSize.x : 0.0f ),
				ImMax( controlSize.y, labelSize.y ) ) );

			ImGui::ItemSize( totalBB, ( controlSize.y - labelSize.y ) * 0.5f > 0.0f ? ( controlSize.y - labelSize.y ) * 0.5f : 0.0f );
			if ( !ImGui::ItemAdd( totalBB, id ) )
				return false;

			bool bHovered, bHeld;
			bool bPressed = ImGui::ButtonBehavior( totalBB, id, &bHovered, &bHeld );
			if ( bPressed )
			{
				*pbValue = !*pbValue;
				ImGui::MarkItemEdited( id );
			}

			ImGui::RenderNavCursor( totalBB, id );
			DrawFn( pWindow->DrawList, controlBB, *pbValue, bHovered, bHeld );

			if ( labelSize.x > 0.0f )
			{
				const ImVec2 labelPos( controlBB.Max.x + style.ItemInnerSpacing.x,
					controlBB.Min.y + ( controlSize.y - labelSize.y ) * 0.5f );
				ImGui::RenderText( labelPos, pszLabel, pszLabelEnd, false );
			}

			return bPressed;
		}
	}

	bool Toggle( const char *pszLabel, bool *pbValue )
	{
		// 30x15px, per the design guide's Toggles section ("30x15 is the
		// canonical size per 2b").
		static constexpr ImVec2 kTrackSize( 30.0f, 15.0f );
		static constexpr float kKnobSize = 11.0f;
		static constexpr float kInset = 1.0f; // "1px inset padding"

		return BooleanControl( pszLabel, pbValue, kTrackSize,
			[]( ImDrawList *pDrawList, const ImRect &bb, bool bValue, bool bHovered, bool /*bHeld*/ )
			{
				// Track: always accent-tinted regardless of on/off (the
				// design guide's own stated rule -- see Widgets.h's Toggle()
				// comment and DECISIONS.md/the guide's open question 6 for
				// why "off" has no distinct captured color). Hover nudges
				// the alpha up slightly so the control still reads as
				// interactive, without inventing an "off" palette the
				// source material never specified.
				const float flTrackAlpha = bHovered ? 0.4f : 0.3f;
				const ImU32 trackFill   = ImGui::GetColorU32( ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, flTrackAlpha ) );
				const ImU32 trackBorder = ImGui::GetColorU32( ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, 0.65f ) );
				const ImU32 knobFill    = ImGui::GetColorU32( ImVec4( 0xcf / 255.0f, 0xef / 255.0f, 0xf7 / 255.0f, 1.00f ) );

				pDrawList->AddRectFilled( bb.Min, bb.Max, trackFill ); // 0px radius -- "no radius (square)"
				pDrawList->AddRect( bb.Min, bb.Max, trackBorder, 0.0f, 0, 1.0f );

				const float flKnobX = bValue
					? ( bb.Max.x - kInset - kKnobSize )
					: ( bb.Min.x + kInset );
				const float flKnobY = bb.Min.y + ( bb.GetHeight() - kKnobSize ) * 0.5f;
				const ImVec2 knobMin( flKnobX, flKnobY );
				const ImVec2 knobMax( flKnobX + kKnobSize, flKnobY + kKnobSize );
				pDrawList->AddRectFilled( knobMin, knobMax, knobFill );
			} );
	}

	bool Checkbox( const char *pszLabel, bool *pbValue )
	{
		// 12x12px, per the design guide's Checkboxes section.
		static constexpr ImVec2 kBoxSize( 12.0f, 12.0f );
		static constexpr float kMarkSize = 5.0f;

		return BooleanControl( pszLabel, pbValue, kBoxSize,
			[]( ImDrawList *pDrawList, const ImRect &bb, bool bValue, bool bHovered, bool /*bHeld*/ )
			{
				ImU32 fill, border;
				if ( bValue )
				{
					fill   = ImGui::GetColorU32( ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, bHovered ? 0.28f : 0.20f ) );
					border = ImGui::GetColorU32( ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, 0.70f ) );
				}
				else
				{
					fill   = ImGui::GetColorU32( ImVec4( 1.0f, 1.0f, 1.0f, bHovered ? 0.07f : 0.04f ) );
					border = ImGui::GetColorU32( ImVec4( 1.0f, 1.0f, 1.0f, 0.18f ) );
				}

				pDrawList->AddRectFilled( bb.Min, bb.Max, fill );
				pDrawList->AddRect( bb.Min, bb.Max, border, 0.0f, 0, 1.0f );

				if ( bValue )
				{
					// "a filled square (not a checkmark glyph)", centered.
					const ImVec2 markMin(
						bb.Min.x + ( bb.GetWidth() - kMarkSize ) * 0.5f,
						bb.Min.y + ( bb.GetHeight() - kMarkSize ) * 0.5f );
					const ImVec2 markMax = markMin + ImVec2( kMarkSize, kMarkSize );
					const ImU32 markFill = ImGui::GetColorU32( ImVec4( 0xcf / 255.0f, 0xef / 255.0f, 0xf7 / 255.0f, 1.00f ) );
					pDrawList->AddRectFilled( markMin, markMax, markFill );
				}
			} );
	}
}
