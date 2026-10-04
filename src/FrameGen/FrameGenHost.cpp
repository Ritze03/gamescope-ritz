// FrameGenHost.cpp -- see FrameGenHost.h for the model and the API contract.
//
// Layout of this file:
//   1. cross-thread state   (config, gate, frame request, reset flag, status)
//   2. render-thread state  (Host: the Interpolator, the ring, the outputs)
//   3. RecordBaseLayer()    (the one per-composite entry point)

#include "FrameGenHost.h"
#include "FrameGenFormat.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "../../subprojects/FrameGen/gpu/framegen.h"
#include "../../subprojects/FrameGen/gpu/pacing.h"

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "log.hpp"
#include "main.hpp"

static LogScope fg_log( "framegen" );

namespace fghost
{
	namespace
	{
		// -------------------------------------------------------------
		//  1. Cross-thread state
		// -------------------------------------------------------------

		// The user's config, packed into one word so a reader never sees a torn
		// mix of two SetConfig() calls. bits 0-3 multiplier, 4-5 quality, 6-7
		// safety, 8-9 hud protection, 10-11 mode, 12 priority, 13-22 target fps, 23 pause at refresh,
		// 24-25 UI protection, 26 enabled, 27 limit to GPU speed. The crosshair box size is
		// its own word, g_uUiBox.
		constexpr int kMinTargetFps = 30;
		constexpr int kMaxTargetFps = 1000;

		uint32_t Pack( const Config &c )
		{
			int nMult = c.multiplier < 2 ? 0 : std::min( c.multiplier, kMaxMultiplier );
			Mode eMode = c.mode == Mode::Target ? Mode::Target : Mode::Fixed;
			// A fixed multiplier below 2x is not a real choice: it is disabled.
			const bool bEnabled = c.enabled && !( eMode == Mode::Fixed && nMult < 2 );
			const int nTarget = c.targetFps <= 0 ? 0 : std::min( std::max( c.targetFps, kMinTargetFps ), kMaxTargetFps );
			return uint32_t( nMult )
				| ( uint32_t( c.quality ) << 4 )
				| ( uint32_t( c.safety ) << 6 )
				| ( uint32_t( c.hud ) << 8 )
				| ( uint32_t( eMode ) << 10 )
				| ( uint32_t( c.priority ) << 12 )
				| ( uint32_t( nTarget ) << 13 )
				| ( uint32_t( c.pauseAtRefresh ? 1 : 0 ) << 23 )
				| ( uint32_t( c.ui ) << 24 )
				| ( uint32_t( bEnabled ? 1 : 0 ) << 26 )
				| ( uint32_t( c.gpuLimit ? 1 : 0 ) << 27 );
		}

		// The crosshair box size, its own word (the main one is full): tenths of a
		// percent of the game's height.
		uint32_t PackBox( const Config &c )
		{
			return uint32_t( std::min( std::max( c.uiBoxTenths, kUiBoxMinTenths ), kUiBoxMaxTenths ) );
		}

		std::atomic<uint32_t> g_uUiBox{ uint32_t( kUiBoxDefaultTenths ) };

		// ---- the box preview (the Inspector's picture; see FrameGenHost.h) ----
		std::atomic<uint64_t> g_ulBoxPreviewWantedNs{ 0 };   // when the overlay last asked
		std::atomic<uint64_t> g_ulGameFrameSize{ 0 };        // (w << 32) | h of the last layer 0 the renderer saw
		std::mutex g_BoxPreviewMutex;
		BoxPreview g_BoxPreview;                             // guarded by g_BoxPreviewMutex
		std::atomic<uint64_t> g_ulBoxPreviewGeneration{ 0 };

		constexpr uint64_t kBoxPreviewLapseNs = 400ull * 1000ull * 1000ull;     // the overlay stopped asking
		constexpr uint64_t kBoxPreviewIntervalNs = 100ull * 1000ull * 1000ull;  // at most 10 captures a second

		// The one capture in flight (render thread only; the staging image is single).
		struct BoxPending_t
		{
			bool bPending = false;
			uint64_t ulSeq = 0;           // the submission that carries the crop
			uint64_t ulLastRecordNs = 0;
			BoxPreview meta;              // everything but the pixels, filled at record time
		};
		BoxPending_t g_BoxPending;

		Config Unpack( uint32_t u, uint32_t uBox )
		{
			Config c;
			c.uiBoxTenths = std::min( std::max( int( uBox ), kUiBoxMinTenths ), kUiBoxMaxTenths );
			c.multiplier = int( u & 0xFu );
			c.quality = Quality( ( u >> 4 ) & 0x3u );
			c.safety = Safety( ( u >> 6 ) & 0x3u );
			c.hud = HudProtect( ( u >> 8 ) & 0x3u );
			c.enabled = ( ( u >> 26 ) & 1u ) != 0;
			c.mode = Mode( ( u >> 10 ) & 0x3u );
			c.priority = Priority( ( u >> 12 ) & 0x1u );
			c.targetFps = int( ( u >> 13 ) & 0x3FFu );
			c.pauseAtRefresh = ( ( u >> 23 ) & 1u ) != 0;
			c.ui = UiProt( ( u >> 24 ) & 0x3u );
			c.gpuLimit = ( ( u >> 27 ) & 1u ) != 0;
			return c;
		}

		std::atomic<uint32_t> g_uConfig{ Pack( Config{} ) };

		// The motion-blur config, its own word (the frame generation word is full):
		// bits 0-3 samples, 4-10 amount percent, 11 relative (1 = game), 12 weights
		// (1 = gaussian), 13 enabled. Kept apart from g_uConfig so a blur edit never
		// reconfigures frame generation and vice versa; a reader that sees one
		// updated and not the other just gets the previous other half for one frame.
		constexpr uint32_t kBlurEnabledBit = 1u << 13;

		uint32_t PackBlur( const BlurConfig &b )
		{
			const int nSamples = std::min( std::max( b.samples, kMinBlurSamples ), kMaxBlurSamples );
			const int nAmount = std::min( std::max( b.amountPercent, 0 ), 100 );
			return uint32_t( nSamples )
				| ( uint32_t( nAmount ) << 4 )
				| ( uint32_t( b.relative == BlurRel::Game ? 1 : 0 ) << 11 )
				| ( uint32_t( b.weights == BlurWeighting::Gaussian ? 1 : 0 ) << 12 )
				| ( b.enabled ? kBlurEnabledBit : 0u );
		}

		BlurConfig UnpackBlur( uint32_t u )
		{
			BlurConfig b;
			b.samples = int( u & 0xFu );
			b.amountPercent = int( ( u >> 4 ) & 0x7Fu );
			b.relative = ( ( u >> 11 ) & 1u ) ? BlurRel::Game : BlurRel::Shown;
			b.weights = ( ( u >> 12 ) & 1u ) ? BlurWeighting::Gaussian : BlurWeighting::Even;
			b.enabled = ( u & kBlurEnabledBit ) != 0;
			return b;
		}

		std::atomic<uint32_t> g_uBlur{ PackBlur( BlurConfig{} ) };

		// The lag spike buffer's config, its own word (a third switch next to
		// frame generation and blur, independent of both): bit 0 enabled, bits 1-2
		// test mode, bits 3-6 look-back minutes (1..10), bits 7-14 max buffer ms
		// (0..250).
		constexpr uint32_t kLagEnabledBit = 1u;

		uint32_t PackLag( const LagBufferConfig &l )
		{
			const int nLook = std::min( std::max( l.lookbackMin, kMinLagLookbackMin ), kMaxLagLookbackMin );
			const int nMax = std::min( std::max( l.maxBufferMs, 0 ), kMaxLagBufferMs );
			const uint32_t uMode = std::min( uint32_t( l.testMode ), 2u );
			return ( l.enabled ? kLagEnabledBit : 0u ) | ( uMode << 1 ) | ( uint32_t( nLook ) << 3 ) | ( uint32_t( nMax ) << 7 );
		}

		LagBufferConfig UnpackLag( uint32_t u )
		{
			LagBufferConfig l;
			l.enabled = ( u & kLagEnabledBit ) != 0;
			l.testMode = LagTest( std::min( ( u >> 1 ) & 0x3u, 2u ) );
			l.lookbackMin = int( ( u >> 3 ) & 0xFu );
			l.maxBufferMs = int( ( u >> 7 ) & 0xFFu );
			return l;
		}

		std::atomic<uint32_t> g_uLag{ PackLag( LagBufferConfig{} ) };

		// The gate word. bit 0: frame generation OR motion blur OR the lag spike buffer is on. bit 1: the
		// render thread holds resources. RenderWanted() is `!= 0`, so with both off
		// and everything released the per-frame cost is this one load.
		constexpr uint32_t kGateEnabled = 1u << 0;
		constexpr uint32_t kGateLive = 1u << 1;
		std::atomic<uint32_t> g_uGate{ 0 };

		// Recomputes bit 0 after either config changed.
		void UpdateGate()
		{
			const bool bFg = ( ( g_uConfig.load( std::memory_order_relaxed ) >> 26 ) & 1u ) != 0;
			const bool bBlur = ( g_uBlur.load( std::memory_order_relaxed ) & kBlurEnabledBit ) != 0;
			const bool bLag = ( g_uLag.load( std::memory_order_relaxed ) & kLagEnabledBit ) != 0;
			if ( bFg || bBlur || bLag )
			{
				g_uGate.fetch_or( kGateEnabled, std::memory_order_relaxed );
			}
			else
			{
				const uint32_t uOld = g_uGate.fetch_and( ~kGateEnabled, std::memory_order_relaxed );
				// Switched off while holding resources: the release happens inside the
				// next vulkan_composite(), so make sure one comes (direct scanout may
				// resume the moment the backends stop forcing a full composite).
				if ( uOld & kGateLive )
					force_repaint();
			}
		}

		std::mutex g_FrameMutex;
		FrameRequest g_Frame;   // t < 0 (inert) until pacing drives the renderer

