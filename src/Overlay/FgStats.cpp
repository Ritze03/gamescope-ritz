// The frame generation stats overlay: a small debug readout in the top left corner, off by default.
// NOT part of the HUD (the user had the HUD's perf stats removed 2026-09-03); it shares the HUD's
// layer and ImGui draw list only so it costs no extra layer. The text is Overlay/FgStats.h.
// See superdoc/features/fps-display.md.
#include "FgStats.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>

#include "convar.h"
#include "log.hpp"
#include "steamcompmgr.hpp"
#include "Fonts.h"
#include "FrameGen/FrameGenDebug.h"
#include "FrameGen/FrameGenHost.h"
#include "FrameGen/FrameGenV3.h"
#include "FrameGen/HostShown.h"

#include "imgui.h"

namespace fgstats
{
	static LogScope s_Log( "fgstats" );

	// Test aid: 1 = show regardless of the settings switch.
	static gamescope::ConVar<int> cv_ritz_fg_stats( "ritz_fg_stats", 0,
		"Debug: 1 = show the frame generation stats overlay (top left) regardless of the settings switch." );

	static std::atomic<bool> s_bEnabled{ false };

	void SetEnabled( bool bEnabled )
	{
		if ( s_bEnabled.exchange( bEnabled, std::memory_order_relaxed ) != bEnabled )
			force_repaint();   // appear / disappear on the next paint, not whenever the game next commits
	}

	bool Enabled() { return s_bEnabled.load( std::memory_order_relaxed ); }
	bool Wanted() { return Enabled() || cv_ritz_fg_stats != 0; }

	namespace
	{
		constexpr uint64_t kRefreshNs = 250ull * 1000ull * 1000ull;   // the text changes at most 4 times a second

		const char *MotionName( fghost::Motion m )
		{
			switch ( m )
			{
				case fghost::Motion::V1:      return "V1";
				case fghost::Motion::V2:      return "V2";
				case fghost::Motion::Learned: return "Learned";
				case fghost::Motion::V3:      return "V3";
			}
			return "";
		}

		// Same wording as FrameGen_ModeText() in steamcompmgr.cpp.
		std::string ModeText( const fghost::Config &cfg )
		{
			char sz[ 48 ];
			if ( cfg.mode == fghost::Mode::Fixed )
				snprintf( sz, sizeof( sz ), "fixed %dx", cfg.multiplier );
			else if ( cfg.mode == fghost::Mode::Target && cfg.targetFps > 0 )
				snprintf( sz, sizeof( sz ), "target %d fps", cfg.targetFps );
			else if ( cfg.mode == fghost::Mode::Target )
				snprintf( sz, sizeof( sz ), "target display refresh" );
			else
				return "";
			return sz;
		}

		// Our own baseline for the discarded rate. `Why:` fghostshown::TakeRates() keeps function-static
		// baselines that the `frame generation status` line owns; calling it here would steal its interval.
		struct Baseline_t
		{
			uint64_t ulNs = 0, ulCommits = 0, ulDiscarded = 0;
		};

		float DiscardedRate( uint64_t ulNow, Baseline_t &b )
		{
			const fghostshown::Counters &c = fghostshown::Get();
			const uint64_t ulCommits = c.ulCommits.load( std::memory_order_relaxed );
			const uint64_t ulDiscarded = c.ulDiscarded.load( std::memory_order_relaxed );
			float fl = -1.0f;
			const uint64_t ulDt = ulNow - b.ulNs;
			// A stale baseline (the overlay was off, or paints stopped) says nothing about now.
			if ( b.ulNs != 0 && ulDt > 0 && ulDt < 3000ull * 1000ull * 1000ull && ulCommits > b.ulCommits )
				fl = float( double( ulDiscarded - b.ulDiscarded ) * 1e9 / double( ulDt ) );   // commits == 0: not the nested Wayland backend, no number
			b = { ulNow, ulCommits, ulDiscarded };
			return fl;
		}

