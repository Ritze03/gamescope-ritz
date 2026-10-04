// FrameGenHost.cpp -- see FrameGenHost.h for the model and the API contract.
//
// Layout of this file:
//   1. cross-thread state   (config, gate, frame request, reset flag, status)
//   2. render-thread state  (Host: the Interpolator, the ring, the outputs)
//   3. RecordBaseLayer()    (the one per-composite entry point)

#include "FrameGenHost.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "../../subprojects/FrameGen/gpu/framegen.h"

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
		// safety, 8-9 hud protection, 10-11 mode, 12 priority, 13-22 target fps, 23 pause at refresh.
		constexpr int kMinTargetFps = 30;
		constexpr int kMaxTargetFps = 1000;

		uint32_t Pack( const Config &c )
		{
			int nMult = c.multiplier < 2 ? 0 : std::min( c.multiplier, kMaxMultiplier );
			Mode eMode = c.mode;
			// A fixed multiplier below 2x is not a mode: it is Off.
			if ( eMode == Mode::Fixed && nMult < 2 )
				eMode = Mode::Off;
			const int nTarget = c.targetFps <= 0 ? 0 : std::min( std::max( c.targetFps, kMinTargetFps ), kMaxTargetFps );
			return uint32_t( nMult )
				| ( uint32_t( c.quality ) << 4 )
				| ( uint32_t( c.safety ) << 6 )
				| ( uint32_t( c.hud ) << 8 )
				| ( uint32_t( eMode ) << 10 )
				| ( uint32_t( c.priority ) << 12 )
				| ( uint32_t( nTarget ) << 13 )
				| ( uint32_t( c.pauseAtRefresh ? 1 : 0 ) << 23 );
		}

		Config Unpack( uint32_t u )
		{
			Config c;
			c.multiplier = int( u & 0xFu );
			c.quality = Quality( ( u >> 4 ) & 0x3u );
			c.safety = Safety( ( u >> 6 ) & 0x3u );
			c.hud = HudProtect( ( u >> 8 ) & 0x3u );
			c.mode = Mode( ( u >> 10 ) & 0x3u );
			c.priority = Priority( ( u >> 12 ) & 0x1u );
			c.targetFps = int( ( u >> 13 ) & 0x3FFu );
			c.pauseAtRefresh = ( ( u >> 23 ) & 1u ) != 0;
			return c;
		}

		std::atomic<uint32_t> g_uConfig{ Pack( Config{} ) };

		// The gate word. bit 0: enabled (mode != Off). bit 1: the render thread
		// holds resources. RenderWanted() is `!= 0`, so with FG off and everything
		// released the per-frame cost is this one load.
		constexpr uint32_t kGateEnabled = 1u << 0;
		constexpr uint32_t kGateLive = 1u << 1;
		std::atomic<uint32_t> g_uGate{ 0 };

		std::mutex g_FrameMutex;
		FrameRequest g_Frame;   // t < 0 (inert) until pacing drives the renderer

		std::atomic<bool> g_bResetRequested{ false };

		std::atomic<uint8_t> g_eReason{ uint8_t( Unavailable::Ok ) };
		std::atomic<float> g_flLastPairGpuMs{ -1.0f };
		std::atomic<float> g_flLastEstimateMs{ -1.0f };
		std::atomic<float> g_flLastSynthMs{ -1.0f };
		std::atomic<uint32_t> g_uCostSeq{ 0 };

		std::mutex g_PacingMutex;
		PacingStatus g_Pacing;

		// -------------------------------------------------------------
		//  2. Render-thread state
		// -------------------------------------------------------------

		// The ring and every output are ALWAYS DRM_FORMAT_ARGB8888
		// (VK_FORMAT_B8G8R8A8_UNORM), whatever the game's own 8-bit format is:
		// cs_fg_copy.comp samples the logical rgba and writes it, so channel
		// order never matters and the library sees one fixed viewFormat.
		constexpr uint32_t kHostDrmFormat = DRM_FORMAT_ARGB8888;
		constexpr VkFormat kHostVkFormat = VK_FORMAT_B8G8R8A8_UNORM;

		// The ring holds the last three real frames: Low latency only ever uses the
		// newest pair (two of them), Smoothness may still be inside the pair before
		// it while the newest has already arrived (see Pacing.h).
		constexpr int kRingSize = 3;

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

		// Crosshair protection tunables. The ROI is a centred square of 3% of the
		// frame's area (~332 px at 2560x1440): the user's "the middle one to maybe
		// three percent of the screen", which fits a crosshair two to three times
		// the usual size. A pixel is "still" when no channel moved more than
		// 3.5/255 (a hair above 3 levels, so rounding never decides) and is masked
		// after 8 consecutive still real frames: long enough that a one-frame
		// stall of the picture does not mask a whole scene, short enough that a
		// crosshair is protected within ~0.1 s of play.
		constexpr double kXhairAreaFrac = 0.03;
		constexpr float kXhairThreshold = 3.5f / 255.0f;
		constexpr uint32_t kXhairStableFrames = 8;

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

			// D11: the private ring. Real frames fill the slots in turn; the newest
			// is `nCurSlot`. Each slot remembers the commit id it holds (0 = empty),
			// which is how a pair is found: pacing names its frames by id. Nothing
			// here is ever a game buffer.
			gamescope::Rc<CVulkanTexture> pRing[ kRingSize ];
			uint64_t ulSlotId[ kRingSize ] = {};
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
			uint32_t uAppliedConfig = 0;
			// A failed init/reconfigure is not retried until the size or the
			// structural setting changes (or FG is switched off and on): init
			// builds pipelines, so retrying per frame would be a stutter machine.
			uint64_t ulFailedKey = 0;

			// D16: one open profile = one real pair (estimate + its synths).
			bool bProfiling = false;
			bool bProfileOpen = false;
			uint64_t ulProfileSeq = 0;   // submission sequence of the last cb that wrote into it
			int nProfileSynths = 0;

			// Crosshair protection (superdoc/features/frame-generation.md). All of
			// it is ROI-sized (kXhairAreaFrac of the frame, centred), created on
			// first use while Static HUD protection is on, and released with the
			// ring (teardown / size change, both after a waitIdle). The patch
			// textures are per RING SLOT (the original ROI pixels + mask of the real
			// frame in that slot), so a synth of an older pair still gets ITS
			// frame's crosshair back.
			gamescope::Rc<CVulkanTexture> pXhCnt[2];             // ping-pong stability counters (.r = count/255)
			gamescope::Rc<CVulkanTexture> pXhPatch[ kRingSize ]; // .rgb = the real frame's ORIGINAL ROI pixels, .a = mask
			bool bXhPatchValid[ kRingSize ] = {};                // that slot's patch holds a usable mask
			int nXhCnt = 0;                // the counter texture written for the newest real frame
			bool bXhHist = false;          // counters + the newest slot's patch describe the previous real frame (no gap)
			bool bXhFailed = false;        // textures could not be created: stay off until size change / teardown
			uint32_t uXhX = 0, uXhY = 0, uXhSize = 0;   // the ROI in frame pixels
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

		framegen::Settings ToLibrarySettings( const Config &c )
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
			// The crosshair counters compare consecutive real frames, so any gap
			// restarts them (the next frame copied in starts every counter at 0).
			H.bXhHist = false;
			for ( bool &b : H.bXhPatchValid )
				b = false;
		}

		// The ring slot holding commit `ulId`, or -1.
		int FindSlot( const Host_t &H, uint64_t ulId )
		{
			if ( !ulId )
				return -1;
			for ( int i = 0; i < kRingSize; i++ )
			{
				if ( H.ulSlotId[ i ] == ulId )
					return i;
			}
			return -1;
		}

		// The crosshair textures. Callers have already waited for the device to
		// go idle (Teardown, the size-change path): same rule as the ring.
		void ReleaseCrosshair( Host_t &H )
		{
			for ( auto &p : H.pXhCnt )
				p = nullptr;
			for ( auto &p : H.pXhPatch )
				p = nullptr;
			H.bXhHist = false;
			for ( bool &b : H.bXhPatchValid )
				b = false;
			H.bXhFailed = false;
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
			for ( OutSlot_t &o : H.out )
				o.pTex = nullptr;
			ReleaseCrosshair( H );
			H.bLive = false;
			H.uWidth = H.uHeight = 0;
			DropFrames( H );
			H.bProfileOpen = false;
			H.uAppliedConfig = 0;
			g_uGate.fetch_and( ~kGateLive, std::memory_order_relaxed );
			g_flLastPairGpuMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastEstimateMs.store( -1.0f, std::memory_order_relaxed );
			g_flLastSynthMs.store( -1.0f, std::memory_order_relaxed );
		}

		bool CreateTexture( gamescope::Rc<CVulkanTexture> &out, uint32_t w, uint32_t h )
		{
			CVulkanTexture::createFlags flags;
			flags.bSampled = true;   // the library samples prev/curr; the composite samples outputs
			flags.bStorage = true;   // the copy shader / the library writes them

			gamescope::Rc<CVulkanTexture> pTex = new CVulkanTexture();
			if ( !pTex->BInit( w, h, 1u, kHostDrmFormat, flags, nullptr ) )
				return false;
			out = std::move( pTex );
			return true;
		}

		bool CreateRing( Host_t &H )
		{
			for ( auto &p : H.pRing )
			{
				if ( !CreateTexture( p, H.uWidth, H.uHeight ) )
					return false;
			}
			return true;
		}

		// The ROI textures for the current frame size; false = feature off for now.
		bool EnsureCrosshair( Host_t &H )
		{
			if ( H.pXhPatch[0] )
				return true;
			if ( H.bXhFailed )
				return false;

			uint32_t uSide = uint32_t( std::lround( std::sqrt( kXhairAreaFrac * double( H.uWidth ) * double( H.uHeight ) ) ) );
			uSide = std::min( std::max( uSide, 32u ), std::min( H.uWidth, H.uHeight ) );
			H.uXhSize = uSide;
			H.uXhX = ( H.uWidth - uSide ) / 2;
			H.uXhY = ( H.uHeight - uSide ) / 2;

			bool bOk = CreateTexture( H.pXhCnt[0], uSide, uSide ) && CreateTexture( H.pXhCnt[1], uSide, uSide );
			for ( int i = 0; bOk && i < kRingSize; i++ )
				bOk = CreateTexture( H.pXhPatch[ i ], uSide, uSide );
			if ( !bOk )
			{
				// Not worth failing frame generation over: just run without it.
				fg_log.errorf( "crosshair protection unavailable: could not create its %ux%u textures", uSide, uSide );
				ReleaseCrosshair( H );
				H.bXhFailed = true;
				return false;
			}
			H.bXhHist = false;
			return true;
		}

		// == cs_fg_crosshair_{detect,inpaint,patch}.comp's xhair_t block.
		struct XhairPush_t
		{
			int32_t ox, oy, w, h;
			float flThr;
			uint32_t uK;
			uint32_t uReset;
			uint32_t uMode;
		};

		void XhairBind( CVulkanCmdBuffer *pCb, uint32_t uSlot, const gamescope::Rc<CVulkanTexture> &pTex )
		{
			pCb->bindTexture( uSlot, pTex );
			pCb->setTextureSrgb( uSlot, true );   // raw UNORM view, like every layer-0 pass
			pCb->setSamplerUnnormalized( uSlot, true );
			pCb->setSamplerNearest( uSlot, true );
		}

		void XhairDispatch( CVulkanCmdBuffer *pCb, const Host_t &H, ShaderType eShader, uint32_t uMode, bool bReset )
		{
			const XhairPush_t push = { int32_t( H.uXhX ), int32_t( H.uXhY ), int32_t( H.uXhSize ), int32_t( H.uXhSize ),
				kXhairThreshold, kXhairStableFrames, bReset ? 1u : 0u, uMode };
			pCb->bindPipeline( g_device.pipeline( eShader ) );
			pCb->uploadConstantsRaw( &push, sizeof( push ) );
			pCb->dispatch( div_roundup( H.uXhSize, 8 ), div_roundup( H.uXhSize, 8 ) );
		}

		// Crosshair protection, removal half. Runs once per NEW real frame, right
		// after it was copied into ring slot nNew (same command buffer):
		//   1. stability: compare the new frame's ROI with the previous frame's
		//      ORIGINAL ROI (the previous slot's patch texture) and update the
		//      per-pixel counters;
		//   2. mask + patch: save the new frame's original ROI pixels and the
		//      dilated mask of long-still pixels into nNew's patch texture;
		//   3. inpaint: overwrite the masked pixels of the ring slot from their
		//      surroundings, so estimate + warp see plain background there.
		// Why on every real frame and not only when a pair is generated: the
		// counters must see consecutive real frames; pass-through pairs would
		// otherwise leave gaps. The previous frame's ring slot was already
		// inpainted in ITS turn and is only ever used as `prev`, so nothing is
		// redone for it. Every pass is ROI-sized (a few % of the frame).
		// nPrev = the slot of the previous real frame (-1 on a reset).
		void RecordCrosshairDetect( CVulkanCmdBuffer *pCb, Host_t &H, int nNew, int nPrev )
		{
			const bool bReset = !H.bXhHist || nPrev < 0;
			const int nIn = H.nXhCnt;
			const int nOut = 1 - H.nXhCnt;

			// 1. stability. On a reset frame the old counters/patch are neither bound
			// nor read: they may never have been written (UNDEFINED layout), and
			// binding them first would stop the target passes below from discarding
			// them the way a first-sight destination must.
			pCb->clearState();
			XhairBind( pCb, 0, H.pRing[ nNew ] );
			if ( !bReset )
			{
				XhairBind( pCb, 1, H.pXhPatch[ nPrev ] );
				XhairBind( pCb, 2, H.pXhCnt[ nIn ] );
			}
			pCb->bindTarget( H.pXhCnt[ nOut ] );
			XhairDispatch( pCb, H, SHADER_TYPE_FG_XHAIR_DETECT, 0, bReset );

			// 2. mask + patch (writes this frame's own patch texture; the previous
			// frame's, read by pass 1, is a different one).
			pCb->clearState();
			XhairBind( pCb, 0, H.pRing[ nNew ] );
			XhairBind( pCb, 1, H.pXhCnt[ nOut ] );
			pCb->bindTarget( H.pXhPatch[ nNew ] );
			XhairDispatch( pCb, H, SHADER_TYPE_FG_XHAIR_DETECT, 1, bReset );

			// 3. inpaint. Nothing can be masked on a reset frame (every counter is 0).
			if ( !bReset )
			{
				pCb->clearState();
				XhairBind( pCb, 0, H.pXhPatch[ nNew ] );
				pCb->bindTarget( H.pRing[ nNew ] );
				XhairDispatch( pCb, H, SHADER_TYPE_FG_INPAINT, 0, false );
			}
			pCb->clearState();

			H.nXhCnt = nOut;
			H.bXhHist = true;
			H.bXhPatchValid[ nNew ] = !bReset;
		}

		// Crosshair protection, restore half: out = mix(out, original, mask) over
		// the ROI, with the patch of the real frame the synth leads UP TO (its
		// curr). Records after recordSynth() into the same output texture.
		void RecordCrosshairPatch( CVulkanCmdBuffer *pCb, const Host_t &H, int nCurrSlot, const gamescope::Rc<CVulkanTexture> &pOut )
		{
			pCb->clearState();
			XhairBind( pCb, 0, H.pXhPatch[ nCurrSlot ] );
			pCb->bindTarget( pOut );
			XhairDispatch( pCb, H, SHADER_TYPE_FG_XHAIR_PATCH, 0, false );
			pCb->clearState();
		}

		uint64_t FailKey( uint32_t w, uint32_t h, const Config &c )
		{
			return ( uint64_t( w ) << 32 ) ^ ( uint64_t( h ) << 8 ) ^ uint64_t( c.quality == Quality::Performance ? 4 : 2 ) ^ 0x8000000000000000ull;
		}

		// Brings the library to (w, h, cfg): init the first time, resize on a size
		// change, apply a changed preset. False = not usable this frame.
		bool EnsureReady( Host_t &H, uint32_t w, uint32_t h, const Config &c, uint32_t uPackedConfig )
		{
			const uint64_t ulKey = FailKey( w, h, c );
			if ( H.ulFailedKey == ulKey )
				return false;

			const framegen::Settings want = ToLibrarySettings( c );

			// Only the library's own presets matter to what is already applied; mode,
			// priority and target are pacing's business and never reconfigure it.
			const uint32_t uLibraryConfig = uPackedConfig & 0x3F0u;

			if ( !H.bLive )
			{
				framegen::InitInfo info;
				info.phys = g_device.physDev();
				info.device = g_device.device();
				info.gdpa = g_device.vk.GetDeviceProcAddr;
				info.width = w;
				info.height = h;
				info.viewFormat = kHostVkFormat;
				info.getMemProps = g_device.vk.GetPhysicalDeviceMemoryProperties;
				info.pipelineCache = VK_NULL_HANDLE;
				info.settings = want;

				H.uWidth = w;
				H.uHeight = h;
				if ( !H.interp.init( info ) || !CreateRing( H ) )
				{
					fg_log.errorf( "frame generation unavailable: could not create the interpolator or its frame ring at %ux%u", w, h );
					Teardown( H );
					H.ulFailedKey = ulKey;
					return false;
				}

				H.bLive = true;
				g_uGate.fetch_or( kGateLive, std::memory_order_relaxed );
				H.uAppliedConfig = uLibraryConfig;
				H.bProfiling = g_device.supportsTimestamps() && H.interp.setProfiling( true );
				fg_log.infof( "frame generation ready: %ux%u, flowScale %u, GPU timing %s",
					w, h, want.flowScale, H.bProfiling ? "on" : "n/a" );
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
				for ( OutSlot_t &o : H.out )
					o.pTex = nullptr;
				ReleaseCrosshair( H );
				if ( !H.interp.resize( w, h, kHostVkFormat ) || !CreateRing( H ) )
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
					// in flight changes, so no wait.
					H.interp.setSettings( want );
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
		// once the pair is over: when the next estimate is about to reset the query
		// pool, or when this composite is not still playing the profile's pair.
		// (An unretired profile is dropped at the next estimate rather than kept:
		// it would be read half-overwritten.)
		//
		// Published: the estimate's time, the mean time of one synth, and their sum
		// for the synths profiled -- the cost guard budgets from the first two.
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

			// The profile is a chain of timestamps, one per pass group, each delta
			// being "time since the previous stamp". The first stamp of every synth
			// recorded in a LATER composite than the estimate would include the idle
			// time between the two composites (up to a frame), so that one delta is
			// dropped -- an under-count of a lookup/mismatch pass (~0.01-0.04 ms)
			// instead of an over-count of ~16 ms. The first synth is recorded in the
			// estimate's own command buffer, contiguous with it, and keeps all of its.
			uint64_t ulEstTicks = 0;
			uint64_t ulSynthTicks = 0;
			int nSynth = 0;
			const char *pszPrev = "";
			for ( size_t i = 0; i < names.size(); i++ )
			{
				const bool bSynthStart = !strcmp( names[i], "lookup" )
					|| ( !strcmp( names[i], "mismatch" ) && strcmp( pszPrev, "lookup" ) != 0 );
				if ( bSynthStart )
					nSynth++;
				pszPrev = names[i];

				if ( bSynthStart && nSynth >= 2 )
					continue;
				if ( nSynth == 0 )
					ulEstTicks += ticks[i];
				else
					ulSynthTicks += ticks[i];
			}

			const double flToMs = double( g_device.timestampPeriodNs() ) * 1e-6;
			const double flEstMs = double( ulEstTicks ) * flToMs;
			const double flSynthMs = nSynth > 0 ? double( ulSynthTicks ) * flToMs / double( nSynth ) : -1.0;
			const double flPairMs = flEstMs + double( ulSynthTicks ) * flToMs;
			// A wrapped counter (timestampValidBits < 64) would show up as an absurd
			// value; keep the previous reading instead.
			if ( flPairMs < 0.0 || flPairMs > 1000.0 || flEstMs < 0.0 )
				return;
			g_flLastPairGpuMs.store( float( flPairMs ), std::memory_order_relaxed );
			g_flLastEstimateMs.store( float( flEstMs ), std::memory_order_relaxed );
			if ( flSynthMs >= 0.0 )
				g_flLastSynthMs.store( float( flSynthMs ), std::memory_order_relaxed );
			g_uCostSeq.fetch_add( 1, std::memory_order_relaxed );
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

		if ( Mode( ( uPacked >> 10 ) & 0x3u ) != Mode::Off )
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

	Config GetConfig()
	{
		return Unpack( g_uConfig.load( std::memory_order_relaxed ) );
	}

	bool Enabled()
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
			case Unavailable::Hdr:         return "Not available with HDR";
			case Unavailable::YCbCr:       return "Not available for video (YCbCr) surfaces";
			case Unavailable::NotEightBit: return "Needs an 8-bit game format (10-bit is not supported)";
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
		}
		s.costSeq = g_uCostSeq.load( std::memory_order_relaxed );
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
		const Config cfg = Unpack( uPackedConfig );

		// Switched off (or never on): release everything. The first branch is the
		// whole cost of the off path once resources are gone, and
		// vulkan_composite() does not even reach it then (RenderWanted() == 0).
		if ( cfg.mode == Mode::Off )
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

		FrameRequest req;
		{
			std::lock_guard<std::mutex> lock( g_FrameMutex );
			req = g_Frame;
		}

		// Pacing has not driven the renderer, or says it is settled pass-through:
		// stay completely inert. Enabled() is true so the backends already
		// composite, but nothing is copied or run. The frames are forgotten, so the
		// first frame after the inert stretch starts a clean sequence (no ancient
		// "previous" frame, crosshair counters restarted).
		if ( req.t < 0.0f || req.newestId == 0 )
		{
			if ( g_pHost && ( g_uGate.load( std::memory_order_relaxed ) & kGateLive ) )
				DropFrames( *g_pHost );
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
		Unavailable eWhy = Unavailable::Ok;
		if ( g_bOutputHDREnabled || ColorspaceIsHDR( eColorspace ) || eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU )
			eWhy = Unavailable::Hdr;
		else if ( pLayer0->isYcbcr() )
			eWhy = Unavailable::YCbCr;
		else if ( pLayer0->format() != VK_FORMAT_B8G8R8A8_UNORM && pLayer0->format() != VK_FORMAT_R8G8B8A8_UNORM )
			eWhy = Unavailable::NotEightBit;
		else if ( uWidth < framegen::Interpolator::kMinSize || uHeight < framegen::Interpolator::kMinSize )
			eWhy = Unavailable::TooSmall;

		if ( eWhy != Unavailable::Ok )
		{
			SetReason( eWhy );
			DropFrames( H );
			return nullptr;
		}

		if ( !EnsureReady( H, uWidth, uHeight, cfg, uPackedConfig ) )
		{
			SetReason( Unavailable::InitFailed );
			return nullptr;
		}
		SetReason( Unavailable::Ok );

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

		const bool bXhair = cfg.hud != HudProtect::Off && EnsureCrosshair( H );

		// D11: a newestId not seen before is a new real frame. Copy it into the next
		// ring slot (in this very command buffer) whatever this composite shows, so
		// later pairs always have it.
		if ( !H.bHaveCur || req.newestId != H.ulCurId )
		{
			const int nPrevSlot = H.bHaveCur ? H.nCurSlot : -1;
			const int nNewSlot = H.bHaveCur ? ( H.nCurSlot + 1 ) % kRingSize : 0;
			H.nCurSlot = nNewSlot;
			H.bHaveCur = true;
			H.ulCurId = req.newestId;
			H.ulSlotId[ nNewSlot ] = req.newestId;
			H.bXhPatchValid[ nNewSlot ] = false;
			// The estimate and the cached outputs belong to pairs of frames; the
			// estimate's frames may now be gone from the ring (overwritten), the
			// outputs stay valid (they are finished images keyed by outId).
			if ( H.bEstimateValid && ( FindSlot( H, H.ulEstPrev ) < 0 || FindSlot( H, H.ulEstCurr ) < 0 ) )
				H.bEstimateValid = false;

			CVulkanCmdBuffer *pCb = Cmd();
			pCb->bindPipeline( g_device.pipeline( SHADER_TYPE_FG_COPY ) );
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

			// Static HUD protection also keeps a still crosshair out of the
			// interpolation: remove it from the ring frame now, patch it back onto
			// the generated frames later. Off = none of this is recorded.
			if ( bXhair )
			{
				RecordCrosshairDetect( pCb, H, nNewSlot, nPrevSlot );
				pCb->insertBarrier();   // flush the ring slot / patch for later command buffers
			}
			else
			{
				H.bXhHist = false;
				H.bXhPatchValid[ nNewSlot ] = false;
			}
		}

		// ---- the generated frame, if this output is one ----
		bool bPairPlaying = false;   // this composite shows a synth of the profile's pair
		if ( req.t > 0.0f && req.t < 1.0f && req.prevId && req.currId && req.prevId != req.currId )
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

				if ( !H.bEstimateValid || H.ulEstPrev != req.prevId || H.ulEstCurr != req.currId )
				{
					// D16: a new estimate resets the library's query pool; read the previous
					// pair's timings first, if they have retired.
					if ( H.bProfiling )
					{
						HarvestProfile( H, true, false );
						H.nProfileSynths = 0;
					}

					CVulkanCmdBuffer *pCb = Cmd();
					pCb->prepareSrcImage( pPrev );
					pCb->prepareSrcImage( pCurr );
					pCb->insertBarrier();
					bOk = H.interp.recordEstimate( pCb->rawBuffer(), pPrev->srgbView(), pCurr->srgbView() );
					if ( bOk )
					{
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
						bOk = CreateTexture( pSlot->pTex, uWidth, uHeight );

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
						bOk = H.interp.recordSynth( pCb->rawBuffer(), pPrev->srgbView(), pCurr->srgbView(), pOut->srgbView(), req.t );
						if ( bOk )
						{
							pCb->markDirty( pOut );
							// Put the crosshair back (only if THIS pair's curr frame was inpainted).
							if ( bXhair && H.bXhPatchValid[ nCurrSlot ] )
								RecordCrosshairPatch( pCb, H, nCurrSlot, pSlot->pTex );
							pCb->insertBarrier();   // flush for the consumers in later command buffers
							pSlot->bValid = true;
							pSlot->ulOutId = req.outId;
							pSlot->ulUse = ++H.ulUseClock;
							H.nPairSynths++;
							if ( H.bProfileOpen )
							{
								H.nProfileSynths++;
								bProfileWritten = true;
							}
							pResult = pSlot->pTex;
						}
					}
				}

				if ( !bOk )
				{
					// Nothing was recorded for the failing call, so `pResult` stays null:
					// the real frame is shown for this output (library contract: never
					// present an `out` whose record returned false).
					SetReason( Unavailable::RecordFailed );
				}
			}
			// else: a frame of the pair is gone from the ring -- the real frame is shown.
		}

		if ( pCmd )
		{
			const uint64_t ulSeq = g_device.submit( std::move( pCmd ) );
			if ( bProfileWritten )
				H.ulProfileSeq = ulSeq;
		}

		// Cheap poll for the finished pair's timings (a counter read, never a wait).
		if ( H.bProfiling )
			HarvestProfile( H, false, bPairPlaying );

		return pResult;
	}
}
