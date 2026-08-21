// M1 ImGui render shell: a toggleable placeholder overlay rendered into an
// offscreen Vulkan texture and injected as a layer in paint_all()'s frame.
//
// Scope (see superdoc/planning/SPEC.md, Milestone M1): rendering only. There
// is no input capture here -- the panel is visible but not interactive. The
// toggle is a ConVar/ConCommand so it is testable from the console without
// any input plumbing. Input capture is Milestone M2.
#pragma once

class CVulkanCmdBuffer;
struct FrameInfo_t;

namespace gamescope
{
	// Called once per paint_all(), on the steamcompmgr thread, right before the
	// frame is handed to the backend's Present(). If the overlay is visible or
	// still fading out, this draws the M1 placeholder window into an offscreen
	// texture on the general (graphics) queue and appends a Layer_t for it to
	// *pFrameInfo. A no-op (adds nothing) when fully hidden.
	//
	// ponytail: relies on pFrameInfo already containing at least one valid
	// layer for paint_all()'s own bValidContents check (computed earlier in
	// paint_all(), before this call) to have passed -- an overlay toggled on
	// with literally no window of any kind present won't render in M1. Every
	// M1 acceptance scenario runs vkcube as the base layer, so this doesn't
	// block M1; fixing it means reordering paint_all()'s bValidContents check,
	// out of scope for a "minimal touch" here.
	void SettingsOverlay_AddLayer( FrameInfo_t *pFrameInfo );

	// Called by vulkan_composite() right after it obtains its compute-queue
	// command buffer, before recording any dispatches that might sample the
	// overlay's texture. If SettingsOverlay_AddLayer() submitted a new overlay
	// frame on the general queue since the last call, this adds the GPU-side
	// timeline-semaphore wait to pComputeCmdBuffer so the compute submission
	// cannot execute against the overlay texture before ImGui's own draw has
	// finished -- the cross-queue synchronization point between the two
	// queues. A no-op if the overlay didn't render anything this frame.
	void SettingsOverlay_WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer );
}