		void Gather( uint64_t ulNow, std::vector<std::string> &lines )
		{
			static Baseline_t s_Base;
			const fghost::Config cfg = fghost::GetConfig();
			const fghost::PacingStatus st = fghost::GetPacingStatus();
			const fghost::RenderStatus rs = fghost::GetRenderStatus();

			Input in;
			in.bFrameGen = cfg.enabled;
			in.sMotion = MotionName( cfg.motion );
			const int nBlend = cfg.motion == fghost::Motion::V3 ? cfg.v3Blend : 0;
			if ( nBlend )
				in.sBlend = fghost::BlendLabel( nBlend, fghost::BlendActive() );
			in.sQuality = cfg.quality == fghost::Quality::Quality ? "Quality" : "Performance";
			in.sFormat = rs.formatTag ? rs.formatTag : "";
			in.sModeText = ModeText( cfg );
			in.bGenerating = st.fgActive;
			if ( st.valid )
			{
				in.flGameFps = st.gameFps;
				in.flShownFps = st.presentedFps;
				in.flMultiplier = st.effectiveMultiplier;
				in.flDelayMs = st.delayMs;
			}
			in.flDiscardedPerSec = DiscardedRate( ulNow, s_Base );
			in.flEstimateMs = rs.lastEstimateMs;
			in.flSynthMs = rs.lastSynthMs;
			in.flPairMs = rs.lastPairGpuMs;
			in.flUiMs = rs.lastUiMs;
			in.sUiProtection = fghost::UiModeName( cfg.ui, cfg.uiWholeFrame );
			{
				// What the user wants (the effective Debug view), and "(not available)" when the library refused it.
				const int nView = fghost::GetDebugConfig().view;
				if ( nView > 0 )
				{
					in.sView = fghost::DebugViewName( nView );
					in.bViewRefused = fghost::DebugViewRefused();
				}
			}
			std::string sReason = st.valid ? fghost::PassReasonText( st.reason ) : "";
			if ( sReason.empty() && rs.reason != fghost::Unavailable::Ok )
				sReason = fghost::UnavailableText( rs.reason );
			in.sReason = sReason;
			lines = FormatLines( in );
		}
	}

	float Draw( ImDrawList *pList, float flDisplayW, float flDisplayH, float flYOffset )
	{
		if ( !Wanted() || !pList )
			return 0.0f;

		static std::vector<std::string> s_Lines;
		static uint64_t s_ulLast = 0;
		const uint64_t ulNow = get_time_in_nanos();
		if ( s_Lines.empty() || ulNow - s_ulLast >= kRefreshNs )
		{
			std::vector<std::string> prev = s_Lines;
			Gather( ulNow, s_Lines );
			s_ulLast = ulNow;
			if ( s_Lines != prev )
			{
				std::string s;
				for ( const std::string &l : s_Lines )
					s += ( s.empty() ? "" : " | " ) + l;
				s_Log.debugf( "stats: %s", s.c_str() );
			}
		}

		// Mono 13px (Style::Value, the styling guide's numeric readout), scaled up on big outputs.
		ImFont *pFont = gamescope::fonts::Get( gamescope::fonts::Style::Value );
		const float flScale = std::max( 1.0f, flDisplayH / 1080.0f );
		const float flSize = 13.0f * flScale;
		const float flLine = flSize * 1.3f;
		const float flPad = 7.0f * flScale;
		const float flMargin = 10.0f * flScale;

		float flW = 0.0f;
		for ( const std::string &l : s_Lines )
			flW = std::max( flW, pFont->CalcTextSizeA( flSize, FLT_MAX, 0.0f, l.c_str() ).x );
		const ImVec2 p0( flMargin, flMargin + flYOffset );
		const ImVec2 p1( p0.x + flW + 2.0f * flPad, p0.y + flLine * float( s_Lines.size() ) + 2.0f * flPad );
		if ( p1.x > flDisplayW || p1.y > flDisplayH )
			return 0.0f;

		pList->AddRectFilled( p0, p1, IM_COL32( 12, 14, 18, 200 ), 4.0f * flScale );
		for ( size_t i = 0; i < s_Lines.size(); i++ )
			pList->AddText( pFont, flSize, ImVec2( p0.x + flPad, p0.y + flPad + flLine * float( i ) ), IM_COL32( 232, 236, 240, 255 ), s_Lines[ i ].c_str() );
		return p1.y;
	}
}
