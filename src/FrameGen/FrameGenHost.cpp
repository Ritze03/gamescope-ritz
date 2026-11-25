// FrameGenHost.cpp -- see FrameGenHost.h for the model and the API contract.
//
// Layout of this file:
//   1. cross-thread state   (config, gate, frame request, reset flag, status)
//   2. render-thread state  (Host: the Interpolator, the ring, the outputs)
//   3. RecordBaseLayer()    (the one per-composite entry point)

#include "FrameGenHost.h"
#include "FrameGenFormat.h"
#include "FrameGenReach.h"
#include "FrameGenV3.h"
#include "FrameGenCapture.h"
#include "StaleFrame.h"
#include "FrameGenDebug.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/dma-buf.h>

#include "../../subprojects/FrameGen/gpu/framegen.h"
#include "../../subprojects/FrameGen/gpu/pacing.h"

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "log.hpp"
#include "main.hpp"
#include "convar.h"

static LogScope fg_log( "framegen" );

// steamcompmgr.cpp: what is known about the commit a new real frame came from (the stale-frame guard's
// diagnostics), and how many commits re-committed a held buffer without a new one (update_wayland_res()).
bool FrameGen_CommitFacts( uint64_t ulCommitId, fghost::stale::CommitFacts *pOut );
uint64_t FrameGen_RecommitCount( uint64_t *pulKept, uint64_t *pulParkedNow );

namespace fghost
{
	// The library's blend values and the host's table must agree: a pin that renumbers, adds or drops a learned blend fails
	// here instead of silently offering the wrong names. HybridVariants.h is pure and does not include the library header
	// (Vulkan), so the check lives in the host.
	static_assert( kHybridVariants[ 0 ].nBlend == framegen::kV3BlendFluidity && kHybridVariants[ 1 ].nBlend == framegen::kV3BlendConsistency,
		"the library's kV3BlendFluidity / kV3BlendConsistency differ from the table in src/FrameGen/HybridVariants.h" );
	static_assert( kHybridBlendMax == framegen::kV3BlendMax,
		"the library's kV3BlendMax differs from the number of entries in src/FrameGen/HybridVariants.h: add or remove a table row" );

	namespace
	{
		// -------------------------------------------------------------
		//  1. Cross-thread state
		// -------------------------------------------------------------

		// The user's config, packed into one word so a reader never sees a torn
		// mix of two SetConfig() calls. bits 0-3 multiplier, 4-5 quality, 6-7
		// safety, 8-9 hud protection, 10-11 mode, 12 priority, 13-22 target fps, 23 pause at refresh,
		// 24-25 UI protection, 26 enabled, 27 limit to GPU speed, 28-29 motion version, 30 safety's third bit (VeryLow = 4 needs it; bits 6-7 hold the low two), 31 pause while cursor visible. The
		// crosshair box size is its own word, g_uUiBox, and Learned motion's runtime settings (net,
		// precision, zero-motion test, static guard) are another, g_uLearned (the main word is full), and the
		// motion search reach (radii, pyramid side, penalty) a fourth, g_uReach, and Motion version 3's four values a fifth, 64-bit one, g_ulV3.
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
				| ( ( uint32_t( c.safety ) & 0x3u ) << 6 )
				| ( uint32_t( c.hud ) << 8 )
				| ( uint32_t( eMode ) << 10 )
				| ( uint32_t( c.priority ) << 12 )
				| ( uint32_t( nTarget ) << 13 )
				| ( uint32_t( EffectiveCapAtRefresh( c.pauseAtRefresh, eMode ) ? 1 : 0 ) << 23 )
				| ( uint32_t( c.ui ) << 24 )
				| ( uint32_t( bEnabled ? 1 : 0 ) << 26 )
				| ( uint32_t( c.gpuLimit ? 1 : 0 ) << 27 )
				| ( uint32_t( c.motion ) << 28 )
				| ( ( ( uint32_t( c.safety ) >> 2 ) & 1u ) << 30 )
				| ( uint32_t( c.pauseWhenCursorVisible ? 1 : 0 ) << 31 );
		}

		// Learned motion's runtime settings, their own word: bits 0-3 are FREE (they held the net
		// choice until 2026-11-25, when Learned kept only its default network; written as 0,
		// ignored on read; the other fields did not move), 4 half precision, 5 zero-motion test,
		// 6 static guard (mask 0x7F).
		// Round 1 had the net and precision in bits 30-31 of the main word; round 2 needs two
		// more bits and the main word is full, so all four moved here (the main word's
		// defaults still pack to what they did: bits 28-31 are 0 for V1).
		uint32_t PackLearned( const Config &c )
		{
			return ( uint32_t( c.learnedHalf ? 1 : 0 ) << 4 )
				| ( uint32_t( c.learnedZeroTest ? 1 : 0 ) << 5 )
				| ( uint32_t( c.learnedStaticGuard ? 1 : 0 ) << 6 );
		}

		// The crosshair box size, its own word (the main one is full): tenths of a
		// percent of the game's height.
		uint32_t PackBox( const Config &c )
		{
			return PackUiBox( c.uiBoxTenths, c.uiWholeFrame );   // bits 0-7 tenths, bit 8 Whole screen V2
		}

		static_assert( sizeof( framegen::Settings ) == 164, "framegen::Settings changed size: re-check ToLibrarySettings() and the library pin" );
		static_assert( uint8_t( framegen::DebugView::Off ) == 0 && uint8_t( framegen::DebugView::Motion ) == 1
			&& uint8_t( framegen::DebugView::UiMask ) == 2 && uint8_t( framegen::DebugView::UiClean ) == 3,
			"LibraryDebugView() (FrameGenDebug.h) maps the row's index onto these values" );
		static_assert( kUiBoxMaxTenths <= 255, "the box size has 8 bits (48-55) in the library-config key" );
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

		Config Unpack( uint32_t u, uint32_t uBox, uint32_t uLearned, uint32_t uReach, uint64_t ulV3 )
		{
			Config c;
			UnpackReach( uReach, &c );
			UnpackV3( ulV3, &c );
			UnpackUiBox( uBox, &c.uiBoxTenths, &c.uiWholeFrame );
			c.multiplier = int( u & 0xFu );
			c.quality = Quality( ( u >> 4 ) & 0x3u );
			c.safety = Safety( ( ( u >> 6 ) & 0x3u ) | ( ( ( u >> 30 ) & 1u ) << 2 ) );
			c.hud = HudProtect( ( u >> 8 ) & 0x3u );
			c.enabled = ( ( u >> 26 ) & 1u ) != 0;
			c.mode = Mode( ( u >> 10 ) & 0x3u );
			c.priority = Priority( ( u >> 12 ) & 0x1u );
			c.targetFps = int( ( u >> 13 ) & 0x3FFu );
			c.pauseAtRefresh = ( ( u >> 23 ) & 1u ) != 0;
			c.ui = UiProt( ( u >> 24 ) & 0x3u );
			c.gpuLimit = ( ( u >> 27 ) & 1u ) != 0;
			c.pauseWhenCursorVisible = ( ( u >> 31 ) & 1u ) != 0;
			c.motion = Motion( ( u >> 28 ) & 0x3u );
			if ( c.motion > Motion::V3 )
				c.motion = Motion::V1;
			c.learnedHalf = ( ( uLearned >> 4 ) & 1u ) != 0;
			c.learnedZeroTest = ( ( uLearned >> 5 ) & 1u ) != 0;
			c.learnedStaticGuard = ( ( uLearned >> 6 ) & 1u ) != 0;
			return c;
		}

		std::atomic<uint32_t> g_uConfig{ Pack( Config{} ) };
		std::atomic<uint32_t> g_uLearned{ PackLearned( Config{} ) };
		std::atomic<uint32_t> g_uReach{ PackReach( Config{} ) };
		std::atomic<uint64_t> g_ulV3{ PackV3( Config{} ) };

		// The motion-blur config, its own word (the frame generation word is full):
		// bits 0-3 samples, 4-10 amount percent, 11 relative (1 = game), 12 weights
		// (1 = gaussian), 13 enabled, 14 own interpolator settings, 15-16 own motion version
		// (1 = V2, 2 = Learned, 3 = V3), 17 own quality (1 = performance), 18 pause while cursor visible
		// (read by steamcompmgr only; not in the library-config key). Kept apart from g_uConfig so a blur edit never
		// reconfigures frame generation and vice versa; a reader that sees one
		// updated and not the other just gets the previous other half for one frame.
		constexpr uint32_t kBlurEnabledBit = 1u << 13;
		constexpr uint32_t kBlurOwnBit = 1u << 14;
		constexpr uint32_t kBlurPauseBit = 1u << 18;

		uint32_t PackBlur( const BlurConfig &b )
		{
			const int nSamples = std::min( std::max( b.samples, kMinBlurSamples ), kMaxBlurSamples );
			const int nAmount = std::min( std::max( b.amountPercent, 0 ), 100 );
			return uint32_t( nSamples )
				| ( uint32_t( nAmount ) << 4 )
				| ( uint32_t( b.relative == BlurRel::Game ? 1 : 0 ) << 11 )
				| ( uint32_t( b.weights == BlurWeighting::Gaussian ? 1 : 0 ) << 12 )
				| ( b.enabled ? kBlurEnabledBit : 0u )
				| ( b.useOwnInterp ? kBlurOwnBit : 0u )
				| ( uint32_t( b.motion ) << 15 )
				| ( uint32_t( b.quality ) << 17 )
				| ( b.pauseWhenCursorVisible ? kBlurPauseBit : 0u );
		}

		BlurConfig UnpackBlur( uint32_t u )
		{
			BlurConfig b;
			b.samples = int( u & 0xFu );
			b.amountPercent = int( ( u >> 4 ) & 0x7Fu );
			b.relative = ( ( u >> 11 ) & 1u ) ? BlurRel::Game : BlurRel::Shown;
			b.weights = ( ( u >> 12 ) & 1u ) ? BlurWeighting::Gaussian : BlurWeighting::Even;
			b.enabled = ( u & kBlurEnabledBit ) != 0;
			b.useOwnInterp = ( u & kBlurOwnBit ) != 0;
			b.motion = Motion( ( u >> 15 ) & 0x3u );
			if ( b.motion > Motion::V3 )
				b.motion = Motion::V1;
			b.quality = Quality( ( u >> 17 ) & 1u );
			b.pauseWhenCursorVisible = ( u & kBlurPauseBit ) != 0;
			return b;
		}

		std::atomic<uint32_t> g_uBlur{ PackBlur( BlurConfig{} ) };

