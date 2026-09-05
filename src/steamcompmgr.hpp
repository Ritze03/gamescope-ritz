#include <stdint.h>

#include "wlr_begin.hpp"
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/dmabuf.h>
#include "wlr_end.hpp"

extern uint32_t currentOutputWidth;
extern uint32_t currentOutputHeight;

unsigned int get_time_in_milliseconds(void);
uint64_t get_time_in_nanos();
void sleep_for_nanos(uint64_t nanos);
void sleep_until_nanos(uint64_t nanos);
timespec nanos_to_timespec( uint64_t ulNanos );

void steamcompmgr_main(int argc, char **argv);

#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "vblankmanager.hpp"

#include <mutex>
#include <vector>

#include <X11/extensions/Xfixes.h>

struct _XDisplay;
struct steamcompmgr_win_t;
struct xwayland_ctx_t;
class gamescope_xwayland_server_t;

static const uint32_t g_zposBase = 0;
static const uint32_t g_zposOverride = 1;
static const uint32_t g_zposExternalOverlay = 2;
static const uint32_t g_zposOverlay = 3;
static const uint32_t g_zposCursor = 4;
static const uint32_t g_zposMuraCorrection = 5;
// FPS display (M4): its own always-drawn HUD layer -- independent of
// g_zposSettingsOverlay below so it stays visible whether or not the
// settings panel itself is open (see SPEC.md Feature 3's lifetime note: the
// two have separate visibility flags by design). Sits above the cursor and
// mura-correction layers but, per the 2026-09-03 reorder (see paint_all()'s
// comment in steamcompmgr.cpp), BELOW the settings overlay -- the user
// wants the HUD readable but not drawn over the settings panel/Shell.
static const uint32_t g_zposFpsDisplay = 6;
// Toast notifications: above the FPS HUD -- a toast confirming an action
// (e.g. a config profile applied from inside the settings panel) must stay
// visible whether or not the settings panel itself is open, same
// independent-lifetime reasoning as g_zposFpsDisplay above (see
// Overlay/Notifications.h). Like the HUD, drawn BELOW the settings overlay
// since 2026-09-03 -- see g_zposSettingsOverlay's comment.
static const uint32_t g_zposNotifications = 7;
// Settings overlay (M1): drawn topmost of all, above the cursor, the FPS
// HUD and the toasts -- matches how the Steam overlay/settings panels of
// other compositors sit above pointer decoration once open.
// Why: originally this sat *below* the FPS HUD and toasts (an inverted
// z-order that a stale comment in paint_all() claimed was the opposite of
// what the code actually did). The user asked for the HUD and toasts to
// sit beneath the settings overlay/Shell instead, so this constant moved
// to the top of the group on 2026-09-03 and paint_all() was reordered to
// match -- keep the push order in steamcompmgr.cpp's paint_all() and these
// values in agreement; the composite shader blends strictly in push order
// and does not sort by zpos (cs_composite_blit.comp / BlendLayer()).
static const uint32_t g_zposSettingsOverlay = 8;

extern bool g_bHDRItmEnable;
extern bool g_bForceHDRSupportDebug;

extern EStreamColorspace g_ForcedNV12ColorSpace;

struct CursorBarrierInfo
{
	int x1 = 0;
	int y1 = 0;
	int x2 = 0;
	int y2 = 0;
};

struct CursorBarrier
{
	PointerBarrier obj = None;
	CursorBarrierInfo info = {};
};

class MouseCursor
{
public:
	explicit MouseCursor(xwayland_ctx_t *ctx);

	int x() const;
	int y() const;

	void paint(steamcompmgr_win_t *window, steamcompmgr_win_t *fit, FrameInfo_t *frameInfo);
	void setDirty();

	// Will take ownership of data.
	bool setCursorImage(char *data, int w, int h, int hx, int hy);
	bool setCursorImageByName(const char *name);

	void hide()
	{
		wlserver_lock();
		wlserver_mousehide();
		wlserver_unlock( false );
		checkSuspension();
	}

	void UpdatePosition();

	bool isHidden() { return wlserver.bCursorHidden || m_imageEmpty; }
	bool imageEmpty() const { return m_imageEmpty; }

	void undirty() { getTexture(); }

	xwayland_ctx_t *getCtx() const { return m_ctx; }

	bool needs_server_flush() const { return m_needs_server_flush; }
	void inform_flush() { m_needs_server_flush = false; }

	void GetDesiredSize( int& nWidth, int &nHeight );

	void checkSuspension();

	bool IsConstrained() const { return m_bConstrained; }
private:

	bool getTexture();

