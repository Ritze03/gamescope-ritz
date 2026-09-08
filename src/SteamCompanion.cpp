// The Steam companion overlay's runtime -- see SteamCompanion.h for what this
// is, SteamCompanionCmd.h for the rules it obeys, and
// superdoc/features/steam-companion.md for the user-facing feature (including
// the honest list of what the web chat cannot do).

#include "SteamCompanion.h"
#include "SteamCompanionCmd.h"

#include "Config/ConfigManager.h"
#include "Keybinds.h"
#include "Overlay/Notifications.h"
#include "Utils/Process.h"
#include "log.hpp"
#include "steamcompmgr.hpp"
#include "steamcompmgr_shared.hpp"
#include "wlserver.hpp"
#include "xwayland_ctx.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static LogScope companion_log( "companion" );

namespace gamescope::companion
{
	namespace
	{
		// ---------------------------------------------------------------------
		//  State -- steamcompmgr thread only, except g_nPendingPresses
		// ---------------------------------------------------------------------
		struct State
		{
			pid_t  nPid    = -1;      // the browser process; -1 == nothing running
			pid_t  nPgid   = -1;      // its process group -- see Spawn()
			Window uWindow = None;    // its toplevel, once found on our Xwayland

			bool bWantShown = false;  // what the user last asked for
			bool bApplied   = false;  // the properties on uWindow match bWantShown

			// The trap watchdog (SteamCompanionCmd.h's PropsFor comment):
			// consecutive ticks seen hidden-but-still-holding-input-focus.
			int  nTrapStrikes = 0;
			bool bTrapReported = false;
		};

		State g_State;

		// The hotkey path's only contact with this file.
		std::atomic<int> g_nPendingPresses{ 0 };

		// One press cannot be worth more than a handful of state changes, and
		// a stuck key must not make the compositor fork in a loop.
		constexpr int kMaxPressesPerTick = 4;

		// ---------------------------------------------------------------------
		//  Settings
		// ---------------------------------------------------------------------
		// Read on a press, never per frame: config::LoadGlobal() is file I/O
		// (ConfigManager.h's own warning), and a press happens at human speed.
		struct Options
		{
			bool        bEnabled;
			std::string sCommand;
			std::string sUrl;
		};

		Options ReadOptions()
		{
			const config::OverlaySettings o = config::LoadGlobal().overlay;
			return Options{ o.companion_enabled, o.companion_command, o.companion_url };
		}

		// The browser's private profile directory, substituted for {profile}.
		//
		// `Why it is not optional:` without an isolated profile, launching
		// chromium while the user's own chromium is running on the HOST just
		// opens a tab in that existing browser, on the host's display -- the
		// second process forwards the URL over the first one's singleton
		// socket and exits. Nothing would ever appear inside gamescope, and
		// the failure would look like "the hotkey does nothing". The same
		// isolation is also what lets the one-time Steam login be remembered
		// without touching the user's day-to-day browser profile.
		std::string ProfileDir()
		{
			return config::ConfigRoot() + "/companion-browser";
		}

		// ---------------------------------------------------------------------
		//  Finding the browser binary -- before forking, so we can say so
		// ---------------------------------------------------------------------
		// execvp() failing in the child is a message nobody sees: the child is
		// already forked, its stderr goes wherever gamescope's does, and the
		// user gets a hotkey that silently did nothing. So resolve argv[0] the
		// same way execvp would, HERE, and turn a miss into a toast. The search
		// itself lives in Utils/Process.h -- SteamFriends.cpp's join needs the
		// identical check before it fires `steam`, and one PATH walk with one
		// test is better than two that can drift.
		using Process::ExecutableExists;

		void Toast( std::string sText, Notifications::Kind kind )
		{
			// Notifications::Show() is documented as steamcompmgr-thread only;
			// every call site in this file is reached from Tick().
			Notifications::Show( std::move( sText ), kind, 5.0f );
		}

