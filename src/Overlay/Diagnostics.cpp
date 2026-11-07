// Diagnostics runtime -- see Diagnostics.h and superdoc/features/diagnostics.md.
//
// THREADS. The compositor paints at 280 Hz, so nothing here may block on a
// child process on the paint thread. A check runs on its own short-lived worker
// thread; the result is published under g_Mutex as one struct, and everything
// the UI reads is a copy of it (Diagnostics_View()). Notifications::Show() is
// documented as steamcompmgr-thread-only, so the worker never toasts: it sets a
// flag and Diagnostics_Tick(), on the paint thread, raises the toast.
#include "Diagnostics.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Config/ConfigManager.h"
#include "Notifications.h"
#include "backend.h"
#include "log.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "Utils/Process.h"

extern char **environ;

static LogScope diag_log( "diagnostics" );

namespace gamescope
{
	namespace
	{
		using namespace diagnostics;

		// ---- the launch environment ---------------------------------------
		std::vector<std::string> g_vecHostEnv;
		bool                     g_bHostEnvCaptured = false;

		const char *HostGetenv( const char *pszName )
		{
			const size_t n = strlen( pszName );
			for ( const std::string &s : g_vecHostEnv )
				if ( s.size() > n && s.compare( 0, n, pszName ) == 0 && s[ n ] == '=' )
					return s.c_str() + n + 1;
			return nullptr;
		}

		// What gamescope sets for the game it hosts, or what only makes sense
		// inside it. A browser started with these would load our WSI layer,
		// connect to gamescope's own nested session, or pick the Steam runtime's
		// libraries. `Why a list and not "start empty":` the browser still needs
		// the session's HOME, XDG_*, DBUS_SESSION_BUS_ADDRESS, PATH...
		bool DropForBrowser( const std::string &s )
		{
			static const char *kPrefixes[] = {
				"LD_PRELOAD=", "LD_LIBRARY_PATH=", "ENABLE_GAMESCOPE", "GAMESCOPE_",
				"SDL_VIDEODRIVER=", "SDL_VIDEO_DRIVER=", "SRT_URLOPEN_PREFER_STEAM=", "STEAM_GAMESCOPE_",
				"STEAM_MANGOAPP_", "STEAM_USE_MANGOAPP=", "PRESSURE_VESSEL", "vk_",
				"XDG_CURRENT_DESKTOP=gamescope",
			};
			for ( const char *p : kPrefixes )
				if ( s.compare( 0, strlen( p ), p ) == 0 )
					return true;
			return false;
		}

		std::vector<char *> MakeEnvp( std::vector<std::string> &vecStorage, bool bForBrowser )
		{
			vecStorage.clear();
			for ( const std::string &s : g_vecHostEnv )
				if ( !bForBrowser || !DropForBrowser( s ) )
					vecStorage.push_back( s );
			std::vector<char *> envp;
			for ( std::string &s : vecStorage )
				envp.push_back( s.data() );
			envp.push_back( nullptr );
			return envp;
		}

		// ---- spawning ------------------------------------------------------
		// posix_spawnp with a fixed argv: no shell, stdin from /dev/null, every
		// fd above stderr closed, signals back to default, its own session.
		// pStdoutFd != nullptr captures stdout into a pipe (read end returned).
		pid_t Spawn( char *const argv[], std::vector<char *> &envp, int *pStdoutFd )
		{
			int nPipe[ 2 ] = { -1, -1 };
			if ( pStdoutFd && pipe2( nPipe, O_CLOEXEC ) != 0 )
				return -1;

			posix_spawn_file_actions_t fa;
			posix_spawn_file_actions_init( &fa );
			posix_spawn_file_actions_addopen( &fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0 );
			if ( pStdoutFd )
				posix_spawn_file_actions_adddup2( &fa, nPipe[ 1 ], STDOUT_FILENO );
			else
				posix_spawn_file_actions_addopen( &fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0 );
			posix_spawn_file_actions_addopen( &fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0 );
			posix_spawn_file_actions_addclosefrom_np( &fa, 3 );

			posix_spawnattr_t at;
			posix_spawnattr_init( &at );
			sigset_t none, all;
			sigemptyset( &none );
			sigfillset( &all );
			posix_spawnattr_setsigmask( &at, &none );
			posix_spawnattr_setsigdefault( &at, &all );
			posix_spawnattr_setflags( &at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID );

			pid_t nPid = -1;
			const int nErr = posix_spawnp( &nPid, argv[ 0 ], &fa, &at, argv, envp.data() );
			posix_spawn_file_actions_destroy( &fa );
			posix_spawnattr_destroy( &at );

			if ( pStdoutFd )
			{
				close( nPipe[ 1 ] );
				if ( nErr != 0 )
					close( nPipe[ 0 ] );
				else
					*pStdoutFd = nPipe[ 0 ];
			}
			return nErr == 0 ? nPid : -1;
		}