		// Debug options (FrameGenDebug.h): its own word, the Config word is full.
		std::atomic<uint32_t> g_uDebug{ PackDebug( DebugConfig{} ) };

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
			return ( ( l.enabled && kLagBufferAvailable ) ? kLagEnabledBit : 0u ) | ( uMode << 1 ) | ( uint32_t( nLook ) << 3 ) | ( uint32_t( nMax ) << 7 );
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
		// V3 Hybrid: whether the library's learned blend is really in use (Interpolator::v3BlendActive(), refreshed by the
		// render thread at the end of every EnsureReady()), and why not when a variant is selected but is not (string
		// literal, "" = no cheap reason). Read by the compositor thread's status line through BlendActive() / BlendWhy().
		std::atomic<bool> g_bBlendActive{ false };
		std::atomic<bool> g_bV35Active{ false };   // V3.5's motion passes really run (Interpolator::v3MotionActive())
		std::atomic<const char *> g_pszBlendWhy{ "" };
		// Debug view: the row's index of the view the library draws now (0 = none), and whether the wanted one was refused.
		std::atomic<int> g_nDebugViewActive{ 0 };
		std::atomic<bool> g_bDebugViewRefused{ false };
		std::atomic<const char *> g_pszMotionNote{ "" };  // the library's reason V2 is refused (string literals), else ""
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
			// The pair and window it was generated from (ritz_fg_capture: a repaint reports the output's own).
			uint64_t ulPrevId = 0, ulCurrId = 0;
			float flT0 = 1.0f, flT1 = 1.0f;
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
			// ritz_fg_capture watch (the `stale` flag): a stamp per slot, new on every write of the slot, moved with
			// the texture by ResizeRing; the estimate remembers the stamps of the two slots it was recorded from.
			uint64_t ulSlotWrite[ kRingMax ] = {};
			uint64_t ulWriteClock = 0;
			uint64_t ulEstPrevStamp = 0, ulEstCurrStamp = 0;
			int nRing = 0;          // slots created (kRingMin..kRingMax); FOLLOWS the pacer's depth: grows at once, shrinks after a dwell
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
			// Debug options, "Only generated frames": the last GENERATED output shown on a non-real tick (slot index, its outId so a
			// reused slot is noticed, and when), the image a real tick shows again.
			int nLastGenSlot = -1;
			uint64_t ulLastGenOutId = 0;
			uint64_t ulLastGenNs = 0;
			uint64_t ulDebugHeld = 0, ulDebugReal = 0, ulDebugGen = 0;   // real ticks replaced by the held image / real ticks seen / generated ticks seen (log only)

