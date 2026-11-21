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
		// The watch request (written before g_uState goes to kArmed, read by Start()).
		std::atomic<bool> g_bWatchReq{ false };
		std::atomic<int> g_nWatchSeconds{ kWatchDefaultSeconds };
		std::atomic<float> g_flWatchSens{ 1.0f };

		// One thing for the writer to do, in order.
		struct Job
		{
			enum Kind { Header, Frame, End, Measure, Dump } eKind = Frame;
			uint64_t ulSeq = 0;                  // Frame / Measure / Dump: the submission that carries (the last of) its copies
			std::string sText;                   // Header: all lines; Frame: its line; Dump: the lines before the frame lines
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
			// Measure: the record, its pair (ids + staging, always both) and the blend position.
			uint64_t ulRec = 0;
			uint64_t ulRealId[ 2 ] = {};
			float flT = 0.0f;
			// Dump: one frame line + images per record in the window; the reals listed once in `dumpReals`.
			struct DumpFrame { std::string sLine; Image out; };
			std::vector<DumpFrame> dumpFrames;
			std::vector<Image> dumpReals;
			std::string sTail;                   // Dump: timing + end lines
			int nDump = 0;
		};

		// The block means of the real frames, by commit id (each real is used by several outputs).
		struct MeansCache
		{
			std::deque<std::pair<uint64_t, BlockMeans>> v;
			const BlockMeans *Find( uint64_t ulId ) const
			{
				for ( const auto &e : v )
				{
					if ( e.first == ulId )
						return &e.second;
				}
				return nullptr;
			}
			const BlockMeans *Put( uint64_t ulId, BlockMeans &&m )
			{
				if ( v.size() >= 8 )
					v.pop_front();
				v.emplace_back( ulId, std::move( m ) );
				return &v.back().second;
			}
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

			// ---- watch mode (bWatch) ----
			bool bWatch = false;
			int nWatchSeconds = 0;
			float flSens = 1.0f;
			uint64_t ulWatchEndNs = 0;
			std::string sWatchHeader;          // `watch v=1 ...`
			HeaderInfo header;                 // for the dumps' capture lines
			std::string sSettingsText;         // the setting lines, for the dumps
			std::unique_ptr<RollingWindow> pWindow;
			std::vector<WatchRec> recs;        // reserved at the start, never reallocated (the writer fills coarse / pair in place)
			size_t nRecCap = 0;
			uint64_t ulDroppedRecs = 0, ulNotMeasuredDumping = 0, ulTriggers = 0;
			uint64_t ulLastCurr = 0, ulLastOut = 0;
			std::atomic<uint64_t> ulAnalysedEnd{ 0 };   // every record < this one with images was analysed
			// The pending dump trigger (paint thread and writer).
			std::mutex trigMutex;
			bool bTrig = false;
			std::string sTrigKind;
			uint64_t ulTrigRec = 0;
			float flTrigCoarse = -1.0f, flTrigPair = -1.0f, flTrigThr = -1.0f;
			bool bBlockedNow = false;          // this composite's Add() was refused because of the trigger record
			bool bFrozen = false;              // a dump is being written: nothing is admitted
			std::atomic<bool> bDumpDone{ false };
			int nDumps = 0;
			// Analysis cost (writer).
			uint64_t ulAnalysisNs = 0, ulAnalysisMaxNs = 0, ulAnalysed = 0, ulPixelTriggers = 0;
			MeansCache means;
			uint64_t ulTrigId = 0, ulTrigSameAs = 0;   // a `repeat` trigger's two identical frames
			// The repeat test (writer): the watch's, and the burst's over its stored real frames.
			RepeatDetector repeat;
			std::string sBurstRepeatLines;
			int nBurstRealsChecked = 0, nBurstRepeat = 0, nBurstStatic = 0;
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

				// Waits for the submission `ulSeq` to retire. A counter read, never a device wait. False: aborted / failed.
				auto WaitSeq = [ & ]( uint64_t ulSeq ) -> bool
				{
					const auto tStart = std::chrono::steady_clock::now();
					while ( g_device.completedSeqNo() < ulSeq )
					{
						if ( s->bAbort.load( std::memory_order_relaxed ) )
							return false;
						if ( std::chrono::steady_clock::now() - tStart > std::chrono::seconds( 10 ) )
						{
							Bad( "a staging copy never retired" );
							return false;
						}
						std::this_thread::sleep_for( std::chrono::microseconds( 250 ) );
					}
					return !s->bAbort.load( std::memory_order_relaxed );
				};

				if ( j.eKind == Job::Measure )
				{
					// The pixel trigger: block means of the output against the blend of its pair (FrameGenCapture.h).
					if ( !bBad && WaitSeq( j.ulSeq ) )
					{
						const auto t0 = std::chrono::steady_clock::now();
						BlockMeans mOut;
						const BlockMeans *pM[ 2 ] = {};
						bool bOk = ComputeBlockMeans( j.out.pPixels, j.out.uPitch, s->uWidth, s->uHeight, s->nVkFormat, kMetricStep, &mOut );
						for ( int i = 0; i < 2 && bOk; i++ )
						{
							pM[ i ] = s->means.Find( j.ulRealId[ i ] );
							if ( !pM[ i ] )
							{
								BlockMeans m;
								bOk = ComputeBlockMeans( j.reals[ i ].pPixels, j.reals[ i ].uPitch, s->uWidth, s->uHeight, s->nVkFormat, kMetricStep, &m );
								if ( bOk )
								{
									// A real frame seen for the first time: the repeat test (a slot carrying a new id over an old image).
									const RepeatDetector::Result rr = s->repeat.Add( j.ulRealId[ i ], m,
										RowHash( j.reals[ i ].pPixels, j.reals[ i ].uPitch, s->uWidth, s->uHeight, s->uBpp ) );
									if ( rr.eKind == RepeatDetector::kStatic )
										s->recs[ size_t( j.ulRec ) ].uFlags |= kFlagStatic;
									else if ( rr.eKind == RepeatDetector::kRepeat )
									{
										s->recs[ size_t( j.ulRec ) ].uFlags |= kFlagRepeat;
										cap_log.warnf( "watch: real frame %llu is identical to real frame %llu (a different frame between): a repeat",
											(unsigned long long)j.ulRealId[ i ], (unsigned long long)rr.ulSameAs );
										std::lock_guard<std::mutex> lock( s->trigMutex );
										if ( !s->bTrig )
										{
											s->bTrig = true;
											s->sTrigKind = "repeat";
											s->ulTrigRec = j.ulRec;
											s->flTrigCoarse = s->flTrigPair = s->flTrigThr = -1.0f;
											s->ulTrigId = j.ulRealId[ i ];
											s->ulTrigSameAs = rr.ulSameAs;
										}
									}
									pM[ i ] = s->means.Put( j.ulRealId[ i ], std::move( m ) );
								}
							}
						}
						// A cache insert may have dropped pM[0] (deque front): look it up again.
						if ( bOk )
							pM[ 0 ] = s->means.Find( j.ulRealId[ 0 ] );
						if ( bOk && pM[ 0 ] && pM[ 1 ] )
						{
							const float flCoarse = BlendDiff( mOut, *pM[ 0 ], *pM[ 1 ], j.flT );
							const float flPair = BlockDiff( *pM[ 0 ], *pM[ 1 ] );
							if ( flCoarse >= 0.0f && flPair >= 0.0f )
							{
								WatchRec &r = s->recs[ size_t( j.ulRec ) ];
								r.flCoarse = flCoarse;
								r.flPair = flPair;
								if ( PixelTriggers( flCoarse, flPair, s->flSens ) )
								{
									s->ulPixelTriggers++;
									std::lock_guard<std::mutex> lock( s->trigMutex );
									if ( !s->bTrig )
									{
										s->bTrig = true;
										s->sTrigKind = "pixel";
										s->ulTrigRec = j.ulRec;
										s->flTrigCoarse = flCoarse;
										s->flTrigPair = flPair;
										s->flTrigThr = Threshold( flPair, s->flSens );
										s->ulTrigId = s->ulTrigSameAs = 0;
									}
								}
							}
						}
						const uint64_t ulNs = uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - t0 ).count() );
						s->ulAnalysisNs += ulNs;
						s->ulAnalysisMaxNs = std::max( s->ulAnalysisMaxNs, ulNs );
						s->ulAnalysed++;
					}
					// Published AFTER the trigger: the paint thread never evicts a triggering record unseen.
					s->ulAnalysedEnd.store( j.ulRec + 1, std::memory_order_release );
					continue;
				}

				if ( j.eKind == Job::Dump )
				{
					// A frozen window, written as a normal v=1 capture folder.
					if ( !bBad && WaitSeq( j.ulSeq ) )
					{
						const std::string sDumpDir = s->sDir + "/dump-" + std::to_string( j.nDump );
						bool bOk = mkdir( sDumpDir.c_str(), 0755 ) == 0;
						int nRealsOk = 0;
						for ( const Job::Image &im : j.dumpReals )
						{
							if ( bOk && WriteImageFile( sDumpDir + "/" + im.sName, im, uRowBytes, s->uHeight ) )
								nRealsOk++;
							else
								bOk = false;
						}
						std::string sText = j.sText;
						for ( const Job::DumpFrame &f : j.dumpFrames )
						{
							bOk = bOk && WriteImageFile( sDumpDir + "/" + f.out.sName, f.out, uRowBytes, s->uHeight );
							sText += f.sLine + "\n";
						}
						sText += j.sTail;
						FILE *f = bOk ? fopen( ( sDumpDir + "/capture.txt" ).c_str(), "wb" ) : nullptr;
						bOk = f && fwrite( sText.data(), 1, sText.size(), f ) == sText.size();
						if ( f )
							bOk = ( fclose( f ) == 0 ) && bOk;
						if ( bOk )
							cap_log.infof( "watch: dump %d written (%zu frames, %d real frames) into %s", j.nDump, j.dumpFrames.size(), nRealsOk, sDumpDir.c_str() );
						else
							Bad( "a dump could not be written (disk full or not writable)" );
					}
					s->bDumpDone.store( true, std::memory_order_release );
					continue;
				}

				if ( j.eKind == Job::End && s->bWatch )
				{
					const char *pszReason = bBad ? "disk" : s->bAbort.load( std::memory_order_relaxed ) ? "shutdown" : j.sReason.c_str();
					WatchSummary sum;
					std::string sOut = s->sWatchHeader + "\n";
					for ( const WatchRec &r : s->recs )
					{
						SummaryAdd( &sum, r );
						sOut += FormatWatchLine( r ) + "\n";
					}
					sum.ulDroppedRecords = s->ulDroppedRecs;
					sum.ulNotMeasuredDumping = s->ulNotMeasuredDumping;
					sum.nDumps = s->nDumps;
					sum.ulTriggers = s->ulTriggers + s->ulPixelTriggers;
					char szCost[ 200 ];
					snprintf( szCost, sizeof( szCost ), "cost analysed=%llu analysis_avg_ms=%.3f analysis_max_ms=%.3f images_skipped=%d pixel_triggers=%llu",
						(unsigned long long)s->ulAnalysed, s->ulAnalysed ? double( s->ulAnalysisNs ) / double( s->ulAnalysed ) / 1e6 : 0.0,
						double( s->ulAnalysisMaxNs ) / 1e6, s->nDropped, (unsigned long long)s->ulPixelTriggers );
					sOut += std::string( szCost ) + "\n";
					const std::string sSummary = FormatWatchSummary( sum, pszReason, FormatSpanTokens( ComputeSpanStats( s->recs ) ) );
					sOut += sSummary + "\n";
					FILE *f = fopen( ( s->sDir + "/watch.txt" ).c_str(), "wb" );
					if ( !f || fwrite( sOut.data(), 1, sOut.size(), f ) != sOut.size() )
						cap_log.errorf( "watch: could not write watch.txt" );
					if ( f )
						fclose( f );
					cap_log.infof( "watch finished (%s): %s; %s; in %s", pszReason, sSummary.c_str(), szCost, s->sDir.c_str() );
					s->bDone.store( true, std::memory_order_release );
					return;
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
						{
							nRealsOk++;
							// The repeat test over the stored real frames (in the order they were first needed).
							BlockMeans m;
							if ( ComputeBlockMeans( j.reals[ i ].pPixels, j.reals[ i ].uPitch, s->uWidth, s->uHeight, s->nVkFormat, kMetricStep, &m ) )
							{
								s->nBurstRealsChecked++;
								const RepeatDetector::Result rr = s->repeat.Add( j.ulRealId[ i ], m,
									RowHash( j.reals[ i ].pPixels, j.reals[ i ].uPitch, s->uWidth, s->uHeight, s->uBpp ) );
								if ( rr.eKind != RepeatDetector::kNone )
								{
									( rr.eKind == RepeatDetector::kRepeat ? s->nBurstRepeat : s->nBurstStatic )++;
									char sz[ 128 ];
									snprintf( sz, sizeof( sz ), "repeat kind=%s id=%llu same_as=%llu\n", rr.eKind == RepeatDetector::kRepeat ? "repeat" : "static",
										(unsigned long long)j.ulRealId[ i ], (unsigned long long)rr.ulSameAs );
									s->sBurstRepeatLines += sz;
								}
							}
						}
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
				char szRepeat[ 128 ];
				snprintf( szRepeat, sizeof( szRepeat ), "repeat_check reals=%d repeat=%d static=%d\n", s->nBurstRealsChecked, s->nBurstRepeat, s->nBurstStatic );
				if ( s->nBurstRepeat )
					cap_log.warnf( "capture: %d stored real frame(s) repeat an earlier one with a different frame between (see the repeat lines)", s->nBurstRepeat );
				const std::string sTiming = s->sBurstRepeatLines + szRepeat + FormatTimingLine( s->ulFirstFrameNs, s->ulLastFrameNs, s->ulMaxDecisionGapNs, s->ulGapAfterCounter,
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
		g_bWatchReq.store( false, std::memory_order_relaxed );
		g_ulDueNs.store( get_time_in_nanos() + uint64_t( nSeconds ) * 1000ull * 1000ull * 1000ull, std::memory_order_relaxed );
		g_uState.store( kArmed, std::memory_order_release );
		if ( psMessage )
			*psMessage = nSeconds > 0 ? "capture armed: it starts in " + std::to_string( nSeconds ) + " s" : "capture starting now";
		return true;
	}

	bool RequestWatch( int nSeconds, float flSensitivity, std::string *psMessage )
	{
		uint32_t uExpected = kIdle;
		if ( !g_uState.compare_exchange_strong( uExpected, kArming, std::memory_order_acquire, std::memory_order_relaxed ) )
		{
			if ( psMessage )
				*psMessage = uExpected == kRunning ? "a capture is running; refused"
					: uExpected == kWriting ? "a capture is still writing its files; refused" : "a capture is already armed; refused";
			return false;
		}
		g_bWatchReq.store( true, std::memory_order_relaxed );
		g_nWatchSeconds.store( nSeconds, std::memory_order_relaxed );
		g_flWatchSens.store( flSensitivity, std::memory_order_relaxed );
		g_ulDueNs.store( get_time_in_nanos(), std::memory_order_relaxed );
		g_uState.store( kArmed, std::memory_order_release );
		if ( psMessage )
		{
			char sz[ 160 ];
			snprintf( sz, sizeof( sz ), "watch starting now for %d s, sensitivity %g%s", nSeconds, double( flSensitivity ),
				flSensitivity > 0.0f ? "" : " (every measured frame triggers)" );
			*psMessage = sz;
		}
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
			else if ( s->bWatch && ulNowNs >= s->ulWatchEndNs )
				Finish( *s, "watch_done" );   // a watch ends on its time only (a paused game is no reason to stop)
			else if ( !s->bWatch && ulNowNs - s->ulLastProgressNs > kTimeoutNs )
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
				const bool bWatch = g_bWatchReq.load( std::memory_order_relaxed );
				const uint64_t ulNeed = bWatch ? WatchRequiredBytes( env.uWidth, env.uHeight, uBpp, g_nWatchSeconds.load( std::memory_order_relaxed ) )
					: RequiredBytes( env.uWidth, env.uHeight, uBpp, kFrames );
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
			const std::string sStamp = std::string( szStamp ) + ( g_bWatchReq.load( std::memory_order_relaxed ) ? "-watch" : "" );
			std::string sDir = sBase + "/" + sStamp;
			for ( int n = 1; mkdir( sDir.c_str(), 0755 ) != 0; n++ )
			{
				if ( errno != EEXIST || n > 99 )
					return Refuse( "could not create the capture directory" );
				sDir = sBase + "/" + sStamp + "-" + std::to_string( n );
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
		if ( g_bWatchReq.load( std::memory_order_relaxed ) )
		{
			// The watch keeps everything in memory; the writer writes watch.txt at the end and the dumps in between.
			s.bWatch = true;
			s.nWatchSeconds = g_nWatchSeconds.load( std::memory_order_relaxed );
			s.flSens = g_flWatchSens.load( std::memory_order_relaxed );
			s.ulWatchEndNs = ulNowNs + uint64_t( s.nWatchSeconds ) * 1000000000ull;
			s.header = h;
			s.sWatchHeader = FormatWatchHeader( h, s.nWatchSeconds, s.flSens );
			for ( const std::string &sLine : FormatSettingLines( *env.pSettings ) )
				s.sSettingsText += sLine + "\n";
			s.pWindow = std::make_unique<RollingWindow>( int( s.pOuts.size() ), int( s.pReals.size() ) );
			s.nRecCap = size_t( s.nWatchSeconds ) * kWatchRecordsPerSecond + 1000;
			s.recs.reserve( s.nRecCap );
			s.thread = std::thread( WriterMain, &s );
			g_uState.store( kRunning, std::memory_order_release );
			cap_log.infof( "watch started: %d s, sensitivity %g, %ux%u (format %s), into %s", s.nWatchSeconds, double( s.flSens ), env.uWidth,
				env.uHeight, VkFormatName( env.nVkFormat ), s.sDir.c_str() );
			return true;
		}
		s.thread = std::thread( WriterMain, &s );
		Enqueue( s, std::move( head ) );
		g_uState.store( kRunning, std::memory_order_release );
		cap_log.infof( "capture started: %d generated frames of %ux%u (format %s) into %s", kFrames, env.uWidth, env.uHeight,
			VkFormatName( env.nVkFormat ), s.sDir.c_str() );
		return true;
	}

	namespace
	{
		// The watch's per-record work (paint thread): the telemetry record with its flags, a host-flag trigger,
		// and -- unless a dump is being written -- the record's images into the rolling window plus a Measure job.
		void WatchRecord( Session &s, CVulkanCmdBuffer *pCmd, const FrameIn &in )
		{
			if ( s.recs.size() >= s.nRecCap )
			{
				s.ulDroppedRecs++;
				return;
			}
			WatchRec r;
			r.ulIndex = s.recs.size();
			r.ulNs = get_time_in_nanos();
			r.ulOutId = in.ulOutId;
			r.ulPrevId = in.ulPrevId;
			r.ulCurrId = in.ulCurrId;
			r.ulNewestId = in.ulNewestId;
			r.flT0 = in.flT0;
			r.flT1 = in.flT1;
			r.nArrivalPrevNs = Arrivals().Find( in.ulPrevId );
			r.nArrivalCurrNs = Arrivals().Find( in.ulCurrId );
			r.bEstimateRecorded = in.nEstimateRet >= 0;
			r.nEstimateRet = int8_t( in.nEstimateRet );
			r.nSynthRet = int8_t( in.nSynthRet );
			r.bCached = in.bCached;
			r.bNear = in.bNear;
			r.nOutSlot = int8_t( in.nOutSlot );
			r.nPrevSlot = int8_t( in.nPrevSlot );
			r.nCurrSlot = int8_t( in.nCurrSlot );
			FlagInput fl;
			fl.ulOutId = in.ulOutId;
			fl.ulPrevId = in.ulPrevId;
			fl.ulCurrId = in.ulCurrId;
			fl.bEstimateRecorded = r.bEstimateRecorded;
			fl.nEstimateRet = in.nEstimateRet;
			fl.nSynthRet = in.nSynthRet;
			fl.bNear = in.bNear;
			fl.bStaleHost = in.bStaleHost;
			fl.bMismatchHost = in.bMismatchHost;
			r.uFlags = EvalFlags( fl, &s.ulLastCurr, &s.ulLastOut );

			if ( r.uFlags & kDumpFlags )
			{
				s.ulTriggers++;
				std::lock_guard<std::mutex> lock( s.trigMutex );
				if ( !s.bTrig && !s.bFrozen && s.nDumps < kWatchMaxDumps )
				{
					s.bTrig = true;
					s.sTrigKind = ( r.uFlags & kFlagStale ) ? "stale" : ( r.uFlags & kFlagPairMismatch ) ? "pairmismatch" : "refused";
					s.ulTrigRec = r.ulIndex;
					s.flTrigCoarse = s.flTrigPair = s.flTrigThr = -1.0f;
					s.ulTrigId = s.ulTrigSameAs = 0;
				}
			}

			if ( s.bFrozen )
				s.ulNotMeasuredDumping++;
			else if ( pCmd && in.pOut )
			{
				// The images: the reals must be in the window already or still in the host's ring.
				const RollingWindow &w = *s.pWindow;
				auto InWindow = [ & ]( uint64_t ulId )
				{
					for ( int k = 0; k < int( s.pReals.size() ); k++ )
					{
						if ( w.RealId( k ) == ulId && ulId )
							return true;
					}
					return false;
				};
				const bool bReals = ( InWindow( in.ulPrevId ) || in.pPrev ) && ( InWindow( in.ulCurrId ) || in.pCurr );
				if ( !bReals )
					s.nDropped++;
				else
				{
					uint64_t ulTrigRec = ~0ull;
					{
						std::lock_guard<std::mutex> lock( s.trigMutex );
						if ( s.bTrig )
							ulTrigRec = s.ulTrigRec;
					}
					const uint64_t ulAnalysed = s.ulAnalysedEnd.load( std::memory_order_acquire );
					const RollingWindow::AddResult a = s.pWindow->Add( r.ulIndex, in.ulPrevId, in.ulCurrId,
						[ & ]( uint64_t ulRec ) { return ulRec < ulAnalysed && ulRec != ulTrigRec; } );
					if ( !a.ok )
					{
						s.nDropped++;
						if ( a.bBlocked && ulTrigRec != ~0ull )
							s.bBlockedNow = true;
					}
					else
					{
						pCmd->copyImage( in.pOut, s.pOuts[ size_t( a.nOut ) ] );
						const gamescope::Rc<CVulkanTexture> *pSrc[ 2 ] = { &in.pPrev, &in.pCurr };
						for ( int i = 0; i < 2; i++ )
						{
							if ( a.bRealNew[ i ] )
								pCmd->copyImage( *pSrc[ i ], s.pReals[ size_t( a.nReal[ i ] ) ] );
						}
						Job j;
						j.eKind = Job::Measure;
						j.ulRec = r.ulIndex;
						j.flT = 0.5f * ( in.flT0 + in.flT1 );
						j.out = { std::string(), s.pOuts[ size_t( a.nOut ) ]->mappedData(), s.pOuts[ size_t( a.nOut ) ]->rowPitch() };
						for ( int i = 0; i < 2; i++ )
						{
							j.ulRealId[ i ] = i == 0 ? in.ulPrevId : in.ulCurrId;
							j.reals[ i ] = { std::string(), s.pReals[ size_t( a.nReal[ i ] ) ]->mappedData(), s.pReals[ size_t( a.nReal[ i ] ) ]->rowPitch() };
						}
						s.pending.push_back( std::move( j ) );
					}
				}
			}
			s.recs.push_back( r );
		}

		// After this composite's jobs are queued (paint thread): resume after a written dump, or freeze the
		// window and queue a dump once the trigger has kWatchAfterTrigger records after it (or blocks the window).
		void WatchAfterSubmit( Session &s, uint64_t ulSeq )
		{
			if ( s.bFrozen )
			{
				if ( !s.bDumpDone.load( std::memory_order_acquire ) )
					return;
				s.bFrozen = false;
				s.bDumpDone.store( false, std::memory_order_relaxed );
				s.pWindow->Clear();
				// Everything older was analysed or never will be: the cleared window holds nothing.
				std::lock_guard<std::mutex> lock( s.trigMutex );
				s.bTrig = false;
				return;
			}
			std::string sKind;
			uint64_t ulTrigRec = 0;
			float flC = -1.0f, flP = -1.0f, flThr = -1.0f;
			uint64_t ulTrigId = 0, ulTrigSameAs = 0;
			{
				std::lock_guard<std::mutex> lock( s.trigMutex );
				if ( !s.bTrig )
					return;
				if ( s.nDumps >= kWatchMaxDumps )
				{
					s.bTrig = false;
					return;
				}
				sKind = s.sTrigKind;
				ulTrigRec = s.ulTrigRec;
				flC = s.flTrigCoarse;
				flP = s.flTrigPair;
				flThr = s.flTrigThr;
				ulTrigId = s.ulTrigId;
				ulTrigSameAs = s.ulTrigSameAs;
			}
			const std::vector<RollingWindow::Entry> &e = s.pWindow->Entries();
			int nAfter = 0;
			for ( const RollingWindow::Entry &x : e )
				nAfter += x.ulRec > ulTrigRec;
			const bool bLast = g_uState.load( std::memory_order_relaxed ) != kRunning;   // the watch is ending: dump what there is
			if ( nAfter < kWatchAfterTrigger && !s.bBlockedNow && !bLast )
				return;
			s.bBlockedNow = false;
			if ( s.bEndQueued || e.empty() )
				return;

			// Freeze: build the dump from the window (its records' copies all ride in submissions <= ulSeq).
			s.bFrozen = true;
			s.nDumps++;
			Job d;
			d.eKind = Job::Dump;
			d.ulSeq = ulSeq;
			d.nDump = s.nDumps;
			HeaderInfo h = s.header;
			h.nFrames = int( e.size() );
			d.sText = FormatHeaderLine( h ) + "\n" + s.sSettingsText;
			const std::vector<DecisionRec> decisions = Decisions().Snapshot( kDecisionsDumped );
			for ( const DecisionRec &r : decisions )
				d.sText += FormatDecisionLine( r ) + "\n";
			int nTrigSeq = -1;
			std::map<uint64_t, int> realsDone;
			for ( size_t k = 0; k < e.size(); k++ )
			{
				const WatchRec &r = s.recs[ size_t( e[ k ].ulRec ) ];
				if ( e[ k ].ulRec == ulTrigRec )
					nTrigSeq = int( k );
				FrameInfo f;
				f.nSeq = int( k );
				f.ulOutId = r.ulOutId;
				f.ulPrevId = r.ulPrevId;
				f.ulCurrId = r.ulCurrId;
				f.ulNewestId = r.ulNewestId;
				f.flT0 = r.flT0;
				f.flT1 = r.flT1;
				f.nArrivalPrevNs = r.nArrivalPrevNs;
				f.nArrivalCurrNs = r.nArrivalCurrNs;
				f.ulNowNs = r.ulNs;
				f.uWidth = s.uWidth;
				f.uHeight = s.uHeight;
				f.bEstimateRecorded = r.bEstimateRecorded;
				f.nEstimateRet = r.nEstimateRet;
				f.nSynthRet = r.nSynthRet;
				f.bCached = r.bCached;
				f.bNear = r.bNear;
				f.nOutSlot = r.nOutSlot;
				f.nPrevSlot = r.nPrevSlot;
				f.nCurrSlot = r.nCurrSlot;
				f.sOutFile = OutFileName( int( k ) );
				f.sPrevFile = RealFileName( r.ulPrevId );
				f.sCurrFile = RealFileName( r.ulCurrId );
				Job::DumpFrame df;
				df.sLine = FormatFrameLine( f );
				df.out = { f.sOutFile, s.pOuts[ size_t( e[ k ].nOut ) ]->mappedData(), s.pOuts[ size_t( e[ k ].nOut ) ]->rowPitch() };
				d.dumpFrames.push_back( std::move( df ) );
				for ( int i = 0; i < 2; i++ )
				{
					if ( realsDone.count( e[ k ].ulId[ i ] ) )
						continue;
					realsDone[ e[ k ].ulId[ i ] ] = 1;
					const size_t nR = size_t( e[ k ].nReal[ i ] );
					d.dumpReals.push_back( { RealFileName( e[ k ].ulId[ i ] ), s.pReals[ nR ]->mappedData(), s.pReals[ nR ]->rowPitch() } );
				}
			}
			d.sText += FormatTriggerLine( sKind.c_str(), ulTrigRec, nTrigSeq, flC, flP, flThr, ulTrigId, ulTrigSameAs ) + "\n";
			uint64_t ulMaxGap = 0, ulGapAfter = 0;
			ulMaxGap = MaxDecisionGapNs( decisions, &ulGapAfter );
			const uint64_t ulFirst = s.recs[ size_t( e.front().ulRec ) ].ulNs, ulLast = s.recs[ size_t( e.back().ulRec ) ].ulNs;
			d.sTail = FormatTimingLine( ulFirst, ulLast, ulMaxGap, ulGapAfter, 0 ) + "\n"
				+ FormatEndLine( int( e.size() ), int( d.dumpReals.size() ), 0, ( "trigger_" + sKind ).c_str() ) + "\n";
			cap_log.infof( "watch: %s trigger at record %llu; writing dump %d (%zu frames)", sKind.c_str(), (unsigned long long)ulTrigRec,
				d.nDump, e.size() );
			Enqueue( s, std::move( d ) );
		}
	}

	void RecordFrame( CVulkanCmdBuffer *pCmd, const FrameIn &in )
	{
		Session *s = g_pSession;
		if ( !s || g_uState.load( std::memory_order_relaxed ) != kRunning )
			return;
		if ( s->bWatch )
		{
			WatchRecord( *s, pCmd, in );
			return;
		}
		if ( !pCmd || !in.pOut )
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
			j.ulRealId[ j.nReals ] = need[ i ].ulId;
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
		f.bNear = in.bNear;
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
		if ( !s )
			return;
		for ( Job &j : s->pending )
		{
			j.ulSeq = ulSeq;
			Enqueue( *s, std::move( j ) );
		}
		s->pending.clear();
		if ( s->bWatch && s->pWindow && !s->bEndQueued )
			WatchAfterSubmit( *s, ulSeq );
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
	"ritz_fg_capture starts now; ritz_fg_capture <seconds> (0..600) starts that many seconds later. "
	"ritz_fg_capture watch [seconds 1..600, default 60] [sensitivity, default 1; 0 = every frame triggers] watches every generated "
	"frame for that long, writes watch.txt and saves up to 3 dumps around a faulty-looking frame by itself.",
	[]( std::span<std::string_view> args )
	{
		const fghost::capture::ArgResult r = fghost::capture::ParseArgs( args );
		if ( !r.ok )
		{
			console_log.errorf( "%s", r.error.c_str() );
			return;
		}
		std::string sMessage;
		if ( !( r.watch ? fghost::capture::RequestWatch( r.seconds, r.sensitivity, &sMessage ) : fghost::capture::Request( r.seconds, &sMessage ) ) )
		{
			console_log.errorf( "ritz_fg_capture: %s", sMessage.c_str() );
			return;
		}
		console_log.infof( "ritz_fg_capture: %s (the files are written under the cache directory, the path is logged when it starts)", sMessage.c_str() );
		if ( !fghost::Active() )
			console_log.warnf( "ritz_fg_capture: frame generation is off; the capture waits up to %d s after its start time for it, then gives up",
				int( fghost::capture::kTimeoutNs / 1000000000ull ) );
	} );
