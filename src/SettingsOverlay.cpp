// M1 ImGui render shell -- see SettingsOverlay.h and
// superdoc/planning/SPEC.md's Architecture section for the design this
// follows. Renders a placeholder ImGui window into an offscreen texture on
// CVulkanDevice's general (graphics+compute) queue, then hands the finished
// texture to paint_all() as one more FrameInfo_t layer, exactly like the
// "HACK HACK HACK" blank-overlay layer already does for a client-less
// texture (src/steamcompmgr.cpp, paint_all()).
//
// Cross-queue synchronization: ImGui draws on the general queue while
// vulkan_composite() samples the result on the compute queue, in the same
// frame. The two are tied together with a dedicated Vulkan timeline
// semaphore (CVulkanDevice::CreateTimelineSemaphore(), the same primitive
// CTimeline/Timeline.h wraps for DRM-syncobj cross-process sync -- this is
// the same mechanism, just not DRM-syncobj-backed since both sides live in
// the same VkDevice/process and never need to leave it):
//   - the general-queue command buffer that draws ImGui AddSignal()s it;
//   - the compute-queue command buffer that composites AddDependency()s on
//     that exact signal point, via SettingsOverlay_WaitForRender(), before
//     it can be recorded to sample the overlay texture.
// That GPU-side wait is what actually prevents vulkan_composite() from ever
// reading a torn/still-being-drawn overlay frame; nothing here CPU-blocks to
// achieve that. The one CPU-side wait that does exist (below, before
// re-recording a new overlay frame) is only to know when it's safe to reuse/
// free *our own* previous general-queue command buffer -- a Vulkan command
// buffer must not be freed while it may still be executing -- and is
// expected to return immediately, since a full vblank period (during which
// the compute queue's own GPU-side wait already ran) has elapsed by then.
//
// ponytail: the overlay texture is created VK_SHARING_MODE_EXCLUSIVE (the
// only mode CVulkanTexture::BInit() supports) and never gets an explicit
// queue-family-ownership-transfer barrier pair (a "release" on the general
// queue, matching "acquire" on the compute queue) when the two queue
// families genuinely differ (they don't on many current GPUs, where
// CVulkanDevice collapses to one combined family -- see the queue family
// selection in rendervulkan.cpp). The timeline semaphore above still fully
// prevents the actual data race (no torn/stale reads: signal+wait carries
// its own memory dependency per the Vulkan spec), so this is a validation-
// layer-level gap, not a functional one, on hardware with genuinely split
// queue families. Upgrade path: create the texture VK_SHARING_MODE_CONCURRENT
// across both families, or add the release/acquire barrier pair.

#include "SettingsOverlay.h"
#include "Overlay/PanelDisplay.h"
#include "Overlay/FpsDisplay.h"
#include "Overlay/PanelShaders.h"
#include "Overlay/PanelAudio.h"
#include "Overlay/PanelConfig.h"
#include "Overlay/PanelLog.h"
#include "Overlay/Fonts.h"
#include "Overlay/Widgets.h"
#include "Overlay/Chrome.h"
#include "Overlay/Palette.h"
#include "Overlay/UI/Shell.h"
#include "Config/ConfigManager.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include <linux/input-event-codes.h>

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "main.hpp"
#include "log.hpp"
#include "convar.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

namespace gamescope
{
	static LogScope s_OverlayLog( "settings_overlay" );

	// M2: the single source of truth for "does the overlay currently own all
	// input" is g_bSettingsOverlayCapturing (below), a std::atomic<bool> so
	// wlserver's input handlers (main thread) can read it without racing the
	// ConVar's own plain, non-atomic m_Value (read every frame on the
	// steamcompmgr thread by UpdateFadeAlpha() below -- a pre-existing M1
	// simplification this milestone doesn't need to fix, since the atomic
	// mirror is now the thing anything correctness-sensitive reads). The
	// ConVar's callback keeps the mirror in sync however the ConVar changes
	// (gamescopectl, the console, or SettingsOverlay_ToggleVisible() below).
	static std::atomic<bool> g_bSettingsOverlayCapturing = false;

	static ConVar<bool> cv_settings_overlay_visible(
		"settings_overlay_visible", false,
		"Show/hide the settings overlay. Ctrl+Shift+O toggles it too.",
		[]( ConVar<bool> &cv )
		{
			g_bSettingsOverlayCapturing.store( cv.Get(), std::memory_order_release );
		},
		/* bRunCallbackAtStartup = */ true );

	static ConCommand cc_toggle_settings_overlay(
		"toggle_settings_overlay", "Toggle the settings overlay.",
		[]( std::span<std::string_view> args )
		{
			cv_settings_overlay_visible.SetValue( !cv_settings_overlay_visible.Get() );
		} );

	// Keyboard-control toggles (see ConfigSchema.h's OverlaySettings comment
	// for the interpretation these implement, and SettingsOverlay.h for the
	// exact contract each has with wlserver.cpp).
	//
	// Seeded from global.json once at startup (below, alongside the startup
	// announcement's own config read -- see EnsureStartupAnnounceConfigLoaded())
	// rather than left at their compiled-in defaults forever: both are
	// process-level "how the overlay behaves" preferences, the same class as
	// fade_ms, so a value the user has previously chosen (were a future
	// panel to expose these) should stick across launches. Runtime changes
	// via the console/gamescopectl only affect the current session, exactly
	// like every other ConVar in this codebase -- persisting a live edit
	// back to disk is future work for whichever panel ends up hosting these
	// (see this task's report).
	static ConVar<bool> cv_settings_overlay_capture_keyboard(
		"settings_overlay_capture_keyboard", true,
		"true (default): the overlay captures all keyboard input while open, none reaches the game (M2 behavior). "
		"false: keyboard input passes through to the game even while the overlay is open (mouse capture is unaffected). "
		"Ctrl+Shift+O always still works to close the overlay either way." );

	static ConVar<bool> cv_settings_overlay_keyboard_nav(
		"settings_overlay_keyboard_nav", true,
		"Whether Tab/arrow-key ImGui keyboard navigation of the overlay's own widgets is enabled while it holds keyboard capture." );

	void SettingsOverlay_ToggleVisible()
	{
		cv_settings_overlay_visible.SetValue( !cv_settings_overlay_visible.Get() );
	}

	bool SettingsOverlay_IsCapturingInput()
	{
		return g_bSettingsOverlayCapturing.load( std::memory_order_acquire );
	}

	bool SettingsOverlay_IsCapturingKeyboard()
	{
		return SettingsOverlay_IsCapturingInput() && cv_settings_overlay_capture_keyboard.Get();
	}

	// Fade in/out on toggle (decision: replaces the dropped backdrop-blur
	// treatment, see DECISIONS.md #5). SPEC leaves the exact duration as a
	// design decision, not an engineering one -- 200ms is picked here as a
	// reasonable default, matching gamescope's own fade-out duration order of
	// magnitude elsewhere (g_FadeOutDuration).
	static constexpr unsigned int k_uOverlayFadeMs = 200;

	// EXPERIMENTAL (blurred-background branch): DECISIONS.md #5 dropped true
	// backdrop blur because ImGui itself cannot sample-and-blur what's behind
	// a window. But gamescope's *compositor* already can: FrameInfo_t::
	// blurLayer0 (rendervulkan.hpp) drives a real two-pass separable Gaussian
	// blur of layer 0 (src/shaders/cs_gaussian_blur_horizontal.comp +
	// cs_composite_blur.comp) that's been shipping for years as the "blur the
	// game behind Steam's own overlay/QAM" feature (g_BlurMode, driven by the
	// GAMESCOPE_BLUR_MODE X11 property, see steamcompmgr.cpp). That is
	// *exactly* the primitive this design needs: blur the base layer, draw
	// our translucent panel layer -- already at the design's 0.88 `surface`
	// alpha, see Overlay/Widgets.cpp -- on top of it, unblurred. So instead of
	// inventing a second blur pass/pipeline, this reuses that one: below,
	// SettingsOverlay_AddLayer() requests it on *pFrameInfo directly (via
	// std::max against whatever Steam's own g_BlurMode already asked for, so
	// the two requests compose instead of one clobbering the other) whenever
	// the overlay is visible or still fading, entirely inside this file --
	// no changes to steamcompmgr.cpp, rendervulkan.{hpp,cpp}, or a new shader
	// needed.
	//
	// Radius: BlitPushData_t turns FrameInfo_t::blurRadius into the shader's
	// u_blur_radius as (blurRadius*2)-1 (rendervulkan.cpp), and blur.h's
	// gaussian_blur() picks its weight/offset table by thresholding that
	// value -- u_blur_radius=21 selects the table whose offsets extend to
	// ~20.4px (the "radius<=21" branch), landing almost exactly on the design
	// guide's specified blur(20-22px). Solving (blurRadius*2)-1=21 gives
	// blurRadius=11, so that's the constant used below -- now as the CEILING
	// of the range OverlaySettings::background_blur (0..1, General tab) maps
	// onto, rather than a fixed value: background_blur=1.0 reproduces the
	// original design-matched ~20.4px blur exactly, background_blur=0.0
	// means no blur pass is requested at all (see the radius computation
	// below), and everything in between scales linearly.
	static constexpr int k_nMaxOverlayBlurRadius = 11;

	// ponytail: this blurs the *entire* base layer uniformly while the
	// overlay is visible, not a per-window region sampled from directly
	// behind each floating panel (true CSS-style backdrop-filter). That's a
	// real, deliberate simplification from the design guide's "blur behind
	// every floating window" wording -- but it is exactly what this
	// codebase's existing Steam-blur feature already does (blur everything,
	// draw overlay layers sharp on top), so it's reusing an established
	// pattern rather than inventing a cheaper one. A true per-window masked
	// blur would need the blur pass to know each panel's screen rect and
	// composite region-by-region -- meaningfully more machinery for a look
	// that, with the panels' own near-opaque 0.88 alpha, reads very similarly
	// in practice. See the task's measured screenshots/verdict for whether
	// that gap matters visually.
	//
	// ponytail: does not add the design guide's `saturate(1.1-1.15)` boost --
	// that needs its own shader tap (or a change to the existing blur
	// shaders, which are shared with Steam's own blur feature and out of
	// this branch's shader-touching budget); the blur radius alone is the
	// experiment's one thing to get right.

	static bool s_bImguiInitialized = false;

	static OwningRc<CVulkanTexture> s_pOverlayTexture;
	static uint32_t s_uTextureWidth = 0;
	static uint32_t s_uTextureHeight = 0;
	// The freshly-(re)created texture has never been rendered into, so its
	// layout is still VK_IMAGE_LAYOUT_UNDEFINED and needs an explicit initial
	// transition. Every other frame it's already GENERAL from the previous
	// frame's own end-of-render state -- see the file-level comment.
	static bool s_bTextureNeedsInitialBarrier = true;

