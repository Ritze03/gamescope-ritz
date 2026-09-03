// M4 FPS display -- see superdoc/planning/SPEC.md's "Per-feature sections ->
// 3. FPS display" and superdoc/planning/DECISIONS.md #16/#17.
//
// Scope reduction (2026-09-03, the user's own call -- see CHANGELOG.md and
// superdoc/meta/TERMINOLOGY.md's "profiler" entry): this HUD used to be a
// small performance profiler (CPU/GPU load, a frametime graph, a percentile
// row, Now Playing) built on top of the FPS readout. All of that is gone.
// This file draws exactly one thing: the FPS integer, positioned by a
// 9-point anchor plus pixel margins. Phase 2 (2026-09-03, same day) rebuilt
// the `system.hud` settings area on top of what Phase 1 left -- update
// modes, hide-above-X, a plain backdrop, a two-way text-colour choice and a
// lag-spike reaction, a drop shadow -- see
// superdoc/features/fps-display.md for the whole feature as it stands now.
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
	namespace ui { class Registry; }

	// Called once per paint_all(), on the steamcompmgr thread. Reads
	// gamescope-ritz's fps_display config (loaded lazily on first call) and,
	// when enabled and not currently hidden by "hide above X", draws the
	// readout (game frame rate, backdrop, text-colour treatment per config)
	// into its own offscreen texture and appends a Layer_t for it to
	// *pFrameInfo. A no-op when disabled.
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

	// Declares this feature's settings as the E2 `system.hud` area: the
	// master switch, placement (anchor + margins), font size, update mode,
	// hide-above-X, backdrop opacity, text colour and shadow strength -- see
	// this file's header comment and superdoc/features/fps-display.md.
	//
	// This REPLACED FpsDisplay_DrawSettingsPanel(), the six-tab panel issue
	// #59 built and P5 deleted. It is a declaration, not a draw call: it places no pixel,
	// runs at startup rather than per frame, and takes no ImGui context --
	// which is what let the last escape hatch for this area go.
	//
	// Only the SETTINGS half of FpsDisplay.cpp moved. The HUD drawn over the
	// game keeps its own ImGui context, its own offscreen texture and its
	// own submission path, all untouched by the redesign.
	void FpsDisplay_RegisterArea( ui::Registry &reg );
}
