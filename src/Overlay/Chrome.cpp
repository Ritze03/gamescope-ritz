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
// Title bar history: an earlier pass here deliberately kept ImGui's native
// title bar and added a second custom-drawn row underneath it (icon+status
// dot+meta), on the theory that reimplementing drag/collapse/close by hand
// wasn't worth it just to merge two rows into one. That compromise is what
// screenshot evidence showed as "the top bar is far too transparent" -- the
// native bar's own TitleBg/TitleBgActive fill (Widgets.cpp's `raised` token,
// white @ 4-5%) is nearly invisible against a bright game, and it reads as
// two thin, mismatched strips instead of one solid 34px bar. This pass
// removes the native title bar outright (ImGuiWindowFlags_NoTitleBar) and
// draws the whole spec §5 bar as one thing -- see DrawTitleBar() below for
// how drag/collapse/close are reimplemented on ImGui's own primitives
// (StartMouseMovingWindow, ButtonBehavior via InvisibleButton) rather than
// by hand from nothing.
#include "Chrome.h"

#include "Fonts.h"
#include "../SettingsOverlay.h"
#include "Config/ConfigManager.h"

#include <cmath>
#include <cstddef>

// ImVec2 operator+/- (used below by the status-dot glow, dock top-edge
// marker, hint-line shadow offset, and the custom title bar's own layout)
// aren't exported by imgui.h by default -- must be defined before *any*
// include of imgui.h in this translation unit, so Palette.h (which also
// includes imgui.h) is pulled in only after this, same ordering rule as
// Widgets.cpp.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
// imgui_internal.h for ImGui::StartMouseMovingWindow()/GetCurrentWindow() --
// the custom title bar's drag reimplementation calls the exact same
// internal primitive ImGui's own native title bar drag path uses (see
// imgui.cpp's RenderWindowTitleBarContents()/UpdateMouseMovingWindowNewFrame()
// callers), not a from-scratch mouse-delta hack. Same precedent as
// Widgets.cpp, which already includes this for ButtonBehavior()/
// SliderBehavior().
#include "imgui_internal.h"
#include "Palette.h"

