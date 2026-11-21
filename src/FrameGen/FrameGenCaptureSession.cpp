// FrameGenCaptureSession.cpp -- the GPU half of `ritz_fg_capture`: see FrameGenCapture.h and
// superdoc/features/frame-generation-capture.md.
//
// HOW THE RECORDS STAY TOGETHER. The output slot is reused after three synths and a ring slot after
// three real frames, so nothing is ever read back from a live slot later. For every generated frame
// the copies of its output and of each real frame of its pair that this capture has not stored yet are
// recorded into the SAME command buffer as the synth (right after it), into staging images that only
// the capture owns. A record is then a self-contained set of staging images + one text line; the writer
// thread waits for the submission that carries the copies to retire (the timeline counter, never a
// device wait) and only then reads the staging memory.

#include "FrameGenCapture.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <thread>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../../subprojects/FrameGen/gpu/framegen.h"

#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "log.hpp"
#include "convar.h"
#include "FrameGenHost.h"

#include "Changelog.h"
#include "RitzVersion.h"

static LogScope cap_log( "framegen_capture" );

namespace fghost::capture
{
	namespace
	{
		enum : uint32_t { kIdle = 0, kArmed = 1, kRunning = 2, kWriting = 3, kArming = 4 };

		std::atomic<uint32_t> g_uState{ kIdle };
		std::atomic<uint64_t> g_ulDueNs{ 0 };   // the armed capture's start time (monotonic); written before g_uState goes to kArmed

		// One thing for the writer to do, in order.
		struct Job
		{
			enum Kind { Header, Frame, End } eKind = Frame;
			uint64_t ulSeq = 0;                  // Frame: the submission that carries its copies
			std::string sText;                   // Header: all lines; Frame: its line; End: unused
			std::string sReason;                 // End
			struct Image
			{
				std::string sName;
				const uint8_t *pPixels = nullptr;
				uint32_t uPitch = 0;
			};
			Image out;
			Image reals[ 2 ];
			int nReals = 0;
		};

		struct Session
		{
			std::string sDir;
			uint32_t uWidth = 0, uHeight = 0, uBpp = 0;
			uint64_t ulConfigKey = 0;
			uint64_t ulLastProgressNs = 0;
			int nVkFormat = 0;
			uint32_t uDrmFormat = 0;
			float flPrepareMs = 0.0f;
			bool bPreparedEarly = false;
			// For the `timing` line.
			uint64_t ulStartNs = 0, ulFirstFrameNs = 0, ulLastFrameNs = 0;
			uint64_t ulMaxDecisionGapNs = 0, ulGapAfterCounter = 0, ulLastDecisionToStartNs = 0;

			// Staging (paint thread): N outputs, N + 2 real frames, each an own host-visible linear image.
			std::vector<gamescope::Rc<CVulkanTexture>> pOuts;
			std::vector<gamescope::Rc<CVulkanTexture>> pReals;
			size_t nOutsUsed = 0, nRealsUsed = 0;
			std::map<uint64_t, size_t> realByCommit;   // commit id -> its staging image (a real frame is stored ONCE)

			int nCaptured = 0;     // frames recorded (the next one's seq)
			int nDropped = 0;
			std::vector<Job> pending;      // this composite's records, handed over by Submitted()
			std::string sFinishReason;     // set: end the burst when the pending records are handed over
			bool bEndQueued = false;

			// The writer.
			std::thread thread;
			std::mutex mutex;
			std::condition_variable cv;
			std::deque<Job> queue;
			std::atomic<bool> bAbort{ false };
			std::atomic<bool> bFailed{ false };
			std::atomic<bool> bDone{ false };
			std::atomic<int> nFramesWritten{ 0 };
			std::atomic<int> nRealsWritten{ 0 };
		};

		Session *g_pSession = nullptr;   // paint thread; the writer gets its own pointer
		void DropPrepared();

		void Enqueue( Session &s, Job &&j )
		{
			{
				std::lock_guard<std::mutex> lock( s.mutex );
				s.queue.push_back( std::move( j ) );
			}
			s.cv.notify_one();
		}

