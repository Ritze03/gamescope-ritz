// HUD layouts Phase 3 -- see HudLayoutEditor.h for the file-level design
// note (why this lives in the Shell's ImGui context, not the HUD's).

#include "HudLayoutEditor.h"

#include "Colors.h"
#include "Tokens.h"

#include "Overlay/FpsDisplay.h"
#include "Config/ConfigManager.h"
#include "steamcompmgr.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace gamescope::ui::hudedit
{
	namespace
	{
		// -----------------------------------------------------------------
		// State. All of it is session-local to one edit -- Begin() seeds it
		// fresh every time, so nothing here needs to survive IsActive()
		// going false.
		// -----------------------------------------------------------------
		bool s_bActive = false;
		std::string s_sLayoutName;      // "" until Save() names one, same meaning as FpsDisplaySettings::layout_name
		config::HudLayout s_Working;    // the copy being edited -- what Draw() reads/writes
		config::HudLayout s_Snapshot;   // Begin()'s own copy, restored by Cancel()

		int    s_nDragging = -1;        // index into kModuleOrder (0 fps, 1 cpu, 2 gpu, 3 media), -1 = nothing grabbed
		ImVec2 s_GrabOffsetShell;       // mouse - box top-left (Shell space), recorded at grab time

		// This frame's active snaps, for the guide lines -- recomputed every
		// frame a drag is in progress, cleared otherwise.
		bool  s_bSnapXActive = false, s_bSnapYActive = false;
		float s_flGuideX = 0.0f, s_flGuideY = 0.0f;

		constexpr const char *kModuleNames[4] = { "FPS", "CPU", "GPU", "MEDIA" };

		// Snap tolerance, Shell-space physical pixels (task brief: "within
		// 8 physical pixels").
		constexpr float kSnapTolerancePx = 8.0f;

		// -----------------------------------------------------------------
		// origin <-> (vertical, horizontal) index, this file's own copy of
		// FpsDisplay.cpp's identical kPlacements/ParsePlacement -- that
		// copy is a file-local static there (not exported), and
		// FpsDisplay.cpp's own header comment on ITS copy already
		// establishes the precedent: kept as a separate copy per file
		// rather than a shared header, since Notifications.cpp has a third
		// one and none of the three are exported today.
		// -----------------------------------------------------------------
		constexpr const char *kPlacements[3][3] = {
			{ "top-left",    "top-center",    "top-right"    },
			{ "center-left", "center",        "center-right" },
			{ "bottom-left", "bottom-center", "bottom-right" },
		};

		void ParsePlacement( const std::string &sPlacement, int &nVert, int &nHoriz )
		{
			for ( int v = 0; v < 3; v++ )
				for ( int h = 0; h < 3; h++ )
					if ( sPlacement == kPlacements[v][h] ) { nVert = v; nHoriz = h; return; }
			nVert = 0;
			nHoriz = 2; // FpsDisplay.cpp's own fallback (top-right)
		}

		// kModuleOrder's index -> the working layout's own HudLayoutModule.
		// Same shape as FpsDisplay.cpp's file-local ModulePlacement(), one
		// more file-local copy for the same reason as ParsePlacement above.
		config::HudLayoutModule &ModuleAt( config::HudLayout &layout, int i )
		{
			switch ( i )
			{
			case 0:  return layout.fps.placement;
			case 1:  return layout.cpu;
			case 2:  return layout.gpu;
			default: return layout.media;
			}
		}

		// -----------------------------------------------------------------
		// Shell-space geometry for the four modules, derived fresh every
		// Draw() call from FpsDisplay_GetModuleRects()'s HUD-space answer
		// by simple per-axis ratio -- see HudLayoutEditor.h's file comment
		// and FpsDisplay_GetModuleRects()'s own comment for why the two
		// display spaces are not assumed to match (and deliberately not
		// overlay.display_scale, which governs the Shell's own widget
		// sizing, not a mapping between two independent ImGui contexts'
		// pixel spaces).
		// -----------------------------------------------------------------
		struct ShellRect { float x0, y0, x1, y1; bool bEnabled; };

		// -----------------------------------------------------------------
		// Snapping.
		// -----------------------------------------------------------------
		struct AxisTarget
		{
			float flPos;
			bool  bScreen; // true: one of the screen's own three anchors (nHalf meaningful); false: another module's edge/centre
			int   nHalf;   // 0 = left/top, 1 = centre/middle, 2 = right/bottom -- only meaningful when bScreen
		};

		struct SnapOutcome
		{
			bool  bSnapped = false;
			float flNewMin = 0.0f;
			bool  bScreen  = false;
			int   nHalf    = -1;
			float flGuide  = 0.0f;
		};

		// Tests the box's own three reference points (min/centre/max on this
		// axis) against every target, and returns the globally closest pair
		// within `flTolerance` -- so a box that is simultaneously close to
		// two different targets on the same axis snaps to whichever is
		// nearer, not whichever target happened to be tested first.
		SnapOutcome SnapAxis( float flMin, float flSize, const std::vector<AxisTarget> &targets, float flTolerance )
		{
			const float refs[3] = { flMin, flMin + flSize * 0.5f, flMin + flSize };
			SnapOutcome out;
			float flBestDist = flTolerance;
			for ( float flRef : refs )
			{
				for ( const AxisTarget &t : targets )
				{
					const float flDist = std::fabs( t.flPos - flRef );
					if ( flDist <= flBestDist )
					{
						flBestDist   = flDist;
						out.bSnapped = true;
						out.flNewMin = flMin + ( t.flPos - flRef );
						out.bScreen  = t.bScreen;
						out.nHalf    = t.nHalf;
						out.flGuide  = t.flPos;
					}
				}
			}
			if ( !out.bSnapped )
				out.flNewMin = flMin;
			return out;
		}
	}

	bool IsActive() { return s_bActive; }

	void Begin()
	{
		s_sLayoutName = FpsDisplay_ActiveLayoutName();
		s_Working  = config::ResolveLayoutCached( s_sLayoutName );
		s_Snapshot = s_Working;
		s_nDragging = -1;
		s_bSnapXActive = s_bSnapYActive = false;
		s_bActive = true;
	}

	namespace
	{
		void Cancel()
		{
			s_Working = s_Snapshot; // nothing was ever persisted mid-drag; this is symmetry with Save(), not a real restore
			s_bActive = false;
			s_nDragging = -1;
		}

		void Save()
		{
			if ( s_sLayoutName.empty() )
			{
				const std::optional<std::string> oName = config::SanitizeLayoutName( "custom" );
				s_sLayoutName = oName.value_or( "custom" );
				FpsDisplay_SetActiveLayoutName( s_sLayoutName );
			}
			config::SaveLayout( s_sLayoutName, s_Working );
			config::ReloadLayoutCache();
			s_bActive = false;
			s_nDragging = -1;
			force_repaint();
		}
	}

	bool HandleEscape()
	{
		if ( !s_bActive )
			return false;
		Cancel();
		return true;
	}

	void Draw()
	{
		if ( !s_bActive )
			return;

		// The HUD's own 500ms repaint-timer thread (FpsDisplay.cpp) is far
		// too slow to carry a live drag -- unconditional, every frame this
		// runs, same as this file's own header comment promises.
		force_repaint();

		const ImGuiIO &io = ImGui::GetIO();
		const ImVec2 shellSize = io.DisplaySize;
		if ( shellSize.x <= 0.0f || shellSize.y <= 0.0f )
			return;

		HudModuleRect hudRects[4];
		float flHudW = 0.0f, flHudH = 0.0f;
		FpsDisplay_GetModuleRects( s_Working, hudRects, &flHudW, &flHudH );
		flHudW = std::max( flHudW, 1.0f );
		flHudH = std::max( flHudH, 1.0f );

		const float flScaleX = shellSize.x / flHudW;
		const float flScaleY = shellSize.y / flHudH;

		ShellRect rects[4];
		for ( int i = 0; i < 4; ++i )
		{
			rects[i].bEnabled = hudRects[i].bEnabled;
			rects[i].x0 = hudRects[i].x * flScaleX;
			rects[i].y0 = hudRects[i].y * flScaleY;
			rects[i].x1 = ( hudRects[i].x + hudRects[i].w ) * flScaleX;
			rects[i].y1 = ( hudRects[i].y + hudRects[i].h ) * flScaleY;
		}

		// =================================================================
		//  Input: grab / drag / release. Before any drawing, on THIS
		//  frame's rects, same "decide input before a pixel is painted"
		//  ordering Shell.cpp's own RunKeyboard()/dropdown dismissal use.
		// =================================================================
		const ImVec2 mousePos = io.MousePos;
		const bool bMouseDown = ImGui::IsMouseDown( ImGuiMouseButton_Left );

		if ( s_nDragging >= 0 && !bMouseDown )
			s_nDragging = -1;

		// The chrome bar (drawn below) claims the top strip -- keep grabs
		// out of it without depending on ImGui hover-state ordering against
		// widgets this same frame hasn't submitted yet.
		const float flChromeH = Px( 56.0f );

		if ( s_nDragging < 0 && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && mousePos.y > flChromeH )
		{
			for ( int i = 3; i >= 0; --i ) // topmost (kModuleOrder's Media) first
			{
				if ( !rects[i].bEnabled )
					continue;
				if ( mousePos.x >= rects[i].x0 && mousePos.x <= rects[i].x1 &&
				     mousePos.y >= rects[i].y0 && mousePos.y <= rects[i].y1 )
				{
					s_nDragging = i;
					s_GrabOffsetShell = ImVec2( mousePos.x - rects[i].x0, mousePos.y - rects[i].y0 );
					break;
				}
			}
		}

		s_bSnapXActive = s_bSnapYActive = false;

		if ( s_nDragging >= 0 )
		{
			const int idx = s_nDragging;
			const float flBoxW = rects[idx].x1 - rects[idx].x0;
			const float flBoxH = rects[idx].y1 - rects[idx].y0;
			const float flDesiredMinX = mousePos.x - s_GrabOffsetShell.x;
			const float flDesiredMinY = mousePos.y - s_GrabOffsetShell.y;

			std::vector<AxisTarget> xTargets = {
				{ 0.0f,              true, 0 },
				{ shellSize.x * 0.5f, true, 1 },
				{ shellSize.x,        true, 2 },
			};
			std::vector<AxisTarget> yTargets = {
				{ 0.0f,              true, 0 },
				{ shellSize.y * 0.5f, true, 1 },
				{ shellSize.y,        true, 2 },
			};
			for ( int j = 0; j < 4; ++j )
			{
				if ( j == idx || !rects[j].bEnabled )
					continue;
				xTargets.push_back( { rects[j].x0, false, -1 } );
				xTargets.push_back( { ( rects[j].x0 + rects[j].x1 ) * 0.5f, false, -1 } );
				xTargets.push_back( { rects[j].x1, false, -1 } );
				yTargets.push_back( { rects[j].y0, false, -1 } );
				yTargets.push_back( { ( rects[j].y0 + rects[j].y1 ) * 0.5f, false, -1 } );
				yTargets.push_back( { rects[j].y1, false, -1 } );
			}

			const SnapOutcome snapX = SnapAxis( flDesiredMinX, flBoxW, xTargets, kSnapTolerancePx );
			const SnapOutcome snapY = SnapAxis( flDesiredMinY, flBoxH, yTargets, kSnapTolerancePx );

			float flFinalMinX = snapX.bSnapped ? snapX.flNewMin : flDesiredMinX;
			float flFinalMinY = snapY.bSnapped ? snapY.flNewMin : flDesiredMinY;
			flFinalMinX = std::clamp( flFinalMinX, 0.0f, std::max( 0.0f, shellSize.x - flBoxW ) );
			flFinalMinY = std::clamp( flFinalMinY, 0.0f, std::max( 0.0f, shellSize.y - flBoxH ) );

			if ( snapX.bSnapped ) { s_bSnapXActive = true; s_flGuideX = snapX.flGuide; }
			if ( snapY.bSnapped ) { s_bSnapYActive = true; s_flGuideY = snapY.flGuide; }

			// Recompute from the new top-left rather than accumulate a
			// delta onto x/y -- x/y describe the ORIGIN corner's point, not
			// the box, and the origin can itself change this same frame
			// (a screen-edge/centre snap below), so the only correct
			// source is "where is the box now."
			config::HudLayoutModule &m = ModuleAt( s_Working, idx );
			int nVert = 0, nHoriz = 0;
			ParsePlacement( m.origin, nVert, nHoriz );
			// Only a SCREEN anchor rewrites the origin -- snapping to
			// another module's edge aligns the box without telling the
			// layout anything new about where on the SCREEN it belongs
			// (see this file's header / hud-layouts.md for why a screen
			// anchor is the one case this matters: it is what keeps the
			// placement correct at a different resolution).
			if ( snapX.bSnapped && snapX.bScreen ) nHoriz = snapX.nHalf;
			if ( snapY.bSnapped && snapY.bScreen ) nVert  = snapY.nHalf;

			const ImVec2 hudMin( flFinalMinX / flScaleX, flFinalMinY / flScaleY );
			const ImVec2 pointHud( hudMin.x + hudRects[idx].w * ( nHoriz * 0.5f ),
			                       hudMin.y + hudRects[idx].h * ( nVert  * 0.5f ) );

			m.x = std::clamp( pointHud.x / flHudW, 0.0f, 1.0f );
			m.y = std::clamp( pointHud.y / flHudH, 0.0f, 1.0f );
			m.origin = kPlacements[ nVert ][ nHoriz ];

			// This frame's own drawing (below) uses `rects[idx]` -- update
			// it to the post-snap/post-clamp position so the box does not
			// lag one frame behind the input that moved it.
			rects[idx].x0 = flFinalMinX;
			rects[idx].y0 = flFinalMinY;
			rects[idx].x1 = flFinalMinX + flBoxW;
			rects[idx].y1 = flFinalMinY + flBoxH;
		}

		// =================================================================
		//  Draw. One full-surface window, no title bar/background of its
		//  own -- same shape as Shell.cpp's own launcher window (D25) --
		//  so Save/Cancel are real ImGui items (hover/click for free) while
		//  everything else paints straight onto its draw list.
		// =================================================================
		ImGui::SetNextWindowPos( ImVec2( 0.0f, 0.0f ) );
		ImGui::SetNextWindowSize( shellSize );
		ImGui::SetNextWindowFocus();
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );

		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBackground;

		if ( ImGui::Begin( "##hudlayouteditor", nullptr, flags ) )
		{
			ImDrawList *pDraw = ImGui::GetWindowDrawList();

			// A LIGHT scrim -- the game and the live HUD both have to stay
			// readable underneath, unlike the palette's own near-opaque
			// scrim (Shell.cpp's DrawPalette), which has a settings sheet
			// to separate itself from rather than content the user is
			// actively trying to place things over.
			pDraw->AddRectFilled( ImVec2( 0.0f, 0.0f ), shellSize, IM_COL32( 0, 0, 0, 70 ) );

			// Guide lines -- drawn UNDER the boxes so a box's own outline
			// stays the crisper line where the two cross.
			if ( s_bSnapXActive )
				pDraw->AddLine( ImVec2( s_flGuideX, 0.0f ), ImVec2( s_flGuideX, shellSize.y ), Accent( 0.6f ), Hairline() );
			if ( s_bSnapYActive )
				pDraw->AddLine( ImVec2( 0.0f, s_flGuideY ), ImVec2( shellSize.x, s_flGuideY ), Accent( 0.6f ), Hairline() );

			for ( int i = 0; i < 4; ++i )
			{
				if ( !rects[i].bEnabled )
					continue;

				const ImVec2 rmin( rects[i].x0, rects[i].y0 );
				const ImVec2 rmax( rects[i].x1, rects[i].y1 );
				const bool bDragging = s_nDragging == i;
				const bool bHovered  = !bDragging && s_nDragging < 0 &&
					mousePos.x >= rmin.x && mousePos.x <= rmax.x &&
					mousePos.y >= rmin.y && mousePos.y <= rmax.y;

				const ImU32 fillCol = bDragging ? Accent( 0.16f ) : bHovered ? Accent( 0.08f ) : IM_COL32( 0, 0, 0, 0 );
				if ( fillCol & IM_COL32_A_MASK )
					pDraw->AddRectFilled( rmin, rmax, fillCol, 2.0f );

				const ImU32 lineCol = bDragging ? Accent( 1.0f ) : bHovered ? Accent( 0.8f ) : Col( Role::LineControl );
				pDraw->AddRect( rmin, rmax, lineCol, 2.0f, 0, bDragging ? 2.0f * Hairline() : Hairline() );

				const ImVec2 textSize = ImGui::CalcTextSize( kModuleNames[i] );
				const ImVec2 textPos(
					( rmin.x + rmax.x ) * 0.5f - textSize.x * 0.5f,
					( rmin.y + rmax.y ) * 0.5f - textSize.y * 0.5f );
				pDraw->AddText( textPos, Col( Role::TextPrimary ), kModuleNames[i] );
			}

			// ---- chrome bar --------------------------------------------
			pDraw->AddRectFilled( ImVec2( 0.0f, 0.0f ), ImVec2( shellSize.x, flChromeH ), Col( Role::Surface ) );
			pDraw->AddLine( ImVec2( 0.0f, flChromeH ), ImVec2( shellSize.x, flChromeH ), Col( Role::LineRegion ), Hairline() );

			ImGui::SetCursorScreenPos( ImVec2( Px( 16.0f ), flChromeH * 0.5f - ImGui::GetFrameHeight() * 0.5f ) );
			ImGui::BeginGroup();
			if ( ImGui::Button( "Save" ) )
				Save();
			ImGui::SameLine();
			if ( ImGui::Button( "Cancel" ) )
				Cancel();
			ImGui::SameLine();
			ImGui::TextDisabled( "Esc to cancel" );
			ImGui::EndGroup();
		}
		ImGui::End();

		ImGui::PopStyleVar( 2 );
	}
}
