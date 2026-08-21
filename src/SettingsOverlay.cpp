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

#include <algorithm>
#include <memory>

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

	// M1: testable from the console without any input work (Milestone M2 owns
	// actually routing keyboard/mouse into this). `settings_overlay_visible 1`
	// or `toggle_settings_overlay` both work.
	static ConVar<bool> cv_settings_overlay_visible(
		"settings_overlay_visible", false,
		"Show/hide the M1 settings overlay placeholder. Rendering only -- "
		"there is no input capture yet (Milestone M2)." );

	static ConCommand cc_toggle_settings_overlay(
		"toggle_settings_overlay", "Toggle the M1 settings overlay placeholder.",
		[]( std::span<std::string_view> args )
		{
			cv_settings_overlay_visible.SetValue( !cv_settings_overlay_visible.Get() );
		} );

	// Fade in/out on toggle (decision: replaces the dropped backdrop-blur
	// treatment, see DECISIONS.md #5). SPEC leaves the exact duration as a
	// design decision, not an engineering one -- 200ms is picked here as a
	// reasonable default, matching gamescope's own fade-out duration order of
	// magnitude elsewhere (g_FadeOutDuration).
	static constexpr unsigned int k_uOverlayFadeMs = 200;

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

	static bool s_bWasVisible = false;
	static unsigned int s_uFadeAnchorTimeMs = 0;
	static float s_flFadeAtAnchor = 0.0f;
	static float s_flCurrentAlpha = 0.0f;

	static uint64_t s_ulLastFrameTimeNanos = 0;

	// Applies the "glass instrument" palette from ui-design-guide.md: flat/
	// square corners, 1px hairline borders, cyan accent, near-black
	// translucent surfaces. Cheap to do via ImGuiStyle -- custom fonts
	// (IBM Plex Sans/Mono) are not (they need embedding font data), so those
	// are deliberately left for the M8 polish milestone; ImGui's built-in
	// default font is used here instead.
	static void ApplyDesignGuideStyle()
	{
		ImGuiStyle &style = ImGui::GetStyle();

		style.WindowRounding = 0.0f;
		style.ChildRounding = 0.0f;
		style.FrameRounding = 0.0f;
		style.PopupRounding = 0.0f;
		style.ScrollbarRounding = 0.0f;
		style.GrabRounding = 0.0f;
		style.TabRounding = 0.0f;

		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.FrameBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;

		style.WindowPadding = ImVec2( 12.0f, 10.0f );
		style.FramePadding = ImVec2( 8.0f, 4.0f );
		style.ItemSpacing = ImVec2( 8.0f, 6.0f );

		const ImVec4 accent     = ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, 1.00f );
		const ImVec4 accentSoft = ImVec4( 0x4f / 255.0f, 0xb8 / 255.0f, 0xd6 / 255.0f, 0.22f );
		const ImVec4 surface    = ImVec4( 0x09 / 255.0f, 0x0a / 255.0f, 0x0c / 255.0f, 0.88f );
		const ImVec4 raised     = ImVec4( 1.00f, 1.00f, 1.00f, 0.05f );
		const ImVec4 hairline   = ImVec4( 1.00f, 1.00f, 1.00f, 0.10f );
		const ImVec4 text       = ImVec4( 0.92f, 0.94f, 0.95f, 1.00f );
		const ImVec4 textDim    = ImVec4( 0.92f, 0.94f, 0.95f, 0.50f );
		const ImVec4 transparent = ImVec4( 0.0f, 0.0f, 0.0f, 0.0f );

		ImVec4 *colors = style.Colors;
		colors[ImGuiCol_Text]             = text;
		colors[ImGuiCol_TextDisabled]     = textDim;
		colors[ImGuiCol_WindowBg]         = surface;
		colors[ImGuiCol_ChildBg]          = transparent;
		colors[ImGuiCol_PopupBg]          = surface;
		colors[ImGuiCol_Border]           = hairline;
		colors[ImGuiCol_BorderShadow]     = transparent;
		colors[ImGuiCol_FrameBg]          = raised;
		colors[ImGuiCol_FrameBgHovered]   = accentSoft;
		colors[ImGuiCol_FrameBgActive]    = accentSoft;
		colors[ImGuiCol_TitleBg]          = raised;
		colors[ImGuiCol_TitleBgActive]    = raised;
		colors[ImGuiCol_TitleBgCollapsed] = raised;
		colors[ImGuiCol_CheckMark]        = accent;
		colors[ImGuiCol_SliderGrab]       = accent;
		colors[ImGuiCol_SliderGrabActive] = accent;
		colors[ImGuiCol_Button]           = raised;
		colors[ImGuiCol_ButtonHovered]    = accentSoft;
		colors[ImGuiCol_ButtonActive]     = accent;
		colors[ImGuiCol_Header]           = accentSoft;
		colors[ImGuiCol_HeaderHovered]    = accentSoft;
		colors[ImGuiCol_HeaderActive]     = accent;
		colors[ImGuiCol_Separator]        = hairline;
		colors[ImGuiCol_ResizeGrip]       = accentSoft;
		colors[ImGuiCol_ResizeGripHovered]= accent;
		colors[ImGuiCol_ResizeGripActive] = accent;
	}

	static void EnsureImguiInit()
	{
		if ( s_bImguiInitialized )
			return;

		// Guards against leaking a context if ImGui_ImplVulkan_Init() below
		// fails: EnsureImguiInit() is retried every frame while the overlay is
		// visible (s_bImguiInitialized only ever latches true, never false),
		// so without this a repeated failure would call CreateContext() again
		// on every single frame.
		if ( ImGui::GetCurrentContext() != nullptr )
			return;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr; // no persisted window layout in M1
		ApplyDesignGuideStyle();

		s_pTimelineSemaphore = g_device.CreateTimelineSemaphore( 0, /* bShared = */ false );

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

	static bool EnsureTexture( uint32_t uWidth, uint32_t uHeight )
	{
		if ( s_pOverlayTexture && s_uTextureWidth == uWidth && s_uTextureHeight == uHeight )
			return true;

		OwningRc<CVulkanTexture> pNewTexture = new CVulkanTexture();

		CVulkanTexture::createFlags flags;
		flags.bSampled = true;
		flags.bColorAttachment = true;

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
		const bool bVisible = cv_settings_overlay_visible.Get();
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

	static void DrawPlaceholderWindow()
	{
		ImGui::SetNextWindowPos( ImVec2( 64.0f, 64.0f ), ImGuiCond_FirstUseEver );
		ImGui::SetNextWindowSize( ImVec2( 440.0f, 260.0f ), ImGuiCond_FirstUseEver );

		ImGui::Begin( "gamescope-ritz settings (M1 placeholder)", nullptr, ImGuiWindowFlags_NoCollapse );

		ImGui::TextUnformatted( "Settings overlay render shell -- Milestone 1" );
		ImGui::Separator();
		ImGui::TextWrapped(
			"This proves the render pipeline end to end: ImGui draws on the "
			"general Vulkan queue, gets composited as a normal layer, and "
			"fades in/out on toggle." );
		ImGui::Spacing();

		ImGui::Text( "Layer alpha: %.2f", s_flCurrentAlpha );

		static float flDummySlider = 0.5f;
		ImGui::SliderFloat( "Dummy slider", &flDummySlider, 0.0f, 1.0f );

		static bool bDummyToggle = false;
		ImGui::Checkbox( "Dummy toggle", &bDummyToggle );

		ImGui::Spacing();
		ImGui::TextDisabled(
			"Toggle: `toggle_settings_overlay` or `settings_overlay_visible 0|1`." );
		ImGui::TextDisabled(
			"Input capture lands in Milestone 2 -- these widgets aren't clickable yet." );

		ImGui::End();
	}

	// Records the ImGui draw into s_pOverlayTexture on the general queue and
	// submits it, signaling s_pTimelineSemaphore at the returned point on
	// success. Returns false (nothing submitted) on failure.
	static bool RenderAndSubmit()
	{
		if ( s_bHasPrevSubmission )
		{
			// See the file-level comment: this CPU wait only guards freeing our
			// own previous frame's command buffer, not the compute queue's
			// read of the texture (that's the GPU-side wait added in
			// SettingsOverlay_WaitForRender()). Expected to return immediately.
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

	void SettingsOverlay_AddLayer( FrameInfo_t *pFrameInfo )
	{
		UpdateFadeAlpha();

		if ( s_flCurrentAlpha <= 0.0f )
			return;

		if ( g_nOutputWidth == 0 || g_nOutputHeight == 0 )
			return;

		EnsureImguiInit();
		if ( !s_bImguiInitialized )
			return;

		if ( !EnsureTexture( g_nOutputWidth, g_nOutputHeight ) )
			return;

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
		DrawPlaceholderWindow();
		PanelDisplay_Draw(); // M3: Display panel, see Overlay/PanelDisplay.cpp
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
		layer->opacity = s_flCurrentAlpha;
		layer->filter = GamescopeUpscaleFilter::LINEAR;
		layer->blackBorder = false;
		layer->applyColorMgmt = false; // drm-plane-only; not exercised by the SDL/vkcube M1 test path
		// ImGui's default pipeline blend state produces straight (non-
		// premultiplied) alpha, not gamescope's default premultiplied
		// assumption -- COVERAGE is the mode this codebase already uses for
		// the same reason (cv_overlay_unmultiplied_alpha, the Steam overlay).
		layer->eAlphaBlendingMode = ALPHA_BLENDING_MODE_COVERAGE;
		layer->ctm = nullptr;
		layer->hdr_metadata_blob = nullptr;
		layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
	}

	void SettingsOverlay_WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer )
	{
		if ( !s_bHasPendingWaitPoint )
			return;

		pComputeCmdBuffer->AddDependency( s_pTimelineSemaphore, s_ulPendingWaitPoint );
	}
}