		std::atomic<bool> g_bResetRequested{ false };

		std::atomic<uint8_t> g_eReason{ uint8_t( Unavailable::Ok ) };
		std::atomic<const char *> g_pszFormatTag{ "" };   // string literals only (PlanTag)
		std::atomic<float> g_flLastPairGpuMs{ -1.0f };
		std::atomic<float> g_flLastEstimateMs{ -1.0f };
		std::atomic<float> g_flLastSynthMs{ -1.0f };
		std::atomic<float> g_flLastUiMs{ -1.0f };
		std::atomic<uint32_t> g_uCostSeq{ 0 };

		std::mutex g_PacingMutex;
		PacingStatus g_Pacing;

		// -------------------------------------------------------------
		//  2. Render-thread state
		// -------------------------------------------------------------

		// The ring and every output are in ONE format per FormatPlan
		// (FrameGenFormat.h), fixed for as long as the Interpolator lives:
		//   8-bit game   DRM_FORMAT_ARGB8888       (B8G8R8A8_UNORM)
		//   10-bit game  DRM_FORMAT_ABGR2101010    (A2B10G10R10_UNORM_PACK32)
		//   fp16 game    DRM_FORMAT_ABGR16161616F  (R16G16B16A16_SFLOAT)
		// whatever the game's own channel order is: cs_fg_copy.comp samples the
		// logical rgba and writes it, so order never matters and the library sees
		// one fixed viewFormat. A game that changes format class re-creates the
		// Interpolator (EnsureReady), after a waitIdle like every other teardown.
		uint32_t RingDrmFormat( const FormatPlan &plan )
		{
			return VulkanFormatToDRM( plan.eRingVk, true );
		}

		ShaderType CopyShader( const FormatPlan &plan )
		{
			switch ( plan.eFormat )
			{
				case framegen::Format::Rgb10:   return SHADER_TYPE_FG_COPY_RGB10;
				case framegen::Format::RgbaF16: return SHADER_TYPE_FG_COPY_F16;
				default:                        return SHADER_TYPE_FG_COPY;
			}
		}

		// What this device can do beyond 8-bit, decided once: the storage format
		// feature (rgb10_a2 needs shaderStorageImageExtendedFormats, enabled at device
		// creation only where offered) and sampled + linear filter + storage on the
		// ring format -- the same three the library's own init() checks, so a "no"
		// here is the same "no" it would give, with the reason known up front. The
		// library's shader variants are always built (src/meson.build).
		const FormatCaps &DeviceCaps()
		{
			static const FormatCaps caps = []
			{
				auto Usable = []( VkFormat eFormat )
				{
					VkFormatProperties props;
					g_device.vk.GetPhysicalDeviceFormatProperties( g_device.physDev(), eFormat, &props );
					constexpr VkFormatFeatureFlags uNeeded = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
						| VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
					return ( props.optimalTilingFeatures & uNeeded ) == uNeeded;
				};
				FormatCaps c;
				c.bRgb10 = g_device.supportsStorageImageExtendedFormats() && Usable( VK_FORMAT_A2B10G10R10_UNORM_PACK32 );
				c.bF16 = Usable( VK_FORMAT_R16G16B16A16_SFLOAT );
				fg_log.infof( "frame generation formats: 8-bit yes, 10-bit %s%s, fp16 %s",
					c.bRgb10 ? "yes" : "no",
					c.bRgb10 || g_device.supportsStorageImageExtendedFormats() ? "" : " (no shaderStorageImageExtendedFormats)",
					c.bF16 ? "yes" : "no" );
				return c;
			}();
			return caps;
		}

		// The ring holds the last real frames: three (framegen::pacing::kHistory)
		// normally -- Low latency only ever uses the newest pair, Smoothness may still
		// be inside the pair before it -- and up to kRingMax = 65 with a lag spike
		// buffer. It FOLLOWS the pacer's HistoryDepth() (FrameRequest::historyDepth),
		// which itself follows the buffer's size, not its cap (the user: "the amount of
		// frames should be dynamically be determined by the buffer size"):
		//   * GROW: at once, every paint it is wanted (the pacer asks ahead of the
		//     buffer's ramp), new empty slots at the OLDEST end so the depth is gained
		//     immediately. Allocation only, no GPU wait.
		//   * SHRINK: only after the wanted depth has stayed at least kRingShrinkSlack
		//     lower than the ring for kRingShrinkDwellNs (the library's own depth already
		//     dwells 2 s), by a deliberate g_device.waitIdle() -- the one place the
		//     host waits for the GPU for this -- which also lets the library free its
		//     unused UI-protection copies (releaseUnusedUi()). Why a wait: a freed slot
		//     may still be read by a composite in flight, and the library has no
		//     deferred free. Why a dwell + slack: so a wobbling depth does not stall.
		constexpr int kRingMin = framegen::pacing::kHistory;
		constexpr int kRingMax = framegen::pacing::kHistoryMax;
		constexpr int kRingShrinkSlack = 2;
		constexpr uint64_t kRingShrinkDwellNs = 4ull * 1000ull * 1000ull * 1000ull;

		// Pooled outputs. One composite shows one output; the next paint records
		// into another while the previous composite may still be sampling its own
		// (same queue, ordered by barriers), and one more keeps the last repaintable
		// output alive across a skipped vblank. Three is that: shown + next + spare.
		// (The old per-k pool needed up to 3 as well, for 4x.)
		constexpr int kOutPool = 3;

		// Generated frames per real pair, well under the library's 32-set
		// descriptor ring (FrameGen/Pacing.h's kMaxSynthsPerPair is the same
		// number; pacing already stops asking, this is the renderer's own guard).
		constexpr int kMaxSynthsPerPair = 24;

		struct OutSlot_t
		{
			gamescope::Rc<CVulkanTexture> pTex;
			uint64_t ulOutId = 0;
			bool bValid = false;
			uint64_t ulUse = 0;   // LRU clock
		};

		struct Host_t
		{
			framegen::Interpolator interp;
			bool bLive = false;
			uint32_t uWidth = 0, uHeight = 0;
			// The format the Interpolator, the ring and the outputs were created for
			// (valid while bLive, and for a failed init's key).
			FormatPlan plan;
			// The colourspace tag of the frames in the ring. A change (SDR <-> HDR10 on the
			// same 10-bit surface) makes the old frames a different picture: drop them.
			bool bHaveColorspace = false;
			GamescopeAppTextureColorspace eColorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

			// D11: the private ring. Real frames fill the slots in turn; the newest
			// is `nCurSlot`. Each slot remembers the commit id it holds (0 = empty),
			// which is how a pair is found: pacing names its frames by id. Nothing
			// here is ever a game buffer.
			gamescope::Rc<CVulkanTexture> pRing[ kRingMax ];
			uint64_t ulSlotId[ kRingMax ] = {};
			int nRing = 0;           // slots created (kRingMin..kRingMax); FOLLOWS the pacer's depth: grows at once, shrinks after a dwell
			// Ring shrink bookkeeping (the dwell before a deliberate waitIdle + free).
			uint64_t ulShrinkSinceNs = 0;   // when the wanted depth first stayed >= kRingShrinkSlack below nRing (0 = not shrinking)
			int nShrinkMaxWant = 0;         // the largest depth wanted since then
			int nGrowFailAt = 0;            // a ring grow to this depth failed (0 = none): not retried until the wanted depth drops below it
			// The library's Settings::uiHistory as applied (3 = default; follows the depth with UI protection on).
			uint32_t nUiHist = framegen::pacing::kHistory;
			int nUiGrowFailAt = 0;          // a uiHistory raise to this value failed (0 = none)
			int nCurSlot = 0;
			bool bHaveCur = false;   // pRing[nCurSlot] holds a valid real frame
			uint64_t ulCurId = 0;    // the commit id of pRing[nCurSlot]

			// D13: whether the library holds a motion estimate, and for which pair
			// (the library keeps exactly one: the latest), how many synths it has
			// served, and the cache of generated frames.
			bool bEstimateValid = false;
			uint64_t ulEstPrev = 0, ulEstCurr = 0;
			int nPairSynths = 0;
			OutSlot_t out[ kOutPool ];
			uint64_t ulUseClock = 0;

			// What the library is currently configured with.
			uint64_t uAppliedConfig = 0;
			// A failed init/reconfigure is not retried until the size or the
			// structural setting changes (or FG is switched off and on): init
			// builds pipelines, so retrying per frame would be a stutter machine.
			uint64_t ulFailedKey = 0;

			// D16: one open profile = one real pair (estimate + its synths).
			bool bProfiling = false;
			bool bProfileOpen = false;
			uint64_t ulProfileSeq = 0;   // submission sequence of the last cb that wrote into it
			int nProfileSynths = 0;
			int nProfileBlurSamples = 0;   // samples of the blurred outputs recorded in the open profile (0 = none)

			// UI protection (superdoc/features/frame-generation.md) is the FrameGen
			// library's own; the host only has to observe every new real frame
			// (recordObserve) and to keep the ring un-inpainted. Nothing of it is
			// stored here except bookkeeping for the timing harvest and the log-once.
			bool bEstContig = false;       // the open profile's estimate was recorded in the same command buffer as the observe before it
			bool bUiSetFailLogged = false; // setSettings() refused the UI settings once already (logged once)
			bool bUiEvictLogged = false;   // a recordSynth was refused with UI protection on (logged once)

			// The newest real frame (commit id) handed to the library's UI protection,
			// by recordObserve (generating) OR recordTrack (inert). Not cleared by
			// DropFrames(): that drops the host's ring, not the library's counters, and
			// a tracked frame must not be tracked again by a repaint of it.
			uint64_t ulUiFedId = 0;
			uint32_t uTrackedRun = 0;      // frames tracked since the last observe (the resume log)
			uint64_t ulTrackedTotal = 0;
			bool bTrackFailLogged = false; // a recordTrack was refused (logged once)
		};

