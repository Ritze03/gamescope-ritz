// M4 FPS display -- see FpsDisplay.h and superdoc/planning/SPEC.md's
// "Per-feature sections -> 3. FPS display" for the design this follows.
//
// Render shape deliberately mirrors SettingsOverlay.cpp (own offscreen
// Vulkan texture on the general queue, own timeline semaphore for
// cross-queue sync with the compute-queue composite/screenshot paths, own
// Layer_t pushed into paint_all()'s frame) -- see that file's header
// comment for why that shape is correct here too. The one structural
// difference that matters: this owns a fully separate Dear ImGui *context*
// from SettingsOverlay's, not just separate textures/state. Two reasons:
//   1. Lifetime -- this must keep rendering every frame regardless of the
//      settings panel's own open/closed state (the subtlety this milestone
//      is explicitly about getting right), so it cannot share a NewFrame()/
//      Render() bracket gated on cv_settings_overlay_visible.
//   2. Isolation from concurrent work -- SettingsOverlay.cpp is being
//      actively extended by other in-flight milestones; not touching its
//      internals (and not being touched by them) is much safer with a
//      second, independent context than by threading a second draw pass
//      through its existing one.
// ImGui's "current context" is process-global, and SettingsOverlay.cpp's
// own EnsureImguiInit()/NewFrame()/Render() calls never explicitly save or
// restore it (reasonable in isolation -- M1 had no reason to expect a
// second context to ever exist). So FpsDisplay_AddLayer() is the side
// responsible for coexisting safely: it always explicitly
// ImGui::SetCurrentContext()s to its own context on entry and restores
// whatever was current beforehand on every exit path, so control always
// returns to paint_all() with the context unchanged from how it found it --
// correct regardless of whether SettingsOverlay's or this file's AddLayer
// runs first in a given frame. FpsDisplay_DrawSettingsPanel() is the
// opposite: it must *not* touch the current context at all, since it's
// meant to be called from inside SettingsOverlay's own NewFrame()/Render()
// bracket and draw its widgets into that context.

#include "FpsDisplay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "main.hpp"
#include "log.hpp"
#include "convar.h"
#include "Config/ConfigManager.h"
#include "Config/AppId.h"
#include "Fonts.h"
#include "Palette.h"
#include "UI/Registry.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

// Set in commit.cpp's commit_t::Signal(), the same computation that feeds
// mangoapp's app_frametime_ns field (DECISIONS.md #16/#17) -- read directly
// here rather than through mangoapp's own shared mangoapp_msg_v1 struct,
// which two independent writers race on (see commit.cpp's comment at the
// write site for why that struct isn't safe to read from here).
extern std::atomic<uint64_t> g_ulLastAppFrametimeNs;

namespace gamescope
{
	static LogScope s_FpsLog( "fps_display" );

	// -------------------------------------------------------------------
	// Config: loaded lazily once, cached locally, written back on edit.
	// -------------------------------------------------------------------

	static bool s_bConfigLoaded = false;
	static uint64_t s_ulLoadedGeneration = 0;
	static config::Settings s_Settings;

	// -------------------------------------------------------------------
	// Keeping the HUD's own readout fresh while the game client is idle.
	//
	// Same root cause and same remedy as SettingsOverlay.cpp's RequestRepaint()
	// (see that file's comment for the full "gamescope does not free-run"
	// background): paint_all() only runs when something marks the frame
	// dirty, and until this fix nothing in this file ever did, so
	// FpsDisplay_AddLayer() -- and everything above that only updates from
	// inside it -- stalled the instant the game client stopped producing
	// frames of its own.
	//
	// A single number still needs a periodic nudge: without one, a game that
	// stops committing frames (paused, alt-tabbed) leaves the last FPS value
	// frozen on screen instead of decaying/refreshing. 500ms matches this
	// file's own history of picking a cheap, unhurried cadence for exactly
	// this kind of keepalive.
	//
	// A FIRST ATTEMPT hung this off FpsDisplay_AddLayer itself -- call
	// force_repaint() from in there, gated to at most once per 500ms, the
	// same shape as a periodic interval check. Measured (headless, idle
	// client, HUD on): it produced exactly one extra frame and then went
	// silent forever. The reason is structural, not a bug in the gate:
	// force_repaint() only reaches the NEXT vblank -- steamcompmgr re-arms
	// its vblank timer on every tick regardless of demand
	// (steamcompmgr.cpp, ~16ms later at a 60Hz default), so the flag it sets
	// is consumed almost immediately, not held for 500ms. FpsDisplay_AddLayer
	// only runs from inside that one resulting paint, sees its own last
	// request was <500ms ago, correctly does NOT re-arm yet (that's the
	// no-busy-loop guarantee working as intended) -- and then nothing is
	// left to wake the loop up again at the 500ms mark, because the only
	// thing that ever calls this function is a paint that already happened.
	// A once-per-500ms gate evaluated only from inside the paint path can
	// throttle repaint REQUESTS; it cannot manufacture a request that fires
	// on a clock nothing is driving.
	//
	// So the actual 500ms clock has to live outside paint_all() entirely:
	// a small dedicated thread, sleeping in fixed steps and calling
	// force_repaint() only while the HUD is enabled. Started lazily, once,
	// from EnsureConfigLoaded() (see that function), which runs
	// unconditionally the first time ANYTHING touches this feature's
	// config -- a paint, opening the settings panel, the toggle command --
	// so it comes up even if a game client has never rendered a single
	// frame this session.
	//
	// s_bHudEnabledForTimer mirrors s_Settings.fps_display.enabled for this
	// thread to read without touching s_Settings itself (which, like the
	// rest of this file's config state, is only ever safely read/written
	// from the render-thread call sites) -- kept in sync at every place
	// fps_display.enabled is assigned (EnsureConfigLoaded's own reload,
	// cc_toggle_fps_display, and the `hud.enabled` registry switch).
	static std::atomic<bool> s_bHudEnabledForTimer{ false };

