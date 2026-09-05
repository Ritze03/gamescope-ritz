// Toast notification system -- see Notifications.h for the design this
// follows (FpsDisplay.cpp's independent-context/texture/semaphore shape,
// DECISIONS.md #25's global-placement/per-game-mute split).
#include "Notifications.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "main.hpp"
#include "log.hpp"
#include "convar.h"
#include "Config/ConfigManager.h"
#include "Fonts.h"
#include "Palette.h"
#include "UI/Registry.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

namespace gamescope::Notifications
{
	static LogScope s_NotifLog( "notifications" );

	// -------------------------------------------------------------------
	// Config: loaded lazily once, cached locally, refreshed on generation
	// bump -- same pattern as FpsDisplay.cpp's EnsureConfigLoaded().
	// -------------------------------------------------------------------

	static bool s_bConfigLoaded = false;
	static uint64_t s_ulLoadedGeneration = 0;
	// Effective (per-game-override-if-active, else global) settings --
	// `muted` is read from here, following the same rule every other
	// per-game-eligible field in this codebase does.
	static config::Settings s_Settings;
	// Placement is a process-level, *always global* preference
	// (ConfigSchema.h's OverlaySettings comment, DECISIONS.md #25) -- it is
	// never present in a profile or per-game snapshot file at all, so it
	// must be read from global.json directly rather than from s_Settings
	// above: a game with "Override Global Config" on would otherwise see
	// the compiled-in default instead of the user's real placement choice.
	static config::OverlaySettings s_GlobalOverlay;

	// Definition of the live scale/opacity state declared in Notifications.h
	// -- see that header's comment for the full seed-once/push-on-edit
	// contract this follows (Palette.h's g_LiveTheme's own shape).
	LiveTheme g_LiveTheme;

	static bool s_bLiveThemeLoaded = false;

	static void EnsureLiveThemeLoaded()
	{
		if ( s_bLiveThemeLoaded )
			return;
		s_bLiveThemeLoaded = true;
		// Seeded straight from global.json, not from s_GlobalOverlay below --
		// this only ever needs to run once (PanelConfig.cpp's DrawGeneralTab()
		// keeps g_LiveTheme current after this), and notification_scale/
		// opacity_notifications are process-level/global.json-only fields
		// (ConfigSchema.h's OverlaySettings comment), same as everything
		// s_GlobalOverlay itself loads.
		const config::OverlaySettings &o = config::LoadGlobal().overlay;
		g_LiveTheme.flScale = o.notification_scale;
		g_LiveTheme.flOpacity = o.opacity_notifications;
	}

	static void EnsureConfigLoaded()
	{
		EnsureLiveThemeLoaded();
		const uint64_t ulGeneration = config::ConfigGeneration();
		if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
			return;
		s_Settings = config::ResolveEffective( config::SessionAppId() );
		s_GlobalOverlay = config::LoadGlobal().overlay;
		s_ulLoadedGeneration = ulGeneration;
		s_bConfigLoaded = true;
	}

	// Mute is a normal per-game-eligible field (NotificationSettings::muted)
	// -- routes exactly like FpsDisplay's own cfg.enabled edits.
	static void PersistMuted()
	{
		config::EnqueueRoutedWrite( s_Settings );
	}

	// Placement must always land in global.json, regardless of whether a
	// per-game override is currently active for this session -- routing it
	// through EnqueueRoutedWrite() like PersistMuted() above would send it
	// into a per-game snapshot instead, exactly what DECISIONS.md #25 says
	// must never happen. Reads global.json fresh (a small blocking read,
	// fine here: this only runs once per user click on the placement
	// picker, not per frame -- see Config/ConfigManager.h's threading note)
	// so this write never clobbers any other global field a concurrent
	// panel edit already changed since s_GlobalOverlay was last loaded.
	static void PersistPlacement()
	{
		config::Settings global = config::LoadGlobal();
		global.overlay.notification_placement = s_GlobalOverlay.notification_placement;
		config::EnqueueGlobalWrite( std::move( global ) );
	}