	// The general-queue command buffer from the previous frame we rendered,
	// kept alive only long enough to CPU-confirm it's done (see file-level
	// comment) before being freed/replaced.
	static std::unique_ptr<CVulkanCmdBuffer> s_pPrevCmdBuffer;
	static std::shared_ptr<VulkanTimelineSemaphore_t> s_pTimelineSemaphore;
	static uint64_t s_ulSignalCounter = 0;
	static uint64_t s_ulPrevSignalPoint = 0;
	static bool s_bHasPrevSubmission = false;

	// Set once per SettingsOverlay_AddLayer() render, read by every
	// SettingsOverlay_WaitForRender() call for the rest of that paint_all()
	// invocation -- there can be more than one consumer of the overlay texture
	// in a single frame (the live vulkan_composite() Present() path, and a
	// vulkan_screenshot() taken the same frame), and a Vulkan timeline wait is
	// safe to add to more than one submission waiting on the same point (an
	// already-reached point is just an immediate no-op wait), so this is
	// intentionally *not* consumed/cleared after one use.
	static bool s_bHasPendingWaitPoint = false;
	static uint64_t s_ulPendingWaitPoint = 0;

	// Issue #22, return half of the cross-queue handshake.
	//
	// s_pTimelineSemaphore above only orders render -> composite (general
	// queue writes the texture, compute queue then samples it). Nothing
	// ordered composite -> next render, so the next frame's
	// VK_ATTACHMENT_LOAD_OP_CLEAR could begin on the general queue while the
	// compute queue was still sampling the same image: a write-after-read
	// race over a full-screen, alpha-blended layer, ie. corruption anywhere
	// on screen rather than just in the overlay's own pixels.
	//
	// Every vulkan_composite() submission that could sample this texture
	// signals s_pReadDoneSemaphore at a fresh point, and the next
	// RenderAndSubmit() makes its general-queue submission WAIT on the last
	// such point before it clears. That closes the window rather than
	// narrowing it, and needs no second texture -- the reverted
	// double-buffering attempt (e171f72) only moved the race one vblank
	// further out.
	//
	// Deadlock safety: a point is only ever waited on after it has been
	// promoted to s_ulRegisteredReadDonePoint, which happens exclusively in
	// SettingsOverlay_CommitReads(), called only once the compute submission
	// carrying that signal has actually been handed to the queue. A pending
	// point belonging to a command buffer that got dropped is simply never
	// promoted, and the general queue never blocks on it.
	static std::shared_ptr<VulkanTimelineSemaphore_t> s_pReadDoneSemaphore;
	static uint64_t s_ulReadDoneCounter = 0;
	static uint64_t s_ulPendingReadDonePoint = 0;
	static uint64_t s_ulRegisteredReadDonePoint = 0;

	static bool s_bWasVisible = false;
	static unsigned int s_uFadeAnchorTimeMs = 0;
	static float s_flFadeAtAnchor = 0.0f;
	static float s_flCurrentAlpha = 0.0f;

	static uint64_t s_ulLastFrameTimeNanos = 0;

	// Definition of the live blur/darkening state declared in
	// SettingsOverlay.h -- see that header's comment for the full seed-once/
	// push-on-edit contract this follows (Palette.h's/Notifications.h's
	// g_LiveTheme shape).
	BackgroundLiveTheme g_BackgroundLiveTheme;

	static bool s_bBackgroundLiveThemeLoaded = false;

	static void EnsureBackgroundLiveThemeLoaded()
	{
		if ( s_bBackgroundLiveThemeLoaded )
			return;
		s_bBackgroundLiveThemeLoaded = true;
		// Seeded straight from global.json -- this only ever needs to run
		// once (PanelConfig.cpp's DrawGeneralTab() keeps g_BackgroundLiveTheme
		// current after this), and background_blur/background_darkening are
		// process-level/global.json-only fields (ConfigSchema.h's
		// OverlaySettings comment).
		const config::OverlaySettings &o = config::LoadGlobal().overlay;
		g_BackgroundLiveTheme.flBlur = o.background_blur;
		g_BackgroundLiveTheme.flDarkening = o.background_darkening;
	}

	// This file's own ImGui context. Held explicitly rather than relying on
	// whatever happens to be globally current: there are TWO ImGui contexts in
	// this process (this one and Overlay/FpsDisplay.cpp's independent FPS HUD),
	// each with its own ImGui_ImplVulkan backend data, font atlas, descriptor
	// pool and per-frame vertex/index buffers. Rendering one context's draw data
	// while the other's backend is current would hand ImGui the wrong buffers and
	// descriptor sets entirely.
	//
	// Previously this file relied on ImGui::CreateContext() leaving its context
	// current forever and on FpsDisplay always restoring it. That happened to
	// hold, but only because of the exact init order and FpsDisplay's discipline
	// -- it was a global-state coupling between two files with no enforcement.
	// Both files now save/set/restore symmetrically, so neither depends on the
	// other's behaviour.
	static ImGuiContext *s_pImguiContext = nullptr;

	// RAII save/restore for the globally-current ImGui context. Every exit path
	// out of the draw pass below restores what was current on entry.
	struct ScopedImguiContext
	{
		ImGuiContext *pPrev;
		explicit ScopedImguiContext( ImGuiContext *pUse )
			: pPrev( ImGui::GetCurrentContext() )
		{
			if ( pUse )
				ImGui::SetCurrentContext( pUse );
		}
		~ScopedImguiContext() { ImGui::SetCurrentContext( pPrev ); }
		ScopedImguiContext( const ScopedImguiContext & ) = delete;
		ScopedImguiContext &operator=( const ScopedImguiContext & ) = delete;
	};

	// Creates this file's context if needed and leaves it CURRENT on success.
	// The caller is responsible for restoring the previous context (see
	// ScopedImguiContext) -- mirrors FpsDisplay.cpp's EnsureImguiInit().
	static void EnsureImguiInit()
	{
		if ( s_bImguiInitialized )
		{
			ImGui::SetCurrentContext( s_pImguiContext );
			return;
		}

		// Guards against leaking a context if ImGui_ImplVulkan_Init() below
		// fails: EnsureImguiInit() is retried every frame while the overlay is
		// visible (s_bImguiInitialized only ever latches true, never false),
		// so without this a repeated failure would call CreateContext() again
		// on every single frame. Keyed on our OWN context handle now, not on
		// "is any context current" -- the old form also aborted init whenever
		// another file's context merely happened to be current, which with two
		// contexts in the process is not a safe thing to key on.
		if ( s_pImguiContext != nullptr )
		{
			ImGui::SetCurrentContext( s_pImguiContext );
			return;
		}

		IMGUI_CHECKVERSION();
		s_pImguiContext = ImGui::CreateContext();
		// CreateContext() only makes the new context current when there was no
		// current context; be explicit rather than depend on that.
		ImGui::SetCurrentContext( s_pImguiContext );

		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr; // no persisted window layout in M1
		// M2: no wl_surface/hardware cursor plane is driven for the overlay
		// (out of scope, see the ponytail note near SettingsOverlay_QueueMouseMotionAbsolute's
		// caller in wlserver.cpp) -- ask ImGui to draw its own software
		// cursor into the offscreen texture instead so the pointer is
		// visible at all while the overlay owns it.
		io.MouseDrawCursor = true;
		// M8 part 2 (issue #14): applies the "glass instrument" ImGuiStyle
		// palette/metrics from ui-design-guide.md's Component styling
		// section -- see Overlay/Widgets.h/.cpp for the full styling (this
		// used to be a function local to this file; moved out because #14
		// owns widget-level styling, see Widgets.h's file comment for the
		// scope boundary with #15, which owns window/panel chrome instead).
		gamescope::widgets::ApplyStyle();

		// M8 part 1 (issue #13, typeface swapped to Geist by #53): builds
		// the Geist font atlas for this context. Must happen before
		// ImGui_ImplVulkan_Init() below -- the Vulkan backend uploads
		// whatever io.Fonts holds at Init() time, so the atlas has to be
		// finished first (see Overlay/Fonts.h).
		gamescope::fonts::Load();

		s_pTimelineSemaphore = g_device.CreateTimelineSemaphore( 0, /* bShared = */ false );
		s_pReadDoneSemaphore = g_device.CreateTimelineSemaphore( 0, /* bShared = */ false );

		static VkFormat s_ColorAttachmentFormat = VK_FORMAT_B8G8R8A8_UNORM;

		ImGui_ImplVulkan_InitInfo init_info = {};
		init_info.ApiVersion = VK_API_VERSION_1_3;
		init_info.Instance = g_device.instance();
		init_info.PhysicalDevice = g_device.physDev();
		init_info.Device = g_device.device();
		init_info.QueueFamily = g_device.generalQueueFamily();
		init_info.Queue = g_device.generalQueue();
		init_info.DescriptorPool = VK_NULL_HANDLE;
		init_info.DescriptorPoolSize = 64; // >= IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE
		init_info.MinImageCount = 2;
		init_info.ImageCount = 2;
		init_info.PipelineCache = VK_NULL_HANDLE;
		init_info.UseDynamicRendering = true;
		init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
		init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
		init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_ColorAttachmentFormat;
		init_info.CheckVkResultFn = []( VkResult err )
		{
			if ( err != VK_SUCCESS )
				s_OverlayLog.errorf( "ImGui Vulkan backend: VkResult %d", (int)err );
		};

		if ( !ImGui_ImplVulkan_Init( &init_info ) )
		{
			s_OverlayLog.errorf( "ImGui_ImplVulkan_Init failed" );
			return;
		}

		s_bImguiInitialized = true;
	}