	static void EnsureRepaintTimerThread()
	{
		static std::atomic<bool> s_bStarted{ false };
		if ( s_bStarted.exchange( true ) )
			return;

		std::thread( []
		{
			for ( ;; )
			{
				std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
				if ( s_bHudEnabledForTimer.load( std::memory_order_relaxed ) )
					force_repaint();
			}
		} ).detach();
	}

	// M7: reloads whenever PanelConfig.cpp bumps config::ConfigGeneration()
	// (profile applied, override toggled, another game's config copied in),
	// not just on this file's very first draw -- s_Settings.fps_display is
	// read directly every frame in DrawReadout()/FpsDisplay_AddLayer(), so a
	// plain reload is all a mid-session change needs here (unlike
	// PanelDisplay.cpp/PanelShaders.cpp, nothing else caches a "live" copy
	// of these fields to push).
	static void EnsureConfigLoaded()
	{
		// See s_bHudEnabledForTimer's own comment: this is the one call site
		// guaranteed to run the first time anything touches this feature,
		// independent of whether a game client has ever painted a frame.
		EnsureRepaintTimerThread();

		const uint64_t ulGeneration = config::ConfigGeneration();
		if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
			return;
		s_Settings = config::ResolveEffective( config::SessionAppId() );
		s_ulLoadedGeneration = ulGeneration;
		s_bConfigLoaded = true;
		s_bHudEnabledForTimer.store( s_Settings.fps_display.enabled, std::memory_order_relaxed );
	}

	// M7: routes through config::IsSessionOverrideActive() instead of
	// always writing global.json -- superseded M4's original "always
	// global.json" simplification.
	static void PersistSettings()
	{
		config::EnqueueRoutedWrite( s_Settings );
	}

	// Console/gamescopectl affordance for testing without input capture
	// into the settings panel's own checkbox (that widget isn't clickable
	// until Milestone 2 lands) -- same shape as SettingsOverlay.cpp's
	// cc_toggle_settings_overlay.
	static ConCommand cc_toggle_fps_display(
		"toggle_fps_display", "Toggle the M4 FPS display readout on/off.",
		[]( std::span<std::string_view> args )
		{
			EnsureConfigLoaded();
			s_Settings.fps_display.enabled = !s_Settings.fps_display.enabled;
			PersistSettings();
			s_bHudEnabledForTimer.store( s_Settings.fps_display.enabled, std::memory_order_relaxed );

			// Same reasoning as SettingsOverlay.cpp's visibility ConVar
			// callback: the master toggle itself is a state change without a
			// game frame. The background repaint-timer thread (see
			// EnsureRepaintTimerThread) would eventually pick up the new
			// s_bHudEnabledForTimer value on its own within one 500ms tick,
			// but this makes the very first frame -- on OR off -- show up
			// immediately rather than after up to half a second of nothing.
			// force_repaint(), not hasRepaint, for the same reason as
			// everywhere else in this file: this runs outside paint_all()
			// entirely (a console/gamescopectl command callback), so only
			// g_bForceRepaint's top-of-loop consumption (and its nudge)
			// reliably reaches a possibly-idle main loop.
			force_repaint();
		} );

	// -------------------------------------------------------------------
	// Smoothing: a single-pole EMA over the raw per-commit frametime, for
	// the headline number (DECISIONS.md #16: the game's own frame rate).
	// -------------------------------------------------------------------

	static uint64_t s_ulLastRawFrametimeNs = 0;
	// Seeded at a plausible 60fps so the very first frames show a sane
	// number instead of 0/infinity before the first real sample arrives.
	static float s_flSmoothedFrametimeMs = 1000.0f / 60.0f;

	// -------------------------------------------------------------------
	// Lag-spike detection: a ring buffer of raw (unsmoothed) per-frame game
	// frametimes. Every entry is a real g_ulLastAppFrametimeNs sample
	// (commit_t::Signal()'s game-frame delta, DECISIONS.md #16/#17) -- never
	// the compositor's composite rate or the display's refresh rate, so it
	// stays on the same clock as the headline number.
	//
	// Scope reduction (2026-09-03): this used to also feed a frametime graph
	// and a percentile row (removed -- see superdoc/meta/TERMINOLOGY.md's
	// "profiler" entry). The raw history buffer itself is KEPT, deliberately
	// unused by anything in this file right now: Phase 2 (a separate task)
	// builds lag-spike detection on top of it, and this is the underlying
	// per-frame data that detection needs. Nothing currently reads
	// s_flFrametimeHistoryMs/s_nHistoryCount -- see PushFrametimeSample's own
	// comment for why it's still fed every frame regardless.
	//
	// Sized at 240 samples to match this repo's own spec text for this exact
	// feature (ui-mockup-precise-spec.md §11's FPS-config-window footer:
	// "sampling 500 ms · 240-frame window") rather than an invented number.
	static constexpr int kHistoryCapacity = 240;
	static float s_flFrametimeHistoryMs[kHistoryCapacity] = {};
	static int s_nHistoryCount = 0;  // valid samples so far (caps at kHistoryCapacity)
	static int s_nHistoryHead = 0;   // index the NEXT sample will be written to

	// Kept feeding the ring buffer even though nothing in this file reads it
	// right now (see the buffer's own comment) -- the alternative is Phase 2
	// discovering it needs per-frame history that was thrown away along with
	// the graph that used to visualise it.
	static void PushFrametimeSample( float flMs )
	{
		s_flFrametimeHistoryMs[s_nHistoryHead] = flMs;
		s_nHistoryHead = ( s_nHistoryHead + 1 ) % kHistoryCapacity;
		if ( s_nHistoryCount < kHistoryCapacity )
			++s_nHistoryCount;
	}