		bool WriteImageFile( const std::string &sPath, const Job::Image &im, uint32_t uRowBytes, uint32_t uHeight )
		{
			FILE *f = fopen( sPath.c_str(), "wb" );
			if ( !f )
				return false;
			bool bOk = WriteTightRows( f, im.pPixels, im.uPitch, uRowBytes, uHeight );
			bOk = ( fclose( f ) == 0 ) && bOk;
			return bOk;
		}

		void WriterMain( Session *s )
		{
			FILE *pText = nullptr;
			bool bBad = false;
			std::string sBadWhy;
			const uint32_t uRowBytes = s->uWidth * s->uBpp;
			auto Bad = [ & ]( const std::string &sWhy )
			{
				if ( !bBad )
					cap_log.errorf( "capture stopped: %s", sWhy.c_str() );
				bBad = true;
				sBadWhy = sWhy;
				s->bFailed.store( true, std::memory_order_relaxed );
			};

			for ( ;; )
			{
				Job j;
				{
					std::unique_lock<std::mutex> lock( s->mutex );
					s->cv.wait( lock, [ & ] { return !s->queue.empty() || s->bAbort.load( std::memory_order_relaxed ); } );
					if ( s->queue.empty() )
					{
						// Aborted (process exit) with nothing queued: write the end line below.
						j.eKind = Job::End;
						j.sReason = "shutdown";
					}
					else
					{
						j = std::move( s->queue.front() );
						s->queue.pop_front();
					}
				}

				if ( j.eKind == Job::Header )
				{
					pText = fopen( ( s->sDir + "/capture.txt" ).c_str(), "wb" );
					if ( !pText || fwrite( j.sText.data(), 1, j.sText.size(), pText ) != j.sText.size() )
						Bad( "could not write capture.txt" );
					continue;
				}

				if ( j.eKind == Job::Frame )
				{
					if ( bBad )
						continue;
					if ( s->bAbort.load( std::memory_order_relaxed ) )
						continue;   // exit: skip what is left, the end line says why
					// Wait for the submission that carries this record's copies. A counter read, never a device wait.
					const auto tStart = std::chrono::steady_clock::now();
					while ( g_device.completedSeqNo() < j.ulSeq )
					{
						if ( s->bAbort.load( std::memory_order_relaxed ) )
							break;
						if ( std::chrono::steady_clock::now() - tStart > std::chrono::seconds( 10 ) )
						{
							Bad( "a staging copy never retired" );
							break;
						}
						std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
					}
					if ( bBad || s->bAbort.load( std::memory_order_relaxed ) )
						continue;

					bool bOk = WriteImageFile( s->sDir + "/" + j.out.sName, j.out, uRowBytes, s->uHeight );
					int nRealsOk = 0;
					for ( int i = 0; i < j.nReals && bOk; i++ )
					{
						bOk = WriteImageFile( s->sDir + "/" + j.reals[ i ].sName, j.reals[ i ], uRowBytes, s->uHeight );
						if ( bOk )
							nRealsOk++;
					}
					s->nRealsWritten.fetch_add( nRealsOk, std::memory_order_relaxed );
					if ( bOk )
					{
						const std::string sLine = j.sText + "\n";
						if ( pText && fwrite( sLine.data(), 1, sLine.size(), pText ) == sLine.size() )
							s->nFramesWritten.fetch_add( 1, std::memory_order_relaxed );
						else
							bOk = false;
					}
					if ( !bOk )
						Bad( "a file could not be written (disk full or not writable)" );
					continue;
				}

				// End.
				const char *pszReason = bBad ? "disk" : s->bAbort.load( std::memory_order_relaxed ) ? "shutdown" : j.sReason.c_str();
				const int nFrames = s->nFramesWritten.load( std::memory_order_relaxed );
				const int nReals = s->nRealsWritten.load( std::memory_order_relaxed );
				// Frames that were recorded but never written (a failed disk, an exit) are dropped too.
				const int nDropped = s->nDropped + ( s->nCaptured - nFrames );
				const std::string sTiming = FormatTimingLine( s->ulFirstFrameNs, s->ulLastFrameNs, s->ulMaxDecisionGapNs, s->ulGapAfterCounter,
					s->ulLastDecisionToStartNs ) + "\n";
				const std::string sEnd = sTiming + FormatEndLine( nFrames, nReals, nDropped, pszReason ) + "\n";
				if ( pText )
				{
					fwrite( sEnd.data(), 1, sEnd.size(), pText );
					fclose( pText );
					pText = nullptr;
				}
				cap_log.infof( "capture finished (%s): %d frames, %d real frames, %d dropped in %s", pszReason, nFrames, nReals, nDropped, s->sDir.c_str() );
				s->bDone.store( true, std::memory_order_release );
				return;
			}
		}