		// ---------------------------------------------------------------------
		//  Process group membership -- "is this window ours?"
		// ---------------------------------------------------------------------
		// The window is found by PROCESS GROUP, not by WM_CLASS or by title.
		// `Why:` a class name is a fact about one browser (chromium derives it
		// from the URL's host, firefox says "Navigator"), so matching on it
		// would quietly stop working the moment the user changed the command
		// -- which is a setting this feature deliberately offers. The process
		// group is a fact about US: Spawn() puts the child in its own group,
		// every descendant inherits it, and steamcompmgr_win_t::pid is the
		// real client pid from the XRes extension. So any window any part of
		// the browser opens is recognisable, whatever browser it is.
		pid_t PgidOf( pid_t nPid )
		{
			if ( nPid <= 0 )
				return -1;
			// getpgid() answers for any process, not only our children.
			const pid_t nPgid = getpgid( nPid );
			return nPgid > 0 ? nPgid : -1;
		}

		bool IsOurs( const steamcompmgr_win_t *w )
		{
			if ( !w || g_State.nPgid <= 0 || w->pid <= 0 )
				return false;
			if ( w->type != steamcompmgr_win_type_t::XWAYLAND )
				return false;
			return PgidOf( w->pid ) == g_State.nPgid;
		}

		// ---------------------------------------------------------------------
		//  X property helpers
		// ---------------------------------------------------------------------
		void SetCardinal( xwayland_ctx_t *ctx, Window uWindow, Atom atom, uint32_t uValue )
		{
			const unsigned long ulValue = uValue;
			XChangeProperty( ctx->dpy, uWindow, atom, XA_CARDINAL, 32, PropModeReplace,
			                 (const unsigned char *)&ulValue, 1 );
		}

		// Show and hide, in the ONE place either is spelled. Both properties
		// always move together -- see SteamCompanionCmd.h's PropsFor().
		void ApplyProps( xwayland_ctx_t *ctx, bool bShown )
		{
			if ( g_State.uWindow == None )
				return;

			const OverlayProps props = PropsFor( bShown );
			SetCardinal( ctx, g_State.uWindow, ctx->atoms.opacityAtom,        props.uOpacity );
			SetCardinal( ctx, g_State.uWindow, ctx->atoms.steamInputFocusAtom, props.uInputFocus );
			XFlush( ctx->dpy );

			g_State.bApplied = true;
			g_State.nTrapStrikes = 0;
			companion_log.debugf( "%s: opacity=%u input_focus=%u", bShown ? "shown" : "hidden",
				props.uOpacity, props.uInputFocus );
		}