	// CPU-waits for our own previous general-queue submission to retire, then
	// releases its command buffer. Two callers: RenderAndSubmit(), where it is
	// expected to return immediately and only guards reuse of the command
	// buffer, and EnsureTexture(), where it additionally guarantees nothing is
	// still writing the texture we are about to drop.
	static void DrainPrevSubmission()
	{
		if ( !s_bHasPrevSubmission )
			return;

		VkSemaphoreWaitInfo waitInfo = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &s_pTimelineSemaphore->pVkSemaphore,
			.pValues = &s_ulPrevSignalPoint,
		};
		g_device.vk.WaitSemaphores( g_device.device(), &waitInfo, UINT64_MAX );
		s_pPrevCmdBuffer.reset();
		s_bHasPrevSubmission = false;
	}

	static bool EnsureTexture( uint32_t uWidth, uint32_t uHeight )
	{
		if ( s_pOverlayTexture && s_uTextureWidth == uWidth && s_uTextureHeight == uHeight )
			return true;

		// Output resolution changed (or first ever texture). Unlike the
		// compute side -- whose CVulkanCmdBuffer::m_textureRefs holds its own
		// Rc<> on everything it sampled -- the general-queue ImGui submission
		// references the texture only as a raw VkImage/VkImageView in its
		// VkRenderingAttachmentInfo, so it keeps nothing alive. Dropping
		// s_pOverlayTexture below while that submission is still in flight
		// could therefore destroy an image the GPU is actively rendering
		// into. Resizes are rare, so just drain first rather than building
		// deferred-destruction machinery for it.
		DrainPrevSubmission();

		OwningRc<CVulkanTexture> pNewTexture = new CVulkanTexture();

		CVulkanTexture::createFlags flags;
		flags.bSampled = true;
		flags.bColorAttachment = true;
		// See createFlags::bGeneralQueueShared. This texture is written on the
		// general queue (RenderAndSubmit) and sampled on the compute queue
		// (vulkan_composite), which are different queue families on AMD.
		flags.bGeneralQueueShared = true;

		// Render-target + sampled, per superdoc/planning/SPEC.md's Architecture
		// section -- no new CVulkanTexture usage flag is needed for this.
		if ( !pNewTexture->BInit( uWidth, uHeight, 1u, VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM ), flags ) )
		{
			s_OverlayLog.errorf( "failed to (re)create the overlay's offscreen texture at %ux%u", uWidth, uHeight );
			return false;
		}

		// If a compute submission from a previous frame is still sampling the
		// old s_pOverlayTexture, its own Rc<> ref (held by that submission's
		// CVulkanCmdBuffer::m_textureRefs) keeps it alive until that
		// submission is recycled -- replacing our own reference here is safe.
		s_pOverlayTexture = std::move( pNewTexture );
		s_uTextureWidth = uWidth;
		s_uTextureHeight = uHeight;
		s_bTextureNeedsInitialBarrier = true;
		return true;
	}

	static void UpdateFadeAlpha()
	{
		// Reads the atomic mirror, not the ConVar directly -- see the
		// comment above g_bSettingsOverlayCapturing's declaration. This is
		// the steamcompmgr thread; the ConVar can change from wlserver's
		// main-thread hotkey handler or from gamescopectl/console on
		// whatever thread issues that command.
		const bool bVisible = SettingsOverlay_IsCapturingInput();
		const unsigned int uNow = get_time_in_milliseconds();

		if ( bVisible != s_bWasVisible )
		{
			s_flFadeAtAnchor = s_flCurrentAlpha;
			s_uFadeAnchorTimeMs = uNow;
			s_bWasVisible = bVisible;
		}

		const float flElapsed = float( uNow - s_uFadeAnchorTimeMs );
		const float flT = k_uOverlayFadeMs > 0 ? std::clamp( flElapsed / (float)k_uOverlayFadeMs, 0.0f, 1.0f ) : 1.0f;
		const float flTarget = bVisible ? 1.0f : 0.0f;

		s_flCurrentAlpha = s_flFadeAtAnchor + ( flTarget - s_flFadeAtAnchor ) * flT;
	}

	// M8 part 3 (issue #15): hosted through chrome::BeginPanelWindow() as the
	// dock's "System Monitor" panel (see Overlay/Chrome.h) -- SPEC.md's UI
	// structure lists an "FPS HUD panel" hosting exactly
	// FpsDisplay_DrawSettingsPanel()'s controls, which this window has done
	// since M4; issue #27 renamed the panel/dock label/PanelId to "System
	// Monitor" (the panel now hosts a module framework, not just FPS) while
	// leaving the underlying FpsDisplay* file/function names alone (see
	// FpsDisplay.h's own header comment for why).
	//
	// The M1 render-shell scaffolding that used to live in this window
	// (the "Settings overlay render shell -- Milestone 1" heading, its proof-
	// of-pipeline paragraph, the "Layer alpha" readout, a dummy slider/toggle,
	// the toggle-hotkey hint line, and an M2 capture-check text field) has
	// been removed here -- it was developer-only scaffolding that was never
	// meant to ship to end users. Removing it does not remove the *ability*
	// to manually verify input capture: the real panels already have plenty
	// of sliders/toggles (Display, Shaders) and a text field (Config/Profiles'
	// "New profile name"), so those are the manual-test surface for pointer/
	// keyboard capture now.
	static void DrawFpsHudPanel()
	{
		if ( !gamescope::chrome::BeginPanelWindow( "SYSTEM MONITOR", gamescope::chrome::PanelId::SystemMonitor,
			ImVec2( 64.0f, 64.0f ), ImVec2( 460.0f, 640.0f ) ) )
			return;

		gamescope::FpsDisplay_DrawSettingsPanel(); // M4 (see FpsDisplay.h)

		gamescope::chrome::EndPanelWindow();
	}

	// ----------------------------------------------------------------------
	// Startup announcement: a brief, self-dismissing "gamescope-ritz is
	// active" toast with the Ctrl+Shift+O hint, shown once per process
	// launch regardless of whether the user ever opens the settings overlay
	// (task brief: "on start, show some kind of animation ... with a hint on
	// how to open the overlay"). Drawn straight onto the same offscreen
	// texture/ImGui context/frame as the settings overlay itself (no second
	// Vulkan pipeline, no second timeline handshake) but entirely
	// independent of cv_settings_overlay_visible -- see
	// SettingsOverlay_AddLayer()'s bNeedsRender/bDrawPanels split below.
	// ----------------------------------------------------------------------

	// Total budget kept short and one-shot deliberately (task brief: "appears
	// over the game every launch, so anything slow or loud becomes an
	// irritation by the tenth time"): ~2.6s end to end, most of it the
	// (non-blocking, purely cosmetic) hold phase.
	static constexpr unsigned int k_uStartupAnnounceFadeInMs  = 280;
	static constexpr unsigned int k_uStartupAnnounceHoldMs    = 1800;
	static constexpr unsigned int k_uStartupAnnounceFadeOutMs = 550;
	static constexpr unsigned int k_uStartupAnnounceTotalMs =
		k_uStartupAnnounceFadeInMs + k_uStartupAnnounceHoldMs + k_uStartupAnnounceFadeOutMs;

	static bool s_bStartupAnnounceConfigLoaded = false;
	static bool s_bStartupAnnounceEnabled = true;
	static bool s_bStartupAnnounceStarted = false;
	static unsigned int s_uStartupAnnounceStartMs = 0;

	// Lazy, one-time read of the disable switch -- matches the established
	// pattern every panel's own EnsureConfigLoaded() already uses for its
	// first draw (see e.g. PanelDisplay.cpp), so this isn't a new kind of
	// blocking-I/O-on-the-steamcompmgr-thread exception, just the same one.
	// LoadGlobal() directly, not ResolveEffective() -- OverlaySettings is a
	// process-level preference that only ever lives in global.json (see
	// ConfigSchema.h's comment on OverlaySettings), so a per-game override
	// file is never the right place to look this up.
	static void EnsureStartupAnnounceConfigLoaded()
	{
		if ( s_bStartupAnnounceConfigLoaded )
			return;
		s_bStartupAnnounceConfigLoaded = true;
		s_bStartupAnnounceEnabled = config::LoadGlobal().overlay.startup_announce_enabled;
	}

	// flT in 0..1: fade-in (ease-out cubic, feels snappier/more "game-like"
	// than linear for a slide-and-fade entrance), full hold, then a plain
	// linear fade-out. Returns 0 once the whole thing has finished.
	static float StartupAnnounceAlpha( unsigned int uElapsedMs )
	{
		if ( uElapsedMs < k_uStartupAnnounceFadeInMs )
		{
			const float t = float( uElapsedMs ) / float( k_uStartupAnnounceFadeInMs );
			return 1.0f - ( 1.0f - t ) * ( 1.0f - t ) * ( 1.0f - t );
		}

		const unsigned int uAfterHold = k_uStartupAnnounceFadeInMs + k_uStartupAnnounceHoldMs;
		if ( uElapsedMs < uAfterHold )
			return 1.0f;

		if ( uElapsedMs < k_uStartupAnnounceTotalMs )
		{
			const float t = float( uElapsedMs - uAfterHold ) / float( k_uStartupAnnounceFadeOutMs );
			return 1.0f - t;
		}

		return 0.0f;
	}

	// Draws the toast card with the ImGui foreground draw list of the
	// current frame -- deliberately not an ImGui::Begin() window: it needs
	// no title bar, no focus/Z-order interaction with the dock/panel windows
	// (Chrome.h's territory, not touched here), and no input at all (it is
	// purely decorative and self-dismissing). Every color this draws bakes
	// flAlpha in directly (rather than relying on the FrameInfo_t layer's
	// own opacity, which SettingsOverlay_AddLayer() may set to something
	// else entirely if the settings overlay's own panels happen to be
	// rendering in the same frame -- see that function's comment) so the
	// toast's own fade is correct standalone regardless.
	static void DrawStartupAnnounce( float flAlpha, unsigned int uElapsedMs )
	{
		flAlpha = std::clamp( flAlpha, 0.0f, 1.0f );
		if ( flAlpha <= 0.0f )
			return;

		ImDrawList *pDrawList = ImGui::GetForegroundDrawList();

		// Slide down into place on the way in (ease-out, matching
		// StartupAnnounceAlpha's own curve so the motion and the fade read
		// as one movement); holds still once fully in; drifts up slightly on
		// the way out. Purely a Y offset -- keeps this a simple, cheap
		// animation rather than needing a transform on every draw call.
		float flSlideOffsetY = 0.0f;
		if ( uElapsedMs < k_uStartupAnnounceFadeInMs )
		{
			const float t = float( uElapsedMs ) / float( k_uStartupAnnounceFadeInMs );
			const float eased = 1.0f - ( 1.0f - t ) * ( 1.0f - t ) * ( 1.0f - t );
			flSlideOffsetY = ( 1.0f - eased ) * -18.0f;
		}
		else
		{
			const unsigned int uAfterHold = k_uStartupAnnounceFadeInMs + k_uStartupAnnounceHoldMs;
			if ( uElapsedMs > uAfterHold )
			{
				const float t = std::clamp( float( uElapsedMs - uAfterHold ) / float( k_uStartupAnnounceFadeOutMs ), 0.0f, 1.0f );
				flSlideOffsetY = -8.0f * t;
			}
		}

		const float flCardWidth = 480.0f;
		const float flPadX = 22.0f;
		const float flPadTop = 16.0f;
		const char *pszTitle = "GAMESCOPE-RITZ ACTIVE";
		const char *pszHint = "opens the settings overlay";
		const char *pszHotkey = "CTRL + SHIFT + O";

		ImFont *pTitleFont = gamescope::fonts::Get( gamescope::fonts::Style::Hero );
		ImFont *pHintFont = gamescope::fonts::Get( gamescope::fonts::Style::Meta );
		const float flTitleSize = 20.0f;
		const float flHintSize = 13.0f;

		const ImVec2 titleSize = pTitleFont->CalcTextSizeA( flTitleSize, FLT_MAX, 0.0f, pszTitle );
		const ImVec2 hotkeySize = pHintFont->CalcTextSizeA( flHintSize, FLT_MAX, 0.0f, pszHotkey );
		const ImVec2 hintSize = pHintFont->CalcTextSizeA( flHintSize, FLT_MAX, 0.0f, pszHint );

		const float flUnderlineY = flPadTop + titleSize.y + 10.0f;
		const float flHintY = flUnderlineY + 12.0f;
		const float flCardHeight = flHintY + hintSize.y + 14.0f;

		const float flOutputWidth = (float)s_uTextureWidth;
		const float flCardX = ( flOutputWidth - flCardWidth ) * 0.5f;
		const float flCardY = 56.0f + flSlideOffsetY;

		const ImVec2 cardMin( flCardX, flCardY );
		const ImVec2 cardMax( flCardX + flCardWidth, flCardY + flCardHeight );

		// Card: surface fill + hairline border, same tokens the rest of the
		// overlay's chrome uses (Palette.h) so this reads as the same
		// product, not a bolted-on splash screen.
		pDrawList->AddRectFilled( cardMin, cardMax, palette::Black( 0.35f * flAlpha ), 4.0f );
		pDrawList->AddRectFilled( cardMin, cardMax, IM_COL32( 0x09, 0x0A, 0x0C, (int)( 0.90f * flAlpha * 255.0f + 0.5f ) ), 4.0f );
		pDrawList->AddRect( cardMin, cardMax, palette::White( 0.10f * flAlpha ), 4.0f );

		// Status dot -- the same 6x6 square dot used in the overlay's own
		// title bars (Widgets::ReadoutStrip's bLeadingDot), so "the product
		// is alive" reads consistently everywhere it appears.
		const float flDotSize = 6.0f;
		const ImVec2 dotMin( cardMin.x + flPadX, cardMin.y + flPadTop + titleSize.y * 0.5f - flDotSize * 0.5f );
		pDrawList->AddRectFilled( dotMin, ImVec2( dotMin.x + flDotSize, dotMin.y + flDotSize ), palette::Accent( flAlpha ) );

		const ImVec2 titlePos( dotMin.x + flDotSize + 10.0f, cardMin.y + flPadTop );
		pDrawList->AddText( pTitleFont, flTitleSize, titlePos, palette::Text( 0.94f * flAlpha ), pszTitle );

		// Animated accent underline: grows in from the left during the
		// fade-in/hold, matching the design guide's accent-underline
		// treatment elsewhere (dock active top edge, focused-group left
		// edge) -- this is the one clearly "animated, not just faded" touch
		// beyond the slide, per the task's "animate it, don't just print
		// static text" instruction.
		const float flUnderlineGrowMs = k_uStartupAnnounceFadeInMs + 260.0f;
		const float flGrowT = std::clamp( float( uElapsedMs ) / flUnderlineGrowMs, 0.0f, 1.0f );
		const float flUnderlineWidth = ( flCardWidth - flPadX * 2.0f ) * flGrowT;
		const float flUnderlineY0 = cardMin.y + flUnderlineY;
		pDrawList->AddRectFilled(
			ImVec2( cardMin.x + flPadX, flUnderlineY0 ),
			ImVec2( cardMin.x + flPadX + flUnderlineWidth, flUnderlineY0 + 2.0f ),
			palette::Accent( 0.85f * flAlpha ) );

		// Hint line: "CTRL + SHIFT + O" in accent (matches the hotkey-glyph
		// treatment the dock uses for its own per-button hotkeys) followed
		// by the dim explainer in Meta.
		const ImVec2 hotkeyPos( cardMin.x + flPadX, cardMin.y + flHintY );
		pDrawList->AddText( pHintFont, flHintSize, hotkeyPos, palette::Accent( 0.9f * flAlpha ), pszHotkey );
		const ImVec2 hintPos( hotkeyPos.x + hotkeySize.x + 8.0f, cardMin.y + flHintY );
		pDrawList->AddText( pHintFont, flHintSize, hintPos, palette::Text( 0.55f * flAlpha ), pszHint );
	}

	// Records the ImGui draw into s_pOverlayTexture on the general queue and
	// submits it, signaling s_pTimelineSemaphore at the returned point on
	// success. Returns false (nothing submitted) on failure.
	static bool RenderAndSubmit()
	{
		// Guards freeing our own previous frame's command buffer. The compute
		// queue's *read* of the texture is handled separately, by the
		// s_pReadDoneSemaphore dependency added below -- GPU-side, so it does
		// not stall the CPU. Expected to return immediately.
		DrainPrevSubmission();

		if ( !g_device.vk.CmdBeginRendering || !g_device.vk.CmdEndRendering )
		{
			// Pre-existing gap flagged by the feasibility scout: gamescope's
			// physical device selection only requires Vulkan 1.2, but these
			// entry points are resolved assuming 1.3. Fail soft rather than
			// crash on a driver where that gap actually bites.
			s_OverlayLog.errorf( "vkCmdBeginRendering/vkCmdEndRendering not available on this device" );
			return false;
		}

		auto cmdBuffer = g_device.generalCommandBuffer();
		if ( !cmdBuffer )
			return false;

		// Issue #22: don't let this frame's LOAD_OP_CLEAR start until the
		// compute queue has finished sampling the texture for the last
		// composite that was actually submitted. GPU-side wait, so the CPU
		// does not stall here. See s_pReadDoneSemaphore's comment.
		if ( s_ulRegisteredReadDonePoint )
			cmdBuffer->AddDependency( s_pReadDoneSemaphore, s_ulRegisteredReadDonePoint );

		VkCommandBuffer rawCmdBuffer = cmdBuffer->rawBuffer();

		if ( s_bTextureNeedsInitialBarrier )
		{
			VkImageMemoryBarrier barrier = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = 0,
				.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = s_pOverlayTexture->vkImage(),
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1,
					.layerCount = 1,
				},
			};
			g_device.vk.CmdPipelineBarrier( rawCmdBuffer,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier );
			s_bTextureNeedsInitialBarrier = false;
		}

		// Rendering (not sampling) view: the UNORM interpretation of the
		// mutable-format image, so ImGui writes raw encoded bytes directly
		// like any ordinary UI framebuffer. bind_all_layers() later samples
		// this same texture through its sRGB-format view instead (because our
		// Layer_t below is tagged GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB), so
		// the compute composite decodes it to linear automatically -- the
		// same treatment any other sRGB client texture gets.
		VkRenderingAttachmentInfo colorAttachment = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = s_pOverlayTexture->srgbView(),
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } } },
		};

		VkRenderingInfo renderingInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = { { 0, 0 }, { s_uTextureWidth, s_uTextureHeight } },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &colorAttachment,
		};

		g_device.vk.CmdBeginRendering( rawCmdBuffer, &renderingInfo );
		ImGui_ImplVulkan_RenderDrawData( ImGui::GetDrawData(), rawCmdBuffer );
		g_device.vk.CmdEndRendering( rawCmdBuffer );

		const uint64_t ulSignalPoint = ++s_ulSignalCounter;
		cmdBuffer->AddSignal( s_pTimelineSemaphore, ulSignalPoint );

		g_device.submitInternal( cmdBuffer.get() );

		s_pPrevCmdBuffer = std::move( cmdBuffer );
		s_ulPrevSignalPoint = ulSignalPoint;
		s_bHasPrevSubmission = true;

		s_ulPendingWaitPoint = ulSignalPoint;
		s_bHasPendingWaitPoint = true;

		return true;
	}

	// Forward-declared here, defined in the M2 section at the end of this
	// file (after SettingsOverlay_WaitForRender) so the M1 render-shell code
	// above stays contiguous. Drains the producer-side input queue into
	// ImGuiIO -- see its own definition for the full comment.
	static void DrainInputQueue();

	// Caches the CTM blob background_darkening builds (see
	// SettingsOverlay_AddLayer() below) so a steady-state frame -- the
	// overwhelmingly common case once the fade-in finishes -- reuses the
	// same shared_ptr instead of asking the backend for a fresh blob every
	// single frame (a real drmModeCreatePropertyBlob() ioctl + a matching
	// free of the old one on the DRM backend; see BackendBlob's own "no-op
	// on non-DRM backends" comment in backend.h for why this is cheap
	// everywhere else but still worth not doing 60+ times a second on the
	// target this ships to). Only rebuilds when flStrength has visibly
	// moved since the last call -- e.g. during the fade in/out ramp -- via a
	// coarser-than-float-epsilon threshold, since nothing downstream can
	// tell a <1/512 change in dim strength apart anyway.
	static std::shared_ptr<gamescope::BackendBlob> GetDarkeningCtmBlob( float flStrength )
	{
		static std::shared_ptr<gamescope::BackendBlob> s_pBlob;
		static float s_flCachedStrength = -1.0f;

		if ( !s_pBlob || std::fabs( flStrength - s_flCachedStrength ) > ( 1.0f / 512.0f ) )
		{
			const float k = 1.0f - flStrength;
			s_pBlob = GetBackend()->CreateBackendBlob( glm::mat3x4
			{
				k, 0, 0, 0,
				0, k, 0, 0,
				0, 0, k, 0
			} );
			s_flCachedStrength = flStrength;
		}

		return s_pBlob;
	}

	void SettingsOverlay_AddLayer( FrameInfo_t *pFrameInfo )
	{
		UpdateFadeAlpha();
		EnsureBackgroundLiveThemeLoaded();

		// Save/restore the globally-current ImGui context across this entire
		// pass (see s_pImguiContext). Covers DrainInputQueue() below too, which
		// writes ImGuiIO and so must run against OUR context, not whichever one
		// the caller happened to leave current.
		ScopedImguiContext imguiScope( s_pImguiContext );

		// M2: drain queued input even while fully hidden (alpha == 0), not
		// just while visible/fading -- see DrainInputQueue()'s own comment
		// for why a key/button release delivered after the overlay has
		// already faded all the way out would otherwise sit in the queue
		// forever, leaking a stuck "held" state into ImGui next time the
		// overlay reopens. Only runs once the context exists; before that
		// the queue can only be empty, since wlserver only ever queues
		// while g_bSettingsOverlayCapturing is true, which requires having
		// opened the overlay (and therefore initialized ImGui) at least once.
		if ( s_bImguiInitialized )
			DrainInputQueue();

		// Startup announcement: timed independently of the settings
		// overlay's own visibility (cv_settings_overlay_visible /
		// s_flCurrentAlpha) -- it must play even if the user never opens the
		// overlay this session.
		EnsureStartupAnnounceConfigLoaded();

		// Issue #30: warm the one-time ImGui/Vulkan setup (context creation,
		// font atlas build, ImGui_ImplVulkan_Init()'s descriptor pool/pipeline
		// compilation, and the first offscreen texture allocation) BEFORE
		// arming the announcement's fade-in timer, not on the same call that
		// first draws it. Previously EnsureImguiInit()/EnsureTexture() only
		// ran once bDrawPanels || flStartupAlpha > 0.0f was already true, and
		// the timer was armed unconditionally the very first time this
		// function ran -- so the entire one-time setup cost landed on the
		// exact frame the toast started fading in (frame 1), producing the
		// visible hitch right as it first became visible. Doing the warm-up
		// here instead, gated only on the output size being known (not on
		// whether anything will actually be drawn this frame), moves that
		// cost to an earlier call where nothing is drawn yet -- flStartupAlpha
		// is still 0.0f until the timer is armed below, so the early return
		// a few lines down still fires on that call, same as it always did
		// before the overlay was ever touched. The timer -- and the toast's
		// first visible pixel -- only start once EnsureImguiInit()/
		// EnsureTexture() are confirmed to have actually completed, so the
		// fade-in the user sees always begins on an already-warm context.
		//
		// Measured (see #30's PR/commit): before this change, the call that
		// first logged the one-time setup finishing and the call that first
		// drew the toast at nonzero alpha were the same call, every run.
		// After this change, the setup finishes one call earlier than the
		// first visible alpha>0 draw, every run -- the two no longer land on
		// the same frame.
		if ( s_bStartupAnnounceEnabled && !s_bStartupAnnounceStarted &&
			g_nOutputWidth != 0 && g_nOutputHeight != 0 )
		{
			EnsureImguiInit();
			if ( s_bImguiInitialized && EnsureTexture( g_nOutputWidth, g_nOutputHeight ) )
			{
				s_bStartupAnnounceStarted = true;
				s_uStartupAnnounceStartMs = get_time_in_milliseconds();
			}
		}
		const unsigned int uStartupElapsedMs = s_bStartupAnnounceStarted
			? ( get_time_in_milliseconds() - s_uStartupAnnounceStartMs )
			: 0;
		const bool bStartupAnnounceActive = s_bStartupAnnounceStarted && uStartupElapsedMs < k_uStartupAnnounceTotalMs;
		const float flStartupAlpha = bStartupAnnounceActive ? StartupAnnounceAlpha( uStartupElapsedMs ) : 0.0f;

		const bool bDrawPanels = s_flCurrentAlpha > 0.0f;
		if ( !bDrawPanels && flStartupAlpha <= 0.0f )
			return;

		if ( g_nOutputWidth == 0 || g_nOutputHeight == 0 )
			return;

		EnsureImguiInit();
		if ( !s_bImguiInitialized )
			return;

		if ( !EnsureTexture( g_nOutputWidth, g_nOutputHeight ) )
			return;

		// Issue #51: apply any font-atlas rebuild PanelConfig.cpp's General
		// tab queued (RebuildAll(), on the Display-scale slider's release)
		// before this frame draws anything -- ScopedImguiContext above
		// already made our context current, so this only ever touches our
		// own pending rebuild, if any (a no-op every other frame). Doing
		// this here, rather than letting RebuildAll() rebuild synchronously
		// from wherever PanelConfig.cpp calls it (mid-frame, from inside
		// this same context's own widget-drawing pass, since that call
		// happens further down this very call stack), is the fix: see
		// Fonts.cpp's RebuildAll() for why a same-frame rebuild corrupts
		// whatever this frame already drew before reaching that call.
		// A rebuild requested from OFF the render thread (a registration
		// setter reached through overlay_e2_set runs on the console thread)
		// is performed HERE, not where it was requested -- see Fonts.h's
		// RequestRebuild() for why doing it there aborts the compositor.
		// Pumped immediately before ApplyPendingRebuild() so the rebuild it
		// schedules for this context lands on this same frame.
		gamescope::fonts::PumpRequestedRebuild();
		gamescope::fonts::ApplyPendingRebuild();

		const uint64_t ulNowNanos = get_time_in_nanos();
		float flDeltaTime = s_ulLastFrameTimeNanos == 0
			? ( 1.0f / 60.0f )
			: float( ulNowNanos - s_ulLastFrameTimeNanos ) / 1e9f;
		s_ulLastFrameTimeNanos = ulNowNanos;
		flDeltaTime = std::clamp( flDeltaTime, 1.0f / 1000.0f, 1.0f );

		ImGuiIO &io = ImGui::GetIO();
		io.DisplaySize = ImVec2( (float)s_uTextureWidth, (float)s_uTextureHeight );
		io.DeltaTime = flDeltaTime;

		// Keyboard-control toggle (see ConfigSchema.h's OverlaySettings
		// comment): purely an ImGui-side flag, re-applied every frame this
		// context draws so a runtime console/gamescopectl change takes
		// effect immediately -- never touches wlserver.cpp's own capture
		// routing (SettingsOverlay_IsCapturingKeyboard()), so this can never
		// regress M2's release-safety guarantees.
		if ( cv_settings_overlay_keyboard_nav.Get() )
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		else
			io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		if ( bDrawPanels )
		{
			// ------------------------------------------------------------
			// THE E2 GATE (redesign phase 2). One branch, one frame, no
			// mixture.
			// ------------------------------------------------------------
			// `overlay_e2` is false by default, so the else-branch below is
			// byte-for-byte the behaviour every existing user has: the same
			// five floating panels and the same dock, drawn in the same
			// order.
			//
			// When it is true, the E2 shell replaces ALL of it -- including
			// the dock, the window frames and the whole floating-window
			// layer. Deliberately an either/or rather than a shell that
			// hosts the old windows: running both would mean two competing
			// ideas of where a panel lives on one screen, which is the
			// state that made the old layer expensive in the first place.
			//
			// The two paths share no ImGui window ids (the shell's are all
			// "##e2*"), so flipping the ConVar mid-session leaves nothing
			// behind for the other path to trip over.
			//
			// The FPS HUD over the game is drawn by FpsDisplay.cpp's own
			// context and is NOT in this branch at all -- neither path
			// touches it. Only the HUD's settings half moves, and in the
			// E2 path it is hosted as the `system.monitor` area.
			if ( gamescope::ui::shell::Enabled() )
			{
				gamescope::ui::shell::Draw();
			}
			else
			{
				DrawFpsHudPanel();
				PanelDisplay_Draw(); // M3: Display panel, see Overlay/PanelDisplay.cpp
				PanelShaders_Draw(); // M6: Shaders panel, see Overlay/PanelShaders.cpp
				PanelAudio_Draw(); // M5: Audio panel, see Overlay/PanelAudio.cpp
				PanelConfig_Draw(); // M7: Config panel, see Overlay/PanelConfig.cpp
				PanelLog_Draw(); // issue #39: Log panel, see Overlay/PanelLog.cpp
				// M8 part 3 (issue #15): drawn last so the dock's own window lands on
				// top of the panel windows above in ImGui's per-frame Begin-order Z
				// stack -- see Overlay/Chrome.h's DrawDock() comment.
				chrome::DrawDock();
			}
		}
		if ( flStartupAlpha > 0.0f )
			DrawStartupAnnounce( flStartupAlpha, uStartupElapsedMs );
		ImGui::Render();

		if ( !RenderAndSubmit() )
			return;

		FrameInfo_t::Layer_t *layer = pFrameInfo->layers.push();
		if ( !layer )
			return; // out of layer slots this frame -- see SettingsOverlay.h

		layer->tex = s_pOverlayTexture;
		layer->zpos = g_zposSettingsOverlay;
		layer->offset = { 0.0f, 0.0f };
		layer->scale = { 1.0f, 1.0f };
		// The settings-panel layer's own fade always drives this when the
		// panels are actually drawing this frame; when only the startup
		// toast is (the overwhelmingly common case -- the panels are closed
		// by default), the toast's own baked-in-per-pixel alpha
		// (DrawStartupAnnounce()'s flAlpha parameter) is the only fade that
		// matters, so the layer itself can stay fully opaque. The one edge
		// case this doesn't perfectly decouple -- the user toggles the
		// overlay open in the ~2.6s the toast is still playing -- leaves the
		// toast very slightly extra-dimmed by s_flCurrentAlpha's own fade-in
		// for a moment; not worth a second Vulkan layer to close.
		layer->opacity = bDrawPanels ? s_flCurrentAlpha : 1.0f;
		layer->filter = GamescopeUpscaleFilter::LINEAR;
		layer->blackBorder = false;
		layer->applyColorMgmt = false; // drm-plane-only; not exercised by the SDL/vkcube M1 test path
		// Issue #32: this was ALPHA_BLENDING_MODE_COVERAGE on the theory that
		// ImGui's blend state produces straight (non-premultiplied) alpha --
		// true of the *inputs* to each draw call, but not of the *offscreen
		// target's* accumulated pixels, which is what actually gets sampled
		// here. ImGui's Vulkan backend blends every draw with
		// SrcAlpha/OneMinusSrcAlpha for color, standard "over"; cleared to
		// transparent black (0,0,0,0) above (RenderAndSubmit()'s clearValue)
		// each frame. "Over" onto a transparent destination always yields a
		// PREMULTIPLIED result (out.rgb = src.rgb * src.a) wherever the
		// destination stayed low-alpha -- i.e. exactly the open-background
		// case. Where the destination was already near-opaque (another
		// window's own fill, drawn earlier in the same offscreen pass), the
		// accumulated alpha is ~1, so premultiplied and straight read the
		// same and the bug is invisible -- exactly issue #32's reported
		// "shows over other windows, vanishes over the game" pattern.
		// COVERAGE's shader path (BlendLayer(), alphamode.h) multiplies this
		// already-premultiplied layerColor by layerAlpha a *second* time,
		// squaring alpha for every translucent-over-background pixel -- the
		// focus glow's faint outer rings (well under 10% alpha) get squared
		// toward zero, reading as invisible. PREMULTIPLIED's shader path
		// takes layerColor as-is (correct for an already-premultiplied
		// source) and only applies opacity, matching what this offscreen
		// texture actually contains. Confirmed by tracing the accumulation,
		// not by tuning kGlowPeakA -- cranking that up would have "fixed"
		// the symptom while leaving the real double-multiply in place.
		layer->eAlphaBlendingMode = ALPHA_BLENDING_MODE_PREMULTIPLIED;
		layer->ctm = nullptr;
		layer->hdr_metadata_blob = nullptr;
		layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

		// Request the compositor's existing blur-behind pass (see
		// k_nMaxOverlayBlurRadius's comment above for why this reuses Steam's
		// own blurLayer0 primitive instead of a new one). The radius is
		// General tab's background_blur (0..1, g_BackgroundLiveTheme.flBlur)
		// linearly mapped onto 0..k_nMaxOverlayBlurRadius -- flBlur=0 means
		// no blur pass is requested at all (nOverlayBlurRadius stays 0 below,
		// so blurLayer0/blurRadius/useFSRLayer0/useNISLayer0 are left
		// untouched), not a minimum blur. On top of that, ramp by the same
		// s_flCurrentAlpha the panel layer's own opacity just used above, so
		// the blur fades in/out in lockstep with the panels instead of
		// snapping to full strength the instant the fade starts -- otherwise
		// a hard-edged full blur would "pop" in while the glass panels are
		// still fading up.
		//
		// vulkan_composite() picks exactly one of {FSR, NIS, blur, plain
		// blit} per call (rendervulkan.cpp) -- if the base layer's own
		// upscale filter already requested FSR/NIS this frame, blurLayer0
		// here would silently lose that if/else and never run. Clear both so
		// our request actually takes effect; this matches the precedent
		// steamcompmgr.cpp's own g_BlurMode path already sets when *it*
		// requests blurLayer0 (falling back to plain bilinear/nearest on the
		// now-blurred base layer -- upscale sharpness doesn't matter once
		// it's blurred anyway).
		const int nOverlayBlurRadius = std::clamp(
			(int)std::lround( k_nMaxOverlayBlurRadius * g_BackgroundLiveTheme.flBlur * s_flCurrentAlpha ),
			0, k_nMaxOverlayBlurRadius );

		if ( nOverlayBlurRadius > 0 )
		{
			pFrameInfo->blurLayer0 = std::max( pFrameInfo->blurLayer0, BLUR_MODE_ALWAYS );
			pFrameInfo->blurRadius = std::max( pFrameInfo->blurRadius, nOverlayBlurRadius );
			pFrameInfo->useFSRLayer0 = false;
			pFrameInfo->useNISLayer0 = false;
		}

		// background_darkening (General tab, g_BackgroundLiveTheme.flDarkening):
		// a *native-compositor* dim -- multiplies the base game layer's
		// (layers[0]) own pixels directly in the composite shader, via
		// FrameInfo_t::Layer_t::ctm, the exact primitive steamcompmgr.cpp
		// already uses for e.g. its 709->2020 and mura-correction color
		// matrices (composite.h: `color.rgb = vec4(color.rgb,1) * u_ctm[i]`).
		// The former opacity_background ImGui-drawn flat veil control was
		// removed (ConfigSchema.h's comment) as redundant now that this is a
		// real, working dim -- see blur.h's gaussian_blur() comment for why
		// it didn't used to work at all when background_blur was also on.
		//
		// Same s_flCurrentAlpha ramp as the blur above, so darkening also
		// fades in/out with the overlay rather than snapping.
		//
		// ponytail: steamcompmgr.cpp's own ctm assignments are always a
		// direct overwrite (nullptr or exactly one blob), never composed
		// with another matrix already on the layer -- so this follows that
		// same precedent instead of inventing matrix composition nothing
		// else in the codebase does. If layers[0] already carries a
		// meaningful ctm (HDR wide-gamut conversion, mura correction), this
		// intentionally backs off rather than risk silently corrupting that
		// color-managed pipeline for a cosmetic dim -- darkening simply has
		// no visible effect in that (uncommon, non-SDR-desktop) case.
		const float flDarkenStrength = std::clamp(
			g_BackgroundLiveTheme.flDarkening * s_flCurrentAlpha, 0.0f, 1.0f );

		if ( flDarkenStrength > 0.0f && pFrameInfo->layers.count() > 0 )
		{
			FrameInfo_t::Layer_t &baseLayer = pFrameInfo->layers.get( 0 );
			if ( !baseLayer.ctm )
				baseLayer.ctm = GetDarkeningCtmBlob( flDarkenStrength );
		}
	}

	void SettingsOverlay_WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer )
	{
		if ( !s_bHasPendingWaitPoint )
			return;

		pComputeCmdBuffer->AddDependency( s_pTimelineSemaphore, s_ulPendingWaitPoint );

		// Issue #22 return half: have this compute submission tell us when it
		// is done reading the texture, so the next RenderAndSubmit() can wait
		// for it. Signalled unconditionally for any composite that reached
		// here, even one whose FrameInfo_t happens not to carry our layer
		// (the commit_t::ShouldPreemptivelyUpscale() composite, for one) --
		// over-synchronising by one submission is free, and it keeps this
		// independent of which layers ended up in the frame.
		s_ulPendingReadDonePoint = ++s_ulReadDoneCounter;
		pComputeCmdBuffer->AddSignal( s_pReadDoneSemaphore, s_ulPendingReadDonePoint );
	}

	// Called by vulkan_composite() immediately after the compute submission is
	// handed to the queue. Only now is the pending read-done point guaranteed
	// to be reachable, so only now may a later general-queue submission wait
	// on it. See s_pReadDoneSemaphore's comment for why this ordering is what
	// makes the handshake deadlock-free.
	void SettingsOverlay_CommitReads()
	{
		if ( !s_ulPendingReadDonePoint )
			return;

		s_ulRegisteredReadDonePoint = s_ulPendingReadDonePoint;
		s_ulPendingReadDonePoint = 0;
	}

	// ======================================================================
	// M2: input capture and release.
	//
	// wlserver's input handlers (wlserver_key/wlserver_handle_key,
	// wlserver_mousemotion/wlserver_touchmotion/wlserver_mousebutton/
	// wlserver_mousewheel and their DRM-raw-listener equivalents) run on the
	// MAIN thread (wlserver_run()'s event loop). ImGuiIO is only ever
	// touched from the STEAMCOMPMGR thread, inside SettingsOverlay_AddLayer()
	// -- ImGui's own context is not thread-safe, and this repo's own
	// threading model (see architecture/overview.md) already keeps the
	// render loop single-threaded for the same class of reason the M1
	// header comment gives for drawing ImGui on steamcompmgr in the first
	// place. So the producer (wlserver, any event) never calls into ImGui
	// directly -- it only appends a small POD event to s_InputQueue under
	// s_InputQueueLock; DrainInputQueue() (steamcompmgr thread, called from
	// SettingsOverlay_AddLayer() every paint_all()) is the only consumer and
	// the only code that calls ImGuiIO's Add*Event() functions. This is the
	// exact same producer/queue-drains-under-lock shape
	// gamescope_xwayland_server_t::retrieve_commits() already uses for its
	// own cross-thread handoff (wlserver.cpp) -- reused here rather than
	// inventing a second cross-thread pattern.
	//
	// Release safety (see SPEC.md's "Release must be airtight" risk): a key
	// or mouse button whose PRESS was routed to the game must have its
	// RELEASE routed to the game too, even if the overlay toggles open in
	// between (and symmetrically for a press routed to the overlay). This is
	// NOT decided here by "is the overlay open right now" -- it's decided by
	// wlserver.cpp's own s_setKeysForwardedToGame/Overlay (and the mouse-
	// button equivalent) tracking sets, keyed by which side actually
	// received the press. See the comment on wlserver_route_key_to_overlay()
	// in wlserver.cpp for the full reasoning. On this side, DrainInputQueue()
	// adds a second, independent backstop: the moment it observes the
	// overlay transition from capturing to not-capturing, it calls
	// io.ClearInputKeys()/io.ClearInputMouse() so ImGui can never retain a
	// stuck "held" widget-interaction key/button/drag state (e.g. a slider
	// mid-drag) across the next time the overlay reopens, regardless of
	// whether every individual release made it through the queue in time.
	// ======================================================================

	namespace
	{
		struct QueuedInputEvent
		{
			enum class Kind : uint8_t
			{
				Key,
				MouseMotionDelta,
				MouseMotionAbsolute,
				MouseButton,
				MouseWheel,
			};

			Kind kind;
			uint32_t uCode = 0;    // linux keycode (Key) or button code (MouseButton)
			bool bPressed = false; // Key, MouseButton
			double x = 0.0;        // dx (MouseMotionDelta) / normalized X (MouseMotionAbsolute) / wheel X
			double y = 0.0;        // dy (MouseMotionDelta) / normalized Y (MouseMotionAbsolute) / wheel Y
			std::string sUtf8Text; // Key press only -- layout-correct text already resolved
			                       // against the real xkb_state in wlserver.cpp; see
			                       // SettingsOverlay_QueueKeyEvent()'s comment in the header.
			// Real wall-clock arrival time (get_time_in_nanos(), same clock
			// flDeltaTime is built from below), stamped at QueueEvent() time --
			// i.e. as close to the real libinput/wlserver event as this
			// cross-thread handoff gets. See DrainInputQueue()'s file comment
			// on why this is needed: without it, several events queued between
			// two frames (e.g. a fast double-click's down/up/down/up at a low
			// framerate) all get folded into one drain and lose any notion of
			// how far apart they really happened, making click timing track
			// frame duration instead of wall-clock time.
			uint64_t ulTimestampNanos = 0;
		};
	}

	static std::mutex s_InputQueueLock;
	static std::vector<QueuedInputEvent> s_InputQueue;

	// ---- Producer side (main thread) --------------------------------------

	static void QueueEvent( QueuedInputEvent ev )
	{
		ev.ulTimestampNanos = get_time_in_nanos();
		std::lock_guard<std::mutex> lock( s_InputQueueLock );
		s_InputQueue.push_back( std::move( ev ) );
	}

	void SettingsOverlay_QueueKeyEvent( uint32_t uLinuxKeycode, bool bPressed, std::string sUtf8Text )
	{
		QueueEvent( { .kind = QueuedInputEvent::Kind::Key, .uCode = uLinuxKeycode, .bPressed = bPressed, .sUtf8Text = std::move( sUtf8Text ) } );
	}

	void SettingsOverlay_QueueMouseMotionDelta( double dx, double dy )
	{
		QueueEvent( { .kind = QueuedInputEvent::Kind::MouseMotionDelta, .x = dx, .y = dy } );
	}

	void SettingsOverlay_QueueMouseMotionAbsolute( double flNormalizedX, double flNormalizedY )
	{
		QueueEvent( { .kind = QueuedInputEvent::Kind::MouseMotionAbsolute, .x = flNormalizedX, .y = flNormalizedY } );
	}

	void SettingsOverlay_QueueMouseButton( uint32_t uLinuxButton, bool bPressed )
	{
		QueueEvent( { .kind = QueuedInputEvent::Kind::MouseButton, .uCode = uLinuxButton, .bPressed = bPressed } );
	}

	void SettingsOverlay_QueueMouseWheel( double flX, double flY )
	{
		QueueEvent( { .kind = QueuedInputEvent::Kind::MouseWheel, .x = flX, .y = flY } );
	}

	// ---- Consumer side (steamcompmgr thread, inside DrainInputQueue()) ----

	// Overlay-local cursor position, in overlay-texture pixels. Kept here
	// (not read back from ImGuiIO) so relative-motion deltas (from
	// wlserver_mousemotion(), the grabbed/relative-pointer path) have
	// something to accumulate onto between drains.
	static double s_flCursorX = 0.0;
	static double s_flCursorY = 0.0;
	static bool s_bCursorInitialized = false;

	// Physical modifier keys currently held, tracked independently of xkb:
	// the virtual keyboard device most backends feed through (everything
	// except the DRM/libinput keyboard group) never receives
	// wlr_keyboard_notify_key()/notify_modifiers() calls of its own (only
	// wlr_seat_keyboard_notify_key(), which updates the *seat's* state, not
	// the device's xkb_state) -- see wlserver_key() in wlserver.cpp. So
	// keyboard->xkb_state's modifier-depressed bits can't be trusted equally
	// across all backends. Tracking the handful of modifier keycodes
	// ourselves, evdev-code-in/ImGui-flag-out, sidesteps that entirely and
	// is correct on every backend by construction.
	static int s_nCtrlHeld = 0;
	static int s_nShiftHeld = 0;
	static int s_nAltHeld = 0;
	static int s_nSuperHeld = 0;

	static bool s_bWasCapturingLastDrain = false;
	// Keyboard-only capture-toggle release-safety backstop -- see
	// DrainInputQueue()'s own comment on the two independent `if`s.
	static bool s_bWasCapturingKeyboardLastDrain = false;

	static ImGuiKey ImGuiKeyForKeycode( uint32_t uLinuxKeycode )
	{
		switch ( uLinuxKeycode )
		{
			case KEY_LEFTCTRL:   return ImGuiKey_LeftCtrl;
			case KEY_RIGHTCTRL:  return ImGuiKey_RightCtrl;
			case KEY_LEFTSHIFT:  return ImGuiKey_LeftShift;
			case KEY_RIGHTSHIFT: return ImGuiKey_RightShift;
			case KEY_LEFTALT:    return ImGuiKey_LeftAlt;
			case KEY_RIGHTALT:   return ImGuiKey_RightAlt;
			case KEY_LEFTMETA:   return ImGuiKey_LeftSuper;
			case KEY_RIGHTMETA:  return ImGuiKey_RightSuper;

			case KEY_TAB:        return ImGuiKey_Tab;
			case KEY_LEFT:       return ImGuiKey_LeftArrow;
			case KEY_RIGHT:      return ImGuiKey_RightArrow;
			case KEY_UP:         return ImGuiKey_UpArrow;
			case KEY_DOWN:       return ImGuiKey_DownArrow;
			case KEY_PAGEUP:     return ImGuiKey_PageUp;
			case KEY_PAGEDOWN:   return ImGuiKey_PageDown;
			case KEY_HOME:       return ImGuiKey_Home;
			case KEY_END:        return ImGuiKey_End;
			case KEY_INSERT:     return ImGuiKey_Insert;
			case KEY_DELETE:     return ImGuiKey_Delete;
			case KEY_BACKSPACE:  return ImGuiKey_Backspace;
			case KEY_SPACE:      return ImGuiKey_Space;
			case KEY_ENTER:      return ImGuiKey_Enter;
			case KEY_KPENTER:    return ImGuiKey_KeypadEnter;
			case KEY_ESC:        return ImGuiKey_Escape;

			case KEY_0: return ImGuiKey_0;
			case KEY_1: return ImGuiKey_1;
			case KEY_2: return ImGuiKey_2;
			case KEY_3: return ImGuiKey_3;
			case KEY_4: return ImGuiKey_4;
			case KEY_5: return ImGuiKey_5;
			case KEY_6: return ImGuiKey_6;
			case KEY_7: return ImGuiKey_7;
			case KEY_8: return ImGuiKey_8;
			case KEY_9: return ImGuiKey_9;

			case KEY_A: return ImGuiKey_A;
			case KEY_B: return ImGuiKey_B;
			case KEY_C: return ImGuiKey_C;
			case KEY_D: return ImGuiKey_D;
			case KEY_E: return ImGuiKey_E;
			case KEY_F: return ImGuiKey_F;
			case KEY_G: return ImGuiKey_G;
			case KEY_H: return ImGuiKey_H;
			case KEY_I: return ImGuiKey_I;
			case KEY_J: return ImGuiKey_J;
			case KEY_K: return ImGuiKey_K;
			case KEY_L: return ImGuiKey_L;
			case KEY_M: return ImGuiKey_M;
			case KEY_N: return ImGuiKey_N;
			case KEY_O: return ImGuiKey_O;
			case KEY_P: return ImGuiKey_P;
			case KEY_Q: return ImGuiKey_Q;
			case KEY_R: return ImGuiKey_R;
			case KEY_S: return ImGuiKey_S;
			case KEY_T: return ImGuiKey_T;
			case KEY_U: return ImGuiKey_U;
			case KEY_V: return ImGuiKey_V;
			case KEY_W: return ImGuiKey_W;
			case KEY_X: return ImGuiKey_X;
			case KEY_Y: return ImGuiKey_Y;
			case KEY_Z: return ImGuiKey_Z;

			case KEY_F1:  return ImGuiKey_F1;
			case KEY_F2:  return ImGuiKey_F2;
			case KEY_F3:  return ImGuiKey_F3;
			case KEY_F4:  return ImGuiKey_F4;
			case KEY_F5:  return ImGuiKey_F5;
			case KEY_F6:  return ImGuiKey_F6;
			case KEY_F7:  return ImGuiKey_F7;
			case KEY_F8:  return ImGuiKey_F8;
			case KEY_F9:  return ImGuiKey_F9;
			case KEY_F10: return ImGuiKey_F10;
			case KEY_F11: return ImGuiKey_F11;
			case KEY_F12: return ImGuiKey_F12;

			default: return ImGuiKey_None;
		}
	}

	static int ImGuiMouseButtonForLinuxButton( uint32_t uLinuxButton )
	{
		switch ( uLinuxButton )
		{
			case BTN_LEFT:   return ImGuiMouseButton_Left;
			case BTN_RIGHT:  return ImGuiMouseButton_Right;
			case BTN_MIDDLE: return ImGuiMouseButton_Middle;
			default:         return -1;
		}
	}

	static void HandleKeyEvent( ImGuiIO &io, uint32_t uLinuxKeycode, bool bPressed, const std::string &sUtf8Text )
	{
		// Modifiers: feed the specific Left/Right key (so ImGui's own
		// per-key state is exact) plus the recomputed merged Mod flag (the
		// form widgets/shortcuts actually query) -- this is the standard
		// pattern ImGui's own platform backends use.
		switch ( uLinuxKeycode )
		{
			case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
				s_nCtrlHeld = std::max( 0, s_nCtrlHeld + ( bPressed ? 1 : -1 ) );
				io.AddKeyEvent( ImGuiKeyForKeycode( uLinuxKeycode ), bPressed );
				io.AddKeyEvent( ImGuiMod_Ctrl, s_nCtrlHeld > 0 );
				return;
			case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
				s_nShiftHeld = std::max( 0, s_nShiftHeld + ( bPressed ? 1 : -1 ) );
				io.AddKeyEvent( ImGuiKeyForKeycode( uLinuxKeycode ), bPressed );
				io.AddKeyEvent( ImGuiMod_Shift, s_nShiftHeld > 0 );
				return;
			case KEY_LEFTALT: case KEY_RIGHTALT:
				s_nAltHeld = std::max( 0, s_nAltHeld + ( bPressed ? 1 : -1 ) );
				io.AddKeyEvent( ImGuiKeyForKeycode( uLinuxKeycode ), bPressed );
				io.AddKeyEvent( ImGuiMod_Alt, s_nAltHeld > 0 );
				return;
			case KEY_LEFTMETA: case KEY_RIGHTMETA:
				s_nSuperHeld = std::max( 0, s_nSuperHeld + ( bPressed ? 1 : -1 ) );
				io.AddKeyEvent( ImGuiKeyForKeycode( uLinuxKeycode ), bPressed );
				io.AddKeyEvent( ImGuiMod_Super, s_nSuperHeld > 0 );
				return;
			default: break;
		}

		const ImGuiKey key = ImGuiKeyForKeycode( uLinuxKeycode );
		if ( key != ImGuiKey_None )
			io.AddKeyEvent( key, bPressed );

		// Text input: only on press, and only the already layout-translated
		// UTF-8 text wlserver_dispatch_key() resolved against the keyboard's
		// real xkb_state (see SettingsOverlay_QueueKeyEvent()'s comment in
		// the header) -- correct on any layout, and AddInputCharactersUTF8()
		// takes the whole multi-byte string in one call so umlauts and other
		// non-ASCII characters aren't truncated to a single byte.
		if ( bPressed && !sUtf8Text.empty() )
			io.AddInputCharactersUTF8( sUtf8Text.c_str() );
	}

	static void ClampCursorToTexture()
	{
		s_flCursorX = std::clamp( s_flCursorX, 0.0, double( s_uTextureWidth > 0 ? s_uTextureWidth - 1 : 0 ) );
		s_flCursorY = std::clamp( s_flCursorY, 0.0, double( s_uTextureHeight > 0 ? s_uTextureHeight - 1 : 0 ) );
	}

	// ---- Motion-event sanity checks (#65, #75, #76) -----------------------
	//
	// DrainInputQueue() used to accept every raw motion coordinate/delta from
	// wlserver with no check at all. Under rapid successive pointer events, a
	// spurious motion event reliably shows up carrying an out-of-range
	// coordinate -- confirmed live for two independent paths:
	//   - CWaylandInputThread::Wayland_Pointer_Motion() (WaylandBackend.cpp):
	//     normalized X == exactly -1.0, i.e. one full output-width off.
	//   - CWaylandInputThread::Wayland_RelativePointer_RelativeMotion():
	//     dx == dy == -8388608 (raw wl_fixed_t), i.e. exactly -32768.0 px
	//     after wl_fixed_to_double() -- suspiciously exactly INT16_MIN
	//     (-2^15) run through wl_fixed_from_int(), though nothing in this
	//     repo's own source constructs that literal (grepped for
	//     "8388608"/"INT16_MIN"/"32768": no hit outside FSR shader math) --
	//     so this value is not manufactured by gamescope-ritz's own code; it
	//     arrives already-formed over the wire (from the host compositor's
	//     relative-pointer implementation, or the kernel/libinput chain
	//     feeding it -- e.g. an uninitialised or sentinel int16 value from
	//     whatever synthesised the input) and gets passed straight through
	//     unchecked. Left unexplained upstream of this file; see #65's
	//     comment thread for the full investigation and its caveat about
	//     ydotool-driven repro not being confirmed to match a physical mouse.
	//
	// When one of these lands mid-press during a title-bar drag, ImGui's
	// focus-follows-active-item logic reassigns NavWindow to whatever panel
	// sits at the (clamped) garbage coordinate -- clamping the *final*
	// cursor position (ClampCursorToTexture(), above) does NOT fix this: a
	// clamped position is still a legitimate-looking, valid-in-bounds
	// coordinate that ImGui treats as real motion to a real place, just the
	// wrong one. The fix has to reject the bad sample before it ever
	// perturbs s_flCursorX/Y, not clean up the value afterwards.
	//
	// The actual predicates (SettingsOverlay_IsSaneNormalizedCoord() /
	// SettingsOverlay_IsSaneMotionDelta()) live in SettingsOverlay.h, not
	// here -- header-only and free of ImGui/Vulkan so
	// tests/test_settings_overlay_input.cpp can exercise the exact boundary
	// values without linking the whole overlay renderer.

	// Rate-limited so a host that spams the bad sample can't flood the log.
	static void LogRejectedMotion( const char *pszKind, double x, double y )
	{
		static uint64_t s_ulLastLogNanos = 0;
		const uint64_t ulNow = get_time_in_nanos();
		if ( ulNow - s_ulLastLogNanos > 1'000'000'000ull )
		{
			s_OverlayLog.warnf( "rejected out-of-range %s pointer event (%.3f, %.3f) -- see issue #65", pszKind, x, y );
			s_ulLastLogNanos = ulNow;
		}
	}

	static void DrainInputQueue()
	{
		// Note: still proceeds to the capturing-edge check below even when
		// nothing was queued this drain -- a toggle-off with no further
		// input at all must still clear ImGui's held state.
		std::vector<QueuedInputEvent> events;
		{
			std::lock_guard<std::mutex> lock( s_InputQueueLock );
			events.swap( s_InputQueue );
		}

		ImGuiIO &io = ImGui::GetIO();

		if ( !s_bCursorInitialized && s_uTextureWidth > 0 && s_uTextureHeight > 0 )
		{
			s_flCursorX = s_uTextureWidth * 0.5;
			s_flCursorY = s_uTextureHeight * 0.5;
			s_bCursorInitialized = true;
		}

		bool bCursorMoved = false;

		// Frame-rate-independent double-click fix -- see the QueuedInputEvent::
		// ulTimestampNanos comment for the "why" and superdoc/planning/
		// overlay-presentation-architecture.md for the investigation.
		//
		// Recap: when this drain collects more than one MouseButton event (a
		// low framerate, or any frame that simply lags a beat, lets the
		// producer queue several real events before the next drain), handing
		// them all to ImGui via one shared NewFrame() call is not neutral --
		// ImGui's own input-event trickling (io.ConfigInputTrickleEventQueue,
		// on by default) applies only ONE same-button transition per
		// NewFrame() call and defers the rest to subsequent calls, so a fast
		// double-click's down/up/down/up ends up spread across several
		// *frames*, each one only becoming a "click" (and getting stamped
		// with ImGui's internal clock, g.Time) once its own later NewFrame()
		// runs. g.Time always advances by the real DeltaTime of whichever
		// NewFrame() call is doing the advancing, so the delay this
		// introduces between the two clicks' registered times is N real
		// frame periods, not the real N milliseconds the clicks were
		// actually apart -- at low framerate that easily blows past
		// io.MouseDoubleClickTime's default 0.3s, breaking a double-click
		// that a wall clock would call well within the window. (Confirmed by
		// instrumentation before this fix: a realistic 150ms double-click
		// registers correctly for framerates down to ~8fps and silently
		// stops registering below that, purely from frame-period growth --
		// exactly the reported "double-click window depends on framerate"
		// symptom, with no change to real click timing at all.)
		//
		// Simply turning trickling off is NOT the fix (tried and measured):
		// without it, ImGui applies every event in the batch but keeps only
		// the NET button-state change for the frame, so a whole press+release
		// landing in one drain (routine at low fps, since a real click's own
		// down-to-up gap is often under one frame period) collapses to no
		// click at all -- worse than the bug it was meant to cure.
		//
		// Fix: give each MouseButton event in this batch, other than the
		// batch's last event, its own correctly-timed "micro" NewFrame()/
		// EndFrame() cycle (no windows opened, nothing rendered) with
		// DeltaTime set to the REAL gap since the last time ImGui's clock was
		// advanced -- so g.Time tracks real elapsed wall-clock time between
		// transitions instead of render-frame count, while every other event
		// kind (motion/wheel/key) is applied without a pump of its own,
		// exactly as before. The batch's last event is left pending for the
		// caller's own real, visible NewFrame() (SettingsOverlay_AddLayer(),
		// right after this function returns), so the real render cadence and
		// its DeltaTime are untouched -- this only tightens the timing of
		// transitions that would otherwise be trickle-delayed within the
		// *same* drain.
		for ( size_t i = 0; i < events.size(); i++ )
		{
			const QueuedInputEvent &ev = events[i];
			switch ( ev.kind )
			{
				case QueuedInputEvent::Kind::Key:
					HandleKeyEvent( io, ev.uCode, ev.bPressed, ev.sUtf8Text );
					break;

				case QueuedInputEvent::Kind::MouseMotionDelta:
					if ( SettingsOverlay_IsSaneMotionDelta( ev.x, ev.y ) )
					{
						s_flCursorX += ev.x;
						s_flCursorY += ev.y;
						bCursorMoved = true;
					}
					else
					{
						LogRejectedMotion( "relative", ev.x, ev.y );
					}
					break;

				case QueuedInputEvent::Kind::MouseMotionAbsolute:
					if ( SettingsOverlay_IsSaneNormalizedCoord( ev.x ) && SettingsOverlay_IsSaneNormalizedCoord( ev.y ) )
					{
						s_flCursorX = ev.x * s_uTextureWidth;
						s_flCursorY = ev.y * s_uTextureHeight;
						bCursorMoved = true;
					}
					else
					{
						LogRejectedMotion( "absolute", ev.x, ev.y );
					}
					break;

				case QueuedInputEvent::Kind::MouseButton:
				{
					const int nButton = ImGuiMouseButtonForLinuxButton( ev.uCode );
					if ( nButton >= 0 )
					{
						io.AddMouseButtonEvent( nButton, ev.bPressed );

						const bool bLastEventInBatch = ( i + 1 == events.size() );
						if ( !bLastEventInBatch )
						{
							if ( bCursorMoved )
							{
								ClampCursorToTexture();
								io.AddMousePosEvent( (float)s_flCursorX, (float)s_flCursorY );
								bCursorMoved = false;
							}

							float flMicroDeltaTime = s_ulLastFrameTimeNanos == 0
								? ( 1.0f / 60.0f )
								: float( ev.ulTimestampNanos - s_ulLastFrameTimeNanos ) / 1e9f;
							flMicroDeltaTime = std::clamp( flMicroDeltaTime, 1.0f / 1000.0f, 1.0f );
							s_ulLastFrameTimeNanos = ev.ulTimestampNanos;

							io.DeltaTime = flMicroDeltaTime;
							ImGui::NewFrame();
							ImGui::EndFrame();
						}
					}
					break;
				}

				case QueuedInputEvent::Kind::MouseWheel:
					// Inverted on both axes: libinput's wheel sign is the
					// opposite of what feels right in the overlay.
					// ponytail: hardcoded rather than a setting -- make it
					// one if anyone actually wants the other direction.
					io.AddMouseWheelEvent( -(float)ev.x, -(float)ev.y );
					break;
			}
		}

		if ( bCursorMoved )
		{
			ClampCursorToTexture();
			io.AddMousePosEvent( (float)s_flCursorX, (float)s_flCursorY );
		}

		// Release-safety backstop (see the file-level M2 comment): the
		// instant capturing drops, forcibly clear anything ImGui still
		// thinks is held/dragging, regardless of whether every individual
		// release event made it through the queue by now.
		const bool bCapturingNow = SettingsOverlay_IsCapturingInput();
		const bool bCapturingKeyboardNow = SettingsOverlay_IsCapturingKeyboard();

		bool bClearedKeysAlready = false;
		if ( s_bWasCapturingLastDrain && !bCapturingNow )
		{
			io.ClearInputKeys();
			io.ClearInputMouse();
			s_nCtrlHeld = s_nShiftHeld = s_nAltHeld = s_nSuperHeld = 0;
			bClearedKeysAlready = true;
		}

		// Same backstop, narrower trigger: the keyboard-only capture toggle
		// (settings_overlay_capture_keyboard) can drop while the overlay
		// stays open and still capturing the mouse -- e.g. the user flips it
		// off mid-session with a key held for keyboard navigation. From that
		// instant, wlserver.cpp routes all further key events straight to
		// the game (see SettingsOverlay_IsCapturingKeyboard()), so no more
		// releases for anything already-held-in-ImGui will ever reach this
		// queue. Independent `if`, not `else if` -- both edges can fire the
		// same drain (overlay closing always implies this one too, per
		// SettingsOverlay_IsCapturingKeyboard()'s definition), and re-running
		// an already-empty ClearInputKeys() is harmless, whereas skipping it
		// on a s_bWasCapturingKeyboardLastDrain that's gone stale (see below)
		// would not be. Clears keys only, never ClearInputMouse() -- this
		// toggle never affects mouse capture, so a mouse drag still
		// legitimately owned by the overlay must survive it.
		if ( !bClearedKeysAlready && s_bWasCapturingKeyboardLastDrain && !bCapturingKeyboardNow )
		{
			io.ClearInputKeys();
			s_nCtrlHeld = s_nShiftHeld = s_nAltHeld = s_nSuperHeld = 0;
		}

		s_bWasCapturingLastDrain = bCapturingNow;
		s_bWasCapturingKeyboardLastDrain = bCapturingKeyboardNow;
	}
}
