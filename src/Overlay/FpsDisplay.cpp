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

	// Spec §10 Row 3: three Mono 400 10 @ 55% white items, 10px gaps. Task
	// brief substitutes "average" for the mockup's temperature slot (a
	// reading this feature has no honest data source for) -- 1% low, 0.1%
	// low, average, all from the same history window as the graph. Draws
	// pre-measured strings (see DrawReadout, which builds these once and
	// reuses the same strings/sizes for both layout and drawing).
	static void DrawPercentileRow( ImDrawList *pDrawList, ImVec2 origin, float flTextOpacity,
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
			pDrawList->AddText( ImVec2( flX, origin.y ), color, items[i] );
			flX += sizes[i].x + kItemGap;
		}
		ImGui::PopFont();
	}

	static void DrawReadout()
	{
		const config::FpsDisplaySettings &cfg = s_Settings.fps_display;

		const int nFps = (int)std::lround( UpdateAndGetSmoothedFps() );
		RecomputePercentilesIfDue( get_time_in_nanos() );

		ImDrawList *pDrawList = ImGui::GetBackgroundDrawList();
		// M8 part 1 (issue #13): IBM Plex Mono is genuinely monospaced, so
		// a fixed-width formatted string ("%3d FPS") is tabular by
		// construction -- every digit occupies the same advance width, so
		// the readout can no longer jitter horizontally as the number
		// changes. This replaces the former DrawTabularInt() helper, which
		// existed only to fake that property (per-glyph draws at a hand-
		// measured pitch) before a real tabular-figures font existed; now
		// that one does, the workaround is gone.
		ImFont *pFont = gamescope::fonts::Get( gamescope::fonts::Style::Hero );
		const float flFontSize = cfg.font_size; // still user-configurable (M4's own font-size slider) -- ImGui scales the baked Hero glyphs to whatever size is requested, same mechanism the pre-M8 code already relied on

		const bool bAdditive = cfg.blend_mode == "additive";
		// additive + a filled backdrop rect would make the backdrop
		// itself glow (SPEC.md B5) -- auto-disable rather than combine
		// them, matching the settings panel's own auto-disable of the
		// backdrop controls in additive mode (FpsDisplay_DrawSettingsPanel).
		const bool bDrawBackdrop = cfg.backdrop_enabled && !bAdditive;

		// Gamescope's own layer blend modes (rendervulkan.hpp) are
		// PREMULTIPLIED/COVERAGE/NONE -- there is no whole-layer additive
		// mode to hand this off to. "Additive" here is approximated at
		// the draw-list level (no backdrop, brighter/accent-tinted text)
		// rather than literal GPU ADD blend-func compositing against the
		// scene -- SPEC.md flags this exact interaction as a design-guide
		// call, not a technical one.
		const ImVec4 textColorBase = bAdditive
			? ImVec4( 0x7d / 255.0f, 0xe6 / 255.0f, 0xf7 / 255.0f, 1.0f ) // brighter cyan "glow"
			: ImVec4( 0.92f, 0.94f, 0.95f, 1.0f );
		const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(
			ImVec4( textColorBase.x, textColorBase.y, textColorBase.z, cfg.text_opacity ) );

		// Right-justified in a fixed 3-character field (blank-, not zero-,
		// padded) -- 0-999 is plenty for a frame-rate readout.
		char szNum[8];
		snprintf( szNum, sizeof( szNum ), "%3d", std::clamp( nFps, 0, 999 ) );
		const ImVec2 numSize = pFont->CalcTextSizeA( flFontSize, FLT_MAX, 0.0f, szNum );

		// Spec §10 Row 1: number (Hero) -> "FPS" unit (Meta, dim) -> spacer
		// -> frametime in ms (accent-tinted) -- was one flat "%3d FPS" run
		// in a single color/size; split so the unit and the ms readout can
		// each carry their own spec'd size/color (gap list item 6).
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		static constexpr const char *kUnitText = " FPS";
		const ImVec2 unitSize = ImGui::CalcTextSize( kUnitText );
		ImGui::PopFont();

		char szMs[16];
		snprintf( szMs, sizeof( szMs ), "  %.1fms", s_flSmoothedFrametimeMs );
		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
		const ImVec2 msSize = bAdditive ? ImVec2( 0.0f, 0.0f ) : ImGui::CalcTextSize( szMs );
		ImGui::PopFont();

		const ImVec2 textSize( numSize.x + unitSize.x + msSize.x, std::max( numSize.y, std::max( unitSize.y, msSize.y ) ) );

		// Row 2 (graph) / Row 3 (percentiles): both independently toggleable
		// (spec §11's "ROWS checkbox list"), reusing Row 1's font_size/
		// backdrop/blend_mode/text_opacity rather than a second set of
		// per-row settings.
		const bool bShowGraph = cfg.graph_enabled;
		const bool bShowPercentiles = cfg.percentiles_enabled;

		constexpr float kRowGap = 4.0f;      // spec §10 row2: "4px above/below gaps"
		constexpr float kGraphHeight = 18.0f; // spec §10 row2: "18px tall"

		char szOnePct[24] = "", szPointOnePct[24] = "", szAvg[24] = "";
		const char *const percentileItems[3] = { szOnePct, szPointOnePct, szAvg };
		ImVec2 percentileSizes[3] = {};
		float flPercentileRowWidth = 0.0f;
		float flPercentileRowHeight = 0.0f;
		if ( bShowPercentiles )
		{
			// %3d, same reasoning as szNum above: fixed-width digits so
			// neither an individual number nor the row's total width
			// jitters as a value crosses a digit boundary (task brief:
			// "digits do not jitter").
			snprintf( szOnePct, sizeof( szOnePct ), "1%% %3d", std::clamp( (int)std::lround( s_flOnePercentLowFps ), 0, 999 ) );
			snprintf( szPointOnePct, sizeof( szPointOnePct ), "0.1%% %3d", std::clamp( (int)std::lround( s_flPointOnePercentLowFps ), 0, 999 ) );
			snprintf( szAvg, sizeof( szAvg ), "avg %3d", std::clamp( (int)std::lround( s_flAverageFps ), 0, 999 ) );

			constexpr float kItemGap = 10.0f; // spec §10 row3: "10px gaps"
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
			for ( int i = 0; i < 3; ++i )
			{
				percentileSizes[i] = ImGui::CalcTextSize( percentileItems[i] );
				flPercentileRowHeight = std::max( flPercentileRowHeight, percentileSizes[i].y );
			}
			ImGui::PopFont();
			flPercentileRowWidth = percentileSizes[0].x + kItemGap + percentileSizes[1].x + kItemGap + percentileSizes[2].x;
		}

		// Spec §10: "container ... min-width 186px" -- convert to a content-
		// width floor using whatever padding is actually configured (rather
		// than assuming the spec's own fixed 8px/11px split, which this
		// readout already collapses to one symmetric backdrop_padding value)
		// so the HUD still reads as "at least this wide" regardless of the
		// padding slider.
		constexpr float kMinContainerWidth = 186.0f;
		const float flContentWidthFloor = kMinContainerWidth - cfg.backdrop_padding * 2.0f;
		const float flContentWidth = std::max( { textSize.x, flPercentileRowWidth, flContentWidthFloor } );

		float flContentHeight = textSize.y;
		if ( bShowGraph )
			flContentHeight += kRowGap + kGraphHeight;
		if ( bShowPercentiles )
			flContentHeight += kRowGap + flPercentileRowHeight;

		// Spec §10: "Default anchor: top-right, offset 32/32" -- was a fixed
		// top-left 16/16 (gap list item 6); this context's io.DisplaySize is
		// the actual output resolution (see FpsDisplay_AddLayer()), so the
		// right/top offsets are computed against it rather than hand-tuned.
		constexpr float kAnchorOffset = 32.0f;
		const ImVec2 io_display = ImGui::GetIO().DisplaySize;
		const ImVec2 rectMax( io_display.x - kAnchorOffset, kAnchorOffset + flContentHeight + cfg.backdrop_padding * 2.0f );
		const ImVec2 rectMin( rectMax.x - flContentWidth - cfg.backdrop_padding * 2.0f, kAnchorOffset );
		const ImVec2 textPos( rectMin.x + cfg.backdrop_padding, rectMin.y + cfg.backdrop_padding );

		if ( bDrawBackdrop )
		{
			// Spec §10: "square corners (radius 0 -- unlike windows)" is the
			// mockup's own default, but backdrop_rounding stays a real user
			// setting (M4's own control, still exposed in
			// FpsDisplay_DrawSettingsPanel below) rather than forced to 0.
			const ImU32 backdropColor = ImGui::ColorConvertFloat4ToU32( ImVec4( 0x09 / 255.0f, 0x0b / 255.0f, 0x0e / 255.0f, cfg.backdrop_opacity ) );
			pDrawList->AddRectFilled( rectMin, rectMax, backdropColor, cfg.backdrop_rounding );
			pDrawList->AddRect( rectMin, rectMax, ImGui::GetColorU32( gamescope::palette::White( 0.12f ) ), cfg.backdrop_rounding );
		}

		ImVec2 cursor = textPos;
		pDrawList->AddText( pFont, flFontSize, cursor, textColor, szNum );
		cursor.x += numSize.x;

		ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Meta ) );
		const ImU32 unitColor = ImGui::GetColorU32( gamescope::palette::White( 0.50f ) );
		pDrawList->AddText( ImVec2( cursor.x, cursor.y + ( numSize.y - unitSize.y ) ), unitColor, kUnitText );
		cursor.x += unitSize.x;
		ImGui::PopFont();

		if ( !bAdditive )
		{
			// Spec §10: frametime readout color oklch(.86 .09 218) = #89E0F8
			// -- close to, but distinct from, the general accent-value token
			// (#78DBF6), so it's kept as its own literal rather than routed
			// through Palette.h's accent family.
			ImGui::PushFont( gamescope::fonts::Get( gamescope::fonts::Style::Value ) );
			const ImU32 msColor = ImGui::GetColorU32( ImVec4( 0x89 / 255.0f, 0xe0 / 255.0f, 0xf8 / 255.0f, cfg.text_opacity ) );
			pDrawList->AddText( ImVec2( cursor.x, cursor.y + ( numSize.y - msSize.y ) ), msColor, szMs );
			ImGui::PopFont();
		}

		float flCursorY = textPos.y + textSize.y;
		if ( bShowGraph )
		{
			flCursorY += kRowGap;
			DrawFrametimeGraph( pDrawList, ImVec2( textPos.x, flCursorY ), flContentWidth, kGraphHeight );
			flCursorY += kGraphHeight;
		}
		if ( bShowPercentiles )
		{
			flCursorY += kRowGap;
			DrawPercentileRow( pDrawList, ImVec2( textPos.x, flCursorY ), cfg.text_opacity, percentileItems, percentileSizes );
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
		ImGui::TextUnformatted( "FPS display (M4)" );
		ImGui::PopFont();

		bool bChanged = false;
		// Widgets::Checkbox, not ::Toggle: this settings block IS the design
		// guide's own named example of the "List rows" checkbox-row pattern
		// ("FPS HUD's row toggles: 11x11 checkbox + label") -- see Widgets.h's
		// Checkbox() comment.
		bChanged |= widgets::Checkbox( "Show FPS counter", &cfg.enabled );

		ImGui::BeginDisabled( !cfg.enabled );

		// Spec §11's "ROWS checkbox list" -- Row 2 (frametime graph) / Row 3
		// (percentile stats), independently toggleable, sharing every other
		// setting on this panel (font size, backdrop, blend mode, opacity)
		// rather than getting their own.
		bChanged |= widgets::Checkbox( "Frametime graph", &cfg.graph_enabled );
		bChanged |= widgets::Checkbox( "Percentile row (1% / 0.1% / avg)", &cfg.percentiles_enabled );

		SetStockSliderFullWidth( "Font size" );
		bChanged |= ImGui::SliderFloat( "Font size", &cfg.font_size, 10.0f, 48.0f, "%.0f px" );

		static const char *s_BlendModes[] = { "alpha", "additive" };
		int nBlendIdx = cfg.blend_mode == "additive" ? 1 : 0;
		if ( ImGui::Combo( "Blend mode", &nBlendIdx, s_BlendModes, 2 ) )
		{
			cfg.blend_mode = s_BlendModes[nBlendIdx];
			bChanged = true;
		}

		SetStockSliderFullWidth( "Text opacity" );
		bChanged |= ImGui::SliderFloat( "Text opacity", &cfg.text_opacity, 0.0f, 1.0f );

		// Additive pairs oddly with a filled backdrop (the backdrop itself
		// would glow) -- auto-disable rather than let the two silently
		// combine (SPEC.md B5); DrawReadout() enforces the same rule on
		// the render side regardless of what's stored here.
		const bool bBackdropAvailable = cfg.blend_mode != "additive";
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

		ImGui::EndDisabled();

		if ( bChanged )
			PersistSettings();
	}
}