		// ---------------------------------------------------------------------
		//  Spawn
		// ---------------------------------------------------------------------
		void Spawn( xwayland_ctx_t *ctx, const Options &opts )
		{
			const char *pszDisplay = ctx->xwayland_server
				? ctx->xwayland_server->get_nested_display_name() : nullptr;
			if ( !pszDisplay || !*pszDisplay )
			{
				Toast( "Steam chat: gamescope has no X display to open it on.",
					Notifications::Kind::Error );
				return;
			}

			const std::string sProfileDir = ProfileDir();
			std::vector<std::string> vecArgs;
			std::string sError;
			if ( !BuildArgv( opts.sCommand, opts.sUrl, sProfileDir, &vecArgs, &sError ) )
			{
				companion_log.errorf( "browser command is unusable: %s", sError.c_str() );
				Toast( "Steam chat: " + sError + " Fix it in Settings > System > Steam chat.",
					Notifications::Kind::Error );
				return;
			}

			if ( !ExecutableExists( vecArgs[ 0 ] ) )
			{
				companion_log.errorf( "\"%s\" is not installed (or not on PATH).", vecArgs[ 0 ].c_str() );
				Toast( "Steam chat needs \"" + vecArgs[ 0 ] + "\", which isn't installed. Install it, "
				       "or set a different browser in Settings > System > Steam chat.",
					Notifications::Kind::Error );
				return;
			}

			// Best effort: the browser will make it itself if it can, but
			// saying so here means a permissions problem is logged by us.
			if ( mkdir( sProfileDir.c_str(), 0700 ) != 0 && errno != EEXIST )
				companion_log.warnf( "could not create %s: %s", sProfileDir.c_str(), strerror( errno ) );

			std::vector<char *> vecArgv;
			vecArgv.reserve( vecArgs.size() + 1 );
			for ( std::string &s : vecArgs )
				vecArgv.push_back( s.data() );
			vecArgv.push_back( nullptr );

			// Everything the child needs, computed BEFORE the fork -- the
			// preamble runs between fork() and exec() in a process whose other
			// threads are gone, so it allocates as little as it can get away
			// with.
			const std::string sDisplay( pszDisplay );

			const pid_t nPid = Process::SpawnProcess( vecArgv.data(), [ &sDisplay ]()
				{
					// Its own process group, so Shutdown() can kill the whole
					// browser tree with one killpg even if the browser has
					// re-parented parts of itself away from us.
					setpgid( 0, 0 );

					// And, if gamescope is killed outright rather than shut
					// down, the kernel does it for us. PDEATHSIG follows the
					// forking THREAD, which is the steamcompmgr thread -- it
					// exits at teardown, before main.cpp's own KillAllChildren,
					// so this fires at exactly the right moment.
					Process::SetDeathSignal( SIGTERM );

					// On GAMESCOPE'S Xwayland, and nothing else. Unsetting the
					// Wayland sockets matters: a browser that finds one will
					// happily open a Wayland window on the HOST session
					// instead, which is the failure this whole feature exists
					// to avoid.
					setenv( "DISPLAY", sDisplay.c_str(), 1 );
					unsetenv( "WAYLAND_DISPLAY" );
					unsetenv( "GAMESCOPE_WAYLAND_DISPLAY" );
					setenv( "GAMESCOPE_RITZ_COMPANION", "1", 1 );
				} );

			if ( nPid <= 0 )
			{
				Toast( "Steam chat: could not start \"" + vecArgs[ 0 ] + "\".",
					Notifications::Kind::Error );
				return;
			}

			g_State.nPid   = nPid;
			g_State.nPgid  = nPid;   // setpgid(0,0) makes the pgid the child's own pid
			g_State.uWindow = None;
			g_State.bWantShown = true;
			g_State.bApplied = false;
			g_State.nTrapStrikes = 0;
			g_State.bTrapReported = false;

			companion_log.infof( "launched \"%s\" on %s as pid %d", vecArgs[ 0 ].c_str(),
				sDisplay.c_str(), (int)nPid );

			// THE FIRST-PRESS DELAY, ANSWERED WITH A TOAST rather than with
			// nothing. A cold browser takes seconds to put a window up, and a
			// hotkey that changes nothing on screen for that long reads as a
			// hotkey that did not work -- so the user presses it again, and on
			// a TOGGLE the second press means "hide". One self-dismissing
			// toast costs nothing, uses machinery that is already on screen
			// for every other confirmation in this fork, and turns a dead
			// three seconds into a wait the user understands. Only the LAUNCH
			// gets one; show and hide are instant and need no narration.
			Toast( "Opening Steam chat...", Notifications::Kind::Info );
		}

		// ---------------------------------------------------------------------
		//  One press
		// ---------------------------------------------------------------------
		void HandlePress( xwayland_ctx_t *ctx )
		{
			const ToggleAct act = PlanToggle( g_State.nPid > 0, g_State.bWantShown );

			if ( act == ToggleAct::Launch )
			{
				const Options opts = ReadOptions();
				if ( !opts.bEnabled )
				{
					// Deliberately a message and not silence: the chord is
					// bound and swallowed whatever this switch says, so a
					// silent no-op would look like a broken hotkey.
					Toast( "Steam chat is switched off. Turn it on in Settings > System > Steam "
					       "chat, or rebind this key in Setup > Keybinds.",
						Notifications::Kind::Info );
					return;
				}
				Spawn( ctx, opts );
				return;
			}

			g_State.bWantShown = ( act == ToggleAct::Show );
			g_State.bApplied = false;
			ApplyProps( ctx, g_State.bWantShown );
		}

