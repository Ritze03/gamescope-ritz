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

	// Slider: superdoc/planning/ui-mockup-precise-spec.md §7 measures a
	// track/handle geometry a styled stock ImGui::SliderInt/SliderFloat
	// genuinely cannot reach (a 5px track separate from an 18px hit-height,
	// an accent-gradient fill, and a fixed 8x18px handle with a glow, versus
	// stock's one full-height frame + grab) -- an earlier pass here left
	// sliders as style-only stock widgets on the theory that the remaining
	// gap was cosmetic; the spec's own pixel comparison (its gap list item
	// 2) says otherwise ("value text *inside* the bar" reads as a
	// fundamentally different control, not a close approximation). This
	// implementation keeps the *behavior* byte-for-byte stock by reusing
	// ImGui's own internal primitives move-for-move -- ItemAdd(),
	// ItemHoverable(), SliderBehavior() (the drag/keyboard/gamepad math),
	// and TempInputScalar() (ctrl+click / Enter-to-type) are all the exact
	// functions ImGui::SliderScalar() itself calls (imgui_widgets.cpp) --
	// and only replaces the final draw call with the spec's geometry, the
	// same "reuse the primitive, replace the paint" shape Toggle()/
	// Checkbox() above already use via ButtonBehavior().
	namespace
	{
		// Shared by SliderFloat()/SliderInt() below -- one generic scalar
		// implementation, matching how ImGui::SliderFloat/SliderInt
		// themselves both funnel into ImGui::SliderScalar(). pszValueText/
		// pszMinText/pszMaxText are pre-formatted by the caller (each knows
		// its own %-spec type; %f vs %d can't be branched on generically
		// here without duplicating a printf parser).
		bool SliderControl( const char *pszLabel, ImGuiDataType eDataType, void *pValue,
			const void *pMin, const void *pMax, const char *pszFormat, ImGuiSliderFlags nFlags,
			const char *pszValueText, const char *pszMinText, const char *pszMaxText )
		{
			ImGuiWindow *pWindow = ImGui::GetCurrentWindow();
			if ( pWindow->SkipItems )
				return false;

			ImGuiContext &g = *GImGui;
			const ImGuiStyle &style = g.Style;
			const ImGuiID id = pWindow->GetID( pszLabel );
			const char *pszLabelEnd = ImGui::FindRenderedTextEnd( pszLabel );

			// Deliberately *not* ImGui::CalcItemWidth(): with no caller
			// override that falls back to window->DC.ItemWidth, whose
			// un-pushed default is window->Size.x * 0.65f -- a heuristic
			// imgui.cpp itself sizes for the classic "frame + trailing
			// label" stock-widget row, which this custom widget doesn't
			// draw (its label lives on its own line above the track, see
			// below). Every caller inherited that 65% cap with nothing to
			// its right filling the rest, i.e. exactly the reported "sliders
			// are not spanning the full width". Spec §7's Slider entry
			// itself says "Track: full width", so default to the row's full
			// available width -- while still honoring an explicit
			// ImGui::SetNextItemWidth()/negative-width caller override via
			// CalcItemWidth(), same as every other ImGui item.
			const float flWidth = ( g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth )
				? ImGui::CalcItemWidth()
				: ImGui::GetContentRegionAvail().x;

			// ---- Layout: label+value line, 5px gap, 18px track hit-row,
			// 5px gap, min/max line -- spec §3/§7. ----
			ImGui::PushFont( fonts::Get( fonts::Style::Label ) );
			const ImVec2 labelSize = ImGui::CalcTextSize( pszLabel, pszLabelEnd, false );
			ImGui::PopFont();
			ImGui::PushFont( fonts::Get( fonts::Style::Value ) );
			const ImVec2 valueSize = ImGui::CalcTextSize( pszValueText );
			ImGui::PopFont();
			ImGui::PushFont( fonts::Get( fonts::Style::ScaleMark ) );
			const ImVec2 minTextSize = pszMinText ? ImGui::CalcTextSize( pszMinText ) : ImVec2( 0.0f, 0.0f );
			const ImVec2 maxTextSize = pszMaxText ? ImGui::CalcTextSize( pszMaxText ) : ImVec2( 0.0f, 0.0f );
			const float flMarkRowH = ImMax( minTextSize.y, maxTextSize.y );
			ImGui::PopFont();

			constexpr float kLabelTrackGap = 5.0f;
			constexpr float kTrackMarkGap = 5.0f;
			constexpr float kHitHeight = 18.0f; // "row hit-height 18px"
			constexpr float kTrackHeight = 5.0f;
			constexpr float kTrackRounding = 3.0f;
			constexpr float kHandleW = 8.0f;
			constexpr float kHandleH = 18.0f;

			const float flLabelRowH = ImMax( labelSize.y, valueSize.y );
			const bool bHasMarks = pszMinText != nullptr || pszMaxText != nullptr;

			const ImVec2 pos = pWindow->DC.CursorPos;
			const float flTrackTop = pos.y + flLabelRowH + kLabelTrackGap;
			const ImRect trackHitBB( ImVec2( pos.x, flTrackTop ), ImVec2( pos.x + flWidth, flTrackTop + kHitHeight ) );

			float flTotalH = ( flTrackTop - pos.y ) + kHitHeight;
			if ( bHasMarks )
				flTotalH += kTrackMarkGap + flMarkRowH;
			const ImRect totalBB( pos, ImVec2( pos.x + flWidth, pos.y + flTotalH ) );

			// ---- Item registration + interaction: verbatim
			// ImGui::SliderScalar() shape (imgui_widgets.cpp), just against
			// trackHitBB instead of a single frame_bb. ----
			const bool bTempInputAllowed = ( nFlags & ImGuiSliderFlags_NoInput ) == 0;
			ImGui::ItemSize( totalBB, style.FramePadding.y );
			if ( !ImGui::ItemAdd( totalBB, id, &trackHitBB, bTempInputAllowed ? ImGuiItemFlags_Inputable : 0 ) )
				return false;

			const bool bHovered = ImGui::ItemHoverable( trackHitBB, id, g.LastItemData.ItemFlags );
			bool bTempInputActive = bTempInputAllowed && ImGui::TempInputIsActive( id );
			if ( !bTempInputActive )
			{
				const bool bClicked = bHovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left, ImGuiInputFlags_None, id );
				const bool bMakeActive = ( bClicked || g.NavActivateId == id );
				if ( bMakeActive && bClicked )
					ImGui::SetKeyOwner( ImGuiKey_MouseLeft, id );
				if ( bMakeActive && bTempInputAllowed )
					if ( ( bClicked && g.IO.KeyCtrl ) || ( g.NavActivateId == id && ( g.NavActivateFlags & ImGuiActivateFlags_PreferInput ) ) )
						bTempInputActive = true;

				if ( bMakeActive )
					memcpy( &g.ActiveIdValueOnActivation, pValue, ImGui::DataTypeGetInfo( eDataType )->Size );

				if ( bMakeActive && !bTempInputActive )
				{
					ImGui::SetActiveID( id, pWindow );
					ImGui::SetFocusID( id, pWindow );
					ImGui::FocusWindow( pWindow );
					g.ActiveIdUsingNavDirMask |= ( 1 << ImGuiDir_Left ) | ( 1 << ImGuiDir_Right );
				}
			}

			if ( bTempInputActive )
			{
				// Ctrl+click / Enter-to-type -- same fallback ImGui's own
				// SliderScalar() uses; drawn as a plain text input box over
				// the (18px-tall) track hit-row rather than the spec's
				// track/handle art, since there's nothing to draw a
				// track/handle *for* while the value is literal free text.
				const bool bClampEnabled = ( nFlags & ImGuiSliderFlags_ClampOnInput ) != 0;
				return ImGui::TempInputScalar( trackHitBB, id, pszLabel, eDataType, pValue, pszFormat,
					bClampEnabled ? pMin : nullptr, bClampEnabled ? pMax : nullptr );
			}

			ImRect grabBB;
			const bool bChanged = ImGui::SliderBehavior( trackHitBB, id, eDataType, pValue, pMin, pMax, pszFormat, nFlags, &grabBB );
			if ( bChanged )
				ImGui::MarkItemEdited( id );

			ImGui::RenderNavCursor( trackHitBB, id );

			// ---- Draw: spec §7 geometry. grabBB's X center (computed by
			// SliderBehavior() against style.GrabMinSize=8, ApplyStyle())
			// is reused as the 8x18 handle's center so the handle tracks
			// the exact same fraction stock dragging/keyboard math drives. ----
			ImDrawList *pDrawList = pWindow->DrawList;

			ImGui::PushFont( fonts::Get( fonts::Style::Label ) );
			pDrawList->AddText( ImVec2( pos.x, pos.y + ( flLabelRowH - labelSize.y ) * 0.5f ),
				ImGui::GetColorU32( gamescope::palette::Text( 0.62f ) ), pszLabel, pszLabelEnd );
			ImGui::PopFont();

			ImGui::PushFont( fonts::Get( fonts::Style::Value ) );
			pDrawList->AddText( ImVec2( pos.x + flWidth - valueSize.x, pos.y + ( flLabelRowH - valueSize.y ) * 0.5f ),
				ImGui::GetColorU32( gamescope::palette::kAccentValue ), pszValueText );
			ImGui::PopFont();

			const float flTrackCenterY = trackHitBB.GetCenter().y;
			const ImVec2 trackMin( pos.x, flTrackCenterY - kTrackHeight * 0.5f );
			const ImVec2 trackMax( pos.x + flWidth, flTrackCenterY + kTrackHeight * 0.5f );
			pDrawList->AddRectFilled( trackMin, trackMax, ImGui::GetColorU32( gamescope::palette::White( 0.09f ) ), kTrackRounding );

			// Spec §12 "Disabled-but-visible control": on top of the whole
			// row's automatic x34% opacity (ImGui::BeginDisabled() already
			// multiplies style.Alpha, which every ImGui::GetColorU32() call
			// below picks up for free -- see ApplyStyle()'s DisabledAlpha
			// comment), the fill/handle hue itself turns plain white
			// (@30%/@45%) instead of staying accent-tinted-but-dim. Checked
			// via the same ImGuiItemFlags_Disabled flag BeginDisabled()
			// itself sets, exactly like stock ImGui widgets check it.
			const bool bDisabled = ( g.CurrentItemFlags & ImGuiItemFlags_Disabled ) != 0;

			const float flHandleCenterX = ImClamp( grabBB.GetCenter().x, trackMin.x, trackMax.x );
			if ( flHandleCenterX > trackMin.x )
			{
				if ( bDisabled )
				{
					const ImU32 fillWhite = ImGui::GetColorU32( gamescope::palette::White( 0.30f ) );
					pDrawList->AddRectFilled( trackMin, ImVec2( flHandleCenterX, trackMax.y ), fillWhite, kTrackRounding );
				}
				else
				{
					const ImU32 fillLo = ImGui::GetColorU32( gamescope::palette::Accent( 0.50f ) );
					const ImU32 fillHi = ImGui::GetColorU32( gamescope::palette::kAccentGradHi );
					pDrawList->AddRectFilledMultiColor( trackMin, ImVec2( flHandleCenterX, trackMax.y ), fillLo, fillHi, fillHi, fillLo );
				}
			}

			const ImVec2 handleCenter( flHandleCenterX, flTrackCenterY );

			if ( !bDisabled )
			{
				// Handle glow, approximating spec's `0 0 12px accent@80%`
				// box-shadow as two enlarged, low-alpha rects behind the
				// handle (same recipe spec §4/§7/§8 gives for every glow
				// ImGui has no primitive for) -- skipped while disabled: an
				// inert control glowing like a live one would contradict
				// the "plain white" look the spec gives it.
				const ImU32 glowOuter = ImGui::GetColorU32( gamescope::palette::Accent( 0.18f ) );
				const ImU32 glowInner = ImGui::GetColorU32( gamescope::palette::Accent( 0.30f ) );
				pDrawList->AddRectFilled( handleCenter - ImVec2( 8.0f, 13.0f ), handleCenter + ImVec2( 8.0f, 13.0f ), glowOuter, 6.0f );
				pDrawList->AddRectFilled( handleCenter - ImVec2( 6.0f, 11.0f ), handleCenter + ImVec2( 6.0f, 11.0f ), glowInner, 5.0f );
			}

			const ImU32 handleColor = bDisabled
				? ImGui::GetColorU32( gamescope::palette::White( 0.45f ) )
				: ImGui::GetColorU32( gamescope::palette::kAccentHandle );
			pDrawList->AddRectFilled( handleCenter - ImVec2( kHandleW * 0.5f, kHandleH * 0.5f ),
				handleCenter + ImVec2( kHandleW * 0.5f, kHandleH * 0.5f ),
				handleColor, 1.0f );

			if ( bHasMarks )
			{
				const float flMarkY = trackHitBB.Max.y + kTrackMarkGap;
				const ImU32 markColor = ImGui::GetColorU32( gamescope::palette::White( 0.26f ) );
				ImGui::PushFont( fonts::Get( fonts::Style::ScaleMark ) );
				if ( pszMinText )
					pDrawList->AddText( ImVec2( pos.x, flMarkY ), markColor, pszMinText );
				if ( pszMaxText )
					pDrawList->AddText( ImVec2( pos.x + flWidth - maxTextSize.x, flMarkY ), markColor, pszMaxText );
				ImGui::PopFont();
			}

			return bChanged;
		}
	}

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
		// Spec's "1px inset content padding" is measured from the *inside*
		// of the 1px track border, not from the track's outer edge -- using
		// it as the sole offset from bb.Min/Max put the knob flush against
		// the border's inner face (reported: "the lever needs one
		// horizontal pixel of additional spacing towards the edge"). 2px
		// here = 1px border + 1px content padding, matching the vertical
		// inset the centering below already produces ((15-11)/2 = 2px).
		static constexpr float kInset = 2.0f;

		return BooleanControl( pszLabel, pbValue, kTrackSize,
			[]( ImDrawList *pDrawList, const ImRect &bb, bool bValue, bool bHovered, bool /*bHeld*/ )
			{
				// On: track accent@30% (35% hovered), border accent@65%,
				// per spec §7 Toggle -- these are measured values, not an
				// invention.
				//
				// ponytail/invented (spec §7, §14: "the off-state track
				// color is not captured anywhere in the handoff -- every
				// toggle in the mockup is shown on"): off uses the spec's
				// own proposed in-style invention -- track white@7%, border
				// white@18%, knob white@55% -- flagged here exactly as the
				// spec flags it, not presented as measured.
				// Routed through ImGui::GetColorU32(ImU32) rather than passed
				// as raw palette:: constants -- that overload multiplies in
				// g.Style.Alpha, which is how ImGui::BeginDisabled()'s
				// DisabledAlpha (spec §12's "whole row x34% opacity" rule)
				// reaches a custom-drawn widget; skipping it would silently
				// stop these controls from dimming when disabled.
				ImU32 trackFill, trackBorder, knobFill;
				if ( bValue )
				{
					trackFill   = ImGui::GetColorU32( gamescope::palette::Accent( bHovered ? 0.35f : 0.30f ) );
					trackBorder = ImGui::GetColorU32( gamescope::palette::Accent( 0.65f ) );
					knobFill    = ImGui::GetColorU32( gamescope::palette::kAccentKnob );
				}
				else
				{
					trackFill   = ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.10f : 0.07f ) );
					trackBorder = ImGui::GetColorU32( gamescope::palette::White( 0.18f ) );
					knobFill    = ImGui::GetColorU32( gamescope::palette::White( 0.55f ) );
				}

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
				// Per spec §7 Checkbox: checked border accent@70%, fill
				// accent@20%; unchecked border white@18%, fill white@4%.
				// Hover nudges the fill alpha up slightly (in-style
				// invention, spec §12 -- hover was never designed).
				ImU32 fill, border;
				if ( bValue )
				{
					fill   = ImGui::GetColorU32( gamescope::palette::Accent( bHovered ? 0.28f : 0.20f ) );
					border = ImGui::GetColorU32( gamescope::palette::Accent( 0.70f ) );
				}
				else
				{
					fill   = ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.07f : 0.04f ) );
					border = ImGui::GetColorU32( gamescope::palette::White( 0.18f ) );
				}

				pDrawList->AddRectFilled( bb.Min, bb.Max, fill );
				pDrawList->AddRect( bb.Min, bb.Max, border, 0.0f, 0, 1.0f );

				if ( bValue )
				{
					// "a filled square (not a checkmark glyph)", centered,
					// accent-knob colored per spec §7.
					const ImVec2 markMin(
						bb.Min.x + ( bb.GetWidth() - kMarkSize ) * 0.5f,
						bb.Min.y + ( bb.GetHeight() - kMarkSize ) * 0.5f );
					const ImVec2 markMax = markMin + ImVec2( kMarkSize, kMarkSize );
					pDrawList->AddRectFilled( markMin, markMax, ImGui::GetColorU32( gamescope::palette::kAccentKnob ) );
				}
			} );
	}

	bool SliderFloat( const char *pszLabel, float *pflValue, float flMin, float flMax, const char *pszValueFormat, int nFlags )
	{
		char szValue[32], szMin[32], szMax[32];
		std::snprintf( szValue, sizeof( szValue ), pszValueFormat, (double)*pflValue );
		std::snprintf( szMin, sizeof( szMin ), pszValueFormat, (double)flMin );
		std::snprintf( szMax, sizeof( szMax ), pszValueFormat, (double)flMax );

		return SliderControl( pszLabel, ImGuiDataType_Float, pflValue, &flMin, &flMax,
			pszValueFormat, (ImGuiSliderFlags)nFlags, szValue, szMin, szMax );
	}

	bool SliderInt( const char *pszLabel, int *pnValue, int nMin, int nMax, const char *pszValueFormat, int nFlags )
	{
		char szValue[32], szMin[32], szMax[32];
		std::snprintf( szValue, sizeof( szValue ), pszValueFormat, *pnValue );
		std::snprintf( szMin, sizeof( szMin ), pszValueFormat, nMin );
		std::snprintf( szMax, sizeof( szMax ), pszValueFormat, nMax );

		return SliderControl( pszLabel, ImGuiDataType_S32, pnValue, &nMin, &nMax,
			pszValueFormat, (ImGuiSliderFlags)nFlags, szValue, szMin, szMax );
	}

	bool SegmentedControl( const char *pszId, int *pnSelected, const char *const *pszLabels, int nCount )
	{
		if ( nCount <= 0 )
			return false;

		ImGui::PushID( pszId );

		ImGuiWindow *pWindow = ImGui::GetCurrentWindow();
		ImDrawList *pDrawList = pWindow->DrawList;

		constexpr float kGap = 3.0f;
		constexpr float kPadY = 6.0f;
		const float flAvailWidth = ImGui::GetContentRegionAvail().x;
		const float flSegWidth = ( flAvailWidth - kGap * ( nCount - 1 ) ) / nCount;
		const float flSegHeight = ImGui::GetFontSize() + kPadY * 2.0f;

		bool bChanged = false;

		for ( int i = 0; i < nCount; i++ )
		{
			if ( i > 0 )
				ImGui::SameLine( 0.0f, kGap );

			ImGui::PushID( i );
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const ImVec2 size( flSegWidth, flSegHeight );
			const bool bClicked = ImGui::InvisibleButton( "##seg", size );
			const bool bHovered = ImGui::IsItemHovered();
			const bool bActive = ( i == *pnSelected );

			if ( bClicked && !bActive )
			{
				*pnSelected = i;
				bChanged = true;
			}

			// Per spec §7 Segmented control: inactive fill white@4%, border
			// white@8%, Mono 500 text @50% white; active fill accent@24%,
			// border accent@60%, Mono 600 text accent-seg-text (#A9EAFD).
			// Hover nudge on the inactive cell is an in-style invention
			// (spec §12 -- hover was never designed).
			const ImU32 fill = bActive
				? ImGui::GetColorU32( gamescope::palette::Accent( 0.24f ) )
				: ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.07f : 0.04f ) );
			const ImU32 border = bActive
				? ImGui::GetColorU32( gamescope::palette::Accent( 0.60f ) )
				: ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.12f : 0.08f ) );
			const ImU32 text = bActive
				? ImGui::GetColorU32( gamescope::palette::kAccentSegText )
				: ImGui::GetColorU32( gamescope::palette::White( 0.50f ) );

			pDrawList->AddRectFilled( pos, pos + size, fill ); // 0px radius -- controls stay flat/square
			pDrawList->AddRect( pos, pos + size, border );

			ImGui::PushFont( fonts::Get( bActive ? fonts::Style::SegmentActive : fonts::Style::SegmentLabel ) );
			const ImVec2 textSize = ImGui::CalcTextSize( pszLabels[i] );
			const ImVec2 textPos(
				pos.x + ( size.x - textSize.x ) * 0.5f,
				pos.y + ( size.y - textSize.y ) * 0.5f );
			pDrawList->AddText( textPos, text, pszLabels[i] );
			ImGui::PopFont();

			ImGui::PopID();
		}

		ImGui::PopID();
		return bChanged;
	}

	bool PositionGrid( const char *pszId, int *pnVert, int *pnHoriz )
	{
		ImGui::PushID( pszId );

		ImGuiWindow *pWindow = ImGui::GetCurrentWindow();
		ImDrawList *pDrawList = pWindow->DrawList;

		// Spec §11 ANCHOR block: 30x30px cells, 3px gaps.
		constexpr float kCellSize = 30.0f;
		constexpr float kGap = 3.0f;

		bool bChanged = false;
		const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();

		for ( int v = 0; v < 3; v++ )
		{
			for ( int h = 0; h < 3; h++ )
			{
				ImGui::PushID( v * 3 + h );

				const ImVec2 pos( gridOrigin.x + h * ( kCellSize + kGap ), gridOrigin.y + v * ( kCellSize + kGap ) );
				ImGui::SetCursorScreenPos( pos );

				const ImVec2 size( kCellSize, kCellSize );
				const bool bClicked = ImGui::InvisibleButton( "##cell", size );
				const bool bHovered = ImGui::IsItemHovered();
				const bool bActive = ( v == *pnVert && h == *pnHoriz );

				if ( bClicked && !bActive )
				{
					*pnVert = v;
					*pnHoriz = h;
					bChanged = true;
				}

				// Spec §11: cells fill white@5%/border white@9%; selected
				// fill accent@30%/border accent@70%. Hover nudge on the
				// inactive cell is the same in-style invention
				// SegmentedControl() above makes (spec §12: hover was
				// never designed).
				const ImU32 fill = bActive
					? ImGui::GetColorU32( gamescope::palette::Accent( 0.30f ) )
					: ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.08f : 0.05f ) );
				const ImU32 border = bActive
					? ImGui::GetColorU32( gamescope::palette::Accent( 0.70f ) )
					: ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.14f : 0.09f ) );

				pDrawList->AddRectFilled( pos, pos + size, fill ); // 0px radius -- flat/square, same as every other control here
				pDrawList->AddRect( pos, pos + size, border );

				ImGui::PopID();
			}
		}

		ImGui::SetCursorScreenPos( ImVec2( gridOrigin.x, gridOrigin.y + 3 * kCellSize + 2 * kGap ) );
		ImGui::Dummy( ImVec2( 3 * kCellSize + 2 * kGap, 0.0f ) ); // register the full grid footprint as one item block

		ImGui::PopID();
		return bChanged;
	}

	void ReadoutStrip( const char *pszText, bool bLeadingDot )
	{
		ImGuiWindow *pWindow = ImGui::GetCurrentWindow();
		if ( pWindow->SkipItems )
			return;

		constexpr float kPadX = 9.0f;
		constexpr float kPadY = 6.0f;
		constexpr float kDotSize = 6.0f;

		ImGui::PushFont( fonts::Get( fonts::Style::Meta ) );
		const ImVec2 textSize = ImGui::CalcTextSize( pszText );
		ImGui::PopFont();

		const float flWidth = ImGui::GetContentRegionAvail().x;
		const float flTextLeftInset = bLeadingDot ? kDotSize + 6.0f : 0.0f;
		const ImVec2 size( flWidth, textSize.y + kPadY * 2.0f );
		const ImVec2 pos = ImGui::GetCursorScreenPos();

		ImGui::Dummy( size ); // register the item -- this row has no interaction

		ImDrawList *pDrawList = pWindow->DrawList;
		pDrawList->AddRectFilled( pos, pos + size, ImGui::GetColorU32( gamescope::palette::White( 0.03f ) ) );

		float flTextX = pos.x + kPadX;
		if ( bLeadingDot )
		{
			const ImVec2 dotCenter( pos.x + kPadX + kDotSize * 0.5f, pos.y + size.y * 0.5f );
			pDrawList->AddRectFilled( dotCenter - ImVec2( kDotSize * 0.5f, kDotSize * 0.5f ),
				dotCenter + ImVec2( kDotSize * 0.5f, kDotSize * 0.5f ), ImGui::GetColorU32( gamescope::palette::kAccent ) );
			flTextX = pos.x + kPadX + flTextLeftInset;
		}

		ImGui::PushFont( fonts::Get( fonts::Style::Meta ) );
		pDrawList->AddText( ImVec2( flTextX, pos.y + kPadY ), ImGui::GetColorU32( gamescope::palette::White( 0.34f ) ), pszText );
		ImGui::PopFont();
	}

	bool BeginGroupBlock( const char *pszId, bool bActive )
	{
		// Spec §6: fill white@2.2% (active: 3.2%), border white@6% (active:
		// 7%), 12px padding, 10px row gap, square corners.
		ImGui::PushStyleColor( ImGuiCol_ChildBg, gamescope::palette::ToVec4( gamescope::palette::White( bActive ? 0.032f : 0.022f ) ) );
		ImGui::PushStyleColor( ImGuiCol_Border, gamescope::palette::ToVec4( gamescope::palette::White( bActive ? 0.07f : 0.06f ) ) );
		ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 0.0f );
		ImGui::PushStyleVar( ImGuiStyleVar_ChildBorderSize, 1.0f );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 12.0f, 12.0f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8.0f, 10.0f ) );

		ImGui::PushID( pszId );
		return ImGui::BeginChild( "##group", ImVec2( 0.0f, 0.0f ),
			ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
	}

	void EndGroupBlock( bool bActive )
	{
		ImGui::EndChild();
		ImGui::PopID();
		ImGui::PopStyleVar( 4 ); // ItemSpacing, WindowPadding, ChildBorderSize, ChildRounding
		ImGui::PopStyleColor( 2 ); // Border, ChildBg

		if ( bActive )
		{
			// 2px accent left edge replacing the left border -- spec §6's
			// "featured/active group" treatment. Drawn over the just-closed
			// child's own rect: GetItemRect*() reflects the child now that
			// EndChild() has registered it as an item on the *parent*
			// window, same trick BeginGroupBlock()'s caller-visible geometry
			// relies on nowhere else -- this is the only place that needs it.
			const ImVec2 mn = ImGui::GetItemRectMin();
			const ImVec2 mx = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddRectFilled( mn, ImVec2( mn.x + 2.0f, mx.y ),
				ImGui::GetColorU32( gamescope::palette::Accent( 0.80f ) ) );
		}
	}
}
