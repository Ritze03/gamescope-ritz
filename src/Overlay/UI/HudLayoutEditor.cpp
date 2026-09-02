// HUD layouts Phase 3 -- see HudLayoutEditor.h for the file-level design
// note (why this lives in the Shell's ImGui context, not the HUD's).

#include "HudLayoutEditor.h"

#include "Colors.h"
#include "Controls.h"
#include "Tokens.h"

#include "Overlay/Fonts.h"
#include "Overlay/FpsDisplay.h"
#include "Config/ConfigManager.h"
#include "steamcompmgr.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
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
		// Atomic because it is read from the COMPOSITE thread as well as the
		// Shell's own: FpsDisplay_AddLayer()'s edit-mode zpos and
		// SettingsOverlay.cpp's edit-mode blur suppression both branch on
		// IsActive() while Begin()/Save()/Cancel() write it from the overlay
		// side. A torn read is impossible for a bool, but the data race was
		// real UB; relaxed ordering is enough since nothing else is published
		// through it.
		std::atomic<bool> s_bActive{ false };
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

		// -----------------------------------------------------------------
		// The editing affordance's own geometry (base units, Px()-scaled --
		// see Tokens.h -- so the editor's chrome tracks display_scale exactly
		// like the Shell's does).
		//
		// `Why:` a module's TRUE extent is tiny -- roughly 95x40 physical px
		// at the default 18px HUD font -- which is neither readable nor
		// aimable as a drag target, and all four modules default to the same
		// (0,0) top-left, so at true extent they stack into one illegible
		// clump. The drawn box is therefore the AFFORDANCE, not a mirror of
		// the module's pixel extent: it is the module's natural rect grown to
		// a floor of kMinBoxW x kMinBoxH, ANCHORED AT THE NATURAL TOP-LEFT
		// and growing right/down only. That anchor is what keeps "what you
		// drag" and "where the module lands" the same point -- the drawn
		// box's top-left IS the module's top-left, at every size. Everything
		// that has to be true of the real module rather than of the
		// affordance -- snap targets, the on-screen clamp, and the x/y
		// write-back -- is computed from the NATURAL rect, so a padded box is
		// free to overhang the screen edge while the module it stands for
		// still reaches that edge exactly.
		// -----------------------------------------------------------------
		constexpr float kMinBoxWBase       = 140.0f;
		constexpr float kMinBoxHBase       =  52.0f;
		constexpr float kBoxRoundBase      =   4.0f;
		constexpr float kBoxStrokeBase     =   1.5f;  // idle outline
		constexpr float kBoxStrokeHotBase  =   2.5f;  // hovered / dragging outline

		// Coincident-stack cascade. Modules whose natural top-left agree
		// within kCoincidentEpsBase are drawn stepped down-right by
		// kCascadeStepBase each, in kModuleOrder order, so a stack reads as
		// several boxes and every one of them has an exposed corner to grab.
		// DRAWING AND HIT-TESTING ONLY -- the offset is never added to the
		// natural rect and therefore never reaches HudLayoutModule::x/y.
		constexpr float kCascadeStepBase    = 18.0f;
		constexpr float kCoincidentEpsBase  =  6.0f;

		// The always-drawn placement reference: a margin frame and a centre
		// cross, so the editor reads as a coordinate space even before
		// anything is dragged.
		constexpr float kGuideMarginBase = 24.0f;

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
		// `n*` is the module's TRUE rect (Shell space) -- the only thing
		// snapping, clamping and the x/y write-back ever look at. `d*` is the
		// drawn/hit-tested affordance: the natural rect floored to the
		// minimum box size and shifted by this frame's cascade offset. See
		// kMinBoxWBase's note above for why the two are separate.
		struct ModuleBox
		{
			bool  bEnabled;
			float nx0, ny0, nx1, ny1;
			float dx0, dy0, dx1, dy1;
			float flCascade;
		};

		void RefreshDrawBox( ModuleBox &b )
		{
			b.dx0 = b.nx0 + b.flCascade;
			b.dy0 = b.ny0 + b.flCascade;
			b.dx1 = b.dx0 + std::max( b.nx1 - b.nx0, Px( kMinBoxWBase ) );
			b.dy1 = b.dy0 + std::max( b.ny1 - b.ny0, Px( kMinBoxHBase ) );
		}

		// The cascade for a coincident stack. The module currently being
		// dragged is deliberately excluded (offset 0, and it does not count
		// towards anyone else's): a drag records its grab offset against the
		// NATURAL top-left, so a cascade that changed as the module left the
		// stack would make the box jump under the pointer mid-drag. Excluded
		// from the start, the only shift is one instant at mouse-down, which
		// reads as picking the box up off the pile.
		void AssignCascade( ModuleBox ( &boxes )[4], int nDragging )
		{
			const float flEps  = Px( kCoincidentEpsBase );
			const float flStep = Px( kCascadeStepBase );
			for ( int i = 0; i < 4; ++i )
			{
				int nStack = 0;
				if ( boxes[i].bEnabled && i != nDragging )
				{
					for ( int j = 0; j < i; ++j )
					{
						if ( !boxes[j].bEnabled || j == nDragging )
							continue;
						if ( std::fabs( boxes[j].nx0 - boxes[i].nx0 ) <= flEps &&
						     std::fabs( boxes[j].ny0 - boxes[i].ny0 ) <= flEps )
							++nStack;
					}
				}
				boxes[i].flCascade = flStep * (float)nStack;
				RefreshDrawBox( boxes[i] );
			}
		}

		// -----------------------------------------------------------------
		// Chrome bar geometry (Save / Cancel / the "Esc to cancel" hint).
		//
		// Bottom-centre, sized to its own content rather than a full-width
		// strip. `Why:` every module's DEFAULT placement is x=0,y=0,
		// origin="top-left" (ConfigSchema.h), and at the default font size
		// a module's box is barely 40px tall -- entirely inside a top
		// strip reserved for chrome, which made every module ungrabbable
		// on a fresh config. Bottom-centre is the region a top-left-
		// anchored module is least likely to ever occupy.
		//
		// Computed from ImGui's own style/font metrics (CalcTextSize +
		// FramePadding/ItemSpacing) rather than drawn first and measured --
		// the input-decide pass below needs this rect BEFORE any widget is
		// submitted, same "decide input before a pixel is painted"
		// ordering as the module grab-gate itself. Buttons are then placed
		// at the identical position with default (unsized) ImGui::Button()
		// calls, so the drawn bar and this computed rect never disagree.
		// -----------------------------------------------------------------
		struct ChromeRect { float x0, y0, x1, y1; };

		constexpr float kChromeBarPadXBase   = 16.0f; // base units, see Tokens.h's Px()
		constexpr float kChromeBarPadYBase   = 10.0f;
		// The buttons' own frame padding / gap, pushed as style vars around
		// BOTH the measure below and the draw, so the computed rect and the
		// painted bar cannot disagree (same rule ComputeChromeRect already
		// followed for the default style).
		constexpr float kChromeBtnPadXBase   = 14.0f;
		constexpr float kChromeBtnPadYBase   =  7.0f;
		constexpr float kChromeItemGapBase   = 12.0f;
		constexpr float kChromeMarginBotBase = 24.0f; // gap from the screen's bottom edge
		constexpr float kChromeRounding      = 6.0f;  // physical px, same convention as this file's own 2.0f module-box rounding

		ChromeRect ComputeChromeRect( const ImVec2 &shellSize )
		{
			// Measured under the same font and the same FramePadding/
			// ItemSpacing the buttons are drawn with -- Draw() pushes both
			// before calling this and pops them after the bar is painted.
			const ImGuiStyle &style = ImGui::GetStyle();
			const float flSaveW   = ImGui::CalcTextSize( "Save" ).x   + style.FramePadding.x * 2.0f;
			const float flCancelW = ImGui::CalcTextSize( "Cancel" ).x + style.FramePadding.x * 2.0f;
			const ImVec2 escSize  = ImGui::CalcTextSize( "Esc to cancel" );
			const float flBtnH    = ImGui::GetFrameHeight();

			const float flContentW = flSaveW + style.ItemSpacing.x + flCancelW + style.ItemSpacing.x + escSize.x;
			const float flContentH = std::max( flBtnH, escSize.y );

			const float flBarW = flContentW + Px( kChromeBarPadXBase ) * 2.0f;
			const float flBarH = flContentH + Px( kChromeBarPadYBase ) * 2.0f;

			ChromeRect r;
			r.x0 = ( shellSize.x - flBarW ) * 0.5f;
			r.x1 = r.x0 + flBarW;
			r.y1 = shellSize.y - Px( kChromeMarginBotBase );
			r.y0 = r.y1 - flBarH;
			return r;
		}

		bool PointInRect( const ChromeRect &r, const ImVec2 &p )
		{
			return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1;
		}

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

		ModuleBox boxes[4];
		for ( int i = 0; i < 4; ++i )
		{
			boxes[i].bEnabled = hudRects[i].bEnabled;
			boxes[i].nx0 = hudRects[i].x * flScaleX;
			boxes[i].ny0 = hudRects[i].y * flScaleY;
			boxes[i].nx1 = ( hudRects[i].x + hudRects[i].w ) * flScaleX;
			boxes[i].ny1 = ( hudRects[i].y + hudRects[i].h ) * flScaleY;
			boxes[i].flCascade = 0.0f;
			RefreshDrawBox( boxes[i] );
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

		AssignCascade( boxes, s_nDragging );

		// Chrome type and metrics, pushed for the WHOLE frame: the measure
		// (ComputeChromeRect, immediately below, which the grab gate needs
		// before any widget exists) and the draw (the buttons themselves,
		// far below) must see the identical font and style, or the hit-
		// tested bar and the painted bar are two different rects. Popped
		// once, after ImGui::End().
		//
		// `Why:` the buttons used to draw in the atlas's default font at its
		// own baked 11.5px, which is ~10 physical px of chrome on a 1280x800
		// screen. The Shell's Label type role is the size the rest of the
		// overlay's body text uses, and it scales with display_scale.
		ImGui::PushFont( fonts::Get( fonts::Style::Label ), fonts::RasterSize( TypeSizePx( TypeRole::Label ) ) );
		ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( Px( kChromeBtnPadXBase ), Px( kChromeBtnPadYBase ) ) );
		ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( Px( kChromeItemGapBase ), Px( kChromeBtnPadYBase ) ) );

		// The chrome bar (drawn below) claims its own compact rect, wherever
		// bottom-centre puts it -- keep grabs out of THAT rect specifically
		// (not a reserved screen strip) without depending on ImGui
		// hover-state ordering against widgets this same frame hasn't
		// submitted yet. A click anywhere else, including the top strip
		// where every module defaults to, is free to start a drag; a
		// module parked underneath the bar is grabbable outside the bar's
		// own rect, and the bar always wins inside it so Save/Cancel stay
		// clickable.
		const ChromeRect chrome = ComputeChromeRect( shellSize );

		if ( s_nDragging < 0 && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !PointInRect( chrome, mousePos ) )
		{
			// SMALLEST box containing the point wins, not topmost. `Why:`
			// with every module defaulting to (0,0) the boxes overlap
			// heavily, and a plain topmost-first test makes a small box
			// stacked under a large one permanently unreachable -- the large
			// one swallows every click inside it. Smallest-first is the
			// standard fix (it is what makes nested hit targets selectable);
			// the cascade above then guarantees each box in a coincident
			// stack has an exposed corner too. Equal areas tie to the later
			// index, preserving kModuleOrder's own "later draws on top".
			int   nPick = -1;
			float flPickArea = 0.0f;
			for ( int i = 0; i < 4; ++i )
			{
				if ( !boxes[i].bEnabled )
					continue;
				if ( mousePos.x < boxes[i].dx0 || mousePos.x > boxes[i].dx1 ||
				     mousePos.y < boxes[i].dy0 || mousePos.y > boxes[i].dy1 )
					continue;
				const float flArea = ( boxes[i].dx1 - boxes[i].dx0 ) * ( boxes[i].dy1 - boxes[i].dy0 );
				if ( nPick < 0 || flArea <= flPickArea )
				{
					nPick = i;
					flPickArea = flArea;
				}
			}
			if ( nPick >= 0 )
			{
				s_nDragging = nPick;
				// Against the NATURAL top-left, not the drawn one: the
				// natural rect is what the drag arithmetic below moves, and
				// the dragged module's cascade is dropped to 0 on the very
				// next line.
				s_GrabOffsetShell = ImVec2( mousePos.x - boxes[nPick].nx0, mousePos.y - boxes[nPick].ny0 );
				AssignCascade( boxes, s_nDragging );
			}
		}

		s_bSnapXActive = s_bSnapYActive = false;

		if ( s_nDragging >= 0 )
		{
			const int idx = s_nDragging;
			// The module's REAL size, never the padded affordance: it is what
			// the snap references and the on-screen clamp have to be true of,
			// so a small module can still be pushed flush against the screen
			// edge even though its box overhangs it.
			const float flBoxW = boxes[idx].nx1 - boxes[idx].nx0;
			const float flBoxH = boxes[idx].ny1 - boxes[idx].ny0;
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
				if ( j == idx || !boxes[j].bEnabled )
					continue;
				xTargets.push_back( { boxes[j].nx0, false, -1 } );
				xTargets.push_back( { ( boxes[j].nx0 + boxes[j].nx1 ) * 0.5f, false, -1 } );
				xTargets.push_back( { boxes[j].nx1, false, -1 } );
				yTargets.push_back( { boxes[j].ny0, false, -1 } );
				yTargets.push_back( { ( boxes[j].ny0 + boxes[j].ny1 ) * 0.5f, false, -1 } );
				yTargets.push_back( { boxes[j].ny1, false, -1 } );
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

			// This frame's own drawing (below) uses `boxes[idx]` -- update
			// it to the post-snap/post-clamp position so the box does not
			// lag one frame behind the input that moved it.
			boxes[idx].nx0 = flFinalMinX;
			boxes[idx].ny0 = flFinalMinY;
			boxes[idx].nx1 = flFinalMinX + flBoxW;
			boxes[idx].ny1 = flFinalMinY + flBoxH;
			RefreshDrawBox( boxes[idx] );
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

			// A VERY light scrim. The game underneath is the thing the user
			// is placing a HUD over, so it has to stay readable -- unlike the
			// palette's own near-opaque scrim (Shell.cpp's DrawPalette),
			// which has a settings sheet to separate itself from rather than
			// content the user is actively aiming at. The Shell's
			// background_blur pass is suppressed outright for the same reason
			// (SettingsOverlay.cpp, `hudedit::IsActive()`).
			pDraw->AddRectFilled( ImVec2( 0.0f, 0.0f ), shellSize, IM_COL32( 0, 0, 0, 38 ) );

			// ---- the placement surface itself --------------------------
			// A margin frame and a centre cross, drawn ALWAYS rather than
			// only mid-drag, so the editor states its coordinate space up
			// front: this is where the screen's centre is, and this is how
			// close to the edge a module can sensibly sit. Role::Line (white
			// 10%) is the kit's own decorative-rule colour -- deliberately
			// the faintest thing on screen; it is background reference, not
			// chrome.
			{
				const float flMargin = Px( kGuideMarginBase );
				const ImU32  refCol  = Col( Role::Line );
				const float  flRef   = Hairline();
				if ( shellSize.x > flMargin * 2.0f && shellSize.y > flMargin * 2.0f )
					pDraw->AddRect( ImVec2( flMargin, flMargin ),
					                ImVec2( shellSize.x - flMargin, shellSize.y - flMargin ),
					                refCol, 0.0f, 0, flRef );
				const float flCx = shellSize.x * 0.5f;
				const float flCy = shellSize.y * 0.5f;
				pDraw->AddLine( ImVec2( flCx, 0.0f ), ImVec2( flCx, shellSize.y ), refCol, flRef );
				pDraw->AddLine( ImVec2( 0.0f, flCy ), ImVec2( shellSize.x, flCy ), refCol, flRef );
			}

			// Snap guide lines -- drawn UNDER the boxes so a box's own
			// outline stays the crisper line where the two cross.
			if ( s_bSnapXActive )
				pDraw->AddLine( ImVec2( s_flGuideX, 0.0f ), ImVec2( s_flGuideX, shellSize.y ), Accent( 0.7f ), Hairline() );
			if ( s_bSnapYActive )
				pDraw->AddLine( ImVec2( 0.0f, s_flGuideY ), ImVec2( shellSize.x, s_flGuideY ), Accent( 0.7f ), Hairline() );

			const float flRound = Px( kBoxRoundBase );

			for ( int i = 0; i < 4; ++i )
			{
				if ( !boxes[i].bEnabled )
					continue;

				const ImVec2 rmin( boxes[i].dx0, boxes[i].dy0 );
				const ImVec2 rmax( boxes[i].dx1, boxes[i].dy1 );
				const bool bDragging = s_nDragging == i;
				const bool bHovered  = !bDragging && s_nDragging < 0 &&
					mousePos.x >= rmin.x && mousePos.x <= rmax.x &&
					mousePos.y >= rmin.y && mousePos.y <= rmax.y;

				// Three states, and every one of them is a filled, outlined
				// box: an idle box that drew as outline-only was invisible
				// against a bright game frame, which is what made the whole
				// editor read as a smudge. The flat black backing under the
				// accent tint is what guarantees the label's contrast
				// whatever is behind it.
				pDraw->AddRectFilled( rmin, rmax, IM_COL32( 0, 0, 0, bDragging ? 150 : 120 ), flRound );
				pDraw->AddRectFilled( rmin, rmax,
					Accent( bDragging ? 0.30f : bHovered ? 0.20f : 0.12f ), flRound );

				const ImU32 lineCol = bDragging ? Accent( 1.0f )
				                    : bHovered  ? Accent( 0.85f )
				                                : Col( Role::LineControl );
				pDraw->AddRect( rmin, rmax, lineCol, flRound, 0,
					std::max( Hairline(), Px( bDragging || bHovered ? kBoxStrokeHotBase : kBoxStrokeBase ) ) );

				// The module's TRUE extent, faintly, whenever the affordance
				// is bigger than it -- so "the box is a handle, the module is
				// this" is visible rather than a rule in a doc. Shares the
				// box's top-left, which is the whole point of anchoring the
				// minimum size there.
				const float flNatW = boxes[i].nx1 - boxes[i].nx0;
				const float flNatH = boxes[i].ny1 - boxes[i].ny0;
				if ( flNatW > 1.0f && flNatH > 1.0f &&
				     ( rmax.x - rmin.x - flNatW > 1.0f || rmax.y - rmin.y - flNatH > 1.0f ) )
				{
					pDraw->AddRect( rmin, ImVec2( rmin.x + flNatW, rmin.y + flNatH ),
					                Accent( 0.35f ), 0.0f, 0, Hairline() );
				}

				// The Shell's own body type, not the HUD's font size: this
				// label names a target in the EDITOR's UI, so it belongs to
				// the editor's type ladder (Tokens.h's TypeRole) and has to
				// stay readable no matter how small hud.font_size is.
				const float flPad = Px( tok::kS );
				ui::DrawText( ImRect( ImVec2( rmin.x + flPad, rmin.y ), ImVec2( rmax.x - flPad, rmax.y ) ),
				              TypeRole::Label, Col( Role::TextPrimary ), kModuleNames[i], TextAlign::Center );
			}

			// ---- chrome bar: bottom-centre, sized to its own buttons ----
			// (see ComputeChromeRect's own comment for the why).
			const ImVec2 chromeMin( chrome.x0, chrome.y0 );
			const ImVec2 chromeMax( chrome.x1, chrome.y1 );
			pDraw->AddRectFilled( chromeMin, chromeMax, Col( Role::Surface ), kChromeRounding );
			pDraw->AddRect( chromeMin, chromeMax, Col( Role::LineRegion ), kChromeRounding, 0, Hairline() );

			ImGui::SetCursorScreenPos( ImVec2(
				chrome.x0 + Px( kChromeBarPadXBase ),
				chrome.y0 + Px( kChromeBarPadYBase ) ) );
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

		// Two window style vars above, plus the two chrome ones pushed
		// before the grab gate.
		ImGui::PopStyleVar( 4 );
		ImGui::PopFont();
	}
}