	// -------------------------------------------------------------------
	// Placement: 9 anchor positions, stored on disk as one of the strings
	// below (ConfigSchema.h's OverlaySettings::notification_placement).
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
					if ( sPlacement == kPlacements[ v ][ h ] )
					{
						nVert = v;
						nHoriz = h;
						return;
					}
				}
			}
			// Unrecognized/legacy value -- fall back to the same corner
			// FpsDisplay.cpp's own HUD anchors to by default.
			nVert = 0;
			nHoriz = 2;
		}

		std::string ComposePlacement( int nVert, int nHoriz )
		{
			return kPlacements[ std::clamp( nVert, 0, 2 ) ][ std::clamp( nHoriz, 0, 2 ) ];
		}
	}

	// -------------------------------------------------------------------
	// UI scale/opacity: every size this file draws already routes through
	// GetUiScale(), and every toast's alpha already routes through
	// DrawToasts()'s flAlpha, so both live fields (g_LiveTheme, see
	// Notifications.h) only need wiring in these two spots.
	// -------------------------------------------------------------------

	static float GetUiScale()
	{
		return g_LiveTheme.flScale;
	}

	static float GetUiOpacity()
	{
		return g_LiveTheme.flOpacity;
	}

	// -------------------------------------------------------------------
	// Toast queue: several at once (stacked), long text (wrapped, and
	// hard-truncated past kMaxTextLen so one Show() call can't blow up a
	// card), and a queue cap so a burst can't fill the screen.
	// -------------------------------------------------------------------

	namespace
	{
		struct Toast
		{
			std::string sText;
			Kind kind = Kind::Info;
			float flDurationSec = 4.0f;
			float flAgeSec = 0.0f; // seconds since Show() queued this toast
		};

		constexpr size_t kMaxVisible = 4;   // stacked cards drawn at once
		constexpr size_t kMaxQueued = 8;    // hard cap (incl. visible) -- burst protection
		constexpr size_t kMaxTextLen = 240; // hard truncation length, independent of wrap width
		// Design-exploration variant (issue #41, "calm"): the issue's own
		// ground rule to revisit is "checking against real interaction, not
		// just read as a code/static swatch" -- watching this in motion (not
		// just eyeballing the numbers), the original 40px slide at 0.22s in
		// reads as a hard snap when 2-3 toasts queue in a burst (a real
		// case: PersistMuted()/PersistPlacement()-adjacent flows can fire
		// several in a row). Slower in/out (0.22/0.28 -> 0.32/0.36s) and a
		// shorter travel distance (40 -> 26px) together read as "settling
		// into place" rather than "sliding in from off-screen" -- calmer
		// without being sluggish; duration/queue-cap/text-length behavior
		// is unchanged. kCardGap widened slightly (10 -> 13px) so a stack
		// of several cards doesn't read as densely packed at the new, more
		// deliberate pace.
		constexpr float kInAnimSec = 0.32f;
		constexpr float kOutAnimSec = 0.36f;
		constexpr float kCardWidth = 360.0f;
		constexpr float kCardGap = 13.0f;
		constexpr float kEdgeMargin = 24.0f;
		constexpr float kSlideDistance = 26.0f;

		std::deque<Toast> s_Toasts;

		// 0 while entering (fading/sliding in), 1 while fully shown, back
		// down to 0 while leaving (fading/sliding out) -- drives both the
		// card's alpha and its slide offset from one number.
		float ComputeVisibility( const Toast &t )
		{
			if ( t.flAgeSec < kInAnimSec )
				return t.flAgeSec / kInAnimSec;
			const float flTimeLeft = ( t.flDurationSec + kOutAnimSec ) - t.flAgeSec;
			if ( flTimeLeft < kOutAnimSec )
				return std::clamp( flTimeLeft / kOutAnimSec, 0.0f, 1.0f );
			return 1.0f;
		}

		float EaseOutCubic( float x )
		{
			const float p = 1.0f - std::clamp( x, 0.0f, 1.0f );
			return 1.0f - p * p * p;
		}

		ImU32 KindColor( Kind kind )
		{
			switch ( kind )
			{
				// Spec (ui-mockup-precise-spec.md §1) tokens, reused here
				// even though Palette.h itself doesn't expose them yet
				// (it's owned by a sibling worker this task must not
				// touch -- see this file's header comment).
				case Kind::Ok: return IM_COL32( 0x6E, 0xD2, 0x74, 255 ); // spec's "ok" status green
				case Kind::Warning: return IM_COL32( 0xF3, 0x82, 0x1D, 255 ); // spec's "spike" amber
				case Kind::Error:   return IM_COL32( 0xE5, 0x5B, 0x5B, 255 ); // this file's own addition -- no red token exists in Palette.h yet
				case Kind::Info:
				default:            return gamescope::palette::kAccent;
			}
		}

		ImU32 KindColorAlpha( Kind kind, float flAlpha )
		{
			ImVec4 v = ImGui::ColorConvertU32ToFloat4( KindColor( kind ) );
			v.w = flAlpha;
			return ImGui::ColorConvertFloat4ToU32( v );
		}

		void UpdateToasts( float flDeltaTime )
		{
			for ( Toast &t : s_Toasts )
				t.flAgeSec += flDeltaTime;

			// Every queued toast ages (not just the visible ones) -- with
			// kMaxQueued capped at 8 and kMaxVisible at 4, a fully-loaded
			// queue drains to fully visible within one toast's own
			// duration, so nothing waits indefinitely off-screen.
			while ( !s_Toasts.empty() && s_Toasts.front().flAgeSec >= s_Toasts.front().flDurationSec + kOutAnimSec )
				s_Toasts.pop_front();
		}
	}

	void Show( std::string sText, Kind kind, float flDurationSec )
	{
		EnsureConfigLoaded();
		if ( s_Settings.notifications.muted )
			return; // per-game (if overridden) or global mute -- nothing queued, no toast ever appears

		if ( sText.empty() )
			return;

		if ( sText.size() > kMaxTextLen )
		{
			sText.resize( kMaxTextLen - 1 );
			sText += "\xE2\x80\xA6"; // UTF-8 U+2026 HORIZONTAL ELLIPSIS
		}

		if ( s_Toasts.size() >= kMaxQueued )
			s_Toasts.pop_front(); // drop the oldest to make room -- a burst must never grow the queue unbounded

		Toast t;
		t.sText = std::move( sText );
		t.kind = kind;
		t.flDurationSec = std::max( flDurationSec, 0.5f );
		s_Toasts.push_back( std::move( t ) );
	}

	// Console affordance for manual verification without needing a real
	// call site wired up yet -- same shape as FpsDisplay.cpp's
	// cc_toggle_fps_display.
	static ConCommand cc_notify_test(
		"notify_test", "Queue a test toast notification (args: [kind] [text...], kind = info|success|warning|error).",
		[]( std::span<std::string_view> args )
		{
			// args[0] is always this command's own name (ConCommand::Exec /
			// CallWithArgString both prepend it -- see convar.cpp/.h) --
			// real arguments start at args[1].
			Kind kind = Kind::Info;
			size_t uTextStart = 1;
			if ( args.size() > 1 )
			{
				if ( args[1] == "success" ) { kind = Kind::Ok; uTextStart = 2; }
				else if ( args[1] == "warning" ) { kind = Kind::Warning; uTextStart = 2; }
				else if ( args[1] == "error" )   { kind = Kind::Error;   uTextStart = 2; }
				else if ( args[1] == "info" )    { kind = Kind::Info;    uTextStart = 2; }
			}

			std::string sText;
			for ( size_t i = uTextStart; i < args.size(); i++ )
			{
				if ( !sText.empty() )
					sText += ' ';
				sText += args[i];
			}
			if ( sText.empty() )
				sText = "Test notification";

			Show( std::move( sText ), kind );
		} );

	// -------------------------------------------------------------------
	// Render pipeline: own ImGui context, offscreen texture, general-queue
	// submission and timeline semaphore -- identical shape to
	// FpsDisplay.cpp's own (see that file's header comment for the full
	// rationale); this is a second, independent instance of it, not a
	// third pattern.
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

	static std::shared_ptr<VulkanTimelineSemaphore_t> s_pReadDoneSemaphore;
	static uint64_t s_ulReadDoneCounter = 0;
	static uint64_t s_ulPendingReadDonePoint = 0;
	static uint64_t s_ulRegisteredReadDonePoint = 0;

	static uint64_t s_ulLastFrameTimeNanos = 0;

	// Issue #30 looked at this file's own EnsureImguiInit()/EnsureTexture()
	// as a structurally-identical latent case of the settings overlay's
	// startup hitch and deliberately left it lazy: nothing calls Show() at
	// process start, so its first use was "genuine first use", and warming a
	// second context at every launch read as added cost with no observed
	// hitch to fix. That reasoning did not survive contact with the user
	// (requests-2026-09-05 item 6): "When the first notification is being
	// shown, there is always a small but visible lag spike ... I would
	// rather have a ghost lag at launch than a lag while being mid-game."
	// Genuine first use IS mid-game -- the first toast usually follows a
	// settings edit -- and the hitch is real. So these are now warmed at
	// launch by WarmUp() below; they are still written as lazy guards so a
	// warm-up that never ran (output size unknown, Vulkan init failed) falls
	// back to exactly the old behaviour rather than to no toasts at all.
	static void EnsureImguiInit()
	{
		if ( s_bImguiInitialized )
			return;

		if ( s_pImguiContext != nullptr )
			return; // a previous ImGui_ImplVulkan_Init() attempt already failed once this run

		ImGuiContext *pPrevContext = ImGui::GetCurrentContext();

		IMGUI_CHECKVERSION();
		s_pImguiContext = ImGui::CreateContext();
		// See FpsDisplay.cpp's EnsureImguiInit() comment for why this
		// explicit SetCurrentContext() (rather than trusting
		// CreateContext() to leave the new context current) is load-bearing
		// once more than one ImGui context exists in the process.
		ImGui::SetCurrentContext( s_pImguiContext );

		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr;

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
				s_NotifLog.errorf( "ImGui Vulkan backend: VkResult %d", (int)err );
		};

		if ( !ImGui_ImplVulkan_Init( &init_info ) )
		{
			s_NotifLog.errorf( "ImGui_ImplVulkan_Init failed" );
			ImGui::SetCurrentContext( pPrevContext );
			return;
		}

		s_bImguiInitialized = true;
		ImGui::SetCurrentContext( pPrevContext );
	}

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

		DrainPrevSubmission();

		OwningRc<CVulkanTexture> pNewTexture = new CVulkanTexture();

		CVulkanTexture::createFlags flags;
		flags.bSampled = true;
		flags.bColorAttachment = true;
		flags.bGeneralQueueShared = true;

		if ( !pNewTexture->BInit( uWidth, uHeight, 1u, VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM ), flags ) )
		{
			s_NotifLog.errorf( "failed to (re)create the notifications offscreen texture at %ux%u", uWidth, uHeight );
			return false;
		}

		s_pOverlayTexture = std::move( pNewTexture );
		s_uTextureWidth = uWidth;
		s_uTextureHeight = uHeight;
		s_bTextureNeedsInitialBarrier = true;
		return true;
	}

	// The ONE (font, size) pair every toast is drawn with. Shared by
	// DrawToasts() and WarmUp()'s glyph pass so the two cannot drift: a
	// warm-up at a size the real draw does not use would bake the wrong
	// glyphs and leave the first-toast stall exactly where it was.
	static ImFont *ToastFont()
	{
		return gamescope::fonts::Get( gamescope::fonts::Style::Label );
	}

	static float ToastFontSize()
	{
		// Issue #99: snapped to a whole pixel -- 14 * a fractional scale is
		// otherwise served by the nearest integer bake and resampled. See
		// Fonts.h's RasterSize(). 14 matches Style::Label's real size
		// (Fonts.cpp kSpecs) -- issue #83.
		return fonts::RasterSize( 14.0f * GetUiScale() );
	}

	static void DrawToasts()
	{
		if ( s_Toasts.empty() )
			return;

		ImDrawList *pDrawList = ImGui::GetBackgroundDrawList();
		const ImVec2 io_display = ImGui::GetIO().DisplaySize;
		const float flScale = GetUiScale();
		const float flOpacity = GetUiOpacity();

		int nVert = 0, nHoriz = 2;
		ParsePlacement( s_GlobalOverlay.notification_placement, nVert, nHoriz );

		ImFont *pFont = ToastFont();
		const float flFontSize = ToastFontSize();
		const float flPadding = 12.0f * flScale;
		const float flAccentBarW = 3.0f * flScale;
		const float flCardWidth = kCardWidth * flScale;
		const float flWrapWidth = flCardWidth - flAccentBarW - flPadding * 2.0f;
		const float flGap = kCardGap * flScale;
		const float flMargin = kEdgeMargin * flScale;
		const float flRounding = 8.0f * flScale;

		const size_t nVisible = std::min( s_Toasts.size(), kMaxVisible );

		struct Card
		{
			const Toast *pToast;
			ImVec2 size;
			float flVisibility;
		};

		// Newest-first: cards[0] sits nearest the anchor edge, older toasts
		// get pushed away from it -- s_Toasts is oldest-first (front),
		// newest-last (back), so this walks it from the back.
		std::vector<Card> cards;
		cards.reserve( nVisible );
		float flTotalHeight = 0.0f;
		for ( size_t i = 0; i < nVisible; i++ )
		{
			const Toast &t = s_Toasts[ s_Toasts.size() - 1 - i ];
			const ImVec2 textSize = pFont->CalcTextSizeA( flFontSize, FLT_MAX, flWrapWidth, t.sText.c_str() );
			const ImVec2 cardSize( flCardWidth, textSize.y + flPadding * 2.0f );
			cards.push_back( { &t, cardSize, ComputeVisibility( t ) } );
			flTotalHeight += cardSize.y;
			if ( i + 1 < nVisible )
				flTotalHeight += flGap;
		}

		const float flStartY = ( nVert == 0 ) ? flMargin
			: ( nVert == 1 ) ? ( io_display.y - flTotalHeight ) * 0.5f
			: ( io_display.y - flMargin - flTotalHeight );

		const float flX = ( nHoriz == 0 ) ? flMargin
			: ( nHoriz == 1 ) ? ( io_display.x - flCardWidth ) * 0.5f
			: ( io_display.x - flMargin - flCardWidth );

		// The in/out slide direction: an edge-anchored column slides in
		// from its own screen edge; the dead-center column (nHoriz == 1)
		// has no horizontal edge to use, so it slides from whichever
		// vertical edge it's anchored to instead.
		const float flOutwardX = ( nHoriz == 0 ) ? -1.0f : ( nHoriz == 2 ? 1.0f : 0.0f );
		const float flOutwardY = ( nHoriz == 1 ) ? ( nVert == 2 ? 1.0f : -1.0f ) : 0.0f;

		float flCursorY = flStartY;
		for ( const Card &card : cards )
		{
			const float flEased = EaseOutCubic( card.flVisibility );
			const float flOffset = ( 1.0f - flEased ) * kSlideDistance * flScale;
			const ImVec2 rectMin( flX + flOutwardX * flOffset, flCursorY + flOutwardY * flOffset );
			const ImVec2 rectMax( rectMin.x + card.size.x, rectMin.y + card.size.y );

			// opacity_notifications scales the whole card uniformly (bg,
			// border, accent bar, text all key off flAlpha below) -- same
			// multiplicative role Chrome.cpp's flWindowAlphaFocused/
			// flWindowAlphaUnfocused play for panel windows, just applied
			// here instead of via ImGuiStyle since
			// toasts draw straight into the background draw list.
			const float flAlpha = card.flVisibility * flOpacity;

			const ImU32 uBg = ImGui::ColorConvertFloat4ToU32( gamescope::palette::SurfaceVec4( 0.92f * flAlpha ) );
			pDrawList->AddRectFilled( rectMin, rectMax, uBg, flRounding );
			pDrawList->AddRect( rectMin, rectMax, gamescope::palette::White( 0.10f * flAlpha ), flRounding );

			// Kind-colored accent edge on the left of the card -- the same
			// "this one is live" left-edge accent language Palette.h's own
			// comment describes for the rest of the overlay (dock top
			// edge, active-group left edge), applied per-toast here.
			//
			// DRAWN AS THE CARD, CLIPPED TO A STRIP -- not as its own
			// narrow rect. The obvious version (AddRectFilled over
			// rectMin..rectMin+flAccentBarW with ImDrawFlags_RoundCornersLeft)
			// is what shipped, and it produced the user's 2026-08-24 report:
			// "The left edge is doubled" / "The colored part is next to it,
			// instead of on top of it." Both symptoms are one geometric
			// fault. ImDrawList::PathRect() clamps a corner radius to the
			// rect's own size, so a 3*scale-wide strip asking for an
			// 8*scale radius silently gets ~flAccentBarW-1 instead. The
			// strip therefore carried a *different*, much tighter corner
			// curve than the card it was supposed to sit inside: above and
			// below the card's radius-8 arc the strip stood on bare
			// background (it read as a bar beside the card), and within
			// those few pixels the strip's own tight curve, the card's wide
			// curve and the 1px border arc between them read as a doubled
			// edge with a seam. It got worse with scale, because every
			// radius grows but the strip's clamp does not.
			//
			// Filling the WHOLE card rect in the accent colour, with the
			// card's own rounding, inside a clip rect that exposes only the
			// leftmost flAccentBarW pixels, makes the accent's outer
			// silhouette the card's silhouette *by construction* -- there is
			// no second radius that can disagree with the first, at any
			// scale. The clip is intersected with the current one, so this
			// cannot paint outside the card either.
			pDrawList->PushClipRect(
				ImVec2( rectMin.x, rectMin.y ),
				ImVec2( rectMin.x + flAccentBarW, rectMax.y ),
				/* intersect_with_current_clip_rect = */ true );
			pDrawList->AddRectFilled( rectMin, rectMax,
				KindColorAlpha( card.pToast->kind, flAlpha ), flRounding );
			pDrawList->PopClipRect();

			const ImVec2 textPos( rectMin.x + flAccentBarW + flPadding, rectMin.y + flPadding );
			const ImU32 uTextColor = gamescope::palette::Text( 0.92f * flAlpha );
			pDrawList->AddText( pFont, flFontSize, textPos, uTextColor, card.pToast->sText.c_str(), nullptr, flWrapWidth );

			flCursorY += card.size.y + flGap;
		}
	}

	static bool RenderAndSubmit()
	{
		DrainPrevSubmission();

		if ( !g_device.vk.CmdBeginRendering || !g_device.vk.CmdEndRendering )
		{
			s_NotifLog.errorf( "vkCmdBeginRendering/vkCmdEndRendering not available on this device" );
			return false;
		}

		auto cmdBuffer = g_device.generalCommandBuffer();
		if ( !cmdBuffer )
			return false;

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

	// -------------------------------------------------------------------
	// Launch-time warm-up -- requests-2026-09-05 item 6. See Notifications.h
	// for the contract and Fonts.h's WarmGlyphs() for the mechanism it
	// exists to pre-pay.
	// -------------------------------------------------------------------

	// Verification aid for the item-6 fix, off by default and free while
	// off: logs the wall time this file spends on the render thread for
	// every frame that actually renders a toast, and for WarmUp() itself.
	// Left in on purpose so the lead can measure the first toast after a
	// settings change against steady-state frames on real hardware without
	// rebuilding -- see superdoc/features/notifications.md's "Verifying".
	static ConVar<bool> cv_notifications_time_render(
		"notifications_time_render", false,
		"Log the wall time of every toast frame this file renders (and of its launch warm-up), in ms." );

	static bool s_bWarmedUp = false;

	// Two hidden frames, not one: ImGui's Vulkan backend keeps ImageCount
	// (= 2, EnsureImguiInit()'s init_info) vertex/index buffer slots and
	// rotates through them per RenderDrawData(), allocating each slot's
	// buffers the first time it comes round. One frame would leave the
	// second slot's vkAllocateMemory for the first toast's SECOND frame.
	// The glyph string is far longer than any toast, so the buffers this
	// sizes never need the resize path later either.
	static constexpr int kWarmUpFrames = 2;

	static void DrawGlyphWarmUp()
	{
		fonts::WarmGlyphs( ImGui::GetBackgroundDrawList(), ToastFont(), ToastFontSize(),
			ImGui::GetIO().DisplaySize.x );
	}

	// One hidden frame on this file's context -- NewFrame, optionally the
	// glyph pass, Render, RenderAndSubmit() -- sized to the current
	// texture. Shared by WarmUp() and ReWarmTextureIfResized() so the two
	// cannot drift. The caller has made s_pImguiContext current and holds a
	// texture; it also owns clearing s_bHasPendingWaitPoint afterwards,
	// since nothing a hidden frame draws is ever composited.
	static bool RunHiddenFrame( bool bDrawGlyphs )
	{
		ImGuiIO &io = ImGui::GetIO();
		io.DisplaySize = ImVec2( (float)s_uTextureWidth, (float)s_uTextureHeight );
		io.DeltaTime = 1.0f / 60.0f;

		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		if ( bDrawGlyphs )
			DrawGlyphWarmUp();
		ImGui::Render();
		return RenderAndSubmit();
	}

	void WarmUp()
	{
		if ( s_bWarmedUp )
			return;
		if ( g_nOutputWidth == 0 || g_nOutputHeight == 0 )
			return; // no texture size to warm against yet -- the caller retries next frame
		s_bWarmedUp = true; // one attempt; anything that fails below stays on the lazy path

		const uint64_t ulStartNanos = get_time_in_nanos();

		// Cost 5 of the scout's ranking: two blocking JSON reads
		// (ResolveEffective() + LoadGlobal()) that AddLayer() would otherwise
		// do on the first frame after this file is first touched -- which,
		// since the first toast usually follows a settings edit, is the very
		// frame that also bumps the generation and forces the re-read.
		EnsureConfigLoaded();

		ImGuiContext *pPrevContext = ImGui::GetCurrentContext();

		// Costs 2 and 4: context + font atlas parse, shader modules +
		// vkCreateGraphicsPipelines with no pipeline cache.
		EnsureImguiInit();
		if ( !s_bImguiInitialized )
			return; // EnsureImguiInit() already restored pPrevContext on failure

		ImGui::SetCurrentContext( s_pImguiContext );

		// Cost 3: the full-output BGRA8 offscreen texture.
		if ( EnsureTexture( g_nOutputWidth, g_nOutputHeight ) )
		{
			// Cost 1, the big one: the first RenderDrawData() creates and
			// uploads the font atlas texture and vkQueueWaitIdle()s the
			// general queue. Drawing the whole baked range here means that
			// upload -- and the atlas growth a long toast would otherwise
			// trigger later -- happens now, once, with nothing on screen.
			for ( int i = 0; i < kWarmUpFrames; i++ )
			{
				if ( !RunHiddenFrame( /* bDrawGlyphs = */ true ) )
					break;
			}

			// NO LAYER IS PUSHED -- deliberately. AddLayer() early-outs on an
			// empty queue, so the texture the frames above wrote is never
			// sampled by any composite; a Layer_t here would make
			// layers.count() > 1 and force full composition every frame,
			// defeating direct scanout for the rest of the session. For the
			// same reason no compute pass needs to wait on these submissions:
			// clear the wait point RenderAndSubmit() armed, so WaitForRender()
			// stays the no-op it was before anything was ever drawn. The
			// general-queue work itself is still fenced -- the next real
			// RenderAndSubmit()'s DrainPrevSubmission() waits on it before
			// touching the texture.
			s_bHasPendingWaitPoint = false;
		}

		ImGui::SetCurrentContext( pPrevContext );

		// s_ulLastFrameTimeNanos is untouched on purpose: AddLayer() owns it
		// and runs every frame regardless of whether a toast is queued, so
		// the first real toast's delta is already sane.

		const double flMs = double( get_time_in_nanos() - ulStartNanos ) / 1e6;
		s_NotifLog.infof( "warm-up done in %.2f ms (%d hidden frames, glyphs U+%04X..U+%04X at %.0fpx, texture %ux%u, layers pushed: 0)",
			flMs, kWarmUpFrames, fonts::kBakedFirst, fonts::kBakedLast, ToastFontSize(), s_uTextureWidth, s_uTextureHeight );
	}

	// The residual first-toast cost (2026-09-05, second pass). With the
	// warm-up above in place, a fresh launch still measured the first toast
	// frame at 3-5 ms against a 0.1-1 ms steady state. Everything AddLayer()
	// does inside cv_notifications_time_render's window runs identically
	// every frame -- except EnsureTexture(), which is keyed on the OUTPUT
	// size, and that size can change after WarmUp() ran: in nested mode the
	// SDL backend rewrites g_nOutputWidth/Height from its event thread on
	// SDL_WINDOWEVENT_SIZE_CHANGED, and a tiling compositor resizes the
	// window right after it maps, i.e. after the first sized frame the
	// warm-up keys off. The first toast then re-creates the full-output
	// BGRA8 texture (vkCreateImage + vkAllocateMemory, ~8 MB at 1080p),
	// re-arms the UNDEFINED->GENERAL barrier, and its vkQueueSubmit faults
	// the fresh backing pages in -- all on the render thread, all inside the
	// timed window, exactly once.
	//
	// So while no toast is queued, once the output size has sat at a value
	// the texture does not match for kResizeSettleNanos, re-create it and
	// run one hidden frame right then. Settle-gated so an interactive window
	// resize (a new size per frame) does not allocate a texture per frame:
	// the cost lands once, ~half a second after the resize ends, on a frame
	// with nothing on screen and a window the user has just been watching
	// change -- not mid-toast. Wall-clock rather than frame-counted so it
	// means the same thing at 40 Hz and 144 Hz. A toast that is already up
	// during a resize takes the existing lazy path in AddLayer() (the
	// texture is re-created on its next frame) and the settle clock is
	// reset by the size match.
	//
	// Not mirrored in FpsDisplay.cpp: an enabled HUD draws every frame, so
	// its texture is re-created on the resize frame itself and there is no
	// gap; a disabled HUD has no texture. The Shell re-creates lazily on its
	// next drawn frame, which is a user opening it.
	static constexpr uint64_t kResizeSettleNanos = 500'000'000ull;
	static uint32_t s_uSettleWidth = 0;
	static uint32_t s_uSettleHeight = 0;
	static uint64_t s_ulSettleSinceNanos = 0;

	static void ReWarmTextureIfResized( uint64_t ulNowNanos )
	{
		if ( !s_bWarmedUp || !s_bImguiInitialized || !s_pOverlayTexture )
			return; // never warmed (or init failed): the lazy path is the fallback, as documented
		if ( g_nOutputWidth == 0 || g_nOutputHeight == 0 )
			return;

		if ( g_nOutputWidth == s_uTextureWidth && g_nOutputHeight == s_uTextureHeight )
		{
			s_ulSettleSinceNanos = 0;
			return; // the common case, every frame: nothing to do
		}

		if ( s_ulSettleSinceNanos == 0 || g_nOutputWidth != s_uSettleWidth || g_nOutputHeight != s_uSettleHeight )
		{
			s_uSettleWidth = g_nOutputWidth;
			s_uSettleHeight = g_nOutputHeight;
			s_ulSettleSinceNanos = ulNowNanos;
			return;
		}

		if ( ulNowNanos - s_ulSettleSinceNanos < kResizeSettleNanos )
			return;
		s_ulSettleSinceNanos = 0;

		const uint64_t ulStartNanos = get_time_in_nanos();
		ImGuiContext *pPrevContext = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext( s_pImguiContext );

		bool bOk = EnsureTexture( g_nOutputWidth, g_nOutputHeight );
		if ( bOk )
		{
			// One frame, no glyphs: the atlas and the vertex/index buffers
			// survived the resize; only the render target is new. The frame
			// records the initial barrier and the clear, and its submit is
			// what pins the new allocation's pages.
			bOk = RunHiddenFrame( /* bDrawGlyphs = */ false );
			// Same as WarmUp(): no layer was pushed, nothing samples this,
			// so no compute pass needs to wait on it.
			s_bHasPendingWaitPoint = false;
		}

		ImGui::SetCurrentContext( pPrevContext );

		s_NotifLog.infof( "re-warmed the offscreen texture at %ux%u after a resize in %.2f ms (ok=%d, layers pushed: 0)",
			s_uTextureWidth, s_uTextureHeight, double( get_time_in_nanos() - ulStartNanos ) / 1e6, (int)bOk );
	}

	void AddLayer( FrameInfo_t *pFrameInfo )
	{
		EnsureConfigLoaded();

		const uint64_t ulNowNanos = get_time_in_nanos();
		float flDeltaTime = s_ulLastFrameTimeNanos == 0
			? ( 1.0f / 60.0f )
			: float( ulNowNanos - s_ulLastFrameTimeNanos ) / 1e9f;
		s_ulLastFrameTimeNanos = ulNowNanos;
		flDeltaTime = std::clamp( flDeltaTime, 1.0f / 1000.0f, 1.0f );

		UpdateToasts( flDeltaTime );

		if ( s_Toasts.empty() )
		{
			// Nothing to draw this frame -- skip the whole render pass, same
			// as FpsDisplay's disabled-state early-out. The quiet frames are
			// also where a resize's texture re-creation is pre-paid.
			ReWarmTextureIfResized( ulNowNanos );
			return;
		}

		if ( g_nOutputWidth == 0 || g_nOutputHeight == 0 )
			return;

		// Only frames that actually render are timed -- see
		// cv_notifications_time_render. The clock starts here, past the
		// early-outs, so the log line is the render-thread cost of a toast
		// frame and nothing else.
		const uint64_t ulRenderStartNanos = cv_notifications_time_render ? get_time_in_nanos() : 0;

		ImGuiContext *pPrevContext = ImGui::GetCurrentContext();
		auto RestoreContext = [pPrevContext] { ImGui::SetCurrentContext( pPrevContext ); };

		EnsureImguiInit();
		if ( !s_bImguiInitialized )
			return; // EnsureImguiInit() already restored pPrevContext on failure

		ImGui::SetCurrentContext( s_pImguiContext );

		// For the timing log below: a toast frame that had to (re)create the
		// texture is the resize case ReWarmTextureIfResized() exists to
		// pre-empt, and the one thing in this window that is not steady-state.
		CVulkanTexture *pTextureBefore = s_pOverlayTexture.get();
		if ( !EnsureTexture( g_nOutputWidth, g_nOutputHeight ) )
		{
			RestoreContext();
			return;
		}
		const bool bTextureRecreated = s_pOverlayTexture.get() != pTextureBefore;

		ImGuiIO &io = ImGui::GetIO();
		io.DisplaySize = ImVec2( (float)s_uTextureWidth, (float)s_uTextureHeight );
		io.DeltaTime = flDeltaTime;

		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		DrawToasts();
		ImGui::Render();

		const bool bSubmitted = RenderAndSubmit();

		RestoreContext();

		if ( cv_notifications_time_render )
		{
			s_NotifLog.infof( "toast frame rendered in %.3f ms (%zu queued, warmed=%d, texture %ux%u, recreated=%d)",
				double( get_time_in_nanos() - ulRenderStartNanos ) / 1e6, s_Toasts.size(), (int)s_bWarmedUp,
				s_uTextureWidth, s_uTextureHeight, (int)bTextureRecreated );
		}

		if ( !bSubmitted )
			return;

		FrameInfo_t::Layer_t *layer = pFrameInfo->layers.push();
		if ( !layer )
			return; // out of layer slots this frame

		layer->tex = s_pOverlayTexture;
		layer->zpos = g_zposNotifications;
		layer->offset = { 0.0f, 0.0f };
		layer->scale = { 1.0f, 1.0f };
		layer->opacity = 1.0f; // per-toast alpha is already baked into the texture (DrawToasts' flAlpha)
		layer->filter = GamescopeUpscaleFilter::LINEAR;
		layer->blackBorder = false;
		layer->applyColorMgmt = false;
		layer->eAlphaBlendingMode = ALPHA_BLENDING_MODE_COVERAGE;
		layer->ctm = nullptr;
		layer->hdr_metadata_blob = nullptr;
		layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
	}

	void WaitForRender( CVulkanCmdBuffer *pComputeCmdBuffer )
	{
		if ( !s_bHasPendingWaitPoint )
			return;

		pComputeCmdBuffer->AddDependency( s_pTimelineSemaphore, s_ulPendingWaitPoint );

		s_ulPendingReadDonePoint = ++s_ulReadDoneCounter;
		pComputeCmdBuffer->AddSignal( s_pReadDoneSemaphore, s_ulPendingReadDonePoint );
	}

	void CommitReads()
	{
		if ( !s_ulPendingReadDonePoint )
			return;

		s_ulRegisteredReadDonePoint = s_ulPendingReadDonePoint;
		s_ulPendingReadDonePoint = 0;
	}

	// =====================================================================
	//  P3 part B -- the E2 registrations
	// =====================================================================
	// The same three settings the deleted legacy panel drew, declared
	// instead.
	// They live here rather than in the area file for the reason stated in
	// Notifications.h: everything they bind to is file-static in this
	// translation unit, and a second writer of notification_placement is
	// exactly the bug this arrangement prevents.
	void RegisterRows( ui::Area &area )
	{
		// NO area.Group() HERE. PanelConfig.cpp's BuildAppearanceArea()
		// opens the "Notifications" group and registers
		// overlay.notification_scale into it immediately before calling this
		// (2026-08-24) -- Area::Group() pushes a band rather than looking one
		// up, so opening it again here would draw a second, identically
		// titled header with these rows under it.

		// The user, 2026-08-24: "Notification position selection should look
		// the same as the system monitors placement". It is the same question
		// -- which of nine screen anchors -- so it is now the same control:
		// P3c's `Kind::Composite` / `CompositeKind::Anchor`, whose body the
		// shell draws with controls::AnchorGrid(). This was a nine-option
		// Choice, which the segmented helper had to downgrade to a dropdown
		// (nine long labels never fit a lane), so the two rows that ask the
		// identical question looked nothing alike.
		//
		// REUSED, NOT REIMPLEMENTED: no new kind, no new atom, and no second
		// grid. FpsDisplay.cpp's `monitor.anchor` and this row are two
		// declarations of the same kind, so they cannot drift apart about what
		// a 3x3 anchor looks like or how it is driven.
		//
		// NO MARGINS, unlike the monitor's. The Monitor's grid carries
		// `margin_v`/`margin_h` Params because FpsDisplaySettings has those
		// two fields; OverlaySettings has no notification-margin key, and
		// inventing one would be a config-schema change this task explicitly
		// forbids. CompositeValue()'s margin line is already conditional on
		// ParamCount() >= 2, so a grid with no Params renders correctly.
		//
		// Two bindings, one string. Each axis re-parses the stored
		// "top-right"-style value before composing, exactly as
		// FpsDisplay.cpp's pair does: the axes are a VIEW of the string, never
		// a second representation of it, so there is no half of the value that
		// can go stale. The on-disk key and its format are unchanged.
		area.Composite( "overlay.notification_placement", "Toast placement",
			ui::CompositeKind::Anchor,
			ui::AnyBind::Of<int>(
				[]() -> int
				{
					EnsureConfigLoaded();
					int nVert = 0, nHoriz = 2;
					ParsePlacement( s_GlobalOverlay.notification_placement, nVert, nHoriz );
					return nVert;
				},
				[]( int nVert )
				{
					EnsureConfigLoaded();
					int nOldVert = 0, nHoriz = 2;
					ParsePlacement( s_GlobalOverlay.notification_placement, nOldVert, nHoriz );
					s_GlobalOverlay.notification_placement = ComposePlacement( nVert, nHoriz );
					PersistPlacement();
				} ),
			ui::AnyBind::Of<int>(
				[]() -> int
				{
					EnsureConfigLoaded();
					int nVert = 0, nHoriz = 2;
					ParsePlacement( s_GlobalOverlay.notification_placement, nVert, nHoriz );
					return nHoriz;
				},
				[]( int nHoriz )
				{
					EnsureConfigLoaded();
					int nVert = 0, nOldHoriz = 2;
					ParsePlacement( s_GlobalOverlay.notification_placement, nVert, nOldHoriz );
					s_GlobalOverlay.notification_placement = ComposePlacement( nVert, nHoriz );
					PersistPlacement();
				} ) )
			.Help( "Which corner or edge toast notifications appear at. Always global -- shared by "
			       "every game, even one with its own config override, because a toast's position "
			       "is a property of the screen rather than of the game." )
			.Default( 0, 2 )                               // "top-right", the schema default
			.Keywords( "notification toast placement position corner anchor" );

		area.Switch( "notifications.muted", "Mute notifications",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.notifications.muted; },
				[]( bool bMuted )
				{
					s_Settings.notifications.muted = bMuted;
					PersistMuted();
				} ) )
			.Help( "Silences every toast. Unlike placement above, this follows the usual per-game "
			       "routing: with Override Global Config on it is saved for this game only." )
			.Default( false )
			.Keywords( "notification toast mute silence quiet" );

		area.Action( "notifications.test", "Test notification", "send",
			[]{ Show( "This is a test notification.", Kind::Info ); } )
			.Help( "Shows one toast right now, so placement can be judged where it actually "
			       "appears rather than from a diagram." )
			.DisabledUnless( []{ EnsureConfigLoaded(); return !s_Settings.notifications.muted; },
			                 "notifications are muted" )
			.Keywords( "notification toast test preview try" );
	}
}