		Host_t *g_pHost = nullptr;   // leaked on purpose: see HostGet()

		// Created on first use, never destroyed. Off = zero cost means the object
		// does not even exist until FG is first enabled; and not destroying it at
		// exit avoids running ~CVulkanTexture (which needs the device) during
		// static destruction, in whatever order that happens.
		Host_t &HostGet()
		{
			if ( !g_pHost )
				g_pHost = new Host_t();
			return *g_pHost;
		}

		framegen::Settings ToLibrarySettings( const Config &c, const BlurConfig &b, uint32_t nUiHistory )
		{
			framegen::Settings s;   // everything else stays at the library's approved defaults

			if ( c.quality == Quality::Performance )
			{
				s.flowScale = 4;
				s.subpixel = false;
			}
			else
			{
				s.flowScale = 2;
				s.subpixel = true;
			}

			// The whole-frame and scene-cut fallbacks keep the library's defaults
			// (0.15, 30) for every preset but Off, which disables all three
			// fallbacks (validate() accepts 254/255, 1.0 and 255).
			s.globalFallback = 0.15f;
			s.sceneCutSad = 30.0f;
			switch ( c.safety )
			{
				case Safety::Low:     s.trustLow = 16.0f; s.trustHigh = 56.0f; break;
				case Safety::Default: s.trustLow = 12.0f; s.trustHigh = 40.0f; break;
				case Safety::High:    s.trustLow = 8.0f;  s.trustHigh = 28.0f; break;
				case Safety::Off:
					s.trustLow = 254.0f; s.trustHigh = 255.0f;
					s.globalFallback = 1.0f;
					s.sceneCutSad = 255.0f;
					break;
			}

			switch ( c.hud )
			{
				case HudProtect::Off:    s.hudBonus = 0.0f; break;
				case HudProtect::Normal: s.hudBonus = 1.0f; break;
				case HudProtect::Strong: s.hudBonus = 2.5f; break;
			}

			// UI protection is the library's. The box side is the user's (a fraction
			// of the frame height); everything else (still frames, threshold, V2's
			// contrast / leave / re-entry, uiEstimateFill = false) stays at the
			// library's defaults.
			s.uiBoxHeightFrac = float( c.uiBoxTenths ) / 1000.0f;
			switch ( c.ui )
			{
				case UiProt::Off:         s.uiProtection = framegen::UiProtection::Off; break;
				case UiProt::Crosshair:   s.uiProtection = framegen::UiProtection::Crosshair; break;
				case UiProt::WholeScreen: s.uiProtection = framegen::UiProtection::WholeScreen; break;
				case UiProt::CrosshairV2: s.uiProtection = framegen::UiProtection::CrosshairV2; break;
			}

			// Motion blur: the sample count and weights (runtime, no wait). The window
			// and the cadence are the pacer's business; the library only averages.
			// 0 = off: recordSynthBlur then collapses to recordSynth at the window's end.
			s.blurSamples = b.enabled ? uint32_t( std::min( std::max( b.samples, kMinBlurSamples ), kMaxBlurSamples ) ) : 0u;
			s.blurWeights = b.weights == BlurWeighting::Even ? framegen::BlurWeights::Even : framegen::BlurWeights::Gaussian;
			// The deep UI-protection history follows the pacer's depth (SyncUiHistory);
			// a preset change must not reset it to the default.
			s.uiHistory = nUiHistory;
			return s;
		}

		void SetReason( Unavailable e )
		{
			g_eReason.store( uint8_t( e ), std::memory_order_relaxed );
		}

		void InvalidateOutputs( Host_t &H )
		{
			for ( OutSlot_t &o : H.out )
				o.bValid = false;
		}

		// Forget the previous frames and every cached output. Frees nothing, so no
		// wait is needed.
		void DropFrames( Host_t &H )
		{
			H.bHaveCur = false;
			H.ulCurId = 0;
			for ( uint64_t &id : H.ulSlotId )
				id = 0;
			H.bEstimateValid = false;
			H.nPairSynths = 0;
			InvalidateOutputs( H );
			// The library's stillness counters compare consecutive observed frames; a
			// gap here only costs the exact "N consecutive frames" (it compares the
			// frames on either side), so there is nothing to reset on its side.
		}

		// The ring slot holding commit `ulId`, or -1.
		int FindSlot( const Host_t &H, uint64_t ulId )
		{
			if ( !ulId )
				return -1;
			for ( int i = 0; i < H.nRing; i++ )
			{
				if ( H.ulSlotId[ i ] == ulId )
					return i;
			}
			return -1;
		}

		// Release everything. DECISION: g_device.waitIdle() first -- the library
		// has no deferred free, and earlier records (ours and the composite's
		// that sampled our outputs) may still be on the queue.
		void Teardown( Host_t &H )
		{
			g_device.waitIdle();   // WAITIDLE 1 of 3: teardown (switched off, or a failed init/resize/reconfigure)
			H.interp.destroy();    // also cleans up a half-built init; a no-op when never initialised
			for ( auto &p : H.pRing )
				p = nullptr;
			H.nRing = 0;
			H.ulShrinkSinceNs = 0;
			H.nGrowFailAt = 0;
			H.nUiHist = framegen::pacing::kHistory;
			H.nUiGrowFailAt = 0;
			for ( OutSlot_t &o : H.out )
				o.pTex = nullptr;
			H.bLive = false;
			H.uWidth = H.uHeight = 0;
			DropFrames( H );
			H.bProfileOpen = false;
			H.uAppliedConfig = 0;
			g_uGate.fetch_and( ~kGateLive, std::memory_order_relaxed );
			g_flLastPairGpuMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastEstimateMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastSynthMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastUiMs.store( -1.0f, std::memory_order_relaxed );
			g_pszFormatTag.store( "", std::memory_order_relaxed );
			H.bHaveColorspace = false;
			H.ulUiFedId = 0;   // the library's counters went with the Interpolator
			H.uTrackedRun = 0;
		}

		bool CreateTexture( gamescope::Rc<CVulkanTexture> &out, uint32_t w, uint32_t h, const FormatPlan &plan )
		{
			CVulkanTexture::createFlags flags;
			flags.bSampled = true;   // the library samples prev/curr; the composite samples outputs
			flags.bStorage = true;   // the copy shader / the library writes them

			gamescope::Rc<CVulkanTexture> pTex = new CVulkanTexture();
			if ( !pTex->BInit( w, h, 1u, RingDrmFormat( plan ), flags, nullptr ) )
				return false;
			out = std::move( pTex );
			return true;
		}

		// Brings the ring to nNew slots, KEEPING the newest frames in age order: slots
		// are rearranged by moving the Rc pointers (the textures themselves, and so the
		// library's cache keys -- their views -- never move), nCurSlot ends as nNew - 1
		// and the next real frame goes into slot 0 = the oldest. Growing puts the new
		// empty slots at the oldest end (the depth is gained immediately); shrinking
		// drops the oldest frames, which FREES their textures: the caller must have
		// waited for the GPU. False = a texture could not be created; nothing changed.
		bool ResizeRing( Host_t &H, int nNew )
		{
			nNew = std::min( std::max( nNew, kRingMin ), kRingMax );
			const int nOld = H.nRing;
			if ( nNew == nOld )
				return true;

			gamescope::Rc<CVulkanTexture> pTex[ kRingMax ];
			uint64_t ulId[ kRingMax ] = {};
			int n = 0;
			for ( ; n < nNew - nOld; n++ )
			{
				if ( !CreateTexture( pTex[ n ], H.uWidth, H.uHeight, H.plan ) )
					return false;   // never used by the GPU: freed at once by the locals
			}
			const int nCurEff = H.bHaveCur ? H.nCurSlot : nOld - 1;
			const int nKeep = std::min( nOld, nNew );
			for ( int i = nOld - nKeep; i < nOld; i++ )   // age index, 0 = oldest
			{
				const int nSrc = ( nCurEff + 1 + i ) % nOld;
				pTex[ n ] = std::move( H.pRing[ nSrc ] );
				ulId[ n ] = H.ulSlotId[ nSrc ];
				n++;
			}
			for ( int i = 0; i < kRingMax; i++ )
			{
				H.pRing[ i ] = i < nNew ? std::move( pTex[ i ] ) : nullptr;
				H.ulSlotId[ i ] = i < nNew ? ulId[ i ] : 0;
			}
			H.nRing = nNew;
			H.nCurSlot = nNew - 1;
			return true;
		}

		// The plan is part of the key: a 10-bit game that fails to start must not
		// block the 8-bit game the user switches to (or the other way round).
		uint64_t FailKey( uint32_t w, uint32_t h, const Config &c, const FormatPlan &plan )
		{
			return ( uint64_t( w ) << 32 ) ^ ( uint64_t( h ) << 8 ) ^ uint64_t( c.quality == Quality::Performance ? 4 : 2 )
				^ ( uint64_t( plan.eFormat ) << 50 ) ^ ( uint64_t( plan.eTransfer ) << 53 ) ^ 0x8000000000000000ull;
		}

