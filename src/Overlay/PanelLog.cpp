// Issue #39 LOG panel -- see PanelLog.h. Both tabs share DrawLogView()
// below; the only difference between them is which LogCapture ring buffer
// they read and whether that capture is known to be active at all.
#include "PanelLog.h"

#include "LogCapture.h"
#include "Chrome.h"
#include "Palette.h"
#include "UI/Registry.h"

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

		ImVec4 ColorForPriority( LogPriority ePriority )
		{
			switch ( ePriority )
			{
				case LOG_ERROR:   return ImVec4( 0.95f, 0.35f, 0.35f, 1.0f );
				case LOG_WARNING: return ImVec4( 0.95f, 0.75f, 0.30f, 1.0f );
				case LOG_DEBUG:   return palette::ToVec4( palette::White( 0.55f ) );
				case LOG_INFO:
				default:          return palette::ToVec4( palette::kTextRgb );
			}
		}

		// One line's worth of a fixed-width [scope] prefix, colored by
		// priority, then the raw text. Scope is empty for game-tab lines
		// (there's no LogScope prefix for raw process output).
		void DrawOneLine( const LogCapture::Line &line )
		{
			ImVec4 col = ColorForPriority( line.ePriority );
			if ( !line.sScope.empty() )
			{
				ImGui::TextColored( col, "[%s]", line.sScope.c_str() );
				ImGui::SameLine( 0.0f, 6.0f );
			}
			ImGui::PushStyleColor( ImGuiCol_Text, col );
			// TextUnformatted, not Text() -- log lines can contain '%'
			// freely and must never be interpreted as a format string.
			ImGui::TextUnformatted( line.sText.c_str() );
			ImGui::PopStyleColor();
		}

		// The standard ImGui log-window idiom: stick to the bottom every
		// frame the view was already at the bottom *before* this frame's
		// new content was laid out, stop the instant the user scrolls up
		// (GetScrollY() then reads less than GetScrollMaxY()), resume
		// automatically once they scroll back down themselves -- no
		// separate "auto-scroll" flag needed, the scroll position itself
		// *is* the state.
		void DrawLogView( const char *pszChildId, const std::vector<LogCapture::Line> &vecLines, const char *pszEmptyMessage )
		{
			ImGui::BeginChild( pszChildId, ImVec2( 0.0f, -ImGui::GetFrameHeightWithSpacing() ),
				ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar );

			if ( vecLines.empty() )
			{
				ImGui::TextDisabled( "%s", pszEmptyMessage );
			}
			else
			{
				// Every buffered Line is exactly one visual row (multi-line
				// log calls and raw child output are both pre-split on '\n'
				// by LogCapture -- see its PushSplitLines()/reader threads),
				// so a uniform-height clipper is safe even at thousands of
				// lines: only ever build draw data for what's actually on
				// screen.
				ImGuiListClipper clipper;
				clipper.Begin( (int)vecLines.size() );
				while ( clipper.Step() )
				{
					for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++ )
						DrawOneLine( vecLines[ (size_t)i ] );
				}
				clipper.End();

				if ( ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f )
					ImGui::SetScrollHereY( 1.0f );
			}

			ImGui::EndChild();

			if ( ImGui::Button( "Copy to clipboard" ) )
			{
				std::string sAll;
				for ( const LogCapture::Line &line : vecLines )
				{
					if ( !line.sScope.empty() )
					{
						sAll += '[';
						sAll += line.sScope;
						sAll += "] ";
					}
					sAll += line.sText;
					sAll += '\n';
				}
				ImGui::SetClipboardText( sAll.c_str() );
			}
			ImGui::SameLine();
			ImGui::TextDisabled( "%d line%s", (int)vecLines.size(), vecLines.size() == 1 ? "" : "s" );
		}
	}

	// Kept cheap even while the panel is closed: a generation compare per
	// tab, no copy unless something actually changed -- matches every other
	// panel calling its own Ensure*Loaded()/GetState() unconditionally each
	// frame (PanelAudio_Draw() etc).
	static void RefreshBothTabs()
	{
		RefreshTab( s_GamescopeTab, LogCapture::GetGamescopeLogGeneration(), LogCapture::GetGamescopeLog );
		RefreshTab( s_GameTab, LogCapture::GetGameLogGeneration(), LogCapture::GetGameLog );
	}

	// The LEGACY (`overlay_e2 0`) two-tab body. Its E2 successor is
	// PanelLog_RegisterArea() below, which declares the same information as
	// filter rows plus an Area::Content() body; this stays only while both
	// shells coexist, and the legacy one must not change.
	static void DrawBodyContent()
	{
		if ( ImGui::BeginTabBar( "LogTabs" ) )
		{
			if ( ImGui::BeginTabItem( "Gamescope" ) )
			{
				DrawLogView( "##gamescopelog", s_GamescopeTab.vecLines,
					"No log output captured yet." );
				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "Game" ) )
			{
				if ( !LogCapture::IsGameCaptureActive() )
				{
					ImGui::TextColored( ImVec4( 0.95f, 0.65f, 0.25f, 1.0f ),
						"Not capturing -- no game was launched this session." );
					ImGui::TextDisabled( "gamescope was started with no sub-command "
						"(e.g. run as a bare compositor, or via a tool that launches "
						"the game separately), so there is no child process to capture "
						"stdout/stderr from." );
				}
				else
				{
					DrawLogView( "##gamelog", s_GameTab.vecLines,
						s_GameTab.bHasEverSeenData
							? "The game hasn't printed anything else yet."
							: "Capturing -- nothing printed to stdout/stderr yet." );
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}

	void PanelLog_Draw()
	{
		RefreshBothTabs();

		if ( !chrome::BeginPanelWindow( "LOG", chrome::PanelId::Log,
			ImVec2( 64.0f, 64.0f ), ImVec2( 620.0f, 420.0f ) ) )
			return;

		DrawBodyContent();

		chrome::EndPanelWindow();
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
		std::vector<ui::ContentLine> BuildLines()
		{
			RefreshBothTabs();

			std::vector<ui::ContentLine> vec;
			vec.reserve( s_GamescopeTab.vecLines.size() + s_GameTab.vecLines.size() );

			for ( const LogCapture::Line &line : s_GamescopeTab.vecLines )
			{
				if ( !PassesFilters( line, 0 ) )
					continue;
				vec.push_back( ui::ContentLine{ ContentSeverity( line.ePriority ),
					line.sScope.empty() ? std::string( "gamescope" ) : line.sScope, line.sText } );
			}
			for ( const LogCapture::Line &line : s_GameTab.vecLines )
			{
				if ( !PassesFilters( line, 1 ) )
					continue;
				vec.push_back( ui::ContentLine{ ContentSeverity( line.ePriority ),
					std::string( "game" ), line.sText } );
			}
			return vec;
		}

		size_t CountShown() { return BuildLines().size(); }
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

		// ---- Copy, and issue #81 ----------------------------------------
		// THE BUTTON IS DISABLED, ON PURPOSE, AND SAYS WHY.
		//
		// #81: the old "Copy to clipboard" button called
		// ImGui::SetClipboardText() and appeared to work. It never reached
		// the system clipboard, because no clipboard handler is wired for
		// the overlay's ImGui context -- with io.SetClipboardTextFn unset,
		// ImGui falls back to an internal buffer that only ImGui itself can
		// read. Nothing outside this process ever sees the text.
		//
		// Wiring it properly is not a small fix and is not this task's
		// scope: gamescope is the compositor, so a real implementation
		// means offering a wl_data_source selection on its own seat (and,
		// nested, deciding what "the system clipboard" even means when the
		// host session owns one too). That is a feature, with its own
		// design questions.
		//
		// So the row ships DISABLED with a mandatory reason (SPEC §3.13)
		// rather than shipping a button that lies. A control that looks
		// right and does nothing is precisely issues #25 and #68 -- the
		// thing this whole redesign exists to stop -- and "it silently did
		// nothing" is strictly worse than "it told you it cannot".
		a.Action( "log.copy", "Copy to clipboard", "copy", []{} )
			.Help( "Copies every visible line as plain text. Unavailable until the overlay's "
			       "ImGui context has a clipboard handler -- see issue #81." )
			.DisabledUnless( []{ return false; },
				"no clipboard handler is wired for the overlay yet, so a copy would silently "
				"reach nothing outside gamescope (issue #81)" )
			.Keywords( "copy clipboard export text" );

		// ---- the captured text itself ------------------------------------
		a.Content( []{ return BuildLines(); } );
		a.FollowsTail( []{ return s_bAutoScroll; } );
	}
}