		uint64_t NowMs()
		{
			return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch() ).count();
		}

		// Runs argv, captures up to 4 KiB of stdout, kills it after nTimeoutMs.
		RunResult RunCapture( char *const argv[], int nTimeoutMs )
		{
			RunResult r;
			if ( !Process::ExecutableExists( argv[ 0 ] ) )
			{
				r.eOutcome = RunOutcome::NotInstalled;
				return r;
			}

			std::vector<std::string> vecStorage;
			std::vector<char *> envp = MakeEnvp( vecStorage, /* bForBrowser */ false );
			int nFd = -1;
			const pid_t nPid = Spawn( argv, envp, &nFd );
			if ( nPid < 0 )
			{
				r.eOutcome = errno == ENOENT ? RunOutcome::NotInstalled : RunOutcome::Failed;
				r.nExit = -1;
				return r;
			}

			const uint64_t ulDeadline = NowMs() + (uint64_t)nTimeoutMs;
			bool bTimedOut = false;
			bool bEof = false;
			while ( !bEof )
			{
				const uint64_t ulNow = NowMs();
				if ( ulNow >= ulDeadline ) { bTimedOut = true; break; }
				pollfd pfd = { nFd, POLLIN, 0 };
				const int nPoll = poll( &pfd, 1, (int)( ulDeadline - ulNow ) );
				if ( nPoll < 0 && errno == EINTR )
					continue;
				if ( nPoll <= 0 ) { bTimedOut = nPoll == 0; break; }
				char sz[ 512 ];
				const ssize_t n = read( nFd, sz, sizeof( sz ) );
				if ( n > 0 ) { if ( r.sOut.size() < 4096 ) r.sOut.append( sz, (size_t)n ); }
				else if ( n == 0 ) bEof = true;
				else if ( errno != EINTR ) break;
			}
			close( nFd );

			// stdout closed: it should be exiting. Give it the rest of the budget.
			int nStatus = 0;
			bool bReaped = false;
			while ( !bTimedOut )
			{
				const pid_t nW = waitpid( nPid, &nStatus, WNOHANG );
				if ( nW == nPid ) { bReaped = true; break; }
				if ( nW < 0 && errno != EINTR ) { bReaped = true; nStatus = 0; break; }   // reaped elsewhere
				if ( NowMs() >= ulDeadline ) { bTimedOut = true; break; }
				usleep( 2000 );
			}
			if ( bTimedOut && !bReaped )
			{
				kill( nPid, SIGKILL );
				waitpid( nPid, &nStatus, 0 );
				r.eOutcome = RunOutcome::TimedOut;
				return r;
			}

			r.nExit = WIFEXITED( nStatus ) ? WEXITSTATUS( nStatus ) : -1;
			r.eOutcome = r.nExit == 0 ? RunOutcome::Ok : RunOutcome::Failed;
			return r;
		}

		// ---- the checks ------------------------------------------------------
		struct Published
		{
			bool          bRunning = false;
			bool          bEverChecked = false;
			ScanoutResult scanout;
			bool          bToastPending = false;   // set by a startup run that found Off
		};
		std::mutex g_Mutex;
		Published  g_Pub;
		bool       g_bStartupStarted = false;
		bool       g_bToasted = false;