		// Brings the library to (w, h, cfg): init the first time, resize on a size
		// change, apply a changed preset. False = not usable this frame.
		bool EnsureReady( Host_t &H, uint32_t w, uint32_t h, const FormatPlan &plan, const Config &c, uint32_t uPackedConfig, const BlurConfig &b, uint32_t uPackedBlur )
		{
			const uint64_t ulKey = FailKey( w, h, c, plan );
			if ( H.ulFailedKey == ulKey )
				return false;

			// The game changed format class (8-bit <-> 10-bit <-> fp16, or fp16 scRGB <->
			// fp16 encoded): the library's pipelines belong to ONE format (resize() cannot
			// change it), so start over. Teardown() waits for the GPU first.
			if ( H.bLive && H.plan != plan )
			{
				fg_log.infof( "frame generation: the game's format changed; restarting the interpolator" );
				Teardown( H );
			}

			const framegen::Settings want = ToLibrarySettings( c, b, H.nUiHist );

			// Only the library's own presets (quality, safety, HUD protection, UI
			// protection, and the blur's sample count and weights) matter to what is
			// already applied; mode, priority, target and the blur's amount / relative
			// are pacing's business and never reconfigure it. The blur sample count is
			// 0 while the blur is off, so its enabled bit is part of the key.
			const uint32_t uBlurLib = uPackedBlur & ( 0xFu | ( 1u << 12 ) | kBlurEnabledBit );
			const uint64_t uLibraryConfig = uint64_t( uPackedConfig & ( 0x3F0u | ( 0x3u << 24 ) ) ) | ( uint64_t( uBlurLib ) << 32 )
				| ( uint64_t( c.uiBoxTenths ) << 48 );

			if ( !H.bLive )
			{
				framegen::InitInfo info;
				info.phys = g_device.physDev();
				info.device = g_device.device();
				info.gdpa = g_device.vk.GetDeviceProcAddr;
				info.width = w;
				info.height = h;
				info.viewFormat = plan.eRingVk;
				info.getMemProps = g_device.vk.GetPhysicalDeviceMemoryProperties;
				// Lets the library refuse a format this device cannot use WITH a reason
				// (never consulted for Rgba8, so the 8-bit path is unchanged).
				info.getFormatProps = g_device.vk.GetPhysicalDeviceFormatProperties;
				info.transfer = plan.eTransfer;
				info.linearWhiteNits = plan.flLinearWhiteNits;
				info.pipelineCache = VK_NULL_HANDLE;
				info.settings = want;

				H.plan = plan;
				H.uWidth = w;
				H.uHeight = h;
				if ( !H.interp.init( info ) || !ResizeRing( H, kRingMin ) )
				{
					fg_log.errorf( "frame generation unavailable: could not create the interpolator or its frame ring at %ux%u (format %u): %s", w, h,
						unsigned( plan.eFormat ), H.interp.initError() ? H.interp.initError() : "see the log above" );
					Teardown( H );
					H.ulFailedKey = ulKey;
					return false;
				}

				H.bLive = true;
				g_uGate.fetch_or( kGateLive, std::memory_order_relaxed );
				H.uAppliedConfig = uLibraryConfig;
				H.bProfiling = g_device.supportsTimestamps() && H.interp.setProfiling( true );
				fg_log.infof( "frame generation ready: %ux%u, flowScale %u, GPU timing %s, format %s%s",
					w, h, want.flowScale, H.bProfiling ? "on" : "n/a",
					plan.eFormat == framegen::Format::Rgb10 ? "10-bit"
						: plan.eFormat == framegen::Format::RgbaF16
							? ( plan.eTransfer == framegen::Transfer::Linear ? "fp16 linear (scRGB)" : "fp16 encoded" ) : "8-bit",
					ColorspaceIsHDR( H.eColorspace ) ? ", HDR layer" : "" );
				return true;
			}

			if ( w != H.uWidth || h != H.uHeight )
			{
				// A resolution change. resize() frees buffers earlier records still use.
				g_device.waitIdle();   // WAITIDLE 2 of 3: size change
				H.uWidth = w;
				H.uHeight = h;
				DropFrames( H );
				for ( auto &p : H.pRing )
					p = nullptr;
				H.nRing = 0;
				for ( OutSlot_t &o : H.out )
					o.pTex = nullptr;
				if ( !H.interp.resize( w, h, plan.eRingVk ) || !ResizeRing( H, kRingMin ) )
				{
					fg_log.errorf( "frame generation unavailable: could not resize to %ux%u", w, h );
					Teardown( H );
					H.ulFailedKey = ulKey;
					return false;
				}
				// resize() keeps the library's active settings; the preset check below
				// applies a pending change.
			}

			if ( uLibraryConfig != H.uAppliedConfig )
			{
				if ( want.flowScale != H.interp.settings().flowScale )
				{
					// Structural (Quality <-> Performance): reconfigure() re-creates the
					// size-dependent buffers, so every earlier record must have retired,
					// and the next synth needs a new estimate.
					g_device.waitIdle();   // WAITIDLE 3 of 3: Quality preset change
					if ( !H.interp.reconfigure( want ) )
					{
						fg_log.errorf( "frame generation unavailable: could not reconfigure (flowScale %u)", want.flowScale );
						Teardown( H );
						H.ulFailedKey = ulKey;
						return false;
					}
					H.bEstimateValid = false;
					InvalidateOutputs( H );
					H.bProfileOpen = false;
				}
				else
				{
					// Runtime change: every record after this uses the new values; nothing
					// in flight changes, so no wait. A refusal keeps the old settings; it is
					// logged once and not retried every frame (the next change retries).
					if ( !H.interp.setSettings( want ) && !H.bUiSetFailLogged )
					{
						H.bUiSetFailLogged = true;
						fg_log.errorf( "frame generation: the library refused the new settings; keeping the old ones" );
					}
				}
				H.uAppliedConfig = uLibraryConfig;
			}
			return true;
		}

		// D16. Reads the open profile if (and only if) the submissions that wrote
		// it have retired -- the timeline semaphore's counter says so without
		// waiting. Never calls readTimings() on an unretired profile: that call
		// waits (VK_QUERY_RESULT_WAIT_BIT) and would stall the compositor.
		//
		// A pair's profile holds a VARIABLE number of synths now, so it is read
		// once the pair is over: when the next estimate OR observe is about to reset
		// the query pool (the library resets it at an observe too), or when this
		// composite is not still playing the profile's pair. (An unretired profile
		// is dropped at that point rather than kept: it would be read
		// half-overwritten.)
		//
		// Published: the estimate's time (the observe's UI cost included: it is per
		// real frame too), the mean time of one synth (ui_patch included), and their
		// sum for the synths profiled -- the cost guard budgets from the first two.
		// Also the UI share on its own (g_flLastUiMs), for the status line.
		void HarvestProfile( Host_t &H, bool bAboutToRecordEstimate, bool bPairStillPlaying )
		{
			if ( !H.bProfileOpen )
				return;
			if ( !bAboutToRecordEstimate && ( bPairStillPlaying || H.nProfileSynths == 0 ) )
				return;   // more synths of this pair may still be recorded into it

			const uint64_t ulDone = g_device.completedSeqNo();
			if ( ulDone < H.ulProfileSeq )
			{
				if ( bAboutToRecordEstimate )
					H.bProfileOpen = false;
				return;
			}

			H.bProfileOpen = false;

			std::vector<const char *> names;
			std::vector<uint64_t> ticks;
			if ( !H.interp.readTimings( names, ticks ) || names.size() != ticks.size() )
				return;

			// The profile is a chain of timestamps, one per pass group, each delta being
			// "time since the previous stamp". What can be in it, in order (the library
			// resets the pool at an observe only when no profile is waiting for an
			// estimate, and at an estimate only when no observe opened one):
			//   start, [ui_detect, ui_inpaint]*      one pair per observed real frame
			//   luma .. pixel field                  the motion estimate
			//   [lookup,] mismatch, resolve, fallback[, ui_patch]    one per synth
			//   blur lookup, blur mismatch, blur resolve, blur fixup[, ui_patch]
			//                                        one per BLURRED output: counted as
			//                                        its sample count (H.nProfileBlurSamples),
			//                                        so "one synth" stays the cost of one
			//                                        sample and pacing's blurSamples x synth
			//                                        budget (pacing.h's cost guard) is right
			// A delta is only usable when the previous stamp was recorded straight
			// before it. Idle time (up to a frame) hides in the ones that are not, and
			// those are dropped -- an under-count of a pass (~0.01-0.05 ms) instead of
			// an over-count of ~16 ms:
			//   * ui_detect unless it directly follows `start` (an observe that was
			//     chained after an earlier one's ui_inpaint, or after a synth);
			//   * luma behind an ui_inpaint, unless the estimate was recorded in that
			//     observe's own command buffer (H.bEstContig);
			//   * the first stamp of a synth, unless it directly follows the estimate
			//     (the first synth is recorded in the estimate's command buffer).
			uint64_t ulEstTicks = 0, ulSynthTicks = 0;
			uint64_t ulDetect = 0, ulInpaint = 0, ulPatch = 0;
			int nSynth = 0, nBlurOutputs = 0, nDetect = 0, nInpaint = 0, nPatch = 0;
			bool bHaveEst = false, bInEst = false;
			const char *pszPrev = "";
			for ( size_t i = 0; i < names.size(); i++ )
			{
				const char *pszName = names[i];
				const bool bObserve = !strcmp( pszName, "ui_detect" ) || !strcmp( pszName, "ui_inpaint" );
				const bool bSynthStart = !bObserve
					&& ( !strcmp( pszName, "lookup" ) || !strcmp( pszName, "blur lookup" )
						|| ( !strcmp( pszName, "mismatch" ) && strcmp( pszPrev, "lookup" ) != 0 ) );
				const bool bLuma = !strcmp( pszName, "luma" );
				const bool bAfterObserve = !strcmp( pszPrev, "ui_inpaint" );
				pszPrev = pszName;

				if ( bObserve )
				{
					bInEst = false;
					if ( !strcmp( pszName, "ui_detect" ) )
					{
						if ( i == 0 ) { ulDetect += ticks[i]; nDetect++; }
					}
					else
					{
						ulInpaint += ticks[i];
						nInpaint++;
					}
					continue;
				}
				if ( bLuma )
				{
					bHaveEst = true;
					bInEst = true;
					if ( bAfterObserve && !H.bEstContig )
						continue;
				}
				if ( bSynthStart )
				{
					nSynth++;
					if ( !strcmp( pszName, "blur lookup" ) )
						nBlurOutputs++;
					const bool bContiguous = bInEst;
					bInEst = false;
					if ( !bContiguous )
						continue;
				}
				if ( !strcmp( pszName, "ui_patch" ) )
				{
					ulPatch += ticks[i];
					nPatch++;
				}
				if ( nSynth == 0 )
				{
					if ( bInEst )
						ulEstTicks += ticks[i];
				}
				else
				{
					ulSynthTicks += ticks[i];
				}
			}

			const double flToMs = double( g_device.timestampPeriodNs() ) * 1e-6;
			const double flObserveMs = ( nDetect ? double( ulDetect ) * flToMs / nDetect : 0.0 )
				+ ( nInpaint ? double( ulInpaint ) * flToMs / nInpaint : 0.0 );
			const double flPatchMs = nPatch ? double( ulPatch ) * flToMs / nPatch : 0.0;
			// A wrapped counter (timestampValidBits < 64) would show up as an absurd
			// value; keep the previous reading instead.
			if ( flObserveMs < 0.0 || flObserveMs > 1000.0 || flPatchMs < 0.0 || flPatchMs > 1000.0 )
				return;
			if ( nDetect + nInpaint + nPatch > 0 )
				g_flLastUiMs.store( float( flObserveMs + flPatchMs ), std::memory_order_relaxed );

			// A blurred output is nProfileBlurSamples synths' worth of work (one more
			// sample than it costs when its window ends on the real frame: pacing.h's
			// "conservative count").
			const int nSynthEquiv = nSynth - nBlurOutputs + nBlurOutputs * std::max( H.nProfileBlurSamples, 1 );
			const double flSynthMs = nSynth > 0 ? double( ulSynthTicks ) * flToMs / double( nSynthEquiv ) : -1.0;
			if ( bHaveEst )
			{
				// The observe is per real frame, like the estimate: it belongs to the
				// estimate's share, so the cost guard and "FG x ms" include it.
				const double flEstMs = double( ulEstTicks ) * flToMs + flObserveMs;
				const double flPairMs = flEstMs + double( ulSynthTicks ) * flToMs;
				if ( flPairMs < 0.0 || flPairMs > 1000.0 || flEstMs < 0.0 )
					return;
				g_flLastPairGpuMs.store( float( flPairMs ), std::memory_order_relaxed );
				g_flLastEstimateMs.store( float( flEstMs ), std::memory_order_relaxed );
			}
			else if ( flSynthMs < 0.0 )
			{
				return;   // an observe-only profile: nothing for the cost guard
			}
			// (A profile without an estimate -- the older pair's synths while a newer
			// observe reset the pool -- still tells the synth cost.)
			if ( flSynthMs >= 0.0 )
				g_flLastSynthMs.store( float( flSynthMs ), std::memory_order_relaxed );
			g_uCostSeq.fetch_add( 1, std::memory_order_relaxed );

			// One debug line per ~100 readings (`gamescopectl log_framegen debug`), so
			// a run can be checked without the overlay: what each share costs.
			static uint32_t s_uLogCount = 0;
			if ( ( s_uLogCount++ % 100 ) == 0 )
				fg_log.debugf( "timing: estimate %.3f ms (observe %.3f), synth %.3f ms (patch %.3f), %d synth(s) in the profile, %d blurred output(s) x %d samples",
					g_flLastEstimateMs.load( std::memory_order_relaxed ), flObserveMs,
					g_flLastSynthMs.load( std::memory_order_relaxed ), flPatchMs, nSynth, nBlurOutputs, H.nProfileBlurSamples );
		}

