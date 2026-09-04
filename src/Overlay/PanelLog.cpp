// Issue #39 LOG panel -- see PanelLog.h. The two legacy tabs became one
// merged, filtered view; all that survives of them here is the pair of
// generation-gated snapshots BuildLines() reads.
#include "PanelLog.h"

#include "LogCapture.h"
#include "Palette.h"
#include "UI/Registry.h"
#include "steamcompmgr.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		// Cached copy of the last-read snapshot, refreshed only when the
		// ring buffer's generation actually moved (LogCapture::Get*Log()
		// copies its whole buffer per call -- this is what keeps an idle,
		// merely-open log panel from re-copying up to LogCapture.cpp's
		// kMaxLines lines every single frame for nothing). This is the
		// "cheap snapshot the UI reads" half of the never-block-the-
		// render-thread contract -- LogCapture's own mutex-guarded ring
		// buffer + background writers are the other half.
		struct TabState
		{
			std::vector<LogCapture::Line> vecLines;
			uint64_t ulLastSeenGeneration = 0;
			bool bHasEverSeenData = false;
		};

		TabState s_GamescopeTab;
		TabState s_GameTab;

		void RefreshTab( TabState &tab, uint64_t ulCurrentGeneration, LogCapture::Snapshot (*fnFetch)() )
		{
			if ( ulCurrentGeneration == tab.ulLastSeenGeneration )
				return;

			LogCapture::Snapshot snap = fnFetch();
			tab.vecLines = std::move( snap.vecLines );
			tab.ulLastSeenGeneration = snap.ulGeneration;
			tab.bHasEverSeenData = tab.bHasEverSeenData || !tab.vecLines.empty();
		}
	}

	// Kept cheap even while the panel is closed: a generation compare per
	// tab, no copy unless something actually changed -- matches every other
	// panel calling its own Ensure*Loaded()/GetState() unconditionally each
	// frame, the way every panel's own Ensure*Loaded() did.
	static void RefreshBothTabs()
	{
		RefreshTab( s_GamescopeTab, LogCapture::GetGamescopeLogGeneration(), LogCapture::GetGamescopeLog );
		RefreshTab( s_GameTab, LogCapture::GetGameLogGeneration(), LogCapture::GetGameLog );
	}

	// =====================================================================
	//  The Log AREA (E2, P3 part C)
	// =====================================================================
	// The two tabs became the two members of `log.sources`. That is not a
	// cosmetic move: a tab bar says "look at one of these", and what a
	// reader of a log actually wants is "show me these two interleaved,
	// hide that one" -- which is a SET, and D7's rule for a chip bank is
	// exactly "one setting whose value is a set". The severity bank is the
	// same shape and was not expressible as a tab at all.
	//
	// NONE OF THIS FILTER STATE IS PERSISTED, deliberately. It is a view of
	// a session's own ring buffer, so there is no config key for it and
	// none was added -- a filter that survived a restart would hide lines
	// from a later session for a reason nobody remembers setting.
	namespace
	{
		// Bit 0 gamescope, bit 1 game.
		uint32_t s_nSources = 0b11;
		// Bit 0 error, bit 1 warn, bit 2 info, bit 3 debug.
		uint32_t s_nSeverity = 0b1111;
		std::string s_sFilter;
		bool s_bAutoScroll = true;

		int SeverityBit( LogPriority ePriority )
		{
			switch ( ePriority )
			{
				case LOG_ERROR:   return 0;
				case LOG_WARNING: return 1;
				case LOG_DEBUG:   return 3;
				case LOG_INFO:
				default:          return 2;
			}
		}

		// The shell's ContentLine severity scale (0 info, 1 debug, 2 warn,
		// 3 error). Mapped here rather than shared as an enum because the
		// shell must not have to know what a LogPriority is.
		int ContentSeverity( LogPriority ePriority )
		{
			switch ( ePriority )
			{
				case LOG_ERROR:   return 3;
				case LOG_WARNING: return 2;
				case LOG_DEBUG:   return 1;
				case LOG_INFO:
				default:          return 0;
			}
		}

		bool PassesFilters( const LogCapture::Line &line, int nSourceBit )
		{
			if ( !( s_nSources & ( 1u << nSourceBit ) ) )
				return false;
			if ( !( s_nSeverity & ( 1u << SeverityBit( line.ePriority ) ) ) )
				return false;
			if ( !s_sFilter.empty() && line.sText.find( s_sFilter ) == std::string::npos )
				return false;
			return true;
		}

		// One merged, filtered view of both buffers. Rebuilt per call; the
		// per-tab snapshots underneath it are still generation-gated, so an
		// idle log copies nothing.
		//
		// THE TWO RINGS ARE MERGED IN ARRIVAL ORDER, not one after the other.
		// Appending every gamescope line and then every game line -- which is
		// what this did before there was anything to order by -- puts the two
		// streams in separate blocks, so a game line and the gamescope line
		// that caused it can sit thousands of rows apart. LogCapture::Line's
		// ulSeq is a single global counter shared by both rings, so sorting on
		// it restores the true interleaving exactly, with no tie-breaking
		// needed (two lines can share a millisecond; they cannot share a seq).
		std::vector<ui::ContentLine> BuildLines()
		{
			RefreshBothTabs();

			std::vector<ui::ContentLine> vec;
			vec.reserve( s_GamescopeTab.vecLines.size() + s_GameTab.vecLines.size() );

			for ( const LogCapture::Line &line : s_GamescopeTab.vecLines )
			{
				if ( !PassesFilters( line, 0 ) )
					continue;
				// Designated, not positional: ContentLine gained structural
				// fields (eKind, sLead) BETWEEN sText and ulSeq for the
				// Changelog, which silently rebound a positional ulSeq onto
				// eKind. Naming the fields makes this init immune to the
				// next field added in the middle of the struct.
				vec.push_back( ui::ContentLine{
					.nSeverity    = ContentSeverity( line.ePriority ),
					.sScope       = line.sScope.empty() ? std::string( "gamescope" ) : line.sScope,
					.sText        = line.sText,
					.ulSeq        = line.ulSeq,
					.ulTimeMs     = line.ulRealtimeMs } );
			}
			for ( const LogCapture::Line &line : s_GameTab.vecLines )
			{
				if ( !PassesFilters( line, 1 ) )
					continue;
				vec.push_back( ui::ContentLine{
					.nSeverity    = ContentSeverity( line.ePriority ),
					.sScope       = std::string( "game" ),
					.sText        = line.sText,
					.ulSeq        = line.ulSeq,
					.ulTimeMs     = line.ulRealtimeMs } );
			}

			std::sort( vec.begin(), vec.end(),
				[]( const ui::ContentLine &a, const ui::ContentLine &b )
				{ return a.ulSeq < b.ulSeq; } );
			return vec;
		}

		// Counts WITHOUT building the view. CountShown() feeds the area
		// summary and a Live fact, both of which run every frame the Log is on
		// screen -- building (and now sorting) the whole line vector just to
		// take its size copied every visible string for a number.
		size_t CountShown()
		{
			RefreshBothTabs();

			size_t n = 0;
			for ( const LogCapture::Line &line : s_GamescopeTab.vecLines )
				n += PassesFilters( line, 0 );
			for ( const LogCapture::Line &line : s_GameTab.vecLines )
				n += PassesFilters( line, 1 );
			return n;
		}
	}

	void PanelLog_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.log", "Log", ui::Section::System );

		a.Keywords( "log output stdout stderr console messages errors warnings" );
		a.Summary( []
		{
			const size_t nTotal = s_GamescopeTab.vecLines.size() + s_GameTab.vecLines.size();
			char sz[ 64 ];
			std::snprintf( sz, sizeof( sz ), "%zu of %zu lines shown", CountShown(), nTotal );
			return std::string( sz );
		} );

		// ---- Filter ------------------------------------------------------
		a.Group( "Filter" );

		static constexpr ui::Option kSources[] = {
			{ 0, "gamescope" }, { 1, "game" },
		};
		a.Bank( "log.sources", "Sources",
			ui::AnyBind::Of<int>(
				[]{ return (int)s_nSources; },
				[]( int n ) { s_nSources = (uint32_t)n; } ),
			kSources, IM_ARRAYSIZE( kSources ) )
			.Help( "Which captured streams the view shows. One setting whose value is a set -- "
			       "not two switches, because it is one decision about what you are reading." )
			.Default( 0b11 )
			.Keywords( "source subsystem filter gamescope game stream" );

		static constexpr ui::Option kSeverities[] = {
			{ 0, "error" }, { 1, "warn" }, { 2, "info" }, { 3, "debug" },
		};
		a.Bank( "log.severity", "Severity",
			ui::AnyBind::Of<int>(
				[]{ return (int)s_nSeverity; },
				[]( int n ) { s_nSeverity = (uint32_t)n; } ),
			kSeverities, IM_ARRAYSIZE( kSeverities ) )
			.Help( "Which severities stay in view. Hidden lines are still captured, so "
			       "widening this brings them straight back." )
			.Default( 0b1111 )
			.Keywords( "severity error warning info debug level filter" );

		a.Text( "log.filter", "Text filter",
			ui::AnyBind::Of<std::string>(
				[]{ return s_sFilter; },
				[]( std::string s ) { s_sFilter = std::move( s ); } ) )
			.Help( "Keeps only lines containing this text." )
			.Default( std::string() )
			.Keywords( "filter search substring grep text find" );

		a.Switch( "log.autoscroll", "Auto-scroll",
			ui::AnyBind::Of<bool>(
				[]{ return s_bAutoScroll; },
				[]( bool b ) { s_bAutoScroll = b; } ) )
			.Help( "Keeps the newest line in view as the buffer grows. Scrolling up pauses it "
			       "either way; this is the master switch." )
			.Default( true )
			.Keywords( "autoscroll follow tail newest" );

		// ---- Diagnostics -------------------------------------------------
		a.Group( "Diagnostics" );

		a.Facts( "log.buffer", "Buffer",
			[]
			{
				const size_t nTotal = s_GamescopeTab.vecLines.size() + s_GameTab.vecLines.size();
				size_t nErrors = 0;
				for ( const LogCapture::Line &l : s_GamescopeTab.vecLines )
					nErrors += ( l.ePriority == LOG_ERROR );
				char sz[ 64 ];
				std::snprintf( sz, sizeof( sz ), "%zu lines  ·  %zu error%s",
					nTotal, nErrors, nErrors == 1 ? "" : "s" );
				return std::string( sz );
			} )
			.Help( "What the capture rings currently hold." )
			.Keywords( "buffer capacity lines errors capture ring" )
			.Live( "gamescope", []
			{
				char sz[ 32 ];
				std::snprintf( sz, sizeof( sz ), "%zu lines", s_GamescopeTab.vecLines.size() );
				return ui::Fact{ "gamescope", sz };
			} )
			.Live( "game", []
			{
				char sz[ 48 ];
				if ( !LogCapture::IsGameCaptureActive() )
					std::snprintf( sz, sizeof( sz ), "not capturing" );
				else
					std::snprintf( sz, sizeof( sz ), "%zu lines", s_GameTab.vecLines.size() );
				return ui::Fact{ "game", sz };
			} )
			.Live( "shown", []
			{
				char sz[ 32 ];
				std::snprintf( sz, sizeof( sz ), "%zu", CountShown() );
				return ui::Fact{ "shown", sz };
			} );

		// ---- Copy (issue #81, now fixed) --------------------------------
		// This row used to ship DISABLED with a reason: the old
		// implementation called ImGui::SetClipboardText(), which -- with no
		// io.SetClipboardTextFn wired for the overlay's context -- wrote to
		// an internal buffer nothing outside this process could read.
		//
		// The fix is not an ImGui clipboard handler. gamescope *is* the
		// compositor, so it goes to the compositor's own clipboard:
		// gamescope_post_selection() broadcasts the text to every Xwayland
		// server, to gamescope's native Wayland clients, and (when nested)
		// to the host session's clipboard. See
		// superdoc/features/clipboard-sync.md.
		a.Action( "log.copy", "Copy to clipboard", "copy", []
			{
				std::string sOut;
				for ( const ui::ContentLine &line : BuildLines() )
				{
					if ( !line.sScope.empty() )
					{
						sOut += line.sScope;
						sOut += ": ";
					}
					sOut += line.sText;
					sOut += '\n';
				}
				gamescope_post_selection( std::move( sOut ) );
			} )
			.Help( "Copies every visible line as plain text to the clipboard, where it can be "
			       "pasted into a game, into another gamescope window, or -- when gamescope is "
			       "running nested -- into any application on the host session." )
			.Keywords( "copy clipboard export text" );

		// ---- the captured text itself ------------------------------------
		a.Content( []{ return BuildLines(); } );
		a.FollowsTail( []{ return s_bAutoScroll; } );

		// THE SHEET IS THE LOG. Everything declared above configures the VIEW
		// -- which sources, which severities, which substring, whether to
		// follow the tail, and what the capture rings hold -- and none of it
		// is the thing you opened this screen to read. Stacked above the body
		// as ordinary sheet rows they took ~360px before the first line of
		// text (the six-row toolbar the conformance audit flagged as the Log
		// having become "a different design"). In the Inspector they keep
		// every property of a row -- grammar, help, reset, palette entry --
		// and give the whole sheet back to the content.
		a.RowsInInspector();
	}
}
