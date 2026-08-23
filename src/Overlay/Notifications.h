// Toast notification system -- stylish, self-dismissing on-screen messages
// the rest of the codebase can fire (a config profile applied, an override
// toggled, ...) via Notifications::Show(). See superdoc/planning/DECISIONS.md
// #25 for the design split this follows: *placement* (where toasts anchor on
// screen) is a global-only preference, *muting* is per-game, using the config
// system's existing two-level (global vs. per-game full-snapshot) layering --
// see Config/ConfigManager.h's header comment and DECISIONS.md #19.
//
// Lifetime note (same subtlety FpsDisplay.h calls out, and for the same
// reason): toasts must keep rendering every composited frame while the
// settings panel is closed -- a toast confirming a change made *inside* the
// settings panel would be pointless if it vanished the instant the panel
// that triggered it closes. So this follows FpsDisplay.cpp's established
// pattern exactly: its own fully independent ImGui context, offscreen
// texture, general-queue submission and timeline semaphore, and its own
// Layer_t pushed into paint_all()'s frame (topmost of everything, see
// steamcompmgr.hpp's g_zposNotifications) -- not a second, competing shape.
#pragma once

#include <string>

class CVulkanCmdBuffer;
struct FrameInfo_t;

namespace gamescope::ui { class Area; }

namespace gamescope::Notifications
{
	enum class Kind
	{
		Info,
		Ok,      // renamed from the more obvious "Success" -- X11/X.h #defines Success to 0, and this header is pulled in after Xlib.h in steamcompmgr.cpp/rendervulkan.cpp
		Warning,
		Error,
	};

	// Live-tunable scale/opacity for the toast stack (window-chrome overhaul's
	// General tab, Config/ConfigSchema.h's OverlaySettings::notification_scale /
	// opacity_notifications). Same live-push shape as Palette.h's g_LiveTheme:
	// process-level and global.json-only (ApplyProfile() never touches
	// `overlay`, so a per-game override or applied profile never changes
	// these), seeded once from global.json by this file's own
	// EnsureConfigLoaded(), then written straight into by
	// PanelConfig.cpp's DrawGeneralTab() on every slider edit -- that bypass
	// is what makes the change visible the very next frame instead of
	// waiting on a config-generation bump that General-tab edits never
	// trigger. This file is the only reader.
	struct LiveTheme
	{
		float flScale = 1.0f;   // OverlaySettings::notification_scale
		float flOpacity = 0.9f; // OverlaySettings::opacity_notifications
	};
	extern LiveTheme g_LiveTheme;

	// Queues one toast for display. This is the "clean internal API...the
	// rest of the codebase can call" the task brief asks for -- e.g.
	// PanelConfig.cpp calls this when a profile is applied or an override is
	// toggled. Callable from anywhere on the steamcompmgr thread (the only
	// thread any real call site runs on today; this file keeps no lock of
	// its own, same single-thread assumption as FpsDisplay.cpp/PanelConfig.cpp).
	//
	// A no-op (nothing queued, no toast ever appears) when the current
	// session's notifications are muted -- config::NotificationSettings::muted,
	// resolved the normal per-game-override-else-global way every other
	// per-game-eligible setting is (Config/ConfigManager.h's ResolveEffective).
	// Overlong text is truncated with an ellipsis, and the queue is capped
	// (oldest dropped first) so a burst of Show() calls can never fill the
	// screen -- see Notifications.cpp's kMaxTextLen/kMaxQueued.
	void Show( std::string sText, Kind kind = Kind::Info, float flDurationSec = 4.0f );

	// Called once per paint_all(), on the steamcompmgr thread, same contract
	// as FpsDisplay_AddLayer() (see FpsDisplay.h): reads gamescope-ritz's
	// notification config (loaded lazily, refreshed on config generation
	// bump) and, when at least one toast is pending, draws the current stack
	// into its own offscreen texture and appends a Layer_t for it to
	// *pFrameInfo. A no-op when nothing is queued.
	void AddLayer( FrameInfo_t *pFrameInfo );

	// Called right after vulkan_composite()/vulkan_screenshot() obtain their
	// compute-queue command buffer, before recording any dispatches that
	// might sample the notification texture -- same cross-queue
	// synchronization role as FpsDisplay_WaitForRender(), against this
	// feature's own timeline semaphore. A no-op if nothing was rendered this
	// frame.
	void WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer );

	// Same contract as FpsDisplay_CommitReads() -- call once the compute
	// submission is on the queue. No-op when nothing is pending.
	void CommitReads();

	// Draws this feature's own settings controls (the global placement
	// picker, the per-game/global mute toggle, a "send test notification"
	// button) using whichever ImGui context is *currently active* at the
	// call site -- meant to be invoked from inside the settings overlay's
	// own panel draw (PanelConfig.cpp's "Notifications" tab), same contract
	// as FpsDisplay_DrawSettingsPanel(). Edits are persisted to
	// gamescope-ritz's config (placement always to global.json -- see
	// ConfigSchema.h's OverlaySettings comment -- mute through the same
	// session-routed write every other per-game-eligible panel uses).
	void DrawSettingsPanel();

	// P3 part B: the same three settings as E2 registrations, added into an
	// area the caller owns (Appearance hosts them). Declared here, and
	// implemented in Notifications.cpp, because the state they bind to --
	// s_GlobalOverlay, s_Settings, PersistPlacement(), PersistMuted() -- is
	// file-static there and must stay that way; a second file reaching it
	// would be a second writer of notification_placement.
	//
	// The nine placements are ONE Choice rather than the 3x3 grid
	// DrawSettingsPanel() draws. Not a preference: ui::Kind::Composite is
	// declared in the taxonomy but the shell does not render one yet, so an
	// Anchor here would be a control that registers correctly and draws
	// nothing -- precisely issues #25 and #68. One nine-option Choice is
	// still ONE setting with ONE value, which is what issue #27 was
	// actually about when it replaced two independent segmented controls
	// with the grid. The grid returns when Composite lands.
	void RegisterRows( ui::Area &area );
}
