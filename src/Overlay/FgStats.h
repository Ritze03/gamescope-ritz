// The frame generation stats overlay's text (pure: no ImGui, no gamescope globals, so the
// unit test needs no other sources). The drawing and the number gathering are
// Overlay/FgStats.cpp. NOT part of the HUD: superdoc/features/fps-display.md.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

struct ImDrawList;

namespace fgstats
{
	// Every number and string the block shows. A number < 0 is "not known" and prints as "-",
	// never as a 0 pretending to be a measurement.
	struct Input
	{
		bool        bFrameGen = false;     // frame generation switched on
		std::string sMotion;               // "V1", "V2", "Learned", "V3"
		std::string sBlend;                // V3 Hybrid's BlendLabel() ("A-A", "A-A(off)"), "" = none
		std::string sQuality;              // "Quality" / "Performance"
		std::string sFormat;               // the renderer's format tag ("10-bit"), "" = plain 8-bit
		std::string sModeText;             // "fixed 2x" / "target 280 fps" / "target display refresh"
		bool        bGenerating = false;
		float       flGameFps = -1.0f;
		float       flShownFps = -1.0f;
		float       flMultiplier = -1.0f;
		float       flDelayMs = -1.0f;
		float       flDiscardedPerSec = -1.0f;
		float       flEstimateMs = -1.0f;  // the library's GPU timestamps, last values
		float       flSynthMs = -1.0f;
		float       flPairMs = -1.0f;
		float       flUiMs = -1.0f;
		std::string sUiProtection;         // "Off" / "Crosshair" / "Crosshair V2" / "Whole screen" / "Whole screen V2"
		std::string sReason;               // why it is not generating ("" = none)
	};

	inline std::string Num( float fl, const char *pszFmt )
	{
		if ( fl < 0.0f )
			return "-";
		char sz[ 24 ];
		snprintf( sz, sizeof( sz ), pszFmt, (double)fl );
		return sz;
	}

	// 1 to 6 short plain-ASCII lines.
	inline std::vector<std::string> FormatLines( const Input &in )
	{
		std::vector<std::string> v;
		if ( !in.bFrameGen )
		{
			v.push_back( "frame generation off" );
			return v;
		}

		std::string s = in.sMotion;
		if ( !in.sBlend.empty() )
			s = "V3 Hybrid " + in.sBlend;
		if ( !in.sQuality.empty() )
			s += "   " + in.sQuality;
		if ( !in.sFormat.empty() )
			s += "   " + in.sFormat;
		v.push_back( s );

		s = "game " + Num( in.flGameFps, "%.0f" ) + " -> shown " + Num( in.flShownFps, "%.0f" ) + " fps";
		s += in.bGenerating ? "   " + Num( in.flMultiplier, "%.1f" ) + "x" : std::string( "   passing through" );
		if ( !in.sModeText.empty() )
			s += "   " + in.sModeText;
		v.push_back( s );

		v.push_back( "delay " + Num( in.flDelayMs, "%.1f" ) + " ms   discarded " + Num( in.flDiscardedPerSec, "%.1f" ) + "/s" );
		v.push_back( "FG GPU: estimate " + Num( in.flEstimateMs, "%.2f" ) + "  synth " + Num( in.flSynthMs, "%.2f" ) +
			"  pair " + Num( in.flPairMs, "%.2f" ) + "  UI " + Num( in.flUiMs, "%.2f" ) + " ms" );
		if ( !in.sUiProtection.empty() )
			v.push_back( "UI protection: " + in.sUiProtection );
		if ( !in.bGenerating && !in.sReason.empty() )
			v.push_back( "not generating: " + in.sReason );
		return v;
	}

	// ---- the live half (FgStats.cpp; steamcompmgr thread draws) ----
	void SetEnabled( bool bEnabled );   // the settings switch (the lead wires `framegen.debug_stats` here); any thread
	bool Enabled();
	bool Wanted();                      // Enabled() || ConVar ritz_fg_stats != 0
	// Draws the block into `pList` (top left, `flYOffset` px lower to clear a top-left HUD readout).
	// Returns the block's bottom edge in px, or 0 when nothing was drawn.
	float Draw( ImDrawList *pList, float flDisplayW, float flDisplayH, float flYOffset );
}