		// Joins a still-running writer at process exit (never touches the textures: the Session is leaked).
		struct ExitJoiner
		{
			~ExitJoiner()
			{
				Session *s = g_pSession;
				if ( s && s->thread.joinable() )
				{
					s->bAbort.store( true, std::memory_order_relaxed );
					s->cv.notify_all();
					s->thread.join();
				}
			}
		};

		std::string CacheBase()
		{
			const char *pszXdg = getenv( "XDG_CACHE_HOME" );
			std::string s;
			if ( pszXdg && *pszXdg )
				s = pszXdg;
			else
			{
				const char *pszHome = getenv( "HOME" );
				s = std::string( pszHome && *pszHome ? pszHome : "." ) + "/.cache";
			}
			return s + "/gamescope-ritz/fg-capture";
		}

		// mkdir -p; false on failure.
		bool MakeDirs( const std::string &sPath )
		{
			std::string s;
			for ( size_t i = 0; i <= sPath.size(); i++ )
			{
				if ( i == sPath.size() || sPath[ i ] == '/' )
				{
					if ( !s.empty() && mkdir( s.c_str(), 0755 ) != 0 && errno != EEXIST )
						return false;
				}
				if ( i < sPath.size() )
					s += sPath[ i ];
			}
			return true;
		}

		// The first ancestor of `sPath` that exists (statvfs needs one).
		std::string ExistingAncestor( std::string sPath )
		{
			struct stat st;
			while ( sPath.size() > 1 && stat( sPath.c_str(), &st ) != 0 )
			{
				const size_t n = sPath.find_last_of( '/' );
				if ( n == std::string::npos )
					return ".";
				sPath = n == 0 ? "/" : sPath.substr( 0, n );
			}
			return sPath.empty() ? "." : sPath;
		}

		void Finish( Session &s, const char *pszReason )
		{
			if ( g_uState.load( std::memory_order_relaxed ) != kRunning )
				return;
			g_uState.store( kWriting, std::memory_order_relaxed );
			if ( !s.pending.empty() )
			{
				s.sFinishReason = pszReason;   // queued by Submitted(), after the records of this composite
				return;
			}
			Job e;
			e.eKind = Job::End;
			e.sReason = pszReason;
			s.bEndQueued = true;
			Enqueue( s, std::move( e ) );
		}

		void Release()
		{
			Session *s = g_pSession;
			if ( s->thread.joinable() )
				s->thread.join();
			g_pSession = nullptr;
			delete s;   // frees the staging images here, on the paint thread
			g_uState.store( kIdle, std::memory_order_release );
		}
	}

	// ------------------------------------------------------------------
	//  Console thread
	// ------------------------------------------------------------------

	bool Request( int nSeconds, std::string *psMessage )
	{
		// idle -> arming claims the slot (a refused call must not touch the armed deadline), then the deadline
		// is stored, then Armed publishes it: the paint thread never sees Armed without its deadline.
		uint32_t uExpected = kIdle;
		if ( !g_uState.compare_exchange_strong( uExpected, kArming, std::memory_order_acquire, std::memory_order_relaxed ) )
		{
			if ( psMessage )
				*psMessage = uExpected == kRunning ? "a capture is running; refused"
					: uExpected == kWriting ? "a capture is still writing its files; refused" : "a capture is already armed; refused";
			return false;
		}
		g_ulDueNs.store( get_time_in_nanos() + uint64_t( nSeconds ) * 1000ull * 1000ull * 1000ull, std::memory_order_relaxed );
		g_uState.store( kArmed, std::memory_order_release );
		if ( psMessage )
			*psMessage = nSeconds > 0 ? "capture armed: it starts in " + std::to_string( nSeconds ) + " s" : "capture starting now";
		return true;
	}