		// ---------------------------------------------------------------------
		//  Housekeeping
		// ---------------------------------------------------------------------
		void Reap()
		{
			if ( g_State.nPid <= 0 )
				return;

			int nStatus = 0;
			const pid_t nDone = waitpid( g_State.nPid, &nStatus, WNOHANG );
			if ( nDone != g_State.nPid )
				return;

			companion_log.infof( "the browser (pid %d) exited; the next press starts a new one.",
				(int)g_State.nPid );
			g_State = State{};
		}

		// The window the browser opened, once it exists. Also re-validates a
		// window we already found: a browser can close and re-open its
		// toplevel without exiting.
		void TrackWindow( xwayland_ctx_t *ctx )
		{
			if ( g_State.nPid <= 0 )
				return;

			steamcompmgr_win_t *pBest = nullptr;
			int nBestArea = 0;
			bool bStillThere = false;

			for ( steamcompmgr_win_t *w = ctx->list; w; w = w->xwayland().next )
			{
				if ( !IsOurs( w ) )
					continue;
				if ( g_State.uWindow != None && w->xwayland().id == g_State.uWindow )
					bStillThere = true;
				if ( w->xwayland().a.map_state != IsViewable )
					continue;
				const Rect r = w->GetGeometry();
				const int nArea = r.nWidth * r.nHeight;
				if ( nArea > nBestArea )
				{
					nBestArea = nArea;
					pBest = w;
				}
			}

			if ( g_State.uWindow != None && bStillThere )
				return;

			if ( !pBest || nBestArea <= 0 )
			{
				if ( g_State.uWindow != None )
				{
					companion_log.infof( "the browser's window went away; waiting for a new one." );
					g_State.uWindow = None;
					g_State.bApplied = false;
				}
				return;
			}

			g_State.uWindow = pBest->xwayland().id;
			g_State.bApplied = false;

			// STEAM_OVERLAY is set once and never moves: it says WHAT the
			// window is, not whether it is on screen. Only the pair in
			// PropsFor() toggles.
			SetCardinal( ctx, g_State.uWindow, ctx->atoms.overlayAtom, 1 );

			// Fill the screen whatever size the browser asked for. Doing it
			// here rather than relying on a browser flag is what keeps the
			// setting "any browser command you like" honest -- see
			// handle_desktop_window()'s companion guard for the other half.
			if ( pBest->GetGeometry().nWidth != ctx->root_width ||
			     pBest->GetGeometry().nHeight != ctx->root_height )
			{
				XMoveResizeWindow( ctx->dpy, g_State.uWindow, 0, 0,
					ctx->root_width, ctx->root_height );
			}

			companion_log.infof( "window 0x%lx is the companion overlay (%dx%d)",
				g_State.uWindow, ctx->root_width, ctx->root_height );
		}

		// THE TRAP, watched live. The static_asserts in SteamCompanionCmd.h
		// pin the INTENT; this pins the PLUMBING between that intent and the X
		// server -- a future edit that sets the opacity but forgets the input
		// focus (or sets them in a path that never flushes) shows up here as a
		// loud, named error instead of as "the game stopped answering the
		// keyboard and nobody knows why".
		void WatchForSwallowedInput( const xwayland_ctx_t *ctx )
		{
			if ( g_State.uWindow == None || g_State.bWantShown )
			{
				g_State.nTrapStrikes = 0;
				return;
			}

			const steamcompmgr_win_t *pWin = nullptr;
			for ( const steamcompmgr_win_t *w = ctx->list; w; w = w->xwayland().next )
			{
				if ( w->xwayland().id == g_State.uWindow )
				{
					pWin = w;
					break;
				}
			}
			if ( !pWin || pWin->inputFocusMode == 0 )
			{
				g_State.nTrapStrikes = 0;
				g_State.bTrapReported = false;
				return;
			}

			// A PropertyNotify takes a round trip; only a state that PERSISTS
			// is a bug. ~2 seconds at any refresh rate this compositor runs at.
			if ( ++g_State.nTrapStrikes < 120 || g_State.bTrapReported )
				return;

			g_State.bTrapReported = true;
			companion_log.errorf( "BUG: the companion overlay is hidden but still holds "
				"STEAM_INPUT_FOCUS (%u) -- every keystroke is being swallowed and the game gets "
				"none. Hiding must clear BOTH _NET_WM_WINDOW_OPACITY and STEAM_INPUT_FOCUS; see "
				"src/SteamCompanionCmd.h's PropsFor().", pWin->inputFocusMode );
			Toast( "Steam chat is hidden but is still holding the keyboard - this is a bug; "
			       "press the chat key twice to recover.", Notifications::Kind::Error );
		}
	}