	static float UpdateAndGetSmoothedFps()
	{
		const uint64_t ulRaw = g_ulLastAppFrametimeNs.load( std::memory_order_relaxed );
		if ( ulRaw != 0 && ulRaw != s_ulLastRawFrametimeNs )
		{
			s_ulLastRawFrametimeNs = ulRaw;

			// Clamp a single wild sample (e.g. a resume-from-pause hitch)
			// from dominating the EMA for multiple seconds.
			const float flMs = std::clamp( (float)ulRaw / 1e6f, 0.1f, 2000.0f );
			constexpr float kAlpha = 0.10f;
			s_flSmoothedFrametimeMs = s_flSmoothedFrametimeMs * ( 1.0f - kAlpha ) + flMs * kAlpha;

			// Same raw sample feeds the lag-spike history -- one history
			// entry per real game frame, exactly matching the headline
			// number's own clock.
			PushFrametimeSample( flMs );
		}
		return 1000.0f / s_flSmoothedFrametimeMs;
	}

	// -------------------------------------------------------------------
	// Render pipeline: own ImGui context, offscreen texture, general-queue
	// submission and timeline semaphore. See the file-level comment for
	// why this is a second, independent instance of SettingsOverlay.cpp's
	// pattern rather than sharing it.
	// -------------------------------------------------------------------

	static ImGuiContext *s_pImguiContext = nullptr;
	static bool s_bImguiInitialized = false;

	static OwningRc<CVulkanTexture> s_pOverlayTexture;
	static uint32_t s_uTextureWidth = 0;
	static uint32_t s_uTextureHeight = 0;
	static bool s_bTextureNeedsInitialBarrier = true;

	static std::unique_ptr<CVulkanCmdBuffer> s_pPrevCmdBuffer;
	static std::shared_ptr<VulkanTimelineSemaphore_t> s_pTimelineSemaphore;
	static uint64_t s_ulSignalCounter = 0;
	static uint64_t s_ulPrevSignalPoint = 0;
	static bool s_bHasPrevSubmission = false;

	static bool s_bHasPendingWaitPoint = false;
	static uint64_t s_ulPendingWaitPoint = 0;

	// Issue #22 return half. Identical mechanism to SettingsOverlay.cpp's
	// s_pReadDoneSemaphore -- see that file's comment for the full rationale.
	// This context is deliberately independent of the settings overlay's: the
	// HUD stays up while the settings panel is closed, so it owns its own
	// texture, its own semaphores and its own read-done bookkeeping.
	static std::shared_ptr<VulkanTimelineSemaphore_t> s_pReadDoneSemaphore;
	static uint64_t s_ulReadDoneCounter = 0;
	static uint64_t s_ulPendingReadDonePoint = 0;
	static uint64_t s_ulRegisteredReadDonePoint = 0;

	static uint64_t s_ulLastFrameTimeNanos = 0;

	static void EnsureImguiInit()
	{
		if ( s_bImguiInitialized )
			return;

		if ( s_pImguiContext != nullptr )
			return; // a previous ImGui_ImplVulkan_Init() attempt already failed once this run

		ImGuiContext *pPrevContext = ImGui::GetCurrentContext();

		IMGUI_CHECKVERSION();
		s_pImguiContext = ImGui::CreateContext();
		// M8 part 1 (issue #13) fix: the vendored ImGui version's own
		// CreateContext() only leaves the *new* context current when there
		// was no previous context -- otherwise it explicitly restores
		// whatever was current before the call (imgui.cpp's CreateContext():
		// "Restore previous context if any, else keep new one."). Since
		// SettingsOverlay.cpp's context is usually already current by the
		// time this runs (both are driven from the same paint_all() call),
		// relying on CreateContext() to leave s_pImguiContext current was
		// wrong -- it silently left SettingsOverlay's context active, so
		// every line below (GetIO(), fonts::Load(), ImGui_ImplVulkan_Init())
		// was operating on the WRONG context/atlas, corrupting
		// SettingsOverlay's font atlas and asserting
		// ("Already initialized a renderer backend!") the moment
		// ImGui_ImplVulkan_Init() tried to double-init that same IO. Found
		// while verifying M8's per-context font loading (Overlay/Fonts.cpp)
		// with both the settings overlay and the FPS HUD enabled together --
		// a real pre-existing latent bug, not something typography
		// introduced, but one this milestone's own correctness depends on.
		ImGui::SetCurrentContext( s_pImguiContext );

		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr;

		// M8 part 1 (issue #13, typeface swapped to Geist by #53): builds
		// the Geist font atlas for this context (a separate context/atlas
		// from SettingsOverlay's own -- see the file-level comment and
		// Overlay/Fonts.h). Must happen before ImGui_ImplVulkan_Init()
		// below, same reasoning as SettingsOverlay.cpp's own
		// EnsureImguiInit().
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
		init_info.DescriptorPoolSize = 64;
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
				s_FpsLog.errorf( "ImGui Vulkan backend: VkResult %d", (int)err );
		};

		if ( !ImGui_ImplVulkan_Init( &init_info ) )
		{
			s_FpsLog.errorf( "ImGui_ImplVulkan_Init failed" );
			ImGui::SetCurrentContext( pPrevContext );
			return;
		}

