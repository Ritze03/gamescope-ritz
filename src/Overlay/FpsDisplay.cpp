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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "main.hpp"
#include "log.hpp"
#include "convar.h"
#include "Config/ConfigManager.h"
#include "Config/AppId.h"
#include "Fonts.h"
#include "Widgets.h"
#include "Palette.h"
#include "Metrics/SystemStats.h"

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

	// M7: reloads whenever PanelConfig.cpp bumps config::ConfigGeneration()
	// (profile applied, override toggled, another game's config copied in),
	// not just on this file's very first draw -- s_Settings.fps_display is
	// read directly every frame in DrawReadout()/FpsDisplay_AddLayer(), so a
	// plain reload is all a mid-session change needs here (unlike
	// PanelDisplay.cpp/PanelShaders.cpp, nothing else caches a "live" copy
	// of these fields to push).
	static void EnsureConfigLoaded()
	{
		const uint64_t ulGeneration = config::ConfigGeneration();
		if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
			return;
		s_Settings = config::ResolveEffective( config::SessionAppId() );
		s_ulLoadedGeneration = ulGeneration;
		s_bConfigLoaded = true;
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
	// Frametime history: a ring buffer of raw (unsmoothed) per-frame game
	// frametimes, feeding both the frametime graph and the percentile row.
	// Every entry is a real g_ulLastAppFrametimeNs sample (commit_t::Signal()'s
	// game-frame delta, DECISIONS.md #16/#17) -- never the compositor's
	// composite rate or the display's refresh rate, so the graph/percentiles
	// stay on the same clock as the headline number, per this feature's task
	// brief.
	//
	// Sized at 240 samples to match this repo's own spec text for this exact
	// feature (ui-mockup-precise-spec.md §11's FPS-config-window footer:
	// "sampling 500 ms · 240-frame window") rather than an invented number.
	// At typical 60-240fps that's roughly 1-4 seconds of real play: long
	// enough that a stutter is still on-screen a moment after it happened
	// and that the 1%/0.1% low buckets have more than a couple of samples to
	// average (240 * 1% = 2.4 -> 2 samples; 240 * 0.1% = 0.24 -> rounds up to
	// the single worst frame in the window -- a real limitation of a 240-deep
	// window worth knowing: a proper 0.1% low usually wants thousands of
	// frames, so this one reads more like "worst frame in the last few
	// seconds" than a statistically deep percentile; still an honest number
	// over its stated window, just a coarse one), short enough that the
	// graph reads as "right now" instead of a stale minute-old trace. The
	// on-screen graph draws only as many bars as fit the HUD's width (well
	// under 240 at typical container sizes) from the tail of this buffer;
	// the percentile math uses the buffer's full span.
	static constexpr int kHistoryCapacity = 240;
	static float s_flFrametimeHistoryMs[kHistoryCapacity] = {};
	static int s_nHistoryCount = 0;  // valid samples so far (caps at kHistoryCapacity)
	static int s_nHistoryHead = 0;   // index the NEXT sample will be written to

	static void PushFrametimeSample( float flMs )
	{
		s_flFrametimeHistoryMs[s_nHistoryHead] = flMs;
		s_nHistoryHead = ( s_nHistoryHead + 1 ) % kHistoryCapacity;
		if ( s_nHistoryCount < kHistoryCapacity )
			++s_nHistoryCount;
	}

	// Graph y-axis ceiling (ms mapped to the top of the 18px strip). Chases
	// the window's worst sample with a slow EMA rather than the window's raw
	// max, specifically so a single stutter doesn't yank the whole graph's
	// scale around (the task brief's own ask: "a scale that does not
	// rescale distractingly on every spike"). A fresh spike still reads
	// clearly in the meantime -- it's drawn capped at the strip's full
	// height and in the amber outlier color (see DrawFrametimeGraph) before
	// the ceiling has caught up to it, exactly the "obvious at a glance"
	// treatment the brief asks for, and the ceiling only widens to
	// accommodate it a little at a time. Floored at a 60fps-equivalent
	// frametime so a rock-steady high-fps game doesn't get zoomed in so far
	// that sub-millisecond GPU noise reads as huge bars.
	static float s_flGraphCeilingMs = ( 1000.0f / 60.0f ) * 1.5f;

	static void UpdateGraphCeiling()
	{
		if ( s_nHistoryCount == 0 )
			return;

		float flMaxMs = 0.0f;
		for ( int i = 0; i < s_nHistoryCount; ++i )
			flMaxMs = std::max( flMaxMs, s_flFrametimeHistoryMs[i] );

		const float flDesiredCeiling = std::max( flMaxMs * 1.15f, 1000.0f / 60.0f );
		constexpr float kCeilingAlpha = 0.03f; // slow -- see this block's own comment for why
		s_flGraphCeilingMs = s_flGraphCeilingMs * ( 1.0f - kCeilingAlpha ) + flDesiredCeiling * kCeilingAlpha;
	}

	// -------------------------------------------------------------------
	// Percentiles: 1% low, 0.1% low, average -- all computed over the same
	// kHistoryCapacity window and the same game-frametime clock as the
	// headline number and the graph (never mixed with mangoapp's composited/
	// display clocks). Recomputed at most every 500ms (matching the spec's
	// own "sampling 500 ms" footer text) rather than every frame: sorting up
	// to 240 floats every single draw is wasted work when the result would
	// otherwise change (and visually jitter) every frame for no benefit --
	// stats like these are meant to be read as a settled number, not a
	// twitchy one.
	// -------------------------------------------------------------------

	static float s_flAverageFps = 60.0f;
	static float s_flOnePercentLowFps = 60.0f;
	static float s_flPointOnePercentLowFps = 60.0f;
	static uint64_t s_ulLastPercentileComputeNanos = 0;

	// Average fps of the worst flFraction of samples in the window (e.g.
	// 0.01 for 1% low, 0.001 for 0.1% low) -- the standard "1% low" gamer
	// definition (mean of the slowest bucket), not the single value at that
	// percentile rank. pSortedMs must be ascending by frametime, so the
	// worst (highest-ms/lowest-fps) samples are its tail.
	static float WorstBucketAverageFps( const float *pSortedMs, int nCount, float flFraction )
	{
		const int nBucket = std::clamp( (int)std::lround( nCount * flFraction ), 1, nCount );
		float flSumMs = 0.0f;
		for ( int i = nCount - nBucket; i < nCount; ++i )
			flSumMs += pSortedMs[i];
		return 1000.0f / std::max( flSumMs / nBucket, 0.01f );
	}

	static void RecomputePercentilesIfDue( uint64_t ulNowNanos )
	{
		if ( s_nHistoryCount == 0 )
			return;

		constexpr uint64_t kRecomputeIntervalNs = 500ull * 1000000ull; // spec §11 footer: "sampling 500 ms"
		if ( s_ulLastPercentileComputeNanos != 0 && ulNowNanos - s_ulLastPercentileComputeNanos < kRecomputeIntervalNs )
			return;
		s_ulLastPercentileComputeNanos = ulNowNanos;

		float flSorted[kHistoryCapacity];
		std::copy( s_flFrametimeHistoryMs, s_flFrametimeHistoryMs + s_nHistoryCount, flSorted );
		std::sort( flSorted, flSorted + s_nHistoryCount );

		float flSumMs = 0.0f;
		for ( int i = 0; i < s_nHistoryCount; ++i )
			flSumMs += flSorted[i];
		s_flAverageFps = 1000.0f / std::max( flSumMs / s_nHistoryCount, 0.01f );

		s_flOnePercentLowFps = WorstBucketAverageFps( flSorted, s_nHistoryCount, 0.01f );
		s_flPointOnePercentLowFps = WorstBucketAverageFps( flSorted, s_nHistoryCount, 0.001f );
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

			// Same raw sample feeds the graph/percentile history -- one
			// history entry per real game frame, exactly matching the
			// headline number's own clock.
			PushFrametimeSample( flMs );
			UpdateGraphCeiling();
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

		// M8 part 1 (issue #13): builds the IBM Plex font atlas for this
		// context (a separate context/atlas from SettingsOverlay's own --
		// see the file-level comment and Overlay/Fonts.h). Must happen
		// before ImGui_ImplVulkan_Init() below, same reasoning as
		// SettingsOverlay.cpp's own EnsureImguiInit().
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

	// Spec §7 "Meters and graphs" / §10 Row 2: vertical bars, 1px gaps,
	// bottom-aligned in an 18px-tall strip; normal bar accent @ 60%, outlier
	// bar spike-amber @ 80%; bar heights 33-100% of the strip. Draws the most
	// recent bars that fit flWidth, right-aligned (newest at the right edge,
	// reading left-to-right as old-to-new, the universal waveform/graph
	// convention) out of the shared kHistoryCapacity history -- see that
	// buffer's own comment for why 240 samples is comfortably more history
	// than a HUD-width graph ever draws in one frame.
	static void DrawFrametimeGraph( ImDrawList *pDrawList, ImVec2 origin, float flWidth, float flHeight )
	{
		constexpr float kBarWidth = 2.0f;
		constexpr float kBarGap = 1.0f; // spec §7: "1px gaps"
		constexpr float kPitch = kBarWidth + kBarGap;

		const int nBarsFit = std::max( 1, (int)( flWidth / kPitch ) );
		const int nBars = std::min( nBarsFit, s_nHistoryCount );
		const float flBottom = origin.y + flHeight;

		// A bar reads as a stutter when it's meaningfully worse than the
		// *current* short-term pace (the same EMA the headline number uses),
		// not some stale window-wide average -- otherwise a game that has
		// genuinely settled into a lower framerate would paint its entire
		// graph amber forever. 50% slower AND at least 2ms slower (so a
		// 240fps game's sub-millisecond jitter doesn't false-positive).
		const float flBaselineMs = s_flSmoothedFrametimeMs;

		float flX = origin.x + flWidth - kBarWidth;
		for ( int i = 0; i < nBars && flX >= origin.x; ++i )
		{
			// s_nHistoryHead is the next WRITE slot, so head-1 is the most
			// recently written sample; walk backwards through the ring.
			const int nIdx = ( s_nHistoryHead - 1 - i + kHistoryCapacity ) % kHistoryCapacity;
			const float flMs = s_flFrametimeHistoryMs[nIdx];

			const float flFrac = std::clamp( flMs / std::max( s_flGraphCeilingMs, 0.001f ), 0.0f, 1.0f );
			const float flHeightFrac = 0.33f + 0.67f * flFrac; // spec: "bar heights 33-100% of strip"
			const float flBarHeight = flHeightFrac * flHeight;

			const bool bOutlier = flMs > flBaselineMs * 1.5f && flMs > flBaselineMs + 2.0f;
			const ImU32 barColor = bOutlier
				? IM_COL32( 0xF3, 0x82, 0x1D, 204 ) // spec §7 outlier bar: #F3821D @ 80% (0.80*255 = 204)
				: gamescope::palette::Accent( 0.60f ); // spec §7 normal bar: accent @ 60%

			pDrawList->AddRectFilled( ImVec2( flX, flBottom - flBarHeight ), ImVec2( flX + kBarWidth, flBottom ), barColor );
			flX -= kPitch;
		}
	}

	// -------------------------------------------------------------------
	// Issue #29 (System Monitor part 3/3): blend_mode gains "inverted"
	// alongside alpha/additive, and each module (FPS/CPU/GPU/Media) gets an
	// optional per-module colour override. Shared helpers for both below --
	// placed ahead of DrawPercentileRow() since that function (and every
	// module's own Draw*ModuleContent() further down) calls AddTextInverted().
	// -------------------------------------------------------------------

	// A filled backdrop only makes sense in "alpha" mode -- additive
	// already auto-disables it (a filled rect would itself glow, SPEC.md
	// B5) and inverted does too, for the same reason this issue's own text
	// anticipates: a static backdrop fill sits behind the outline/fill
	// treatment AddTextInverted() draws and would just read as visual
	// noise rather than helping legibility (verified below -- see the
	// settings panel's blend-mode combo and this file's own commit
	// message for the explicit "tested together" combination).
	static bool ModuleBackdropAllowed( const config::FpsDisplaySettings &cfg )
	{
		return cfg.blend_mode == "alpha";
	}

	// Packs an ImVec4 (0..1 floats, alpha ignored) into the 0xRRGGBB int
	// config::FpsDisplaySettings::color_fps/cpu/gpu/media store on disk.
	static int PackColorRgb( ImVec4 col )
	{
		auto Channel = []( float f ) { return std::clamp( (int)( f * 255.0f + 0.5f ), 0, 255 ); };
		return ( Channel( col.x ) << 16 ) | ( Channel( col.y ) << 8 ) | Channel( col.z );
	}

	static ImVec4 UnpackColorRgb( int nPacked, float flAlpha = 1.0f )
	{
		return ImVec4(
			( ( nPacked >> 16 ) & 0xFF ) / 255.0f,
			( ( nPacked >> 8 ) & 0xFF ) / 255.0f,
			( nPacked & 0xFF ) / 255.0f,
			flAlpha );
	}

	// Resolves one module's "value" text colour: the user's explicit
	// ColorEdit3 override when set, else `defaultColor` -- a Palette.h
	// accent-family token (never an invented literal, per this issue's own
	// instruction). An unset override therefore moves automatically if
	// issue #37's hue-selectable accent work changes what that Palette.h
	// token resolves to at runtime; a set override is a deliberate,
	// explicit user choice and intentionally does NOT track the accent hue
	// -- see ConfigSchema.h's color_fps/cpu/gpu/media field comment for the
	// full rationale this file was asked to document.
	static ImVec4 ModuleColorVec4( const std::optional<int> &oOverride, ImU32 defaultColor, float flAlpha = 1.0f )
	{
		ImVec4 col = oOverride.has_value() ? UnpackColorRgb( *oOverride ) : gamescope::palette::ToVec4( defaultColor );
		col.w = flAlpha;
		return col;
	}

	static ImU32 ModuleColorU32( const std::optional<int> &oOverride, ImU32 defaultColor, float flAlpha )
	{
		return ImGui::ColorConvertFloat4ToU32( ModuleColorVec4( oOverride, defaultColor, flAlpha ) );
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
	// picture happen. Reaching that would need a fourth blend mode in
	// rendervulkan.hpp's compute shader (this issue's scope note keeps
	// this file out of rendervulkan.hpp/.comp) or safe cross-queue sharing
	// of the app's own texture into this pass (the kind of setup
	// s_pOverlayTexture's own bGeneralQueueShared flag needed for THIS
	// file's texture) -- both genuinely out of this issue's scope, flagged
	// here as the honest follow-up rather than attempted blind.
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
	// legible on its own just reads as noise, exactly as issue #29's own
	// text anticipated for this combination.
	static void AddTextInverted( ImDrawList *pDrawList, ImVec2 pos, const char *pszText )
	{
		static constexpr ImVec2 kOffsets[4] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
		const ImU32 outlineColor = IM_COL32( 0, 0, 0, 235 );
		for ( const ImVec2 &off : kOffsets )
			pDrawList->AddText( ImVec2( pos.x + off.x, pos.y + off.y ), outlineColor, pszText );
		pDrawList->AddText( pos, IM_COL32( 255, 255, 255, 255 ), pszText );
	}

	// Same, for the explicit-font/size AddText overload (FPS module's Hero
	// number, which draws at the user's font_size slider rather than a
	// font style's own baked size).
	static void AddTextInvertedSized( ImDrawList *pDrawList, ImFont *pFont, float flFontSize, ImVec2 pos, const char *pszText )
	{
		static constexpr ImVec2 kOffsets[4] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
		const ImU32 outlineColor = IM_COL32( 0, 0, 0, 235 );
		for ( const ImVec2 &off : kOffsets )
			pDrawList->AddText( pFont, flFontSize, ImVec2( pos.x + off.x, pos.y + off.y ), outlineColor, pszText );
		pDrawList->AddText( pFont, flFontSize, pos, IM_COL32( 255, 255, 255, 255 ), pszText );
	}

	// Spec §10 Row 3: three Mono 400 10 @ 55% white items, 10px gaps. Task
	// brief substitutes "average" for the mockup's temperature slot (a
	// reading this feature has no honest data source for) -- 1% low, 0.1%
	// low, average, all from the same history window as the graph. Draws
	// pre-measured strings (see DrawReadout, which builds these once and
	// reuses the same strings/sizes for both layout and drawing).
	static void DrawPercentileRow( ImDrawList *pDrawList, ImVec2 origin, float flTextOpacity, bool bInverted,
		const char *const ( &items )[3], const ImVec2 ( &sizes )[3] )
	{
		constexpr float kItemGap = 10.0f; // spec §10: "10px gaps"
		const ImU32 color = ImGui::GetColorU32( gamescope::palette::White( 0.55f * flTextOpacity ) ); // spec: "@ 55% white"

		// PushFont, then the 3-arg AddText(pos, col, text) overload (implicit
		// current font/size) -- same pattern as Row 1's unit/ms text below,
		// not pFont->CalcTextSizeA's explicit-size form (that one's reserved
		// for Row 1's Hero number, whose size is the user's font_size slider
		// rather than the Meta style's own baked size).
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		float flX = origin.x;
		for ( int i = 0; i < 3; ++i )
		{
			if ( bInverted )
				AddTextInverted( pDrawList, ImVec2( flX, origin.y ), items[i] );
			else
				pDrawList->AddText( ImVec2( flX, origin.y ), color, items[i] );
			flX += sizes[i].x + kItemGap;
		}
		ImGui::PopFont();
	}

	// Row 1's unit-text run ("FPS"), needed by both the measure and draw
	// halves of the FPS module below.
	static constexpr const char *kUnitText = " FPS";
	// spec §10 row2: "4px above/below gaps" / "18px tall".
	static constexpr float kRowGap = 4.0f;
	static constexpr float kGraphHeight = 18.0f;

	// Shared box backdrop for every module (issue #28: factored out of what
	// was originally DrawFpsModuleContent's own inline block, so the CPU/
	// GPU/Media modules below draw an identical backdrop rather than a
	// second copy of the same four lines). Colours are Palette.h's own
	// tokens/§1 literals -- no new tokens invented, per this issue's own
	// scope note that #29 owns per-module colour customization.
	static void DrawModuleBackdrop( ImDrawList *pDrawList, ImVec2 origin, ImVec2 boxSize, bool bDrawBackdrop, const config::FpsDisplaySettings &cfg )
	{
		if ( !bDrawBackdrop )
			return;

		const ImVec2 rectMin = origin;
		const ImVec2 rectMax( origin.x + boxSize.x, origin.y + boxSize.y );
		// Spec §10: "square corners (radius 0)" is the mockup's own default,
		// but backdrop_rounding stays a real user setting -- see
		// DrawFpsModuleContent's own identical comment.
		const ImU32 backdropColor = ImGui::ColorConvertFloat4ToU32( ImVec4( 0x09 / 255.0f, 0x0b / 255.0f, 0x0e / 255.0f, cfg.backdrop_opacity ) );
		pDrawList->AddRectFilled( rectMin, rectMax, backdropColor, cfg.backdrop_rounding );
		pDrawList->AddRect( rectMin, rectMax, ImGui::GetColorU32( gamescope::palette::White( 0.12f ) ), cfg.backdrop_rounding );
	}

	// -------------------------------------------------------------------
	// Placement: 9 anchor positions (issue #26/#27's shared 3x3 grid
	// model), stored on disk as one of the strings below
	// (ConfigSchema.h's FpsDisplaySettings::placement). This is this
	// file's own copy of the same kPlacements/ParsePlacement shape
	// Notifications.cpp uses for notification_placement -- kept as a
	// separate copy rather than a shared header since Notifications.cpp's
	// own version is a file-local static, not exported, and the two
	// features' placement fields are independently persisted (this one is
	// a normal per-layer field; notification_placement is deliberately
	// global-only -- see that field's own ConfigSchema.h comment).
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
	}

	// -------------------------------------------------------------------
	// Module framework (issue #27): the readout is a fixed sequence of
	// content modules -- FPS, CPU, GPU, Media, in that order (task brief,
	// verbatim) -- each its own backdrop-boxed block, stacked from the
	// selected anchor's edge inward. Issue #28 filled in the CPU/GPU/Media
	// content (search this file for "CPU/GPU/Media modules (issue #28)");
	// every module now reports real content from Metrics/SystemStats.h's
	// background-polled snapshot, following the same
	// Measure<X>Module()/Draw<X>Module() shape MeasureFpsModule()/
	// DrawFpsModuleContent() established. A module still reports zero size
	// from its measure function when its own per-module `enabled` toggle
	// (config::FpsDisplaySettings::cpu_enabled/gpu_enabled/media_enabled)
	// is off -- this framework's contract for "not present" is unchanged:
	// it draws nothing and reserves no stack space or gap.
	//
	// Order is FIXED and EDGE-RELATIVE, not a fixed top-to-bottom screen
	// order: kModuleOrder's first entry (FPS) always ends up the module
	// nearest whichever edge the anchor selects, with later entries
	// (CPU, GPU, Media) stacking further from that edge, inward. This is
	// the literal reading of the task brief's own "coming from the
	// selected edge" phrasing, and it resolves the one placement question
	// the issue explicitly left for the implementer: for a BOTTOM-edge
	// anchor, FPS sits closest to the bottom edge and the stack grows
	// UPWARD from it (CPU above FPS, then GPU, then Media) -- i.e. the
	// on-screen top-to-bottom reading is Media/GPU/CPU/FPS, the mirror
	// image of a top-edge anchor's FPS/CPU/GPU/Media. The alternative
	// (keep FPS visually topmost regardless of anchor) was rejected
	// because it would make "first in the fixed order" mean a different
	// module depending on which edge is picked, which is not what "fixed
	// order coming from the selected edge" describes -- an edge-relative
	// order is the only reading where the rule is the same rule at every
	// anchor. Center-row anchors (center-left/center/center-right) use
	// the task brief's own explicit default for centred views -- "top
	// first" -- so they take the same (unmirrored) order as a top-edge
	// anchor.
	// -------------------------------------------------------------------

	enum class ModuleKind { Fps, Cpu, Gpu, Media, Count };
	static constexpr int kModuleCount = (int)ModuleKind::Count;

	// Fixed stacking order, edge-relative -- see the block comment above.
	static constexpr ModuleKind kModuleOrder[kModuleCount] = {
		ModuleKind::Fps, ModuleKind::Cpu, ModuleKind::Gpu, ModuleKind::Media,
	};

	// Vertical gap between stacked module boxes -- distinct from kRowGap
	// (the gap between a module's own internal rows). Issue #29: was a
	// fixed constant here; now config::FpsDisplaySettings::module_spacing,
	// a real user-facing slider (this issue's own "at least one new
	// styling option" acceptance criterion) -- see DrawReadout(), which
	// reads cfg.module_spacing directly at both of this constant's old use
	// sites.

	// FPS module's measured layout: every string/size the draw half needs,
	// computed once by MeasureFpsModule() and consumed by
	// DrawFpsModuleContent() -- split so the module framework can learn
	// this module's size (for stacking) before it has an origin to draw
	// at.
	struct FpsModuleLayout
	{
		bool bAdditive = false;
		bool bInverted = false;
		bool bDrawBackdrop = false;
		ImU32 textColor = 0;
		char szNum[8] = "";
		ImVec2 numSize{};
		ImVec2 unitSize{};
		char szMs[16] = "";
		ImVec2 msSize{};
		ImVec2 textSize{};
		bool bShowGraph = false;
		bool bShowPercentiles = false;
		char szOnePct[24] = "";
		char szPointOnePct[24] = "";
		char szAvg[24] = "";
		ImVec2 percentileSizes[3] = {};
		float flPercentileRowWidth = 0.0f;
		float flPercentileRowHeight = 0.0f;
		float flContentWidth = 0.0f;
		float flContentHeight = 0.0f;
	};

	// M8 part 1 (issue #13): IBM Plex Mono is genuinely monospaced, so a
	// fixed-width formatted string ("%3d FPS") is tabular by construction
	// -- every digit occupies the same advance width, so the readout
	// cannot jitter horizontally as the number changes. This replaces the
	// former DrawTabularInt() helper, which existed only to fake that
	// property (per-glyph draws at a hand-measured pitch) before a real
	// tabular-figures font existed; now that one does, the workaround is
	// gone. Issue #27: any future module (#28's CPU/GPU/Media) showing a
	// number must follow this same fixed-width-field convention.
	static FpsModuleLayout MeasureFpsModule( int nFps )
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		FpsModuleLayout L;

		L.bAdditive = cfg.blend_mode == "additive";
		L.bInverted = cfg.blend_mode == "inverted";
		// additive/inverted + a filled backdrop rect would make the
		// backdrop itself glow (additive, SPEC.md B5) or just read as
		// noise behind an already-legible outline/fill pair (inverted) --
		// auto-disable rather than combine them, matching the settings
		// panel's own auto-disable of the backdrop controls in either mode
		// (FpsDisplay_DrawSettingsPanel).
		L.bDrawBackdrop = cfg.backdrop_enabled && ModuleBackdropAllowed( cfg );

		// Gamescope's own layer blend modes (rendervulkan.hpp) are
		// PREMULTIPLIED/COVERAGE/NONE -- there is no whole-layer additive
		// mode to hand this off to. "Additive" here is approximated at
		// the draw-list level (no backdrop, brighter/accent-tinted text)
		// rather than literal GPU ADD blend-func compositing against the
		// scene -- SPEC.md flags this exact interaction as a design-guide
		// call, not a technical one. "Inverted" draws via AddTextInverted()
		// instead (see that function's own comment) rather than a flat
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
		const float flFontSize = cfg.font_size; // still user-configurable (M4's own font-size slider) -- ImGui scales the baked Hero glyphs to whatever size is requested, same mechanism the pre-M8 code already relied on
		L.numSize = pFont->CalcTextSizeA( flFontSize, FLT_MAX, 0.0f, L.szNum );

		// Spec §10 Row 1: number (Hero) -> "FPS" unit (Meta, dim) -> spacer
		// -> frametime in ms (accent-tinted) -- was one flat "%3d FPS" run
		// in a single color/size; split so the unit and the ms readout can
		// each carry their own spec'd size/color (gap list item 6).
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		L.unitSize = ImGui::CalcTextSize( kUnitText );
		ImGui::PopFont();

		snprintf( L.szMs, sizeof( L.szMs ), "  %.1fms", s_flSmoothedFrametimeMs );
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		L.msSize = L.bAdditive ? ImVec2( 0.0f, 0.0f ) : ImGui::CalcTextSize( L.szMs );
		ImGui::PopFont();

		L.textSize = ImVec2( L.numSize.x + L.unitSize.x + L.msSize.x, std::max( L.numSize.y, std::max( L.unitSize.y, L.msSize.y ) ) );

		// Row 2 (graph) / Row 3 (percentiles): both independently toggleable
		// (spec §11's "ROWS checkbox list"), reusing Row 1's font_size/
		// backdrop/blend_mode/text_opacity rather than a second set of
		// per-row settings.
		L.bShowGraph = cfg.graph_enabled;
		L.bShowPercentiles = cfg.percentiles_enabled;

		if ( L.bShowPercentiles )
		{
			// %3d, same reasoning as szNum above: fixed-width digits so
			// neither an individual number nor the row's total width
			// jitters as a value crosses a digit boundary (task brief:
			// "digits do not jitter").
			snprintf( L.szOnePct, sizeof( L.szOnePct ), "1%% %3d", std::clamp( (int)std::lround( s_flOnePercentLowFps ), 0, 999 ) );
			snprintf( L.szPointOnePct, sizeof( L.szPointOnePct ), "0.1%% %3d", std::clamp( (int)std::lround( s_flPointOnePercentLowFps ), 0, 999 ) );
			snprintf( L.szAvg, sizeof( L.szAvg ), "avg %3d", std::clamp( (int)std::lround( s_flAverageFps ), 0, 999 ) );

			const char *const percentileItems[3] = { L.szOnePct, L.szPointOnePct, L.szAvg };
			constexpr float kItemGap = 10.0f; // spec §10 row3: "10px gaps"
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			for ( int i = 0; i < 3; ++i )
			{
				L.percentileSizes[i] = ImGui::CalcTextSize( percentileItems[i] );
				L.flPercentileRowHeight = std::max( L.flPercentileRowHeight, L.percentileSizes[i].y );
			}
			ImGui::PopFont();
			L.flPercentileRowWidth = L.percentileSizes[0].x + kItemGap + L.percentileSizes[1].x + kItemGap + L.percentileSizes[2].x;
		}

		// Spec §10: "container ... min-width 186px" -- convert to a content-
		// width floor using whatever padding is actually configured (rather
		// than assuming the spec's own fixed 8px/11px split, which this
		// readout already collapses to one symmetric backdrop_padding value)
		// so the HUD still reads as "at least this wide" regardless of the
		// padding slider.
		constexpr float kMinContainerWidth = 186.0f;
		const float flContentWidthFloor = kMinContainerWidth - cfg.backdrop_padding * 2.0f;
		L.flContentWidth = std::max( { L.textSize.x, L.flPercentileRowWidth, flContentWidthFloor } );

		L.flContentHeight = L.textSize.y;
		if ( L.bShowGraph )
			L.flContentHeight += kRowGap + kGraphHeight;
		if ( L.bShowPercentiles )
			L.flContentHeight += kRowGap + L.flPercentileRowHeight;

		return L;
	}

	// Draws the FPS module's backdrop + content into the box
	// [origin, origin+boxSize) -- boxSize is exactly what MeasureFpsModule()
	// implied (content size + 2*backdrop_padding), computed by MeasureModule().
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

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		const ImVec2 unitPos( cursor.x, cursor.y + ( L.numSize.y - L.unitSize.y ) );
		if ( L.bInverted )
			AddTextInverted( pDrawList, unitPos, kUnitText );
		else
			pDrawList->AddText( unitPos, ImGui::GetColorU32( gamescope::palette::White( 0.50f ) ), kUnitText );
		cursor.x += L.unitSize.x;
		ImGui::PopFont();

		if ( !L.bAdditive )
		{
			// Spec §10: frametime readout color oklch(.86 .09 218) = #89E0F8
			// -- close to, but distinct from, the general accent-value token
			// (#78DBF6), so it's kept as its own literal rather than routed
			// through Palette.h's accent family.
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
			const ImVec2 msPos( cursor.x, cursor.y + ( L.numSize.y - L.msSize.y ) );
			if ( L.bInverted )
				AddTextInverted( pDrawList, msPos, L.szMs );
			else
				pDrawList->AddText( msPos, ImGui::GetColorU32( ImVec4( 0x89 / 255.0f, 0xe0 / 255.0f, 0xf8 / 255.0f, cfg.text_opacity ) ), L.szMs );
			ImGui::PopFont();
		}

		float flCursorY = textPos.y + L.textSize.y;
		if ( L.bShowGraph )
		{
			flCursorY += kRowGap;
			DrawFrametimeGraph( pDrawList, ImVec2( textPos.x, flCursorY ), L.flContentWidth, kGraphHeight );
			flCursorY += kGraphHeight;
		}
		if ( L.bShowPercentiles )
		{
			flCursorY += kRowGap;
			const char *const percentileItems[3] = { L.szOnePct, L.szPointOnePct, L.szAvg };
			DrawPercentileRow( pDrawList, ImVec2( textPos.x, flCursorY ), cfg.text_opacity, L.bInverted, percentileItems, L.percentileSizes );
		}
	}

	// -------------------------------------------------------------------
	// CPU/GPU/Media modules (issue #28) -- fill in the stubs #27 left.
	// Same measure/draw split as the FPS module above: a Measure*Module()
	// call builds every string/size the draw half needs (so the framework
	// can learn a module's box size before it has an origin to draw at),
	// and a Draw*ModuleContent() draws into [origin, origin+boxSize).
	//
	// Values come from Metrics::Get*State() (Metrics/SystemStats.h) -- a
	// cheap mutex-guarded copy of a background thread's last poll, never a
	// direct sysfs/proc read or `playerctl` spawn on this (steamcompmgr)
	// thread. See that header's own comment for the full threading
	// contract and data-source rationale (verified live on this machine's
	// AMD RX 7900 XTX for the GPU sysfs paths).
	//
	// Every numeric field below is drawn through Fonts::Style::Value/Meta
	// (both IBM Plex Mono, genuinely monospaced) at a fixed printf field
	// width, the same "digits do not jitter" convention MeasureFpsModule's
	// own comment documents -- e.g. "%3d%%" for GPU busy, not "%d%%".
	// -------------------------------------------------------------------

	// ---- CPU/RAM module -------------------------------------------------

	struct CpuModuleLayout
	{
		bool bDrawBackdrop = false;
		char szLoadValue[16] = "";  // e.g. " 1.23" or "  n/a"
		char szRamValue[24] = "";   // e.g. " 6.1/31.9GB" or " n/a"
		ImVec2 loadLabelSize{}, loadValueSize{}, ramLabelSize{}, ramValueSize{};
		float flContentWidth = 0.0f;
		float flContentHeight = 0.0f;
	};

	static constexpr const char *kCpuLoadLabel = "CPU";
	static constexpr const char *kCpuRamLabel = "  RAM";

	static CpuModuleLayout MeasureCpuModule()
	{
		CpuModuleLayout L;
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		L.bDrawBackdrop = cfg.backdrop_enabled && ModuleBackdropAllowed( cfg );

		const gamescope::Metrics::CpuState cpu = gamescope::Metrics::GetCpuState();

		// Fixed-width fields (see this section's header comment) -- a
		// missing /proc/loadavg or /proc/meminfo read (this issue's own
		// "degrade gracefully" ask, though far less likely than a missing
		// amdgpu node) reads as "n/a" in the same field width rather than
		// a fabricated 0.
		if ( cpu.bLoadAvailable )
			snprintf( L.szLoadValue, sizeof( L.szLoadValue ), " %4.2f", cpu.flLoad1 );
		else
			snprintf( L.szLoadValue, sizeof( L.szLoadValue ), "  n/a" );

		if ( cpu.bMemAvailable )
		{
			const float flUsedGb = (float)cpu.ulMemUsedKb / ( 1024.0f * 1024.0f );
			const float flTotalGb = (float)cpu.ulMemTotalKb / ( 1024.0f * 1024.0f );
			snprintf( L.szRamValue, sizeof( L.szRamValue ), " %4.1f/%4.1fGB", flUsedGb, flTotalGb );
		}
		else
		{
			snprintf( L.szRamValue, sizeof( L.szRamValue ), " n/a" );
		}

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		L.loadLabelSize = ImGui::CalcTextSize( kCpuLoadLabel );
		L.ramLabelSize = ImGui::CalcTextSize( kCpuRamLabel );
		ImGui::PopFont();

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		L.loadValueSize = ImGui::CalcTextSize( L.szLoadValue );
		L.ramValueSize = ImGui::CalcTextSize( L.szRamValue );
		ImGui::PopFont();

		L.flContentWidth = L.loadLabelSize.x + L.loadValueSize.x + L.ramLabelSize.x + L.ramValueSize.x;
		L.flContentHeight = std::max( { L.loadLabelSize.y, L.loadValueSize.y, L.ramLabelSize.y, L.ramValueSize.y } );
		return L;
	}

	static void DrawCpuModuleContent( ImDrawList *pDrawList, ImVec2 origin, ImVec2 boxSize, const CpuModuleLayout &L )
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		DrawModuleBackdrop( pDrawList, origin, boxSize, L.bDrawBackdrop, cfg );

		const ImVec2 textPos( origin.x + cfg.backdrop_padding, origin.y + cfg.backdrop_padding );
		const bool bInverted = cfg.blend_mode == "inverted";
		const ImU32 labelColor = ImGui::GetColorU32( gamescope::palette::White( 0.55f * cfg.text_opacity ) ); // matches Row 3's own "55% white" meta treatment
		const ImU32 valueColor = ModuleColorU32( cfg.color_cpu, gamescope::palette::kAccentIcon, cfg.text_opacity );

		ImVec2 cursor = textPos;
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, kCpuLoadLabel ); else pDrawList->AddText( cursor, labelColor, kCpuLoadLabel );
		ImGui::PopFont();
		cursor.x += L.loadLabelSize.x;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, L.szLoadValue ); else pDrawList->AddText( cursor, valueColor, L.szLoadValue );
		ImGui::PopFont();
		cursor.x += L.loadValueSize.x;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, kCpuRamLabel ); else pDrawList->AddText( cursor, labelColor, kCpuRamLabel );
		ImGui::PopFont();
		cursor.x += L.ramLabelSize.x;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, L.szRamValue ); else pDrawList->AddText( cursor, valueColor, L.szRamValue );
		ImGui::PopFont();
	}

	// ---- GPU module -------------------------------------------------------

	struct GpuModuleLayout
	{
		bool bDrawBackdrop = false;
		bool bGpuFound = false;
		char szUnavailableLine[40] = "";
		char szBusyValue[8] = "";     // " 63%"
		char szVramValue[24] = "";    // " 12.1/24.0GB"
		char szSensorsLine[40] = "";  // " 63.0C   96.0W" or "sensors unavailable"
		ImVec2 unavailableSize{};
		ImVec2 busyLabelSize{}, busyValueSize{}, vramLabelSize{}, vramValueSize{}, sensorsLineSize{};
		float flRow1Height = 0.0f;
		float flContentWidth = 0.0f;
		float flContentHeight = 0.0f;
	};

	static constexpr const char *kGpuBusyLabel = "GPU";
	static constexpr const char *kGpuVramLabel = "  VRAM";

	static GpuModuleLayout MeasureGpuModule()
	{
		GpuModuleLayout L;
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		L.bDrawBackdrop = cfg.backdrop_enabled && ModuleBackdropAllowed( cfg );

		const gamescope::Metrics::GpuState gpu = gamescope::Metrics::GetGpuState();
		L.bGpuFound = gpu.bGpuFound;

		if ( !gpu.bGpuFound )
		{
			// No amdgpu DRM device found this session -- non-AMD hardware,
			// or an AMD card whose driver hasn't bound. Honest
			// "unavailable" line rather than a fabricated 0% or a crash on
			// a missing sysfs file -- this issue's own explicit
			// requirement (this machine is AMD; NVIDIA/Intel were never
			// testable here, so this path is the honest fallback for them).
			snprintf( L.szUnavailableLine, sizeof( L.szUnavailableLine ), "GPU  unavailable (no amdgpu)" );
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			L.unavailableSize = ImGui::CalcTextSize( L.szUnavailableLine );
			ImGui::PopFont();
			L.flContentWidth = L.unavailableSize.x;
			L.flContentHeight = L.unavailableSize.y;
			return L;
		}

		snprintf( L.szBusyValue, sizeof( L.szBusyValue ), " %3d%%", gpu.nBusyPercent );
		const float flUsedGb = (float)gpu.ulVramUsedBytes / ( 1024.0f * 1024.0f * 1024.0f );
		const float flTotalGb = (float)gpu.ulVramTotalBytes / ( 1024.0f * 1024.0f * 1024.0f );
		snprintf( L.szVramValue, sizeof( L.szVramValue ), " %4.1f/%4.1fGB", flUsedGb, flTotalGb );

		if ( gpu.bHwmonFound )
			snprintf( L.szSensorsLine, sizeof( L.szSensorsLine ), "%5.1fC   %5.1fW", gpu.flTempC, gpu.flPowerWatts );
		else
			snprintf( L.szSensorsLine, sizeof( L.szSensorsLine ), "sensors unavailable" ); // amdgpu DRM node found, but no matching hwmon node -- temp/power specifically unavailable

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		L.busyLabelSize = ImGui::CalcTextSize( kGpuBusyLabel );
		L.vramLabelSize = ImGui::CalcTextSize( kGpuVramLabel );
		L.sensorsLineSize = ImGui::CalcTextSize( L.szSensorsLine );
		ImGui::PopFont();

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		L.busyValueSize = ImGui::CalcTextSize( L.szBusyValue );
		L.vramValueSize = ImGui::CalcTextSize( L.szVramValue );
		ImGui::PopFont();

		const float flRow1Width = L.busyLabelSize.x + L.busyValueSize.x + L.vramLabelSize.x + L.vramValueSize.x;
		L.flRow1Height = std::max( { L.busyLabelSize.y, L.busyValueSize.y, L.vramLabelSize.y, L.vramValueSize.y } );
		L.flContentWidth = std::max( flRow1Width, L.sensorsLineSize.x );
		L.flContentHeight = L.flRow1Height + kRowGap + L.sensorsLineSize.y;
		return L;
	}

	static void DrawGpuModuleContent( ImDrawList *pDrawList, ImVec2 origin, ImVec2 boxSize, const GpuModuleLayout &L )
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		DrawModuleBackdrop( pDrawList, origin, boxSize, L.bDrawBackdrop, cfg );

		const ImVec2 textPos( origin.x + cfg.backdrop_padding, origin.y + cfg.backdrop_padding );
		const bool bInverted = cfg.blend_mode == "inverted";
		const ImU32 labelColor = ImGui::GetColorU32( gamescope::palette::White( 0.55f * cfg.text_opacity ) );
		const ImU32 valueColor = ModuleColorU32( cfg.color_gpu, gamescope::palette::kAccentKnob, cfg.text_opacity );

		if ( !L.bGpuFound )
		{
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			if ( bInverted ) AddTextInverted( pDrawList, textPos, L.szUnavailableLine ); else pDrawList->AddText( textPos, labelColor, L.szUnavailableLine );
			ImGui::PopFont();
			return;
		}

		ImVec2 cursor = textPos;
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, kGpuBusyLabel ); else pDrawList->AddText( cursor, labelColor, kGpuBusyLabel );
		ImGui::PopFont();
		cursor.x += L.busyLabelSize.x;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, L.szBusyValue ); else pDrawList->AddText( cursor, valueColor, L.szBusyValue );
		ImGui::PopFont();
		cursor.x += L.busyValueSize.x;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, kGpuVramLabel ); else pDrawList->AddText( cursor, labelColor, kGpuVramLabel );
		ImGui::PopFont();
		cursor.x += L.vramLabelSize.x;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		if ( bInverted ) AddTextInverted( pDrawList, cursor, L.szVramValue ); else pDrawList->AddText( cursor, valueColor, L.szVramValue );
		ImGui::PopFont();

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		const ImVec2 sensorsPos( textPos.x, textPos.y + L.flRow1Height + kRowGap );
		if ( bInverted ) AddTextInverted( pDrawList, sensorsPos, L.szSensorsLine ); else pDrawList->AddText( sensorsPos, labelColor, L.szSensorsLine );
		ImGui::PopFont();
	}

	// ---- Media module -------------------------------------------------------

	// Byte-truncation, not UTF-8-aware: acceptable here since this only
	// clips an already-rare long title/artist string for display width,
	// and a clipped multibyte glyph at the very tail is a cosmetic edge
	// case (the font atlas renders whatever bytes remain, at worst one
	// stray/missing glyph), not a correctness one.
	static std::string TruncateForDisplay( const std::string &s, size_t nMaxLen )
	{
		if ( s.size() <= nMaxLen )
			return s;
		return s.substr( 0, nMaxLen ) + "...";
	}

	struct MediaModuleLayout
	{
		bool bDrawBackdrop = false;
		bool bPlayerAvailable = false;
		char szStatusLine[24] = "";
		char szTrackLine[160] = "";
		ImVec2 statusSize{}, trackSize{};
		float flContentWidth = 0.0f;
		float flContentHeight = 0.0f;
	};

	static MediaModuleLayout MeasureMediaModule()
	{
		MediaModuleLayout L;
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		L.bDrawBackdrop = cfg.backdrop_enabled && ModuleBackdropAllowed( cfg );

		const gamescope::Metrics::MediaState media = gamescope::Metrics::GetMediaState();
		L.bPlayerAvailable = media.bPlayerAvailable;

		if ( !media.bPlayerAvailable )
		{
			// No MPRIS player currently open (playerctl missing, or simply
			// nothing playing) -- an honestly-empty module, not an error,
			// per this issue's own acceptance criterion.
			snprintf( L.szStatusLine, sizeof( L.szStatusLine ), "no media playing" );
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			L.statusSize = ImGui::CalcTextSize( L.szStatusLine );
			ImGui::PopFont();
			L.flContentWidth = L.statusSize.x;
			L.flContentHeight = L.statusSize.y;
			return L;
		}

		const char *pszStatusWord = "media";
		switch ( media.eStatus )
		{
		case gamescope::Metrics::PlaybackStatus::Playing: pszStatusWord = "playing"; break;
		case gamescope::Metrics::PlaybackStatus::Paused:  pszStatusWord = "paused";  break;
		case gamescope::Metrics::PlaybackStatus::Stopped: pszStatusWord = "stopped"; break;
		default: break;
		}
		snprintf( L.szStatusLine, sizeof( L.szStatusLine ), "MEDIA  %s", pszStatusWord );

		const std::string sTitle = TruncateForDisplay( media.sTitle.empty() ? "(unknown title)" : media.sTitle, 40 );
		const std::string sTrack = media.sArtist.empty() ? sTitle : ( sTitle + "  -  " + TruncateForDisplay( media.sArtist, 24 ) );
		snprintf( L.szTrackLine, sizeof( L.szTrackLine ), "%s", sTrack.c_str() );

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		L.statusSize = ImGui::CalcTextSize( L.szStatusLine );
		ImGui::PopFont();

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		L.trackSize = ImGui::CalcTextSize( L.szTrackLine );
		ImGui::PopFont();

		L.flContentWidth = std::max( L.statusSize.x, L.trackSize.x );
		L.flContentHeight = L.statusSize.y + kRowGap + L.trackSize.y;
		return L;
	}

	static void DrawMediaModuleContent( ImDrawList *pDrawList, ImVec2 origin, ImVec2 boxSize, const MediaModuleLayout &L )
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		DrawModuleBackdrop( pDrawList, origin, boxSize, L.bDrawBackdrop, cfg );

		const ImVec2 textPos( origin.x + cfg.backdrop_padding, origin.y + cfg.backdrop_padding );
		const bool bInverted = cfg.blend_mode == "inverted";
		const ImU32 labelColor = ImGui::GetColorU32( gamescope::palette::White( 0.55f * cfg.text_opacity ) );
		const ImU32 valueColor = ModuleColorU32( cfg.color_media, gamescope::palette::kAccentHandle, cfg.text_opacity );

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		if ( bInverted ) AddTextInverted( pDrawList, textPos, L.szStatusLine ); else pDrawList->AddText( textPos, labelColor, L.szStatusLine );
		ImGui::PopFont();

		if ( !L.bPlayerAvailable )
			return;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		const ImVec2 trackPos( textPos.x, textPos.y + L.statusSize.y + kRowGap );
		if ( bInverted ) AddTextInverted( pDrawList, trackPos, L.szTrackLine ); else pDrawList->AddText( trackPos, valueColor, L.szTrackLine );
		ImGui::PopFont();
	}

	// Measures one module in stacking order, returning its full box size
	// (content + 2*backdrop_padding on each axis, matching what each
	// Draw*ModuleContent() above expects as boxSize) -- (0,0) means "not
	// present," this framework's contract for a module with no content
	// (still used by any module whose own `enabled` toggle is off -- see
	// DrawReadout()'s per-module gating below).
	static ImVec2 MeasureModule( ModuleKind kind, int nFps, FpsModuleLayout &outFpsLayout, CpuModuleLayout &outCpuLayout, GpuModuleLayout &outGpuLayout, MediaModuleLayout &outMediaLayout )
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;
		switch ( kind )
		{
		case ModuleKind::Fps:
		{
			outFpsLayout = MeasureFpsModule( nFps );
			return ImVec2( outFpsLayout.flContentWidth + cfg.backdrop_padding * 2.0f, outFpsLayout.flContentHeight + cfg.backdrop_padding * 2.0f );
		}
		case ModuleKind::Cpu:
		{
			if ( !cfg.cpu_enabled )
				return ImVec2( 0.0f, 0.0f );
			outCpuLayout = MeasureCpuModule();
			return ImVec2( outCpuLayout.flContentWidth + cfg.backdrop_padding * 2.0f, outCpuLayout.flContentHeight + cfg.backdrop_padding * 2.0f );
		}
		case ModuleKind::Gpu:
		{
			if ( !cfg.gpu_enabled )
				return ImVec2( 0.0f, 0.0f );
			outGpuLayout = MeasureGpuModule();
			return ImVec2( outGpuLayout.flContentWidth + cfg.backdrop_padding * 2.0f, outGpuLayout.flContentHeight + cfg.backdrop_padding * 2.0f );
		}
		case ModuleKind::Media:
		{
			if ( !cfg.media_enabled )
				return ImVec2( 0.0f, 0.0f );
			outMediaLayout = MeasureMediaModule();
			return ImVec2( outMediaLayout.flContentWidth + cfg.backdrop_padding * 2.0f, outMediaLayout.flContentHeight + cfg.backdrop_padding * 2.0f );
		}
		default:
			return ImVec2( 0.0f, 0.0f );
		}
	}

	static void DrawModule( ModuleKind kind, ImDrawList *pDrawList, ImVec2 origin, ImVec2 boxSize, const FpsModuleLayout &fpsLayout, const CpuModuleLayout &cpuLayout, const GpuModuleLayout &gpuLayout, const MediaModuleLayout &mediaLayout )
	{
		switch ( kind )
		{
		case ModuleKind::Fps:
			DrawFpsModuleContent( pDrawList, origin, boxSize, fpsLayout );
			break;
		case ModuleKind::Cpu:
			DrawCpuModuleContent( pDrawList, origin, boxSize, cpuLayout );
			break;
		case ModuleKind::Gpu:
			DrawGpuModuleContent( pDrawList, origin, boxSize, gpuLayout );
			break;
		case ModuleKind::Media:
			DrawMediaModuleContent( pDrawList, origin, boxSize, mediaLayout );
			break;
		default:
			break;
		}
	}

	static void DrawReadout()
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;

		const int nFps = (int)std::lround( UpdateAndGetSmoothedFps() );
		RecomputePercentilesIfDue( get_time_in_nanos() );

		ImDrawList *pDrawList = ImGui::GetBackgroundDrawList();

		int nVert = 0, nHoriz = 2;
		ParsePlacement( cfg.placement, nVert, nHoriz );

		// Edge-relative stacking order -- see kModuleOrder's block comment
		// for the bottom-edge mirroring rationale. nVert: 0=top, 1=center,
		// 2=bottom (ParsePlacement's convention, matching kPlacements'
		// row order above).
		ModuleKind order[kModuleCount];
		if ( nVert == 2 ) // bottom edge: mirrored so FPS ends up nearest it
		{
			for ( int i = 0; i < kModuleCount; ++i )
				order[i] = kModuleOrder[kModuleCount - 1 - i];
		}
		else // top edge, or centre row defaulting to the top-first reading
		{
			for ( int i = 0; i < kModuleCount; ++i )
				order[i] = kModuleOrder[i];
		}

		// Pass 1: measure every module in stacking order. Every module now
		// has real content (issue #28) -- a module still reports zero size
		// (this framework's own "not present" contract) when its own
		// `enabled` toggle is off, exactly like a module with no content
		// would have before this issue.
		FpsModuleLayout fpsLayout;
		CpuModuleLayout cpuLayout;
		GpuModuleLayout gpuLayout;
		MediaModuleLayout mediaLayout;
		ImVec2 sizes[kModuleCount];
		float flStackWidth = 0.0f;
		float flStackHeight = 0.0f;
		int nPresent = 0;
		for ( int i = 0; i < kModuleCount; ++i )
		{
			sizes[i] = MeasureModule( order[i], nFps, fpsLayout, cpuLayout, gpuLayout, mediaLayout );
			const bool bPresent = sizes[i].x > 0.0f || sizes[i].y > 0.0f;
			if ( !bPresent )
				continue;
			flStackWidth = std::max( flStackWidth, sizes[i].x );
			if ( nPresent > 0 )
				flStackHeight += cfg.module_spacing;
			flStackHeight += sizes[i].y;
			++nPresent;
		}

		if ( nPresent == 0 )
			return; // nothing to draw (shouldn't happen while fps_display.enabled, but safe)

		// Anchor the whole stack (all modules share one column, width =
		// widest present module) against the selected 3x3 cell, offset by
		// the independent vertical/horizontal margins -- replaces the old
		// hardcoded top-right kAnchorOffset=32 constant. This context's
		// io.DisplaySize is the actual output resolution (see
		// FpsDisplay_AddLayer()).
		const ImVec2 io_display = ImGui::GetIO().DisplaySize;

		const float flX = ( nHoriz == 0 ) ? cfg.margin_horizontal
			: ( nHoriz == 1 ) ? ( io_display.x - flStackWidth ) * 0.5f
			: ( io_display.x - cfg.margin_horizontal - flStackWidth );
		const float flStartY = ( nVert == 0 ) ? cfg.margin_vertical
			: ( nVert == 1 ) ? ( io_display.y - flStackHeight ) * 0.5f
			: ( io_display.y - cfg.margin_vertical - flStackHeight );

		// Clamp so a large margin (or, once #28 lands, several stacked
		// modules) can never push the stack off-screen -- issue #27's own
		// verification ask ("does not clip off-screen at any anchor").
		const float flClampedX = std::clamp( flX, 0.0f, std::max( 0.0f, io_display.x - flStackWidth ) );
		const float flClampedY = std::clamp( flStartY, 0.0f, std::max( 0.0f, io_display.y - flStackHeight ) );

		// Pass 2: draw each present module at its stacked position.
		float flCursorY = flClampedY;
		for ( int i = 0; i < kModuleCount; ++i )
		{
			const bool bPresent = sizes[i].x > 0.0f || sizes[i].y > 0.0f;
			if ( !bPresent )
				continue;
			DrawModule( order[i], pDrawList, ImVec2( flClampedX, flCursorY ), sizes[i], fpsLayout, cpuLayout, gpuLayout, mediaLayout );
			flCursorY += sizes[i].y + cfg.module_spacing;
		}
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

		// Issue #28: starts the CPU/GPU/media background poll thread
		// (Metrics/SystemStats.h -- mirrors Audio::Init()'s own
		// call-unconditionally-every-frame, only-first-call-has-effect
		// shape). Cheap: an atomic exchange check once the thread is up.
		// Started here rather than gated on `enabled` below so the first
		// numbers are already warm by the time a user opens the panel and
		// turns the readout on, rather than starting cold.
		gamescope::Metrics::Init();

		if ( !s_Settings.fps_display.enabled )
			return;

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

	// Stock ImGui::SliderFloat() sizes its frame via CalcItemWidth(), which
	// defaults to 65% of the window width (imgui.cpp's
	// window->DC.ItemWidthDefault) and leaves the rest of the row for the
	// trailing label -- so every slider below stopped two-thirds of the
	// way across the panel with no override (reported: "sliders are not
	// spanning the full width"). Size the frame explicitly instead: full
	// row width minus exactly what the label needs, so frame+label
	// together reach the row's right edge -- same "row spans full width,
	// track fills what the label/value doesn't need" fix
	// widgets::SliderControl() applies for its own sliders (Widgets.cpp);
	// this file's sliders are plain stock ImGui rather than that custom
	// widget (see this function's own header comment), so the fix has to
	// be applied per call site here instead of in one shared place.
	static void SetStockSliderFullWidth( const char *pszLabel )
	{
		const ImGuiStyle &style = ImGui::GetStyle();
		const float flLabelW = ImGui::CalcTextSize( pszLabel ).x;
		const float flGap = flLabelW > 0.0f ? style.ItemInnerSpacing.x : 0.0f;
		ImGui::SetNextItemWidth( std::max( 1.0f, ImGui::GetContentRegionAvail().x - flLabelW - flGap ) );
	}

	// Issue #29: one module's optional colour override -- a checkbox to
	// toggle the override on/off (ColorEdit3 alone has no "unset" concept)
	// plus, while on, a stock ImGui::ColorEdit3 (this issue's own
	// acceptance criterion: "no custom widget needed here"). Unchecked
	// shows/edits `defaultColor` (the Palette.h accent token this module
	// falls back to) as a preview only -- it never gets written back to
	// `oColor` unless the checkbox is actually on, so leaving it alone
	// keeps the module tracking Palette.h (and, transitively, #37's
	// accent-hue selection) exactly as ConfigSchema.h's field comment
	// promises.
	static bool DrawModuleColorPicker( const char *pszLabel, std::optional<int> &oColor, ImU32 defaultColor )
	{
		bool bChanged = false;
		bool bCustom = oColor.has_value();
		char szCheckLabel[32];
		snprintf( szCheckLabel, sizeof( szCheckLabel ), "Custom %s colour", pszLabel );
		if ( widgets::Checkbox( szCheckLabel, &bCustom ) )
		{
			oColor = bCustom ? std::optional<int>( PackColorRgb( gamescope::palette::ToVec4( defaultColor ) ) ) : std::nullopt;
			bChanged = true;
		}
		if ( bCustom )
		{
			ImVec4 col = ModuleColorVec4( oColor, defaultColor );
			float rgb[3] = { col.x, col.y, col.z };
			ImGui::SameLine();
			if ( ImGui::ColorEdit3( pszLabel, rgb, ImGuiColorEditFlags_NoInputs ) )
			{
				oColor = PackColorRgb( ImVec4( rgb[0], rgb[1], rgb[2], 1.0f ) );
				bChanged = true;
			}
		}
		return bChanged;
	}

	void FpsDisplay_DrawSettingsPanel()
	{
		EnsureConfigLoaded();
		config::FpsDisplaySettings &cfg = s_Settings.fps_display;

		ImGui::Spacing();
		ImGui::Separator();
		// Section role (Sans 500). This draws inside SettingsOverlay's own
		// ImGui context/atlas (see this function's declared contract in
		// FpsDisplay.h), not this file's own separate FPS-readout context,
		// so it's safe to pull a font from gamescope::fonts::Get() here --
		// SettingsOverlay.cpp's EnsureImguiInit() already called Load() on
		// that context before any panel draws.
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Section ) );
		// Issue #27: renamed from "FPS display (M4)" -- this panel now
		// hosts the whole module framework (FPS today; #28 adds CPU/GPU/
		// Media), not just the FPS readout, so "System Monitor" is the
		// name that actually describes it. File/function names stay
		// FpsDisplay* internally (this file's own header comment) to
		// avoid churning M4's existing surface for a rename that gains
		// nothing outside the UI-visible strings.
		ImGui::TextUnformatted( "System Monitor" );
		ImGui::PopFont();

		bool bChanged = false;
		// Widgets::Checkbox, not ::Toggle: this settings block IS the design
		// guide's own named example of the "List rows" checkbox-row pattern
		// ("FPS HUD's row toggles: 11x11 checkbox + label") -- see Widgets.h's
		// Checkbox() comment.
		bChanged |= widgets::Checkbox( "Show FPS counter", &cfg.enabled );

		ImGui::BeginDisabled( !cfg.enabled );

		// Issue #27: 3x3 placement grid + independent margins, replacing
		// the old hardcoded top-right/32px constant. widgets::PositionGrid
		// is issue #26's shared widget (Widgets.h/.cpp) -- reused verbatim
		// rather than forking a second grid control, per the coordinator's
		// note: same 9-cell model this file's ParsePlacement/ComposePlacement
		// already speak (kPlacements above).
		ImGui::Spacing();
		ImGui::TextUnformatted( "Placement" );
		int nVert = 0, nHoriz = 2;
		ParsePlacement( cfg.placement, nVert, nHoriz );
		if ( widgets::PositionGrid( "##SysMonPlacement", &nVert, &nHoriz ) )
		{
			cfg.placement = ComposePlacement( nVert, nHoriz );
			bChanged = true;
		}

		SetStockSliderFullWidth( "Vertical margin" );
		bChanged |= ImGui::SliderFloat( "Vertical margin", &cfg.margin_vertical, 0.0f, 128.0f, "%.0f px" );
		SetStockSliderFullWidth( "Horizontal margin" );
		bChanged |= ImGui::SliderFloat( "Horizontal margin", &cfg.margin_horizontal, 0.0f, 128.0f, "%.0f px" );
		ImGui::Spacing();

		// Spec §11's "ROWS checkbox list" -- Row 2 (frametime graph) / Row 3
		// (percentile stats), independently toggleable, sharing every other
		// setting on this panel (font size, backdrop, blend mode, opacity)
		// rather than getting their own.
		bChanged |= widgets::Checkbox( "Frametime graph", &cfg.graph_enabled );
		bChanged |= widgets::Checkbox( "Percentile row (1% / 0.1% / avg)", &cfg.percentiles_enabled );

		// Issue #28: per-module enable toggles for the CPU/GPU/Media
		// modules issue #27's ordering framework reserved slots for --
		// same List-rows checkbox pattern as the two row toggles above,
		// independent of `enabled` (the readout as a whole).
		ImGui::Spacing();
		bChanged |= widgets::Checkbox( "CPU module (load, RAM)", &cfg.cpu_enabled );
		bChanged |= widgets::Checkbox( "GPU module (usage, VRAM, temp, power)", &cfg.gpu_enabled );
		bChanged |= widgets::Checkbox( "Media module (now playing)", &cfg.media_enabled );

		// Issue #29's own "at least one new styling option" -- see
		// ConfigSchema.h's module_spacing field comment for the two
		// alternatives considered and rejected (corner rounding
		// independent of the backdrop, a font-weight-per-module override).
		SetStockSliderFullWidth( "Module spacing" );
		bChanged |= ImGui::SliderFloat( "Module spacing", &cfg.module_spacing, 0.0f, 32.0f, "%.0f px" );

		SetStockSliderFullWidth( "Font size" );
		bChanged |= ImGui::SliderFloat( "Font size", &cfg.font_size, 10.0f, 48.0f, "%.0f px" );

		// Issue #29: "inverted" is a third value -- see AddTextInverted()'s
		// own header comment for what it draws and why.
		static const char *s_BlendModes[] = { "alpha", "additive", "inverted" };
		int nBlendIdx = cfg.blend_mode == "additive" ? 1 : ( cfg.blend_mode == "inverted" ? 2 : 0 );
		if ( ImGui::Combo( "Blend mode", &nBlendIdx, s_BlendModes, 3 ) )
		{
			cfg.blend_mode = s_BlendModes[nBlendIdx];
			bChanged = true;
		}

		SetStockSliderFullWidth( "Text opacity" );
		bChanged |= ImGui::SliderFloat( "Text opacity", &cfg.text_opacity, 0.0f, 1.0f );

		// Additive pairs oddly with a filled backdrop (the backdrop itself
		// would glow) and inverted's own outline/fill pair is already
		// legible without one (a filled backdrop behind it just reads as
		// noise) -- auto-disable rather than let either combine with a
		// backdrop (SPEC.md B5, and this issue's own predicted "Inverted +
		// backdrop" combination); DrawReadout() enforces the same rule
		// (ModuleBackdropAllowed()) on the render side regardless of what's
		// stored here.
		const bool bBackdropAvailable = ModuleBackdropAllowed( cfg );
		ImGui::BeginDisabled( !bBackdropAvailable );
		bChanged |= widgets::Checkbox( "Backdrop", &cfg.backdrop_enabled );
		ImGui::BeginDisabled( !( bBackdropAvailable && cfg.backdrop_enabled ) );
		SetStockSliderFullWidth( "Backdrop opacity" );
		bChanged |= ImGui::SliderFloat( "Backdrop opacity", &cfg.backdrop_opacity, 0.0f, 1.0f );
		SetStockSliderFullWidth( "Backdrop rounding" );
		bChanged |= ImGui::SliderFloat( "Backdrop rounding", &cfg.backdrop_rounding, 0.0f, 16.0f, "%.0f px" );
		SetStockSliderFullWidth( "Backdrop padding" );
		bChanged |= ImGui::SliderFloat( "Backdrop padding", &cfg.backdrop_padding, 0.0f, 24.0f, "%.0f px" );
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		// Issue #29: per-module colour overrides -- ignored while Inverted
		// is active (that mode's whole point is a guaranteed-legible pair
		// no fixed hue can improve on), same "greyed out, mode owns the
		// treatment" precedent additive already sets for the backdrop
		// controls above.
		ImGui::Spacing();
		ImGui::TextUnformatted( "Module colours" );
		ImGui::BeginDisabled( cfg.blend_mode == "inverted" );
		bChanged |= DrawModuleColorPicker( "FPS", cfg.color_fps, gamescope::palette::kAccentValue );
		bChanged |= DrawModuleColorPicker( "CPU", cfg.color_cpu, gamescope::palette::kAccentIcon );
		bChanged |= DrawModuleColorPicker( "GPU", cfg.color_gpu, gamescope::palette::kAccentKnob );
		bChanged |= DrawModuleColorPicker( "Media", cfg.color_media, gamescope::palette::kAccentHandle );
		ImGui::EndDisabled();

		ImGui::EndDisabled();

		if ( bChanged )
			PersistSettings();
	}
}
