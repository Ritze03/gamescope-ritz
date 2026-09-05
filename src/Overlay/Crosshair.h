// Compositor-drawn crosshair -- see superdoc/features/crosshair.md.
//
// Why it exists: the user runs frame generation externally (lsfg-vk, a
// Vulkan layer inside the game's own process). That interpolates the
// game's frame, so anything drawn INSIDE the game -- above all its own
// crosshair -- smears between real frames. A crosshair gamescope draws is
// composited after interpolation, over the finished frame, and cannot.
//
// Where it draws: into the FPS HUD's own layer (Overlay/FpsDisplay.cpp),
// not a layer of its own. k_nMaxLayers is 6, a busy frame already fills
// it, and LayerStack_t::push() fails silently when it does; the HUD's layer
// already spans the whole output and sits above the game and below the
// Shell and the toasts, which is exactly where a crosshair belongs. This
// file therefore owns the crosshair's CONFIG, its SETTINGS AREA, its
// right-click state and its per-frame DRAW into a draw list it is handed;
// it owns no texture, no ImGui context and no Layer_t.
//
// Geometry and the hide animation's arithmetic live in CrosshairMath.h
// (pure, unit-tested); this file only resolves config into them.
#pragma once

#include <cstdint>

struct ImDrawList;

namespace gamescope
{
	namespace ui { class Registry; }

	// Where this frame's crosshair goes, in output pixels: the centre of the
	// game's on-screen rect (layer 0), and how many output pixels one game
	// pixel covers per axis (1 when the game is drawn 1:1). FpsDisplay.cpp
	// derives it from paint_all()'s FrameInfo_t -- see
	// FpsDisplay_AddLayer(). flDrawOffsetY shifts the whole drawing down
	// inside the HUD texture (0 normally; the texture height when the HUD
	// is rendering the readout and the crosshair into separate halves).
	struct CrosshairFrame
	{
		float flCenterX = 0.0f;
		float flCenterY = 0.0f;
		float flGamePixelScaleX = 1.0f;
		float flGamePixelScaleY = 1.0f;
		float flDrawOffsetY = 0.0f;
	};

	// True when the master switch is on. Reads (and lazily loads/reloads)
	// this feature's own config cache; safe from the steamcompmgr thread
	// and from the settings UI (which draws on that same thread).
	bool Crosshair_IsEnabled();

	// Draws the crosshair for this frame into `pDrawList` (the HUD's
	// background draw list). Returns true while the right-click hide
	// animation is still moving, so the caller keeps forcing repaints; a
	// static crosshair (idle, or fully hidden) returns false and costs no
	// extra frames. A no-op returning false when disabled.
	bool Crosshair_Draw( ImDrawList *pDrawList, const CrosshairFrame &frame, uint64_t ulNowNs );

	// Called from wlserver's pointer-button dispatch, on the wlserver
	// thread, for a BTN_RIGHT press/release that is being delivered TO THE
	// GAME (never one the Shell/Launcher captured). Records the press time
	// in an atomic the render side reads; never touches the event itself.
	// Release restores the crosshair instantly (no reverse animation).
	void Crosshair_NotifyRightButton( bool bPressed );

	// Declares the `system.crosshair` settings area. A declaration, not a
	// draw call -- same contract as FpsDisplay_RegisterArea().
	void Crosshair_RegisterArea( ui::Registry &reg );
}