	bool Pending()
	{
		return g_uState.load( std::memory_order_relaxed ) != kIdle;
	}

	bool Running()
	{
		return g_uState.load( std::memory_order_relaxed ) == kRunning;
	}

	// ------------------------------------------------------------------
	//  Paint thread
	// ------------------------------------------------------------------

	void Poll( uint64_t ulNowNs, uint64_t ulConfigKey )
	{
		const uint32_t uState = g_uState.load( std::memory_order_acquire );
		if ( uState == kIdle || uState == kArming )
			return;
		if ( uState == kArmed )
		{
			// Armed but never able to start (no live renderer: frame generation off): give up after the timeout.
			if ( ulNowNs > g_ulDueNs.load( std::memory_order_relaxed ) + kTimeoutNs )
			{
				cap_log.warnf( "capture not started: frame generation was not running for %d s after the start time", int( kTimeoutNs / 1000000000ull ) );
				DropPrepared();
				g_uState.store( kIdle, std::memory_order_release );
			}
			return;
		}
		Session *s = g_pSession;
		if ( !s )
		{
			g_uState.store( kIdle, std::memory_order_release );
			return;
		}
		if ( uState == kRunning )
		{
			if ( s->bFailed.load( std::memory_order_relaxed ) )
				Finish( *s, "disk" );
			else if ( ulConfigKey != s->ulConfigKey )
				Finish( *s, "settings" );
			else if ( ulNowNs - s->ulLastProgressNs > kTimeoutNs )
				Finish( *s, "timeout" );
			return;
		}
		// Writing: free everything once the writer is done (it logged the result itself).
		if ( s->bDone.load( std::memory_order_acquire ) )
			Release();
	}

	bool DueToStart( uint64_t ulNowNs )
	{
		// True while a capture is armed: the renderer entry point then calls Start() every composite, which
		// prepares early (once the renderer is live) and flips to running at the due time.
		(void)ulNowNs;
		return g_uState.load( std::memory_order_acquire ) == kArmed;
	}

	namespace
	{
		// A prepared session has no writer thread yet: dropping it frees the staging and removes its empty directory.
		void DropPrepared()
		{
			Session *s = g_pSession;
			if ( !s || s->thread.joinable() )
				return;
			if ( !s->sDir.empty() )
				rmdir( s->sDir.c_str() );
			g_pSession = nullptr;
			delete s;
		}