		// ---- the box preview: record (render thread) and publish ----
		// The box as the library places it (Interpolator::computeBox): centred, the
		// preview never uses an offset.
		void BoxRect( uint32_t w, uint32_t h, int nTenths, uint32_t *pbw, uint32_t *pbh, int *pbx, int *pby )
		{
			framegen::Settings st;
			st.uiBoxHeightFrac = float( nTenths ) / 1000.0f;
			framegen::Interpolator::boxSize( st, w, h, pbw, pbh );
			*pbx = ( int( w ) - int( *pbw ) ) / 2;
			*pby = ( int( h ) - int( *pbh ) ) / 2;
		}

		// Records the crop pass for a NEW real frame into pCb, when the overlay wants
		// it, none is in flight and the last was >= 100 ms ago. Returns true when it
		// recorded: the caller submits pCb and then calls BoxPreviewSubmitted(seq).
		bool BoxPreviewRecord( CVulkanCmdBuffer *pCb, const gamescope::Rc<CVulkanTexture> &pLayer0, const FormatPlan &plan, const Config &cfg )
		{
			const uint64_t ulNow = get_time_in_nanos();
			const uint64_t ulWanted = g_ulBoxPreviewWantedNs.load( std::memory_order_relaxed );
			if ( !ulWanted || ulNow - ulWanted > kBoxPreviewLapseNs )
				return false;
			// A capture that never retired (its command buffer was dropped) must not
			// wedge the preview: give up on it after two seconds.
			if ( g_BoxPending.bPending && ulNow - g_BoxPending.ulLastRecordNs > 2000ull * 1000ull * 1000ull )
				g_BoxPending.bPending = false;
			if ( g_BoxPending.bPending || ulNow - g_BoxPending.ulLastRecordNs < kBoxPreviewIntervalNs )
				return false;

			const uint32_t w = pLayer0->width(), h = pLayer0->height();
			uint32_t bw, bh;
			int bx, by;
			BoxRect( w, h, cfg.uiBoxTenths, &bw, &bh, &bx, &by );

			// The crop: the box with its own size again around it (a quarter of the box
			// on every side is the least that shows what the box misses), at least 48
			// px, never more than the frame. Reduced by a whole factor when it would
			// not fit the picture, so large boxes on large frames stay exact means.
			const uint32_t uBoxMax = std::max( bw, bh );
			uint32_t uSide = std::min( std::max( uBoxMax * 2u, 48u ), std::min( w, h ) );
			const uint32_t uFactor = ( uSide + kBoxPreviewMax - 1u ) / kBoxPreviewMax;
			const uint32_t uOut = ( uSide + uFactor - 1u ) / uFactor;
			const uint32_t uSrcSide = uOut * uFactor;
			const int nCx = bx + int( bw ) / 2, nCy = by + int( bh ) / 2;
			const int nX0 = std::min( std::max( nCx - int( uSrcSide ) / 2, 0 ), std::max( int( w ) - int( uSrcSide ), 0 ) );
			const int nY0 = std::min( std::max( nCy - int( uSrcSide ) / 2, 0 ), std::max( int( h ) - int( uSrcSide ), 0 ) );

			UiCropRequest_t req;
			req.uOriginX = uint32_t( nX0 );
			req.uOriginY = uint32_t( nY0 );
			req.uWidth = uOut;
			req.uHeight = uOut;
			req.uFactor = uFactor;
			req.bLinearToSrgb = plan.eFormat == framegen::Format::RgbaF16 && plan.eTransfer == framegen::Transfer::Linear;
			if ( !vulkan_ui_crop_record( pCb, pLayer0, req ) )
				return false;

			BoxPreview &m = g_BoxPending.meta;
			m.uGameW = w;
			m.uGameH = h;
			m.uBoxW = bw;
			m.uBoxH = bh;
			m.uW = uOut;
			m.uH = uOut;
			m.uFactor = uFactor;
			m.flBoxX = float( bx - nX0 ) / float( uFactor );
			m.flBoxY = float( by - nY0 ) / float( uFactor );
			m.flBoxW = float( bw ) / float( uFactor );
			m.flBoxH = float( bh ) / float( uFactor );
			m.bUiOn = cfg.ui != UiProt::Off;
			g_BoxPending.ulLastRecordNs = ulNow;
			g_BoxPending.bPending = true;
			g_BoxPending.ulSeq = ~0ull;   // set by BoxPreviewSubmitted(); never read as retired before that
			return true;
		}

		void BoxPreviewSubmitted( uint64_t ulSeq )
		{
			g_BoxPending.ulSeq = ulSeq;
		}

		// Reads the crop once the submission that carries it has retired (a counter
		// read, never a wait) and publishes it.
		void BoxPreviewPoll()
		{
			if ( !g_BoxPending.bPending || g_BoxPending.ulSeq == ~0ull || g_device.completedSeqNo() < g_BoxPending.ulSeq )
				return;
			g_BoxPending.bPending = false;

			std::lock_guard<std::mutex> lock( g_BoxPreviewMutex );
			const BoxPreview &m = g_BoxPending.meta;
			if ( !vulkan_ui_crop_read( g_BoxPreview.rgba, m.uW, m.uH ) )
				return;
			g_BoxPreview.uGameW = m.uGameW;
			g_BoxPreview.uGameH = m.uGameH;
			g_BoxPreview.uBoxW = m.uBoxW;
			g_BoxPreview.uBoxH = m.uBoxH;
			g_BoxPreview.uW = m.uW;
			g_BoxPreview.uH = m.uH;
			g_BoxPreview.uFactor = m.uFactor;
			g_BoxPreview.flBoxX = m.flBoxX;
			g_BoxPreview.flBoxY = m.flBoxY;
			g_BoxPreview.flBoxW = m.flBoxW;
			g_BoxPreview.flBoxH = m.flBoxH;
			g_BoxPreview.bUiOn = m.bUiOn;
			g_BoxPreview.ulCapturedNs = get_time_in_nanos();
			// Published last: GetBoxPreview() checks the generation before taking the lock.
			g_BoxPreview.ulGeneration = g_ulBoxPreviewGeneration.load( std::memory_order_relaxed ) + 1;
			g_ulBoxPreviewGeneration.store( g_BoxPreview.ulGeneration, std::memory_order_release );
		}

