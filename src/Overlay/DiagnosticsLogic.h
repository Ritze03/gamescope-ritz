// The Diagnostics area's PURE half: what `hyprctl -j getoption
// render:direct_scanout` said, and what that means. No process, no thread, no
// config, no UI -- tests/test_diagnostics.cpp drives every function here.
// Diagnostics.cpp runs hyprctl and publishes the result; PanelDiagnostics.cpp
// draws it. See superdoc/features/diagnostics.md.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace gamescope::diagnostics
{
	enum class ScanoutState
	{
		Pending,        // nothing has finished yet
		NotApplicable,  // this backend has no host to scan out to: no warning
		NoHyprland,     // not under Hyprland: no warning
		On,             // render:direct_scanout is 1 or 2
		Off,            // render:direct_scanout is 0: the one state that warns
		Unknown,        // could not be read: said plainly, never warned about as if Off
	};

	enum class RunOutcome { Ok, NotInstalled, TimedOut, Failed };

	// What running `hyprctl` produced. Filled in by Diagnostics.cpp.
	struct RunResult
	{
		RunOutcome  eOutcome = RunOutcome::Failed;
		int         nExit = 0;
		std::string sOut;
	};

	// What the caller knows about where gamescope is running.
	struct Facts
	{
		bool        bBackendApplicable = false;  // IBackend::HostMayScanOutOutputImages()
		std::string sBackend;                    // the current connector's GetName(); only "SDLWindow" is told apart
		bool        bHyprland = false;           // HYPRLAND_INSTANCE_SIGNATURE was set at launch
	};

	struct ScanoutResult
	{
		ScanoutState eState = ScanoutState::Pending;
		int          nValue = -1;       // render:direct_scanout, when read
		std::string  sHeadline;         // one line for the status row
		std::string  sReason;           // one sentence of why, may be empty
	};

	// `hyprctl -j getoption render:direct_scanout` prints
	//   {"option": "render:direct_scanout", "int": 1, "set": true }
	// An unknown option prints the bare text "no such option" with exit 0, which
	// is not JSON and parses to nullopt like any other garbage.
	inline std::optional<int> ParseDirectScanout( std::string_view sJson )
	{
		nlohmann::json j = nlohmann::json::parse( sJson.begin(), sJson.end(), nullptr, false );
		if ( j.is_discarded() || !j.is_object() )
			return std::nullopt;
		auto it = j.find( "int" );
		if ( it == j.end() || !it->is_number_integer() )
			return std::nullopt;
		const long long n = it->get<long long>();
		if ( n < -1000000 || n > 1000000 )
			return std::nullopt;
		return (int)n;
	}

	inline ScanoutResult DecideDirectScanout( const Facts &f, const RunResult &run )
	{
		ScanoutResult r;

		if ( !f.bBackendApplicable )
		{
			r.eState = ScanoutState::NotApplicable;
			r.sHeadline = "Not applicable";
			if ( f.sBackend == "SDLWindow" )
				r.sReason = "The SDL backend presents through a window swapchain that the host "
				            "composites, so there is nothing for it to scan out. Only the Wayland "
				            "backend sets that up.";
			else
				r.sReason = "This check is for gamescope running as a window inside another "
				            "Wayland compositor, which this session is not.";
			return r;
		}
		if ( !f.bHyprland )
		{
			r.eState = ScanoutState::NoHyprland;
			r.sHeadline = "Hyprland not detected";
			r.sReason = "gamescope was not started under Hyprland, so there is nothing to check.";
			return r;
		}

		r.eState = ScanoutState::Unknown;
		switch ( run.eOutcome )
		{
			case RunOutcome::NotInstalled:
				r.sHeadline = "Could not check";
				r.sReason = "hyprctl was not found.";
				return r;
			case RunOutcome::TimedOut:
				r.sHeadline = "Could not check";
				r.sReason = "hyprctl did not answer within a second.";
				return r;
			case RunOutcome::Failed:
				r.sHeadline = "Could not check";
				r.sReason = "hyprctl failed (exit " + std::to_string( run.nExit ) + ").";
				return r;
			case RunOutcome::Ok:
				break;
		}

		const std::optional<int> oValue = ParseDirectScanout( run.sOut );
		if ( !oValue )
		{
			r.sHeadline = "Could not check";
			r.sReason = run.sOut.find( "no such option" ) != std::string::npos
				? "This Hyprland has no render:direct_scanout option."
				: "hyprctl printed something unexpected.";
			return r;
		}

		r.nValue = *oValue;
		if ( *oValue == 1 || *oValue == 2 )
		{
			r.eState = ScanoutState::On;
			r.sHeadline = *oValue == 2 ? "On (2, games only)" : "On (1)";
		}
		else if ( *oValue == 0 )
		{
			r.eState = ScanoutState::Off;
			r.sHeadline = "Off (0): generated frames are lost";
			r.sReason = "Hyprland draws every frame itself and that draw waits behind the "
			            "game's GPU work.";
		}
		else
		{
			r.sHeadline = "Could not check";
			r.sReason = "render:direct_scanout has the unexpected value " + std::to_string( *oValue ) + ".";
		}
		return r;
	}

	// The only state that warns. It was switched off 2026-11-06 to 2026-11-07 while
	// direct scanout on could crash the GPU; that crash is fixed.
	inline bool WarnsAbout( ScanoutState e ) { return e == ScanoutState::Off; }

	// Whether a state raises the toast and the rail's yellow dot: it warns and the
	// user has not silenced the warning (global.json overlay.diagnostics_silence_warning).
	// The area itself always shows the true state; only the alerts are silenced.
	inline bool Alerts( ScanoutState e, bool bSilenced ) { return !bSilenced && WarnsAbout( e ); }

	// Whether any published check result warns. Derived from the results, not a
	// flag kept beside them, so a future check counts the moment its result is
	// in the range. "Could not check" is not a warning (same rule as the toast).
	template <class It>
	inline bool AnyWarns( It itBegin, It itEnd, bool bSilenced = false )
	{
		for ( ; itBegin != itEnd; ++itBegin )
			if ( Alerts( itBegin->eState, bSilenced ) )
				return true;
		return false;
	}

	inline constexpr const char *kHyprlandRenderDocsUrl =
		"https://wiki.hypr.land/configuring/core/config-options/#render";
}