	// =========================================================================
	//  The public surface
	// =========================================================================
	void RequestToggle()
	{
		g_nPendingPresses.fetch_add( 1, std::memory_order_relaxed );
		nudge_steamcompmgr();
	}

	void Tick( xwayland_ctx_t *ctx )
	{
		if ( !ctx )
			return;

		Reap();

		int nPresses = g_nPendingPresses.exchange( 0, std::memory_order_relaxed );
		if ( nPresses > kMaxPressesPerTick )
			nPresses = kMaxPressesPerTick;
		for ( int i = 0; i < nPresses; i++ )
			HandlePress( ctx );

		if ( g_State.nPid <= 0 )
			return;

		TrackWindow( ctx );

		if ( g_State.uWindow != None && !g_State.bApplied )
			ApplyProps( ctx, g_State.bWantShown );

		WatchForSwallowedInput( ctx );
	}

	bool OwnsWindow( const steamcompmgr_win_t *w )
	{
		return IsOurs( w );
	}

	std::string StatusText()
	{
		const std::string sChord = keybinds::ChordTextFor( keybinds::Action::Companion );
		const Options opts = ReadOptions();

		if ( !opts.bEnabled )
			return "off - " + sChord + " will say so and do nothing";

		std::vector<std::string> vecArgs;
		std::string sError;
		if ( !BuildArgv( opts.sCommand, opts.sUrl, ProfileDir(), &vecArgs, &sError ) )
			return "the browser command is unusable: " + sError;

		if ( !ExecutableExists( vecArgs[ 0 ] ) )
			return "\"" + vecArgs[ 0 ] + "\" is not installed - " + sChord + " will say so";

		if ( g_State.nPid > 0 )
			return sChord + " - " + vecArgs[ 0 ] + " is running, " +
				( g_State.bWantShown ? "shown" : "hidden" );

		return sChord + " - " + vecArgs[ 0 ] + ", not started yet";
	}

	void Shutdown()
	{
		if ( g_State.nPid <= 0 )
			return;

		// The process GROUP, not the process: a browser is a tree, and the
		// tree is what must not survive us. This is the first of three
		// guarantees the child cannot outlive gamescope -- PR_SET_PDEATHSIG
		// (set in the child, fires if we are killed outright) and main.cpp's
		// KillAllChildren()/WaitForAllChildren() at the end of teardown are
		// the other two.
		const pid_t nPgid = g_State.nPgid > 0 ? g_State.nPgid : g_State.nPid;
		companion_log.infof( "teardown: stopping the companion browser (pgid %d)", (int)nPgid );
		killpg( nPgid, SIGTERM );

		// A short, bounded grace period, then the hammer. Bounded because this
		// runs on the teardown path and nothing here may hang the exit.
		for ( int i = 0; i < 100; i++ )   // <= 1s
		{
			int nStatus = 0;
			if ( waitpid( g_State.nPid, &nStatus, WNOHANG ) == g_State.nPid )
			{
				g_State = State{};
				return;
			}
			usleep( 10 * 1000 );
		}

		companion_log.warnf( "the companion browser did not stop; killing it." );
		killpg( nPgid, SIGKILL );
		waitpid( g_State.nPid, nullptr, 0 );
		g_State = State{};
	}
}