		// Test hook, documented in diagnostics.md: lets a headless instance run
		// the whole path against a fake hyprctl, since the real condition (nested
		// Wayland under Hyprland) cannot be had without a host compositor.
		bool BackendApplicable()
		{
			if ( const char *p = getenv( "GS_RITZ_DIAGNOSTICS_ASSUME_WAYLAND" ); p && *p == '1' )
				return true;
			return GetBackend() && GetBackend()->HostMayScanOutOutputImages();
		}

		ScanoutResult CheckHyprlandScanout( const Facts &f )
		{
			RunResult run;
			if ( f.bBackendApplicable && f.bHyprland )
			{
				char *const argv[] = { (char *)"hyprctl", (char *)"-j", (char *)"getoption",
				                       (char *)"render:direct_scanout", nullptr };
				run = RunCapture( argv, 1000 );
			}
			return DecideDirectScanout( f, run );
		}

		// A plain array: each check turns the facts into one result slot.
		using CheckFn = ScanoutResult (*)( const Facts & );
		constexpr CheckFn kChecks[] = { CheckHyprlandScanout };

		void StartRun( bool bStartup )
		{
			Facts f;
			f.bBackendApplicable = BackendApplicable();
			if ( GetBackend() && GetBackend()->GetCurrentConnector() )
				f.sBackend = GetBackend()->GetCurrentConnector()->GetName();   // "SDLWindow", "Wayland", a DRM connector...
			f.bHyprland = HostGetenv( "HYPRLAND_INSTANCE_SIGNATURE" ) != nullptr &&
			              *HostGetenv( "HYPRLAND_INSTANCE_SIGNATURE" ) != '\0';

			{
				std::lock_guard<std::mutex> l( g_Mutex );
				if ( g_Pub.bRunning )
					return;
				g_Pub.bRunning = true;
			}

			std::thread( [ f, bStartup ]
			{
				ScanoutResult res;
				for ( CheckFn fn : kChecks )
					res = fn( f );

				std::lock_guard<std::mutex> l( g_Mutex );
				g_Pub.scanout = res;
				g_Pub.bEverChecked = true;
				g_Pub.bRunning = false;
				if ( bStartup && WarnsAbout( res.eState ) )
					g_Pub.bToastPending = true;
				diag_log.infof( "direct scanout check: %s%s%s", res.sHeadline.c_str(),
					res.sReason.empty() ? "" : " -- ", res.sReason.c_str() );
				// An idle screen paints nothing, so neither the toast nor an open
				// Diagnostics area would see the result until something else did.
				force_repaint();
			} ).detach();
		}