		// UI protection while the renderer is inert (pass-through): the game is fast
		// enough that frame generation is paused, so no frame is copied, observed or
		// shown by us -- but the library's stillness counters must keep following the
		// real frames, or a crosshair / HUD that changed meanwhile restarts cold when
		// generation resumes and shifts around. The user: "make sure that it still
		// keeps track of like static UI elements, like the crosshair, even when it's
		// temporarily deactivated because the FPS is high enough ... so it doesn't
		// start shifting around once it starts generating frames again."
		//
		// recordTrack() (frame-gen-ritz PR #9) is the counter update alone: ~0.003 ms
		// in Crosshair mode, ~0.03 ms in WholeScreen, no clean copy, no ring copy.
		// It samples the GAME's texture directly (raw UNORM view = the encoded values
		// the copy shader would have put in the ring; the library reads rgb only).
		//
		// Once per NEW real frame, and only when the library is live for this layer:
		// a command buffer is built and submitted only then, never an empty one.
		// Never for a frame the library was already handed (see ShouldTrackUi()).
		// No Interpolator is created for it: a pass-through only follows a warm-up
		// (pacing drives the renderer, and so EnsureReady(), until its estimate
		// is valid), so "not live" means generation never got going and there is
		// no state to keep warm -- a lazy init would only buy pipeline-build stalls.
		void TrackWhileInert( Host_t &H, const gamescope::Rc<CVulkanTexture> &pLayer0, GamescopeAppTextureColorspace eColorspace, const Config &cfg, uint64_t ulNewestId )
		{
			// The cheap rejections first: this runs on every inert composite.
			if ( cfg.ui == UiProt::Off || !H.bLive || ulNewestId == 0 || ulNewestId == H.ulUiFedId )
				return;

			FormatPlan plan;
			const Unavailable eWhy = ClassifyLayer( pLayer0->format(), pLayer0->isYcbcr(), eColorspace, DeviceCaps(), &plan );
			const bool bFits = LayerFitsHost( eWhy, plan, H.plan, pLayer0->width(), pLayer0->height(), H.uWidth, H.uHeight,
				H.bHaveColorspace && H.eColorspace == eColorspace );
			if ( !ShouldTrackUi( true, H.bLive, bFits, ulNewestId, H.ulUiFedId ) )
				return;

			H.ulUiFedId = ulNewestId;

			std::unique_ptr<CVulkanCmdBuffer> pCmd = g_device.commandBuffer();
			H.interp.beginFrame();   // once per command buffer, before its first record
			// Hold the game's texture for the command buffer's life, import it from the
			// client (queue-family acquire) and put it in GENERAL: the library samples it
			// in that layout. The same steps the ring copy's dispatch performs.
			pCmd->bindTexture( 0, pLayer0 );
			pCmd->prepareSrcImage( pLayer0.get() );
			pCmd->insertBarrier();
			pCmd->bindTexture( 0, nullptr );
			const bool bOk = H.interp.recordTrack( pCmd->rawBuffer(), pLayer0->srgbView() );
			const bool bPreview = BoxPreviewRecord( pCmd.get(), pLayer0, plan, cfg );
			const uint64_t ulSeq = g_device.submit( std::move( pCmd ) );
			if ( bPreview )
				BoxPreviewSubmitted( ulSeq );

			if ( bOk )
			{
				H.uTrackedRun++;
				H.ulTrackedTotal++;
				if ( ( H.ulTrackedTotal % 120 ) == 1 )
					fg_log.debugf( "UI protection: tracking while passing through (%llu real frames tracked so far)", (unsigned long long)H.ulTrackedTotal );
			}
			else if ( !H.bTrackFailLogged )
			{
				H.bTrackFailLogged = true;
				fg_log.warnf( "UI protection: the library refused recordTrack while passing frames through (a descriptor ring was full, or its first-use allocation failed)" );
			}
		}

		// Makes the host's ring and the library's UI-protection history follow the
		// pacer's depth (see the ring comment above). Called once per active paint.
		void FollowDepth( Host_t &H, int nDepth, bool bUi )
		{
			const int nWant = std::min( std::max( nDepth, kRingMin ), kRingMax );

			// ---- the real-frame ring ----
			if ( nWant > H.nRing )
			{
				H.ulShrinkSinceNs = 0;
				if ( H.nGrowFailAt == 0 || nWant < H.nGrowFailAt )
				{
					H.nGrowFailAt = 0;
					if ( ResizeRing( H, nWant ) )
						fg_log.debugf( "frame ring: grown to %d frames (%ux%u)", H.nRing, H.uWidth, H.uHeight );
					else
					{
						// Not fatal: a pair or a delayed frame that is not in the ring just
						// shows the newest real frame. Not retried until the depth drops.
						H.nGrowFailAt = nWant;
						fg_log.errorf( "frame generation: could not grow the frame ring to %d frames; keeping %d", nWant, H.nRing );
					}
				}
			}
			else if ( nWant <= H.nRing - kRingShrinkSlack )
			{
				H.nGrowFailAt = 0;
				const uint64_t ulNow = get_time_in_nanos();
				if ( !H.ulShrinkSinceNs )
				{
					H.ulShrinkSinceNs = ulNow;
					H.nShrinkMaxWant = nWant;
				}
				else
				{
					H.nShrinkMaxWant = std::max( H.nShrinkMaxWant, nWant );
					if ( ulNow - H.ulShrinkSinceNs >= kRingShrinkDwellNs )
					{
						// The deliberate wait: freeing a slot a composite in flight may
						// still read needs the GPU idle, and so does releaseUnusedUi().
						g_device.waitIdle();   // WAITIDLE 4 of 4: the ring shrank after a dwell (lag spike buffer)
						const int nWas = H.nRing;
						ResizeRing( H, H.nShrinkMaxWant );   // shrinking cannot fail
						if ( H.bEstimateValid && ( FindSlot( H, H.ulEstPrev ) < 0 || FindSlot( H, H.ulEstCurr ) < 0 ) )
							H.bEstimateValid = false;
						H.interp.releaseUnusedUi();
						H.ulShrinkSinceNs = 0;
						fg_log.debugf( "frame ring: shrunk from %d to %d frames", nWas, H.nRing );
					}
				}
			}
			else
			{
				H.ulShrinkSinceNs = 0;
				H.nGrowFailAt = 0;
			}

			// ---- the library's UI-protection history (Settings::uiHistory) ----
			// With protection off it is irrelevant: keep the default (a raise would
			// allocate clean copies nobody uses).
			const uint32_t uUi = bUi ? uint32_t( nWant ) : uint32_t( framegen::pacing::kHistory );
			if ( uUi != H.nUiHist )
			{
				if ( uUi > H.nUiHist && H.nUiGrowFailAt != 0 && int( uUi ) >= H.nUiGrowFailAt )
					return;   // a raise to this much failed already; wait for the depth to drop
				framegen::Settings st = H.interp.settings();
				st.uiHistory = uUi;
				// Growing allocates without a GPU wait (false, nothing changed, on failure);
				// lowering frees nothing (releaseUnusedUi(), above, does that).
				if ( H.interp.setSettings( st ) )
				{
					H.nUiHist = uUi;
					H.nUiGrowFailAt = 0;
				}
				else if ( uUi > H.nUiHist )
				{
					H.nUiGrowFailAt = int( uUi );
					fg_log.errorf( "frame generation: could not raise the UI-protection history to %u frames; keeping %u", uUi, H.nUiHist );
				}
			}
			else
			{
				H.nUiGrowFailAt = 0;
			}
		}

		// The pool slot an output is to be recorded into: a free one, else the
		// least recently used (the one just shown has the newest use, so it is safe).
		OutSlot_t *PickOutSlot( Host_t &H )
		{
			OutSlot_t *pBest = nullptr;
			for ( OutSlot_t &o : H.out )
			{
				if ( !o.bValid )
					return &o;
				if ( !pBest || o.ulUse < pBest->ulUse )
					pBest = &o;
			}
			return pBest;
		}
	}

	// ------------------------------------------------------------------
	//  Public API: config, gate, frame request, reset, status
	// ------------------------------------------------------------------

	void SetConfig( const Config &cfg )
	{
		const uint32_t uPacked = Pack( cfg );
		g_uConfig.store( uPacked, std::memory_order_relaxed );
		g_uUiBox.store( PackBox( cfg ), std::memory_order_relaxed );
		UpdateGate();
	}

	void BoxPreviewWanted()
	{
		g_ulBoxPreviewWantedNs.store( get_time_in_nanos(), std::memory_order_relaxed );
	}

	bool GetBoxPreview( BoxPreview *pOut, uint64_t ulHaveGeneration )
	{
		if ( !pOut || g_ulBoxPreviewGeneration.load( std::memory_order_acquire ) <= ulHaveGeneration )
			return false;
		std::lock_guard<std::mutex> lock( g_BoxPreviewMutex );
		if ( g_BoxPreview.ulGeneration <= ulHaveGeneration )
			return false;
		*pOut = g_BoxPreview;
		return true;
	}

	bool GameFrameSize( uint32_t *puWidth, uint32_t *puHeight )
	{
		const uint64_t ul = g_ulGameFrameSize.load( std::memory_order_relaxed );
		if ( !ul )
			return false;
		*puWidth = uint32_t( ul >> 32 );
		*puHeight = uint32_t( ul & 0xFFFFFFFFu );
		return true;
	}

	void UiBoxPixels( uint32_t uWidth, uint32_t uHeight, int nTenths, uint32_t *puBoxW, uint32_t *puBoxH )
	{
		int bx, by;
		BoxRect( uWidth, uHeight, std::min( std::max( nTenths, kUiBoxMinTenths ), kUiBoxMaxTenths ), puBoxW, puBoxH, &bx, &by );
	}

	void SetBlurConfig( const BlurConfig &cfg )
	{
		g_uBlur.store( PackBlur( cfg ), std::memory_order_relaxed );
		UpdateGate();
	}

	BlurConfig GetBlurConfig()
	{
		return UnpackBlur( g_uBlur.load( std::memory_order_relaxed ) );
	}

	void SetLagBufferConfig( const LagBufferConfig &cfg )
	{
		g_uLag.store( PackLag( cfg ), std::memory_order_relaxed );
		UpdateGate();
	}