namespace gamescope::palette
{
	// Definition for the extern declared in Palette.h -- see that header's
	// LiveTheme comment for why this specific storage lives here.
	LiveTheme g_LiveTheme;
}

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
		// itself (dock buttons, the title-bar sub-header, the dock's active
		// top-edge marker). Deliberately NOT written into ImGuiStyle.Colors[]
		// -- that shared array is Widgets.cpp's ApplyStyle() (issue #14
		// territory, see this file's top comment) -- these are only ever
		// passed straight to ImDrawList calls this file makes. Values come
		// from Palette.h (superdoc/planning/ui-mockup-precise-spec.md §1's
		// Color tokens table) -- see that header's comment for why the hex
		// lives there once instead of being re-typed per file.
		constexpr ImU32 kAccentU32         = gamescope::palette::kAccent;
		constexpr ImU32 kAccentIconU32     = gamescope::palette::kAccentIcon;
		// Dock icon idle/hover -- spec §2 "icon idle" dock row (.62), §12
		// hover invention (+80% on hover, applied here as the glyph's own
		// alpha rather than a full white@80%).
		constexpr ImU32 kIconIdleU32       = IM_COL32( 255, 255, 255, 158 ); // #EFF5FB @ 62%
		constexpr ImU32 kIconHoverU32      = IM_COL32( 255, 255, 255, 204 );
		constexpr ImU32 kHairlineU32       = IM_COL32( 255, 255, 255, 26 );  // #FFFFFF @ 10% -- spec §1 hairline
		constexpr ImU32 kHairlineStrongU32 = IM_COL32( 255, 255, 255, 46 );  // in-style hover invention
		constexpr ImU32 kDockIdleFillU32   = IM_COL32( 255, 255, 255, 11 );  // #FFFFFF @ 4.5% -- spec §8 dock idle fill
		constexpr ImU32 kDockHoverFillU32  = IM_COL32( 255, 255, 255, 20 );  // in-style hover invention

		constexpr float kTitleBarHeight = 34.0f; // spec §5

		// Display open, everything else closed on first-ever show -- a
		// reasonable non-overwhelming default, not a hard limit: see Chrome.h's
		// IsPanelOpen() comment, opening more no longer closes this one.
		bool s_bPanelOpen[(size_t)PanelId::Count] = { true, false, false, false, false };
		// Per-panel "shrunk to just the title bar" state -- this file's own
		// stand-in for ImGui's native window collapse, which only exists on
		// the native title bar path (ImGuiWindowFlags_NoTitleBar disables it
		// entirely, see imgui.cpp's Begin()). Toggled by DrawTitleBar()'s
		// collapse glyph and by double-clicking the bar's drag zone --
		// mirrors the native double-click-title-bar-to-collapse gesture.
		bool s_bPanelCollapsed[(size_t)PanelId::Count] = {};
		// One-frame-stale focus cache: spec §4's "unfocused windows: whole
		// window x94% opacity" (and this pass's focused-border *thickness*)
		// need to be applied via style pushes that ImGui::Begin() itself
		// consumes to draw the window background/border, before this
		// function can know the *new* frame's focus result. Using last
		// frame's cached focus state is a one-frame-late approximation on
		// the exact frame focus changes, imperceptible for a settings
		// panel, and avoids the alternative (a two-pass Begin) entirely.
		bool s_bPanelWasFocused[(size_t)PanelId::Count] = {};

		// Issue #34 state: resizable panels, opening ~50% larger than their
		// measured natural content size. s_nSizeMeasureFrame counts a
		// panel's first few frames spent in an ImGuiWindowFlags_AlwaysAutoResize
		// "priming" pass (see BeginPanelWindow()) before it's ever shown at a
		// real size; saturates at kSizeMeasureFrames once done, never used
		// again after that. s_lastExpandedSize is the single source of truth
		// for "what size should this window be while expanded" from then on
		// -- written once by the priming pass's own measurement, then kept
		// live every subsequent expanded frame from the window's actual
		// (possibly user-resized) size, so collapse/un-collapse always has
		// the right value to freeze/restore. s_bPanelWasCollapsedLastFrame
		// detects the un-collapse transition frame specifically (see its use
		// below) -- separate from s_bPanelWasFocused, which is cached for a
		// different reason (style pushes needing last frame's focus before
		// this frame's is knowable) despite the similar "one-frame-stale"
		// shape.
		int s_nSizeMeasureFrame[(size_t)PanelId::Count] = {};
		ImVec2 s_lastExpandedSize[(size_t)PanelId::Count] = {};
		bool s_bPanelWasCollapsedLastFrame[(size_t)PanelId::Count] = {};

		// Sensible non-overlapping default positions, one fixed slot per
		// PanelId, replacing whatever position each panel's own call site
		// used to hardcode (Chrome.h's BeginPanelWindow() comment explains
		// why: multiple panels can now be open at once, so their *first-ever*
		// positions actually have to not collide). A 3-column x 2-row tile
		// grid sized off io.DisplaySize (so it still spreads out sanely at
		// non-1920x1080 resolutions), generous enough that every panel's own
		// fixed width (380-500px, see each panel's own defaultSize argument)
		// fits inside its cell without touching its neighbor. Only ever
		// consulted the first time a given panel is shown (ImGuiCond_FirstUseEver,
		// in BeginPanelWindow()) -- once dragged, ImGui's own window state
		// remembers where the user put it for as long as this process's
		// ImGui context lives, satisfying "remember position while open"
		// without any extra bookkeeping here.
		ImVec2 TiledDefaultPos( PanelId id )
		{
			struct Slot { int col, row; };
			constexpr Slot kSlots[(size_t)PanelId::Count] = {
				{ 0, 0 }, // Display
				{ 1, 0 }, // Shaders
				{ 2, 0 }, // Fps
				{ 0, 1 }, // Audio
				{ 1, 1 }, // Config
			};

			constexpr float kMarginX = 40.0f;
			constexpr float kMarginY = 40.0f;
			constexpr float kRowHeight = 400.0f; // clears every panel's auto-height in row 0

			const ImGuiIO &io = ImGui::GetIO();
			const float flColWidth = ImMax( 460.0f, ( io.DisplaySize.x - kMarginX * 2.0f ) / 3.0f );

			const Slot &slot = kSlots[(size_t)id];
			return ImVec2( kMarginX + slot.col * flColWidth, kMarginY + slot.row * kRowHeight );
		}

		// ---- Live theme (General tab settings, see Palette.h's LiveTheme) --
		gamescope::palette::LiveTheme *pLiveTheme = &gamescope::palette::g_LiveTheme;
		bool s_bLiveThemeLoaded = false;

		void EnsureLiveThemeLoaded()
		{
			if ( s_bLiveThemeLoaded )
				return;
			s_bLiveThemeLoaded = true;

			// overlay.* is process-level/global-only (ConfigSchema.h's own
			// comment on OverlaySettings) -- deliberately config::LoadGlobal(),
			// never ResolveEffective(): a per-game override file is always
			// written with bIncludeOverlay=false (ConfigManager.cpp's
			// SettingsToJson), so resolving through the current session's
			// per-game file while an override is active would silently read
			// back compiled *defaults* for every one of these fields instead
			// of the user's real preference. Loaded exactly once per process
			// (LoadGlobal() does blocking file I/O -- ConfigManager.h's
			// threading note: not on the vblank-paced render loop);
			// PanelConfig.cpp's General tab is the only thing that changes
			// these again after this, and it writes straight into
			// gamescope::palette::g_LiveTheme itself on every edit -- no
			// generation-bump reload path to wire up the way the per-game
			// panels need, because profile-apply/override-toggle never touch
			// `overlay` at all (ConfigManager.h's ApplyProfile() doc comment).
			const config::Settings s = config::LoadGlobal();
			pLiveTheme->flDockScale = s.overlay.dock_scale;
			pLiveTheme->flDisplayScale = s.overlay.display_scale;
			pLiveTheme->flWindowAlphaFocused = s.overlay.opacity_windows_focused;
			pLiveTheme->flWindowAlphaUnfocused = s.overlay.opacity_windows_unfocused;
			pLiveTheme->flDockAlpha = s.overlay.opacity_dock;
			ImGui::GetIO().FontGlobalScale = s.overlay.display_scale;
		}

		// WindowBg/PopupBg's alpha is baked once at init by Widgets.cpp's
		// ApplyStyle() -- re-applied here every frame so General tab's
		// opacity_windows_focused/unfocused sliders (task requirement: "must
		// take effect live, not on restart") actually reach every popup/
		// combo/tooltip drawn with the shared style (nothing here has a
		// focus concept of its own, so they get the unfocused value as their
		// steady-state default). Panel windows themselves override this
		// per-window in BeginPanelWindow() below, using the same
		// one-frame-cached focus state that already drives their border-
		// alpha/thickness focus treatment.
		void ApplyLiveWindowAlpha()
		{
			ImGuiStyle &style = ImGui::GetStyle();
			style.Colors[ImGuiCol_WindowBg].w = pLiveTheme->flWindowAlphaUnfocused;
			style.Colors[ImGuiCol_PopupBg].w = pLiveTheme->flWindowAlphaUnfocused;
		}

		// One 18x18px hit box for the collapse/close glyph cluster --
		// ButtonBehavior()-based (via InvisibleButton, same primitive every
		// other custom widget in this overlay uses -- Widgets.cpp's
		// Toggle()/Checkbox(), DrawDockButton() below), so hover/press/
		// keyboard-nav semantics match a real ImGui button. Spec §5: "18x18px
		// hit boxes ... stroke #EFF5FB@45% (50% focused) ... no hover state
		// was designed -- use @80% on hover as the in-style invention".
		bool DrawTitleGlyphButton( ImDrawList *pDrawList, const char *pszId, ImVec2 pos, Icon icon, bool bFocused )
		{
			ImGui::SetCursorScreenPos( pos );
			const bool bClicked = ImGui::InvisibleButton( pszId, ImVec2( 18.0f, 18.0f ) );
			const bool bHovered = ImGui::IsItemHovered();
			const float flAlpha = bHovered ? 0.80f : ( bFocused ? 0.50f : 0.45f );
			DrawIcon( pDrawList, icon, pos + ImVec2( 9.0f, 9.0f ), 12.0f, ImGui::GetColorU32( gamescope::palette::White( flAlpha ) ) );
			return bClicked;
		}

		// The full spec §5 title bar, drawn as one literal 34px row of
		// window content flush against the window's top edge -- see this
		// file's top comment for why ImGui's native title bar is gone
		// outright rather than kept-and-decorated. Handles its own hit
		// testing for drag/collapse/close (Chrome.h's BeginPanelWindow()
		// comment has the full rationale for each).
		void DrawTitleBar( const char *pszTitle, PanelId id, bool bFocused, bool bCollapsed )
		{
			ImDrawList *pDrawList = ImGui::GetWindowDrawList();
			const ImVec2 windowPos = ImGui::GetWindowPos();
			const ImVec2 windowSize = ImGui::GetWindowSize();
			const float flContentLeftX = ImGui::GetCursorScreenPos().x; // Pos.x + WindowPadding.x, for handing back to the caller's own content below

			constexpr float kPadX = 12.0f; // spec §5 title-bar horizontal padding
			constexpr float kDotSize = 6.0f;
			constexpr float kButtonSize = 18.0f;
			constexpr float kButtonGap = 2.0f;

			const ImVec2 barMin = windowPos;
			const ImVec2 barMax( windowPos.x + windowSize.x, windowPos.y + kTitleBarHeight );

			// Fill: spec §5 vertical gradient, white 6%->1.5% unfocused,
			// accent 16%->white 2% focused. Bottom border 1px, white@10%
			// unfocused / accent@30% focused.
			const ImU32 gradTop = bFocused ? ImGui::GetColorU32( gamescope::palette::Accent( 0.16f ) ) : ImGui::GetColorU32( gamescope::palette::White( 0.06f ) );
			const ImU32 gradBot = bFocused ? ImGui::GetColorU32( gamescope::palette::White( 0.02f ) ) : ImGui::GetColorU32( gamescope::palette::White( 0.015f ) );
			pDrawList->AddRectFilledMultiColor( barMin, barMax, gradTop, gradTop, gradBot, gradBot );
			const ImU32 borderCol = bFocused ? ImGui::GetColorU32( gamescope::palette::Accent( 0.30f ) ) : ImGui::GetColorU32( gamescope::palette::White( 0.10f ) );
			pDrawList->AddLine( ImVec2( barMin.x, barMax.y ), ImVec2( barMax.x, barMax.y ), borderCol, 1.0f );

			const float flCenterY = windowPos.y + kTitleBarHeight * 0.5f;
			float flCursorX = windowPos.x + kPadX;

			// 6x6 square status dot -- spec §5: always "live" (this panel's
			// window is only ever drawn while its subsystem is on), glow
			// approximated as one 10x10 rect behind it @25% accent.
			const ImVec2 dotCenter( flCursorX + kDotSize * 0.5f, flCenterY );
			pDrawList->AddRectFilled( dotCenter - ImVec2( 5.0f, 5.0f ), dotCenter + ImVec2( 5.0f, 5.0f ), ImGui::GetColorU32( gamescope::palette::Accent( 0.25f ) ) );
			pDrawList->AddRectFilled( dotCenter - ImVec2( kDotSize * 0.5f, kDotSize * 0.5f ), dotCenter + ImVec2( kDotSize * 0.5f, kDotSize * 0.5f ), kAccentU32 );
			flCursorX += kDotSize + 10.0f; // spec §3: 10px row gap in title bars

			// Title -- Mono 600 11 UPPER, spec §2: @86% unfocused / 94% focused.
			ImGui::PushFont( fonts::Get( fonts::Style::Title ) );
			const ImVec2 titleSize = ImGui::CalcTextSize( pszTitle );
			pDrawList->AddText( ImVec2( flCursorX, flCenterY - titleSize.y * 0.5f ),
				ImGui::GetColorU32( gamescope::palette::Text( bFocused ? 0.94f : 0.86f ) ), pszTitle );
			flCursorX += titleSize.x + 10.0f;
			ImGui::PopFont();

			// Meta -- Mono 400 10.5 @30%, spec §5/§2.
			ImGui::PushFont( fonts::Get( fonts::Style::Meta ) );
			static constexpr const char *kMeta = "gamescope-ritz";
			const ImVec2 metaSize = ImGui::CalcTextSize( kMeta );
			pDrawList->AddText( ImVec2( flCursorX, flCenterY - metaSize.y * 0.5f ),
				ImGui::GetColorU32( gamescope::palette::White( 0.30f ) ), kMeta );
			ImGui::PopFont();

			// Glyph button cluster, right-aligned: close then collapse
			// reading left-to-right (i.e. collapse sits left of close),
			// matching spec §5's own listed order ("... -> glyph buttons"
			// with collapse drawn before close in every mockup screenshot's
			// left-to-right reading).
			const ImVec2 closePos( barMax.x - kPadX - kButtonSize, windowPos.y + ( kTitleBarHeight - kButtonSize ) * 0.5f );
			const ImVec2 collapsePos( closePos.x - kButtonGap - kButtonSize, closePos.y );

			if ( DrawTitleGlyphButton( pDrawList, "##collapse", collapsePos, Icon::Collapse, bFocused ) )
				s_bPanelCollapsed[(size_t)id] = !s_bPanelCollapsed[(size_t)id];
			if ( DrawTitleGlyphButton( pDrawList, "##close", closePos, Icon::Close, bFocused ) )
				SetPanelOpen( id, false );

			// Drag zone: the whole bar minus the button cluster. Calling
			// ImGui::StartMouseMovingWindow() on click-down is the same
			// internal primitive ImGui's own native title bar drag uses
			// (imgui.cpp) -- not a from-scratch mouse-delta reimplementation,
			// so multi-viewport clamping/focus-on-move/etc. all come along
			// for free exactly like a real title bar. A double-click toggles
			// collapse, mirroring the native double-click-title-bar gesture
			// (imgui.cpp's own WantCollapseToggle path, which this window
			// can't reach any more once ImGuiWindowFlags_NoTitleBar is set --
			// see Chrome.h's BeginPanelWindow() comment).
			ImGui::SetCursorScreenPos( barMin );
			ImGui::InvisibleButton( "##titledrag", ImVec2( collapsePos.x - kButtonGap - barMin.x, kTitleBarHeight ) );
			if ( ImGui::IsItemHovered() )
			{
				if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
					ImGui::StartMouseMovingWindow( ImGui::GetCurrentWindow() );
				if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
					s_bPanelCollapsed[(size_t)id] = !s_bPanelCollapsed[(size_t)id];
			}

			(void)bCollapsed; // no distinct collapsed-state paint on the bar itself yet -- same icon/geometry either way

			ImGui::SetCursorScreenPos( ImVec2( flContentLeftX, barMax.y + ImGui::GetStyle().WindowPadding.y ) );
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

	bool BeginPanelWindow( const char *pszTitle, PanelId id, ImVec2 /*defaultPos*/, ImVec2 defaultSize )
	{
		if ( !IsPanelOpen( id ) )
			return false;

		EnsureLiveThemeLoaded();
		ApplyLiveWindowAlpha();

		const bool bCollapsed = s_bPanelCollapsed[(size_t)id];

		ImGui::SetNextWindowPos( TiledDefaultPos( id ), ImGuiCond_FirstUseEver );

		// Issue #34: resizable windows, opening ~50% larger than their
		// natural content size (width real growth; height content-driven
		// with only breathing-room padding -- see kHeightPad below, and the
		// measurement-finish block after Begin() for why height isn't also
		// just multiplied by 1.5). "Natural" is measured, not guessed: the
		// first kSizeMeasureFrames frames a panel is ever shown, it's drawn
		// with ImGuiWindowFlags_AlwaysAutoResize (below) and no
		// SetNextWindowSize call at all, so ImGui fits the window to this
		// panel's own real widget layout. ImGui computes each frame's
		// auto-fit size from the PREVIOUS frame's submitted content
		// (imgui.cpp's CalcWindowAutoFitSize(), called at End()), so more
		// than one frame is needed before a reading is trustworthy -- 3
		// gives it margin over simple, static panel layouts like these.
		// Once measured, the result seeds s_lastExpandedSize exactly once
		// (ImGuiCond_FirstUseEver below), after which ordinary ImGui
		// window-size persistence takes over and the user's own resizes
		// stick -- no per-frame re-assertion fighting them back, unlike the
		// old fixed-size scheme this replaces.
		constexpr int kSizeMeasureFrames = 3;
		constexpr float kHeightPad = 48.0f; // breathing room once content is already the limiting factor

		int &nMeasureFrame = s_nSizeMeasureFrame[(size_t)id];
		ImVec2 &lastExpandedSize = s_lastExpandedSize[(size_t)id];
		bool &bWasCollapsed = s_bPanelWasCollapsedLastFrame[(size_t)id];

		const bool bPriming = !bCollapsed && nMeasureFrame < kSizeMeasureFrames;

		if ( bCollapsed )
		{
			// Shrunk to just the title bar (s_bPanelCollapsed's own stand-in
			// for native collapse) -- width follows whatever the window was
			// last expanded to, not defaultSize, so shading doesn't silently
			// change a window's width out from under the user. Always: this
			// is one of the two collapse-toggle transition directions the
			// task's acceptance criteria still wants Always for (the other
			// is the un-collapse restore just below).
			const float flWidth = lastExpandedSize.x > 0.0f ? lastExpandedSize.x : defaultSize.x * 1.5f;
			ImGui::SetNextWindowSize( ImVec2( flWidth, kTitleBarHeight ), ImGuiCond_Always );
		}
		else if ( !bPriming && bWasCollapsed )
		{
			// Un-collapsing this frame: restore exactly what the window was
			// before shading, rather than snapping to the opening size
			// (task's own "not left in an inconsistent size after
			// un-shading").
			ImGui::SetNextWindowSize( lastExpandedSize, ImGuiCond_Always );
		}
		else if ( !bPriming )
		{
			// Steady state (including the first real frame right after
			// measurement finishes): seed with the last known expanded size.
			// ImGuiCond_FirstUseEver makes every call here a no-op once
			// consumed once, exactly like SetNextWindowPos() above already
			// does every frame for position -- so the user's own drag-resize
			// (ImGuiWindowFlags_NoResize is gone below) sticks across
			// frames.
			ImGui::SetNextWindowSize( lastExpandedSize, ImGuiCond_FirstUseEver );
		}
		// else: priming -- ImGuiWindowFlags_AlwaysAutoResize (below) drives
		// size entirely this frame; no SetNextWindowSize call at all.

		// Spec §4: window corner radius 4px (controls stay flat/0px --
		// that's Widgets.cpp's ApplyStyle(), unaffected by this
		// window-scoped push).
		ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 4.0f );
		// Spec §4 "unfocused windows: whole window x94% opacity", and this
		// pass's own thicker focused border (2px vs the shared 1px hairline
		// -- spec's 42%-alpha border alone tested as too subtle, task
		// feedback: "make focus unmistakable") -- both read last frame's
		// cached focus (s_bPanelWasFocused's comment above explains why:
		// Begin() itself draws using these before this frame's focus is
		// knowable).
		const bool bWasFocused = s_bPanelWasFocused[(size_t)id];
		ImGui::PushStyleVar( ImGuiStyleVar_Alpha, bWasFocused ? 1.0f : 0.94f );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, bWasFocused ? 2.0f : 1.0f );

		// Focused-vs-unfocused window opacity split (opacity_windows_focused/
		// opacity_windows_unfocused, ConfigSchema.h) -- overrides the shared
		// WindowBg alpha ApplyLiveWindowAlpha() just set (that stays the
		// popup/combo default, which has no per-window focus concept) for
		// exactly this window. Must be pushed before Begin(), which reads
		// WindowBg immediately to paint the background; same one-frame-
		// cached bWasFocused the Alpha/WindowBorderSize pushes above already
		// use, for the same reason (this frame's real focus isn't knowable
		// until after Begin()).
		{
			ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
			bg.w = bWasFocused ? pLiveTheme->flWindowAlphaFocused : pLiveTheme->flWindowAlphaUnfocused;
			ImGui::PushStyleColor( ImGuiCol_WindowBg, bg );
		}

		// No native title bar (see this file's top comment) -- DrawTitleBar()
		// below draws the whole thing as content instead. No native p_open:
		// the close glyph calls SetPanelOpen() itself, and the native close-
		// button path lives inside RenderWindowTitleBarContents(), which
		// Begin() never calls once NoTitleBar is set.
		//
		// Issue #34: resizable, so NoResize is no longer unconditional --
		// still forced while collapsed (frozen at title-bar height, no grip
		// to offer -- resizing a shaded window would either do nothing
		// visible or leave it inconsistent once un-shaded, so this just
		// doesn't offer the grip at all) and while priming (AlwaysAutoResize
		// already owns sizing those frames; NoResize would conflict with it).
		//
		// NoScrollbar/NoScrollWithMouse: this outer window must never scroll
		// itself -- see the "##body" child BeginChild() below for why. Scroll
		// (wheel or scrollbar-grip drag alike) lives entirely on that child
		// now.
		ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		if ( bCollapsed )
			windowFlags |= ImGuiWindowFlags_NoResize;
		else if ( bPriming )
			windowFlags |= ImGuiWindowFlags_AlwaysAutoResize;
		ImGui::Begin( pszTitle, nullptr, windowFlags );

		const bool bFocused = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
		s_bPanelWasFocused[(size_t)id] = bFocused;

		// Issue #34 bookkeeping: advance/finish the measurement pass, or
		// (once past it) keep s_lastExpandedSize tracking the window's real,
		// possibly user-resized size every frame -- so a later collapse, or
		// a later reopen after being closed at the dock, always has the
		// right value to freeze/restore.
		if ( bPriming )
		{
			if ( ++nMeasureFrame == kSizeMeasureFrames )
			{
				// This frame's GetWindowSize() is ImGui's own converged
				// auto-fit result for the panel's actual widget layout --
				// the "measured natural size" this issue asks for, not a
				// guess. Width: flat 1.5x, real growth for breathing room.
				// Height: capped to content-plus-padding rather than also
				// multiplied by 1.5 -- the auto-fit content height already
				// IS "what's needed" (zero slack by construction), so any
				// multiplier above 1.0 already overshoots it; a small fixed
				// pad reads as intentional breathing room instead of the
				// dead vertical space the task's "never taller than needed"
				// qualifier is asking to avoid.
				const ImVec2 natural = ImGui::GetWindowSize();
				lastExpandedSize = ImVec2(
					natural.x * 1.5f,
					ImMin( natural.y * 1.5f, natural.y + kHeightPad ) );
			}
		}
		else if ( !bCollapsed )
		{
			lastExpandedSize = ImGui::GetWindowSize();
		}
		bWasCollapsed = bCollapsed;

		// Spec §4: "Focused window: border becomes accent@42%". ImGui's own
		// Begin()/focus-follows-click z-ordering already gives "focused
		// window renders last (on top)" for free.
		ImGui::PushStyleColor( ImGuiCol_Border,
			bFocused ? gamescope::palette::ToVec4( gamescope::palette::kAccent, 0.42f )
			         : ImGui::GetStyle().Colors[ImGuiCol_Border] );

		if ( bFocused )
		{
			// Accent glow, spec §4: "0 0 40px -20px accent@60%" -- a blurred
			// CSS box-shadow, which has no ImDrawList equivalent without an
			// actual blur pass (out of scope here -- "keep it cheap"). The
			// previous approximation was 2 flat-alpha rings, which is exactly
			// what "steppy" describes: 2 layers means 2 hard edges. Two changes
			// make the same trick read as a continuous glow instead:
			//
			// 1. More (kGlowLayers), thinner rings whose alpha follows an
			//    *exponential* falloff rather than a linear one -- a blurred
			//    shadow's brightness decays roughly like a Gaussian, not
			//    linearly, so matching that curve hides the seams in far fewer
			//    layers than a linear ramp needs. Ring width also grows outward
			//    (offsets spaced by t^1.5, not t) so sampling is densest right
			//    at the border -- where the eye is looking and the gradient is
			//    steepest -- and coarsest far out, where alpha is already near
			//    zero and a wide step goes unnoticed.
			// 2. Each ring's corner radius is the window's own radius PLUS that
			//    ring's own outward offset, instead of an independent hand-
			//    picked radius per layer. Offsetting a rounded rect outward by
			//    d is a Minkowski sum: radius r becomes exactly r+d. Rings that
			//    don't track this drift out of concentricity fastest at the
			//    corners -- why corners are where banding is always worst first.
			//
			// Drawn on the foreground draw list, not this window's own, so it's
			// never clipped to the window's bounds.
			ImDrawList *pFg = ImGui::GetForegroundDrawList();
			const ImVec2 wp = ImGui::GetWindowPos();
			const ImVec2 ws = ImGui::GetWindowSize();
			constexpr float kWindowRadius = 4.0f;  // spec §4 "Corner radius 4px" == WindowRounding pushed above
			constexpr int   kGlowLayers   = 5;
			constexpr float kGlowReach    = 20.0f; // outermost ring's offset from the window edge, px
			constexpr float kGlowPeakA    = 0.22f; // alpha of the innermost (thinnest, brightest) ring
			constexpr float kGlowDecay    = 2.5f;  // exponential falloff rate over the 0..1 layer range

			float flPrevOffset = 0.0f;
			for ( int i = 1; i <= kGlowLayers; ++i )
			{
				const float t = (float)i / (float)kGlowLayers;
				const float flOffset = kGlowReach * powf( t, 1.5f );
				const float flThickness = flOffset - flPrevOffset;
				const float flMid = ( flPrevOffset + flOffset ) * 0.5f;
				const float flAlpha = kGlowPeakA * expf( -kGlowDecay * t );

				pFg->AddRect( wp - ImVec2( flMid, flMid ), wp + ws + ImVec2( flMid, flMid ),
					ImGui::GetColorU32( gamescope::palette::Accent( flAlpha ) ),
					kWindowRadius + flMid, 0, flThickness );

				flPrevOffset = flOffset;
			}
		}

		DrawTitleBar( pszTitle, id, bFocused, bCollapsed );

		if ( bCollapsed )
		{
			// Mirrors ImGui's own native "collapsed windows still draw their
			// title bar, nothing else" behavior -- the window (title bar,
			// border, focus glow) is fully drawn above; there's simply no
			// body to show while shrunk to kTitleBarHeight tall. Must
			// balance this Begin() with End() (and every push above) right
			// here, since the caller is told NOT to call EndPanelWindow()
			// when this returns false (Chrome.h's contract).
			//
			// Crash fix: DrawTitleBar() unconditionally ends with
			// SetCursorScreenPos() to hand the cursor back to the caller's
			// content below the bar -- fine on every other frame, since the
			// caller then goes on to submit real widgets that grow
			// CursorMaxPos past that position. Collapsed, nothing is drawn
			// after DrawTitleBar(): End() below would be the very next
			// call, and ImGui 1.92's
			// ErrorCheckUsingSetCursorPosToExtendParentBoundaries() hard
			// asserts (not the pre-1.89 silent-fixup path) whenever
			// SetCursorPos()/SetCursorScreenPos() pushed the cursor past
			// the window's content bounds with no item submitted to justify
			// it -- exactly what a title-bar-only, kTitleBarHeight-tall
			// collapsed window's tiny content region triggers. A zero-size
			// Dummy() is ImGui's own documented fix (imgui.cpp's
			// ErrorCheckUsingSetCursorPosToExtendParentBoundaries comment):
			// it "submits" the position without drawing or resizing
			// anything, so the boundary check is satisfied instead of
			// tripping. Repro was double-clicking (or collapse-glyph-
			// clicking) a panel's title bar, which crashed the whole
			// process one frame later via SIGABRT (assert -> abort()) --
			// see superdoc/planning/ISSUES.md.
			ImGui::Dummy( ImVec2( 0.0f, 0.0f ) );
			ImGui::PopStyleColor( 2 ); // Border, WindowBg
			ImGui::End();
			ImGui::PopStyleVar( 3 ); // WindowBorderSize, Alpha, WindowRounding
			return false;
		}

		// Scroll fix: the panel's body goes in its own child window/region
		// from here down, rather than directly as more of the outer
		// window's own content the way it did before this pass. Root cause
		// of "scrollbar grip shrinks (so content height IS computed right)
		// but the view never moves, wheel or grip-drag alike": DrawTitleBar()
		// hands the cursor back to the caller via the outer window's own
		// screen-space cursor, anchored off ImGui::GetWindowPos() (constant,
		// NOT scroll-adjusted, which is exactly right for a header that must
		// stay pinned on screen while the body beneath it scrolls). But the
		// outer window was ALSO the thing scrolling (nothing suppressed its
		// own scrollbar), so every subsequent widget's on-screen position
		// worked out to windowPos + a fixed offset regardless of
		// window->Scroll -- the scroll and unscroll canceled out
		// algebraically, pinning the *entire* body to the header's fixed
		// screen position no matter how far the window had scrolled. A
		// child region gets its own independent Scroll/ClipRect, so it
		// scrolls correctly on its own, while NoScrollbar/NoScrollWithMouse
		// above keep the outer window itself from ever having scroll state
		// to cancel against in the first place.
		ImGui::BeginChild( "##body", ImVec2( 0.0f, 0.0f ) );

		return true;
	}

	void EndPanelWindow()
	{
		ImGui::EndChild(); // "##body", opened in BeginPanelWindow()
		ImGui::PopStyleColor( 2 ); // ImGuiCol_Border, ImGuiCol_WindowBg -- both pushed in BeginPanelWindow()
		ImGui::End();
		ImGui::PopStyleVar( 3 ); // WindowBorderSize, Alpha, WindowRounding -- all pushed in BeginPanelWindow()
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

		// Per spec §8 Dock: idle fill white@4.5%/border white@10%/icon
		// white@62%; open/active fill accent@13%/border accent@50%/icon
		// accent-icon (#A3E3F6), plus a 2px accent-edge top marker (inset
		// 8px each side, i.e. 38px wide, flush with the button's own top
		// border) with an approximated glow underneath it. A previous pass
		// here used a full accent border + a much stronger (22%) fill and
		// had no top-edge marker at all -- gap list item 7.
		bool DrawDockButton( ImDrawList *pDrawList, Icon icon, bool bActive, const char *pszLabel, float flSize )
		{
			ImGui::PushID( pszLabel );
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const bool bClicked = ImGui::InvisibleButton( "##dockbtn", ImVec2( flSize, flSize ) );
			const bool bHovered = ImGui::IsItemHovered();

			const ImU32 bgCol = bActive ? ImGui::GetColorU32( gamescope::palette::Accent( 0.13f ) )
				: ( bHovered ? kDockHoverFillU32 : kDockIdleFillU32 );
			const ImU32 borderCol = bActive ? ImGui::GetColorU32( gamescope::palette::Accent( 0.50f ) )
				: ( bHovered ? kHairlineStrongU32 : kHairlineU32 );
			const ImU32 iconCol = bActive ? kAccentIconU32 : ( bHovered ? kIconHoverU32 : kIconIdleU32 );

			pDrawList->AddRectFilled( pos, ImVec2( pos.x + flSize, pos.y + flSize ), bgCol );
			pDrawList->AddRect( pos, ImVec2( pos.x + flSize, pos.y + flSize ), borderCol );

			if ( bActive )
			{
				// Proportional to flSize (spec's own numbers are all relative
				// to the 54px canonical button -- dock_scale, General tab,
				// resizes this whole button, so these markers scale with it
				// too instead of drifting off-proportion at non-1.0 scale).
				const float flRatio = flSize / 54.0f;
				const float kEdgeInset = 8.0f * flRatio;
				const float kEdgeThickness = 2.0f * flRatio;
				const ImVec2 edgeMin( pos.x + kEdgeInset, pos.y );
				const ImVec2 edgeMax( pos.x + flSize - kEdgeInset, pos.y + kEdgeThickness );
				// Glow: spec "0 0 10px accent@90% ~= one 42x6px rect under
				// it @25% accent".
				pDrawList->AddRectFilled( ImVec2( pos.x + flSize * 0.5f - 21.0f * flRatio, pos.y ),
					ImVec2( pos.x + flSize * 0.5f + 21.0f * flRatio, pos.y + 6.0f * flRatio ),
					ImGui::GetColorU32( gamescope::palette::Accent( 0.25f ) ) );
				pDrawList->AddRectFilled( edgeMin, edgeMax, ImGui::GetColorU32( gamescope::palette::kAccentEdge ) );
			}

			DrawIcon( pDrawList, icon, ImVec2( pos.x + flSize * 0.5f, pos.y + flSize * 0.5f ), flSize * 0.4f, iconCol );

			if ( bHovered )
				ImGui::SetTooltip( "%s", pszLabel );

			ImGui::PopID();
			return bClicked;
		}
	}

	void DrawDock()
	{
		EnsureLiveThemeLoaded();

		ImGuiIO &io = ImGui::GetIO();
		const float flDockScale = gamescope::palette::g_LiveTheme.flDockScale;
		const float kButtonSize = 54.0f * flDockScale; // spec §1: keep >=44px physical -- dock_scale's own 0.85 floor (ConfigSchema.h) guarantees that
		const float kGap = 5.0f * flDockScale;
		const float kPad = 6.0f * flDockScale;
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
		// Dock fill is .86 alpha, distinct from windows' .88 (spec §1
		// `surface` vs §8's own "fill rgba(9,10,12,.86)") -- push a
		// dock-only override rather than changing the shared WindowBg token.
		// opacity_dock (General tab) drives this live via g_LiveTheme.
		ImGui::PushStyleColor( ImGuiCol_WindowBg, gamescope::palette::SurfaceVec4( gamescope::palette::g_LiveTheme.flDockAlpha ) );
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
			pDrawList->AddRectFilled( pos, ImVec2( pos.x + kButtonSize, pos.y + kButtonSize ), kDockIdleFillU32 );
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

			// Spec §8 "Close button (after divider): fill white@3%, border
			// white@8%" -- distinct (dimmer) from the idle panel-toggle
			// buttons' 4.5%/10%.
			const ImU32 closeFill = ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.07f : 0.03f ) );
			const ImU32 closeBorder = ImGui::GetColorU32( gamescope::palette::White( bHovered ? 0.12f : 0.08f ) );
			pDrawList->AddRectFilled( pos, ImVec2( pos.x + kButtonSize, pos.y + kButtonSize ), closeFill );
			pDrawList->AddRect( pos, ImVec2( pos.x + kButtonSize, pos.y + kButtonSize ), closeBorder );
			DrawIcon( pDrawList, Icon::Close, ImVec2( pos.x + kButtonSize * 0.5f, pos.y + kButtonSize * 0.5f ),
				kButtonSize * 0.4f, bHovered ? kIconHoverU32 : kIconIdleU32 );

			if ( bHovered )
				ImGui::SetTooltip( "Close overlay" );
			if ( bClicked )
				SettingsOverlay_ToggleVisible();
		}

		const ImVec2 dockWindowPos = ImGui::GetWindowPos();
		const ImVec2 dockWindowSize = ImGui::GetWindowSize();

		ImGui::End();
		ImGui::PopStyleColor(); // ImGuiCol_WindowBg
		ImGui::PopStyleVar( 3 );

		// Hint line, centered under the dock container -- spec §8: Mono 400
		// 10.5 @42% with a text shadow, 10px gap. Drawn on the background
		// draw list (own tiny "window") so it isn't clipped to the dock's
		// own bounds and doesn't participate in the dock's padding/layout.
		{
			ImGui::PushFont( fonts::Get( fonts::Style::Meta ) );
			// Our actual binding is Ctrl+Shift+O (SettingsOverlay.cpp's
			// cv_toggle hotkey), not the mockup's own Shift+Tab -- spec §8's
			// own copy-pattern note says to substitute the real binding
			// rather than print a key we don't bind.
			static constexpr const char *kHintText = "CTRL+SHIFT+O toggle overlay \xC2\xB7 game input paused";
			const ImVec2 hintSize = ImGui::CalcTextSize( kHintText );
			const ImVec2 hintPos(
				dockWindowPos.x + dockWindowSize.x * 0.5f - hintSize.x * 0.5f,
				dockWindowPos.y + dockWindowSize.y + 10.0f );

			ImDrawList *pFgDrawList = ImGui::GetForegroundDrawList();
			const ImU32 shadowCol = ImGui::GetColorU32( gamescope::palette::Black( 0.90f ) );
			pFgDrawList->AddText( hintPos + ImVec2( 0.0f, 1.0f ), shadowCol, kHintText );
			pFgDrawList->AddText( hintPos, ImGui::GetColorU32( gamescope::palette::White( 0.42f ) ), kHintText );
			ImGui::PopFont();
		}
	}
}
