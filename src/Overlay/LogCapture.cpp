// See LogCapture.h. No ImGui here -- PanelLog.cpp owns the drawing.
#include "LogCapture.h"

#include <atomic>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

static LogScope s_LogCaptureLog( "logcapture" );

namespace gamescope::LogCapture
{
	namespace
	{
		// ---- Bounded ring buffer --------------------------------------
		// 4000 lines per tab: generous enough to cover a full boot +
		// play-session's worth of scrollback (gamescope's own startup log
		// alone is a few hundred lines; a chatty game can produce a lot
		// more) while staying trivially bounded in memory -- at a very
		// pessimistic ~200 bytes/line that's ~800KB per tab, nowhere near
		// worth trading off against "can scroll back far enough to be
		// useful." Same "fixed cap, not an ever-growing container" shape
		// Notifications.cpp's kMaxQueued/kMaxVisible already use for
		// exactly this class of problem (an unbounded stream that must not
		// become unbounded memory).
		constexpr size_t kMaxLines = 4000;

		// ONE sequence for BOTH rings. The panel merges the gamescope and
		// game buffers into a single view, so a per-ring counter would hand
		// two different lines the same number. Shared and atomic, it is
		// unique across the merged view and orders the two streams against
		// each other even when their timestamps land in the same millisecond.
		std::atomic<uint64_t> s_ulNextSeq { 1 };

		uint64_t NowMs()
		{
			struct timespec ts {};
			clock_gettime( CLOCK_REALTIME, &ts );
			return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)( ts.tv_nsec / 1000000 );
		}

		class RingBuffer
		{
		public:
			void Push( LogPriority ePriority, std::string_view svScope, std::string_view svText )
			{
				// Stamped BEFORE taking the lock: the clock read and the
				// sequence bump do not need the ring's mutex, and keeping
				// them outside it shortens the critical section on a path
				// every log() call in the process goes through.
				const uint64_t ulSeq = s_ulNextSeq.fetch_add( 1, std::memory_order_relaxed );
				const uint64_t ulNow = NowMs();

				std::lock_guard<std::mutex> lock( m_Mutex );
				m_Lines.push_back( Line{ ePriority, std::string( svScope ), std::string( svText ),
					ulSeq, ulNow } );
				if ( m_Lines.size() > kMaxLines )
					m_Lines.pop_front();
				m_ulGeneration++;
			}

			// Splits on '\n' so every buffered Line is exactly one visual
			// row -- log() calls can embed multiple lines in one call
			// (e.g. convar.cpp's "%.*s: %.*s\n%.*s"), and raw child output
			// arrives as an arbitrary byte stream with no framing at all.
			void PushSplitLines( LogPriority ePriority, std::string_view svScope, std::string_view svText )
			{
				size_t nStart = 0;
				while ( nStart <= svText.size() )
				{
					size_t nEnd = svText.find( '\n', nStart );
					if ( nEnd == std::string_view::npos )
					{
						if ( nStart < svText.size() )
							Push( ePriority, svScope, svText.substr( nStart ) );
						break;
					}
					Push( ePriority, svScope, svText.substr( nStart, nEnd - nStart ) );
					nStart = nEnd + 1;
				}
			}

			Snapshot GetSnapshot()
			{
				std::lock_guard<std::mutex> lock( m_Mutex );
				Snapshot snap;
				snap.vecLines.assign( m_Lines.begin(), m_Lines.end() );
				snap.ulGeneration = m_ulGeneration;
				return snap;
			}

			uint64_t Generation()
			{
				std::lock_guard<std::mutex> lock( m_Mutex );
				return m_ulGeneration;
			}

		private:
			std::mutex m_Mutex;
			std::deque<Line> m_Lines;
			uint64_t m_ulGeneration = 0;
		};

		RingBuffer &GamescopeRing()
		{
			static RingBuffer s_Ring;
			return s_Ring;
		}

		RingBuffer &GameRing()
		{
			static RingBuffer s_Ring;
			return s_Ring;
		}

		bool s_bGameCaptureActive = false;