		// Space check, staging allocation, directory: the expensive part, done early for an armed capture so the
		// stall is not inside the burst. False: refused (logged), state back to idle.
		bool Prepare( const Env &env, uint64_t ulConfigKey )
		{
			const auto tStart = std::chrono::steady_clock::now();
			auto Refuse = [ & ]( const char *pszWhy )
			{
				cap_log.errorf( "capture refused: %s", pszWhy );
				g_uState.store( kIdle, std::memory_order_release );
				return false;
			};
			const uint32_t uBpp = VkFormatBytes( env.nVkFormat );
			if ( uBpp == 0 )
				return Refuse( "the game's pixel format is not one the capture can store" );

			// Space first: nothing is created before it is known to fit.
			const std::string sBase = CacheBase();
			{
				struct statvfs sv;
				const std::string sAnc = ExistingAncestor( sBase );
				if ( statvfs( sAnc.c_str(), &sv ) != 0 )
					return Refuse( "could not read the free space of the cache directory" );
				const uint64_t ulAvail = uint64_t( sv.f_bavail ) * uint64_t( sv.f_frsize );
				const uint64_t ulNeed = RequiredBytes( env.uWidth, env.uHeight, uBpp, kFrames );
				if ( !HasEnoughSpace( ulAvail, ulNeed ) )
				{
					cap_log.errorf( "capture refused: not enough free space in %s (%llu MiB free, %llu MiB needed)", sAnc.c_str(),
						(unsigned long long)( ulAvail >> 20 ), (unsigned long long)( ulNeed >> 20 ) );
					g_uState.store( kIdle, std::memory_order_release );
					return false;
				}
			}

			std::unique_ptr<Session> pS = std::make_unique<Session>();
			pS->uWidth = env.uWidth;
			pS->uHeight = env.uHeight;
			pS->uBpp = uBpp;
			pS->nVkFormat = env.nVkFormat;
			pS->uDrmFormat = env.uDrmFormat;
			pS->ulConfigKey = ulConfigKey;

			CVulkanTexture::createFlags flags;
			flags.bMappable = true;
			flags.bTransferDst = true;
			const size_t nOuts = size_t( kFrames ), nReals = size_t( kFrames ) + 2;
			for ( size_t i = 0; i < nOuts + nReals; i++ )
			{
				gamescope::Rc<CVulkanTexture> pTex = new CVulkanTexture();
				if ( !pTex->BInit( env.uWidth, env.uHeight, 1u, env.uDrmFormat, flags, nullptr ) || !pTex->mappedData()
					|| int( pTex->format() ) != env.nVkFormat || pTex->rowPitch() < env.uWidth * uBpp )
					return Refuse( "could not allocate the staging images (not enough memory, or the format cannot be copied)" );
				( i < nOuts ? pS->pOuts : pS->pReals ).push_back( std::move( pTex ) );
			}

			char szStamp[ 32 ];
			struct tm tmLocal;
			const time_t tNow = time( nullptr );
			localtime_r( &tNow, &tmLocal );
			strftime( szStamp, sizeof( szStamp ), "%Y%m%d-%H%M%S", &tmLocal );
			if ( !MakeDirs( sBase ) )
				return Refuse( "could not create the cache directory" );
			std::string sDir = sBase + "/" + szStamp;
			for ( int n = 1; mkdir( sDir.c_str(), 0755 ) != 0; n++ )
			{
				if ( errno != EEXIST || n > 99 )
					return Refuse( "could not create the capture directory" );
				sDir = sBase + "/" + szStamp + "-" + std::to_string( n );
			}
			pS->sDir = sDir;
			pS->flPrepareMs = float( std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - tStart ).count() );
			g_pSession = pS.release();
			return true;
		}
	}

	bool Start( const Env &env, uint64_t ulNowNs, uint64_t ulConfigKey )
	{
		if ( g_uState.load( std::memory_order_acquire ) != kArmed || !env.pSettings )
			return false;
		const uint64_t ulDue = g_ulDueNs.load( std::memory_order_relaxed );

		// A prepared session of another size or format is dropped and prepared again (simplest, leak-free).
		if ( g_pSession && ( g_pSession->uWidth != env.uWidth || g_pSession->uHeight != env.uHeight || g_pSession->nVkFormat != env.nVkFormat ) )
		{
			cap_log.infof( "capture: the game's size or format changed after the staging was prepared; preparing again" );
			DropPrepared();
		}
		if ( !g_pSession )
		{
			if ( !Prepare( env, ulConfigKey ) )
				return false;
			g_pSession->bPreparedEarly = ulNowNs < ulDue;
			if ( g_pSession->bPreparedEarly )
				cap_log.infof( "capture prepared: staging for %d frames of %ux%u (format %s) in %.1f ms, into %s; starts in %.1f s", kFrames,
					env.uWidth, env.uHeight, VkFormatName( env.nVkFormat ), double( g_pSession->flPrepareMs ), g_pSession->sDir.c_str(),
					double( ulDue - ulNowNs ) / 1e9 );
			else
				cap_log.infof( "capture: preparing the staging took %.1f ms (immediate start)", double( g_pSession->flPrepareMs ) );
		}
		if ( ulNowNs < ulDue )
			return true;   // prepared, waiting for the start time

		Session &s = *g_pSession;
		s.ulConfigKey = ulConfigKey;
		s.ulLastProgressNs = ulNowNs;

		char szWall[ 40 ];
		struct tm tmLocal;
		const time_t tNow = time( nullptr );
		localtime_r( &tNow, &tmLocal );
		strftime( szWall, sizeof( szWall ), "%Y-%m-%dT%H:%M:%S%z", &tmLocal );

		HeaderInfo h;
		h.sWallStart = szWall;
		h.ulStartNs = ulNowNs;
		h.uWidth = env.uWidth;
		h.uHeight = env.uHeight;
		h.nVkFormat = env.nVkFormat;
		h.uBytesPerPixel = s.uBpp;
		h.sLibraryCommit = gamescope::k_szRitzFrameGenCommit;
		h.sRitzVersion = g_Changelog_Version;
		h.sRitzCommit = gamescope::k_szRitzCommit;
		h.bRitzDirty = gamescope::k_bRitzDirty;
		h.sMotionWanted = env.pszMotionWanted;
		h.sMotionEffective = env.pszMotionEffective;
		h.uFlowScale = env.pSettings->flowScale;
		h.nFrames = kFrames;
		h.flPrepareMs = s.flPrepareMs;
		h.bPreparedEarly = s.bPreparedEarly;
		Job head;
		head.eKind = Job::Header;
		head.sText = FormatHeaderLine( h ) + "\n";
		for ( const std::string &sLine : FormatSettingLines( *env.pSettings ) )
			head.sText += sLine + "\n";
		const std::vector<DecisionRec> decisions = Decisions().Snapshot( kDecisionsDumped );
		for ( const DecisionRec &r : decisions )
			head.sText += FormatDecisionLine( r ) + "\n";
		s.ulMaxDecisionGapNs = MaxDecisionGapNs( decisions, &s.ulGapAfterCounter );
		s.ulStartNs = ulNowNs;
		s.ulLastDecisionToStartNs = decisions.empty() || ulNowNs < decisions.back().ns ? 0 : ulNowNs - decisions.back().ns;

		static ExitJoiner s_Joiner;
		(void)s_Joiner;
		s.thread = std::thread( WriterMain, &s );
		Enqueue( s, std::move( head ) );
		g_uState.store( kRunning, std::memory_order_release );
		cap_log.infof( "capture started: %d generated frames of %ux%u (format %s) into %s", kFrames, env.uWidth, env.uHeight,
			VkFormatName( env.nVkFormat ), s.sDir.c_str() );
		return true;
	}

	void RecordFrame( CVulkanCmdBuffer *pCmd, const FrameIn &in )
	{
		Session *s = g_pSession;
		if ( !s || g_uState.load( std::memory_order_relaxed ) != kRunning || !pCmd || !in.pOut )
			return;
		s->ulLastProgressNs = get_time_in_nanos();

		// The reals this record needs: stored already, or a free staging image and a ring copy to take.
		struct Need { uint64_t ulId; const gamescope::Rc<CVulkanTexture> *pSrc; size_t nIdx; bool bNew; };
		Need need[ 2 ];
		int nNeed = 0;
		size_t nRealsToUse = s->nRealsUsed;
		bool bOk = s->nOutsUsed < s->pOuts.size();
		const std::pair<uint64_t, const gamescope::Rc<CVulkanTexture> *> pair[ 2 ] = { { in.ulPrevId, &in.pPrev }, { in.ulCurrId, &in.pCurr } };
		for ( const auto &p : pair )
		{
			if ( !bOk )
				break;
			const auto it = s->realByCommit.find( p.first );
			if ( it != s->realByCommit.end() )
			{
				need[ nNeed++ ] = { p.first, nullptr, it->second, false };
			}
			else if ( *p.second && nRealsToUse < s->pReals.size() )
			{
				need[ nNeed++ ] = { p.first, p.second, nRealsToUse++, true };
			}
			else
			{
				bOk = false;   // the frame is gone from the ring and was never stored, or the staging ran out
			}
		}
		if ( !bOk )
		{
			s->nDropped++;
			if ( s->nCaptured + s->nDropped >= kFrames )
				Finish( *s, "complete" );
			return;
		}

		// All of it can be recorded: the copies (output first, then the new reals), same command buffer.
		const size_t nOutIdx = s->nOutsUsed++;
		pCmd->copyImage( in.pOut, s->pOuts[ nOutIdx ] );

		Job j;
		j.eKind = Job::Frame;
		j.out = { OutFileName( s->nCaptured ), s->pOuts[ nOutIdx ]->mappedData(), s->pOuts[ nOutIdx ]->rowPitch() };
		for ( int i = 0; i < nNeed; i++ )
		{
			if ( !need[ i ].bNew )
				continue;
			pCmd->copyImage( *need[ i ].pSrc, s->pReals[ need[ i ].nIdx ] );
			s->realByCommit[ need[ i ].ulId ] = need[ i ].nIdx;
			j.reals[ j.nReals++ ] = { RealFileName( need[ i ].ulId ), s->pReals[ need[ i ].nIdx ]->mappedData(), s->pReals[ need[ i ].nIdx ]->rowPitch() };
		}
		s->nRealsUsed = nRealsToUse;

		FrameInfo f;
		f.nSeq = s->nCaptured;
		f.ulOutId = in.ulOutId;
		f.ulPrevId = in.ulPrevId;
		f.ulCurrId = in.ulCurrId;
		f.ulNewestId = in.ulNewestId;
		f.flT0 = in.flT0;
		f.flT1 = in.flT1;
		f.nArrivalPrevNs = Arrivals().Find( in.ulPrevId );
		f.nArrivalCurrNs = Arrivals().Find( in.ulCurrId );
		f.ulNowNs = get_time_in_nanos();
		if ( s->nCaptured == 0 )
			s->ulFirstFrameNs = f.ulNowNs;
		s->ulLastFrameNs = f.ulNowNs;
		f.uWidth = s->uWidth;
		f.uHeight = s->uHeight;
		f.bEstimateRecorded = in.nEstimateRet >= 0;
		f.nEstimateRet = in.nEstimateRet;
		f.nSynthRet = in.nSynthRet;
		f.bCached = in.bCached;
		f.nOutSlot = in.nOutSlot;
		f.nPrevSlot = in.nPrevSlot;
		f.nCurrSlot = in.nCurrSlot;
		f.sOutFile = j.out.sName;
		f.sPrevFile = RealFileName( in.ulPrevId );
		f.sCurrFile = RealFileName( in.ulCurrId );
		j.sText = FormatFrameLine( f );
		s->pending.push_back( std::move( j ) );
		s->nCaptured++;
		if ( s->nCaptured + s->nDropped >= kFrames )
			Finish( *s, "complete" );
	}

	void Submitted( uint64_t ulSeq )
	{
		Session *s = g_pSession;
		if ( !s || s->pending.empty() )
			return;
		for ( Job &j : s->pending )
		{
			j.ulSeq = ulSeq;
			Enqueue( *s, std::move( j ) );
		}
		s->pending.clear();
		if ( !s->sFinishReason.empty() && !s->bEndQueued )
		{
			Job e;
			e.eKind = Job::End;
			e.sReason = s->sFinishReason;
			s->bEndQueued = true;
			Enqueue( *s, std::move( e ) );
		}
	}

	void Abort( const char *pszReason )
	{
		Session *s = g_pSession;
		if ( s && g_uState.load( std::memory_order_relaxed ) == kRunning )
			Finish( *s, pszReason );
	}
}

