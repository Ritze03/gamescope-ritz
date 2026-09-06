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
// it owns no ImGui context, no Layer_t and no texture. The one Vulkan
// object it does own is a small host-visible staging buffer for Apply
// Scaling's stretched raster, which is copied INTO the HUD's texture
// (see Crosshair_RecordUpload), never composited as a layer of its own.
//
// Geometry and the hide animation's arithmetic live in CrosshairMath.h
// (pure, unit-tested); this file only resolves config into them.
#pragma once

#include <cstdint>

struct ImDrawList;
class CVulkanCmdBuffer;
class CVulkanTexture;

namespace gamescope
{
	namespace ui { class Registry; }

	// Where this frame's crosshair goes, in output pixels: the centre of the
	// game's on-screen rect (layer 0), and how many output pixels one game
	// pixel covers per axis (1 when the game is drawn 1:1). FpsDisplay.cpp
	// derives it from paint_all()'s FrameInfo_t -- see
	// FpsDisplay_AddLayer(). uGameWidth/Height is the game's own committed
	// buffer size in game pixels (g_uBaseLayerSourceWidth/Height); 0 when
	// unknown, in which case Apply Scaling falls back to the pixel path at
	// scale 1.
	//
	// bReserveInvertMarker: true when the HUD layer is in Inverted text
	// mode (ALPHA_BLENDING_MODE_INVERT). That shader tells the readout's
	// digits apart from everything else by a marker in the texel itself --
	// G == 0 means "digit" (src/shaders/alphamode.h) -- so a crosshair
	// colour with no green at all (pure red, blue, magenta, black) would
	// read as digit coverage and invert the game instead of showing. When
	// set, every crosshair colour has its G nudged from 0 to 1, a one-count
	// change the eye cannot see, and nothing else about the drawing moves.
	// Off (Fixed text colour, or no readout) the colours are used exactly
	// as configured. Replaced the 2026-09-05 "split mode" (a double-height
	// texture and a second Layer_t for the crosshair), 2026-09-06.
	struct CrosshairFrame
	{
		float flCenterX = 0.0f;
		float flCenterY = 0.0f;
		float flGamePixelScaleX = 1.0f;
		float flGamePixelScaleY = 1.0f;
		uint32_t uGameWidth = 0;
		uint32_t uGameHeight = 0;
		bool bReserveInvertMarker = false;
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
	//
	// Two rendering paths (superdoc/features/crosshair.md): with Apply
	// Scaling OFF every primitive is a whole-pixel rect at output
	// resolution, AA off, into pDrawList. With it ON nothing goes into the
	// draw list at all: the crosshair is Build() at the GAME's resolution,
	// rasterised on the CPU (its own bounding box + 1 texel margin),
	// stretched to the output by a CPU bilinear resample at layer 0's
	// per-axis scale -- the look of a stretched in-game raster -- and held
	// for Crosshair_RecordUpload() to copy straight into the HUD texture.
	// The raster is re-built only when its geometry, colours, placement or
	// the hide animation change.
	bool Crosshair_Draw( ImDrawList *pDrawList, const CrosshairFrame &frame, uint64_t ulNowNs );

	// Called by FpsDisplay.cpp's RenderAndSubmit() on the general-queue
	// command buffer it is about to render the HUD with, OUTSIDE the
	// render pass, after the previous HUD submission has been drained and
	// after pHudTexture's initial layout barrier. When this frame's
	// Crosshair_Draw() produced a stretched raster: clears pHudTexture,
	// copies the raster into it (a small host-visible staging buffer this
	// file owns, rewritten only when the pixels changed) and returns true
	// -- the caller then LOADs the texture in its render pass instead of
	// clearing it, so the readout still draws over the crosshair. Returns
	// false, recording nothing, when there is nothing to copy.
	//
	// Why a copy into the HUD texture rather than an ImGui image quad:
	// everything ImGui draws goes through its SRC_ALPHA blend, so a
	// stretched edge texel of coverage w landed premultiplied and the
	// composite (which reads the HUD texel as straight alpha) showed it at
	// w * w -- crushed soft edges, a dimmer line and a gap that read too
	// wide. See crosshair::ResampleToOutput() in CrosshairMath.h.
	bool Crosshair_RecordUpload( CVulkanCmdBuffer *pCmdBuffer, CVulkanTexture *pHudTexture );

	// Called from wlserver's pointer-button dispatch, on the wlserver
	// thread, for a BTN_RIGHT press/release that is being delivered TO THE
	// GAME (never one the Shell/Launcher captured). Records the edge and
	// its time in an atomic the render side reads; never touches the event
	// itself. With "Animate back" on (the default) a release plays the
	// hide animation backwards from wherever it was; off, it restores the
	// crosshair instantly.
	void Crosshair_NotifyRightButton( bool bPressed );

	// Declares the `system.crosshair` settings area. A declaration, not a
	// draw call -- same contract as FpsDisplay_RegisterArea().
	void Crosshair_RegisterArea( ui::Registry &reg );
}
