#include <catch2/catch_test_macros.hpp>

#include <stdlib.h>

#include <string>

#include "Utils/Process.h"

// Keep in sync with k_pszStashedOverlayPreload in Process.cpp.
static constexpr const char *k_pszStashedOverlayPreload = "GAMESCOPE_STEAM_OVERLAY_LD_PRELOAD";

static const char *k_pszOverlay = "/home/user/.local/share/Steam/ubuntu12_64/gameoverlayrenderer.so";

static std::string GetEnv( const char *pszName )
{
	const char *pszValue = getenv( pszName );
	REQUIRE( pszValue != nullptr );
	return pszValue;
}

TEST_CASE("RemoveSteamOverlayFromPreload", "[process]") {
	SECTION("does nothing when LD_PRELOAD is unset") {
		unsetenv( "LD_PRELOAD" );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( getenv( "LD_PRELOAD" ) == nullptr );
	}

	SECTION("unsets LD_PRELOAD when the overlay is the only entry") {
		setenv( "LD_PRELOAD", k_pszOverlay, 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( getenv( "LD_PRELOAD" ) == nullptr );
	}

	SECTION("removes the overlay and keeps the other entries") {
		setenv( "LD_PRELOAD", ( std::string{ "libfirst.so:" } + k_pszOverlay + ":liblast.so" ).c_str(), 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so:liblast.so" );
	}

	SECTION("handles space-separated lists") {
		setenv( "LD_PRELOAD", ( std::string{ "libfirst.so " } + k_pszOverlay ).c_str(), 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so" );
	}

	SECTION("leaves the other entries alone when the overlay is absent") {
		setenv( "LD_PRELOAD", "libfirst.so:liblast.so", 1 );
		gamescope::Process::RemoveSteamOverlayFromPreload();
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so:liblast.so" );
	}
}

TEST_CASE("RestoreSteamOverlayPreload", "[process]") {
	SECTION("reports when there is nothing stashed") {
		unsetenv( k_pszStashedOverlayPreload );
		setenv( "LD_PRELOAD", "libuntouched.so", 1 );
		REQUIRE( gamescope::Process::RestoreSteamOverlayPreload() == false );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libuntouched.so" );
	}

	SECTION("puts the stashed preload back and consumes the stash") {
		setenv( k_pszStashedOverlayPreload, k_pszOverlay, 1 );
		unsetenv( "LD_PRELOAD" );
		REQUIRE( gamescope::Process::RestoreSteamOverlayPreload() == true );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == k_pszOverlay );
		REQUIRE( getenv( k_pszStashedOverlayPreload ) == nullptr );
	}
}

// ---- The Steam overlay dummy-window route (superdoc/features/steam-overlay.md) ----

#include <filesystem>
#include <fstream>
#include <optional>

static const char *k_pszOverlay32 = "/home/user/.local/share/Steam/ubuntu12_32/gameoverlayrenderer.so";

TEST_CASE("Steam overlay dummy route: the game carries neither piece", "[process][steam-overlay]") {
	const std::string sBoth = std::string( "libfirst.so:" ) + k_pszOverlay32 + ":" + k_pszOverlay;

	SECTION("the overlay preload entries go, other entries stay") {
		auto env = gamescope::Process::ComputeGameOverlayEnv( sBoth );
		REQUIRE( env.oLdPreload.has_value() );
		REQUIRE( *env.oLdPreload == "libfirst.so" );
		REQUIRE( env.bVulkanLayer == false );
	}

	SECTION("LD_PRELOAD is unset when the overlay was the only entry") {
		auto env = gamescope::Process::ComputeGameOverlayEnv( std::string( k_pszOverlay ) );
		REQUIRE( !env.oLdPreload.has_value() );
	}

	SECTION("applying it unsets the enable var and sets the disable var") {
		setenv( "LD_PRELOAD", sBoth.c_str(), 1 );
		setenv( "ENABLE_VK_LAYER_VALVE_steam_overlay_1", "1", 1 );
		unsetenv( "DISABLE_VK_LAYER_VALVE_steam_overlay_1" );
		gamescope::Process::ApplySteamOverlayEnv( gamescope::Process::ComputeGameOverlayEnv( sBoth ) );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == "libfirst.so" );
		REQUIRE( getenv( "ENABLE_VK_LAYER_VALVE_steam_overlay_1" ) == nullptr );
		REQUIRE( GetEnv( "DISABLE_VK_LAYER_VALVE_steam_overlay_1" ) == "1" );
		unsetenv( "DISABLE_VK_LAYER_VALVE_steam_overlay_1" );
		unsetenv( "LD_PRELOAD" );
	}
}

TEST_CASE("Steam overlay dummy route: the dummy window carries both", "[process][steam-overlay]") {
	const std::string sBoth = std::string( "libfirst.so:" ) + k_pszOverlay32 + ":" + k_pszOverlay;

	SECTION("both overlay entries come from the stash, nothing else") {
		auto env = gamescope::Process::ComputeDummyOverlayEnv( std::string( "libgame.so" ), sBoth, "" );
		REQUIRE( env.oLdPreload.has_value() );
		REQUIRE( *env.oLdPreload == std::string( k_pszOverlay32 ) + ":" + k_pszOverlay );
		REQUIRE( env.bVulkanLayer == true );
	}

	SECTION("without a stash the overlay entries already in LD_PRELOAD are used") {
		auto env = gamescope::Process::ComputeDummyOverlayEnv( sBoth, std::nullopt, "" );
		REQUIRE( *env.oLdPreload == std::string( k_pszOverlay32 ) + ":" + k_pszOverlay );
	}

	SECTION("with neither, the install lookup result is used") {
		auto env = gamescope::Process::ComputeDummyOverlayEnv( std::nullopt, std::nullopt, "/steam/steamrt64/gameoverlayrenderer.so" );
		REQUIRE( *env.oLdPreload == "/steam/steamrt64/gameoverlayrenderer.so" );
		REQUIRE( env.bVulkanLayer == true );
	}

	SECTION("with nothing at all it still starts, hosting nothing") {
		auto env = gamescope::Process::ComputeDummyOverlayEnv( std::nullopt, std::nullopt, "" );
		REQUIRE( !env.oLdPreload.has_value() );
		REQUIRE( env.bVulkanLayer == true );
	}

	SECTION("applying it sets the enable var and clears the disable var") {
		setenv( "DISABLE_VK_LAYER_VALVE_steam_overlay_1", "1", 1 );
		unsetenv( "ENABLE_VK_LAYER_VALVE_steam_overlay_1" );
		gamescope::Process::ApplySteamOverlayEnv( gamescope::Process::ComputeDummyOverlayEnv( std::nullopt, sBoth, "" ) );
		REQUIRE( GetEnv( "ENABLE_VK_LAYER_VALVE_steam_overlay_1" ) == "1" );
		REQUIRE( getenv( "DISABLE_VK_LAYER_VALVE_steam_overlay_1" ) == nullptr );
		REQUIRE( GetEnv( "LD_PRELOAD" ) == std::string( k_pszOverlay32 ) + ":" + k_pszOverlay );
		unsetenv( "ENABLE_VK_LAYER_VALVE_steam_overlay_1" );
		unsetenv( "LD_PRELOAD" );
	}
}

TEST_CASE("Steam overlay dummy route: the stash is consumed", "[process][steam-overlay]") {
	setenv( k_pszStashedOverlayPreload, k_pszOverlay, 1 );
	setenv( "LD_PRELOAD", "libuntouched.so", 1 );
	auto oStash = gamescope::Process::TakeStashedOverlayPreload();
	REQUIRE( oStash.has_value() );
	REQUIRE( *oStash == k_pszOverlay );
	REQUIRE( getenv( k_pszStashedOverlayPreload ) == nullptr );
	REQUIRE( GetEnv( "LD_PRELOAD" ) == "libuntouched.so" );
	REQUIRE( !gamescope::Process::TakeStashedOverlayPreload().has_value() );
	unsetenv( "LD_PRELOAD" );
}

TEST_CASE("Steam overlay dummy route: switch", "[process][steam-overlay]") {
	unsetenv( "GS_RITZ_STEAM_OVERLAY_DUMMY" );
	REQUIRE( gamescope::Process::SteamOverlayDummyRequested() == false );
	setenv( "GS_RITZ_STEAM_OVERLAY_DUMMY", "1", 1 );
	REQUIRE( gamescope::Process::SteamOverlayDummyRequested() == true );
	setenv( "GS_RITZ_STEAM_OVERLAY_DUMMY", "0", 1 );
	REQUIRE( gamescope::Process::SteamOverlayDummyRequested() == false );
	unsetenv( "GS_RITZ_STEAM_OVERLAY_DUMMY" );
}

TEST_CASE("Steam overlay dummy route: install lookup fallback", "[process][steam-overlay]") {
	std::string sTemplate = ( std::filesystem::temp_directory_path() / "gamescope-ritz-overlay-XXXXXX" ).string();
	REQUIRE( mkdtemp( sTemplate.data() ) != nullptr );
	const std::filesystem::path root = sTemplate;

	REQUIRE( gamescope::Process::FindSteamOverlayRenderer( { root.string() } ) == "" );

	SECTION("older clients keep it in ubuntu12_64") {
		std::filesystem::create_directories( root / "ubuntu12_64" );
		std::ofstream( root / "ubuntu12_64" / "gameoverlayrenderer.so" ) << "x";
		REQUIRE( gamescope::Process::FindSteamOverlayRenderer( { root.string() } ) == ( root / "ubuntu12_64" / "gameoverlayrenderer.so" ).string() );
	}

	SECTION("current clients keep it in steamrt64, which wins") {
		std::filesystem::create_directories( root / "ubuntu12_64" );
		std::filesystem::create_directories( root / "steamrt64" );
		std::ofstream( root / "ubuntu12_64" / "gameoverlayrenderer.so" ) << "x";
		std::ofstream( root / "steamrt64" / "gameoverlayrenderer.so" ) << "x";
		REQUIRE( gamescope::Process::FindSteamOverlayRenderer( { "/nonexistent-steam-root", root.string() } ) == ( root / "steamrt64" / "gameoverlayrenderer.so" ).string() );
	}

	std::filesystem::remove_all( root );
}