	void updateCursorFeedback( bool bForce = false );

	int m_x = 0, m_y = 0;
	bool m_bConstrained = false;
	int m_hotspotX = 0, m_hotspotY = 0;

	gamescope::OwningRc<CVulkanTexture> m_texture;
	bool m_dirty;
	uint64_t m_ulLastConnectorId = 0;
	bool m_imageEmpty;

	xwayland_ctx_t *m_ctx;

	bool m_bCursorVisibleFeedback = false;
	bool m_needs_server_flush = false;
};

extern std::vector< wlr_surface * > wayland_surfaces_deleted;

extern bool hasFocusWindow;

// These are used for touch scaling, so it's really the window that's focused for touch
extern float focusedWindowScaleX;
extern float focusedWindowScaleY;
extern float focusedWindowOffsetX;
extern float focusedWindowOffsetY;

extern bool g_bFSRActive;

extern uint32_t inputCounter;
extern uint64_t g_lastWinSeq;

void nudge_steamcompmgr( void );
void force_repaint( void );

// The base plane's own committed buffer size -- the game's render
// resolution before any stretch/upscale gamescope applied -- as of the
// last time paint_window_commit() painted the base plane. Written and read
// on the steamcompmgr thread only. Overlay/FpsDisplay.cpp divides layer 0's
// on-screen size by this to get "output pixels per game pixel" per axis,
// which is what the crosshair's Apply Scaling needs (see
// superdoc/features/crosshair.md). Kept separate from Layer_t because
// layer 0's texture may already be gamescope's pre-emptively upscaled copy
// (ShouldPreemptivelyUpscale()), whose size says nothing about the game's.
extern uint32_t g_uBaseLayerSourceWidth;
extern uint32_t g_uBaseLayerSourceHeight;

extern void mangoapp_update( uint64_t visible_frametime, uint64_t app_frametime_ns, uint64_t latency_ns );
struct wlr_surface *steamcompmgr_get_server_input_surface( size_t idx );
wlserver_vk_swapchain_feedback* steamcompmgr_get_base_layer_swapchain_feedback();

struct wlserver_x11_surface_info *lookup_x11_surface_info_from_xid( gamescope_xwayland_server_t *xwayland_server, uint32_t xid );

extern gamescope::VBlankTime g_SteamCompMgrVBlankTime;
extern pid_t focusWindow_pid;
extern std::atomic<std::shared_ptr<std::string>> focusWindow_engine;

void init_xwayland_ctx(uint32_t serverId, gamescope_xwayland_server_t *xwayland_server);
void gamescope_set_selection(std::string contents, GamescopeSelection eSelection);

// Clipboard sync -- see superdoc/features/clipboard-sync.md.
//
// Post new CLIPBOARD contents from any thread. The steamcompmgr thread picks
// it up on its next loop and broadcasts it to every Xwayland server, to our
// own native Wayland clients, and to the host compositor. Needed because the
// transfer that produced the text ran on a worker thread, and none of those
// three destinations may be touched from there.
void gamescope_post_selection( std::string contents );
void gamescope_set_reshade_effect(std::string effect_path);
void gamescope_clear_reshade_effect();

MouseCursor *steamcompmgr_get_current_cursor();
MouseCursor *steamcompmgr_get_server_cursor(uint32_t serverId);

extern gamescope::ConVar<bool> cv_tearing_enabled;

extern void steamcompmgr_set_app_refresh_cycle_override( gamescope::GamescopeScreenType type, int override_fps, bool change_refresh, bool change_fps_cap );

// Issue #68: the only live-effect entry point for g_bForceRelativeMouse --
// see this function's own definition comment in steamcompmgr.cpp for why
// writing the global directly (as the pre-#68 PanelDisplay.cpp did) has no
// effect once the process is already running.
extern void steamcompmgr_set_force_relative_mouse( bool bForce );

// Nudges the game-side fallback-cursor policy (steamcompmgr.cpp's
// SetDefaultCursorImage(), gated by the Cursor tab's "Use everywhere"
// toggle -- PanelCursor.h's CursorAppearance::bEverywhere) to reapply on
// the steamcompmgr thread next frame. Safe to call from any thread: it only
// flips an atomic flag, mirroring steamcompmgr_set_force_relative_mouse()'s
// own cross-thread handoff above -- MouseCursor itself is only ever touched
// from the steamcompmgr thread's own per-frame loop, never from here. See
// steamcompmgr.cpp's ApplyDefaultCursorPolicy()/ProcessPendingCursorFallbackPolicy().
extern void steamcompmgr_notify_cursor_appearance_changed();
