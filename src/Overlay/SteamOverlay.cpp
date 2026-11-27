// See SteamOverlay.h.
#include "SteamOverlay.h"

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "Config/ConfigManager.h"
#include "convar.h"
#include "log.hpp"
#include "steamcompmgr.hpp"
#include "Utils/Process.h"

namespace gamescope
{
	namespace
	{
		LogScope steamoverlay_log( "steam_overlay_ctl" );

		// All three resolve together, once, from global.json (read lazily, like
		// Diagnostics.cpp: main() asks before the normal config load).
		std::once_flag    g_ResolveOnce;
		std::atomic<bool> g_bControl{ false };
		std::atomic<bool> g_bControlAtLaunch{ false };
		std::atomic<bool> g_bEnabled{ true };

		void Resolve()
		{
			std::call_once( g_ResolveOnce, []
			{
				const config::Settings s = config::LoadGlobal();
				g_bControl.store( s.overlay.steam_overlay_control );
				g_bEnabled.store( s.overlay.steam_overlay_enabled );
				const bool bLaunch = steamoverlay::ResolveControl( Process::SteamOverlayDummyOverride(), s.overlay.steam_overlay_control );
				g_bControlAtLaunch.store( bLaunch );
				steamoverlay_log.infof( "steam overlay control %s (key %s), enable %s",
					bLaunch ? "on" : "off", s.overlay.steam_overlay_control ? "on" : "off",
					s.overlay.steam_overlay_enabled ? "on" : "off" );
			} );
		}
	}

	bool SteamOverlay_Control()
	{
		Resolve();
		return g_bControl.load();
	}

	void SteamOverlay_SetControl( bool bControl )
	{
		Resolve();   // fixes the launch value before the first change
		g_bControl.store( bControl );
		config::Settings s = config::LoadGlobal();
		s.overlay.steam_overlay_control = bControl;
		config::EnqueueGlobalWrite( s );
		force_repaint();
	}

	bool SteamOverlay_ControlAtLaunch()
	{
		Resolve();
		return g_bControlAtLaunch.load();
	}

	bool SteamOverlay_Enabled()
	{
		Resolve();
		return g_bEnabled.load();
	}

	void SteamOverlay_SetEnabled( bool bEnabled )
	{
		Resolve();
		g_bEnabled.store( bEnabled );
		config::Settings s = config::LoadGlobal();
		s.overlay.steam_overlay_enabled = bEnabled;
		config::EnqueueGlobalWrite( s );
		// The steamcompmgr loop compares steamoverlay::HelperShouldRun() with what runs
		// and forks / kills; wake it (it may be idle).
		nudge_steamcompmgr();
	}

	// Without a settings tab (and for scripts): gamescopectl ritz_steam_overlay_enabled 0|1.
	static ConCommand cc_ritz_steam_overlay_enabled(
		"ritz_steam_overlay_enabled",
		"Show or set \"Enable Steam Overlay\": ritz_steam_overlay_enabled [0|1]. Starts or "
		"stops the dummy window live while Steam overlay control was on at launch.",
		[]( std::span<std::string_view> args )
		{
			if ( args.size() >= 2 )
				SteamOverlay_SetEnabled( args[ 1 ] != "0" );
			steamoverlay_log.infof( "ritz_steam_overlay_enabled = %d (control at launch %d)",
				(int)SteamOverlay_Enabled(), (int)SteamOverlay_ControlAtLaunch() );
		} );
}
