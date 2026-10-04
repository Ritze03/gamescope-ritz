// FrameGenHost.cpp -- see FrameGenHost.h for the model and the API contract.
//
// Layout of this file:
//   1. cross-thread state   (config, gate, slot, reset flag, status)
//   2. render-thread state  (Host: the Interpolator, the ring, the outputs)
//   3. RecordBaseLayer()    (the one per-composite entry point)

#include "FrameGenHost.h"

#include <algorithm>
#include <atomic>
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
		// safety, 8-9 hud protection.
		constexpr uint32_t kMaxMultiplier = 4;

		uint32_t Pack( const Config &c )
		{
			int nMult = c.multiplier < 2 ? 0 : std::min( c.multiplier, (int)kMaxMultiplier );
			return uint32_t( nMult )
				| ( uint32_t( c.quality ) << 4 )
				| ( uint32_t( c.safety ) << 6 )
				| ( uint32_t( c.hud ) << 8 );
		}

		Config Unpack( uint32_t u )
		{
			Config c;
			c.multiplier = int( u & 0xFu );
			c.quality = Quality( ( u >> 4 ) & 0x3u );
			c.safety = Safety( ( u >> 6 ) & 0x3u );
			c.hud = HudProtect( ( u >> 8 ) & 0x3u );
			return c;
		}

		std::atomic<uint32_t> g_uConfig{ Pack( Config{} ) };

		// The gate word. bit 0: enabled (multiplier >= 2). bit 1: the render
		// thread holds resources. RenderWanted() is `!= 0`, so with FG off and
		// everything released the per-frame cost is this one load.
		constexpr uint32_t kGateEnabled = 1u << 0;
		constexpr uint32_t kGateLive = 1u << 1;
		std::atomic<uint32_t> g_uGate{ 0 };

		struct Slot_t
		{
			uint64_t ulPairId = 0;
			int nK = 0;
			int nN = 0;   // < 2: pacing has not driven the renderer (yet)
		};
		std::mutex g_SlotMutex;
		Slot_t g_Slot;

		std::atomic<bool> g_bResetRequested{ false };

		std::atomic<uint8_t> g_eReason{ uint8_t( Unavailable::Ok ) };
		std::atomic<float> g_flLastPairGpuMs{ -1.0f };

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

		// Generated frames per pair are N-1 <= 3.
		constexpr int kMaxOutputs = int( kMaxMultiplier ) - 1;

		struct Host_t
		{
			framegen::Interpolator interp;
			bool bLive = false;
			uint32_t uWidth = 0, uHeight = 0;

			// D11: the private ring. Real frames alternate between the slots; the
			// newest is `nCurSlot`, the one before it the other. Nothing here is
			// ever a game buffer.
			gamescope::Rc<CVulkanTexture> pRing[2];
			int nCurSlot = 0;
			bool bHaveCur = false;   // pRing[nCurSlot] holds a valid real frame
			bool bHavePrev = false;  // ...and pRing[1 - nCurSlot] holds the one before it
			uint64_t ulCurPair = 0;  // the pairId of pRing[nCurSlot]

			// D13: whether the library holds a motion estimate for the current
			// (prev, curr), and the cache of generated frames for it.
			bool bEstimateValid = false;
			gamescope::Rc<CVulkanTexture> pOut[ kMaxOutputs ];
			bool bOutValid[ kMaxOutputs ] = {};
			int nOutN[ kMaxOutputs ] = {};   // the n the cached frame was made for (t = k/n)

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
			int nProfileSynthsWanted = 0;
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

			switch ( c.safety )
			{
				case Safety::Low:     s.trustLow = 16.0f; s.trustHigh = 56.0f; break;
				case Safety::Default: s.trustLow = 12.0f; s.trustHigh = 40.0f; break;
				case Safety::High:    s.trustLow = 8.0f;  s.trustHigh = 28.0f; break;
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

		// Forget the previous frame and every cached output. Frees nothing, so no
		// wait is needed.
		void DropFrames( Host_t &H )
		{
			H.bHaveCur = false;
			H.bHavePrev = false;
			H.ulCurPair = 0;
			H.bEstimateValid = false;
			for ( bool &b : H.bOutValid )
				b = false;
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
			for ( auto &p : H.pOut )
				p = nullptr;
			H.bLive = false;
			H.uWidth = H.uHeight = 0;
			DropFrames( H );
			H.bProfileOpen = false;
			H.uAppliedConfig = 0;
			g_uGate.fetch_and( ~kGateLive, std::memory_order_relaxed );
			g_flLastPairGpuMs.store( -1.0f, std::memory_order_relaxed );
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
				H.uAppliedConfig = uPackedConfig;
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
				for ( auto &p : H.pOut )
					p = nullptr;
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

			if ( uPackedConfig != H.uAppliedConfig )
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
					for ( bool &b : H.bOutValid )
						b = false;
					H.bProfileOpen = false;
				}
				else
				{
					// Runtime change: every record after this uses the new values; nothing
					// in flight changes, so no wait.
					H.interp.setSettings( want );
				}
				H.uAppliedConfig = uPackedConfig;
			}
			return true;
		}

		// D16. Reads the open profile if (and only if) the submissions that wrote
		// it have retired -- the timeline semaphore's counter says so without
		// waiting. Never calls readTimings() on an unretired profile: that call
		// waits (VK_QUERY_RESULT_WAIT_BIT) and would stall the compositor.
		//
		// bAboutToRecordEstimate: the next recordEstimate() resets the query
		// pool, so an unretired profile is dropped rather than kept (it would be
		// read half-overwritten).
		void HarvestProfile( Host_t &H, bool bAboutToRecordEstimate )
		{
			if ( !H.bProfileOpen )
				return;

			const bool bComplete = H.nProfileSynths >= H.nProfileSynthsWanted;
			if ( !bComplete && !bAboutToRecordEstimate )
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
			uint64_t ulTicks = 0;
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
				ulTicks += ticks[i];
			}

			const double flMs = double( ulTicks ) * double( g_device.timestampPeriodNs() ) * 1e-6;
			// A wrapped counter (timestampValidBits < 64) would show up as an absurd
			// value; keep the previous reading instead.
			if ( flMs < 0.0 || flMs > 1000.0 )
				return;
			g_flLastPairGpuMs.store( float( flMs ), std::memory_order_relaxed );
		}
	}

	// ------------------------------------------------------------------
	//  Public API: config, gate, slot, reset, status
	// ------------------------------------------------------------------

	void SetConfig( const Config &cfg )
	{
		const uint32_t uPacked = Pack( cfg );
		g_uConfig.store( uPacked, std::memory_order_relaxed );

		if ( ( uPacked & 0xFu ) >= 2 )
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

	void SetSlot( uint64_t pairId, int k, int n )
	{
		std::lock_guard<std::mutex> lock( g_SlotMutex );
		g_Slot = Slot_t{ pairId, k, n };
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
			case PassReason::RefreshLimit:        return "Display refresh too low for this multiplier";
			case PassReason::CostGuard:           return "Stepped down to keep up with the GPU";
			case PassReason::GameTooFast:         return "Game already fills the display";
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
		s.lastPairGpuMs = s.timestampsSupported ? g_flLastPairGpuMs.load( std::memory_order_relaxed ) : -1.0f;
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
		if ( cfg.multiplier < 2 )
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

		Slot_t slot;
		{
			std::lock_guard<std::mutex> lock( g_SlotMutex );
			slot = g_Slot;
		}

		// Pacing has not driven the renderer: stay completely inert. Enabled() is
		// true so the backends already composite, but nothing is copied or run.
		if ( slot.nN < 2 || slot.nN > int( kMaxMultiplier ) )
			return nullptr;

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
		// (created lazily: a pass-through composite of an already-seen pair records nothing)
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

		// D11: a pairId not seen before is a new real frame. Copy it into the other
		// ring slot (in this very command buffer) whatever this composite shows, so
		// the next pair always has a prev.
		if ( !H.bHaveCur || slot.ulPairId != H.ulCurPair )
		{
			const int nNewSlot = H.bHaveCur ? 1 - H.nCurSlot : 0;
			H.bHavePrev = H.bHaveCur;
			H.nCurSlot = nNewSlot;
			H.bHaveCur = true;
			H.ulCurPair = slot.ulPairId;
			H.bEstimateValid = false;
			for ( bool &b : H.bOutValid )
				b = false;

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
		}

		const bool bGenerate = slot.nK >= 1 && slot.nK < slot.nN && H.bHavePrev;
		if ( bGenerate )
		{
			const int nK = slot.nK;
			const int nIdx = nK - 1;

			if ( H.bOutValid[ nIdx ] && H.nOutN[ nIdx ] == slot.nN )
			{
				// D13: this slot of this pair was generated by an earlier composite.
				pResult = H.pOut[ nIdx ];
			}
			else
			{
				CVulkanTexture *pPrev = H.pRing[ 1 - H.nCurSlot ].get();
				CVulkanTexture *pCurr = H.pRing[ H.nCurSlot ].get();
				bool bOk = true;

				if ( !H.pOut[ nIdx ] )
					bOk = CreateTexture( H.pOut[ nIdx ], uWidth, uHeight );

				if ( bOk && !H.bEstimateValid )
				{
					// D16: a new estimate resets the library's query pool; read the previous
					// pair's timings first, if they have retired.
					if ( H.bProfiling )
					{
						HarvestProfile( H, true );
						H.nProfileSynths = 0;
						H.nProfileSynthsWanted = slot.nN - 1;
					}

					CVulkanCmdBuffer *pCb = Cmd();
					pCb->prepareSrcImage( pPrev );
					pCb->prepareSrcImage( pCurr );
					pCb->insertBarrier();
					bOk = H.interp.recordEstimate( pCb->rawBuffer(), pPrev->srgbView(), pCurr->srgbView() );
					if ( bOk )
					{
						H.bEstimateValid = true;
						if ( H.bProfiling )
						{
							H.bProfileOpen = true;
							bProfileWritten = true;
						}
					}
				}

				if ( bOk )
				{
					CVulkanTexture *pOut = H.pOut[ nIdx ].get();
					CVulkanCmdBuffer *pCb = Cmd();
					// The output is fully overwritten: prepareDestImage discards its old
					// contents (UNDEFINED -> GENERAL), and insertBarrier orders it after
					// every earlier read of it (the previous pair's composite).
					pCb->prepareDestImage( pOut );
					pCb->insertBarrier();
					bOk = H.interp.recordSynth( pCb->rawBuffer(), pPrev->srgbView(), pCurr->srgbView(), pOut->srgbView(),
						float( nK ) / float( slot.nN ) );
					if ( bOk )
					{
						pCb->markDirty( pOut );
						pCb->insertBarrier();   // flush for the consumers in later command buffers
						H.bOutValid[ nIdx ] = true;
						H.nOutN[ nIdx ] = slot.nN;
						if ( H.bProfileOpen )
						{
							H.nProfileSynths++;
							bProfileWritten = true;
						}
						pResult = H.pOut[ nIdx ];
					}
				}

				if ( !bOk )
				{
					// Nothing was recorded for the failing call, so `pResult` stays null:
					// the real frame is shown for this slot (library contract: never
					// present an `out` whose record returned false).
					SetReason( Unavailable::RecordFailed );
				}
			}
		}

		if ( pCmd )
		{
			const uint64_t ulSeq = g_device.submit( std::move( pCmd ) );
			if ( bProfileWritten )
				H.ulProfileSeq = ulSeq;
		}

		// Cheap poll for the finished pair's timings (a counter read, never a wait).
		if ( H.bProfiling )
			HarvestProfile( H, false );

		return pResult;
	}
}