		s_bImguiInitialized = true;
		ImGui::SetCurrentContext( pPrevContext );
	}

	// CPU-waits for our own previous general-queue submission to retire, then
	// releases its command buffer. Mirrors SettingsOverlay.cpp's function of
	// the same name; called from RenderAndSubmit() (where it should return
	// immediately) and from EnsureTexture() before a resize drops the texture.
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

		// Drain before dropping the old texture (see DrainPrevSubmission) -- the in-flight general-queue
		// submission holds no Rc<> on it (it only names a raw VkImageView in
		// its VkRenderingAttachmentInfo). Same reasoning as the identical call
		// in SettingsOverlay.cpp's EnsureTexture().
		DrainPrevSubmission();

		OwningRc<CVulkanTexture> pNewTexture = new CVulkanTexture();

		CVulkanTexture::createFlags flags;
		flags.bSampled = true;
		flags.bColorAttachment = true;
		// Written on the general queue, sampled on the compute queue: needs
		// CONCURRENT sharing across both families. See
		// CVulkanTexture::createFlags::bGeneralQueueShared.
		flags.bGeneralQueueShared = true;

		if ( !pNewTexture->BInit( uWidth, uHeight, 1u, VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM ), flags ) )
		{
			s_FpsLog.errorf( "failed to (re)create the FPS display's offscreen texture at %ux%u", uWidth, uHeight );
			return false;
		}

		s_pOverlayTexture = std::move( pNewTexture );
		s_uTextureWidth = uWidth;
		s_uTextureHeight = uHeight;
		s_bTextureNeedsInitialBarrier = true;
		return true;
	}

	// -------------------------------------------------------------------
	// Issue #29 (System Monitor part 3/3): blend_mode gains "inverted"
	// alongside alpha/additive, plus an optional colour override for the
	// FPS number's text. Shared helpers below.
	// -------------------------------------------------------------------

	// A filled backdrop only makes sense in "alpha" mode -- additive
	// already auto-disables it (a filled rect would itself glow, SPEC.md
	// B5) and inverted does too, for the same reason this issue's own text
	// anticipates: a static backdrop fill sits behind the outline/fill
	// treatment AddTextInverted() draws and would just read as visual
	// noise rather than helping legibility.
	static bool ModuleBackdropAllowed( const config::FpsDisplaySettings &cfg )
	{
		return cfg.blend_mode == "alpha";
	}

	// Unpacks the 0xRRGGBB int config::FpsDisplaySettings::color_fps stores
	// on disk into an ImVec4 (0..1 floats, alpha ignored).
	static ImVec4 UnpackColorRgb( int nPacked, float flAlpha = 1.0f )
	{
		return ImVec4(
			( ( nPacked >> 16 ) & 0xFF ) / 255.0f,
			( ( nPacked >> 8 ) & 0xFF ) / 255.0f,
			( nPacked & 0xFF ) / 255.0f,
			flAlpha );
	}

	// Resolves the FPS number's "value" text colour: the user's explicit
	// override when set, else `defaultColor` -- a Palette.h accent-family
	// token (never an invented literal). An unset override therefore moves
	// automatically if issue #37's hue-selectable accent work changes what
	// that Palette.h token resolves to at runtime; a set override is a
	// deliberate, explicit user choice and intentionally does NOT track the
	// accent hue.
	static ImVec4 ModuleColorVec4( const std::optional<int> &oOverride, ImU32 defaultColor, float flAlpha = 1.0f )
	{
		ImVec4 col = oOverride.has_value() ? UnpackColorRgb( *oOverride ) : gamescope::palette::ToVec4( defaultColor );
		col.w = flAlpha;
		return col;
	}

	// Issue #29's "Inverted" blend mode. What "inverted" means here, and
	// why it is NOT a literal per-pixel GPU-blend destination-invert
	// (VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR against the actual composited
	// game frame), is worth being explicit about rather than silently
	// under-delivering the task brief's own literal wording:
	//
	// This whole readout renders into its own isolated offscreen texture
	// (s_pOverlayTexture, cleared to transparent every frame -- see
	// RenderAndSubmit()'s VK_ATTACHMENT_LOAD_OP_CLEAR) on the general
	// queue, entirely independent of the game's own frame. It is handed to
	// paint_all() as one more Layer_t and composited onto the actual game
	// content LATER, on the compute queue, by vulkan_composite() -- using
	// one of exactly three fixed per-layer blend equations baked into that
	// compute shader (rendervulkan.hpp's AlphaBlendingMode_t: PREMULTIPLIED
	// / COVERAGE / NONE; see rendervulkan.cpp's u_alphaMode packing). A
	// literal "invert whatever's underneath" blend mode is a property of
	// THAT later compositing step, not of this file's own draw pass --
	// this file's "destination" during its own ImGui rendering is only
	// ever this texture's own (normally transparent) prior content, never
	// the game frame, so no amount of Vulkan blend-state work confined to
	// this file can make a real per-pixel invert of the actual game
	// picture happen.
	//
	// What IS both real and fully in-scope: pairing a black outline with a
	// white fill is the same "reads over anything" technique real
	// injected overlays (RTSS, MangoHud) rely on, and it is a content-
	// INDEPENDENT guarantee by construction (mostly-transparent glyph
	// shapes rather than one large flat-color block, which is also the
	// literal "useful for OLED" property the task brief asks for) rather
	// than a fixed single colour that can still fail against a
	// similar-toned background the way plain "alpha" mode's text can.
	// Backdrop is auto-disabled in this mode (ModuleBackdropAllowed()) --
	// a solid backdrop fill behind an outline/fill pair that's already
	// legible on its own just reads as noise.
	static void AddTextInvertedSized( ImDrawList *pDrawList, ImFont *pFont, float flFontSize, ImVec2 pos, const char *pszText )
	{
		static constexpr ImVec2 kOffsets[4] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
		const ImU32 outlineColor = IM_COL32( 0, 0, 0, 235 );
		for ( const ImVec2 &off : kOffsets )
			pDrawList->AddText( pFont, flFontSize, ImVec2( pos.x + off.x, pos.y + off.y ), outlineColor, pszText );
		pDrawList->AddText( pFont, flFontSize, pos, IM_COL32( 255, 255, 255, 255 ), pszText );
	}

	// Same, for the implicit-current-font AddText overload (the " FPS" unit
	// label, which draws at the Meta style's own baked size).
	static void AddTextInverted( ImDrawList *pDrawList, ImVec2 pos, const char *pszText )
	{
		static constexpr ImVec2 kOffsets[4] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
		const ImU32 outlineColor = IM_COL32( 0, 0, 0, 235 );
		for ( const ImVec2 &off : kOffsets )
			pDrawList->AddText( ImVec2( pos.x + off.x, pos.y + off.y ), outlineColor, pszText );
		pDrawList->AddText( pos, IM_COL32( 255, 255, 255, 255 ), pszText );
	}

	// Shared box backdrop (issue #28: factored out so a future module would
	// draw an identical backdrop rather than a second copy of the same four
	// lines -- kept even with only one module left, since the FPS module
	// still uses it). Colours are Palette.h's own tokens/§1 literals.
	static void DrawModuleBackdrop( ImDrawList *pDrawList, ImVec2 origin, ImVec2 boxSize, bool bDrawBackdrop, const config::FpsDisplaySettings &cfg )
	{
		if ( !bDrawBackdrop )
			return;

		const ImVec2 rectMin = origin;
		const ImVec2 rectMax( origin.x + boxSize.x, origin.y + boxSize.y );
		const ImU32 backdropColor = ImGui::ColorConvertFloat4ToU32( ImVec4( 0x09 / 255.0f, 0x0b / 255.0f, 0x0e / 255.0f, cfg.backdrop_opacity ) );
		pDrawList->AddRectFilled( rectMin, rectMax, backdropColor, cfg.backdrop_rounding );
		pDrawList->AddRect( rectMin, rectMax, ImGui::GetColorU32( gamescope::palette::White( 0.12f ) ), cfg.backdrop_rounding );
	}

	// -------------------------------------------------------------------
	// Placement: 9 anchor positions (issue #26/#27's shared 3x3 grid
	// model) plus pixel margins -- this file's own copy of the same
	// kPlacements/ParsePlacement shape Notifications.cpp uses for
	// notification_placement (kept as a separate copy rather than a shared
	// header since Notifications.cpp's own version is a file-local static,
	// not exported).
	//
	// Scope reduction (2026-09-03): the named-layout system (per-module
	// manual x/y placement, layouts/<name>.json) is gone -- see
	// superdoc/meta/TERMINOLOGY.md's "profiler" entry and CHANGELOG.md.
	// With only one module left, placement goes back to this simpler
	// anchor + margin model.
	// -------------------------------------------------------------------

	namespace
	{
		// [vertical: top/center/bottom][horizontal: left/center/right]
		constexpr const char *kPlacements[3][3] = {
			{ "top-left",    "top-center",    "top-right"    },
			{ "center-left", "center",        "center-right" },
			{ "bottom-left", "bottom-center", "bottom-right" },
		};

		void ParsePlacement( const std::string &sPlacement, int &nVert, int &nHoriz )
		{
			for ( int v = 0; v < 3; v++ )
			{
				for ( int h = 0; h < 3; h++ )
				{
					if ( sPlacement == kPlacements[v][h] )
					{
						nVert = v;
						nHoriz = h;
						return;
					}
				}
			}
			// Unrecognized/legacy value -- fall back to this readout's
			// original hardcoded default (top-right).
			nVert = 0;
			nHoriz = 2;
		}

		std::string ComposePlacement( int nVert, int nHoriz )
		{
			return kPlacements[std::clamp( nVert, 0, 2 )][std::clamp( nHoriz, 0, 2 )];
		}

		// Resolves `anchor`'s named 3x3 cell, offset by the independent
		// margin_x/margin_y, into the box's top-left pixel position --
		// replaces the old per-module HudLayout::x/y/origin resolution
		// (ResolveModuleOrigin(), deleted with the rest of the module
		// framework) now that there is only ever one box to place. Clamped
		// fully on-screen so a large margin can never push the box off the
		// edge of the display.
		ImVec2 ResolveAnchoredOrigin( const std::string &sAnchor, float flMarginX, float flMarginY, ImVec2 boxSize, ImVec2 ioDisplay )
		{
			int nVert = 0, nHoriz = 2;
			ParsePlacement( sAnchor, nVert, nHoriz );

			const float flX = ( nHoriz == 0 ) ? flMarginX
				: ( nHoriz == 1 ) ? ( ioDisplay.x - boxSize.x ) * 0.5f
				: ( ioDisplay.x - flMarginX - boxSize.x );
			const float flY = ( nVert == 0 ) ? flMarginY
				: ( nVert == 1 ) ? ( ioDisplay.y - boxSize.y ) * 0.5f
				: ( ioDisplay.y - flMarginY - boxSize.y );

			return ImVec2(
				std::clamp( flX, 0.0f, std::max( 0.0f, ioDisplay.x - boxSize.x ) ),
				std::clamp( flY, 0.0f, std::max( 0.0f, ioDisplay.y - boxSize.y ) ) );
		}
	}

	// -------------------------------------------------------------------
	// The FPS module: number, unit label, backdrop, blend-mode treatment.
	// Everything that made this a small profiler (CPU/GPU load, the
	// frametime graph, the percentile row, Now Playing) is gone -- see
	// this file's header comment.
	// -------------------------------------------------------------------

	// Measured layout: every string/size the draw half needs, computed once
	// so the readout's own box size is known before DrawFpsModuleContent()
	// draws into it.
	struct FpsModuleLayout
	{
		bool bAdditive = false;
		bool bInverted = false;
		bool bDrawBackdrop = false;
		ImU32 textColor = 0;
		char szNum[8] = "";
		ImVec2 numSize{};
		ImVec2 unitSize{};
		ImVec2 textSize{};
		float flContentWidth = 0.0f;
		float flContentHeight = 0.0f;
	};

	static constexpr const char *kUnitText = " FPS";

	// M8 part 1 (issue #13, typeface swapped to Geist by #53): Geist Mono
	// is genuinely monospaced, so a fixed-width formatted string
	// ("%3d") is tabular by construction -- every digit occupies the same
	// advance width, so the readout cannot jitter horizontally as the
	// number changes.
	static FpsModuleLayout MeasureFpsModule( int nFps )
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		FpsModuleLayout L;

		L.bAdditive = cfg.blend_mode == "additive";
		L.bInverted = cfg.blend_mode == "inverted";
		// additive/inverted + a filled backdrop rect would make the
		// backdrop itself glow (additive, SPEC.md B5) or just read as
		// noise behind an already-legible outline/fill pair (inverted) --
		// auto-disable rather than combine them.
		L.bDrawBackdrop = cfg.backdrop_enabled && ModuleBackdropAllowed( cfg );

		// Gamescope's own layer blend modes (rendervulkan.hpp) are
		// PREMULTIPLIED/COVERAGE/NONE -- there is no whole-layer additive
		// mode to hand this off to. "Additive" here is approximated at
		// the draw-list level (no backdrop, brighter/accent-tinted text)
		// rather than literal GPU ADD blend-func compositing against the
		// scene. "Inverted" draws via AddTextInvertedSized() instead
		// (see that function's own comment) rather than a flat
		// L.textColor, so its value here is unused when L.bInverted.
		const ImVec4 textColorBase = L.bAdditive
			? ImVec4( 0x7d / 255.0f, 0xe6 / 255.0f, 0xf7 / 255.0f, 1.0f ) // brighter cyan "glow"
			: ModuleColorVec4( cfg.color_fps, gamescope::palette::kAccentValue );
		L.textColor = ImGui::ColorConvertFloat4ToU32(
			ImVec4( textColorBase.x, textColorBase.y, textColorBase.z, cfg.text_opacity ) );

		// Right-justified in a fixed 3-character field (blank-, not zero-,
		// padded) -- 0-999 is plenty for a frame-rate readout.
		snprintf( L.szNum, sizeof( L.szNum ), "%3d", std::clamp( nFps, 0, 999 ) );
		ImFont *pFont = gamescope::fonts::Get( gamescope::fonts::Style::Hero );
		const float flFontSize = cfg.font_size; // still user-configurable (M4's own font-size slider) -- ImGui scales the baked Hero glyphs to whatever size is requested
		L.numSize = pFont->CalcTextSizeA( flFontSize, FLT_MAX, 0.0f, L.szNum );

		// Issue #73: the unit label is independently hideable
		// (fps_label_enabled) so the module can show just the number --
		// zero-sized when off. This field's own settings row was removed
		// (see this file's header comment), but the field and this
		// behaviour stay: an old config that turned it off keeps that
		// choice.
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		L.unitSize = cfg.fps_label_enabled ? ImGui::CalcTextSize( kUnitText ) : ImVec2( 0.0f, 0.0f );
		ImGui::PopFont();

		L.textSize = ImVec2( L.numSize.x + L.unitSize.x, std::max( L.numSize.y, L.unitSize.y ) );
		L.flContentWidth = L.textSize.x;
		L.flContentHeight = L.textSize.y;

		return L;
	}

	// Draws the FPS module's backdrop + content into the box
	// [origin, origin+boxSize).
	static void DrawFpsModuleContent( ImDrawList *pDrawList, ImVec2 origin, ImVec2 boxSize, const FpsModuleLayout &L )
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;

		const ImVec2 rectMin = origin;
		const ImVec2 textPos( rectMin.x + cfg.backdrop_padding, rectMin.y + cfg.backdrop_padding );

		DrawModuleBackdrop( pDrawList, origin, boxSize, L.bDrawBackdrop, cfg );

		ImFont *pFont = gamescope::fonts::Get( gamescope::fonts::Style::Hero );
		const float flFontSize = cfg.font_size;

		ImVec2 cursor = textPos;
		if ( L.bInverted )
			AddTextInvertedSized( pDrawList, pFont, flFontSize, cursor, L.szNum );
		else
			pDrawList->AddText( pFont, flFontSize, cursor, L.textColor, L.szNum );
		cursor.x += L.numSize.x;

		if ( cfg.fps_label_enabled ) // issue #73
		{
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			const ImVec2 unitPos( cursor.x, cursor.y + ( L.numSize.y - L.unitSize.y ) );
			if ( L.bInverted )
				AddTextInverted( pDrawList, unitPos, kUnitText );
			else
				pDrawList->AddText( unitPos, ImGui::GetColorU32( gamescope::palette::White( 0.50f ) ), kUnitText );
			ImGui::PopFont();
		}
	}

	// Scope reduction (2026-09-03): DrawReadout() used to measure and place
	// a fixed sequence of modules (Fps/Cpu/Gpu/Media) each against its own
	// entry in a resolved config::HudLayout. That whole framework (ModuleKind,
	// kModuleOrder, MeasureModule, ModulePlacement, ResolveModuleOrigin,
	// DrawModule, and the CPU/GPU/Media modules themselves) is gone -- there
	// is exactly one module now, placed by the plain anchor+margin model
	// above.
	static void DrawReadout()
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;

		const int nFps = (int)std::lround( UpdateAndGetSmoothedFps() );

		ImDrawList *pDrawList = ImGui::GetBackgroundDrawList();
		const ImVec2 io_display = ImGui::GetIO().DisplaySize; // actual output resolution, see FpsDisplay_AddLayer()

		const FpsModuleLayout L = MeasureFpsModule( nFps );
		const ImVec2 boxSize( L.flContentWidth + cfg.backdrop_padding * 2.0f, L.flContentHeight + cfg.backdrop_padding * 2.0f );
		const ImVec2 origin = ResolveAnchoredOrigin( cfg.anchor, (float)cfg.margin_x, (float)cfg.margin_y, boxSize, io_display );

		DrawFpsModuleContent( pDrawList, origin, boxSize, L );
	}

	static bool RenderAndSubmit()
	{
		DrainPrevSubmission();

		if ( !g_device.vk.CmdBeginRendering || !g_device.vk.CmdEndRendering )
		{
			s_FpsLog.errorf( "vkCmdBeginRendering/vkCmdEndRendering not available on this device" );
			return false;
		}

		auto cmdBuffer = g_device.generalCommandBuffer();
		if ( !cmdBuffer )
			return false;

		// Issue #22: block this frame's LOAD_OP_CLEAR on the GPU until the
		// last submitted composite finished sampling the texture.
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

	void FpsDisplay_AddLayer( FrameInfo_t *pFrameInfo )
	{
		EnsureConfigLoaded();

		if ( !s_Settings.fps_display.enabled )
			return;

		// Idle-client keepalive lives entirely in the background repaint-
		// timer thread now (EnsureConfigLoaded -> EnsureRepaintTimerThread)
		// -- see that thread's own comment for why a per-paint request here
		// can't sustain a slower-than-vblank cadence by itself.

		if ( g_nOutputWidth == 0 || g_nOutputHeight == 0 )
			return;

		// See the file-level comment: always explicitly save/restore the
		// current ImGui context around this whole pass, since
		// SettingsOverlay.cpp's own calls assume nothing else touches it.
		ImGuiContext *pPrevContext = ImGui::GetCurrentContext();
		auto RestoreContext = [pPrevContext] { ImGui::SetCurrentContext( pPrevContext ); };

		EnsureImguiInit();
		if ( !s_bImguiInitialized )
			return; // EnsureImguiInit() already restored pPrevContext on failure

		ImGui::SetCurrentContext( s_pImguiContext );

		if ( !EnsureTexture( g_nOutputWidth, g_nOutputHeight ) )
		{
			RestoreContext();
			return;
		}

		const uint64_t ulNowNanos = get_time_in_nanos();
		float flDeltaTime = s_ulLastFrameTimeNanos == 0
			? ( 1.0f / 60.0f )
			: float( ulNowNanos - s_ulLastFrameTimeNanos ) / 1e9f;
		s_ulLastFrameTimeNanos = ulNowNanos;
		flDeltaTime = std::clamp( flDeltaTime, 1.0f / 1000.0f, 1.0f );

		ImGuiIO &io = ImGui::GetIO();
		io.DisplaySize = ImVec2( (float)s_uTextureWidth, (float)s_uTextureHeight );
		io.DeltaTime = flDeltaTime;

		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		DrawReadout();
		ImGui::Render();

		const bool bSubmitted = RenderAndSubmit();

		RestoreContext();

		if ( !bSubmitted )
			return;

		// ponytail: relies on the same paint_all()-level bValidContents
		// precondition SettingsOverlay.h documents at
		// SettingsOverlay_AddLayer() -- not re-derived here since it's
		// shared paint_all() behaviour, not specific to this feature.
		FrameInfo_t::Layer_t *layer = pFrameInfo->layers.push();
		if ( !layer )
			return; // out of layer slots this frame

		// The HUD layer sits ABOVE the Shell (g_zposFpsDisplay >
		// g_zposSettingsOverlay, steamcompmgr.hpp) so the live readout
		// stays visible whether or not the settings panel is open --
		// deliberate, see that file's own comment.
		layer->tex = s_pOverlayTexture;
		layer->zpos = g_zposFpsDisplay;
		layer->offset = { 0.0f, 0.0f };
		layer->scale = { 1.0f, 1.0f };
		layer->opacity = 1.0f; // no fade -- this HUD is either on or off, per its own `enabled` setting
		layer->filter = GamescopeUpscaleFilter::LINEAR;
		layer->blackBorder = false;
		layer->applyColorMgmt = false;
		layer->eAlphaBlendingMode = ALPHA_BLENDING_MODE_COVERAGE; // straight (non-premultiplied) alpha, same reasoning as SettingsOverlay's own layer
		layer->ctm = nullptr;
		layer->hdr_metadata_blob = nullptr;
		layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
	}

	void FpsDisplay_WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer )
	{
		if ( !s_bHasPendingWaitPoint )
			return;

		pComputeCmdBuffer->AddDependency( s_pTimelineSemaphore, s_ulPendingWaitPoint );

		// Issue #22 return half -- see s_pReadDoneSemaphore.
		s_ulPendingReadDonePoint = ++s_ulReadDoneCounter;
		pComputeCmdBuffer->AddSignal( s_pReadDoneSemaphore, s_ulPendingReadDonePoint );
	}

	// Called by vulkan_composite() once the compute submission is actually on
	// the queue, promoting the pending read-done point to one it is safe for a
	// later general-queue submission to wait on. See SettingsOverlay.cpp's
	// SettingsOverlay_CommitReads() for why the promotion must happen here and
	// not at record time.
	void FpsDisplay_CommitReads()
	{
		if ( !s_ulPendingReadDonePoint )
			return;

		s_ulRegisteredReadDonePoint = s_ulPendingReadDonePoint;
		s_ulPendingReadDonePoint = 0;
	}

	// -------------------------------------------------------------------
	// The HUD AREA (E2, P3 part C) -- the settings half of this file.
	//
	// Scope reduction (2026-09-03): this used to declare seven module
	// switches, a "hud.edit_layout" drag-editor action, per-module colour
	// overrides, and Diagnostics/Statistics groups full of graphs -- all
	// gone along with the profiler modules and the named-layout system they
	// depended on (see this file's header comment). What's left is
	// deliberately minimal: Show HUD, placement (anchor + margins), and font
	// size. Phase 2 (a separate task) rebuilds this tab properly.
	// -------------------------------------------------------------------

	void FpsDisplay_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.hud", "HUD", ui::Section::System );

		a.Keywords( "hud fps overlay performance" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			return s_Settings.fps_display.enabled ? std::string( "on" ) : std::string( "off" );
		} );

		// The reason every gated row shares. The master switch is
		// deliberately NOT gated by itself -- SPEC §3.13's exception: a
		// control that is the cause of a greying stays reachable.
		auto MonitorOn = []{ EnsureConfigLoaded(); return s_Settings.fps_display.enabled; };
		constexpr const char *kOffReason = "the HUD is off";

		// =================================================================
		//  HUD
		// =================================================================
		a.Group( "HUD" );

		a.Switch( "hud.enabled", "Show HUD",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.fps_display.enabled; },
				[]( bool b )
				{
					EnsureConfigLoaded();
					s_Settings.fps_display.enabled = b;
					PersistSettings();
					s_bHudEnabledForTimer.store( b, std::memory_order_relaxed );
					// See cc_toggle_fps_display's identical call for why: an
					// immediate frame for the toggle itself, on top of the
					// background repaint-timer thread that sustains the
					// cadence afterward.
					force_repaint();
				} ) )
			.Help( "Shows your frame rate over the game. It stays visible even after you close "
			       "this settings menu." )
			.Default( config::FpsDisplaySettings{}.enabled )
			.Keywords( "hud overlay show fps display enable" );

		// =================================================================
		//  Placement
		// =================================================================
		a.Group( "Placement" );

		// The stored format is the same "top-right"/"center-left" string
		// kPlacements has always written. The two axes are a VIEW of that
		// string, not a new representation of it -- each setter re-parses
		// the current value before composing, rather than keeping a second
		// copy of the other axis that could drift.
		a.Composite( "hud.anchor", "Placement", ui::CompositeKind::Anchor,
			ui::AnyBind::Of<int>(
				[]
				{
					EnsureConfigLoaded();
					int nV = 0, nH = 2;
					ParsePlacement( s_Settings.fps_display.anchor, nV, nH );
					return nV;
				},
				[]( int nV )
				{
					EnsureConfigLoaded();
					int nOldV = 0, nH = 2;
					ParsePlacement( s_Settings.fps_display.anchor, nOldV, nH );
					s_Settings.fps_display.anchor = ComposePlacement( nV, nH );
					PersistSettings();
				} ),
			ui::AnyBind::Of<int>(
				[]
				{
					EnsureConfigLoaded();
					int nV = 0, nH = 2;
					ParsePlacement( s_Settings.fps_display.anchor, nV, nH );
					return nH;
				},
				[]( int nH )
				{
					EnsureConfigLoaded();
					int nV = 0, nOldH = 2;
					ParsePlacement( s_Settings.fps_display.anchor, nV, nOldH );
					s_Settings.fps_display.anchor = ComposePlacement( nV, nH );
					PersistSettings();
				} ) )
			.Help( "Which screen corner the HUD sticks to. The margins below move it a bit away "
			       "from that corner." )
			.Default( 0, 2 )
			.Keywords( "anchor placement position corner where margin offset" )
			.DisabledUnless( MonitorOn, kOffReason )
			.Param( "margin_x", "Horizontal margin",
				ui::AnyBind::Of<int>(
					[]{ EnsureConfigLoaded(); return s_Settings.fps_display.margin_x; },
					[]( int n ) { EnsureConfigLoaded(); s_Settings.fps_display.margin_x = n; PersistSettings(); } ) )
				.Range( 0.0f, 128.0f ).Step( 4.0f ).Unit( "px" )
				.Default( config::FpsDisplaySettings{}.margin_x )
				.Help( "How far the HUD sits from the left or right edge." )
			.Param( "margin_y", "Vertical margin",
				ui::AnyBind::Of<int>(
					[]{ EnsureConfigLoaded(); return s_Settings.fps_display.margin_y; },
					[]( int n ) { EnsureConfigLoaded(); s_Settings.fps_display.margin_y = n; PersistSettings(); } ) )
				.Range( 0.0f, 128.0f ).Step( 4.0f ).Unit( "px" )
				.Default( config::FpsDisplaySettings{}.margin_y )
				.Help( "How far the HUD sits from the top or bottom edge." );

		// =================================================================
		//  Appearance
		// =================================================================
		a.Group( "Appearance" );

		a.Slider( "hud.font_size", "Font size",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.fps_display.font_size; },
				[]( float f ) { EnsureConfigLoaded(); s_Settings.fps_display.font_size = f; PersistSettings(); } ) )
			.Help( "How big the HUD's text is." )
			.Range( 10.0f, 48.0f )
			.Step( 1.0f )
			.Unit( "px" )
			.Default( config::FpsDisplaySettings{}.font_size )
			.Keywords( "font size text scale hud" )   // 39 positions, whole px
			.DisabledUnless( MonitorOn, kOffReason );
	}
}