		// Blocking-read loop for one stream (stdout or stderr) of the
		// launched game. Lives on its own thread for the lifetime of the
		// pipe (until the write end -- the child's dup2'd fd 1/2 -- closes,
		// i.e. the game exited) so nothing here ever runs on, or can stall,
		// the render thread. This is also what keeps the child from
		// blocking on a full pipe: as long as this thread is alive and not
		// stuck elsewhere, it's continuously draining, so the 64KB pipe
		// buffer a blocking write() in the child could fill never does.
		// Every read is immediately both buffered AND written back out to
		// gamescope's own stdout/stderr (never touched by the child's
		// dup2 -- only the forked child's *own* fd table was ever
		// redirected) so existing "pipe gamescope's own output" users keep
		// seeing the game's output too, just relayed rather than inherited
		// directly.
		void GameReaderThreadMain( int nReadFd, int nPassthroughFd, LogPriority ePriority, const char *pszThreadName )
		{
			pthread_setname_np( pthread_self(), pszThreadName );

			std::string sPending; // bytes read since the last '\n' -- carried across read() calls
			char buf[ 4096 ];
			for ( ;; )
			{
				ssize_t sszRead = read( nReadFd, buf, sizeof( buf ) );
				if ( sszRead <= 0 )
					break; // 0 == EOF (game exited and closed its fd), <0 == pipe gone

				// Passthrough first, verbatim -- best-effort, a stalled
				// terminal on the far end must never back up into skipping
				// buffering (which is what actually feeds the panel).
				ssize_t sszOff = 0;
				while ( sszOff < sszRead )
				{
					ssize_t sszWritten = write( nPassthroughFd, buf + sszOff, sszRead - sszOff );
					if ( sszWritten <= 0 )
						break;
					sszOff += sszWritten;
				}

				sPending.append( buf, (size_t)sszRead );

				size_t nStart = 0;
				for ( ;; )
				{
					size_t nEnd = sPending.find( '\n', nStart );
					if ( nEnd == std::string::npos )
						break;
					GameRing().Push( ePriority, "", std::string_view( sPending ).substr( nStart, nEnd - nStart ) );
					nStart = nEnd + 1;
				}
				sPending.erase( 0, nStart );
			}

			if ( !sPending.empty() )
				GameRing().Push( ePriority, "", sPending );

			close( nReadFd );
		}
	}

	void InitGamescopeCapture()
	{
		static bool s_bInitialized = false;
		if ( s_bInitialized )
			return;
		s_bInitialized = true;

		LogScope::AddGlobalLoggingListener(
			[]( LogPriority ePriority, std::string_view svScope, std::string_view svText )
			{
				GamescopeRing().PushSplitLines( ePriority, svScope, svText );
			} );
	}

	Snapshot GetGamescopeLog()
	{
		return GamescopeRing().GetSnapshot();
	}

	uint64_t GetGamescopeLogGeneration()
	{
		return GamescopeRing().Generation();
	}

	GameCaptureHandles StartGameCapture()
	{
		GameCaptureHandles handles;

		int nStdoutFds[2] = { -1, -1 };
		int nStderrFds[2] = { -1, -1 };
		if ( pipe2( nStdoutFds, O_CLOEXEC ) != 0 )
		{
			s_LogCaptureLog.errorf_errno( "Failed to create stdout capture pipe" );
			return handles;
		}
		if ( pipe2( nStderrFds, O_CLOEXEC ) != 0 )
		{
			s_LogCaptureLog.errorf_errno( "Failed to create stderr capture pipe" );
			close( nStdoutFds[0] );
			close( nStdoutFds[1] );
			return handles;
		}

		const int nStdoutRead = nStdoutFds[0], nStdoutWrite = nStdoutFds[1];
		const int nStderrRead = nStderrFds[0], nStderrWrite = nStderrFds[1];

		// Start draining *before* the child exists -- see
		// GameReaderThreadMain's comment on why this is what actually
		// makes "the child can't block on a full pipe" true rather than
		// just usually true.
		std::thread( GameReaderThreadMain, nStdoutRead, STDOUT_FILENO, LOG_INFO, "gamescope-logcap-o" ).detach();
		std::thread( GameReaderThreadMain, nStderrRead, STDERR_FILENO, LOG_ERROR, "gamescope-logcap-e" ).detach();

		handles.bOk = true;
		handles.vecExtraKeepFds = { nStdoutWrite, nStderrWrite };
		handles.fnPreambleInChild = [ nStdoutWrite, nStderrWrite ]()
		{
			dup2( nStdoutWrite, STDOUT_FILENO );
			dup2( nStderrWrite, STDERR_FILENO );
			close( nStdoutWrite );
			close( nStderrWrite );
		};
		handles.fnCloseParentWriteEnds = [ nStdoutWrite, nStderrWrite ]()
		{
			close( nStdoutWrite );
			close( nStderrWrite );
		};

		s_bGameCaptureActive = true;
		return handles;
	}

	Snapshot GetGameLog()
	{
		return GameRing().GetSnapshot();
	}

	uint64_t GetGameLogGeneration()
	{
		return GameRing().Generation();
	}

	bool IsGameCaptureActive()
	{
		return s_bGameCaptureActive;
	}
}