// `gamescopectl ritz_fg_capture [seconds]` -- runs on the console thread and only sets atomics.
static gamescope::ConCommand cc_ritz_fg_capture(
	"ritz_fg_capture",
	"Troubleshooting: record the next 40 generated frames of Frame generation, with the real frames they were made from, "
	"the library settings and the pacer's recent decisions, to files under ~/.cache/gamescope-ritz/fg-capture/. "
	"ritz_fg_capture starts now; ritz_fg_capture <seconds> (0..600) starts that many seconds later.",
	[]( std::span<std::string_view> args )
	{
		const fghost::capture::ArgResult r = fghost::capture::ParseArgs( args );
		if ( !r.ok )
		{
			console_log.errorf( "%s", r.error.c_str() );
			return;
		}
		std::string sMessage;
		if ( !fghost::capture::Request( r.seconds, &sMessage ) )
		{
			console_log.errorf( "ritz_fg_capture: %s", sMessage.c_str() );
			return;
		}
		console_log.infof( "ritz_fg_capture: %s (the files are written under the cache directory, the path is logged when it starts)", sMessage.c_str() );
		if ( !fghost::Active() )
			console_log.warnf( "ritz_fg_capture: frame generation is off; the capture waits up to %d s after its start time for it, then gives up",
				int( fghost::capture::kTimeoutNs / 1000000000ull ) );
	} );
