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
	// (Gamescope/Display, Shaders, System Monitor, Audio, Config/Profiles)
	// -- deliberately excludes ReShade's future M9 Adaptive Brightness
	// group (that's content *inside* the Shaders panel, not a new panel).
	enum class PanelId
	{
		Display,
		Shaders,
		// Issue #27: renamed from Fps -- this panel now hosts the whole
		// module framework (FPS today; #28 adds CPU/GPU/Media), not just
		// the FPS readout. The underlying feature file stays FpsDisplay.*
		// (see that file's own header comment on why), but the identity
		// this enum exposes to the rest of Chrome.* is the new name.
		SystemMonitor,
		Audio,
		Config,
		Log, // issue #39: gamescope's own log + the launched game's captured stdout/stderr, two tabs

		Count,
	};

	// Whether a panel's window is currently shown. Independent per panel --
	// opening one no longer closes the others (that "exclusive" behavior was
	// a stopgap for a real bug: five windows all defaulting to the same
	// first-use position landed stacked directly on top of each other,
	// burying panels like Audio without dragging). The actual fix is
	// BeginPanelWindow()'s own per-panel default position (see its comment)
	// -- with that in place, multiple panels can be open at once (the
	// user's own ask) without landing in a pile. Panels stay independently
	// *movable* once open. ponytail: no persisted open/closed state across
	// overlay toggles or process restarts; SPEC's Build order gives per-game
	// *window position* persistence as a stretch goal already deferred past
	// v1's floating-window milestones, and dock open/closed state is the
	// same class of "nice later, not now" as that.
	bool IsPanelOpen( PanelId id );
	void SetPanelOpen( PanelId id, bool bOpen );

	// Wraps ImGui::Begin() for one of the five panel windows. Returns false
	// (and the caller must NOT call EndPanelWindow() or draw anything) when
	// the panel is closed at the dock -- this is what actually makes "closed"
	// mean "not drawn" rather than just "not interactable" -- or while the
	// panel is collapsed (see below): the window itself is still drawn (its
	// title bar stays visible/draggable/re-expandable), just not the panel's
	// own body content this frame. When it returns true, the caller must
	// call EndPanelWindow() exactly once before returning, mirroring
	// ImGui::Begin()/End()'s own pairing contract; when it returns false,
	// the window (if any) has already been fully closed out internally --
	// the caller must NOT call EndPanelWindow() in that case either way.
	//
	// pszTitle is the window's title, shown UPPERCASE in the custom title
	// bar (spec §5) -- pass it already-uppercased (every current call site
	// does: "DISPLAY", "SHADERS", "AUDIO", "CONFIG / PROFILES"). defaultSize
	// is only a fallback bootstrap value now (issue #34: windows are
	// resizable, and their real first-ever-shown size is measured from the
	// panel's own content and grown from there -- see Chrome.cpp's
	// BeginPanelWindow() for the full algorithm); defaultSize is used
	// verbatim only if a panel is collapsed before its own measurement ever
	// finishes, which otherwise never happens in normal use. defaultPos
	// is intentionally NOT used for position any more: with multiple panels
	// allowed open at once (see IsPanelOpen()'s comment), each panel gets a
	// fixed, non-overlapping tile position keyed off its own PanelId (see
	// Chrome.cpp's TiledDefaultPos()) instead of whatever position each
	// panel's own call site happened to hardcode -- that's what actually
	// solves "five windows stacked on each other" rather than just papering
	// over it with exclusivity. Applied via ImGuiCond_FirstUseEver, so it
	// only ever matters the very first time a given panel is shown for the
	// life of the process -- once a user drags a panel, ImGui's own window
	// state remembers that position for as long as the overlay's ImGui
	// context lives, including across this panel being closed and reopened
	// ("remember position while open").
	//
	// Draws this milestone's window chrome once Begin() succeeds: a fully
	// custom-drawn 34px title bar (spec §5 -- status dot, UPPERCASE title,
	// meta text, focus-dependent gradient fill, 18x18 collapse/close glyph
	// buttons) replacing ImGui's own native title bar outright (see
	// Chrome.cpp's file comment for why the earlier "native title bar + a
	// second custom row underneath" compromise is gone -- it read as the
	// under-styled, nearly-invisible bar this replaces), 4px window corner
	// radius, a focused-window accent border @ 42% alpha plus an accent glow
	// (spec §4's "unmistakable" focus treatment) and accent header gradient,
	// and the whole window at 94% opacity while unfocused. The custom title
	// bar reimplements drag (issue #42: a hand-rolled per-frame
	// SetWindowPos() delta while the drag zone's InvisibleButton is
	// active -- not ImGui::StartMouseMovingWindow(), which the window's
	// own NoMove flag, added by the same issue to kill
	// click-anywhere-in-the-body dragging, blocks unconditionally for
	// every caller), collapse (a hand-rolled "shrink to
	// just the title bar" toggle, since ImGui's native collapse mechanism is
	// wired to its native title bar, which no longer exists once
	// ImGuiWindowFlags_NoTitleBar is set -- right-click the bar (issue #33),
	// or the collapse glyph, both deferred to next frame exactly like
	// ImGui's own window->WantCollapseToggle is) and close (middle-click
	// the bar (issue #33), or the collapse/close glyph
	// buttons are ButtonBehavior()-based InvisibleButtons, the same
	// primitive every other custom widget in this overlay -- Widgets.cpp's
	// Toggle()/Checkbox(), DrawDockButton() below -- is built on, so hover/
	// press/keyboard-nav semantics match a real ImGui button).
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
