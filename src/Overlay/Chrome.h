// M8 part 3 (issue #15): window and dock chrome for the settings overlay --
// panel window frames, the title-bar icon/status-dot treatment, the bottom
// dock that shows/hides each panel, the accent left-edge "this one is live"
// affordance, and the hand-drawn icon set from data/icons/*.svg.
//
// Scope boundary (see the M8p3 task brief): this file owns window frames,
// the title bar, dock/tab chrome, panel outer borders, the accent left-edge,
// overall window layout/sizing, and icon loading/drawing. It does NOT own
// individual widget styling (buttons/sliders/checkboxes/combos/text inputs/
// separators) or the shared ImGuiStyle color/metric setup -- that's
// SettingsOverlay.cpp's ApplyDesignGuideStyle() (issue #14's territory);
// nothing here pushes/pops ImGuiCol_* globally.
//
// Structural fix this milestone makes (see SPEC.md's "UI structure"): before
// this, all five panels + the M1 placeholder window were unconditionally
// `_Draw()`n every frame as five overlapping top-level windows -- correct
// for parallel milestone development, not the design. SPEC.md's UI
// structure calls for free-floating panel windows (stock ImGui, no docking
// branch -- see SPEC.md's Architecture section for why) shown/hidden by a
// bottom dock, one icon button per panel, exactly like the design guide's
// Dock component. That's what BeginPanelWindow()/EndPanelWindow()/DrawDock()
// below implement: each panel now only exists while its dock button is on.
#pragma once

struct ImVec2;
struct ImDrawList;

namespace gamescope::chrome
{
	// One entry per panel hosted in its own floating window, in dock
	// left-to-right order. Matches SPEC.md's "UI structure" panel list
	// (Gamescope/Display, Shaders, FPS HUD, Audio, Config/Profiles) --
	// deliberately excludes ReShade's future M9 Adaptive Brightness group
	// (that's content *inside* the Shaders panel, not a new panel).
	enum class PanelId
	{
		Display,
		Shaders,
		Fps,
		Audio,
		Config,

		Count,
	};

	// Whether a panel's window is currently shown. Defaults to true for
	// every panel (matches this milestone's starting point, where all
	// panels were unconditionally visible) -- ponytail: no persisted
	// open/closed state across overlay toggles or process restarts; SPEC's
	// Build order gives per-game *window position* persistence as a stretch
	// goal already deferred past v1's floating-window milestones, and dock
	// open/closed state is the same class of "nice later, not now" as that.
	bool IsPanelOpen( PanelId id );
	void SetPanelOpen( PanelId id, bool bOpen );

	// Wraps ImGui::Begin() for one of the five panel windows. Returns false
	// (and the caller must NOT call EndPanelWindow() or draw anything) when
	// the panel is closed at the dock -- this is what actually makes "closed"
	// mean "not drawn" rather than just "not interactable". When it returns
	// true, the caller must call EndPanelWindow() exactly once before
	// returning, mirroring ImGui::Begin()/End()'s own pairing contract.
	//
	// pszTitle/defaultPos/defaultSize are passed straight through to
	// ImGui::Begin()/SetNextWindowPos()/SetNextWindowSize() (ImGuiCond_FirstUseEver)
	// -- callers keep whatever layout position they already tuned, this only
	// changes *whether* and *how* the window gets opened, not where.
	//
	// Draws this milestone's window chrome once Begin() succeeds: the
	// design guide's Title font on the native title bar text (unchanged from
	// M8 part 1), a slim icon+status-dot sub-header row *inside* the content
	// area just under the native title bar (see Chrome.cpp's file comment
	// for why this sits next to, not on top of, ImGui's own title bar), and
	// a 2px accent left-edge stripe on the window frame while it's the
	// focused window -- the design guide's "active/live group" affordance,
	// applied at panel-window granularity since this design's top-level nav
	// is free-floating windows, not a docked tab strip (SPEC.md's UI
	// structure). Clicking the native title-bar close button closes the
	// panel the same way un-toggling its dock button does (both write the
	// same IsPanelOpen() state) -- SPEC's own scope note about the "18x18
	// bare glyph" close/collapse buttons is deliberately not reimplemented
	// per-widget here (ponytail, see Chrome.cpp).
	bool BeginPanelWindow( const char *pszTitle, PanelId id, ImVec2 defaultPos, ImVec2 defaultSize );
	void EndPanelWindow();

	// Draws the bottom dock (design guide's Dock component): a brand glyph,
	// one toggle button per PanelId (click to show/hide that panel's
	// window), a hairline divider, and a trailing button that closes the
	// whole overlay. Call once per frame, after every panel's own
	// Begin/Draw/End, so the dock's own window naturally lands on top of the
	// panel windows in ImGui's per-frame Begin-order Z stack.
	void DrawDock();

	// ----------------------------------------------------------------------
	// Icons: hand-transcribed from data/icons/*.svg (see Chrome.cpp's file
	// comment for why -- no SVG rasterizer/atlas step, just the same
	// rects/circles/lines/wedges the SVGs already describe, redrawn with
	// ImDrawList primitives so tinting is "pass a different color", exactly
	// what currentColor gives the SVGs for free). Exposed here (not kept
	// file-local to Chrome.cpp) since data/icons/checkbox-mark.svg exists for
	// "wherever a checkbox glyph is needed as an icon rather than a live
	// ImGui widget" per its README -- a future non-chrome caller may want it
	// without duplicating the geometry.
	enum class Icon
	{
		Settings,
		Display,
		Shaders,
		Performance,
		Audio,
		Profiles,
		Reset,
		Close,
		Collapse,
		DockMore,
		CheckboxMark,
	};

	// Draws `icon` centered at `center`, scaled so its 20x20 SVG viewBox
	// maps onto a `flSize` x `flSize` box, tinted `uColor` (an ImU32 --
	// typed as plain unsigned int here so this header doesn't need to
	// include imgui.h; callers already have one from ImGui::ColorConvertFloat4ToU32()
	// or ImGui::GetColorU32()). pDrawList is any live ImDrawList (window or
	// foreground) -- typed void* for the same reason; Chrome.cpp casts it
	// back to ImDrawList*.
	void DrawIcon( void *pDrawList, Icon icon, ImVec2 center, float flSize, unsigned int uColor );
}