		// Browser launches still running; reaped with WNOHANG on later ticks.
		// gamescope is a subreaper, so a finished one stays a zombie otherwise.
		// Guarded: opened from the console thread (overlay_e2_set) or the paint thread.
		std::mutex         g_BrowserMutex;
		std::vector<pid_t> g_vecBrowserPids;
	}

	void Diagnostics_CaptureHostEnv()
	{
		if ( g_bHostEnvCaptured )
			return;
		g_bHostEnvCaptured = true;
		for ( char **e = environ; e && *e; ++e )
			g_vecHostEnv.emplace_back( *e );
	}

	void Diagnostics_Recheck()
	{
		StartRun( /* bStartup */ false );
	}

	void Diagnostics_Tick()
	{
		{
			std::lock_guard<std::mutex> l( g_BrowserMutex );
			for ( size_t i = 0; i < g_vecBrowserPids.size(); )
			{
				const pid_t n = waitpid( g_vecBrowserPids[ i ], nullptr, WNOHANG );
				if ( n != 0 )
					g_vecBrowserPids.erase( g_vecBrowserPids.begin() + (ptrdiff_t)i );
				else
					++i;
			}
		}

		if ( !g_bStartupStarted )
		{
			if ( g_nOutputWidth == 0 || g_nOutputHeight == 0 || !GetBackend() )
				return;
			g_bStartupStarted = true;
			StartRun( /* bStartup */ true );
			return;
		}

		if ( g_bToasted )
			return;
		bool bToast = false;
		{
			std::lock_guard<std::mutex> l( g_Mutex );
			bToast = g_Pub.bToastPending;
			g_Pub.bToastPending = false;
		}
		if ( bToast )
		{
			g_bToasted = true;
			// Blocked by gamescope: "scanout is off, turn it on" would be wrong.
			if ( Diagnostics_Silenced() || Diagnostics_BlockScanout() )
				return;
			Notifications::Show(
				"Hyprland has direct scanout off, so frames gamescope makes are lost. "
				"Set render:direct_scanout to 1 or 2. Details: Diagnostics.",
				Notifications::Kind::Warning, 12.0f );
		}
	}

	bool Diagnostics_OpenDocs()
	{
		if ( !Process::ExecutableExists( "xdg-open" ) )
			return false;
		std::vector<std::string> vecStorage;
		std::vector<char *> envp = MakeEnvp( vecStorage, /* bForBrowser */ true );
		char *const argv[] = { (char *)"xdg-open", (char *)kHyprlandRenderDocsUrl, nullptr };
		const pid_t nPid = Spawn( argv, envp, nullptr );
		if ( nPid < 0 )
			return false;
		std::lock_guard<std::mutex> l( g_BrowserMutex );
		g_vecBrowserPids.push_back( nPid );
		return true;
	}

	bool Diagnostics_HasWarning()
	{
		std::lock_guard<std::mutex> l( g_Mutex );
		const ScanoutResult r = WithBlock( g_Pub.scanout, Diagnostics_BlockScanout() );
		return AnyWarns( &r, &r + 1, Diagnostics_Silenced() );
	}

	namespace
	{
		std::atomic<int> g_nSilenced{ -1 };   // -1: not read from global.json yet
	}

	bool Diagnostics_Silenced()
	{
		int n = g_nSilenced.load();
		if ( n < 0 )
		{
			n = config::LoadGlobal().overlay.diagnostics_silence_warning ? 1 : 0;
			g_nSilenced.store( n );
		}
		return n == 1;
	}

	void Diagnostics_SetSilenced( bool bSilenced )
	{
		g_nSilenced.store( bSilenced ? 1 : 0 );
		config::Settings s = config::LoadGlobal();
		s.overlay.diagnostics_silence_warning = bSilenced;
		config::EnqueueGlobalWrite( s );
		force_repaint();
	}

	namespace
	{
		std::atomic<int> g_nBlockScanout{ -1 };   // -1: not read from global.json yet
	}

	// Read on the Wayland present path every frame: the atomic only. The first call
	// reads global.json once (the same lazy read Diagnostics_Silenced() does).
	bool Diagnostics_BlockScanout()
	{
		int n = g_nBlockScanout.load( std::memory_order_relaxed );
		if ( n < 0 )
		{
			n = config::LoadGlobal().overlay.diagnostics_block_scanout ? 1 : 0;
			g_nBlockScanout.store( n, std::memory_order_relaxed );
		}
		return n == 1;
	}

	void Diagnostics_SetBlockScanout( bool bBlock )
	{
		g_nBlockScanout.store( bBlock ? 1 : 0, std::memory_order_relaxed );
		config::Settings s = config::LoadGlobal();
		s.overlay.diagnostics_block_scanout = bBlock;
		config::EnqueueGlobalWrite( s );
		// An idle screen presents nothing: repaint so the plane appears or goes now.
		force_repaint();
	}

	bool Diagnostics_ScanoutApplicable()
	{
		return BackendApplicable();
	}

	DiagnosticsView Diagnostics_View()
	{
		std::lock_guard<std::mutex> l( g_Mutex );
		DiagnosticsView v;
		v.bRunning = g_Pub.bRunning;
		v.bEverChecked = g_Pub.bEverChecked;
		v.scanout = WithBlock( g_Pub.scanout, Diagnostics_BlockScanout() );
		return v;
	}
}
