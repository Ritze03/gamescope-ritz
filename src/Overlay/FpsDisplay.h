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

#include <string>

class CVulkanCmdBuffer;
struct FrameInfo_t;

namespace gamescope::config { struct HudLayout; }

namespace gamescope
{
	namespace ui { class Registry; }

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

	// Declares this feature's settings as the E2 `system.hud` area:
	// the master switch, the seven module toggles, the placement anchor and
	// its margins, the appearance controls, the per-module colour overrides
	// and the 60-second statistics history.
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

	// HUD layouts Phase 3 (superdoc/architecture/hud-layouts.md's "seam for
	// Phase 3's editor") -- one module's on-screen box, in HUD display-pixel
	// space (the same space DrawReadout() computes ModulePlacement()/
	// ResolveModuleOrigin() in, NOT the Shell's own io.DisplaySize -- see
	// FpsDisplay_GetModuleRects()'s own comment for why the two can differ
	// and how a caller converts between them).
	struct HudModuleRect
	{
		float x, y, w, h;
		bool bEnabled;
	};

	// Measures every module (Fps, Cpu, Gpu, Media -- kModuleOrder's fixed
	// order, matched by `out`'s index) against the SUPPLIED `layout` --
	// deliberately a parameter rather than reading config::ResolveLayoutCached()
	// itself, so a caller (Overlay/UI/HudLayoutEditor.cpp) can pass a
	// not-yet-saved working copy and see its own edits reflected immediately.
	// A module with an empty box (`HudModuleRect::bEnabled == false`) reports
	// (0,0,0,0) for x/y/w/h, the same "not present" contract MeasureModule()
	// uses internally.
	//
	// Reuses DrawReadout()'s own measure+resolve pipeline (MeasureModule(),
	// ModulePlacement(), ResolveModuleOrigin()) rather than a second copy --
	// see FpsDisplay.cpp for why this is safe to call from a DIFFERENT ImGui
	// context (the Shell's, not this file's own s_pImguiContext) without an
	// explicit context switch: it measures text against whatever ImGui
	// context is current on entry (the fonts are close enough for a drag
	// preview; pixel-perfect parity with the live HUD render is not the
	// goal), and reads only PLAIN, already-cached numeric state for
	// everything else (the last smoothed FPS/frametime/percentile values,
	// and the HUD's own last-known texture size) rather than touching this
	// file's own ImGui context or re-measuring a live frame.
	//
	// *pflDisplayW/*pflDisplayH receive the HUD's own last-known display
	// size (its offscreen texture's resolution) -- a sane fallback
	// (the compositor's current output size, or a hardcoded 1920x1080 if
	// even that is unknown) when the HUD has never rendered a frame yet, so
	// the editor always has something non-zero to divide by.
	void FpsDisplay_GetModuleRects( const config::HudLayout &layout, HudModuleRect (&out)[4],
	                                float *pflDisplayW, float *pflDisplayH );

	// HUD layouts Phase 3: the two calls Overlay/UI/HudLayoutEditor.cpp
	// makes to find and, on first Save(), name the layout the active
	// session's HUD resolves -- FpsDisplaySettings::layout_name lives in
	// this file's own EnsureConfigLoaded()-cached s_Settings, same as every
	// other fps_display field, so these are its read/write seam rather
	// than the editor keeping (and risking desyncing) a second copy.
	//
	// FpsDisplay_ActiveLayoutName() is EnsureConfigLoaded()'d first, same
	// as every other accessor in this header's implementation.
	const std::string &FpsDisplay_ActiveLayoutName();

	// Sets layout_name for the active session and persists it via this
	// file's usual EnqueueRoutedWrite() routing (PersistSettings()) -- the
	// editor's Save() calls this exactly once, only the first time a
	// layout is ever named (its working copy's layout_name was empty at
	// Begin()). Issues no force_repaint() of its own -- the editor's Save()
	// already issues one once it has finished writing the layout content
	// itself, and this call has no visible effect on its own until that
	// content write lands.
	void FpsDisplay_SetActiveLayoutName( const std::string &sName );
}
