// M8 part 3 (issue #15) -- see Chrome.h for the API-level design notes.
// This file has two halves: the icon renderer (top), then the panel-window
// and dock chrome that uses it (bottom).
//
// Icon renderer: data/icons/*.svg's own README documents its rules (rects/
// circles/lines/polygon wedges on a 20x20 viewBox, currentColor throughout,
// no rounded corners) precisely because every icon in the set is already
// built from simple geometric primitives -- there is no freehand path data
// to rasterize. That made "how do we get these into ImGui" a real choice
// between two routes: (a) rasterize each SVG to a bitmap and bake it into
// the font atlas / a texture, which means vendoring an SVG parser (none is
// currently vendored -- grep for nanosvg/lunasvg/etc. against subprojects/
// turns up nothing) purely to re-derive shapes this codebase already has
// the coordinates for in readable form, or (b) transcribe each icon's own
// primitives directly into ImDrawList calls once, by hand, here. (b) was
// chosen: no new dependency, no atlas-baking step, and -- unlike a baked
// bitmap -- these stay crisp at any button size the chrome ever draws them
// at, with tinting being "pass a different ImU32", the direct equivalent of
// the SVGs' own currentColor. The tradeoff, honestly: this file's geometry
// and the .svg files' geometry are now two independent copies of the same
// shapes that could drift if one is edited without the other -- acceptable
// for an 11-icon set that the design guide treats as closed/settled, not
// something expected to grow arbitrarily.
//
// Panel/dock chrome: see Chrome.h's file comment for the structural problem
// this solves (five always-on overlapping windows -> five windows shown/
// hidden from a bottom dock, per SPEC.md's UI structure).
//
// ponytail: the design guide's single 30-34px title-bar header row (status
// dot + icon + title + meta + collapse/close glyph-button cluster, all in
// one row replacing ImGui's own title bar) is NOT reimplemented as one
// literal custom-drawn row here. Doing that means reimplementing ImGui's
// title-bar hit-testing/drag/collapse/close behavior by hand (Begin()'s own
// title bar already does all of that correctly, for free) just to move a
// few pixels of decoration into the same visual row. Instead: keep ImGui's
// native title bar (drag, the Title font from M8 part 1, and a native close
// X wired to this panel's open/closed dock state), and add a slim second
// row of icon + status dot + meta text as ordinary window content directly
// underneath it. Same information, same "this panel is X and it's live"
// read, a fraction of the risk. If a future pass wants the literal single-
// row treatment, this is the seam to replace.
#include "Chrome.h"

#include "Fonts.h"
#include "../SettingsOverlay.h"

#include <cmath>
#include <cstddef>

#include "imgui.h"

namespace gamescope::chrome
{
	// ======================================================================
	// Icons
	// ======================================================================

	namespace
	{
		// Maps data/icons/*.svg's shared 20x20 viewBox onto a flSize x flSize
		// box centered at `center`. Every DrawXIcon() helper below takes
		// viewBox-space coordinates/lengths (i.e. the exact numbers copied
		// out of the .svg files) and goes through P()/L() to place them.
		struct IconSpace
		{
			ImVec2 origin;
			float flScale;

			ImVec2 P( float x, float y ) const { return ImVec2( origin.x + x * flScale, origin.y + y * flScale ); }
			float  L( float flLen ) const { return flLen * flScale; }
		};

		IconSpace MakeSpace( ImVec2 center, float flSize )
		{
			return IconSpace{ ImVec2( center.x - flSize * 0.5f, center.y - flSize * 0.5f ), flSize / 20.0f };
		}

		// Fills a rect (viewBox coords) rotated by flDegrees about
		// (flCx, flCy) -- settings.svg's 8 gear teeth are the only shapes in
		// the set that need this.
		void FillRotatedRect( ImDrawList *pDrawList, const IconSpace &s,
			float x0, float y0, float x1, float y1,
			float flCx, float flCy, float flDegrees, ImU32 col )
		{
			const float flRad = flDegrees * (float)M_PI / 180.0f;
			const float c = cosf( flRad ), sn = sinf( flRad );
			const ImVec2 corners[4] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
			ImVec2 pts[4];
			for ( int i = 0; i < 4; i++ )
			{
				const float dx = corners[i].x - flCx;
				const float dy = corners[i].y - flCy;
				pts[i] = s.P( flCx + dx * c - dy * sn, flCy + dx * sn + dy * c );
			}
			pDrawList->AddConvexPolyFilled( pts, 4, col );
		}

