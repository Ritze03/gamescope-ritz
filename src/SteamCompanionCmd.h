#pragma once

// =============================================================================
//  The Steam companion overlay -- the parts with no compositor in them
// =============================================================================
// See superdoc/features/steam-companion.md for the whole feature and
// superdoc/planning/steam-friends-window.md for why it is a browser on
// gamescope's own Xwayland rather than Steam's real Friends window.
//
// WHAT LIVES HERE. Three things, and only things a test can run with no X
// server, no compositor and no child process:
//
//   * PropsFor()   -- the two X properties that make a window the interactive
//                     overlay, as ONE value, so "shown" and "hidden" cannot be
//                     described in two places that drift apart. This is the
//                     home of the trap the feasibility study measured.
//   * PlanToggle() -- what one press of the chord means, given what is running.
//   * BuildArgv()  -- the browser command line, from the user's command string,
//                     their URL and the private profile directory.
//
// Everything that forks, talks to Xlib or touches the window list is
// src/SteamCompanion.{h,cpp}; this header is deliberately free of all of it so
// tests/test_steam_companion.cpp can hold the rules.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gamescope::companion
{
	// =========================================================================
	//  The two properties -- and THE TRAP
	// =========================================================================
	// steamcompmgr promotes an X11 window on its own Xwayland to a fullscreen
	// interactive overlay from three properties it already reads:
	//
	//   STEAM_OVERLAY          = 1        -> it is the overlay window
	//   STEAM_INPUT_FOCUS      = 1        -> mouse AND keyboard go to it
	//   _NET_WM_WINDOW_OPACITY = 0 / max  -> hidden / shown
	//
	// STEAM_OVERLAY is set once, when the window is first found, and never
	// changes. The pair below is what show and hide actually move.
	//
	// THE TRAP, measured before any of this was written (the study's §5, and
	// build-release/verify-shots/steam-friends-2026-09-08/09-hidden-input-
	// check.png): an overlay at opacity 0 that STILL carries
	// STEAM_INPUT_FOCUS=1 is invisible and keeps swallowing every keystroke.
	// The game looks focused and answers nothing. The cause is one line in
	// steamcompmgr.cpp's DetermineAndApplyFocus():
	//
	//     if ( w->isOverlay && w->inputFocusMode ) inputFocus = w;
	//
	// which is not conditional on opacity, and should not be -- an overlay
	// that wants input while translucent is a legitimate thing. So the fix
	// belongs here: HIDING MUST CLEAR BOTH. Expressing the pair as one
	// function is what makes "clear both" the only reachable spelling, and
	// the static_asserts below are the check that fails the BUILD if a future
	// edit reintroduces it. tests/test_steam_companion.cpp asserts the same
	// thing again at run time, and SteamCompanion.cpp carries a live watchdog
	// for the plumbing between here and the X server.
	inline constexpr uint32_t kOpaque = 0xFFFFFFFFu;

	struct OverlayProps
	{
		uint32_t uOpacity;      // _NET_WM_WINDOW_OPACITY
		uint32_t uInputFocus;   // STEAM_INPUT_FOCUS

		constexpr bool operator==( const OverlayProps &o ) const
		{
			return uOpacity == o.uOpacity && uInputFocus == o.uInputFocus;
		}
	};

	constexpr OverlayProps PropsFor( bool bShown )
	{
		return bShown ? OverlayProps{ kOpaque, 1u }
		              : OverlayProps{ 0u,      0u };
	}

	static_assert( PropsFor( false ).uInputFocus == 0u,
		"Hiding the companion overlay MUST clear STEAM_INPUT_FOCUS as well as the opacity: "
		"DetermineAndApplyFocus() routes input to an overlay purely on inputFocusMode, so an "
		"invisible window that keeps it swallows every keystroke and the game answers nothing. "
		"See this header's comment and superdoc/features/steam-companion.md." );
	static_assert( PropsFor( false ).uOpacity == 0u,
		"Hiding the companion overlay must set the opacity to 0." );
	static_assert( PropsFor( true ).uInputFocus == 1u,
		"Showing the companion overlay must take input focus, or it is a picture." );
	static_assert( PropsFor( true ).uOpacity == kOpaque,
		"Showing the companion overlay must set it fully opaque." );

	// =========================================================================
	//  What one press means
	// =========================================================================
	// The whole toggle state machine. Deliberately total and three-valued:
	// there is no "do nothing" case, because a chord that sometimes does
	// nothing is a chord the user presses twice.
	//
	// Launch is also what a press does when the browser DIED while hidden --
	// bRunning is "is the child alive right now", re-answered every tick by
	// the reaper, not "did we ever start one".
	//
	// A press while a launch is still in flight (the window has not appeared
	// yet) reads as bRunning=true, bShown=true, so it plans a Hide: the user
	// changed their mind during the wait, and the window comes up hidden
	// rather than jumping into their face seconds later. That is the reason
	// this takes "is it running" and not "is its window up".
	enum class ToggleAct : uint8_t
	{
		Launch,   // no browser -- start one, and show it when its window appears
		Show,
		Hide,
	};

	constexpr ToggleAct PlanToggle( bool bRunning, bool bShown )
	{
		if ( !bRunning )
			return ToggleAct::Launch;
		return bShown ? ToggleAct::Hide : ToggleAct::Show;
	}

	// =========================================================================
	//  The command line
	// =========================================================================
	// The user owns the command string (Settings -> System -> Steam chat), so
	// it has to be split the way a person expects a command line to split,
	// without handing it to a shell -- there is no shell in the middle here,
	// SpawnProcess() execvp()s argv directly.
	//
	// Grammar, kept to the smallest thing that is not surprising:
	//   * whitespace separates arguments;
	//   * '...' is literal, including backslashes;
	//   * "..." honours \" and \\ and is otherwise literal;
	//   * a backslash outside quotes escapes the next character;
	//   * an unterminated quote is an error, never a silently-swallowed rest.
	// No globbing, no variable expansion, no `;`/`|`/`&&`. A command string is
	// a command, not a script.
	inline bool SplitCommand( std::string_view svCommand, std::vector<std::string> *pOut,
	                          std::string *psError )
	{
		auto Fail = [ & ]( const char *pszWhy ) {
			if ( psError ) *psError = pszWhy;
			return false;
		};

		std::vector<std::string> args;
		std::string sCur;
		bool bInWord = false;

		for ( size_t i = 0; i < svCommand.size(); i++ )
		{
			const char c = svCommand[ i ];

			if ( c == ' ' || c == '\t' || c == '\n' || c == '\r' )
			{
				if ( bInWord )
				{
					args.push_back( sCur );
					sCur.clear();
					bInWord = false;
				}
				continue;
			}

			bInWord = true;

			if ( c == '\'' )
			{
				const size_t nEnd = svCommand.find( '\'', i + 1 );
				if ( nEnd == std::string_view::npos )
					return Fail( "There is a single quote with no closing quote after it." );
				sCur.append( svCommand.substr( i + 1, nEnd - i - 1 ) );
				i = nEnd;
				continue;
			}

			if ( c == '"' )
			{
				size_t j = i + 1;
				bool bClosed = false;
				for ( ; j < svCommand.size(); j++ )
				{
					if ( svCommand[ j ] == '\\' && j + 1 < svCommand.size() &&
					     ( svCommand[ j + 1 ] == '"' || svCommand[ j + 1 ] == '\\' ) )
					{
						sCur.push_back( svCommand[ j + 1 ] );
						j++;
						continue;
					}
					if ( svCommand[ j ] == '"' )
					{
						bClosed = true;
						break;
					}
					sCur.push_back( svCommand[ j ] );
				}
				if ( !bClosed )
					return Fail( "There is a double quote with no closing quote after it." );
				i = j;
				continue;
			}

			if ( c == '\\' && i + 1 < svCommand.size() )
			{
				sCur.push_back( svCommand[ i + 1 ] );
				i++;
				continue;
			}

			sCur.push_back( c );
		}

		if ( bInWord )
			args.push_back( sCur );

		if ( args.empty() )
			return Fail( "The browser command is empty." );

		if ( pOut )
			*pOut = std::move( args );
		if ( psError )
			psError->clear();
		return true;
	}

	// The two placeholders the command string may carry.
	inline constexpr std::string_view kUrlToken     = "{url}";
	inline constexpr std::string_view kProfileToken = "{profile}";

	inline std::string ReplaceAll( std::string s, std::string_view svFrom, std::string_view svTo )
	{
		if ( svFrom.empty() )
			return s;
		for ( size_t n = s.find( svFrom ); n != std::string::npos; n = s.find( svFrom, n + svTo.size() ) )
			s.replace( n, svFrom.size(), svTo );
		return s;
	}

	// argv for the browser.
	//
	// SUBSTITUTION HAPPENS AFTER SPLITTING, ALWAYS. A URL (or a profile path)
	// containing a space, a quote or a semicolon therefore lands inside ONE
	// argument and can never become a second one -- the user's URL cannot add
	// a flag to their browser, whatever it contains. That ordering is the
	// whole of this function's security story, and it is the reason the URL is
	// not simply pasted into the command string before splitting.
	//
	// A command with no {url} anywhere gets the URL appended as a final
	// argument, so `firefox --kiosk` does the obvious thing.
	inline bool BuildArgv( std::string_view svCommand, std::string_view svUrl,
	                       std::string_view svProfileDir, std::vector<std::string> *pOut,
	                       std::string *psError )
	{
		std::vector<std::string> args;
		if ( !SplitCommand( svCommand, &args, psError ) )
			return false;

		bool bSawUrl = false;
		for ( std::string &s : args )
		{
			if ( s.find( kUrlToken ) != std::string::npos )
				bSawUrl = true;
			s = ReplaceAll( std::move( s ), kUrlToken, svUrl );
			s = ReplaceAll( std::move( s ), kProfileToken, svProfileDir );
		}

		if ( !bSawUrl && !svUrl.empty() )
			args.emplace_back( svUrl );

		if ( pOut )
			*pOut = std::move( args );
		return true;
	}
}