	LagBufferConfig GetLagBufferConfig()
	{
		return UnpackLag( g_uLag.load( std::memory_order_relaxed ) );
	}

	Config GetConfig()
	{
		return Unpack( g_uConfig.load( std::memory_order_relaxed ), g_uUiBox.load( std::memory_order_relaxed ) );
	}

	bool Enabled()
	{
		return ( ( g_uConfig.load( std::memory_order_relaxed ) >> 26 ) & 1u ) != 0;
	}

	bool Active()
	{
		return ( g_uGate.load( std::memory_order_relaxed ) & kGateEnabled ) != 0;
	}

	bool RenderWanted()
	{
		return g_uGate.load( std::memory_order_relaxed ) != 0;
	}

	void SetFrame( const FrameRequest &request )
	{
		std::lock_guard<std::mutex> lock( g_FrameMutex );
		g_Frame = request;
	}

	void Reset()
	{
		g_bResetRequested.store( true, std::memory_order_relaxed );
	}

	const char *UnavailableText( Unavailable eReason )
	{
		switch ( eReason )
		{
			case Unavailable::Ok:        return "";
			case Unavailable::Hdr:         return "Not available for this HDR pass-through surface";
			case Unavailable::YCbCr:       return "Not available for video (YCbCr) surfaces";
			case Unavailable::Format:      return "Not available for this game pixel format";
			case Unavailable::HdrFormat:   return "HDR / 10-bit format not supported by this GPU";
			case Unavailable::TooSmall:    return "Game resolution is too small";
			case Unavailable::InitFailed:  return "Could not start on this GPU";
			case Unavailable::RecordFailed:return "A generation step failed; showing real frames";
		}
		return "";
	}

	const char *PassReasonText( PassReason eReason )
	{
		switch ( eReason )
		{
			case PassReason::Normal:                return "";
			case PassReason::Off:                 return "Off";
			case PassReason::WarmingUp:           return "Waiting for a second frame";
			case PassReason::CostGuard:           return "Limited to keep up with the GPU";
			case PassReason::GameTooFast:         return "Game already reaches the refresh rate or target";
			case PassReason::GameStalled:         return "Frame gap; showing real frames";
			case PassReason::RendererUnavailable: return "Not available here";
			case PassReason::WaitingForDecoder:   return "Waiting for the next video frame";
		}
		return "";
	}

	RenderStatus GetRenderStatus()
	{
		RenderStatus s;
		s.reason = Unavailable( g_eReason.load( std::memory_order_relaxed ) );
		s.timestampsSupported = g_device.supportsTimestamps();
		if ( s.timestampsSupported )
		{
			s.lastPairGpuMs = g_flLastPairGpuMs.load( std::memory_order_relaxed );
			s.lastEstimateMs = g_flLastEstimateMs.load( std::memory_order_relaxed );
			s.lastSynthMs = g_flLastSynthMs.load( std::memory_order_relaxed );
			s.lastUiMs = g_flLastUiMs.load( std::memory_order_relaxed );
		}
		s.costSeq = g_uCostSeq.load( std::memory_order_relaxed );
		s.formatTag = g_pszFormatTag.load( std::memory_order_relaxed );
		return s;
	}

	void PublishPacingStatus( const PacingStatus &status )
	{
		std::lock_guard<std::mutex> lock( g_PacingMutex );
		g_Pacing = status;
	}

	PacingStatus GetPacingStatus()
	{
		std::lock_guard<std::mutex> lock( g_PacingMutex );
		return g_Pacing;
	}

	// ------------------------------------------------------------------
	//  3. The per-composite entry point
	// ------------------------------------------------------------------

