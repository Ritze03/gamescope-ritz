// M4 FPS display -- see superdoc/planning/SPEC.md's "Per-feature sections ->
// 3. FPS display" and superdoc/planning/DECISIONS.md #16/#17.
//
// Lifetime note (the subtlety that milestone is most likely to get wrong):
// this readout has its own visibility flag, entirely independent of the
// settings panel's `cv_settings_overlay_visible`. It must keep rendering
// every composited frame while the settings panel is closed. To keep that
// guarantee bulletproof against however SettingsOverlay.cpp evolves under
// other in-flight work, this owns a fully separate ImGui context, offscreen
// texture, general-queue submission and timeline semaphore from
// SettingsOverlay's -- structurally the same shape (see SettingsOverlay.h's
// own comments for why that shape looks the way it does), just a second,
// independent instance of it, so nothing here can accidentally get coupled
// to the settings panel's own toggle state.
#pragma once

class CVulkanCmdBuffer;
struct FrameInfo_t;

namespace gamescope
{
	// Called once per paint_all(), on the steamcompmgr thread. Reads
	// gamescope-ritz's fps_display config (loaded lazily on first call) and,
	// when enabled, draws the readout (game frame rate, short rolling
	// average, backdrop, blend-mode treatment per config) into its own
	// offscreen texture and appends a Layer_t for it to *pFrameInfo. A no-op
	// when disabled.
	void FpsDisplay_AddLayer( FrameInfo_t *pFrameInfo );

	// Called right after vulkan_composite()/vulkan_screenshot() obtain their
	// compute-queue command buffer, before recording any dispatches that
	// might sample the FPS display's texture -- same cross-queue
	// synchronization role as SettingsOverlay_WaitForRender(), against this
	// feature's own timeline semaphore. A no-op if nothing was rendered this
	// frame.
	void FpsDisplay_WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer );

	// Issue #22, same contract as SettingsOverlay_CommitReads() -- call once
	// the compute submission is on the queue. No-op when nothing is pending.
	void FpsDisplay_CommitReads();

	// Draws this feature's own settings controls (enabled toggle, row
	// toggles for the frametime graph and percentile row, font size,
	// backdrop group, blend mode, text opacity) using whichever ImGui
	// context is *currently active* at the call site -- meant to be invoked
	// from inside the settings overlay's own panel draw, so its widgets
	// render as part of that panel. Edits are persisted to gamescope-ritz's
	// config (debounced onto the background write thread ConfigManager
	// already provides).
	void FpsDisplay_DrawSettingsPanel();
}