			// What the library is currently configured with.
			uint64_t uAppliedConfig = 0;
			// The reach word (PackReach) last applied: the 64-bit key above has no free bit left, so the four reach values
			// are compared on their own. 0 = none applied (a real reach word is never 0: the radii start at 1).
			uint32_t uAppliedReach = 0;
			// The V3 word (PackV3) last applied, for the same reason. 0 = none applied (a real V3 word is never 0:
			// the occlusion softness and unsure sensitivity start at 25 hundredths).
			uint64_t ulAppliedV3 = 0;
			int nV35Logged = 0;     // V3.5's state last logged, see PublishBlend()
			int nBlendLogged = 0;   // the blend state last logged (variant * 4 + state), see PublishBlend()
			// The Debug view index (FrameGenDebug.h) last handled by ApplyDebugView(): accepted OR refused, so a refused view
			// is not retried every frame (the next change of the row retries). 0 = Off, which init() starts with.
			int nDebugViewHandled = 0;
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
				case Safety::VeryLow: s.trustLow = 40.0f; s.trustHigh = 68.0f; break;
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
			UiBoxSettings( c.ui == UiProt::CrosshairV2, c.uiWholeFrame, c.uiBoxTenths, &s.uiBoxHeightFrac, &s.uiBoxWidth, &s.uiBoxHeight );
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
			// Motion version: a request; ResolveMotion() drops V2 / Learned / V3 to V1 where the library refuses it.
			s.motionVersion = c.motion == Motion::V2 ? framegen::MotionVersion::V2
				: c.motion == Motion::Learned ? framegen::MotionVersion::Learned
				: c.motion == Motion::V3 ? framegen::MotionVersion::V3 : framegen::MotionVersion::V1;
			// Learned's activation precision, zero-motion test and static guard (every other
			// version ignores all three). The library has ONE net and refuses any other value
			// under EVERY motion version, so the net is always E2v5.
			s.learnedNet = framegen::LearnedNet::E2v5;
			s.learnedHalf = c.learnedHalf;
			s.learnedZeroTest = c.learnedZeroTest;
			s.learnedStaticGuard = c.learnedStaticGuard;
			// The one Quality setting also picks V2's mode (V1 ignores it).
			s.v2Mode = c.quality == Quality::Performance ? framegen::V2Mode::Performance : framegen::V2Mode::Quality;
			// Motion search reach (V1 and V2; Learned ignores the radii). Always from the Frame generation Config,
			// also when only Motion blur is on (one interpolator). The two radii and the penalty are runtime;
			// pyramidMinSide is structural and takes EnsureReady()'s reconfigure() route. Clamped here too: the
			// library refuses an out-of-range value with a generic reason.
			s.searchRadius = uint32_t( std::clamp( c.searchRadius, kSearchRadiusMin, kSearchRadiusMax ) );
			s.searchRadiusTop = uint32_t( std::clamp( c.searchRadiusTop, kSearchRadiusMin, kSearchRadiusMax ) );
			s.pyramidMinSide = uint32_t( std::clamp( c.pyramidMinSide, kPyramidMinSideMin, kPyramidMinSideMax ) );
			s.searchPenalty = ReachIsFinite( c.searchPenalty ) ? std::clamp( c.searchPenalty, 0.0f, kSearchPenaltyMax ) : kSearchPenaltyDefault;
			// Motion version 3's four values (V3 only; every other version ignores them). Always from the Frame
			// generation Config, also when only Motion blur is on (one interpolator). All runtime. Clamped here too
			// (bit-pattern finite test, the project builds with -ffast-math); the library refuses an out-of-range
			// value with a generic reason.
			s.v3Smear = ReachIsFinite( c.v3Smear ) ? std::clamp( c.v3Smear, kV3SmearMin, kV3SmearMax ) : kV3SmearDefault;
			s.v3OcclSoft = ReachIsFinite( c.v3OcclSoft ) ? std::clamp( c.v3OcclSoft, kV3OcclSoftMin, kV3OcclSoftMax ) : kV3OcclSoftDefault;
			s.v3Unsure = ReachIsFinite( c.v3Unsure ) ? std::clamp( c.v3Unsure, kV3UnsureMin, kV3UnsureMax ) : kV3UnsureDefault;
			s.v3SceneCut = ReachIsFinite( c.v3SceneCut ) ? std::clamp( c.v3SceneCut, kV3SceneCutMin, kV3SceneCutMax ) : kV3SceneCutDefault;
			s.v3ColourWindow = c.v3ColourWindow;
			s.v3DetailSplit = c.v3DetailSplit;
			s.v3Guard = c.v3Guard == 1 ? framegen::V3Guard::Guard
				: c.v3Guard == 2 ? framegen::V3Guard::V2Motion : framegen::V3Guard::Off;
			// V3 Hybrid: 0 = off (V3's own rule), 1..kHybridBlendMax = the active variants (HybridVariants.h; the V3 word already limits it). Runtime; the library
			// falls back to the rule for formats other than Rgba8/Bgra8 and while v3DetailSplit is on.
			s.v3Blend = uint8_t( c.v3Blend >= 1 && c.v3Blend <= kHybridBlendMax ? c.v3Blend : 0 );
			s.v3Motion = c.v3Motion == 1 ? framegen::kV3MotionV35 : framegen::kV3MotionClassic;   // V3.5; the library ignores it under every other version
			return s;
		}

		// V2 / Learned only where the library takes them (V2: every format whose V2 synthesis
		// shaders are built in, a GPU with the subgroup operations, a size it can hold under V2;
		// Learned: the ml shaders and weights built into the library, flowScale 2): else V1, so
		// frame generation never dies for want of them, and the library's reason goes to the
		// Status line. Needs a live interpolator (the device check). Logged when the reason
		// changes. Learned is checked with flowScale 2 (ToLibrarySettings may carry 4 from the
		// Quality row's Performance; EnsureReady() forces 2 for Learned right after, so the
		// check must judge the value that will actually be applied).
		framegen::Settings ResolveMotion( const Host_t &H, framegen::Settings s )
		{
			const bool bLearned = s.motionVersion == framegen::MotionVersion::Learned;
			const bool bV3 = s.motionVersion == framegen::MotionVersion::V3;
			const char *pszWhy = nullptr;
			if ( s.motionVersion != framegen::MotionVersion::V1 )
			{
				framegen::Settings sCheck = s;
				if ( bLearned )
					sCheck.flowScale = 2;
				// V3 honours flowScale (1, 2, 4), so it is checked with the Quality row's own value, which
				// EnsureReady() leaves alone for V3.
				pszWhy = H.interp.settingsError( sCheck );
				// V3.5 refused (a library without it): plain V3 / V3 Hybrid with the same settings, logged once per change.
				if ( pszWhy && bV3 && s.v3Motion != 0 )
				{
					sCheck.v3Motion = framegen::kV3MotionClassic;
					if ( !H.interp.settingsError( sCheck ) )
					{
						fg_log.warnf( "frame generation: V3.5 refused, Motion version 3 runs with its own motion detection: %s", pszWhy );
						s.v3Motion = framegen::kV3MotionClassic;
						pszWhy = nullptr;
					}
					else
						sCheck.v3Motion = s.v3Motion;
				}
				// The Motion guard (a chosen Guard) needs what V2 needs. A device that runs V3 but not
				// V2 keeps V3 with the guard Off, logged only: the optional guard must not cost V3 or leave a lasting Status note.
				if ( pszWhy && bV3 && s.v3Guard != framegen::V3Guard::Off )
				{
					sCheck.v3Guard = framegen::V3Guard::Off;
					if ( !H.interp.settingsError( sCheck ) )
					{
						fg_log.infof( "frame generation: Motion guard unavailable, Motion version 3 runs without it: %s", pszWhy );
						s.v3Guard = framegen::V3Guard::Off;
						pszWhy = nullptr;
					}
				}
			}
			const char *pszName = bLearned ? "Learned motion" : bV3 ? "Motion version 3" : "Motion version 2";
			if ( pszWhy )
				s.motionVersion = framegen::MotionVersion::V1;
			const char *pszOld = g_pszMotionNote.exchange( pszWhy ? pszWhy : "", std::memory_order_relaxed );
			if ( pszWhy && pszOld != pszWhy )
				fg_log.warnf( "frame generation: %s refused, running V1: %s", pszName, pszWhy );
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

		// ritz_fg_capture watch flags (detection only, FrameGenCapture.h FlagInput). Stale: the library's estimate is
		// reused, but a slot of its pair was written since it was recorded.
		bool CaptureStale( const Host_t &H, int nPrevSlot, int nCurrSlot )
		{
			return nPrevSlot >= 0 && nCurrSlot >= 0
				&& ( H.ulSlotWrite[ nPrevSlot ] != H.ulEstPrevStamp || H.ulSlotWrite[ nCurrSlot ] != H.ulEstCurrStamp );
		}
		// Mismatch: an id of the pair is held by more than one ring slot (which one is read is then a guess).
		bool CaptureDupIds( const Host_t &H, uint64_t ulPrevId, uint64_t ulCurrId )
		{
			int nPrev = 0, nCurr = 0;
			for ( int i = 0; i < H.nRing; i++ )
			{
				nPrev += ulPrevId && H.ulSlotId[ i ] == ulPrevId;
				nCurr += ulCurrId && H.ulSlotId[ i ] == ulCurrId;
			}
			return nPrev > 1 || nCurr > 1;
		}

		// Release everything. DECISION: g_device.waitIdle() first -- the library
		// has no deferred free, and earlier records (ours and the composite's
		// that sampled our outputs) may still be on the queue.
		void Teardown( Host_t &H )
		{
			capture::Abort( "teardown" );   // a running capture ends and still writes what it has
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
			H.uAppliedReach = 0;
			H.ulAppliedV3 = 0;
			H.nBlendLogged = 0;
			H.nV35Logged = 0;
			H.nDebugViewHandled = 0;
			g_nDebugViewActive.store( 0, std::memory_order_relaxed );
			g_bDebugViewRefused.store( false, std::memory_order_relaxed );
			g_bBlendActive.store( false, std::memory_order_relaxed );
			g_bV35Active.store( false, std::memory_order_relaxed );
			g_pszBlendWhy.store( "", std::memory_order_relaxed );
			g_uGate.fetch_and( ~kGateLive, std::memory_order_relaxed );
			g_flLastPairGpuMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastEstimateMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastSynthMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastUiMs.store( -1.0f, std::memory_order_relaxed );
			g_pszFormatTag.store( "", std::memory_order_relaxed );
			g_pszMotionNote.store( "", std::memory_order_relaxed );
			H.bHaveColorspace = false;
			H.ulUiFedId = 0;   // the library's counters went with the Interpolator
			H.uTrackedRun = 0;
		}

		bool CreateTexture( gamescope::Rc<CVulkanTexture> &out, uint32_t w, uint32_t h, const FormatPlan &plan )
		{
			CVulkanTexture::createFlags flags;
			flags.bSampled = true;   // the library samples prev/curr; the composite samples outputs
			flags.bStorage = true;   // the copy shader / the library writes them
			flags.bTransferSrc = true;   // ritz_fg_capture copies ring frames and outputs into its staging images

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
			uint64_t ulStamp[ kRingMax ] = {};
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
				ulStamp[ n ] = H.ulSlotWrite[ nSrc ];
				n++;
			}
			for ( int i = 0; i < kRingMax; i++ )
			{
				H.pRing[ i ] = i < nNew ? std::move( pTex[ i ] ) : nullptr;
				H.ulSlotId[ i ] = i < nNew ? ulId[ i ] : 0;
				H.ulSlotWrite[ i ] = i < nNew ? ulStamp[ i ] : 0;
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
				^ ( uint64_t( plan.eFormat ) << 50 ) ^ ( uint64_t( plan.eTransfer ) << 53 ) ^ 0x8000000000000000ull
				// pyramidMinSide is structural and can make reconfigure() fail: a changed value must retry.
				^ ( uint64_t( c.pyramidMinSide ) * 0x9E3779B97F4A7C15ull );
		}

		// V3 Hybrid's honest state: asks the live interpolator (render thread only, the one that owns it) whether the
		// learned blend runs, publishes that for the status line (BlendActive() / BlendWhy()) and logs a change once.
		// Called at the end of every EnsureReady(), so it follows a rebuild (the format changes at runtime: the launcher
		// window is 8-bit, a game may be 10-bit) and every settings change. The query is a few compares.
		// `Why a reason here:` the library falls back to V3's rule without an error for a format without a blend build,
		// with the detail split on, or when V3 itself fell back; the status must not claim a variant that is not running.
		void PublishBlend( Host_t &H, const Config &c )
		{
			const int nSel = c.motion == Motion::V3 ? c.v3Blend : 0;
			const bool bActive = nSel != 0 && H.interp.v3BlendActive();
			const char *pszWhy = "";
			if ( nSel != 0 && !bActive )
			{
				const framegen::Format eFmt = H.plan.eFormat;
				if ( eFmt != framegen::Format::Rgba8 && eFmt != framegen::Format::Rgb10 )
					pszWhy = "format";
				else if ( H.interp.settings().v3DetailSplit )
					pszWhy = "detail split";
			}
			g_bBlendActive.store( bActive, std::memory_order_relaxed );
			g_pszBlendWhy.store( pszWhy, std::memory_order_relaxed );
			// V3.5: logged on a change of state (0 not asked, 1 running, 2 asked but not running).
			const bool bV35 = c.motion == Motion::V3 && c.v3Motion == 1 && H.interp.v3MotionActive();
			g_bV35Active.store( bV35, std::memory_order_relaxed );
			const int nV35 = c.motion != Motion::V3 || c.v3Motion != 1 ? 0 : bV35 ? 1 : 2;
			if ( nV35 != H.nV35Logged )
			{
				H.nV35Logged = nV35;
				fg_log.infof( "frame generation: Motion version 3 motion detection %s", nV35 == 1 ? "V3.5" : nV35 == 2 ? "V3.5 requested but not active (V3's own runs)" : "V3 (own)" );
			}
			const int nKey = nSel * 4 + ( nSel == 0 ? 0 : bActive ? 1 : 2 );
			if ( nKey != H.nBlendLogged )
			{
				H.nBlendLogged = nKey;
				if ( nSel == 0 )
					fg_log.infof( "frame generation: Motion version 3 learned blend off" );
				else if ( bActive )
					fg_log.infof( "frame generation: Motion version 3 learned blend %s", BlendText( nSel ) );
				else
					fg_log.infof( "frame generation: Motion version 3 learned blend %s not active%s%s", BlendText( nSel ),
						pszWhy[ 0 ] ? ": " : "", pszWhy );
			}
		}

		// The library's debug view (Settings::debugView, superdoc/features/frame-generation-debug.md). `nView` is the Debug view
		// row's index. A runtime setSettings(): no reconfigure, no estimate dropped, so a change neither resets pacing nor shows
		// a real frame. Applied on top of the library's CURRENT settings (never the freshly resolved ones), so the view can
		// never be the reason another setting stays unapplied; a refusal (the debug shader variants of this format are not built
		// in, or the pipeline could not be created) leaves the settings unchanged, is logged and published once per change.
		// `Why` a view change alone is applied here and not through the library-config key: that key is full (64 bits), and a
		// view change must not take the reconfigure / motion-resolution path at all.
		void ApplyDebugView( Host_t &H, int nView )
		{
			if ( nView == H.nDebugViewHandled )
				return;
			H.nDebugViewHandled = nView;
			framegen::Settings st = H.interp.settings();
			st.debugView = framegen::DebugView( LibraryDebugView( nView ) );
			if ( H.interp.setSettings( st ) )
			{
				g_nDebugViewActive.store( nView, std::memory_order_relaxed );
				g_bDebugViewRefused.store( false, std::memory_order_relaxed );
				fg_log.infof( "frame generation: debug view %s", DebugViewName( nView ) );
				return;
			}
			const char *pszWhy = H.interp.settingsError( st );
			g_bDebugViewRefused.store( true, std::memory_order_relaxed );
			fg_log.errorf( "frame generation: debug view %s not available: %s", DebugViewName( nView ),
				pszWhy ? pszWhy : "the library could not create its pipeline" );
			// Fall back to no view at all, so the picture is never marked with another view than the one the row names.
			st.debugView = framegen::DebugView::Off;
			if ( H.interp.setSettings( st ) )
				g_nDebugViewActive.store( 0, std::memory_order_relaxed );
		}

		// Brings the library to (w, h, cfg): init the first time, resize on a size
		// change, apply a changed preset. False = not usable this frame.
		bool EnsureReady( Host_t &H, uint32_t w, uint32_t h, const FormatPlan &plan, const Config &c, uint32_t uPackedConfig, const BlurConfig &b, uint32_t uPackedBlur, uint32_t uPackedLearned, int nDebugView )
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
				capture::Abort( "format" );
				Teardown( H );
			}

			framegen::Settings want = ToLibrarySettings( c, b, H.nUiHist );

			// Only the library's own presets (quality, safety, HUD protection, UI
			// protection (safety spans bits 6-7 and 30), and the blur's sample count and weights) matter to what is
			// already applied; mode, priority, target and the blur's amount / relative
			// are pacing's business and never reconfigure it. The blur sample count is
			// 0 while the blur is off, so its enabled bit is part of the key.
			const uint32_t uBlurLib = uPackedBlur & ( 0xFu | ( 1u << 12 ) | kBlurEnabledBit );
			const uint64_t uLibraryConfig = uint64_t( uPackedConfig & ( 0x3F0u | ( 0x3u << 24 ) | ( 0x3u << 28 ) | ( 0x1u << 30 ) ) ) | ( uint64_t( uBlurLib ) << 32 )
				| ( uint64_t( c.uiBoxTenths ) << 48 ) | ( uint64_t( uPackedLearned & 0x7Fu ) << 56 )
				| ( uint64_t( c.ui == UiProt::CrosshairV2 && c.uiWholeFrame ? 1 : 0 ) << 63 );   // Whole screen V2's explicit full-frame box

			const uint32_t uReach = PackReach( c );
			const uint64_t ulV3 = PackV3( c );

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
				// Lets the library refuse V2 on a GPU without the subgroup operations it needs.
				info.getProps2 = g_device.vk.GetPhysicalDeviceProperties2;
				info.pipelineCache = VK_NULL_HANDLE;
				// Always V1 here: init() refuses V2 / Learned outright where the device lacks the
				// subgroup operations (or a format's V2 shaders are missing), which would be
				// "unavailable", not "fall back". V2 / Learned is switched on below once it is
				// known to be accepted (setSettings creates its pipelines, no GPU wait).
				info.settings = want;
				info.settings.motionVersion = framegen::MotionVersion::V1;

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
				// ponytail: V2 / Learned is not switched on here but by the change block below (it
				// needs its own flowScale, see there), so the applied key is left
				// unequal to the wanted one and the code falls through.
				const bool bWantV2 = want.motionVersion != framegen::MotionVersion::V1;
				H.uAppliedConfig = bWantV2 ? ~uLibraryConfig : uLibraryConfig;
				H.uAppliedReach = uReach;   // init() took want's reach values (info.settings = want)
				H.ulAppliedV3 = ulV3;       // ... and its V3 values
				H.bProfiling = g_device.supportsTimestamps() && H.interp.setProfiling( true );
				fg_log.infof( "frame generation ready: %ux%u, flowScale %u, GPU timing %s, format %s%s",
					w, h, want.flowScale, H.bProfiling ? "on" : "n/a",
					plan.eFormat == framegen::Format::Rgb10 ? "10-bit"
						: plan.eFormat == framegen::Format::RgbaF16
							? ( plan.eTransfer == framegen::Transfer::Linear ? "fp16 linear (scRGB)" : "fp16 encoded" ) : "8-bit",
					ColorspaceIsHDR( H.eColorspace ) ? ", HDR layer" : "" );
				if ( !bWantV2 )
				{
					PublishBlend( H, c );
					ApplyDebugView( H, nDebugView );
					return true;
				}
			}

			if ( w != H.uWidth || h != H.uHeight )
			{
				// A resolution change. resize() frees buffers earlier records still use.
				capture::Abort( "resize" );
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
					const char *pszSize = H.interp.sizeError( w, h );
					fg_log.errorf( "frame generation unavailable: could not resize to %ux%u: %s", w, h,
						pszSize ? pszSize : "see the log above" );
					Teardown( H );
					H.ulFailedKey = ulKey;
					return false;
				}
				// resize() keeps the library's active settings; the preset check below
				// applies a pending change.
			}

			if ( uLibraryConfig != H.uAppliedConfig || uReach != H.uAppliedReach || ulV3 != H.ulAppliedV3 )
			{
				want = ResolveMotion( H, want );
				// V2 always estimates at the library's own flowScale 2 / sub-pixel (its
				// measured numbers), so the one Quality setting is only V2's mode there:
				// a Quality change while V2 runs is a plain setSettings() (no GPU wait, no
				// lost estimate). V1's quality (flowScale 4 + no sub-pixel for Performance)
				// is applied when the version goes back to V1 -- which drops the estimate
				// anyway, so its reconfigure() wait costs nothing extra; the same goes
				// for a V1 (Performance) -> V2 switch.
				// Learned the same, for another reason: its nets were trained at flowScale 2 and the
				// library refuses any other (the Quality row's Performance is ignored while it runs).
				// V3 is the exception: it honours flowScale (1, 2, 4), so it follows the Quality row like V1
				// (Quality 2, Performance 4: 0.89 ms against 0.44 ms per real pair at 1080p) and keeps the
				// row's sub-pixel value, which it ignores.
				if ( want.motionVersion != framegen::MotionVersion::V1 && want.motionVersion != framegen::MotionVersion::V3 )
				{
					want.flowScale = 2;
					want.subpixel = true;
				}
				const framegen::MotionVersion eMotionWas = H.interp.settings().motionVersion;
				const framegen::UiProtection eUiWas = H.interp.settings().uiProtection;
				const float flUiBoxWas = H.interp.settings().uiBoxHeightFrac;
				const uint32_t uUiBoxWWas = H.interp.settings().uiBoxWidth, uUiBoxHWas = H.interp.settings().uiBoxHeight;
				const bool bFlowChange = want.flowScale != H.interp.settings().flowScale;
				const bool bPyramidChange = want.pyramidMinSide != H.interp.settings().pyramidMinSide;
				if ( bFlowChange || bPyramidChange )
				{
					// Structural (Quality <-> Performance, or the pyramid's smallest level): reconfigure() re-creates the
					// size-dependent buffers, so every earlier record must have retired,
					// and the next synth needs a new estimate.
					g_device.waitIdle();   // WAITIDLE 3 of 3: Quality preset change
					if ( !H.interp.reconfigure( want ) )
					{
						fg_log.errorf( "frame generation unavailable: could not reconfigure (flowScale %u, pyramidMinSide %u; changed: %s)",
							want.flowScale, want.pyramidMinSide,
							bFlowChange && bPyramidChange ? "both" : bFlowChange ? "flowScale" : "pyramidMinSide" );
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
					// V3.5's pipelines are created here the first time (about 275 ms with an empty shader cache): if that fails, retry as plain
					// V3 / V3 Hybrid so frame generation never ends up on a refused request; the status shows `V3.5(off)`.
					bool bSet = H.interp.setSettings( want );
					if ( !bSet && want.v3Motion != 0 )
					{
						fg_log.errorf( "frame generation: the library could not create V3.5 (its pipelines); running Motion version 3 with its own motion detection" );
						want.v3Motion = framegen::kV3MotionClassic;
						bSet = H.interp.setSettings( want );
					}
					if ( !bSet )
					{
						if ( want.motionVersion == framegen::MotionVersion::V2 )
							g_pszMotionNote.store( "the library could not create V2", std::memory_order_relaxed );
						else if ( want.motionVersion == framegen::MotionVersion::V3 )
							g_pszMotionNote.store( want.v3Guard != framegen::V3Guard::Off && want.v3Guard != H.interp.settings().v3Guard
								? "the library could not create the Motion guard (V2's pipelines or buffers)"
								: "the library could not create Motion version 3 (its pipelines or buffers)", std::memory_order_relaxed );
						else if ( want.motionVersion == framegen::MotionVersion::Learned )
							g_pszMotionNote.store( "the library could not create Learned motion (its pipelines or buffers)", std::memory_order_relaxed );
						if ( !H.bUiSetFailLogged )
						{
							H.bUiSetFailLogged = true;
							fg_log.errorf( "frame generation: the library refused the new settings; keeping the old ones" );
						}
					}
				}
				// A V1 <-> V2 switch dropped the library's estimate (its synths now refuse):
				// forget it so the next paint records a fresh estimate first, and show no
				// output the other version made. ponytail: releaseUnusedMotion() is never
				// called -- it needs a GPU wait and there is no natural spot (the ring-shrink
				// wait is rare); the unused version's pipelines/buffers stay until teardown.
				if ( H.interp.settings().motionVersion != eMotionWas )
				{
					H.bEstimateValid = false;
					InvalidateOutputs( H );
				}
				// A change of the UI-protection mode or its box drops the library's estimate too (its synths refuse
				// until the next recordEstimate): forget ours so the pair is re-estimated at the next paint instead of
				// showing real frames, flagged as a failed step, for the rest of the pair.
				else if ( H.interp.settings().uiProtection != eUiWas || H.interp.settings().uiBoxHeightFrac != flUiBoxWas
					|| H.interp.settings().uiBoxWidth != uUiBoxWWas || H.interp.settings().uiBoxHeight != uUiBoxHWas )
					H.bEstimateValid = false;
				// A V3 Hybrid variant switch (or V3 <-> V3 Hybrid) is only a runtime setting: the same library motion
				// version, no estimate dropped, no GPU wait. PublishBlend() below logs it.
				H.uAppliedConfig = uLibraryConfig;
				H.uAppliedReach = uReach;
				H.ulAppliedV3 = ulV3;
			}
			PublishBlend( H, c );
			ApplyDebugView( H, nDebugView );
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
				// Motion version 3's synth is `project` (when it has interior samples) then `synth` / `blur synth`; its
				// first stamp of a group starts a synth, and a blurred output is the one `blur synth` stamp
				// (counted as blurSamples synths below, like V1's `blur lookup`). Without this a V3 synth would be
				// counted as estimate time, idle gap included (15 ms against 0.9 measured; found 2026-11-19).
				const bool bV3Project = !strcmp( pszName, "project" );
				const bool bV3Synth = ( !strcmp( pszName, "synth" ) || !strcmp( pszName, "blur synth" ) )
					&& strcmp( pszPrev, "project" ) != 0;
				const bool bV3BlurSynth = !strcmp( pszName, "blur synth" );
				const bool bSynthStart = !bObserve
					&& ( !strcmp( pszName, "lookup" ) || !strcmp( pszName, "blur lookup" ) || bV3Project || bV3Synth
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
				if ( bV3BlurSynth )
					nBlurOutputs++;
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
		// The box as the library places it (Interpolator::boxRect: centred, the host
		// sets no offset) and the work rectangle it protects (workRect: the box grown
		// by the mode's margin, 2 px for V1, 1 px for V2). Asked of the library itself,
		// with the live Config's mode, so the preview can never disagree with it.
		void BoxRects( uint32_t w, uint32_t h, int nTenths, UiProt eUi, bool bWholeFrame, int32_t box[ 4 ], int32_t work[ 4 ] )
		{
			framegen::Settings st;
			UiBoxSettings( eUi == UiProt::CrosshairV2, bWholeFrame, nTenths, &st.uiBoxHeightFrac, &st.uiBoxWidth, &st.uiBoxHeight );
			switch ( eUi )
			{
				case UiProt::Off:         st.uiProtection = framegen::UiProtection::Off; break;
				case UiProt::Crosshair:   st.uiProtection = framegen::UiProtection::Crosshair; break;
				case UiProt::WholeScreen: st.uiProtection = framegen::UiProtection::WholeScreen; break;
				case UiProt::CrosshairV2: st.uiProtection = framegen::UiProtection::CrosshairV2; break;
			}
			framegen::Interpolator::boxRect( st, w, h, box );
			framegen::Interpolator::workRect( st, w, h, work );
		}

		// Records the crop pass for a NEW real frame into pCb, when the overlay wants
		// it, none is in flight and the last was >= 100 ms ago. Returns true when it
		// recorded: the caller submits pCb and then calls BoxPreviewSubmitted(seq).
		bool BoxPreviewRecord( CVulkanCmdBuffer *pCb, const gamescope::Rc<CVulkanTexture> &pLayer0, const FormatPlan &plan, const Config &cfg )
		{
			// Whole screen V2: the box is the whole frame, a crop of it shows nothing (the overlay hides the preview).
			if ( cfg.ui == UiProt::CrosshairV2 && cfg.uiWholeFrame )
				return false;
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
			int32_t box[ 4 ], work[ 4 ];
			BoxRects( w, h, cfg.uiBoxTenths, cfg.ui, cfg.uiWholeFrame, box, work );
			const int bx = box[ 0 ], by = box[ 1 ];
			const uint32_t bw = uint32_t( box[ 2 ] ), bh = uint32_t( box[ 3 ] );

			// The crop: the box with its own size again around it (a quarter of the box
			// on every side is the least that shows what the box misses), at least 48
			// px, never more than the frame, and always the work rectangle (the box
			// plus its 1-2 px margin) with some context. Reduced by a whole factor when
			// it would not fit the picture, so large boxes on large frames stay exact means.
			const uint32_t uBoxMax = std::max( bw, bh );
			const uint32_t uWorkMax = std::max( uint32_t( work[ 2 ] ), uint32_t( work[ 3 ] ) );
			uint32_t uSide = std::min( std::max( { uBoxMax * 2u, uWorkMax + 16u, 48u } ), std::min( w, h ) );
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
			m.uWorkW = uint32_t( work[ 2 ] );
			m.uWorkH = uint32_t( work[ 3 ] );
			m.flWorkX = float( work[ 0 ] - nX0 ) / float( uFactor );
			m.flWorkY = float( work[ 1 ] - nY0 ) / float( uFactor );
			m.flWorkW = float( work[ 2 ] ) / float( uFactor );
			m.flWorkH = float( work[ 3 ] ) / float( uFactor );
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
			g_BoxPreview.uWorkW = m.uWorkW;
			g_BoxPreview.uWorkH = m.uWorkH;
			g_BoxPreview.flWorkX = m.flWorkX;
			g_BoxPreview.flWorkY = m.flWorkY;
			g_BoxPreview.flWorkW = m.flWorkW;
			g_BoxPreview.flWorkH = m.flWorkH;
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

		// ---- the stale real-frame guard (StaleFrame.h; superdoc/features/frame-generation-stale-frames.md) ----
		// A new commit's buffer is not trusted to hold new content: before it is copied into the ring, a probe
		// compares 64x64 texels of it with the ring's newest, 2-back and 3-back frames. A frame identical to an
		// older one and not to the newest is a step back in time: it never enters the ring (so no pair can name
		// it: FindSlot fails), and while it is the newest commit the newest GOOD ring frame is shown instead of
		// layer 0 (which is that stale buffer). Synchronous (submit + wait) because a synth of this very paint
		// may use the new frame as `curr`.
		gamescope::ConVar<bool> cv_ritz_fg_stale_check( "ritz_fg_stale_check", true,
			"Frame generation: probe every new game frame and drop one that has the content of an earlier frame (0 = no probe, the old behaviour)." );
		gamescope::ConVar<int> cv_ritz_fg_stale_test( "ritz_fg_stale_test", 0,
			"Frame generation test: n > 0 marks every n-th new game frame stale on purpose (the fail-safe path); 0 = off." );
		gamescope::ConVar<bool> cv_ritz_fg_handback_check( "ritz_fg_handback_check", true,
			"Frame generation: do not use a new game frame whose buffer had already been handed back to the game when it was first taken (explicit sync only; 0 = use it)." );

		struct StaleState_t
		{
			uint64_t ulStaleId = 0;        // the newest commit judged stale (never in the ring); 0 = none
			uint64_t ulNewFrames = 0;      // new real frames the guard saw (the forced test's clock)
			uint64_t ulLastLogNs = 0;      // rate limit of the stale line
			uint64_t ulSuppressed = 0;     // stale lines not printed since the last one
			uint64_t ulLastRepeatLogNs = 0;
			uint64_t ulRepeatsAtLog = 0;
			// Two image sets: [0] the synchronous probe, [1] the diagnostic second look (read later, see StaleDiag*).
			gamescope::Rc<CVulkanTexture> pMask[ 2 ];      // 64x64 rgba8: the probe's per-texel equality
			gamescope::Rc<CVulkanTexture> pStaging[ 2 ];   // its mappable copy
			bool bInitFailLogged = false;
			// The diagnostic second look: the game's buffer of the newest ring frame probed again against the ring
			// copy taken of it, at the end of the host's command buffer of the paint that copied it (the game must
			// not write a buffer gamescope still holds, so any difference means its rendering landed after the copy).
			uint64_t ulDiagId = 0;          // the last frame given a second look
			uint64_t ulDiagPendingId = 0;   // recorded, not read yet (0 = none)
			uint64_t ulDiagSeq = ~0ull;     // its submission (~0 = recorded, not submitted yet)
			uint64_t ulDiagLastLogNs = 0;
			uint64_t ulDiagSuppressed = 0;
			// What the compositor knew about the commit of the newest probed frame (FrameGen_CommitFacts()), and
			// of the frame given the second look, as it was at its copy.
			stale::CommitFacts lastFacts;
			uint64_t ulLastFactsId = 0;
			stale::CommitFacts diagFacts;
			uint64_t ulHandedLastLogNs = 0;
			uint64_t ulHandedSuppressed = 0;
		};
		StaleState_t &g_Stale = *new StaleState_t();   // render thread only; leaked on purpose like the Host (no ~CVulkanTexture at exit)
		// Read by ritz_fg_stale_stats (console thread).
		std::atomic<uint64_t> g_ulStaleCount{ 0 }, g_ulStaleForced{ 0 }, g_ulRepeatCount{ 0 }, g_ulProbeCount{ 0 };
		std::atomic<uint64_t> g_ulProbeNsSum{ 0 }, g_ulProbeNsMax{ 0 };
		std::atomic<uint64_t> g_ulHeldPaints{ 0 };   // paints that showed the newest good ring frame instead of a stale layer 0
		std::atomic<uint64_t> g_ulDiagChecks{ 0 }, g_ulDiagChanged{ 0 };
		// The readiness / hand-back diagnostics (StaleFrame.h CommitFacts): new frames refused because the game
		// already had their buffer back; the facts of the stale verdicts and of the "changed after taken" frames.
		std::atomic<uint64_t> g_ulHandedBack{ 0 };
		std::atomic<uint64_t> g_ulTakenExplicit{ 0 }, g_ulTakenReadyAtCommit{ 0 }, g_ulTakenRecommit{ 0 };   // every new frame taken
		std::atomic<uint64_t> g_ulStaleReleased{ 0 }, g_ulStaleRecommit{ 0 }, g_ulStaleReadyAtCommit{ 0 }, g_ulStaleExplicit{ 0 }, g_ulStaleWritePending{ 0 };
		std::atomic<uint64_t> g_ulChangedReleased{ 0 }, g_ulChangedRecommit{ 0 }, g_ulChangedWritePending{ 0 };

		// The game buffer's implicit write fence (DMA_BUF_IOCTL_EXPORT_SYNC_FILE with the read flag: the fences a
		// reader waits for), polled without waiting: 1 still pending, 0 signalled, -1 unknown. With explicit
		// sync the driver may attach none, which reads as signalled.
		int WriteFencePending( const gamescope::Rc<CVulkanTexture> &pTex )
		{
#ifdef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
			const wlr_dmabuf_attributes &dmabuf = pTex->dmabuf();
			if ( dmabuf.n_planes < 1 || dmabuf.fd[ 0 ] < 0 )
				return -1;
			dma_buf_export_sync_file req = {};
			req.flags = DMA_BUF_SYNC_READ;
			req.fd = -1;
			if ( ioctl( dmabuf.fd[ 0 ], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req ) != 0 || req.fd < 0 )
				return -1;
			pollfd pfd = {};
			pfd.fd = req.fd;
			pfd.events = POLLIN;
			const int n = poll( &pfd, 1, 0 );
			close( req.fd );
			return n > 0 ? 0 : n == 0 ? 1 : -1;
#else
			(void)pTex;
			return -1;
#endif
		}

		bool StaleProbeImages( int nSet )
		{
			if ( g_Stale.pMask[ nSet ] && g_Stale.pStaging[ nSet ] )
				return true;
			CVulkanTexture::createFlags mask;
			mask.bStorage = true;
			mask.bSampled = true;
			mask.bTransferSrc = true;
			CVulkanTexture::createFlags staging;
			staging.bMappable = true;
			staging.bTransferDst = true;
			gamescope::Rc<CVulkanTexture> pMask = new CVulkanTexture();
			gamescope::Rc<CVulkanTexture> pStaging = new CVulkanTexture();
			if ( !pMask->BInit( stale::kGrid, stale::kGrid, 1u, DRM_FORMAT_ABGR8888, mask, nullptr )
				|| !pStaging->BInit( stale::kGrid, stale::kGrid, 1u, DRM_FORMAT_ABGR8888, staging, nullptr )
				|| !pStaging->mappedData() )
			{
				if ( !g_Stale.bInitFailLogged )
				{
					g_Stale.bInitFailLogged = true;
					fg_log.errorf( "frame generation: could not create the stale-frame probe's images; frames are not checked" );
				}
				return false;
			}
			g_Stale.pMask[ nSet ] = std::move( pMask );
			g_Stale.pStaging[ nSet ] = std::move( pStaging );
			return true;
		}

		// Records the probe of pTest against up to three references (null = unused) into pCb, image set nSet.
		void StaleProbeRecord( CVulkanCmdBuffer *pCb, int nSet, const gamescope::Rc<CVulkanTexture> &pTest, const gamescope::Rc<CVulkanTexture> *pRefs )
		{
			pCb->bindPipeline( g_device.pipeline( SHADER_TYPE_FG_STALE_PROBE ) );
			for ( int i = 0; i <= stale::kMaxRef; i++ )
			{
				const gamescope::Rc<CVulkanTexture> &p = ( i > 0 && pRefs[ i - 1 ] ) ? pRefs[ i - 1 ] : pTest;
				pCb->bindTexture( i, p );
				pCb->setTextureSrgb( i, true );             // raw UNORM views: the encoded values, as the ring copy reads and writes them
				pCb->setSamplerUnnormalized( i, true );
				pCb->setSamplerNearest( i, true );
			}
			pCb->bindTarget( g_Stale.pMask[ nSet ] );
			pCb->dispatch( stale::kGrid / 8, stale::kGrid / 8 );
			for ( int i = 0; i <= stale::kMaxRef; i++ )
				pCb->bindTexture( i, nullptr );
			pCb->copyImage( g_Stale.pMask[ nSet ], g_Stale.pStaging[ nSet ] );
		}

		// Counts the equal texels per reference of image set nSet (its submission must have retired).
		void StaleProbeCount( int nSet, int pnEqual[ stale::kMaxRef ] )
		{
			const uint8_t *pPix = g_Stale.pStaging[ nSet ]->mappedData();
			const uint32_t uPitch = g_Stale.pStaging[ nSet ]->rowPitch();
			for ( int i = 0; i < stale::kMaxRef; i++ )
				pnEqual[ i ] = 0;
			for ( int y = 0; y < stale::kGrid; y++ )
			{
				const uint8_t *pRow = pPix + size_t( y ) * uPitch;
				for ( int x = 0; x < stale::kGrid; x++ )
				{
					for ( int i = 0; i < stale::kMaxRef; i++ )
						pnEqual[ i ] += pRow[ x * 4 + i ] >= 128 ? 1 : 0;
				}
			}
		}

		// Probes pTest against up to three references (null = unused) and counts the equal texels per reference.
		// Submits its own command buffer and WAITS for it. False = the probe could not run (no verdict).
		bool StaleProbe( const gamescope::Rc<CVulkanTexture> &pTest, const gamescope::Rc<CVulkanTexture> *pRefs, int pnEqual[ stale::kMaxRef ] )
		{
			if ( !StaleProbeImages( 0 ) )
				return false;
			const uint64_t ulStartNs = get_time_in_nanos();
			std::unique_ptr<CVulkanCmdBuffer> pCb = g_device.commandBuffer();
			StaleProbeRecord( pCb.get(), 0, pTest, pRefs );
			const uint64_t ulSeq = g_device.submit( std::move( pCb ) );
			g_device.wait( ulSeq );
			StaleProbeCount( 0, pnEqual );
			const uint64_t ulNs = get_time_in_nanos() - ulStartNs;
			g_ulProbeCount.fetch_add( 1, std::memory_order_relaxed );
			g_ulProbeNsSum.fetch_add( ulNs, std::memory_order_relaxed );
			if ( ulNs > g_ulProbeNsMax.load( std::memory_order_relaxed ) )
				g_ulProbeNsMax.store( ulNs, std::memory_order_relaxed );
			return true;
		}

		// True = the new real frame ulNewestId must not be used (judged stale now, or already before). Called only
		// for a newestId that is not the ring's newest, right before the ring copy.
		bool StaleGuard( Host_t &H, const gamescope::Rc<CVulkanTexture> &pLayer0, uint64_t ulNewestId )
		{
			if ( !cv_ritz_fg_stale_check )
			{
				g_Stale.ulStaleId = 0;
				return false;
			}
			if ( ulNewestId == g_Stale.ulStaleId )
				return true;   // judged before: a repaint while it is still the newest commit
			g_Stale.ulNewFrames++;
			if ( !H.bHaveCur || H.nRing <= 0 )
				return false;   // nothing to compare with (and nothing else to show)

			// What the compositor knows about this commit, now that it is first taken (logged with a verdict, and
			// kept for the second look). A buffer the game already has back may be receiving its NEXT frame while
			// it is read: not used (the frame to distrust is this one, not the later frame that then looks
			// "stale"). See superdoc/features/frame-generation-stale-frames.md, "The early hand-back".
			stale::CommitFacts facts;
			FrameGen_CommitFacts( ulNewestId, &facts );
			facts.nWritePending = WriteFencePending( pLayer0 );
			g_Stale.lastFacts = facts;
			g_Stale.ulLastFactsId = ulNewestId;
			if ( facts.bExplicitSync )
				g_ulTakenExplicit.fetch_add( 1, std::memory_order_relaxed );
			if ( facts.bReadyAtCommit )
				g_ulTakenReadyAtCommit.fetch_add( 1, std::memory_order_relaxed );
			if ( facts.bRecommit )
				g_ulTakenRecommit.fetch_add( 1, std::memory_order_relaxed );
			char szFacts[ 384 ];
			stale::FormatFacts( facts, szFacts, sizeof( szFacts ) );
			if ( cv_ritz_fg_handback_check && facts.nReleased == 1 )
			{
				g_Stale.ulStaleId = ulNewestId;
				const uint64_t ulHanded = g_ulHandedBack.fetch_add( 1, std::memory_order_relaxed ) + 1;
				const uint64_t ulNowNs = get_time_in_nanos();
				if ( ulNowNs - g_Stale.ulHandedLastLogNs >= 1000000000ull )
				{
					fg_log.infof( "frame generation: game frame %llu had already been handed back to the game when it was taken (its buffer may hold a later frame); not used (%llu so far, %llu not logged since the last line; %s)",
						(unsigned long long)ulNewestId, (unsigned long long)ulHanded, (unsigned long long)g_Stale.ulHandedSuppressed, szFacts );
					g_Stale.ulHandedLastLogNs = ulNowNs;
					g_Stale.ulHandedSuppressed = 0;
				}
				else
				{
					g_Stale.ulHandedSuppressed++;
				}
				return true;
			}

			// The ring's frames by age: 0 = newest (nCurSlot), 1 = the one before, 2 = the one before that.
			gamescope::Rc<CVulkanTexture> pRefs[ stale::kMaxRef ];
			uint64_t ulRefId[ stale::kMaxRef ] = {};
			bool bValid[ stale::kMaxRef ] = {};
			for ( int i = 0; i < stale::kMaxRef && i < H.nRing; i++ )
			{
				const int nSlot = ( H.nCurSlot - i + H.nRing ) % H.nRing;
				if ( H.ulSlotId[ nSlot ] != 0 && H.pRing[ nSlot ] )
				{
					pRefs[ i ] = H.pRing[ nSlot ];
					ulRefId[ i ] = H.ulSlotId[ nSlot ];
					bValid[ i ] = true;
				}
			}

			stale::Result r;
			const bool bForced = stale::ForcedStale( cv_ritz_fg_stale_test, g_Stale.ulNewFrames );
			if ( bForced )
			{
				r.eVerdict = stale::Verdict::Stale;
			}
			else
			{
				int nEqual[ stale::kMaxRef ] = {};
				if ( !StaleProbe( pLayer0, pRefs, nEqual ) )
					return false;
				r = stale::Judge( stale::kGrid * stale::kGrid, nEqual, bValid, stale::kMaxRef );
			}

			const uint64_t ulNow = get_time_in_nanos();
			if ( r.eVerdict == stale::Verdict::Repeat )
			{
				const uint64_t ulRepeats = g_ulRepeatCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
				// Counted, not acted on (a still picture repeats too). At most one line a minute, so a morning
				// journal shows how often it happens.
				if ( ulNow - g_Stale.ulLastRepeatLogNs >= 60ull * 1000000000ull )
				{
					fg_log.infof( "frame generation: %llu new game frame(s) were plain repeats of the frame before (%llu so far; used as usual)",
						(unsigned long long)( ulRepeats - g_Stale.ulRepeatsAtLog ), (unsigned long long)ulRepeats );
					g_Stale.ulLastRepeatLogNs = ulNow;
					g_Stale.ulRepeatsAtLog = ulRepeats;
				}
				return false;
			}
			if ( r.eVerdict != stale::Verdict::Stale )
				return false;

			g_Stale.ulStaleId = ulNewestId;
			const uint64_t ulCount = g_ulStaleCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
			if ( bForced )
				g_ulStaleForced.fetch_add( 1, std::memory_order_relaxed );
			else
			{
				if ( facts.nReleased == 1 )
					g_ulStaleReleased.fetch_add( 1, std::memory_order_relaxed );
				if ( facts.bRecommit )
					g_ulStaleRecommit.fetch_add( 1, std::memory_order_relaxed );
				if ( facts.bReadyAtCommit )
					g_ulStaleReadyAtCommit.fetch_add( 1, std::memory_order_relaxed );
				if ( facts.bExplicitSync )
					g_ulStaleExplicit.fetch_add( 1, std::memory_order_relaxed );
				if ( facts.nWritePending == 1 )
					g_ulStaleWritePending.fetch_add( 1, std::memory_order_relaxed );
			}
			if ( ulNow - g_Stale.ulLastLogNs >= 1000000000ull )
			{
				const unsigned long long ulRepeats = (unsigned long long)g_ulRepeatCount.load( std::memory_order_relaxed );
				if ( bForced )
					fg_log.infof( "frame generation: game frame %llu marked stale by ritz_fg_stale_test; not used (%llu so far, %llu not logged since the last line)",
						(unsigned long long)ulNewestId, (unsigned long long)ulCount, (unsigned long long)g_Stale.ulSuppressed );
				else
					fg_log.infof( "frame generation: game frame %llu has the content of the frame %d before it (frame %llu); not used (%llu so far, %llu not logged since the last line; %llu plain repeats; %s)",
						(unsigned long long)ulNewestId, r.nBack, (unsigned long long)ulRefId[ r.nBack - 1 ], (unsigned long long)ulCount,
						(unsigned long long)g_Stale.ulSuppressed, ulRepeats, szFacts );
				g_Stale.ulLastLogNs = ulNow;
				g_Stale.ulSuppressed = 0;
			}
			else
			{
				g_Stale.ulSuppressed++;
			}
			return true;
		}

		// The newest commit is one the guard refused: layer 0 is that stale buffer.
		bool NewestIsStale( uint64_t ulNewestId )
		{
			return ulNewestId != 0 && ulNewestId == g_Stale.ulStaleId;
		}

		// The diagnostic second look, read side: once its submission retired (a counter read, never a wait),
		// count; any texel that differs means the game's buffer changed after the ring copy took it.
		void StaleDiagPoll()
		{
			if ( !g_Stale.ulDiagPendingId || g_Stale.ulDiagSeq == ~0ull || g_device.completedSeqNo() < g_Stale.ulDiagSeq )
				return;
			int nEqual[ stale::kMaxRef ] = {};
			StaleProbeCount( 1, nEqual );
			const uint64_t ulId = g_Stale.ulDiagPendingId;
			g_Stale.ulDiagPendingId = 0;
			g_ulDiagChecks.fetch_add( 1, std::memory_order_relaxed );
			const int nAll = stale::kGrid * stale::kGrid;
			if ( nEqual[ 0 ] >= nAll )
				return;
			const uint64_t ulChanged = g_ulDiagChanged.fetch_add( 1, std::memory_order_relaxed ) + 1;
			// Has the game got that buffer back by now (while it was still gamescope's newest frame at the copy)?
			stale::CommitFacts now;
			FrameGen_CommitFacts( ulId, &now );
			const int nReleasedNow = now.bFound ? now.nReleased : -1;
			if ( nReleasedNow == 1 || g_Stale.diagFacts.nReleased == 1 )
				g_ulChangedReleased.fetch_add( 1, std::memory_order_relaxed );
			if ( g_Stale.diagFacts.bRecommit )
				g_ulChangedRecommit.fetch_add( 1, std::memory_order_relaxed );
			if ( g_Stale.diagFacts.nWritePending == 1 )
				g_ulChangedWritePending.fetch_add( 1, std::memory_order_relaxed );
			const uint64_t ulNow = get_time_in_nanos();
			if ( ulNow - g_Stale.ulDiagLastLogNs >= 1000000000ull )
			{
				char szFacts[ 384 ];
				stale::FormatFacts( g_Stale.diagFacts, szFacts, sizeof( szFacts ) );
				fg_log.infof( "frame generation: game buffer of frame %llu changed after it was taken (%d of %d probed texels differ; %llu so far, %llu not logged since the last line; released to the game by now %s; at the copy %s)",
					(unsigned long long)ulId, nAll - nEqual[ 0 ], nAll, (unsigned long long)ulChanged, (unsigned long long)g_Stale.ulDiagSuppressed,
					now.bFound ? stale::TriText( nReleasedNow, "YES", "no" ) : "commit gone", szFacts );
				g_Stale.ulDiagLastLogNs = ulNow;
				g_Stale.ulDiagSuppressed = 0;
			}
			else
			{
				g_Stale.ulDiagSuppressed++;
			}
		}

		// The diagnostic second look, record side: at the end of the host's command buffer of the paint that copied
		// the newest frame into the ring, probe the game's buffer (layer 0, still held by gamescope) against that copy.
		bool StaleDiagRecord( Host_t &H, CVulkanCmdBuffer *pCb, const gamescope::Rc<CVulkanTexture> &pLayer0, uint64_t ulNewestId )
		{
			if ( !cv_ritz_fg_stale_check || !pCb || !H.bHaveCur || H.ulCurId != ulNewestId || g_Stale.ulDiagId == ulNewestId
				|| g_Stale.ulDiagPendingId || !H.pRing[ H.nCurSlot ] || !StaleProbeImages( 1 ) )
				return false;
			const gamescope::Rc<CVulkanTexture> pRefs[ stale::kMaxRef ] = { H.pRing[ H.nCurSlot ], nullptr, nullptr };
			StaleProbeRecord( pCb, 1, pLayer0, pRefs );
			g_Stale.diagFacts = g_Stale.ulLastFactsId == ulNewestId ? g_Stale.lastFacts : stale::CommitFacts{};
			g_Stale.ulDiagId = ulNewestId;
			g_Stale.ulDiagPendingId = ulNewestId;
			g_Stale.ulDiagSeq = ~0ull;   // set by StaleDiagSubmitted()
			return true;
		}

		void StaleDiagSubmitted( uint64_t ulSeq )
		{
			g_Stale.ulDiagSeq = ulSeq;
		}
	}

	// `gamescopectl ritz_fg_stale_stats`: the stale-frame guard's counters and the probe's cost.
	static gamescope::ConCommand cc_ritz_fg_stale_stats( "ritz_fg_stale_stats",
		"Frame generation: print the stale-frame guard's counters (stale frames dropped, plain repeats, probe time).",
		[]( std::span<std::string_view> )
		{
			const uint64_t ulProbes = g_ulProbeCount.load( std::memory_order_relaxed );
			uint64_t ulRecommitsKept = 0, ulParkedNow = 0;
			const uint64_t ulRecommits = FrameGen_RecommitCount( &ulRecommitsKept, &ulParkedNow );
			const double flMean = ulProbes ? double( g_ulProbeNsSum.load( std::memory_order_relaxed ) ) / double( ulProbes ) * 1e-6 : 0.0;
			console_log.infof( "ritz_fg_stale_stats: stale %llu (forced %llu), plain repeats %llu, held paints %llu, probes %llu, probe mean %.3f ms, max %.3f ms, "
				"second looks %llu, changed after taken %llu",
				(unsigned long long)g_ulStaleCount.load( std::memory_order_relaxed ), (unsigned long long)g_ulStaleForced.load( std::memory_order_relaxed ),
				(unsigned long long)g_ulRepeatCount.load( std::memory_order_relaxed ), (unsigned long long)g_ulHeldPaints.load( std::memory_order_relaxed ),
				(unsigned long long)ulProbes, flMean, double( g_ulProbeNsMax.load( std::memory_order_relaxed ) ) * 1e-6,
				(unsigned long long)g_ulDiagChecks.load( std::memory_order_relaxed ), (unsigned long long)g_ulDiagChanged.load( std::memory_order_relaxed ) );
			console_log.infof( "ritz_fg_stale_stats: handed back before taken %llu (not used); stale with: released %llu, re-commit %llu, ready at commit %llu, explicit sync %llu, write fence pending %llu; "
				"changed after taken with: released %llu, re-commit %llu, write fence pending %llu; re-commits without a new buffer %llu (release kept %llu, still held %llu); "
				"new frames taken with explicit sync %llu, ready at commit %llu, re-commit %llu",
				(unsigned long long)g_ulHandedBack.load( std::memory_order_relaxed ),
				(unsigned long long)g_ulStaleReleased.load( std::memory_order_relaxed ), (unsigned long long)g_ulStaleRecommit.load( std::memory_order_relaxed ),
				(unsigned long long)g_ulStaleReadyAtCommit.load( std::memory_order_relaxed ), (unsigned long long)g_ulStaleExplicit.load( std::memory_order_relaxed ),
				(unsigned long long)g_ulStaleWritePending.load( std::memory_order_relaxed ),
				(unsigned long long)g_ulChangedReleased.load( std::memory_order_relaxed ), (unsigned long long)g_ulChangedRecommit.load( std::memory_order_relaxed ),
				(unsigned long long)g_ulChangedWritePending.load( std::memory_order_relaxed ),
				(unsigned long long)ulRecommits, (unsigned long long)ulRecommitsKept, (unsigned long long)ulParkedNow,
				(unsigned long long)g_ulTakenExplicit.load( std::memory_order_relaxed ), (unsigned long long)g_ulTakenReadyAtCommit.load( std::memory_order_relaxed ),
				(unsigned long long)g_ulTakenRecommit.load( std::memory_order_relaxed ) );
		} );

	// ------------------------------------------------------------------
	//  Public API: config, gate, frame request, reset, status
	// ------------------------------------------------------------------

	void SetConfig( const Config &cfg )
	{
		const uint32_t uPacked = Pack( cfg );
		g_uConfig.store( uPacked, std::memory_order_relaxed );
		g_uUiBox.store( PackBox( cfg ), std::memory_order_relaxed );
		g_uLearned.store( PackLearned( cfg ), std::memory_order_relaxed );
		g_uReach.store( PackReach( cfg ), std::memory_order_relaxed );
		g_ulV3.store( PackV3( cfg ), std::memory_order_relaxed );
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
		framegen::Settings st;
		st.uiBoxHeightFrac = float( std::min( std::max( nTenths, kUiBoxMinTenths ), kUiBoxMaxTenths ) ) / 1000.0f;
		framegen::Interpolator::boxSize( st, uWidth, uHeight, puBoxW, puBoxH );
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

	void SetDebugConfig( const DebugConfig &cfg )
	{
		g_uDebug.store( PackDebug( cfg ), std::memory_order_relaxed );
	}

	DebugConfig GetDebugConfig()
	{
		return UnpackDebug( g_uDebug.load( std::memory_order_relaxed ) );
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
		return Unpack( g_uConfig.load( std::memory_order_relaxed ), g_uUiBox.load( std::memory_order_relaxed ), g_uLearned.load( std::memory_order_relaxed ), g_uReach.load( std::memory_order_relaxed ), g_ulV3.load( std::memory_order_relaxed ) );
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
		return g_uGate.load( std::memory_order_relaxed ) != 0 || capture::Pending();
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

	bool BlendActive() { return g_bBlendActive.load( std::memory_order_relaxed ); }
	bool V35Active() { return g_bV35Active.load( std::memory_order_relaxed ); }
	const char *BlendWhy() { return g_pszBlendWhy.load( std::memory_order_relaxed ); }
	int  DebugViewActive() { return g_nDebugViewActive.load( std::memory_order_relaxed ); }
	bool DebugViewRefused() { return g_bDebugViewRefused.load( std::memory_order_relaxed ); }

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
		s.motionNote = g_pszMotionNote.load( std::memory_order_relaxed );
		return s;
	}

	void PublishPacingStatus( const PacingStatus &status )
	{
		std::lock_guard<std::mutex> lock( g_PacingMutex );
		g_Pacing = status;
		g_Pacing.publishedNs = get_time_in_nanos();
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
		uint32_t uPackedConfig = g_uConfig.load( std::memory_order_relaxed );
		const uint32_t uPackedBlur = g_uBlur.load( std::memory_order_relaxed );
		const BlurConfig blurCfg = UnpackBlur( uPackedBlur );
		// The blur's own Motion version / Quality replace Frame generation's while the blur
		// is on (see BlurConfig::useOwnInterp). Done on the packed word so the library-config
		// key in EnsureReady() sees the effective values too.
		if ( blurCfg.enabled && blurCfg.useOwnInterp )
			uPackedConfig = ( uPackedConfig & ~( 0x3u << 4 | 0x3u << 28 ) ) | ( uint32_t( blurCfg.quality ) << 4 ) | ( uint32_t( blurCfg.motion ) << 28 );
		const uint32_t uPackedLearned = g_uLearned.load( std::memory_order_relaxed );
		Config cfg = Unpack( uPackedConfig, g_uUiBox.load( std::memory_order_relaxed ), uPackedLearned, g_uReach.load( std::memory_order_relaxed ), g_ulV3.load( std::memory_order_relaxed ) );
		// Motion blur's own Motion version has no V3 Hybrid entry: its V3 is plain V3, whatever Frame generation's row says
		// (the other V3 settings are shared; the blend is Frame generation's choice only).
		if ( blurCfg.enabled && blurCfg.useOwnInterp )
		{
			cfg.v3Blend = 0;
			cfg.v3Motion = 0;
		}
		const bool bLagOn = ( g_uLag.load( std::memory_order_relaxed ) & kLagEnabledBit ) != 0;

		// ritz_fg_capture (FrameGenCapture.h): idle = this one relaxed load. Otherwise it times out, ends on a
		// changed configuration and frees a finished burst; the key is any value that differs between two settings.
		uint64_t ulCaptureKey = 0;
		if ( capture::Pending() )
		{
			ulCaptureKey = 1469598103934665603ull;
			for ( const uint64_t ul : { uint64_t( uPackedConfig ), uint64_t( uPackedBlur ), uint64_t( uPackedLearned ),
				uint64_t( g_uUiBox.load( std::memory_order_relaxed ) ), uint64_t( g_uReach.load( std::memory_order_relaxed ) ), uint64_t( g_uDebug.load( std::memory_order_relaxed ) ),
				g_ulV3.load( std::memory_order_relaxed ), uint64_t( g_uLag.load( std::memory_order_relaxed ) ) } )
				ulCaptureKey = ( ulCaptureKey ^ ul ) * 1099511628211ull;
			capture::Poll( get_time_in_nanos(), ulCaptureKey );
		}

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
		StaleDiagPoll();

		// An armed capture whose time has come starts here, as soon as the renderer is live (also while pacing
		// passes real frames through: the burst then waits for generated frames, and times out if none come).
		if ( capture::Pending() )
		{
			const uint64_t ulNowNs = get_time_in_nanos();
			if ( capture::DueToStart( ulNowNs ) && g_pHost && ( g_uGate.load( std::memory_order_relaxed ) & kGateLive )
				&& g_pHost->nRing > 0 && g_pHost->pRing[ 0 ] )
			{
				const framegen::Settings sInEffect = g_pHost->interp.settings();
				const auto MotionText = []( framegen::MotionVersion e ) -> const char *
				{
					return e == framegen::MotionVersion::V2 ? "V2" : e == framegen::MotionVersion::Learned ? "Learned"
						: e == framegen::MotionVersion::V3 ? "V3" : "V1";
				};
				capture::Env env;
				env.uWidth = g_pHost->uWidth;
				env.uHeight = g_pHost->uHeight;
				env.nVkFormat = int( g_pHost->pRing[ 0 ]->format() );
				env.uDrmFormat = g_pHost->pRing[ 0 ]->drmFormat();
				env.pSettings = &sInEffect;
				env.pszMotionWanted = cfg.motion == Motion::V2 ? "V2" : cfg.motion == Motion::Learned ? "Learned" : cfg.motion == Motion::V3 ? "V3" : "V1";
				env.pszMotionEffective = MotionText( sInEffect.motionVersion );
				env.nBlend = cfg.motion == Motion::V3 ? cfg.v3Blend : 0;
				env.bBlendActive = g_pHost->interp.v3BlendActive();
				env.nV35 = cfg.motion == Motion::V3 ? cfg.v3Motion : 0;
				env.bV35Active = g_pHost->interp.v3MotionActive();
				env.nView = DebugViewActive();   // what the library really draws (0 when refused)
				capture::Start( env, ulNowNs, ulCaptureKey );
			}
		}

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

		if ( !EnsureReady( H, uWidth, uHeight, plan, cfg, uPackedConfig, blurCfg, uPackedBlur, uPackedLearned, UnpackDebug( g_uDebug.load( std::memory_order_relaxed ) ).view ) )
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
				vulkan_paint_split_begin( pCmd.get() );   // diagnostic timing split
				H.interp.beginFrame();   // once per command buffer, before its first record
			}
			return pCmd.get();
		};

		gamescope::Rc<CVulkanTexture> pResult;

		bool bObservedThisCb = false;
		bool bBoxPreviewRecorded = false;

		// D11: a newestId not seen before is a new real frame. Copy it into the next
		// ring slot (in this very command buffer) whatever this composite shows, so
		// later pairs always have it. Unless the stale guard refuses it (StaleGuard: its content is an older frame's).
		if ( ( !H.bHaveCur || req.newestId != H.ulCurId ) && !StaleGuard( H, pLayer0, req.newestId ) )
		{
			const int nNewSlot = H.bHaveCur ? ( H.nCurSlot + 1 ) % H.nRing : 0;
			H.nCurSlot = nNewSlot;
			H.bHaveCur = true;
			H.ulCurId = req.newestId;
			H.ulSlotId[ nNewSlot ] = req.newestId;
			H.ulSlotWrite[ nNewSlot ] = ++H.ulWriteClock;
			// The estimate and the cached outputs belong to pairs of frames; the
			// estimate's frames may now be gone from the ring (overwritten), the
			// outputs stay valid (they are finished images keyed by outId).
			if ( H.bEstimateValid && ( FindSlot( H, H.ulEstPrev ) < 0 || FindSlot( H, H.ulEstCurr ) < 0 ) )
				H.bEstimateValid = false;

			CVulkanCmdBuffer *pCb = Cmd();
			vulkan_paint_split_mark_pair();
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
				if ( capture::Running() )
				{
					// A repaint of a cached output is a generated frame too: flagged cached=1, no synth this composite.
					capture::FrameIn fi;
					fi.pOut = pHit->pTex;
					if ( nPrevSlot >= 0 )
						fi.pPrev = H.pRing[ nPrevSlot ];
					if ( nCurrSlot >= 0 )
						fi.pCurr = H.pRing[ nCurrSlot ];
					fi.ulOutId = req.outId;
					fi.ulPrevId = req.prevId;
					fi.ulCurrId = req.currId;
					fi.ulNewestId = req.newestId;
					fi.flT0 = req.t0;
					fi.flT1 = req.t1;
					fi.nSynthRet = 1;
					fi.bCached = true;
					fi.bMismatchHost = pHit->ulPrevId != req.prevId || pHit->ulCurrId != req.currId || CaptureDupIds( H, req.prevId, req.currId );
					fi.nOutSlot = int( pHit - H.out );
					fi.nPrevSlot = nPrevSlot;
					fi.nCurrSlot = nCurrSlot;
					capture::RecordFrame( Cmd(), fi );
				}
			}
			else if ( nPrevSlot >= 0 && nCurrSlot >= 0 )
			{
				CVulkanTexture *pPrev = H.pRing[ nPrevSlot ].get();
				CVulkanTexture *pCurr = H.pRing[ nCurrSlot ].get();
				bool bOk = true;
				bool bSynthRefused = false;   // the failing call was recordSynth() (not the estimate)
				int nEstimateRet = -1;        // recordEstimate's return when it was recorded in this composite (ritz_fg_capture)

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
					nEstimateRet = bOk ? 1 : 0;
					if ( bOk )
					{
						H.bEstContig = bObservedThisCb;
						H.bEstimateValid = true;
						H.ulEstPrev = req.prevId;
						H.ulEstCurr = req.currId;
						H.ulEstPrevStamp = H.ulSlotWrite[ nPrevSlot ];
						H.ulEstCurrStamp = H.ulSlotWrite[ nCurrSlot ];
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
					{
						pResult = pNear->pTex;
						if ( capture::Running() )
						{
							// ritz_fg_capture: the over-the-cap repaint, with the output's OWN pair, window and id.
							const int nNearPrev = FindSlot( H, pNear->ulPrevId ), nNearCurr = FindSlot( H, pNear->ulCurrId );
							capture::FrameIn fi;
							fi.pOut = pNear->pTex;
							if ( nNearPrev >= 0 )
								fi.pPrev = H.pRing[ nNearPrev ];
							if ( nNearCurr >= 0 )
								fi.pCurr = H.pRing[ nNearCurr ];
							fi.ulOutId = pNear->ulOutId;
							fi.ulPrevId = pNear->ulPrevId;
							fi.ulCurrId = pNear->ulCurrId;
							fi.ulNewestId = req.newestId;
							fi.flT0 = pNear->flT0;
							fi.flT1 = pNear->flT1;
							fi.nEstimateRet = nEstimateRet;
							fi.nSynthRet = 1;
							fi.bNear = true;
							fi.bMismatchHost = CaptureDupIds( H, pNear->ulPrevId, pNear->ulCurrId );
							fi.nOutSlot = int( pNear - H.out );
							fi.nPrevSlot = nNearPrev;
							fi.nCurrSlot = nNearCurr;
							capture::RecordFrame( Cmd(), fi );
						}
					}
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
							pSlot->ulPrevId = req.prevId;
							pSlot->ulCurrId = req.currId;
							pSlot->flT0 = req.t0;
							pSlot->flT1 = req.t1;
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
							if ( capture::Running() )
							{
								capture::FrameIn fi;
								fi.pOut = pSlot->pTex;
								fi.pPrev = H.pRing[ nPrevSlot ];
								fi.pCurr = H.pRing[ nCurrSlot ];
								fi.ulOutId = req.outId;
								fi.ulPrevId = req.prevId;
								fi.ulCurrId = req.currId;
								fi.ulNewestId = req.newestId;
								fi.flT0 = req.t0;
								fi.flT1 = req.t1;
								fi.nEstimateRet = nEstimateRet;
								fi.nSynthRet = 1;
								fi.bCached = false;
								fi.bStaleHost = nEstimateRet < 0 && CaptureStale( H, nPrevSlot, nCurrSlot );
								fi.bMismatchHost = H.ulEstPrev != req.prevId || H.ulEstCurr != req.currId || CaptureDupIds( H, req.prevId, req.currId );
								fi.nOutSlot = int( pSlot - H.out );
								fi.nPrevSlot = nPrevSlot;
								fi.nCurrSlot = nCurrSlot;
								capture::RecordFrame( pCb, fi );
							}
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
				if ( !bOk && capture::Running() )
				{
					// ritz_fg_capture watch: a refused estimate / synth is a telemetry record (no image: the real frame is shown).
					capture::FrameIn fi;
					fi.ulOutId = req.outId;
					fi.ulPrevId = req.prevId;
					fi.ulCurrId = req.currId;
					fi.ulNewestId = req.newestId;
					fi.flT0 = req.t0;
					fi.flT1 = req.t1;
					fi.nEstimateRet = nEstimateRet;
					fi.nSynthRet = bSynthRefused ? 0 : -1;
					fi.nPrevSlot = nPrevSlot;
					fi.nCurrSlot = nCurrSlot;
					capture::RecordFrame( nullptr, fi );
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

		// The stale guard: layer 0 is the refused stale buffer too, so the real frame shown is the newest GOOD one.
		if ( !pResult && NewestIsStale( req.newestId ) && H.bHaveCur && H.pRing[ H.nCurSlot ] )
		{
			pResult = H.pRing[ H.nCurSlot ];
			g_ulHeldPaints.fetch_add( 1, std::memory_order_relaxed );
		}
		// Debug options, "Only generated frames" (FrameGenDebug.h; superdoc/features/frame-generation-debug.md). Done last, on the
		// finished decision, so the ring copy, observe and pairs above ran exactly as without it.
		// `Why here and not in the pacer:` the real tick's paint is what feeds the ring and the UI-protection history; only the
		// image shown is replaced.
		if ( g_uDebug.load( std::memory_order_relaxed ) & 1u )
		{
			const bool bRealTick = IsRealTick( req.prevId, req.t1 );
			const uint64_t ulNowNs = get_time_in_nanos();
			( bRealTick ? H.ulDebugReal : H.ulDebugGen )++;
			if ( !bRealTick )
			{
				// Remember which pool slot the generated image is in, and keep it the most recently used so the next synth
				// (LRU) does not overwrite an image a composite still reads while it is held.
				for ( OutSlot_t &o : H.out )
				{
					if ( o.bValid && pResult && o.pTex == pResult )
					{
						o.ulUse = ++H.ulUseClock;
						H.nLastGenSlot = int( &o - H.out );
						H.ulLastGenOutId = o.ulOutId;
						H.ulLastGenNs = ulNowNs;
					}
				}
			}
			else
			{
				OutSlot_t *pHold = ( H.nLastGenSlot >= 0 && H.out[ H.nLastGenSlot ].bValid && H.out[ H.nLastGenSlot ].ulOutId == H.ulLastGenOutId
					&& H.out[ H.nLastGenSlot ].pTex ) ? &H.out[ H.nLastGenSlot ] : nullptr;
				// "Generating" = the interpolator is live AND the pacer's own steady flag says it is generating (a fresh status; it
				// is republished about every 250 ms, an older one counts as not generating). A pass-through or a scene cut
				// therefore ends the hold; the 100 ms cap bounds the lag of that flag.
				bool bGenerating = false;
				if ( H.bLive && pHold )
				{
					const PacingStatus ps = GetPacingStatus();
					bGenerating = ps.valid && ps.generating && ulNowNs - ps.publishedNs <= 500ull * 1000ull * 1000ull;
				}
				if ( ShouldHoldGenerated( true, true, bGenerating, pHold != nullptr, pHold ? ulNowNs - H.ulLastGenNs : 0 ) )
				{
					pHold->ulUse = ++H.ulUseClock;   // stays the most recently used
					pResult = pHold->pTex;
					if ( ++H.ulDebugHeld == 1 || H.ulDebugHeld % 100 == 0 )
						fg_log.debugf( "debug: only generated frames: %llu real ticks shown as the last generated image (of %llu real, %llu generated ticks)",
							(unsigned long long)H.ulDebugHeld, (unsigned long long)H.ulDebugReal, (unsigned long long)H.ulDebugGen );
				}
			}
		}

		// The stale guard's diagnostic second look: only in a command buffer this paint records anyway (the one
		// that copied the frame), as its last dispatch.
		const bool bDiagRecorded = StaleDiagRecord( H, pCmd.get(), pLayer0, req.newestId );

		if ( pCmd )
		{
			vulkan_paint_split_end( pCmd.get() );
			const uint64_t ulSeq = g_device.submit( std::move( pCmd ) );
			if ( bDiagRecorded )
				StaleDiagSubmitted( ulSeq );
			if ( bProfileWritten )
				H.ulProfileSeq = ulSeq;
			if ( bBoxPreviewRecorded )
				BoxPreviewSubmitted( ulSeq );
			capture::Submitted( ulSeq );   // a no-op unless this composite recorded capture copies
		}

		// Cheap poll for the finished pair's timings (a counter read, never a wait).
		if ( H.bProfiling )
			HarvestProfile( H, false, bPairPlaying );

		return pResult;
	}
}