	gamescope::Rc<CVulkanTexture> RecordBaseLayer( gamescope::Rc<CVulkanTexture> pLayer0, GamescopeAppTextureColorspace eColorspace )
	{
		const uint32_t uPackedConfig = g_uConfig.load( std::memory_order_relaxed );
		const Config cfg = Unpack( uPackedConfig, g_uUiBox.load( std::memory_order_relaxed ) );
		const uint32_t uPackedBlur = g_uBlur.load( std::memory_order_relaxed );
		const BlurConfig blurCfg = UnpackBlur( uPackedBlur );
		const bool bLagOn = ( g_uLag.load( std::memory_order_relaxed ) & kLagEnabledBit ) != 0;

		// Frame generation, motion blur and the lag spike buffer all off (or never on): release
		// everything. The first branch is the whole cost of the off path once
		// resources are gone, and vulkan_composite() does not even reach it then
		// (RenderWanted() == 0).
		if ( !cfg.enabled && !blurCfg.enabled && !bLagOn )
		{
			if ( g_pHost )
			{
				if ( g_uGate.load( std::memory_order_relaxed ) & kGateLive )
					Teardown( *g_pHost );
				g_pHost->ulFailedKey = 0;
			}
			SetReason( Unavailable::Ok );
			return nullptr;
		}

		// Not an eligible composite (partial / pre-emptive / already graded).
		if ( !pLayer0 )
			return nullptr;

		// For the box-size slider's "px" figure and the preview: the game's frame
		// size as the renderer sees it.
		g_ulGameFrameSize.store( ( uint64_t( pLayer0->width() ) << 32 ) | pLayer0->height(), std::memory_order_relaxed );
		BoxPreviewPoll();

		FrameRequest req;
		{
			std::lock_guard<std::mutex> lock( g_FrameMutex );
			req = g_Frame;
		}

		// Pacing has not driven the renderer, or says it is settled pass-through:
		// stay completely inert. Enabled() is true so the backends already
		// composite, but nothing is copied or run. The frames are forgotten, so the
		// first frame after the inert stretch starts a clean sequence (no ancient
		// "previous" frame).
		if ( req.t < 0.0f || req.newestId == 0 )
		{
			if ( g_pHost && ( g_uGate.load( std::memory_order_relaxed ) & kGateLive ) )
			{
				// The library's UI counters keep following the real frames (nothing here
				// touches the Interpolator otherwise: DropFrames() forgets the HOST's ring
				// only, and EnsureReady() is not reached, so no setSettings()/resize()).
				TrackWhileInert( *g_pHost, pLayer0, eColorspace, cfg, req.newestId );
				DropFrames( *g_pHost );
			}
			return nullptr;
		}

		Host_t &H = HostGet();

		if ( g_bResetRequested.exchange( false, std::memory_order_relaxed ) )
			DropFrames( H );

		// D4: where the library cannot run, the real frame is shown and the status
		// says why. The ring is left allocated (cheap) but its frames are dropped,
		// so generation resumes one real frame after the condition clears.
		const uint32_t uWidth = pLayer0->width();
		const uint32_t uHeight = pLayer0->height();
		// The output being HDR (g_bOutputHDREnabled) is NOT a reason by itself: SDR
		// content on an HDR output is a plain 8-bit layer 0 and runs as always. What
		// matters is the layer's own format and colourspace tag (FrameGenFormat.h).
		FormatPlan plan;
		Unavailable eWhy = ClassifyLayer( pLayer0->format(), pLayer0->isYcbcr(), eColorspace, DeviceCaps(), &plan );
		if ( eWhy == Unavailable::Ok && ( uWidth < framegen::Interpolator::kMinSize || uHeight < framegen::Interpolator::kMinSize ) )
			eWhy = Unavailable::TooSmall;

		if ( eWhy != Unavailable::Ok )
		{
			SetReason( eWhy );
			g_pszFormatTag.store( "", std::memory_order_relaxed );
			DropFrames( H );
			return nullptr;
		}

		// A colourspace change on the same format class (SDR <-> HDR10 on a 10-bit
		// surface) makes the ring's frames a different picture: forget them.
		if ( H.bHaveColorspace && H.eColorspace != eColorspace )
			DropFrames( H );
		H.bHaveColorspace = true;
		H.eColorspace = eColorspace;

		if ( !EnsureReady( H, uWidth, uHeight, plan, cfg, uPackedConfig, blurCfg, uPackedBlur ) )
		{
			// A 10-bit / fp16 game that cannot start says why in terms of the format,
			// not the generic "could not start": the cause is almost always the format.
			SetReason( plan.eFormat == framegen::Format::Rgba8 ? Unavailable::InitFailed : Unavailable::HdrFormat );
			g_pszFormatTag.store( "", std::memory_order_relaxed );
			return nullptr;
		}
		SetReason( Unavailable::Ok );
		g_pszFormatTag.store( PlanTag( plan, eColorspace ), std::memory_order_relaxed );

		const bool bUi = cfg.ui != UiProt::Off;

		// The ring and the library's UI-protection history FOLLOW the pacer's depth
		// (the lag spike buffer's size, not its cap): grow at once, shrink after a
		// dwell. Checked every paint, before this paint's copy and observe.
		FollowDepth( H, req.historyDepth, bUi );

		// ---- everything below records into one command buffer of our own ----
		// (created lazily: a pass-through composite of an already-seen frame records nothing)
		std::unique_ptr<CVulkanCmdBuffer> pCmd;
		bool bProfileWritten = false;
		auto Cmd = [&]() -> CVulkanCmdBuffer *
		{
			if ( !pCmd )
			{
				pCmd = g_device.commandBuffer();
				H.interp.beginFrame();   // once per command buffer, before its first record
			}
			return pCmd.get();
		};

		gamescope::Rc<CVulkanTexture> pResult;

		bool bObservedThisCb = false;
		bool bBoxPreviewRecorded = false;

		// D11: a newestId not seen before is a new real frame. Copy it into the next
		// ring slot (in this very command buffer) whatever this composite shows, so
		// later pairs always have it.
		if ( !H.bHaveCur || req.newestId != H.ulCurId )
		{
			const int nNewSlot = H.bHaveCur ? ( H.nCurSlot + 1 ) % H.nRing : 0;
			H.nCurSlot = nNewSlot;
			H.bHaveCur = true;
			H.ulCurId = req.newestId;
			H.ulSlotId[ nNewSlot ] = req.newestId;
			// The estimate and the cached outputs belong to pairs of frames; the
			// estimate's frames may now be gone from the ring (overwritten), the
			// outputs stay valid (they are finished images keyed by outId).
			if ( H.bEstimateValid && ( FindSlot( H, H.ulEstPrev ) < 0 || FindSlot( H, H.ulEstCurr ) < 0 ) )
				H.bEstimateValid = false;

			CVulkanCmdBuffer *pCb = Cmd();
			pCb->bindPipeline( g_device.pipeline( CopyShader( H.plan ) ) );
			pCb->bindTexture( 0, pLayer0 );
			pCb->setTextureSrgb( 0, true );             // raw UNORM view: encoded values, as the effects passes read it
			pCb->setSamplerUnnormalized( 0, true );
			pCb->setSamplerNearest( 0, true );
			pCb->bindTarget( H.pRing[ nNewSlot ] );
			pCb->dispatch( div_roundup( uWidth, 8 ), div_roundup( uHeight, 8 ) );
			// Flush the dispatch's write->read barrier NOW: the library reads this slot
			// in this or a LATER command buffer, and a barrier's destination scope covers
			// every later command on the queue, not just the rest of this buffer.
			pCb->insertBarrier();
			pCb->bindTexture( 0, nullptr );

			// The Inspector's box preview, when it is on screen (a crop of this very
			// frame; it rides in this command buffer, see BoxPreviewRecord).
			bBoxPreviewRecorded = BoxPreviewRecord( pCb, pLayer0, H.plan, cfg );

			// UI protection (the library's): hand it the new real frame, EVERY new real
			// frame, also those that are only passed through -- its stillness counters
			// compare consecutive real frames. The ring holds the REAL frame; the
			// library keeps its own inpainted clean copy and pastes the real pixels
			// back over each generated frame (it reads them from the `curr` view that
			// recordSynth gets). The view is the cache key: a ring slot's earlier
			// entry is replaced, so the library keeps exactly the three ring frames.
			if ( bUi )
			{
				// The observe resets the library's query pool (when no profile is
				// waiting for an estimate): read the previous pair's timings first, if
				// they have retired.
				if ( H.bProfiling )
				{
					HarvestProfile( H, true, false );
					H.nProfileSynths = 0;
					H.nProfileBlurSamples = 0;
				}
				// A frame is tracked (inert) OR observed (here), never both: this one is
				// recorded as the library's newest from now on.
				H.ulUiFedId = req.newestId;
				if ( H.uTrackedRun )
				{
					fg_log.debugf( "UI protection: observing again after %u tracked real frames (counters kept)", H.uTrackedRun );
					H.uTrackedRun = 0;
				}
				if ( H.interp.recordObserve( pCb->rawBuffer(), H.pRing[ nNewSlot ]->srgbView() ) )
				{
					bObservedThisCb = true;
					if ( H.bProfiling )
					{
						H.bProfileOpen = true;
						bProfileWritten = true;
					}
				}
			}
		}

		// ---- the generated (or blurred) frame, if this output is one ----
		// THE ONE RENDERER RULE (pacing's Decision, see FrameRequest): a pair means
		// recordSynthBlur( prev, curr, out, t0, t1 ) -- a plain synth when t0 == t1,
		// the (blurred) real frame currId when the window ends at 1; no pair means no
		// renderer work, the real frame showId is shown (below).
		bool bPairPlaying = false;   // this composite shows a synth of the profile's pair
		if ( req.prevId && req.currId && req.prevId != req.currId )
		{
			const int nPrevSlot = FindSlot( H, req.prevId );
			const int nCurrSlot = FindSlot( H, req.currId );
			bPairPlaying = H.bProfileOpen && req.prevId == H.ulEstPrev && req.currId == H.ulEstCurr;

			OutSlot_t *pHit = nullptr;
			for ( OutSlot_t &o : H.out )
			{
				if ( o.bValid && o.ulOutId == req.outId )
					pHit = &o;
			}

			if ( pHit )
			{
				// D13: this output was generated by an earlier composite (a repaint).
				pHit->ulUse = ++H.ulUseClock;
				pResult = pHit->pTex;
			}
			else if ( nPrevSlot >= 0 && nCurrSlot >= 0 )
			{
				CVulkanTexture *pPrev = H.pRing[ nPrevSlot ].get();
				CVulkanTexture *pCurr = H.pRing[ nCurrSlot ].get();
				bool bOk = true;
				bool bSynthRefused = false;   // the failing call was recordSynth() (not the estimate)

				if ( !H.bEstimateValid || H.ulEstPrev != req.prevId || H.ulEstCurr != req.currId )
				{
					// D16: a new estimate resets the library's query pool; read the previous
					// pair's timings first, if they have retired.
					// (Not when this composite's observe just opened the profile: it was
					// harvested before the observe, and the pool now holds records of the
					// command buffer being built -- reading it would wait for work that is
					// not even submitted.)
					if ( H.bProfiling && !bObservedThisCb )
					{
						HarvestProfile( H, true, false );
						H.nProfileSynths = 0;
						H.nProfileBlurSamples = 0;
					}

					CVulkanCmdBuffer *pCb = Cmd();
					pCb->prepareSrcImage( pPrev );
					pCb->prepareSrcImage( pCurr );
					pCb->insertBarrier();
					bOk = H.interp.recordEstimate( pCb->rawBuffer(), pPrev->srgbView(), pCurr->srgbView() );
					if ( bOk )
					{
						H.bEstContig = bObservedThisCb;
						H.bEstimateValid = true;
						H.ulEstPrev = req.prevId;
						H.ulEstCurr = req.currId;
						H.nPairSynths = 0;
						if ( H.bProfiling )
						{
							H.bProfileOpen = true;
							bProfileWritten = true;
							bPairPlaying = true;
						}
					}
				}

				if ( bOk && H.nPairSynths >= kMaxSynthsPerPair )
				{
					// Over the per-pair synth cap (pacing stops asking long before this):
					// show the most recent output of the pool, or the real frame.
					const OutSlot_t *pNear = nullptr;
					for ( const OutSlot_t &o : H.out )
					{
						if ( o.bValid && ( !pNear || o.ulUse > pNear->ulUse ) )
							pNear = &o;
					}
					if ( pNear )
						pResult = pNear->pTex;
				}
				else if ( bOk )
				{
					OutSlot_t *pSlot = PickOutSlot( H );
					if ( !pSlot->pTex )
						bOk = CreateTexture( pSlot->pTex, uWidth, uHeight, H.plan );

					if ( bOk )
					{
						CVulkanTexture *pOut = pSlot->pTex.get();
						CVulkanCmdBuffer *pCb = Cmd();
						// The output is fully overwritten: prepareDestImage discards its old
						// contents (UNDEFINED -> GENERAL), and insertBarrier orders it after
						// every earlier read of it (an earlier composite).
						pSlot->bValid = false;
						pCb->prepareDestImage( pOut );
						pCb->insertBarrier();
						// A window of zero length (blur off, or amount 0) is one instant: the
						// library's own plain-synth path, identical to recordSynth( t1 ).
						const bool bBlurred = req.t0 < req.t1 && H.interp.settings().blurSamples >= 2;
						bOk = H.interp.recordSynthBlur( pCb->rawBuffer(), pPrev->srgbView(), pCurr->srgbView(), pOut->srgbView(), req.t0, req.t1 );
						bSynthRefused = !bOk;
						if ( bOk )
						{
							pCb->markDirty( pOut );
							pCb->insertBarrier();   // flush for the consumers in later command buffers
							pSlot->bValid = true;
							pSlot->ulOutId = req.outId;
							pSlot->ulUse = ++H.ulUseClock;
							H.nPairSynths++;
							if ( H.bProfileOpen )
							{
								H.nProfileSynths++;
								if ( bBlurred )
									H.nProfileBlurSamples = int( H.interp.settings().blurSamples );
								bProfileWritten = true;
							}
							pResult = pSlot->pTex;
						}
					}
				}

				if ( !bOk && bSynthRefused && bUi )
				{
					// recordSynth() refused with UI protection on: one of the pair's
					// observed frames was evicted/re-observed after the estimate. Not a
					// failure of generation: nothing was recorded for this call, so
					// `pResult` stays null and the real frame is shown for this slot,
					// and the next pair is fine. (With the 3-slot ring the library keeps
					// exactly the ring's three frames, so this should not happen.)
					if ( !H.bUiEvictLogged )
					{
						H.bUiEvictLogged = true;
						fg_log.warnf( "recordSynth refused with UI protection on (an observed frame of the pair was evicted, or a descriptor ring was full); showing the real frame" );
					}
					H.bEstimateValid = false;
				}
				else if ( !bOk )
				{
					// Nothing was recorded for the failing call, so `pResult` stays null:
					// the real frame is shown for this output (library contract: never
					// present an `out` whose record returned false).
					SetReason( Unavailable::RecordFailed );
				}
			}
			// else: a frame of the pair is gone from the ring -- the real frame is shown.
		}
		else if ( req.showId && req.showId != req.newestId )
		{
			// A real frame that is not the newest (a lag-spike buffer plays behind real
			// time): the ring still holds it, so show that copy instead of layer 0.
			// (Gone from the ring: layer 0, the newest, stays.)
			const int nShowSlot = FindSlot( H, req.showId );
			if ( nShowSlot >= 0 )
				pResult = H.pRing[ nShowSlot ];
		}

		if ( pCmd )
		{
			const uint64_t ulSeq = g_device.submit( std::move( pCmd ) );
			if ( bProfileWritten )
				H.ulProfileSeq = ulSeq;
			if ( bBoxPreviewRecorded )
				BoxPreviewSubmitted( ulSeq );
		}

		// Cheap poll for the finished pair's timings (a counter read, never a wait).
		if ( H.bProfiling )
			HarvestProfile( H, false, bPairPlaying );

		return pResult;
	}
}