		void DrawSettingsIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddCircle( s.P( 10, 10 ), s.L( 6 ), col, 0, s.L( 1.5f ) );
			dl->AddCircleFilled( s.P( 10, 10 ), s.L( 2 ), col );
			for ( int i = 0; i < 8; i++ )
				FillRotatedRect( dl, s, 9.2f, 1.7f, 10.8f, 4.2f, 10.0f, 10.0f, i * 45.0f, col );
		}

		void DrawDisplayIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddRect( s.P( 3, 3.5f ), s.P( 17, 13 ), col, 0.0f, s.L( 1.5f ) );
			dl->AddLine( s.P( 10, 13 ), s.P( 10, 15.3f ), col, s.L( 1.5f ) );
			dl->AddLine( s.P( 6.5f, 15.3f ), s.P( 13.5f, 15.3f ), col, s.L( 1.5f ) );
			dl->AddRectFilled( s.P( 5.7f, 6.6f ), s.P( 9.7f, 8.0f ), col );
			dl->AddRectFilled( s.P( 5.7f, 9.5f ), s.P( 13.3f, 10.9f ), col );
		}

		void DrawShadersIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddCircle( s.P( 10, 10 ), s.L( 6 ), col, 0, s.L( 1.5f ) );
			// "M10 4A6 6 0 0 1 10 16Z" -- the right half-disc.
			dl->PathClear();
			dl->PathLineTo( s.P( 10, 10 ) );
			dl->PathArcTo( s.P( 10, 10 ), s.L( 6 ), -(float)M_PI * 0.5f, (float)M_PI * 0.5f, 24 );
			dl->PathFillConvex( col );
		}

		void DrawPerformanceIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddRectFilled( s.P( 3.6f, 12.0f ), s.P( 6.2f, 16.0f ), col );
			dl->AddRectFilled( s.P( 7.3f, 9.0f ), s.P( 9.9f, 16.0f ), col );
			dl->AddRectFilled( s.P( 11.0f, 6.0f ), s.P( 13.6f, 16.0f ), col );
			dl->AddRectFilled( s.P( 14.7f, 3.3f ), s.P( 17.3f, 16.0f ), col );
		}

		void DrawAudioIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddRectFilled( s.P( 3, 7.8f ), s.P( 6, 12.2f ), col );
			const ImVec2 cone[4] = { s.P( 6, 7.8f ), s.P( 11, 4.2f ), s.P( 11, 15.8f ), s.P( 6, 12.2f ) };
			dl->AddConvexPolyFilled( cone, 4, col );

			// Two sound-wave arcs -- geometrically reconstructed from the
			// SVG's relative-arc path commands (chord endpoints + radius
			// uniquely determine the circle up to a left/right center
			// choice; the rightward-bulging choice is the one that reads as
			// "sound waves" rather than curling back into the cone).
			dl->PathClear();
			dl->PathArcTo( s.P( 8.7917f, 10.0f ), s.L( 5.0f ), -0.5705f, 0.5705f, 12 );
			dl->PathStroke( col, s.L( 1.5f ) );

			dl->PathClear();
			dl->PathArcTo( s.P( 8.6547f, 10.0f ), s.L( 8.0f ), -0.6119f, 0.6119f, 16 );
			dl->PathStroke( col, s.L( 1.5f ) );
		}

		void DrawProfilesIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddLine( s.P( 3.5f, 5.5f ), s.P( 13, 5.5f ), col, s.L( 1.5f ) );
			dl->AddLine( s.P( 3.5f, 10 ), s.P( 13, 10 ), col, s.L( 1.5f ) );
			dl->AddLine( s.P( 3.5f, 14.5f ), s.P( 13, 14.5f ), col, s.L( 1.5f ) );
			dl->AddCircleFilled( s.P( 15.7f, 5.5f ), s.L( 1.15f ), col );
			dl->AddCircleFilled( s.P( 15.7f, 10 ), s.L( 1.15f ), col );
			dl->AddCircleFilled( s.P( 15.7f, 14.5f ), s.L( 1.15f ), col );
		}

		void DrawResetIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			// Broken-ring + arrowhead, per the README's own documented
			// fallback for this icon ("the fallback is to drop the
			// arrowhead's subtlety concern entirely and render reset as a
			// plain broken-ring glyph"). The gap sits roughly where the
			// SVG's own arc endpoints are (~-55deg to ~205deg sweep,
			// computed from the path's start/end points and radius).
			dl->PathClear();
			dl->PathArcTo( s.P( 10, 10 ), s.L( 6 ), -55.0f * (float)M_PI / 180.0f, 205.0f * (float)M_PI / 180.0f, 32 );
			dl->PathStroke( col, s.L( 1.5f ) );
			dl->AddTriangleFilled( s.P( 5.53f, 5.38f ), s.P( 5.96f, 9.44f ), s.P( 2.15f, 7.66f ), col );
		}

		void DrawCloseIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddLine( s.P( 5, 5 ), s.P( 15, 15 ), col, s.L( 1.5f ) );
			dl->AddLine( s.P( 15, 5 ), s.P( 5, 15 ), col, s.L( 1.5f ) );
		}

		void DrawCollapseIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddLine( s.P( 5, 10 ), s.P( 15, 10 ), col, s.L( 1.5f ) );
		}

		void DrawDockMoreIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddCircleFilled( s.P( 6, 10 ), s.L( 1.3f ), col );
			dl->AddCircleFilled( s.P( 10, 10 ), s.L( 1.3f ), col );
			dl->AddCircleFilled( s.P( 14, 10 ), s.L( 1.3f ), col );
		}

		void DrawCheckboxMarkIcon( ImDrawList *dl, const IconSpace &s, ImU32 col )
		{
			dl->AddRect( s.P( 4, 4 ), s.P( 16, 16 ), col, 0.0f, s.L( 1.5f ) );
			dl->AddRectFilled( s.P( 7.5f, 7.5f ), s.P( 12.5f, 12.5f ), col );
		}
	}

	void DrawIcon( void *pDrawListVoid, Icon icon, ImVec2 center, float flSize, unsigned int uColor )
	{
		ImDrawList *dl = static_cast<ImDrawList *>( pDrawListVoid );
		const IconSpace s = MakeSpace( center, flSize );
		const ImU32 col = (ImU32)uColor;

		switch ( icon )
		{
			case Icon::Settings:     DrawSettingsIcon( dl, s, col ); break;
			case Icon::Display:      DrawDisplayIcon( dl, s, col ); break;
			case Icon::Shaders:      DrawShadersIcon( dl, s, col ); break;
			case Icon::Performance:  DrawPerformanceIcon( dl, s, col ); break;
			case Icon::Audio:        DrawAudioIcon( dl, s, col ); break;
			case Icon::Profiles:     DrawProfilesIcon( dl, s, col ); break;
			case Icon::Reset:        DrawResetIcon( dl, s, col ); break;
			case Icon::Close:        DrawCloseIcon( dl, s, col ); break;
			case Icon::Collapse:     DrawCollapseIcon( dl, s, col ); break;
			case Icon::DockMore:     DrawDockMoreIcon( dl, s, col ); break;
			case Icon::CheckboxMark: DrawCheckboxMarkIcon( dl, s, col ); break;
		}
	}

	// ======================================================================
	// Panel windows + dock
	// ======================================================================

	namespace
	{
		// Local-only color tokens for the chrome elements this file draws
		// itself (dock buttons, the icon/status-dot sub-header, the accent
		// left-edge). Deliberately NOT written into ImGuiStyle.Colors[] --
		// that shared array is SettingsOverlay.cpp's ApplyDesignGuideStyle()
		// (issue #14 territory, see this file's top comment) -- these are
		// only ever passed straight to ImDrawList calls this file makes.
		// Values are the design guide's own tokens (ui-design-guide.md's
		// Color palette table), converted from its oklch/rgba notation to
		// sRGB bytes once, here.
		constexpr ImU32 kAccentU32        = IM_COL32( 0x4f, 0xb8, 0xd6, 255 );
		constexpr ImU32 kAccentHiU32      = IM_COL32( 0xdd, 0xf3, 0xf8, 255 );
		constexpr ImU32 kAccentSoftBgU32  = IM_COL32( 0x4f, 0xb8, 0xd6, 56 );
		constexpr ImU32 kHairlineU32      = IM_COL32( 255, 255, 255, 26 );
		constexpr ImU32 kHairlineStrongU32= IM_COL32( 255, 255, 255, 41 );
		constexpr ImU32 kRaisedU32        = IM_COL32( 255, 255, 255, 13 );
		constexpr ImU32 kRaisedHoverU32   = IM_COL32( 255, 255, 255, 20 );
		constexpr ImU32 kIconIdleU32      = IM_COL32( 255, 255, 255, 153 );
		constexpr ImU32 kIconHoverU32     = IM_COL32( 255, 255, 255, 191 );

		bool s_bPanelOpen[(size_t)PanelId::Count] = { true, true, true, true, true };

		Icon IconForPanel( PanelId id )
		{
			switch ( id )
			{
				case PanelId::Display: return Icon::Display;
				case PanelId::Shaders: return Icon::Shaders;
				case PanelId::Fps:     return Icon::Performance;
				case PanelId::Audio:   return Icon::Audio;
				case PanelId::Config:  return Icon::Profiles;
				default:                return Icon::Settings;
			}
		}

		// Slim icon + status-dot sub-header, drawn as ordinary window
		// content right under ImGui's own native title bar, plus the 2px
		// accent left-edge on the window frame while focused. See this
		// file's top ponytail comment for why this isn't one literal
		// custom title-bar row.
		void DrawPanelChromeHeader( PanelId id )
		{
			if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) )
			{
				ImDrawList *pDrawList = ImGui::GetWindowDrawList();
				const ImVec2 windowPos = ImGui::GetWindowPos();
				const ImVec2 windowSize = ImGui::GetWindowSize();
				pDrawList->AddRectFilled( windowPos, ImVec2( windowPos.x + 2.0f, windowPos.y + windowSize.y ), kAccentU32 );
			}

			ImDrawList *pDrawList = ImGui::GetWindowDrawList();
			constexpr float kDotRadius = 3.0f;
			constexpr float kIconSize = 14.0f;
			constexpr float kRowHeight = kIconSize;
			const ImVec2 cursor = ImGui::GetCursorScreenPos();

			const ImVec2 dotCenter( cursor.x + kDotRadius, cursor.y + kRowHeight * 0.5f );
			pDrawList->AddCircleFilled( dotCenter, kDotRadius, kAccentU32 );

			const float flIconLeft = cursor.x + kDotRadius * 2.0f + 6.0f;
			const ImVec2 iconCenter( flIconLeft + kIconSize * 0.5f, cursor.y + kRowHeight * 0.5f );
			DrawIcon( pDrawList, IconForPanel( id ), iconCenter, kIconSize, kIconIdleU32 );

			ImGui::Dummy( ImVec2( flIconLeft + kIconSize + 6.0f - cursor.x, kRowHeight ) );
			ImGui::SameLine();
			ImGui::PushFont( fonts::Get( fonts::Style::Meta ) );
			ImGui::TextDisabled( "gamescope-ritz" );
			ImGui::PopFont();
			ImGui::Separator();
		}
	}

	bool IsPanelOpen( PanelId id )
	{
		return s_bPanelOpen[(size_t)id];
	}

	void SetPanelOpen( PanelId id, bool bOpen )
	{
		s_bPanelOpen[(size_t)id] = bOpen;
	}

	bool BeginPanelWindow( const char *pszTitle, PanelId id, ImVec2 defaultPos, ImVec2 defaultSize )
	{
		if ( !IsPanelOpen( id ) )
			return false;

		ImGui::SetNextWindowPos( defaultPos, ImGuiCond_FirstUseEver );
		ImGui::SetNextWindowSize( defaultSize, ImGuiCond_FirstUseEver );

		// Design guide "Spacing & layout": window corner radius 3-4px
		// (controls stay flat/0px -- that's Widgets.cpp's ApplyStyle(),
		// unaffected by this window-scoped push). "Focused window: accent
		// border at ~42% opacity ... the *only* focus indicator" -- pushed
		// unconditionally and popped after End() rather than branched on
		// focus, since ImGui doesn't know this window's focus state until
		// after Begin() returns; the border color itself is corrected to
		// accent-vs-hairline below, inside the window, once focus is known.
		ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 4.0f );

		bool bStillOpen = true;
		ImGui::PushFont( fonts::Get( fonts::Style::Title ) );
		ImGui::Begin( pszTitle, &bStillOpen, ImGuiWindowFlags_NoCollapse );
		ImGui::PopFont();

		if ( !bStillOpen )
			SetPanelOpen( id, false );

		// The window's Border color is a single shared ImGuiStyle value
		// (Widgets.cpp's `hairline` token) -- recolor just *this* window's
		// border here, per-frame, rather than trying to give every window a
		// different static style color. PopStyleColor in EndPanelWindow().
		const bool bFocused = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
		ImGui::PushStyleColor( ImGuiCol_Border,
			bFocused ? ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, 0.42f )
			         : ImGui::GetStyle().Colors[ImGuiCol_Border] );

		DrawPanelChromeHeader( id );

		return true;
	}

	void EndPanelWindow()
	{
		ImGui::PopStyleColor(); // ImGuiCol_Border, pushed in BeginPanelWindow()
		ImGui::End();
		ImGui::PopStyleVar(); // WindowRounding, pushed in BeginPanelWindow()
	}

	namespace
	{
		struct DockEntry
		{
			PanelId id;
			Icon icon;
			const char *pszLabel;
		};

		constexpr DockEntry kDockEntries[] = {
			{ PanelId::Display, Icon::Display,     "Display" },
			{ PanelId::Shaders, Icon::Shaders,     "Shaders" },
			{ PanelId::Fps,     Icon::Performance, "FPS HUD" },
			{ PanelId::Audio,   Icon::Audio,       "Audio" },
			{ PanelId::Config,  Icon::Profiles,    "Config / Profiles" },
		};

		bool DrawDockButton( ImDrawList *pDrawList, Icon icon, bool bActive, const char *pszLabel, float flSize )
		{
			ImGui::PushID( pszLabel );
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const bool bClicked = ImGui::InvisibleButton( "##dockbtn", ImVec2( flSize, flSize ) );
			const bool bHovered = ImGui::IsItemHovered();

			const ImU32 bgCol = bActive ? kAccentSoftBgU32 : ( bHovered ? kRaisedHoverU32 : kRaisedU32 );
			const ImU32 borderCol = bActive ? kAccentU32 : ( bHovered ? kHairlineStrongU32 : kHairlineU32 );
			const ImU32 iconCol = bActive ? kAccentHiU32 : ( bHovered ? kIconHoverU32 : kIconIdleU32 );

			pDrawList->AddRectFilled( pos, ImVec2( pos.x + flSize, pos.y + flSize ), bgCol );
			pDrawList->AddRect( pos, ImVec2( pos.x + flSize, pos.y + flSize ), borderCol );
			DrawIcon( pDrawList, icon, ImVec2( pos.x + flSize * 0.5f, pos.y + flSize * 0.5f ), flSize * 0.4f, iconCol );

			if ( bHovered )
				ImGui::SetTooltip( "%s", pszLabel );

			ImGui::PopID();
			return bClicked;
		}
	}

	void DrawDock()
	{
		ImGuiIO &io = ImGui::GetIO();
		constexpr float kButtonSize = 54.0f;
		constexpr float kGap = 5.0f;
		constexpr float kPad = 6.0f;
		constexpr int kPanelCount = (int)PanelId::Count;

		const float flContentWidth =
			kButtonSize                                                    // brand glyph
			+ kGap
			+ kPanelCount * kButtonSize + ( kPanelCount - 1 ) * kGap        // panel toggles
			+ kGap + 1.0f + kGap                                           // divider
			+ kButtonSize;                                                 // close overlay

		ImGui::SetNextWindowPos( ImVec2( io.DisplaySize.x * 0.5f, io.DisplaySize.y - 38.0f ),
			ImGuiCond_Always, ImVec2( 0.5f, 1.0f ) );
		ImGui::SetNextWindowSize( ImVec2( flContentWidth + kPad * 2.0f, kButtonSize + kPad * 2.0f ), ImGuiCond_Always );

		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( kPad, kPad ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 4.0f );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 1.0f );
		ImGui::Begin( "##gamescope_ritz_overlay_dock", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing );

		ImDrawList *pDrawList = ImGui::GetWindowDrawList();

		// Brand glyph: SPEC.md's icon list calls settings.svg the "dock
		// entry point / overlay identity" icon -- static (no separate
		// "settings panel" exists beyond the five already docked here, so
		// there is nothing for a click on this one to open).
		{
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			pDrawList->AddRectFilled( pos, ImVec2( pos.x + kButtonSize, pos.y + kButtonSize ), kRaisedU32 );
			pDrawList->AddRect( pos, ImVec2( pos.x + kButtonSize, pos.y + kButtonSize ), kHairlineU32 );
			DrawIcon( pDrawList, Icon::Settings, ImVec2( pos.x + kButtonSize * 0.5f, pos.y + kButtonSize * 0.5f ),
				kButtonSize * 0.4f, kAccentU32 );
			ImGui::Dummy( ImVec2( kButtonSize, kButtonSize ) );
		}

		for ( const DockEntry &entry : kDockEntries )
		{
			ImGui::SameLine( 0.0f, kGap );
			const bool bOpen = IsPanelOpen( entry.id );
			if ( DrawDockButton( pDrawList, entry.icon, bOpen, entry.pszLabel, kButtonSize ) )
				SetPanelOpen( entry.id, !bOpen );
		}

		// Hairline divider, then the trailing "close the whole overlay"
		// button -- design guide: "a 1px divider before the trailing close
		// button".
		ImGui::SameLine( 0.0f, kGap );
		{
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			pDrawList->AddLine( ImVec2( pos.x, pos.y ), ImVec2( pos.x, pos.y + kButtonSize ), kHairlineU32, 1.0f );
			ImGui::Dummy( ImVec2( 1.0f, kButtonSize ) );
		}

		ImGui::SameLine( 0.0f, kGap );
		{
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const bool bClicked = ImGui::InvisibleButton( "##dock_close_overlay", ImVec2( kButtonSize, kButtonSize ) );
			const bool bHovered = ImGui::IsItemHovered();

			pDrawList->AddRectFilled( pos, ImVec2( pos.x + kButtonSize, pos.y + kButtonSize ), bHovered ? kRaisedHoverU32 : kRaisedU32 );
			pDrawList->AddRect( pos, ImVec2( pos.x + kButtonSize, pos.y + kButtonSize ), bHovered ? kHairlineStrongU32 : kHairlineU32 );
			DrawIcon( pDrawList, Icon::Close, ImVec2( pos.x + kButtonSize * 0.5f, pos.y + kButtonSize * 0.5f ),
				kButtonSize * 0.4f, bHovered ? kIconHoverU32 : kIconIdleU32 );

			if ( bHovered )
				ImGui::SetTooltip( "Close overlay" );
			if ( bClicked )
				SettingsOverlay_ToggleVisible();
		}

		ImGui::End();
		ImGui::PopStyleVar( 3 );
	}
}
