// Issue #39 LOG panel -- see PanelLog.h. Both tabs share DrawLogView()
// below; the only difference between them is which LogCapture ring buffer
// they read and whether that capture is known to be active at all.
#include "PanelLog.h"

#include "LogCapture.h"
#include "Chrome.h"
#include "Palette.h"

#include <cstdint>
#include <string>
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

	// The panel's body, with no window around it -- see PanelLog.h's
	// PanelLog_DrawBody(). Verbatim from PanelLog_Draw().
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

	void PanelLog_DrawBody()
	{
		RefreshBothTabs();
		DrawBodyContent();
	}
}
