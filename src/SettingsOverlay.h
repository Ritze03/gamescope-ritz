// M1 ImGui render shell: a toggleable placeholder overlay rendered into an
// offscreen Vulkan texture and injected as a layer in paint_all()'s frame.
//
// M2 (this file's second half, below): full keyboard/mouse capture while the
// overlay is open, and airtight release on toggle-off. See
// superdoc/planning/SPEC.md's "Input capture and release" section and
// superdoc/planning/ISSUES.md issues #5-#7 for the design this follows.
#pragma once

#include <cstdint>
#include <string>

class CVulkanCmdBuffer;
struct FrameInfo_t;

namespace gamescope
{
	// Live-tunable background blur/darkening (window-chrome overhaul's
	// General tab, Config/ConfigSchema.h's OverlaySettings::background_blur /
	// background_darkening) -- same seed-once/push-on-edit shape as
	// Overlay/Palette.h's gamescope::palette::g_LiveTheme and
	// Overlay/Notifications.h's gamescope::Notifications::g_LiveTheme:
	// process-level and global.json-only (ApplyProfile() never touches
	// `overlay`, so a per-game override or applied profile never changes
	// these), seeded once from global.json by this file's own
	// EnsureBackgroundLiveThemeLoaded() (SettingsOverlay.cpp), then written
	// straight into by Overlay/PanelConfig.cpp's DrawGeneralTab() on every
	// slider edit -- that bypass is what makes the change visible the very
	// next frame instead of waiting on a config-generation bump that
	// General-tab edits never trigger. SettingsOverlay.cpp is the only
	// reader (see SettingsOverlay_AddLayer()'s blur-radius/darkening-ctm
	// consumers).
	struct BackgroundLiveTheme
	{
		float flBlur = 0.0f;      // OverlaySettings::background_blur, 0..1
		float flDarkening = 0.0f; // OverlaySettings::background_darkening, 0..1
	};
	extern BackgroundLiveTheme g_BackgroundLiveTheme;

	// Called once per paint_all(), on the steamcompmgr thread, right before the
	// frame is handed to the backend's Present(). If the overlay is visible or
	// still fading out, this draws the settings overlay's panels/dock into an
	// offscreen texture on the general (graphics) queue and appends a Layer_t
	// for it to *pFrameInfo. A no-op (adds nothing) when fully hidden.
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

	// Issue #22. Call from vulkan_composite() immediately after the compute
	// submission built with the command buffer passed to
	// SettingsOverlay_WaitForRender() has been handed to the queue. Publishes
	// that submission's "done sampling the overlay texture" timeline point so
	// the overlay's next general-queue render can wait on it before clearing
	// and redrawing the texture. Safe to call when no overlay render is
	// pending -- it is then a no-op.
	void SettingsOverlay_CommitReads();

	// ------------------------------------------------------------------
	// M2: input capture and release.
	//
	// Threading: everything below this point is documented per-function as
	// either "producer side" (called from wlserver's input handlers, which
	// run on the main thread -- see wlserver.cpp) or "consumer side" (the
	// steamcompmgr thread, inside SettingsOverlay_AddLayer(), the only code
	// that ever touches ImGuiIO -- ImGui's own context is not thread-safe).
	// The producer side never touches ImGuiIO directly; it only appends to a
	// mutex-guarded queue that the consumer drains once per frame. See the
	// file-level comment in SettingsOverlay.cpp for the full rationale.
	// ------------------------------------------------------------------

	// Toggles the overlay's visibility, same effect as the
	// `toggle_settings_overlay` ConCommand. Safe to call from any thread.
	// Called from wlserver's Ctrl+Shift+O hotkey check (main thread).
	void SettingsOverlay_ToggleVisible();

	// Thread-safe (atomic) read of whether the overlay currently wants to
	// own all keyboard/mouse input. wlserver's input handlers check this
	// before forwarding an event to the focused game surface; when true they
	// route the event into the queue below instead of forwarding it.
	bool SettingsOverlay_IsCapturingInput();

	// Keyboard-control toggle (see ConfigSchema.h's OverlaySettings
	// comment): true only when the overlay is open/capturing AND the
	// "capture all keyboard input" setting is on. wlserver_dispatch_key()
	// (wlserver.cpp) uses this instead of SettingsOverlay_IsCapturingInput()
	// to decide where a keypress goes -- mouse routing is untouched by this
	// setting and keeps using SettingsOverlay_IsCapturingInput() directly.
	// Thread-safe the same way SettingsOverlay_IsCapturingInput() is (reads
	// only atomics/ConVars, no locking).
	bool SettingsOverlay_IsCapturingKeyboard();

	// Producer side (main thread). uLinuxKeycode is a raw evdev keycode
	// (KEY_* from linux/input-event-codes.h, NOT an xkb keycode -- no +8),
	// the same convention wlserver_key()/wlserver_handle_key() already use
	// for everything except xkb lookups. ImGuiKey mapping still happens on
	// the consumer side (SettingsOverlay.cpp) so wlserver.cpp does not need
	// to know anything about ImGui -- but layout-correct TEXT can only be
	// produced where the real xkb_state lives (wlserver.cpp, next to the
	// keyboard), so wlserver_dispatch_key() resolves it there (via
	// xkb_state_key_get_utf8() against the keyboard that actually received
	// the press) and passes the already-translated UTF-8 through here.
	// sUtf8Text is empty for a release, for keys with no printable mapping,
	// and for control characters (Enter/Tab/Backspace/Escape/...) -- those
	// stay pure key events, never text, regardless of layout.
	void SettingsOverlay_QueueKeyEvent( uint32_t uLinuxKeycode, bool bPressed, std::string sUtf8Text = std::string() );

	// Producer side. Relative pointer motion (already sensitivity-scaled by
	// the caller, matching wlserver_mousemotion()'s own convention) --
	// accumulated into an overlay-local cursor position on the consumer
	// side, since ImGuiIO wants an absolute position.
	void SettingsOverlay_QueueMouseMotionDelta( double dx, double dy );

	// Producer side. Absolute pointer position, normalized 0..1 across the
	// full output (the same convention wlserver_touchmotion()'s x/y
	// parameters already use before it scales them to pixels) -- this is
	// the path SDL's windowed (non-relative-grab) mouse motion actually
	// takes, see the file-level comment in SettingsOverlay.cpp.
	void SettingsOverlay_QueueMouseMotionAbsolute( double flNormalizedX, double flNormalizedY );

	// Producer side. uLinuxButton is a raw evdev button code (BTN_LEFT etc).
	void SettingsOverlay_QueueMouseButton( uint32_t uLinuxButton, bool bPressed );

	// Producer side. Same units wlserver_mousewheel()'s flX/flY already use.
	void SettingsOverlay_QueueMouseWheel( double flX, double flY );
}
